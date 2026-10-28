// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SCENE_H
#define WAYWAL_WE_SCENE_H

#include "waywal/audio_fft.h"
#include "waywal/shader_engine.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_puppet.h"
#include "waywal/we_script.h"
#include "waywal/we_shader.h"
#include "waywal/we_tex.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_MAX_USER_PROPS     64u
#define WE_MAX_PASS_TEXTURES  8u
#define WE_MAX_CONST_VALS     32u
#define WE_MAX_EFFECT_PASSES  16u
#define WE_MAX_EFFECT_FBOS    8u
#define WE_MAX_SCENE_RT_FBOS  16u
#define WE_MAX_OBJECT_SCRIPTS 8u
#define WE_SCENE_MAX_OBJECTS  512u
#define WE_MAX_SOUND_TRACKS   8u

typedef enum {
    WE_SOUND_MODE_LOOP = 0,
    WE_SOUND_MODE_SINGLE = 1,
    WE_SOUND_MODE_RANDOM = 2,
} we_sound_playback_mode_t;

typedef struct {
    we_script_prop_target_t target;
    char uniform_name[64];
    we_script_t *vm;
} we_obj_script_t;

typedef enum {
    WE_PROP_FLOAT = 0,
    WE_PROP_BOOL = 1,
    WE_PROP_INT = 2,
    WE_PROP_COLOR = 3,
    WE_PROP_TEXT = 4,
} we_prop_type_t;

typedef struct {
    char key[64];
    char text[128];
    we_prop_type_t type;
    float val[4];
    uint32_t num_components;
    char str_val[256];
} we_user_prop_t;

typedef struct {
    char workshop_id[64];
    char workshop_dir[4096];
    char title[256];
    char type[64];  /* "scene", "video", "web", "application" */
    char file[256]; /* Default "scene.json" or video filename */
    char preview[256];
    we_user_prop_t props[WE_MAX_USER_PROPS];
    uint32_t prop_count;
} we_project_t;

typedef enum {
    WE_BLEND_NORMAL = 0,
    WE_BLEND_ADDITIVE = 1,
    WE_BLEND_MULTIPLY = 2,
    WE_BLEND_SCREEN = 3,
    WE_BLEND_DISABLED = 4,
} we_blend_mode_t;

typedef struct {
    char name[64];
    char user_prop[64]; /* Optional bound user property key */
    float val[4];
    uint32_t num_components;
    char *script_src;
} we_const_val_t;

typedef struct {
    char target_name[64]; /* "previous" or "_rt_..." */
    int32_t slot;         /* 0..7 */
} we_pass_bind_t;

typedef struct {
    bool initialized;
    int32_t mvp;
    int32_t model_matrix;
    int32_t model_matrix_inv;
    int32_t vp_matrix;
    int32_t view_proj_matrix;
    int32_t time;
    int32_t daytime;
    int32_t frametime;
    int32_t pointer_pos;
    int32_t pointer_pos_last;
    int32_t parallax_pos;
    int32_t screen;
    int32_t texel_size;
    int32_t texel_size_half;
    int32_t alpha;
    int32_t brightness;
    int32_t user_alpha;
    int32_t color;
    int32_t color4;
    int32_t eye_pos;
    int32_t view_up;
    int32_t view_forward;
    int32_t view_right;
    int32_t orient_up;
    int32_t orient_right;
    int32_t orient_forward;
    int32_t media_playing;
    int32_t album_art;
    int32_t audio_l16;
    int32_t audio_r16;
    int32_t audio_l32;
    int32_t audio_r32;
    int32_t audio_l64;
    int32_t audio_r64;
    int32_t tex_sampler[WE_MAX_PASS_TEXTURES];
    int32_t tex_resolution[WE_MAX_PASS_TEXTURES];
    int32_t tex_translation[WE_MAX_PASS_TEXTURES];
    int32_t tex_rotation[WE_MAX_PASS_TEXTURES];
    int32_t custom_uniforms[WE_SHADER_MAX_UNIFORMS];
} we_pass_uniform_locs_t;

