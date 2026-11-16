// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/image_loader.h"
#include "waywal/os_compat.h"
#include "waywal/we_asset_writer.h"
#include "waywal/we_json.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_particle.h"
#include "waywal/we_shader.h"
#include "waywal/wwpkg.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "we_script_internal.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#endif
#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#endif
#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    we_asset_writer_err_t err;
} json_buf_t;

typedef struct {
    char *path;
    uint8_t *data;
    size_t size;
} owned_asset_t;

typedef struct {
    owned_asset_t *items;
    size_t count;
    size_t cap;
} asset_list_t;

typedef struct {
    const we_material_pass_t *pass;
    char shader_name[128];
} shader_override_t;

typedef struct {
    shader_override_t *shader_overrides;
    size_t shader_override_count;
    size_t shader_override_cap;
} export_ctx_t;

static pthread_once_t g_cloc_once = PTHREAD_ONCE_INIT;
static locale_t g_cloc = (locale_t)0;
static atomic_uint_fast64_t g_atomic_tmp_seq = 0;

static void init_c_locale_once(void) { g_cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0); }

static locale_t get_c_locale(void)
{
    pthread_once(&g_cloc_once, init_c_locale_once);
    return g_cloc;
}

static inline void set_err(we_asset_writer_err_t *out_err, we_asset_writer_err_t err)
{
    if (out_err) {
        *out_err = err;
    }
}

static void jb_init(json_buf_t *jb)
{
    memset(jb, 0, sizeof(*jb));
    jb->err = WE_WRITER_OK;
}

static void jb_destroy(json_buf_t *jb)
{
    free(jb->data);
    jb->data = NULL;
    jb->len = 0u;
    jb->cap = 0u;
}

static bool jb_reserve(json_buf_t *jb, size_t extra)
{
    if (jb->err != WE_WRITER_OK) {
        return false;
    }
    if (extra > SIZE_MAX - jb->len - 1u) {
        jb->err = WE_WRITER_ERR_OVERFLOW;
        return false;
    }
    size_t need = jb->len + extra + 1u;
    if (need <= jb->cap) {
        return true;
    }
    size_t new_cap = (jb->cap == 0u) ? 4096u : jb->cap;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = need;
            break;
        }
        new_cap *= 2u;
    }
    char *grown = (char *)realloc(jb->data, new_cap);
    if (!grown) {
        jb->err = WE_WRITER_ERR_OOM;
        return false;
    }
    jb->data = grown;
    jb->cap = new_cap;
    return true;
}

static bool jb_raw(json_buf_t *jb, const char *s, size_t n)
{
    if (!jb_reserve(jb, n)) {
        return false;
    }
    if (n > 0u) {
        memcpy(jb->data + jb->len, s, n);
        jb->len += n;
    }
    jb->data[jb->len] = '\0';
    return true;
}

static bool jb_cstr(json_buf_t *jb, const char *s) { return jb_raw(jb, s, strlen(s)); }

static bool jb_ch(json_buf_t *jb, char c) { return jb_raw(jb, &c, 1u); }

static bool jb_str(json_buf_t *jb, const char *s)
{
    if (!s) {
        s = "";
    }
    if (!jb_ch(jb, '"')) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            if (!jb_cstr(jb, "\\\"")) {
                return false;
            }
            break;
        case '\\':
            if (!jb_cstr(jb, "\\\\")) {
                return false;
            }
            break;
        case '\b':
            if (!jb_cstr(jb, "\\b")) {
                return false;
            }
            break;
        case '\f':
            if (!jb_cstr(jb, "\\f")) {
                return false;
            }
            break;
        case '\n':
            if (!jb_cstr(jb, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!jb_cstr(jb, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!jb_cstr(jb, "\\t")) {
                return false;
            }
            break;
        default:
            if (c < 0x20u) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)c);
                if (!jb_cstr(jb, esc)) {
                    return false;
                }
            } else {
                if (!jb_ch(jb, (char)c)) {
                    return false;
                }
            }
            break;
        }
    }
    return jb_ch(jb, '"');
}

static bool jb_key(json_buf_t *jb, bool *first, const char *k)
{
    if (!*first && !jb_ch(jb, ',')) {
        return false;
    }
    *first = false;
    return jb_str(jb, k) && jb_ch(jb, ':');
}

static bool format_f32_c(json_buf_t *jb, float v, char *out, size_t out_cap)
{
    if (!isfinite(v)) {
        jb->err = WE_WRITER_ERR_NON_FINITE;
        return false;
    }
    if (v == 0.0f) {
        v = 0.0f;
    }

    locale_t cloc = get_c_locale();
    locale_t old_loc = (locale_t)0;
    if (cloc != (locale_t)0) {
        old_loc = uselocale(cloc);
    }

    bool ok = false;
    for (int prec = 6; prec <= 9; ++prec) {
        int n = snprintf(out, out_cap, "%.*g", prec, (double)v);
        if (n <= 0 || (size_t)n >= out_cap) {
            continue;
        }
        char *end = NULL;
        float rt = strtof(out, &end);
        if (end && *end == '\0' && rt == v) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        snprintf(out, out_cap, "%.9g", (double)v);
    }

    if (cloc != (locale_t)0) {
        uselocale(old_loc);
    }
    return true;
}

static bool jb_f32(json_buf_t *jb, float v)
{
    char buf[64];
    if (!format_f32_c(jb, v, buf, sizeof(buf))) {
        return false;
    }
    return jb_cstr(jb, buf);
}

static bool jb_i64(json_buf_t *jb, long long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return jb_cstr(jb, buf);
}

static bool jb_vec_str(json_buf_t *jb, const float *vals, size_t n)
{
    if (!jb_ch(jb, '"')) {
        return false;
    }
    for (size_t i = 0u; i < n; ++i) {
        char buf[64];
        if (!format_f32_c(jb, vals[i], buf, sizeof(buf))) {
            return false;
        }
        if (i > 0u && !jb_ch(jb, ' ')) {
            return false;
        }
        if (!jb_cstr(jb, buf)) {
            return false;
        }
    }
    return jb_ch(jb, '"');
}

static bool jb_user_value(json_buf_t *jb, const char *user_prop)
{
    const char *neq = strstr(user_prop, "!=");
    const char *eq = neq ? NULL : strchr(user_prop, '=');
    if (neq) {
        size_t klen = (size_t)(neq - user_prop);
        char key[64];
        if (klen >= sizeof(key)) {
            klen = sizeof(key) - 1u;
        }
        memcpy(key, user_prop, klen);
        key[klen] = '\0';
        char cond[96];
        int cn = snprintf(cond, sizeof(cond), "!%s", neq + 2);
        if (cn < 0 || (size_t)cn >= sizeof(cond)) {
            jb->err = WE_WRITER_ERR_LIMIT_EXCEEDED;
            return false;
        }
        return jb_cstr(jb, "{\"condition\":") && jb_str(jb, cond) && jb_cstr(jb, ",\"name\":") &&
               jb_str(jb, key) && jb_ch(jb, '}');
    }
    if (eq) {
        size_t klen = (size_t)(eq - user_prop);
        char key[64];
        if (klen >= sizeof(key)) {
            klen = sizeof(key) - 1u;
        }
        memcpy(key, user_prop, klen);
        key[klen] = '\0';
        return jb_cstr(jb, "{\"condition\":") && jb_str(jb, eq + 1) && jb_cstr(jb, ",\"name\":") &&
               jb_str(jb, key) && jb_ch(jb, '}');
    }
    return jb_str(jb, user_prop);
}

/* Reconstructs {"user": ..., "value": ...} matching we_jval_floats condition encoding. */
static bool jb_user_prefix(json_buf_t *jb, const char *user_prop, bool *wrapped)
{
    *wrapped = false;
    if (!user_prop || user_prop[0] == '\0') {
        return true;
    }
    *wrapped = true;
    return jb_cstr(jb, "{\"user\":") && jb_user_value(jb, user_prop) && jb_cstr(jb, ",\"value\":");
}

static bool jb_bound_f32(json_buf_t *jb, float val, const char *user_prop)
{
    bool wrapped = false;
    if (!jb_user_prefix(jb, user_prop, &wrapped) || !jb_f32(jb, val)) {
        return false;
    }
    return wrapped ? jb_ch(jb, '}') : true;
}

static bool jb_bound_bool(json_buf_t *jb, bool val, const char *user_prop)
{
    bool wrapped = false;
    if (!jb_user_prefix(jb, user_prop, &wrapped) || !jb_cstr(jb, val ? "true" : "false")) {
        return false;
    }
    return wrapped ? jb_ch(jb, '}') : true;
}

static bool jb_bound_vec(json_buf_t *jb, const float *vals, size_t n, const char *user_prop)
{
    bool wrapped = false;
    if (!jb_user_prefix(jb, user_prop, &wrapped) || !jb_vec_str(jb, vals, n)) {
        return false;
    }
    return wrapped ? jb_ch(jb, '}') : true;
}

static bool jb_bound_str(json_buf_t *jb, const char *val, const char *user_prop)
{
    bool wrapped = false;
    if (!jb_user_prefix(jb, user_prop, &wrapped) || !jb_str(jb, val)) {
        return false;
    }
    return wrapped ? jb_ch(jb, '}') : true;
}

static bool jb_bound_i64(json_buf_t *jb, long long val, const char *user_prop)
{
    bool wrapped = false;
    if (!jb_user_prefix(jb, user_prop, &wrapped) || !jb_i64(jb, val)) {
        return false;
    }
    return wrapped ? jb_ch(jb, '}') : true;
}

