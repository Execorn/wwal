#ifndef WAYWAL_GIF_TRANSCODE_H
#define WAYWAL_GIF_TRANSCODE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Checks whether the specified file is a valid GIF by reading magic bytes (GIF87a / GIF89a).
 */
bool is_gif_file(const char *filepath);

/**
 * Resolves a video input path for hardware-accelerated playback.
 * If filepath is an animated GIF, this:
 *  1. Derives a deterministic content-addressed hash based on file size, modification time,
 *     and sample payload bytes.
 *  2. Checks the persistent cache at $XDG_CACHE_HOME/wwal/transcoded/<hash>.mp4
 *  3. On cache hit: returns the cached MP4 path immediately (0s overhead).
 *  4. On cache miss: invokes ffmpeg with fast visually-lossless H.264 YUV420p parameters,
 *     pad filters for even dimensions, and +faststart atom placement, atomically renaming
 *     the result to the cache target.
 *
 * If filepath is not a GIF, filepath is copied to out_path unchanged.
 *
 * Returns true on success, false if file access or transcoding failed.
 */
bool gif_transcode_resolve(const char *filepath, char *out_path, size_t out_path_size);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_GIF_TRANSCODE_H */
