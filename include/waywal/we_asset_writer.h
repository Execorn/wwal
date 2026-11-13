// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_ASSET_WRITER_H
#define WAYWAL_WE_ASSET_WRITER_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include("waywal/we_scene.h")
#include "waywal/we_scene.h"
#endif

#if __has_include("waywal/we_project.h")
#include "waywal/we_project.h"
#endif

#if __has_include("waywal/we_tex.h")
#include "waywal/we_tex.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WAYWAL_WE_TEX_H
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
    WE_TEX_FLAG_NO_INTERPOLATION = (1u << 0),
    WE_TEX_FLAG_CLAMP_UVS = (1u << 1),
    WE_TEX_FLAG_IS_GIF = (1u << 2),
    WE_TEX_FLAG_IS_VIDEO_TEXTURE = (1u << 5),
} we_tex_flags_t;

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
#endif /* WAYWAL_WE_TEX_H */

static_assert(sizeof(we_tex_frame_t) == 32, "we_tex_frame_t must be 32 bytes");

#ifndef WAYWAL_WE_PROJECT_H
typedef struct we_project_stub we_project_t;
#endif

#ifndef WAYWAL_WE_SCENE_H
typedef struct we_scene we_scene_t;
#endif

#define WE_PKG_MAX_WRITE_ENTRIES 65536u
#define WE_PKG_MAX_WRITE_SIZE    (2ull * 1024ull * 1024ull * 1024ull)
#define WE_LZ4_MAX_INPUT_SIZE    0x7E000000u
#define WE_TEX_MAX_DIM           16384u
#define WE_TEX_MAX_MIPS          32u
#define WE_TEX_MAX_IMAGES        1024u
#define WE_TEX_MAX_FRAMES        16384u

typedef enum : uint32_t {
    WE_WRITER_OK = 0u,
    WE_WRITER_ERR_INVALID_ARG = 1u,
    WE_WRITER_ERR_INVALID_PATH = 2u,
    WE_WRITER_ERR_DUPLICATE_PATH = 3u,
    WE_WRITER_ERR_LIMIT_EXCEEDED = 4u,
    WE_WRITER_ERR_OVERFLOW = 5u,
    WE_WRITER_ERR_NON_FINITE = 6u,
    WE_WRITER_ERR_UNSUPPORTED_FMT = 7u,
    WE_WRITER_ERR_OOM = 8u,
    WE_WRITER_ERR_IO = 9u,
} we_asset_writer_err_t;

/* ------------------------------------------------------------------------- */
/* 0. Shared Path Canonicalization & Atomic File Write Helpers               */
/* ------------------------------------------------------------------------- */

/* Validates and canonicalizes a relative asset path (lowercase ASCII, forward slashes,
 * rejecting leading '/'/'\\', ':' drive letters, control bytes, '.'/'..' segments, and trailing
 * '/'). */
bool we_normalize_rel_path(const char *raw_path, char *out_norm, size_t out_cap, uint32_t *out_len);

/* Atomically writes data to path via a sibling O_NOFOLLOW temporary file, fsync, and rename(). */
bool we_write_file_atomic(const char *path, const void *data, size_t size,
                          we_asset_writer_err_t *out_err);

/* ------------------------------------------------------------------------- */
/* 1. Deterministic PKGV0022 Archive Writer                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *path;    /* Relative asset path (normalized & sorted internally) */
    const uint8_t *data; /* Payload bytes (NULL allowed iff size == 0) */
    size_t size;         /* Payload byte length */
} we_pkg_write_entry_t;

/* Serializes entries into a canonical, lexicographically sorted PKGV0022 buffer.
 * Caller owns *out_buf and releases it with free(). */
bool we_pkg_write_mem(const we_pkg_write_entry_t *entries, size_t entry_count, uint8_t **out_buf,
                      size_t *out_size, we_asset_writer_err_t *out_err);

/* Writes PKGV0022 atomically via temporary file + fsync + rename(). */
bool we_pkg_write_file(const char *out_path, const we_pkg_write_entry_t *entries,
                       size_t entry_count, we_asset_writer_err_t *out_err);

/* ------------------------------------------------------------------------- */
/* 2. Freestanding C23 LZ4 Raw Block Compressor (Zero liblz4 DT_NEEDED)      */
/* ------------------------------------------------------------------------- */

