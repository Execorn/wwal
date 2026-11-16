// SPDX-License-Identifier: GPL-3.0-or-later
#include "we_script_internal.h"

#include <time.h>

/* =========================================================================
 * Tokenizer
 * ========================================================================= */

typedef struct {
    we_script_t *vm;
    uint8_t *lint_arena;
    size_t lint_cap;
    size_t lint_used;
    bool lint_mode;
    uint32_t lint_flags;
    bool has_error;
    bool depth_exceeded;
    bool aborted;
    uint32_t err_line;
    uint32_t err_col;
    char err_msg[256];
} js_parse_ctx_t;

typedef struct {
    js_parse_ctx_t *ctx;
    const char *src;
    size_t len;
    size_t pos;
    size_t line_start;
    uint32_t line;
    uint32_t col_offset;
    uint32_t cur_line;
    uint32_t cur_col;
    uint32_t tpl_line;
    uint32_t tpl_col;
    uint32_t depth;
    js_token_t cur;
} js_parser_t;

#define AST_CHUNK_CAP 16u

typedef struct ast_ptr_chunk {
    struct ast_ptr_chunk *next;
    void *items[AST_CHUNK_CAP];
} ast_ptr_chunk_t;

typedef struct {
    ast_ptr_chunk_t *head;
    ast_ptr_chunk_t *tail;
    uint32_t tail_used;
    uint32_t total;
} ast_ptr_list_t;

typedef struct ast_var_chunk {
    struct ast_var_chunk *next;
    ast_var_item_t items[AST_CHUNK_CAP];
} ast_var_chunk_t;

typedef struct {
    ast_var_chunk_t *head;
    ast_var_chunk_t *tail;
    uint32_t tail_used;
    uint32_t total;
} ast_var_list_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} parse_strbuf_t;

static inline bool parser_aborted(const js_parser_t *p)
{
    return p->ctx->aborted || (p->ctx->vm && p->ctx->vm->aborted);
}

static void parser_fail(js_parser_t *p, uint32_t line, uint32_t col, const char *msg)
{
    js_parse_ctx_t *c = p->ctx;
    if (!c->has_error) {
        c->has_error = true;
        c->err_line = line > 0u ? line : 1u;
        c->err_col = col > 0u ? col : 1u;
        snprintf(c->err_msg, sizeof(c->err_msg), "%s", msg ? msg : "syntax error");
    }
    if (c->vm) {
        c->vm->parse_error = true;
    }
}

static void parser_fail_depth(js_parser_t *p, uint32_t line, uint32_t col, const char *msg)
{
    js_parse_ctx_t *c = p->ctx;
    c->depth_exceeded = true;
    c->aborted = true;
    parser_fail(p, line, col, msg);
    if (c->vm) {
        c->vm->depth_exceeded = true;
        c->vm->aborted = true;
    }
}

static void *parser_alloc(js_parser_t *p, size_t sz)
{
    js_parse_ctx_t *c = p->ctx;
    if (c->vm) {
        return script_arena_alloc(c->vm, sz);
    }
    if (!c->lint_arena || sz == 0u) {
        return NULL;
    }
    size_t aligned = 8u;
    if (ckd_add(&aligned, sz, (size_t)7u)) {
        parser_fail(p, p->cur_line, p->cur_col, "script exceeds 64 KiB parser scratch arena");
        c->aborted = true;
        return NULL;
    }
    aligned &= ~(size_t)7u;
    if (aligned > c->lint_cap || c->lint_used > c->lint_cap - aligned) {
        parser_fail(p, p->cur_line, p->cur_col, "script exceeds 64 KiB parser scratch arena");
        c->aborted = true;
        return NULL;
    }
    void *ptr = c->lint_arena + c->lint_used;
    c->lint_used += aligned;
    memset(ptr, 0, sz);
    return ptr;
}

