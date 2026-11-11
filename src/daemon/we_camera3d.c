// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_camera3d.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    float a = aspect > 0.0001f ? aspect : 1.0f;
    float z_diff = near_z - far_z;
    if (fabsf(z_diff) < 1e-6f) {
        z_diff = -1.0f;
    }
    out[0] = f / a;
    out[5] = f;
    out[10] = (far_z + near_z) / z_diff;
    out[11] = -1.0f;
    out[14] = (2.0f * far_z * near_z) / z_diff;
}

void mat4_look_at(const float eye[3], const float target[3], const float up[3], float out[16])
{
    float fx = target[0] - eye[0];
    float fy = target[1] - eye[1];
    float fz = target[2] - eye[2];
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

    /* Side vector s = f x up */
    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float slen = sqrtf(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-6f) {
        sx /= slen;
        sy /= slen;
        sz /= slen;
    } else {
        sx = 1.0f;
        sy = 0.0f;
        sz = 0.0f;
    }

    /* Orthogonalized True Up u = s x f */
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

    out[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    out[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    out[14] = (fx * eye[0] + fy * eye[1] + fz * eye[2]);
    out[15] = 1.0f;
}

void mat4_ortho_gl(float left, float right, float bottom, float top, float near_z, float far_z,
                   float out[16])
{
    memset(out, 0, 16 * sizeof(float));
    float rl = right - left;
    float tb = top - bottom;
    float fn = far_z - near_z;
    out[0] = 2.0f / (fabsf(rl) > 1e-6f ? rl : 1.0f);
    out[5] = 2.0f / (fabsf(tb) > 1e-6f ? tb : 1.0f);
    out[10] = -2.0f / (fabsf(fn) > 1e-6f ? fn : 1.0f);
    out[12] = -(right + left) / (fabsf(rl) > 1e-6f ? rl : 1.0f);
    out[13] = -(top + bottom) / (fabsf(tb) > 1e-6f ? tb : 1.0f);
    out[14] = -(far_z + near_z) / (fabsf(fn) > 1e-6f ? fn : 1.0f);
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
    float len_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (len_sq > 1e-12f) {
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
    float v_len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (v_len > 1e-6f) {
        float theta = atan2f(v_len, q[3]);
        float factor = theta / v_len;
        out[0] = q[0] * factor;
        out[1] = q[1] * factor;
        out[2] = q[2] * factor;
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
    float cos_half_theta = quat_dot(q0, q1);
    float q1_adj[4] = {q1[0], q1[1], q1[2], q1[3]};
    if (cos_half_theta < 0.0f) {
        cos_half_theta = -cos_half_theta;
        q1_adj[0] = -q1_adj[0];
        q1_adj[1] = -q1_adj[1];
        q1_adj[2] = -q1_adj[2];
        q1_adj[3] = -q1_adj[3];
    }
    if (cos_half_theta > 0.9995f) {
        out[0] = q0[0] + t * (q1_adj[0] - q0[0]);
        out[1] = q0[1] + t * (q1_adj[1] - q0[1]);
        out[2] = q0[2] + t * (q1_adj[2] - q0[2]);
        out[3] = q0[3] + t * (q1_adj[3] - q0[3]);
        quat_normalize(out);
        return;
    }
    float half_theta = acosf(fminf(fmaxf(cos_half_theta, -1.0f), 1.0f));
    float sin_half_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_half_theta * cos_half_theta));
    if (fabsf(sin_half_theta) < 1e-6f) {
        out[0] = 0.5f * (q0[0] + q1_adj[0]);
        out[1] = 0.5f * (q0[1] + q1_adj[1]);
        out[2] = 0.5f * (q0[2] + q1_adj[2]);
        out[3] = 0.5f * (q0[3] + q1_adj[3]);
        quat_normalize(out);
        return;
    }
    float ratio_a = sinf((1.0f - t) * half_theta) / sin_half_theta;
    float ratio_b = sinf(t * half_theta) / sin_half_theta;
    out[0] = q0[0] * ratio_a + q1_adj[0] * ratio_b;
    out[1] = q0[1] * ratio_a + q1_adj[1] * ratio_b;
    out[2] = q0[2] * ratio_a + q1_adj[2] * ratio_b;
    out[3] = q0[3] * ratio_a + q1_adj[3] * ratio_b;
    quat_normalize(out);
}

void quat_squad(const float q0[4], const float a[4], const float b[4], const float q1[4], float t,
                float out[4])
{
    float s0[4], s1[4];
    quat_slerp(q0, q1, t, s0);
    quat_slerp(a, b, t, s1);
    quat_slerp(s0, s1, 2.0f * t * (1.0f - t), out);
    quat_normalize(out);
}

/* Computes Shoemake control point a_i = q_i * exp( -(log(q_i^-1 * q_{i-1}) + log(q_i^-1 * q_{i+1}))
 * / 4 ) */
static void compute_squad_control_point(const float q_prev[4], const float q_curr[4],
                                        const float q_next[4], float out_a[4])
{
    float inv_q[4];
    quat_inv(q_curr, inv_q);

    float d_prev[4], d_next[4];
    quat_mul(inv_q, q_prev, d_prev);
    quat_mul(inv_q, q_next, d_next);

    if (d_prev[3] < 0.0f) {
        d_prev[0] = -d_prev[0];
        d_prev[1] = -d_prev[1];
        d_prev[2] = -d_prev[2];
        d_prev[3] = -d_prev[3];
    }
    if (d_next[3] < 0.0f) {
        d_next[0] = -d_next[0];
        d_next[1] = -d_next[1];
        d_next[2] = -d_next[2];
        d_next[3] = -d_next[3];
    }

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
    float x = q[0], y = q[1], z = q[2], w = q[3];
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

    out[12] = pos ? pos[0] : 0.0f;
    out[13] = pos ? pos[1] : 0.0f;
    out[14] = pos ? pos[2] : 0.0f;
    out[15] = 1.0f;
}

/* =========================================================================
 * Centripetal Catmull-Rom Position Spline with Gauss-Legendre Arc-Length
 * ========================================================================= */

typedef struct {
    float p0[3], p1[3], p2[3], p3[3];
    float t0, t1, t2, t3;
} catmull_segment_t;

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

/* 5-point Gauss-Legendre quadrature integration of spline speed ||C'(u)|| on [t1, u_target] */
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

    if (count == 1) {
        if (out_pos) {
            memcpy(out_pos, kfs[0].position, 3 * sizeof(float));
        }
        if (out_quat) {
            memcpy(out_quat, kfs[0].orientation_quat, 4 * sizeof(float));
            quat_normalize(out_quat);
        }
        if (out_fov) {
            *out_fov = kfs[0].fov_deg > 0.0f ? kfs[0].fov_deg : 60.0f;
        }
        if (out_focal_dist) {
            *out_focal_dist = kfs[0].focal_distance > 0.0f ? kfs[0].focal_distance : 1000.0f;
        }
        if (out_aperture) {
            *out_aperture = kfs[0].aperture_fstop > 0.0f ? kfs[0].aperture_fstop : 2.8f;
        }
        return;
    }

    float t_start = kfs[0].time_sec;
    float t_end = kfs[count - 1].time_sec;
    float duration = t_end - t_start;
    if (duration <= 1e-4f) {
        duration = (float)(count - 1);
        t_start = 0.0f;
        t_end = duration;
    }

    float t_eval = (float)time_sec;
    if (loop_mode == WE_CAM_PATH_LOOP) {
        float rel = fmodf(t_eval - t_start, duration);
        if (rel < 0.0f) {
            rel += duration;
        }
        t_eval = t_start + rel;
    } else if (loop_mode == WE_CAM_PATH_PINGPONG) {
        float period = 2.0f * duration;
        float rel = fmodf(t_eval - t_start, period);
        if (rel < 0.0f) {
            rel += period;
        }
        if (rel > duration) {
            rel = period - rel;
        }
        t_eval = t_start + rel;
    } else {
        /* WE_CAM_PATH_ONCE */
        t_eval = fminf(fmaxf(t_eval, t_start), t_end);
    }

    /* Find segment i */
    uint32_t i = 0;
    for (uint32_t k = 0; k < count - 1; k++) {
        float tk = kfs[k].time_sec;
        float tk1 = kfs[k + 1].time_sec;
        if (duration == (float)(count - 1) && kfs[0].time_sec == kfs[count - 1].time_sec) {
            tk = (float)k;
            tk1 = (float)(k + 1);
        }
        if (t_eval >= tk && t_eval <= tk1) {
            i = k;
            break;
        }
    }
    if (i >= count - 1) {
        i = count - 2;
    }

    float seg_t0 = kfs[i].time_sec;
    float seg_t1 = kfs[i + 1].time_sec;
    if (duration == (float)(count - 1) && kfs[0].time_sec == kfs[count - 1].time_sec) {
        seg_t0 = (float)i;
        seg_t1 = (float)(i + 1);
    }
    float seg_dur = seg_t1 - seg_t0;
    float tau = seg_dur > 1e-5f ? (t_eval - seg_t0) / seg_dur : 0.0f;
    tau = fminf(fmaxf(tau, 0.0f), 1.0f);

    /* Construct 4 control points for Catmull-Rom */
    catmull_segment_t seg;
    if (loop_mode == WE_CAM_PATH_LOOP) {
        uint32_t idx0 = (i == 0) ? (count - 1) : (i - 1);
        uint32_t idx3 = (i + 2 < count) ? (i + 2) : ((i + 2) % count);
        memcpy(seg.p0, kfs[idx0].position, 3 * sizeof(float));
        memcpy(seg.p1, kfs[i].position, 3 * sizeof(float));
        memcpy(seg.p2, kfs[i + 1].position, 3 * sizeof(float));
        memcpy(seg.p3, kfs[idx3].position, 3 * sizeof(float));
    } else {
        memcpy(seg.p1, kfs[i].position, 3 * sizeof(float));
        memcpy(seg.p2, kfs[i + 1].position, 3 * sizeof(float));
        if (i > 0) {
            memcpy(seg.p0, kfs[i - 1].position, 3 * sizeof(float));
        } else {
            for (int k = 0; k < 3; k++) {
                seg.p0[k] = 2.0f * seg.p1[k] - seg.p2[k];
            }
        }
        if (i + 2 < count) {
            memcpy(seg.p3, kfs[i + 2].position, 3 * sizeof(float));
        } else {
            for (int k = 0; k < 3; k++) {
                seg.p3[k] = 2.0f * seg.p2[k] - seg.p1[k];
            }
        }
    }

    /* Centripetal knot parameterization: alpha = 0.5 (or tension override) */
    float alpha = kfs[i].tension > 0.0f ? kfs[i].tension : 0.5f;
    float d01 = sqrtf((seg.p1[0] - seg.p0[0]) * (seg.p1[0] - seg.p0[0]) +
                      (seg.p1[1] - seg.p0[1]) * (seg.p1[1] - seg.p0[1]) +
                      (seg.p1[2] - seg.p0[2]) * (seg.p1[2] - seg.p0[2]));
    float d12 = sqrtf((seg.p2[0] - seg.p1[0]) * (seg.p2[0] - seg.p1[0]) +
                      (seg.p2[1] - seg.p1[1]) * (seg.p2[1] - seg.p1[1]) +
                      (seg.p2[2] - seg.p1[2]) * (seg.p2[2] - seg.p1[2]));
    float d23 = sqrtf((seg.p3[0] - seg.p2[0]) * (seg.p3[0] - seg.p2[0]) +
                      (seg.p3[1] - seg.p2[1]) * (seg.p3[1] - seg.p2[1]) +
                      (seg.p3[2] - seg.p2[2]) * (seg.p3[2] - seg.p2[2]));

    float dt01 = fmaxf(powf(fmaxf(d01, 1e-4f), alpha), 1e-4f);
    float dt12 = fmaxf(powf(fmaxf(d12, 1e-4f), alpha), 1e-4f);
    float dt23 = fmaxf(powf(fmaxf(d23, 1e-4f), alpha), 1e-4f);

    seg.t0 = 0.0f;
    seg.t1 = seg.t0 + dt01;
    seg.t2 = seg.t1 + dt12;
    seg.t3 = seg.t2 + dt23;

    /* 5-point Gauss-Legendre Arc-Length Inversion */
    float total_seg_len = eval_arc_length(&seg, seg.t2);
    float target_len = tau * total_seg_len;
    float u = seg.t1 + tau * (seg.t2 - seg.t1);

    if (total_seg_len > 1e-4f) {
        float u_low = seg.t1;
        float u_high = seg.t2;
        for (int iter = 0; iter < 8; iter++) {
            float s_cur = eval_arc_length(&seg, u);
            float err = s_cur - target_len;
            if (fabsf(err) < 1e-4f) {
                break;
            }
            float deriv[3];
            eval_catmull_pos_and_deriv(&seg, u, NULL, deriv);
            float speed = sqrtf(deriv[0] * deriv[0] + deriv[1] * deriv[1] + deriv[2] * deriv[2]);
            if (speed > 1e-5f) {
                float u_step = u - err / speed;
                if (u_step >= u_low && u_step <= u_high) {
                    u = u_step;
                } else {
                    u = 0.5f * (u_low + u_high);
                }
            } else {
                u = 0.5f * (u_low + u_high);
            }
            if (err > 0.0f) {
                u_high = u;
            } else {
                u_low = u;
            }
        }
    }

    if (out_pos) {
        eval_catmull_pos_and_deriv(&seg, u, out_pos, NULL);
    }

    /* Shoemake SQUAD Orientation Interpolation */
    if (out_quat) {
        float q0[4], q1[4], q2[4], q3[4];
        if (loop_mode == WE_CAM_PATH_LOOP) {
            uint32_t idx0 = (i == 0) ? (count - 1) : (i - 1);
            uint32_t idx3 = (i + 2 < count) ? (i + 2) : ((i + 2) % count);
            memcpy(q0, kfs[idx0].orientation_quat, 4 * sizeof(float));
            memcpy(q1, kfs[i].orientation_quat, 4 * sizeof(float));
            memcpy(q2, kfs[i + 1].orientation_quat, 4 * sizeof(float));
            memcpy(q3, kfs[idx3].orientation_quat, 4 * sizeof(float));
        } else {
            memcpy(q1, kfs[i].orientation_quat, 4 * sizeof(float));
            memcpy(q2, kfs[i + 1].orientation_quat, 4 * sizeof(float));
            if (i > 0) {
                memcpy(q0, kfs[i - 1].orientation_quat, 4 * sizeof(float));
            } else {
                memcpy(q0, q1, 4 * sizeof(float));
            }
            if (i + 2 < count) {
                memcpy(q3, kfs[i + 2].orientation_quat, 4 * sizeof(float));
            } else {
                memcpy(q3, q2, 4 * sizeof(float));
            }
        }
        quat_normalize(q0);
        quat_normalize(q1);
        quat_normalize(q2);
        quat_normalize(q3);

        /* Enforce shortest-arc quaternion sequence */
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

        float a1[4], b2[4];
        compute_squad_control_point(q0, q1, q2, a1);
        compute_squad_control_point(q1, q2, q3, b2);

        quat_squad(q1, a1, b2, q2, tau, out_quat);
    }

    if (out_fov) {
        float f0 = kfs[i].fov_deg > 0.0f ? kfs[i].fov_deg : 60.0f;
        float f1 = kfs[i + 1].fov_deg > 0.0f ? kfs[i + 1].fov_deg : 60.0f;
        *out_fov = (1.0f - tau) * f0 + tau * f1;
    }
    if (out_focal_dist) {
        float d0 = kfs[i].focal_distance > 0.0f ? kfs[i].focal_distance : 1000.0f;
        float d1 = kfs[i + 1].focal_distance > 0.0f ? kfs[i + 1].focal_distance : 1000.0f;
        *out_focal_dist = (1.0f - tau) * d0 + tau * d1;
    }
    if (out_aperture) {
        float a0 = kfs[i].aperture_fstop > 0.0f ? kfs[i].aperture_fstop : 2.8f;
        float a1 = kfs[i + 1].aperture_fstop > 0.0f ? kfs[i + 1].aperture_fstop : 2.8f;
        *out_aperture = (1.0f - tau) * a0 + tau * a1;
    }
}

/* =========================================================================
 * Camera Initializer & Runtime Update Engine (Zero Heap Allocations)
 * ========================================================================= */

void we_camera3d_init_default(we_camera3d_t *cam, float viewport_w, float viewport_h)
{
    if (!cam) {
        return;
    }
    memset(cam, 0, sizeof(*cam));
    cam->projection_mode = WE_PROJ_PERSPECTIVE_3D;
    cam->path_enabled = 0;
    cam->path_loop_mode = WE_CAM_PATH_LOOP;
    cam->keyframe_count = 0;

    float w = viewport_w > 0.0f ? viewport_w : 1920.0f;
    float h = viewport_h > 0.0f ? viewport_h : 1080.0f;
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

    float aspect = aspect_ratio > 0.001f ? aspect_ratio : (16.0f / 9.0f);

    /* Step 1: Evaluate Spline Path if enabled */
    if (cam->path_enabled && cam->keyframe_count >= 1) {
        float quat[4];
        we_camera3d_eval_spline(cam->keyframes, cam->keyframe_count, cam->path_loop_mode, time_sec,
                                cam->eye_pos, quat, &cam->fov_y_deg, &cam->dof_focal_dist, NULL);

        /* Derive Look-At target and Up from unit quaternion orientation */
        float rot_mat[16];
        quat_to_mat4(quat, NULL, rot_mat);

        /* Forward vector in camera space: (0, 0, -1) */
        float fwd[3] = {-rot_mat[8], -rot_mat[9], -rot_mat[10]};
        cam->target_pos[0] = cam->eye_pos[0] + fwd[0] * 100.0f;
        cam->target_pos[1] = cam->eye_pos[1] + fwd[1] * 100.0f;
        cam->target_pos[2] = cam->eye_pos[2] + fwd[2] * 100.0f;

        cam->up_vec[0] = rot_mat[4];
        cam->up_vec[1] = rot_mat[5];
        cam->up_vec[2] = rot_mat[6];
    }

    /* Step 2: Critically Damped Mouse Parallax Offset */
    float target_px = ((pointer_norm_x * 2.0f) - 1.0f) * cam->parallax_amount[0];
    float target_py = ((pointer_norm_y * 2.0f) - 1.0f) * cam->parallax_amount[1];

    /* Camera forward and side vectors for parallax plane displacement */
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
    eye_eff[0] = cam->eye_pos[0] + target_px * sx + target_py * ux;
    eye_eff[1] = cam->eye_pos[1] + target_px * sy + target_py * uy;
    eye_eff[2] = cam->eye_pos[2] + target_px * sz + target_py * uz;

    /* Step 3: Compute View, Projection, and ViewProjection Matrices */
    if (cam->projection_mode == WE_PROJ_PERSPECTIVE_3D) {
        mat4_look_at(eye_eff, cam->target_pos, cam->up_vec, cam->view_matrix);
        float fovy_rad = cam->fov_y_deg * (float)(M_PI / 180.0);
        if (fovy_rad < 0.1f) {
            fovy_rad = 0.1f;
        }
        if (fovy_rad > 2.8f) {
            fovy_rad = 2.8f;
        }
        float nz = cam->near_z > 0.01f ? cam->near_z : 1.0f;
        float fz_val = cam->far_z > nz ? cam->far_z : (nz + 1000.0f);
        mat4_perspective_rh_gl(fovy_rad, aspect, nz, fz_val, cam->proj_matrix);
        mat4_mul_mat4(cam->proj_matrix, cam->view_matrix, cam->view_proj_matrix);
    } else {
        /* Orthographic 2D / 2.5D Parallax */
        mat4_look_at(eye_eff, cam->target_pos, cam->up_vec, cam->view_matrix);
        float half_h = 540.0f;
        float half_w = half_h * aspect;
        float nz = cam->near_z > 0.01f ? cam->near_z : 0.1f;
        float fz_val = cam->far_z > nz ? cam->far_z : 5000.0f;
        mat4_ortho_gl(-half_w, half_w, -half_h, half_h, nz, fz_val, cam->proj_matrix);
        mat4_mul_mat4(cam->proj_matrix, cam->view_matrix, cam->view_proj_matrix);
    }
}

void we_camera3d_evaluate(we_camera3d_t *cam, double time_sec, float pointer_norm_x,
                          float pointer_norm_y, float aspect_ratio)
{
    we_camera3d_update(cam, time_sec, pointer_norm_x, pointer_norm_y, aspect_ratio);
}

/* Computes thin-lens Circle-of-Confusion blur radius in pixels */
float we_camera3d_compute_coc_px(const we_camera3d_t *cam, float layer_z)
{
    if (!cam || cam->dof_enabled <= 0.0f || cam->dof_bokeh_radius_px <= 0.0f) {
        return 0.0f;
    }
    float focal_dist = cam->dof_focal_dist > 1.0f ? cam->dof_focal_dist : 1000.0f;
    float focal_region = cam->dof_focal_region >= 0.0f ? cam->dof_focal_region : 0.0f;
    float r_max = cam->dof_bokeh_radius_px > 0.0f ? cam->dof_bokeh_radius_px : 16.0f;

    float delta_z = fabsf(layer_z - focal_dist);
    float half_band = 0.5f * focal_region;
    if (delta_z <= half_band) {
        return 0.0f;
    }
    float d = delta_z - half_band;
    float coc = r_max * (d / (d + focal_dist));
    return fminf(fmaxf(coc, 0.0f), r_max);
}

/* =========================================================================
 * 18-FLOP Cofactor Inverse-Transpose Normal Matrix (M_{3x3}^-1)^T
 * ========================================================================= */

void we_camera3d_compute_normal_matrix(const float m[16], float out[9])
{
    if (!m || !out) {
        return;
    }
    /* Extract 3 columns of upper-left 3x3 block */
    float c0[3] = {m[0], m[1], m[2]};
    float c1[3] = {m[4], m[5], m[6]};
    float c2[3] = {m[8], m[9], m[10]};

    /* Adjugate matrix transpose via cross products: N = [c1 x c2 | c2 x c0 | c0 x c1] */
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
    float inv_det = fabsf(det) > 1e-12f ? (1.0f / det) : 1.0f;

    out[0] = n0[0] * inv_det;
    out[1] = n0[1] * inv_det;
    out[2] = n0[2] * inv_det;

    out[3] = n1[0] * inv_det;
    out[4] = n1[1] * inv_det;
    out[5] = n1[2] * inv_det;

    out[6] = n2[0] * inv_det;
    out[7] = n2[1] * inv_det;
    out[8] = n2[2] * inv_det;
}

/* =========================================================================
 * Dynamic 8-Light std140 UBO Packing Engine (544 Bytes)
 * ========================================================================= */

void we_lighting_pack_ubo(const we_scene_light_t *lights, uint32_t light_count,
                          const float ambient_rgb[3], const float sky_rgb[3],
                          float normal_map_strength, we_lighting_uniforms_t *out_ubo)
{
    if (!out_ubo) {
        return;
    }
    memset(out_ubo, 0, sizeof(*out_ubo));

    if (ambient_rgb) {
        out_ubo->ambient_color_rgb[0] = ambient_rgb[0];
        out_ubo->ambient_color_rgb[1] = ambient_rgb[1];
        out_ubo->ambient_color_rgb[2] = ambient_rgb[2];
    } else {
        out_ubo->ambient_color_rgb[0] = 0.1f;
        out_ubo->ambient_color_rgb[1] = 0.1f;
        out_ubo->ambient_color_rgb[2] = 0.1f;
    }

    if (sky_rgb) {
        out_ubo->sky_color_rgb[0] = sky_rgb[0];
        out_ubo->sky_color_rgb[1] = sky_rgb[1];
        out_ubo->sky_color_rgb[2] = sky_rgb[2];
    } else {
        out_ubo->sky_color_rgb[0] = 0.2f;
        out_ubo->sky_color_rgb[1] = 0.2f;
        out_ubo->sky_color_rgb[2] = 0.3f;
    }

    out_ubo->normal_map_strength = normal_map_strength > 0.0f ? normal_map_strength : 1.0f;

    uint32_t active = light_count < WE_MAX_SCENE_LIGHTS ? light_count : WE_MAX_SCENE_LIGHTS;
    out_ubo->active_light_count = active;

    if (lights && active > 0) {
        for (uint32_t i = 0; i < active; i++) {
            memcpy(&out_ubo->lights[i], &lights[i], sizeof(we_scene_light_t));
        }
    }
    for (uint32_t i = active; i < WE_MAX_SCENE_LIGHTS; i++) {
        memset(&out_ubo->lights[i], 0, sizeof(we_scene_light_t));
        out_ubo->lights[i].light_type = WE_LIGHT_DISABLED;
    }
}
