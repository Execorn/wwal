// SPDX-License-Identifier: GPL-3.0-or-later
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

/* =========================================================================
 * Runtime Value Representation & Fixed 16 MiB Arena
 * ========================================================================= */

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
    uint8_t dim;        /* 2, 3, or 4 */
    uint8_t bind_tag;   /* 0=none, 1=layer.origin, 2=layer.scale, 3=layer.angles, 4=layer.color,
                           5=layer.size */
    uint8_t layer_slot; /* 0=thisLayer, 1..WE_SCRIPT_MAX_SCENE_LAYERS=scene_layers[slot-1] */
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

/* =========================================================================
 * Tokenizer
 * ========================================================================= */

typedef struct {
    we_script_t *vm;
    const char *src;
    size_t len;
    size_t pos;
    uint32_t depth;
    js_token_t cur;
} js_parser_t;

static void lexer_skip_ws_and_comments(js_parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n') {
                p->pos++;
            }
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) {
                p->pos++;
            }
            p->pos = (p->pos + 1 < p->len) ? (p->pos + 2) : p->len;
        } else {
            break;
        }
    }
}

static void lexer_next(js_parser_t *p)
{
    lexer_skip_ws_and_comments(p);
    memset(&p->cur, 0, sizeof(p->cur));
    if (p->pos >= p->len) {
        p->cur.type = TOK_EOF;
        return;
    }

    char c = p->src[p->pos];

    /* Identifiers & keywords */
    if (isalpha((unsigned char)c) || c == '_' || c == '$') {
        size_t start = p->pos++;
        while (p->pos < p->len && (isalnum((unsigned char)p->src[p->pos]) ||
                                   p->src[p->pos] == '_' || p->src[p->pos] == '$')) {
            p->pos++;
        }
        size_t ilen = p->pos - start;
        const char *s = p->src + start;
        if (ilen == 3 && strncmp(s, "var", 3) == 0) {
            p->cur.type = TOK_VAR;
        } else if (ilen == 3 && strncmp(s, "let", 3) == 0) {
            p->cur.type = TOK_LET;
        } else if (ilen == 5 && strncmp(s, "const", 5) == 0) {
            p->cur.type = TOK_CONST;
        } else if (ilen == 8 && strncmp(s, "function", 8) == 0) {
            p->cur.type = TOK_FUNCTION;
        } else if (ilen == 6 && strncmp(s, "export", 6) == 0) {
            p->cur.type = TOK_EXPORT;
        } else if (ilen == 6 && strncmp(s, "return", 6) == 0) {
            p->cur.type = TOK_RETURN;
        } else if (ilen == 2 && strncmp(s, "if", 2) == 0) {
            p->cur.type = TOK_IF;
        } else if (ilen == 4 && strncmp(s, "else", 4) == 0) {
            p->cur.type = TOK_ELSE;
        } else if (ilen == 3 && strncmp(s, "for", 3) == 0) {
            p->cur.type = TOK_FOR;
        } else if (ilen == 5 && strncmp(s, "while", 5) == 0) {
            p->cur.type = TOK_WHILE;
        } else if (ilen == 2 && strncmp(s, "do", 2) == 0) {
            p->cur.type = TOK_DO;
        } else if (ilen == 6 && strncmp(s, "switch", 6) == 0) {
            p->cur.type = TOK_SWITCH;
        } else if (ilen == 4 && strncmp(s, "case", 4) == 0) {
            p->cur.type = TOK_CASE;
        } else if (ilen == 7 && strncmp(s, "default", 7) == 0) {
            p->cur.type = TOK_DEFAULT;
        } else if (ilen == 3 && strncmp(s, "try", 3) == 0) {
            p->cur.type = TOK_TRY;
        } else if (ilen == 5 && strncmp(s, "catch", 5) == 0) {
            p->cur.type = TOK_CATCH;
        } else if (ilen == 7 && strncmp(s, "finally", 7) == 0) {
            p->cur.type = TOK_FINALLY;
        } else if (ilen == 5 && strncmp(s, "throw", 5) == 0) {
            p->cur.type = TOK_THROW;
        } else if (ilen == 5 && strncmp(s, "break", 5) == 0) {
            p->cur.type = TOK_BREAK;
        } else if (ilen == 8 && strncmp(s, "continue", 8) == 0) {
            p->cur.type = TOK_CONTINUE;
        } else if (ilen == 3 && strncmp(s, "new", 3) == 0) {
            p->cur.type = TOK_NEW;
        } else if (ilen == 4 && strncmp(s, "this", 4) == 0) {
            p->cur.type = TOK_THIS;
        } else if (ilen == 6 && strncmp(s, "typeof", 6) == 0) {
            p->cur.type = TOK_TYPEOF;
        } else if (ilen == 4 && strncmp(s, "true", 4) == 0) {
            p->cur.type = TOK_TRUE;
        } else if (ilen == 5 && strncmp(s, "false", 5) == 0) {
            p->cur.type = TOK_FALSE;
        } else if (ilen == 4 && strncmp(s, "null", 4) == 0) {
            p->cur.type = TOK_NULL;
        } else if (ilen == 9 && strncmp(s, "undefined", 9) == 0) {
            p->cur.type = TOK_UNDEF;
        } else {
            p->cur.type = TOK_IDENT;
            p->cur.text = script_arena_strdup(p->vm, s, ilen);
        }
        return;
    }

    /* Numeric literals */
    if (isdigit((unsigned char)c) ||
        (c == '.' && p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1]))) {
        size_t rem = p->len - p->pos;
        size_t clen = rem < 63 ? rem : 63;
        char nbuf[64];
        memcpy(nbuf, p->src + p->pos, clen);
        nbuf[clen] = '\0';
        char *endptr = NULL;
        double val = 0.0;
        if (clen > 2 && nbuf[0] == '0' && (nbuf[1] == 'x' || nbuf[1] == 'X')) {
            val = (double)strtoull(nbuf, &endptr, 16);
        } else {
            val = strtod(nbuf, &endptr);
        }
        if (endptr && endptr > nbuf) {
            p->pos += (size_t)(endptr - nbuf);
        } else {
            p->pos++;
        }
        p->cur.type = TOK_NUM;
        p->cur.num = val;
        return;
    }

    /* String literals ('...', "...", `...`) */
    if (c == '\'' || c == '"' || c == '`') {
        char quote = c;
        p->pos++;
        size_t start = p->pos;
        char tmp[1024];
        size_t tlen = 0;
        while (p->pos < p->len && p->src[p->pos] != quote) {
            char ch = p->src[p->pos++];
            if (ch == '\\' && p->pos < p->len) {
                char esc = p->src[p->pos++];
                switch (esc) {
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    ch = esc;
                    break;
                }
            }
            if (tlen + 1 < sizeof(tmp)) {
                tmp[tlen++] = ch;
            }
        }
        if (p->pos < p->len && p->src[p->pos] == quote) {
            p->pos++;
        }
        (void)start;
        p->cur.type = TOK_STR;
        p->cur.text = script_arena_strdup(p->vm, tmp, tlen);
        return;
    }

    /* Multi-character operators */
    char c2 = (p->pos + 1 < p->len) ? p->src[p->pos + 1] : '\0';
    char c3 = (p->pos + 2 < p->len) ? p->src[p->pos + 2] : '\0';
    char c4 = (p->pos + 3 < p->len) ? p->src[p->pos + 3] : '\0';

    if (c == '>' && c2 == '>' && c3 == '>' && c4 == '=') {
        p->pos += 4;
        p->cur.type = TOK_USHR_ASSIGN;
        return;
    }
    if (c == '>' && c2 == '>' && c3 == '>') {
        p->pos += 3;
        p->cur.type = TOK_USHR;
        return;
    }
    if (c == '<' && c2 == '<' && c3 == '=') {
        p->pos += 3;
        p->cur.type = TOK_SHL_ASSIGN;
        return;
    }
    if (c == '>' && c2 == '>' && c3 == '=') {
        p->pos += 3;
        p->cur.type = TOK_SHR_ASSIGN;
        return;
    }
    if (c == '=' && c2 == '=' && c3 == '=') {
        p->pos += 3;
        p->cur.type = TOK_STRICT_EQ;
        return;
    }
    if (c == '!' && c2 == '=' && c3 == '=') {
        p->pos += 3;
        p->cur.type = TOK_STRICT_NEQ;
        return;
    }
    if (c == '+' && c2 == '+') {
        p->pos += 2;
        p->cur.type = TOK_INC;
        return;
    }
    if (c == '-' && c2 == '-') {
        p->pos += 2;
        p->cur.type = TOK_DEC;
        return;
    }
    if (c == '+' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_PLUS_ASSIGN;
        return;
    }
    if (c == '-' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_MINUS_ASSIGN;
        return;
    }
    if (c == '*' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_STAR_ASSIGN;
        return;
    }
    if (c == '/' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_SLASH_ASSIGN;
        return;
    }
    if (c == '%' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_PERCENT_ASSIGN;
        return;
    }
    if (c == '&' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_AND_ASSIGN;
        return;
    }
    if (c == '|' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_OR_ASSIGN;
        return;
    }
    if (c == '^' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_XOR_ASSIGN;
        return;
    }
    if (c == '=' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_EQ;
        return;
    }
    if (c == '!' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_NEQ;
        return;
    }
    if (c == '<' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_LE;
        return;
    }
    if (c == '>' && c2 == '=') {
        p->pos += 2;
        p->cur.type = TOK_GE;
        return;
    }
    if (c == '&' && c2 == '&') {
        p->pos += 2;
        p->cur.type = TOK_AND;
        return;
    }
    if (c == '|' && c2 == '|') {
        p->pos += 2;
        p->cur.type = TOK_OR;
        return;
    }
    if (c == '?' && c2 == '?') {
        p->pos += 2;
        p->cur.type = TOK_NULLISH;
        return;
    }
    if (c == '<' && c2 == '<') {
        p->pos += 2;
        p->cur.type = TOK_SHL;
        return;
    }
    if (c == '>' && c2 == '>') {
        p->pos += 2;
        p->cur.type = TOK_SHR;
        return;
    }

    p->pos++;
    switch (c) {
    case '(':
        p->cur.type = TOK_LPAREN;
        break;
    case ')':
        p->cur.type = TOK_RPAREN;
        break;
    case '{':
        p->cur.type = TOK_LBRACE;
        break;
    case '}':
        p->cur.type = TOK_RBRACE;
        break;
    case '[':
        p->cur.type = TOK_LBRACK;
        break;
    case ']':
        p->cur.type = TOK_RBRACK;
        break;
    case ';':
        p->cur.type = TOK_SEMI;
        break;
    case ',':
        p->cur.type = TOK_COMMA;
        break;
    case '.':
        p->cur.type = TOK_DOT;
        break;
    case ':':
        p->cur.type = TOK_COLON;
        break;
    case '?':
        p->cur.type = TOK_QUESTION;
        break;
    case '+':
        p->cur.type = TOK_PLUS;
        break;
    case '-':
        p->cur.type = TOK_MINUS;
        break;
    case '*':
        p->cur.type = TOK_STAR;
        break;
    case '/':
        p->cur.type = TOK_SLASH;
        break;
    case '%':
        p->cur.type = TOK_PERCENT;
        break;
    case '=':
        p->cur.type = TOK_ASSIGN;
        break;
    case '<':
        p->cur.type = TOK_LT;
        break;
    case '>':
        p->cur.type = TOK_GT;
        break;
    case '!':
        p->cur.type = TOK_NOT;
        break;
    case '&':
        p->cur.type = TOK_BIT_AND;
        break;
    case '|':
        p->cur.type = TOK_BIT_OR;
        break;
    case '^':
        p->cur.type = TOK_BIT_XOR;
        break;
    case '~':
        p->cur.type = TOK_BIT_NOT;
        break;
    default:
        p->vm->parse_error = true;
        p->cur.type = TOK_EOF;
        break;
    }
}

/* =========================================================================
 * Recursive-Descent AST Parser
 * ========================================================================= */

static ast_node_t *alloc_node(js_parser_t *p, ast_type_t type)
{
    ast_node_t *n = (ast_node_t *)script_arena_alloc(p->vm, sizeof(ast_node_t));
    if (n) {
        n->type = type;
    }
    return n;
}

static ast_node_t *parse_expr(js_parser_t *p);
static ast_node_t *parse_stmt(js_parser_t *p);
static ast_node_t *parse_func_decl(js_parser_t *p, bool is_exported);

static ast_node_t *parse_primary(js_parser_t *p)
{
    js_tok_type_t t = p->cur.type;
    if (t == TOK_FUNCTION) {
        return parse_func_decl(p, false);
    }
    if (t == TOK_NUM) {
        ast_node_t *n = alloc_node(p, AST_LITERAL);
        if (n) {
            n->lit.type = VAL_NUM;
            n->lit.u.num = p->cur.num;
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_STR) {
        ast_node_t *n = alloc_node(p, AST_LITERAL);
        if (n) {
            n->lit.type = VAL_STR;
            n->lit.u.str = p->cur.text ? p->cur.text : "";
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_TRUE || t == TOK_FALSE) {
        ast_node_t *n = alloc_node(p, AST_LITERAL);
        if (n) {
            n->lit.type = VAL_BOOL;
            n->lit.u.bval = (t == TOK_TRUE);
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_NULL) {
        ast_node_t *n = alloc_node(p, AST_LITERAL);
        if (n) {
            n->lit.type = VAL_NULL;
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_UNDEF) {
        ast_node_t *n = alloc_node(p, AST_LITERAL);
        if (n) {
            n->lit.type = VAL_UNDEF;
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_THIS) {
        ast_node_t *n = alloc_node(p, AST_THIS);
        lexer_next(p);
        return n;
    }
    if (t == TOK_IDENT) {
        ast_node_t *n = alloc_node(p, AST_IDENT);
        if (n) {
            n->name = p->cur.text;
        }
        lexer_next(p);
        return n;
    }
    if (t == TOK_LPAREN) {
        lexer_next(p);
        ast_node_t *e = parse_expr(p);
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        return e;
    }
    if (t == TOK_LBRACK) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_ARRAY_LIT);
        ast_node_t *items[256];
        uint32_t cnt = 0;
        while (p->cur.type != TOK_RBRACK && p->cur.type != TOK_EOF) {
            ast_node_t *elem = parse_expr(p);
            if (cnt < 256u) {
                items[cnt++] = elem;
            }
            if (p->cur.type == TOK_COMMA) {
                lexer_next(p);
            } else {
                break;
            }
        }
        if (p->cur.type == TOK_RBRACK) {
            lexer_next(p);
        }
        if (n && cnt > 0) {
            n->children = (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
            if (n->children) {
                memcpy(n->children, items, cnt * sizeof(ast_node_t *));
                n->child_count = cnt;
            }
        }
        return n;
    }
    if (t == TOK_LBRACE) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_OBJ_LIT);
        const char *keys[256];
        ast_node_t *vals[256];
        uint32_t cnt = 0;
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
            const char *k = NULL;
            if (p->cur.type == TOK_IDENT || p->cur.type == TOK_STR) {
                k = p->cur.text;
                lexer_next(p);
            } else if (p->cur.type == TOK_NUM) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "%g", p->cur.num);
                k = script_arena_strdup(p->vm, kbuf, strlen(kbuf));
                lexer_next(p);
            } else {
                break;
            }
            if (p->cur.type == TOK_COLON) {
                lexer_next(p);
            }
            ast_node_t *vnode = parse_expr(p);
            if (cnt < 256u) {
                keys[cnt] = k;
                vals[cnt] = vnode;
                cnt++;
            }
            if (p->cur.type == TOK_COMMA) {
                lexer_next(p);
            } else {
                break;
            }
        }
        if (p->cur.type == TOK_RBRACE) {
            lexer_next(p);
        }
        if (n && cnt > 0) {
            n->keys = (const char **)script_arena_alloc(p->vm, cnt * sizeof(const char *));
            n->children = (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
            if (n->keys && n->children) {
                memcpy(n->keys, keys, cnt * sizeof(const char *));
                memcpy(n->children, vals, cnt * sizeof(ast_node_t *));
                n->child_count = cnt;
            }
        }
        return n;
    }
    if (t == TOK_NEW) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_NEW);
        if (p->cur.type == TOK_IDENT && n) {
            n->name = p->cur.text;
            lexer_next(p);
        }
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
            ast_node_t *args[16];
            uint32_t cnt = 0;
            while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF && cnt < 16) {
                args[cnt++] = parse_expr(p);
                if (p->cur.type == TOK_COMMA) {
                    lexer_next(p);
                } else {
                    break;
                }
            }
            if (p->cur.type == TOK_RPAREN) {
                lexer_next(p);
            }
            if (n && cnt > 0) {
                n->children = (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
                if (n->children) {
                    memcpy(n->children, args, cnt * sizeof(ast_node_t *));
                    n->child_count = cnt;
                }
            }
        }
        return n;
    }

    p->vm->parse_error = true;
    lexer_next(p);
    return alloc_node(p, AST_LITERAL);
}

static bool parser_step_chain(js_parser_t *p, uint32_t *chain_steps)
{
    if (++(*chain_steps) > 256u) {
        p->vm->parse_error = true;
        p->vm->depth_exceeded = true;
        p->vm->aborted = true;
        p->cur.type = TOK_EOF;
        return false;
    }
    return true;
}

static ast_node_t *parse_postfix(js_parser_t *p)
{
    ast_node_t *lhs = parse_primary(p);
    uint32_t chain_steps = 0;
    while (p->cur.type != TOK_EOF) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        if (p->cur.type == TOK_DOT) {
            lexer_next(p);
            const char *prop = p->cur.text;
            if (p->cur.type == TOK_IDENT) {
                lexer_next(p);
            }
            ast_node_t *m = alloc_node(p, AST_MEMBER);
            if (m) {
                m->left = lhs;
                m->name = prop;
            }
            lhs = m;
        } else if (p->cur.type == TOK_LBRACK) {
            lexer_next(p);
            ast_node_t *idx = parse_expr(p);
            if (p->cur.type == TOK_RBRACK) {
                lexer_next(p);
            }
            ast_node_t *ix = alloc_node(p, AST_INDEX);
            if (ix) {
                ix->left = lhs;
                ix->right = idx;
            }
            lhs = ix;
        } else if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
            ast_node_t *args[16];
            uint32_t cnt = 0;
            while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF && cnt < 16) {
                args[cnt++] = parse_expr(p);
                if (p->cur.type == TOK_COMMA) {
                    lexer_next(p);
                } else {
                    break;
                }
            }
            if (p->cur.type == TOK_RPAREN) {
                lexer_next(p);
            }
            ast_node_t *call = alloc_node(p, AST_CALL);
            if (call) {
                call->left = lhs;
                if (cnt > 0) {
                    call->children =
                        (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
                    if (call->children) {
                        memcpy(call->children, args, cnt * sizeof(ast_node_t *));
                        call->child_count = cnt;
                    }
                }
            }
            lhs = call;
        } else if (p->cur.type == TOK_INC || p->cur.type == TOK_DEC) {
            js_tok_type_t op = p->cur.type;
            lexer_next(p);
            ast_node_t *u = alloc_node(p, AST_UPDATE);
            if (u) {
                u->op = op;
                u->prefix = false;
                u->left = lhs;
            }
            lhs = u;
        } else {
            break;
        }
    }
    return lhs;
}

