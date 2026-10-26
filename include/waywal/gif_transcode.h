// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_GIF_TRANSCODE_H
#define WAYWAL_GIF_TRANSCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Checks whether the specified file is a valid GIF by reading magic bytes (GIF87a / GIF89a).
 */
bool is_gif_file(const char *filepath);

/**
 * Optimizes a video or animated GIF input path for low-resource hardware playback.
 * Transcodes and caches inputs that are GIFs, exceed (max_width x max_height),
 * or use heavy B-frame / multi-ref encoding into a lightweight H.264 IPPP (refs=1, bf=0)
 * stream in $XDG_CACHE_HOME/wwal/transcoded/<hash>.mp4.
 */
bool video_transcode_optimize(const char *filepath, uint32_t max_width, uint32_t max_height,
                              char *out_path, size_t out_path_size);

/**
 * Resolves a video/GIF input path using default 2560x1440 maximum dimensions.
 */
bool gif_transcode_resolve(const char *filepath, char *out_path, size_t out_path_size);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_GIF_TRANSCODE_H */
