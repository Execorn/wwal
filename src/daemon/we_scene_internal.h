// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SCENE_INTERNAL_H
#define WAYWAL_WE_SCENE_INTERNAL_H

#include "waywal/we_json.h"
#include "waywal/we_scene.h"

static inline void jval_free(we_jval_t *v) { we_jval_free(v); }

static inline we_jval_t *json_parse_root(const char *src, size_t len)
{
    return we_json_parse_root(src, len);
}

static inline const we_jval_t *jobj_get(const we_jval_t *obj, const char *key)
{
    return we_jobj_get(obj, key);
}

static inline const char *jval_str(const we_jval_t *v, const char *def)
{
    return we_jval_str(v, def);
}

static inline const we_user_prop_t *project_find_prop(const we_project_t *proj, const char *key)
{
    return we_project_find_prop(proj, key);
}

static inline uint32_t jval_floats(const we_jval_t *v, const we_project_t *proj, float *out,
                                   uint32_t max_n, char *out_user_prop, size_t prop_cap)
{
    return we_jval_floats(v, proj, out, max_n, out_user_prop, prop_cap);
}

static inline float jval_float(const we_jval_t *v, const we_project_t *proj, float def,
                               char *out_user_prop, size_t prop_cap)
{
    return we_jval_float(v, proj, def, out_user_prop, prop_cap);
}

static inline bool jval_bool_resolved(const we_jval_t *v, const we_project_t *proj, bool def,
                                      char *out_user_prop, size_t prop_cap)
{
    return we_jval_bool_resolved(v, proj, def, out_user_prop, prop_cap);
}

void sync_layer_from_obj(const we_scene_object_t *obj, we_script_layer_state_t *ls);
void sync_obj_from_layer(we_scene_object_t *obj, const we_script_layer_state_t *ls);
void sync_scene_layers_to_vm(const we_scene_t *scene, we_script_t *vm);
void sync_scene_layers_from_vm(we_scene_t *scene, const we_script_t *vm);
void build_script_env(const we_scene_t *scene, uint32_t canvas_w, uint32_t canvas_h, float dt_sec,
                      float time_sec, const waywal_shader_frame_params_t *params,
                      we_script_env_t *env);
void bind_project_props_to_script(we_script_t *vm, const we_project_t *proj);
void apply_scene_script_user_properties(we_scene_t *scene);

#endif /* WAYWAL_WE_SCENE_INTERNAL_H */