static ast_node_t *parse_unary(js_parser_t *p)
{
    if (++p->depth > 64u) {
        p->depth--;
        p->vm->parse_error = true;
        p->vm->depth_exceeded = true;
        p->vm->aborted = true;
        p->cur.type = TOK_EOF;
        return alloc_node(p, AST_LITERAL);
    }
    if (p->cur.type == TOK_INC || p->cur.type == TOK_DEC) {
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *u = alloc_node(p, AST_UPDATE);
        if (u) {
            u->op = op;
            u->prefix = true;
            u->left = parse_unary(p);
        }
        p->depth--;
        return u;
    }
    if (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS || p->cur.type == TOK_NOT ||
        p->cur.type == TOK_BIT_NOT || p->cur.type == TOK_TYPEOF) {
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *u = alloc_node(p, AST_UNARY);
        if (u) {
            u->op = op;
            u->left = parse_unary(p);
        }
        p->depth--;
        return u;
    }
    ast_node_t *res = parse_postfix(p);
    p->depth--;
    return res;
}

static ast_node_t *parse_mul(js_parser_t *p)
{
    ast_node_t *lhs = parse_unary(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH || p->cur.type == TOK_PERCENT) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_unary(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_add(js_parser_t *p)
{
    ast_node_t *lhs = parse_mul(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_mul(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_shift(js_parser_t *p)
{
    ast_node_t *lhs = parse_add(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_SHL || p->cur.type == TOK_SHR || p->cur.type == TOK_USHR) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_add(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_rel(js_parser_t *p)
{
    ast_node_t *lhs = parse_shift(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_LT || p->cur.type == TOK_LE || p->cur.type == TOK_GT ||
           p->cur.type == TOK_GE) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_shift(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_eq(js_parser_t *p)
{
    ast_node_t *lhs = parse_rel(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_EQ || p->cur.type == TOK_STRICT_EQ || p->cur.type == TOK_NEQ ||
           p->cur.type == TOK_STRICT_NEQ) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_rel(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_bit(js_parser_t *p)
{
    ast_node_t *lhs = parse_eq(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_BIT_AND || p->cur.type == TOK_BIT_XOR || p->cur.type == TOK_BIT_OR) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_eq(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_logic_and(js_parser_t *p)
{
    ast_node_t *lhs = parse_bit(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_AND) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_bit(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_logic_or(js_parser_t *p)
{
    ast_node_t *lhs = parse_logic_and(p);
    uint32_t chain_steps = 0;
    while (p->cur.type == TOK_OR || p->cur.type == TOK_NULLISH) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_logic_and(p);
        ast_node_t *b = alloc_node(p, AST_BINARY);
        if (b) {
            b->op = op;
            b->left = lhs;
            b->right = rhs;
        }
        lhs = b;
    }
    return lhs;
}

static ast_node_t *parse_ternary(js_parser_t *p)
{
    ast_node_t *cond = parse_logic_or(p);
    if (p->cur.type == TOK_QUESTION) {
        lexer_next(p);
        ast_node_t *t_br = parse_expr(p);
        if (p->cur.type == TOK_COLON) {
            lexer_next(p);
        }
        ast_node_t *e_br = parse_expr(p);
        ast_node_t *tn = alloc_node(p, AST_TERNARY);
        if (tn) {
            tn->cond = cond;
            tn->then_branch = t_br;
            tn->else_branch = e_br;
        }
        return tn;
    }
    return cond;
}

static ast_node_t *parse_expr(js_parser_t *p)
{
    if (++p->depth > 64u) {
        p->depth--;
        p->vm->parse_error = true;
        p->vm->depth_exceeded = true;
        p->vm->aborted = true;
        p->cur.type = TOK_EOF;
        return alloc_node(p, AST_LITERAL);
    }
    ast_node_t *lhs = parse_ternary(p);
    if (p->cur.type == TOK_ASSIGN || p->cur.type == TOK_PLUS_ASSIGN ||
        p->cur.type == TOK_MINUS_ASSIGN || p->cur.type == TOK_STAR_ASSIGN ||
        p->cur.type == TOK_SLASH_ASSIGN || p->cur.type == TOK_PERCENT_ASSIGN ||
        p->cur.type == TOK_SHL_ASSIGN || p->cur.type == TOK_SHR_ASSIGN ||
        p->cur.type == TOK_USHR_ASSIGN || p->cur.type == TOK_AND_ASSIGN ||
        p->cur.type == TOK_OR_ASSIGN || p->cur.type == TOK_XOR_ASSIGN) {
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_expr(p);
        ast_node_t *a = alloc_node(p, AST_ASSIGN);
        if (a) {
            a->op = op;
            a->left = lhs;
            a->right = rhs;
        }
        p->depth--;
        return a;
    }
    p->depth--;
    return lhs;
}

static ast_node_t *parse_var_decl(js_parser_t *p, bool consume_semi)
{
    lexer_next(p); /* skip var/let/const */
    ast_node_t *n = alloc_node(p, AST_VAR_DECL);
    ast_var_item_t items[16];
    uint32_t cnt = 0;

    while (p->cur.type == TOK_IDENT && cnt < 16) {
        items[cnt].name = p->cur.text;
        items[cnt].init = NULL;
        lexer_next(p);
        if (p->cur.type == TOK_ASSIGN) {
            lexer_next(p);
            items[cnt].init = parse_expr(p);
        }
        cnt++;
        if (p->cur.type == TOK_COMMA) {
            lexer_next(p);
        } else {
            break;
        }
    }
    if (consume_semi && p->cur.type == TOK_SEMI) {
        lexer_next(p);
    }
    if (n && cnt > 0) {
        n->decls = (ast_var_item_t *)script_arena_alloc(p->vm, cnt * sizeof(ast_var_item_t));
        if (n->decls) {
            memcpy(n->decls, items, cnt * sizeof(ast_var_item_t));
            n->decl_count = cnt;
        }
    }
    return n;
}

static ast_node_t *parse_func_decl(js_parser_t *p, bool is_exported)
{
    lexer_next(p); /* skip 'function' */
    ast_node_t *fn = alloc_node(p, AST_FUNC_DECL);
    if (fn) {
        fn->is_exported = is_exported;
    }
    if (p->cur.type == TOK_IDENT) {
        if (fn) {
            fn->name = p->cur.text;
        }
        lexer_next(p);
    }
    if (p->cur.type == TOK_LPAREN) {
        lexer_next(p);
        const char *params[16];
        uint32_t cnt = 0;
        while (p->cur.type == TOK_IDENT && cnt < 16) {
            params[cnt++] = p->cur.text;
            lexer_next(p);
            if (p->cur.type == TOK_COMMA) {
                lexer_next(p);
            } else {
                break;
            }
        }
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        if (fn && cnt > 0) {
            fn->params = (const char **)script_arena_alloc(p->vm, cnt * sizeof(const char *));
            if (fn->params) {
                memcpy(fn->params, params, cnt * sizeof(const char *));
                fn->param_count = cnt;
            }
        }
    }
    if (fn) {
        fn->body = parse_stmt(p);
    }
    return fn;
}

static ast_node_t *parse_stmt_inner(js_parser_t *p)
{
    if (p->cur.type == TOK_SEMI) {
        lexer_next(p);
        return alloc_node(p, AST_BLOCK);
    }
    if (p->cur.type == TOK_LBRACE) {
        lexer_next(p);
        ast_node_t *blk = alloc_node(p, AST_BLOCK);
        ast_node_t *stmts[256];
        uint32_t cnt = 0;
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->vm->aborted) {
            ast_node_t *st = parse_stmt(p);
            if (cnt < 256u) {
                stmts[cnt++] = st;
            } else {
                break;
            }
        }
        if (p->cur.type == TOK_RBRACE) {
            lexer_next(p);
        }
        if (blk && cnt > 0) {
            blk->children = (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
            if (blk->children) {
                memcpy(blk->children, stmts, cnt * sizeof(ast_node_t *));
                blk->child_count = cnt;
            }
        }
        return blk;
    }
    if (p->cur.type == TOK_EXPORT) {
        lexer_next(p);
        if (p->cur.type == TOK_FUNCTION) {
            return parse_func_decl(p, true);
        }
        if (p->cur.type == TOK_VAR || p->cur.type == TOK_LET || p->cur.type == TOK_CONST) {
            ast_node_t *vd = parse_var_decl(p, true);
            if (vd) {
                vd->is_exported = true;
            }
            return vd;
        }
    }
    if (p->cur.type == TOK_FUNCTION) {
        return parse_func_decl(p, false);
    }
    if (p->cur.type == TOK_VAR || p->cur.type == TOK_LET || p->cur.type == TOK_CONST) {
        return parse_var_decl(p, true);
    }
    if (p->cur.type == TOK_IF) {
        lexer_next(p);
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
        }
        ast_node_t *cond = parse_expr(p);
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        ast_node_t *t_br = parse_stmt(p);
        ast_node_t *e_br = NULL;
        if (p->cur.type == TOK_ELSE) {
            lexer_next(p);
            e_br = parse_stmt(p);
        }
        ast_node_t *n = alloc_node(p, AST_IF);
        if (n) {
            n->cond = cond;
            n->then_branch = t_br;
            n->else_branch = e_br;
        }
        return n;
    }
    if (p->cur.type == TOK_SWITCH) {
        lexer_next(p);
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
        }
        ast_node_t *disc = parse_expr(p);
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        if (p->cur.type == TOK_LBRACE) {
            lexer_next(p);
        }
        ast_node_t *n = alloc_node(p, AST_SWITCH);
        if (n) {
            n->cond = disc;
        }
        ast_node_t *case_exprs[64];
        ast_node_t *case_bodies[64];
        uint32_t case_cnt = 0;
        while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->vm->aborted) {
            ast_node_t *cexpr = NULL;
            if (p->cur.type == TOK_CASE) {
                lexer_next(p);
                cexpr = parse_expr(p);
                if (p->cur.type == TOK_COLON) {
                    lexer_next(p);
                }
            } else if (p->cur.type == TOK_DEFAULT) {
                lexer_next(p);
                if (p->cur.type == TOK_COLON) {
                    lexer_next(p);
                }
            } else {
                (void)parse_stmt(p);
                continue;
            }
            ast_node_t *cblk = alloc_node(p, AST_BLOCK);
            ast_node_t *cstmts[64];
            uint32_t scnt = 0;
            while (p->cur.type != TOK_CASE && p->cur.type != TOK_DEFAULT &&
                   p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !p->vm->aborted) {
                ast_node_t *st = parse_stmt(p);
                if (scnt < 64u) {
                    cstmts[scnt++] = st;
                }
            }
            if (cblk && scnt > 0) {
                cblk->children =
                    (ast_node_t **)script_arena_alloc(p->vm, scnt * sizeof(ast_node_t *));
                if (cblk->children) {
                    memcpy(cblk->children, cstmts, scnt * sizeof(ast_node_t *));
                    cblk->child_count = scnt;
                }
            }
            if (case_cnt < 64u) {
                case_exprs[case_cnt] = cexpr;
                case_bodies[case_cnt] = cblk;
                case_cnt++;
            }
        }
        if (p->cur.type == TOK_RBRACE) {
            lexer_next(p);
        }
        if (n && case_cnt > 0) {
            n->case_exprs =
                (ast_node_t **)script_arena_alloc(p->vm, case_cnt * sizeof(ast_node_t *));
            n->children = (ast_node_t **)script_arena_alloc(p->vm, case_cnt * sizeof(ast_node_t *));
            if (n->case_exprs && n->children) {
                memcpy(n->case_exprs, case_exprs, case_cnt * sizeof(ast_node_t *));
                memcpy(n->children, case_bodies, case_cnt * sizeof(ast_node_t *));
                n->child_count = case_cnt;
            }
        }
        return n;
    }
    if (p->cur.type == TOK_TRY) {
        lexer_next(p);
        ast_node_t *try_body = parse_stmt(p);
        const char *catch_var = NULL;
        ast_node_t *catch_body = NULL;
        ast_node_t *finally_body = NULL;
        if (p->cur.type == TOK_CATCH) {
            lexer_next(p);
            if (p->cur.type == TOK_LPAREN) {
                lexer_next(p);
                if (p->cur.type == TOK_IDENT) {
                    catch_var = p->cur.text;
                    lexer_next(p);
                }
                if (p->cur.type == TOK_RPAREN) {
                    lexer_next(p);
                }
            }
            catch_body = parse_stmt(p);
        }
        if (p->cur.type == TOK_FINALLY) {
            lexer_next(p);
            finally_body = parse_stmt(p);
        }
        ast_node_t *n = alloc_node(p, AST_TRY);
        if (n) {
            n->body = try_body;
            n->name = catch_var;
            n->else_branch = catch_body;
            n->step = finally_body;
        }
        return n;
    }
    if (p->cur.type == TOK_THROW) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_THROW);
        if (p->cur.type != TOK_SEMI && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
            if (n) {
                n->left = parse_expr(p);
            }
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        return n;
    }
    if (p->cur.type == TOK_WHILE) {
        lexer_next(p);
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
        }
        ast_node_t *cond = parse_expr(p);
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        ast_node_t *body = parse_stmt(p);
        ast_node_t *n = alloc_node(p, AST_WHILE);
        if (n) {
            n->cond = cond;
            n->body = body;
        }
        return n;
    }
    if (p->cur.type == TOK_DO) {
        lexer_next(p);
        ast_node_t *body = parse_stmt(p);
        if (p->cur.type == TOK_WHILE) {
            lexer_next(p);
        }
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
        }
        ast_node_t *cond = parse_expr(p);
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        ast_node_t *n = alloc_node(p, AST_DO_WHILE);
        if (n) {
            n->body = body;
            n->cond = cond;
        }
        return n;
    }
    if (p->cur.type == TOK_FOR) {
        lexer_next(p);
        if (p->cur.type == TOK_LPAREN) {
            lexer_next(p);
        }
        ast_node_t *init = NULL;
        if (p->cur.type == TOK_VAR || p->cur.type == TOK_LET || p->cur.type == TOK_CONST) {
            init = parse_var_decl(p, false);
        } else if (p->cur.type != TOK_SEMI) {
            init = parse_expr(p);
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        ast_node_t *cond = NULL;
        if (p->cur.type != TOK_SEMI) {
            cond = parse_expr(p);
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        ast_node_t *step = NULL;
        if (p->cur.type != TOK_RPAREN) {
            step = parse_expr(p);
        }
        if (p->cur.type == TOK_RPAREN) {
            lexer_next(p);
        }
        ast_node_t *body = parse_stmt(p);
        ast_node_t *n = alloc_node(p, AST_FOR);
        if (n) {
            n->init = init;
            n->cond = cond;
            n->step = step;
            n->body = body;
        }
        return n;
    }
    if (p->cur.type == TOK_RETURN) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_RETURN);
        if (p->cur.type != TOK_SEMI && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
            if (n) {
                n->left = parse_expr(p);
            }
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        return n;
    }
    if (p->cur.type == TOK_BREAK) {
        lexer_next(p);
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        return alloc_node(p, AST_BREAK);
    }
    if (p->cur.type == TOK_CONTINUE) {
        lexer_next(p);
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        return alloc_node(p, AST_CONTINUE);
    }

    ast_node_t *e = parse_expr(p);
    if (p->cur.type == TOK_SEMI) {
        lexer_next(p);
    }
    ast_node_t *es = alloc_node(p, AST_EXPR_STMT);
    if (es) {
        es->left = e;
    }
    return es;
}

static ast_node_t *parse_stmt(js_parser_t *p)
{
    if (++p->depth > 64u) {
        p->depth--;
        p->vm->parse_error = true;
        p->vm->depth_exceeded = true;
        p->vm->aborted = true;
        p->cur.type = TOK_EOF;
        return alloc_node(p, AST_BLOCK);
    }
    ast_node_t *res = parse_stmt_inner(p);
    p->depth--;
    return res;
}

static ast_node_t *parse_program(js_parser_t *p)
{
    ast_node_t *prog = alloc_node(p, AST_PROGRAM);
    ast_node_t *stmts[256];
    uint32_t cnt = 0;
    while (p->cur.type != TOK_EOF && cnt < 256 && !p->vm->aborted) {
        stmts[cnt++] = parse_stmt(p);
    }
    if (prog && cnt > 0) {
        prog->children = (ast_node_t **)script_arena_alloc(p->vm, cnt * sizeof(ast_node_t *));
        if (prog->children) {
            memcpy(prog->children, stmts, cnt * sizeof(ast_node_t *));
            prog->child_count = cnt;
        }
    }
    return prog;
}

/* =========================================================================
 * Watchdog, Value Helpers & Built-in Objects (Vec2/3/4, Math, WEMath, etc.)
 * ========================================================================= */

static inline bool vm_check_watchdog(we_script_t *vm)
{
    if (vm->aborted) {
        return false;
    }
    if (++vm->step_count > WE_SCRIPT_MAX_STEPS) {
        vm->aborted = true;
        vm->timed_out = true;
        return false;
    }
    if ((vm->step_count & 63u) == 0u) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t ds = (int64_t)now.tv_sec - (int64_t)vm->start_ts.tv_sec;
        int64_t dn = (int64_t)now.tv_nsec - (int64_t)vm->start_ts.tv_nsec;
        int64_t elapsed_ns = ds * 1000000000LL + dn;
        if (elapsed_ns > (int64_t)WE_SCRIPT_TIMEOUT_NS) {
            vm->aborted = true;
            vm->timed_out = true;
            return false;
        }
    }
    return true;
}

static inline js_val_t val_undef(void)
{
    js_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_UNDEF;
    return v;
}

static inline js_val_t val_num(double n)
{
    js_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_NUM;
    v.u.num = isfinite(n) ? n : 0.0;
    return v;
}

static inline js_val_t val_bool(bool b)
{
    js_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_BOOL;
    v.u.bval = b;
    return v;
}

static inline js_val_t val_str(const char *s)
{
    js_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_STR;
    v.u.str = s ? s : "";
    return v;
}

static inline js_val_t val_builtin(js_builtin_id_t id)
{
    js_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_BUILTIN_OBJ;
    v.u.builtin_id = id;
    return v;
}

static js_val_t val_new_vec(we_script_t *vm, uint8_t dim, float x, float y, float z, float w)
{
    js_vec_obj_t *vo = (js_vec_obj_t *)script_arena_alloc(vm, sizeof(js_vec_obj_t));
    if (!vo) {
        return val_undef();
    }
    vo->dim = dim;
    vo->bind_tag = 0;
    vo->v[0] = isfinite(x) ? x : 0.0f;
    vo->v[1] = isfinite(y) ? y : 0.0f;
    vo->v[2] = isfinite(z) ? z : 0.0f;
    vo->v[3] = isfinite(w) ? w : 0.0f;
    js_val_t val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_VEC;
    val.u.vec = vo;
    return val;
}

static double val_to_num(js_val_t v)
{
    switch (v.type) {
    case VAL_NUM:
        return v.u.num;
    case VAL_BOOL:
        return v.u.bval ? 1.0 : 0.0;
    case VAL_STR:
        return v.u.str ? strtod(v.u.str, NULL) : 0.0;
    case VAL_VEC:
        return v.u.vec ? (double)v.u.vec->v[0] : 0.0;
    default:
        return 0.0;
    }
}

static inline float sanitize_f(float x, float fallback) { return isfinite(x) ? x : fallback; }

static bool val_to_bool(js_val_t v)
{
    switch (v.type) {
    case VAL_BOOL:
        return v.u.bval;
    case VAL_NUM:
        return v.u.num != 0.0 && !isnan(v.u.num);
    case VAL_STR:
        return v.u.str != NULL && v.u.str[0] != '\0';
    case VAL_VEC:
    case VAL_ARR:
    case VAL_OBJ:
    case VAL_FUNC:
    case VAL_BUILTIN_OBJ:
    case VAL_SCENE_LAYER:
    case VAL_AUDIO_BUFFERS:
    case VAL_AUDIO_CHANNEL:
        return true;
    default:
        return false;
    }
}

static void val_to_str_buf(js_val_t v, char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    switch (v.type) {
    case VAL_STR:
        snprintf(buf, cap, "%s", v.u.str ? v.u.str : "");
        break;
    case VAL_NUM:
        snprintf(buf, cap, "%g", isfinite(v.u.num) ? v.u.num : 0.0);
        break;
    case VAL_BOOL:
        snprintf(buf, cap, "%s", v.u.bval ? "true" : "false");
        break;
    case VAL_NULL:
        snprintf(buf, cap, "null");
        break;
    case VAL_UNDEF:
        snprintf(buf, cap, "undefined");
        break;
    case VAL_VEC:
        if (v.u.vec) {
            if (v.u.vec->dim == 2) {
                snprintf(buf, cap, "%g %g", v.u.vec->v[0], v.u.vec->v[1]);
            } else if (v.u.vec->dim == 3) {
                snprintf(buf, cap, "%g %g %g", v.u.vec->v[0], v.u.vec->v[1], v.u.vec->v[2]);
            } else {
                snprintf(buf, cap, "%g %g %g %g", v.u.vec->v[0], v.u.vec->v[1], v.u.vec->v[2],
                         v.u.vec->v[3]);
            }
        } else {
            buf[0] = '\0';
        }
        break;
    default:
        snprintf(buf, cap, "[object Object]");
        break;
    }
}

static bool js_val_equals(js_val_t lv, js_val_t rv, bool strict)
{
    if (strict && lv.type != rv.type) {
        return false;
    }
    if (lv.type == VAL_UNDEF || lv.type == VAL_NULL || rv.type == VAL_UNDEF ||
        rv.type == VAL_NULL) {
        if (strict) {
            return lv.type == rv.type;
        }
        return (lv.type == VAL_UNDEF || lv.type == VAL_NULL) &&
               (rv.type == VAL_UNDEF || rv.type == VAL_NULL);
    }
    if (lv.type == VAL_STR && rv.type == VAL_STR) {
        return strcmp(lv.u.str ? lv.u.str : "", rv.u.str ? rv.u.str : "") == 0;
    }
    if (lv.type == VAL_BOOL && rv.type == VAL_BOOL) {
        return lv.u.bval == rv.u.bval;
    }
    if (lv.type == VAL_OBJ || rv.type == VAL_OBJ) {
        return lv.type == VAL_OBJ && rv.type == VAL_OBJ && lv.u.obj == rv.u.obj;
    }
    if (lv.type == VAL_ARR || rv.type == VAL_ARR) {
        return lv.type == VAL_ARR && rv.type == VAL_ARR && lv.u.arr == rv.u.arr;
    }
    if (lv.type == VAL_VEC || rv.type == VAL_VEC) {
        return lv.type == VAL_VEC && rv.type == VAL_VEC && lv.u.vec == rv.u.vec;
    }
    if (lv.type == VAL_FUNC || rv.type == VAL_FUNC) {
        return lv.type == VAL_FUNC && rv.type == VAL_FUNC && lv.u.func == rv.u.func;
    }
    if (lv.type == VAL_BUILTIN_OBJ || rv.type == VAL_BUILTIN_OBJ) {
        return lv.type == VAL_BUILTIN_OBJ && rv.type == VAL_BUILTIN_OBJ &&
               lv.u.builtin_id == rv.u.builtin_id;
    }
    if (lv.type == VAL_SCENE_LAYER || rv.type == VAL_SCENE_LAYER) {
        return lv.type == VAL_SCENE_LAYER && rv.type == VAL_SCENE_LAYER &&
               lv.u.layer_slot == rv.u.layer_slot;
    }
    double a = val_to_num(lv);
    double b = val_to_num(rv);
    return fabs(a - b) <= 1e-9;
}

static void sync_bound_vec(we_script_t *vm, js_vec_obj_t *vo)
{
    if (!vm || !vo || vo->bind_tag == 0) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        vo->v[i] = sanitize_f(vo->v[i], 0.0f);
    }
    we_script_layer_state_t *target = &vm->layer;
    if (vo->layer_slot > 0 && (uint32_t)(vo->layer_slot - 1u) < WE_SCRIPT_MAX_SCENE_LAYERS) {
        we_script_scene_layer_slot_t *sl = &vm->scene_layers[vo->layer_slot - 1u];
        sl->accessed = true;
        target = &sl->state;
    }
    switch (vo->bind_tag) {
    case 1:
        memcpy(target->origin, vo->v, 3 * sizeof(float));
        target->dirty_mask |= WE_LAYER_DIRTY_ORIGIN;
        break;
    case 2:
        memcpy(target->scale, vo->v, 3 * sizeof(float));
        target->dirty_mask |= WE_LAYER_DIRTY_SCALE;
        break;
    case 3:
        memcpy(target->angles, vo->v, 3 * sizeof(float));
        target->dirty_mask |= WE_LAYER_DIRTY_ANGLES;
        break;
    case 4:
        memcpy(target->color, vo->v, 3 * sizeof(float));
        target->dirty_mask |= WE_LAYER_DIRTY_COLOR;
        break;
    case 5:
        memcpy(target->size, vo->v, 2 * sizeof(float));
        target->dirty_mask |= WE_LAYER_DIRTY_SIZE;
        break;
    default:
        break;
    }
}

static int vec_prop_index(const char *prop)
{
    if (!prop || prop[0] == '\0' || prop[1] != '\0') {
        return -1;
    }
    switch (prop[0]) {
    case 'x':
    case 'r':
        return 0;
    case 'y':
    case 'g':
        return 1;
    case 'z':
    case 'b':
        return 2;
    case 'w':
    case 'a':
        return 3;
    default:
        return -1;
    }
}

static void obj_set_prop(we_script_t *vm, js_obj_t *obj, const char *key, js_val_t val)
{
    if (!vm || !obj || !key) {
        return;
    }
    for (uint32_t i = 0; i < obj->count; i++) {
        if (obj->props[i].key && strcmp(obj->props[i].key, key) == 0) {
            obj->props[i].val = val;
            return;
        }
    }
    if (obj->count >= obj->cap) {
        uint32_t ncap = obj->cap == 0 ? 8u : obj->cap * 2u;
        js_prop_t *np = (js_prop_t *)script_arena_alloc(vm, ncap * sizeof(js_prop_t));
        if (!np) {
            return;
        }
        if (obj->count > 0 && obj->props) {
            memcpy(np, obj->props, obj->count * sizeof(js_prop_t));
        }
        obj->props = np;
        obj->cap = ncap;
    }
    obj->props[obj->count].key = script_arena_strdup(vm, key, strlen(key));
    obj->props[obj->count].val = val;
    obj->count++;
}

static js_val_t obj_get_prop(const js_obj_t *obj, const char *key)
{
    if (!obj || !key) {
        return val_undef();
    }
    for (uint32_t i = 0; i < obj->count; i++) {
        if (obj->props[i].key && strcmp(obj->props[i].key, key) == 0) {
            return obj->props[i].val;
        }
    }
    return val_undef();
}

static void shared_set_val(we_script_shared_t *sh, const char *key, js_val_t val)
{
    if (!sh || !key || key[0] == '\0') {
        return;
    }
    we_script_shared_entry_t *ent = NULL;
    for (uint32_t i = 0; i < sh->count; i++) {
        if (strcmp(sh->entries[i].key, key) == 0) {
            ent = &sh->entries[i];
            break;
        }
    }
    if (!ent) {
        if (sh->count >= WE_SCRIPT_MAX_SHARED_ENTRIES) {
            return;
        }
        ent = &sh->entries[sh->count++];
        memset(ent, 0, sizeof(*ent));
        snprintf(ent->key, sizeof(ent->key), "%s", key);
    }
    ent->is_bool = false;
    ent->is_str = false;
    if (val.type == VAL_VEC && val.u.vec) {
        ent->num_components = val.u.vec->dim;
        memcpy(ent->val, val.u.vec->v, 4 * sizeof(float));
    } else if (val.type == VAL_BOOL) {
        ent->num_components = 1;
        ent->is_bool = true;
        ent->val[0] = val.u.bval ? 1.0f : 0.0f;
    } else if (val.type == VAL_STR && val.u.str) {
        ent->num_components = 1;
        ent->is_str = true;
        snprintf(ent->str_val, sizeof(ent->str_val), "%s", val.u.str);
    } else {
        ent->num_components = 1;
        ent->val[0] = (float)val_to_num(val);
    }
}

static js_val_t shared_get_val(we_script_t *vm, const we_script_shared_t *sh, const char *key)
{
    if (!sh || !key) {
        return val_undef();
    }
    for (uint32_t i = 0; i < sh->count; i++) {
        const we_script_shared_entry_t *ent = &sh->entries[i];
        if (strcmp(ent->key, key) == 0) {
            if (ent->is_str) {
                return val_str(script_arena_strdup(vm, ent->str_val, strlen(ent->str_val)));
            }
            if (ent->is_bool) {
                return val_bool(ent->val[0] != 0.0f);
            }
            if (ent->num_components >= 2) {
                return val_new_vec(vm, (uint8_t)ent->num_components, ent->val[0], ent->val[1],
                                   ent->val[2], ent->val[3]);
            }
            return val_num((double)ent->val[0]);
        }
    }
    return val_undef();
}

static js_val_t construct_vec(we_script_t *vm, uint8_t dim, const js_val_t *args, uint32_t argc)
{
    float v[4] = {0.0f, 0.0f, 0.0f, dim == 4 ? 1.0f : 0.0f};
    if (argc == 1) {
        if (args[0].type == VAL_VEC && args[0].u.vec) {
            memcpy(v, args[0].u.vec->v, 4 * sizeof(float));
        } else if (args[0].type == VAL_STR && args[0].u.str) {
            const char *p = args[0].u.str;
            for (int i = 0; i < (int)dim && *p; i++) {
                while (*p && (isspace((unsigned char)*p) || *p == ',')) {
                    p++;
                }
                char *endp = NULL;
                float f = strtof(p, &endp);
                if (endp == p) {
                    break;
                }
                v[i] = f;
                p = endp;
            }
        } else {
            float s = (float)val_to_num(args[0]);
            v[0] = v[1] = v[2] = v[3] = s;
        }
    } else if (argc >= 2) {
        if (args[0].type == VAL_VEC && args[0].u.vec && args[0].u.vec->dim == 2) {
            v[0] = args[0].u.vec->v[0];
            v[1] = args[0].u.vec->v[1];
            v[2] = (float)val_to_num(args[1]);
            if (argc >= 3) {
                v[3] = (float)val_to_num(args[2]);
            }
        } else {
            for (uint32_t i = 0; i < argc && i < 4u; i++) {
                v[i] = (float)val_to_num(args[i]);
            }
        }
    }
    if (dim < 3) {
        v[2] = 0.0f;
    }
    if (dim < 4) {
        v[3] = 0.0f;
    }
    return val_new_vec(vm, dim, v[0], v[1], v[2], v[3]);
}

static void extract_other_vec(js_val_t arg, float out[4])
{
    if (arg.type == VAL_VEC && arg.u.vec) {
        memcpy(out, arg.u.vec->v, 4 * sizeof(float));
    } else {
        float s = sanitize_f((float)val_to_num(arg), 0.0f);
        out[0] = out[1] = out[2] = out[3] = s;
    }
}

static js_val_t call_vec_method(we_script_t *vm, js_vec_obj_t *vo, const char *method,
                                const js_val_t *args, uint32_t argc)
{
    if (!vo || !method) {
        return val_undef();
    }
    uint8_t d = vo->dim;
    float b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (argc >= 2 && args[0].type == VAL_NUM && args[1].type == VAL_NUM) {
        b[0] = (float)args[0].u.num;
        b[1] = (float)args[1].u.num;
        if (argc >= 3) {
            b[2] = (float)val_to_num(args[2]);
        }
        if (argc >= 4) {
            b[3] = (float)val_to_num(args[3]);
        }
    } else if (argc >= 1) {
        extract_other_vec(args[0], b);
    }

    if (strcmp(method, "add") == 0) {
        return val_new_vec(vm, d, vo->v[0] + b[0], vo->v[1] + b[1], d >= 3 ? vo->v[2] + b[2] : 0.0f,
                           d >= 4 ? vo->v[3] + b[3] : 0.0f);
    }
    if (strcmp(method, "subtract") == 0 || strcmp(method, "sub") == 0) {
        return val_new_vec(vm, d, vo->v[0] - b[0], vo->v[1] - b[1], d >= 3 ? vo->v[2] - b[2] : 0.0f,
                           d >= 4 ? vo->v[3] - b[3] : 0.0f);
    }
    if (strcmp(method, "multiply") == 0 || strcmp(method, "mul") == 0) {
        return val_new_vec(vm, d, vo->v[0] * b[0], vo->v[1] * b[1], d >= 3 ? vo->v[2] * b[2] : 0.0f,
                           d >= 4 ? vo->v[3] * b[3] : 0.0f);
    }
    if (strcmp(method, "divide") == 0 || strcmp(method, "div") == 0) {
        return val_new_vec(vm, d, fabsf(b[0]) > 1e-12f ? vo->v[0] / b[0] : 0.0f,
                           fabsf(b[1]) > 1e-12f ? vo->v[1] / b[1] : 0.0f,
                           (d >= 3 && fabsf(b[2]) > 1e-12f) ? vo->v[2] / b[2] : 0.0f,
                           (d >= 4 && fabsf(b[3]) > 1e-12f) ? vo->v[3] / b[3] : 0.0f);
    }
    if (strcmp(method, "lengthSqr") == 0) {
        double sum = 0.0;
        for (uint8_t i = 0; i < d; i++) {
            sum += (double)vo->v[i] * (double)vo->v[i];
        }
        return val_num(sum);
    }
    if (strcmp(method, "length") == 0) {
        double sum = 0.0;
        for (uint8_t i = 0; i < d; i++) {
            sum += (double)vo->v[i] * (double)vo->v[i];
        }
        return val_num(sqrt(sum));
    }
    if (strcmp(method, "distance") == 0) {
        double sum = 0.0;
        for (uint8_t i = 0; i < d; i++) {
            double diff = (double)vo->v[i] - (double)b[i];
            sum += diff * diff;
        }
        return val_num(sqrt(sum));
    }
    if (strcmp(method, "dot") == 0) {
        double sum = 0.0;
        for (uint8_t i = 0; i < d; i++) {
            sum += (double)vo->v[i] * (double)b[i];
        }
        return val_num(sum);
    }
    if (strcmp(method, "cross") == 0) {
        return val_new_vec(vm, 3, vo->v[1] * b[2] - vo->v[2] * b[1],
                           vo->v[2] * b[0] - vo->v[0] * b[2], vo->v[0] * b[1] - vo->v[1] * b[0],
                           0.0f);
    }
    if (strcmp(method, "normalize") == 0) {
        double sum = 0.0;
        for (uint8_t i = 0; i < d; i++) {
            sum += (double)vo->v[i] * (double)vo->v[i];
        }
        float len = (float)sqrt(sum);
        if (len <= 1e-12f) {
            return val_new_vec(vm, d, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        float inv = 1.0f / len;
        return val_new_vec(vm, d, vo->v[0] * inv, vo->v[1] * inv, d >= 3 ? vo->v[2] * inv : 0.0f,
                           d >= 4 ? vo->v[3] * inv : 0.0f);
    }
    if (strcmp(method, "copy") == 0 || strcmp(method, "clone") == 0) {
        return val_new_vec(vm, d, vo->v[0], vo->v[1], d >= 3 ? vo->v[2] : 0.0f,
                           d >= 4 ? vo->v[3] : 0.0f);
    }
    if (strcmp(method, "negate") == 0) {
        return val_new_vec(vm, d, -vo->v[0], -vo->v[1], d >= 3 ? -vo->v[2] : 0.0f,
                           d >= 4 ? -vo->v[3] : 0.0f);
    }
    if (strcmp(method, "min") == 0) {
        return val_new_vec(vm, d, fminf(vo->v[0], b[0]), fminf(vo->v[1], b[1]),
                           d >= 3 ? fminf(vo->v[2], b[2]) : 0.0f,
                           d >= 4 ? fminf(vo->v[3], b[3]) : 0.0f);
    }
    if (strcmp(method, "max") == 0) {
        return val_new_vec(vm, d, fmaxf(vo->v[0], b[0]), fmaxf(vo->v[1], b[1]),
                           d >= 3 ? fmaxf(vo->v[2], b[2]) : 0.0f,
                           d >= 4 ? fmaxf(vo->v[3], b[3]) : 0.0f);
    }
    if (strcmp(method, "mix") == 0 || strcmp(method, "lerp") == 0) {
        float t = (argc >= 2) ? (float)val_to_num(args[1]) : 0.5f;
        return val_new_vec(vm, d, vo->v[0] + t * (b[0] - vo->v[0]),
                           vo->v[1] + t * (b[1] - vo->v[1]),
                           d >= 3 ? vo->v[2] + t * (b[2] - vo->v[2]) : 0.0f,
                           d >= 4 ? vo->v[3] + t * (b[3] - vo->v[3]) : 0.0f);
    }
    if (strcmp(method, "toString") == 0) {
        char buf[128];
        if (d == 2) {
            snprintf(buf, sizeof(buf), "%g %g", vo->v[0], vo->v[1]);
        } else if (d == 3) {
            snprintf(buf, sizeof(buf), "%g %g %g", vo->v[0], vo->v[1], vo->v[2]);
        } else {
            snprintf(buf, sizeof(buf), "%g %g %g %g", vo->v[0], vo->v[1], vo->v[2], vo->v[3]);
        }
        return val_str(script_arena_strdup(vm, buf, strlen(buf)));
    }
    return val_undef();
}

static js_val_t call_builtin_method(we_script_t *vm, js_builtin_id_t bid, const char *method,
                                    const js_val_t *args, uint32_t argc)
{
    double a0 = (argc >= 1) ? val_to_num(args[0]) : 0.0;
    double a1 = (argc >= 2) ? val_to_num(args[1]) : 0.0;
    double a2 = (argc >= 3) ? val_to_num(args[2]) : 0.0;

    if (bid == BUILTIN_MATH) {
        if (strcmp(method, "sin") == 0)
            return val_num(sin(a0));
        if (strcmp(method, "cos") == 0)
            return val_num(cos(a0));
        if (strcmp(method, "tan") == 0)
            return val_num(tan(a0));
        if (strcmp(method, "asin") == 0)
            return val_num(asin(fmin(fmax(a0, -1.0), 1.0)));
        if (strcmp(method, "acos") == 0)
            return val_num(acos(fmin(fmax(a0, -1.0), 1.0)));
        if (strcmp(method, "atan") == 0)
            return val_num(atan(a0));
        if (strcmp(method, "atan2") == 0)
            return val_num(atan2(a0, a1));
        if (strcmp(method, "abs") == 0)
            return val_num(fabs(a0));
        if (strcmp(method, "min") == 0) {
            if (argc == 0)
                return val_num(0.0);
            double m = a0;
            for (uint32_t i = 1; i < argc; i++) {
                double v = val_to_num(args[i]);
                if (v < m)
                    m = v;
            }
            return val_num(m);
        }
        if (strcmp(method, "max") == 0) {
            if (argc == 0)
                return val_num(0.0);
            double m = a0;
            for (uint32_t i = 1; i < argc; i++) {
                double v = val_to_num(args[i]);
                if (v > m)
                    m = v;
            }
            return val_num(m);
        }
        if (strcmp(method, "floor") == 0)
            return val_num(floor(a0));
        if (strcmp(method, "ceil") == 0)
            return val_num(ceil(a0));
        if (strcmp(method, "round") == 0)
            return val_num(round(a0));
        if (strcmp(method, "trunc") == 0)
            return val_num(trunc(a0));
        if (strcmp(method, "sign") == 0)
            return val_num(a0 > 0.0 ? 1.0 : (a0 < 0.0 ? -1.0 : 0.0));
        if (strcmp(method, "sqrt") == 0)
            return val_num(a0 >= 0.0 ? sqrt(a0) : 0.0);
        if (strcmp(method, "cbrt") == 0)
            return val_num(cbrt(a0));
        if (strcmp(method, "hypot") == 0)
            return val_num(hypot(a0, a1));
        if (strcmp(method, "pow") == 0)
            return val_num(pow(a0, a1));
        if (strcmp(method, "exp") == 0)
            return val_num(exp(a0));
        if (strcmp(method, "log") == 0)
            return val_num(a0 > 0.0 ? log(a0) : 0.0);
        if (strcmp(method, "log2") == 0)
            return val_num(a0 > 0.0 ? log2(a0) : 0.0);
        if (strcmp(method, "log10") == 0)
            return val_num(a0 > 0.0 ? log10(a0) : 0.0);
        if (strcmp(method, "random") == 0) {
            uint64_t x = vm->rng_state;
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            vm->rng_state = x;
            return val_num((double)((x * 0x2545F4914F6CDD1DULL) >> 40) / 16777216.0);
        }
    } else if (bid == BUILTIN_WEMATH) {
        if (strcmp(method, "deg2rad") == 0)
            return val_num(a0 * (M_PI / 180.0));
        if (strcmp(method, "rad2deg") == 0)
            return val_num(a0 * (180.0 / M_PI));
        if (strcmp(method, "smoothStep") == 0 || strcmp(method, "smoothstep") == 0) {
            double span = a1 - a0;
            double t = (fabs(span) > 1e-12) ? ((a2 - a0) / span) : 0.0;
            t = fmin(fmax(t, 0.0), 1.0);
            return val_num(t * t * (3.0 - 2.0 * t));
        }
        if (strcmp(method, "mix") == 0 || strcmp(method, "lerp") == 0) {
            if (argc >= 2 && args[0].type == VAL_VEC && args[0].u.vec) {
                return call_vec_method(vm, args[0].u.vec, "mix", args + 1, argc - 1);
            }
            return val_num(a0 + a2 * (a1 - a0));
        }
        if (strcmp(method, "clamp") == 0) {
            return val_num(fmin(fmax(a0, a1), a2));
        }
        if (strcmp(method, "mod") == 0) {
            if (fabs(a1) < 1e-12)
                return val_num(0.0);
            double r = fmod(a0, a1);
            if (r < 0.0)
                r += fabs(a1);
            return val_num(r);
        }
    } else if (bid == BUILTIN_WECOLOR) {
        float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (argc >= 1) {
            extract_other_vec(args[0], c);
        }
        if (strcmp(method, "hsv2rgb") == 0) {
            float h = c[0] - floorf(c[0]);
            float s = fminf(fmaxf(c[1], 0.0f), 1.0f);
            float v = fminf(fmaxf(c[2], 0.0f), 1.0f);
            float r = v, g = v, b = v;
            if (s > 0.0f) {
                float h6 = h * 6.0f;
                int sector = (int)floorf(h6) % 6;
                float f = h6 - floorf(h6);
                float p = v * (1.0f - s);
                float q = v * (1.0f - s * f);
                float t = v * (1.0f - s * (1.0f - f));
                switch (sector) {
                case 0:
                    r = v;
                    g = t;
                    b = p;
                    break;
                case 1:
                    r = q;
                    g = v;
                    b = p;
                    break;
                case 2:
                    r = p;
                    g = v;
                    b = t;
                    break;
                case 3:
                    r = p;
                    g = q;
                    b = v;
                    break;
                case 4:
                    r = t;
                    g = p;
                    b = v;
                    break;
                default:
                    r = v;
                    g = p;
                    b = q;
                    break;
                }
            }
            return val_new_vec(vm, 3, r, g, b, 0.0f);
        }
        if (strcmp(method, "rgb2hsv") == 0) {
            float r = c[0], g = c[1], b = c[2];
            float mx = fmaxf(r, fmaxf(g, b));
            float mn = fminf(r, fminf(g, b));
            float d = mx - mn;
            float h = 0.0f;
            float s = (mx > 1e-6f) ? (d / mx) : 0.0f;
            float v = mx;
            if (d > 1e-6f) {
                if (mx == r) {
                    h = (g - b) / d + (g < b ? 6.0f : 0.0f);
                } else if (mx == g) {
                    h = (b - r) / d + 2.0f;
                } else {
                    h = (r - g) / d + 4.0f;
                }
                h /= 6.0f;
            }
            return val_new_vec(vm, 3, h, s, v, 0.0f);
        }
        if (strcmp(method, "normalizeColor") == 0) {
            return val_new_vec(vm, 3, c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 0.0f);
        }
    } else if (bid == BUILTIN_ENGINE) {
        if (strcmp(method, "registerAudioBuffers") == 0) {
            vm->uses_audio = true;
            uint32_t res = (argc >= 1) ? (uint32_t)val_to_num(args[0]) : 64u;
            if (res != 16u && res != 32u && res != 64u) {
                res = 64u;
            }
            js_val_t ab;
            memset(&ab, 0, sizeof(ab));
            ab.type = VAL_AUDIO_BUFFERS;
            ab.u.audio.res = res;
            return ab;
        }
        if (strcmp(method, "timeOfDay") == 0) {
            return val_num((double)vm->env.time_of_day);
        }
        if (strcmp(method, "isRunningInEditor") == 0 || strcmp(method, "isMobileDevice") == 0) {
            return val_bool(false);
        }
        if (strcmp(method, "isDesktopDevice") == 0 || strcmp(method, "isWallpaper") == 0) {
            return val_bool(true);
        }
    } else if (bid == BUILTIN_SCENE || bid == BUILTIN_THIS_LAYER) {
        if (strcmp(method, "getLayer") == 0 && argc >= 1 && args[0].type == VAL_STR &&
            args[0].u.str) {
            const char *lname = args[0].u.str;
            for (uint32_t i = 0; i < WE_SCRIPT_MAX_SCENE_LAYERS; i++) {
                if (vm->scene_layers[i].valid &&
                    strcmp(vm->scene_layers[i].state.name, lname) == 0) {
                    vm->scene_layers[i].accessed = true;
                    js_val_t v;
                    memset(&v, 0, sizeof(v));
                    v.type = VAL_SCENE_LAYER;
                    v.u.layer_slot = i;
                    return v;
                }
            }
            if (strcmp(vm->layer.name, lname) == 0) {
                return val_builtin(BUILTIN_THIS_LAYER);
            }
            return val_undef();
        }
    } else if (bid == BUILTIN_CONSOLE) {
        return val_undef();
    } else if (bid == BUILTIN_SCRIPT_PROPS_BUILDER) {
        return val_builtin(BUILTIN_SCRIPT_PROPS_BUILDER);
    }
    return val_undef();
}

static js_val_t get_scene_layer_member(we_script_t *vm, uint32_t slot, const char *prop)
{
    if (!vm || !prop || slot >= WE_SCRIPT_MAX_SCENE_LAYERS || !vm->scene_layers[slot].valid) {
        return val_undef();
    }
    we_script_scene_layer_slot_t *sl = &vm->scene_layers[slot];
    sl->accessed = true;
    if (strcmp(prop, "origin") == 0) {
        js_val_t v = {.type = VAL_VEC, .u.vec = &sl->origin_vec};
        return v;
    }
    if (strcmp(prop, "scale") == 0) {
        js_val_t v = {.type = VAL_VEC, .u.vec = &sl->scale_vec};
        return v;
    }
    if (strcmp(prop, "angles") == 0) {
        js_val_t v = {.type = VAL_VEC, .u.vec = &sl->angles_vec};
        return v;
    }
    if (strcmp(prop, "color") == 0) {
        js_val_t v = {.type = VAL_VEC, .u.vec = &sl->color_vec};
        return v;
    }
    if (strcmp(prop, "size") == 0) {
        js_val_t v = {.type = VAL_VEC, .u.vec = &sl->size_vec};
        return v;
    }
    if (strcmp(prop, "alpha") == 0)
        return val_num((double)sl->state.alpha);
    if (strcmp(prop, "visible") == 0)
        return val_bool(sl->state.visible);
    if (strcmp(prop, "name") == 0)
        return val_str(sl->state.name);
    if (strcmp(prop, "id") == 0)
        return val_num((double)sl->state.id);
    return val_undef();
}

static js_val_t get_builtin_member(we_script_t *vm, js_builtin_id_t bid, const char *prop)
{
    if (!prop) {
        return val_undef();
    }
    if (bid == BUILTIN_MATH) {
        if (strcmp(prop, "PI") == 0)
            return val_num(M_PI);
        if (strcmp(prop, "E") == 0)
            return val_num(M_E);
        if (strcmp(prop, "LN2") == 0)
            return val_num(0.6931471805599453);
        if (strcmp(prop, "LN10") == 0)
            return val_num(2.302585092994046);
        if (strcmp(prop, "SQRT2") == 0)
            return val_num(1.4142135623730951);
        if (strcmp(prop, "SQRT1_2") == 0)
            return val_num(0.7071067811865476);
    } else if (bid == BUILTIN_WEMATH) {
        if (strcmp(prop, "deg2rad") == 0)
            return val_num(M_PI / 180.0);
        if (strcmp(prop, "rad2deg") == 0)
            return val_num(180.0 / M_PI);
    } else if (bid == BUILTIN_ENGINE) {
        if (strcmp(prop, "frametime") == 0)
            return val_num((double)vm->env.frametime);
        if (strcmp(prop, "runtime") == 0)
            return val_num((double)vm->env.runtime);
        if (strcmp(prop, "timeOfDay") == 0)
            return val_num((double)vm->env.time_of_day);
        if (strcmp(prop, "screenResolution") == 0)
            return val_new_vec(vm, 2, vm->env.screen_resolution[0], vm->env.screen_resolution[1],
                               0.0f, 0.0f);
        if (strcmp(prop, "canvasSize") == 0)
            return val_new_vec(vm, 2, vm->env.canvas_size[0], vm->env.canvas_size[1], 0.0f, 0.0f);
        if (strcmp(prop, "isRunningInEditor") == 0 || strcmp(prop, "isMobileDevice") == 0)
            return val_bool(false);
        if (strcmp(prop, "isDesktopDevice") == 0 || strcmp(prop, "isWallpaper") == 0)
            return val_bool(true);
    } else if (bid == BUILTIN_INPUT) {
        if (strcmp(prop, "cursorWorldPosition") == 0)
            return val_new_vec(vm, 3, vm->env.cursor_world[0], vm->env.cursor_world[1],
                               vm->env.cursor_world[2], 0.0f);
        if (strcmp(prop, "cursorScreenPosition") == 0)
            return val_new_vec(vm, 2, vm->env.cursor_screen[0], vm->env.cursor_screen[1], 0.0f,
                               0.0f);
        if (strcmp(prop, "cursorLeftDown") == 0)
            return val_bool(vm->env.cursor_left_down);
    } else if (bid == BUILTIN_THIS_LAYER) {
        if (strcmp(prop, "origin") == 0) {
            js_val_t v = {.type = VAL_VEC, .u.vec = &vm->layer_origin_vec};
            return v;
        }
        if (strcmp(prop, "scale") == 0) {
            js_val_t v = {.type = VAL_VEC, .u.vec = &vm->layer_scale_vec};
            return v;
        }
        if (strcmp(prop, "angles") == 0) {
            js_val_t v = {.type = VAL_VEC, .u.vec = &vm->layer_angles_vec};
            return v;
        }
        if (strcmp(prop, "color") == 0) {
            js_val_t v = {.type = VAL_VEC, .u.vec = &vm->layer_color_vec};
            return v;
        }
        if (strcmp(prop, "size") == 0) {
            js_val_t v = {.type = VAL_VEC, .u.vec = &vm->layer_size_vec};
            return v;
        }
        if (strcmp(prop, "alpha") == 0)
            return val_num((double)vm->layer.alpha);
        if (strcmp(prop, "visible") == 0)
            return val_bool(vm->layer.visible);
        if (strcmp(prop, "name") == 0)
            return val_str(vm->layer.name);
        if (strcmp(prop, "id") == 0)
            return val_num((double)vm->layer.id);
        for (uint32_t i = 0; i < vm->this_prop_count; i++) {
            if (vm->this_props[i].name && strcmp(vm->this_props[i].name, prop) == 0) {
                return vm->this_props[i].val;
            }
        }
    } else if (bid == BUILTIN_SHARED) {
        return shared_get_val(vm, vm->shared, prop);
    }
    return val_undef();
}

static bool apply_layer_state_prop(we_script_layer_state_t *st, js_vec_obj_t *origin_vec,
                                   js_vec_obj_t *scale_vec, js_vec_obj_t *angles_vec,
                                   js_vec_obj_t *color_vec, js_vec_obj_t *size_vec,
                                   const char *prop, js_val_t rhs)
{
    float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    extract_other_vec(rhs, tmp);
    for (int i = 0; i < 4; i++) {
        tmp[i] = sanitize_f(tmp[i], 0.0f);
    }
    if (strcmp(prop, "origin") == 0) {
        memcpy(st->origin, tmp, 3 * sizeof(float));
        memcpy(origin_vec->v, tmp, 3 * sizeof(float));
        st->dirty_mask |= WE_LAYER_DIRTY_ORIGIN;
        return true;
    }
    if (strcmp(prop, "scale") == 0) {
        memcpy(st->scale, tmp, 3 * sizeof(float));
        memcpy(scale_vec->v, tmp, 3 * sizeof(float));
        st->dirty_mask |= WE_LAYER_DIRTY_SCALE;
        return true;
    }
    if (strcmp(prop, "angles") == 0) {
        memcpy(st->angles, tmp, 3 * sizeof(float));
        memcpy(angles_vec->v, tmp, 3 * sizeof(float));
        st->dirty_mask |= WE_LAYER_DIRTY_ANGLES;
        return true;
    }
    if (strcmp(prop, "color") == 0) {
        memcpy(st->color, tmp, 3 * sizeof(float));
        memcpy(color_vec->v, tmp, 3 * sizeof(float));
        st->dirty_mask |= WE_LAYER_DIRTY_COLOR;
        return true;
    }
    if (strcmp(prop, "size") == 0) {
        memcpy(st->size, tmp, 2 * sizeof(float));
        memcpy(size_vec->v, tmp, 2 * sizeof(float));
        st->dirty_mask |= WE_LAYER_DIRTY_SIZE;
        return true;
    }
    if (strcmp(prop, "alpha") == 0) {
        st->alpha = sanitize_f((float)val_to_num(rhs), 1.0f);
        st->dirty_mask |= WE_LAYER_DIRTY_ALPHA;
        return true;
    }
    if (strcmp(prop, "visible") == 0) {
        st->visible = val_to_bool(rhs);
        st->dirty_mask |= WE_LAYER_DIRTY_VISIBLE;
        return true;
    }
    return false;
}

static void set_scene_layer_member(we_script_t *vm, uint32_t slot, const char *prop, js_val_t rhs)
{
    if (!vm || !prop || slot >= WE_SCRIPT_MAX_SCENE_LAYERS || !vm->scene_layers[slot].valid) {
        return;
    }
    we_script_scene_layer_slot_t *sl = &vm->scene_layers[slot];
    sl->accessed = true;
    (void)apply_layer_state_prop(&sl->state, &sl->origin_vec, &sl->scale_vec, &sl->angles_vec,
                                 &sl->color_vec, &sl->size_vec, prop, rhs);
}

static void set_layer_member(we_script_t *vm, const char *prop, js_val_t rhs)
{
    if (!prop) {
        return;
    }
    if (apply_layer_state_prop(&vm->layer, &vm->layer_origin_vec, &vm->layer_scale_vec,
                               &vm->layer_angles_vec, &vm->layer_color_vec, &vm->layer_size_vec,
                               prop, rhs)) {
        return;
    }
    for (uint32_t i = 0; i < vm->this_prop_count; i++) {
        if (vm->this_props[i].name && strcmp(vm->this_props[i].name, prop) == 0) {
            vm->this_props[i].val = rhs;
            return;
        }
    }
    if (vm->this_prop_count < WE_SCRIPT_MAX_PROPS) {
        vm->this_props[vm->this_prop_count].name = script_arena_strdup(vm, prop, strlen(prop));
        vm->this_props[vm->this_prop_count].val = rhs;
        vm->this_prop_count++;
    }
}

/* =========================================================================
 * AST Evaluator
 * ========================================================================= */

typedef enum {
    FLOW_NORMAL = 0,
    FLOW_RETURN,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_THROW,
} exec_flow_t;

typedef struct exec_scope {
    struct exec_scope *parent;
    js_var_slot_t vars[WE_SCRIPT_MAX_LOCALS];
    uint32_t count;
    exec_flow_t flow;
    js_val_t ret_val;
    uint32_t call_depth;
} exec_scope_t;

static js_var_slot_t *find_var_slot(we_script_t *vm, exec_scope_t *scope, const char *name)
{
    if (!name) {
        return NULL;
    }
    for (exec_scope_t *s = scope; s != NULL; s = s->parent) {
        for (uint32_t i = 0; i < s->count; i++) {
            if (s->vars[i].name && strcmp(s->vars[i].name, name) == 0) {
                return &s->vars[i];
            }
        }
    }
    for (uint32_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].name && strcmp(vm->globals[i].name, name) == 0) {
            return &vm->globals[i];
        }
    }
    return NULL;
}

static js_var_slot_t *declare_var_slot(we_script_t *vm, exec_scope_t *scope, const char *name)
{
    if (!name) {
        return NULL;
    }
    if (scope) {
        for (uint32_t i = 0; i < scope->count; i++) {
            if (scope->vars[i].name && strcmp(scope->vars[i].name, name) == 0) {
                return &scope->vars[i];
            }
        }
        if (scope->count < WE_SCRIPT_MAX_LOCALS) {
            js_var_slot_t *sl = &scope->vars[scope->count++];
            sl->name = name;
            sl->val = val_undef();
            return sl;
        }
    }
    for (uint32_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].name && strcmp(vm->globals[i].name, name) == 0) {
            return &vm->globals[i];
        }
    }
    if (vm->global_count < WE_SCRIPT_MAX_GLOBALS) {
        js_var_slot_t *sl = &vm->globals[vm->global_count++];
        sl->name = name;
        sl->val = (strcmp(name, "shared") == 0) ? val_builtin(BUILTIN_SHARED) : val_undef();
        return sl;
    }
    return NULL;
}

static js_val_t eval_node(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node);
static void exec_stmt(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node);

static js_val_t invoke_ast_func(we_script_t *vm, const ast_node_t *fn, const js_val_t *args,
                                uint32_t argc, uint32_t depth)
{
    if (!vm || !fn || !vm_check_watchdog(vm)) {
        return val_undef();
    }
    if (depth > 32u) {
        vm->depth_exceeded = true;
        vm->aborted = true;
        return val_undef();
    }
    exec_scope_t fscope;
    memset(&fscope, 0, sizeof(fscope));
    fscope.parent = NULL;
    fscope.call_depth = depth;

    for (uint32_t i = 0; i < fn->param_count && i < WE_SCRIPT_MAX_LOCALS; i++) {
        fscope.vars[fscope.count].name = fn->params[i];
        fscope.vars[fscope.count].val = (i < argc) ? args[i] : val_undef();
        fscope.count++;
    }

    exec_stmt(vm, &fscope, fn->body);
    return (fscope.flow == FLOW_RETURN) ? fscope.ret_val : val_undef();
}

static js_val_t apply_assign_op(we_script_t *vm, js_tok_type_t op, js_val_t cur, js_val_t rhs)
{
    if (op == TOK_ASSIGN) {
        return rhs;
    }
    if (op == TOK_PLUS_ASSIGN && (cur.type == VAL_STR || rhs.type == VAL_STR)) {
        char s1[256];
        char s2[256];
        char buf[512];
        val_to_str_buf(cur, s1, sizeof(s1));
        val_to_str_buf(rhs, s2, sizeof(s2));
        snprintf(buf, sizeof(buf), "%s%s", s1, s2);
        return val_str(script_arena_strdup(vm, buf, strlen(buf)));
    }
    double a = val_to_num(cur);
    double b = val_to_num(rhs);
    switch (op) {
    case TOK_PLUS_ASSIGN:
        return val_num(a + b);
    case TOK_MINUS_ASSIGN:
        return val_num(a - b);
    case TOK_STAR_ASSIGN:
        return val_num(a * b);
    case TOK_SLASH_ASSIGN:
        return val_num(fabs(b) > 1e-12 ? (a / b) : 0.0);
    case TOK_PERCENT_ASSIGN:
        return val_num(fabs(b) > 1e-12 ? fmod(a, b) : 0.0);
    case TOK_SHL_ASSIGN:
        return val_num((double)((int32_t)a << ((int32_t)b & 31)));
    case TOK_SHR_ASSIGN:
        return val_num((double)((int32_t)a >> ((int32_t)b & 31)));
    case TOK_USHR_ASSIGN:
        return val_num((double)((uint32_t)(int32_t)a >> ((uint32_t)(int32_t)b & 31u)));
    case TOK_AND_ASSIGN:
        return val_num((double)((int32_t)a & (int32_t)b));
    case TOK_OR_ASSIGN:
        return val_num((double)((int32_t)a | (int32_t)b));
    case TOK_XOR_ASSIGN:
        return val_num((double)((int32_t)a ^ (int32_t)b));
    default:
        return rhs;
    }
}

static js_val_t eval_lvalue_update(we_script_t *vm, exec_scope_t *scope, const ast_node_t *lhs,
                                   js_tok_type_t op, js_val_t rhs_val, bool is_inc_dec, bool prefix)
{
    if (!lhs) {
        return val_undef();
    }
    if (lhs->type == AST_IDENT && lhs->name) {
        js_var_slot_t *sl = find_var_slot(vm, scope, lhs->name);
        if (!sl) {
            /* Undeclared identifier assignment binds to global scope per JS semantics */
            sl = declare_var_slot(vm, NULL, lhs->name);
        }
        if (!sl) {
            return val_undef();
        }
        js_val_t old_v = sl->val;
        js_val_t new_v;
        if (is_inc_dec) {
            double d = val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0);
            new_v = val_num(d);
        } else {
            new_v = apply_assign_op(vm, op, old_v, rhs_val);
        }
        sl->val = new_v;
        if (strcmp(lhs->name, "shared") == 0) {
            if (new_v.type == VAL_OBJ && new_v.u.obj && vm->shared) {
                for (uint32_t i = 0; i < new_v.u.obj->count; i++) {
                    shared_set_val(vm->shared, new_v.u.obj->props[i].key,
                                   new_v.u.obj->props[i].val);
                }
            }
            if (!scope || (sl >= vm->globals && sl < vm->globals + vm->global_count)) {
                sl->val = val_builtin(BUILTIN_SHARED);
            }
        }
        return (is_inc_dec && !prefix) ? old_v : new_v;
    }
    if (lhs->type == AST_MEMBER && lhs->left && lhs->name) {
        js_val_t obj_v = eval_node(vm, scope, lhs->left);
        if (obj_v.type == VAL_VEC && obj_v.u.vec) {
            int cidx = vec_prop_index(lhs->name);
            if (cidx >= 0 && cidx < 4) {
                js_val_t old_v = val_num((double)obj_v.u.vec->v[cidx]);
                js_val_t new_v = is_inc_dec ? val_num(old_v.u.num + (op == TOK_INC ? 1.0 : -1.0))
                                            : apply_assign_op(vm, op, old_v, rhs_val);
                obj_v.u.vec->v[cidx] = sanitize_f((float)val_to_num(new_v), 0.0f);
                sync_bound_vec(vm, obj_v.u.vec);
                return (is_inc_dec && !prefix) ? old_v : new_v;
            }
        } else if (obj_v.type == VAL_BUILTIN_OBJ && obj_v.u.builtin_id == BUILTIN_THIS_LAYER) {
            js_val_t old_v = get_builtin_member(vm, BUILTIN_THIS_LAYER, lhs->name);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            set_layer_member(vm, lhs->name, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (obj_v.type == VAL_SCENE_LAYER) {
            js_val_t old_v = get_scene_layer_member(vm, obj_v.u.layer_slot, lhs->name);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            set_scene_layer_member(vm, obj_v.u.layer_slot, lhs->name, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (obj_v.type == VAL_BUILTIN_OBJ && obj_v.u.builtin_id == BUILTIN_SHARED) {
            js_val_t old_v = shared_get_val(vm, vm->shared, lhs->name);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            shared_set_val(vm->shared, lhs->name, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (obj_v.type == VAL_OBJ && obj_v.u.obj) {
            js_val_t old_v = obj_get_prop(obj_v.u.obj, lhs->name);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            obj_set_prop(vm, obj_v.u.obj, lhs->name, new_v);
            if (lhs->left->type == AST_IDENT && lhs->left->name &&
                strcmp(lhs->left->name, "shared") == 0) {
                shared_set_val(vm->shared, lhs->name, new_v);
            }
            return (is_inc_dec && !prefix) ? old_v : new_v;
        }
    }
    if (lhs->type == AST_INDEX && lhs->left && lhs->right) {
        js_val_t base = eval_node(vm, scope, lhs->left);
        js_val_t idx_v = eval_node(vm, scope, lhs->right);
        if (base.type == VAL_ARR && base.u.arr) {
            int32_t idx = (int32_t)val_to_num(idx_v);
            if (idx >= 0 && idx < 4096) {
                js_arr_t *arr = base.u.arr;
                if ((uint32_t)idx >= arr->cap) {
                    uint32_t ncap = (uint32_t)idx + 8u;
                    js_val_t *ni = (js_val_t *)script_arena_alloc(vm, ncap * sizeof(js_val_t));
                    if (ni) {
                        if (arr->count > 0 && arr->items) {
                            memcpy(ni, arr->items, arr->count * sizeof(js_val_t));
                        }
                        arr->items = ni;
                        arr->cap = ncap;
                    }
                }
                if ((uint32_t)idx < arr->cap) {
                    if ((uint32_t)idx >= arr->count) {
                        arr->count = (uint32_t)idx + 1u;
                    }
                    js_val_t old_v = arr->items[idx];
                    js_val_t new_v = is_inc_dec
                                         ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                         : apply_assign_op(vm, op, old_v, rhs_val);
                    arr->items[idx] = new_v;
                    return (is_inc_dec && !prefix) ? old_v : new_v;
                }
            }
        } else if (base.type == VAL_OBJ && base.u.obj && idx_v.type == VAL_STR && idx_v.u.str) {
            js_val_t old_v = obj_get_prop(base.u.obj, idx_v.u.str);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            obj_set_prop(vm, base.u.obj, idx_v.u.str, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (base.type == VAL_BUILTIN_OBJ && base.u.builtin_id == BUILTIN_SHARED &&
                   idx_v.type == VAL_STR && idx_v.u.str) {
            js_val_t old_v = shared_get_val(vm, vm->shared, idx_v.u.str);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            shared_set_val(vm->shared, idx_v.u.str, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (base.type == VAL_BUILTIN_OBJ && base.u.builtin_id == BUILTIN_THIS_LAYER &&
                   idx_v.type == VAL_STR && idx_v.u.str) {
            js_val_t old_v = get_builtin_member(vm, BUILTIN_THIS_LAYER, idx_v.u.str);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            set_layer_member(vm, idx_v.u.str, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        } else if (base.type == VAL_SCENE_LAYER && idx_v.type == VAL_STR && idx_v.u.str) {
            js_val_t old_v = get_scene_layer_member(vm, base.u.layer_slot, idx_v.u.str);
            js_val_t new_v = is_inc_dec ? val_num(val_to_num(old_v) + (op == TOK_INC ? 1.0 : -1.0))
                                        : apply_assign_op(vm, op, old_v, rhs_val);
            set_scene_layer_member(vm, base.u.layer_slot, idx_v.u.str, new_v);
            return (is_inc_dec && !prefix) ? old_v : new_v;
        }
    }
    return val_undef();
}

static void arr_push_item(we_script_t *vm, js_arr_t *arr, js_val_t item)
{
    if (!vm || !arr) {
        return;
    }
    if (arr->count >= arr->cap && arr->cap < 4096u) {
        uint32_t ncap = arr->cap == 0 ? 8u : arr->cap * 2u;
        if (ncap > 4096u) {
            ncap = 4096u;
        }
        js_val_t *ni = (js_val_t *)script_arena_alloc(vm, ncap * sizeof(js_val_t));
        if (ni) {
            if (arr->count > 0 && arr->items) {
                memcpy(ni, arr->items, arr->count * sizeof(js_val_t));
            }
            arr->items = ni;
            arr->cap = ncap;
        }
    }
    if (arr->count < arr->cap && arr->items) {
        arr->items[arr->count++] = item;
    }
}

static js_val_t call_arr_method(we_script_t *vm, exec_scope_t *scope, js_val_t recv,
                                const char *method, const js_val_t *args, uint32_t argc)
{
    js_arr_t *arr = recv.u.arr;
    if (!arr || !method) {
        return val_undef();
    }
    if (strcmp(method, "push") == 0) {
        for (uint32_t i = 0; i < argc; i++) {
            arr_push_item(vm, arr, args[i]);
        }
        return val_num((double)arr->count);
    }
    if (strcmp(method, "pop") == 0) {
        if (arr->count == 0 || !arr->items) {
            return val_undef();
        }
        return arr->items[--arr->count];
    }
    if (strcmp(method, "indexOf") == 0) {
        js_val_t needle = argc >= 1 ? args[0] : val_undef();
        int32_t from = argc >= 2 ? (int32_t)val_to_num(args[1]) : 0;
        if (from < 0) {
            from = (int32_t)arr->count + from;
            if (from < 0) {
                from = 0;
            }
        }
        for (uint32_t i = (uint32_t)from; i < arr->count; i++) {
            if (js_val_equals(arr->items[i], needle, true)) {
                return val_num((double)i);
            }
        }
        return val_num(-1.0);
    }
    if (strcmp(method, "includes") == 0) {
        js_val_t needle = argc >= 1 ? args[0] : val_undef();
        for (uint32_t i = 0; i < arr->count; i++) {
            if (js_val_equals(arr->items[i], needle, true)) {
                return val_bool(true);
            }
        }
        return val_bool(false);
    }
    if (strcmp(method, "slice") == 0) {
        int32_t len = (int32_t)arr->count;
        int32_t start = argc >= 1 ? (int32_t)val_to_num(args[0]) : 0;
        int32_t end = (argc >= 2 && args[1].type != VAL_UNDEF) ? (int32_t)val_to_num(args[1]) : len;
        if (start < 0) {
            start = len + start;
        }
        if (start < 0) {
            start = 0;
        }
        if (start > len) {
            start = len;
        }
        if (end < 0) {
            end = len + end;
        }
        if (end < start) {
            end = start;
        }
        if (end > len) {
            end = len;
        }
        js_arr_t *out = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
        if (!out) {
            return val_undef();
        }
        for (int32_t i = start; i < end; i++) {
            arr_push_item(vm, out, arr->items[i]);
        }
        js_val_t v = {.type = VAL_ARR, .u.arr = out};
        return v;
    }
    if (strcmp(method, "splice") == 0) {
        int32_t len = (int32_t)arr->count;
        int32_t start = argc >= 1 ? (int32_t)val_to_num(args[0]) : len;
        if (start < 0) {
            start = len + start;
        }
        if (start < 0) {
            start = 0;
        }
        if (start > len) {
            start = len;
        }
        int32_t del_cnt =
            (argc >= 2 && args[1].type != VAL_UNDEF) ? (int32_t)val_to_num(args[1]) : (len - start);
        if (del_cnt < 0) {
            del_cnt = 0;
        }
        if (start + del_cnt > len) {
            del_cnt = len - start;
        }
        js_arr_t *removed = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
        for (int32_t i = 0; i < del_cnt && removed; i++) {
            arr_push_item(vm, removed, arr->items[start + i]);
        }
        uint32_t ins_cnt = argc > 2u ? (argc - 2u) : 0u;
        uint32_t tail_cnt = (uint32_t)(len - (start + del_cnt));
        uint32_t new_cnt = (uint32_t)start + ins_cnt + tail_cnt;
        if (new_cnt <= 4096u) {
            js_val_t *ni =
                (js_val_t *)script_arena_alloc(vm, (new_cnt > 0 ? new_cnt : 8u) * sizeof(js_val_t));
            if (ni) {
                if (start > 0 && arr->items) {
                    memcpy(ni, arr->items, (uint32_t)start * sizeof(js_val_t));
                }
                for (uint32_t i = 0; i < ins_cnt; i++) {
                    ni[(uint32_t)start + i] = args[2u + i];
                }
                if (tail_cnt > 0 && arr->items) {
                    memcpy(ni + (uint32_t)start + ins_cnt, arr->items + start + del_cnt,
                           tail_cnt * sizeof(js_val_t));
                }
                arr->items = ni;
                arr->cap = new_cnt > 0 ? new_cnt : 8u;
                arr->count = new_cnt;
            }
        }
        js_val_t v = {.type = VAL_ARR, .u.arr = removed};
        return v;
    }
    if (strcmp(method, "concat") == 0) {
        js_arr_t *out = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
        if (!out) {
            return val_undef();
        }
        for (uint32_t i = 0; i < arr->count; i++) {
            arr_push_item(vm, out, arr->items[i]);
        }
        for (uint32_t a = 0; a < argc; a++) {
            if (args[a].type == VAL_ARR && args[a].u.arr) {
                for (uint32_t k = 0; k < args[a].u.arr->count; k++) {
                    arr_push_item(vm, out, args[a].u.arr->items[k]);
                }
            } else {
                arr_push_item(vm, out, args[a]);
            }
        }
        js_val_t v = {.type = VAL_ARR, .u.arr = out};
        return v;
    }
    if (strcmp(method, "join") == 0) {
        const char *sep =
            (argc >= 1 && args[0].type == VAL_STR && args[0].u.str) ? args[0].u.str : ",";
        char buf[1024];
        size_t pos = 0;
        buf[0] = '\0';
        for (uint32_t i = 0; i < arr->count; i++) {
            if (i > 0 && pos + strlen(sep) + 1u < sizeof(buf)) {
                size_t sl = strlen(sep);
                memcpy(buf + pos, sep, sl);
                pos += sl;
                buf[pos] = '\0';
            }
            char elem_s[256];
            if (arr->items[i].type == VAL_NULL || arr->items[i].type == VAL_UNDEF) {
                elem_s[0] = '\0';
            } else {
                val_to_str_buf(arr->items[i], elem_s, sizeof(elem_s));
            }
            size_t el = strlen(elem_s);
            if (pos + el + 1u < sizeof(buf)) {
                memcpy(buf + pos, elem_s, el);
                pos += el;
                buf[pos] = '\0';
            }
        }
        return val_str(script_arena_strdup(vm, buf, pos));
    }
    if ((strcmp(method, "forEach") == 0 || strcmp(method, "map") == 0 ||
         strcmp(method, "filter") == 0) &&
        argc >= 1 && args[0].type == VAL_FUNC && args[0].u.func) {
        bool is_map = (strcmp(method, "map") == 0);
        bool is_filter = (strcmp(method, "filter") == 0);
        js_arr_t *out = NULL;
        if (is_map || is_filter) {
            out = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
        }
        uint32_t d = scope ? (scope->call_depth + 1u) : 1u;
        uint32_t snap_count = arr->count;
        for (uint32_t i = 0; i < snap_count && !vm->aborted; i++) {
            js_val_t item = (i < arr->count) ? arr->items[i] : val_undef();
            js_val_t cb_args[3] = {item, val_num((double)i), recv};
            js_val_t r = invoke_ast_func(vm, args[0].u.func, cb_args, 3, d);
            if (is_map && out) {
                arr_push_item(vm, out, r);
            } else if (is_filter && out && val_to_bool(r)) {
                arr_push_item(vm, out, item);
            }
        }
        if (is_map || is_filter) {
            js_val_t v = {.type = VAL_ARR, .u.arr = out};
            return v;
        }
        return val_undef();
    }
    return val_undef();
}

static js_val_t eval_node_inner(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node)
{
    switch (node->type) {
    case AST_LITERAL:
        return node->lit;

    case AST_FUNC_DECL: {
        js_val_t v;
        memset(&v, 0, sizeof(v));
        v.type = VAL_FUNC;
        v.u.func = node;
        return v;
    }

    case AST_THIS:
        return val_builtin(BUILTIN_THIS_LAYER);

    case AST_IDENT: {
        if (!node->name) {
            return val_undef();
        }
        js_var_slot_t *sl = find_var_slot(vm, scope, node->name);
        if (sl) {
            return sl->val;
        }
        if (strcmp(node->name, "Math") == 0)
            return val_builtin(BUILTIN_MATH);
        if (strcmp(node->name, "WEMath") == 0)
            return val_builtin(BUILTIN_WEMATH);
        if (strcmp(node->name, "WEColor") == 0)
            return val_builtin(BUILTIN_WECOLOR);
        if (strcmp(node->name, "engine") == 0)
            return val_builtin(BUILTIN_ENGINE);
        if (strcmp(node->name, "input") == 0)
            return val_builtin(BUILTIN_INPUT);
        if (strcmp(node->name, "thisLayer") == 0 || strcmp(node->name, "thisObject") == 0)
            return val_builtin(BUILTIN_THIS_LAYER);
        if (strcmp(node->name, "thisScene") == 0)
            return val_builtin(BUILTIN_SCENE);
        if (strcmp(node->name, "shared") == 0)
            return val_builtin(BUILTIN_SHARED);
        if (strcmp(node->name, "console") == 0)
            return val_builtin(BUILTIN_CONSOLE);
        if (strcmp(node->name, "NaN") == 0)
            return val_num(0.0);
        if (strcmp(node->name, "Infinity") == 0)
            return val_num(1e30);
        return val_undef();
    }

    case AST_ARRAY_LIT: {
        js_arr_t *arr = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
        if (!arr) {
            return val_undef();
        }
        if (node->child_count > 0) {
            arr->cap = node->child_count;
            arr->items = (js_val_t *)script_arena_alloc(vm, arr->cap * sizeof(js_val_t));
            if (arr->items) {
                for (uint32_t i = 0; i < node->child_count; i++) {
                    arr->items[i] = eval_node(vm, scope, node->children[i]);
                }
                arr->count = node->child_count;
            }
        }
        js_val_t v = {.type = VAL_ARR, .u.arr = arr};
        return v;
    }

    case AST_OBJ_LIT: {
        js_obj_t *obj = (js_obj_t *)script_arena_alloc(vm, sizeof(js_obj_t));
        if (!obj) {
            return val_undef();
        }
        for (uint32_t i = 0; i < node->child_count; i++) {
            js_val_t pv = eval_node(vm, scope, node->children[i]);
            obj_set_prop(vm, obj, node->keys[i], pv);
        }
        js_val_t v = {.type = VAL_OBJ, .u.obj = obj};
        return v;
    }

    case AST_NEW: {
        js_val_t args[16];
        uint32_t argc = node->child_count < 16u ? node->child_count : 16u;
        for (uint32_t i = 0; i < argc; i++) {
            args[i] = eval_node(vm, scope, node->children[i]);
        }
        if (node->name) {
            if (strcmp(node->name, "Vec2") == 0)
                return construct_vec(vm, 2, args, argc);
            if (strcmp(node->name, "Vec3") == 0)
                return construct_vec(vm, 3, args, argc);
            if (strcmp(node->name, "Vec4") == 0)
                return construct_vec(vm, 4, args, argc);
            if (strcmp(node->name, "Array") == 0) {
                js_arr_t *arr = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
                if (arr && argc == 1 && args[0].type == VAL_NUM && args[0].u.num > 0) {
                    uint32_t len = (uint32_t)fmin(args[0].u.num, 1024.0);
                    arr->cap = len;
                    arr->count = len;
                    arr->items = (js_val_t *)script_arena_alloc(vm, len * sizeof(js_val_t));
                }
                js_val_t v = {.type = VAL_ARR, .u.arr = arr};
                return v;
            }
            if (strcmp(node->name, "Object") == 0) {
                js_obj_t *obj = (js_obj_t *)script_arena_alloc(vm, sizeof(js_obj_t));
                js_val_t v = {.type = VAL_OBJ, .u.obj = obj};
                return v;
            }
        }
        return val_undef();
    }

    case AST_MEMBER: {
        js_val_t base = eval_node(vm, scope, node->left);
        const char *prop = node->name ? node->name : "";
        if (base.type == VAL_VEC && base.u.vec) {
            int cidx = vec_prop_index(prop);
            if (cidx >= 0 && cidx < 4) {
                return val_num((double)base.u.vec->v[cidx]);
            }
        } else if (base.type == VAL_BUILTIN_OBJ) {
            return get_builtin_member(vm, base.u.builtin_id, prop);
        } else if (base.type == VAL_SCENE_LAYER) {
            return get_scene_layer_member(vm, base.u.layer_slot, prop);
        } else if (base.type == VAL_AUDIO_BUFFERS) {
            js_val_t ch;
            memset(&ch, 0, sizeof(ch));
            ch.type = VAL_AUDIO_CHANNEL;
            ch.u.audio.res = base.u.audio.res;
            if (strcmp(prop, "left") == 0) {
                ch.u.audio.channel = 1;
                return ch;
            }
            if (strcmp(prop, "right") == 0) {
                ch.u.audio.channel = 2;
                return ch;
            }
            if (strcmp(prop, "average") == 0) {
                ch.u.audio.channel = 0;
                return ch;
            }
            if (strcmp(prop, "resolution") == 0) {
                return val_num((double)base.u.audio.res);
            }
        } else if (base.type == VAL_AUDIO_CHANNEL) {
            if (strcmp(prop, "length") == 0) {
                return val_num((double)base.u.audio.res);
            }
        } else if (base.type == VAL_ARR && base.u.arr) {
            if (strcmp(prop, "length") == 0) {
                return val_num((double)base.u.arr->count);
            }
        } else if (base.type == VAL_STR && base.u.str) {
            if (strcmp(prop, "length") == 0) {
                return val_num((double)strlen(base.u.str));
            }
        } else if (base.type == VAL_OBJ && base.u.obj) {
            return obj_get_prop(base.u.obj, prop);
        }
        return val_undef();
    }

    case AST_INDEX: {
        js_val_t base = eval_node(vm, scope, node->left);
        js_val_t idx_v = eval_node(vm, scope, node->right);
        if (base.type == VAL_AUDIO_CHANNEL) {
            int32_t i = (int32_t)val_to_num(idx_v);
            uint32_t res = base.u.audio.res > 0 ? base.u.audio.res : 64u;
            if (i >= 0 && (uint32_t)i < res) {
                uint32_t band = ((uint32_t)i * 64u) / res;
                if (band >= 64u) {
                    band = 63u;
                }
                if (base.u.audio.channel == 1)
                    return val_num((double)vm->env.audio_left[band]);
                if (base.u.audio.channel == 2)
                    return val_num((double)vm->env.audio_right[band]);
                return val_num((double)vm->env.audio_average[band]);
            }
            return val_num(0.0);
        }
        if (base.type == VAL_ARR && base.u.arr) {
            int32_t i = (int32_t)val_to_num(idx_v);
            if (i >= 0 && (uint32_t)i < base.u.arr->count) {
                return base.u.arr->items[i];
            }
            return val_undef();
        }
        if (base.type == VAL_VEC && base.u.vec) {
            int32_t i = (int32_t)val_to_num(idx_v);
            if (i >= 0 && i < 4) {
                return val_num((double)base.u.vec->v[i]);
            }
        }
        if (base.type == VAL_OBJ && base.u.obj && idx_v.type == VAL_STR && idx_v.u.str) {
            return obj_get_prop(base.u.obj, idx_v.u.str);
        }
        if (base.type == VAL_BUILTIN_OBJ && base.u.builtin_id == BUILTIN_SHARED &&
            idx_v.type == VAL_STR && idx_v.u.str) {
            return shared_get_val(vm, vm->shared, idx_v.u.str);
        }
        if (base.type == VAL_BUILTIN_OBJ && base.u.builtin_id == BUILTIN_THIS_LAYER &&
            idx_v.type == VAL_STR && idx_v.u.str) {
            return get_builtin_member(vm, BUILTIN_THIS_LAYER, idx_v.u.str);
        }
        if (base.type == VAL_SCENE_LAYER && idx_v.type == VAL_STR && idx_v.u.str) {
            return get_scene_layer_member(vm, base.u.layer_slot, idx_v.u.str);
        }
        return val_undef();
    }

    case AST_CALL: {
        js_val_t args[16];
        uint32_t argc = node->child_count < 16u ? node->child_count : 16u;
        for (uint32_t i = 0; i < argc; i++) {
            args[i] = eval_node(vm, scope, node->children[i]);
        }

        if (node->left && node->left->type == AST_MEMBER && node->left->left && node->left->name) {
            js_val_t recv = eval_node(vm, scope, node->left->left);
            const char *method = node->left->name;
            if (recv.type == VAL_VEC && recv.u.vec) {
                return call_vec_method(vm, recv.u.vec, method, args, argc);
            }
            if (recv.type == VAL_BUILTIN_OBJ) {
                return call_builtin_method(vm, recv.u.builtin_id, method, args, argc);
            }
            if (recv.type == VAL_ARR && recv.u.arr) {
                return call_arr_method(vm, scope, recv, method, args, argc);
            }
            if (recv.type == VAL_OBJ && recv.u.obj) {
                /* Support createScriptProperties().addSlider({...}).finish() */
                if (strncmp(method, "add", 3) == 0 && argc >= 1 && args[0].type == VAL_OBJ &&
                    args[0].u.obj) {
                    js_val_t k = obj_get_prop(args[0].u.obj, "name");
                    js_val_t v = obj_get_prop(args[0].u.obj, "value");
                    if (k.type == VAL_STR && k.u.str) {
                        obj_set_prop(vm, recv.u.obj, k.u.str, v);
                    }
                    return recv;
                }
                if (strcmp(method, "finish") == 0) {
                    return recv;
                }
                js_val_t mfn = obj_get_prop(recv.u.obj, method);
                if (mfn.type == VAL_FUNC && mfn.u.func) {
                    uint32_t d = scope ? (scope->call_depth + 1u) : 1u;
                    return invoke_ast_func(vm, mfn.u.func, args, argc, d);
                }
            }
            return val_undef();
        }

        if (node->left && node->left->type == AST_IDENT && node->left->name) {
            const char *fname = node->left->name;
            if (strcmp(fname, "Vec2") == 0)
                return construct_vec(vm, 2, args, argc);
            if (strcmp(fname, "Vec3") == 0)
                return construct_vec(vm, 3, args, argc);
            if (strcmp(fname, "Vec4") == 0)
                return construct_vec(vm, 4, args, argc);
            if (strcmp(fname, "parseFloat") == 0 || strcmp(fname, "Number") == 0)
                return val_num(argc >= 1 ? val_to_num(args[0]) : 0.0);
            if (strcmp(fname, "parseInt") == 0)
                return val_num(argc >= 1 ? trunc(val_to_num(args[0])) : 0.0);
            if (strcmp(fname, "isNaN") == 0)
                return val_bool(argc >= 1 ? isnan(val_to_num(args[0])) : false);
            if (strcmp(fname, "createScriptProperties") == 0) {
                js_obj_t *obj = (js_obj_t *)script_arena_alloc(vm, sizeof(js_obj_t));
                js_val_t v = {.type = VAL_OBJ, .u.obj = obj};
                return v;
            }
        }

        js_val_t callee = eval_node(vm, scope, node->left);
        if (callee.type == VAL_FUNC && callee.u.func) {
            uint32_t d = scope ? (scope->call_depth + 1u) : 1u;
            return invoke_ast_func(vm, callee.u.func, args, argc, d);
        }
        return val_undef();
    }

    case AST_UNARY: {
        js_val_t v = eval_node(vm, scope, node->left);
        switch (node->op) {
        case TOK_MINUS:
            return val_num(-val_to_num(v));
        case TOK_PLUS:
            return val_num(val_to_num(v));
        case TOK_NOT:
            return val_bool(!val_to_bool(v));
        case TOK_BIT_NOT:
            return val_num((double)(~(int32_t)val_to_num(v)));
        case TOK_TYPEOF:
            switch (v.type) {
            case VAL_NUM:
                return val_str("number");
            case VAL_BOOL:
                return val_str("boolean");
            case VAL_STR:
                return val_str("string");
            case VAL_FUNC:
                return val_str("function");
            case VAL_UNDEF:
                return val_str("undefined");
            default:
                return val_str("object");
            }
        default:
            return v;
        }
    }

    case AST_UPDATE:
        return eval_lvalue_update(vm, scope, node->left, node->op, val_undef(), true, node->prefix);

    case AST_ASSIGN: {
        js_val_t rhs = eval_node(vm, scope, node->right);
        return eval_lvalue_update(vm, scope, node->left, node->op, rhs, false, false);
    }

    case AST_TERNARY: {
        js_val_t c = eval_node(vm, scope, node->cond);
        return val_to_bool(c) ? eval_node(vm, scope, node->then_branch)
                              : eval_node(vm, scope, node->else_branch);
    }

    case AST_BINARY: {
        if (node->op == TOK_AND) {
            js_val_t lv = eval_node(vm, scope, node->left);
            if (!val_to_bool(lv)) {
                return lv;
            }
            return eval_node(vm, scope, node->right);
        }
        if (node->op == TOK_OR) {
            js_val_t lv = eval_node(vm, scope, node->left);
            if (val_to_bool(lv)) {
                return lv;
            }
            return eval_node(vm, scope, node->right);
        }
        if (node->op == TOK_NULLISH) {
            js_val_t lv = eval_node(vm, scope, node->left);
            if (lv.type != VAL_UNDEF && lv.type != VAL_NULL) {
                return lv;
            }
            return eval_node(vm, scope, node->right);
        }

        js_val_t lv = eval_node(vm, scope, node->left);
        js_val_t rv = eval_node(vm, scope, node->right);

        if (node->op == TOK_PLUS && (lv.type == VAL_STR || rv.type == VAL_STR)) {
            char s1[256];
            char s2[256];
            char buf[512];
            val_to_str_buf(lv, s1, sizeof(s1));
            val_to_str_buf(rv, s2, sizeof(s2));
            snprintf(buf, sizeof(buf), "%s%s", s1, s2);
            return val_str(script_arena_strdup(vm, buf, strlen(buf)));
        }

        if (node->op == TOK_EQ || node->op == TOK_STRICT_EQ || node->op == TOK_NEQ ||
            node->op == TOK_STRICT_NEQ) {
            bool strict = (node->op == TOK_STRICT_EQ || node->op == TOK_STRICT_NEQ);
            bool eq = js_val_equals(lv, rv, strict);
            return val_bool((node->op == TOK_EQ || node->op == TOK_STRICT_EQ) ? eq : !eq);
        }

        double a = val_to_num(lv);
        double b = val_to_num(rv);
        switch (node->op) {
        case TOK_PLUS:
            return val_num(a + b);
        case TOK_MINUS:
            return val_num(a - b);
        case TOK_STAR:
            return val_num(a * b);
        case TOK_SLASH:
            return val_num(fabs(b) > 1e-12 ? (a / b) : 0.0);
        case TOK_PERCENT:
            return val_num(fabs(b) > 1e-12 ? fmod(a, b) : 0.0);
        case TOK_LT:
            return val_bool(a < b);
        case TOK_LE:
            return val_bool(a <= b);
        case TOK_GT:
            return val_bool(a > b);
        case TOK_GE:
            return val_bool(a >= b);
        case TOK_BIT_AND:
            return val_num((double)((int32_t)a & (int32_t)b));
        case TOK_BIT_OR:
            return val_num((double)((int32_t)a | (int32_t)b));
        case TOK_BIT_XOR:
            return val_num((double)((int32_t)a ^ (int32_t)b));
        case TOK_SHL:
            return val_num((double)((int32_t)a << ((int32_t)b & 31)));
        case TOK_SHR:
            return val_num((double)((int32_t)a >> ((int32_t)b & 31)));
        case TOK_USHR:
            return val_num((double)((uint32_t)(int32_t)a >> ((uint32_t)(int32_t)b & 31u)));
        default:
            return val_undef();
        }
    }

    default:
        return val_undef();
    }
}

static js_val_t eval_node(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node)
{
    if (!node || !vm_check_watchdog(vm)) {
        return val_undef();
    }
    if (++vm->eval_depth > 128u) {
        vm->eval_depth--;
        vm->depth_exceeded = true;
        vm->aborted = true;
        return val_undef();
    }
    js_val_t res = eval_node_inner(vm, scope, node);
    vm->eval_depth--;
    return res;
}

static void exec_stmt_inner(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node)
{
    switch (node->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        for (uint32_t i = 0; i < node->child_count && !vm->aborted; i++) {
            exec_stmt(vm, scope, node->children[i]);
            if (scope && scope->flow != FLOW_NORMAL) {
                break;
            }
        }
        break;

    case AST_VAR_DECL:
        for (uint32_t i = 0; i < node->decl_count && !vm->aborted; i++) {
            js_var_slot_t *sl = declare_var_slot(vm, scope, node->decls[i].name);
            js_val_t init_v =
                node->decls[i].init ? eval_node(vm, scope, node->decls[i].init) : val_undef();
            if (sl && node->decls[i].init) {
                sl->val = init_v;
            }
            if (node->decls[i].name && strcmp(node->decls[i].name, "shared") == 0) {
                if (init_v.type == VAL_OBJ && init_v.u.obj && vm->shared) {
                    for (uint32_t k = 0; k < init_v.u.obj->count; k++) {
                        shared_set_val(vm->shared, init_v.u.obj->props[k].key,
                                       init_v.u.obj->props[k].val);
                    }
                }
                if (sl && (!scope || (sl >= vm->globals && sl < vm->globals + vm->global_count))) {
                    sl->val = val_builtin(BUILTIN_SHARED);
                }
            }
        }
        break;

    case AST_FUNC_DECL:
        if (node->name) {
            js_var_slot_t *sl = declare_var_slot(vm, scope, node->name);
            if (sl) {
                sl->val.type = VAL_FUNC;
                sl->val.u.func = node;
            }
            if (strcmp(node->name, "init") == 0) {
                vm->fn_init = node;
            } else if (strcmp(node->name, "update") == 0) {
                vm->fn_update = node;
            } else if (strcmp(node->name, "cursorMove") == 0) {
                vm->fn_cursor_move = node;
            } else if (strcmp(node->name, "applyUserProperties") == 0) {
                vm->fn_apply_user_props = node;
            }
        }
        break;

    case AST_IF:
        if (val_to_bool(eval_node(vm, scope, node->cond))) {
            exec_stmt(vm, scope, node->then_branch);
        } else if (node->else_branch) {
            exec_stmt(vm, scope, node->else_branch);
        }
        break;

    case AST_SWITCH: {
        js_val_t disc = eval_node(vm, scope, node->cond);
        int32_t match_idx = -1;
        int32_t default_idx = -1;
        if (node->case_exprs && node->children) {
            for (uint32_t i = 0; i < node->child_count && !vm->aborted; i++) {
                if (node->case_exprs[i] == NULL) {
                    if (default_idx < 0) {
                        default_idx = (int32_t)i;
                    }
                } else {
                    js_val_t cv = eval_node(vm, scope, node->case_exprs[i]);
                    if (js_val_equals(disc, cv, true)) {
                        match_idx = (int32_t)i;
                        break;
                    }
                }
            }
            if (match_idx < 0) {
                match_idx = default_idx;
            }
            if (match_idx >= 0) {
                for (uint32_t i = (uint32_t)match_idx; i < node->child_count && !vm->aborted; i++) {
                    exec_stmt(vm, scope, node->children[i]);
                    if (scope) {
                        if (scope->flow == FLOW_BREAK) {
                            scope->flow = FLOW_NORMAL;
                            break;
                        }
                        if (scope->flow != FLOW_NORMAL) {
                            break;
                        }
                    }
                }
            }
        }
        break;
    }

    case AST_TRY: {
        exec_stmt(vm, scope, node->body);
        if (scope && scope->flow == FLOW_THROW && !vm->aborted) {
            js_val_t thrown = scope->ret_val;
            scope->flow = FLOW_NORMAL;
            scope->ret_val = val_undef();
            if (node->else_branch) {
                if (node->name) {
                    js_var_slot_t *sl = declare_var_slot(vm, scope, node->name);
                    if (sl) {
                        sl->val = thrown;
                    }
                }
                exec_stmt(vm, scope, node->else_branch);
            }
        }
        if (node->step && !vm->aborted) {
            exec_flow_t saved_flow = scope ? scope->flow : FLOW_NORMAL;
            js_val_t saved_ret = scope ? scope->ret_val : val_undef();
            if (scope) {
                scope->flow = FLOW_NORMAL;
            }
            exec_stmt(vm, scope, node->step);
            if (scope && scope->flow == FLOW_NORMAL) {
                scope->flow = saved_flow;
                scope->ret_val = saved_ret;
            }
        }
        break;
    }

    case AST_THROW:
        if (scope) {
            scope->ret_val = node->left ? eval_node(vm, scope, node->left) : val_undef();
            scope->flow = FLOW_THROW;
        }
        break;

    case AST_WHILE:
        while (!vm->aborted && val_to_bool(eval_node(vm, scope, node->cond))) {
            exec_stmt(vm, scope, node->body);
            if (scope) {
                if (scope->flow == FLOW_BREAK) {
                    scope->flow = FLOW_NORMAL;
                    break;
                }
                if (scope->flow == FLOW_CONTINUE) {
                    scope->flow = FLOW_NORMAL;
                } else if (scope->flow != FLOW_NORMAL) {
                    break;
                }
            }
        }
        break;

    case AST_DO_WHILE:
        do {
            exec_stmt(vm, scope, node->body);
            if (scope) {
                if (scope->flow == FLOW_BREAK) {
                    scope->flow = FLOW_NORMAL;
                    break;
                }
                if (scope->flow == FLOW_CONTINUE) {
                    scope->flow = FLOW_NORMAL;
                } else if (scope->flow != FLOW_NORMAL) {
                    break;
                }
            }
        } while (!vm->aborted && val_to_bool(eval_node(vm, scope, node->cond)));
        break;

    case AST_FOR:
        if (node->init) {
            if (node->init->type == AST_VAR_DECL) {
                exec_stmt(vm, scope, node->init);
            } else {
                (void)eval_node(vm, scope, node->init);
            }
        }
        while (!vm->aborted && (!node->cond || val_to_bool(eval_node(vm, scope, node->cond)))) {
            exec_stmt(vm, scope, node->body);
            if (scope) {
                if (scope->flow == FLOW_BREAK) {
                    scope->flow = FLOW_NORMAL;
                    break;
                }
                if (scope->flow == FLOW_CONTINUE) {
                    scope->flow = FLOW_NORMAL;
                } else if (scope->flow != FLOW_NORMAL) {
                    break;
                }
            }
            if (node->step && !vm->aborted) {
                (void)eval_node(vm, scope, node->step);
            }
        }
        break;

    case AST_RETURN:
        if (scope) {
            scope->ret_val = node->left ? eval_node(vm, scope, node->left) : val_undef();
            scope->flow = FLOW_RETURN;
        }
        break;

    case AST_BREAK:
        if (scope) {
            scope->flow = FLOW_BREAK;
        }
        break;

    case AST_CONTINUE:
        if (scope) {
            scope->flow = FLOW_CONTINUE;
        }
        break;

    case AST_EXPR_STMT:
        (void)eval_node(vm, scope, node->left);
        break;

    default:
        (void)eval_node(vm, scope, node);
        break;
    }
}

static void exec_stmt(we_script_t *vm, exec_scope_t *scope, const ast_node_t *node)
{
    if (!node || !vm_check_watchdog(vm)) {
        return;
    }
    if (scope && scope->flow != FLOW_NORMAL) {
        return;
    }
    if (++vm->eval_depth > 128u) {
        vm->eval_depth--;
        vm->depth_exceeded = true;
        vm->aborted = true;
        return;
    }
    exec_stmt_inner(vm, scope, node);
    vm->eval_depth--;
}

/* =========================================================================
 * Public SceneScript Lifecycle & Callback Evaluation API
 * ========================================================================= */

static void sync_layer_to_vm(we_script_t *vm, const we_script_layer_state_t *layer)
{
    if (layer) {
        vm->layer = *layer;
    } else {
        memset(&vm->layer, 0, sizeof(vm->layer));
        vm->layer.scale[0] = vm->layer.scale[1] = vm->layer.scale[2] = 1.0f;
        vm->layer.color[0] = vm->layer.color[1] = vm->layer.color[2] = 1.0f;
        vm->layer.alpha = 1.0f;
        vm->layer.visible = true;
    }

    vm->layer_origin_vec.dim = 3;
    vm->layer_origin_vec.bind_tag = 1;
    vm->layer_origin_vec.layer_slot = 0;
    memcpy(vm->layer_origin_vec.v, vm->layer.origin, 3 * sizeof(float));

    vm->layer_scale_vec.dim = 3;
    vm->layer_scale_vec.bind_tag = 2;
    vm->layer_scale_vec.layer_slot = 0;
    memcpy(vm->layer_scale_vec.v, vm->layer.scale, 3 * sizeof(float));

    vm->layer_angles_vec.dim = 3;
    vm->layer_angles_vec.bind_tag = 3;
    vm->layer_angles_vec.layer_slot = 0;
    memcpy(vm->layer_angles_vec.v, vm->layer.angles, 3 * sizeof(float));

    vm->layer_color_vec.dim = 3;
    vm->layer_color_vec.bind_tag = 4;
    vm->layer_color_vec.layer_slot = 0;
    memcpy(vm->layer_color_vec.v, vm->layer.color, 3 * sizeof(float));

    vm->layer_size_vec.dim = 2;
    vm->layer_size_vec.bind_tag = 5;
    vm->layer_size_vec.layer_slot = 0;
    memcpy(vm->layer_size_vec.v, vm->layer.size, 2 * sizeof(float));
}

static void reset_watchdog(we_script_t *vm)
{
    vm->step_count = 0;
    vm->aborted = false;
    vm->timed_out = false;
    vm->eval_depth = 0;
    clock_gettime(CLOCK_MONOTONIC, &vm->start_ts);
}

we_script_t *we_script_create(const char *src, size_t len, we_script_shared_t *shared)
{
    if (!src || len == 0) {
        return NULL;
    }

    we_script_t *vm = calloc(1, sizeof(we_script_t));
    if (!vm) {
        return NULL;
    }

    /* Fixed 16 MiB anonymous mmap arena (demand-paged by Linux kernel) */
    void *mem = mmap(NULL, WE_SCRIPT_ARENA_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        free(vm);
        return NULL;
    }
    vm->arena_base = (uint8_t *)mem;
    vm->arena_cap = WE_SCRIPT_ARENA_SIZE - WE_SCRIPT_SCRATCH_SIZE;
    vm->arena_used = 0;
    vm->shared = shared;
    vm->rng_state = 0xD1B54A32D192ED03ULL;

    /* Optional runtime QuickJS dlopen probe with zero DT_NEEDED startup overhead */
    const char *use_qjs = getenv("WWAL_USE_QUICKJS");
    if (use_qjs && strcmp(use_qjs, "1") == 0) {
        vm->quickjs_dl = dlopen("libquickjs.so", RTLD_LAZY | RTLD_LOCAL);
        if (vm->quickjs_dl) {
            if (!dlsym(vm->quickjs_dl, "JS_NewRuntime") || !dlsym(vm->quickjs_dl, "JS_Eval")) {
                dlclose(vm->quickjs_dl);
                vm->quickjs_dl = NULL;
            }
        }
    }

    vm->env.frametime = 1.0f / 60.0f;
    vm->env.screen_resolution[0] = 1920.0f;
    vm->env.screen_resolution[1] = 1080.0f;
    vm->env.canvas_size[0] = 1920.0f;
    vm->env.canvas_size[1] = 1080.0f;
    sync_layer_to_vm(vm, NULL);

    reset_watchdog(vm);

    js_parser_t parser = {
        .vm = vm,
        .src = src,
        .len = len,
        .pos = 0,
        .depth = 0,
    };
    lexer_next(&parser);
    vm->root_ast = parse_program(&parser);
    if (!vm->root_ast || vm->oom) {
        we_script_destroy(vm);
        return NULL;
    }

    /* Execute top-level declarations (functions, global variables, scriptProperties) */
    size_t pre_exec_used = vm->arena_used;
    reset_watchdog(vm);
    if (vm->depth_exceeded) {
        vm->aborted = true;
    } else {
        exec_stmt(vm, NULL, vm->root_ast);
    }
    vm->last_steps = vm->step_count;
    if (vm->aborted) {
        vm->arena_used = pre_exec_used;
    }
    vm->ast_watermark = vm->arena_used;

    return vm;
}

void we_script_destroy(we_script_t *script)
{
    if (!script) {
        return;
    }
    if (script->quickjs_dl) {
        dlclose(script->quickjs_dl);
    }
    if (script->arena_base && script->arena_cap > 0) {
        munmap(script->arena_base, WE_SCRIPT_ARENA_SIZE);
    }
    free(script);
}

void we_script_set_prop(we_script_t *script, const char *name, const float *val,
                        uint32_t num_components, const char *str_val)
{
    if (!script || !name || name[0] == '\0') {
        return;
    }

    we_script_prop_item_t *target = NULL;
    for (uint32_t i = 0; i < script->user_prop_count; i++) {
        if (strcmp(script->user_props[i].name, name) == 0) {
            target = &script->user_props[i];
            break;
        }
    }
    if (!target && script->user_prop_count < WE_SCRIPT_MAX_PROPS) {
        target = &script->user_props[script->user_prop_count++];
        memset(target, 0, sizeof(*target));
        snprintf(target->name, sizeof(target->name), "%s", name);
    }
    if (target) {
        target->num_components = num_components <= 4u ? num_components : 4u;
        if (val && target->num_components > 0) {
            memcpy(target->val, val, target->num_components * sizeof(float));
        }
        if (str_val) {
            snprintf(target->str_val, sizeof(target->str_val), "%s", str_val);
        } else {
            target->str_val[0] = '\0';
        }
    }

    /* Also mirror into global scriptProperties object if defined */
    js_var_slot_t *sp = find_var_slot(script, NULL, "scriptProperties");
    if (sp && sp->val.type == VAL_OBJ && sp->val.u.obj) {
        js_val_t existing = obj_get_prop(sp->val.u.obj, name);
        if (str_val && str_val[0] != '\0') {
            js_val_t pv = val_str(script_arena_strdup(script, str_val, strlen(str_val)));
            obj_set_prop(script, sp->val.u.obj, name, pv);
        } else if (num_components >= 2 && val) {
            if (existing.type == VAL_VEC && existing.u.vec) {
                existing.u.vec->dim = (uint8_t)(num_components <= 4u ? num_components : 4u);
                existing.u.vec->v[0] = val[0];
                existing.u.vec->v[1] = val[1];
                existing.u.vec->v[2] = num_components >= 3 ? val[2] : 0.0f;
                existing.u.vec->v[3] = num_components >= 4 ? val[3] : 0.0f;
            } else {
                js_val_t pv = val_new_vec(script, (uint8_t)num_components, val[0], val[1],
                                          num_components >= 3 ? val[2] : 0.0f,
                                          num_components >= 4 ? val[3] : 0.0f);
                obj_set_prop(script, sp->val.u.obj, name, pv);
            }
        } else if (val && num_components == 1) {
            obj_set_prop(script, sp->val.u.obj, name, val_num((double)val[0]));
        } else if (str_val) {
            obj_set_prop(script, sp->val.u.obj, name, val_str(""));
        } else {
            obj_set_prop(script, sp->val.u.obj, name, val_undef());
        }
    }
}

void we_script_register_scene_layer(we_script_t *script, uint32_t slot,
                                    const we_script_layer_state_t *layer)
{
    if (!script || slot >= WE_SCRIPT_MAX_SCENE_LAYERS) {
        return;
    }
    we_script_scene_layer_slot_t *sl = &script->scene_layers[slot];
    if (!layer) {
        sl->valid = false;
        sl->accessed = false;
        return;
    }
    sl->valid = true;
    sl->accessed = false;
    sl->state = *layer;
    sl->state.dirty_mask = 0;

    uint8_t lslot = (uint8_t)(slot + 1u);
    sl->origin_vec.dim = 3;
    sl->origin_vec.bind_tag = 1;
    sl->origin_vec.layer_slot = lslot;
    memcpy(sl->origin_vec.v, sl->state.origin, 3 * sizeof(float));

    sl->scale_vec.dim = 3;
    sl->scale_vec.bind_tag = 2;
    sl->scale_vec.layer_slot = lslot;
    memcpy(sl->scale_vec.v, sl->state.scale, 3 * sizeof(float));

    sl->angles_vec.dim = 3;
    sl->angles_vec.bind_tag = 3;
    sl->angles_vec.layer_slot = lslot;
    memcpy(sl->angles_vec.v, sl->state.angles, 3 * sizeof(float));

    sl->color_vec.dim = 3;
    sl->color_vec.bind_tag = 4;
    sl->color_vec.layer_slot = lslot;
    memcpy(sl->color_vec.v, sl->state.color, 3 * sizeof(float));

    sl->size_vec.dim = 2;
    sl->size_vec.bind_tag = 5;
    sl->size_vec.layer_slot = lslot;
    memcpy(sl->size_vec.v, sl->state.size, 2 * sizeof(float));
}

bool we_script_read_scene_layer(const we_script_t *script, uint32_t slot,
                                we_script_layer_state_t *out_layer)
{
    if (!script || slot >= WE_SCRIPT_MAX_SCENE_LAYERS || !out_layer) {
        return false;
    }
    const we_script_scene_layer_slot_t *sl = &script->scene_layers[slot];
    if (!sl->valid || !sl->accessed) {
        return false;
    }
    *out_layer = sl->state;
    return true;
}

#define WE_SCRIPT_MAX_FWD 1024u

typedef struct {
    const void *old_ptr;
    void *new_ptr;
} promote_fwd_t;

typedef struct {
    promote_fwd_t entries[WE_SCRIPT_MAX_FWD];
    uint32_t count;
} promote_fwd_table_t;

static void *fwd_lookup(const promote_fwd_table_t *tbl, const void *old_ptr)
{
    if (!tbl || !old_ptr) {
        return NULL;
    }
    for (uint32_t i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].old_ptr == old_ptr) {
            return tbl->entries[i].new_ptr;
        }
    }
    return NULL;
}

static void fwd_record(promote_fwd_table_t *tbl, const void *old_ptr, void *new_ptr)
{
    if (!tbl || !old_ptr || !new_ptr || tbl->count >= WE_SCRIPT_MAX_FWD) {
        return;
    }
    tbl->entries[tbl->count].old_ptr = old_ptr;
    tbl->entries[tbl->count].new_ptr = new_ptr;
    tbl->count++;
}

static inline bool ptr_above_watermark(const we_script_t *vm, const void *p)
{
    return p && (const uint8_t *)p >= vm->arena_base + vm->ast_watermark &&
           (const uint8_t *)p < vm->arena_base + vm->arena_cap;
}

static inline bool ptr_in_scratch(const uint8_t *scratch, size_t scratch_used, const void *p)
{
    return p && (const uint8_t *)p >= scratch && (const uint8_t *)p < scratch + scratch_used;
}

static void *scratch_alloc_copy(uint8_t *scratch, size_t scratch_cap, size_t *scratch_used,
                                const void *src, size_t size)
{
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned == 0) {
        aligned = 8u;
    }
    if (*scratch_used + aligned > scratch_cap) {
        return NULL;
    }
    void *dst = scratch + *scratch_used;
    *scratch_used += aligned;
    memset(dst, 0, aligned);
    if (src && size > 0) {
        memcpy(dst, src, size);
    }
    return dst;
}

static const char *evacuate_str_to_scratch(const we_script_t *vm, const char *s, uint8_t *scratch,
                                           size_t scratch_cap, size_t *scratch_used)
{
    if (!ptr_above_watermark(vm, s)) {
        return s;
    }
    size_t len = strlen(s);
    char *copy = (char *)scratch_alloc_copy(scratch, scratch_cap, scratch_used, s, len + 1u);
    return copy ? copy : "";
}

static void evacuate_val_to_scratch(const we_script_t *vm, js_val_t *v, uint8_t *scratch,
                                    size_t scratch_cap, size_t *scratch_used, uint32_t depth,
                                    promote_fwd_table_t *fwd)
{
    if (!v || depth > 64u) {
        return;
    }
    switch (v->type) {
    case VAL_STR:
        v->u.str = evacuate_str_to_scratch(vm, v->u.str, scratch, scratch_cap, scratch_used);
        break;
    case VAL_VEC:
        if (v->u.vec) {
            js_vec_obj_t *orig_vec = v->u.vec;
            js_vec_obj_t *existing = (js_vec_obj_t *)fwd_lookup(fwd, orig_vec);
            if (existing) {
                v->u.vec = existing;
                return;
            }
            if (ptr_above_watermark(vm, orig_vec)) {
                js_vec_obj_t *nv = (js_vec_obj_t *)scratch_alloc_copy(
                    scratch, scratch_cap, scratch_used, orig_vec, sizeof(js_vec_obj_t));
                if (nv) {
                    v->u.vec = nv;
                    fwd_record(fwd, orig_vec, nv);
                }
            }
        }
        break;
    case VAL_ARR:
        if (v->u.arr) {
            js_arr_t *orig_arr = v->u.arr;
            js_arr_t *existing = (js_arr_t *)fwd_lookup(fwd, orig_arr);
            if (existing) {
                v->u.arr = existing;
                return;
            }
            js_arr_t *na = orig_arr;
            if (ptr_above_watermark(vm, orig_arr)) {
                js_arr_t *copy = (js_arr_t *)scratch_alloc_copy(scratch, scratch_cap, scratch_used,
                                                                orig_arr, sizeof(js_arr_t));
                if (copy) {
                    na = copy;
                }
            }
            v->u.arr = na;
            fwd_record(fwd, orig_arr, na);
            if (na->items && na->cap > 0) {
                if (ptr_above_watermark(vm, na->items)) {
                    js_val_t *ni = (js_val_t *)scratch_alloc_copy(
                        scratch, scratch_cap, scratch_used, na->items, na->cap * sizeof(js_val_t));
                    if (ni) {
                        na->items = ni;
                    }
                }
                for (uint32_t i = 0; i < na->count; i++) {
                    evacuate_val_to_scratch(vm, &na->items[i], scratch, scratch_cap, scratch_used,
                                            depth + 1u, fwd);
                }
            }
        }
        break;
    case VAL_OBJ:
        if (v->u.obj) {
            js_obj_t *orig_obj = v->u.obj;
            js_obj_t *existing = (js_obj_t *)fwd_lookup(fwd, orig_obj);
            if (existing) {
                v->u.obj = existing;
                return;
            }
            js_obj_t *no = orig_obj;
            if (ptr_above_watermark(vm, orig_obj)) {
                js_obj_t *copy = (js_obj_t *)scratch_alloc_copy(scratch, scratch_cap, scratch_used,
                                                                orig_obj, sizeof(js_obj_t));
                if (copy) {
                    no = copy;
                }
            }
            v->u.obj = no;
            fwd_record(fwd, orig_obj, no);
            if (no->props && no->cap > 0) {
                if (ptr_above_watermark(vm, no->props)) {
                    js_prop_t *np = (js_prop_t *)scratch_alloc_copy(
                        scratch, scratch_cap, scratch_used, no->props, no->cap * sizeof(js_prop_t));
                    if (np) {
                        no->props = np;
                    }
                }
                for (uint32_t i = 0; i < no->count; i++) {
                    no->props[i].key = evacuate_str_to_scratch(vm, no->props[i].key, scratch,
                                                               scratch_cap, scratch_used);
                    evacuate_val_to_scratch(vm, &no->props[i].val, scratch, scratch_cap,
                                            scratch_used, depth + 1u, fwd);
                }
            }
        }
        break;
    default:
        break;
    }
}

static const char *promote_str_from_scratch(we_script_t *vm, const char *s, const uint8_t *scratch,
                                            size_t scratch_used)
{
    if (!ptr_in_scratch(scratch, scratch_used, s)) {
        return s;
    }
    return script_arena_strdup(vm, s, strlen(s));
}

static void promote_val_from_scratch(we_script_t *vm, js_val_t *v, const uint8_t *scratch,
                                     size_t scratch_used, uint32_t depth, promote_fwd_table_t *fwd)
{
    if (!v || depth > 64u) {
        return;
    }
    switch (v->type) {
    case VAL_STR:
        v->u.str = promote_str_from_scratch(vm, v->u.str, scratch, scratch_used);
        break;
    case VAL_VEC:
        if (v->u.vec) {
            js_vec_obj_t *orig_vec = v->u.vec;
            js_vec_obj_t *existing = (js_vec_obj_t *)fwd_lookup(fwd, orig_vec);
            if (existing) {
                v->u.vec = existing;
                return;
            }
            if (ptr_in_scratch(scratch, scratch_used, orig_vec)) {
                js_vec_obj_t *nv = (js_vec_obj_t *)script_arena_alloc(vm, sizeof(js_vec_obj_t));
                if (nv) {
                    *nv = *orig_vec;
                    v->u.vec = nv;
                    fwd_record(fwd, orig_vec, nv);
                }
            }
        }
        break;
    case VAL_ARR:
        if (v->u.arr) {
            js_arr_t *orig_arr = v->u.arr;
            js_arr_t *existing = (js_arr_t *)fwd_lookup(fwd, orig_arr);
            if (existing) {
                v->u.arr = existing;
                return;
            }
            js_arr_t *na = orig_arr;
            if (ptr_in_scratch(scratch, scratch_used, orig_arr)) {
                js_arr_t *copy = (js_arr_t *)script_arena_alloc(vm, sizeof(js_arr_t));
                if (copy) {
                    *copy = *orig_arr;
                    na = copy;
                }
            }
            v->u.arr = na;
            fwd_record(fwd, orig_arr, na);
            if (na->items && na->cap > 0) {
                if (ptr_in_scratch(scratch, scratch_used, na->items)) {
                    js_val_t *ni = (js_val_t *)script_arena_alloc(vm, na->cap * sizeof(js_val_t));
                    if (ni) {
                        memcpy(ni, na->items, na->count * sizeof(js_val_t));
                        na->items = ni;
                    }
                }
                for (uint32_t i = 0; i < na->count; i++) {
                    promote_val_from_scratch(vm, &na->items[i], scratch, scratch_used, depth + 1u,
                                             fwd);
                }
            }
        }
        break;
    case VAL_OBJ:
        if (v->u.obj) {
            js_obj_t *orig_obj = v->u.obj;
            js_obj_t *existing = (js_obj_t *)fwd_lookup(fwd, orig_obj);
            if (existing) {
                v->u.obj = existing;
                return;
            }
            js_obj_t *no = orig_obj;
            if (ptr_in_scratch(scratch, scratch_used, orig_obj)) {
                js_obj_t *copy = (js_obj_t *)script_arena_alloc(vm, sizeof(js_obj_t));
                if (copy) {
                    *copy = *orig_obj;
                    no = copy;
                }
            }
            v->u.obj = no;
            fwd_record(fwd, orig_obj, no);
            if (no->props && no->cap > 0) {
                if (ptr_in_scratch(scratch, scratch_used, no->props)) {
                    js_prop_t *np =
                        (js_prop_t *)script_arena_alloc(vm, no->cap * sizeof(js_prop_t));
                    if (np) {
                        memcpy(np, no->props, no->count * sizeof(js_prop_t));
                        no->props = np;
                    }
                }
                for (uint32_t i = 0; i < no->count; i++) {
                    no->props[i].key =
                        promote_str_from_scratch(vm, no->props[i].key, scratch, scratch_used);
                    promote_val_from_scratch(vm, &no->props[i].val, scratch, scratch_used,
                                             depth + 1u, fwd);
                }
            }
        }
        break;
    default:
        break;
    }
}

static void promote_global_values(we_script_t *vm)
{
    if (!vm || vm->arena_used <= vm->ast_watermark) {
        return;
    }

    uint8_t *scratch = vm->arena_base + vm->arena_cap;
    size_t scratch_cap = WE_SCRIPT_SCRATCH_SIZE;
    size_t scratch_used = 0;

    promote_fwd_table_t evac_fwd = {.count = 0};
    for (uint32_t i = 0; i < vm->global_count; i++) {
        vm->globals[i].name =
            evacuate_str_to_scratch(vm, vm->globals[i].name, scratch, scratch_cap, &scratch_used);
        evacuate_val_to_scratch(vm, &vm->globals[i].val, scratch, scratch_cap, &scratch_used, 0,
                                &evac_fwd);
    }
    for (uint32_t i = 0; i < vm->this_prop_count; i++) {
        vm->this_props[i].name = evacuate_str_to_scratch(vm, vm->this_props[i].name, scratch,
                                                         scratch_cap, &scratch_used);
        evacuate_val_to_scratch(vm, &vm->this_props[i].val, scratch, scratch_cap, &scratch_used, 0,
                                &evac_fwd);
    }

    vm->arena_used = vm->ast_watermark;

    if (scratch_used == 0) {
        return;
    }

    promote_fwd_table_t prom_fwd = {.count = 0};
    for (uint32_t i = 0; i < vm->global_count; i++) {
        vm->globals[i].name =
            promote_str_from_scratch(vm, vm->globals[i].name, scratch, scratch_used);
        promote_val_from_scratch(vm, &vm->globals[i].val, scratch, scratch_used, 0, &prom_fwd);
    }
    for (uint32_t i = 0; i < vm->this_prop_count; i++) {
        vm->this_props[i].name =
            promote_str_from_scratch(vm, vm->this_props[i].name, scratch, scratch_used);
        promote_val_from_scratch(vm, &vm->this_props[i].val, scratch, scratch_used, 0, &prom_fwd);
    }
}

static bool run_value_callback(we_script_t *vm, const ast_node_t *fn, const we_script_env_t *env,
                               we_script_layer_state_t *inout_layer, const float *in_val,
                               uint32_t num_components, float *out_val)
{
    if (out_val && in_val && num_components > 0) {
        memcpy(out_val, in_val, num_components * sizeof(float));
    }
    if (!vm || !fn) {
        return false;
    }
    if (vm->depth_exceeded) {
        vm->aborted = true;
        return false;
    }

    if (env) {
        vm->env = *env;
    }
    we_script_layer_state_t saved_layer;
    if (inout_layer) {
        saved_layer = *inout_layer;
    } else {
        memset(&saved_layer, 0, sizeof(saved_layer));
    }
    sync_layer_to_vm(vm, inout_layer);
    reset_watchdog(vm);

    js_val_t arg_v;
    if (num_components >= 2 && in_val) {
        uint8_t d = (uint8_t)(num_components <= 4u ? num_components : 4u);
        arg_v = val_new_vec(vm, d, in_val[0], in_val[1], d >= 3 ? in_val[2] : 0.0f,
                            d >= 4 ? in_val[3] : 1.0f);
    } else if (in_val && num_components == 1) {
        arg_v = val_num((double)in_val[0]);
    } else {
        arg_v = val_num(0.0);
    }

    js_val_t ret = invoke_ast_func(vm, fn, &arg_v, 1, 1);
    vm->last_steps = vm->step_count;

    if (vm->aborted) {
        if (inout_layer) {
            *inout_layer = saved_layer;
        }
        if (out_val && in_val && num_components > 0) {
            memcpy(out_val, in_val, num_components * sizeof(float));
        }
        promote_global_values(vm);
        return false;
    }

    if (inout_layer) {
        *inout_layer = vm->layer;
    }

    if (out_val && num_components > 0) {
        js_val_t src_v = (ret.type != VAL_UNDEF) ? ret : arg_v;
        if (src_v.type == VAL_VEC && src_v.u.vec) {
            uint32_t n = num_components <= 4u ? num_components : 4u;
            for (uint32_t i = 0; i < n; i++) {
                out_val[i] = isfinite(src_v.u.vec->v[i]) ? src_v.u.vec->v[i] : 0.0f;
            }
        } else if (src_v.type == VAL_BOOL) {
            out_val[0] = src_v.u.bval ? 1.0f : 0.0f;
        } else if (src_v.type == VAL_NUM) {
            float s = isfinite((float)src_v.u.num) ? (float)src_v.u.num : 0.0f;
            for (uint32_t i = 0; i < num_components && i < 4u; i++) {
                out_val[i] = s;
            }
        }
    }
    promote_global_values(vm);
    return true;
}

bool we_script_eval_init(we_script_t *script, const we_script_env_t *env,
                         we_script_layer_state_t *inout_layer, const float *in_val,
                         uint32_t num_components, float *out_val)
{
    if (!script) {
        return false;
    }
    if (script->depth_exceeded) {
        script->aborted = true;
        return false;
    }
    if (!script->fn_init) {
        if (out_val && in_val && num_components > 0) {
            memcpy(out_val, in_val, num_components * sizeof(float));
        }
        return true;
    }
    bool ok = run_value_callback(script, script->fn_init, env, inout_layer, in_val, num_components,
                                 out_val);
    if (ok && !script->aborted && script->arena_used < script->arena_cap / 2u) {
        script->ast_watermark = script->arena_used;
    }
    return ok;
}

bool we_script_eval_update(we_script_t *script, const we_script_env_t *env,
                           we_script_layer_state_t *inout_layer, const float *in_val,
                           uint32_t num_components, float *out_val)
{
    if (!script) {
        return false;
    }
    if (script->depth_exceeded) {
        script->aborted = true;
        return false;
    }
    if (!script->fn_update) {
        if (out_val && in_val && num_components > 0) {
            memcpy(out_val, in_val, num_components * sizeof(float));
        }
        return true;
    }
    return run_value_callback(script, script->fn_update, env, inout_layer, in_val, num_components,
                              out_val);
}

bool we_script_eval_cursor_move(we_script_t *script, const we_script_env_t *env,
                                we_script_layer_state_t *inout_layer)
{
    if (!script || !script->fn_cursor_move) {
        return false;
    }
    if (script->depth_exceeded) {
        script->aborted = true;
        return false;
    }
    if (env) {
        script->env = *env;
    }
    we_script_layer_state_t saved_layer;
    if (inout_layer) {
        saved_layer = *inout_layer;
    }
    sync_layer_to_vm(script, inout_layer);
    reset_watchdog(script);

    js_obj_t *ev = (js_obj_t *)script_arena_alloc(script, sizeof(js_obj_t));
    if (ev) {
        obj_set_prop(script, ev, "worldPosition",
                     val_new_vec(script, 3, script->env.cursor_world[0],
                                 script->env.cursor_world[1], script->env.cursor_world[2], 0.0f));
        obj_set_prop(script, ev, "screenPosition",
                     val_new_vec(script, 2, script->env.cursor_screen[0],
                                 script->env.cursor_screen[1], 0.0f, 0.0f));
    }
    js_val_t arg = {.type = VAL_OBJ, .u.obj = ev};
    (void)invoke_ast_func(script, script->fn_cursor_move, &arg, 1, 1);
    script->last_steps = script->step_count;

    if (script->aborted) {
        if (inout_layer) {
            *inout_layer = saved_layer;
        }
        promote_global_values(script);
        return false;
    }
    if (inout_layer) {
        *inout_layer = script->layer;
    }
    promote_global_values(script);
    return true;
}

bool we_script_eval_apply_user_properties(we_script_t *script, const we_script_env_t *env,
                                          we_script_layer_state_t *inout_layer)
{
    if (!script || !script->fn_apply_user_props) {
        return false;
    }
    if (script->depth_exceeded) {
        script->aborted = true;
        return false;
    }
    if (env) {
        script->env = *env;
    }
    we_script_layer_state_t saved_layer;
    if (inout_layer) {
        saved_layer = *inout_layer;
    }
    sync_layer_to_vm(script, inout_layer);
    reset_watchdog(script);

    js_obj_t *props = (js_obj_t *)script_arena_alloc(script, sizeof(js_obj_t));
    if (props) {
        for (uint32_t i = 0; i < script->user_prop_count; i++) {
            const we_script_prop_item_t *up = &script->user_props[i];
            js_obj_t *pnode = (js_obj_t *)script_arena_alloc(script, sizeof(js_obj_t));
            if (!pnode) {
                break;
            }
            js_val_t pv;
            if (up->str_val[0] != '\0') {
                pv = val_str(up->str_val);
            } else if (up->num_components >= 3) {
                pv = val_new_vec(script, 3, up->val[0], up->val[1], up->val[2], 0.0f);
            } else if (up->num_components == 2) {
                pv = val_new_vec(script, 2, up->val[0], up->val[1], 0.0f, 0.0f);
            } else if (up->num_components == 1) {
                pv = val_num((double)up->val[0]);
            } else {
                pv = val_str(up->str_val);
            }
            obj_set_prop(script, pnode, "value", pv);
            js_val_t pnode_v = {.type = VAL_OBJ, .u.obj = pnode};
            obj_set_prop(script, props, up->name, pnode_v);
        }
    }
    js_val_t arg = {.type = VAL_OBJ, .u.obj = props};
    (void)invoke_ast_func(script, script->fn_apply_user_props, &arg, 1, 1);
    script->last_steps = script->step_count;

    if (script->aborted) {
        if (inout_layer) {
            *inout_layer = saved_layer;
        }
        promote_global_values(script);
        return false;
    }
    if (inout_layer) {
        *inout_layer = script->layer;
    }
    promote_global_values(script);
    return true;
}

bool we_script_was_aborted(const we_script_t *script) { return script ? script->aborted : false; }

uint32_t we_script_last_step_count(const we_script_t *script)
{
    return script ? script->last_steps : 0u;
}

bool we_script_uses_audio(const we_script_t *script) { return script ? script->uses_audio : false; }

size_t we_script_arena_used(const we_script_t *script) { return script ? script->arena_used : 0u; }
