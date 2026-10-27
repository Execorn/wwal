// SPDX-License-Identifier: GPL-3.0-or-later
#include "we_script_internal.h"

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
        char stack_tmp[1024];
        stack_tmp[0] = '\0';
        char *buf = stack_tmp;
        size_t cap = sizeof(stack_tmp);
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
                case '0':
                    ch = '\0';
                    break;
                default:
                    ch = esc;
                    break;
                }
            }
            if (tlen + 1 >= cap) {
                size_t ncap = cap * 2u;
                char *nbuf = (buf == stack_tmp) ? (char *)malloc(ncap) : (char *)realloc(buf, ncap);
                if (nbuf) {
                    if (buf == stack_tmp) {
                        memcpy(nbuf, stack_tmp, tlen);
                    }
                    buf = nbuf;
                    cap = ncap;
                }
            }
            if (tlen + 1 < cap) {
                buf[tlen++] = ch;
            }
        }
        if (p->pos < p->len && p->src[p->pos] == quote) {
            p->pos++;
        }
        buf[tlen] = '\0';
        p->cur.type = TOK_STR;
        p->cur.text = script_arena_strdup(p->vm, buf, tlen);
        if (buf != stack_tmp) {
            free(buf);
        }
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

ast_node_t *we_script_parse_program(we_script_t *vm, const char *src, size_t len)
{
    js_parser_t parser = {
        .vm = vm,
        .src = src,
        .len = len,
        .pos = 0,
        .depth = 0,
    };
    lexer_next(&parser);
    return parse_program(&parser);
}
