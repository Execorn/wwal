#include "waywal/bezier.h"
#include <math.h>

const bezier_curve_t BEZIER_LINEAR      = { 0.0f,  0.0f, 1.0f,  1.0f };
const bezier_curve_t BEZIER_DEFAULT     = { 0.25f, 0.1f, 0.25f, 1.0f };
const bezier_curve_t BEZIER_EASE_IN     = { 0.42f, 0.0f, 1.0f,  1.0f };
const bezier_curve_t BEZIER_EASE_OUT    = { 0.0f,  0.0f, 0.58f, 1.0f };
const bezier_curve_t BEZIER_EASE_IN_OUT = { 0.42f, 0.0f, 0.58f, 1.0f };

bezier_curve_t bezier_create(float p1x, float p1y, float p2x, float p2y) {
    if (p1x < 0.0f) p1x = 0.0f;
    if (p1x > 1.0f) p1x = 1.0f;
    if (p2x < 0.0f) p2x = 0.0f;
    if (p2x > 1.0f) p2x = 1.0f;

    return (bezier_curve_t){
        .p1x = p1x,
        .p1y = p1y,
        .p2x = p2x,
        .p2y = p2y,
    };
}

float bezier_eval(const bezier_curve_t *curve, float progress) {
    if (!curve) return progress;
    if (progress <= 0.0f) return 0.0f;
    if (progress >= 1.0f) return 1.0f;

    /* Check for linear identity curve */
    if (curve->p1x == curve->p1y && curve->p2x == curve->p2y) {
        return progress;
    }

    const float cx = 3.0f * curve->p1x;
    const float bx = 3.0f * (curve->p2x - curve->p1x) - cx;
    const float ax = 1.0f - cx - bx;

    const float cy = 3.0f * curve->p1y;
    const float by = 3.0f * (curve->p2y - curve->p1y) - cy;
    const float ay = 1.0f - cy - by;

    /* Newton-Raphson iteration: solve x(t) = progress for t */
    float t = progress;
    for (int i = 0; i < 6; ++i) {
        float x = ((ax * t + bx) * t + cx) * t;
        float d = (3.0f * ax * t + 2.0f * bx) * t + cx;
        if (fabsf(d) < 1e-6f) {
            break;
        }
        float err = x - progress;
        if (fabsf(err) < 1e-6f) {
            break;
        }
        t -= err / d;
        if (t < 0.0f || t > 1.0f) {
            /* If Newton-Raphson diverges outside valid [0, 1] range, fallback to bisection */
            break;
        }
    }

    /* Fallback bisection if not converged or out of bounds */
    if (t < 0.0f || t > 1.0f) {
        float t0 = 0.0f, t1 = 1.0f;
        t = progress;
        for (int i = 0; i < 16; ++i) {
            float x = ((ax * t + bx) * t + cx) * t;
            if (fabsf(x - progress) < 1e-5f) {
                break;
            }
            if (progress > x) {
                t0 = t;
            } else {
                t1 = t;
            }
            t = (t0 + t1) * 0.5f;
        }
    }

    /* Evaluate y(t) */
    return ((ay * t + by) * t + cy) * t;
}
