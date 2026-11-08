// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SHADER_PARSER_H
#define WAYWAL_SHADER_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_MAX_SHADER_PASSES   5u  /* BufferA, BufferB, BufferC, BufferD, Image */
#define WAYWAL_MAX_SHADER_CHANNELS 4u  /* iChannel0..iChannel3 */
#define WAYWAL_MAX_SHADER_UNIFORMS 64u /* Max uniform controls per shader */

typedef enum {
    WAYWAL_SHADER_PASS_BUFFER_A = 0,
    WAYWAL_SHADER_PASS_BUFFER_B = 1,
    WAYWAL_SHADER_PASS_BUFFER_C = 2,
    WAYWAL_SHADER_PASS_BUFFER_D = 3,
    WAYWAL_SHADER_PASS_IMAGE = 4,
    WAYWAL_SHADER_PASS_COMMON = 5,
} waywal_shader_pass_id_t;

typedef enum {
    WAYWAL_CHAN_TARGET_NONE = 0,
    WAYWAL_CHAN_TARGET_AUDIO = 1,
    WAYWAL_CHAN_TARGET_NOISE = 2,
    WAYWAL_CHAN_TARGET_WALLPAPER = 3,
    WAYWAL_CHAN_TARGET_ALBUM_ART = 4,
    WAYWAL_CHAN_TARGET_DEPTH = 5,
    WAYWAL_CHAN_TARGET_BUFFER_A = 6,
    WAYWAL_CHAN_TARGET_BUFFER_B = 7,
    WAYWAL_CHAN_TARGET_BUFFER_C = 8,
    WAYWAL_CHAN_TARGET_BUFFER_D = 9,
    WAYWAL_CHAN_TARGET_IMAGE = 10,
    WAYWAL_CHAN_TARGET_KEYBOARD = 11,
    WAYWAL_CHAN_TARGET_CUBEMAP = 12,
} waywal_shader_chan_target_t;

typedef struct {
    waywal_shader_chan_target_t target;
    char image_path[512];
} waywal_shader_channel_t;

typedef struct {
    float res_scale;     /* Resolution scaling factor (0.1..2.0, default 1.0) */
    uint32_t target_fps; /* Target FPS cap (0 = unconstrained) */
    waywal_shader_channel_t channels[WAYWAL_MAX_SHADER_CHANNELS];
} waywal_shader_pass_desc_t;

typedef enum {
    WAYWAL_UNIFORM_TYPE_FLOAT = 0,
    WAYWAL_UNIFORM_TYPE_BOOL = 1,
    WAYWAL_UNIFORM_TYPE_INT = 2,
    WAYWAL_UNIFORM_TYPE_VEC2 = 3,
    WAYWAL_UNIFORM_TYPE_VEC3 = 4,
    WAYWAL_UNIFORM_TYPE_VEC4 = 5,
    WAYWAL_UNIFORM_TYPE_COLOR = 6,
} waywal_shader_uniform_type_t;

typedef struct {
    char name[64];   /* GLSL uniform identifier, e.g. "u_speed" */
    char label[128]; /* User-visible display label in Studio UI */
    waywal_shader_uniform_type_t type;
    uint32_t num_components; /* 1 for float/bool/int, 2 for vec2, 3 for vec3/color, 4 for vec4 */
    float default_val[4];    /* Initial default values */
    float min_val[4];        /* Minimum slider/control bounds */
    float max_val[4];        /* Maximum slider/control bounds */
    float step;              /* Slider step resolution (0.0 = continuous) */
    bool has_range;          /* True if min/max explicitly specified */
} waywal_shader_uniform_def_t;

typedef struct {
    char title[128];
    char author[128];
    char description[512];
    char license[64];
    float global_scale;
    uint32_t global_fps;
    bool has_multi_pass;
    waywal_shader_pass_desc_t passes[WAYWAL_MAX_SHADER_PASSES];
    uint32_t uniform_count;
    waywal_shader_uniform_def_t uniforms[WAYWAL_MAX_SHADER_UNIFORMS];
} waywal_shader_meta_t;

/* Initializes shader metadata structure with sane defaults */
void waywal_shader_meta_init(waywal_shader_meta_t *meta);

/* Parses shader GLSL comments and pragmas from memory */
bool waywal_shader_parse_meta(const char *src, size_t len, waywal_shader_meta_t *out_meta);

/* Reads and parses a GLSL shader file from disk */
bool waywal_shader_parse_file(const char *filepath, waywal_shader_meta_t *out_meta);

/* Finds a uniform definition by name */
const waywal_shader_uniform_def_t *waywal_shader_find_uniform(const waywal_shader_meta_t *meta,
                                                              const char *name);

/* Converts a uniform type enum to a GLSL type string ("float", "vec3", etc.) */
const char *waywal_shader_uniform_type_str(waywal_shader_uniform_type_t type);

/* Formats GLSL declarations for all uniforms into out_buf */
size_t waywal_shader_format_uniform_decls(const waywal_shader_meta_t *meta, char *out_buf,
                                          size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SHADER_PARSER_H */
