// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_TYPES_H
#define WAYWAL_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t x;
    int32_t y;
} point_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} rect_t;

typedef union {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
    uint32_t u32;
} color_rgba_t;

typedef enum {
    WAYWAL_FORMAT_INVALID = 0,
    WAYWAL_FORMAT_ARGB8888 = 1,
    WAYWAL_FORMAT_XRGB8888 = 2,
    WAYWAL_FORMAT_XRGB2101010 = 3,
    WAYWAL_FORMAT_ARGB2101010 = 4,
} waywal_pixel_format_t;

typedef enum {
    WAYWAL_SCALING_FILL = 0,    /* Crop to fill / cover preserving aspect ratio (default) */
    WAYWAL_SCALING_FIT = 1,     /* Fit entire image with letterbox / pillarbox */
    WAYWAL_SCALING_STRETCH = 2, /* Stretch to exact destination dimensions */
    WAYWAL_SCALING_CENTER = 3,  /* 1:1 unscaled centered */
    WAYWAL_SCALING_TILE = 4,    /* Repeated tile */
    WAYWAL_SCALING_SPAN = 5,    /* Panoramic span across all monitors */
} waywal_scaling_mode_t;

#ifndef WAYWAL_TRANSITION_TYPE_DEFINED
#define WAYWAL_TRANSITION_TYPE_DEFINED
typedef enum {
    WAYWAL_TRANSITION_NONE = 0,
    WAYWAL_TRANSITION_SIMPLE = 1,
    WAYWAL_TRANSITION_FADE = 2,
    WAYWAL_TRANSITION_WIPE = 3,
    WAYWAL_TRANSITION_GROW = 4,
    WAYWAL_TRANSITION_OUTER = 5,
    WAYWAL_TRANSITION_WAVE = 6,
    WAYWAL_TRANSITION_NOISE = 7,
    WAYWAL_TRANSITION_CROSSZOOM = 8,
    WAYWAL_TRANSITION_SLIDE = 9,
    WAYWAL_TRANSITION_GLITCH = 10,
    WAYWAL_TRANSITION_BURN = 11,
    WAYWAL_TRANSITION_RIPPLE = 12,
    WAYWAL_TRANSITION_PIXELATE = 13,
    WAYWAL_TRANSITION_DOOM = 14,
    WAYWAL_TRANSITION_SWIRL = 15,
    WAYWAL_TRANSITION_CUBE = 16,
    WAYWAL_TRANSITION_LUMA = 17,
    WAYWAL_TRANSITION_LIGHT_LEAK = 18,
    WAYWAL_TRANSITION_PAGE_CURL = 19,
    WAYWAL_TRANSITION_CUSTOM = 20,
    WAYWAL_TRANSITION_RANDOM = 21,
} waywal_transition_type_t;
#endif

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_TYPES_H */