static const char *parser_strdup(js_parser_t *p, const char *s, size_t len)
{
    js_parse_ctx_t *c = p->ctx;
    if (c->vm) {
        char *dup = script_arena_strdup(c->vm, s, len);
        return dup ? dup : "";
    }
    if (!s || len == 0u) {
        return "";
    }
    size_t aligned = 0u;
    if (!c->lint_arena || len > c->lint_cap - 1u || ckd_add(&aligned, len, (size_t)8u)) {
        parser_fail(p, p->cur_line, p->cur_col, "script exceeds 64 KiB parser scratch arena");
        c->aborted = true;
        return "";
    }
    aligned &= ~(size_t)7u;
    if (aligned > c->lint_cap - c->lint_used) {
        parser_fail(p, p->cur_line, p->cur_col, "script exceeds 64 KiB parser scratch arena");
        c->aborted = true;
        return "";
    }
    char *dst = (char *)(c->lint_arena + c->lint_used);
    c->lint_used += aligned;
    memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

static bool ast_ptr_list_push(js_parser_t *p, ast_ptr_list_t *list, void *ptr)
{
    if (p->ctx->lint_mode) {
        return true;
    }
    if (list->total >= 65536u) {
        parser_fail(p, p->cur_line, p->cur_col, "parser list element limit exceeded");
        p->ctx->aborted = true;
        return false;
    }
    if (!list->tail || list->tail_used >= AST_CHUNK_CAP) {
        ast_ptr_chunk_t *chunk = (ast_ptr_chunk_t *)parser_alloc(p, sizeof(ast_ptr_chunk_t));
        if (!chunk) {
            return false;
        }
        if (list->tail) {
            list->tail->next = chunk;
        } else {
            list->head = chunk;
        }
        list->tail = chunk;
        list->tail_used = 0u;
    }
    list->tail->items[list->tail_used++] = ptr;
    list->total++;
    return true;
}

static void **ast_ptr_list_flatten(js_parser_t *p, const ast_ptr_list_t *list, uint32_t *out_cnt)
{
    if (p->ctx->lint_mode || list->total == 0u) {
        *out_cnt = 0u;
        return NULL;
    }
    size_t bytes = 0u;
    if (ckd_mul(&bytes, (size_t)list->total, sizeof(void *))) {
        parser_fail(p, p->cur_line, p->cur_col, "parser list size overflow");
        p->ctx->aborted = true;
        *out_cnt = 0u;
        return NULL;
    }
    void **arr = (void **)parser_alloc(p, bytes);
    if (!arr) {
        *out_cnt = 0u;
        return NULL;
    }
    uint32_t pos = 0u;
    for (const ast_ptr_chunk_t *c = list->head; c != NULL; c = c->next) {
        uint32_t n = (c == list->tail) ? list->tail_used : AST_CHUNK_CAP;
        if (n > 0u) {
            memcpy(arr + pos, c->items, (size_t)n * sizeof(void *));
            pos += n;
        }
    }
    *out_cnt = pos;
    return arr;
}

static bool ast_var_list_push(js_parser_t *p, ast_var_list_t *list, const char *name,
                              ast_node_t *init)
{
    if (p->ctx->lint_mode) {
        return true;
    }
    if (list->total >= 65536u) {
        parser_fail(p, p->cur_line, p->cur_col, "variable declaration limit exceeded");
        p->ctx->aborted = true;
        return false;
    }
    if (!list->tail || list->tail_used >= AST_CHUNK_CAP) {
        ast_var_chunk_t *chunk = (ast_var_chunk_t *)parser_alloc(p, sizeof(ast_var_chunk_t));
        if (!chunk) {
            return false;
        }
        if (list->tail) {
            list->tail->next = chunk;
        } else {
            list->head = chunk;
        }
        list->tail = chunk;
        list->tail_used = 0u;
    }
    list->tail->items[list->tail_used].name = name;
    list->tail->items[list->tail_used].init = init;
    list->tail_used++;
    list->total++;
    return true;
}

static ast_var_item_t *ast_var_list_flatten(js_parser_t *p, const ast_var_list_t *list,
                                            uint32_t *out_cnt)
{
    if (p->ctx->lint_mode || list->total == 0u) {
        *out_cnt = 0u;
        return NULL;
    }
    size_t bytes = 0u;
    if (ckd_mul(&bytes, (size_t)list->total, sizeof(ast_var_item_t))) {
        parser_fail(p, p->cur_line, p->cur_col, "variable list size overflow");
        p->ctx->aborted = true;
        *out_cnt = 0u;
        return NULL;
    }
    ast_var_item_t *arr = (ast_var_item_t *)parser_alloc(p, bytes);
    if (!arr) {
        *out_cnt = 0u;
        return NULL;
    }
    uint32_t pos = 0u;
    for (const ast_var_chunk_t *c = list->head; c != NULL; c = c->next) {
        uint32_t n = (c == list->tail) ? list->tail_used : AST_CHUNK_CAP;
        if (n > 0u) {
            memcpy(arr + pos, c->items, (size_t)n * sizeof(ast_var_item_t));
            pos += n;
        }
    }
    *out_cnt = pos;
    return arr;
}

static bool parse_strbuf_push(parse_strbuf_t *sb, js_parser_t *p, uint32_t line, uint32_t col,
                              char ch)
{
    if (sb->len >= WE_SCRIPT_MAX_STRING_BYTES) {
        parser_fail(p, line, col, "string literal exceeds maximum length");
        p->ctx->aborted = true;
        return false;
    }
    if (p->ctx->lint_mode) {
        sb->len++;
        return true;
    }
    if (sb->len + 1u >= sb->cap) {
        size_t ncap = sb->cap == 0u ? 128u : sb->cap * 2u;
        if (ncap > WE_SCRIPT_MAX_STRING_BYTES + 1u) {
            ncap = WE_SCRIPT_MAX_STRING_BYTES + 1u;
        }
        char *nbuf = (char *)realloc(sb->buf, ncap);
        if (!nbuf) {
            parser_fail(p, line, col, "out of memory parsing string literal");
            p->ctx->aborted = true;
            if (p->ctx->vm) {
                p->ctx->vm->oom = true;
                p->ctx->vm->aborted = true;
            }
            return false;
        }
        sb->buf = nbuf;
        sb->cap = ncap;
    }
    sb->buf[sb->len++] = ch;
    return true;
}

static void parse_strbuf_free(parse_strbuf_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0u;
    sb->cap = 0u;
}

static inline void lexer_bump_line(js_parser_t *p, size_t new_pos)
{
    p->line++;
    p->line_start = new_pos;
    p->col_offset = 0u;
}

static void lexer_skip_ws_and_comments(js_parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
            if (c == '\n') {
                lexer_bump_line(p, p->pos);
            }
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n') {
                p->pos++;
            }
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '*') {
            uint32_t cl = p->line > 0u ? p->line : 1u;
            uint32_t cc = (uint32_t)(p->pos - p->line_start + 1u) + p->col_offset;
            p->pos += 2;
            bool closed = false;
            while (p->pos + 1 < p->len) {
                if (p->src[p->pos] == '*' && p->src[p->pos + 1] == '/') {
                    p->pos += 2;
                    closed = true;
                    break;
                }
                char mc = p->src[p->pos++];
                if (mc == '\n') {
                    lexer_bump_line(p, p->pos);
                }
            }
            if (!closed) {
                p->pos = p->len;
                parser_fail(p, cl, cc, "unterminated block comment");
                break;
            }
        } else {
            break;
        }
    }
}

static void scan_template_body(const char *src, size_t len, size_t *pos, int depth);

