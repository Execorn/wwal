// SPDX-License-Identifier: GPL-3.0-or-later
#include "we_scene_internal.h"

#include "waywal/image_loader.h"
#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(HAVE_FFMPEG_SWDEC) && __has_include(<libavformat/avformat.h>)
#define WE_HAVE_LIBAV_AUDIO 1
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#endif

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

void sync_layer_from_obj(const we_scene_object_t *obj, we_script_layer_state_t *ls)
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
    snprintf(ls->text, sizeof(ls->text), "%s", obj->text_str);
    ls->pointsize = obj->text_pointsize > 0 ? obj->text_pointsize : 32;
    snprintf(ls->halign, sizeof(ls->halign), "%s",
             obj->text_halign[0] != '\0' ? obj->text_halign : "center");
    snprintf(ls->valign, sizeof(ls->valign), "%s",
             obj->text_valign[0] != '\0' ? obj->text_valign : "center");
    ls->volume = obj->sound_volume;
    ls->audio_playing = obj->sound_playing;
    ls->audio_cmd = 0;
    ls->id = obj->id;
    ls->dirty_mask = 0;
}

void sync_obj_from_layer(we_scene_object_t *obj, const we_script_layer_state_t *ls)
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
        if (obj->size[0] != ls->size[0] || obj->size[1] != ls->size[1]) {
            memcpy(obj->size, ls->size, sizeof(obj->size));
            if (obj->is_text_object) {
                obj->text_dirty = true;
            }
        }
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
    if (ls->dirty_mask & WE_LAYER_DIRTY_TEXT) {
        if (strcmp(obj->text_str, ls->text) != 0) {
            snprintf(obj->text_str, sizeof(obj->text_str), "%s", ls->text);
            obj->text_dirty = true;
        }
        obj->user_prop_text[0] = '\0';
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_POINTSIZE) {
        int32_t new_ps = ls->pointsize >= 4 ? ls->pointsize : 4;
        if (obj->text_pointsize != new_ps) {
            obj->text_pointsize = new_ps;
            obj->text_dirty = true;
        }
        obj->user_prop_pointsize[0] = '\0';
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_HALIGN) {
        if (strcmp(obj->text_halign, ls->halign) != 0) {
            snprintf(obj->text_halign, sizeof(obj->text_halign), "%s", ls->halign);
            obj->text_dirty = true;
        }
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_VALIGN) {
        if (strcmp(obj->text_valign, ls->valign) != 0) {
            snprintf(obj->text_valign, sizeof(obj->text_valign), "%s", ls->valign);
            obj->text_dirty = true;
        }
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_VOLUME) {
        obj->sound_volume = fminf(fmaxf(ls->volume, 0.0f), 2.0f);
        obj->user_prop_volume[0] = '\0';
    }
    if (ls->dirty_mask & WE_LAYER_DIRTY_AUDIO_CMD) {
        if (ls->audio_cmd == 1) { /* play */
            if (!obj->sound_playing && !obj->sound_paused) {
                obj->sound_cursor_frame = 0;
            }
            obj->sound_playing = true;
            obj->sound_paused = false;
            obj->sound_wait_timer = 0.0f;
        } else if (ls->audio_cmd == 2) { /* stop */
            obj->sound_playing = false;
            obj->sound_paused = false;
            obj->sound_cursor_frame = 0;
            obj->sound_wait_timer = 0.0f;
        } else if (ls->audio_cmd == 3) { /* pause */
            obj->sound_playing = false;
            obj->sound_paused = true;
        }
    }
}

void sync_scene_layers_to_vm(const we_scene_t *scene, we_script_t *vm)
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

void sync_scene_layers_from_vm(we_scene_t *scene, const we_script_t *vm)
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

void build_script_env(const we_scene_t *scene, uint32_t canvas_w, uint32_t canvas_h, float dt_sec,
                      float time_sec, const waywal_shader_frame_params_t *params,
                      we_script_env_t *env)
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

void bind_project_props_to_script(we_script_t *vm, const we_project_t *proj)
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
    } else if (target == WE_SCRIPT_PROP_TEXT) {
        obj->user_prop_text[0] = '\0';
    } else if (target == WE_SCRIPT_PROP_POINTSIZE) {
        obj->user_prop_pointsize[0] = '\0';
    } else if (target == WE_SCRIPT_PROP_VOLUME) {
        obj->user_prop_volume[0] = '\0';
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

void apply_scene_script_user_properties(we_scene_t *scene)
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
    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->is_text_object && obj->text_dirty) {
            we_scene_update_text_texture(scene, obj);
        }
    }
}

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool resample_stereo_f32_to_48k(float *src_stereo, size_t src_frames, uint32_t sample_rate,
                                       float **out_stereo_f32, size_t *out_frame_count)
{
    if (!src_stereo || src_frames == 0 || sample_rate < 4000u || sample_rate > 384000u) {
        free(src_stereo);
        return false;
    }
    if (sample_rate == 48000u) {
        if (src_frames > 48000u * 600u) {
            src_frames = 48000u * 600u;
        }
        *out_stereo_f32 = src_stereo;
        *out_frame_count = src_frames;
        return true;
    }

    size_t dst_frames = (size_t)(((uint64_t)src_frames * 48000ULL) / (uint64_t)sample_rate);
    if (dst_frames == 0) {
        free(src_stereo);
        return false;
    }
    if (dst_frames > 48000u * 600u) {
        dst_frames = 48000u * 600u;
    }
    float *dst_stereo = malloc(dst_frames * 2u * sizeof(float));
    if (!dst_stereo) {
        free(src_stereo);
        return false;
    }

    double step = (double)sample_rate / 48000.0;
    for (size_t i = 0; i < dst_frames; ++i) {
        double src_pos = (double)i * step;
        size_t i0 = (size_t)src_pos;
        if (i0 >= src_frames) {
            i0 = src_frames - 1;
        }
        size_t i1 = (i0 + 1 < src_frames) ? (i0 + 1) : i0;
        float frac = (float)(src_pos - (double)i0);
        dst_stereo[i * 2u + 0u] =
            src_stereo[i0 * 2u + 0u] +
            frac * (src_stereo[i1 * 2u + 0u] - src_stereo[i0 * 2u + 0u]);
        dst_stereo[i * 2u + 1u] =
            src_stereo[i0 * 2u + 1u] +
            frac * (src_stereo[i1 * 2u + 1u] - src_stereo[i0 * 2u + 1u]);
    }

    free(src_stereo);
    *out_stereo_f32 = dst_stereo;
    *out_frame_count = dst_frames;
    return true;
}

