// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/bezier.h"

#include <math.h>

const bezier_curve_t BEZIER_LINEAR = {.p1x = 0.0f, .p1y = 0.0f, .p2x = 1.0f, .p2y = 1.0f};
const bezier_curve_t BEZIER_DEFAULT = {.p1x = 0.25f, .p1y = 0.1f, .p2x = 0.25f, .p2y = 1.0f};
const bezier_curve_t BEZIER_EASE_IN = {.p1x = 0.42f, .p1y = 0.0f, .p2x = 1.0f, .p2y = 1.0f};
const bezier_curve_t BEZIER_EASE_OUT = {.p1x = 0.0f, .p1y = 0.0f, .p2x = 0.58f, .p2y = 1.0f};
const bezier_curve_t BEZIER_EASE_IN_OUT = {.p1x = 0.42f, .p1y = 0.0f, .p2x = 0.58f, .p2y = 1.0f};

static inline float sanitize_unit_clamp_f32(float v)
{
    if (!isfinite(v) || v <= 0.0f) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

static double bezier_solve_t_f64(double cx1, double cx2, double target)
{
    const double c = 3.0 * cx1;
    const double b = 3.0 * (cx2 - cx1) - c;
    const double a = 1.0 - c - b;

    double lo = 0.0;
    double hi = 1.0;
    double t = target;
    double prev_step = 1.0;

    for (int iter = 0; iter < 40; ++iter) {
        const double x = ((a * t + b) * t + c) * t;
        const double f = x - target;
        if (fabs(f) <= 1e-10) {
            return t;
        }
        if (f < 0.0) {
            lo = t;
        } else {
            hi = t;
        }
        if ((hi - lo) <= 1e-10) {
            return 0.5 * (lo + hi);
        }

        const double df = (3.0 * a * t + 2.0 * b) * t + c;
        const double d2f = 6.0 * a * t + 2.0 * b;
        if (fabs(df) > 1e-12) {
            const double denom = 2.0 * df * df - f * d2f;
            const double step = (fabs(denom) > 1e-14) ? ((2.0 * f * df) / denom) : (f / df);
            const double cand = t - step;
            /* Reject steps that escape the shrinking bracket or fail to contract. */
            if (isfinite(cand) && cand > lo && cand < hi && fabs(step) <= 0.85 * prev_step) {
                if (fabs(step) <= 1e-11) {
                    return cand;
                }
                prev_step = fabs(step);
                t = cand;
                continue;
            }
        }

        t = 0.5 * (lo + hi);
        prev_step = hi - lo;
    }

    return 0.5 * (lo + hi);
}

bezier_curve_t bezier_create(float p1x, float p1y, float p2x, float p2y)
{
    return (bezier_curve_t){
        .p1x = sanitize_unit_clamp_f32(p1x),
        .p1y = isfinite(p1y) ? p1y : 0.0f,
        .p2x = sanitize_unit_clamp_f32(p2x),
        .p2y = isfinite(p2y) ? p2y : 0.0f,
    };
}

float waywal_cubic_bezier_solve_t(const bezier_curve_t *curve, float progress)
{
    if (!isfinite(progress) || progress <= 0.0f) {
        return 0.0f;
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }
    if (!curve) {
        return progress;
    }
    if (!isfinite(curve->p1x) || !isfinite(curve->p2x)) {
        return 0.0f;
    }

    const double cx1 = fmin(fmax((double)curve->p1x, 0.0), 1.0);
    const double cx2 = fmin(fmax((double)curve->p2x, 0.0), 1.0);
    return (float)bezier_solve_t_f64(cx1, cx2, (double)progress);
}

float bezier_solve_t(const bezier_curve_t *curve, float progress)
{
    return waywal_cubic_bezier_solve_t(curve, progress);
}

float bezier_eval(const bezier_curve_t *curve, float progress)
{
    if (!isfinite(progress) || progress <= 0.0f) {
        return 0.0f;
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }
    if (!curve) {
        return progress;
    }
    if (!isfinite(curve->p1x) || !isfinite(curve->p1y) || !isfinite(curve->p2x) ||
        !isfinite(curve->p2y)) {
        return 0.0f;
    }

    const double cx1 = fmin(fmax((double)curve->p1x, 0.0), 1.0);
    const double cx2 = fmin(fmax((double)curve->p2x, 0.0), 1.0);
    const double t = bezier_solve_t_f64(cx1, cx2, (double)progress);

    const double cy = 3.0 * (double)curve->p1y;
    const double by = 3.0 * ((double)curve->p2y - (double)curve->p1y) - cy;
    const double ay = 1.0 - cy - by;
    const double y = ((ay * t + by) * t + cy) * t;
    const float yf = (float)y;
    return isfinite(yf) ? yf : 0.0f;
}
