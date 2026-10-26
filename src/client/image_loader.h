// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_IMAGE_LOADER_H
#define WAYWAL_IMAGE_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    union {
        uint8_t *data;
        uint8_t *pixels;
    };
    union {
        uint32_t width;
        uint32_t w;
    };
    union {
        uint32_t height;
        uint32_t h;
    };
    union {
        uint32_t channels;
        uint32_t bpp;
    };
    uint32_t stride;
    union {
        size_t size;
        size_t byte_size;
    };
} raw_image_t;

/* Loads an image from disk into 32-bit pixel format.
 * Returns true on success, filling *out_pixels, *out_w, *out_h.
 * Memory allocated for *out_pixels must be freed with image_free(). */
bool image_load(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h);
bool image_load_raw(const char *filepath, raw_image_t *out_img);

bool image_load_fd(int fd, raw_image_t *out_img);
bool image_load_fd_rgba(int fd, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h);

bool image_load_from_memory(const uint8_t *buf, size_t size, raw_image_t *out_img);
bool image_load_from_memory_rgba(const uint8_t *buf, size_t size, uint8_t **out_pixels,
                                 uint32_t *out_w, uint32_t *out_h);

void image_free(uint8_t *pixels);
void raw_image_free(raw_image_t *img);

#ifndef __cplusplus
#define WAYWAL_IMG_LOAD_SEL(_1, _2, _3, _4, NAME, ...) NAME
#define image_load(...)                                                                            \
    WAYWAL_IMG_LOAD_SEL(__VA_ARGS__, image_load, image_load_raw, image_load_raw, 0)(__VA_ARGS__)

#define WAYWAL_IMG_LOAD_FD_SEL(_1, _2, _3, _4, NAME, ...) NAME
#define image_load_fd(...)                                                                         \
    WAYWAL_IMG_LOAD_FD_SEL(__VA_ARGS__, image_load_fd_rgba, image_load_fd, image_load_fd,          \
                           0)(__VA_ARGS__)

#define WAYWAL_IMG_LOAD_MEM_SEL(_1, _2, _3, _4, _5, NAME, ...) NAME
#define image_load_from_memory(...)                                                                \
    WAYWAL_IMG_LOAD_MEM_SEL(__VA_ARGS__, image_load_from_memory_rgba, image_load_from_memory,      \
                            image_load_from_memory, 0)(__VA_ARGS__)

#define image_free(x) _Generic((x), raw_image_t *: raw_image_free, default: image_free)(x)
#endif

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IMAGE_LOADER_H */