typedef struct {
    char shader_name[128];
    we_blend_mode_t blending;
    bool depthtest_enabled;
    bool cull_enabled;

    char tex_paths[WE_MAX_PASS_TEXTURES][256];
    we_tex_t *textures[WE_MAX_PASS_TEXTURES];
    uint32_t bound_tex_mask;

    we_shader_combo_t combos[WE_SHADER_MAX_COMBOS];
    uint32_t combo_count;

    we_const_val_t const_vals[WE_MAX_CONST_VALS];
    uint32_t const_val_count;

    /* Effect pass routing */
    char command[32];    /* "", "copy", "swap" */
    char target_fbo[64]; /* "" or "_rt_..." */
    we_pass_bind_t binds[WE_MAX_PASS_TEXTURES];
    uint32_t bind_count;

    /* Preprocessed & compiled GL program */
    we_shader_preprocessed_t prep;
    uint32_t gl_prog;
    we_pass_uniform_locs_t ulocs;
} we_material_pass_t;

typedef struct {
    char name[128];
    uint32_t scale;
    uint32_t gl_fbo;
    uint32_t gl_tex;
    uint32_t width;
    uint32_t height;
} we_rt_fbo_t;

typedef struct {
    char name[128];
    char file_path[256];
    char replacement_key[64];
    bool visible;
    char visible_user_prop[64];

    we_rt_fbo_t fbos[WE_MAX_EFFECT_FBOS];
    uint32_t fbo_count;

    we_material_pass_t *passes;
    uint32_t pass_count;
} we_effect_t;

typedef struct we_scene_object {
    int32_t id;
    int32_t parent_id;
    char name[128];
    char image_path[256];

    float origin[3];
    float scale[3];
    float angles[3];
    float size[2];
    float parallax_depth[2];

    float alpha;
    char alpha_user_prop[64];
    float brightness;
    float color[3];
    char color_user_prop[64];
    int32_t color_blend_mode;
    bool visible;
    char visible_user_prop[64];
    bool copybackground;
    bool needs_full_fb_copy;
    bool fullscreen;
    bool passthrough;

    we_material_pass_t base_pass;
    bool has_base_pass;

    we_effect_t *effects;
    uint32_t effect_count;

    /* Ping-pong FBO pair for object effect chain */
    uint32_t ping_fbo[2];
    uint32_t ping_tex[2];
    uint32_t ping_w;
    uint32_t ping_h;

    /* Puppet Warp .mdl skeletal mesh, SIMD Particle System, and SceneScripts */
    we_puppet_t *puppet;
    we_particle_sys_t *particle_sys;
    we_obj_script_t scripts[WE_MAX_OBJECT_SCRIPTS];
    uint32_t script_count;

    /* Sound object ("sound" array in scene.json) */
    bool is_sound_object;
    we_sound_playback_mode_t sound_mode;
    float sound_volume;
    char user_prop_volume[64];
    bool sound_startsilent;
    float sound_mintime;
    float sound_maxtime;
    float sound_wait_timer;
    uint32_t sound_track_count;
    float *sound_tracks_pcm[WE_MAX_SOUND_TRACKS]; /* Interleaved stereo F32 @ 48000 Hz */
    size_t sound_tracks_frames[WE_MAX_SOUND_TRACKS];
    uint32_t sound_active_track;
    size_t sound_cursor_frame;
    bool sound_playing;
    bool sound_paused;

    /* Text layer ("text" object in scene.json) */
    bool is_text_object;
    char text_str[512];
    char user_prop_text[64];
    char text_font[128];
    int32_t text_pointsize;
    char user_prop_pointsize[64];
    char text_halign[32]; /* "center", "left", "right" */
    char text_valign[32]; /* "center", "top", "bottom" */
    int32_t text_padding;
    bool text_opaque_bg;
    float text_bg_color[3];
    bool text_dirty;
} we_scene_object_t;

