// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_SHADER_H
#define WAYWAL_WE_SHADER_H

#include "waywal/we_pkg.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_SHADER_MAX_COMBOS   64u
#define WE_SHADER_MAX_UNIFORMS 64u
#define WE_SHADER_MAX_VARYINGS 32u
#define WE_SHADER_MAX_INCLUDES 32u

typedef struct {
    char name[64];
    int32_t value;
} we_shader_combo_t;

typedef struct {
    char name[64];         /* GLSL identifier, e.g. "g_Speed" or "g_Texture1" */
    char type[32];         /* GLSL type, e.g. "float", "vec3", "sampler2D" */
    char material_key[64]; /* JSON "material" key for constantshadervalues / user properties */
    char combo_name[64];   /* JSON "combo" auto-enabled if texture bound or default present */
    char default_tex[256]; /* JSON "default" texture path if sampler2D */
    float default_val[4];  /* JSON "default" numeric values (up to vec4) */
    uint32_t num_components;
    bool has_default_val;
    int32_t sampler_slot; /* 0..7 if g_TextureN, -1 otherwise */
} we_shader_uniform_meta_t;

typedef struct {
    we_shader_combo_t combos[WE_SHADER_MAX_COMBOS];
    uint32_t combo_count;

    we_shader_uniform_meta_t uniforms[WE_SHADER_MAX_UNIFORMS];
    uint32_t uniform_count;
} we_shader_meta_t;

typedef struct {
    char *vert_glsl;
    char *frag_glsl;
    we_shader_meta_t meta;
} we_shader_preprocessed_t;

/* Looks up a built-in Wallpaper Engine shader include or fallback shader by path.
 * Supports:
 * - "common.h", "common_vertex.h", "common_fragment.h", "common_blending.h", "common_particles.h"
 * - "genericimage2.vert", "genericimage2.frag" (and "shaders/genericimage2.vert", etc.) */
const char *we_shader_get_builtin(const char *path);

/* Parses // [COMBO] directives and inline uniform JSON comments from a shader source stage,
 * merging into inout_meta. */
void we_shader_extract_metadata(const char *src, we_shader_meta_t *inout_meta);

/* Preprocesses a single shader stage (.vert or .frag) into GLSL ES 3.10 source.
 * Expands #include / #require (looking up in pkg first if non-NULL, then built-in headers),
 * injects active #define combos, and converts legacy GLSL/HLSL constructs. */
char *we_shader_preprocess_stage(const char *raw_src, bool is_frag, const we_pkg_t *pkg,
                                 const we_shader_combo_t *override_combos,
                                 size_t override_combo_count, uint32_t bound_tex_mask,
                                 bool tex0_is_gif, we_shader_meta_t *inout_meta);

/* Preprocesses a matched (.vert, .frag) pair into GLSL ES 3.10, including cross-stage
 * varying vector width reconciliation (promoting narrower vec2/vec3 varyings to vec4
 * so glLinkProgram succeeds even when .vert and .frag disagree on v_TexCoord width). */
bool we_shader_preprocess_pair(const char *vert_raw, const char *frag_raw, const we_pkg_t *pkg,
                               const we_shader_combo_t *override_combos,
                               size_t override_combo_count, uint32_t bound_tex_mask,
                               bool tex0_is_gif, we_shader_preprocessed_t *out_prep);

void we_shader_preprocessed_free(we_shader_preprocessed_t *prep);

/* Compiles and links a preprocessed (.vert, .frag) pair into an OpenGL ES 3.10 program,
 * binding standard Wallpaper Engine vertex attribute locations before linking:
 * 0: a_Position, 1: a_TexCoord, 2: a_Color, 3: a_Normal,
 * 4: a_Tangent4, 5: a_BlendIndices, 6: a_BlendWeights.
 * Returns 0 on failure. */
uint32_t we_shader_compile_program(const char *vert_glsl, const char *frag_glsl,
                                   const char *debug_name);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_SHADER_H */