static bool canonicalize_jb(json_buf_t *jb, char **out_json, size_t *out_len,
                            we_asset_writer_err_t *out_err)
{
    if (jb->err != WE_WRITER_OK || !jb->data) {
        set_err(out_err, jb->err != WE_WRITER_OK ? jb->err : WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    size_t canon_len = 0u;
    char *canon = wwpkg_canonicalize_json(jb->data, jb->len, &canon_len);
    if (!canon) {
        /* wwpkg_canonicalize_json rejects non-finite numbers and duplicate object keys. */
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    *out_json = canon;
    if (out_len) {
        *out_len = canon_len;
    }
    set_err(out_err, WE_WRITER_OK);
    return true;
}

static bool finish_jb(json_buf_t *jb, bool ok, char **out_json, size_t *out_len,
                      we_asset_writer_err_t *out_err)
{
    bool res = false;
    if (!ok) {
        set_err(out_err, jb->err != WE_WRITER_OK ? jb->err : WE_WRITER_ERR_INVALID_ARG);
    } else {
        res = canonicalize_jb(jb, out_json, out_len, out_err);
    }
    jb_destroy(jb);
    return res;
}

static const char *prop_type_name(we_prop_type_t t)
{
    switch (t) {
    case WE_PROP_BOOL:
        return "bool";
    case WE_PROP_INT:
        return "combo";
    case WE_PROP_COLOR:
        return "color";
    case WE_PROP_TEXT:
        return "textinput";
    case WE_PROP_FLOAT:
    default:
        return "slider";
    }
}

bool we_project_serialize_json(const we_project_t *proj, char **out_json, size_t *out_len,
                               we_asset_writer_err_t *out_err)
{
    if (!proj || !out_json) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    *out_json = NULL;
    if (out_len) {
        *out_len = 0u;
    }
    if (proj->prop_count > WE_MAX_USER_PROPS) {
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    bool first = true;

    const char *file_val = proj->file[0] != '\0' ? proj->file : "scene.json";
    const char *preview_val = proj->preview[0] != '\0' ? proj->preview : "preview.jpg";
    const char *title_val = proj->title[0] != '\0' ? proj->title : "Untitled Wallpaper";
    const char *type_val = proj->type[0] != '\0' ? proj->type : "scene";

    ok = ok && jb_key(&jb, &first, "file") && jb_str(&jb, file_val);
    ok = ok && jb_key(&jb, &first, "preview") && jb_str(&jb, preview_val);
    ok = ok && jb_key(&jb, &first, "title") && jb_str(&jb, title_val);
    ok = ok && jb_key(&jb, &first, "type") && jb_str(&jb, type_val);
    if (proj->workshop_id[0] != '\0') {
        ok = ok && jb_key(&jb, &first, "workshopid") && jb_str(&jb, proj->workshop_id);
    }

    ok = ok && jb_key(&jb, &first, "general") && jb_cstr(&jb, "{\"properties\":{");
    bool pfirst = true;
    for (uint32_t i = 0u; ok && i < proj->prop_count; ++i) {
        const we_user_prop_t *up = &proj->props[i];
        if (up->key[0] == '\0') {
            jb.err = WE_WRITER_ERR_INVALID_ARG;
            ok = false;
            break;
        }
        for (uint32_t c = 0u; c < 4u; ++c) {
            if (!isfinite(up->val[c])) {
                jb.err = WE_WRITER_ERR_NON_FINITE;
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }

        const char *label = up->text[0] != '\0' ? up->text : up->key;
        ok = ok && jb_key(&jb, &pfirst, up->key) && jb_ch(&jb, '{');
        bool pf = true;
        ok = ok && jb_key(&jb, &pf, "text") && jb_str(&jb, label);
        ok = ok && jb_key(&jb, &pf, "type") && jb_str(&jb, prop_type_name(up->type));
        ok = ok && jb_key(&jb, &pf, "value");
        if (up->type == WE_PROP_BOOL) {
            ok = ok && jb_cstr(&jb, up->val[0] != 0.0f ? "true" : "false");
        } else if (up->type == WE_PROP_INT) {
            ok = ok && jb_i64(&jb, (long long)llroundf(up->val[0]));
        } else if (up->type == WE_PROP_COLOR) {
            uint32_t ncomp =
                (up->num_components >= 3u && up->num_components <= 4u) ? up->num_components : 3u;
            ok = ok && jb_vec_str(&jb, up->val, ncomp);
        } else if (up->type == WE_PROP_TEXT) {
            ok = ok && jb_str(&jb, up->str_val);
        } else {
            ok = ok && jb_f32(&jb, up->val[0]);
        }
        ok = ok && jb_ch(&jb, '}');
    }
    ok = ok && jb_cstr(&jb, "}}");
    ok = ok && jb_ch(&jb, '}');

    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static const char *blend_mode_name(we_blend_mode_t b)
{
    switch (b) {
    case WE_BLEND_ADDITIVE:
        return "additive";
    case WE_BLEND_MULTIPLY:
        return "multiply";
    case WE_BLEND_SCREEN:
        return "screen";
    case WE_BLEND_DISABLED:
        return "disabled";
    case WE_BLEND_NORMAL:
    default:
        return "normal";
    }
}

static const char *light_type_name(uint32_t lt)
{
    switch (lt) {
    case WE_LIGHT_DIRECTIONAL:
        return "directional";
    case WE_LIGHT_SPOT:
        return "spot";
    case WE_LIGHT_TUBE:
        return "tube";
    case WE_LIGHT_POINT:
    default:
        return "point";
    }
}

static const char *sound_mode_name(we_sound_playback_mode_t m)
{
    switch (m) {
    case WE_SOUND_MODE_SINGLE:
        return "single";
    case WE_SOUND_MODE_RANDOM:
        return "random";
    case WE_SOUND_MODE_LOOP:
    default:
        return "loop";
    }
}

static const char *export_ctx_shader_name(const export_ctx_t *ectx, const we_material_pass_t *pass)
{
    if (ectx && pass) {
        for (size_t i = 0u; i < ectx->shader_override_count; ++i) {
            if (ectx->shader_overrides[i].pass == pass) {
                return ectx->shader_overrides[i].shader_name;
            }
        }
    }
    return pass ? pass->shader_name : "";
}

static bool script_unparse_str_lit(json_buf_t *jb, const char *s)
{
    if (!s) {
        s = "";
    }
    if (!jb_ch(jb, '"')) {
        return false;
    }
    for (const char *p = s; *p != '\0'; ++p) {
        switch (*p) {
        case '\\':
            if (!jb_cstr(jb, "\\\\")) {
                return false;
            }
            break;
        case '"':
            if (!jb_cstr(jb, "\\\"")) {
                return false;
            }
            break;
        case '\n':
            if (!jb_cstr(jb, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!jb_cstr(jb, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!jb_cstr(jb, "\\t")) {
                return false;
            }
            break;
        default:
            if (!jb_ch(jb, *p)) {
                return false;
            }
            break;
        }
    }
    return jb_ch(jb, '"');
}

static bool script_unparse_tpl_part(json_buf_t *jb, const char *s)
{
    if (!s) {
        return true;
    }
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        char c = s[i];
        if (c == '\\') {
            if (!jb_cstr(jb, "\\\\")) {
                return false;
            }
        } else if (c == '`') {
            if (!jb_cstr(jb, "\\`")) {
                return false;
            }
        } else if (c == '$' && s[i + 1u] == '{') {
            if (!jb_cstr(jb, "\\${")) {
                return false;
            }
            ++i;
        } else if (c == '\n') {
            if (!jb_cstr(jb, "\\n")) {
                return false;
            }
        } else if (c == '\r') {
            if (!jb_cstr(jb, "\\r")) {
                return false;
            }
        } else if (c == '\t') {
            if (!jb_cstr(jb, "\\t")) {
                return false;
            }
        } else if (!jb_ch(jb, c)) {
            return false;
        }
    }
    return true;
}

static const char *script_tok_op_str(js_tok_type_t op)
{
    switch (op) {
    case TOK_PLUS:
        return "+";
    case TOK_MINUS:
        return "-";
    case TOK_STAR:
        return "*";
    case TOK_SLASH:
        return "/";
    case TOK_PERCENT:
        return "%";
    case TOK_INC:
        return "++";
    case TOK_DEC:
        return "--";
    case TOK_ASSIGN:
        return "=";
    case TOK_PLUS_ASSIGN:
        return "+=";
    case TOK_MINUS_ASSIGN:
        return "-=";
    case TOK_STAR_ASSIGN:
        return "*=";
    case TOK_SLASH_ASSIGN:
        return "/=";
    case TOK_PERCENT_ASSIGN:
        return "%=";
    case TOK_SHL_ASSIGN:
        return "<<=";
    case TOK_SHR_ASSIGN:
        return ">>=";
    case TOK_USHR_ASSIGN:
        return ">>>=";
    case TOK_AND_ASSIGN:
        return "&=";
    case TOK_OR_ASSIGN:
        return "|=";
    case TOK_XOR_ASSIGN:
        return "^=";
    case TOK_EQ:
        return "==";
    case TOK_STRICT_EQ:
        return "===";
    case TOK_NEQ:
        return "!=";
    case TOK_STRICT_NEQ:
        return "!==";
    case TOK_LT:
        return "<";
    case TOK_LE:
        return "<=";
    case TOK_GT:
        return ">";
    case TOK_GE:
        return ">=";
    case TOK_AND:
        return "&&";
    case TOK_OR:
        return "||";
    case TOK_NULLISH:
        return "??";
    case TOK_NOT:
        return "!";
    case TOK_BIT_AND:
        return "&";
    case TOK_BIT_OR:
        return "|";
    case TOK_BIT_XOR:
        return "^";
    case TOK_BIT_NOT:
        return "~";
    case TOK_SHL:
        return "<<";
    case TOK_SHR:
        return ">>";
    case TOK_USHR:
        return ">>>";
    case TOK_TYPEOF:
        return "typeof ";
    default:
        return "";
    }
}

static bool script_unparse_expr(json_buf_t *jb, const ast_node_t *node, int depth);
static bool script_unparse_stmt(json_buf_t *jb, const ast_node_t *node, int depth);

static bool script_unparse_braced_stmt(json_buf_t *jb, const ast_node_t *stmt, int depth)
{
    if (!stmt) {
        return jb_cstr(jb, "{}");
    }
    if (stmt->type == AST_BLOCK) {
        return script_unparse_stmt(jb, stmt, depth);
    }
    return jb_ch(jb, '{') && script_unparse_stmt(jb, stmt, depth + 1) && jb_ch(jb, '}');
}

static bool script_unparse_var_decl(json_buf_t *jb, const ast_node_t *node, int depth,
                                    bool with_semi)
{
    bool ok = jb_cstr(jb, "var ");
    for (uint32_t i = 0u; ok && i < node->decl_count; ++i) {
        if (i > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        const char *nm = (node->decls && node->decls[i].name) ? node->decls[i].name : "_v";
        ok = ok && jb_cstr(jb, nm);
        if (ok && node->decls && node->decls[i].init) {
            ok = ok && jb_ch(jb, '=') && script_unparse_expr(jb, node->decls[i].init, depth + 1);
        }
    }
    return ok && (!with_semi || jb_ch(jb, ';'));
}

static bool script_unparse_func_decl(json_buf_t *jb, const ast_node_t *node, int depth)
{
    bool ok = true;
    if (node->is_exported) {
        ok = ok && jb_cstr(jb, "export ");
    }
    ok = ok && jb_cstr(jb, "function");
    if (ok && node->name && node->name[0] != '\0') {
        ok = ok && jb_ch(jb, ' ') && jb_cstr(jb, node->name);
    }
    ok = ok && jb_ch(jb, '(');
    for (uint32_t i = 0u; ok && i < node->param_count; ++i) {
        if (i > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        const char *pn = (node->params && node->params[i]) ? node->params[i] : "_p";
        ok = ok && jb_cstr(jb, pn);
    }
    ok = ok && jb_ch(jb, ')');
    return ok && script_unparse_braced_stmt(jb, node->body, depth + 1);
}

static bool script_unparse_expr(json_buf_t *jb, const ast_node_t *node, int depth)
{
    if (!node) {
        return jb_cstr(jb, "undefined");
    }
    if (depth > 128) {
        jb->err = WE_WRITER_ERR_LIMIT_EXCEEDED;
        return false;
    }

    switch (node->type) {
    case AST_LITERAL:
        switch (node->lit.type) {
        case VAL_UNDEF:
            return jb_cstr(jb, "undefined");
        case VAL_NULL:
            return jb_cstr(jb, "null");
        case VAL_BOOL:
            return jb_cstr(jb, node->lit.u.bval ? "true" : "false");
        case VAL_NUM: {
            double d = node->lit.u.num;
            if (isnan(d)) {
                return jb_cstr(jb, "NaN");
            }
            if (isinf(d)) {
                return jb_cstr(jb, signbit(d) ? "(-Infinity)" : "Infinity");
            }
            if (d == 0.0) {
                d = 0.0;
            }
            char nbuf[64];
            if (!we_json_format_double(d, nbuf, sizeof(nbuf))) {
                jb->err = WE_WRITER_ERR_OVERFLOW;
                return false;
            }
            return (d < 0.0) ? (jb_ch(jb, '(') && jb_cstr(jb, nbuf) && jb_ch(jb, ')'))
                             : jb_cstr(jb, nbuf);
        }
        case VAL_STR:
            return script_unparse_str_lit(jb, node->lit.u.str);
        default:
            return jb_cstr(jb, "undefined");
        }

    case AST_TEMPLATE: {
        bool ok = jb_ch(jb, '`');
        uint32_t ecnt = node->tpl.expr_count;
        for (uint32_t i = 0u; ok && i <= ecnt; ++i) {
            const char *seg = (node->tpl.parts && node->tpl.parts[i]) ? node->tpl.parts[i] : "";
            ok = ok && script_unparse_tpl_part(jb, seg);
            if (ok && i < ecnt) {
                ok = ok && jb_cstr(jb, "${") &&
                     script_unparse_expr(jb, node->tpl.exprs ? node->tpl.exprs[i] : NULL,
                                         depth + 1) &&
                     jb_ch(jb, '}');
            }
        }
        return ok && jb_ch(jb, '`');
    }

    case AST_IDENT:
        return jb_cstr(jb, node->name ? node->name : "undefined");

    case AST_THIS:
        return jb_cstr(jb, "this");

    case AST_FUNC_DECL:
        return script_unparse_func_decl(jb, node, depth + 1);

    case AST_ARRAY_LIT: {
        bool ok = jb_ch(jb, '[');
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            ok =
                ok && script_unparse_expr(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok && jb_ch(jb, ']');
    }

    case AST_OBJ_LIT: {
        bool ok = jb_ch(jb, '{');
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            const char *k = (node->keys && node->keys[i]) ? node->keys[i] : "";
            ok = ok && script_unparse_str_lit(jb, k) && jb_ch(jb, ':') &&
                 script_unparse_expr(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok && jb_ch(jb, '}');
    }

    case AST_UNARY: {
        const char *op = script_tok_op_str(node->op);
        return jb_ch(jb, '(') && jb_cstr(jb, op) && jb_ch(jb, '(') &&
               script_unparse_expr(jb, node->left, depth + 1) && jb_cstr(jb, "))");
    }

    case AST_UPDATE: {
        const char *op = script_tok_op_str(node->op);
        if (node->prefix) {
            return jb_ch(jb, '(') && jb_cstr(jb, op) &&
                   script_unparse_expr(jb, node->left, depth + 1) && jb_ch(jb, ')');
        }
        return jb_ch(jb, '(') && script_unparse_expr(jb, node->left, depth + 1) &&
               jb_cstr(jb, op) && jb_ch(jb, ')');
    }

    case AST_BINARY:
    case AST_ASSIGN: {
        const char *op = script_tok_op_str(node->op);
        return jb_ch(jb, '(') && script_unparse_expr(jb, node->left, depth + 1) &&
               jb_cstr(jb, op) && script_unparse_expr(jb, node->right, depth + 1) && jb_ch(jb, ')');
    }

    case AST_TERNARY:
        return jb_ch(jb, '(') && script_unparse_expr(jb, node->cond, depth + 1) && jb_ch(jb, '?') &&
               script_unparse_expr(jb, node->then_branch, depth + 1) && jb_ch(jb, ':') &&
               script_unparse_expr(jb, node->else_branch, depth + 1) && jb_ch(jb, ')');

    case AST_MEMBER: {
        bool wrap = !node->left || (node->left->type != AST_IDENT && node->left->type != AST_THIS &&
                                    node->left->type != AST_MEMBER &&
                                    node->left->type != AST_INDEX && node->left->type != AST_CALL);
        bool ok = true;
        if (wrap) {
            ok = ok && jb_ch(jb, '(');
        }
        ok = ok && script_unparse_expr(jb, node->left, depth + 1);
        if (wrap) {
            ok = ok && jb_ch(jb, ')');
        }
        return ok && jb_ch(jb, '.') && jb_cstr(jb, node->name ? node->name : "");
    }

    case AST_INDEX: {
        bool wrap =
            !node->left || (node->left->type != AST_IDENT && node->left->type != AST_THIS &&
                            node->left->type != AST_MEMBER && node->left->type != AST_INDEX &&
                            node->left->type != AST_CALL && node->left->type != AST_ARRAY_LIT);
        bool ok = true;
        if (wrap) {
            ok = ok && jb_ch(jb, '(');
        }
        ok = ok && script_unparse_expr(jb, node->left, depth + 1);
        if (wrap) {
            ok = ok && jb_ch(jb, ')');
        }
        return ok && jb_ch(jb, '[') && script_unparse_expr(jb, node->right, depth + 1) &&
               jb_ch(jb, ']');
    }

    case AST_CALL: {
        bool wrap = node->left && node->left->type == AST_FUNC_DECL;
        bool ok = true;
        if (wrap) {
            ok = ok && jb_ch(jb, '(');
        }
        ok = ok && script_unparse_expr(jb, node->left, depth + 1);
        if (wrap) {
            ok = ok && jb_ch(jb, ')');
        }
        ok = ok && jb_ch(jb, '(');
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            ok =
                ok && script_unparse_expr(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok && jb_ch(jb, ')');
    }

    case AST_NEW: {
        bool ok = jb_cstr(jb, "(new ") && jb_cstr(jb, node->name ? node->name : "Object") &&
                  jb_ch(jb, '(');
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            ok =
                ok && script_unparse_expr(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok && jb_cstr(jb, "))");
    }

    default:
        return jb_cstr(jb, "undefined");
    }
}

static bool script_unparse_stmt(json_buf_t *jb, const ast_node_t *node, int depth)
{
    if (!node) {
        return true;
    }
    if (depth > 128) {
        jb->err = WE_WRITER_ERR_LIMIT_EXCEEDED;
        return false;
    }

    switch (node->type) {
    case AST_PROGRAM: {
        bool ok = true;
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, '\n');
            }
            ok =
                ok && script_unparse_stmt(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok;
    }

    case AST_BLOCK: {
        bool ok = jb_ch(jb, '{');
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            ok =
                ok && script_unparse_stmt(jb, node->children ? node->children[i] : NULL, depth + 1);
        }
        return ok && jb_ch(jb, '}');
    }

    case AST_VAR_DECL:
        return script_unparse_var_decl(jb, node, depth, true);

    case AST_FUNC_DECL:
        return script_unparse_func_decl(jb, node, depth);

    case AST_IF: {
        bool ok = jb_cstr(jb, "if(") && script_unparse_expr(jb, node->cond, depth + 1) &&
                  jb_ch(jb, ')') && script_unparse_braced_stmt(jb, node->then_branch, depth + 1);
        if (ok && node->else_branch) {
            ok = ok && jb_cstr(jb, "else ") &&
                 script_unparse_braced_stmt(jb, node->else_branch, depth + 1);
        }
        return ok;
    }

    case AST_WHILE:
        return jb_cstr(jb, "while(") && script_unparse_expr(jb, node->cond, depth + 1) &&
               jb_ch(jb, ')') && script_unparse_braced_stmt(jb, node->body, depth + 1);

    case AST_DO_WHILE:
        return jb_cstr(jb, "do ") && script_unparse_braced_stmt(jb, node->body, depth + 1) &&
               jb_cstr(jb, "while(") && script_unparse_expr(jb, node->cond, depth + 1) &&
               jb_cstr(jb, ");");

    case AST_FOR: {
        bool ok = jb_cstr(jb, "for(");
        if (ok && node->init) {
            if (node->init->type == AST_VAR_DECL) {
                ok = script_unparse_var_decl(jb, node->init, depth + 1, false);
            } else {
                ok = script_unparse_expr(jb, node->init, depth + 1);
            }
        }
        ok = ok && jb_ch(jb, ';');
        if (ok && node->cond) {
            ok = script_unparse_expr(jb, node->cond, depth + 1);
        }
        ok = ok && jb_ch(jb, ';');
        if (ok && node->step) {
            ok = script_unparse_expr(jb, node->step, depth + 1);
        }
        return ok && jb_ch(jb, ')') && script_unparse_braced_stmt(jb, node->body, depth + 1);
    }

    case AST_SWITCH: {
        bool ok = jb_cstr(jb, "switch(") && script_unparse_expr(jb, node->cond, depth + 1) &&
                  jb_cstr(jb, "){");
        for (uint32_t i = 0u; ok && i < node->child_count; ++i) {
            const ast_node_t *ce = node->case_exprs ? node->case_exprs[i] : NULL;
            const ast_node_t *cb = node->children ? node->children[i] : NULL;
            if (ce) {
                ok = ok && jb_cstr(jb, "case ") && script_unparse_expr(jb, ce, depth + 1) &&
                     jb_ch(jb, ':');
            } else {
                ok = ok && jb_cstr(jb, "default:");
            }
            if (ok && cb) {
                if (cb->type == AST_BLOCK) {
                    for (uint32_t s = 0u; ok && s < cb->child_count; ++s) {
                        ok = ok && script_unparse_stmt(jb, cb->children ? cb->children[s] : NULL,
                                                       depth + 1);
                    }
                } else {
                    ok = ok && script_unparse_stmt(jb, cb, depth + 1);
                }
            }
        }
        return ok && jb_ch(jb, '}');
    }

    case AST_TRY: {
        bool ok = jb_cstr(jb, "try") && script_unparse_braced_stmt(jb, node->body, depth + 1);
        if (ok && node->else_branch) {
            ok = ok && jb_cstr(jb, "catch");
            if (ok && node->name && node->name[0] != '\0') {
                ok = ok && jb_ch(jb, '(') && jb_cstr(jb, node->name) && jb_ch(jb, ')');
            }
            ok = ok && script_unparse_braced_stmt(jb, node->else_branch, depth + 1);
        }
        if (ok && node->step) {
            ok = ok && jb_cstr(jb, "finally") &&
                 script_unparse_braced_stmt(jb, node->step, depth + 1);
        }
        return ok;
    }

    case AST_THROW:
        return jb_cstr(jb, "throw ") && script_unparse_expr(jb, node->left, depth + 1) &&
               jb_ch(jb, ';');

    case AST_RETURN:
        if (!node->left) {
            return jb_cstr(jb, "return;");
        }
        return jb_cstr(jb, "return ") && script_unparse_expr(jb, node->left, depth + 1) &&
               jb_ch(jb, ';');

    case AST_BREAK:
        return jb_cstr(jb, "break;");

    case AST_CONTINUE:
        return jb_cstr(jb, "continue;");

    case AST_EXPR_STMT: {
        const ast_node_t *expr = node->left;
        bool wrap = expr && (expr->type == AST_OBJ_LIT || expr->type == AST_FUNC_DECL);
        bool ok = true;
        if (wrap) {
            ok = ok && jb_ch(jb, '(');
        }
        ok = ok && script_unparse_expr(jb, expr, depth + 1);
        if (wrap) {
            ok = ok && jb_ch(jb, ')');
        }
        return ok && jb_ch(jb, ';');
    }

    default:
        return script_unparse_expr(jb, node, depth + 1) && jb_ch(jb, ';');
    }
}

static bool script_prop_matches_project(const we_scene_t *scene, const we_script_prop_item_t *sp)
{
    if (!scene || !sp) {
        return false;
    }
    const we_user_prop_t *up = we_project_find_prop(&scene->project, sp->name);
    if (!up) {
        return false;
    }
    if (up->num_components == 0u || up->type == WE_PROP_TEXT) {
        return sp->num_components == 0u && strcmp(up->str_val, sp->str_val) == 0;
    }
    if (up->num_components != sp->num_components) {
        return false;
    }
    uint32_t nc = (sp->num_components <= 4u) ? sp->num_components : 4u;
    for (uint32_t c = 0u; c < nc; ++c) {
        if (fabsf(up->val[c] - sp->val[c]) > 1e-6f) {
            return false;
        }
    }
    return true;
}

static bool jb_script_object_fields(json_buf_t *jb, const we_scene_t *scene, const we_script_t *vm,
                                    const char *raw_src)
{
    json_buf_t sbuf;
    const char *src_to_emit = raw_src;
    if (!src_to_emit || src_to_emit[0] == '\0') {
        if (!vm || !vm->root_ast) {
            return false;
        }
        jb_init(&sbuf);
        if (!script_unparse_stmt(&sbuf, vm->root_ast, 0) || !sbuf.data) {
            jb->err = sbuf.err != WE_WRITER_OK ? sbuf.err : WE_WRITER_ERR_INVALID_ARG;
            jb_destroy(&sbuf);
            return false;
        }
        src_to_emit = sbuf.data;
    } else {
        memset(&sbuf, 0, sizeof(sbuf));
    }

    bool ok = jb_cstr(jb, "\"script\":") && jb_str(jb, src_to_emit);
    jb_destroy(&sbuf);
    if (!ok || !vm || vm->user_prop_count == 0u) {
        return ok;
    }

    uint32_t limit =
        (vm->user_prop_count < WE_SCRIPT_MAX_PROPS) ? vm->user_prop_count : WE_SCRIPT_MAX_PROPS;
    bool has_sp = false;
    for (uint32_t i = 0u; i < limit; ++i) {
        const we_script_prop_item_t *sp = &vm->user_props[i];
        if (sp->name[0] != '\0' && !script_prop_matches_project(scene, sp)) {
            has_sp = true;
            break;
        }
    }
    if (!has_sp) {
        return true;
    }

    ok = ok && jb_cstr(jb, ",\"scriptproperties\":{");
    bool sp_first = true;
    for (uint32_t i = 0u; ok && i < limit; ++i) {
        const we_script_prop_item_t *sp = &vm->user_props[i];
        if (sp->name[0] == '\0' || script_prop_matches_project(scene, sp)) {
            continue;
        }
        /* Skip duplicate property keys to keep canonicalization well-formed. */
        bool dup = false;
        for (uint32_t j = 0u; j < i; ++j) {
            if (strcmp(vm->user_props[j].name, sp->name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        ok = ok && jb_key(jb, &sp_first, sp->name);
        if (sp->num_components == 0u) {
            ok = ok && jb_str(jb, sp->str_val);
        } else if (sp->num_components == 1u) {
            ok = ok && jb_f32(jb, sp->val[0]);
        } else {
            uint32_t nc = (sp->num_components <= 4u) ? sp->num_components : 4u;
            ok = ok && jb_vec_str(jb, sp->val, nc);
        }
    }
    return ok && jb_ch(jb, '}');
}

static const we_script_t *find_obj_script(const we_scene_object_t *obj,
                                          we_script_prop_target_t target, const char *uniform_name)
{
    if (!obj || obj->script_count == 0u) {
        return NULL;
    }
    uint32_t cnt =
        (obj->script_count < WE_MAX_OBJECT_SCRIPTS) ? obj->script_count : WE_MAX_OBJECT_SCRIPTS;
    for (uint32_t s = 0u; s < cnt; ++s) {
        const we_obj_script_t *sc = &obj->scripts[s];
        if (sc->target != target || !sc->vm) {
            continue;
        }
        if (target == WE_SCRIPT_PROP_UNIFORM) {
            if (!uniform_name || strcmp(sc->uniform_name, uniform_name) != 0) {
                continue;
            }
        }
        return sc->vm;
    }
    return NULL;
}

static bool jb_emit_script_prop_wrap_open(json_buf_t *jb, const we_scene_t *scene,
                                          const we_script_t *vm, const char *raw_src,
                                          const char *user_prop)
{
    bool ok = jb_ch(jb, '{') && jb_script_object_fields(jb, scene, vm, raw_src);
    if (ok && user_prop && user_prop[0] != '\0') {
        ok = jb_cstr(jb, ",\"user\":") && jb_user_value(jb, user_prop);
    }
    return ok && jb_cstr(jb, ",\"value\":");
}

static bool jb_bound_f32_opt_script(json_buf_t *jb, const we_scene_t *scene, float val,
                                    const char *user_prop, const we_script_t *vm,
                                    const char *raw_src)
{
    if ((!vm || !vm->root_ast) && (!raw_src || raw_src[0] == '\0')) {
        return jb_bound_f32(jb, val, user_prop);
    }
    return jb_emit_script_prop_wrap_open(jb, scene, vm, raw_src, user_prop) && jb_f32(jb, val) &&
           jb_ch(jb, '}');
}

static bool jb_bound_bool_opt_script(json_buf_t *jb, const we_scene_t *scene, bool val,
                                     const char *user_prop, const we_script_t *vm)
{
    if (!vm || !vm->root_ast) {
        return jb_bound_bool(jb, val, user_prop);
    }
    return jb_emit_script_prop_wrap_open(jb, scene, vm, NULL, user_prop) &&
           jb_cstr(jb, val ? "true" : "false") && jb_ch(jb, '}');
}

static bool jb_bound_vec_opt_script(json_buf_t *jb, const we_scene_t *scene, const float *vals,
                                    size_t n, const char *user_prop, const we_script_t *vm,
                                    const char *raw_src)
{
    if ((!vm || !vm->root_ast) && (!raw_src || raw_src[0] == '\0')) {
        return jb_bound_vec(jb, vals, n, user_prop);
    }
    return jb_emit_script_prop_wrap_open(jb, scene, vm, raw_src, user_prop) &&
           jb_vec_str(jb, vals, n) && jb_ch(jb, '}');
}

static bool jb_bound_str_opt_script(json_buf_t *jb, const we_scene_t *scene, const char *val,
                                    const char *user_prop, const we_script_t *vm)
{
    if (!vm || !vm->root_ast) {
        return jb_bound_str(jb, val, user_prop);
    }
    return jb_emit_script_prop_wrap_open(jb, scene, vm, NULL, user_prop) && jb_str(jb, val) &&
           jb_ch(jb, '}');
}

static bool jb_bound_i64_opt_script(json_buf_t *jb, const we_scene_t *scene, long long val,
                                    const char *user_prop, const we_script_t *vm)
{
    if (!vm || !vm->root_ast) {
        return jb_bound_i64(jb, val, user_prop);
    }
    return jb_emit_script_prop_wrap_open(jb, scene, vm, NULL, user_prop) && jb_i64(jb, val) &&
           jb_ch(jb, '}');
}

static bool serialize_pass_body(json_buf_t *jb, const we_scene_t *scene,
                                const we_scene_object_t *script_obj, const we_material_pass_t *pass,
                                bool include_routing, const char *material_rel_path,
                                const export_ctx_t *ectx)
{
    bool ok = jb_ch(jb, '{');
    bool first = true;

    if (include_routing) {
        if (pass->command[0] != '\0') {
            ok = ok && jb_key(jb, &first, "command") && jb_str(jb, pass->command);
        }
        if (pass->target_fbo[0] != '\0') {
            ok = ok && jb_key(jb, &first, "target") && jb_str(jb, pass->target_fbo);
        }
        if (material_rel_path && material_rel_path[0] != '\0') {
            ok = ok && jb_key(jb, &first, "material") && jb_str(jb, material_rel_path);
        }
        if (pass->bind_count > 0u) {
            ok = ok && jb_key(jb, &first, "bind") && jb_ch(jb, '[');
            for (uint32_t b = 0u; ok && b < pass->bind_count && b < WE_MAX_PASS_TEXTURES; ++b) {
                if (b > 0u) {
                    ok = ok && jb_ch(jb, ',');
                }
                ok = ok && jb_cstr(jb, "{\"index\":") && jb_i64(jb, pass->binds[b].slot) &&
                     jb_cstr(jb, ",\"name\":") && jb_str(jb, pass->binds[b].target_name) &&
                     jb_ch(jb, '}');
            }
            ok = ok && jb_ch(jb, ']');
        }
    }

    const char *sh_out = export_ctx_shader_name(ectx, pass);
    if (sh_out && sh_out[0] != '\0') {
        ok = ok && jb_key(jb, &first, "shader") && jb_str(jb, sh_out);
    }
    ok = ok && jb_key(jb, &first, "blending") && jb_str(jb, blend_mode_name(pass->blending));
    ok = ok && jb_key(jb, &first, "depthtest") &&
         jb_str(jb, pass->depthtest_enabled ? "enabled" : "disabled");
    ok = ok && jb_key(jb, &first, "depthwrite") &&
         jb_str(jb, pass->depthwrite_enabled ? "enabled" : "disabled");
    ok = ok && jb_key(jb, &first, "cullmode") &&
         jb_str(jb, pass->cull_enabled ? "normal" : "nocull");

    int last_tex = -1;
    for (int s = (int)WE_MAX_PASS_TEXTURES - 1; s >= 0; --s) {
        if (pass->tex_paths[s][0] != '\0') {
            last_tex = s;
            break;
        }
    }
    if (last_tex >= 0) {
        ok = ok && jb_key(jb, &first, "textures") && jb_ch(jb, '[');
        for (int s = 0; ok && s <= last_tex; ++s) {
            if (s > 0) {
                ok = ok && jb_ch(jb, ',');
            }
            if (pass->tex_paths[s][0] != '\0') {
                ok = ok && jb_str(jb, pass->tex_paths[s]);
            } else {
                ok = ok && jb_cstr(jb, "null");
            }
        }
        ok = ok && jb_ch(jb, ']');
    }

    if (pass->combo_count > 0u) {
        ok = ok && jb_key(jb, &first, "combos") && jb_ch(jb, '{');
        bool cf = true;
        for (uint32_t c = 0u; ok && c < pass->combo_count && c < WE_SHADER_MAX_COMBOS; ++c) {
            if (pass->combos[c].name[0] == '\0') {
                continue;
            }
            ok = ok && jb_key(jb, &cf, pass->combos[c].name) && jb_i64(jb, pass->combos[c].value);
        }
        ok = ok && jb_ch(jb, '}');
    }

    bool has_extra_uniform_script = false;
    if (script_obj && script_obj->script_count > 0u) {
        uint32_t scnt = (script_obj->script_count < WE_MAX_OBJECT_SCRIPTS)
                            ? script_obj->script_count
                            : WE_MAX_OBJECT_SCRIPTS;
        for (uint32_t s = 0u; s < scnt; ++s) {
            const we_obj_script_t *sc = &script_obj->scripts[s];
            if (sc->target == WE_SCRIPT_PROP_UNIFORM && sc->uniform_name[0] != '\0' && sc->vm &&
                sc->vm->root_ast) {
                bool found = false;
                for (uint32_t c = 0u; c < pass->const_val_count && c < WE_MAX_CONST_VALS; ++c) {
                    if (strcmp(pass->const_vals[c].name, sc->uniform_name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    has_extra_uniform_script = true;
                    break;
                }
            }
        }
    }

    if (pass->const_val_count > 0u || has_extra_uniform_script) {
        ok = ok && jb_key(jb, &first, "constantshadervalues") && jb_ch(jb, '{');
        bool cvf = true;
        for (uint32_t c = 0u; ok && c < pass->const_val_count && c < WE_MAX_CONST_VALS; ++c) {
            const we_const_val_t *cv = &pass->const_vals[c];
            if (cv->name[0] == '\0') {
                continue;
            }
            uint32_t nc =
                (cv->num_components >= 1u && cv->num_components <= 4u) ? cv->num_components : 1u;
            const we_script_t *uvm = find_obj_script(script_obj, WE_SCRIPT_PROP_UNIFORM, cv->name);
            const char *raw_sc =
                (cv->script_src && cv->script_src[0] != '\0') ? cv->script_src : NULL;
            ok = ok && jb_key(jb, &cvf, cv->name);
            if (nc == 1u) {
                ok = ok &&
                     jb_bound_f32_opt_script(jb, scene, cv->val[0], cv->user_prop, uvm, raw_sc);
            } else {
                ok = ok &&
                     jb_bound_vec_opt_script(jb, scene, cv->val, nc, cv->user_prop, uvm, raw_sc);
            }
        }
        if (ok && has_extra_uniform_script) {
            uint32_t scnt = (script_obj->script_count < WE_MAX_OBJECT_SCRIPTS)
                                ? script_obj->script_count
                                : WE_MAX_OBJECT_SCRIPTS;
            for (uint32_t s = 0u; ok && s < scnt; ++s) {
                const we_obj_script_t *sc = &script_obj->scripts[s];
                if (sc->target != WE_SCRIPT_PROP_UNIFORM || sc->uniform_name[0] == '\0' ||
                    !sc->vm || !sc->vm->root_ast) {
                    continue;
                }
                bool seen = false;
                for (uint32_t c = 0u; c < pass->const_val_count && c < WE_MAX_CONST_VALS; ++c) {
                    if (strcmp(pass->const_vals[c].name, sc->uniform_name) == 0) {
                        seen = true;
                        break;
                    }
                }
                for (uint32_t prev = 0u; !seen && prev < s; ++prev) {
                    if (script_obj->scripts[prev].target == WE_SCRIPT_PROP_UNIFORM &&
                        strcmp(script_obj->scripts[prev].uniform_name, sc->uniform_name) == 0) {
                        seen = true;
                    }
                }
                if (seen) {
                    continue;
                }
                ok = ok && jb_key(jb, &cvf, sc->uniform_name) &&
                     jb_bound_f32_opt_script(jb, scene, 0.0f, NULL, sc->vm, NULL);
            }
        }
        ok = ok && jb_ch(jb, '}');
    }

    return ok && jb_ch(jb, '}');
}

static bool build_sound_path(int32_t obj_id, uint32_t obj_idx, uint32_t trk_idx, char *out,
                             size_t cap)
{
    int n = snprintf(out, cap, "sounds/sound_%d_%u_%u.wav", obj_id, (unsigned int)obj_idx,
                     (unsigned int)trk_idx);
    return n >= 0 && (size_t)n < cap;
}

static bool serialize_object_text(json_buf_t *jb, const we_scene_t *scene,
                                  const we_scene_object_t *obj, bool *first)
{
    const we_script_t *txt_vm = find_obj_script(obj, WE_SCRIPT_PROP_TEXT, NULL);
    const we_script_t *ps_vm = find_obj_script(obj, WE_SCRIPT_PROP_POINTSIZE, NULL);
    bool ok = jb_key(jb, first, "text") &&
              jb_bound_str_opt_script(jb, scene, obj->text_str, obj->user_prop_text, txt_vm);
    if (obj->text_font[0] != '\0') {
        ok = ok && jb_key(jb, first, "font") && jb_str(jb, obj->text_font);
    }
    ok = ok && jb_key(jb, first, "pointsize") &&
         jb_bound_i64_opt_script(jb, scene, obj->text_pointsize, obj->user_prop_pointsize, ps_vm);
    if (obj->text_halign[0] != '\0') {
        ok = ok && jb_key(jb, first, "horizontalalign") && jb_str(jb, obj->text_halign);
    }
    if (obj->text_valign[0] != '\0') {
        ok = ok && jb_key(jb, first, "verticalalign") && jb_str(jb, obj->text_valign);
    }
    ok = ok && jb_key(jb, first, "padding") && jb_i64(jb, obj->text_padding);
    ok = ok && jb_key(jb, first, "opaquebackground") &&
         jb_cstr(jb, obj->text_opaque_bg ? "true" : "false");
    ok = ok && jb_key(jb, first, "backgroundcolor") && jb_vec_str(jb, obj->text_bg_color, 3u);
    return ok;
}

static bool serialize_object_light(json_buf_t *jb, const we_scene_object_t *obj, bool *first)
{
    bool ok = jb_key(jb, first, "type") && jb_str(jb, "light");
    ok = ok && jb_key(jb, first, "light") && jb_ch(jb, '{');
    bool lf = true;
    ok = ok && jb_key(jb, &lf, "lighttype") && jb_str(jb, light_type_name(obj->light_type));
    ok = ok && jb_key(jb, &lf, "color") && jb_bound_vec(jb, obj->color, 3u, obj->color_user_prop);
    ok = ok && jb_key(jb, &lf, "intensity") && jb_f32(jb, obj->light_intensity);
    ok = ok && jb_key(jb, &lf, "radius") && jb_f32(jb, obj->light_radius);
    ok = ok && jb_key(jb, &lf, "coneangle") && jb_f32(jb, obj->light_cone_angle);
    ok = ok && jb_key(jb, &lf, "innerconeangle") && jb_f32(jb, obj->light_inner_cone_angle);
    ok = ok && jb_key(jb, &lf, "specularpower") && jb_f32(jb, obj->light_specular_power);
    ok = ok && jb_key(jb, &lf, "direction") && jb_vec_str(jb, obj->light_direction, 3u);
    ok = ok && jb_key(jb, &lf, "tubelength") && jb_f32(jb, obj->light_tube_length);
    return ok && jb_ch(jb, '}');
}

static bool serialize_object_sound(json_buf_t *jb, const we_scene_t *scene,
                                   const we_scene_object_t *obj, uint32_t obj_idx, bool *first)
{
    const we_script_t *vol_vm = find_obj_script(obj, WE_SCRIPT_PROP_VOLUME, NULL);
    bool ok = jb_key(jb, first, "playbackmode") && jb_str(jb, sound_mode_name(obj->sound_mode));
    ok = ok && jb_key(jb, first, "volume") &&
         jb_bound_f32_opt_script(jb, scene, obj->sound_volume, obj->user_prop_volume, vol_vm, NULL);
    ok = ok && jb_key(jb, first, "startsilent") &&
         jb_cstr(jb, obj->sound_startsilent ? "true" : "false");
    ok = ok && jb_key(jb, first, "mintime") && jb_f32(jb, obj->sound_mintime);
    ok = ok && jb_key(jb, first, "maxtime") && jb_f32(jb, obj->sound_maxtime);
    ok = ok && jb_key(jb, first, "sound") && jb_ch(jb, '[');
    /* Parser requires >= 1 sound entry to preserve is_sound_object on reload. */
    uint32_t tc = obj->sound_track_count;
    if (tc == 0u) {
        tc = 1u;
    } else if (tc > WE_MAX_SOUND_TRACKS) {
        tc = WE_MAX_SOUND_TRACKS;
    }
    for (uint32_t t = 0u; ok && t < tc; ++t) {
        if (t > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        char spath[128];
        if (!build_sound_path(obj->id, obj_idx, t, spath, sizeof(spath))) {
            jb->err = WE_WRITER_ERR_LIMIT_EXCEEDED;
            return false;
        }
        ok = ok && jb_str(jb, spath);
    }
    return ok && jb_ch(jb, ']');
}

static bool serialize_object_effects(json_buf_t *jb, const we_scene_t *scene,
                                     const we_scene_object_t *obj, uint32_t obj_idx,
                                     const export_ctx_t *ectx, bool *first)
{
    if (!obj->effects) {
        jb->err = WE_WRITER_ERR_INVALID_ARG;
        return false;
    }
    bool ok = jb_key(jb, first, "effects") && jb_ch(jb, '[');
    for (uint32_t e = 0u; ok && e < obj->effect_count; ++e) {
        const we_effect_t *eff = &obj->effects[e];
        if (e > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        char efile_buf[128];
        const char *efile = eff->file_path;
        if (!efile || efile[0] == '\0') {
            int en = snprintf(efile_buf, sizeof(efile_buf), "effects/effect_%u_%u.json",
                              (unsigned int)obj_idx, (unsigned int)e);
            if (en < 0 || (size_t)en >= sizeof(efile_buf)) {
                jb->err = WE_WRITER_ERR_LIMIT_EXCEEDED;
                return false;
            }
            efile = efile_buf;
        }
        ok = ok && jb_ch(jb, '{');
        bool ef = true;
        ok = ok && jb_key(jb, &ef, "file") && jb_str(jb, efile);
        ok = ok && jb_key(jb, &ef, "name") && jb_str(jb, eff->name[0] ? eff->name : efile);
        ok = ok && jb_key(jb, &ef, "visible") &&
             jb_bound_bool(jb, eff->visible, eff->visible_user_prop);
        if (eff->pass_count > 0u) {
            if (!eff->passes) {
                jb->err = WE_WRITER_ERR_INVALID_ARG;
                return false;
            }
            ok = ok && jb_key(jb, &ef, "passes") && jb_ch(jb, '[');
            for (uint32_t p = 0u; ok && p < eff->pass_count; ++p) {
                if (p > 0u) {
                    ok = ok && jb_ch(jb, ',');
                }
                ok = ok && serialize_pass_body(jb, scene, NULL, &eff->passes[p], false, NULL, ectx);
            }
            ok = ok && jb_ch(jb, ']');
        }
        ok = ok && jb_ch(jb, '}');
    }
    return ok && jb_ch(jb, ']');
}

static void sanitize_puppet_stem(const we_scene_t *scene, const we_scene_object_t *obj,
                                 uint32_t obj_idx, char *out_stem, size_t stem_cap)
{
    char clean[48];
    size_t out_len = 0u;
    const char *src = (obj->name[0] != '\0') ? obj->name : "puppet";
    for (size_t i = 0u; src[i] != '\0' && out_len + 1u < sizeof(clean); ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch)) {
            clean[out_len++] = (char)tolower(ch);
        } else if (out_len > 0u && clean[out_len - 1u] != '_') {
            clean[out_len++] = '_';
        }
    }
    while (out_len > 0u && clean[out_len - 1u] == '_') {
        --out_len;
    }
    if (out_len == 0u) {
        snprintf(clean, sizeof(clean), "puppet");
    } else {
        clean[out_len] = '\0';
    }

    bool unique_id = (obj->id > 0);
    if (unique_id && scene && scene->objects) {
        for (uint32_t j = 0u; j < scene->object_count; ++j) {
            if (j != obj_idx && scene->objects[j].id == obj->id) {
                unique_id = false;
                break;
            }
        }
    }

    if (unique_id) {
        snprintf(out_stem, stem_cap, "%s_%d", clean, obj->id);
    } else {
        snprintf(out_stem, stem_cap, "%s_%u", clean, (unsigned int)obj_idx);
    }
}

static bool build_puppet_mdl_rel(const we_scene_t *scene, const we_scene_object_t *obj,
                                 uint32_t obj_idx, char *out_rel, size_t cap)
{
    char stem[80];
    sanitize_puppet_stem(scene, obj, obj_idx, stem, sizeof(stem));
    char raw[128];
    int n = snprintf(raw, sizeof(raw), "models/%s.mdl", stem);
    if (n < 0 || (size_t)n >= sizeof(raw)) {
        return false;
    }
    return we_normalize_rel_path(raw, out_rel, cap, NULL);
}

static bool has_json_ext(const char *path)
{
    size_t len = path ? strlen(path) : 0u;
    return len >= 5u && strcasecmp(path + len - 5u, ".json") == 0;
}

static bool build_particle_rel(const we_scene_t *scene, const we_scene_object_t *obj,
                               uint32_t obj_idx, char *out_rel, size_t cap)
{
    if (obj->image_path[0] != '\0' && has_json_ext(obj->image_path)) {
        char norm_img[320];
        if (we_normalize_rel_path(obj->image_path, norm_img, sizeof(norm_img), NULL) &&
            strncasecmp(norm_img, "models/", 7u) != 0) {
            if (strncasecmp(norm_img, "particles/", 10u) == 0) {
                return snprintf(out_rel, cap, "%s", norm_img) < (int)cap;
            }
            char prefixed[320];
            int pn = snprintf(prefixed, sizeof(prefixed), "particles/%s", norm_img);
            if (pn >= 0 && (size_t)pn < sizeof(prefixed)) {
                return we_normalize_rel_path(prefixed, out_rel, cap, NULL);
            }
        }
    }

    bool unique_id = (obj->id >= 0);
    if (unique_id && scene && scene->objects) {
        for (uint32_t j = 0u; j < scene->object_count; ++j) {
            if (j != obj_idx && scene->objects[j].id == obj->id) {
                unique_id = false;
                break;
            }
        }
    }
    char raw[128];
    int n = unique_id
                ? snprintf(raw, sizeof(raw), "particles/particle_%d.json", obj->id)
                : snprintf(raw, sizeof(raw), "particles/particle_%u.json", (unsigned int)obj_idx);
    if (n < 0 || (size_t)n >= sizeof(raw)) {
        return false;
    }
    return we_normalize_rel_path(raw, out_rel, cap, NULL);
}

static bool serialize_puppet_layers(json_buf_t *jb, const we_puppet_t *puppet, bool *first)
{
    if (!puppet || puppet->layer_count == 0u) {
        return true;
    }
    bool ok = jb_key(jb, first, "puppet_layers") && jb_ch(jb, '[');
    uint32_t count =
        (puppet->layer_count < WE_MDL_MAX_LAYERS) ? puppet->layer_count : WE_MDL_MAX_LAYERS;
    for (uint32_t i = 0u; ok && i < count; ++i) {
        const we_puppet_layer_t *ly = &puppet->layers[i];
        bool lfirst = true;
        if (i > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        ok = ok && jb_ch(jb, '{');
        if (ly->anim_id >= 0) {
            ok = ok && jb_key(jb, &lfirst, "animation") && jb_i64(jb, ly->anim_id);
            if (ok && ly->anim_name[0] != '\0') {
                ok = jb_key(jb, &lfirst, "name") && jb_str(jb, ly->anim_name);
            }
        } else {
            ok = ok && jb_key(jb, &lfirst, "animation") && jb_str(jb, ly->anim_name);
            if (ok && ly->anim_name[0] != '\0') {
                ok = jb_key(jb, &lfirst, "name") && jb_str(jb, ly->anim_name);
            }
        }
        ok = ok && jb_key(jb, &lfirst, "rate") && jb_f32(jb, ly->rate);
        ok = ok && jb_key(jb, &lfirst, "blend") && jb_f32(jb, ly->blend);
        ok = ok && jb_key(jb, &lfirst, "visible") && jb_cstr(jb, ly->visible ? "true" : "false");
        ok = ok && jb_ch(jb, '}');
    }
    return ok && jb_ch(jb, ']');
}

static bool serialize_object(json_buf_t *jb, const we_scene_t *scene, const we_scene_object_t *obj,
                             uint32_t obj_idx, const char *image_override,
                             const char *puppet_override, const char *particle_override,
                             const export_ctx_t *ectx)
{
    bool ok = jb_ch(jb, '{');
    bool first = true;
    const char *img_out = image_override ? image_override : obj->image_path;
    char auto_puppet_rel[160];
    const char *puppet_out = puppet_override;
    char auto_particle_rel[320];
    const char *particle_out = particle_override;

    if (!puppet_out && obj->puppet && !obj->is_text_object && !obj->passthrough) {
        if (!build_puppet_mdl_rel(scene, obj, obj_idx, auto_puppet_rel, sizeof(auto_puppet_rel))) {
            jb->err = WE_WRITER_ERR_INVALID_PATH;
            return false;
        }
        puppet_out = auto_puppet_rel;
    } else if (!puppet_out && !particle_out && obj->particle_sys && !obj->is_text_object &&
               !obj->passthrough) {
        if (!build_particle_rel(scene, obj, obj_idx, auto_particle_rel,
                                sizeof(auto_particle_rel))) {
            jb->err = WE_WRITER_ERR_INVALID_PATH;
            return false;
        }
        particle_out = auto_particle_rel;
    }

    ok = ok && jb_key(jb, &first, "id") && jb_i64(jb, obj->id);
    if (obj->parent_id >= 0) {
        ok = ok && jb_key(jb, &first, "parent") && jb_i64(jb, obj->parent_id);
    }
    ok = ok && jb_key(jb, &first, "name") && jb_str(jb, obj->name[0] ? obj->name : "layer");
    if (puppet_out && puppet_out[0] != '\0') {
        /* Omit "image" on puppet layers so scene parser enters direct puppet loading. */
        ok = ok && jb_key(jb, &first, "puppet") && jb_str(jb, puppet_out);
        if (ok && obj->puppet && obj->puppet->layer_count > 0u) {
            ok = serialize_puppet_layers(jb, obj->puppet, &first);
        }
    } else if (particle_out && particle_out[0] != '\0') {
        /* Emit "particle" directly and omit "image" so reload never writes model wrappers. */
        ok = ok && jb_key(jb, &first, "particle") && jb_str(jb, particle_out);
    } else if (img_out[0] != '\0') {
        ok = ok && jb_key(jb, &first, "image") && jb_str(jb, img_out);
    }

    const we_script_t *orig_vm = find_obj_script(obj, WE_SCRIPT_PROP_ORIGIN, NULL);
    const we_script_t *scale_vm = find_obj_script(obj, WE_SCRIPT_PROP_SCALE, NULL);
    const we_script_t *ang_vm = find_obj_script(obj, WE_SCRIPT_PROP_ANGLES, NULL);
    const we_script_t *alpha_vm = find_obj_script(obj, WE_SCRIPT_PROP_ALPHA, NULL);
    const we_script_t *col_vm = find_obj_script(obj, WE_SCRIPT_PROP_COLOR, NULL);
    const we_script_t *vis_vm = find_obj_script(obj, WE_SCRIPT_PROP_VISIBLE, NULL);

    ok = ok && jb_key(jb, &first, "origin") &&
         jb_bound_vec_opt_script(jb, scene, obj->origin, 3u, NULL, orig_vm, NULL);
    ok = ok && jb_key(jb, &first, "scale") &&
         jb_bound_vec_opt_script(jb, scene, obj->scale, 3u, NULL, scale_vm, NULL);
    ok = ok && jb_key(jb, &first, "angles") &&
         jb_bound_vec_opt_script(jb, scene, obj->angles, 3u, NULL, ang_vm, NULL);
    ok = ok && jb_key(jb, &first, "size") && jb_vec_str(jb, obj->size, 2u);
    ok = ok && jb_key(jb, &first, "parallaxDepth") && jb_vec_str(jb, obj->parallax_depth, 2u);
    ok = ok && jb_key(jb, &first, "alpha") &&
         jb_bound_f32_opt_script(jb, scene, obj->alpha, obj->alpha_user_prop, alpha_vm, NULL);
    ok = ok && jb_key(jb, &first, "brightness") && jb_f32(jb, obj->brightness);
    ok = ok && jb_key(jb, &first, "color") &&
         jb_bound_vec_opt_script(jb, scene, obj->color, 3u, obj->color_user_prop, col_vm, NULL);
    ok = ok && jb_key(jb, &first, "colorBlendMode") && jb_i64(jb, obj->color_blend_mode);
    ok = ok && jb_key(jb, &first, "visible") &&
         jb_bound_bool_opt_script(jb, scene, obj->visible, obj->visible_user_prop, vis_vm);
    ok = ok && jb_key(jb, &first, "copybackground") &&
         jb_cstr(jb, obj->copybackground ? "true" : "false");
    if (obj->fullscreen) {
        ok = ok && jb_key(jb, &first, "fullscreen") && jb_cstr(jb, "true");
    }
    if (obj->passthrough) {
        ok = ok && jb_key(jb, &first, "passthrough") && jb_cstr(jb, "true");
    }
    if (ok && obj->is_text_object) {
        ok = serialize_object_text(jb, scene, obj, &first);
    }
    if (ok && obj->is_light_object) {
        ok = serialize_object_light(jb, obj, &first);
    }
    if (ok && obj->is_sound_object) {
        ok = serialize_object_sound(jb, scene, obj, obj_idx, &first);
    }
    if (ok && obj->effect_count > 0u) {
        ok = serialize_object_effects(jb, scene, obj, obj_idx, ectx, &first);
    }
    return ok && jb_ch(jb, '}');
}

static const char *projection_name(uint32_t mode)
{
    switch (mode) {
    case WE_PROJ_PERSPECTIVE_3D:
        return "perspective";
    case WE_PROJ_DEPTH_PARALLAX_25D:
        return "depthparallax";
    case WE_PROJ_ORTHOGRAPHIC_2D:
    default:
        return "orthographic";
    }
}

static const char *cam_loop_name(uint32_t mode)
{
    switch (mode) {
    case WE_CAM_PATH_PINGPONG:
        return "pingpong";
    case WE_CAM_PATH_ONCE:
        return "once";
    case WE_CAM_PATH_LOOP:
    default:
        return "loop";
    }
}

static bool serialize_scene_general(json_buf_t *jb, const we_scene_t *scene, bool *first)
{
    bool ok = jb_key(jb, first, "general") && jb_ch(jb, '{');
    bool gf = true;
    ok = ok && jb_key(jb, &gf, "orthogonalprojection") && jb_cstr(jb, "{\"width\":") &&
         jb_f32(jb, scene->ortho_width) && jb_cstr(jb, ",\"height\":") &&
         jb_f32(jb, scene->ortho_height) && jb_ch(jb, '}');
    ok = ok && jb_key(jb, &gf, "clearcolor") && jb_vec_str(jb, scene->clear_color, 3u);
    ok = ok && jb_key(jb, &gf, "clearenabled") &&
         jb_cstr(jb, scene->clear_enabled ? "true" : "false");
    ok = ok && jb_key(jb, &gf, "cameraparallax") &&
         jb_cstr(jb, scene->camera_parallax ? "true" : "false");
    ok = ok && jb_key(jb, &gf, "cameraparallaxamount") && jb_f32(jb, scene->camera_parallax_amount);
    ok = ok && jb_key(jb, &gf, "cameraparallaxdelay") && jb_f32(jb, scene->camera_parallax_delay);
    ok = ok && jb_key(jb, &gf, "cameraparallaxmouseinfluence") &&
         jb_f32(jb, scene->camera_parallax_mouse_influence);
    ok =
        ok && jb_key(jb, &gf, "camerashake") && jb_cstr(jb, scene->camera_shake ? "true" : "false");
    ok = ok && jb_key(jb, &gf, "camerashakeamplitude") && jb_f32(jb, scene->camera_shake_amp);
    ok = ok && jb_key(jb, &gf, "camerashakeroughness") && jb_f32(jb, scene->camera_shake_roughness);
    ok = ok && jb_key(jb, &gf, "camerashakespeed") && jb_f32(jb, scene->camera_shake_speed);
    return ok && jb_ch(jb, '}');
}

static bool serialize_scene_camera(json_buf_t *jb, const we_scene_t *scene, bool *first)
{
    const float *eye = (scene->is_3d || scene->camera.fov_y_deg > 0.0f) ? scene->camera.eye_pos
                                                                        : scene->camera_eye;
    const float *ctr = (scene->is_3d || scene->camera.fov_y_deg > 0.0f) ? scene->camera.target_pos
                                                                        : scene->camera_center;
    const float *up =
        (scene->is_3d || scene->camera.fov_y_deg > 0.0f) ? scene->camera.up_vec : scene->camera_up;

    bool ok = jb_key(jb, first, "camera") && jb_ch(jb, '{');
    bool cf = true;
    ok = ok && jb_key(jb, &cf, "projection") &&
         jb_str(jb, projection_name(scene->camera.projection_mode));
    ok = ok && jb_key(jb, &cf, "fov") &&
         jb_f32(jb, scene->camera.fov_y_deg > 0.0f ? scene->camera.fov_y_deg : 60.0f);
    ok = ok && jb_key(jb, &cf, "nearz") &&
         jb_f32(jb, scene->camera.near_z > 0.0f ? scene->camera.near_z : 1.0f);
    ok = ok && jb_key(jb, &cf, "farz") &&
         jb_f32(jb, scene->camera.far_z > 0.0f ? scene->camera.far_z : 10000.0f);
    ok = ok && jb_key(jb, &cf, "eye") && jb_vec_str(jb, eye, 3u);
    ok = ok && jb_key(jb, &cf, "center") && jb_vec_str(jb, ctr, 3u);
    ok = ok && jb_key(jb, &cf, "up") && jb_vec_str(jb, up, 3u);
    return ok && jb_ch(jb, '}');
}

static bool serialize_scene_camerapaths(json_buf_t *jb, const we_scene_t *scene, bool *first)
{
    bool ok = jb_key(jb, first, "camerapaths") && jb_ch(jb, '{');
    bool cpf = true;
    ok = ok && jb_key(jb, &cpf, "mode") && jb_str(jb, cam_loop_name(scene->camera.path_loop_mode));
    ok = ok && jb_key(jb, &cpf, "keyframes") && jb_ch(jb, '[');
    for (uint32_t k = 0u; ok && k < scene->camera.keyframe_count; ++k) {
        const we_camera_kf_t *kf = &scene->camera.keyframes[k];
        if (k > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        ok = ok && jb_ch(jb, '{');
        bool kff = true;
        ok = ok && jb_key(jb, &kff, "time") && jb_f32(jb, kf->time_sec);
        ok = ok && jb_key(jb, &kff, "position") && jb_vec_str(jb, kf->position, 3u);
        ok = ok && jb_key(jb, &kff, "orientation") && jb_ch(jb, '[') &&
             jb_f32(jb, kf->orientation_quat[0]) && jb_ch(jb, ',') &&
             jb_f32(jb, kf->orientation_quat[1]) && jb_ch(jb, ',') &&
             jb_f32(jb, kf->orientation_quat[2]) && jb_ch(jb, ',') &&
             jb_f32(jb, kf->orientation_quat[3]) && jb_ch(jb, ']');
        ok = ok && jb_key(jb, &kff, "fov") && jb_f32(jb, kf->fov_deg);
        ok = ok && jb_key(jb, &kff, "focaldistance") && jb_f32(jb, kf->focal_distance);
        ok = ok && jb_key(jb, &kff, "aperture") && jb_f32(jb, kf->aperture_fstop);
        ok = ok && jb_key(jb, &kff, "tension") && jb_f32(jb, kf->tension);
        ok = ok && jb_ch(jb, '}');
    }
    return ok && jb_ch(jb, ']') && jb_ch(jb, '}');
}

static bool serialize_scene_with_overrides(const we_scene_t *scene,
                                           const char *const *image_overrides,
                                           const char *const *puppet_overrides,
                                           const char *const *particle_overrides,
                                           const export_ctx_t *ectx, char **out_json,
                                           size_t *out_len, we_asset_writer_err_t *out_err)
{
    if (!scene || !out_json) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    *out_json = NULL;
    if (out_len) {
        *out_len = 0u;
    }
    if (scene->object_count > WE_SCENE_MAX_OBJECTS ||
        (scene->object_count > 0u && !scene->objects)) {
        set_err(out_err, scene->object_count > WE_SCENE_MAX_OBJECTS ? WE_WRITER_ERR_LIMIT_EXCEEDED
                                                                    : WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    if (scene->camera.keyframe_count > WE_CAMERA_MAX_KEYFRAMES) {
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    bool first = true;

    ok = ok && serialize_scene_general(&jb, scene, &first);
    /* Avoid emitting "is3d":true for 2.5D so parser preserves projection_mode == 2. */
    if (ok && scene->is_3d && scene->camera.projection_mode == WE_PROJ_PERSPECTIVE_3D) {
        ok = jb_key(&jb, &first, "is3d") && jb_cstr(&jb, "true");
    }
    if (ok) {
        ok = serialize_scene_camera(&jb, scene, &first);
    }
    if (ok && scene->camera.keyframe_count > 0u) {
        ok = serialize_scene_camerapaths(&jb, scene, &first);
    }

    ok = ok && jb_key(&jb, &first, "objects") && jb_ch(&jb, '[');
    for (uint32_t i = 0u; ok && i < scene->object_count; ++i) {
        if (i > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        const char *ovr = image_overrides ? image_overrides[i] : NULL;
        const char *povr = puppet_overrides ? puppet_overrides[i] : NULL;
        const char *part_ovr = particle_overrides ? particle_overrides[i] : NULL;
        ok = ok && serialize_object(&jb, scene, &scene->objects[i], i, ovr, povr, part_ovr, ectx);
    }
    ok = ok && jb_ch(&jb, ']') && jb_ch(&jb, '}');

    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

bool we_scene_serialize_json(const we_scene_t *scene, char **out_json, size_t *out_len,
                             we_asset_writer_err_t *out_err)
{
    return serialize_scene_with_overrides(scene, NULL, NULL, NULL, NULL, out_json, out_len,
                                          out_err);
}

static bool mkdir_p(const char *dir)
{
    char buf[PATH_MAX];
    size_t len = strlen(dir);
    if (len == 0u || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, dir, len + 1u);
    while (len > 1u && buf[len - 1u] == '/') {
        buf[--len] = '\0';
    }
    for (char *p = buf + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static void asset_list_free(asset_list_t *al)
{
    for (size_t i = 0u; i < al->count; ++i) {
        free(al->items[i].path);
        free(al->items[i].data);
    }
    free(al->items);
    memset(al, 0, sizeof(*al));
}

static const owned_asset_t *asset_list_find(const asset_list_t *al, const char *norm_path)
{
    for (size_t i = 0u; i < al->count; ++i) {
        if (strcasecmp(al->items[i].path, norm_path) == 0) {
            return &al->items[i];
        }
    }
    return NULL;
}

static bool asset_list_has(const asset_list_t *al, const char *norm_path)
{
    return asset_list_find(al, norm_path) != NULL;
}

static void export_ctx_free(export_ctx_t *ectx)
{
    if (!ectx) {
        return;
    }
    free(ectx->shader_overrides);
    memset(ectx, 0, sizeof(*ectx));
}

static bool export_ctx_add_shader_override(export_ctx_t *ectx, const we_material_pass_t *pass,
                                           const char *shader_name, we_asset_writer_err_t *out_err)
{
    if (!ectx || !pass || !shader_name) {
        return true;
    }
    for (size_t i = 0u; i < ectx->shader_override_count; ++i) {
        if (ectx->shader_overrides[i].pass == pass) {
            snprintf(ectx->shader_overrides[i].shader_name,
                     sizeof(ectx->shader_overrides[i].shader_name), "%s", shader_name);
            return true;
        }
    }
    if (ectx->shader_override_count == ectx->shader_override_cap) {
        size_t ncap = (ectx->shader_override_cap == 0u) ? 16u : (ectx->shader_override_cap * 2u);
        shader_override_t *grown =
            (shader_override_t *)realloc(ectx->shader_overrides, ncap * sizeof(*grown));
        if (!grown) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        ectx->shader_overrides = grown;
        ectx->shader_override_cap = ncap;
    }
    ectx->shader_overrides[ectx->shader_override_count].pass = pass;
    snprintf(ectx->shader_overrides[ectx->shader_override_count].shader_name,
             sizeof(ectx->shader_overrides[0].shader_name), "%s", shader_name);
    ectx->shader_override_count++;
    return true;
}

static bool asset_list_add_owned(asset_list_t *al, const char *rel_path, uint8_t *owned_data,
                                 size_t size, we_asset_writer_err_t *out_err)
{
    char norm[1024];
    if (!we_normalize_rel_path(rel_path, norm, sizeof(norm), NULL)) {
        free(owned_data);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    if (asset_list_has(al, norm)) {
        free(owned_data);
        return true;
    }
    if (al->count == al->cap) {
        size_t ncap = (al->cap == 0u) ? 16u : (al->cap * 2u);
        owned_asset_t *grown = (owned_asset_t *)realloc(al->items, ncap * sizeof(*grown));
        if (!grown) {
            free(owned_data);
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        al->items = grown;
        al->cap = ncap;
    }
    char *pcopy = strdup(norm);
    if (!pcopy) {
        free(owned_data);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    al->items[al->count].path = pcopy;
    al->items[al->count].data = owned_data;
    al->items[al->count].size = size;
    al->count++;
    return true;
}

static bool asset_list_add_copy(asset_list_t *al, const char *rel_path, const uint8_t *src,
                                size_t size, we_asset_writer_err_t *out_err)
{
    uint8_t *copy = NULL;
    if (size > 0u) {
        copy = (uint8_t *)malloc(size);
        if (!copy) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        memcpy(copy, src, size);
    }
    return asset_list_add_owned(al, rel_path, copy, size, out_err);
}

static bool encode_tex_from_memory(asset_list_t *al, const char *norm, const we_tex_t *tex,
                                   bool all_raw, we_asset_writer_err_t *out_err)
{
    we_tex_encode_image_t *imgs = (we_tex_encode_image_t *)calloc(tex->image_count, sizeof(*imgs));
    uint8_t **temp_swiz = NULL;
    size_t temp_count = 0u;
    size_t total_mips = 0u;
    for (uint32_t i = 0u; i < tex->image_count; ++i) {
        total_mips += tex->images[i].mip_count;
    }
    we_tex_encode_mip_t *all_emips =
        (we_tex_encode_mip_t *)calloc(total_mips ? total_mips : 1u, sizeof(*all_emips));
    if (!all_raw && total_mips > 0u) {
        temp_swiz = (uint8_t **)calloc(total_mips, sizeof(*temp_swiz));
    }
    if (!imgs || !all_emips || (!all_raw && total_mips > 0u && !temp_swiz)) {
        free(imgs);
        free(all_emips);
        free(temp_swiz);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    size_t moff = 0u;
    bool prep_ok = true;
    for (uint32_t i = 0u; i < tex->image_count && prep_ok; ++i) {
        imgs[i].mip_count = tex->images[i].mip_count;
        imgs[i].mips = all_emips + moff;
        for (uint32_t m = 0u; m < tex->images[i].mip_count; ++m) {
            const we_tex_mip_t *sm = &tex->images[i].mips[m];
            we_tex_encode_mip_t *dm = &all_emips[moff++];
            dm->width = sm->width;
            dm->height = sm->height;
            if (all_raw) {
                dm->pixels = sm->data;
                dm->byte_size = sm->data_size;
            } else {
                if (sm->width == 0u || sm->height == 0u ||
                    (size_t)sm->width > SIZE_MAX / 4u / (size_t)sm->height) {
                    prep_ok = false;
                    break;
                }
                size_t npx = (size_t)sm->width * (size_t)sm->height;
                size_t nb = npx * 4u;
                uint8_t *rgba = (uint8_t *)malloc(nb);
                if (!rgba) {
                    prep_ok = false;
                    break;
                }
                if (sm->is_bgra_swiz) {
                    for (size_t p = 0u; p < npx; ++p) {
                        rgba[p * 4u + 0u] = sm->rgba8[p * 4u + 2u];
                        rgba[p * 4u + 1u] = sm->rgba8[p * 4u + 1u];
                        rgba[p * 4u + 2u] = sm->rgba8[p * 4u + 0u];
                        rgba[p * 4u + 3u] = sm->rgba8[p * 4u + 3u];
                    }
                } else {
                    memcpy(rgba, sm->rgba8, nb);
                }
                temp_swiz[temp_count++] = rgba;
                dm->pixels = rgba;
                dm->byte_size = nb;
            }
        }
    }

    if (!prep_ok) {
        for (size_t k = 0u; k < temp_count; ++k) {
            free(temp_swiz[k]);
        }
        free(temp_swiz);
        free(all_emips);
        free(imgs);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    we_tex_encode_spec_t spec = {
        .format = all_raw ? tex->format : WE_TEX_FMT_RGBA8888,
        .flags = tex->flags & ~WE_TEX_FLAG_IS_VIDEO_TEXTURE,
        .texture_width = tex->texture_width ? tex->texture_width : all_emips[0].width,
        .texture_height = tex->texture_height ? tex->texture_height : all_emips[0].height,
        .image_width = tex->image_width ? tex->image_width : all_emips[0].width,
        .image_height = tex->image_height ? tex->image_height : all_emips[0].height,
        .enable_lz4 = true,
        .image_count = tex->image_count,
        .images = imgs,
        .frame_count = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frame_count : 0u,
        .gif_width = tex->gif_width,
        .gif_height = tex->gif_height,
        .frames = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frames : NULL,
    };

    uint8_t *enc_buf = NULL;
    size_t enc_sz = 0u;
    bool ok = we_tex_encode_v5(&spec, &enc_buf, &enc_sz, out_err);
    for (size_t k = 0u; k < temp_count; ++k) {
        free(temp_swiz[k]);
    }
    free(temp_swiz);
    free(all_emips);
    free(imgs);
    if (!ok) {
        return false;
    }
    return asset_list_add_owned(al, norm, enc_buf, enc_sz, out_err);
}

static bool decode_tex_mip_rgba8(we_tex_format_t fmt, const we_tex_mip_t *sm, uint8_t *out_rgba,
                                 we_asset_writer_err_t *out_err)
{
    if (!sm || sm->width == 0u || sm->height == 0u || sm->width > WE_TEX_MAX_DIM ||
        sm->height > WE_TEX_MAX_DIM || (size_t)sm->width > SIZE_MAX / 4u / (size_t)sm->height) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    size_t npx = (size_t)sm->width * (size_t)sm->height;
    if (sm->rgba8) {
        if (sm->is_bgra_swiz) {
            for (size_t p = 0u; p < npx; ++p) {
                out_rgba[p * 4u + 0u] = sm->rgba8[p * 4u + 2u];
                out_rgba[p * 4u + 1u] = sm->rgba8[p * 4u + 1u];
                out_rgba[p * 4u + 2u] = sm->rgba8[p * 4u + 0u];
                out_rgba[p * 4u + 3u] = sm->rgba8[p * 4u + 3u];
            }
        } else {
            memcpy(out_rgba, sm->rgba8, npx * 4u);
        }
        return true;
    }
    if (!sm->data || sm->data_size == 0u) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }

    switch (fmt) {
    case WE_TEX_FMT_RGBA8888:
        if (sm->data_size < npx * 4u) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        memcpy(out_rgba, sm->data, npx * 4u);
        return true;
    case WE_TEX_FMT_RG88:
        if (sm->data_size < npx * 2u) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        for (size_t p = 0u; p < npx; ++p) {
            uint8_t r = sm->data[p * 2u + 0u];
            uint8_t g = sm->data[p * 2u + 1u];
            out_rgba[p * 4u + 0u] = r;
            out_rgba[p * 4u + 1u] = r;
            out_rgba[p * 4u + 2u] = r;
            out_rgba[p * 4u + 3u] = g;
        }
        return true;
    case WE_TEX_FMT_R8:
        if (sm->data_size < npx) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        for (size_t p = 0u; p < npx; ++p) {
            uint8_t r = sm->data[p];
            out_rgba[p * 4u + 0u] = r;
            out_rgba[p * 4u + 1u] = r;
            out_rgba[p * 4u + 2u] = r;
            out_rgba[p * 4u + 3u] = r;
        }
        return true;
    case WE_TEX_FMT_DXT1:
        if (!we_decode_dxt1_rgba8(sm->data, sm->data_size, sm->width, sm->height, out_rgba)) {
            set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
            return false;
        }
        return true;
    case WE_TEX_FMT_DXT3:
        if (!we_decode_dxt3_rgba8(sm->data, sm->data_size, sm->width, sm->height, out_rgba)) {
            set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
            return false;
        }
        return true;
    case WE_TEX_FMT_DXT5:
        if (!we_decode_dxt5_rgba8(sm->data, sm->data_size, sm->width, sm->height, out_rgba)) {
            set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
            return false;
        }
        return true;
    case WE_TEX_FMT_BC7:
        if (!we_decode_bc7_rgba8(sm->data, sm->data_size, sm->width, sm->height, out_rgba)) {
            set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
            return false;
        }
        return true;
    default:
        set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
        return false;
    }
}

static bool encode_tex_decoded_mips(asset_list_t *al, const char *norm, const we_tex_t *tex,
                                    we_asset_writer_err_t *out_err)
{
    we_tex_encode_image_t *imgs = (we_tex_encode_image_t *)calloc(tex->image_count, sizeof(*imgs));
    size_t total_mips = 0u;
    for (uint32_t i = 0u; i < tex->image_count; ++i) {
        total_mips += tex->images[i].mip_count;
    }
    we_tex_encode_mip_t *all_emips =
        (we_tex_encode_mip_t *)calloc(total_mips ? total_mips : 1u, sizeof(*all_emips));
    uint8_t **decoded_bufs =
        total_mips ? (uint8_t **)calloc(total_mips, sizeof(*decoded_bufs)) : NULL;
    if (!imgs || !all_emips || (total_mips > 0u && !decoded_bufs)) {
        free(imgs);
        free(all_emips);
        free(decoded_bufs);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    size_t moff = 0u;
    size_t dec_count = 0u;
    bool prep_ok = true;
    for (uint32_t i = 0u; i < tex->image_count && prep_ok; ++i) {
        imgs[i].mip_count = tex->images[i].mip_count;
        imgs[i].mips = all_emips + moff;
        for (uint32_t m = 0u; m < tex->images[i].mip_count; ++m) {
            const we_tex_mip_t *sm = &tex->images[i].mips[m];
            we_tex_encode_mip_t *dm = &all_emips[moff++];
            dm->width = sm->width;
            dm->height = sm->height;
            if (sm->width == 0u || sm->height == 0u ||
                (size_t)sm->width > SIZE_MAX / 4u / (size_t)sm->height) {
                prep_ok = false;
                set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
                break;
            }
            size_t nb = (size_t)sm->width * (size_t)sm->height * 4u;
            uint8_t *rgba = (uint8_t *)malloc(nb);
            if (!rgba) {
                prep_ok = false;
                set_err(out_err, WE_WRITER_ERR_OOM);
                break;
            }
            decoded_bufs[dec_count++] = rgba;
            if (!decode_tex_mip_rgba8(tex->format, sm, rgba, out_err)) {
                prep_ok = false;
                break;
            }
            dm->pixels = rgba;
            dm->byte_size = nb;
        }
    }

    bool ok = false;
    if (prep_ok && total_mips > 0u) {
        we_tex_encode_spec_t spec = {
            .format = WE_TEX_FMT_RGBA8888,
            .flags = tex->flags & ~WE_TEX_FLAG_IS_VIDEO_TEXTURE,
            .texture_width = tex->texture_width ? tex->texture_width : all_emips[0].width,
            .texture_height = tex->texture_height ? tex->texture_height : all_emips[0].height,
            .image_width = tex->image_width ? tex->image_width : all_emips[0].width,
            .image_height = tex->image_height ? tex->image_height : all_emips[0].height,
            .enable_lz4 = true,
            .image_count = tex->image_count,
            .images = imgs,
            .frame_count = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frame_count : 0u,
            .gif_width = tex->gif_width,
            .gif_height = tex->gif_height,
            .frames = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frames : NULL,
        };
        uint8_t *enc_buf = NULL;
        size_t enc_sz = 0u;
        if (we_tex_encode_v5(&spec, &enc_buf, &enc_sz, out_err)) {
            ok = asset_list_add_owned(al, norm, enc_buf, enc_sz, out_err);
        }
    }

    for (size_t k = 0u; k < dec_count; ++k) {
        free(decoded_bufs[k]);
    }
    free(decoded_bufs);
    free(all_emips);
    free(imgs);
    return ok;
}

static GLuint compile_readback_quad_prog(void)
{
    static const char *k_vs = "#version 310 es\n"
                              "out vec2 v_uv;\n"
                              "void main() {\n"
                              "  vec2 p = vec2((gl_VertexID & 1) * 2 - 1, (gl_VertexID & 2) - 1);\n"
                              "  v_uv = p * 0.5 + 0.5;\n"
                              "  gl_Position = vec4(p, 0.0, 1.0);\n"
                              "}\n";
    static const char *k_fs = "#version 310 es\n"
                              "precision highp float;\n"
                              "in vec2 v_uv;\n"
                              "uniform sampler2D u_tex;\n"
                              "out vec4 o_col;\n"
                              "void main() { o_col = texture(u_tex, v_uv); }\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!vs || !fs) {
        if (vs) {
            glDeleteShader(vs);
        }
        if (fs) {
            glDeleteShader(fs);
        }
        return 0;
    }
    glShaderSource(vs, 1, &k_vs, NULL);
    glCompileShader(vs);
    GLint ok_v = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok_v);
    glShaderSource(fs, 1, &k_fs, NULL);
    glCompileShader(fs);
    GLint ok_f = 0;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok_f);
    if (!ok_v || !ok_f) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool readback_gl_tex_via_quad(GLuint src_tex, uint32_t w, uint32_t h, uint8_t *out_rgba)
{
    GLuint prog = compile_readback_quad_prog();
    if (!prog) {
        return false;
    }

    GLint prev_prog = 0;
    GLint prev_vao = 0;
    GLint prev_vp[4] = {0};
    GLint prev_active = GL_TEXTURE0;
    GLint prev_tex = 0;
    GLint prev_rfbo = 0;
    GLint prev_dfbo = 0;
    GLint prev_pack = 4;
    GLint prev_pbo = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_rfbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_dfbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
    GLboolean was_blend = glIsEnabled(GL_BLEND);
    GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean was_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

    GLuint dst_tex = 0;
    GLuint fbo = 0;
    GLuint vao = 0;
    glGenTextures(1, &dst_tex);
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    bool ok = false;
    if (dst_tex && fbo && vao) {
        glBindTexture(GL_TEXTURE_2D, dst_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, (GLsizei)w, (GLsizei)h);
            glUseProgram(prog);
            glBindTexture(GL_TEXTURE_2D, src_tex);
            glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            while (glGetError() != GL_NO_ERROR) {
            }
            glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
            ok = (glGetError() == GL_NO_ERROR);
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }

    if (was_blend) {
        glEnable(GL_BLEND);
    }
    if (was_depth) {
        glEnable(GL_DEPTH_TEST);
    }
    if (was_cull) {
        glEnable(GL_CULL_FACE);
    }
    if (was_scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_rfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_dfbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindVertexArray((GLuint)prev_vao);
    glUseProgram((GLuint)prev_prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);

    if (vao) {
        glDeleteVertexArrays(1, &vao);
    }
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
    }
    if (dst_tex) {
        glDeleteTextures(1, &dst_tex);
    }
    glDeleteProgram(prog);
    return ok;
}

static bool export_texture_from_gl(asset_list_t *al, const we_scene_t *scene, const char *norm,
                                   const we_tex_t *tex, bool allow_pkg_fallback_on_incomplete,
                                   bool *out_try_pkg, we_asset_writer_err_t *out_err)
{
    *out_try_pkg = false;
    EGLDisplay prev_dpy = eglGetCurrentDisplay();
    EGLContext prev_ctx = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    bool switched_ctx = false;

    if (prev_ctx == EGL_NO_CONTEXT && scene && scene->gl_initialized &&
        scene->egl_display != NULL && scene->egl_context != NULL) {
        if (eglMakeCurrent((EGLDisplay)scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           (EGLContext)scene->egl_context)) {
            switched_ctx = true;
        }
    }

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        *out_try_pkg = true;
        return false;
    }

    uint32_t img_count = (tex->image_count > 0u) ? tex->image_count : 1u;
    if (img_count > WE_TEX_MAX_IMAGES) {
        if (switched_ctx) {
            eglMakeCurrent(prev_dpy != EGL_NO_DISPLAY ? prev_dpy : (EGLDisplay)scene->egl_display,
                           prev_draw, prev_read, prev_ctx);
        }
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    we_tex_format_t out_fmt = WE_TEX_FMT_RGBA8888;
    if (tex->format == WE_TEX_FMT_R8 || tex->format == WE_TEX_FMT_RG88) {
        out_fmt = tex->format;
    }

    we_tex_encode_image_t *eimgs = (we_tex_encode_image_t *)calloc(img_count, sizeof(*eimgs));
    we_tex_encode_mip_t **mip_chains =
        (we_tex_encode_mip_t **)calloc(img_count, sizeof(*mip_chains));
    uint32_t *mip_counts = (uint32_t *)calloc(img_count, sizeof(*mip_counts));
    if (!eimgs || !mip_chains || !mip_counts) {
        free(eimgs);
        free(mip_chains);
        free(mip_counts);
        if (switched_ctx) {
            eglMakeCurrent(prev_dpy != EGL_NO_DISPLAY ? prev_dpy : (EGLDisplay)scene->egl_display,
                           prev_draw, prev_read, prev_ctx);
        }
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    GLint prev_rfbo = 0;
    GLint prev_dfbo = 0;
    GLint prev_tex = 0;
    GLint prev_pack = 4;
    GLint prev_pbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_rfbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_dfbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);

    GLuint rb_fbo = 0;
    glGenFramebuffers(1, &rb_fbo);

    bool ok = (rb_fbo != 0);
    for (uint32_t i = 0u; ok && i < img_count; ++i) {
        GLuint src_tex = (img_count > 1u && tex->frame_gl_texs) ? (GLuint)tex->frame_gl_texs[i]
                                                                : (GLuint)tex->gl_tex;
        uint32_t w = tex->texture_width ? tex->texture_width : tex->image_width;
        uint32_t h = tex->texture_height ? tex->texture_height : tex->image_height;
        if (tex->images && i < tex->image_count && tex->images[i].mip_count > 0u &&
            tex->images[i].mips) {
            if (tex->images[i].mips[0].width > 0u) {
                w = tex->images[i].mips[0].width;
            }
            if (tex->images[i].mips[0].height > 0u) {
                h = tex->images[i].mips[0].height;
            }
        }
        if (src_tex == 0 || !glIsTexture(src_tex) || w == 0u || h == 0u || w > WE_TEX_MAX_DIM ||
            h > WE_TEX_MAX_DIM || (size_t)w > SIZE_MAX / 4u / (size_t)h) {
            ok = false;
            *out_try_pkg = (src_tex == 0 || !glIsTexture(src_tex));
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            break;
        }

        size_t npx = (size_t)w * (size_t)h;
        uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
        if (!rgba) {
            ok = false;
            set_err(out_err, WE_WRITER_ERR_OOM);
            break;
        }

        GLint swiz_r = GL_RED;
        GLint swiz_b = GL_BLUE;
        glBindTexture(GL_TEXTURE_2D, src_tex);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &swiz_r);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &swiz_b);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, rb_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_tex,
                               0);
        bool read_ok = false;
        bool used_quad = false;
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            while (glGetError() != GL_NO_ERROR) {
            }
            glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            read_ok = (glGetError() == GL_NO_ERROR);
        } else if (allow_pkg_fallback_on_incomplete) {
            /* Unmodified compressed GPU textures fall back to exact package bytes when present. */
            *out_try_pkg = true;
            free(rgba);
            ok = false;
            break;
        } else {
            read_ok = readback_gl_tex_via_quad(src_tex, w, h, rgba);
            used_quad = true;
        }
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

        if (!read_ok) {
            free(rgba);
            ok = false;
            set_err(out_err, WE_WRITER_ERR_IO);
            break;
        }

        /* Restore canonical RGBA channel order if GL uploaded BGRA-swizzled source directly. */
        if (!used_quad && out_fmt == WE_TEX_FMT_RGBA8888 && swiz_r == GL_BLUE && swiz_b == GL_RED) {
            for (size_t p = 0u; p < npx; ++p) {
                uint8_t tmp = rgba[p * 4u + 0u];
                rgba[p * 4u + 0u] = rgba[p * 4u + 2u];
                rgba[p * 4u + 2u] = tmp;
            }
        }

        uint8_t *lvl0 = rgba;
        if (out_fmt == WE_TEX_FMT_R8) {
            uint8_t *r8 = (uint8_t *)malloc(npx);
            if (!r8) {
                free(rgba);
                ok = false;
                set_err(out_err, WE_WRITER_ERR_OOM);
                break;
            }
            for (size_t p = 0u; p < npx; ++p) {
                r8[p] = rgba[p * 4u + 0u];
            }
            free(rgba);
            lvl0 = r8;
        } else if (out_fmt == WE_TEX_FMT_RG88) {
            uint8_t *rg8 = (uint8_t *)malloc(npx * 2u);
            if (!rg8) {
                free(rgba);
                ok = false;
                set_err(out_err, WE_WRITER_ERR_OOM);
                break;
            }
            for (size_t p = 0u; p < npx; ++p) {
                rg8[p * 2u + 0u] = rgba[p * 4u + 0u];
                rg8[p * 2u + 1u] = rgba[p * 4u + 1u];
            }
            free(rgba);
            lvl0 = rg8;
        }

        if (!we_tex_generate_mips(out_fmt, w, h, lvl0, &mip_chains[i], &mip_counts[i]) ||
            mip_counts[i] == 0u) {
            free(lvl0);
            ok = false;
            set_err(out_err, WE_WRITER_ERR_OOM);
            break;
        }
        free(lvl0);
        eimgs[i].mip_count = mip_counts[i];
        eimgs[i].mips = mip_chains[i];
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_rfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_dfbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
    if (rb_fbo) {
        glDeleteFramebuffers(1, &rb_fbo);
    }
    if (switched_ctx) {
        eglMakeCurrent(prev_dpy != EGL_NO_DISPLAY ? prev_dpy : (EGLDisplay)scene->egl_display,
                       prev_draw, prev_read, prev_ctx);
    }

    if (ok) {
        we_tex_encode_spec_t spec = {
            .format = out_fmt,
            .flags = tex->flags & ~WE_TEX_FLAG_IS_VIDEO_TEXTURE,
            .texture_width = tex->texture_width ? tex->texture_width : eimgs[0].mips[0].width,
            .texture_height = tex->texture_height ? tex->texture_height : eimgs[0].mips[0].height,
            .image_width = tex->image_width ? tex->image_width : eimgs[0].mips[0].width,
            .image_height = tex->image_height ? tex->image_height : eimgs[0].mips[0].height,
            .enable_lz4 = true,
            .image_count = img_count,
            .images = eimgs,
            .frame_count = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frame_count : 0u,
            .gif_width = tex->gif_width,
            .gif_height = tex->gif_height,
            .frames = ((tex->flags & WE_TEX_FLAG_IS_GIF) != 0u) ? tex->frames : NULL,
        };
        uint8_t *enc_buf = NULL;
        size_t enc_sz = 0u;
        if (!we_tex_encode_v5(&spec, &enc_buf, &enc_sz, out_err) ||
            !asset_list_add_owned(al, norm, enc_buf, enc_sz, out_err)) {
            ok = false;
        }
    }

    for (uint32_t i = 0u; i < img_count; ++i) {
        if (mip_chains[i]) {
            we_tex_free_generated_mips(mip_chains[i], mip_counts[i]);
        }
    }
    free(mip_counts);
    free(mip_chains);
    free(eimgs);
    return ok;
}

static bool pkg_blob_is_tex_container(const uint8_t *data, size_t sz)
{
    if (!data) {
        return false;
    }
    if (sz >= 8u && memcmp(data, "TEXV", 4u) == 0) {
        return true;
    }
    if (sz >= 12u && memcmp(data + 4u, "TEXV", 4u) == 0) {
        return true;
    }
    return false;
}

static bool export_texture_asset(asset_list_t *al, const we_scene_t *scene, const char *tex_name,
                                 const we_tex_t *tex, we_asset_writer_err_t *out_err)
{
    if (!tex_name || tex_name[0] == '\0' || strncmp(tex_name, "_rt_", 4u) == 0 ||
        strncasecmp(tex_name, "util/", 5u) == 0) {
        return true;
    }

    char norm_in[512];
    if (!we_normalize_rel_path(tex_name, norm_in, sizeof(norm_in), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    char target[512];
    size_t nlen = strlen(norm_in);
    bool has_ext = (nlen > 4u && strcasecmp(norm_in + nlen - 4u, ".tex") == 0);
    int tn;
    if (strncasecmp(norm_in, "materials/", 10u) == 0) {
        tn = snprintf(target, sizeof(target), has_ext ? "%s" : "%s.tex", norm_in);
    } else {
        tn = snprintf(target, sizeof(target), has_ext ? "materials/%s" : "materials/%s.tex",
                      norm_in);
    }
    char norm[512];
    if (tn < 0 || (size_t)tn >= sizeof(target) ||
        !we_normalize_rel_path(target, norm, sizeof(norm), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    if (asset_list_has(al, norm)) {
        return true;
    }

    if (tex && tex->image_count > 0u && tex->images) {
        bool raw_fmt_ok = (tex->format == WE_TEX_FMT_RGBA8888 || tex->format == WE_TEX_FMT_RG88 ||
                           tex->format == WE_TEX_FMT_R8);
        bool all_raw = raw_fmt_ok;
        bool all_rgba8 = true;
        bool all_decodable = true;
        for (uint32_t i = 0u; i < tex->image_count; ++i) {
            if (tex->images[i].mip_count == 0u || !tex->images[i].mips) {
                all_raw = false;
                all_rgba8 = false;
                all_decodable = false;
                break;
            }
            for (uint32_t m = 0u; m < tex->images[i].mip_count; ++m) {
                const we_tex_mip_t *mip = &tex->images[i].mips[m];
                bool has_data = (mip->data != NULL && mip->data_size > 0u);
                bool has_rgba8 = (mip->rgba8 != NULL);
                if (!has_data) {
                    all_raw = false;
                }
                if (!has_rgba8) {
                    all_rgba8 = false;
                }
                if (!has_data && !has_rgba8) {
                    all_decodable = false;
                }
            }
        }
        if (all_raw || all_rgba8) {
            return encode_tex_from_memory(al, norm, tex, all_raw, out_err);
        }
        if (all_decodable) {
            return encode_tex_decoded_mips(al, norm, tex, out_err);
        }
    }

    bool try_pkg = true;
    if (tex && tex->gl_tex != 0u && !tex->is_video_mp4) {
        bool pkg_avail = (scene && scene->pkg != NULL);
        if (export_texture_from_gl(al, scene, norm, tex, pkg_avail, &try_pkg, out_err)) {
            return true;
        }
        if (!try_pkg) {
            return false;
        }
    }

    if (scene && scene->pkg) {
        const char *cands[5] = {norm, norm_in, tex_name, NULL, NULL};
        char alt0[512];
        char alt1[512];
        if (!has_ext) {
            if (snprintf(alt0, sizeof(alt0), "%s.tex", norm_in) < (int)sizeof(alt0)) {
                cands[3] = alt0;
            }
            if (snprintf(alt1, sizeof(alt1), "materials/%s", norm_in) < (int)sizeof(alt1)) {
                cands[4] = alt1;
            }
        }
        for (int i = 0; i < 5; ++i) {
            if (!cands[i] || cands[i][0] == '\0') {
                continue;
            }
            size_t rsz = 0u;
            bool owned = false;
            const uint8_t *rdata = we_pkg_resolve(scene->pkg, cands[i], &rsz, &owned);
            if (!rdata) {
                continue;
            }
            if (pkg_blob_is_tex_container(rdata, rsz)) {
                return owned ? asset_list_add_owned(al, norm, (uint8_t *)(uintptr_t)rdata, rsz,
                                                    out_err)
                             : asset_list_add_copy(al, norm, rdata, rsz, out_err);
            }
            /* Transcode raw PNG/JPG/WebP assets instead of writing non-TEX bytes as .tex. */
            uint8_t *bgra = NULL;
            uint32_t iw = 0u, ih = 0u;
            bool dec = image_load_from_memory_rgba(rdata, rsz, &bgra, &iw, &ih);
            if (owned) {
                free((void *)(uintptr_t)rdata);
            }
            if (!dec || !bgra || iw == 0u || ih == 0u || iw > WE_TEX_MAX_DIM ||
                ih > WE_TEX_MAX_DIM || (size_t)iw > SIZE_MAX / 4u / (size_t)ih) {
                if (bgra) {
                    image_free(bgra);
                }
                set_err(out_err, WE_WRITER_ERR_UNSUPPORTED_FMT);
                return false;
            }
            size_t npx = (size_t)iw * (size_t)ih;
            uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
            if (!rgba) {
                image_free(bgra);
                set_err(out_err, WE_WRITER_ERR_OOM);
                return false;
            }
            for (size_t p = 0u; p < npx; ++p) {
                rgba[p * 4u + 0u] = bgra[p * 4u + 2u];
                rgba[p * 4u + 1u] = bgra[p * 4u + 1u];
                rgba[p * 4u + 2u] = bgra[p * 4u + 0u];
                rgba[p * 4u + 3u] = bgra[p * 4u + 3u];
            }
            image_free(bgra);

            we_tex_encode_mip_t *emips = NULL;
            uint32_t mcnt = 0u;
            bool m_ok =
                we_tex_generate_mips(WE_TEX_FMT_RGBA8888, iw, ih, rgba, &emips, &mcnt) && mcnt > 0u;
            free(rgba);
            if (!m_ok) {
                if (emips) {
                    we_tex_free_generated_mips(emips, mcnt);
                }
                set_err(out_err, WE_WRITER_ERR_OOM);
                return false;
            }
            we_tex_encode_image_t eimg = {.mip_count = mcnt, .mips = emips};
            we_tex_encode_spec_t spec = {
                .format = WE_TEX_FMT_RGBA8888,
                .flags = WE_TEX_FLAG_NONE,
                .texture_width = iw,
                .texture_height = ih,
                .image_width = iw,
                .image_height = ih,
                .enable_lz4 = true,
                .image_count = 1u,
                .images = &eimg,
            };
            uint8_t *enc_buf = NULL;
            size_t enc_sz = 0u;
            bool enc_ok = we_tex_encode_v5(&spec, &enc_buf, &enc_sz, out_err);
            we_tex_free_generated_mips(emips, mcnt);
            if (!enc_ok) {
                return false;
            }
            return asset_list_add_owned(al, norm, enc_buf, enc_sz, out_err);
        }
    }

    if (tex && !tex->is_video_mp4) {
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    return true;
}

static const char *strip_shader_preamble(const char *src, bool is_frag)
{
    static const char k_hdr[] = "#version 310 es\nprecision highp float;\n";
    if (!src || strncmp(src, k_hdr, sizeof(k_hdr) - 1u) != 0) {
        return src;
    }
    const char *marker = is_frag ? "#define gl_FragColor we_FragColor\n"
                                 : "#define texture2DGrad we_texture2DGrad\n";
    const char *hit = strstr(src, marker);
    if (!hit) {
        return src;
    }
    const char *cur = hit + strlen(marker);
    /* Strip combo guards emitted immediately after the preamble by assemble_final_stage. */
    while (strncmp(cur, "#ifndef ", 8u) == 0) {
        const char *l1 = strchr(cur, '\n');
        if (!l1) {
            break;
        }
        const char *line2 = l1 + 1u;
        if (strncmp(line2, "#define ", 8u) != 0) {
            break;
        }
        const char *l2 = strchr(line2, '\n');
        if (!l2) {
            break;
        }
        const char *line3 = l2 + 1u;
        if (strncmp(line3, "#endif\n", 7u) != 0) {
            break;
        }
        cur = line3 + 7u;
    }
    return cur;
}

static bool export_shader_includes(asset_list_t *al, const we_pkg_t *pkg, const char *src,
                                   int depth, we_asset_writer_err_t *out_err)
{
    if (!pkg || !src || depth > 8) {
        return true;
    }
    const char *p = src;
    while ((p = strstr(p, "#include")) != NULL) {
        p += 8u;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p != '"') {
            continue;
        }
        ++p;
        const char *end = strchr(p, '"');
        if (!end || end == p || (size_t)(end - p) >= 200u) {
            continue;
        }
        char inc_name[200];
        memcpy(inc_name, p, (size_t)(end - p));
        inc_name[end - p] = '\0';
        p = end + 1u;

        char cands[3][256];
        snprintf(cands[0], sizeof(cands[0]), "shaders/%s", inc_name);
        snprintf(cands[1], sizeof(cands[1]), "shaders/effects/%s", inc_name);
        snprintf(cands[2], sizeof(cands[2]), "%s", inc_name);
        for (int i = 0; i < 3; ++i) {
            size_t isz = 0u;
            bool owned = false;
            const uint8_t *idata = we_pkg_resolve(pkg, cands[i], &isz, &owned);
            if (!idata) {
                continue;
            }
            char *text = (char *)malloc(isz + 1u);
            if (!text) {
                if (owned) {
                    free((void *)(uintptr_t)idata);
                }
                set_err(out_err, WE_WRITER_ERR_OOM);
                return false;
            }
            memcpy(text, idata, isz);
            text[isz] = '\0';
            bool added = owned ? asset_list_add_owned(al, cands[i], (uint8_t *)(uintptr_t)idata,
                                                      isz, out_err)
                               : asset_list_add_copy(al, cands[i], idata, isz, out_err);
            bool rec_ok = added && export_shader_includes(al, pkg, text, depth + 1, out_err);
            free(text);
            if (!rec_ok) {
                return false;
            }
            break;
        }
    }
    return true;
}

static int cmp_shader_combo_ptr(const void *a, const void *b)
{
    const we_shader_combo_t *ca = *(const we_shader_combo_t *const *)a;
    const we_shader_combo_t *cb = *(const we_shader_combo_t *const *)b;
    return strcmp(ca->name, cb->name);
}

static bool build_shader_stage_payload(const we_material_pass_t *pass, const char *raw_body,
                                       uint8_t **out_bytes, size_t *out_sz,
                                       we_asset_writer_err_t *out_err)
{
    json_buf_t sb;
    jb_init(&sb);
    bool ok = true;

    if (pass->combo_count > 0u) {
        const we_shader_combo_t *sorted[WE_SHADER_MAX_COMBOS];
        uint32_t scnt = 0u;
        for (uint32_t c = 0u; c < pass->combo_count && c < WE_SHADER_MAX_COMBOS; ++c) {
            if (pass->combos[c].name[0] != '\0') {
                sorted[scnt++] = &pass->combos[c];
            }
        }
        if (scnt > 1u) {
            qsort(sorted, scnt, sizeof(sorted[0]), cmp_shader_combo_ptr);
        }
        for (uint32_t c = 0u; ok && c < scnt; ++c) {
            char def_line[160];
            int dn = snprintf(def_line, sizeof(def_line), "#ifndef %s\n#define %s %d\n#endif\n",
                              sorted[c]->name, sorted[c]->name, sorted[c]->value);
            if (dn > 0 && (size_t)dn < sizeof(def_line)) {
                ok = jb_raw(&sb, def_line, (size_t)dn);
            }
        }
    }

    if (ok && raw_body) {
        ok = jb_cstr(&sb, raw_body);
    }
    if (!ok) {
        set_err(out_err, sb.err != WE_WRITER_OK ? sb.err : WE_WRITER_ERR_OOM);
        jb_destroy(&sb);
        return false;
    }
    *out_bytes = (uint8_t *)sb.data;
    *out_sz = sb.len;
    return true;
}

static uint64_t fnv1a_extend(uint64_t h, const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool resolve_raw_shader_stage(const we_scene_t *scene, const we_material_pass_t *pass,
                                     bool is_frag, char **out_owned, we_asset_writer_err_t *out_err)
{
    *out_owned = NULL;
    const char *ext = is_frag ? "frag" : "vert";
    if (scene && scene->pkg) {
        char cands[3][256];
        snprintf(cands[0], sizeof(cands[0]), "shaders/%s.%s", pass->shader_name, ext);
        snprintf(cands[1], sizeof(cands[1]), "shaders/effects/%s.%s", pass->shader_name, ext);
        snprintf(cands[2], sizeof(cands[2]), "%s.%s", pass->shader_name, ext);
        for (int i = 0; i < 3; ++i) {
            size_t ssz = 0u;
            bool owned = false;
            const uint8_t *sdata = we_pkg_resolve(scene->pkg, cands[i], &ssz, &owned);
            if (sdata && ssz > 0u) {
                char *str = (char *)malloc(ssz + 1u);
                if (!str) {
                    if (owned) {
                        free((void *)(uintptr_t)sdata);
                    }
                    set_err(out_err, WE_WRITER_ERR_OOM);
                    return false;
                }
                memcpy(str, sdata, ssz);
                str[ssz] = '\0';
                if (owned) {
                    free((void *)(uintptr_t)sdata);
                }
                *out_owned = str;
                return true;
            }
            if (owned && sdata) {
                free((void *)(uintptr_t)sdata);
            }
        }
    }

    char builtin_key[160];
    snprintf(builtin_key, sizeof(builtin_key), "%s.%s", pass->shader_name, ext);
    const char *builtin = we_shader_get_builtin(builtin_key);
    if (builtin) {
        *out_owned = strdup(builtin);
        if (!*out_owned) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        return true;
    }

    const char *live = is_frag ? pass->prep.frag_glsl : pass->prep.vert_glsl;
    if (live && live[0] != '\0') {
        const char *body = strip_shader_preamble(live, is_frag);
        *out_owned = strdup(body);
        if (!*out_owned) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        return true;
    }

    return true;
}

static bool asset_bytes_equal(const owned_asset_t *entry, const uint8_t *bytes, size_t sz)
{
    if (!entry || entry->size != sz) {
        return false;
    }
    return sz == 0u || memcmp(entry->data, bytes, sz) == 0;
}

static bool export_pass_assets(asset_list_t *al, const we_scene_t *scene,
                               const we_material_pass_t *pass, export_ctx_t *ectx,
                               we_asset_writer_err_t *out_err)
{
    for (uint32_t s = 0u; s < WE_MAX_PASS_TEXTURES; ++s) {
        if (pass->tex_paths[s][0] != '\0' || pass->textures[s]) {
            const char *tname = pass->tex_paths[s][0] != '\0' ? pass->tex_paths[s] : "texture";
            if (!export_texture_asset(al, scene, tname, pass->textures[s], out_err)) {
                return false;
            }
        }
    }
    if (pass->shader_name[0] == '\0') {
        return true;
    }

    char *raw_stages[2] = {NULL, NULL};
    uint8_t *payloads[2] = {NULL, NULL};
    size_t payload_sz[2] = {0u, 0u};
    bool ok = true;

    for (int e = 0; ok && e < 2; ++e) {
        ok = resolve_raw_shader_stage(scene, pass, e == 1, &raw_stages[e], out_err);
        if (ok && raw_stages[e] && scene && scene->pkg) {
            ok = export_shader_includes(al, scene->pkg, raw_stages[e], 0, out_err);
        }
        if (ok && raw_stages[e]) {
            ok = build_shader_stage_payload(pass, raw_stages[e], &payloads[e], &payload_sz[e],
                                            out_err);
        }
    }
    free(raw_stages[0]);
    free(raw_stages[1]);

    if (!ok || (!payloads[0] && !payloads[1])) {
        free(payloads[0]);
        free(payloads[1]);
        return ok;
    }

    uint64_t hash = 14695981039346656037ULL;
    for (int e = 0; e < 2; ++e) {
        if (payloads[e]) {
            hash = fnv1a_extend(hash, payloads[e], payload_sz[e]);
        }
    }

    char chosen_name[128];
    snprintf(chosen_name, sizeof(chosen_name), "%s", pass->shader_name);
    for (uint32_t attempt = 0u;; ++attempt) {
        if (attempt == 1u) {
            snprintf(chosen_name, sizeof(chosen_name), "%.100s__v_%08x", pass->shader_name,
                     (unsigned int)(hash & 0xFFFFFFFFu));
        } else if (attempt > 1u) {
            snprintf(chosen_name, sizeof(chosen_name), "%.92s__v_%08x_%u", pass->shader_name,
                     (unsigned int)(hash & 0xFFFFFFFFu), (unsigned int)(attempt - 1u));
        }

        char vnorm[256];
        char fnorm[256];
        char vraw[256];
        char fraw[256];
        int vn = snprintf(vraw, sizeof(vraw), "shaders/%s.vert", chosen_name);
        int fn = snprintf(fraw, sizeof(fraw), "shaders/%s.frag", chosen_name);
        if (vn < 0 || (size_t)vn >= sizeof(vraw) || fn < 0 || (size_t)fn >= sizeof(fraw) ||
            !we_normalize_rel_path(vraw, vnorm, sizeof(vnorm), NULL) ||
            !we_normalize_rel_path(fraw, fnorm, sizeof(fnorm), NULL)) {
            free(payloads[0]);
            free(payloads[1]);
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }

        const owned_asset_t *ev = asset_list_find(al, vnorm);
        const owned_asset_t *ef = asset_list_find(al, fnorm);
        bool match_v = (!payloads[0] || !ev || asset_bytes_equal(ev, payloads[0], payload_sz[0]));
        bool match_f = (!payloads[1] || !ef || asset_bytes_equal(ef, payloads[1], payload_sz[1]));
        if (match_v && match_f) {
            if (payloads[0] && !ev &&
                !asset_list_add_owned(al, vnorm, payloads[0], payload_sz[0], out_err)) {
                free(payloads[1]);
                return false;
            } else if (payloads[0] && ev) {
                free(payloads[0]);
            }
            if (payloads[1] && !ef &&
                !asset_list_add_owned(al, fnorm, payloads[1], payload_sz[1], out_err)) {
                return false;
            } else if (payloads[1] && ef) {
                free(payloads[1]);
            }
            if (attempt > 0u && !export_ctx_add_shader_override(ectx, pass, chosen_name, out_err)) {
                return false;
            }
            return true;
        }
    }
}

static bool serialize_single_pass_material(const we_scene_t *scene,
                                           const we_scene_object_t *script_obj,
                                           const we_material_pass_t *pass,
                                           const char *self_mat_path, const export_ctx_t *ectx,
                                           char **out_json, size_t *out_len,
                                           we_asset_writer_err_t *out_err)
{
    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    if (self_mat_path && self_mat_path[0] != '\0') {
        ok = ok && jb_cstr(&jb, "\"material\":") && jb_str(&jb, self_mat_path) && jb_ch(&jb, ',');
    }
    ok = ok && jb_cstr(&jb, "\"passes\":[") &&
         serialize_pass_body(&jb, scene, script_obj, pass, false, NULL, ectx) && jb_cstr(&jb, "]}");
    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static bool serialize_effect_json(const we_scene_t *scene, const we_effect_t *eff,
                                  const export_ctx_t *ectx, char **out_json, size_t *out_len,
                                  we_asset_writer_err_t *out_err)
{
    if (!eff || (eff->pass_count > 0u && !eff->passes)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    bool first = true;
    if (eff->replacement_key[0] != '\0') {
        ok = ok && jb_key(&jb, &first, "replacementkey") && jb_str(&jb, eff->replacement_key);
    }
    if (eff->fbo_count > 0u) {
        ok = ok && jb_key(&jb, &first, "fbos") && jb_ch(&jb, '[');
        for (uint32_t f = 0u; ok && f < eff->fbo_count && f < WE_MAX_EFFECT_FBOS; ++f) {
            if (f > 0u) {
                ok = ok && jb_ch(&jb, ',');
            }
            ok = ok && jb_cstr(&jb, "{\"name\":") && jb_str(&jb, eff->fbos[f].name) &&
                 jb_cstr(&jb, ",\"scale\":") &&
                 jb_i64(&jb, eff->fbos[f].scale ? eff->fbos[f].scale : 1u) && jb_ch(&jb, '}');
        }
        ok = ok && jb_ch(&jb, ']');
    }
    ok = ok && jb_key(&jb, &first, "passes") && jb_ch(&jb, '[');
    for (uint32_t p = 0u; ok && p < eff->pass_count; ++p) {
        if (p > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && serialize_pass_body(&jb, scene, NULL, &eff->passes[p], true, NULL, ectx);
    }
    ok = ok && jb_cstr(&jb, "]}");
    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static const char *particle_renderer_name(we_renderer_type_t rt)
{
    switch (rt) {
    case WE_RENDERER_SPRITETRAIL:
        return "spritetrail";
    case WE_RENDERER_ROPETRAIL:
        return "ropetrail";
    case WE_RENDERER_SPRITE:
    default:
        return "sprite";
    }
}

static const char *particle_emitter_name(we_emitter_type_t et)
{
    switch (et) {
    case WE_EMITTER_SPHERERANDOM:
        return "sphererandom";
    case WE_EMITTER_POINT:
        return "point";
    case WE_EMITTER_BOXRANDOM:
    default:
        return "boxrandom";
    }
}

static bool serialize_particle_json(const we_particle_sys_t *ps, const char *mat_rel,
                                    char **out_json, size_t *out_len,
                                    we_asset_writer_err_t *out_err)
{
    if (!ps) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    bool first = true;

    ok = ok && jb_key(&jb, &first, "maxcount") && jb_i64(&jb, (long long)ps->max_count);
    ok = ok && jb_key(&jb, &first, "starttime") && jb_f32(&jb, ps->start_time);
    int pflags = (ps->world_space ? 1 : 0) | (ps->perspective ? 4 : 0);
    ok = ok && jb_key(&jb, &first, "flags") && jb_i64(&jb, pflags);
    const char *mpath = (mat_rel && mat_rel[0] != '\0') ? mat_rel : ps->material_path;
    if (mpath && mpath[0] != '\0') {
        ok = ok && jb_key(&jb, &first, "material") && jb_str(&jb, mpath);
    }

    ok = ok && jb_key(&jb, &first, "renderer") && jb_ch(&jb, '[') && jb_ch(&jb, '{');
    bool rf = true;
    ok = ok && jb_key(&jb, &rf, "name") && jb_str(&jb, particle_renderer_name(ps->renderer_type));
    ok = ok && jb_key(&jb, &rf, "length") && jb_f32(&jb, ps->trail_length);
    ok = ok && jb_key(&jb, &rf, "minlength") && jb_f32(&jb, ps->trail_min_length);
    ok = ok && jb_key(&jb, &rf, "maxlength") && jb_f32(&jb, ps->trail_max_length);
    ok = ok && jb_key(&jb, &rf, "subdivision") && jb_i64(&jb, (long long)ps->subdivision);
    ok = ok && jb_cstr(&jb, "}]");

    if (ps->control_point_count > 0u) {
        ok = ok && jb_key(&jb, &first, "controlpoint") && jb_ch(&jb, '[');
        uint32_t ccnt = (ps->control_point_count < WE_PARTICLE_MAX_CONTROL_PTS)
                            ? ps->control_point_count
                            : WE_PARTICLE_MAX_CONTROL_PTS;
        for (uint32_t c = 0u; ok && c < ccnt; ++c) {
            const we_particle_cp_t *cp = &ps->control_points[c];
            bool cf = true;
            if (c > 0u) {
                ok = ok && jb_ch(&jb, ',');
            }
            int cp_flags = (cp->lock_to_pointer ? 1 : 0) | (cp->world_space ? 2 : 0);
            ok = ok && jb_ch(&jb, '{') && jb_key(&jb, &cf, "id") && jb_i64(&jb, cp->id) &&
                 jb_key(&jb, &cf, "offset") && jb_vec_str(&jb, cp->offset, 3u) &&
                 jb_key(&jb, &cf, "flags") && jb_i64(&jb, cp_flags) && jb_ch(&jb, '}');
        }
        ok = ok && jb_ch(&jb, ']');
    }

    ok = ok && jb_key(&jb, &first, "emitter") && jb_ch(&jb, '[');
    uint32_t ecnt = (ps->emitter_count < WE_PARTICLE_MAX_EMITTERS) ? ps->emitter_count
                                                                   : WE_PARTICLE_MAX_EMITTERS;
    for (uint32_t e = 0u; ok && e < ecnt; ++e) {
        const we_particle_emitter_t *em = &ps->emitters[e];
        bool ef = true;
        if (e > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && jb_ch(&jb, '{') && jb_key(&jb, &ef, "name") &&
             jb_str(&jb, particle_emitter_name(em->type)) && jb_key(&jb, &ef, "distancemin") &&
             jb_vec_str(&jb, em->distance_min, 3u) && jb_key(&jb, &ef, "distancemax") &&
             jb_vec_str(&jb, em->distance_max, 3u) && jb_key(&jb, &ef, "directions") &&
             jb_vec_str(&jb, em->directions, 3u) && jb_key(&jb, &ef, "origin") &&
             jb_vec_str(&jb, em->origin, 3u) && jb_key(&jb, &ef, "speedmin") &&
             jb_f32(&jb, em->speed_min) && jb_key(&jb, &ef, "speedmax") &&
             jb_f32(&jb, em->speed_max) && jb_key(&jb, &ef, "rate") && jb_f32(&jb, em->rate) &&
             jb_key(&jb, &ef, "instantaneous") && jb_i64(&jb, (long long)em->instantaneous) &&
             jb_key(&jb, &ef, "controlpoint") && jb_i64(&jb, em->control_point) && jb_ch(&jb, '}');
    }
    ok = ok && jb_ch(&jb, ']');

    ok = ok && jb_key(&jb, &first, "initializer") && jb_ch(&jb, '[');
    uint32_t icnt = (ps->initializer_count < WE_PARTICLE_MAX_INITIALIZERS)
                        ? ps->initializer_count
                        : WE_PARTICLE_MAX_INITIALIZERS;
    for (uint32_t i = 0u; ok && i < icnt; ++i) {
        const we_particle_initializer_t *iz = &ps->initializers[i];
        bool izf = true;
        if (i > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && jb_ch(&jb, '{');
        switch (iz->type) {
        case WE_INIT_LIFETIME_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "lifetimerandom") &&
                 jb_key(&jb, &izf, "min") && jb_f32(&jb, iz->min_val[0]) &&
                 jb_key(&jb, &izf, "max") && jb_f32(&jb, iz->max_val[0]);
            break;
        case WE_INIT_SIZE_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "sizerandom") &&
                 jb_key(&jb, &izf, "min") && jb_f32(&jb, iz->min_val[0]) &&
                 jb_key(&jb, &izf, "max") && jb_f32(&jb, iz->max_val[0]);
            break;
        case WE_INIT_ALPHA_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "alpharandom") &&
                 jb_key(&jb, &izf, "min") && jb_f32(&jb, iz->min_val[0]) &&
                 jb_key(&jb, &izf, "max") && jb_f32(&jb, iz->max_val[0]);
            break;
        case WE_INIT_COLOR_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "colorrandom") &&
                 jb_key(&jb, &izf, "min") && jb_vec_str(&jb, iz->min_val, 3u) &&
                 jb_key(&jb, &izf, "max") && jb_vec_str(&jb, iz->max_val, 3u);
            break;
        case WE_INIT_VELOCITY_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "velocityrandom") &&
                 jb_key(&jb, &izf, "min") && jb_vec_str(&jb, iz->min_val, 3u) &&
                 jb_key(&jb, &izf, "max") && jb_vec_str(&jb, iz->max_val, 3u);
            break;
        case WE_INIT_ROTATION_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "rotationrandom") &&
                 jb_key(&jb, &izf, "min") && jb_vec_str(&jb, iz->min_val, 3u) &&
                 jb_key(&jb, &izf, "max") && jb_vec_str(&jb, iz->max_val, 3u);
            break;
        case WE_INIT_ANGULAR_VELOCITY_RANDOM:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "angularvelocityrandom") &&
                 jb_key(&jb, &izf, "min") && jb_vec_str(&jb, iz->min_val, 3u) &&
                 jb_key(&jb, &izf, "max") && jb_vec_str(&jb, iz->max_val, 3u);
            break;
        case WE_INIT_TURBULENT_VELOCITY_RANDOM:
        default:
            ok = ok && jb_key(&jb, &izf, "name") && jb_str(&jb, "turbulentvelocityrandom") &&
                 jb_key(&jb, &izf, "speedmin") && jb_f32(&jb, iz->min_val[0]) &&
                 jb_key(&jb, &izf, "speedmax") && jb_f32(&jb, iz->max_val[0]) &&
                 jb_key(&jb, &izf, "scale") && jb_f32(&jb, iz->scale) &&
                 jb_key(&jb, &izf, "offset") && jb_f32(&jb, iz->offset) &&
                 jb_key(&jb, &izf, "forward") && jb_vec_str(&jb, iz->forward, 3u);
            break;
        }
        ok = ok && jb_key(&jb, &izf, "exponent") && jb_f32(&jb, iz->exponent) && jb_ch(&jb, '}');
    }
    ok = ok && jb_ch(&jb, ']');

    ok = ok && jb_key(&jb, &first, "operator") && jb_ch(&jb, '[');
    uint32_t ocnt = (ps->operator_count < WE_PARTICLE_MAX_OPERATORS) ? ps->operator_count
                                                                     : WE_PARTICLE_MAX_OPERATORS;
    for (uint32_t o = 0u; ok && o < ocnt; ++o) {
        const we_particle_operator_t *op = &ps->operators[o];
        bool opf = true;
        if (o > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && jb_ch(&jb, '{');
        switch (op->type) {
        case WE_OP_MOVEMENT:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "movement") &&
                 jb_key(&jb, &opf, "gravity") && jb_vec_str(&jb, op->gravity, 3u) &&
                 jb_key(&jb, &opf, "drag") && jb_f32(&jb, op->drag);
            break;
        case WE_OP_ANGULAR_MOVEMENT:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "angularmovement") &&
                 jb_key(&jb, &opf, "drag") && jb_f32(&jb, op->drag) && jb_key(&jb, &opf, "force") &&
                 jb_vec_str(&jb, op->gravity, 3u);
            break;
        case WE_OP_ALPHA_FADE:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "alphafade") &&
                 jb_key(&jb, &opf, "fadeintime") && jb_f32(&jb, op->fade_in_time) &&
                 jb_key(&jb, &opf, "fadeouttime") && jb_f32(&jb, op->fade_out_time);
            break;
        case WE_OP_SIZE_CHANGE:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "sizechange") &&
                 jb_key(&jb, &opf, "starttime") && jb_f32(&jb, op->start_time) &&
                 jb_key(&jb, &opf, "endtime") && jb_f32(&jb, op->end_time) &&
                 jb_key(&jb, &opf, "startvalue") && jb_f32(&jb, op->start_val[0]) &&
                 jb_key(&jb, &opf, "endvalue") && jb_f32(&jb, op->end_val[0]);
            break;
        case WE_OP_COLOR_CHANGE:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "colorchange") &&
                 jb_key(&jb, &opf, "starttime") && jb_f32(&jb, op->start_time) &&
                 jb_key(&jb, &opf, "endtime") && jb_f32(&jb, op->end_time) &&
                 jb_key(&jb, &opf, "startvalue") && jb_vec_str(&jb, op->start_val, 3u) &&
                 jb_key(&jb, &opf, "endvalue") && jb_vec_str(&jb, op->end_val, 3u);
            break;
        case WE_OP_OSCILLATE_ALPHA:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "oscillatealpha") &&
                 jb_key(&jb, &opf, "frequencymin") && jb_f32(&jb, op->freq_min) &&
                 jb_key(&jb, &opf, "frequencymax") && jb_f32(&jb, op->freq_max) &&
                 jb_key(&jb, &opf, "scalemin") && jb_f32(&jb, op->scale_min) &&
                 jb_key(&jb, &opf, "scalemax") && jb_f32(&jb, op->scale_max);
            break;
        case WE_OP_OSCILLATE_SIZE:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "oscillatesize") &&
                 jb_key(&jb, &opf, "frequencymin") && jb_f32(&jb, op->freq_min) &&
                 jb_key(&jb, &opf, "frequencymax") && jb_f32(&jb, op->freq_max) &&
                 jb_key(&jb, &opf, "scalemin") && jb_f32(&jb, op->scale_min) &&
                 jb_key(&jb, &opf, "scalemax") && jb_f32(&jb, op->scale_max);
            break;
        case WE_OP_OSCILLATE_POSITION:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "oscillateposition") &&
                 jb_key(&jb, &opf, "frequencymin") && jb_f32(&jb, op->freq_min) &&
                 jb_key(&jb, &opf, "frequencymax") && jb_f32(&jb, op->freq_max) &&
                 jb_key(&jb, &opf, "scalemin") && jb_f32(&jb, op->scale_min) &&
                 jb_key(&jb, &opf, "scalemax") && jb_f32(&jb, op->scale_max) &&
                 jb_key(&jb, &opf, "mask") && jb_vec_str(&jb, op->mask, 3u);
            break;
        case WE_OP_VORTEX:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "vortex") &&
                 jb_key(&jb, &opf, "controlpoint") && jb_i64(&jb, op->control_point) &&
                 jb_key(&jb, &opf, "axis") && jb_vec_str(&jb, op->axis, 3u) &&
                 jb_key(&jb, &opf, "offset") && jb_vec_str(&jb, op->offset, 3u) &&
                 jb_key(&jb, &opf, "distanceinner") && jb_f32(&jb, op->distance_inner) &&
                 jb_key(&jb, &opf, "distanceouter") && jb_f32(&jb, op->distance_outer) &&
                 jb_key(&jb, &opf, "speedinner") && jb_f32(&jb, op->speed_inner) &&
                 jb_key(&jb, &opf, "speedouter") && jb_f32(&jb, op->speed_outer) &&
                 jb_key(&jb, &opf, "centerforce") && jb_f32(&jb, op->center_force);
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "controlpointattract") &&
                 jb_key(&jb, &opf, "controlpoint") && jb_i64(&jb, op->control_point) &&
                 jb_key(&jb, &opf, "offset") && jb_vec_str(&jb, op->offset, 3u) &&
                 jb_key(&jb, &opf, "distanceinner") && jb_f32(&jb, op->distance_inner) &&
                 jb_key(&jb, &opf, "distanceouter") && jb_f32(&jb, op->distance_outer) &&
                 jb_key(&jb, &opf, "scale") && jb_f32(&jb, op->center_force);
            break;
        case WE_OP_TURBULENCE:
        default:
            ok = ok && jb_key(&jb, &opf, "name") && jb_str(&jb, "turbulence") &&
                 jb_key(&jb, &opf, "scale") && jb_f32(&jb, op->turb_scale) &&
                 jb_key(&jb, &opf, "speed") && jb_f32(&jb, op->turb_speed) &&
                 jb_key(&jb, &opf, "timescale") && jb_f32(&jb, op->turb_timescale) &&
                 jb_key(&jb, &opf, "mask") && jb_vec_str(&jb, op->mask, 3u);
            break;
        }
        ok = ok && jb_ch(&jb, '}');
    }
    ok = ok && jb_cstr(&jb, "]}");
    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static inline void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static bool encode_wav_pcm16_stereo(const float *pcm, size_t frames, uint8_t **out_buf,
                                    size_t *out_size, we_asset_writer_err_t *out_err)
{
    if (frames > (UINT32_MAX - 44u) / 4u) {
        set_err(out_err, WE_WRITER_ERR_OVERFLOW);
        return false;
    }
    uint32_t data_bytes = (uint32_t)(frames * 4u);
    size_t total = 44u + (size_t)data_bytes;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    memcpy(buf, "RIFF", 4u);
    put_u32_le(buf + 4u, 36u + data_bytes);
    memcpy(buf + 8u, "WAVEfmt ", 8u);
    put_u32_le(buf + 16u, 16u);
    put_u16_le(buf + 20u, 1u);
    put_u16_le(buf + 22u, 2u);
    put_u32_le(buf + 24u, 48000u);
    put_u32_le(buf + 28u, 48000u * 4u);
    put_u16_le(buf + 32u, 4u);
    put_u16_le(buf + 34u, 16u);
    memcpy(buf + 36u, "data", 4u);
    put_u32_le(buf + 40u, data_bytes);

    uint8_t *dst = buf + 44u;
    size_t samples = frames * 2u;
    for (size_t i = 0u; i < samples; ++i) {
        float s = pcm[i];
        if (!isfinite(s)) {
            s = 0.0f;
        }
        if (s > 1.0f) {
            s = 1.0f;
        } else if (s < -1.0f) {
            s = -1.0f;
        }
        int32_t q = (int32_t)lrintf(s * 32767.0f);
        if (q > 32767) {
            q = 32767;
        } else if (q < -32768) {
            q = -32768;
        }
        put_u16_le(dst + i * 2u, (uint16_t)(int16_t)q);
    }

    *out_buf = buf;
    *out_size = total;
    return true;
}

static bool ensure_dir_fd_beneath(int root_dirfd, const char *rel_dir, int *out_dirfd,
                                  we_asset_writer_err_t *out_err)
{
    int cur = dup(root_dirfd);
    if (cur < 0) {
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (!rel_dir || rel_dir[0] == '\0') {
        *out_dirfd = cur;
        return true;
    }

    char copy[PATH_MAX];
    size_t dlen = strlen(rel_dir);
    if (dlen >= sizeof(copy)) {
        close(cur);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    memcpy(copy, rel_dir, dlen + 1u);

    char *p = copy;
    while (*p != '\0') {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
        }
        if (p[0] == '\0' || strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
            close(cur);
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        int next = openat(cur, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(cur, p, 0755) != 0 && errno != EEXIST) {
                close(cur);
                set_err(out_err, WE_WRITER_ERR_IO);
                return false;
            }
            next = openat(cur, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        close(cur);
        if (next < 0) {
            set_err(out_err, errno == ELOOP ? WE_WRITER_ERR_INVALID_PATH : WE_WRITER_ERR_IO);
            return false;
        }
        cur = next;
        if (!slash) {
            break;
        }
        p = slash + 1;
    }

    *out_dirfd = cur;
    return true;
}

static bool write_file_atomic_beneath(int root_dirfd, const char *rel_path, const void *data,
                                      size_t size, we_asset_writer_err_t *out_err)
{
    if (root_dirfd < 0 || !rel_path || rel_path[0] == '\0') {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    char dir_part[PATH_MAX];
    dir_part[0] = '\0';
    const char *leaf = rel_path;
    const char *slash = strrchr(rel_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - rel_path);
        if (dlen == 0u || dlen >= sizeof(dir_part)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        memcpy(dir_part, rel_path, dlen);
        dir_part[dlen] = '\0';
        leaf = slash + 1;
    }
    if (leaf[0] == '\0' || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    int parent_fd = -1;
    if (!ensure_dir_fd_beneath(root_dirfd, dir_part, &parent_fd, out_err)) {
        return false;
    }

    uint64_t seq = (uint64_t)atomic_fetch_add(&g_atomic_tmp_seq, 1u) + 1u;
    char tmp_leaf[96];
    snprintf(tmp_leaf, sizeof(tmp_leaf), ".wwal_tmp_%ld_%llu", (long)getpid(),
             (unsigned long long)seq);

    char tmp_rel[PATH_MAX];
    int trn = dir_part[0] != '\0' ? snprintf(tmp_rel, sizeof(tmp_rel), "%s/%s", dir_part, tmp_leaf)
                                  : snprintf(tmp_rel, sizeof(tmp_rel), "%s", tmp_leaf);
    if (trn < 0 || (size_t)trn >= sizeof(tmp_rel)) {
        close(parent_fd);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    int fd =
        wwal_openat2_beneath(root_dirfd, tmp_rel, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) {
        int saved_errno = errno;
        close(parent_fd);
        set_err(out_err, (saved_errno == ELOOP || saved_errno == EXDEV || saved_errno == EINVAL)
                             ? WE_WRITER_ERR_INVALID_PATH
                             : WE_WRITER_ERR_IO);
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t off = 0u;
    while (off < size) {
        ssize_t w = write(fd, bytes + off, size - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            (void)unlinkat(parent_fd, tmp_leaf, 0);
            close(parent_fd);
            set_err(out_err, WE_WRITER_ERR_IO);
            return false;
        }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) {
        close(fd);
        (void)unlinkat(parent_fd, tmp_leaf, 0);
        close(parent_fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (close(fd) != 0) {
        (void)unlinkat(parent_fd, tmp_leaf, 0);
        close(parent_fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (renameat(parent_fd, tmp_leaf, parent_fd, leaf) != 0) {
        (void)unlinkat(parent_fd, tmp_leaf, 0);
        close(parent_fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (fsync(parent_fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        close(parent_fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    close(parent_fd);
    return true;
}

static bool copy_existing_file_beneath(const char *src_path, int root_dirfd, const char *dst_rel,
                                       we_asset_writer_err_t *out_err)
{
    int fd = open(src_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        set_err(out_err, errno == ELOOP ? WE_WRITER_ERR_INVALID_PATH : WE_WRITER_ERR_IO);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    if (st.st_size < 0) {
        close(fd);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    if (st.st_size > (off_t)(64 * 1024 * 1024)) {
        close(fd);
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = sz ? (uint8_t *)malloc(sz) : NULL;
    if (sz && !buf) {
        close(fd);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    size_t off = 0u;
    while (off < sz) {
        ssize_t r = read(fd, buf + off, sz - off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            set_err(out_err, WE_WRITER_ERR_IO);
            return false;
        }
        if (r == 0) {
            break;
        }
        off += (size_t)r;
    }
    close(fd);
    if (off != sz) {
        free(buf);
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }
    bool ok = write_file_atomic_beneath(root_dirfd, dst_rel, buf ? buf : (const uint8_t *)"", sz,
                                        out_err);
    free(buf);
    return ok;
}

static bool extract_preview_rgba(const we_scene_t *scene, uint8_t **out_rgba, uint32_t *out_w,
                                 uint32_t *out_h)
{
    for (uint32_t i = 0u; i < scene->object_count; ++i) {
        const we_scene_object_t *obj = &scene->objects[i];
        const we_tex_t *t = obj->base_pass.textures[0];
        if (!t || t->image_count == 0u || !t->images || t->images[0].mip_count == 0u ||
            !t->images[0].mips) {
            continue;
        }
        const we_tex_mip_t *m0 = &t->images[0].mips[0];
        if (m0->width == 0u || m0->height == 0u || m0->width > 4096u || m0->height > 4096u) {
            continue;
        }
        size_t npx = (size_t)m0->width * (size_t)m0->height;
        uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
        if (!rgba) {
            return false;
        }
        if (m0->rgba8) {
            if (m0->is_bgra_swiz) {
                for (size_t p = 0u; p < npx; ++p) {
                    rgba[p * 4u + 0u] = m0->rgba8[p * 4u + 2u];
                    rgba[p * 4u + 1u] = m0->rgba8[p * 4u + 1u];
                    rgba[p * 4u + 2u] = m0->rgba8[p * 4u + 0u];
                    rgba[p * 4u + 3u] = m0->rgba8[p * 4u + 3u];
                }
            } else {
                memcpy(rgba, m0->rgba8, npx * 4u);
            }
            *out_rgba = rgba;
            *out_w = m0->width;
            *out_h = m0->height;
            return true;
        }
        if (m0->data && t->format == WE_TEX_FMT_RGBA8888 && m0->data_size == npx * 4u) {
            memcpy(rgba, m0->data, npx * 4u);
            *out_rgba = rgba;
            *out_w = m0->width;
            *out_h = m0->height;
            return true;
        }
        if (m0->data && t->format == WE_TEX_FMT_RG88 && m0->data_size == npx * 2u) {
            for (size_t p = 0u; p < npx; ++p) {
                uint8_t r = m0->data[p * 2u + 0u];
                uint8_t g = m0->data[p * 2u + 1u];
                rgba[p * 4u + 0u] = r;
                rgba[p * 4u + 1u] = r;
                rgba[p * 4u + 2u] = r;
                rgba[p * 4u + 3u] = g;
            }
            *out_rgba = rgba;
            *out_w = m0->width;
            *out_h = m0->height;
            return true;
        }
        if (m0->data && t->format == WE_TEX_FMT_R8 && m0->data_size == npx) {
            for (size_t p = 0u; p < npx; ++p) {
                uint8_t r = m0->data[p];
                rgba[p * 4u + 0u] = r;
                rgba[p * 4u + 1u] = r;
                rgba[p * 4u + 2u] = r;
                rgba[p * 4u + 3u] = r;
            }
            *out_rgba = rgba;
            *out_w = m0->width;
            *out_h = m0->height;
            return true;
        }
        free(rgba);
    }

    uint32_t w = 64u, h = 64u;
    uint8_t *canvas = (uint8_t *)malloc((size_t)w * h * 4u);
    if (!canvas) {
        return false;
    }
    uint8_t cr = (uint8_t)fminf(fmaxf(scene->clear_color[0] * 255.0f, 0.0f), 255.0f);
    uint8_t cg = (uint8_t)fminf(fmaxf(scene->clear_color[1] * 255.0f, 0.0f), 255.0f);
    uint8_t cb = (uint8_t)fminf(fmaxf(scene->clear_color[2] * 255.0f, 0.0f), 255.0f);
    for (size_t p = 0u; p < (size_t)w * h; ++p) {
        canvas[p * 4u + 0u] = cr;
        canvas[p * 4u + 1u] = cg;
        canvas[p * 4u + 2u] = cb;
        canvas[p * 4u + 3u] = 255u;
    }
    *out_rgba = canvas;
    *out_w = w;
    *out_h = h;
    return true;
}

typedef size_t (*we_webp_encode_rgba_fn)(const uint8_t *, int, int, int, float, uint8_t **);
typedef void (*we_webp_free_fn)(void *);
static pthread_once_t g_libwebp_once = PTHREAD_ONCE_INIT;
static we_webp_encode_rgba_fn g_webp_encode_fn = NULL;
static we_webp_free_fn g_webp_free_fn = NULL;

static void init_libwebp_once(void)
{
    /* Keep libwebp off DT_NEEDED and never dlclose to avoid unload/TLS hazards. */
    void *h = dlopen("libwebp.so.7", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
    if (!h) {
        return;
    }
    void *sym_encode = dlsym(h, "WebPEncodeRGBA");
    void *sym_free = dlsym(h, "WebPFree");
    if (!sym_encode || !sym_free) {
        return;
    }
    memcpy(&g_webp_encode_fn, &sym_encode, sizeof(g_webp_encode_fn));
    memcpy(&g_webp_free_fn, &sym_free, sizeof(g_webp_free_fn));
}

static bool maybe_write_preview(const we_scene_t *scene, int root_dirfd, we_project_t *proj_mut,
                                we_asset_writer_err_t *out_err)
{
    we_asset_writer_err_t scratch_err = WE_WRITER_OK;
    char existing[PATH_MAX];
    if (scene->project.workshop_dir[0] != '\0' &&
        we_workshop_find_preview_file(scene->project.workshop_dir, existing, sizeof(existing))) {
        const char *base = strrchr(existing, '/');
        base = base ? (base + 1) : existing;
        if (base[0] != '\0' && strcmp(base, ".") != 0 && strcmp(base, "..") != 0 &&
            strlen(base) < sizeof(proj_mut->preview)) {
            if (copy_existing_file_beneath(existing, root_dirfd, base, &scratch_err)) {
                memcpy(proj_mut->preview, base, strlen(base) + 1u);
                return true;
            }
            if (scratch_err == WE_WRITER_ERR_LIMIT_EXCEEDED) {
                set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
                return false;
            }
        }
    }

    pthread_once(&g_libwebp_once, init_libwebp_once);
    if (!g_webp_encode_fn || !g_webp_free_fn) {
        return true;
    }

    uint8_t *rgba = NULL;
    uint32_t rw = 0u, rh = 0u;
    if (extract_preview_rgba(scene, &rgba, &rw, &rh) && rgba) {
        uint8_t *webp = NULL;
        size_t wsz = g_webp_encode_fn(rgba, (int)rw, (int)rh, (int)(rw * 4u), 80.0f, &webp);
        free(rgba);
        if (wsz > 0u && webp) {
            if (write_file_atomic_beneath(root_dirfd, "preview.webp", webp, wsz, &scratch_err)) {
                snprintf(proj_mut->preview, sizeof(proj_mut->preview), "preview.webp");
            }
            g_webp_free_fn(webp);
        }
    }
    return true;
}

static bool assign_generated_model_paths(uint32_t obj_idx, char *model_rel, size_t model_cap,
                                         char *mat_rel, size_t mat_cap, char **out_img_override,
                                         we_asset_writer_err_t *out_err)
{
    int mn = snprintf(model_rel, model_cap, "models/layer_%u.json", (unsigned int)obj_idx);
    int tn = snprintf(mat_rel, mat_cap, "materials/layer_%u.json", (unsigned int)obj_idx);
    if (mn < 0 || (size_t)mn >= model_cap || tn < 0 || (size_t)tn >= mat_cap) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    *out_img_override = strdup(model_rel);
    if (!*out_img_override) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    return true;
}

static bool export_object_material(asset_list_t *al, const we_scene_t *scene,
                                   const we_scene_object_t *obj, uint32_t obj_idx,
                                   export_ctx_t *ectx, char **out_img_override,
                                   we_asset_writer_err_t *out_err)
{
    if (!export_pass_assets(al, scene, &obj->base_pass, ectx, out_err)) {
        return false;
    }

    char mat_rel[320];
    char model_rel[320];
    bool write_model_wrapper = false;
    bool self_mat_in_material = false;

    if (obj->image_path[0] == '\0') {
        if (!assign_generated_model_paths(obj_idx, model_rel, sizeof(model_rel), mat_rel,
                                          sizeof(mat_rel), out_img_override, out_err)) {
            return false;
        }
        write_model_wrapper = true;
    } else if (!we_normalize_rel_path(obj->image_path, model_rel, sizeof(model_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    } else {
        size_t pkg_sz = 0u;
        bool pkg_owned = false;
        const uint8_t *pkg_mdl =
            scene->pkg ? we_pkg_resolve(scene->pkg, model_rel, &pkg_sz, &pkg_owned) : NULL;
        if (pkg_mdl) {
            we_jval_t *mroot = we_json_parse_root((const char *)pkg_mdl, pkg_sz);
            const char *existing_mat =
                mroot ? we_jval_str(we_jobj_get(mroot, "material"), NULL) : NULL;
            if (existing_mat && existing_mat[0] != '\0' &&
                we_normalize_rel_path(existing_mat, mat_rel, sizeof(mat_rel), NULL)) {
                bool ok = pkg_owned
                              ? asset_list_add_owned(al, model_rel, (uint8_t *)(uintptr_t)pkg_mdl,
                                                     pkg_sz, out_err)
                              : asset_list_add_copy(al, model_rel, pkg_mdl, pkg_sz, out_err);
                we_jval_free(mroot);
                if (!ok) {
                    return false;
                }
            } else {
                if (pkg_owned) {
                    free((void *)(uintptr_t)pkg_mdl);
                }
                we_jval_free(mroot);
                snprintf(mat_rel, sizeof(mat_rel), "%s", model_rel);
                self_mat_in_material = true;
            }
        } else if (strncasecmp(model_rel, "materials/", 10u) == 0 && has_json_ext(model_rel)) {
            snprintf(mat_rel, sizeof(mat_rel), "%s", model_rel);
            self_mat_in_material = true;
        } else if (has_json_ext(model_rel)) {
            int tn = snprintf(mat_rel, sizeof(mat_rel), "materials/layer_%u.json",
                              (unsigned int)obj_idx);
            if (tn < 0 || (size_t)tn >= sizeof(mat_rel)) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
            write_model_wrapper = true;
        } else {
            /* Never overwrite non-JSON image paths (.png, etc.) with model wrapper JSON. */
            if (!assign_generated_model_paths(obj_idx, model_rel, sizeof(model_rel), mat_rel,
                                              sizeof(mat_rel), out_img_override, out_err)) {
                return false;
            }
            write_model_wrapper = true;
        }
    }

    if (write_model_wrapper) {
        json_buf_t mb;
        jb_init(&mb);
        bool ok = jb_cstr(&mb, "{\"material\":") && jb_str(&mb, mat_rel) && jb_ch(&mb, '}');
        char *mjson = NULL;
        size_t mlen = 0u;
        ok = finish_jb(&mb, ok, &mjson, &mlen, out_err);
        if (!ok || !asset_list_add_owned(al, model_rel, (uint8_t *)mjson, mlen, out_err)) {
            return false;
        }
    }

    char *mat_json = NULL;
    size_t mat_len = 0u;
    if (!serialize_single_pass_material(scene, obj, &obj->base_pass,
                                        self_mat_in_material ? mat_rel : NULL, ectx, &mat_json,
                                        &mat_len, out_err)) {
        return false;
    }
    return asset_list_add_owned(al, mat_rel, (uint8_t *)mat_json, mat_len, out_err);
}

static bool export_object_effects_assets(asset_list_t *al, const we_scene_t *scene,
                                         const we_scene_object_t *obj, uint32_t obj_idx,
                                         export_ctx_t *ectx, we_asset_writer_err_t *out_err)
{
    if (obj->effect_count == 0u) {
        return true;
    }
    if (!obj->effects) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    for (uint32_t e = 0u; e < obj->effect_count; ++e) {
        const we_effect_t *eff = &obj->effects[e];
        if (eff->pass_count > 0u && !eff->passes) {
            set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
            return false;
        }
        for (uint32_t p = 0u; p < eff->pass_count; ++p) {
            if (!export_pass_assets(al, scene, &eff->passes[p], ectx, out_err)) {
                return false;
            }
        }
        char erel[320];
        if (eff->file_path[0] != '\0') {
            if (!we_normalize_rel_path(eff->file_path, erel, sizeof(erel), NULL)) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
        } else {
            int en = snprintf(erel, sizeof(erel), "effects/effect_%u_%u.json",
                              (unsigned int)obj_idx, (unsigned int)e);
            if (en < 0 || (size_t)en >= sizeof(erel)) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
        }
        if (!asset_list_has(al, erel)) {
            size_t esz = 0u;
            bool eown = false;
            const uint8_t *eraw = scene->pkg ? we_pkg_resolve(scene->pkg, erel, &esz, &eown) : NULL;
            if (eraw) {
                bool ok =
                    eown ? asset_list_add_owned(al, erel, (uint8_t *)(uintptr_t)eraw, esz, out_err)
                         : asset_list_add_copy(al, erel, eraw, esz, out_err);
                if (!ok) {
                    return false;
                }
            } else {
                char *ej = NULL;
                size_t ejl = 0u;
                if (!serialize_effect_json(scene, eff, ectx, &ej, &ejl, out_err) ||
                    !asset_list_add_owned(al, erel, (uint8_t *)ej, ejl, out_err)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool export_object_sound_assets(asset_list_t *al, const we_scene_object_t *obj,
                                       uint32_t obj_idx, we_asset_writer_err_t *out_err)
{
    if (!obj->is_sound_object) {
        return true;
    }
    for (uint32_t t = 0u; t < obj->sound_track_count && t < WE_MAX_SOUND_TRACKS; ++t) {
        if (!obj->sound_tracks_pcm[t] || obj->sound_tracks_frames[t] == 0u) {
            continue;
        }
        char spath[128];
        if (!build_sound_path(obj->id, obj_idx, t, spath, sizeof(spath))) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        uint8_t *wav = NULL;
        size_t wsz = 0u;
        if (!encode_wav_pcm16_stereo(obj->sound_tracks_pcm[t], obj->sound_tracks_frames[t], &wav,
                                     &wsz, out_err) ||
            !asset_list_add_owned(al, spath, wav, wsz, out_err)) {
            return false;
        }
    }
    return true;
}

static bool jb_f32_arr(json_buf_t *jb, const float *vals, size_t n)
{
    bool ok = jb_ch(jb, '[');
    for (size_t i = 0u; ok && i < n; ++i) {
        if (i > 0u) {
            ok = ok && jb_ch(jb, ',');
        }
        ok = ok && jb_f32(jb, vals[i]);
    }
    return ok && jb_ch(jb, ']');
}

static bool jb_f64(json_buf_t *jb, double v)
{
    if (!isfinite(v)) {
        jb->err = WE_WRITER_ERR_NON_FINITE;
        return false;
    }
    if (v == 0.0) {
        v = 0.0;
    }
    char buf[64];
    if (!we_json_format_double(v, buf, sizeof(buf))) {
        jb->err = WE_WRITER_ERR_OVERFLOW;
        return false;
    }
    return jb_cstr(jb, buf);
}

static bool jb_jval(json_buf_t *jb, const we_jval_t *v, int depth)
{
    if (depth > 32) {
        return false;
    }
    if (!v || v->type == JVAL_NULL) {
        return jb_cstr(jb, "null");
    }
    switch (v->type) {
    case JVAL_BOOL:
        return jb_cstr(jb, v->bval ? "true" : "false");
    case JVAL_NUM:
        return jb_f64(jb, v->num);
    case JVAL_STR:
        return jb_str(jb, v->str ? v->str : "");
    case JVAL_ARR: {
        bool ok = jb_ch(jb, '[');
        for (size_t i = 0u; ok && i < v->arr.count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            ok = ok && jb_jval(jb, v->arr.items[i], depth + 1);
        }
        return ok && jb_ch(jb, ']');
    }
    case JVAL_OBJ: {
        bool ok = jb_ch(jb, '{');
        for (size_t i = 0u; ok && i < v->obj.count; ++i) {
            if (i > 0u) {
                ok = ok && jb_ch(jb, ',');
            }
            ok = ok && jb_str(jb, v->obj.entries[i].key ? v->obj.entries[i].key : "") &&
                 jb_ch(jb, ':') && jb_jval(jb, v->obj.entries[i].val, depth + 1);
        }
        return ok && jb_ch(jb, '}');
    }
    default:
        return false;
    }
}

static we_jval_t *load_existing_sidecar_json(const we_scene_t *scene, const char *sidecar_rel)
{
    if (!scene || scene->project.workshop_dir[0] == '\0' || !sidecar_rel ||
        sidecar_rel[0] == '\0' || strchr(sidecar_rel, '/') != NULL) {
        return NULL;
    }
    int dfd = open(scene->project.workshop_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        return NULL;
    }
    int fd = wwal_openat2_beneath(dfd, sidecar_rel, O_RDONLY, 0);
    close(dfd);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > (off_t)(4u * 1024u * 1024u)) {
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz + 1u);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t got = 0u;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    we_jval_t *root = we_json_parse_root(buf, got);
    free(buf);
    return root;
}

static const char *tangent_mode_str_from_jval(const we_jval_t *tm)
{
    if (!tm) {
        return NULL;
    }
    if (tm->type == JVAL_STR && tm->str && tm->str[0] != '\0') {
        return tm->str;
    }
    if (tm->type == JVAL_NUM && isfinite(tm->num)) {
        int code = (int)tm->num;
        switch (code) {
        case 1:
            return "Aligned";
        case 2:
            return "Vector";
        case 3:
            return "Stepped";
        case 0:
        default:
            return "Auto";
        }
    }
    return NULL;
}

static const char *find_existing_tangent_mode(const we_jval_t *prev_root, uint32_t anim_idx,
                                              int32_t bone_idx, uint32_t kf_idx)
{
    const we_jval_t *curves = prev_root ? we_jobj_get(prev_root, "animation_curves") : NULL;
    if (!curves || curves->type != JVAL_ARR || anim_idx >= curves->arr.count) {
        return "Auto";
    }
    const we_jval_t *cobj = curves->arr.items[anim_idx];
    if (!cobj || cobj->type != JVAL_OBJ) {
        return "Auto";
    }

    const we_jval_t *tracks = we_jobj_get(cobj, "tracks");
    if (tracks && tracks->type == JVAL_ARR) {
        for (size_t t = 0u; t < tracks->arr.count; ++t) {
            const we_jval_t *tobj = tracks->arr.items[t];
            const we_jval_t *bi = tobj ? we_jobj_get(tobj, "bone_index") : NULL;
            if (!bi || bi->type != JVAL_NUM || (int32_t)bi->num != bone_idx) {
                continue;
            }
            const we_jval_t *kfs = we_jobj_get(tobj, "keyframes");
            if (kfs && kfs->type == JVAL_ARR && kf_idx < kfs->arr.count) {
                const char *s =
                    tangent_mode_str_from_jval(we_jobj_get(kfs->arr.items[kf_idx], "tangentMode"));
                if (s) {
                    return s;
                }
            }
        }
    }

    const we_jval_t *channels = we_jobj_get(cobj, "channels");
    if (channels && channels->type == JVAL_ARR) {
        for (size_t ch = 0u; ch < channels->arr.count; ++ch) {
            const we_jval_t *chobj = channels->arr.items[ch];
            const we_jval_t *bi = chobj ? we_jobj_get(chobj, "bone_index") : NULL;
            if (!bi || bi->type != JVAL_NUM || (int32_t)bi->num != bone_idx) {
                continue;
            }
            const we_jval_t *keys = we_jobj_get(chobj, "keys");
            if (keys && keys->type == JVAL_ARR && kf_idx < keys->arr.count) {
                const char *s =
                    tangent_mode_str_from_jval(we_jobj_get(keys->arr.items[kf_idx], "tangentMode"));
                if (s) {
                    return s;
                }
            }
        }
    }
    return "Auto";
}

static bool serialize_puppet_sidecar(const we_scene_t *scene, const we_scene_object_t *obj,
                                     const char *sidecar_rel, const char *mdl_rel,
                                     const char *mat_rel, char **out_json, size_t *out_len,
                                     we_asset_writer_err_t *out_err)
{
    const we_puppet_t *p = obj->puppet;
    const we_rig_pose_ctx_t *rig = (const we_rig_pose_ctx_t *)p->rig_ctx;
    we_jval_t *prev_root = load_existing_sidecar_json(scene, sidecar_rel);
    const we_jval_t *prev_contour = prev_root ? we_jobj_get(prev_root, "contour_polygon") : NULL;
    const we_jval_t *prev_pins = prev_root ? we_jobj_get(prev_root, "pins") : NULL;
    const we_jval_t *prev_ik = prev_root ? we_jobj_get(prev_root, "ik_chains") : NULL;
    const we_jval_t *prev_aim = prev_root ? we_jobj_get(prev_root, "aim_constraints") : NULL;

    json_buf_t jb;
    jb_init(&jb);
    bool first = true;
    bool ok = jb_ch(&jb, '{');

    ok = ok && jb_key(&jb, &first, "version") && jb_i64(&jb, 1);
    ok = ok && jb_key(&jb, &first, "object_id") && jb_i64(&jb, obj->id);
    ok = ok && jb_key(&jb, &first, "object_name") && jb_str(&jb, obj->name[0] ? obj->name : "");
    ok = ok && jb_key(&jb, &first, "puppet") && jb_str(&jb, mdl_rel);
    ok = ok && jb_key(&jb, &first, "material") && jb_str(&jb, mat_rel);
    ok = ok && jb_key(&jb, &first, "contour_polygon") &&
         ((prev_contour && prev_contour->type == JVAL_ARR) ? jb_jval(&jb, prev_contour, 0)
                                                           : jb_cstr(&jb, "[]"));
    ok = ok && jb_key(&jb, &first, "pins") &&
         ((prev_pins && prev_pins->type == JVAL_ARR) ? jb_jval(&jb, prev_pins, 0)
                                                     : jb_cstr(&jb, "[]"));

    ok = ok && jb_key(&jb, &first, "ik_chains");
    if (rig && rig->ik_chain_count > 0u) {
        ok = ok && jb_ch(&jb, '[');
        uint32_t cnt = (rig->ik_chain_count < WE_RIG_MAX_IK_CHAINS) ? rig->ik_chain_count
                                                                    : WE_RIG_MAX_IK_CHAINS;
        for (uint32_t c = 0u; ok && c < cnt; ++c) {
            const we_ik_chain_t *ch = &rig->ik_chains[c];
            bool cfirst = true;
            if (c > 0u) {
                ok = ok && jb_ch(&jb, ',');
            }
            ok = ok && jb_ch(&jb, '{');
            ok = ok && jb_key(&jb, &cfirst, "root_bone") && jb_i64(&jb, ch->root_bone);
            ok = ok && jb_key(&jb, &cfirst, "mid_bone") && jb_i64(&jb, ch->mid_bone);
            ok = ok && jb_key(&jb, &cfirst, "end_bone") && jb_i64(&jb, ch->end_bone);
            ok = ok && jb_key(&jb, &cfirst, "bend_sign") && jb_i64(&jb, ch->bend_sign);
            ok = ok && jb_key(&jb, &cfirst, "enabled") &&
                 jb_cstr(&jb, ch->enabled ? "true" : "false");
            ok =
                ok && jb_key(&jb, &cfirst, "target_world") && jb_f32_arr(&jb, ch->target_world, 3u);
            ok = ok && jb_key(&jb, &cfirst, "pole_world") && jb_f32_arr(&jb, ch->pole_world, 3u);
            ok = ok && jb_key(&jb, &cfirst, "soft_zone_ratio") && jb_f32(&jb, ch->soft_zone_ratio);
            ok = ok && jb_key(&jb, &cfirst, "max_stretch_ratio") &&
                 jb_f32(&jb, ch->max_stretch_ratio);
            ok = ok && jb_key(&jb, &cfirst, "root_angle_min_rad") &&
                 jb_f32(&jb, ch->root_angle_min_rad);
            ok = ok && jb_key(&jb, &cfirst, "root_angle_max_rad") &&
                 jb_f32(&jb, ch->root_angle_max_rad);
            ok = ok && jb_key(&jb, &cfirst, "mid_angle_min_rad") &&
                 jb_f32(&jb, ch->mid_angle_min_rad);
            ok = ok && jb_key(&jb, &cfirst, "mid_angle_max_rad") &&
                 jb_f32(&jb, ch->mid_angle_max_rad);
            ok = ok && jb_key(&jb, &cfirst, "blend_weight") && jb_f32(&jb, ch->blend_weight);
            ok = ok && jb_ch(&jb, '}');
        }
        ok = ok && jb_ch(&jb, ']');
    } else if (prev_ik && prev_ik->type == JVAL_ARR && prev_ik->arr.count > 0u) {
        ok = ok && jb_jval(&jb, prev_ik, 0);
    } else {
        ok = ok && jb_cstr(&jb, "[]");
    }

    ok = ok && jb_key(&jb, &first, "aim_constraints");
    if (rig && rig->aim_count > 0u) {
        ok = ok && jb_ch(&jb, '[');
        uint32_t cnt = (rig->aim_count < WE_RIG_MAX_AIM_CONSTRAINTS) ? rig->aim_count
                                                                     : WE_RIG_MAX_AIM_CONSTRAINTS;
        for (uint32_t a = 0u; ok && a < cnt; ++a) {
            const we_aim_constraint_t *aim = &rig->aim_constraints[a];
            bool afirst = true;
            if (a > 0u) {
                ok = ok && jb_ch(&jb, ',');
            }
            ok = ok && jb_ch(&jb, '{');
            ok = ok && jb_key(&jb, &afirst, "bone_index") && jb_i64(&jb, aim->bone_index);
            ok = ok && jb_key(&jb, &afirst, "enabled") &&
                 jb_cstr(&jb, aim->enabled ? "true" : "false");
            ok = ok && jb_key(&jb, &afirst, "aim_axis_local") &&
                 jb_f32_arr(&jb, aim->aim_axis_local, 3u);
            ok = ok && jb_key(&jb, &afirst, "target_world") &&
                 jb_f32_arr(&jb, aim->target_world, 3u);
            ok = ok && jb_key(&jb, &afirst, "max_cone_rad") && jb_f32(&jb, aim->max_cone_rad);
            ok = ok && jb_key(&jb, &afirst, "blend_weight") && jb_f32(&jb, aim->blend_weight);
            ok = ok && jb_ch(&jb, '}');
        }
        ok = ok && jb_ch(&jb, ']');
    } else if (prev_aim && prev_aim->type == JVAL_ARR && prev_aim->arr.count > 0u) {
        ok = ok && jb_jval(&jb, prev_aim, 0);
    } else {
        ok = ok && jb_cstr(&jb, "[]");
    }

    ok = ok && jb_key(&jb, &first, "bone_drive_modes") && jb_ch(&jb, '[');
    for (uint32_t i = 0u; ok && i < p->bone_count; ++i) {
        const we_mdl_bone_t *b = &p->bones[i];
        bool bfirst = true;
        int64_t parent_id = (b->parent == 0xFFFFFFFFu) ? -1 : (int64_t)b->parent;
        const char *mode = NULL;
        if (rig && i < WE_MDL_MAX_BONES) {
            switch (rig->bone_drive_modes[i]) {
            case WE_BONE_DRIVE_IK:
                mode = "IK";
                break;
            case WE_BONE_DRIVE_AIM:
                mode = "Aim";
                break;
            case WE_BONE_DRIVE_XPBD:
                mode = "XPBD";
                break;
            case WE_BONE_DRIVE_FK:
            default:
                mode = (b->simulation_json[0] != '\0') ? "XPBD" : "FK";
                break;
            }
        } else {
            mode = (b->simulation_json[0] != '\0') ? "XPBD" : "FK";
        }
        if (i > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && jb_ch(&jb, '{');
        ok = ok && jb_key(&jb, &bfirst, "bone_index") && jb_i64(&jb, i);
        ok = ok && jb_key(&jb, &bfirst, "name") && jb_str(&jb, b->name);
        ok = ok && jb_key(&jb, &bfirst, "parent") && jb_i64(&jb, parent_id);
        ok = ok && jb_key(&jb, &bfirst, "drive_mode") && jb_str(&jb, mode);
        if (ok && b->simulation_json[0] != '\0') {
            ok = jb_key(&jb, &bfirst, "simulation_json") && jb_str(&jb, b->simulation_json);
        }
        ok = ok && jb_ch(&jb, '}');
    }
    ok = ok && jb_ch(&jb, ']');

    uint32_t layer_cnt = (p->layer_count > p->anim_count) ? p->layer_count : p->anim_count;
    if (layer_cnt > WE_MDL_MAX_LAYERS) {
        layer_cnt = WE_MDL_MAX_LAYERS;
    }
    ok = ok && jb_key(&jb, &first, "layer_blend_modes") && jb_ch(&jb, '[');
    for (uint32_t l = 0u; ok && l < layer_cnt; ++l) {
        if (l > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        int64_t bmode = rig ? (int64_t)rig->layer_blend_modes[l] : 0;
        ok = ok && jb_i64(&jb, bmode);
    }
    ok = ok && jb_ch(&jb, ']');

    ok = ok && jb_key(&jb, &first, "animation_curves") && jb_ch(&jb, '[');
    for (uint32_t a = 0u; ok && a < p->anim_count; ++a) {
        const we_mdl_anim_t *anim = &p->anims[a];
        bool afirst = true;
        const char *mode_s = (anim->mode == WE_ANIM_MIRROR)
                                 ? "mirror"
                                 : ((anim->mode == WE_ANIM_SINGLE) ? "single" : "loop");
        if (a > 0u) {
            ok = ok && jb_ch(&jb, ',');
        }
        ok = ok && jb_ch(&jb, '{');
        ok = ok && jb_key(&jb, &afirst, "id") && jb_i64(&jb, anim->id);
        ok = ok && jb_key(&jb, &afirst, "name") && jb_str(&jb, anim->name);
        ok = ok && jb_key(&jb, &afirst, "mode") && jb_str(&jb, mode_s);
        ok = ok && jb_key(&jb, &afirst, "fps") && jb_f32(&jb, anim->fps);
        ok = ok && jb_key(&jb, &afirst, "length") && jb_i64(&jb, anim->length);
        ok = ok && jb_key(&jb, &afirst, "tracks") && jb_ch(&jb, '[');
        for (uint32_t t = 0u; ok && t < anim->track_count; ++t) {
            const we_mdl_bone_track_t *tr = &anim->tracks[t];
            bool tfirst = true;
            if (t > 0u) {
                ok = ok && jb_ch(&jb, ',');
            }
            ok = ok && jb_ch(&jb, '{');
            ok = ok && jb_key(&jb, &tfirst, "bone_index") && jb_i64(&jb, tr->bone_index);
            ok = ok && jb_key(&jb, &tfirst, "keyframes") && jb_ch(&jb, '[');
            for (uint32_t k = 0u; ok && k < tr->kf_count; ++k) {
                const we_mdl_keyframe_t *kf = &tr->keyframes[k];
                bool kfirst = true;
                const char *tmode = find_existing_tangent_mode(prev_root, a, tr->bone_index, k);
                if (k > 0u) {
                    ok = ok && jb_ch(&jb, ',');
                }
                ok = ok && jb_ch(&jb, '{');
                ok = ok && jb_key(&jb, &kfirst, "frame") && jb_i64(&jb, k);
                ok = ok && jb_key(&jb, &kfirst, "pos") && jb_f32_arr(&jb, kf->pos, 3u);
                ok = ok && jb_key(&jb, &kfirst, "angle") && jb_f32_arr(&jb, kf->angle, 3u);
                ok = ok && jb_key(&jb, &kfirst, "scale") && jb_f32_arr(&jb, kf->scale, 3u);
                ok = ok && jb_key(&jb, &kfirst, "tangentMode") && jb_str(&jb, tmode);
                ok = ok && jb_ch(&jb, '}');
            }
            ok = ok && jb_ch(&jb, ']') && jb_ch(&jb, '}');
        }
        ok = ok && jb_ch(&jb, ']') && jb_ch(&jb, '}');
    }
    ok = ok && jb_ch(&jb, ']') && jb_ch(&jb, '}');

    we_jval_free(prev_root);
    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static bool resolve_unique_json_asset_path(const asset_list_t *al, const char *base_rel,
                                           const uint8_t *bytes, size_t sz, char *out_rel,
                                           size_t out_cap, bool *out_reuse_existing,
                                           we_asset_writer_err_t *out_err)
{
    *out_reuse_existing = false;
    char norm_base[320];
    if (!we_normalize_rel_path(base_rel, norm_base, sizeof(norm_base), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    const owned_asset_t *existing = asset_list_find(al, norm_base);
    if (!existing) {
        if (snprintf(out_rel, out_cap, "%s", norm_base) >= (int)out_cap) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        return true;
    }
    if (asset_bytes_equal(existing, bytes, sz)) {
        if (snprintf(out_rel, out_cap, "%s", norm_base) >= (int)out_cap) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        *out_reuse_existing = true;
        return true;
    }

    size_t blen = strlen(norm_base);
    size_t stem_len = (blen > 5u && has_json_ext(norm_base)) ? (blen - 5u) : blen;
    for (uint32_t k = 1u; k < 1024u; ++k) {
        char raw[360];
        int n =
            snprintf(raw, sizeof(raw), "%.*s_%u.json", (int)stem_len, norm_base, (unsigned int)k);
        if (n < 0 || (size_t)n >= sizeof(raw)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        char cand[360];
        if (!we_normalize_rel_path(raw, cand, sizeof(cand), NULL)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        const owned_asset_t *hit = asset_list_find(al, cand);
        if (!hit || asset_bytes_equal(hit, bytes, sz)) {
            if (snprintf(out_rel, out_cap, "%s", cand) >= (int)out_cap) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
            *out_reuse_existing = (hit != NULL);
            return true;
        }
    }
    set_err(out_err, WE_WRITER_ERR_DUPLICATE_PATH);
    return false;
}

static bool export_object_puppet_assets(asset_list_t *al, asset_list_t *sidecars,
                                        const we_scene_t *scene, const we_scene_object_t *obj,
                                        uint32_t obj_idx, export_ctx_t *ectx,
                                        char **out_puppet_override, we_asset_writer_err_t *out_err)
{
    char stem[80];
    char raw_mdl[128];
    char raw_mat[128];
    char mdl_rel[160];
    char pref_mat_rel[320];
    char mat_rel[320];

    sanitize_puppet_stem(scene, obj, obj_idx, stem, sizeof(stem));
    int mn = snprintf(raw_mdl, sizeof(raw_mdl), "models/%s.mdl", stem);
    int tn = snprintf(raw_mat, sizeof(raw_mat), "materials/%s.json", stem);
    if (mn < 0 || (size_t)mn >= sizeof(raw_mdl) || tn < 0 || (size_t)tn >= sizeof(raw_mat)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    if (!we_normalize_rel_path(raw_mdl, mdl_rel, sizeof(mdl_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    if (obj->puppet->mat_json_file[0] != '\0' &&
        we_normalize_rel_path(obj->puppet->mat_json_file, pref_mat_rel, sizeof(pref_mat_rel),
                              NULL) &&
        has_json_ext(pref_mat_rel)) {
        /* Preserve an already-normalized material JSON path when present. */
    } else if (!we_normalize_rel_path(raw_mat, pref_mat_rel, sizeof(pref_mat_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    we_material_pass_t pass_copy;
    if (obj->has_base_pass) {
        pass_copy = obj->base_pass;
    } else {
        memset(&pass_copy, 0, sizeof(pass_copy));
        pass_copy.blending = WE_BLEND_NORMAL;
        pass_copy.depthtest_enabled = false;
        pass_copy.depthwrite_enabled = false;
        pass_copy.cull_enabled = false;
        size_t img_len = strlen(obj->image_path);
        bool is_mdl_path =
            (img_len > 4u && strcasecmp(obj->image_path + img_len - 4u, ".mdl") == 0);
        if (obj->image_path[0] != '\0' && !has_json_ext(obj->image_path) && !is_mdl_path) {
            snprintf(pass_copy.tex_paths[0], sizeof(pass_copy.tex_paths[0]), "%s", obj->image_path);
        }
    }
    if (pass_copy.shader_name[0] == '\0') {
        snprintf(pass_copy.shader_name, sizeof(pass_copy.shader_name), "genericimage2");
    }

    if (!export_pass_assets(al, scene, &pass_copy, ectx, out_err)) {
        return false;
    }

    /* Resolve material JSON collision before writing .mdl so shared paths with distinct overrides
     * are not silently overwritten. */
    char *mat_json = NULL;
    size_t mat_len = 0u;
    bool reuse_mat = false;
    if (!serialize_single_pass_material(scene, obj, &pass_copy, NULL, ectx, &mat_json, &mat_len,
                                        out_err) ||
        !resolve_unique_json_asset_path(al, pref_mat_rel, (const uint8_t *)mat_json, mat_len,
                                        mat_rel, sizeof(mat_rel), &reuse_mat, out_err)) {
        free(mat_json);
        return false;
    }
    if (reuse_mat) {
        free(mat_json);
    } else if (!asset_list_add_owned(al, mat_rel, (uint8_t *)mat_json, mat_len, out_err)) {
        return false;
    }

    we_puppet_t pcopy = *obj->puppet;
    int pn = snprintf(pcopy.mat_json_file, sizeof(pcopy.mat_json_file), "%s", mat_rel);
    if (pn < 0 || (size_t)pn >= sizeof(pcopy.mat_json_file)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    we_mdl_write_opts_t mdl_opts;
    we_mdl_write_opts_default(&mdl_opts);
    uint8_t *mdl_bytes = NULL;
    size_t mdl_sz = 0u;
    if (!we_mdl_write_mem(&pcopy, &mdl_opts, &mdl_bytes, &mdl_sz)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    if (!asset_list_add_owned(al, mdl_rel, mdl_bytes, mdl_sz, out_err)) {
        return false;
    }

    bool unique_id = (obj->id >= 0);
    if (unique_id && scene && scene->objects) {
        for (uint32_t j = 0u; j < scene->object_count; ++j) {
            if (j != obj_idx && scene->objects[j].id == obj->id) {
                unique_id = false;
                break;
            }
        }
    }
    char raw_sidecar[128];
    char sidecar_rel[160];
    int sn = unique_id ? snprintf(raw_sidecar, sizeof(raw_sidecar), "studio_rig_%d.json", obj->id)
                       : snprintf(raw_sidecar, sizeof(raw_sidecar), "studio_rig_%u.json",
                                  (unsigned int)obj_idx);
    if (sn < 0 || (size_t)sn >= sizeof(raw_sidecar) ||
        !we_normalize_rel_path(raw_sidecar, sidecar_rel, sizeof(sidecar_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    char *sc_json = NULL;
    size_t sc_len = 0u;
    if (!serialize_puppet_sidecar(scene, obj, sidecar_rel, mdl_rel, mat_rel, &sc_json, &sc_len,
                                  out_err) ||
        !asset_list_add_owned(sidecars, sidecar_rel, (uint8_t *)sc_json, sc_len, out_err)) {
        return false;
    }

    *out_puppet_override = strdup(mdl_rel);
    if (!*out_puppet_override) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    return true;
}

static bool export_object_particle_assets(asset_list_t *al, const we_scene_t *scene,
                                          const we_scene_object_t *obj, uint32_t obj_idx,
                                          export_ctx_t *ectx, char **out_particle_override,
                                          we_asset_writer_err_t *out_err)
{
    char pref_part_rel[320];
    if (!build_particle_rel(scene, obj, obj_idx, pref_part_rel, sizeof(pref_part_rel))) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    char mat_rel[320];
    mat_rel[0] = '\0';
    if (obj->has_base_pass) {
        if (!export_pass_assets(al, scene, &obj->base_pass, ectx, out_err)) {
            return false;
        }
        char pref_mat_rel[320];
        pref_mat_rel[0] = '\0';
        if (obj->particle_sys->material_path[0] != '\0' &&
            we_normalize_rel_path(obj->particle_sys->material_path, pref_mat_rel,
                                  sizeof(pref_mat_rel), NULL) &&
            has_json_ext(pref_mat_rel) && strcasecmp(pref_mat_rel, pref_part_rel) != 0) {
            /* Keep normalized non-colliding particle material path. */
        } else {
            int mn = snprintf(pref_mat_rel, sizeof(pref_mat_rel), "materials/particle_%u.json",
                              (unsigned int)obj_idx);
            if (mn < 0 || (size_t)mn >= sizeof(pref_mat_rel)) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
        }

        char *mat_json = NULL;
        size_t mat_len = 0u;
        bool reuse_mat = false;
        if (!serialize_single_pass_material(scene, obj, &obj->base_pass, NULL, ectx, &mat_json,
                                            &mat_len, out_err) ||
            !resolve_unique_json_asset_path(al, pref_mat_rel, (const uint8_t *)mat_json, mat_len,
                                            mat_rel, sizeof(mat_rel), &reuse_mat, out_err)) {
            free(mat_json);
            return false;
        }
        if (reuse_mat) {
            free(mat_json);
        } else if (!asset_list_add_owned(al, mat_rel, (uint8_t *)mat_json, mat_len, out_err)) {
            return false;
        }
    } else if (obj->particle_sys->material_path[0] != '\0' &&
               we_normalize_rel_path(obj->particle_sys->material_path, mat_rel, sizeof(mat_rel),
                                     NULL)) {
        /* Preserve referenced material path when no in-memory base_pass exists. */
    }

    char *part_json = NULL;
    size_t part_len = 0u;
    char part_rel[320];
    bool reuse_part = false;
    if (!serialize_particle_json(obj->particle_sys, mat_rel[0] ? mat_rel : NULL, &part_json,
                                 &part_len, out_err) ||
        !resolve_unique_json_asset_path(al, pref_part_rel, (const uint8_t *)part_json, part_len,
                                        part_rel, sizeof(part_rel), &reuse_part, out_err)) {
        free(part_json);
        return false;
    }
    if (reuse_part) {
        free(part_json);
    } else if (!asset_list_add_owned(al, part_rel, (uint8_t *)part_json, part_len, out_err)) {
        return false;
    }

    *out_particle_override = strdup(part_rel);
    if (!*out_particle_override) {
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }
    return true;
}

static bool validate_export_paths(const asset_list_t *al, const asset_list_t *sidecars,
                                  const char *export_dir, bool pack_scene_pkg,
                                  we_asset_writer_err_t *out_err)
{
    char probe[PATH_MAX];
    int pn = snprintf(probe, sizeof(probe), "%s/project.json", export_dir);
    if (pn < 0 || (size_t)pn >= sizeof(probe)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    for (size_t i = 0u; i < sidecars->count; ++i) {
        pn = snprintf(probe, sizeof(probe), "%s/%s", export_dir, sidecars->items[i].path);
        if (pn < 0 || (size_t)pn >= sizeof(probe)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
    }
    if (pack_scene_pkg) {
        pn = snprintf(probe, sizeof(probe), "%s/scene.pkg", export_dir);
        if (pn < 0 || (size_t)pn >= sizeof(probe)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
        return true;
    }
    for (size_t i = 0u; i < al->count; ++i) {
        pn = snprintf(probe, sizeof(probe), "%s/%s", export_dir, al->items[i].path);
        if (pn < 0 || (size_t)pn >= sizeof(probe)) {
            set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
            return false;
        }
    }
    return true;
}

static bool collect_scene_export_assets(const we_scene_t *scene, const char *scene_rel,
                                        asset_list_t *al, asset_list_t *sidecars,
                                        export_ctx_t *ectx, we_asset_writer_err_t *out_err)
{
    size_t obj_slots = scene->object_count ? scene->object_count : 1u;
    char **img_overrides = (char **)calloc(obj_slots, sizeof(char *));
    char **puppet_overrides = (char **)calloc(obj_slots, sizeof(char *));
    char **particle_overrides = (char **)calloc(obj_slots, sizeof(char *));
    if (!img_overrides || !puppet_overrides || !particle_overrides) {
        free(img_overrides);
        free(puppet_overrides);
        free(particle_overrides);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0u; ok && i < scene->object_count; ++i) {
        const we_scene_object_t *obj = &scene->objects[i];
        if (obj->puppet && !obj->is_text_object && !obj->passthrough) {
            ok = export_object_puppet_assets(al, sidecars, scene, obj, i, ectx,
                                             &puppet_overrides[i], out_err);
        } else if (obj->particle_sys && !obj->is_text_object && !obj->passthrough) {
            ok = export_object_particle_assets(al, scene, obj, i, ectx, &particle_overrides[i],
                                               out_err);
        } else if (obj->has_base_pass && !obj->is_text_object && !obj->passthrough) {
            ok = export_object_material(al, scene, obj, i, ectx, &img_overrides[i], out_err);
        }
        if (ok) {
            ok = export_object_effects_assets(al, scene, obj, i, ectx, out_err);
        }
        if (ok) {
            ok = export_object_sound_assets(al, obj, i, out_err);
        }
    }

    if (ok) {
        char *scene_json = NULL;
        size_t scene_len = 0u;
        ok = serialize_scene_with_overrides(
                 scene, (const char *const *)img_overrides, (const char *const *)puppet_overrides,
                 (const char *const *)particle_overrides, ectx, &scene_json, &scene_len, out_err) &&
             asset_list_add_owned(al, scene_rel, (uint8_t *)scene_json, scene_len, out_err);
    }

    for (uint32_t i = 0u; i < scene->object_count; ++i) {
        free(img_overrides[i]);
        free(puppet_overrides[i]);
        free(particle_overrides[i]);
    }
    free(img_overrides);
    free(puppet_overrides);
    free(particle_overrides);
    return ok;
}

static bool write_export_outputs(const we_scene_t *scene, const char *scene_rel,
                                 const asset_list_t *al, const asset_list_t *sidecars,
                                 const char *export_dir, bool pack_scene_pkg,
                                 we_asset_writer_err_t *out_err)
{
    if (!mkdir_p(export_dir)) {
        set_err(out_err, WE_WRITER_ERR_IO);
        return false;
    }

    int root_dirfd = open(export_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_dirfd < 0) {
        set_err(out_err, errno == ELOOP ? WE_WRITER_ERR_INVALID_PATH : WE_WRITER_ERR_IO);
        return false;
    }

    we_project_t proj_copy = scene->project;
    if (proj_copy.type[0] == '\0') {
        snprintf(proj_copy.type, sizeof(proj_copy.type), "scene");
    }
    int fn = snprintf(proj_copy.file, sizeof(proj_copy.file), "%s", scene_rel);
    if (fn < 0 || (size_t)fn >= sizeof(proj_copy.file)) {
        close(root_dirfd);
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    if (!maybe_write_preview(scene, root_dirfd, &proj_copy, out_err)) {
        close(root_dirfd);
        return false;
    }

    char *proj_json = NULL;
    size_t proj_len = 0u;
    bool ok = we_project_serialize_json(&proj_copy, &proj_json, &proj_len, out_err) &&
              write_file_atomic_beneath(root_dirfd, "project.json", proj_json, proj_len, out_err);
    free(proj_json);
    if (!ok) {
        close(root_dirfd);
        return false;
    }

    for (size_t i = 0u; i < sidecars->count; ++i) {
        if (!write_file_atomic_beneath(root_dirfd, sidecars->items[i].path,
                                       sidecars->items[i].data ? sidecars->items[i].data
                                                               : (const uint8_t *)"",
                                       sidecars->items[i].size, out_err)) {
            close(root_dirfd);
            return false;
        }
    }

    if (pack_scene_pkg) {
        we_pkg_write_entry_t *entries =
            (we_pkg_write_entry_t *)calloc(al->count ? al->count : 1u, sizeof(*entries));
        if (!entries) {
            close(root_dirfd);
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        for (size_t i = 0u; i < al->count; ++i) {
            entries[i].path = al->items[i].path;
            entries[i].data = al->items[i].data;
            entries[i].size = al->items[i].size;
        }
        uint8_t *pkg_buf = NULL;
        size_t pkg_sz = 0u;
        ok = we_pkg_write_mem(entries, al->count, &pkg_buf, &pkg_sz, out_err) &&
             write_file_atomic_beneath(root_dirfd, "scene.pkg",
                                       pkg_buf ? pkg_buf : (const uint8_t *)"", pkg_sz, out_err);
        free(pkg_buf);
        free(entries);
    } else {
        for (size_t i = 0u; ok && i < al->count; ++i) {
            ok = write_file_atomic_beneath(root_dirfd, al->items[i].path,
                                           al->items[i].data ? al->items[i].data
                                                             : (const uint8_t *)"",
                                           al->items[i].size, out_err);
        }
    }

    if (ok && fsync(root_dirfd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        set_err(out_err, WE_WRITER_ERR_IO);
        ok = false;
    }
    close(root_dirfd);
    return ok;
}

bool we_scene_export_dir(const we_scene_t *scene, const char *export_dir, bool pack_scene_pkg,
                         we_asset_writer_err_t *out_err)
{
    if (!scene || !export_dir || export_dir[0] == '\0') {
        set_err(out_err, WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    if (scene->object_count > WE_SCENE_MAX_OBJECTS ||
        (scene->object_count > 0u && !scene->objects)) {
        set_err(out_err, scene->object_count > WE_SCENE_MAX_OBJECTS ? WE_WRITER_ERR_LIMIT_EXCEEDED
                                                                    : WE_WRITER_ERR_INVALID_ARG);
        return false;
    }
    if (scene->camera.keyframe_count > WE_CAMERA_MAX_KEYFRAMES) {
        set_err(out_err, WE_WRITER_ERR_LIMIT_EXCEEDED);
        return false;
    }

    char scene_rel[256] = "scene.json";
    if (scene->project.file[0] != '\0' &&
        !we_normalize_rel_path(scene->project.file, scene_rel, sizeof(scene_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }
    if (strlen(scene_rel) >= sizeof(scene->project.file)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    asset_list_t al = {0};
    asset_list_t sidecars = {0};
    export_ctx_t ectx = {0};
    bool ok =
        collect_scene_export_assets(scene, scene_rel, &al, &sidecars, &ectx, out_err) &&
        validate_export_paths(&al, &sidecars, export_dir, pack_scene_pkg, out_err) &&
        write_export_outputs(scene, scene_rel, &al, &sidecars, export_dir, pack_scene_pkg, out_err);
    export_ctx_free(&ectx);
    asset_list_free(&al);
    asset_list_free(&sidecars);
    if (ok) {
        set_err(out_err, WE_WRITER_OK);
    }
    return ok;
}