typedef struct we_scene {
    we_project_t project;
    we_pkg_t *pkg;

    /* Scene camera & projection */
    float ortho_width;
    float ortho_height;
    float clear_color[3];
    bool clear_enabled;

    float camera_eye[3];
    float camera_center[3];
    float camera_up[3];

    bool camera_parallax;
    float camera_parallax_amount;
    float camera_parallax_delay;
    float camera_parallax_mouse_influence;

    bool camera_shake;
    float camera_shake_amp;
    float camera_shake_roughness;
    float camera_shake_speed;

    /* Scene objects in ascending back-to-front draw order */
    we_scene_object_t *objects;
    uint32_t object_count;

    /* Shared GLES 3.1 resources */
    bool gl_initialized;
    void *egl_display;
    void *egl_context;
    bool owns_egl;

    uint32_t quad_vao;
    uint32_t quad_vbo;
    uint32_t prog_present;
    uint32_t default_program;
    uint32_t particle_program;
    uint32_t target_fbo;
    int32_t present_u_tex_loc;
    int32_t part_mvp_loc;
    int32_t part_color4_loc;
    int32_t part_trans_loc;
    int32_t part_rot_loc;
    int32_t part_tex0_loc;
    we_pass_uniform_locs_t default_prog_ulocs;

    /* Cached EGL/GL extension function pointers */
    void *fn_eglCreateImageKHR;
    void *fn_eglDestroyImageKHR;
    void *fn_glEGLImageTargetTexture2DOES;
    void *fn_glEGLImageTargetTexStorageEXT;
    void *fn_eglCreateSyncKHR;
    void *fn_eglClientWaitSyncKHR;
    void *fn_eglDestroySyncKHR;
    bool egl_procs_resolved;

    uint32_t scene_fbo;
    uint32_t scene_fbo_tex;
    uint32_t scene_fbo_w;
    uint32_t scene_fbo_h;

    uint32_t full_fb_fbo;
    uint32_t full_fb_tex;
    uint32_t full_fb_w;
    uint32_t full_fb_h;

    /* Built-in utility textures */
    uint32_t tex_util_white;
    uint32_t tex_util_black;
    uint32_t tex_util_flatnormal;
    uint32_t tex_util_noise;
    uint32_t tex_album_art;
    uint8_t *album_art_rgba;
    uint32_t album_art_w;
    uint32_t album_art_h;
    float media_playing;

    /* Global named render targets (_rt_imageLayerComposite_*, etc.) */
    we_rt_fbo_t rt_fbos[WE_MAX_SCENE_RT_FBOS];
    uint32_t rt_fbo_count;

    /* Pointer, simulation clock & SceneScript shared store */
    float last_pointer[2];
    float smooth_parallax[2];
    float sim_time;
    uint64_t frame_counter;
    bool first_frame_done;
    bool uses_audio;
    we_script_shared_t script_shared;

    /* Audio output sink & sound RNG state */
    waywal_audio_fft_t *audio_out;
    uint64_t sound_rng_state;
    double last_audio_step_time;

    /* Imported DMA-BUF BO cache (for waywal_we_scene_forget_bo) */
    struct {
        const dmabuf_bo_t *bo;
        uint32_t gl_tex;
        void *egl_image;
        bool owns_egl;
    } imported_bos[32];
    size_t num_imported_bos;
} we_scene_t;

/* Parses project.json from memory or directory */
bool we_project_parse(const char *json_str, size_t len, we_project_t *out_proj);
bool we_project_load_dir(const char *workshop_dir, we_project_t *out_proj);

/* Applies a single --prop key=val override or a semicolon/newline-delimited override list */
bool we_project_apply_override(we_project_t *proj, const char *key, const char *val_str);
void we_project_apply_overrides_str(we_project_t *proj, const char *overrides_str);

/* Resolves a Workshop ID (e.g. "123456789") or filesystem path to a directory containing
 * project.json */