static void scan_template_expr(const char *src, size_t len, size_t *pos, int depth)
{
    if (depth > 32) {
        *pos = len;
        return;
    }
    int bdepth = 1;
    while (*pos < len && bdepth > 0) {
        char ch = src[*pos];
        if (ch == '\\' && *pos + 1 < len) {
            *pos += 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char q = ch;
            (*pos)++;
            while (*pos < len && src[*pos] != q) {
                if (src[*pos] == '\\' && *pos + 1 < len) {
                    *pos += 2;
                } else {
                    (*pos)++;
                }
            }
            if (*pos < len) {
                (*pos)++;
            }
            continue;
        }
        if (ch == '`') {
            (*pos)++;
            scan_template_body(src, len, pos, depth + 1);
            if (*pos < len && src[*pos] == '`') {
                (*pos)++;
            }
            continue;
        }
        if (ch == '{') {
            bdepth++;
        } else if (ch == '}') {
            bdepth--;
            if (bdepth == 0) {
                return;
            }
        }
        (*pos)++;
    }
}

static void scan_template_body(const char *src, size_t len, size_t *pos, int depth)
{
    if (depth > 32) {
        *pos = len;
        return;
    }
    while (*pos < len) {
        char ch = src[*pos];
        if (ch == '\\' && *pos + 1 < len) {
            *pos += 2;
            continue;
        }
        if (ch == '`') {
            return;
        }
        if (ch == '$' && *pos + 1 < len && src[*pos + 1] == '{') {
            *pos += 2;
            scan_template_expr(src, len, pos, depth + 1);
            if (*pos < len && src[*pos] == '}') {
                (*pos)++;
            }
            continue;
        }
        (*pos)++;
    }
}

static __attribute__((noinline)) void lex_number_literal(js_parser_t *p)
{
    const char *s = p->src + p->pos;
    size_t rem = p->len - p->pos;
    size_t tok_len = 0u;
    bool is_hex = false;

    if (rem > 2u && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        is_hex = true;
        tok_len = 2u;
        while (tok_len < rem && isxdigit((unsigned char)s[tok_len])) {
            tok_len++;
        }
    } else {
        while (tok_len < rem && isdigit((unsigned char)s[tok_len])) {
            tok_len++;
        }
        if (tok_len < rem && s[tok_len] == '.') {
            tok_len++;
            while (tok_len < rem && isdigit((unsigned char)s[tok_len])) {
                tok_len++;
            }
        }
        if (tok_len < rem && (s[tok_len] == 'e' || s[tok_len] == 'E')) {
            size_t exp_pos = tok_len + 1u;
            if (exp_pos < rem && (s[exp_pos] == '+' || s[exp_pos] == '-')) {
                exp_pos++;
            }
            if (exp_pos < rem && isdigit((unsigned char)s[exp_pos])) {
                while (exp_pos < rem && isdigit((unsigned char)s[exp_pos])) {
                    exp_pos++;
                }
                tok_len = exp_pos;
            }
        }
    }

    char nbuf[64];
    if (tok_len == 0u || tok_len >= sizeof(nbuf)) {
        p->pos += (tok_len > 0u) ? tok_len : 1u;
        parser_fail(p, p->cur_line, p->cur_col, "numeric literal too long");
        p->cur.type = TOK_NUM;
        p->cur.num = 0.0;
        return;
    }
    memcpy(nbuf, s, tok_len);
    nbuf[tok_len] = '\0';
    char *endptr = NULL;
    double val = is_hex ? (double)strtoull(nbuf, &endptr, 16) : strtod(nbuf, &endptr);
    if (endptr && endptr > nbuf) {
        p->pos += (size_t)(endptr - nbuf);
    } else {
        p->pos++;
    }
    p->cur.type = TOK_NUM;
    p->cur.num = val;
}

static __attribute__((noinline)) void lex_string_literal(js_parser_t *p, char quote)
{
    uint32_t sline = p->cur_line;
    uint32_t scol = p->cur_col;
    p->pos++;

    parse_strbuf_t sb = {0};
    bool ok = true;
    while (p->pos < p->len && p->src[p->pos] != quote && p->src[p->pos] != '\n' &&
           p->src[p->pos] != '\r') {
        char ch = p->src[p->pos++];
        if (ch == '\\' && p->pos < p->len) {
            char esc = p->src[p->pos++];
            if (esc == '\n') {
                lexer_bump_line(p, p->pos);
                continue;
            }
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
            case '0':
                ch = '\0';
                break;
            default:
                ch = esc;
                break;
            }
        }
        if (ok && !parse_strbuf_push(&sb, p, sline, scol, ch)) {
            ok = false;
        }
    }
    if (p->pos < p->len && p->src[p->pos] == quote) {
        p->pos++;
    } else {
        parser_fail(p, sline, scol, "unterminated string literal");
    }
    p->cur.type = TOK_STR;
    p->cur.len = sb.len;
    if (!ok || p->ctx->lint_mode) {
        p->cur.text = "";
    } else {
        p->cur.text = parser_strdup(p, sb.buf ? sb.buf : "", sb.len);
    }
    parse_strbuf_free(&sb);
}

static void lexer_next(js_parser_t *p)
{
    if (p->line == 0u) {
        p->line = 1u;
    }
    lexer_skip_ws_and_comments(p);
    memset(&p->cur, 0, sizeof(p->cur));
    p->cur_line = p->line;
    p->cur_col = (uint32_t)(p->pos - p->line_start + 1u) + p->col_offset;
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
            p->cur.text = parser_strdup(p, s, ilen);
            p->cur.len = ilen;
        }
        return;
    }

    /* Numeric literals */
    if (isdigit((unsigned char)c) ||
        (c == '.' && p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1]))) {
        lex_number_literal(p);
        return;
    }

    /* Template literals (`...`) */
    if (c == '`') {
        uint32_t tline = p->cur_line;
        uint32_t tcol = p->cur_col;
        p->pos++;
        size_t start = p->pos;
        scan_template_body(p->src, p->len, &p->pos, 0);
        size_t raw_len = p->pos - start;
        for (size_t k = start; k < p->pos; k++) {
            if (p->src[k] == '\n') {
                lexer_bump_line(p, k + 1u);
            }
        }
        if (p->pos < p->len && p->src[p->pos] == '`') {
            p->pos++;
        } else {
            parser_fail(p, tline, tcol, "unterminated template literal");
        }
        p->cur.type = TOK_TEMPLATE;
        p->cur.text = p->src + start;
        p->cur.len = raw_len;
        p->tpl_line = tline;
        p->tpl_col = tcol + 1u;
        return;
    }

    /* String literals ('...', "...") */
    if (c == '\'' || c == '"') {
        lex_string_literal(p, c);
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
        parser_fail(p, p->cur_line, p->cur_col, "unexpected character");
        p->cur.type = TOK_EOF;
        break;
    }
}

