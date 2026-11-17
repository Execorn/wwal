// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SCENE_INTERNAL_H
#define WAYWAL_WE_SCENE_INTERNAL_H

#include "waywal/we_json.h"
#include "waywal/we_scene.h"

void sync_layer_from_obj(const we_scene_object_t *obj, we_script_layer_state_t *ls);
void sync_obj_from_layer(we_scene_object_t *obj, const we_script_layer_state_t *ls);
void sync_scene_layers_to_vm(const we_scene_t *scene, we_script_t *vm);
void sync_scene_layers_from_vm(we_scene_t *scene, const we_script_t *vm);
void build_script_env(const we_scene_t *scene, uint32_t canvas_w, uint32_t canvas_h, float dt_sec,
                      float time_sec, const waywal_shader_frame_params_t *params,
                      we_script_env_t *env);
void bind_project_props_to_script(we_script_t *vm, const we_project_t *proj);
void apply_scene_script_user_properties(we_scene_t *scene);
bool we_audio_decode_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                            size_t *out_frame_count);
bool we_audio_decode_pkg_file(we_pkg_t *pkg, const char *vfs_dir, const char *rel_path,
                              float **out_stereo_f32, size_t *out_frame_count);

bool we_scene_add_overflow_effect_fbo(we_scene_t *scene, we_effect_t *eff, const char *name,
                                      uint32_t scale);
uint32_t we_scene_effect_total_fbo_count(const we_scene_t *scene, const we_effect_t *eff);
we_rt_fbo_t *we_scene_effect_get_fbo(we_scene_t *scene, we_effect_t *eff, uint32_t index);
const we_rt_fbo_t *we_scene_effect_get_fbo_const(const we_scene_t *scene, const we_effect_t *eff,
                                                 uint32_t index);

#endif /* WAYWAL_WE_SCENE_INTERNAL_H */
