// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_BUILTIN_EFFECTS_H
#define WAYWAL_WE_BUILTIN_EFFECTS_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_BUILTIN_EFFECT_COUNT 50u
#define WE_BUILTIN_MAX_PARAMS   8u

typedef enum : uint32_t {
    WE_EFFECT_CAT_MOTION_DISTORTION = 0,
    WE_EFFECT_CAT_LIGHTING_POST = 1,
    WE_EFFECT_CAT_COLOR_ATMOSPHERE = 2,
    WE_EFFECT_CAT_PARTICLE_TRAIL = 3,
} we_builtin_effect_category_t;

/* Backward-compatible category aliases */
#define WE_EFFECT_CAT_DISTORTION        WE_EFFECT_CAT_MOTION_DISTORTION
#define WE_EFFECT_CAT_COLOR_GRADING     WE_EFFECT_CAT_COLOR_ATMOSPHERE
#define WE_EFFECT_CAT_BLUR_GLOW         WE_EFFECT_CAT_LIGHTING_POST
#define WE_EFFECT_CAT_LIGHT_ATMOS       WE_EFFECT_CAT_LIGHTING_POST
#define WE_EFFECT_CAT_GLITCH_RETRO      WE_EFFECT_CAT_LIGHTING_POST
#define WE_EFFECT_CAT_AUDIO_INTERACTIVE WE_EFFECT_CAT_PARTICLE_TRAIL

typedef enum : uint32_t {
    WE_EFFECT_NONE = 0,

    /* Category 0: Motion & Distortion (1..16) */
    WE_EFFECT_WATERWAVES = 1,
    WE_EFFECT_WATERRIPPLE = 2,
    WE_EFFECT_WATERFLOW = 3,
    WE_EFFECT_FOLIAGESWAY = 4,
    WE_EFFECT_SCROLL = 5,
    WE_EFFECT_SHAKE = 6,
    WE_EFFECT_PENDULUM = 7,
    WE_EFFECT_REFRACT = 8,
    WE_EFFECT_FISHEYE = 9,
    WE_EFFECT_PERSPECTIVE = 10,
    WE_EFFECT_TRANSFORM = 11,
    WE_EFFECT_TWIRL = 12,
    WE_EFFECT_SKEW = 13,
    WE_EFFECT_PULSE = 14,
    WE_EFFECT_IRIS = 15,
    WE_EFFECT_MORPH = 16,

    /* Category 1: Lighting & Post (17..32) */
    WE_EFFECT_BLOOM = 17,
    WE_EFFECT_BLUR_KAWASE = 18,
    WE_EFFECT_GODRAYS = 19,
    WE_EFFECT_LIGHTSHAFTS = 20,
    WE_EFFECT_SHINE = 21,
    WE_EFFECT_CHROMATIC_ABERRATION = 22,
    WE_EFFECT_VIGNETTE = 23,
    WE_EFFECT_FILMGRAIN = 24,
    WE_EFFECT_VHS = 25,
    WE_EFFECT_MOTION_BLUR = 26,
    WE_EFFECT_DEPTH_OF_FIELD = 27,
    WE_EFFECT_LENS_FLARE = 28,
    WE_EFFECT_HALFTONE = 29,
    WE_EFFECT_CRT_SCANLINES = 30,
    WE_EFFECT_PIXELATE = 31,
    WE_EFFECT_SHARPEN_CAS = 32,

    /* Category 2: Color & Atmosphere (33..44) */
    WE_EFFECT_TINT = 33,
    WE_EFFECT_COLORGRADING = 34,
    WE_EFFECT_HSV = 35,
    WE_EFFECT_CONTRAST = 36,
    WE_EFFECT_OPAQUEBACKGROUND = 37,
    WE_EFFECT_EDGE_DETECTION = 38,
    WE_EFFECT_OUTLINE = 39,
    WE_EFFECT_CLOUDMOTION = 40,
    WE_EFFECT_FIRE = 41,
    WE_EFFECT_NITRO = 42,
    WE_EFFECT_AURORA = 43,
    WE_EFFECT_RAIN_GLASS = 44,

    /* Category 3: Particle & Trail (45..50) */
    WE_EFFECT_GENERICPARTICLE = 45,
    WE_EFFECT_ROPETRAIL = 46,
    WE_EFFECT_SPRITETRAIL = 47,
    WE_EFFECT_HALO = 48,
    WE_EFFECT_REFRACT_PARTICLE = 49,
    WE_EFFECT_EMBER_SPARK = 50,
} we_builtin_effect_id_t;