/* =========================================================================
 * Recursive-Descent AST Parser
 * ========================================================================= */

static ast_node_t *alloc_node(js_parser_t *p, ast_type_t type)
{
    ast_node_t *n = (ast_node_t *)parser_alloc(p, sizeof(ast_node_t));
    if (n) {
        n->type = type;
    }
    return n;
}

static bool parser_expect(js_parser_t *p, js_tok_type_t tok, const char *msg)
{
    if (p->cur.type == tok) {
        lexer_next(p);
        return true;
    }
    parser_fail(p, p->cur_line, p->cur_col, msg);
    return false;
}

static ast_node_t *parse_expr(js_parser_t *p);
static ast_node_t *parse_stmt(js_parser_t *p);
static ast_node_t *parse_func_decl(js_parser_t *p, bool is_exported);

static __attribute__((noinline)) ast_node_t *parse_template_lit(js_parser_t *p)
{
    const char *raw = p->cur.text ? p->cur.text : "";
    size_t rlen = p->cur.len;
    uint32_t cur_line = p->tpl_line > 0u ? p->tpl_line : 1u;
    uint32_t cur_col = p->tpl_col > 0u ? p->tpl_col : 1u;
    lexer_next(p);

    if (p->depth > 32u) {
        parser_fail_depth(p, cur_line, cur_col, "template nesting depth exceeded");
        p->cur.type = TOK_EOF;
        return alloc_node(p, AST_LITERAL);
    }

    ast_node_t *n = alloc_node(p, AST_TEMPLATE);
    if (!n && !p->ctx->lint_mode) {
        return NULL;
    }

    ast_ptr_list_t parts = {0};
    ast_ptr_list_t exprs = {0};
    parse_strbuf_t seg = {0};
    size_t i = 0;

    while (i < rlen && !parser_aborted(p)) {
        if (raw[i] == '\\' && i + 1 < rlen) {
            char esc = raw[i + 1];
            char out_c = esc;
            if (esc == 'n') {
                out_c = '\n';
            } else if (esc == 'r') {
                out_c = '\r';
            } else if (esc == 't') {
                out_c = '\t';
            }
            if (!parse_strbuf_push(&seg, p, cur_line, cur_col, out_c)) {
                break;
            }
            if (esc == '\n') {
                cur_line++;
                cur_col = 1u;
            } else {
                cur_col += 2u;
            }
            i += 2;
            continue;
        }
        if (raw[i] == '$' && i + 1 < rlen && raw[i + 1] == '{') {
            if (!p->ctx->lint_mode) {
                const char *seg_s = parser_strdup(p, seg.buf ? seg.buf : "", seg.len);
                if (parser_aborted(p) || !ast_ptr_list_push(p, &parts, (void *)seg_s)) {
                    break;
                }
            }
            seg.len = 0u;
            uint32_t dollar_line = cur_line;
            uint32_t dollar_col = cur_col;
            uint32_t expr_line = cur_line;
            uint32_t expr_col = cur_col + 2u;
            cur_col += 2u;
            i += 2;
            size_t expr_start = i;
            scan_template_expr(raw, rlen, &i, 0);
            size_t expr_len = i - expr_start;
            for (size_t k = expr_start; k < i; k++) {
                if (raw[k] == '\n') {
                    cur_line++;
                    cur_col = 1u;
                } else {
                    cur_col++;
                }
            }
            if (i < rlen && raw[i] == '}') {
                cur_col++;
                i++;
            } else {
                parser_fail(p, dollar_line, dollar_col,
                            "unterminated template expression '${...}'");
            }
            size_t mark = p->ctx->lint_used;
            js_parser_t sub = {
                .ctx = p->ctx,
                .src = raw + expr_start,
                .len = expr_len,
                .pos = 0,
                .line_start = 0,
                .line = expr_line,
                .col_offset = expr_col - 1u,
                .depth = p->depth + 1u,
            };
            lexer_next(&sub);
            ast_node_t *expr_node = parse_expr(&sub);
            if (p->ctx->lint_mode) {
                p->ctx->lint_used = mark;
            } else if (!ast_ptr_list_push(p, &exprs, expr_node)) {
                break;
            }
            continue;
        }
        if (!parse_strbuf_push(&seg, p, cur_line, cur_col, raw[i])) {
            break;
        }
        if (raw[i] == '\n') {
            cur_line++;
            cur_col = 1u;
        } else {
            cur_col++;
        }
        i++;
    }

    if (!p->ctx->lint_mode && !parser_aborted(p)) {
        const char *tail_s = parser_strdup(p, seg.buf ? seg.buf : "", seg.len);
        (void)ast_ptr_list_push(p, &parts, (void *)tail_s);
    }
    parse_strbuf_free(&seg);

    if (n && !p->ctx->lint_mode) {
        uint32_t part_cnt = 0u;
        uint32_t expr_cnt = 0u;
        n->tpl.parts = (const char **)ast_ptr_list_flatten(p, &parts, &part_cnt);
        if (exprs.total > 0u) {
            n->tpl.exprs = (ast_node_t **)ast_ptr_list_flatten(p, &exprs, &expr_cnt);
        }
        n->tpl.expr_count = expr_cnt;
    }
    return n;
}

