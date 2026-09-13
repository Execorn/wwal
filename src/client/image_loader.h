#ifndef WAYWAL_IMAGE_LOADER_H
#define WAYWAL_IMAGE_LOADER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads an image from disk into 32-bit RGBA pixel format.
 * Returns true on success, filling *out_pixels, *out_w, *out_h.
 * Memory allocated for *out_pixels must be freed with image_free(). */
bool image_load(const char *filepath, uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h);

void image_free(uint8_t *pixels);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IMAGE_LOADER_H */