/* Backward-compatible effect ID aliases */
#define WE_EFFECT_WATER_RIPPLE        WE_EFFECT_WATERRIPPLE
#define WE_EFFECT_WATER_WAVES         WE_EFFECT_WATERWAVES
#define WE_EFFECT_WATER_FLOW          WE_EFFECT_WATERFLOW
#define WE_EFFECT_FOLIAGE_SWAY        WE_EFFECT_FOLIAGESWAY
#define WE_EFFECT_PERSPECTIVE_WARP    WE_EFFECT_PERSPECTIVE
#define WE_EFFECT_UV_SCROLL           WE_EFFECT_SCROLL
#define WE_EFFECT_UV_SKEW             WE_EFFECT_SKEW
#define WE_EFFECT_HUE_SHIFT           WE_EFFECT_HSV
#define WE_EFFECT_TINT_OKLCH          WE_EFFECT_TINT
#define WE_EFFECT_CONTRAST_BRIGHTNESS WE_EFFECT_CONTRAST
#define WE_EFFECT_COLOR_LUT           WE_EFFECT_COLORGRADING
#define WE_EFFECT_FILM_GRAIN          WE_EFFECT_FILMGRAIN
#define WE_EFFECT_DUAL_KAWASE_BLUR    WE_EFFECT_BLUR_KAWASE
#define WE_EFFECT_BLOOM_GLOW          WE_EFFECT_BLOOM
#define WE_EFFECT_SOBEL_EDGE_DETECT   WE_EFFECT_EDGE_DETECTION
#define WE_EFFECT_CAS_SHARPEN         WE_EFFECT_SHARPEN_CAS
#define WE_EFFECT_HALFTONE_DOTS       WE_EFFECT_HALFTONE
#define WE_EFFECT_GOD_RAYS            WE_EFFECT_GODRAYS
#define WE_EFFECT_SHINE_SHIMMER       WE_EFFECT_SHINE
#define WE_EFFECT_VHS_TRACKING_GLITCH WE_EFFECT_VHS
#define WE_EFFECT_PIXELATE_MOSAIC     WE_EFFECT_PIXELATE
#define WE_EFFECT_AUDIO_PULSE_ZOOM    WE_EFFECT_PULSE
#define WE_EFFECT_RAIN_DROPS          WE_EFFECT_RAIN_GLASS

typedef enum : uint32_t {
    WE_EFFECT_FLAG_NONE = 0u,
    WE_EFFECT_FLAG_NEEDS_DEPTH = (1u << 0),
    WE_EFFECT_FLAG_NEEDS_AUDIO = (1u << 1),
    WE_EFFECT_FLAG_NEEDS_CURSOR = (1u << 2),
    WE_EFFECT_FLAG_MULTIPASS_FBO = (1u << 3),
} we_builtin_effect_flags_t;

typedef struct {
    char name[32];           /* GLSL uniform identifier, e.g. "u_ripple_speed" */
    char label[48];          /* Human-readable UI label in studio inspector */
    uint32_t num_components; /* 1 = float, 2 = vec2, 3 = vec3, 4 = vec4 */
    float default_val[4];
    float min_val[4];
    float max_val[4];
} we_builtin_effect_param_desc_t;

typedef struct {
    uint32_t id;                     /* we_builtin_effect_id_t (4 bytes) */
    uint32_t category;               /* we_builtin_effect_category_t (4 bytes) */
    char slug[64];                   /* Canonical WE effect key, e.g. "waterripple" (64 bytes) */
    char display_name[64];           /* Studio UI title, e.g. "Water Ripple" (64 bytes) */
    uint32_t pass_count;             /* Number of internal GLES passes (1..4) (4 bytes) */
    uint32_t param_count;            /* Active entries in default_params[] (4 bytes) */
    uint32_t flags;                  /* Bitmask of we_builtin_effect_flags_t (4 bytes) */
    uint32_t lz4_compressed_offset;  /* Byte offset inside builtin rodata archive (4 bytes) */
    uint32_t lz4_compressed_size;    /* Compressed byte length (4 bytes) */
    uint32_t uncompressed_glsl_size; /* Uncompressed UTF-8 GLSL ES 3.10 byte length (4 bytes) */
    uint32_t alu_ops;                /* Estimated ALU operation count (4 bytes) */
    uint32_t tex_fetches;            /* Estimated texture fetch count (4 bytes) */
    char default_combos[64];         /* Default COMBO macro settings string (64 bytes) */
    float default_params[16];        /* Default float uniform values (64 bytes) */
    uint32_t reserved[6];            /* Reserved for future ABI extensions (24 bytes) */
} we_builtin_effect_desc_t;

static_assert(sizeof(we_builtin_effect_desc_t) == 320, "we_builtin_effect_desc_t size mismatch");

/* Returns the static descriptor table of all 50 clean-room built-in effects */
const we_builtin_effect_desc_t *we_builtin_effects_get_table(size_t *out_count);

/* Looks up a built-in effect by numeric ID (1..50 or 0..49) */
const we_builtin_effect_desc_t *we_builtin_effect_get_by_id(we_builtin_effect_id_t id);
const we_builtin_effect_desc_t *we_builtin_effect_find_by_id(we_builtin_effect_id_t id);

/* Looks up a built-in effect by WE effect slug or virtual path (O(1) Robin-Hood FNV-1a hash) */
const we_builtin_effect_desc_t *we_builtin_effect_find_by_slug(const char *slug_or_path);

/* Decompresses the GLSL ES 3.10 source for an effect from .rodata into caller buffer.
 * Uses a self-contained inline LZ4 block decoder (zero external DT_NEEDED libraries).
 * Returns number of bytes written (excluding NUL), or 0 on error. */
size_t we_builtin_effect_decompress_glsl(we_builtin_effect_id_t id, char *out_buf,
                                         size_t out_buf_cap);

/* Decompresses a specific stage (true = fragment, false = vertex) into caller buffer. */
size_t we_builtin_effect_decompress_stage(we_builtin_effect_id_t id, bool is_frag, char *out_buf,
                                          size_t out_buf_cap);

/* Returns a cached read-only pointer to the decompressed GLSL source for a stage. */
const char *we_builtin_effect_get_stage_glsl(we_builtin_effect_id_t id, bool is_frag);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_BUILTIN_EFFECTS_H */