bool we_audio_decode_wav_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                                size_t *out_frame_count)
{
    if (!data || size < 44 || !out_stereo_f32 || !out_frame_count) {
        return false;
    }
    *out_stereo_f32 = NULL;
    *out_frame_count = 0;

    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint16_t audio_fmt = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t *pcm_ptr = NULL;
    size_t pcm_bytes = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *hdr = data + pos;
        uint32_t chunk_sz = read_u32_le(hdr + 4);
        pos += 8;
        if (pos + chunk_sz > size) {
            chunk_sz = (uint32_t)(size - pos);
        }

        if (memcmp(hdr, "fmt ", 4) == 0 && chunk_sz >= 16) {
            audio_fmt = read_u16_le(data + pos + 0);
            channels = read_u16_le(data + pos + 2);
            sample_rate = read_u32_le(data + pos + 4);
            block_align = read_u16_le(data + pos + 12);
            bits_per_sample = read_u16_le(data + pos + 14);
            if (audio_fmt == 0xFFFEu && chunk_sz >= 40) {
                audio_fmt = read_u16_le(data + pos + 24);
            }
        } else if (memcmp(hdr, "data", 4) == 0) {
            pcm_ptr = data + pos;
            pcm_bytes = chunk_sz;
            break;
        }
        pos += chunk_sz + (chunk_sz & 1u);
    }

    if (!pcm_ptr || pcm_bytes == 0 || channels == 0 || channels > 8 || sample_rate < 4000u ||
        sample_rate > 384000u) {
        return false;
    }
    if (!((audio_fmt == 1u &&
           (bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 24 ||
            bits_per_sample == 32)) ||
          (audio_fmt == 3u && (bits_per_sample == 32 || bits_per_sample == 64)))) {
        return false;
    }

    size_t bytes_per_sample = bits_per_sample / 8u;
    size_t min_stride = bytes_per_sample * channels;
    size_t frame_stride = (block_align >= min_stride) ? (size_t)block_align : min_stride;
    size_t src_frames = pcm_bytes / frame_stride;
    if (src_frames == 0) {
        return false;
    }
    /* Cap maximum decoded duration at 10 minutes (sample_rate * 600s) to bound RAM */
    if (src_frames > (size_t)sample_rate * 600u) {
        src_frames = (size_t)sample_rate * 600u;
    }

    float *src_stereo = malloc(src_frames * 2u * sizeof(float));
    if (!src_stereo) {
        return false;
    }

    for (size_t f = 0; f < src_frames; ++f) {
        const uint8_t *fp = pcm_ptr + f * frame_stride;
        float ch_val[2] = {0.0f, 0.0f};
        uint16_t read_ch = (channels >= 2) ? 2 : 1;
        for (uint16_t c = 0; c < read_ch; ++c) {
            const uint8_t *sp = fp + c * bytes_per_sample;
            float s = 0.0f;
            if (audio_fmt == 1u) {
                if (bits_per_sample == 8) {
                    s = ((float)sp[0] - 128.0f) / 128.0f;
                } else if (bits_per_sample == 16) {
                    int16_t v = (int16_t)read_u16_le(sp);
                    s = (float)v / 32768.0f;
                } else if (bits_per_sample == 24) {
                    int32_t v = (int32_t)sp[0] | ((int32_t)sp[1] << 8) | ((int32_t)sp[2] << 16);
                    if (v & 0x800000) {
                        v |= ~0xFFFFFF;
                    }
                    s = (float)v / 8388608.0f;
                } else if (bits_per_sample == 32) {
                    int32_t v = (int32_t)read_u32_le(sp);
                    s = (float)((double)v / 2147483648.0);
                }
            } else if (audio_fmt == 3u && bits_per_sample == 32) {
                memcpy(&s, sp, sizeof(float));
                if (!isfinite(s)) {
                    s = 0.0f;
                }
            } else if (audio_fmt == 3u && bits_per_sample == 64) {
                double d = 0.0;
                memcpy(&d, sp, sizeof(double));
                s = isfinite(d) ? (float)d : 0.0f;
            }
            if (s < -1.0f) {
                s = -1.0f;
            }
            if (s > 1.0f) {
                s = 1.0f;
            }
            ch_val[c] = s;
        }
        if (channels == 1) {
            ch_val[1] = ch_val[0];
        }
        src_stereo[f * 2u + 0u] = ch_val[0];
        src_stereo[f * 2u + 1u] = ch_val[1];
    }

    return resample_stereo_f32_to_48k(src_stereo, src_frames, sample_rate, out_stereo_f32,
                                      out_frame_count);
}

#ifdef WE_HAVE_LIBAV_AUDIO
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} we_avio_mem_t;

static int we_avio_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    we_avio_mem_t *m = (we_avio_mem_t *)opaque;
    if (!m || m->pos >= m->size || buf_size <= 0) {
        return AVERROR_EOF;
    }
    size_t rem = m->size - m->pos;
    size_t to_copy = (size_t)buf_size < rem ? (size_t)buf_size : rem;
    memcpy(buf, m->data + m->pos, to_copy);
    m->pos += to_copy;
    return (int)to_copy;
}

