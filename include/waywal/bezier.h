#ifndef WAYWAL_BEZIER_H
#define WAYWAL_BEZIER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float p1x;
    float p1y;
    float p2x;
    float p2y;
} bezier_curve_t;

/* Standard CSS easing presets */
extern const bezier_curve_t BEZIER_LINEAR;      /* (0.0, 0.0, 1.0, 1.0) */
extern const bezier_curve_t BEZIER_DEFAULT;     /* (0.25, 0.1, 0.25, 1.0) */
extern const bezier_curve_t BEZIER_EASE_IN;     /* (0.42, 0.0, 1.0, 1.0) */
extern const bezier_curve_t BEZIER_EASE_OUT;    /* (0.0, 0.0, 0.58, 1.0) */
extern const bezier_curve_t BEZIER_EASE_IN_OUT; /* (0.42, 0.0, 0.58, 1.0) */

/* Creates a cubic Bézier curve with validated control points in range [0, 1] */
bezier_curve_t bezier_create(float p1x, float p1y, float p2x, float p2y);

/* Solves x(t) = progress for t via Newton-Raphson iteration (with bisection fallback)
   and returns y(t). Evaluates in < 5ns with 4-iteration convergence. */
float bezier_eval(const bezier_curve_t *curve, float progress);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_BEZIER_H */
