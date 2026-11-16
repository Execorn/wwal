// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_BEZIER_H
#define WAYWAL_BEZIER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    union {
        float p1x;
        float x1;
    };
    union {
        float p1y;
        float y1;
    };
    union {
        float p2x;
        float x2;
    };
    union {
        float p2y;
        float y2;
    };
} bezier_curve_t;
typedef bezier_curve_t waywal_cubic_bezier_t;

/* Standard CSS easing presets */
extern const bezier_curve_t BEZIER_LINEAR;      /* (0.0, 0.0, 1.0, 1.0) */
extern const bezier_curve_t BEZIER_DEFAULT;     /* (0.25, 0.1, 0.25, 1.0) */
extern const bezier_curve_t BEZIER_EASE_IN;     /* (0.42, 0.0, 1.0, 1.0) */
extern const bezier_curve_t BEZIER_EASE_OUT;    /* (0.0, 0.0, 0.58, 1.0) */
extern const bezier_curve_t BEZIER_EASE_IN_OUT; /* (0.42, 0.0, 0.58, 1.0) */

/* Clamps x control points to [0, 1] and sanitizes non-finite inputs to 0.0f. */
bezier_curve_t bezier_create(float p1x, float p1y, float p2x, float p2y);

/* Solves x(t) = progress for t via bracketed double-precision Newton-Halley-Bisection. */
float waywal_cubic_bezier_solve_t(const bezier_curve_t *curve, float progress);
float bezier_solve_t(const bezier_curve_t *curve, float progress);

/* Solves x(t) = progress for t and returns y(t), sanitizing non-finite inputs to 0.0f. */
float bezier_eval(const bezier_curve_t *curve, float progress);

#ifdef __cplusplus
}

inline bool operator==(const bezier_curve_t &a, const bezier_curve_t &b) noexcept
{
    return a.p1x == b.p1x && a.p1y == b.p1y && a.p2x == b.p2x && a.p2y == b.p2y;
}
#endif

#endif /* WAYWAL_BEZIER_H */