static __attribute__((noinline)) ast_node_t *parse_array_lit(js_parser_t *p)
{
    lexer_next(p);
    ast_node_t *n = alloc_node(p, AST_ARRAY_LIT);
    ast_ptr_list_t items = {0};
    while (p->cur.type != TOK_RBRACK && p->cur.type != TOK_EOF && !parser_aborted(p)) {
        size_t mark = p->ctx->lint_used;
        ast_node_t *elem = parse_expr(p);
        if (p->ctx->lint_mode) {
            p->ctx->lint_used = mark;
        } else if (!ast_ptr_list_push(p, &items, elem)) {
            break;
        }
        if (p->cur.type == TOK_COMMA) {
            lexer_next(p);
        } else {
            break;
        }
    }
    parser_expect(p, TOK_RBRACK, "expected ']'");
    if (n && items.total > 0u && !p->ctx->lint_mode) {
        uint32_t cnt = 0u;
        n->children = (ast_node_t **)ast_ptr_list_flatten(p, &items, &cnt);
        n->child_count = cnt;
    }
    return n;
}

static __attribute__((noinline)) ast_node_t *parse_obj_lit(js_parser_t *p)
{
    lexer_next(p);
    ast_node_t *n = alloc_node(p, AST_OBJ_LIT);
    ast_ptr_list_t keys = {0};
    ast_ptr_list_t vals = {0};
    while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !parser_aborted(p)) {
        size_t mark = p->ctx->lint_used;
        const char *k = NULL;
        if (p->cur.type == TOK_IDENT || p->cur.type == TOK_STR) {
            k = p->cur.text;
            lexer_next(p);
        } else if (p->cur.type == TOK_NUM) {
            if (!p->ctx->lint_mode) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "%g", p->cur.num);
                k = parser_strdup(p, kbuf, strlen(kbuf));
            }
            lexer_next(p);
        } else {
            parser_fail(p, p->cur_line, p->cur_col, "expected object property name");
            break;
        }
        parser_expect(p, TOK_COLON, "expected ':' in object literal");
        ast_node_t *vnode = parse_expr(p);
        if (p->ctx->lint_mode) {
            p->ctx->lint_used = mark;
        } else {
            if (!ast_ptr_list_push(p, &keys, (void *)k) || !ast_ptr_list_push(p, &vals, vnode)) {
                break;
            }
        }
        if (p->cur.type == TOK_COMMA) {
            lexer_next(p);
        } else {
            break;
        }
    }
    parser_expect(p, TOK_RBRACE, "expected '}'");
    if (n && vals.total > 0u && !p->ctx->lint_mode) {
        uint32_t kcnt = 0u;
        uint32_t vcnt = 0u;
        n->keys = (const char **)ast_ptr_list_flatten(p, &keys, &kcnt);
        n->children = (ast_node_t **)ast_ptr_list_flatten(p, &vals, &vcnt);
        if (n->keys && n->children && kcnt == vcnt) {
            n->child_count = vcnt;
        }
    }
    return n;
}

static __attribute__((noinline)) void parse_arg_list_into(js_parser_t *p, ast_node_t *target)
{
    lexer_next(p);
    ast_ptr_list_t args = {0};
    while (p->cur.type != TOK_RPAREN && p->cur.type != TOK_EOF && !parser_aborted(p)) {
        size_t mark = p->ctx->lint_used;
        ast_node_t *arg = parse_expr(p);
        if (p->ctx->lint_mode) {
            p->ctx->lint_used = mark;
        } else if (!ast_ptr_list_push(p, &args, arg)) {
            break;
        }
        if (p->cur.type == TOK_COMMA) {
            lexer_next(p);
        } else {
            break;
        }
    }
    parser_expect(p, TOK_RPAREN, "expected ')'");
    if (target && args.total > 0u && !p->ctx->lint_mode) {
        uint32_t cnt = 0u;
        target->children = (ast_node_t **)ast_ptr_list_flatten(p, &args, &cnt);
        target->child_count = cnt;
    }
}

static __attribute__((noinline)) ast_node_t *parse_new_expr(js_parser_t *p)
{
    lexer_next(p);
    ast_node_t *n = alloc_node(p, AST_NEW);
    if (p->cur.type == TOK_IDENT) {
        if (n) {
            n->name = p->cur.text;
        }
        lexer_next(p);
    } else {
        parser_fail(p, p->cur_line, p->cur_col, "expected constructor name after 'new'");
    }
    if (p->cur.type == TOK_LPAREN) {
        parse_arg_list_into(p, n);
    }
    return n;
}

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
    if (t == TOK_TEMPLATE) {
        return parse_template_lit(p);
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
        parser_expect(p, TOK_RPAREN, "expected ')'");
        return e;
    }
    if (t == TOK_LBRACK) {
        return parse_array_lit(p);
    }
    if (t == TOK_LBRACE) {
        return parse_obj_lit(p);
    }
    if (t == TOK_NEW) {
        return parse_new_expr(p);
    }

    parser_fail(p, p->cur_line, p->cur_col, "unexpected token");
    lexer_next(p);
    return alloc_node(p, AST_LITERAL);
}

static bool parser_step_chain(js_parser_t *p, uint32_t *chain_steps)
{
    if (++(*chain_steps) > 256u) {
        parser_fail_depth(p, p->cur_line, p->cur_col, "expression chain depth exceeded");
        p->cur.type = TOK_EOF;
        return false;
    }
    return true;
}

