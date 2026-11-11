// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_camera3d.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float sanitize_f32(float v, float fallback) { return isfinite(v) ? v : fallback; }

static inline float clamp_f32(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* =========================================================================
 * 4x4 Matrix Mathematics (Column-Major Standard for OpenGL ES 3.1)
 * ========================================================================= */

void mat4_mul_mat4(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

void mat4_perspective_rh_gl(float fovy_rad, float aspect, float near_z, float far_z, float out[16])
{
    memset(out, 0, 16 * sizeof(float));
    float fov = clamp_f32(sanitize_f32(fovy_rad, 1.04719755f), 0.05f, 3.05f);
    float f = 1.0f / tanf(fov * 0.5f);
    float a = (isfinite(aspect) && aspect > 0.0001f) ? aspect : 1.0f;
    float nz = (isfinite(near_z) && near_z > 1e-4f) ? near_z : 0.1f;
    float fz = (isfinite(far_z) && far_z > nz + 1e-4f) ? far_z : (nz + 1000.0f);
    float z_diff = nz - fz;
    if (fabsf(z_diff) < 1e-6f) {
        z_diff = -1.0f;
    }
    out[0] = f / a;
    out[5] = f;
    out[10] = (fz + nz) / z_diff;
    out[11] = -1.0f;
    out[14] = (2.0f * fz * nz) / z_diff;
}

void mat4_look_at(const float eye[3], const float target[3], const float up[3], float out[16])
{
    float ex = sanitize_f32(eye[0], 0.0f);
    float ey = sanitize_f32(eye[1], 0.0f);
    float ez = sanitize_f32(eye[2], 1000.0f);

    float tx = sanitize_f32(target[0], ex);
    float ty = sanitize_f32(target[1], ey);
    float tz = sanitize_f32(target[2], ez - 1000.0f);

    float upx = sanitize_f32(up[0], 0.0f);
    float upy = sanitize_f32(up[1], 1.0f);
    float upz = sanitize_f32(up[2], 0.0f);

    float fx = tx - ex;
    float fy = ty - ey;
    float fz = tz - ez;
    float flen = sqrtf(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-6f) {
        fx /= flen;
        fy /= flen;
        fz /= flen;
    } else {
        fx = 0.0f;
        fy = 0.0f;
        fz = -1.0f;
    }

    float sx = fy * upz - fz * upy;
    float sy = fz * upx - fx * upz;
    float sz = fx * upy - fy * upx;
    float slen = sqrtf(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-6f) {
        sx /= slen;
        sy /= slen;
        sz /= slen;
    } else {
        /* When forward is collinear with up, choose an orthogonal reference axis. */
        if (fabsf(fx) < 0.9f) {
            sx = 0.0f;
            sy = fz;
            sz = -fy;
        } else {
            sx = -fz;
            sy = 0.0f;
            sz = fx;
        }
        slen = sqrtf(sx * sx + sy * sy + sz * sz);
        if (slen > 1e-6f) {
            sx /= slen;
            sy /= slen;
            sz /= slen;
        } else {
            sx = 1.0f;
            sy = 0.0f;
            sz = 0.0f;
        }
    }

    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    out[0] = sx;
    out[1] = ux;
    out[2] = -fx;
    out[3] = 0.0f;

    out[4] = sy;
    out[5] = uy;
    out[6] = -fy;
    out[7] = 0.0f;

    out[8] = sz;
    out[9] = uz;
    out[10] = -fz;
    out[11] = 0.0f;

    out[12] = -(sx * ex + sy * ey + sz * ez);
    out[13] = -(ux * ex + uy * ey + uz * ez);
    out[14] = (fx * ex + fy * ey + fz * ez);
    out[15] = 1.0f;
}

void mat4_ortho_gl(float left, float right, float bottom, float top, float near_z, float far_z,
                   float out[16])
{
    memset(out, 0, 16 * sizeof(float));
    float rl = right - left;
    float tb = top - bottom;
    float fn = far_z - near_z;
    if (!isfinite(rl) || fabsf(rl) <= 1e-6f) {
        rl = 1.0f;
    }
    if (!isfinite(tb) || fabsf(tb) <= 1e-6f) {
        tb = 1.0f;
    }
    if (!isfinite(fn) || fabsf(fn) <= 1e-6f) {
        fn = 1.0f;
    }
    out[0] = 2.0f / rl;
    out[5] = 2.0f / tb;
    out[10] = -2.0f / fn;
    out[12] = sanitize_f32(-(right + left) / rl, 0.0f);
    out[13] = sanitize_f32(-(top + bottom) / tb, 0.0f);
    out[14] = sanitize_f32(-(far_z + near_z) / fn, 0.0f);
    out[15] = 1.0f;
}

/* =========================================================================
 * Unit Quaternion Mathematics & Shoemake SQUAD
 * ========================================================================= */

void quat_identity(float q[4])
{
    q[0] = 0.0f;
    q[1] = 0.0f;
    q[2] = 0.0f;
    q[3] = 1.0f;
}

void quat_normalize(float q[4])
{
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        quat_identity(q);
        return;
    }
    float len_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (isfinite(len_sq) && len_sq > 1e-12f) {
        float inv_len = 1.0f / sqrtf(len_sq);
        q[0] *= inv_len;
        q[1] *= inv_len;
        q[2] *= inv_len;
        q[3] *= inv_len;
    } else {
        quat_identity(q);
    }
}

static inline float quat_dot(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void quat_mul(const float a[4], const float b[4], float out[4])
{
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(out, r, sizeof(r));
}

static void quat_inv(const float q[4], float out[4])
{
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

static void quat_log(const float q[4], float out[4])
{
    /* Canonicalize into the w >= 0 hemisphere so atan2 stays away from the pi branch cut. */
    float qn[4] = {q[0], q[1], q[2], q[3]};
    if (qn[3] < 0.0f) {
        qn[0] = -qn[0];
        qn[1] = -qn[1];
        qn[2] = -qn[2];
        qn[3] = -qn[3];
    }
    float v_len = sqrtf(qn[0] * qn[0] + qn[1] * qn[1] + qn[2] * qn[2]);
    if (v_len > 1e-6f) {
        float theta = atan2f(v_len, qn[3]);
        float factor = theta / v_len;
        out[0] = qn[0] * factor;
        out[1] = qn[1] * factor;
        out[2] = qn[2] * factor;
        out[3] = 0.0f;
    } else {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
    }
}

static void quat_exp(const float v[4], float out[4])
{
    float theta = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (theta > 1e-6f) {
        float factor = sinf(theta) / theta;
        out[0] = v[0] * factor;
        out[1] = v[1] * factor;
        out[2] = v[2] * factor;
        out[3] = cosf(theta);
    } else {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
    }
}

void quat_slerp(const float q0[4], const float q1[4], float t, float out[4])
{
    float s = clamp_f32(sanitize_f32(t, 0.0f), 0.0f, 1.0f);
    float a[4] = {q0[0], q0[1], q0[2], q0[3]};
    float b[4] = {q1[0], q1[1], q1[2], q1[3]};
    quat_normalize(a);
    quat_normalize(b);

    float cos_half_theta = quat_dot(a, b);
    if (cos_half_theta < 0.0f) {
        cos_half_theta = -cos_half_theta;
        b[0] = -b[0];
        b[1] = -b[1];
        b[2] = -b[2];
        b[3] = -b[3];
    }
    if (cos_half_theta > 0.9995f) {
        out[0] = a[0] + s * (b[0] - a[0]);
        out[1] = a[1] + s * (b[1] - a[1]);
        out[2] = a[2] + s * (b[2] - a[2]);
        out[3] = a[3] + s * (b[3] - a[3]);
        quat_normalize(out);
        return;
    }
    float half_theta = acosf(clamp_f32(cos_half_theta, -1.0f, 1.0f));
    float sin_half_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_half_theta * cos_half_theta));
    if (fabsf(sin_half_theta) < 1e-6f) {
        out[0] = 0.5f * (a[0] + b[0]);
        out[1] = 0.5f * (a[1] + b[1]);
        out[2] = 0.5f * (a[2] + b[2]);
        out[3] = 0.5f * (a[3] + b[3]);
        quat_normalize(out);
        return;
    }
    float ratio_a = sinf((1.0f - s) * half_theta) / sin_half_theta;
    float ratio_b = sinf(s * half_theta) / sin_half_theta;
    out[0] = a[0] * ratio_a + b[0] * ratio_b;
    out[1] = a[1] * ratio_a + b[1] * ratio_b;
    out[2] = a[2] * ratio_a + b[2] * ratio_b;
    out[3] = a[3] * ratio_a + b[3] * ratio_b;
    quat_normalize(out);
}

static void quat_slerp_no_invert(const float q0[4], const float q1[4], float s, float out[4])
{
    float a[4] = {q0[0], q0[1], q0[2], q0[3]};
    float b[4] = {q1[0], q1[1], q1[2], q1[3]};
    quat_normalize(a);
    quat_normalize(b);

    float cos_half_theta = clamp_f32(quat_dot(a, b), -1.0f, 1.0f);
    if (cos_half_theta > 0.9995f) {
        out[0] = a[0] + s * (b[0] - a[0]);
        out[1] = a[1] + s * (b[1] - a[1]);
        out[2] = a[2] + s * (b[2] - a[2]);
        out[3] = a[3] + s * (b[3] - a[3]);
        quat_normalize(out);
        return;
    }
    if (cos_half_theta < -0.9995f) {
        /* Exact S^3 antipode fallback along an orthogonal great circle. */
        float ortho[4] = {-a[1], a[0], -a[3], a[2]};
        float ang = s * (float)M_PI;
        float ca = cosf(ang);
        float sa = sinf(ang);
        out[0] = a[0] * ca + ortho[0] * sa;
        out[1] = a[1] * ca + ortho[1] * sa;
        out[2] = a[2] * ca + ortho[2] * sa;
        out[3] = a[3] * ca + ortho[3] * sa;
        quat_normalize(out);
        return;
    }
    float half_theta = acosf(cos_half_theta);
    float sin_half_theta = sqrtf(fmaxf(1e-12f, 1.0f - cos_half_theta * cos_half_theta));
    float ratio_a = sinf((1.0f - s) * half_theta) / sin_half_theta;
    float ratio_b = sinf(s * half_theta) / sin_half_theta;
    out[0] = a[0] * ratio_a + b[0] * ratio_b;
    out[1] = a[1] * ratio_a + b[1] * ratio_b;
    out[2] = a[2] * ratio_a + b[2] * ratio_b;
    out[3] = a[3] * ratio_a + b[3] * ratio_b;
    quat_normalize(out);
}

void quat_squad(const float q0[4], const float a[4], const float b[4], const float q1[4], float t,
                float out[4])
{
    float s = clamp_f32(sanitize_f32(t, 0.0f), 0.0f, 1.0f);
    float q1_al[4] = {q1[0], q1[1], q1[2], q1[3]};
    if (quat_dot(q0, q1_al) < 0.0f) {
        q1_al[0] = -q1_al[0];
        q1_al[1] = -q1_al[1];
        q1_al[2] = -q1_al[2];
        q1_al[3] = -q1_al[3];
    }
    /* Keep control points a and b in their anchor endpoints' hemispheres and interpolate via
     * SlerpNoInvert so 180-deg spatial segments (where dot(a, b) < 0) never flip direction. */
    float a_al[4] = {a[0], a[1], a[2], a[3]};
    if (quat_dot(q0, a_al) < 0.0f) {
        a_al[0] = -a_al[0];
        a_al[1] = -a_al[1];
        a_al[2] = -a_al[2];
        a_al[3] = -a_al[3];
    }
    float b_al[4] = {b[0], b[1], b[2], b[3]};
    if (quat_dot(q1_al, b_al) < 0.0f) {
        b_al[0] = -b_al[0];
        b_al[1] = -b_al[1];
        b_al[2] = -b_al[2];
        b_al[3] = -b_al[3];
    }

    float s0[4], s1[4];
    quat_slerp_no_invert(q0, q1_al, s, s0);
    quat_slerp_no_invert(a_al, b_al, s, s1);
    quat_slerp(s0, s1, 2.0f * s * (1.0f - s), out);
    quat_normalize(out);
}

static void compute_squad_control_point(const float q_prev[4], const float q_curr[4],
                                        const float q_next[4], float out_a[4])
{
    float inv_q[4];
    quat_inv(q_curr, inv_q);

    float d_prev[4], d_next[4];
    quat_mul(inv_q, q_prev, d_prev);
    quat_mul(inv_q, q_next, d_next);

    float log_prev[4], log_next[4];
    quat_log(d_prev, log_prev);
    quat_log(d_next, log_next);

    float sum[4];
    sum[0] = -0.25f * (log_prev[0] + log_next[0]);
    sum[1] = -0.25f * (log_prev[1] + log_next[1]);
    sum[2] = -0.25f * (log_prev[2] + log_next[2]);
    sum[3] = 0.0f;

    float exp_sum[4];
    quat_exp(sum, exp_sum);
    quat_mul(q_curr, exp_sum, out_a);
    quat_normalize(out_a);
}

void quat_to_mat4(const float q[4], const float pos[3], float out[16])
{
    float qn[4] = {q[0], q[1], q[2], q[3]};
    quat_normalize(qn);
    float x = qn[0], y = qn[1], z = qn[2], w = qn[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    out[0] = 1.0f - (yy + zz);
    out[1] = xy + wz;
    out[2] = xz - wy;
    out[3] = 0.0f;

    out[4] = xy - wz;
    out[5] = 1.0f - (xx + zz);
    out[6] = yz + wx;
    out[7] = 0.0f;

    out[8] = xz + wy;
    out[9] = yz - wx;
    out[10] = 1.0f - (xx + yy);
    out[11] = 0.0f;

    out[12] = pos ? sanitize_f32(pos[0], 0.0f) : 0.0f;
    out[13] = pos ? sanitize_f32(pos[1], 0.0f) : 0.0f;
    out[14] = pos ? sanitize_f32(pos[2], 0.0f) : 0.0f;
    out[15] = 1.0f;
}

/* =========================================================================
 * Centripetal Catmull-Rom Position Spline & Pre-Baked Arc-Length + SQUAD LUT
 * ========================================================================= */

#define WE_CAM_ARC_LUT_STEPS        16u
#define WE_CAM_PATH_CACHE_WAYS      2u
#define WE_CAM_PARALLAX_CACHE_SLOTS 8u

typedef struct {
    float p0[3], p1[3], p2[3], p3[3];
    float t0, t1, t2, t3;
} catmull_segment_t;

typedef struct {
    catmull_segment_t catmull;
    float total_len;
    float u_lut[WE_CAM_ARC_LUT_STEPS + 1u];
    float q1[4];
    float a1[4];
    float b2[4];
    float q2[4];
    float t0;
    float t1;
    float fov0, fov1;
    float focal0, focal1;
    float aperture0, aperture1;
} baked_cam_segment_t;

typedef struct {
    uint64_t hash;
    uint32_t count;
    uint32_t loop_mode;
    uint64_t last_use_tick;
    double t_start;
    double t_end;
    double duration;
    float knot_times[WE_CAMERA_MAX_KEYFRAMES];
    baked_cam_segment_t segs[WE_CAMERA_MAX_KEYFRAMES];
    bool valid;
} baked_cam_path_t;

static _Thread_local baked_cam_path_t s_baked_paths[WE_CAM_PATH_CACHE_WAYS];
static _Thread_local uint64_t s_baked_tick = 0;

static void eval_catmull_pos_and_deriv(const catmull_segment_t *s, float u, float out_pos[3],
                                       float out_deriv[3])
{
    float dt10 = s->t1 - s->t0 > 1e-5f ? s->t1 - s->t0 : 1.0f;
    float dt21 = s->t2 - s->t1 > 1e-5f ? s->t2 - s->t1 : 1.0f;
    float dt32 = s->t3 - s->t2 > 1e-5f ? s->t3 - s->t2 : 1.0f;
    float dt20 = s->t2 - s->t0 > 1e-5f ? s->t2 - s->t0 : 1.0f;
    float dt31 = s->t3 - s->t1 > 1e-5f ? s->t3 - s->t1 : 1.0f;

    float a1[3], a2[3], a3[3];
    float da1[3], da2[3], da3[3];
    for (int k = 0; k < 3; k++) {
        a1[k] = ((s->t1 - u) * s->p0[k] + (u - s->t0) * s->p1[k]) / dt10;
        a2[k] = ((s->t2 - u) * s->p1[k] + (u - s->t1) * s->p2[k]) / dt21;
        a3[k] = ((s->t3 - u) * s->p2[k] + (u - s->t2) * s->p3[k]) / dt32;
        da1[k] = (s->p1[k] - s->p0[k]) / dt10;
        da2[k] = (s->p2[k] - s->p1[k]) / dt21;
        da3[k] = (s->p3[k] - s->p2[k]) / dt32;
    }

    float b1[3], b2[3];
    float db1[3], db2[3];
    for (int k = 0; k < 3; k++) {
        b1[k] = ((s->t2 - u) * a1[k] + (u - s->t0) * a2[k]) / dt20;
        b2[k] = ((s->t3 - u) * a2[k] + (u - s->t1) * a3[k]) / dt31;
        db1[k] = (a2[k] - a1[k] + (s->t2 - u) * da1[k] + (u - s->t0) * da2[k]) / dt20;
        db2[k] = (a3[k] - a2[k] + (s->t3 - u) * da2[k] + (u - s->t1) * da3[k]) / dt31;
    }

    if (out_pos) {
        for (int k = 0; k < 3; k++) {
            out_pos[k] = ((s->t2 - u) * b1[k] + (u - s->t1) * b2[k]) / dt21;
        }
    }
    if (out_deriv) {
        for (int k = 0; k < 3; k++) {
            out_deriv[k] = (b2[k] - b1[k] + (s->t2 - u) * db1[k] + (u - s->t1) * db2[k]) / dt21;
        }
    }
}

static float eval_arc_length(const catmull_segment_t *s, float u_target)
{
    static const float xi[5] = {
        0.0f, 0.538469310105683f, -0.538469310105683f, 0.906179845938664f, -0.906179845938664f,
    };
    static const float w[5] = {
        0.568888888888889f, 0.478628670499366f, 0.478628670499366f,
        0.236926885056189f, 0.236926885056189f,
    };

    float half_len = 0.5f * (u_target - s->t1);
    float mid = 0.5f * (u_target + s->t1);
    float sum = 0.0f;

    for (int m = 0; m < 5; m++) {
        float sample_u = half_len * xi[m] + mid;
        float deriv[3];
        eval_catmull_pos_and_deriv(s, sample_u, NULL, deriv);
        float speed = sqrtf(deriv[0] * deriv[0] + deriv[1] * deriv[1] + deriv[2] * deriv[2]);
        sum += w[m] * speed;
    }
    return half_len * sum;
}

static float solve_arc_length_exact(const catmull_segment_t *seg, float target_len,
                                    float u_low_init)
{
    float u_low = u_low_init;
    float u_high = seg->t2;
    float u = 0.5f * (u_low + u_high);
    for (int iter = 0; iter < 10; iter++) {
        float s_cur = eval_arc_length(seg, u);
        float err = s_cur - target_len;
        if (fabsf(err) < 1e-6f) {
            break;
        }
        if (err > 0.0f) {
            u_high = u;
        } else {
            u_low = u;
        }
        float deriv[3];
        eval_catmull_pos_and_deriv(seg, u, NULL, deriv);
        float speed = sqrtf(deriv[0] * deriv[0] + deriv[1] * deriv[1] + deriv[2] * deriv[2]);
        if (speed > 1e-5f) {
            float u_step = u - err / speed;
            if (u_step > u_low && u_step < u_high) {
                u = u_step;
            } else {
                u = 0.5f * (u_low + u_high);
            }
        } else {
            u = 0.5f * (u_low + u_high);
        }
    }
    return u;
}

static uint64_t hash_keyframes_fnv1a(const we_camera_kf_t *kfs, uint32_t count, uint32_t loop_mode)
{
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)count;
    h *= 1099511628211ULL;
    h ^= (uint64_t)loop_mode;
    h *= 1099511628211ULL;

    const uint8_t *bytes = (const uint8_t *)kfs;
    size_t len = (size_t)count * sizeof(we_camera_kf_t);
    size_t idx = 0;
    while (idx + 8u <= len) {
        uint64_t word = 0;
        memcpy(&word, bytes + idx, sizeof(word));
        h ^= word;
        h *= 1099511628211ULL;
        idx += 8u;
    }
    while (idx < len) {
        h ^= (uint64_t)bytes[idx++];
        h *= 1099511628211ULL;
    }
    return h;
}

static void sanitize_single_kf(const we_camera_kf_t *src, const float fallback_pos[3],
                               we_camera_kf_t *dst)
{
    dst->time_sec = src->time_sec;
    dst->position[0] = sanitize_f32(src->position[0], fallback_pos[0]);
    dst->position[1] = sanitize_f32(src->position[1], fallback_pos[1]);
    dst->position[2] = sanitize_f32(src->position[2], fallback_pos[2]);
    memcpy(dst->orientation_quat, src->orientation_quat, 4 * sizeof(float));
    quat_normalize(dst->orientation_quat);
    dst->fov_deg = (isfinite(src->fov_deg) && src->fov_deg > 0.0f)
                       ? clamp_f32(src->fov_deg, 1.0f, 179.0f)
                       : 60.0f;
    dst->focal_distance = (isfinite(src->focal_distance) && src->focal_distance > 0.0f)
                              ? src->focal_distance
                              : 1000.0f;
    dst->aperture_fstop =
        (isfinite(src->aperture_fstop) && src->aperture_fstop > 0.0f) ? src->aperture_fstop : 2.8f;
    dst->tension = (isfinite(src->tension) && src->tension > 0.0f)
                       ? clamp_f32(src->tension, 0.01f, 1.0f)
                       : 0.5f;
}

static const baked_cam_path_t *get_or_bake_cam_path(const we_camera_kf_t *kfs, uint32_t count,
                                                    uint32_t loop_mode)
{
    uint64_t hash = hash_keyframes_fnv1a(kfs, count, loop_mode);
    for (uint32_t w = 0; w < WE_CAM_PATH_CACHE_WAYS; w++) {
        baked_cam_path_t *slot = &s_baked_paths[w];
        if (slot->valid && slot->count == count && slot->loop_mode == loop_mode &&
            slot->hash == hash) {
            slot->last_use_tick = ++s_baked_tick;
            return slot;
        }
    }

    uint32_t victim = 0;
    for (uint32_t w = 1; w < WE_CAM_PATH_CACHE_WAYS; w++) {
        if (!s_baked_paths[w].valid ||
            s_baked_paths[w].last_use_tick < s_baked_paths[victim].last_use_tick) {
            victim = w;
        }
    }

    baked_cam_path_t *path = &s_baked_paths[victim];
    path->valid = false;
    path->hash = hash;
    path->count = count;
    path->loop_mode = loop_mode;
    path->last_use_tick = ++s_baked_tick;

    we_camera_kf_t clean[WE_CAMERA_MAX_KEYFRAMES];
    float prev_pos[3] = {0.0f, 0.0f, 1000.0f};
    bool all_times_finite = true;
    for (uint32_t k = 0; k < count; k++) {
        sanitize_single_kf(&kfs[k], prev_pos, &clean[k]);
        memcpy(prev_pos, clean[k].position, sizeof(prev_pos));
        if (!isfinite(clean[k].time_sec)) {
            all_times_finite = false;
        }
    }

    if (all_times_finite) {
        /* Stable insertion sort keeps equal-timestamp hard cuts in authored order. */
        for (uint32_t k = 1; k < count; k++) {
            we_camera_kf_t cur = clean[k];
            uint32_t j = k;
            while (j > 0 && clean[j - 1].time_sec > cur.time_sec) {
                clean[j] = clean[j - 1];
                j--;
            }
            clean[j] = cur;
        }
    }

    double raw_dur =
        all_times_finite ? ((double)clean[count - 1].time_sec - (double)clean[0].time_sec) : 0.0;
    bool synthetic_times = (!isfinite(raw_dur) || raw_dur <= 1e-4);
    for (uint32_t k = 0; k < count; k++) {
        path->knot_times[k] = synthetic_times ? (float)k : clean[k].time_sec;
    }
    path->t_start = (double)path->knot_times[0];
    path->t_end = (double)path->knot_times[count - 1];
    path->duration = path->t_end - path->t_start;

    /* Pre-align the keyframe quaternion chain sequentially so interior junctions share signs. */
    for (uint32_t k = 1; k < count; k++) {
        if (quat_dot(clean[k - 1].orientation_quat, clean[k].orientation_quat) < 0.0f) {
            clean[k].orientation_quat[0] = -clean[k].orientation_quat[0];
            clean[k].orientation_quat[1] = -clean[k].orientation_quat[1];
            clean[k].orientation_quat[2] = -clean[k].orientation_quat[2];
            clean[k].orientation_quat[3] = -clean[k].orientation_quat[3];
        }
    }

    for (uint32_t i = 0; i < count - 1; i++) {
        baked_cam_segment_t *bs = &path->segs[i];
        bs->t0 = path->knot_times[i];
        bs->t1 = path->knot_times[i + 1];
        bs->fov0 = clean[i].fov_deg;
        bs->fov1 = clean[i + 1].fov_deg;
        bs->focal0 = clean[i].focal_distance;
        bs->focal1 = clean[i + 1].focal_distance;
        bs->aperture0 = clean[i].aperture_fstop;
        bs->aperture1 = clean[i + 1].aperture_fstop;

        catmull_segment_t *seg = &bs->catmull;
        if (loop_mode == WE_CAM_PATH_LOOP) {
            uint32_t idx0 = (i == 0) ? (count - 1) : (i - 1);
            uint32_t idx3 = (i + 2 < count) ? (i + 2) : ((i + 2) % count);
            memcpy(seg->p0, clean[idx0].position, 3 * sizeof(float));
            memcpy(seg->p1, clean[i].position, 3 * sizeof(float));
            memcpy(seg->p2, clean[i + 1].position, 3 * sizeof(float));
            memcpy(seg->p3, clean[idx3].position, 3 * sizeof(float));
        } else {
            memcpy(seg->p1, clean[i].position, 3 * sizeof(float));
            memcpy(seg->p2, clean[i + 1].position, 3 * sizeof(float));
            if (i > 0) {
                memcpy(seg->p0, clean[i - 1].position, 3 * sizeof(float));
            } else {
                for (int c = 0; c < 3; c++) {
                    seg->p0[c] = 2.0f * seg->p1[c] - seg->p2[c];
                }
            }
            if (i + 2 < count) {
                memcpy(seg->p3, clean[i + 2].position, 3 * sizeof(float));
            } else {
                for (int c = 0; c < 3; c++) {
                    seg->p3[c] = 2.0f * seg->p2[c] - seg->p1[c];
                }
            }
        }

        float alpha = clean[i].tension;
        float d01 = sqrtf((seg->p1[0] - seg->p0[0]) * (seg->p1[0] - seg->p0[0]) +
                          (seg->p1[1] - seg->p0[1]) * (seg->p1[1] - seg->p0[1]) +
                          (seg->p1[2] - seg->p0[2]) * (seg->p1[2] - seg->p0[2]));
        float d12 = sqrtf((seg->p2[0] - seg->p1[0]) * (seg->p2[0] - seg->p1[0]) +
                          (seg->p2[1] - seg->p1[1]) * (seg->p2[1] - seg->p1[1]) +
                          (seg->p2[2] - seg->p1[2]) * (seg->p2[2] - seg->p1[2]));
        float d23 = sqrtf((seg->p3[0] - seg->p2[0]) * (seg->p3[0] - seg->p2[0]) +
                          (seg->p3[1] - seg->p2[1]) * (seg->p3[1] - seg->p2[1]) +
                          (seg->p3[2] - seg->p2[2]) * (seg->p3[2] - seg->p2[2]));

        float dt01 = fmaxf(powf(fmaxf(d01, 1e-4f), alpha), 1e-4f);
        float dt12 = fmaxf(powf(fmaxf(d12, 1e-4f), alpha), 1e-4f);
        float dt23 = fmaxf(powf(fmaxf(d23, 1e-4f), alpha), 1e-4f);

        seg->t0 = 0.0f;
        seg->t1 = seg->t0 + dt01;
        seg->t2 = seg->t1 + dt12;
        seg->t3 = seg->t2 + dt23;

        bs->total_len = eval_arc_length(seg, seg->t2);
        bs->u_lut[0] = seg->t1;
        bs->u_lut[WE_CAM_ARC_LUT_STEPS] = seg->t2;
        if (bs->total_len > 1e-5f) {
            float u_prev = seg->t1;
            for (uint32_t s = 1; s < WE_CAM_ARC_LUT_STEPS; s++) {
                float target = ((float)s / (float)WE_CAM_ARC_LUT_STEPS) * bs->total_len;
                u_prev = solve_arc_length_exact(seg, target, u_prev);
                bs->u_lut[s] = u_prev;
            }
        } else {
            for (uint32_t s = 1; s < WE_CAM_ARC_LUT_STEPS; s++) {
                float frac = (float)s / (float)WE_CAM_ARC_LUT_STEPS;
                bs->u_lut[s] = seg->t1 + frac * (seg->t2 - seg->t1);
            }
        }

        float q0[4], q1[4], q2[4], q3[4];
        if (loop_mode == WE_CAM_PATH_LOOP) {
            uint32_t idx0 = (i == 0) ? (count - 1) : (i - 1);
            uint32_t idx3 = (i + 2 < count) ? (i + 2) : ((i + 2) % count);
            memcpy(q0, clean[idx0].orientation_quat, 4 * sizeof(float));
            memcpy(q1, clean[i].orientation_quat, 4 * sizeof(float));
            memcpy(q2, clean[i + 1].orientation_quat, 4 * sizeof(float));
            memcpy(q3, clean[idx3].orientation_quat, 4 * sizeof(float));
        } else {
            memcpy(q1, clean[i].orientation_quat, 4 * sizeof(float));
            memcpy(q2, clean[i + 1].orientation_quat, 4 * sizeof(float));
            memcpy(q0, (i > 0) ? clean[i - 1].orientation_quat : q1, 4 * sizeof(float));
            memcpy(q3, (i + 2 < count) ? clean[i + 2].orientation_quat : q2, 4 * sizeof(float));
        }

        /* Align loop wrap neighbors locally relative to q1 without disturbing interior signs. */
        if (quat_dot(q0, q1) < 0.0f) {
            q0[0] = -q0[0];
            q0[1] = -q0[1];
            q0[2] = -q0[2];
            q0[3] = -q0[3];
        }
        if (quat_dot(q1, q2) < 0.0f) {
            q2[0] = -q2[0];
            q2[1] = -q2[1];
            q2[2] = -q2[2];
            q2[3] = -q2[3];
        }
        if (quat_dot(q2, q3) < 0.0f) {
            q3[0] = -q3[0];
            q3[1] = -q3[1];
            q3[2] = -q3[2];
            q3[3] = -q3[3];
        }

        memcpy(bs->q1, q1, sizeof(bs->q1));
        memcpy(bs->q2, q2, sizeof(bs->q2));
        compute_squad_control_point(q0, q1, q2, bs->a1);
        compute_squad_control_point(q1, q2, q3, bs->b2);
    }

    path->valid = true;
    return path;
}

void we_camera3d_eval_spline(const we_camera_kf_t *kfs, uint32_t count, uint32_t loop_mode,
                             double time_sec, float out_pos[3], float out_quat[4], float *out_fov,
                             float *out_focal_dist, float *out_aperture)
{
    if (!kfs || count == 0) {
        if (out_pos) {
            out_pos[0] = 0.0f;
            out_pos[1] = 0.0f;
            out_pos[2] = 1000.0f;
        }
        if (out_quat) {
            quat_identity(out_quat);
        }
        if (out_fov) {
            *out_fov = 60.0f;
        }
        if (out_focal_dist) {
            *out_focal_dist = 1000.0f;
        }
        if (out_aperture) {
            *out_aperture = 2.8f;
        }
        return;
    }

    if (count > WE_CAMERA_MAX_KEYFRAMES) {
        count = WE_CAMERA_MAX_KEYFRAMES;
    }

    if (count == 1) {
        we_camera_kf_t clean0;
        const float def_pos[3] = {0.0f, 0.0f, 1000.0f};
        sanitize_single_kf(&kfs[0], def_pos, &clean0);
        if (out_pos) {
            memcpy(out_pos, clean0.position, 3 * sizeof(float));
        }
        if (out_quat) {
            memcpy(out_quat, clean0.orientation_quat, 4 * sizeof(float));
        }
        if (out_fov) {
            *out_fov = clean0.fov_deg;
        }
        if (out_focal_dist) {
            *out_focal_dist = clean0.focal_distance;
        }
        if (out_aperture) {
            *out_aperture = clean0.aperture_fstop;
        }
        return;
    }

    const baked_cam_path_t *path = get_or_bake_cam_path(kfs, count, loop_mode);
    double t_in = isfinite(time_sec) ? time_sec : path->t_start;
    double t_eval_d = path->t_start;

    if (loop_mode == WE_CAM_PATH_LOOP && path->duration > 0.0) {
        double rel = fmod(t_in - path->t_start, path->duration);
        if (rel < 0.0) {
            rel += path->duration;
        }
        t_eval_d = path->t_start + rel;
    } else if (loop_mode == WE_CAM_PATH_PINGPONG && path->duration > 0.0) {
        double period = 2.0 * path->duration;
        double rel = fmod(t_in - path->t_start, period);
        if (rel < 0.0) {
            rel += period;
        }
        if (rel > path->duration) {
            rel = period - rel;
        }
        t_eval_d = path->t_start + rel;
    } else {
        if (t_in < path->t_start) {
            t_eval_d = path->t_start;
        } else if (t_in > path->t_end) {
            t_eval_d = path->t_end;
        } else {
            t_eval_d = t_in;
        }
    }

    float t_eval = (float)t_eval_d;

    /* Binary search for the last segment whose start knot is <= t_eval. */
    uint32_t seg_idx = 0;
    if (t_eval >= path->knot_times[count - 1]) {
        seg_idx = count - 2;
    } else if (t_eval > path->knot_times[0]) {
        uint32_t lo = 0;
        uint32_t hi = count - 1;
        while (lo + 1u < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1u);
            if (path->knot_times[mid] <= t_eval) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        seg_idx = (lo < count - 1) ? lo : (count - 2);
    }

    const baked_cam_segment_t *bs = &path->segs[seg_idx];
    float seg_dur = bs->t1 - bs->t0;
    float tau = 0.0f;
    if (seg_dur > 1e-6f) {
        tau = clamp_f32((t_eval - bs->t0) / seg_dur, 0.0f, 1.0f);
    } else {
        tau = (t_eval >= bs->t1) ? 1.0f : 0.0f;
    }

    if (out_pos) {
        float u = bs->catmull.t1;
        if (tau >= 1.0f) {
            u = bs->catmull.t2;
        } else if (tau > 0.0f) {
            if (bs->total_len > 1e-5f) {
                float scaled = tau * (float)WE_CAM_ARC_LUT_STEPS;
                uint32_t lut_i = (uint32_t)scaled;
                if (lut_i >= WE_CAM_ARC_LUT_STEPS) {
                    lut_i = WE_CAM_ARC_LUT_STEPS - 1u;
                }
                float frac = scaled - (float)lut_i;
                float u_lo = bs->u_lut[lut_i];
                float u_hi = bs->u_lut[lut_i + 1u];
                u = u_lo + frac * (u_hi - u_lo);

                float target_len = tau * bs->total_len;
                float err = eval_arc_length(&bs->catmull, u) - target_len;
                if (fabsf(err) > 1e-6f) {
                    float deriv[3];
                    eval_catmull_pos_and_deriv(&bs->catmull, u, NULL, deriv);
                    float speed =
                        sqrtf(deriv[0] * deriv[0] + deriv[1] * deriv[1] + deriv[2] * deriv[2]);
                    if (speed > 1e-5f) {
                        float u_next = u - err / speed;
                        if (u_next >= u_lo && u_next <= u_hi) {
                            u = u_next;
                        }
                    }
                }
            } else {
                u = bs->catmull.t1 + tau * (bs->catmull.t2 - bs->catmull.t1);
            }
        }
        eval_catmull_pos_and_deriv(&bs->catmull, u, out_pos, NULL);
    }

    if (out_quat) {
        quat_squad(bs->q1, bs->a1, bs->b2, bs->q2, tau, out_quat);
    }

    if (out_fov) {
        *out_fov = (1.0f - tau) * bs->fov0 + tau * bs->fov1;
    }
    if (out_focal_dist) {
        *out_focal_dist = (1.0f - tau) * bs->focal0 + tau * bs->focal1;
    }
    if (out_aperture) {
        *out_aperture = (1.0f - tau) * bs->aperture0 + tau * bs->aperture1;
    }
}

/* =========================================================================
 * Camera Initializer & Runtime Update Engine (Zero Heap Allocations)
 * ========================================================================= */

typedef struct {
    uintptr_t cam_key;
    uint64_t last_use_tick;
    double last_time_sec;
    float px;
    float py;
    bool initialized;
} cam_parallax_state_t;

static _Thread_local cam_parallax_state_t s_parallax_states[WE_CAM_PARALLAX_CACHE_SLOTS];
static _Thread_local uint64_t s_parallax_tick = 0;

static void reset_parallax_slot_for_cam(const we_camera3d_t *cam)
{
    uintptr_t key = (uintptr_t)cam;
    for (uint32_t i = 0; i < WE_CAM_PARALLAX_CACHE_SLOTS; i++) {
        if (s_parallax_states[i].cam_key == key) {
            memset(&s_parallax_states[i], 0, sizeof(s_parallax_states[i]));
        }
    }
}

static cam_parallax_state_t *get_parallax_slot_for_cam(const we_camera3d_t *cam)
{
    uintptr_t key = (uintptr_t)cam;
    for (uint32_t i = 0; i < WE_CAM_PARALLAX_CACHE_SLOTS; i++) {
        if (s_parallax_states[i].cam_key == key) {
            s_parallax_states[i].last_use_tick = ++s_parallax_tick;
            return &s_parallax_states[i];
        }
    }
    uint32_t victim = 0;
    for (uint32_t i = 1; i < WE_CAM_PARALLAX_CACHE_SLOTS; i++) {
        if (s_parallax_states[i].cam_key == 0 ||
            s_parallax_states[i].last_use_tick < s_parallax_states[victim].last_use_tick) {
            victim = i;
        }
    }
    memset(&s_parallax_states[victim], 0, sizeof(s_parallax_states[victim]));
    s_parallax_states[victim].cam_key = key;
    s_parallax_states[victim].last_use_tick = ++s_parallax_tick;
    return &s_parallax_states[victim];
}

void we_camera3d_init_default(we_camera3d_t *cam, float viewport_w, float viewport_h)
{
    if (!cam) {
        return;
    }
    reset_parallax_slot_for_cam(cam);
    memset(cam, 0, sizeof(*cam));
    cam->projection_mode = WE_PROJ_PERSPECTIVE_3D;
    cam->path_enabled = 0;
    cam->path_loop_mode = WE_CAM_PATH_LOOP;
    cam->keyframe_count = 0;

    float w = (isfinite(viewport_w) && viewport_w > 0.0f) ? viewport_w : 1920.0f;
    float h = (isfinite(viewport_h) && viewport_h > 0.0f) ? viewport_h : 1080.0f;
    float aspect = w / h;

    cam->eye_pos[0] = w * 0.5f;
    cam->eye_pos[1] = h * 0.5f;
    cam->eye_pos[2] = 1000.0f;

    cam->target_pos[0] = w * 0.5f;
    cam->target_pos[1] = h * 0.5f;
    cam->target_pos[2] = 0.0f;

    cam->up_vec[0] = 0.0f;
    cam->up_vec[1] = 1.0f;
    cam->up_vec[2] = 0.0f;

    cam->fov_y_deg = 60.0f;
    cam->near_z = 1.0f;
    cam->far_z = 10000.0f;

    cam->parallax_amount[0] = 0.0f;
    cam->parallax_amount[1] = 0.0f;
    cam->parallax_smoothing = 0.15f;

    cam->dof_enabled = 0.0f;
    cam->dof_focal_dist = 1000.0f;
    cam->dof_focal_region = 200.0f;
    cam->dof_bokeh_radius_px = 0.0f;
    cam->reserved_pad = 0.0f;

    mat4_look_at(cam->eye_pos, cam->target_pos, cam->up_vec, cam->view_matrix);
    mat4_perspective_rh_gl(cam->fov_y_deg * (float)(M_PI / 180.0), aspect, cam->near_z, cam->far_z,
                           cam->proj_matrix);
    mat4_mul_mat4(cam->proj_matrix, cam->view_matrix, cam->view_proj_matrix);
}

void we_camera3d_update(we_camera3d_t *cam, double time_sec, float pointer_norm_x,
                        float pointer_norm_y, float aspect_ratio)
{
    if (!cam) {
        return;
    }

    double t_sec = isfinite(time_sec) ? time_sec : 0.0;
    float ptr_x = clamp_f32(sanitize_f32(pointer_norm_x, 0.5f), -2.0f, 3.0f);
    float ptr_y = clamp_f32(sanitize_f32(pointer_norm_y, 0.5f), -2.0f, 3.0f);
    float aspect =
        (isfinite(aspect_ratio) && aspect_ratio > 0.001f) ? aspect_ratio : (16.0f / 9.0f);

    cam->eye_pos[0] = sanitize_f32(cam->eye_pos[0], 960.0f);
    cam->eye_pos[1] = sanitize_f32(cam->eye_pos[1], 540.0f);
    cam->eye_pos[2] = sanitize_f32(cam->eye_pos[2], 1000.0f);
    cam->target_pos[0] = sanitize_f32(cam->target_pos[0], cam->eye_pos[0]);
    cam->target_pos[1] = sanitize_f32(cam->target_pos[1], cam->eye_pos[1]);
    cam->target_pos[2] = sanitize_f32(cam->target_pos[2], 0.0f);
    cam->up_vec[0] = sanitize_f32(cam->up_vec[0], 0.0f);
    cam->up_vec[1] = sanitize_f32(cam->up_vec[1], 1.0f);
    cam->up_vec[2] = sanitize_f32(cam->up_vec[2], 0.0f);

    if (cam->path_enabled && cam->keyframe_count >= 1) {
        uint32_t kf_count = cam->keyframe_count <= WE_CAMERA_MAX_KEYFRAMES
                                ? cam->keyframe_count
                                : WE_CAMERA_MAX_KEYFRAMES;
        float quat[4];
        float aperture_fstop = 2.8f;
        we_camera3d_eval_spline(cam->keyframes, kf_count, cam->path_loop_mode, t_sec, cam->eye_pos,
                                quat, &cam->fov_y_deg, &cam->dof_focal_dist, &aperture_fstop);

        if (isfinite(cam->dof_enabled) && cam->dof_enabled > 0.5f && aperture_fstop > 0.05f) {
            bool has_authored_aperture = false;
            for (uint32_t k = 0; k < kf_count; k++) {
                if (isfinite(cam->keyframes[k].aperture_fstop) &&
                    cam->keyframes[k].aperture_fstop > 0.05f) {
                    has_authored_aperture = true;
                    break;
                }
            }
            if (has_authored_aperture || !isfinite(cam->dof_bokeh_radius_px) ||
                cam->dof_bokeh_radius_px <= 0.0f) {
                cam->dof_bokeh_radius_px = clamp_f32(22.4f / aperture_fstop, 0.25f, 16.0f);
            }
        }

        float rot_mat[16];
        quat_to_mat4(quat, NULL, rot_mat);

        float fwd[3] = {-rot_mat[8], -rot_mat[9], -rot_mat[10]};
        cam->target_pos[0] = cam->eye_pos[0] + fwd[0] * 100.0f;
        cam->target_pos[1] = cam->eye_pos[1] + fwd[1] * 100.0f;
        cam->target_pos[2] = cam->eye_pos[2] + fwd[2] * 100.0f;

        cam->up_vec[0] = rot_mat[4];
        cam->up_vec[1] = rot_mat[5];
        cam->up_vec[2] = rot_mat[6];
    }

    float pax = sanitize_f32(cam->parallax_amount[0], 0.0f);
    float pay = sanitize_f32(cam->parallax_amount[1], 0.0f);
    float target_px = ((ptr_x * 2.0f) - 1.0f) * pax;
    float target_py = ((ptr_y * 2.0f) - 1.0f) * pay;

    float smooth_px = target_px;
    float smooth_py = target_py;
    float smoothing = sanitize_f32(cam->parallax_smoothing, 1.0f);
    if (pax != 0.0f || pay != 0.0f) {
        cam_parallax_state_t *st = get_parallax_slot_for_cam(cam);
        if (!st->initialized || smoothing <= 0.0f || smoothing >= 0.999f) {
            st->px = target_px;
            st->py = target_py;
            st->last_time_sec = t_sec;
            st->initialized = true;
        } else {
            double dt = t_sec - st->last_time_sec;
            if (dt < 0.0 || dt > 1.0) {
                st->px = target_px;
                st->py = target_py;
                st->last_time_sec = t_sec;
            } else if (dt > 0.0) {
                float s = clamp_f32(smoothing, 0.001f, 0.999f);
                float alpha = 1.0f - expf(logf(1.0f - s) * (float)dt * 60.0f);
                st->px += alpha * (target_px - st->px);
                st->py += alpha * (target_py - st->py);
                st->last_time_sec = t_sec;
            }
        }
        smooth_px = st->px;
        smooth_py = st->py;
    }

    float fx = cam->target_pos[0] - cam->eye_pos[0];
    float fy = cam->target_pos[1] - cam->eye_pos[1];
    float fz = cam->target_pos[2] - cam->eye_pos[2];
    float flen = sqrtf(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-5f) {
        fx /= flen;
        fy /= flen;
        fz /= flen;
    } else {
        fx = 0.0f;
        fy = 0.0f;
        fz = -1.0f;
    }

    float sx = fy * cam->up_vec[2] - fz * cam->up_vec[1];
    float sy = fz * cam->up_vec[0] - fx * cam->up_vec[2];
    float sz = fx * cam->up_vec[1] - fy * cam->up_vec[0];
    float slen = sqrtf(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-5f) {
        sx /= slen;
        sy /= slen;
        sz /= slen;
    } else {
        sx = 1.0f;
        sy = 0.0f;
        sz = 0.0f;
    }

    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    float eye_eff[3];
    eye_eff[0] = cam->eye_pos[0] + smooth_px * sx + smooth_py * ux;
    eye_eff[1] = cam->eye_pos[1] + smooth_px * sy + smooth_py * uy;
    eye_eff[2] = cam->eye_pos[2] + smooth_px * sz + smooth_py * uz;

    if (cam->projection_mode == WE_PROJ_PERSPECTIVE_3D) {
        mat4_look_at(eye_eff, cam->target_pos, cam->up_vec, cam->view_matrix);
        float fov_deg = sanitize_f32(cam->fov_y_deg, 60.0f);
        float fovy_rad = clamp_f32(fov_deg * (float)(M_PI / 180.0), 0.1f, 2.8f);
        float nz = (isfinite(cam->near_z) && cam->near_z > 0.01f) ? cam->near_z : 1.0f;
        float fz_val = (isfinite(cam->far_z) && cam->far_z > nz) ? cam->far_z : (nz + 1000.0f);
        mat4_perspective_rh_gl(fovy_rad, aspect, nz, fz_val, cam->proj_matrix);
        mat4_mul_mat4(cam->proj_matrix, cam->view_matrix, cam->view_proj_matrix);
    } else {
        /* Scene FBO row 0 is the top of the wallpaper, so orthographic top maps to -half_h. */
        mat4_look_at(eye_eff, cam->target_pos, cam->up_vec, cam->view_matrix);
        float half_h = 540.0f;
        float half_w = half_h * aspect;
        float nz = (isfinite(cam->near_z) && cam->near_z > 0.01f) ? cam->near_z : 0.1f;
        float fz_val = (isfinite(cam->far_z) && cam->far_z > nz) ? cam->far_z : 5000.0f;
        mat4_ortho_gl(-half_w, half_w, half_h, -half_h, nz, fz_val, cam->proj_matrix);
        mat4_mul_mat4(cam->proj_matrix, cam->view_matrix, cam->view_proj_matrix);
    }
}

void we_camera3d_evaluate(we_camera3d_t *cam, double time_sec, float pointer_norm_x,
                          float pointer_norm_y, float aspect_ratio)
{
    we_camera3d_update(cam, time_sec, pointer_norm_x, pointer_norm_y, aspect_ratio);
}

float we_camera3d_compute_coc_px(const we_camera3d_t *cam, float layer_z)
{
    if (!cam || !isfinite(layer_z) || !isfinite(cam->dof_enabled) || cam->dof_enabled <= 0.0f ||
        !isfinite(cam->dof_bokeh_radius_px) || cam->dof_bokeh_radius_px <= 0.0f) {
        return 0.0f;
    }
    float focal_dist = (isfinite(cam->dof_focal_dist) && cam->dof_focal_dist > 1.0f)
                           ? cam->dof_focal_dist
                           : 1000.0f;
    float focal_region = (isfinite(cam->dof_focal_region) && cam->dof_focal_region >= 0.0f)
                             ? cam->dof_focal_region
                             : 0.0f;
    float r_max = clamp_f32(cam->dof_bokeh_radius_px, 0.0f, 64.0f);

    float delta_z = fabsf(layer_z - focal_dist);
    float half_band = 0.5f * focal_region;
    if (delta_z <= half_band) {
        return 0.0f;
    }
    float d = delta_z - half_band;
    float coc = r_max * (d / (d + focal_dist));
    return clamp_f32(sanitize_f32(coc, 0.0f), 0.0f, r_max);
}

/* =========================================================================
 * 18-FLOP Cofactor Inverse-Transpose Normal Matrix (M_{3x3}^-1)^T
 * ========================================================================= */

static void set_mat3_identity(float out[9])
{
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    out[4] = 1.0f;
    out[5] = 0.0f;
    out[6] = 0.0f;
    out[7] = 0.0f;
    out[8] = 1.0f;
}

void we_camera3d_compute_normal_matrix(const float m[16], float out[9])
{
    if (!m || !out) {
        return;
    }
    if (!isfinite(m[0]) || !isfinite(m[1]) || !isfinite(m[2]) || !isfinite(m[4]) ||
        !isfinite(m[5]) || !isfinite(m[6]) || !isfinite(m[8]) || !isfinite(m[9]) ||
        !isfinite(m[10])) {
        set_mat3_identity(out);
        return;
    }

    float c0[3] = {m[0], m[1], m[2]};
    float c1[3] = {m[4], m[5], m[6]};
    float c2[3] = {m[8], m[9], m[10]};

    float n0[3] = {
        c1[1] * c2[2] - c1[2] * c2[1],
        c1[2] * c2[0] - c1[0] * c2[2],
        c1[0] * c2[1] - c1[1] * c2[0],
    };
    float n1[3] = {
        c2[1] * c0[2] - c2[2] * c0[1],
        c2[2] * c0[0] - c2[0] * c0[2],
        c2[0] * c0[1] - c2[1] * c0[0],
    };
    float n2[3] = {
        c0[1] * c1[2] - c0[2] * c1[1],
        c0[2] * c1[0] - c0[0] * c1[2],
        c0[0] * c1[1] - c0[1] * c1[0],
    };

    float det = c0[0] * n0[0] + c0[1] * n0[1] + c0[2] * n0[2];
    /* Degenerate flattened layers have zero determinant; fall back to identity normals. */
    if (!isfinite(det) || fabsf(det) <= 1e-8f) {
        set_mat3_identity(out);
        return;
    }

    float inv_det = 1.0f / det;
    out[0] = n0[0] * inv_det;
    out[1] = n0[1] * inv_det;
    out[2] = n0[2] * inv_det;

    out[3] = n1[0] * inv_det;
    out[4] = n1[1] * inv_det;
    out[5] = n1[2] * inv_det;

    out[6] = n2[0] * inv_det;
    out[7] = n2[1] * inv_det;
    out[8] = n2[2] * inv_det;

    for (int i = 0; i < 9; i++) {
        if (!isfinite(out[i])) {
            set_mat3_identity(out);
            return;
        }
    }
}

/* =========================================================================
 * Dynamic 8-Light std140 UBO Packing Engine (544 Bytes)
 * ========================================================================= */

static void normalize_vec3_fallback(float v[3], float fx, float fy, float fz)
{
    v[0] = sanitize_f32(v[0], 0.0f);
    v[1] = sanitize_f32(v[1], 0.0f);
    v[2] = sanitize_f32(v[2], 0.0f);
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (isfinite(len) && len > 1e-5f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    } else {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
    }
}

void we_lighting_pack_ubo(const we_scene_light_t *lights, uint32_t light_count,
                          const float ambient_rgb[3], const float sky_rgb[3],
                          float normal_map_strength, we_lighting_uniforms_t *out_ubo)
{
    if (!out_ubo) {
        return;
    }
    memset(out_ubo, 0, sizeof(*out_ubo));

    if (ambient_rgb) {
        out_ubo->ambient_color_rgb[0] = clamp_f32(sanitize_f32(ambient_rgb[0], 0.1f), 0.0f, 100.0f);
        out_ubo->ambient_color_rgb[1] = clamp_f32(sanitize_f32(ambient_rgb[1], 0.1f), 0.0f, 100.0f);
        out_ubo->ambient_color_rgb[2] = clamp_f32(sanitize_f32(ambient_rgb[2], 0.1f), 0.0f, 100.0f);
    } else {
        out_ubo->ambient_color_rgb[0] = 0.1f;
        out_ubo->ambient_color_rgb[1] = 0.1f;
        out_ubo->ambient_color_rgb[2] = 0.1f;
    }

    if (sky_rgb) {
        out_ubo->sky_color_rgb[0] = clamp_f32(sanitize_f32(sky_rgb[0], 0.2f), 0.0f, 100.0f);
        out_ubo->sky_color_rgb[1] = clamp_f32(sanitize_f32(sky_rgb[1], 0.2f), 0.0f, 100.0f);
        out_ubo->sky_color_rgb[2] = clamp_f32(sanitize_f32(sky_rgb[2], 0.3f), 0.0f, 100.0f);
    } else {
        out_ubo->sky_color_rgb[0] = 0.2f;
        out_ubo->sky_color_rgb[1] = 0.2f;
        out_ubo->sky_color_rgb[2] = 0.3f;
    }

    out_ubo->normal_map_strength = (isfinite(normal_map_strength) && normal_map_strength >= 0.0f)
                                       ? clamp_f32(normal_map_strength, 0.0f, 10.0f)
                                       : 1.0f;

    uint32_t active = 0;
    if (lights && light_count > 0) {
        active = light_count < WE_MAX_SCENE_LIGHTS ? light_count : WE_MAX_SCENE_LIGHTS;
    }
    out_ubo->active_light_count = active;

    for (uint32_t i = 0; i < active; i++) {
        we_scene_light_t *dst = &out_ubo->lights[i];
        memcpy(dst, &lights[i], sizeof(we_scene_light_t));

        if (dst->light_type > WE_LIGHT_TUBE) {
            dst->light_type = WE_LIGHT_DISABLED;
        }

        dst->position_or_dir[0] = sanitize_f32(dst->position_or_dir[0], 0.0f);
        dst->position_or_dir[1] = sanitize_f32(dst->position_or_dir[1], 0.0f);
        dst->position_or_dir[2] = sanitize_f32(dst->position_or_dir[2], 0.0f);
        if (dst->light_type == WE_LIGHT_DIRECTIONAL) {
            normalize_vec3_fallback(dst->position_or_dir, 0.0f, 0.0f, -1.0f);
        }

        dst->color_rgb[0] = clamp_f32(sanitize_f32(dst->color_rgb[0], 0.0f), 0.0f, 100.0f);
        dst->color_rgb[1] = clamp_f32(sanitize_f32(dst->color_rgb[1], 0.0f), 0.0f, 100.0f);
        dst->color_rgb[2] = clamp_f32(sanitize_f32(dst->color_rgb[2], 0.0f), 0.0f, 100.0f);
        dst->intensity = clamp_f32(sanitize_f32(dst->intensity, 0.0f), 0.0f, 10000.0f);
        dst->attenuation_radius =
            (isfinite(dst->attenuation_radius) && dst->attenuation_radius >= 1e-3f)
                ? clamp_f32(dst->attenuation_radius, 1e-3f, 1.0e6f)
                : 1.0f;
        dst->tube_length = (isfinite(dst->tube_length) && dst->tube_length >= 0.0f)
                               ? clamp_f32(dst->tube_length, 0.0f, 1.0e5f)
                               : 0.0f;
        dst->specular_power = (isfinite(dst->specular_power) && dst->specular_power >= 1.0f)
                                  ? clamp_f32(dst->specular_power, 1.0f, 2048.0f)
                                  : 32.0f;

        dst->spot_dir[0] = sanitize_f32(dst->spot_dir[0], 0.0f);
        dst->spot_dir[1] = sanitize_f32(dst->spot_dir[1], 0.0f);
        dst->spot_dir[2] = sanitize_f32(dst->spot_dir[2], -1.0f);
        if (dst->light_type == WE_LIGHT_SPOT || dst->light_type == WE_LIGHT_TUBE) {
            normalize_vec3_fallback(dst->spot_dir, 0.0f, 0.0f, -1.0f);
        }

        /* GLSL smoothstep(spot_outer_cos, spot_inner_cos, x) requires outer < inner. */
        float inner = clamp_f32(sanitize_f32(dst->spot_inner_cos, 0.8660254f), -1.0f, 1.0f);
        float outer = clamp_f32(sanitize_f32(dst->spot_outer_cos, 0.7071068f), -1.0f, 1.0f);
        if (inner < outer) {
            float tmp = inner;
            inner = outer;
            outer = tmp;
        }
        if (inner - outer < 1e-4f) {
            if (inner <= 1.0f - 1e-4f) {
                inner = outer + 1e-4f;
            } else {
                outer = inner - 1e-4f;
            }
        }
        dst->spot_inner_cos = inner;
        dst->spot_outer_cos = outer;
    }

    for (uint32_t i = active; i < WE_MAX_SCENE_LIGHTS; i++) {
        memset(&out_ubo->lights[i], 0, sizeof(we_scene_light_t));
        out_ubo->lights[i].light_type = WE_LIGHT_DISABLED;
    }
}
