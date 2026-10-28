// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STATE_STORE_H
#define WAYWAL_STATE_STORE_H

#include "waywal/path.h"
#include "waywal/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_STATE_MAGIC       0x57575354U
#define WAYWAL_STATE_VERSION     4U
#define WAYWAL_STATE_MAX_OUTPUTS 16U

typedef enum : uint32_t {
    WAYWAL_CONTENT_STATIC_IMAGE = 0,
    WAYWAL_CONTENT_SOLID_COLOR = 1,
    WAYWAL_CONTENT_VIDEO = 2,
    WAYWAL_CONTENT_SLIDESHOW = 3,
    WAYWAL_CONTENT_SHADER = 4,
    WAYWAL_CONTENT_ANIMATION = 5,
    WAYWAL_CONTENT_WORKSHOP = 6,

    STATIC_IMAGE = WAYWAL_CONTENT_STATIC_IMAGE,
    SOLID_COLOR = WAYWAL_CONTENT_SOLID_COLOR,
    VIDEO = WAYWAL_CONTENT_VIDEO,
    SLIDESHOW = WAYWAL_CONTENT_SLIDESHOW,
    SHADER = WAYWAL_CONTENT_SHADER,
    ANIMATION = WAYWAL_CONTENT_ANIMATION,
    WORKSHOP = WAYWAL_CONTENT_WORKSHOP,
} waywal_content_kind_t;

#ifndef WAYWAL_FILTER_MODE_DEFINED
#define WAYWAL_FILTER_MODE_DEFINED
typedef enum : uint32_t {
    WAYWAL_FILTER_LANCZOS3 = 0,
    WAYWAL_FILTER_MITCHELL = 1,
    WAYWAL_FILTER_BILINEAR = 2,
    WAYWAL_FILTER_NEAREST = 3,
} waywal_filter_mode_t;
#endif

typedef struct {
    char output_name[64];
    waywal_content_kind_t kind;
    char media_path[4096];
    uint32_t solid_color;
    waywal_scaling_mode_t scaling;
    waywal_filter_mode_t filter;
    bool ten_bit;
    uint32_t target_fps;
    uint32_t slideshow_interval_sec;
    bool audio_reactive;
    char prop_overrides[2048];
    float crop_gravity_x;
    float crop_gravity_y;
    bool has_crop_gravity;
} waywal_output_saved_state_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
    waywal_output_saved_state_t outputs[WAYWAL_STATE_MAX_OUTPUTS];
} waywal_state_store_t;

void waywal_state_store_init(waywal_state_store_t *store);
void waywal_state_store_set_namespace(const char *namespace_str);

bool waywal_state_store_resolve_path(path_buf_t *out_path, const char *override_dir);

bool waywal_state_store_save_at(const waywal_state_store_t *store, const char *file_path);
bool waywal_state_store_load_at(waywal_state_store_t *store, const char *file_path);

bool waywal_state_store_save(const waywal_state_store_t *store);
bool waywal_state_store_load(waywal_state_store_t *store);
bool waywal_state_store_save_dir(const waywal_state_store_t *store, const char *override_dir);
bool waywal_state_store_load_dir(waywal_state_store_t *store, const char *override_dir);

#ifndef __cplusplus
#define WAYWAL_STATE_SAVE_SEL(_1, _2, NAME, ...) NAME
#define waywal_state_store_save(...)                                                               \
    WAYWAL_STATE_SAVE_SEL(__VA_ARGS__, waywal_state_store_save_dir, waywal_state_store_save,       \
                          0)(__VA_ARGS__)

#define WAYWAL_STATE_LOAD_SEL(_1, _2, NAME, ...) NAME
#define waywal_state_store_load(...)                                                               \
    WAYWAL_STATE_LOAD_SEL(__VA_ARGS__, waywal_state_store_load_dir, waywal_state_store_load,       \
                          0)(__VA_ARGS__)
#endif

bool waywal_state_store_update_output(waywal_state_store_t *store,
                                      const waywal_output_saved_state_t *state);

const waywal_output_saved_state_t *waywal_state_store_find_output(const waywal_state_store_t *store,
                                                                  const char *output_name);

const char *waywal_filter_mode_to_swww_str(waywal_filter_mode_t filter);

bool waywal_render_shader_preview_bgra(const char *shader_path, uint8_t *out_bgra, uint32_t width,
                                       uint32_t height);

bool waywal_ecosystem_cache_sync(const char *output_name, const char *media_path,
                                 const char *filter_str, const uint8_t *bgra_pixels, uint32_t width,
                                 uint32_t height, bool caelestia_sync, bool no_cache);

#ifdef __cplusplus
}
#endif

#endif
