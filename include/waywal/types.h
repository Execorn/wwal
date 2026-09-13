#ifndef WAYWAL_TYPES_H
#define WAYWAL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    WAYWAL_FORMAT_INVALID  = 0,
    WAYWAL_FORMAT_ARGB8888 = 1,
    WAYWAL_FORMAT_XRGB8888 = 2,
} waywal_pixel_format_t;

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_TYPES_H */
