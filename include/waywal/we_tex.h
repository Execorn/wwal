// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_TEX_H
#define WAYWAL_WE_TEX_H

#include "waywal/demuxer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_TEX_MAX_DECOMP_SIZE (256u * 1024u * 1024u)
#define WE_FIF_UNKNOWN         (-1)
#define WE_FIF_MP4             35

typedef enum {
    WE_TEX_FMT_RGBA8888 = 0,
    WE_TEX_FMT_DXT5 = 4,
    WE_TEX_FMT_DXT3 = 6,
    WE_TEX_FMT_DXT1 = 7,
    WE_TEX_FMT_RG88 = 8,
    WE_TEX_FMT_R8 = 9,
    WE_TEX_FMT_BC7 = 12,
} we_tex_format_t;

typedef enum {
    WE_TEX_FLAG_NONE = 0,
    WE_TEX_FLAG_NO_INTERPOLATION = (1u << 0), /* 1: GL_NEAREST */
    WE_TEX_FLAG_CLAMP_UVS = (1u << 1),        /* 2: GL_CLAMP_TO_EDGE */
    WE_TEX_FLAG_IS_GIF = (1u << 2),           /* 4: Animated spritesheet (TEXS) */
    WE_TEX_FLAG_IS_VIDEO_TEXTURE = (1u << 5), /* 32: Embedded video texture */
} we_tex_flags_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t is_lz4;
    uint32_t decomp_size;
    uint32_t byte_count;
    uint8_t *data;     /* Decompressed raw mipmap bytes */
    size_t data_size;  /* Size of data in bytes */
    uint8_t *rgba8;    /* Decoded RGBA8 pixels (for CPU verification / fallback) */
    bool is_bgra_swiz; /* True if decoded via image_load_from_memory_rgba (BGRA layout) */
} we_tex_mip_t;

typedef struct {
    uint32_t mip_count;
    we_tex_mip_t *mips;
} we_tex_image_t;

typedef struct {
    int32_t image_id;
    float frametime;
    float x;
    float y;
    float width;
    float width_y;
    float height_x;
    float height;
} we_tex_frame_t;

typedef struct we_tex {
    uint32_t texv_version;
    uint32_t texi_version;
    uint32_t texb_version;
    uint32_t texs_version;

    we_tex_format_t format;
    uint32_t flags;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t unk_int0;

    uint32_t image_count;
    int32_t free_image_format;
    bool is_video_mp4;
    we_tex_image_t *images;

    /* Spritesheet animation table (TEXS0001..0003) */
    uint32_t frame_count;
    uint32_t gif_width;
    uint32_t gif_height;
    we_tex_frame_t *frames;
    uint32_t current_frame_idx;
    float frame_time_accum;

    /* Embedded MP4 video state */
    uint8_t *mp4_data;
    size_t mp4_size;
    demuxer_t *demuxer;
    double video_pts_sec;
    void *video_swdec;

    /* OpenGL ES 3.1 texture handle (0 if not uploaded) */
    uint32_t gl_tex;
    uint32_t *frame_gl_texs; /* Optional per-image GL textures for multi-image GIFs */
} we_tex_t;

/* Dual-mode magic reader: supports both 9-byte null-terminated "TEXV0005\0"
 * and length-prefixed [u32 len=8]["TEXV0005"] headers */
bool we_tex_read_magic(const uint8_t *buf, size_t size, size_t *inout_cursor, char out_magic[16]);

/* Safe bounds-checked LZ4 raw block decompressor (returns decompressed size or -1 on error) */
int we_lz4_decompress_safe(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/* Pure-C block decompression fallbacks to RGBA8 (4 bytes per pixel) */
bool we_decode_dxt1_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba);
bool we_decode_dxt3_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba);
bool we_decode_dxt5_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                          uint8_t *dst_rgba);
bool we_decode_bc7_rgba8(const uint8_t *src, size_t src_len, uint32_t width, uint32_t height,
                         uint8_t *dst_rgba);

/* Parses and decompresses a .tex container on the CPU (no OpenGL context required) */
we_tex_t *we_tex_parse_cpu(const uint8_t *data, size_t size);

/* Decodes mip 0 of image 0 into tightly-packed RGBA8 pixels (width * height * 4 bytes) */
bool we_tex_decode_rgba8(we_tex_t *tex, uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h);

/* Uploads a parsed we_tex_t to OpenGL ES 3.1 (requires an active GL context) */
bool we_tex_upload_gl(we_tex_t *tex);

/* Convenience helper: parses and uploads .tex in one step */
we_tex_t *we_tex_load(const uint8_t *data, size_t size, bool upload_gl);

/* Computes g_Texture0Translation (vec2) and g_Texture0Rotation (vec4) for a spritesheet frame */
bool we_tex_get_frame_transform(const we_tex_t *tex, uint32_t frame_idx, float out_translation[2],
                                float out_rotation[4]);

/* Advances spritesheet animation by dt_sec and outputs current frame uniforms */
void we_tex_advance_spritesheet(we_tex_t *tex, float dt_sec, float out_translation[2],
                                float out_rotation[4]);

/* Advances embedded MP4 video stream if present */
bool we_tex_advance_video(we_tex_t *tex, float dt_sec);

/* Frees all CPU and GPU resources owned by a we_tex_t */
void we_tex_destroy(we_tex_t *tex);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_TEX_H */
