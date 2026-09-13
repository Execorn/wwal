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
} waywal_scaling_mode_t;

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_TYPES_H */
