// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SHADER_ENGINE_H
#define WAYWAL_SHADER_ENGINE_H

#include "waywal/audio_fft.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dmabuf_bo dmabuf_bo_t;
typedef struct dmabuf_context dmabuf_context_t;

typedef struct {
    float time_sec;
    float time_delta_sec;
    int32_t frame_index;
    float mouse[4];
    float mouse_smooth[4];
    float mouse_vel[2];
    float workspace_offset[2];
    float date[4];
    float palette_primary[3];
    float palette_secondary[3];
    float palette_tertiary[3];
    const waywal_audio_frame_t *audio;
} waywal_shader_frame_params_t;

typedef struct waywal_shader_engine waywal_shader_engine_t;

char *waywal_shader_wrap_source(const char *user_src);
char *waywal_shader_wrap_pass_source(const char *user_src, int pass_idx);
uint64_t waywal_shader_compute_cache_key(const char *wrapped_src, const char *gpu_id);

waywal_shader_engine_t *waywal_shader_engine_create(dmabuf_context_t *dmabuf_ctx);
void waywal_shader_engine_destroy(waywal_shader_engine_t *eng);

bool waywal_shader_engine_load_source(waywal_shader_engine_t *eng, const char *user_src,
                                      const char *debug_name);
bool waywal_shader_engine_load_file(waywal_shader_engine_t *eng, const char *filepath);
bool waywal_shader_engine_load_depth_parallax(waywal_shader_engine_t *eng, const uint8_t *base_rgba,
                                              uint32_t base_w, uint32_t base_h,
                                              const uint8_t *depth_rgba, uint32_t depth_w,
                                              uint32_t depth_h);
void waywal_shader_engine_set_wallpaper(waywal_shader_engine_t *eng, const uint8_t *rgba,
                                        uint32_t w, uint32_t h);
void waywal_shader_engine_set_depth_strength(waywal_shader_engine_t *eng, float strength);
void waywal_shader_engine_forget_bo(waywal_shader_engine_t *eng, const dmabuf_bo_t *bo);
int waywal_shader_engine_get_channel_target(const waywal_shader_engine_t *eng, int pass_idx,
                                            int chan_idx);

int waywal_shader_engine_get_inotify_fd(const waywal_shader_engine_t *eng);
bool waywal_shader_engine_poll_hot_reload(waywal_shader_engine_t *eng);

bool waywal_shader_engine_render_frame(waywal_shader_engine_t *eng, dmabuf_bo_t *target_bo,
                                       const waywal_shader_frame_params_t *params);
bool waywal_shader_engine_has_active_program(const waywal_shader_engine_t *eng);
bool waywal_shader_engine_last_load_from_cache(const waywal_shader_engine_t *eng);
uint32_t waywal_shader_engine_active_pass_mask(const waywal_shader_engine_t *eng);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SHADER_ENGINE_H */
