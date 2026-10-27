// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SCRIPT_H
#define WAYWAL_WE_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_SCRIPT_ARENA_SIZE         (16u * 1024u * 1024u)
#define WE_SCRIPT_MAX_STEPS          100000u
#define WE_SCRIPT_TIMEOUT_NS         2000000ULL
#define WE_SCRIPT_MAX_SHARED_ENTRIES 64u
#define WE_SCRIPT_MAX_SCENE_LAYERS   64u

#define WE_LAYER_DIRTY_ORIGIN  (1u << 0)
#define WE_LAYER_DIRTY_SCALE   (1u << 1)
#define WE_LAYER_DIRTY_ANGLES  (1u << 2)
#define WE_LAYER_DIRTY_ALPHA   (1u << 3)
#define WE_LAYER_DIRTY_COLOR   (1u << 4)
#define WE_LAYER_DIRTY_VISIBLE (1u << 5)
#define WE_LAYER_DIRTY_SIZE    (1u << 6)

typedef enum {
    WE_SCRIPT_PROP_ORIGIN = 0,
    WE_SCRIPT_PROP_SCALE = 1,
    WE_SCRIPT_PROP_ANGLES = 2,
    WE_SCRIPT_PROP_ALPHA = 3,
    WE_SCRIPT_PROP_COLOR = 4,
    WE_SCRIPT_PROP_VISIBLE = 5,
    WE_SCRIPT_PROP_UNIFORM = 6,
} we_script_prop_target_t;

typedef struct {
    float origin[3];
    float scale[3];
    float angles[3];
    float size[2];
    float alpha;
    float color[3];
    bool visible;
    char name[128];
    int32_t id;
    uint32_t dirty_mask;
} we_script_layer_state_t;

typedef struct {
    float frametime;
    float runtime;
    float time_of_day;
    float screen_resolution[2];
    float canvas_size[2];
    float cursor_world[3];
    float cursor_screen[2];
    bool cursor_left_down;
    float audio_left[64];
    float audio_right[64];
    float audio_average[64];
} we_script_env_t;

typedef struct {
    char key[64];
    uint32_t num_components; /* 1=scalar/bool, 2=Vec2, 3=Vec3, 4=Vec4 */
    bool is_bool;
    bool is_str;
    float val[4];
    char str_val[128];
} we_script_shared_entry_t;

typedef struct {
    we_script_shared_entry_t entries[WE_SCRIPT_MAX_SHARED_ENTRIES];
    uint32_t count;
} we_script_shared_t;

typedef struct we_script we_script_t;

/* Compiles a SceneScript source string inside a fixed 16 MiB sandboxed arena
 * and executes top-level initialization statements. */
we_script_t *we_script_create(const char *src, size_t len, we_script_shared_t *shared);

/* Frees all resources (including the 16 MiB arena) owned by a we_script_t */
void we_script_destroy(we_script_t *script);

/* Sets a user property or scriptproperty visible via scriptProperties / applyUserProperties */
void we_script_set_prop(we_script_t *script, const char *name, const float *val,
                        uint32_t num_components, const char *str_val);

/* Registers and reads back scene layers accessible via thisScene.getLayer(name) */
void we_script_register_scene_layer(we_script_t *script, uint32_t slot,
                                    const we_script_layer_state_t *layer);
bool we_script_read_scene_layer(const we_script_t *script, uint32_t slot,
                                we_script_layer_state_t *out_layer);

/* Invokes exported init(value) under the 100,000-step / 2.0ms watchdog.
 * On watchdog abort or error, copies in_val to out_val and returns false. */
bool we_script_eval_init(we_script_t *script, const we_script_env_t *env,
                         we_script_layer_state_t *inout_layer, const float *in_val,
                         uint32_t num_components, float *out_val);

/* Invokes exported update(value) under the 100,000-step / 2.0ms watchdog.
 * On watchdog abort or error, copies in_val to out_val and returns false. */
bool we_script_eval_update(we_script_t *script, const we_script_env_t *env,
                           we_script_layer_state_t *inout_layer, const float *in_val,
                           uint32_t num_components, float *out_val);

/* Invokes exported cursorMove(event) under the 100,000-step / 2.0ms watchdog */
bool we_script_eval_cursor_move(we_script_t *script, const we_script_env_t *env,
                                we_script_layer_state_t *inout_layer);

/* Invokes exported applyUserProperties(props) under the 100,000-step / 2.0ms watchdog */
bool we_script_eval_apply_user_properties(we_script_t *script, const we_script_env_t *env,
                                          we_script_layer_state_t *inout_layer);

/* Telemetry & watchdog inspection helpers */
bool we_script_was_aborted(const we_script_t *script);
uint32_t we_script_last_step_count(const we_script_t *script);
bool we_script_uses_audio(const we_script_t *script);
size_t we_script_arena_used(const we_script_t *script);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_SCRIPT_H */