static int64_t we_avio_seek(void *opaque, int64_t offset, int whence)
{
    we_avio_mem_t *m = (we_avio_mem_t *)opaque;
    if (!m) {
        return -1;
    }
    if (whence == AVSEEK_SIZE) {
        return (int64_t)m->size;
    }
    int64_t new_pos = 0;
    int base = whence & ~AVSEEK_FORCE;
    if (base == SEEK_SET) {
        new_pos = offset;
    } else if (base == SEEK_CUR) {
        new_pos = (int64_t)m->pos + offset;
    } else if (base == SEEK_END) {
        new_pos = (int64_t)m->size + offset;
    } else {
        return -1;
    }
    if (new_pos < 0 || (size_t)new_pos > m->size) {
        return -1;
    }
    m->pos = (size_t)new_pos;
    return new_pos;
}

static void *we_dlopen_any(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            return h;
        }
    }
    return NULL;
}
#endif

static bool we_audio_decode_ffmpeg_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                                          size_t *out_frame_count)
{
#ifdef WE_HAVE_LIBAV_AUDIO
    if (!data || size < 4 || !out_stereo_f32 || !out_frame_count) {
        return false;
    }
    static const char *const avformat_names[] = {"libavformat.so.61", "libavformat.so.60",
                                                 "libavformat.so.59", "libavformat.so"};
    static const char *const avcodec_names[] = {"libavcodec.so.61", "libavcodec.so.60",
                                                "libavcodec.so.59", "libavcodec.so"};
    static const char *const avutil_names[] = {"libavutil.so.59", "libavutil.so.58",
                                               "libavutil.so.57", "libavutil.so"};

    void *h_avf = we_dlopen_any(avformat_names, sizeof(avformat_names) / sizeof(avformat_names[0]));
    void *h_avc = we_dlopen_any(avcodec_names, sizeof(avcodec_names) / sizeof(avcodec_names[0]));
    void *h_avu = we_dlopen_any(avutil_names, sizeof(avutil_names) / sizeof(avutil_names[0]));
    if (!h_avf || !h_avc || !h_avu) {
        if (h_avf)
            dlclose(h_avf);
        if (h_avc)
            dlclose(h_avc);
        if (h_avu)
            dlclose(h_avu);
        return false;
    }

    void *(*p_av_malloc)(size_t) = NULL;
    void (*p_av_freep)(void *) = NULL;
    AVFrame *(*p_av_frame_alloc)(void) = NULL;
    void (*p_av_frame_free)(AVFrame **) = NULL;
    void (*p_av_frame_unref)(AVFrame *) = NULL;

    const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID) = NULL;
    AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *) = NULL;
    void (*p_avcodec_free_context)(AVCodecContext **) = NULL;
    int (*p_avcodec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *) = NULL;
    int (*p_avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **) = NULL;
    int (*p_avcodec_send_packet)(AVCodecContext *, const AVPacket *) = NULL;
    int (*p_avcodec_receive_frame)(AVCodecContext *, AVFrame *) = NULL;
    AVPacket *(*p_av_packet_alloc)(void) = NULL;
    void (*p_av_packet_free)(AVPacket **) = NULL;
    void (*p_av_packet_unref)(AVPacket *) = NULL;

    AVIOContext *(*p_avio_alloc_context)(unsigned char *, int, int, void *,
                                         int (*)(void *, uint8_t *, int),
                                         int (*)(void *, const uint8_t *, int),
                                         int64_t (*)(void *, int64_t, int)) = NULL;
    void (*p_avio_context_free)(AVIOContext **) = NULL;
    AVFormatContext *(*p_avformat_alloc_context)(void) = NULL;
    int (*p_avformat_open_input)(AVFormatContext **, const char *, const AVInputFormat *,
                                 AVDictionary **) = NULL;
    int (*p_avformat_find_stream_info)(AVFormatContext *, AVDictionary **) = NULL;
    int (*p_av_find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **,
                                 int) = NULL;
    int (*p_av_read_frame)(AVFormatContext *, AVPacket *) = NULL;
    void (*p_avformat_close_input)(AVFormatContext **) = NULL;

    *(void **)(&p_av_malloc) = dlsym(h_avu, "av_malloc");
    *(void **)(&p_av_freep) = dlsym(h_avu, "av_freep");
    *(void **)(&p_av_frame_alloc) = dlsym(h_avu, "av_frame_alloc");
    *(void **)(&p_av_frame_free) = dlsym(h_avu, "av_frame_free");
    *(void **)(&p_av_frame_unref) = dlsym(h_avu, "av_frame_unref");

    *(void **)(&p_avcodec_find_decoder) = dlsym(h_avc, "avcodec_find_decoder");
    *(void **)(&p_avcodec_alloc_context3) = dlsym(h_avc, "avcodec_alloc_context3");
    *(void **)(&p_avcodec_free_context) = dlsym(h_avc, "avcodec_free_context");
    *(void **)(&p_avcodec_parameters_to_context) = dlsym(h_avc, "avcodec_parameters_to_context");
    *(void **)(&p_avcodec_open2) = dlsym(h_avc, "avcodec_open2");
    *(void **)(&p_avcodec_send_packet) = dlsym(h_avc, "avcodec_send_packet");
    *(void **)(&p_avcodec_receive_frame) = dlsym(h_avc, "avcodec_receive_frame");
    *(void **)(&p_av_packet_alloc) = dlsym(h_avc, "av_packet_alloc");
    *(void **)(&p_av_packet_free) = dlsym(h_avc, "av_packet_free");
    *(void **)(&p_av_packet_unref) = dlsym(h_avc, "av_packet_unref");

    *(void **)(&p_avio_alloc_context) = dlsym(h_avf, "avio_alloc_context");
    *(void **)(&p_avio_context_free) = dlsym(h_avf, "avio_context_free");
    *(void **)(&p_avformat_alloc_context) = dlsym(h_avf, "avformat_alloc_context");
    *(void **)(&p_avformat_open_input) = dlsym(h_avf, "avformat_open_input");
    *(void **)(&p_avformat_find_stream_info) = dlsym(h_avf, "avformat_find_stream_info");
    *(void **)(&p_av_find_best_stream) = dlsym(h_avf, "av_find_best_stream");
    *(void **)(&p_av_read_frame) = dlsym(h_avf, "av_read_frame");
    *(void **)(&p_avformat_close_input) = dlsym(h_avf, "avformat_close_input");

    if (!p_av_malloc || !p_av_freep || !p_av_frame_alloc || !p_av_frame_free || !p_av_frame_unref ||
        !p_avcodec_find_decoder || !p_avcodec_alloc_context3 || !p_avcodec_free_context ||
        !p_avcodec_parameters_to_context || !p_avcodec_open2 || !p_avcodec_send_packet ||
        !p_avcodec_receive_frame || !p_av_packet_alloc || !p_av_packet_free ||
        !p_av_packet_unref || !p_avio_alloc_context || !p_avio_context_free ||
        !p_avformat_alloc_context || !p_avformat_open_input || !p_avformat_find_stream_info ||
        !p_av_find_best_stream || !p_av_read_frame || !p_avformat_close_input) {
        dlclose(h_avf);
        dlclose(h_avc);
        dlclose(h_avu);
        return false;
    }

    bool ok = false;
    we_avio_mem_t mem_reader = {.data = data, .size = size, .pos = 0};
    unsigned char *avio_buf = (unsigned char *)p_av_malloc(32768);
    AVIOContext *avio_ctx = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    float *pcm = NULL;
    size_t count_frames = 0;
    uint32_t sample_rate = 0;

    if (!avio_buf) {
        goto cleanup;
    }
    avio_ctx = p_avio_alloc_context(avio_buf, 32768, 0, &mem_reader, we_avio_read_packet, NULL,
                                    we_avio_seek);
    if (!avio_ctx) {
        p_av_freep(&avio_buf);
        goto cleanup;
    }
    fmt_ctx = p_avformat_alloc_context();
    if (!fmt_ctx) {
        goto cleanup;
    }
    fmt_ctx->pb = avio_ctx;
    fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (p_avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
        fmt_ctx = NULL;
        goto cleanup;
    }
    if (p_avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        goto cleanup;
    }
    int stream_idx = p_av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (stream_idx < 0 || (unsigned)stream_idx >= fmt_ctx->nb_streams) {
        goto cleanup;
    }
    AVStream *st = fmt_ctx->streams[stream_idx];
    const AVCodec *codec = p_avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        goto cleanup;
    }
    codec_ctx = p_avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        goto cleanup;
    }
    if (p_avcodec_parameters_to_context(codec_ctx, st->codecpar) < 0) {
        goto cleanup;
    }
    if (p_avcodec_open2(codec_ctx, codec, NULL) < 0) {
        goto cleanup;
    }

    if (codec_ctx->sample_rate < 4000 || codec_ctx->sample_rate > 384000) {
        goto cleanup;
    }
    sample_rate = (uint32_t)codec_ctx->sample_rate;
    size_t max_frames = (size_t)sample_rate * 300u;
    size_t cap_frames = 4096u;
    pcm = malloc(cap_frames * 2u * sizeof(float));
    pkt = p_av_packet_alloc();
    frame = p_av_frame_alloc();
    if (!pcm || !pkt || !frame) {
        goto cleanup;
    }

    while (count_frames < max_frames && p_av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            if (p_avcodec_send_packet(codec_ctx, pkt) >= 0) {
                while (p_avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    int nb = frame->nb_samples;
                    int ch = frame->ch_layout.nb_channels;
                    if (nb > 0 && ch > 0 && count_frames < max_frames) {
                        size_t take = (size_t)nb;
                        if (count_frames + take > max_frames) {
                            take = max_frames - count_frames;
                        }
                        if (count_frames + take > cap_frames) {
                            size_t new_cap = cap_frames * 2u;
                            while (new_cap < count_frames + take) {
                                new_cap *= 2u;
                            }
                            float *np = realloc(pcm, new_cap * 2u * sizeof(float));
                            if (!np) {
                                p_av_frame_unref(frame);
                                break;
                            }
                            pcm = np;
                            cap_frames = new_cap;
                        }
                        enum AVSampleFormat fmt = (enum AVSampleFormat)frame->format;
                        for (size_t s = 0; s < take; ++s) {
                            float lr[2] = {0.0f, 0.0f};
                            int read_ch = (ch >= 2) ? 2 : 1;
                            for (int c = 0; c < read_ch; ++c) {
                                float v = 0.0f;
                                switch (fmt) {
                                case AV_SAMPLE_FMT_FLTP:
                                    v = ((const float *)frame->data[c])[s];
                                    break;
                                case AV_SAMPLE_FMT_FLT:
                                    v = ((const float *)frame->data[0])[s * (size_t)ch + (size_t)c];
                                    break;
                                case AV_SAMPLE_FMT_DBLP:
                                    v = (float)((const double *)frame->data[c])[s];
                                    break;
                                case AV_SAMPLE_FMT_DBL:
                                    v = (float)((const double *)frame->data[0])[s * (size_t)ch +
                                                                                (size_t)c];
                                    break;
                                case AV_SAMPLE_FMT_S16P:
                                    v = (float)((const int16_t *)frame->data[c])[s] / 32768.0f;
                                    break;
                                case AV_SAMPLE_FMT_S16:
                                    v = (float)((const int16_t *)frame->data[0])[s * (size_t)ch +
                                                                                 (size_t)c] /
                                        32768.0f;
                                    break;
                                case AV_SAMPLE_FMT_S32P:
                                    v = (float)((double)((const int32_t *)frame->data[c])[s] /
                                                2147483648.0);
                                    break;
                                case AV_SAMPLE_FMT_S32:
                                    v = (float)((double)((const int32_t *)
                                                             frame->data[0])[s * (size_t)ch +
                                                                             (size_t)c] /
                                                2147483648.0);
                                    break;
                                case AV_SAMPLE_FMT_U8P:
                                    v = ((float)((const uint8_t *)frame->data[c])[s] - 128.0f) /
                                        128.0f;
                                    break;
                                case AV_SAMPLE_FMT_U8:
                                    v = ((float)((const uint8_t *)frame->data[0])[s * (size_t)ch +
                                                                                  (size_t)c] -
                                         128.0f) /
                                        128.0f;
                                    break;
                                default:
                                    v = 0.0f;
                                    break;
                                }
                                if (!isfinite(v))
                                    v = 0.0f;
                                if (v < -1.0f)
                                    v = -1.0f;
                                if (v > 1.0f)
                                    v = 1.0f;
                                lr[c] = v;
                            }
                            if (ch == 1) {
                                lr[1] = lr[0];
                            }
                            pcm[(count_frames + s) * 2u + 0u] = lr[0];
                            pcm[(count_frames + s) * 2u + 1u] = lr[1];
                        }
                        count_frames += take;
                    }
                    p_av_frame_unref(frame);
                }
            }
        }
        p_av_packet_unref(pkt);
    }

    if (count_frames > 0 && pcm) {
        ok = resample_stereo_f32_to_48k(pcm, count_frames, sample_rate, out_stereo_f32,
                                        out_frame_count);
        pcm = NULL;
    }

