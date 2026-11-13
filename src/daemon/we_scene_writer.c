// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_asset_writer.h"
#include "waywal/we_json.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/wwpkg.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    we_asset_writer_err_t err;
    locale_t cloc;
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
    jb->cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

static void jb_destroy(json_buf_t *jb)
{
    free(jb->data);
    jb->data = NULL;
    jb->len = 0u;
    jb->cap = 0u;
    if (jb->cloc != (locale_t)0) {
        freelocale(jb->cloc);
        jb->cloc = (locale_t)0;
    }
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

    locale_t old_loc = (locale_t)0;
    if (jb->cloc != (locale_t)0) {
        old_loc = uselocale(jb->cloc);
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

    if (jb->cloc != (locale_t)0) {
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

/* Reconstructs {"user": ..., "value": ...} matching we_jval_floats condition encoding. */
static bool jb_user_prefix(json_buf_t *jb, const char *user_prop, bool *wrapped)
{
    *wrapped = false;
    if (!user_prop || user_prop[0] == '\0') {
        return true;
    }
    *wrapped = true;
    if (!jb_cstr(jb, "{\"user\":")) {
        return false;
    }
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
        if (!jb_cstr(jb, "{\"condition\":") || !jb_str(jb, cond) || !jb_cstr(jb, ",\"name\":") ||
            !jb_str(jb, key) || !jb_ch(jb, '}')) {
            return false;
        }
    } else if (eq) {
        size_t klen = (size_t)(eq - user_prop);
        char key[64];
        if (klen >= sizeof(key)) {
            klen = sizeof(key) - 1u;
        }
        memcpy(key, user_prop, klen);
        key[klen] = '\0';
        if (!jb_cstr(jb, "{\"condition\":") || !jb_str(jb, eq + 1) || !jb_cstr(jb, ",\"name\":") ||
            !jb_str(jb, key) || !jb_ch(jb, '}')) {
            return false;
        }
    } else {
        if (!jb_str(jb, user_prop)) {
            return false;
        }
    }
    return jb_cstr(jb, ",\"value\":");
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

static bool serialize_pass_body(json_buf_t *jb, const we_material_pass_t *pass,
                                bool include_routing, const char *material_rel_path)
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

    if (pass->shader_name[0] != '\0') {
        ok = ok && jb_key(jb, &first, "shader") && jb_str(jb, pass->shader_name);
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

    if (pass->const_val_count > 0u) {
        ok = ok && jb_key(jb, &first, "constantshadervalues") && jb_ch(jb, '{');
        bool cvf = true;
        for (uint32_t c = 0u; ok && c < pass->const_val_count && c < WE_MAX_CONST_VALS; ++c) {
            const we_const_val_t *cv = &pass->const_vals[c];
            if (cv->name[0] == '\0') {
                continue;
            }
            uint32_t nc =
                (cv->num_components >= 1u && cv->num_components <= 4u) ? cv->num_components : 1u;
            ok = ok && jb_key(jb, &cvf, cv->name);
            if (nc == 1u) {
                ok = ok && jb_bound_f32(jb, cv->val[0], cv->user_prop);
            } else {
                ok = ok && jb_bound_vec(jb, cv->val, nc, cv->user_prop);
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

static bool serialize_object_text(json_buf_t *jb, const we_scene_object_t *obj, bool *first)
{
    bool ok = jb_key(jb, first, "text") && jb_bound_str(jb, obj->text_str, obj->user_prop_text);
    if (obj->text_font[0] != '\0') {
        ok = ok && jb_key(jb, first, "font") && jb_str(jb, obj->text_font);
    }
    ok = ok && jb_key(jb, first, "pointsize") &&
         jb_bound_i64(jb, obj->text_pointsize, obj->user_prop_pointsize);
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

static bool serialize_object_sound(json_buf_t *jb, const we_scene_object_t *obj, uint32_t obj_idx,
                                   bool *first)
{
    bool ok = jb_key(jb, first, "playbackmode") && jb_str(jb, sound_mode_name(obj->sound_mode));
    ok = ok && jb_key(jb, first, "volume") &&
         jb_bound_f32(jb, obj->sound_volume, obj->user_prop_volume);
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

static bool serialize_object_effects(json_buf_t *jb, const we_scene_object_t *obj, uint32_t obj_idx,
                                     bool *first)
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
                ok = ok && serialize_pass_body(jb, &eff->passes[p], false, NULL);
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
                             const char *puppet_override)
{
    bool ok = jb_ch(jb, '{');
    bool first = true;
    const char *img_out = image_override ? image_override : obj->image_path;
    char auto_puppet_rel[160];
    const char *puppet_out = puppet_override;

    if (!puppet_out && obj->puppet && !obj->is_text_object && !obj->passthrough) {
        if (!build_puppet_mdl_rel(scene, obj, obj_idx, auto_puppet_rel, sizeof(auto_puppet_rel))) {
            jb->err = WE_WRITER_ERR_INVALID_PATH;
            return false;
        }
        puppet_out = auto_puppet_rel;
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
    } else if (img_out[0] != '\0') {
        ok = ok && jb_key(jb, &first, "image") && jb_str(jb, img_out);
    }

    ok = ok && jb_key(jb, &first, "origin") && jb_vec_str(jb, obj->origin, 3u);
    ok = ok && jb_key(jb, &first, "scale") && jb_vec_str(jb, obj->scale, 3u);
    ok = ok && jb_key(jb, &first, "angles") && jb_vec_str(jb, obj->angles, 3u);
    ok = ok && jb_key(jb, &first, "size") && jb_vec_str(jb, obj->size, 2u);
    ok = ok && jb_key(jb, &first, "parallaxDepth") && jb_vec_str(jb, obj->parallax_depth, 2u);
    ok = ok && jb_key(jb, &first, "alpha") && jb_bound_f32(jb, obj->alpha, obj->alpha_user_prop);
    ok = ok && jb_key(jb, &first, "brightness") && jb_f32(jb, obj->brightness);
    ok =
        ok && jb_key(jb, &first, "color") && jb_bound_vec(jb, obj->color, 3u, obj->color_user_prop);
    ok = ok && jb_key(jb, &first, "colorBlendMode") && jb_i64(jb, obj->color_blend_mode);
    ok = ok && jb_key(jb, &first, "visible") &&
         jb_bound_bool(jb, obj->visible, obj->visible_user_prop);
    ok = ok && jb_key(jb, &first, "copybackground") &&
         jb_cstr(jb, obj->copybackground ? "true" : "false");
    if (obj->fullscreen) {
        ok = ok && jb_key(jb, &first, "fullscreen") && jb_cstr(jb, "true");
    }
    if (obj->passthrough) {
        ok = ok && jb_key(jb, &first, "passthrough") && jb_cstr(jb, "true");
    }
    if (ok && obj->is_text_object) {
        ok = serialize_object_text(jb, obj, &first);
    }
    if (ok && obj->is_light_object) {
        ok = serialize_object_light(jb, obj, &first);
    }
    if (ok && obj->is_sound_object) {
        ok = serialize_object_sound(jb, obj, obj_idx, &first);
    }
    if (ok && obj->effect_count > 0u) {
        ok = serialize_object_effects(jb, obj, obj_idx, &first);
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
                                           const char *const *puppet_overrides, char **out_json,
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
        ok = ok && serialize_object(&jb, scene, &scene->objects[i], i, ovr, povr);
    }
    ok = ok && jb_ch(&jb, ']') && jb_ch(&jb, '}');

    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

bool we_scene_serialize_json(const we_scene_t *scene, char **out_json, size_t *out_len,
                             we_asset_writer_err_t *out_err)
{
    return serialize_scene_with_overrides(scene, NULL, NULL, out_json, out_len, out_err);
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

static bool mkdir_parent_for(const char *file_path)
{
    const char *slash = strrchr(file_path, '/');
    if (!slash || slash == file_path) {
        return true;
    }
    char dir[PATH_MAX];
    size_t dlen = (size_t)(slash - file_path);
    if (dlen >= sizeof(dir)) {
        return false;
    }
    memcpy(dir, file_path, dlen);
    dir[dlen] = '\0';
    return mkdir_p(dir);
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

static bool asset_list_has(const asset_list_t *al, const char *norm_path)
{
    for (size_t i = 0u; i < al->count; ++i) {
        if (strcasecmp(al->items[i].path, norm_path) == 0) {
            return true;
        }
    }
    return false;
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
        for (uint32_t i = 0u; i < tex->image_count; ++i) {
            if (tex->images[i].mip_count == 0u || !tex->images[i].mips) {
                all_raw = false;
                all_rgba8 = false;
                break;
            }
            for (uint32_t m = 0u; m < tex->images[i].mip_count; ++m) {
                if (!tex->images[i].mips[m].data || tex->images[i].mips[m].data_size == 0u) {
                    all_raw = false;
                }
                if (!tex->images[i].mips[m].rgba8) {
                    all_rgba8 = false;
                }
            }
        }
        if (all_raw || all_rgba8) {
            return encode_tex_from_memory(al, norm, tex, all_raw, out_err);
        }
    }

    if (scene->pkg) {
        const char *cands[2] = {norm, tex_name};
        for (int i = 0; i < 2; ++i) {
            size_t rsz = 0u;
            bool owned = false;
            const uint8_t *rdata = we_pkg_resolve(scene->pkg, cands[i], &rsz, &owned);
            if (rdata) {
                return owned ? asset_list_add_owned(al, norm, (uint8_t *)(uintptr_t)rdata, rsz,
                                                    out_err)
                             : asset_list_add_copy(al, norm, rdata, rsz, out_err);
            }
        }
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
    return hit ? (hit + strlen(marker)) : src;
}

static bool export_pass_assets(asset_list_t *al, const we_scene_t *scene,
                               const we_material_pass_t *pass, we_asset_writer_err_t *out_err)
{
    for (uint32_t s = 0u; s < WE_MAX_PASS_TEXTURES; ++s) {
        if (pass->tex_paths[s][0] != '\0' || pass->textures[s]) {
            const char *tname = pass->tex_paths[s][0] != '\0' ? pass->tex_paths[s] : "texture";
            if (!export_texture_asset(al, scene, tname, pass->textures[s], out_err)) {
                return false;
            }
        }
    }
    if (pass->shader_name[0] != '\0') {
        const char *exts[2] = {"vert", "frag"};
        const char *live_stages[2] = {pass->prep.vert_glsl, pass->prep.frag_glsl};
        for (int e = 0; e < 2; ++e) {
            char spath[256];
            int sn = snprintf(spath, sizeof(spath), "shaders/%s.%s", pass->shader_name, exts[e]);
            if (sn < 0 || (size_t)sn >= sizeof(spath)) {
                set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
                return false;
            }
            if (live_stages[e] && live_stages[e][0] != '\0') {
                const char *body = strip_shader_preamble(live_stages[e], e == 1);
                if (!asset_list_add_copy(al, spath, (const uint8_t *)body, strlen(body), out_err)) {
                    return false;
                }
            } else if (scene->pkg) {
                size_t ssz = 0u;
                bool owned = false;
                const uint8_t *sdata = we_pkg_resolve(scene->pkg, spath, &ssz, &owned);
                if (sdata) {
                    bool ok = owned ? asset_list_add_owned(al, spath, (uint8_t *)(uintptr_t)sdata,
                                                           ssz, out_err)
                                    : asset_list_add_copy(al, spath, sdata, ssz, out_err);
                    if (!ok) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool serialize_single_pass_material(const we_material_pass_t *pass,
                                           const char *self_mat_path, char **out_json,
                                           size_t *out_len, we_asset_writer_err_t *out_err)
{
    json_buf_t jb;
    jb_init(&jb);
    bool ok = jb_ch(&jb, '{');
    if (self_mat_path && self_mat_path[0] != '\0') {
        ok = ok && jb_cstr(&jb, "\"material\":") && jb_str(&jb, self_mat_path) && jb_ch(&jb, ',');
    }
    ok = ok && jb_cstr(&jb, "\"passes\":[") && serialize_pass_body(&jb, pass, false, NULL) &&
         jb_cstr(&jb, "]}");
    return finish_jb(&jb, ok, out_json, out_len, out_err);
}

static bool serialize_effect_json(const we_effect_t *eff, char **out_json, size_t *out_len,
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
        ok = ok && serialize_pass_body(&jb, &eff->passes[p], true, NULL);
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

static bool copy_existing_file(const char *src_path, const char *dst_path,
                               we_asset_writer_err_t *out_err)
{
    int fd = open(src_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > (64 * 1024 * 1024)) {
        close(fd);
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
        return false;
    }
    bool ok = we_write_file_atomic(dst_path, buf ? buf : (const uint8_t *)"", sz, out_err);
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

static void maybe_write_preview(const we_scene_t *scene, const char *export_dir,
                                we_project_t *proj_mut)
{
    /* Use a scratch error so non-fatal preview failures never clobber caller out_err. */
    we_asset_writer_err_t scratch_err = WE_WRITER_OK;
    char existing[PATH_MAX];
    if (scene->project.workshop_dir[0] != '\0' &&
        we_workshop_find_preview_file(scene->project.workshop_dir, existing, sizeof(existing))) {
        const char *base = strrchr(existing, '/');
        base = base ? (base + 1) : existing;
        if (base[0] != '\0' && strlen(base) < sizeof(proj_mut->preview)) {
            char dst[PATH_MAX];
            int dn = snprintf(dst, sizeof(dst), "%s/%s", export_dir, base);
            if (dn >= 0 && (size_t)dn < sizeof(dst) &&
                copy_existing_file(existing, dst, &scratch_err)) {
                memcpy(proj_mut->preview, base, strlen(base) + 1u);
                return;
            }
        }
    }

    pthread_once(&g_libwebp_once, init_libwebp_once);
    if (!g_webp_encode_fn || !g_webp_free_fn) {
        return;
    }

    uint8_t *rgba = NULL;
    uint32_t rw = 0u, rh = 0u;
    if (extract_preview_rgba(scene, &rgba, &rw, &rh) && rgba) {
        uint8_t *webp = NULL;
        size_t wsz = g_webp_encode_fn(rgba, (int)rw, (int)rh, (int)(rw * 4u), 80.0f, &webp);
        free(rgba);
        if (wsz > 0u && webp) {
            char dst[PATH_MAX];
            int dn = snprintf(dst, sizeof(dst), "%s/preview.webp", export_dir);
            if (dn >= 0 && (size_t)dn < sizeof(dst) &&
                we_write_file_atomic(dst, webp, wsz, &scratch_err)) {
                snprintf(proj_mut->preview, sizeof(proj_mut->preview), "preview.webp");
            }
            g_webp_free_fn(webp);
        }
    }
}

static bool has_json_ext(const char *path)
{
    size_t len = path ? strlen(path) : 0u;
    return len >= 5u && strcasecmp(path + len - 5u, ".json") == 0;
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
                                   char **out_img_override, we_asset_writer_err_t *out_err)
{
    if (!export_pass_assets(al, scene, &obj->base_pass, out_err)) {
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
    if (!serialize_single_pass_material(&obj->base_pass, self_mat_in_material ? mat_rel : NULL,
                                        &mat_json, &mat_len, out_err)) {
        return false;
    }
    return asset_list_add_owned(al, mat_rel, (uint8_t *)mat_json, mat_len, out_err);
}

static bool export_object_effects_assets(asset_list_t *al, const we_scene_t *scene,
                                         const we_scene_object_t *obj, uint32_t obj_idx,
                                         we_asset_writer_err_t *out_err)
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
            if (!export_pass_assets(al, scene, &eff->passes[p], out_err)) {
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
                if (!serialize_effect_json(eff, &ej, &ejl, out_err) ||
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
    locale_t old_loc = (locale_t)0;
    if (jb->cloc != (locale_t)0) {
        old_loc = uselocale(jb->cloc);
    }
    int n = snprintf(buf, sizeof(buf), "%.15g", v);
    if (jb->cloc != (locale_t)0) {
        uselocale(old_loc);
    }
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
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
    int fd = openat(dfd, sidecar_rel, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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

static bool export_object_puppet_assets(asset_list_t *al, asset_list_t *sidecars,
                                        const we_scene_t *scene, const we_scene_object_t *obj,
                                        uint32_t obj_idx, char **out_puppet_override,
                                        we_asset_writer_err_t *out_err)
{
    char stem[80];
    char raw_mdl[128];
    char raw_mdl_json[128];
    char raw_mat[128];
    char mdl_rel[160];
    char mdl_json_rel[160];
    char mat_rel[320];

    sanitize_puppet_stem(scene, obj, obj_idx, stem, sizeof(stem));
    int mn = snprintf(raw_mdl, sizeof(raw_mdl), "models/%s.mdl", stem);
    int jn = snprintf(raw_mdl_json, sizeof(raw_mdl_json), "models/%s.json", stem);
    int tn = snprintf(raw_mat, sizeof(raw_mat), "materials/%s.json", stem);
    if (mn < 0 || (size_t)mn >= sizeof(raw_mdl) || jn < 0 || (size_t)jn >= sizeof(raw_mdl_json) ||
        tn < 0 || (size_t)tn >= sizeof(raw_mat)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    if (!we_normalize_rel_path(raw_mdl, mdl_rel, sizeof(mdl_rel), NULL) ||
        !we_normalize_rel_path(raw_mdl_json, mdl_json_rel, sizeof(mdl_json_rel), NULL)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    if (obj->puppet->mat_json_file[0] != '\0' &&
        we_normalize_rel_path(obj->puppet->mat_json_file, mat_rel, sizeof(mat_rel), NULL) &&
        has_json_ext(mat_rel)) {
        /* Preserve an already-normalized material JSON path when present. */
    } else if (!we_normalize_rel_path(raw_mat, mat_rel, sizeof(mat_rel), NULL)) {
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

    if (!export_pass_assets(al, scene, &pass_copy, out_err)) {
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

    char *mat_json = NULL;
    size_t mat_len = 0u;
    if (!serialize_single_pass_material(&pass_copy, NULL, &mat_json, &mat_len, out_err) ||
        !asset_list_add_owned(al, mat_rel, (uint8_t *)mat_json, mat_len, out_err)) {
        return false;
    }

    json_buf_t mb;
    jb_init(&mb);
    bool mok = jb_cstr(&mb, "{\"material\":") && jb_str(&mb, mat_rel) &&
               jb_cstr(&mb, ",\"puppet\":") && jb_str(&mb, mdl_rel) && jb_ch(&mb, '}');
    char *mjson = NULL;
    size_t mlen = 0u;
    if (!finish_jb(&mb, mok, &mjson, &mlen, out_err) ||
        !asset_list_add_owned(al, mdl_json_rel, (uint8_t *)mjson, mlen, out_err)) {
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
                                        we_asset_writer_err_t *out_err)
{
    size_t obj_slots = scene->object_count ? scene->object_count : 1u;
    char **img_overrides = (char **)calloc(obj_slots, sizeof(char *));
    char **puppet_overrides = (char **)calloc(obj_slots, sizeof(char *));
    if (!img_overrides || !puppet_overrides) {
        free(img_overrides);
        free(puppet_overrides);
        set_err(out_err, WE_WRITER_ERR_OOM);
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0u; ok && i < scene->object_count; ++i) {
        const we_scene_object_t *obj = &scene->objects[i];
        if (obj->puppet && !obj->is_text_object && !obj->passthrough) {
            ok = export_object_puppet_assets(al, sidecars, scene, obj, i, &puppet_overrides[i],
                                             out_err);
        } else if (obj->has_base_pass && !obj->is_text_object && !obj->passthrough) {
            ok = export_object_material(al, scene, obj, i, &img_overrides[i], out_err);
        }
        if (ok) {
            ok = export_object_effects_assets(al, scene, obj, i, out_err);
        }
        if (ok) {
            ok = export_object_sound_assets(al, obj, i, out_err);
        }
    }

    if (ok) {
        char *scene_json = NULL;
        size_t scene_len = 0u;
        ok = serialize_scene_with_overrides(scene, (const char *const *)img_overrides,
                                            (const char *const *)puppet_overrides, &scene_json,
                                            &scene_len, out_err) &&
             asset_list_add_owned(al, scene_rel, (uint8_t *)scene_json, scene_len, out_err);
    }

    for (uint32_t i = 0u; i < scene->object_count; ++i) {
        free(img_overrides[i]);
        free(puppet_overrides[i]);
    }
    free(img_overrides);
    free(puppet_overrides);
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

    we_project_t proj_copy = scene->project;
    if (proj_copy.type[0] == '\0') {
        snprintf(proj_copy.type, sizeof(proj_copy.type), "scene");
    }
    int fn = snprintf(proj_copy.file, sizeof(proj_copy.file), "%s", scene_rel);
    if (fn < 0 || (size_t)fn >= sizeof(proj_copy.file)) {
        set_err(out_err, WE_WRITER_ERR_INVALID_PATH);
        return false;
    }

    maybe_write_preview(scene, export_dir, &proj_copy);

    char proj_path[PATH_MAX];
    snprintf(proj_path, sizeof(proj_path), "%s/project.json", export_dir);
    char *proj_json = NULL;
    size_t proj_len = 0u;
    bool ok = we_project_serialize_json(&proj_copy, &proj_json, &proj_len, out_err) &&
              we_write_file_atomic(proj_path, proj_json, proj_len, out_err);
    free(proj_json);
    if (!ok) {
        return false;
    }

    for (size_t i = 0u; i < sidecars->count; ++i) {
        char sdst[PATH_MAX];
        snprintf(sdst, sizeof(sdst), "%s/%s", export_dir, sidecars->items[i].path);
        if (!we_write_file_atomic(
                sdst, sidecars->items[i].data ? sidecars->items[i].data : (const uint8_t *)"",
                sidecars->items[i].size, out_err)) {
            return false;
        }
    }

    if (pack_scene_pkg) {
        we_pkg_write_entry_t *entries =
            (we_pkg_write_entry_t *)calloc(al->count ? al->count : 1u, sizeof(*entries));
        if (!entries) {
            set_err(out_err, WE_WRITER_ERR_OOM);
            return false;
        }
        for (size_t i = 0u; i < al->count; ++i) {
            entries[i].path = al->items[i].path;
            entries[i].data = al->items[i].data;
            entries[i].size = al->items[i].size;
        }
        char pkg_path[PATH_MAX];
        snprintf(pkg_path, sizeof(pkg_path), "%s/scene.pkg", export_dir);
        ok = we_pkg_write_file(pkg_path, entries, al->count, out_err);
        free(entries);
        return ok;
    }

    for (size_t i = 0u; i < al->count; ++i) {
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s", export_dir, al->items[i].path);
        if (!mkdir_parent_for(dst)) {
            set_err(out_err, WE_WRITER_ERR_IO);
            return false;
        }
        if (!we_write_file_atomic(dst, al->items[i].data ? al->items[i].data : (const uint8_t *)"",
                                  al->items[i].size, out_err)) {
            return false;
        }
    }
    return true;
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
    bool ok =
        collect_scene_export_assets(scene, scene_rel, &al, &sidecars, out_err) &&
        validate_export_paths(&al, &sidecars, export_dir, pack_scene_pkg, out_err) &&
        write_export_outputs(scene, scene_rel, &al, &sidecars, export_dir, pack_scene_pkg, out_err);
    asset_list_free(&al);
    asset_list_free(&sidecars);
    if (ok) {
        set_err(out_err, WE_WRITER_OK);
    }
    return ok;
}