static ast_node_t *parse_postfix(js_parser_t *p)
{
    ast_node_t *lhs = parse_primary(p);
    uint32_t chain_steps = 0;
    while (p->cur.type != TOK_EOF && !parser_aborted(p)) {
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        if (p->cur.type == TOK_DOT) {
            lexer_next(p);
            const char *prop = p->cur.text;
            if (p->cur.type == TOK_IDENT) {
                lexer_next(p);
            } else {
                parser_fail(p, p->cur_line, p->cur_col, "expected property name after '.'");
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
            parser_expect(p, TOK_RBRACK, "expected ']'");
            ast_node_t *ix = alloc_node(p, AST_INDEX);
            if (ix) {
                ix->left = lhs;
                ix->right = idx;
            }
            lhs = ix;
        } else if (p->cur.type == TOK_LPAREN) {
            ast_node_t *call = alloc_node(p, AST_CALL);
            if (call) {
                call->left = lhs;
            }
            parse_arg_list_into(p, call);
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
        parser_fail_depth(p, p->cur_line, p->cur_col, "expression recursion depth exceeded");
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

static int tok_binop_prec(js_tok_type_t t)
{
    switch (t) {
    case TOK_OR:
    case TOK_NULLISH:
        return 1;
    case TOK_AND:
        return 2;
    case TOK_BIT_AND:
    case TOK_BIT_XOR:
    case TOK_BIT_OR:
        return 3;
    case TOK_EQ:
    case TOK_STRICT_EQ:
    case TOK_NEQ:
    case TOK_STRICT_NEQ:
        return 4;
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:
        return 5;
    case TOK_SHL:
    case TOK_SHR:
    case TOK_USHR:
        return 6;
    case TOK_PLUS:
    case TOK_MINUS:
        return 7;
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:
        return 8;
    default:
        return 0;
    }
}

static ast_node_t *parse_binop(js_parser_t *p, int min_prec)
{
    ast_node_t *lhs = parse_unary(p);
    uint32_t chain_steps = 0;
    while (!parser_aborted(p)) {
        int prec = tok_binop_prec(p->cur.type);
        if (prec < min_prec) {
            break;
        }
        if (!parser_step_chain(p, &chain_steps)) {
            break;
        }
        js_tok_type_t op = p->cur.type;
        lexer_next(p);
        ast_node_t *rhs = parse_binop(p, prec + 1);
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

static ast_node_t *parse_expr(js_parser_t *p)
{
    if (++p->depth > 64u) {
        p->depth--;
        parser_fail_depth(p, p->cur_line, p->cur_col, "expression recursion depth exceeded");
        p->cur.type = TOK_EOF;
        return alloc_node(p, AST_LITERAL);
    }
    ast_node_t *lhs = parse_binop(p, 1);
    if (p->cur.type == TOK_QUESTION) {
        lexer_next(p);
        ast_node_t *t_br = parse_expr(p);
        parser_expect(p, TOK_COLON, "expected ':' in conditional expression");
        ast_node_t *e_br = parse_expr(p);
        ast_node_t *tn = alloc_node(p, AST_TERNARY);
        if (tn) {
            tn->cond = lhs;
            tn->then_branch = t_br;
            tn->else_branch = e_br;
        }
        lhs = tn;
    }
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

static __attribute__((noinline)) ast_node_t *parse_var_decl(js_parser_t *p, bool consume_semi)
{
    lexer_next(p); /* skip var/let/const */
    ast_node_t *n = alloc_node(p, AST_VAR_DECL);
    ast_var_list_t items = {0};
    bool had_any = false;

    while (p->cur.type == TOK_IDENT && !parser_aborted(p)) {
        had_any = true;
        size_t mark = p->ctx->lint_used;
        const char *name = p->cur.text;
        ast_node_t *init = NULL;
        lexer_next(p);
        if (p->cur.type == TOK_ASSIGN) {
            lexer_next(p);
            init = parse_expr(p);
        }
        if (p->ctx->lint_mode) {
            p->ctx->lint_used = mark;
        } else if (!ast_var_list_push(p, &items, name, init)) {
            break;
        }
        if (p->cur.type == TOK_COMMA) {
            lexer_next(p);
        } else {
            break;
        }
    }
    if (!had_any) {
        parser_fail(p, p->cur_line, p->cur_col, "expected variable identifier");
    }
    if (consume_semi && p->cur.type == TOK_SEMI) {
        lexer_next(p);
    }
    if (n && items.total > 0u && !p->ctx->lint_mode) {
        uint32_t cnt = 0u;
        n->decls = ast_var_list_flatten(p, &items, &cnt);
        n->decl_count = cnt;
    }
    return n;
}

static __attribute__((noinline)) ast_node_t *parse_func_decl(js_parser_t *p, bool is_exported)
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
    if (parser_expect(p, TOK_LPAREN, "expected '(' after function name")) {
        ast_ptr_list_t params = {0};
        while (p->cur.type == TOK_IDENT && !parser_aborted(p)) {
            const char *pname = p->cur.text;
            if (!ast_ptr_list_push(p, &params, (void *)pname)) {
                break;
            }
            lexer_next(p);
            if (p->cur.type == TOK_COMMA) {
                lexer_next(p);
            } else {
                break;
            }
        }
        parser_expect(p, TOK_RPAREN, "expected ')' after parameter list");
        if (fn && params.total > 0u && !p->ctx->lint_mode) {
            uint32_t cnt = 0u;
            fn->params = (const char **)ast_ptr_list_flatten(p, &params, &cnt);
            fn->param_count = cnt;
        }
    }
    if (p->cur.type != TOK_LBRACE) {
        parser_fail(p, p->cur_line, p->cur_col, "expected '{' for function body");
    } else if (fn) {
        fn->body = parse_stmt(p);
    } else {
        (void)parse_stmt(p);
    }
    return fn;
}

static void parse_stmt_list(js_parser_t *p, ast_node_t *container, js_tok_type_t stop_tok)
{
    ast_ptr_list_t stmts = {0};
    while (p->cur.type != stop_tok && p->cur.type != TOK_EOF && !parser_aborted(p)) {
        if (p->ctx->lint_mode) {
            size_t mark = p->ctx->lint_used;
            (void)parse_stmt(p);
            p->ctx->lint_used = mark;
            continue;
        }
        ast_node_t *st = parse_stmt(p);
        if (!ast_ptr_list_push(p, &stmts, st)) {
            break;
        }
    }
    if (container && stmts.total > 0u && !p->ctx->lint_mode) {
        uint32_t cnt = 0u;
        container->children = (ast_node_t **)ast_ptr_list_flatten(p, &stmts, &cnt);
        container->child_count = cnt;
    }
}

static ast_node_t *parse_block(js_parser_t *p)
{
    lexer_next(p);
    ast_node_t *blk = alloc_node(p, AST_BLOCK);
    parse_stmt_list(p, blk, TOK_RBRACE);
    parser_expect(p, TOK_RBRACE, "expected '}'");
    return blk;
}

static __attribute__((noinline)) ast_node_t *parse_switch_stmt(js_parser_t *p)
{
    lexer_next(p);
    parser_expect(p, TOK_LPAREN, "expected '(' after 'switch'");
    ast_node_t *disc = parse_expr(p);
    parser_expect(p, TOK_RPAREN, "expected ')' after switch expression");
    parser_expect(p, TOK_LBRACE, "expected '{' in switch statement");
    ast_node_t *n = alloc_node(p, AST_SWITCH);
    if (n) {
        n->cond = disc;
    }
    ast_ptr_list_t case_exprs = {0};
    ast_ptr_list_t case_bodies = {0};
    while (p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF && !parser_aborted(p)) {
        ast_node_t *cexpr = NULL;
        if (p->cur.type == TOK_CASE) {
            lexer_next(p);
            cexpr = parse_expr(p);
            parser_expect(p, TOK_COLON, "expected ':' after case expression");
        } else if (p->cur.type == TOK_DEFAULT) {
            lexer_next(p);
            parser_expect(p, TOK_COLON, "expected ':' after 'default'");
        } else {
            parser_fail(p, p->cur_line, p->cur_col, "expected 'case' or 'default'");
            (void)parse_stmt(p);
            continue;
        }
        ast_node_t *cblk = alloc_node(p, AST_BLOCK);
        ast_ptr_list_t cstmts = {0};
        while (p->cur.type != TOK_CASE && p->cur.type != TOK_DEFAULT && p->cur.type != TOK_RBRACE &&
               p->cur.type != TOK_EOF && !parser_aborted(p)) {
            if (p->ctx->lint_mode) {
                size_t mark = p->ctx->lint_used;
                (void)parse_stmt(p);
                p->ctx->lint_used = mark;
                continue;
            }
            ast_node_t *st = parse_stmt(p);
            if (!ast_ptr_list_push(p, &cstmts, st)) {
                break;
            }
        }
        if (cblk && cstmts.total > 0u && !p->ctx->lint_mode) {
            uint32_t scnt = 0u;
            cblk->children = (ast_node_t **)ast_ptr_list_flatten(p, &cstmts, &scnt);
            cblk->child_count = scnt;
        }
        if (!p->ctx->lint_mode) {
            if (!ast_ptr_list_push(p, &case_exprs, cexpr) ||
                !ast_ptr_list_push(p, &case_bodies, cblk)) {
                break;
            }
        }
    }
    parser_expect(p, TOK_RBRACE, "expected '}' closing switch");
    if (n && case_bodies.total > 0u && !p->ctx->lint_mode) {
        uint32_t ecnt = 0u;
        uint32_t bcnt = 0u;
        n->case_exprs = (ast_node_t **)ast_ptr_list_flatten(p, &case_exprs, &ecnt);
        n->children = (ast_node_t **)ast_ptr_list_flatten(p, &case_bodies, &bcnt);
        if (n->case_exprs && n->children && ecnt == bcnt) {
            n->child_count = bcnt;
        }
    }
    return n;
}

static __attribute__((noinline)) ast_node_t *parse_try_stmt(js_parser_t *p)
{
    lexer_next(p);
    if (p->cur.type != TOK_LBRACE) {
        parser_fail(p, p->cur_line, p->cur_col, "expected '{' after 'try'");
    }
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
            } else {
                parser_fail(p, p->cur_line, p->cur_col, "expected catch identifier");
            }
            parser_expect(p, TOK_RPAREN, "expected ')' after catch identifier");
        }
        if (p->cur.type != TOK_LBRACE) {
            parser_fail(p, p->cur_line, p->cur_col, "expected '{' for catch body");
        }
        catch_body = parse_stmt(p);
    }
    if (p->cur.type == TOK_FINALLY) {
        lexer_next(p);
        if (p->cur.type != TOK_LBRACE) {
            parser_fail(p, p->cur_line, p->cur_col, "expected '{' for finally body");
        }
        finally_body = parse_stmt(p);
    }
    if (!catch_body && !finally_body) {
        parser_fail(p, p->cur_line, p->cur_col, "expected 'catch' or 'finally' after try");
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

static __attribute__((noinline)) ast_node_t *parse_for_stmt(js_parser_t *p)
{
    lexer_next(p);
    parser_expect(p, TOK_LPAREN, "expected '(' after 'for'");
    ast_node_t *init = NULL;
    if (p->cur.type == TOK_VAR || p->cur.type == TOK_LET || p->cur.type == TOK_CONST) {
        init = parse_var_decl(p, false);
    } else if (p->cur.type != TOK_SEMI) {
        init = parse_expr(p);
    }
    parser_expect(p, TOK_SEMI, "expected ';' after for initializer");
    ast_node_t *cond = NULL;
    if (p->cur.type != TOK_SEMI) {
        cond = parse_expr(p);
    }
    parser_expect(p, TOK_SEMI, "expected ';' after for condition");
    ast_node_t *step = NULL;
    if (p->cur.type != TOK_RPAREN) {
        step = parse_expr(p);
    }
    parser_expect(p, TOK_RPAREN, "expected ')' after for clauses");
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

static ast_node_t *parse_stmt_inner(js_parser_t *p)
{
    if (p->cur.type == TOK_SEMI) {
        lexer_next(p);
        return alloc_node(p, AST_BLOCK);
    }
    if (p->cur.type == TOK_LBRACE) {
        return parse_block(p);
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
        parser_fail(p, p->cur_line, p->cur_col, "expected declaration after 'export'");
        return alloc_node(p, AST_BLOCK);
    }
    if (p->cur.type == TOK_FUNCTION) {
        return parse_func_decl(p, false);
    }
    if (p->cur.type == TOK_VAR || p->cur.type == TOK_LET || p->cur.type == TOK_CONST) {
        return parse_var_decl(p, true);
    }
    if (p->cur.type == TOK_IF) {
        lexer_next(p);
        parser_expect(p, TOK_LPAREN, "expected '(' after 'if'");
        ast_node_t *cond = parse_expr(p);
        parser_expect(p, TOK_RPAREN, "expected ')' after if condition");
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
        return parse_switch_stmt(p);
    }
    if (p->cur.type == TOK_TRY) {
        return parse_try_stmt(p);
    }
    if (p->cur.type == TOK_THROW) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_THROW);
        if (p->cur.type != TOK_SEMI && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
            if (n) {
                n->left = parse_expr(p);
            } else {
                (void)parse_expr(p);
            }
        }
        if (p->cur.type == TOK_SEMI) {
            lexer_next(p);
        }
        return n;
    }
    if (p->cur.type == TOK_WHILE) {
        lexer_next(p);
        parser_expect(p, TOK_LPAREN, "expected '(' after 'while'");
        ast_node_t *cond = parse_expr(p);
        parser_expect(p, TOK_RPAREN, "expected ')' after while condition");
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
        parser_expect(p, TOK_WHILE, "expected 'while' after do body");
        parser_expect(p, TOK_LPAREN, "expected '(' after 'while'");
        ast_node_t *cond = parse_expr(p);
        parser_expect(p, TOK_RPAREN, "expected ')' after do-while condition");
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
        return parse_for_stmt(p);
    }
    if (p->cur.type == TOK_RETURN) {
        lexer_next(p);
        ast_node_t *n = alloc_node(p, AST_RETURN);
        if (p->cur.type != TOK_SEMI && p->cur.type != TOK_RBRACE && p->cur.type != TOK_EOF) {
            if (n) {
                n->left = parse_expr(p);
            } else {
                (void)parse_expr(p);
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
        parser_fail_depth(p, p->cur_line, p->cur_col, "statement nesting depth exceeded");
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
    parse_stmt_list(p, prog, TOK_EOF);
    return prog;
}

ast_node_t *we_script_parse_program(we_script_t *vm, const char *src, size_t len)
{
    js_parse_ctx_t ctx = {
        .vm = vm,
    };
    js_parser_t parser = {
        .ctx = &ctx,
        .src = src,
        .len = len,
        .pos = 0,
        .line_start = 0,
        .line = 1u,
        .depth = 0,
    };
    lexer_next(&parser);
    return parse_program(&parser);
}

bool we_script_lint(const char *src, size_t len, uint32_t flags, we_script_diag_t *out_diag)
{
    we_script_diag_t local_diag;
    memset(&local_diag, 0, sizeof(local_diag));

    if (!src && len > 0u) {
        local_diag.status_code = WE_SCRIPT_LINT_SYNTAX_ERR;
        local_diag.error_line = 1u;
        local_diag.error_col = 1u;
        snprintf(local_diag.error_msg, sizeof(local_diag.error_msg), "%s", "null script source");
        if (out_diag) {
            *out_diag = local_diag;
        }
        return false;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Bounded 56 KiB scratch buffer keeps total lint stack/arena under 64 KiB */
    uint8_t scratch_arena[57344];
    js_parse_ctx_t ctx = {
        .vm = NULL,
        .lint_arena = scratch_arena,
        .lint_cap = sizeof(scratch_arena),
        .lint_used = 0u,
        .lint_mode = true,
        .lint_flags = flags,
    };
    js_parser_t parser = {
        .ctx = &ctx,
        .src = src ? src : "",
        .len = len,
        .pos = 0,
        .line_start = 0,
        .line = 1u,
        .depth = 0,
    };

    lexer_next(&parser);
    (void)parse_program(&parser);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t us = ((int64_t)(t1.tv_sec - t0.tv_sec) * 1000000LL) +
                 ((int64_t)(t1.tv_nsec - t0.tv_nsec) / 1000LL);
    local_diag.lint_time_us = (us > 0) ? (uint32_t)us : 0u;

    if (ctx.depth_exceeded) {
        local_diag.status_code = WE_SCRIPT_LINT_DEPTH_EXCEEDED;
        local_diag.error_line = ctx.err_line > 0u ? ctx.err_line : 1u;
        local_diag.error_col = ctx.err_col > 0u ? ctx.err_col : 1u;
        snprintf(local_diag.error_msg, sizeof(local_diag.error_msg), "%s",
                 ctx.err_msg[0] ? ctx.err_msg : "nesting depth exceeded");
    } else if (ctx.has_error) {
        local_diag.status_code = WE_SCRIPT_LINT_SYNTAX_ERR;
        local_diag.error_line = ctx.err_line > 0u ? ctx.err_line : 1u;
        local_diag.error_col = ctx.err_col > 0u ? ctx.err_col : 1u;
        snprintf(local_diag.error_msg, sizeof(local_diag.error_msg), "%s",
                 ctx.err_msg[0] ? ctx.err_msg : "syntax error");
    } else {
        local_diag.status_code = WE_SCRIPT_LINT_OK;
    }

    if (out_diag) {
        *out_diag = local_diag;
    }
    return local_diag.status_code == WE_SCRIPT_LINT_OK;
}