cleanup:
    free(pcm);
    if (frame)
        p_av_frame_free(&frame);
    if (pkt)
        p_av_packet_free(&pkt);
    if (codec_ctx)
        p_avcodec_free_context(&codec_ctx);
    if (fmt_ctx)
        p_avformat_close_input(&fmt_ctx);
    if (avio_ctx) {
        if (avio_ctx->buffer) {
            p_av_freep(&avio_ctx->buffer);
        }
        p_avio_context_free(&avio_ctx);
    }
    dlclose(h_avf);
    dlclose(h_avc);
    dlclose(h_avu);
    return ok;
#else
    (void)data;
    (void)size;
    (void)out_stereo_f32;
    (void)out_frame_count;
    return false;
#endif
}

bool we_audio_decode_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                            size_t *out_frame_count)
{
    if (we_audio_decode_wav_memory(data, size, out_stereo_f32, out_frame_count)) {
        return true;
    }
    return we_audio_decode_ffmpeg_memory(data, size, out_stereo_f32, out_frame_count);
}

bool we_audio_decode_pkg_file(we_pkg_t *pkg, const char *vfs_dir, const char *rel_path,
                              float **out_stereo_f32, size_t *out_frame_count)
{
    if (!rel_path || rel_path[0] == '\0' || !out_stereo_f32 || !out_frame_count) {
        return false;
    }
    *out_stereo_f32 = NULL;
    *out_frame_count = 0;

    size_t sz = 0;
    bool owned = false;
    const uint8_t *bytes = NULL;
    if (pkg) {
        bytes = we_pkg_resolve(pkg, rel_path, &sz, &owned);
    }
    if (!bytes && vfs_dir && vfs_dir[0] != '\0') {
        char norm[512];
        if (we_pkg_normalize_path(rel_path, strlen(rel_path), norm, sizeof(norm))) {
            char full[4096];
            if (snprintf(full, sizeof(full), "%s/%s", vfs_dir, norm) < (int)sizeof(full)) {
                FILE *fp = fopen(full, "rb");
                if (fp) {
                    if (fseek(fp, 0, SEEK_END) == 0) {
                        long fsz = ftell(fp);
                        if (fsz > 0 && fseek(fp, 0, SEEK_SET) == 0) {
                            uint8_t *buf = malloc((size_t)fsz);
                            if (buf && fread(buf, 1, (size_t)fsz, fp) == (size_t)fsz) {
                                bytes = buf;
                                sz = (size_t)fsz;
                                owned = true;
                            } else {
                                free(buf);
                            }
                        }
                    }
                    fclose(fp);
                }
            }
        }
    }

    if (!bytes || sz == 0) {
        if (owned && bytes) {
            free((void *)bytes);
        }
        return false;
    }

    bool ok = we_audio_decode_memory(bytes, sz, out_stereo_f32, out_frame_count);

    if (owned) {
        free((void *)bytes);
    }
    return ok;
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
            case WE_SCRIPT_PROP_TEXT:
                ncomp = 0;
                break;
            case WE_SCRIPT_PROP_POINTSIZE:
                in_val[0] = (float)obj->text_pointsize;
                ncomp = 1;
                break;
            case WE_SCRIPT_PROP_VOLUME:
                in_val[0] = obj->sound_volume;
                ncomp = 1;
                break;
            }

            if (ncomp > 0) {
                memcpy(out_val, in_val, ncomp * sizeof(float));
            }
            if (we_script_eval_init(sc->vm, &env, &ls, ncomp > 0 ? in_val : NULL, ncomp,
                                    ncomp > 0 ? out_val : NULL)) {
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
                if (ncomp > 0 && (sc->target == WE_SCRIPT_PROP_UNIFORM ||
                                  memcmp(out_val, in_val, ncomp * sizeof(float)) != 0)) {
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
                    case WE_SCRIPT_PROP_TEXT:
                        break;
                    case WE_SCRIPT_PROP_POINTSIZE:
                        if ((int32_t)out_val[0] != obj->text_pointsize && out_val[0] >= 4.0f) {
                            obj->text_pointsize = (int32_t)out_val[0];
                            obj->text_dirty = true;
                        }
                        break;
                    case WE_SCRIPT_PROP_VOLUME:
                        obj->sound_volume = fminf(fmaxf(out_val[0], 0.0f), 2.0f);
                        break;
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->is_text_object && obj->text_dirty) {
            we_scene_update_text_texture(scene, obj);
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
    obj->sound_volume = 1.0f;
    obj->text_pointsize = 32;
    obj->text_padding = 2;

    /* 1. Steam Wallpaper Engine "sound" object */
    const we_jval_t *snd_node = jobj_get(onode, "sound");
    if (snd_node && snd_node->type == JVAL_OBJ && jobj_get(snd_node, "value")) {
        snd_node = jobj_get(snd_node, "value");
    }
    if (snd_node &&
        ((snd_node->type == JVAL_ARR && snd_node->arr.count > 0) ||
         (snd_node->type == JVAL_STR && snd_node->str && snd_node->str[0] != '\0'))) {
        obj->is_sound_object = true;
        obj->sound_volume = jval_float(jobj_get(onode, "volume"), &scene->project, 1.0f,
                                       obj->user_prop_volume, sizeof(obj->user_prop_volume));
        obj->sound_startsilent =
            jval_bool_resolved(jobj_get(onode, "startsilent"), &scene->project, false, NULL, 0);
        obj->sound_mintime =
            fmaxf(0.0f, jval_float(jobj_get(onode, "mintime"), &scene->project, 0.0f, NULL, 0));
        obj->sound_maxtime =
            fmaxf(0.0f, jval_float(jobj_get(onode, "maxtime"), &scene->project, 0.0f, NULL, 0));

        const char *pm = jval_str(jobj_get(onode, "playbackmode"), "loop");
        if (pm && (strcasecmp(pm, "single") == 0 || strcasecmp(pm, "once") == 0)) {
            obj->sound_mode = WE_SOUND_MODE_SINGLE;
        } else if (pm && strcasecmp(pm, "random") == 0) {
            obj->sound_mode = WE_SOUND_MODE_RANDOM;
        } else {
            obj->sound_mode = WE_SOUND_MODE_LOOP;
        }

        size_t item_count = (snd_node->type == JVAL_ARR) ? snd_node->arr.count : 1u;
        for (size_t i = 0; i < item_count && obj->sound_track_count < WE_MAX_SOUND_TRACKS; ++i) {
            const we_jval_t *item =
                (snd_node->type == JVAL_ARR) ? snd_node->arr.items[i] : snd_node;
            const char *spath = jval_str(item, NULL);
            if (!spath && item && item->type == JVAL_OBJ) {
                spath = jval_str(jobj_get(item, "file"), NULL);
            }
            if (spath && spath[0] != '\0') {
                float *pcm = NULL;
                size_t frames = 0;
                if (we_audio_decode_pkg_file(scene->pkg, scene->project.workshop_dir, spath, &pcm,
                                             &frames) &&
                    pcm && frames > 0) {
                    obj->sound_tracks_pcm[obj->sound_track_count] = pcm;
                    obj->sound_tracks_frames[obj->sound_track_count] = frames;
                    obj->sound_track_count++;
                }
            }
        }
        obj->sound_playing =
            (obj->sound_track_count > 0 && !obj->sound_startsilent && obj->visible);
        if (obj->sound_track_count > 0) {
            scene->uses_audio = true;
        }
    }

    /* 2. Steam Wallpaper Engine "text" layer */
    const we_jval_t *tnode = jobj_get(onode, "text");
    if (tnode && (tnode->type == JVAL_STR || tnode->type == JVAL_OBJ)) {
        obj->is_text_object = true;
        const char *raw_txt = NULL;
        if (tnode->type == JVAL_STR) {
            raw_txt = tnode->str;
        } else if (tnode->type == JVAL_OBJ) {
            const we_jval_t *u = jobj_get(tnode, "user");
            if (u && u->type == JVAL_STR && u->str) {
                snprintf(obj->user_prop_text, sizeof(obj->user_prop_text), "%s", u->str);
                const we_user_prop_t *up = project_find_prop(&scene->project, u->str);
                if (up && up->type == WE_PROP_TEXT && up->str_val[0] != '\0') {
                    raw_txt = up->str_val;
                }
            } else if (u && u->type == JVAL_OBJ) {
                const char *uname = jval_str(jobj_get(u, "name"), NULL);
                if (uname) {
                    snprintf(obj->user_prop_text, sizeof(obj->user_prop_text), "%s", uname);
                    const we_user_prop_t *up = project_find_prop(&scene->project, uname);
                    if (up && up->type == WE_PROP_TEXT && up->str_val[0] != '\0') {
                        raw_txt = up->str_val;
                    }
                }
            }
            if (!raw_txt) {
                raw_txt = jval_str(jobj_get(tnode, "value"), "");
            }
        }
        snprintf(obj->text_str, sizeof(obj->text_str), "%s", raw_txt ? raw_txt : "");

        const char *font_str = jval_str(jobj_get(onode, "font"), NULL);
        if (!font_str && tnode->type == JVAL_OBJ) {
            font_str = jval_str(jobj_get(tnode, "font"), NULL);
        }
        snprintf(obj->text_font, sizeof(obj->text_font), "%s",
                 font_str ? font_str : "monospace");

        const we_jval_t *ps_node = jobj_get(onode, "pointsize");
        if (!ps_node && tnode->type == JVAL_OBJ) {
            ps_node = jobj_get(tnode, "pointsize");
        }
        float ps = jval_float(ps_node, &scene->project, 32.0f, obj->user_prop_pointsize,
                              sizeof(obj->user_prop_pointsize));
        obj->text_pointsize = (ps >= 4.0f) ? (int32_t)ps : 32;

        const char *ha = jval_str(jobj_get(onode, "horizontalalign"), NULL);
        if (!ha && tnode->type == JVAL_OBJ) {
            ha = jval_str(jobj_get(tnode, "horizontalalign"), NULL);
        }
        snprintf(obj->text_halign, sizeof(obj->text_halign), "%s", ha ? ha : "center");

        const char *va = jval_str(jobj_get(onode, "verticalalign"), NULL);
        if (!va && tnode->type == JVAL_OBJ) {
            va = jval_str(jobj_get(tnode, "verticalalign"), NULL);
        }
        snprintf(obj->text_valign, sizeof(obj->text_valign), "%s", va ? va : "center");

        const we_jval_t *pad_node = jobj_get(onode, "padding");
        if (!pad_node && tnode->type == JVAL_OBJ) {
            pad_node = jobj_get(tnode, "padding");
        }
        if (pad_node) {
            int32_t pad_v = (int32_t)jval_float(pad_node, &scene->project, 2.0f, NULL, 0);
            obj->text_padding = (pad_v >= 0) ? pad_v : 0;
        }

        const we_jval_t *obg_node = jobj_get(onode, "opaquebackground");
        if (!obg_node && tnode->type == JVAL_OBJ) {
            obg_node = jobj_get(tnode, "opaquebackground");
        }
        obj->text_opaque_bg = jval_bool_resolved(obg_node, &scene->project, false, NULL, 0);

        const we_jval_t *bgc_node = jobj_get(onode, "backgroundcolor");
        if (!bgc_node && tnode->type == JVAL_OBJ) {
            bgc_node = jobj_get(tnode, "backgroundcolor");
        }
        obj->text_bg_color[0] = 0.0f;
        obj->text_bg_color[1] = 0.0f;
        obj->text_bg_color[2] = 0.0f;
        if (bgc_node) {
            jval_floats(bgc_node, &scene->project, obj->text_bg_color, 3, NULL, 0);
        }

        if (!has_explicit_size) {
            obj->size[0] = 512.0f;
            obj->size[1] = 128.0f;
            has_explicit_size = true;
        }

        obj->has_base_pass = true;
        obj->base_pass.blending = WE_BLEND_NORMAL;
        snprintf(obj->base_pass.shader_name, sizeof(obj->base_pass.shader_name), "genericimage2");
        snprintf(obj->base_pass.tex_paths[0], sizeof(obj->base_pass.tex_paths[0]), "_we_text_%d",
                 obj->id);
        we_scene_update_text_texture(scene, obj);
        finalize_material_pass_cpu(&obj->base_pass, scene->pkg);

        attach_object_script(scene, obj, WE_SCRIPT_PROP_TEXT, NULL, tnode, NULL);
        if (ps_node) {
            attach_object_script(scene, obj, WE_SCRIPT_PROP_POINTSIZE, NULL, ps_node, NULL);
        }
    }

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

    if (mat_path[0] != '\0' && !obj->passthrough && !obj->is_text_object) {
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

    attach_object_script(scene, obj, WE_SCRIPT_PROP_ORIGIN, NULL, jobj_get(onode, "origin"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_SCALE, NULL, jobj_get(onode, "scale"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_ANGLES, NULL, jobj_get(onode, "angles"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_ALPHA, NULL, jobj_get(onode, "alpha"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_COLOR, NULL, jobj_get(onode, "color"), NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_VISIBLE, NULL, jobj_get(onode, "visible"),
                         NULL);
    attach_object_script(scene, obj, WE_SCRIPT_PROP_VOLUME, NULL, jobj_get(onode, "volume"), NULL);
    const we_jval_t *top_script = jobj_get(onode, "script");
    if (top_script) {
        attach_object_script(scene, obj,
                             obj->is_text_object ? WE_SCRIPT_PROP_TEXT : WE_SCRIPT_PROP_ORIGIN,
                             NULL, top_script, jval_str(top_script, NULL));
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
                    if (epasses && epasses->type == JVAL_ARR && epasses->arr.count > 0) {
                        size_t cap_passes = epasses->arr.count < WE_MAX_EFFECT_PASSES
                                                ? epasses->arr.count
                                                : WE_MAX_EFFECT_PASSES;
                        eff->passes =
                            (we_material_pass_t *)calloc(cap_passes, sizeof(we_material_pass_t));
                        if (eff->passes) {
                            for (size_t p = 0; p < cap_passes; p++) {
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
                                    for (size_t b = 0; b < binds->arr.count &&
                                                       mp->bind_count < WE_MAX_PASS_TEXTURES;
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
                                    load_material_pass_from_file(mp, scene->pkg, emat,
                                                                 &scene->project);
                                }
                                parse_pass_json_overrides(mp, epn, &scene->project);
                                if (opasses && opasses->type == JVAL_ARR &&
                                    p < opasses->arr.count) {
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
                    }
                    jval_free(eroot);
                }
                obj->effect_count++;
            }
        }
    }

    obj->needs_full_fb_copy = obj->copybackground;
    if (!obj->needs_full_fb_copy && obj->has_base_pass) {
        for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
            if (obj->base_pass.tex_paths[s][0] != '\0' &&
                strncasecmp(obj->base_pass.tex_paths[s], "_rt_FullFrameBuffer", 19) == 0) {
                obj->needs_full_fb_copy = true;
                break;
            }
        }
        for (uint32_t u = 0; !obj->needs_full_fb_copy && u < obj->base_pass.prep.meta.uniform_count;
             u++) {
            if (strncasecmp(obj->base_pass.prep.meta.uniforms[u].default_tex, "_rt_FullFrameBuffer",
                            19) == 0) {
                obj->needs_full_fb_copy = true;
            }
        }
    }
    for (uint32_t e = 0; !obj->needs_full_fb_copy && e < obj->effect_count; e++) {
        const we_effect_t *eff = &obj->effects[e];
        for (uint32_t p = 0; !obj->needs_full_fb_copy && p < eff->pass_count; p++) {
            const we_material_pass_t *mp = &eff->passes[p];
            for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
                if (mp->tex_paths[s][0] != '\0' &&
                    strncasecmp(mp->tex_paths[s], "_rt_FullFrameBuffer", 19) == 0) {
                    obj->needs_full_fb_copy = true;
                    break;
                }
            }
            for (uint32_t b = 0; !obj->needs_full_fb_copy && b < mp->bind_count; b++) {
                if (strncasecmp(mp->binds[b].target_name, "_rt_FullFrameBuffer", 19) == 0) {
                    obj->needs_full_fb_copy = true;
                }
            }
            for (uint32_t u = 0; !obj->needs_full_fb_copy && u < mp->prep.meta.uniform_count; u++) {
                if (strncasecmp(mp->prep.meta.uniforms[u].default_tex, "_rt_FullFrameBuffer", 19) ==
                    0) {
                    obj->needs_full_fb_copy = true;
                }
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
    scene->sound_rng_state = 0x9E3779B97F4A7C15ULL;
    scene->last_audio_step_time = -1.0;

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
            size_t obj_cap =
                objs->arr.count < WE_SCENE_MAX_OBJECTS ? objs->arr.count : WE_SCENE_MAX_OBJECTS;
            scene->objects = calloc(obj_cap, sizeof(we_scene_object_t));
            if (scene->objects) {
                for (size_t i = 0; i < objs->arr.count && scene->object_count < obj_cap; i++) {
                    if (objs->arr.items[i] && objs->arr.items[i]->type == JVAL_OBJ) {
                        parse_scene_object(scene, objs->arr.items[i],
                                           &scene->objects[scene->object_count++]);
                    }
                }
                init_scene_scripts(scene);
                for (uint32_t i = 0; i < scene->object_count; i++) {
                    if (scene->objects[i].is_text_object && scene->objects[i].text_dirty) {
                        we_scene_update_text_texture(scene, &scene->objects[i]);
                    }
                }
            }
        }
        jval_free(sroot);
    } else {
        if (sroot) {
            jval_free(sroot);
        }
        scene->camera_eye[0] = scene->ortho_width * 0.5f;
        scene->camera_eye[1] = scene->ortho_height * 0.5f;
        scene->camera_eye[2] = -1000.0f;
        scene->camera_center[0] = scene->ortho_width * 0.5f;
        scene->camera_center[1] = scene->ortho_height * 0.5f;
        scene->camera_center[2] = 0.0f;
    }

    if (scene->object_count == 0) {
        uint8_t *bgra_px = NULL;
        uint32_t iw = 0, ih = 0;

        if (scene->project.file[0] != '\0' && strchr(scene->project.file, '/') == NULL &&
            strchr(scene->project.file, '\\') == NULL && strcmp(scene->project.file, ".") != 0 &&
            strcmp(scene->project.file, "..") != 0) {
            const char *fdot = strrchr(scene->project.file, '.');
            bool is_meta_ext =
                fdot && (strcasecmp(fdot, ".json") == 0 || strcasecmp(fdot, ".pkg") == 0 ||
                         strcasecmp(fdot, ".html") == 0 || strcasecmp(fdot, ".htm") == 0);
            if (!is_meta_ext) {
                char fpath[4096];
                struct stat fst;
                if (snprintf(fpath, sizeof(fpath), "%s/%s", resolved_dir, scene->project.file) <
                        (int)sizeof(fpath) &&
                    stat(fpath, &fst) == 0 && S_ISREG(fst.st_mode) && fst.st_size > 0) {
                    (void)image_load(fpath, &bgra_px, &iw, &ih);
                }
                if (!bgra_px && scene->pkg) {
                    size_t pkg_sz = 0;
                    bool pkg_owned = false;
                    const uint8_t *pkg_data =
                        we_pkg_resolve(scene->pkg, scene->project.file, &pkg_sz, &pkg_owned);
                    if (pkg_data && pkg_sz > 0) {
                        raw_image_t rimg;
                        if (image_load_from_memory(pkg_data, pkg_sz, &rimg) && rimg.data &&
                            rimg.width > 0 && rimg.height > 0) {
                            bgra_px = rimg.data;
                            iw = rimg.width;
                            ih = rimg.height;
                        }
                    }
                    if (pkg_owned && pkg_data) {
                        free((void *)pkg_data);
                    }
                }
            }
        }

        if (!bgra_px) {
            char prev_path[4096];
            if (we_workshop_find_preview_file(resolved_dir, prev_path, sizeof(prev_path))) {
                (void)image_load(prev_path, &bgra_px, &iw, &ih);
            }
        }

        if (bgra_px && iw > 0 && ih > 0) {
            we_tex_t *tex = (we_tex_t *)calloc(1, sizeof(we_tex_t));
            we_tex_image_t *timg = (we_tex_image_t *)calloc(1, sizeof(we_tex_image_t));
            we_tex_mip_t *tmip = (we_tex_mip_t *)calloc(1, sizeof(we_tex_mip_t));
            we_scene_object_t *objs_arr = (we_scene_object_t *)calloc(1, sizeof(we_scene_object_t));
            if (tex && timg && tmip && objs_arr) {
                if (scene->objects) {
                    free(scene->objects);
                }
                scene->objects = objs_arr;
                scene->object_count = 1;

                tmip->width = iw;
                tmip->height = ih;
                tmip->rgba8 = bgra_px;
                tmip->is_bgra_swiz = true;

                timg->mip_count = 1;
                timg->mips = tmip;

                tex->format = WE_TEX_FMT_RGBA8888;
                tex->flags = WE_TEX_FLAG_CLAMP_UVS;
                tex->texture_width = iw;
                tex->texture_height = ih;
                tex->image_width = iw;
                tex->image_height = ih;
                tex->image_count = 1;
                tex->images = timg;

                we_scene_object_t *obj = &scene->objects[0];
                snprintf(obj->name, sizeof(obj->name), "fallback_preview_quad");
                obj->id = 0;
                obj->parent_id = -1;
                obj->origin[0] = scene->ortho_width * 0.5f;
                obj->origin[1] = scene->ortho_height * 0.5f;
                obj->origin[2] = 0.0f;
                obj->scale[0] = 1.0f;
                obj->scale[1] = 1.0f;
                obj->scale[2] = 1.0f;
                obj->size[0] = scene->ortho_width;
                obj->size[1] = scene->ortho_height;
                obj->alpha = 1.0f;
                obj->brightness = 1.0f;
                obj->color[0] = 1.0f;
                obj->color[1] = 1.0f;
                obj->color[2] = 1.0f;
                obj->visible = true;
                obj->fullscreen = true;
                obj->has_base_pass = true;
                obj->base_pass.blending = WE_BLEND_NORMAL;
                snprintf(obj->base_pass.shader_name, sizeof(obj->base_pass.shader_name),
                         "genericimage2");
                snprintf(obj->base_pass.tex_paths[0], sizeof(obj->base_pass.tex_paths[0]),
                         "_fallback_img");
                obj->base_pass.textures[0] = tex;
                finalize_material_pass_cpu(&obj->base_pass, scene->pkg);
            } else {
                free(tex);
                free(timg);
                free(tmip);
                free(objs_arr);
                image_free(bgra_px);
            }
        } else if (bgra_px) {
            image_free(bgra_px);
        }
    }

    return scene;
}