/* Returns worst-case compressed buffer capacity, or 0 if src_len > WE_LZ4_MAX_INPUT_SIZE. */
size_t we_lz4_compress_bound(size_t src_len);

/* Compresses raw block using 4096-entry stack hash table (16 KiB).
 * Returns compressed byte count (> 0), or 0 if dst_cap is insufficient or input invalid. */
size_t we_lz4_compress_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/* ------------------------------------------------------------------------- */
/* 3. TEXV0005 / TEXI0001 / TEXB0003 / TEXS0003 Encoder                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t width;        /* 1..WE_TEX_MAX_DIM */
    uint32_t height;       /* 1..WE_TEX_MAX_DIM */
    const uint8_t *pixels; /* Tightly packed top-to-bottom scanlines (1, 2, or 4 B/px) */
    size_t byte_size;      /* Exact width * height * bpp */
} we_tex_encode_mip_t;

typedef struct {
    uint32_t mip_count;              /* 1..WE_TEX_MAX_MIPS */
    const we_tex_encode_mip_t *mips; /* Mip chain starting at level 0 */
} we_tex_encode_image_t;

typedef struct {
    we_tex_format_t format;  /* WE_TEX_FMT_RGBA8888 (0), RG88 (8), or R8 (9) */
    uint32_t flags;          /* Bitmask of we_tex_flags_t */
    uint32_t texture_width;  /* GPU padded width (1..16384) */
    uint32_t texture_height; /* GPU padded height (1..16384) */
    uint32_t image_width;    /* Visible content width (<= texture_width) */
    uint32_t image_height;   /* Visible content height (<= texture_height) */
    bool enable_lz4;         /* Compress mips when shrunk; falls back to raw if larger */
    uint32_t image_count;    /* 1..WE_TEX_MAX_IMAGES */
    const we_tex_encode_image_t *images;
    uint32_t frame_count;         /* 0 unless WE_TEX_FLAG_IS_GIF is set (1..16384) */
    uint32_t gif_width;           /* Spritesheet canvas width */
    uint32_t gif_height;          /* Spritesheet canvas height */
    const we_tex_frame_t *frames; /* TEXS0003 frame array */
} we_tex_encode_spec_t;

/* Encodes TEXV0005/TEXI0001/TEXB0003(/TEXS0003) to heap buffer (*out_buf). */
bool we_tex_encode_v5(const we_tex_encode_spec_t *spec, uint8_t **out_buf, size_t *out_size,
                      we_asset_writer_err_t *out_err);

/* Atomically writes an encoded .tex file to disk. */
bool we_tex_write_v5_file(const char *out_path, const we_tex_encode_spec_t *spec,
                          we_asset_writer_err_t *out_err);

/* Generates a full 2x2 box-filter mipmap chain from level 0 raw pixels (RGBA8/RG88/R8). */
bool we_tex_generate_mips(we_tex_format_t format, uint32_t width, uint32_t height,
                          const uint8_t *level0_pixels, we_tex_encode_mip_t **out_mips,
                          uint32_t *out_mip_count);
void we_tex_free_generated_mips(we_tex_encode_mip_t *mips, uint32_t mip_count);

/* ------------------------------------------------------------------------- */
/* 4. Deterministic RFC 8785 JCS Scene & Project Serializer + Package Export */
/* ------------------------------------------------------------------------- */

/* Serializes we_scene_t to canonical RFC 8785 JSON (preserving draw order & user bindings). */
bool we_scene_serialize_json(const we_scene_t *scene, char **out_json, size_t *out_len,
                             we_asset_writer_err_t *out_err);

/* Serializes we_project_t to canonical RFC 8785 JSON. */
bool we_project_serialize_json(const we_project_t *proj, char **out_json, size_t *out_len,
                               we_asset_writer_err_t *out_err);

/* Exports loose authoring directory (project.json, scene.json, textures, shaders,
 * preview.webp) ready for CreatorPackagingWizard::setSourcePath() or packed scene.pkg. */
bool we_scene_export_dir(const we_scene_t *scene, const char *export_dir, bool pack_scene_pkg,
                         we_asset_writer_err_t *out_err);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_ASSET_WRITER_H */