bool we_workshop_resolve_dir(const char *id_or_path, char *out_dir, size_t out_cap);

/* Scans Steam Workshop 431960 directories and returns an array of parsed we_project_t items */
bool we_workshop_scan_all(we_project_t **out_items, size_t *out_count);

/* Locates the preview image file (e.g. preview.jpg/png/gif/webp) for a Workshop directory */
bool we_workshop_find_preview_file(const char *workshop_dir, char *out_path, size_t out_cap);

/* Sanitizes a Workshop item title for safe UTF-8 filenames without splitting multi-byte codepoints
 */
void we_workshop_sanitize_title(const char *title, char *out_buf, size_t out_cap);

/* Populates a directory with human-readable preview symlinks for external pickers and prunes stale
 * symlinks */
bool we_workshop_sync_previews(const char *target_dir, size_t *out_synced, size_t *out_pruned);

/* Loads a Workshop scene on the CPU (parses project.json, scene.pkg, scene.json, materials,
 * effects, preprocesses shaders, and parses .tex headers/mipmaps). */
we_scene_t *we_scene_load_cpu(const char *workshop_dir, const char *prop_overrides);

/* Initializes GLES 3.1 context/resources, uploads all .tex textures, and compiles all shaders */
bool we_scene_init_gl(we_scene_t *scene, dmabuf_context_t *dmabuf_ctx);

/* Convenience helper: loads a Workshop scene and initializes GL resources in one step */
we_scene_t *we_scene_create(dmabuf_context_t *dmabuf_ctx, const char *workshop_dir,
                            const char *prop_overrides);

/* Renders one frame of the scene graph into scene_fbo and blits with opaque alpha=1.0 into
 * target_bo */
bool we_scene_render_frame(we_scene_t *scene, dmabuf_bo_t *target_bo,
                           const waywal_shader_frame_params_t *params);

/* Renders one frame of the scene graph and reads back RGBA8 pixels (works both with GLES 3.1
 * and with software CPU fallback when GL is unavailable in headless unit tests) */
bool we_scene_render_rgba8(we_scene_t *scene, uint32_t width, uint32_t height,
                           const waywal_shader_frame_params_t *params, uint8_t *out_rgba);

/* Evicts a destroyed dmabuf_bo_t from the scene's EGLImage/GL texture cache */
void waywal_we_scene_forget_bo(we_scene_t *scene, const dmabuf_bo_t *bo);

/* Live-updates a user property or material uniform in an active Workshop scene */
bool we_scene_set_uniform_override(we_scene_t *scene, const char *name, const float *values,
                                   uint32_t num_components);

/* Uploads MPRIS album art (RGBA or BGRA) to u_album_art and sets u_media_playing */
void we_scene_set_album_art(we_scene_t *scene, const uint8_t *rgba, uint32_t w, uint32_t h);
void we_scene_set_album_art_bgra(we_scene_t *scene, const uint8_t *bgra, uint32_t w, uint32_t h);
void we_scene_set_media_playing(we_scene_t *scene, float playing);

/* Binds the daemon's audio engine for "sound" layer playback and steps the scene audio mixer */
void we_scene_set_audio_output(we_scene_t *scene, waywal_audio_fft_t *audio_fft);
size_t we_scene_step_audio(we_scene_t *scene, float dt_sec);

/* Rasterizes a "text" object's string into its base_pass texture (CPU + GLES) */
void we_scene_update_text_texture(we_scene_t *scene, we_scene_object_t *obj);

/* Decodes a RIFF/WAV (or compressed audio) buffer in memory to 48000 Hz stereo interleaved float32 */
bool we_audio_decode_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                            size_t *out_frame_count);
bool we_audio_decode_wav_memory(const uint8_t *data, size_t size, float **out_stereo_f32,
                                size_t *out_frame_count);

/* Frees all CPU and GPU resources owned by a we_scene_t */
void we_scene_destroy(we_scene_t *scene);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_SCENE_H */
