// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_particle.h"

#include "waywal/log.h"
#include "waywal/we_json.h"

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <ctype.h>
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t safe_f32_to_u32(float v, uint32_t max_val)
{
    if (!isfinite(v) || v <= 0.0f) {
        return 0u;
    }
    double dv = (double)v;
    if (dv >= (double)max_val) {
        return max_val;
    }
    return (uint32_t)dv;
}

static int32_t safe_f32_to_i32(float v, int32_t def_val)
{
    if (!isfinite(v)) {
        return def_val;
    }
    double dv = (double)v;
    if (dv <= -2147483648.0) {
        return INT32_MIN;
    }
    if (dv >= 2147483647.0) {
        return INT32_MAX;
    }
    return (int32_t)dv;
}

typedef we_jval_t pj_val_t;
#define PJ_ARR JVAL_ARR
#define PJ_OBJ JVAL_OBJ

static inline void pj_free(pj_val_t *v) { we_jval_free(v); }

static inline const pj_val_t *pj_get(const pj_val_t *obj, const char *key)
{
    return we_jobj_get(obj, key);
}

static inline const char *pj_str(const pj_val_t *v, const char *def) { return we_jval_str(v, def); }

static inline uint32_t pj_floats(const pj_val_t *v, float *out, uint32_t max_n)
{
    return we_jval_floats(v, NULL, out, max_n, NULL, 0);
}

static inline float pj_float(const pj_val_t *v, float def)
{
    return we_jval_float(v, NULL, def, NULL, 0);
}

static inline float pj_finite(const pj_val_t *v, float def)
{
    float x = pj_float(v, def);
    return isfinite(x) ? x : def;
}

static inline float clamp_coord(float x, float def_val)
{
    if (!isfinite(x)) {
        return def_val;
    }
    return fminf(fmaxf(x, -1.0e6f), 1.0e6f);
}

static void pj_vec3(const pj_val_t *v, float out[3], float d0, float d1, float d2)
{
    out[0] = d0;
    out[1] = d1;
    out[2] = d2;
    uint32_t n = pj_floats(v, out, 3);
    if (n == 1) {
        out[0] = clamp_coord(out[0], d0);
        out[1] = out[0];
        out[2] = out[0];
    } else if (n > 0) {
        out[0] = clamp_coord(out[0], d0);
        out[1] = clamp_coord(out[1], d1);
        out[2] = clamp_coord(out[2], d2);
    }
}

static void normalize_color_vec3(float c[3])
{
    for (int k = 0; k < 3; k++) {
        if (!isfinite(c[k])) {
            c[k] = 1.0f;
        }
    }
    if (c[0] > 1.001f || c[1] > 1.001f || c[2] > 1.001f) {
        c[0] /= 255.0f;
        c[1] /= 255.0f;
        c[2] /= 255.0f;
    }
    for (int k = 0; k < 3; k++) {
        c[k] = fminf(fmaxf(c[k], 0.0f), 1.0f);
    }
}

/* =========================================================================
 * Fast Xorshift64* PRNG & 3D Curl-Noise
 * ========================================================================= */

static inline float prng_u01(uint64_t *state)
{
    uint64_t x = *state;
    if (x == 0) {
        x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    uint64_t r = x * 0x2545F4914F6CDD1DULL;
    return (float)((r >> 40) & 0xFFFFFFu) / 16777216.0f;
}

static inline float prng_sym(uint64_t *state) { return prng_u01(state) * 2.0f - 1.0f; }

static inline float prng_range(uint64_t *state, float lo, float hi)
{
    return lo + prng_u01(state) * (hi - lo);
}

/* Smooth 3D trigonometric potential field curl-noise: divergence-free velocity perturbation */
static inline void eval_curl_noise_3d(float x, float y, float z, float out_curl[3])
{
    float sx = sinf(x), cx = cosf(x);
    float sy = sinf(y), cy = cosf(y);
    float sz = sinf(z), cz = cosf(z);

    /* Potential vector psi = (sin(y) + cos(z), sin(z) + cos(x), sin(x) + cos(y))
     * curl(psi) = (dPsi_z/dy - dPsi_y/dz, dPsi_x/dz - dPsi_z/dx, dPsi_y/dx - dPsi_x/dy) */
    out_curl[0] = -sy - cz;
    out_curl[1] = -sz - cx;
    out_curl[2] = -sx - cy;
}

static void bind_soa_pointers(we_particle_sys_t *sys)
{
    uint32_t stride = sys->padded_cap;
    float *p = sys->soa_block;
    sys->pos_x = p + 0u * stride;
    sys->pos_y = p + 1u * stride;
    sys->pos_z = p + 2u * stride;
    sys->init_pos_x = p + 3u * stride;
    sys->init_pos_y = p + 4u * stride;
    sys->init_pos_z = p + 5u * stride;
    sys->vel_x = p + 6u * stride;
    sys->vel_y = p + 7u * stride;
    sys->vel_z = p + 8u * stride;
    sys->acc_x = p + 9u * stride;
    sys->acc_y = p + 10u * stride;
    sys->acc_z = p + 11u * stride;
    sys->r = p + 12u * stride;
    sys->g = p + 13u * stride;
    sys->b = p + 14u * stride;
    sys->a = p + 15u * stride;
    sys->init_r = p + 16u * stride;
    sys->init_g = p + 17u * stride;
    sys->init_b = p + 18u * stride;
    sys->init_a = p + 19u * stride;
    sys->size = p + 20u * stride;
    sys->init_size = p + 21u * stride;
    sys->rot_x = p + 22u * stride;
    sys->rot_y = p + 23u * stride;
    sys->rot_z = p + 24u * stride;
    sys->ang_vel_x = p + 25u * stride;
    sys->ang_vel_y = p + 26u * stride;
    sys->ang_vel_z = p + 27u * stride;
    sys->age = p + 28u * stride;
    sys->max_life = p + 29u * stride;
    sys->rng_phase = p + 30u * stride;
}

static void release_particle_gl_buffers(we_particle_sys_t *sys)
{
    if (!sys) {
        return;
    }
    EGLContext cur = eglGetCurrentContext();
    if (cur != EGL_NO_CONTEXT && sys->gl_owner_ctx == (void *)cur) {
        GLuint b;
        if (sys->gl_ebo) {
            b = sys->gl_ebo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_vbo) {
            b = sys->gl_vbo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_vao) {
            b = sys->gl_vao;
            glDeleteVertexArrays(1, &b);
        }
        if (sys->gl_inst_vbo) {
            b = sys->gl_inst_vbo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_inst_vao) {
            b = sys->gl_inst_vao;
            glDeleteVertexArrays(1, &b);
        }
        if (sys->gl_ssbo[0]) {
            GLuint sb[2] = {sys->gl_ssbo[0], sys->gl_ssbo[1]};
            glDeleteBuffers(2, sb);
        }
        if (sys->gl_indirect_bo) {
            b = sys->gl_indirect_bo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_op_ssbo) {
            b = sys->gl_op_ssbo;
            glDeleteBuffers(1, &b);
        }
    }
    sys->gl_ebo = 0u;
    sys->gl_vbo = 0u;
    sys->gl_vao = 0u;
    sys->gl_inst_vbo = 0u;
    sys->gl_inst_vao = 0u;
    sys->gl_ssbo[0] = 0u;
    sys->gl_ssbo[1] = 0u;
    sys->gl_indirect_bo = 0u;
    sys->gl_op_ssbo = 0u;
    sys->gl_quad_cap = 0u;
    sys->gl_ssbo_cap = 0u;
    sys->gl_owner_ctx = NULL;
    sys->gl_uploaded = false;
    sys->gl_gpu_sim_active = false;
}

bool we_particle_realloc_capacity(we_particle_sys_t *sys, uint32_t new_max_count)
{
    if (!sys) {
        return false;
    }
    if (new_max_count == 0u) {
        new_max_count = 1u;
    }
    if (new_max_count > WE_PARTICLE_MAX_CAP) {
        new_max_count = WE_PARTICLE_MAX_CAP;
    }
    uint32_t new_padded = (new_max_count + 7u) & ~7u;
    if (new_padded <= sys->padded_cap) {
        /* Preserve live particles until natural expiration; emitters cap new spawns at max_count.
         */
        sys->max_count = new_max_count;
        return true;
    }

    size_t total_floats = (size_t)WE_PARTICLE_SOA_ARRAYS * (size_t)new_padded;
    size_t alloc_bytes = (total_floats * sizeof(float) + 31u) & ~(size_t)31u;
    float *new_soa = (float *)aligned_alloc(32, alloc_bytes);
    if (!new_soa) {
        return false;
    }
    memset(new_soa, 0, alloc_bytes);

    if (sys->soa_block && sys->alive_count > 0u) {
        uint32_t copy_n = sys->alive_count <= sys->padded_cap ? sys->alive_count : sys->padded_cap;
        for (uint32_t a = 0; a < WE_PARTICLE_SOA_ARRAYS; a++) {
            memcpy(new_soa + (size_t)a * new_padded, sys->soa_block + (size_t)a * sys->padded_cap,
                   copy_n * sizeof(float));
        }
    }

    release_particle_gl_buffers(sys);
    free(sys->soa_block);
    free(sys->vert_buf);
    free(sys->inst_buf);
    sys->soa_block = new_soa;
    sys->vert_buf = NULL;
    sys->inst_buf = NULL;
    sys->inst_cap = 0u;
    sys->inst_count = 0u;
    sys->max_count = new_max_count;
    sys->padded_cap = new_padded;
    sys->quad_cap = new_padded;
    sys->gl_gpu_state_dirty = true;
    bind_soa_pointers(sys);
    return true;
}

void we_particle_reset_and_prewarm(we_particle_sys_t *sys)
{
    if (!sys) {
        return;
    }
    sys->alive_count = 0u;
    sys->sim_time = 0.0;
    sys->rng_state = 0x853C49E6748FEA9BULL;
    sys->cpu_channel_dirty = 0u;
    sys->gl_pending_burst = 0u;
    sys->gl_gpu_sim_active = false;
    sys->gl_gpu_state_dirty = true;
    for (uint32_t e = 0; e < sys->emitter_count; e++) {
        sys->emitters[e].emit_accum = 0.0f;
        sys->emitters[e].burst_done = false;
    }
    if (isfinite(sys->start_time) && sys->start_time > 0.0f) {
        float prewarm = fminf(sys->start_time, 10.0f);
        const float step_dt = 0.05f;
        int steps = (int)(prewarm / step_dt);
        for (int s = 0; s < steps; s++) {
            we_particle_update(sys, step_dt);
        }
        float rem = prewarm - (float)steps * step_dt;
        if (rem > 1e-4f) {
            we_particle_update(sys, rem);
        }
    }
}

/* =========================================================================
 * Particle Creation & JSON Parsing
 * ========================================================================= */

static void normalize_fpair(float *lo, float *hi)
{
    if (*lo > *hi) {
        float tmp = *lo;
        *lo = *hi;
        *hi = tmp;
    }
}

static void parse_json_renderer(we_particle_sys_t *sys, const pj_val_t *root)
{
    sys->renderer_type = WE_RENDERER_SPRITE;
    sys->trail_length = 0.05f;
    sys->trail_min_length = 1.0f;
    sys->trail_max_length = 100.0f;
    sys->subdivision = 1u;
    const pj_val_t *rends = pj_get(root, "renderer");
    if (!rends || rends->type != PJ_ARR || rends->arr.count == 0) {
        return;
    }
    const pj_val_t *rn = rends->arr.items[0];
    if (!rn || rn->type != PJ_OBJ) {
        return;
    }
    const char *rname = pj_str(pj_get(rn, "name"), "sprite");
    if (strcasecmp(rname, "spritetrail") == 0) {
        sys->renderer_type = WE_RENDERER_SPRITETRAIL;
    } else if (strcasecmp(rname, "ropetrail") == 0 || strcasecmp(rname, "rope") == 0) {
        sys->renderer_type = WE_RENDERER_ROPETRAIL;
    } else {
        sys->renderer_type = WE_RENDERER_SPRITE;
    }
    sys->trail_length = fminf(fmaxf(pj_finite(pj_get(rn, "length"), 0.05f), 0.0f), 100.0f);
    sys->trail_min_length = fminf(fmaxf(pj_finite(pj_get(rn, "minlength"), 1.0f), 0.0f), 10000.0f);
    sys->trail_max_length =
        fminf(fmaxf(pj_finite(pj_get(rn, "maxlength"), 100.0f), 0.0f), 10000.0f);
    normalize_fpair(&sys->trail_min_length, &sys->trail_max_length);
    uint32_t sub = safe_f32_to_u32(pj_finite(pj_get(rn, "subdivision"), 1.0f), 64u);
    sys->subdivision = (sub > 0u) ? sub : 1u;
}

static void parse_json_control_points(we_particle_sys_t *sys, const pj_val_t *root)
{
    for (uint32_t c = 0; c < WE_PARTICLE_MAX_CONTROL_PTS; c++) {
        sys->control_points[c].id = (int32_t)c;
    }
    const pj_val_t *cps = pj_get(root, "controlpoint");
    if (!cps || cps->type != PJ_ARR) {
        return;
    }
    for (size_t i = 0; i < cps->arr.count && i < WE_PARTICLE_MAX_CONTROL_PTS; i++) {
        const pj_val_t *cn = cps->arr.items[i];
        if (!cn || cn->type != PJ_OBJ) {
            continue;
        }
        int32_t cid = safe_f32_to_i32(pj_finite(pj_get(cn, "id"), (float)i), (int32_t)i);
        if (cid < 0 || cid >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
            cid = (int32_t)i;
        }
        we_particle_cp_t *cp = &sys->control_points[cid];
        cp->id = cid;
        pj_vec3(pj_get(cn, "offset"), cp->offset, 0.0f, 0.0f, 0.0f);
        memcpy(cp->pos, cp->offset, sizeof(cp->pos));
        int32_t cp_flags = safe_f32_to_i32(pj_finite(pj_get(cn, "flags"), 0.0f), 0);
        cp->lock_to_pointer = (cp_flags & 1) != 0 ||
                              (pj_finite(pj_get(cn, "locktopointer"), 0.0f) != 0.0f) ||
                              (pj_finite(pj_get(cn, "link_mouse"), 0.0f) != 0.0f);
        cp->world_space =
            (cp_flags & 2) != 0 || (pj_finite(pj_get(cn, "worldspace"), 0.0f) != 0.0f);
        if ((uint32_t)(cid + 1) > sys->control_point_count) {
            sys->control_point_count = (uint32_t)(cid + 1);
        }
    }
}

static void parse_json_emitters(we_particle_sys_t *sys, const pj_val_t *root)
{
    const pj_val_t *emits = pj_get(root, "emitter");
    if (!emits || emits->type != PJ_ARR) {
        return;
    }
    for (size_t i = 0; i < emits->arr.count && sys->emitter_count < WE_PARTICLE_MAX_EMITTERS; i++) {
        const pj_val_t *en = emits->arr.items[i];
        if (!en || en->type != PJ_OBJ) {
            continue;
        }
        we_particle_emitter_t *em = &sys->emitters[sys->emitter_count++];
        memset(em, 0, sizeof(*em));
        const char *ename = pj_str(pj_get(en, "name"), "boxrandom");
        if (strcasecmp(ename, "sphererandom") == 0 || strcasecmp(ename, "sphere") == 0) {
            em->type = WE_EMITTER_SPHERERANDOM;
        } else if (strcasecmp(ename, "point") == 0) {
            em->type = WE_EMITTER_POINT;
        } else {
            em->type = WE_EMITTER_BOXRANDOM;
        }
        pj_vec3(pj_get(en, "distancemin"), em->distance_min, 0.0f, 0.0f, 0.0f);
        pj_vec3(pj_get(en, "distancemax"), em->distance_max, 100.0f, 100.0f, 0.0f);
        if (em->type == WE_EMITTER_SPHERERANDOM) {
            em->distance_min[0] = fmaxf(0.0f, em->distance_min[0]);
            em->distance_max[0] = fmaxf(0.0f, em->distance_max[0]);
        }
        for (int k = 0; k < 3; k++) {
            normalize_fpair(&em->distance_min[k], &em->distance_max[k]);
        }
        pj_vec3(pj_get(en, "directions"), em->directions, 1.0f, 1.0f, 1.0f);
        pj_vec3(pj_get(en, "origin"), em->origin, 0.0f, 0.0f, 0.0f);
        float base_speed = clamp_coord(pj_finite(pj_get(en, "speed"), 0.0f), 0.0f);
        const pj_val_t *esmin = pj_get(en, "speedmin");
        if (!esmin) {
            esmin = pj_get(en, "minspeed");
        }
        const pj_val_t *esmax = pj_get(en, "speedmax");
        if (!esmax) {
            esmax = pj_get(en, "maxspeed");
        }
        em->speed_min = clamp_coord(pj_finite(esmin, base_speed), base_speed);
        em->speed_max = clamp_coord(
            pj_finite(esmax, em->speed_min > base_speed ? em->speed_min : base_speed), base_speed);
        normalize_fpair(&em->speed_min, &em->speed_max);
        em->rate = fminf(fmaxf(pj_finite(pj_get(en, "rate"), 10.0f), 0.0f),
                         (float)WE_PARTICLE_MAX_CAP * 100.0f);
        em->instantaneous =
            safe_f32_to_u32(pj_finite(pj_get(en, "instantaneous"), 0.0f), WE_PARTICLE_MAX_CAP);
        int32_t cp_idx = safe_f32_to_i32(pj_finite(pj_get(en, "controlpoint"), 0.0f), 0);
        if (cp_idx < 0 || cp_idx >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
            cp_idx = 0;
        }
        em->control_point = cp_idx;
    }
}

static void parse_json_initializers(we_particle_sys_t *sys, const pj_val_t *root)
{
    const pj_val_t *inits = pj_get(root, "initializer");
    if (!inits || inits->type != PJ_ARR) {
        return;
    }
    for (size_t i = 0;
         i < inits->arr.count && sys->initializer_count < WE_PARTICLE_MAX_INITIALIZERS; i++) {
        const pj_val_t *in = inits->arr.items[i];
        if (!in || in->type != PJ_OBJ) {
            continue;
        }
        const char *iname = pj_str(pj_get(in, "name"), "");
        we_particle_initializer_t *iz = &sys->initializers[sys->initializer_count++];
        memset(iz, 0, sizeof(*iz));
        iz->exponent = fminf(fmaxf(pj_finite(pj_get(in, "exponent"), 1.0f), 0.01f), 64.0f);

        if (strcasecmp(iname, "lifetimerandom") == 0) {
            iz->type = WE_INIT_LIFETIME_RANDOM;
            iz->min_val[0] = fmaxf(0.001f, clamp_coord(pj_finite(pj_get(in, "min"), 1.0f), 1.0f));
            iz->max_val[0] = fmaxf(0.001f, clamp_coord(pj_finite(pj_get(in, "max"), 2.0f), 2.0f));
            normalize_fpair(&iz->min_val[0], &iz->max_val[0]);
        } else if (strcasecmp(iname, "sizerandom") == 0) {
            iz->type = WE_INIT_SIZE_RANDOM;
            iz->min_val[0] = fmaxf(0.0f, clamp_coord(pj_finite(pj_get(in, "min"), 10.0f), 10.0f));
            iz->max_val[0] = fmaxf(0.0f, clamp_coord(pj_finite(pj_get(in, "max"), 20.0f), 20.0f));
            normalize_fpair(&iz->min_val[0], &iz->max_val[0]);
        } else if (strcasecmp(iname, "alpharandom") == 0) {
            iz->type = WE_INIT_ALPHA_RANDOM;
            iz->min_val[0] = fminf(fmaxf(pj_finite(pj_get(in, "min"), 0.5f), 0.0f), 1.0f);
            iz->max_val[0] = fminf(fmaxf(pj_finite(pj_get(in, "max"), 1.0f), 0.0f), 1.0f);
            normalize_fpair(&iz->min_val[0], &iz->max_val[0]);
        } else if (strcasecmp(iname, "colorrandom") == 0) {
            iz->type = WE_INIT_COLOR_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 1.0f, 1.0f, 1.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 1.0f, 1.0f, 1.0f);
            normalize_color_vec3(iz->min_val);
            normalize_color_vec3(iz->max_val);
            for (int c = 0; c < 3; c++) {
                normalize_fpair(&iz->min_val[c], &iz->max_val[c]);
            }
        } else if (strcasecmp(iname, "velocityrandom") == 0) {
            iz->type = WE_INIT_VELOCITY_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, -10.0f, -10.0f, 0.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 10.0f, 10.0f, 0.0f);
            for (int c = 0; c < 3; c++) {
                normalize_fpair(&iz->min_val[c], &iz->max_val[c]);
            }
        } else if (strcasecmp(iname, "rotationrandom") == 0) {
            iz->type = WE_INIT_ROTATION_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 0.0f, 0.0f, 0.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 0.0f, 0.0f, 6.2831853f);
            for (int c = 0; c < 3; c++) {
                normalize_fpair(&iz->min_val[c], &iz->max_val[c]);
            }
        } else if (strcasecmp(iname, "angularvelocityrandom") == 0) {
            iz->type = WE_INIT_ANGULAR_VELOCITY_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 0.0f, 0.0f, -1.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 0.0f, 0.0f, 1.0f);
            for (int c = 0; c < 3; c++) {
                normalize_fpair(&iz->min_val[c], &iz->max_val[c]);
            }
        } else if (strcasecmp(iname, "turbulentvelocityrandom") == 0) {
            iz->type = WE_INIT_TURBULENT_VELOCITY_RANDOM;
            const pj_val_t *tsmin = pj_get(in, "speedmin");
            if (!tsmin) {
                tsmin = pj_get(in, "minspeed");
            }
            const pj_val_t *tsmax = pj_get(in, "speedmax");
            if (!tsmax) {
                tsmax = pj_get(in, "maxspeed");
            }
            iz->min_val[0] = clamp_coord(pj_finite(tsmin, 10.0f), 10.0f);
            iz->max_val[0] = clamp_coord(pj_finite(tsmax, 50.0f), 50.0f);
            normalize_fpair(&iz->min_val[0], &iz->max_val[0]);
            iz->scale = fminf(fmaxf(pj_finite(pj_get(in, "scale"), 0.01f), 1.0e-5f), 100.0f);
            iz->offset = clamp_coord(pj_finite(pj_get(in, "offset"), 0.0f), 0.0f);
            pj_vec3(pj_get(in, "forward"), iz->forward, 1.0f, 1.0f, 0.0f);
        } else {
            sys->initializer_count--;
        }
    }
}

static void parse_json_operators(we_particle_sys_t *sys, const pj_val_t *root)
{
    const pj_val_t *ops = pj_get(root, "operator");
    if (!ops || ops->type != PJ_ARR) {
        return;
    }
    for (size_t i = 0; i < ops->arr.count && sys->operator_count < WE_PARTICLE_MAX_OPERATORS; i++) {
        const pj_val_t *on = ops->arr.items[i];
        if (!on || on->type != PJ_OBJ) {
            continue;
        }
        const char *oname = pj_str(pj_get(on, "name"), "");
        we_particle_operator_t *op = &sys->operators[sys->operator_count++];
        memset(op, 0, sizeof(*op));

        if (strcasecmp(oname, "movement") == 0) {
            op->type = WE_OP_MOVEMENT;
            pj_vec3(pj_get(on, "gravity"), op->gravity, 0.0f, 0.0f, 0.0f);
            op->drag = fminf(fmaxf(pj_finite(pj_get(on, "drag"), 0.0f), -10.0f), 100.0f);
        } else if (strcasecmp(oname, "angularmovement") == 0) {
            op->type = WE_OP_ANGULAR_MOVEMENT;
            op->drag = fminf(fmaxf(pj_finite(pj_get(on, "drag"), 0.0f), -10.0f), 100.0f);
            pj_vec3(pj_get(on, "force"), op->gravity, 0.0f, 0.0f, 0.0f);
        } else if (strcasecmp(oname, "alphafade") == 0) {
            op->type = WE_OP_ALPHA_FADE;
            op->fade_in_time = fminf(fmaxf(pj_finite(pj_get(on, "fadeintime"), 0.1f), 0.0f), 1.0f);
            op->fade_out_time =
                fminf(fmaxf(pj_finite(pj_get(on, "fadeouttime"), 0.8f), 0.0f), 1.0f);
        } else if (strcasecmp(oname, "sizechange") == 0) {
            op->type = WE_OP_SIZE_CHANGE;
            op->start_time = fminf(fmaxf(pj_finite(pj_get(on, "starttime"), 0.0f), 0.0f), 1.0f);
            op->end_time = fminf(fmaxf(pj_finite(pj_get(on, "endtime"), 1.0f), 0.0f), 1.0f);
            normalize_fpair(&op->start_time, &op->end_time);
            op->start_val[0] =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "startvalue"), 1.0f), 1.0f));
            op->end_val[0] =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "endvalue"), 0.0f), 0.0f));
        } else if (strcasecmp(oname, "colorchange") == 0) {
            op->type = WE_OP_COLOR_CHANGE;
            op->start_time = fminf(fmaxf(pj_finite(pj_get(on, "starttime"), 0.0f), 0.0f), 1.0f);
            op->end_time = fminf(fmaxf(pj_finite(pj_get(on, "endtime"), 1.0f), 0.0f), 1.0f);
            normalize_fpair(&op->start_time, &op->end_time);
            pj_vec3(pj_get(on, "startvalue"), op->start_val, 1.0f, 1.0f, 1.0f);
            pj_vec3(pj_get(on, "endvalue"), op->end_val, 1.0f, 1.0f, 1.0f);
            normalize_color_vec3(op->start_val);
            normalize_color_vec3(op->end_val);
        } else if (strcasecmp(oname, "oscillatealpha") == 0) {
            op->type = WE_OP_OSCILLATE_ALPHA;
            op->freq_min = clamp_coord(pj_finite(pj_get(on, "frequencymin"), 1.0f), 1.0f);
            op->freq_max = clamp_coord(pj_finite(pj_get(on, "frequencymax"), 3.0f), 3.0f);
            normalize_fpair(&op->freq_min, &op->freq_max);
            op->scale_min = fminf(fmaxf(pj_finite(pj_get(on, "scalemin"), 0.2f), 0.0f), 100.0f);
            op->scale_max = fminf(fmaxf(pj_finite(pj_get(on, "scalemax"), 1.0f), 0.0f), 100.0f);
            normalize_fpair(&op->scale_min, &op->scale_max);
        } else if (strcasecmp(oname, "oscillatesize") == 0) {
            op->type = WE_OP_OSCILLATE_SIZE;
            op->freq_min = clamp_coord(pj_finite(pj_get(on, "frequencymin"), 1.0f), 1.0f);
            op->freq_max = clamp_coord(pj_finite(pj_get(on, "frequencymax"), 3.0f), 3.0f);
            normalize_fpair(&op->freq_min, &op->freq_max);
            op->scale_min = fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "scalemin"), 0.8f), 0.8f));
            op->scale_max = fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "scalemax"), 1.2f), 1.2f));
            normalize_fpair(&op->scale_min, &op->scale_max);
        } else if (strcasecmp(oname, "oscillateposition") == 0) {
            op->type = WE_OP_OSCILLATE_POSITION;
            op->freq_min = clamp_coord(pj_finite(pj_get(on, "frequencymin"), 1.0f), 1.0f);
            op->freq_max = clamp_coord(pj_finite(pj_get(on, "frequencymax"), 3.0f), 3.0f);
            normalize_fpair(&op->freq_min, &op->freq_max);
            op->scale_min = clamp_coord(pj_finite(pj_get(on, "scalemin"), 5.0f), 5.0f);
            op->scale_max = clamp_coord(pj_finite(pj_get(on, "scalemax"), 15.0f), 15.0f);
            normalize_fpair(&op->scale_min, &op->scale_max);
            pj_vec3(pj_get(on, "mask"), op->mask, 1.0f, 1.0f, 0.0f);
        } else if (strcasecmp(oname, "vortex") == 0) {
            op->type = WE_OP_VORTEX;
            int32_t cp_idx = safe_f32_to_i32(pj_finite(pj_get(on, "controlpoint"), 0.0f), 0);
            op->control_point =
                (cp_idx >= 0 && cp_idx < (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) ? cp_idx : 0;
            pj_vec3(pj_get(on, "axis"), op->axis, 0.0f, 0.0f, 1.0f);
            float alen = sqrtf(op->axis[0] * op->axis[0] + op->axis[1] * op->axis[1] +
                               op->axis[2] * op->axis[2]);
            if (isfinite(alen) && alen > 1e-5f) {
                op->axis[0] /= alen;
                op->axis[1] /= alen;
                op->axis[2] /= alen;
            } else {
                op->axis[0] = 0.0f;
                op->axis[1] = 0.0f;
                op->axis[2] = 1.0f;
            }
            pj_vec3(pj_get(on, "offset"), op->offset, 0.0f, 0.0f, 0.0f);
            op->distance_inner =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "distanceinner"), 50.0f), 50.0f));
            op->distance_outer =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "distanceouter"), 300.0f), 300.0f));
            normalize_fpair(&op->distance_inner, &op->distance_outer);
            op->speed_inner = clamp_coord(pj_finite(pj_get(on, "speedinner"), 200.0f), 200.0f);
            op->speed_outer = clamp_coord(pj_finite(pj_get(on, "speedouter"), 50.0f), 50.0f);
            op->center_force = clamp_coord(
                pj_finite(pj_get(on, "centerforce"), pj_finite(pj_get(on, "scale"), 50.0f)), 50.0f);
        } else if (strcasecmp(oname, "controlpointattract") == 0) {
            op->type = WE_OP_CONTROL_POINT_ATTRACT;
            int32_t cp_idx = safe_f32_to_i32(pj_finite(pj_get(on, "controlpoint"), 0.0f), 0);
            op->control_point =
                (cp_idx >= 0 && cp_idx < (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) ? cp_idx : 0;
            pj_vec3(pj_get(on, "offset"), op->offset, 0.0f, 0.0f, 0.0f);
            op->distance_inner =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "distanceinner"), 0.0f), 0.0f));
            op->distance_outer =
                fmaxf(0.0f, clamp_coord(pj_finite(pj_get(on, "distanceouter"), 300.0f), 300.0f));
            normalize_fpair(&op->distance_inner, &op->distance_outer);
            op->speed_inner = 0.0f;
            op->speed_outer = 0.0f;
            op->center_force = clamp_coord(
                pj_finite(pj_get(on, "scale"), pj_finite(pj_get(on, "centerforce"), 50.0f)), 50.0f);
        } else if (strcasecmp(oname, "turbulence") == 0) {
            op->type = WE_OP_TURBULENCE;
            op->turb_scale = fminf(fmaxf(pj_finite(pj_get(on, "scale"), 0.01f), 1.0e-5f), 100.0f);
            const pj_val_t *tsmin = pj_get(on, "speedmin");
            if (!tsmin) {
                tsmin = pj_get(on, "minspeed");
            }
            const pj_val_t *tsmax = pj_get(on, "speedmax");
            if (!tsmax) {
                tsmax = pj_get(on, "maxspeed");
            }
            float sp_min = clamp_coord(pj_finite(tsmin, 50.0f), 50.0f);
            float sp_max = clamp_coord(pj_finite(tsmax, sp_min), sp_min);
            normalize_fpair(&sp_min, &sp_max);
            float def_sp = 0.5f * (sp_min + sp_max);
            op->turb_speed = clamp_coord(pj_finite(pj_get(on, "speed"), def_sp), def_sp);
            op->turb_timescale = clamp_coord(pj_finite(pj_get(on, "timescale"), 1.0f), 1.0f);
            pj_vec3(pj_get(on, "mask"), op->mask, 1.0f, 1.0f, 0.0f);
        } else {
            sys->operator_count--;
        }
    }
}

we_particle_sys_t *we_particle_create_from_json(const char *json_str, size_t len)
{
    if (!json_str || len == 0) {
        return NULL;
    }

    pj_val_t *root = we_json_parse_root(json_str, len);
    if (!root || root->type != PJ_OBJ) {
        pj_free(root);
        return NULL;
    }

    we_particle_sys_t *sys = calloc(1, sizeof(we_particle_sys_t));
    if (!sys) {
        pj_free(root);
        return NULL;
    }

    sys->rng_state = 0x853C49E6748FEA9BULL;

    int32_t raw_max =
        safe_f32_to_i32(pj_finite(pj_get(root, "maxcount"), (float)WE_PARTICLE_DEFAULT_CAP),
                        (int32_t)WE_PARTICLE_DEFAULT_CAP);
    if (raw_max <= 0) {
        raw_max = (int32_t)WE_PARTICLE_DEFAULT_CAP;
    }
    if ((uint32_t)raw_max > WE_PARTICLE_MAX_CAP) {
        raw_max = (int32_t)WE_PARTICLE_MAX_CAP;
    }
    sys->max_count = (uint32_t)raw_max;
    sys->padded_cap = (sys->max_count + 7u) & ~7u;

    /* Allocate a single 32-byte-aligned block for all 31 SoA float arrays */
    size_t total_floats = (size_t)WE_PARTICLE_SOA_ARRAYS * (size_t)sys->padded_cap;
    size_t alloc_bytes = (total_floats * sizeof(float) + 31u) & ~(size_t)31u;
    sys->soa_block = (float *)aligned_alloc(32, alloc_bytes);
    if (!sys->soa_block) {
        pj_free(root);
        free(sys);
        return NULL;
    }
    memset(sys->soa_block, 0, alloc_bytes);
    bind_soa_pointers(sys);

    float raw_start = pj_finite(pj_get(root, "starttime"), 0.0f);
    sys->start_time = (raw_start > 0.0f) ? fminf(raw_start, 30.0f) : 0.0f;
    snprintf(sys->material_path, sizeof(sys->material_path), "%s",
             pj_str(pj_get(root, "material"), ""));

    int32_t flags = safe_f32_to_i32(pj_finite(pj_get(root, "flags"), 0.0f), 0);
    sys->world_space = (flags & 1) != 0;
    sys->perspective = (flags & 4) != 0;

    const pj_val_t *bl = pj_get(root, "blending");
    if (bl) {
        if (bl->type == JVAL_STR && bl->str) {
            if (strcasecmp(bl->str, "additive") == 0 || strcasecmp(bl->str, "add") == 0) {
                sys->preset_blending = 1u;
            } else if (strcasecmp(bl->str, "translucent") == 0) {
                sys->preset_blending = 2u;
            } else {
                sys->preset_blending = 0u;
            }
        } else {
            sys->preset_blending = safe_f32_to_u32(pj_finite(bl, 0.0f), 8u);
        }
    }

    parse_json_renderer(sys, root);
    parse_json_control_points(sys, root);
    parse_json_emitters(sys, root);
    parse_json_initializers(sys, root);
    parse_json_operators(sys, root);

    pj_free(root);

    sys->quad_cap = sys->padded_cap;
    sys->gl_gpu_ops_dirty = true;
    sys->gl_gpu_state_dirty = true;

    if (sys->start_time > 0.0f) {
        we_particle_reset_and_prewarm(sys);
    }

    return sys;
}

void we_particle_set_pointer(we_particle_sys_t *sys, float world_x, float world_y, float world_z)
{
    if (!sys) {
        return;
    }
    float wx = clamp_coord(world_x, 0.0f);
    float wy = clamp_coord(world_y, 0.0f);
    float wz = clamp_coord(world_z, 0.0f);
    for (uint32_t i = 0; i < WE_PARTICLE_MAX_CONTROL_PTS; i++) {
        if (sys->control_points[i].lock_to_pointer) {
            sys->control_points[i].pos[0] = wx + sys->control_points[i].offset[0];
            sys->control_points[i].pos[1] = wy + sys->control_points[i].offset[1];
            sys->control_points[i].pos[2] = wz + sys->control_points[i].offset[2];
        }
    }
}

/* =========================================================================
 * Particle Spawn & Initializer Evaluation
 * ========================================================================= */

static void spawn_single_particle(we_particle_sys_t *sys, const we_particle_emitter_t *em)
{
    if (sys->alive_count >= sys->max_count) {
        return;
    }
    uint32_t idx = sys->alive_count++;
    uint64_t *rng = &sys->rng_state;

    float cp_pos[3] = {0.0f, 0.0f, 0.0f};
    if (em->control_point >= 0 && em->control_point < (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
        memcpy(cp_pos, sys->control_points[em->control_point].pos, sizeof(cp_pos));
    }

    float px = em->origin[0] + cp_pos[0];
    float py = em->origin[1] + cp_pos[1];
    float pz = em->origin[2] + cp_pos[2];
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;

    if (em->type == WE_EMITTER_BOXRANDOM) {
        for (int k = 0; k < 3; k++) {
            float dmin = isfinite(em->distance_min[k]) ? em->distance_min[k] : 0.0f;
            float dmax = isfinite(em->distance_max[k]) ? em->distance_max[k] : 0.0f;
            normalize_fpair(&dmin, &dmax);
            float off = 0.0f;
            if (fabsf(dmin) <= 1e-5f && dmax >= 0.0f) {
                off = prng_sym(rng) * dmax * em->directions[k];
            } else {
                off = prng_range(rng, dmin, dmax) * em->directions[k];
            }
            if (k == 0) {
                px += off;
            } else if (k == 1) {
                py += off;
            } else {
                pz += off;
            }
        }
    } else if (em->type == WE_EMITTER_SPHERERANDOM) {
        float r_min = fmaxf(0.0f, isfinite(em->distance_min[0]) ? em->distance_min[0] : 0.0f);
        float r_max = fmaxf(0.0f, isfinite(em->distance_max[0]) ? em->distance_max[0] : 0.0f);
        normalize_fpair(&r_min, &r_max);
        float u = prng_u01(rng);
        float rad = r_min + (r_max - r_min) * cbrtf(u);

        /* Marsaglia rejection sampling on unit disk -> unit sphere */
        float x1 = 0.0f, x2 = 0.0f, s = 1.0f;
        for (int tries = 0; tries < 12; tries++) {
            x1 = prng_sym(rng);
            x2 = prng_sym(rng);
            s = x1 * x1 + x2 * x2;
            if (s < 1.0f) {
                break;
            }
        }
        if (s >= 1.0f) {
            x1 = 0.0f;
            x2 = 0.0f;
            s = 0.0f;
        }
        float root = sqrtf(fmaxf(0.0f, 1.0f - s));
        float dx = 2.0f * x1 * root * em->directions[0];
        float dy = 2.0f * x2 * root * em->directions[1];
        float dz = (1.0f - 2.0f * s) * em->directions[2];
        float dlen = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dlen > 1e-5f) {
            dx /= dlen;
            dy /= dlen;
            dz /= dlen;
        }

        px += dx * rad;
        py += dy * rad;
        pz += dz * rad;

        if (fabsf(em->speed_min) > 1e-5f || fabsf(em->speed_max) > 1e-5f) {
            float smin = isfinite(em->speed_min) ? em->speed_min : 0.0f;
            float smax = isfinite(em->speed_max) ? em->speed_max : 0.0f;
            normalize_fpair(&smin, &smax);
            float rspd = prng_range(rng, smin, smax);
            vx = dx * rspd;
            vy = dy * rspd;
            vz = dz * rspd;
        }
    }

    sys->pos_x[idx] = px;
    sys->pos_y[idx] = py;
    sys->pos_z[idx] = pz;
    sys->init_pos_x[idx] = px;
    sys->init_pos_y[idx] = py;
    sys->init_pos_z[idx] = pz;

    sys->vel_x[idx] = vx;
    sys->vel_y[idx] = vy;
    sys->vel_z[idx] = vz;
    sys->acc_x[idx] = 0.0f;
    sys->acc_y[idx] = 0.0f;
    sys->acc_z[idx] = 0.0f;

    sys->r[idx] = 1.0f;
    sys->g[idx] = 1.0f;
    sys->b[idx] = 1.0f;
    sys->a[idx] = 1.0f;
    sys->init_r[idx] = 1.0f;
    sys->init_g[idx] = 1.0f;
    sys->init_b[idx] = 1.0f;
    sys->init_a[idx] = 1.0f;

    sys->size[idx] = 20.0f;
    sys->init_size[idx] = 20.0f;

    sys->rot_x[idx] = 0.0f;
    sys->rot_y[idx] = 0.0f;
    sys->rot_z[idx] = 0.0f;
    sys->ang_vel_x[idx] = 0.0f;
    sys->ang_vel_y[idx] = 0.0f;
    sys->ang_vel_z[idx] = 0.0f;

    sys->age[idx] = 0.0f;
    sys->max_life[idx] = 2.0f;
    sys->rng_phase[idx] = prng_u01(rng) * 6.283185307f;

    for (uint32_t i = 0; i < sys->initializer_count; i++) {
        const we_particle_initializer_t *iz = &sys->initializers[i];
        switch (iz->type) {
        case WE_INIT_LIFETIME_RANDOM: {
            float life = prng_range(rng, iz->min_val[0], iz->max_val[0]);
            sys->max_life[idx] = fmaxf(0.001f, life);
            break;
        }
        case WE_INIT_SIZE_RANDOM: {
            float u = powf(prng_u01(rng), iz->exponent);
            float sz = iz->min_val[0] + u * (iz->max_val[0] - iz->min_val[0]);
            sys->size[idx] = sz;
            sys->init_size[idx] = sz;
            break;
        }
        case WE_INIT_ALPHA_RANDOM: {
            float al = prng_range(rng, iz->min_val[0], iz->max_val[0]);
            sys->a[idx] = al;
            sys->init_a[idx] = al;
            break;
        }
        case WE_INIT_COLOR_RANDOM: {
            float u = prng_u01(rng);
            float cr = iz->min_val[0] + u * (iz->max_val[0] - iz->min_val[0]);
            float cg = iz->min_val[1] + u * (iz->max_val[1] - iz->min_val[1]);
            float cb = iz->min_val[2] + u * (iz->max_val[2] - iz->min_val[2]);
            sys->r[idx] = cr;
            sys->g[idx] = cg;
            sys->b[idx] = cb;
            sys->init_r[idx] = cr;
            sys->init_g[idx] = cg;
            sys->init_b[idx] = cb;
            break;
        }
        case WE_INIT_VELOCITY_RANDOM:
            sys->vel_x[idx] += prng_range(rng, iz->min_val[0], iz->max_val[0]);
            sys->vel_y[idx] += prng_range(rng, iz->min_val[1], iz->max_val[1]);
            sys->vel_z[idx] += prng_range(rng, iz->min_val[2], iz->max_val[2]);
            break;
        case WE_INIT_ROTATION_RANDOM:
            sys->rot_x[idx] = prng_range(rng, iz->min_val[0], iz->max_val[0]);
            sys->rot_y[idx] = prng_range(rng, iz->min_val[1], iz->max_val[1]);
            sys->rot_z[idx] = prng_range(rng, iz->min_val[2], iz->max_val[2]);
            break;
        case WE_INIT_ANGULAR_VELOCITY_RANDOM:
            sys->ang_vel_x[idx] = prng_range(rng, iz->min_val[0], iz->max_val[0]);
            sys->ang_vel_y[idx] = prng_range(rng, iz->min_val[1], iz->max_val[1]);
            sys->ang_vel_z[idx] = prng_range(rng, iz->min_val[2], iz->max_val[2]);
            break;
        case WE_INIT_TURBULENT_VELOCITY_RANDOM: {
            float spd = prng_range(rng, iz->min_val[0], iz->max_val[0]);
            float sc = iz->scale > 0.0f ? iz->scale : 0.01f;
            float curl[3];
            eval_curl_noise_3d(px * sc + iz->offset, py * sc + iz->offset, pz * sc + iz->offset,
                               curl);
            sys->vel_x[idx] += curl[0] * spd * iz->forward[0];
            sys->vel_y[idx] += curl[1] * spd * iz->forward[1];
            sys->vel_z[idx] += curl[2] * spd * iz->forward[2];
            break;
        }
        }
    }
}

/* =========================================================================
 * AVX2 & Scalar Pre-Operator Reset & Operator Kernels
 * ========================================================================= */

static inline float wrap_turb_time_phase(double sim_time, float timescale)
{
    const double two_pi = 6.28318530717958647692;
    double raw = sim_time * (double)timescale;
    if (!isfinite(raw)) {
        return 0.0f;
    }
    double wrapped = fmod(raw, two_pi);
    if (wrapped < 0.0) {
        wrapped += two_pi;
    }
    return (float)wrapped;
}

enum {
    WE_PARTICLE_CH_SIZE = 1u << 0,
    WE_PARTICLE_CH_ALPHA = 1u << 1,
    WE_PARTICLE_CH_COLOR = 1u << 2,
};

static inline float clamped_drag_exp(float drag, float dt)
{
    float d = isfinite(drag) ? drag : 0.0f;
    float arg = fminf(fmaxf(-d * dt, -50.0f), 10.0f);
    return expf(arg);
}

static void op_movement_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t n,
                               float dt)
{
    float damp = clamped_drag_exp(op->drag, dt);
    float gx_dt = op->gravity[0] * dt;
    float gy_dt = op->gravity[1] * dt;
    float gz_dt = op->gravity[2] * dt;
    for (uint32_t i = 0; i < n; i++) {
        float vx = (sys->vel_x[i] + gx_dt + sys->acc_x[i] * dt) * damp;
        float vy = (sys->vel_y[i] + gy_dt + sys->acc_y[i] * dt) * damp;
        float vz = (sys->vel_z[i] + gz_dt + sys->acc_z[i] * dt) * damp;
        sys->vel_x[i] = vx;
        sys->vel_y[i] = vy;
        sys->vel_z[i] = vz;
        sys->pos_x[i] += vx * dt;
        sys->pos_y[i] += vy * dt;
        sys->pos_z[i] += vz * dt;
    }
}

static void op_angular_movement_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                       uint32_t n, float dt)
{
    float damp = clamped_drag_exp(op->drag, dt);
    float fx = op->gravity[0], fy = op->gravity[1], fz = op->gravity[2];
    for (uint32_t i = 0; i < n; i++) {
        sys->ang_vel_x[i] = (sys->ang_vel_x[i] + fx * dt) * damp;
        sys->ang_vel_y[i] = (sys->ang_vel_y[i] + fy * dt) * damp;
        sys->ang_vel_z[i] = (sys->ang_vel_z[i] + fz * dt) * damp;
        sys->rot_x[i] += sys->ang_vel_x[i] * dt;
        sys->rot_y[i] += sys->ang_vel_y[i] * dt;
        sys->rot_z[i] += sys->ang_vel_z[i] * dt;
    }
}

static inline float eval_ramp_lut65(const float *lut, float t)
{
    if (!lut) {
        return 1.0f;
    }
    if (!isfinite(t)) {
        return lut[0];
    }
    float tc = fminf(fmaxf(t, 0.0f), 1.0f);
    float pos = tc * (float)(WE_PARTICLE_RAMP_SAMPLES - 1u);
    uint32_t idx = (uint32_t)pos;
    if (idx >= WE_PARTICLE_RAMP_SAMPLES - 1u) {
        return lut[WE_PARTICLE_RAMP_SAMPLES - 1u];
    }
    float frac = pos - (float)idx;
    return lut[idx] + frac * (lut[idx + 1u] - lut[idx]);
}

static void op_alpha_fade_scalar(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                                 const float *src_a, float *dst_a, uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            dst_a[i] = src_a[i] * eval_ramp_lut65(op->ramp_lut.scalar_lut, t);
        }
        return;
    }
    float fin = op->fade_in_time;
    float fout = op->fade_out_time;
    float inv_fin = (fin > 1e-5f) ? (1.0f / fin) : 0.0f;
    float inv_fout = (fout < 0.99999f) ? (1.0f / (1.0f - fout)) : 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float mlife = fmaxf(sys->max_life[i], 1e-5f);
        float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
        float fi = (fin > 1e-5f) ? fminf(fmaxf(t * inv_fin, 0.0f), 1.0f) : 1.0f;
        float fo = (fout < 0.99999f) ? fminf(fmaxf((1.0f - t) * inv_fout, 0.0f), 1.0f) : 1.0f;
        dst_a[i] = src_a[i] * (fi * fo);
    }
}

static void op_size_change_scalar(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                                  const float *src_sz, float *dst_sz, uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            dst_sz[i] = src_sz[i] * eval_ramp_lut65(op->ramp_lut.scalar_lut, t);
        }
        return;
    }
    float st = op->start_time;
    float et = op->end_time;
    float sv = op->start_val[0];
    float dv = op->end_val[0] - sv;
    float inv_span = (et > st + 1e-5f) ? (1.0f / (et - st)) : 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float mlife = fmaxf(sys->max_life[i], 1e-5f);
        float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
        float u = (inv_span > 0.0f) ? fminf(fmaxf((t - st) * inv_span, 0.0f), 1.0f) : 1.0f;
        dst_sz[i] = src_sz[i] * (sv + u * dv);
    }
}

static void op_color_change_scalar(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                                   const float *src_r, const float *src_g, const float *src_b,
                                   float *dst_r, float *dst_g, float *dst_b, uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            dst_r[i] = src_r[i] * eval_ramp_lut65(op->ramp_lut.color_lut_r, t);
            dst_g[i] = src_g[i] * eval_ramp_lut65(op->ramp_lut.color_lut_g, t);
            dst_b[i] = src_b[i] * eval_ramp_lut65(op->ramp_lut.color_lut_b, t);
        }
        return;
    }
    float st = op->start_time;
    float et = op->end_time;
    float inv_span = (et > st + 1e-5f) ? (1.0f / (et - st)) : 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float mlife = fmaxf(sys->max_life[i], 1e-5f);
        float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
        float u = (inv_span > 0.0f) ? fminf(fmaxf((t - st) * inv_span, 0.0f), 1.0f) : 1.0f;
        dst_r[i] = src_r[i] * (op->start_val[0] + u * (op->end_val[0] - op->start_val[0]));
        dst_g[i] = src_g[i] * (op->start_val[1] + u * (op->end_val[1] - op->start_val[1]));
        dst_b[i] = src_b[i] * (op->start_val[2] + u * (op->end_val[2] - op->start_val[2]));
    }
}

static void op_oscillate_scalar_channel(const float *src, float *dst, const float *age,
                                        const float *rng_phase, uint32_t n,
                                        const we_particle_operator_t *op)
{
    float freq = 0.5f * (op->freq_min + op->freq_max);
    float half_range = 0.5f * (op->scale_max - op->scale_min);
    float base = op->scale_min + half_range;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i] * (base + sinf(age[i] * freq + rng_phase[i]) * half_range);
    }
}

static void op_oscillate_pos_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                    uint32_t n, float dt)
{
    float freq = 0.5f * (op->freq_min + op->freq_max);
    float amp = 0.5f * (op->scale_min + op->scale_max);
    float delta_half = 0.5f * dt * freq;
    float scalar = 2.0f * sinf(delta_half) * amp;
    float mx = scalar * op->mask[0];
    float my = scalar * op->mask[1];
    float mz = scalar * op->mask[2];
    for (uint32_t i = 0; i < n; i++) {
        float c = cosf(sys->age[i] * freq + sys->rng_phase[i] + delta_half);
        sys->pos_x[i] += c * mx;
        sys->pos_y[i] += c * my;
        sys->pos_z[i] += c * mz;
    }
}

static void op_vortex_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t n,
                             float dt)
{
    int32_t cidx = op->control_point;
    if (cidx < 0 || cidx >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
        cidx = 0;
    }
    float cx = sys->control_points[cidx].pos[0] + op->offset[0];
    float cy = sys->control_points[cidx].pos[1] + op->offset[1];
    float cz = sys->control_points[cidx].pos[2] + op->offset[2];
    float ax = op->axis[0], ay = op->axis[1], az = op->axis[2];
    float d_in = op->distance_inner;
    float d_out = fmaxf(op->distance_outer, d_in + 1.0f);
    float inv_d = 1.0f / (d_out - d_in);

    for (uint32_t i = 0; i < n; i++) {
        float dx = sys->pos_x[i] - cx;
        float dy = sys->pos_y[i] - cy;
        float dz = sys->pos_z[i] - cz;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < 1e-4f) {
            continue;
        }
        float inv_dist = 1.0f / dist;
        /* Tangential swirl = cross(axis, d) / dist; centripetal pull = -d / dist */
        float tx = (ay * dz - az * dy) * inv_dist;
        float ty = (az * dx - ax * dz) * inv_dist;
        float tz = (ax * dy - ay * dx) * inv_dist;
        float rx = -dx * inv_dist;
        float ry = -dy * inv_dist;
        float rz = -dz * inv_dist;

        float u = fminf(fmaxf((dist - d_in) * inv_d, 0.0f), 1.0f);
        float t_spd = op->speed_inner + u * (op->speed_outer - op->speed_inner);
        float pull = op->center_force;

        sys->vel_x[i] += (tx * t_spd + rx * pull) * dt;
        sys->vel_y[i] += (ty * t_spd + ry * pull) * dt;
        sys->vel_z[i] += (tz * t_spd + rz * pull) * dt;
    }
}

static void op_turbulence_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                 uint32_t n, float dt)
{
    float sc = op->turb_scale > 0.0f ? op->turb_scale : 0.01f;
    float t_off = wrap_turb_time_phase(sys->sim_time, op->turb_timescale);
    float spd = op->turb_speed * dt;
    for (uint32_t i = 0; i < n; i++) {
        float curl[3];
        eval_curl_noise_3d(sys->pos_x[i] * sc + t_off, sys->pos_y[i] * sc + t_off,
                           sys->pos_z[i] * sc + t_off, curl);
        sys->vel_x[i] += curl[0] * spd * op->mask[0];
        sys->vel_y[i] += curl[1] * spd * op->mask[1];
        sys->vel_z[i] += curl[2] * spd * op->mask[2];
    }
}

static void op_attract_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t n,
                              float dt)
{
    int32_t cidx = op->control_point;
    if (cidx < 0 || cidx >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
        cidx = 0;
    }
    float cx = sys->control_points[cidx].pos[0] + op->offset[0];
    float cy = sys->control_points[cidx].pos[1] + op->offset[1];
    float cz = sys->control_points[cidx].pos[2] + op->offset[2];
    float d_in = op->distance_inner;
    float d_out = op->distance_outer;
    bool has_outer = (d_out > 0.0f);
    float inv_d = (has_outer && d_out > d_in + 1e-4f) ? (1.0f / (d_out - d_in)) : 1e6f;

    for (uint32_t i = 0; i < n; i++) {
        float dx = cx - sys->pos_x[i];
        float dy = cy - sys->pos_y[i];
        float dz = cz - sys->pos_z[i];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < 1e-4f) {
            continue;
        }
        float att = 1.0f;
        if (has_outer) {
            att = fminf(fmaxf((d_out - dist) * inv_d, 0.0f), 1.0f);
        }
        float s = (op->center_force * att * dt) / dist;
        sys->vel_x[i] += dx * s;
        sys->vel_y[i] += dy * s;
        sys->vel_z[i] += dz * s;
    }
}

static bool particle_reset_and_ops_scalar(we_particle_sys_t *sys, float dt)
{
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        return false;
    }

    uint32_t reset_done = 0u;
    for (uint32_t o = 0; o < sys->operator_count; o++) {
        const we_particle_operator_t *op = &sys->operators[o];
        switch (op->type) {
        case WE_OP_MOVEMENT:
            op_movement_scalar(sys, op, n, dt);
            break;
        case WE_OP_ANGULAR_MOVEMENT:
            op_angular_movement_scalar(sys, op, n, dt);
            break;
        case WE_OP_ALPHA_FADE:
            op_alpha_fade_scalar(
                sys, op, (reset_done & WE_PARTICLE_CH_ALPHA) ? sys->a : sys->init_a, sys->a, n);
            reset_done |= WE_PARTICLE_CH_ALPHA;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_ALPHA;
            break;
        case WE_OP_SIZE_CHANGE:
            op_size_change_scalar(sys, op,
                                  (reset_done & WE_PARTICLE_CH_SIZE) ? sys->size : sys->init_size,
                                  sys->size, n);
            reset_done |= WE_PARTICLE_CH_SIZE;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_SIZE;
            break;
        case WE_OP_COLOR_CHANGE:
            op_color_change_scalar(sys, op,
                                   (reset_done & WE_PARTICLE_CH_COLOR) ? sys->r : sys->init_r,
                                   (reset_done & WE_PARTICLE_CH_COLOR) ? sys->g : sys->init_g,
                                   (reset_done & WE_PARTICLE_CH_COLOR) ? sys->b : sys->init_b,
                                   sys->r, sys->g, sys->b, n);
            reset_done |= WE_PARTICLE_CH_COLOR;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_COLOR;
            break;
        case WE_OP_OSCILLATE_ALPHA:
            op_oscillate_scalar_channel((reset_done & WE_PARTICLE_CH_ALPHA) ? sys->a : sys->init_a,
                                        sys->a, sys->age, sys->rng_phase, n, op);
            reset_done |= WE_PARTICLE_CH_ALPHA;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_ALPHA;
            break;
        case WE_OP_OSCILLATE_SIZE:
            op_oscillate_scalar_channel((reset_done & WE_PARTICLE_CH_SIZE) ? sys->size
                                                                           : sys->init_size,
                                        sys->size, sys->age, sys->rng_phase, n, op);
            reset_done |= WE_PARTICLE_CH_SIZE;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_SIZE;
            break;
        case WE_OP_OSCILLATE_POSITION:
            op_oscillate_pos_scalar(sys, op, n, dt);
            break;
        case WE_OP_VORTEX:
            op_vortex_scalar(sys, op, n, dt);
            break;
        case WE_OP_TURBULENCE:
            op_turbulence_scalar(sys, op, n, dt);
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
            op_attract_scalar(sys, op, n, dt);
            break;
        }
    }

    uint32_t stale = sys->cpu_channel_dirty & ~reset_done;
    if (__builtin_expect(stale != 0u, 0)) {
        if (stale & WE_PARTICLE_CH_SIZE) {
            memcpy(sys->size, sys->init_size, n * sizeof(float));
        }
        if (stale & WE_PARTICLE_CH_ALPHA) {
            memcpy(sys->a, sys->init_a, n * sizeof(float));
        }
        if (stale & WE_PARTICLE_CH_COLOR) {
            memcpy(sys->r, sys->init_r, n * sizeof(float));
            memcpy(sys->g, sys->init_g, n * sizeof(float));
            memcpy(sys->b, sys->init_b, n * sizeof(float));
        }
        sys->cpu_channel_dirty &= reset_done;
    }

    bool any_dead = false;
    for (uint32_t i = 0; i < n; i++) {
        float a = sys->age[i] + dt;
        sys->age[i] = a;
        if (a >= sys->max_life[i]) {
            any_dead = true;
        }
    }
    return any_dead;
}

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
/* Accurate Cody-Waite mod pi/2 sin+cos with scalar libm fallback on |x| >= 8192 or non-finite. */
__attribute__((target("avx2,fma"))) static inline void vsin_cos_256_ps(__m256 x, __m256 *out_sin,
                                                                       __m256 *out_cos)
{
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    const __m256 v_limit = _mm256_set1_ps(8192.0f);
    __m256 valid = _mm256_cmp_ps(_mm256_and_ps(x, abs_mask), v_limit, _CMP_LT_OQ);
    int vmask = _mm256_movemask_ps(valid);

    const __m256 inv_pi2 = _mm256_set1_ps(0.63661977236758134f);
    const __m256 p1 = _mm256_set1_ps(1.5707963705062866f);
    const __m256 p2 = _mm256_set1_ps(-4.371138828673793e-08f);
    const __m256 p3 = _mm256_set1_ps(-1.7763568394002505e-15f);

    __m256 x_safe = _mm256_and_ps(x, valid);
    __m256 q_f = _mm256_round_ps(_mm256_mul_ps(x_safe, inv_pi2),
                                 _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 r = _mm256_fnmadd_ps(q_f, p1, x_safe);
    r = _mm256_fnmadd_ps(q_f, p2, r);
    r = _mm256_fnmadd_ps(q_f, p3, r);
    __m256 r2 = _mm256_mul_ps(r, r);

    const __m256 s1 = _mm256_set1_ps(-1.66666657e-01f);
    const __m256 s2 = _mm256_set1_ps(8.33329745e-03f);
    const __m256 s3 = _mm256_set1_ps(-1.98106907e-04f);
    __m256 s_poly = _mm256_fmadd_ps(s3, r2, s2);
    s_poly = _mm256_fmadd_ps(s_poly, r2, s1);
    __m256 s_r = _mm256_fmadd_ps(_mm256_mul_ps(r2, r), s_poly, r);

    const __m256 c1 = _mm256_set1_ps(-0.5f);
    const __m256 c2 = _mm256_set1_ps(4.16663885e-02f);
    const __m256 c3 = _mm256_set1_ps(-1.38797157e-03f);
    const __m256 one = _mm256_set1_ps(1.0f);
    __m256 c_poly = _mm256_fmadd_ps(c3, r2, c2);
    c_poly = _mm256_fmadd_ps(c_poly, r2, c1);
    __m256 c_r = _mm256_fmadd_ps(r2, c_poly, one);

    __m256i q_i = _mm256_cvtps_epi32(q_f);
    __m256 swap_mask =
        _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_and_si256(q_i, _mm256_set1_epi32(1)), 31));
    __m256 sin_sign =
        _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_and_si256(q_i, _mm256_set1_epi32(2)), 30));
    __m256 cos_sign = _mm256_castsi256_ps(_mm256_slli_epi32(
        _mm256_and_si256(_mm256_add_epi32(q_i, _mm256_set1_epi32(1)), _mm256_set1_epi32(2)), 30));

    __m256 sv = _mm256_xor_ps(_mm256_blendv_ps(s_r, c_r, swap_mask), sin_sign);
    __m256 cv = _mm256_xor_ps(_mm256_blendv_ps(c_r, s_r, swap_mask), cos_sign);

    if (__builtin_expect(vmask != 0xFF, 0)) {
        float xa[8] __attribute__((aligned(32)));
        float sa[8] __attribute__((aligned(32)));
        float ca[8] __attribute__((aligned(32)));
        _mm256_store_ps(xa, x);
        _mm256_store_ps(sa, sv);
        _mm256_store_ps(ca, cv);
        for (int k = 0; k < 8; k++) {
            if (((vmask >> k) & 1) == 0) {
                sa[k] = isfinite(xa[k]) ? sinf(xa[k]) : 0.0f;
                ca[k] = isfinite(xa[k]) ? cosf(xa[k]) : 1.0f;
            }
        }
        sv = _mm256_load_ps(sa);
        cv = _mm256_load_ps(ca);
    }

    if (out_sin) {
        *out_sin = sv;
    }
    if (out_cos) {
        *out_cos = cv;
    }
}

__attribute__((target("avx2,fma"))) static inline __m256 vsin256_ps(__m256 x)
{
    __m256 sv;
    vsin_cos_256_ps(x, &sv, NULL);
    return sv;
}

__attribute__((target("avx2,fma"))) static inline __m256 vcos256_ps(__m256 x)
{
    __m256 cv;
    vsin_cos_256_ps(x, NULL, &cv);
    return cv;
}

__attribute__((target("avx2,fma"))) static inline __m256 v_eval_lut65_avx2(const float *lut,
                                                                           __m256 t)
{
    const __m256 v_scale = _mm256_set1_ps((float)(WE_PARTICLE_RAMP_SAMPLES - 1u));
    const __m256 v_max_idx = _mm256_set1_ps((float)(WE_PARTICLE_RAMP_SAMPLES - 2u));
    const __m256i v_one_i = _mm256_set1_epi32(1);

    __m256 pos = _mm256_mul_ps(t, v_scale);
    __m256 idx_f = _mm256_min_ps(_mm256_floor_ps(pos), v_max_idx);
    __m256 frac = _mm256_sub_ps(pos, idx_f);
    __m256i idx0 = _mm256_cvtps_epi32(idx_f);
    __m256i idx1 = _mm256_add_epi32(idx0, v_one_i);

    __m256 v0 = _mm256_i32gather_ps(lut, idx0, 4);
    __m256 v1 = _mm256_i32gather_ps(lut, idx1, 4);
    return _mm256_fmadd_ps(frac, _mm256_sub_ps(v1, v0), v0);
}

__attribute__((target("avx2,fma"))) static void op_movement_avx2(we_particle_sys_t *sys,
                                                                 const we_particle_operator_t *op,
                                                                 uint32_t vec_n, __m256 v_dt)
{
    float dt = _mm256_cvtss_f32(v_dt);
    __m256 v_damp = _mm256_set1_ps(clamped_drag_exp(op->drag, dt));
    __m256 v_gx = _mm256_set1_ps(op->gravity[0]);
    __m256 v_gy = _mm256_set1_ps(op->gravity[1]);
    __m256 v_gz = _mm256_set1_ps(op->gravity[2]);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 vx = _mm256_load_ps(sys->vel_x + i);
        __m256 vy = _mm256_load_ps(sys->vel_y + i);
        __m256 vz = _mm256_load_ps(sys->vel_z + i);

        __m256 ax = _mm256_add_ps(v_gx, _mm256_load_ps(sys->acc_x + i));
        __m256 ay = _mm256_add_ps(v_gy, _mm256_load_ps(sys->acc_y + i));
        __m256 az = _mm256_add_ps(v_gz, _mm256_load_ps(sys->acc_z + i));

        vx = _mm256_mul_ps(_mm256_fmadd_ps(ax, v_dt, vx), v_damp);
        vy = _mm256_mul_ps(_mm256_fmadd_ps(ay, v_dt, vy), v_damp);
        vz = _mm256_mul_ps(_mm256_fmadd_ps(az, v_dt, vz), v_damp);

        _mm256_store_ps(sys->vel_x + i, vx);
        _mm256_store_ps(sys->vel_y + i, vy);
        _mm256_store_ps(sys->vel_z + i, vz);

        _mm256_store_ps(sys->pos_x + i, _mm256_fmadd_ps(vx, v_dt, _mm256_load_ps(sys->pos_x + i)));
        _mm256_store_ps(sys->pos_y + i, _mm256_fmadd_ps(vy, v_dt, _mm256_load_ps(sys->pos_y + i)));
        _mm256_store_ps(sys->pos_z + i, _mm256_fmadd_ps(vz, v_dt, _mm256_load_ps(sys->pos_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_angular_movement_avx2(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t vec_n,
                         __m256 v_dt)
{
    float dt = _mm256_cvtss_f32(v_dt);
    __m256 v_damp = _mm256_set1_ps(clamped_drag_exp(op->drag, dt));
    __m256 v_fx = _mm256_set1_ps(op->gravity[0]);
    __m256 v_fy = _mm256_set1_ps(op->gravity[1]);
    __m256 v_fz = _mm256_set1_ps(op->gravity[2]);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 avx =
            _mm256_mul_ps(_mm256_fmadd_ps(v_fx, v_dt, _mm256_load_ps(sys->ang_vel_x + i)), v_damp);
        __m256 avy =
            _mm256_mul_ps(_mm256_fmadd_ps(v_fy, v_dt, _mm256_load_ps(sys->ang_vel_y + i)), v_damp);
        __m256 avz =
            _mm256_mul_ps(_mm256_fmadd_ps(v_fz, v_dt, _mm256_load_ps(sys->ang_vel_z + i)), v_damp);

        _mm256_store_ps(sys->ang_vel_x + i, avx);
        _mm256_store_ps(sys->ang_vel_y + i, avy);
        _mm256_store_ps(sys->ang_vel_z + i, avz);

        _mm256_store_ps(sys->rot_x + i, _mm256_fmadd_ps(avx, v_dt, _mm256_load_ps(sys->rot_x + i)));
        _mm256_store_ps(sys->rot_y + i, _mm256_fmadd_ps(avy, v_dt, _mm256_load_ps(sys->rot_y + i)));
        _mm256_store_ps(sys->rot_z + i, _mm256_fmadd_ps(avz, v_dt, _mm256_load_ps(sys->rot_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_alpha_fade_avx2(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                   const float *src_a, float *dst_a, uint32_t vec_n, __m256 v_zero, __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 val = v_eval_lut65_avx2(op->ramp_lut.scalar_lut, t);
            _mm256_store_ps(dst_a + i, _mm256_mul_ps(_mm256_load_ps(src_a + i), val));
        }
        return;
    }
    float fin = op->fade_in_time;
    float fout = op->fade_out_time;
    __m256 v_inv_fin = _mm256_set1_ps((fin > 1e-5f) ? (1.0f / fin) : 1e6f);
    __m256 v_inv_fout = _mm256_set1_ps((fout < 0.99999f) ? (1.0f / (1.0f - fout)) : 1e6f);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 age = _mm256_load_ps(sys->age + i);
        __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
        __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
        __m256 fi = (fin > 1e-5f)
                        ? _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(t, v_inv_fin), v_zero), v_one)
                        : v_one;
        __m256 fo =
            (fout < 0.99999f)
                ? _mm256_min_ps(
                      _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_one, t), v_inv_fout), v_zero),
                      v_one)
                : v_one;
        __m256 a = _mm256_load_ps(src_a + i);
        _mm256_store_ps(dst_a + i, _mm256_mul_ps(a, _mm256_mul_ps(fi, fo)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_size_change_avx2(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                    const float *src_sz, float *dst_sz, uint32_t vec_n, __m256 v_zero, __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 val = v_eval_lut65_avx2(op->ramp_lut.scalar_lut, t);
            _mm256_store_ps(dst_sz + i, _mm256_mul_ps(_mm256_load_ps(src_sz + i), val));
        }
        return;
    }
    float st = op->start_time;
    float et = op->end_time;
    float sv = op->start_val[0];
    float dv = op->end_val[0] - sv;
    float inv_span = (et > st + 1e-5f) ? (1.0f / (et - st)) : 0.0f;
    __m256 v_st = _mm256_set1_ps(st);
    __m256 v_inv_span = _mm256_set1_ps(inv_span);
    __m256 v_sv = _mm256_set1_ps(sv);
    __m256 v_dv = _mm256_set1_ps(dv);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 age = _mm256_load_ps(sys->age + i);
        __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
        __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
        __m256 u =
            (inv_span > 0.0f)
                ? _mm256_min_ps(
                      _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(t, v_st), v_inv_span), v_zero),
                      v_one)
                : v_one;
        __m256 mult = _mm256_fmadd_ps(u, v_dv, v_sv);
        _mm256_store_ps(dst_sz + i, _mm256_mul_ps(_mm256_load_ps(src_sz + i), mult));
    }
}

__attribute__((target("avx2,fma"))) static void
op_color_change_avx2(const we_particle_sys_t *sys, const we_particle_operator_t *op,
                     const float *src_r, const float *src_g, const float *src_b, float *dst_r,
                     float *dst_g, float *dst_b, uint32_t vec_n, __m256 v_zero, __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 vr = v_eval_lut65_avx2(op->ramp_lut.color_lut_r, t);
            __m256 vg = v_eval_lut65_avx2(op->ramp_lut.color_lut_g, t);
            __m256 vb = v_eval_lut65_avx2(op->ramp_lut.color_lut_b, t);
            _mm256_store_ps(dst_r + i, _mm256_mul_ps(_mm256_load_ps(src_r + i), vr));
            _mm256_store_ps(dst_g + i, _mm256_mul_ps(_mm256_load_ps(src_g + i), vg));
            _mm256_store_ps(dst_b + i, _mm256_mul_ps(_mm256_load_ps(src_b + i), vb));
        }
        return;
    }
    float st = op->start_time;
    float et = op->end_time;
    float inv_span = (et > st + 1e-5f) ? (1.0f / (et - st)) : 0.0f;
    __m256 v_st = _mm256_set1_ps(st);
    __m256 v_inv_span = _mm256_set1_ps(inv_span);
    __m256 sr = _mm256_set1_ps(op->start_val[0]);
    __m256 sg = _mm256_set1_ps(op->start_val[1]);
    __m256 sb = _mm256_set1_ps(op->start_val[2]);
    __m256 dr = _mm256_set1_ps(op->end_val[0] - op->start_val[0]);
    __m256 dg = _mm256_set1_ps(op->end_val[1] - op->start_val[1]);
    __m256 db = _mm256_set1_ps(op->end_val[2] - op->start_val[2]);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 age = _mm256_load_ps(sys->age + i);
        __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
        __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
        __m256 u =
            (inv_span > 0.0f)
                ? _mm256_min_ps(
                      _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(t, v_st), v_inv_span), v_zero),
                      v_one)
                : v_one;
        _mm256_store_ps(dst_r + i,
                        _mm256_mul_ps(_mm256_load_ps(src_r + i), _mm256_fmadd_ps(u, dr, sr)));
        _mm256_store_ps(dst_g + i,
                        _mm256_mul_ps(_mm256_load_ps(src_g + i), _mm256_fmadd_ps(u, dg, sg)));
        _mm256_store_ps(dst_b + i,
                        _mm256_mul_ps(_mm256_load_ps(src_b + i), _mm256_fmadd_ps(u, db, sb)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_oscillate_avx2_channel(const float *src, float *dst, const float *age, const float *rng_phase,
                          uint32_t vec_n, const we_particle_operator_t *op)
{
    __m256 v_freq = _mm256_set1_ps(0.5f * (op->freq_min + op->freq_max));
    __m256 v_half = _mm256_set1_ps(0.5f);
    __m256 v_smin = _mm256_set1_ps(op->scale_min);
    __m256 v_sdiff = _mm256_set1_ps(op->scale_max - op->scale_min);
    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 phase =
            _mm256_fmadd_ps(_mm256_load_ps(age + i), v_freq, _mm256_load_ps(rng_phase + i));
        __m256 w = _mm256_fmadd_ps(v_half, vsin256_ps(phase), v_half);
        __m256 scale = _mm256_fmadd_ps(w, v_sdiff, v_smin);
        _mm256_store_ps(dst + i, _mm256_mul_ps(_mm256_load_ps(src + i), scale));
    }
}

__attribute__((target("avx2,fma"))) static void
op_oscillate_pos_avx2(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t vec_n,
                      float dt)
{
    float freq = 0.5f * (op->freq_min + op->freq_max);
    float amp = 0.5f * (op->scale_min + op->scale_max);
    __m256 v_freq = _mm256_set1_ps(freq);
    __m256 v_dfreq = _mm256_set1_ps(dt * freq);
    __m256 v_amp_mx = _mm256_set1_ps(amp * op->mask[0]);
    __m256 v_amp_my = _mm256_set1_ps(amp * op->mask[1]);
    __m256 v_amp_mz = _mm256_set1_ps(amp * op->mask[2]);
    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 p0 = _mm256_fmadd_ps(_mm256_load_ps(sys->age + i), v_freq,
                                    _mm256_load_ps(sys->rng_phase + i));
        __m256 p1 = _mm256_add_ps(p0, v_dfreq);
        __m256 ds = _mm256_sub_ps(vsin256_ps(p1), vsin256_ps(p0));
        _mm256_store_ps(sys->pos_x + i,
                        _mm256_fmadd_ps(ds, v_amp_mx, _mm256_load_ps(sys->pos_x + i)));
        _mm256_store_ps(sys->pos_y + i,
                        _mm256_fmadd_ps(ds, v_amp_my, _mm256_load_ps(sys->pos_y + i)));
        _mm256_store_ps(sys->pos_z + i,
                        _mm256_fmadd_ps(ds, v_amp_mz, _mm256_load_ps(sys->pos_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static void op_vortex_avx2(we_particle_sys_t *sys,
                                                               const we_particle_operator_t *op,
                                                               uint32_t vec_n, __m256 v_dt,
                                                               __m256 v_zero, __m256 v_one)
{
    int32_t cidx = op->control_point;
    if (cidx < 0 || cidx >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
        cidx = 0;
    }
    __m256 v_cx = _mm256_set1_ps(sys->control_points[cidx].pos[0] + op->offset[0]);
    __m256 v_cy = _mm256_set1_ps(sys->control_points[cidx].pos[1] + op->offset[1]);
    __m256 v_cz = _mm256_set1_ps(sys->control_points[cidx].pos[2] + op->offset[2]);
    __m256 v_ax = _mm256_set1_ps(op->axis[0]);
    __m256 v_ay = _mm256_set1_ps(op->axis[1]);
    __m256 v_az = _mm256_set1_ps(op->axis[2]);
    float d_in = op->distance_inner;
    float d_out = fmaxf(op->distance_outer, d_in + 1.0f);
    __m256 v_din = _mm256_set1_ps(d_in);
    __m256 v_inv_d = _mm256_set1_ps(1.0f / (d_out - d_in));
    __m256 v_sin = _mm256_set1_ps(op->speed_inner);
    __m256 v_sdiff = _mm256_set1_ps(op->speed_outer - op->speed_inner);
    __m256 v_pull = _mm256_set1_ps(-op->center_force);
    __m256 v_eps = _mm256_set1_ps(1e-4f);
    __m256 v_eps2 = _mm256_set1_ps(1e-8f);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 dx = _mm256_sub_ps(_mm256_load_ps(sys->pos_x + i), v_cx);
        __m256 dy = _mm256_sub_ps(_mm256_load_ps(sys->pos_y + i), v_cy);
        __m256 dz = _mm256_sub_ps(_mm256_load_ps(sys->pos_z + i), v_cz);

        __m256 d2 = _mm256_fmadd_ps(dx, dx, _mm256_fmadd_ps(dy, dy, _mm256_mul_ps(dz, dz)));
        __m256 active_mask = _mm256_cmp_ps(d2, v_eps2, _CMP_GE_OQ);
        __m256 dist = _mm256_max_ps(_mm256_sqrt_ps(d2), v_eps);
        __m256 inv_dist = _mm256_div_ps(v_one, dist);

        __m256 tx = _mm256_mul_ps(_mm256_fmsub_ps(v_ay, dz, _mm256_mul_ps(v_az, dy)), inv_dist);
        __m256 ty = _mm256_mul_ps(_mm256_fmsub_ps(v_az, dx, _mm256_mul_ps(v_ax, dz)), inv_dist);
        __m256 tz = _mm256_mul_ps(_mm256_fmsub_ps(v_ax, dy, _mm256_mul_ps(v_ay, dx)), inv_dist);

        __m256 u = _mm256_min_ps(
            _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(dist, v_din), v_inv_d), v_zero), v_one);
        __m256 t_spd = _mm256_fmadd_ps(u, v_sdiff, v_sin);
        __m256 rad_scale = _mm256_mul_ps(v_pull, inv_dist);

        __m256 fx =
            _mm256_and_ps(active_mask, _mm256_fmadd_ps(tx, t_spd, _mm256_mul_ps(dx, rad_scale)));
        __m256 fy =
            _mm256_and_ps(active_mask, _mm256_fmadd_ps(ty, t_spd, _mm256_mul_ps(dy, rad_scale)));
        __m256 fz =
            _mm256_and_ps(active_mask, _mm256_fmadd_ps(tz, t_spd, _mm256_mul_ps(dz, rad_scale)));

        _mm256_store_ps(sys->vel_x + i, _mm256_fmadd_ps(fx, v_dt, _mm256_load_ps(sys->vel_x + i)));
        _mm256_store_ps(sys->vel_y + i, _mm256_fmadd_ps(fy, v_dt, _mm256_load_ps(sys->vel_y + i)));
        _mm256_store_ps(sys->vel_z + i, _mm256_fmadd_ps(fz, v_dt, _mm256_load_ps(sys->vel_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static void op_turbulence_avx2(we_particle_sys_t *sys,
                                                                   const we_particle_operator_t *op,
                                                                   uint32_t vec_n, float dt,
                                                                   __m256 v_zero)
{
    float sc = op->turb_scale > 0.0f ? op->turb_scale : 0.01f;
    float t_off = wrap_turb_time_phase(sys->sim_time, op->turb_timescale);
    float spd = op->turb_speed * dt;
    __m256 v_sc = _mm256_set1_ps(sc);
    __m256 v_toff = _mm256_set1_ps(t_off);
    __m256 v_spdx = _mm256_set1_ps(spd * op->mask[0]);
    __m256 v_spdy = _mm256_set1_ps(spd * op->mask[1]);
    __m256 v_spdz = _mm256_set1_ps(spd * op->mask[2]);
    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 x = _mm256_fmadd_ps(_mm256_load_ps(sys->pos_x + i), v_sc, v_toff);
        __m256 y = _mm256_fmadd_ps(_mm256_load_ps(sys->pos_y + i), v_sc, v_toff);
        __m256 z = _mm256_fmadd_ps(_mm256_load_ps(sys->pos_z + i), v_sc, v_toff);

        __m256 sx, cx, sy, cy, sz, cz;
        vsin_cos_256_ps(x, &sx, &cx);
        vsin_cos_256_ps(y, &sy, &cy);
        vsin_cos_256_ps(z, &sz, &cz);

        __m256 curl_x = _mm256_sub_ps(_mm256_sub_ps(v_zero, sy), cz);
        __m256 curl_y = _mm256_sub_ps(_mm256_sub_ps(v_zero, sz), cx);
        __m256 curl_z = _mm256_sub_ps(_mm256_sub_ps(v_zero, sx), cy);

        _mm256_store_ps(sys->vel_x + i,
                        _mm256_fmadd_ps(curl_x, v_spdx, _mm256_load_ps(sys->vel_x + i)));
        _mm256_store_ps(sys->vel_y + i,
                        _mm256_fmadd_ps(curl_y, v_spdy, _mm256_load_ps(sys->vel_y + i)));
        _mm256_store_ps(sys->vel_z + i,
                        _mm256_fmadd_ps(curl_z, v_spdz, _mm256_load_ps(sys->vel_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static void op_attract_avx2(we_particle_sys_t *sys,
                                                                const we_particle_operator_t *op,
                                                                uint32_t vec_n, float dt,
                                                                __m256 v_zero, __m256 v_one)
{
    int32_t cidx = op->control_point;
    if (cidx < 0 || cidx >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
        cidx = 0;
    }
    __m256 v_cx = _mm256_set1_ps(sys->control_points[cidx].pos[0] + op->offset[0]);
    __m256 v_cy = _mm256_set1_ps(sys->control_points[cidx].pos[1] + op->offset[1]);
    __m256 v_cz = _mm256_set1_ps(sys->control_points[cidx].pos[2] + op->offset[2]);
    float d_in = op->distance_inner;
    float d_out = op->distance_outer;
    bool has_outer = (d_out > 0.0f);
    float inv_d = (has_outer && d_out > d_in + 1e-4f) ? (1.0f / (d_out - d_in)) : 1e6f;
    __m256 v_dout = _mm256_set1_ps(d_out);
    __m256 v_inv_d = _mm256_set1_ps(inv_d);
    __m256 v_fdt = _mm256_set1_ps(op->center_force * dt);
    __m256 v_eps = _mm256_set1_ps(1e-4f);
    __m256 v_eps2 = _mm256_set1_ps(1e-8f);

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 dx = _mm256_sub_ps(v_cx, _mm256_load_ps(sys->pos_x + i));
        __m256 dy = _mm256_sub_ps(v_cy, _mm256_load_ps(sys->pos_y + i));
        __m256 dz = _mm256_sub_ps(v_cz, _mm256_load_ps(sys->pos_z + i));
        __m256 d2 = _mm256_fmadd_ps(dx, dx, _mm256_fmadd_ps(dy, dy, _mm256_mul_ps(dz, dz)));
        __m256 active_mask = _mm256_cmp_ps(d2, v_eps2, _CMP_GE_OQ);
        __m256 dist = _mm256_max_ps(_mm256_sqrt_ps(d2), v_eps);
        __m256 att = v_one;
        if (has_outer) {
            att = _mm256_min_ps(
                _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_dout, dist), v_inv_d), v_zero), v_one);
        }
        __m256 s = _mm256_and_ps(active_mask, _mm256_div_ps(_mm256_mul_ps(v_fdt, att), dist));
        _mm256_store_ps(sys->vel_x + i, _mm256_fmadd_ps(dx, s, _mm256_load_ps(sys->vel_x + i)));
        _mm256_store_ps(sys->vel_y + i, _mm256_fmadd_ps(dy, s, _mm256_load_ps(sys->vel_y + i)));
        _mm256_store_ps(sys->vel_z + i, _mm256_fmadd_ps(dz, s, _mm256_load_ps(sys->vel_z + i)));
    }
}

__attribute__((target("avx2,fma"))) static bool particle_reset_and_ops_avx2(we_particle_sys_t *sys,
                                                                            float dt)
{
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        return false;
    }
    uint32_t vec_n = (n + 7u) & ~7u;
    if (vec_n > sys->padded_cap) {
        vec_n = sys->padded_cap;
    }

    for (uint32_t tail = n; tail < vec_n; tail++) {
        sys->age[tail] = 0.0f;
        sys->max_life[tail] = 10.0f;
    }

    __m256 v_dt = _mm256_set1_ps(dt);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);
    uint32_t reset_done = 0u;

    for (uint32_t o = 0; o < sys->operator_count; o++) {
        const we_particle_operator_t *op = &sys->operators[o];
        switch (op->type) {
        case WE_OP_MOVEMENT:
            op_movement_avx2(sys, op, vec_n, v_dt);
            break;
        case WE_OP_ANGULAR_MOVEMENT:
            op_angular_movement_avx2(sys, op, vec_n, v_dt);
            break;
        case WE_OP_ALPHA_FADE:
            op_alpha_fade_avx2(sys, op, (reset_done & WE_PARTICLE_CH_ALPHA) ? sys->a : sys->init_a,
                               sys->a, vec_n, v_zero, v_one);
            reset_done |= WE_PARTICLE_CH_ALPHA;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_ALPHA;
            break;
        case WE_OP_SIZE_CHANGE:
            op_size_change_avx2(sys, op,
                                (reset_done & WE_PARTICLE_CH_SIZE) ? sys->size : sys->init_size,
                                sys->size, vec_n, v_zero, v_one);
            reset_done |= WE_PARTICLE_CH_SIZE;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_SIZE;
            break;
        case WE_OP_COLOR_CHANGE:
            op_color_change_avx2(sys, op,
                                 (reset_done & WE_PARTICLE_CH_COLOR) ? sys->r : sys->init_r,
                                 (reset_done & WE_PARTICLE_CH_COLOR) ? sys->g : sys->init_g,
                                 (reset_done & WE_PARTICLE_CH_COLOR) ? sys->b : sys->init_b, sys->r,
                                 sys->g, sys->b, vec_n, v_zero, v_one);
            reset_done |= WE_PARTICLE_CH_COLOR;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_COLOR;
            break;
        case WE_OP_OSCILLATE_ALPHA:
            op_oscillate_avx2_channel((reset_done & WE_PARTICLE_CH_ALPHA) ? sys->a : sys->init_a,
                                      sys->a, sys->age, sys->rng_phase, vec_n, op);
            reset_done |= WE_PARTICLE_CH_ALPHA;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_ALPHA;
            break;
        case WE_OP_OSCILLATE_SIZE:
            op_oscillate_avx2_channel((reset_done & WE_PARTICLE_CH_SIZE) ? sys->size
                                                                         : sys->init_size,
                                      sys->size, sys->age, sys->rng_phase, vec_n, op);
            reset_done |= WE_PARTICLE_CH_SIZE;
            sys->cpu_channel_dirty |= WE_PARTICLE_CH_SIZE;
            break;
        case WE_OP_OSCILLATE_POSITION:
            op_oscillate_pos_avx2(sys, op, vec_n, dt);
            break;
        case WE_OP_VORTEX:
            op_vortex_avx2(sys, op, vec_n, v_dt, v_zero, v_one);
            break;
        case WE_OP_TURBULENCE:
            op_turbulence_avx2(sys, op, vec_n, dt, v_zero);
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
            op_attract_avx2(sys, op, vec_n, dt, v_zero, v_one);
            break;
        }
    }

    uint32_t stale = sys->cpu_channel_dirty & ~reset_done;
    if (__builtin_expect(stale != 0u, 0)) {
        if (stale & WE_PARTICLE_CH_SIZE) {
            memcpy(sys->size, sys->init_size, n * sizeof(float));
        }
        if (stale & WE_PARTICLE_CH_ALPHA) {
            memcpy(sys->a, sys->init_a, n * sizeof(float));
        }
        if (stale & WE_PARTICLE_CH_COLOR) {
            memcpy(sys->r, sys->init_r, n * sizeof(float));
            memcpy(sys->g, sys->init_g, n * sizeof(float));
            memcpy(sys->b, sys->init_b, n * sizeof(float));
        }
        sys->cpu_channel_dirty &= reset_done;
    }

    __m256 v_dead_mask = v_zero;
    for (uint32_t i = 0; i < vec_n; i += 8u) {
        __m256 age = _mm256_add_ps(_mm256_load_ps(sys->age + i), v_dt);
        _mm256_store_ps(sys->age + i, age);
        v_dead_mask = _mm256_or_ps(
            v_dead_mask, _mm256_cmp_ps(age, _mm256_load_ps(sys->max_life + i), _CMP_GE_OQ));
    }
    return _mm256_movemask_ps(v_dead_mask) != 0;
}
#endif

static void copy_particle_slot(we_particle_sys_t *sys, uint32_t dst, uint32_t src)
{
    if (dst == src) {
        return;
    }
    const uint32_t stride = sys->padded_cap;
    float *base = sys->soa_block;
    for (uint32_t a = 0; a < WE_PARTICLE_SOA_ARRAYS; a++) {
        base[dst] = base[src];
        base += stride;
    }
}

void we_particle_update(we_particle_sys_t *sys, float dt_sec)
{
    if (!sys || sys->paused || !isfinite(dt_sec) || dt_sec <= 0.0f) {
        return;
    }
    if (sys->gl_gpu_sim_active) {
        (void)we_particle_gpu_sync_to_cpu(sys);
        sys->gl_gpu_sim_active = false;
    }
    sys->gl_gpu_state_dirty = true;

    float dt = fminf(dt_sec, 0.25f);
    sys->sim_time += (double)dt;

    for (uint32_t e = 0; e < sys->emitter_count; e++) {
        we_particle_emitter_t *em = &sys->emitters[e];
        if (!em->burst_done && em->instantaneous > 0u) {
            em->burst_done = true;
            for (uint32_t b = 0; b < em->instantaneous && sys->alive_count < sys->max_count; b++) {
                spawn_single_particle(sys, em);
            }
        }
        if (em->rate > 0.0f && isfinite(em->rate)) {
            em->emit_accum += em->rate * dt;
            if (!isfinite(em->emit_accum) || em->emit_accum < 0.0f) {
                em->emit_accum = 0.0f;
            }
            if (em->emit_accum > (float)sys->max_count) {
                em->emit_accum = (float)sys->max_count;
            }
            uint32_t to_spawn = (uint32_t)em->emit_accum;
            em->emit_accum -= (float)to_spawn;
            for (uint32_t s = 0; s < to_spawn && sys->alive_count < sys->max_count; s++) {
                spawn_single_particle(sys, em);
            }
        }
    }

    if (sys->alive_count == 0u) {
        return;
    }

    bool any_dead = false;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        any_dead = particle_reset_and_ops_avx2(sys, dt);
    } else
#endif
    {
        any_dead = particle_reset_and_ops_scalar(sys, dt);
    }

    if (!any_dead) {
        return;
    }

    if (sys->renderer_type == WE_RENDERER_ROPETRAIL) {
        /* Order-preserving compaction for connected ribbon trails */
        uint32_t write_idx = 0;
        for (uint32_t i = 0; i < sys->alive_count; i++) {
            if (sys->age[i] < sys->max_life[i]) {
                if (write_idx != i) {
                    copy_particle_slot(sys, write_idx, i);
                }
                write_idx++;
            }
        }
        sys->alive_count = write_idx;
    } else {
        /* O(1) swap-with-last compaction for sprite / spritetrail */
        uint32_t i = 0;
        while (i < sys->alive_count) {
            if (sys->age[i] >= sys->max_life[i]) {
                uint32_t last = sys->alive_count - 1u;
                if (i != last) {
                    copy_particle_slot(sys, i, last);
                }
                sys->alive_count--;
            } else {
                i++;
            }
        }
    }
}

/* =========================================================================
 * Batched Quad & Ribbon Vertex Generation + OpenGL ES 3.1 Rendering
 * ========================================================================= */

static void compute_rope_joint_normal(const we_particle_sys_t *sys, uint32_t idx, uint32_t n,
                                      float *out_nx, float *out_ny)
{
    float nx_sum = 0.0f;
    float ny_sum = 0.0f;
    if (idx > 0u) {
        float dx = sys->pos_x[idx] - sys->pos_x[idx - 1u];
        float dy = sys->pos_y[idx] - sys->pos_y[idx - 1u];
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-5f) {
            nx_sum += -dy / len;
            ny_sum += dx / len;
        }
    }
    if (idx + 1u < n) {
        float dx = sys->pos_x[idx + 1u] - sys->pos_x[idx];
        float dy = sys->pos_y[idx + 1u] - sys->pos_y[idx];
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-5f) {
            nx_sum += -dy / len;
            ny_sum += dx / len;
        }
    }
    float nlen = sqrtf(nx_sum * nx_sum + ny_sum * ny_sum);
    if (nlen > 1e-5f) {
        *out_nx = nx_sum / nlen;
        *out_ny = ny_sum / nlen;
    } else {
        *out_nx = 0.0f;
        *out_ny = 1.0f;
    }
}

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx2,fma"))) static uint32_t
particle_build_sprites_avx2(we_particle_sys_t *sys, uint32_t count)
{
    const __m256 v_half = _mm256_set1_ps(0.5f);
    float rx_x_arr[8] __attribute__((aligned(32)));
    float rx_y_arr[8] __attribute__((aligned(32)));
    uint32_t i = 0;
    for (; i + 8u <= count; i += 8u) {
        __m256 v_hs = _mm256_mul_ps(_mm256_load_ps(sys->size + i), v_half);
        __m256 v_rz = _mm256_load_ps(sys->rot_z + i);
        __m256 v_cs = vcos256_ps(v_rz);
        __m256 v_sn = vsin256_ps(v_rz);
        _mm256_store_ps(rx_x_arr, _mm256_mul_ps(v_cs, v_hs));
        _mm256_store_ps(rx_y_arr, _mm256_mul_ps(v_sn, v_hs));

        for (uint32_t k = 0; k < 8u; ++k) {
            uint32_t idx = i + k;
            float px = sys->pos_x[idx];
            float py = sys->pos_y[idx];
            float pz = sys->pos_z[idx];
            float rx_x = rx_x_arr[k];
            float rx_y = rx_y_arr[k];
            float ry_x = -rx_y;
            float ry_y = rx_x;

            we_particle_vertex_t *v = &sys->vert_buf[sys->quad_count * 4u];
            float cr = sys->r[idx], cg = sys->g[idx], cb = sys->b[idx], ca = sys->a[idx];

            v[0].pos[0] = px - rx_x - ry_x;
            v[0].pos[1] = py - rx_y - ry_y;
            v[0].pos[2] = pz;
            v[0].uv[0] = 0.0f;
            v[0].uv[1] = 0.0f;
            v[0].color[0] = cr;
            v[0].color[1] = cg;
            v[0].color[2] = cb;
            v[0].color[3] = ca;

            v[1].pos[0] = px + rx_x - ry_x;
            v[1].pos[1] = py + rx_y - ry_y;
            v[1].pos[2] = pz;
            v[1].uv[0] = 1.0f;
            v[1].uv[1] = 0.0f;
            v[1].color[0] = cr;
            v[1].color[1] = cg;
            v[1].color[2] = cb;
            v[1].color[3] = ca;

            v[2].pos[0] = px + rx_x + ry_x;
            v[2].pos[1] = py + rx_y + ry_y;
            v[2].pos[2] = pz;
            v[2].uv[0] = 1.0f;
            v[2].uv[1] = 1.0f;
            v[2].color[0] = cr;
            v[2].color[1] = cg;
            v[2].color[2] = cb;
            v[2].color[3] = ca;

            v[3].pos[0] = px - rx_x + ry_x;
            v[3].pos[1] = py - rx_y + ry_y;
            v[3].pos[2] = pz;
            v[3].uv[0] = 0.0f;
            v[3].uv[1] = 1.0f;
            v[3].color[0] = cr;
            v[3].color[1] = cg;
            v[3].color[2] = cb;
            v[3].color[3] = ca;

            sys->quad_count++;
        }
    }
    return i;
}
#endif

uint32_t we_particle_build_vertices(we_particle_sys_t *sys, we_particle_vertex_t **out_verts)
{
    if (!sys) {
        if (out_verts) {
            *out_verts = NULL;
        }
        return 0;
    }
    uint32_t need_quads = sys->padded_cap > 0u ? sys->padded_cap : sys->quad_cap;
    if (need_quads == 0u) {
        if (out_verts) {
            *out_verts = NULL;
        }
        return 0;
    }
    if (!sys->vert_buf || sys->quad_cap < need_quads) {
        we_particle_vertex_t *nb = calloc((size_t)need_quads * 4u, sizeof(we_particle_vertex_t));
        if (!nb) {
            if (out_verts) {
                *out_verts = NULL;
            }
            return 0;
        }
        free(sys->vert_buf);
        sys->vert_buf = nb;
        sys->quad_cap = need_quads;
    }

    sys->quad_count = 0;
    sys->vert_count = 0;
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        if (out_verts) {
            *out_verts = sys->vert_buf;
        }
        return 0;
    }

    if (sys->renderer_type == WE_RENDERER_ROPETRAIL) {
        if (n < 2u) {
            if (out_verts) {
                *out_verts = sys->vert_buf;
            }
            return 0;
        }
        uint32_t segs = n - 1u;
        if (segs > sys->quad_cap) {
            segs = sys->quad_cap;
        }
        for (uint32_t i = 0; i < segs; i++) {
            float x0 = sys->pos_x[i], y0 = sys->pos_y[i], z0 = sys->pos_z[i];
            float x1 = sys->pos_x[i + 1u], y1 = sys->pos_y[i + 1u], z1 = sys->pos_z[i + 1u];
            float nx0 = 0.0f, ny0 = 1.0f;
            float nx1 = 0.0f, ny1 = 1.0f;
            compute_rope_joint_normal(sys, i, n, &nx0, &ny0);
            compute_rope_joint_normal(sys, i + 1u, n, &nx1, &ny1);
            float hs0 = sys->size[i] * 0.5f;
            float hs1 = sys->size[i + 1u] * 0.5f;

            float u0 = (float)i / (float)segs;
            float u1 = (float)(i + 1u) / (float)segs;

            we_particle_vertex_t *v = &sys->vert_buf[sys->quad_count * 4u];
            /* v0: start - normal */
            v[0].pos[0] = x0 - nx0 * hs0;
            v[0].pos[1] = y0 - ny0 * hs0;
            v[0].pos[2] = z0;
            v[0].uv[0] = u0;
            v[0].uv[1] = 0.0f;
            v[0].color[0] = sys->r[i];
            v[0].color[1] = sys->g[i];
            v[0].color[2] = sys->b[i];
            v[0].color[3] = sys->a[i];

            /* v1: end - normal */
            v[1].pos[0] = x1 - nx1 * hs1;
            v[1].pos[1] = y1 - ny1 * hs1;
            v[1].pos[2] = z1;
            v[1].uv[0] = u1;
            v[1].uv[1] = 0.0f;
            v[1].color[0] = sys->r[i + 1u];
            v[1].color[1] = sys->g[i + 1u];
            v[1].color[2] = sys->b[i + 1u];
            v[1].color[3] = sys->a[i + 1u];

            /* v2: end + normal */
            v[2].pos[0] = x1 + nx1 * hs1;
            v[2].pos[1] = y1 + ny1 * hs1;
            v[2].pos[2] = z1;
            v[2].uv[0] = u1;
            v[2].uv[1] = 1.0f;
            v[2].color[0] = sys->r[i + 1u];
            v[2].color[1] = sys->g[i + 1u];
            v[2].color[2] = sys->b[i + 1u];
            v[2].color[3] = sys->a[i + 1u];

            /* v3: start + normal */
            v[3].pos[0] = x0 + nx0 * hs0;
            v[3].pos[1] = y0 + ny0 * hs0;
            v[3].pos[2] = z0;
            v[3].uv[0] = u0;
            v[3].uv[1] = 1.0f;
            v[3].color[0] = sys->r[i];
            v[3].color[1] = sys->g[i];
            v[3].color[2] = sys->b[i];
            v[3].color[3] = sys->a[i];

            sys->quad_count++;
        }
    } else {
        uint32_t count = n < sys->quad_cap ? n : sys->quad_cap;
        uint32_t i = 0;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
        if (sys->renderer_type != WE_RENDERER_SPRITETRAIL && __builtin_cpu_supports("avx2") &&
            __builtin_cpu_supports("fma")) {
            i = particle_build_sprites_avx2(sys, count);
        }
#endif
        for (; i < count; i++) {
            float px = sys->pos_x[i];
            float py = sys->pos_y[i];
            float pz = sys->pos_z[i];
            float hs = sys->size[i] * 0.5f;

            float rx_x = hs, rx_y = 0.0f;
            float ry_x = 0.0f, ry_y = hs;

            if (sys->renderer_type == WE_RENDERER_SPRITETRAIL) {
                float vx = sys->vel_x[i];
                float vy = sys->vel_y[i];
                float spd = sqrtf(vx * vx + vy * vy);
                if (spd > 1e-4f) {
                    float dx = vx / spd;
                    float dy = vy / spd;
                    float stretch = fminf(fmaxf(spd * sys->trail_length, sys->trail_min_length),
                                          sys->trail_max_length);
                    float half_len = fmaxf(hs, hs * stretch);
                    rx_x = -dy * hs;
                    rx_y = dx * hs;
                    ry_x = dx * half_len;
                    ry_y = dy * half_len;
                }
            } else {
                float rz = sys->rot_z[i];
                float cs = cosf(rz);
                float sn = sinf(rz);
                rx_x = cs * hs;
                rx_y = sn * hs;
                ry_x = -sn * hs;
                ry_y = cs * hs;
            }

            we_particle_vertex_t *v = &sys->vert_buf[sys->quad_count * 4u];
            float cr = sys->r[i], cg = sys->g[i], cb = sys->b[i], ca = sys->a[i];

            /* v0: (-1, -1) -> uv(0, 0) */
            v[0].pos[0] = px - rx_x - ry_x;
            v[0].pos[1] = py - rx_y - ry_y;
            v[0].pos[2] = pz;
            v[0].uv[0] = 0.0f;
            v[0].uv[1] = 0.0f;
            v[0].color[0] = cr;
            v[0].color[1] = cg;
            v[0].color[2] = cb;
            v[0].color[3] = ca;

            /* v1: (+1, -1) -> uv(1, 0) */
            v[1].pos[0] = px + rx_x - ry_x;
            v[1].pos[1] = py + rx_y - ry_y;
            v[1].pos[2] = pz;
            v[1].uv[0] = 1.0f;
            v[1].uv[1] = 0.0f;
            v[1].color[0] = cr;
            v[1].color[1] = cg;
            v[1].color[2] = cb;
            v[1].color[3] = ca;

            /* v2: (+1, +1) -> uv(1, 1) */
            v[2].pos[0] = px + rx_x + ry_x;
            v[2].pos[1] = py + rx_y + ry_y;
            v[2].pos[2] = pz;
            v[2].uv[0] = 1.0f;
            v[2].uv[1] = 1.0f;
            v[2].color[0] = cr;
            v[2].color[1] = cg;
            v[2].color[2] = cb;
            v[2].color[3] = ca;

            /* v3: (-1, +1) -> uv(0, 1) */
            v[3].pos[0] = px - rx_x + ry_x;
            v[3].pos[1] = py - rx_y + ry_y;
            v[3].pos[2] = pz;
            v[3].uv[0] = 0.0f;
            v[3].uv[1] = 1.0f;
            v[3].color[0] = cr;
            v[3].color[1] = cg;
            v[3].color[2] = cb;
            v[3].color[3] = ca;

            sys->quad_count++;
        }
    }

    sys->vert_count = sys->quad_count * 4u;
    if (out_verts) {
        *out_verts = sys->vert_buf;
    }
    return sys->vert_count;
}

static inline uint16_t pack_rot_u16_scalar(float rad)
{
    const float inv_two_pi = 0.15915494309189535f;
    float norm = rad * inv_two_pi;
    norm -= floorf(norm);
    float q = roundf(norm * 65536.0f);
    if (!isfinite(q) || q < 0.0f) {
        return 0u;
    }
    uint32_t v = (uint32_t)q;
    return (uint16_t)(v & 0xFFFFu);
}

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx2,fma"))) static uint32_t
particle_build_instances_avx2(we_particle_sys_t *sys, uint32_t count)
{
    const __m256 v_zero = _mm256_setzero_ps();
    const __m256 v_one = _mm256_set1_ps(1.0f);
    const __m256 v_255 = _mm256_set1_ps(255.0f);
    const __m256 v_inv_2pi = _mm256_set1_ps(0.15915494309189535f);
    const __m256 v_65536 = _mm256_set1_ps(65536.0f);
    const __m256i v_mask16 = _mm256_set1_epi32(0xFFFF);
    const __m256i v_frame_1x = _mm256_set1_epi32(256 << 16);

    float pxa[8] __attribute__((aligned(32)));
    float pya[8] __attribute__((aligned(32)));
    float pza[8] __attribute__((aligned(32)));
    float sza[8] __attribute__((aligned(32)));
    uint32_t cola[8] __attribute__((aligned(32)));
    uint32_t rfa[8] __attribute__((aligned(32)));

    we_particle_instance_t *out = sys->inst_buf;
    uint32_t i = 0;
    for (; i + 8u <= count; i += 8u) {
        _mm256_store_ps(pxa, _mm256_load_ps(sys->pos_x + i));
        _mm256_store_ps(pya, _mm256_load_ps(sys->pos_y + i));
        _mm256_store_ps(pza, _mm256_load_ps(sys->pos_z + i));
        _mm256_store_ps(sza, _mm256_load_ps(sys->size + i));

        __m256 vr = _mm256_mul_ps(
            _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(sys->r + i), v_zero), v_one), v_255);
        __m256 vg = _mm256_mul_ps(
            _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(sys->g + i), v_zero), v_one), v_255);
        __m256 vb = _mm256_mul_ps(
            _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(sys->b + i), v_zero), v_one), v_255);
        __m256 va = _mm256_mul_ps(
            _mm256_min_ps(_mm256_max_ps(_mm256_load_ps(sys->a + i), v_zero), v_one), v_255);

        __m256i c_u32 = _mm256_or_si256(
            _mm256_or_si256(_mm256_cvtps_epi32(vr), _mm256_slli_epi32(_mm256_cvtps_epi32(vg), 8)),
            _mm256_or_si256(_mm256_slli_epi32(_mm256_cvtps_epi32(vb), 16),
                            _mm256_slli_epi32(_mm256_cvtps_epi32(va), 24)));
        _mm256_store_si256((__m256i *)cola, c_u32);

        __m256 norm_r = _mm256_mul_ps(_mm256_load_ps(sys->rot_z + i), v_inv_2pi);
        norm_r = _mm256_sub_ps(norm_r, _mm256_floor_ps(norm_r));
        __m256i rot_i =
            _mm256_and_si256(_mm256_cvtps_epi32(_mm256_mul_ps(norm_r, v_65536)), v_mask16);
        _mm256_store_si256((__m256i *)rfa, _mm256_or_si256(rot_i, v_frame_1x));

        for (uint32_t k = 0; k < 8u; k++) {
            we_particle_instance_t *dst = &out[i + k];
            dst->pos[0] = pxa[k];
            dst->pos[1] = pya[k];
            dst->pos[2] = pza[k];
            dst->size = sza[k];
            dst->color_u32 = cola[k];
            uint32_t rf = rfa[k];
            dst->rot_u16 = (uint16_t)(rf & 0xFFFFu);
            dst->frame_u16 = (uint16_t)(rf >> 16);
        }
    }
    return i;
}
#endif

uint32_t we_particle_build_instances(we_particle_sys_t *sys, we_particle_instance_t **out_inst)
{
    if (!sys) {
        if (out_inst) {
            *out_inst = NULL;
        }
        return 0u;
    }
    uint32_t need_cap = sys->padded_cap > 0u ? sys->padded_cap : sys->max_count;
    if (need_cap == 0u) {
        if (out_inst) {
            *out_inst = NULL;
        }
        return 0u;
    }
    if (!sys->inst_buf || sys->inst_cap < need_cap) {
        we_particle_instance_t *nb = calloc((size_t)need_cap, sizeof(we_particle_instance_t));
        if (!nb) {
            if (out_inst) {
                *out_inst = NULL;
            }
            return 0u;
        }
        free(sys->inst_buf);
        sys->inst_buf = nb;
        sys->inst_cap = need_cap;
    }

    uint32_t n = sys->alive_count <= sys->inst_cap ? sys->alive_count : sys->inst_cap;
    sys->inst_count = n;
    if (n == 0u) {
        if (out_inst) {
            *out_inst = sys->inst_buf;
        }
        return 0u;
    }

    uint32_t i = 0;
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    if (sys->renderer_type != WE_RENDERER_SPRITETRAIL && __builtin_cpu_supports("avx2") &&
        __builtin_cpu_supports("fma")) {
        i = particle_build_instances_avx2(sys, n);
    }
#endif
    for (; i < n; i++) {
        we_particle_instance_t *dst = &sys->inst_buf[i];
        dst->pos[0] = sys->pos_x[i];
        dst->pos[1] = sys->pos_y[i];
        dst->pos[2] = sys->pos_z[i];
        dst->size = sys->size[i];

        float r = fminf(fmaxf(sys->r[i], 0.0f), 1.0f) * 255.0f;
        float g = fminf(fmaxf(sys->g[i], 0.0f), 1.0f) * 255.0f;
        float b = fminf(fmaxf(sys->b[i], 0.0f), 1.0f) * 255.0f;
        float a = fminf(fmaxf(sys->a[i], 0.0f), 1.0f) * 255.0f;
        uint32_t cr = isfinite(r) ? (uint32_t)lrintf(r) : 255u;
        uint32_t cg = isfinite(g) ? (uint32_t)lrintf(g) : 255u;
        uint32_t cb = isfinite(b) ? (uint32_t)lrintf(b) : 255u;
        uint32_t ca = isfinite(a) ? (uint32_t)lrintf(a) : 255u;
        dst->color_u32 =
            (cr & 0xFFu) | ((cg & 0xFFu) << 8) | ((cb & 0xFFu) << 16) | ((ca & 0xFFu) << 24);

        if (sys->renderer_type == WE_RENDERER_SPRITETRAIL) {
            float vx = sys->vel_x[i];
            float vy = sys->vel_y[i];
            float spd = sqrtf(vx * vx + vy * vy);
            if (spd > 1e-4f) {
                float dx = vx / spd;
                float dy = vy / spd;
                float stretch = fminf(fmaxf(spd * sys->trail_length, sys->trail_min_length),
                                      sys->trail_max_length);
                float aspect = fmaxf(1.0f, stretch);
                dst->rot_u16 = pack_rot_u16_scalar(atan2f(-dx, dy));
                float q = roundf(aspect * 256.0f);
                dst->frame_u16 = (uint16_t)fminf(fmaxf(q, 256.0f), 65535.0f);
            } else {
                dst->rot_u16 = 0u;
                dst->frame_u16 = 256u;
            }
        } else {
            dst->rot_u16 = pack_rot_u16_scalar(sys->rot_z[i]);
            dst->frame_u16 = 256u;
        }
    }

    if (out_inst) {
        *out_inst = sys->inst_buf;
    }
    return n;
}

typedef struct {
    float pos_age[4];
    float vel_life[4];
    float size_phase_rz[4];
    uint32_t packed[4];
} we_gpu_particle_t;
static_assert(sizeof(we_gpu_particle_t) == 64, "gpu_particle std430 stride mismatch");

static inline uint32_t pack_rgba_u32(float r, float g, float b, float a)
{
    float cr = fminf(fmaxf(isfinite(r) ? r : 1.0f, 0.0f), 1.0f) * 255.0f;
    float cg = fminf(fmaxf(isfinite(g) ? g : 1.0f, 0.0f), 1.0f) * 255.0f;
    float cb = fminf(fmaxf(isfinite(b) ? b : 1.0f, 0.0f), 1.0f) * 255.0f;
    float ca = fminf(fmaxf(isfinite(a) ? a : 1.0f, 0.0f), 1.0f) * 255.0f;
    return ((uint32_t)lrintf(cr) & 0xFFu) | (((uint32_t)lrintf(cg) & 0xFFu) << 8) |
           (((uint32_t)lrintf(cb) & 0xFFu) << 16) | (((uint32_t)lrintf(ca) & 0xFFu) << 24);
}

static inline uint32_t pack_half2_u32(float f0, float f1)
{
    float arr[2] = {f0, f1};
    uint32_t out = 0u;
    for (int k = 0; k < 2; k++) {
        uint32_t u;
        memcpy(&u, &arr[k], sizeof(u));
        uint32_t sign = (u >> 16) & 0x8000u;
        int32_t exp = (int32_t)((u >> 23) & 0xFFu) - 127 + 15;
        uint32_t mant = u & 0x7FFFFFu;
        uint32_t h;
        if (exp <= 0) {
            h = sign;
        } else if (exp >= 31) {
            h = sign | 0x7C00u;
        } else {
            h = sign | ((uint32_t)exp << 10) | (mant >> 13);
        }
        out |= (h << (k * 16));
    }
    return out;
}

static inline float unpack_half_lane(uint32_t word, int lane)
{
    uint32_t h = (word >> (lane * 16)) & 0xFFFFu;
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t u;
    if (exp == 0u) {
        u = sign;
    } else if (exp == 31u) {
        u = sign | 0x7F800000u | (mant << 13);
    } else {
        u = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

typedef struct {
    uint32_t meta[4]; /* type, control_point, prefix_end, 0 */
    float dist_min[4];
    float dist_max[4];
    float dirs[4];
    float origin[4];
} we_gpu_emitter_t;
static_assert(sizeof(we_gpu_emitter_t) == 80, "gpu_emitter std430 stride mismatch");

typedef struct {
    uint32_t kind;
    float exponent;
    float scale;
    float offset;
    float min_val[4];
    float max_val[4];
    float forward[4];
} we_gpu_init_t;
static_assert(sizeof(we_gpu_init_t) == 64, "gpu_init std430 stride mismatch");

typedef struct {
    uint32_t kind;
    int32_t cp;
    uint32_t use_lut;
    float drag;
    float p0[4];
    float p1[4];
    float p2[4];
    float p3[4];
    float p4[4];
    float p5[4];
    float p6[4];
    float p7[4];
    float p8[4];
} we_gpu_op_t;
static_assert(sizeof(we_gpu_op_t) == 160, "gpu_op std430 stride mismatch");

typedef struct {
    we_gpu_emitter_t emitters[WE_PARTICLE_MAX_EMITTERS];
    we_gpu_init_t inits[WE_PARTICLE_MAX_INITIALIZERS];
    we_gpu_op_t ops[WE_PARTICLE_MAX_OPERATORS];
    float lut_s[WE_PARTICLE_MAX_OPERATORS][17][4];
    float lut_g[WE_PARTICLE_MAX_OPERATORS][17][4];
    float lut_b[WE_PARTICLE_MAX_OPERATORS][17][4];
} we_gpu_ssbo_config_t;
static_assert(offsetof(we_gpu_ssbo_config_t, lut_s) == 4224, "gpu_config header size mismatch");
static_assert(sizeof(we_gpu_ssbo_config_t) == 17280, "gpu_config total size mismatch");

typedef struct {
    uint32_t cmds[8][8]; /* Ring of DrawElementsIndirectCommand slots (32 B stride). */
} we_gpu_indirect_t;
static_assert(sizeof(we_gpu_indirect_t) == 256, "gpu_indirect size mismatch");

typedef struct {
    void *dpy;
    void *ctx;
    GLuint inst_prog;
    GLint inst_mvp_loc;
    GLint inst_color4_loc;
    GLint inst_trans_loc;
    GLint inst_rot_loc;
    GLint inst_tex0_loc;
    GLuint cs_sim_prog;
    GLint cs_counts_loc;
    GLint cs_counts2_loc;
    GLint cs_timing_loc;
    GLint cs_trail_loc;
    GLint cs_cp_loc;
    GLint cs_spawn_loc;
    GLuint quad_ebo;
    uint32_t quad_ebo_cap;
    GLuint white_tex;
    GLuint ext_prog;
    GLint ext_mvp_loc;
    GLint ext_color4_loc;
    GLint ext_trans_loc;
    GLint ext_rot_loc;
    GLint ext_tex0_loc;
    float last_mvp[16];
    float last_col4[4];
    float last_trans[2];
    float last_rot4[4];
    GLint last_tex0;
    bool last_uniforms_valid;
} we_part_gl_ctx_cache_t;

static pthread_mutex_t g_part_gl_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static we_part_gl_ctx_cache_t g_part_gl_cache[8];
static uint32_t g_part_gl_cache_next = 0u;

static GLuint compile_gl_shader_parts(GLenum stage, const char *const *parts, GLsizei count)
{
    GLuint sh = glCreateShader(stage);
    if (!sh) {
        return 0u;
    }
    glShaderSource(sh, count, parts, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log_buf[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_WARN("we_particle: GLES shader compile failed: %s", log_buf);
        glDeleteShader(sh);
        return 0u;
    }
    return sh;
}

static GLuint compile_gl_shader_src(GLenum stage, const char *src)
{
    return compile_gl_shader_parts(stage, &src, 1);
}

static GLuint link_compute_program_parts(const char *const *parts, GLsizei count)
{
    GLuint cs = compile_gl_shader_parts(GL_COMPUTE_SHADER, parts, count);
    if (!cs) {
        return 0u;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(cs);
        return 0u;
    }
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glDeleteShader(cs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log_buf[512];
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_WARN("we_particle: compute link failed: %s", log_buf);
        glDeleteProgram(prog);
        return 0u;
    }
    return prog;
}

static GLuint build_instanced_draw_prog(void)
{
    GLint v_ssbo = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v_ssbo);
    if (v_ssbo <= 0) {
        return 0u;
    }
    static const char *vs_src =
        "#version 310 es\n"
        "layout(std430, binding = 0) readonly buffer InstIn { uvec2 pairs[]; } inst_in;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "uniform vec2 g_Texture0Translation;\n"
        "uniform vec4 g_Texture0Rotation;\n"
        "uniform uint u_WwalPartMagic;\n"
        "out vec2 v_TexCoord;\n"
        "out vec4 v_Color;\n"
        "void main() {\n"
        "    uint pid = uint(gl_VertexID) >> 2u;\n"
        "    uint vid = uint(gl_VertexID) & 3u;\n"
        "    uint ib = pid * 3u;\n"
        "    uvec2 p0 = inst_in.pairs[ib + 0u];\n"
        "    uvec2 p1 = inst_in.pairs[ib + 1u];\n"
        "    uvec2 p2 = inst_in.pairs[ib + 2u];\n"
        "    vec3 pos = vec3(uintBitsToFloat(p0.x), uintBitsToFloat(p0.y), "
        "uintBitsToFloat(p1.x));\n"
        "    float hs = uintBitsToFloat(p1.y) * 0.5;\n"
        "    uint rf = p2.y;\n"
        "    float rot_q = float(rf & 0xFFFFu);\n"
        "    float stretch = max(1.0, float(rf >> 16u) * 0.00390625);\n"
        "    vec2 rx = vec2(hs, 0.0);\n"
        "    vec2 ry = vec2(0.0, hs * stretch);\n"
        "    if (rot_q != 0.0) {\n"
        "        float ang = rot_q * 9.587379924285257e-05;\n"
        "        float cs = cos(ang);\n"
        "        float sn = sin(ang);\n"
        "        rx = vec2(cs, sn) * hs;\n"
        "        ry = vec2(-sn, cs) * (hs * stretch);\n"
        "    }\n"
        "    vec2 c = vec2(float((vid & 1u) * 2u) - 1.0, float(vid & 2u) - 1.0);\n"
        "    vec3 worldPos = pos + vec3(c.x * rx + c.y * ry, 0.0);\n"
        "    gl_Position = g_ModelViewProjectionMatrix * vec4(worldPos, 1.0);\n"
        "    if (u_WwalPartMagic == 0x7FFFFFFFu) { gl_Position.z += 1.0; }\n"
        "    vec2 uv = c * 0.5 + 0.5;\n"
        "    v_TexCoord = g_Texture0Translation + uv.x * g_Texture0Rotation.xy + "
        "uv.y * g_Texture0Rotation.zw;\n"
        "    v_Color = unpackUnorm4x8(p2.x);\n"
        "}\n";
    static const char *fs_src = "#version 310 es\n"
                                "precision highp float;\n"
                                "in vec2 v_TexCoord;\n"
                                "in vec4 v_Color;\n"
                                "uniform sampler2D g_Texture0;\n"
                                "uniform vec4 g_Color4;\n"
                                "out vec4 fragColor;\n"
                                "void main() {\n"
                                "    vec4 tex = texture(g_Texture0, v_TexCoord);\n"
                                "    fragColor = tex * v_Color * g_Color4;\n"
                                "}\n";

    GLuint vs = compile_gl_shader_src(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_gl_shader_src(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) {
            glDeleteShader(vs);
        }
        if (fs) {
            glDeleteShader(fs);
        }
        return 0u;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0u;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(prog);
        return 0u;
    }
    return prog;
}

static GLuint build_cs_sim_prog(void)
{
    static const char *const parts[] = {
        "#version 310 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;\n"
        "uniform uint u_WwalPartMagic;\n"
        "uniform uvec4 u_Counts;\n"
        "uniform uvec4 u_Counts2;\n"
        "uniform vec4 u_Timing;\n"
        "uniform vec4 u_TrailP;\n"
        "uniform vec4 u_CpPos[8];\n"
        "uniform uint u_SpawnPfx[8];\n"
        "struct GpuParticle {\n"
        "    vec4 pos_age;\n"
        "    vec4 vel_life;\n"
        "    vec4 size_phase_rz;\n"
        "    uvec4 pbits;\n"
        "};\n"
        "layout(std430, binding = 0) readonly buffer StateIn { GpuParticle parts[]; } ssbo_in;\n"
        "layout(std430, binding = 1) writeonly buffer StateOut { GpuParticle parts[]; } ssbo_out;\n"
        "layout(std430, binding = 2) writeonly buffer InstOut { uvec2 pairs[]; } inst_out;\n"
        "layout(std430, binding = 3) buffer IndirectBuf { uint cmds[64]; } indirect;\n"
        "struct GpuEmitter { uvec4 meta; vec4 dist_min; vec4 dist_max; vec4 dirs; vec4 origin; };\n"
        "struct GpuInit { uint kind; float exponent; float scale; float offset; vec4 min_val; vec4 "
        "max_val; vec4 forward; };\n"
        "struct GpuOp { uint kind; int cp; uint use_lut; float drag; vec4 p0; vec4 p1; vec4 p2; "
        "vec4 p3; vec4 p4; vec4 p5; vec4 p6; vec4 p7; vec4 p8; };\n"
        "layout(std430, binding = 4) readonly buffer ConfigSsbo {\n"
        "    GpuEmitter emitters[8];\n"
        "    GpuInit inits[16];\n"
        "    GpuOp ops[16];\n"
        "    vec4 lut_s[16 * 17];\n"
        "    vec4 lut_g[16 * 17];\n"
        "    vec4 lut_b[16 * 17];\n"
        "} cfg;\n"
        "shared uint s_wg_count;\n"
        "shared uint s_wg_base;\n"
        "uint pcg_u32(inout uint state) {\n",
        "    state = state * 747796405u + 2891336453u;\n"
        "    uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;\n"
        "    return (w >> 22u) ^ w;\n"
        "}\n"
        "float prng_01(inout uint state) {\n"
        "    return float(pcg_u32(state) >> 8u) * 5.960464477539063e-08;\n"
        "}\n"
        "float prng_sym(inout uint state) { return prng_01(state) * 2.0 - 1.0; }\n"
        "float prng_rng(inout uint state, float lo, float hi) { return lo + prng_01(state) * (hi - "
        "lo); }\n"
        "float eval_lut_s(uint op_idx, float t) {\n"
        "    float u = clamp(t, 0.0, 1.0) * 64.0;\n"
        "    uint i0 = min(uint(floor(u)), 63u);\n"
        "    uint i1 = i0 + 1u;\n"
        "    float f = u - float(i0);\n"
        "    uint base = op_idx * 17u;\n"
        "    return mix(cfg.lut_s[base + (i0 >> 2u)][i0 & 3u], cfg.lut_s[base + (i1 >> 2u)][i1 & "
        "3u], f);\n"
        "}\n"
        "vec3 eval_lut_rgb(uint op_idx, float t) {\n"
        "    float u = clamp(t, 0.0, 1.0) * 64.0;\n"
        "    uint i0 = min(uint(floor(u)), 63u);\n"
        "    uint i1 = i0 + 1u;\n"
        "    float f = u - float(i0);\n"
        "    uint base = op_idx * 17u;\n"
        "    uint q0 = base + (i0 >> 2u); uint l0 = i0 & 3u;\n"
        "    uint q1 = base + (i1 >> 2u); uint l1 = i1 & 3u;\n"
        "    return vec3(mix(cfg.lut_s[q0][l0], cfg.lut_s[q1][l1], f),\n"
        "                mix(cfg.lut_g[q0][l0], cfg.lut_g[q1][l1], f),\n"
        "                mix(cfg.lut_b[q0][l0], cfg.lut_b[q1][l1], f));\n"
        "}\n"
        "vec3 eval_curl3(vec3 p) {\n"
        "    vec3 s = sin(p); vec3 c = cos(p);\n"
        "    return vec3(-s.y - c.z, -s.z - c.x, -s.x - c.y);\n"
        "}\n"
        "GpuParticle spawn_gpu_particle(uint slot, uint seed) {\n"
        "    uint rng = seed ^ (slot * 0x9E3779B9u + 0x85EBCA6Bu);\n"
        "    GpuParticle gp;\n"
        "    vec3 pos = vec3(960.0, 540.0, 0.0);\n"
        "    vec3 vel = vec3(0.0);\n"
        "    uint ecnt = u_Counts.z;\n"
        "    if (ecnt > 0u) {\n"
        "        uint eidx = 0u;\n"
        "        for (uint e = 0u; e < ecnt && e < 8u; e++) {\n"
        "            if (slot < u_SpawnPfx[e]) { eidx = e; break; }\n"
        "        }\n"
        "        GpuEmitter em = cfg.emitters[eidx];\n",
        "        uint cp_i = em.meta.y < 8u ? em.meta.y : 0u;\n"
        "        pos = em.origin.xyz + u_CpPos[cp_i].xyz;\n"
        "        if (em.meta.x == 0u) {\n"
        "            for (int k = 0; k < 3; k++) {\n"
        "                float dmin = em.dist_min[k]; float dmax = em.dist_max[k];\n"
        "                if (abs(dmin) <= 1e-5 && dmax >= 0.0) pos[k] += prng_sym(rng) * dmax * "
        "em.dirs[k];\n"
        "                else pos[k] += prng_rng(rng, min(dmin, dmax), max(dmin, dmax)) * "
        "em.dirs[k];\n"
        "            }\n"
        "        } else if (em.meta.x == 1u) {\n"
        "            float r0 = max(0.0, min(em.dist_min.x, em.dist_max.x));\n"
        "            float r1 = max(r0, max(em.dist_min.x, em.dist_max.x));\n"
        "            float rad = r0 + (r1 - r0) * pow(prng_01(rng), 1.0 / 3.0);\n"
        "            float x1 = 0.0, x2 = 0.0, sv = 1.0;\n"
        "            for (int t = 0; t < 12; t++) {\n"
        "                x1 = prng_sym(rng); x2 = prng_sym(rng); sv = x1 * x1 + x2 * x2;\n"
        "                if (sv < 1.0) break;\n"
        "            }\n"
        "            if (sv >= 1.0) { x1 = 0.0; x2 = 0.0; sv = 0.0; }\n"
        "            float rt = sqrt(max(0.0, 1.0 - sv));\n"
        "            vec3 d = vec3(2.0 * x1 * rt, 2.0 * x2 * rt, 1.0 - 2.0 * sv) * em.dirs.xyz;\n"
        "            float dl = length(d);\n"
        "            if (dl > 1e-5) d /= dl;\n"
        "            pos += d * rad;\n"
        "            if (abs(em.dist_min.w) > 1e-5 || abs(em.dist_max.w) > 1e-5) {\n"
        "                vel = d * prng_rng(rng, em.dist_min.w, em.dist_max.w);\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    vec4 rgba = vec4(1.0);\n"
        "    float sz = 20.0; float life = 2.0;\n"
        "    vec3 rot = vec3(0.0); vec3 avel = vec3(0.0);\n"
        "    uint icnt = u_Counts.w;\n"
        "    for (uint ii = 0u; ii < icnt && ii < 16u; ii++) {\n"
        "        GpuInit iz = cfg.inits[ii];\n"
        "        if (iz.kind == 0u) life = max(0.001, prng_rng(rng, iz.min_val.x, iz.max_val.x));\n"
        "        else if (iz.kind == 1u) sz = iz.min_val.x + pow(prng_01(rng), iz.exponent) * "
        "(iz.max_val.x - iz.min_val.x);\n"
        "        else if (iz.kind == 2u) rgba.a = prng_rng(rng, iz.min_val.x, iz.max_val.x);\n"
        "        else if (iz.kind == 3u) rgba.rgb = mix(iz.min_val.xyz, iz.max_val.xyz, "
        "prng_01(rng));\n"
        "        else if (iz.kind == 4u) vel += vec3(prng_rng(rng, iz.min_val.x, iz.max_val.x), "
        "prng_rng(rng, iz.min_val.y, iz.max_val.y), prng_rng(rng, iz.min_val.z, iz.max_val.z));\n"
        "        else if (iz.kind == 5u) rot = vec3(prng_rng(rng, iz.min_val.x, iz.max_val.x), "
        "prng_rng(rng, iz.min_val.y, iz.max_val.y), prng_rng(rng, iz.min_val.z, iz.max_val.z));\n"
        "        else if (iz.kind == 6u) avel = vec3(prng_rng(rng, iz.min_val.x, iz.max_val.x), "
        "prng_rng(rng, iz.min_val.y, iz.max_val.y), prng_rng(rng, iz.min_val.z, iz.max_val.z));\n"
        "        else if (iz.kind == 7u) {\n"
        "            float sp = prng_rng(rng, iz.min_val.x, iz.max_val.x);\n"
        "            float sc = iz.scale > 0.0 ? iz.scale : 0.01;\n"
        "            vel += eval_curl3(pos * sc + iz.offset) * sp * iz.forward.xyz;\n"
        "        }\n"
        "    }\n",
        "    gp.pos_age = vec4(pos, 0.0);\n"
        "    gp.vel_life = vec4(vel, life);\n"
        "    gp.size_phase_rz = vec4(sz, prng_01(rng) * 6.283185307, rot.z, avel.z);\n"
        "    gp.pbits = uvec4(packUnorm4x8(clamp(rgba, 0.0, 1.0)), packHalf2x16(rot.xy), "
        "packHalf2x16(avel.xy), 0u);\n"
        "    return gp;\n"
        "}\n"
        "void main() {\n"
        "    if (gl_LocalInvocationIndex == 0u) s_wg_count = 0u;\n"
        "    barrier();\n"
        "    uint gid = gl_GlobalInvocationID.x;\n"
        "    uint ind_bits = u_Counts2.w;\n"
        "    uint ind_in = ind_bits & 7u;\n"
        "    uint ind_out = (ind_bits >> 8u) & 7u;\n"
        "    uint ind_clr = (ind_bits >> 16u) & 7u;\n"
        "    if (gid == 0u) indirect.cmds[ind_clr * 8u + 0u] = 0u;\n"
        "    uint alive_in = min(indirect.cmds[ind_in * 8u + 0u] / 6u, u_Counts.x);\n"
        "    uint spawn_actual = min(u_Counts.y, u_Counts.x - alive_in);\n"
        "    uint total_work = alive_in + spawn_actual;\n"
        "    GpuParticle p;\n"
        "    vec4 rgba = vec4(1.0);\n"
        "    float sz = 20.0;\n"
        "    bool keep = false;\n"
        "    uint local_off = 0u;\n"
        "    if (gid < total_work && u_WwalPartMagic != 0x7FFFFFFFu) {\n"
        "        p = (gid < alive_in) ? ssbo_in.parts[gid] : spawn_gpu_particle(gid - alive_in, "
        "u_Counts2.z);\n"
        "        rgba = unpackUnorm4x8(p.pbits.x);\n"
        "        sz = p.size_phase_rz.x;\n"
        "        float rng_phase = p.size_phase_rz.y;\n"
        "        float dt = u_Timing.x;\n"
        "        float age = p.pos_age.w;\n"
        "        float t_norm = clamp(age / max(p.vel_life.w, 1e-5), 0.0, 1.0);\n"
        "        uint ocnt = u_Counts2.x;\n"
        "        for (uint o = 0u; o < ocnt && o < 16u; o++) {\n"
        "            uint kind = cfg.ops[o].kind;\n"
        "            if (kind == 0u) {\n"
        "                float damp = exp(clamp(-cfg.ops[o].drag * dt, -50.0, 10.0));\n"
        "                vec3 acc_p = (p.pbits.w != 0u) ? vec3(unpackHalf2x16(p.pbits.w), 0.0) : "
        "vec3(0.0);\n"
        "                p.vel_life.xyz = (p.vel_life.xyz + (cfg.ops[o].p0.xyz + acc_p) * dt) * "
        "damp;\n"
        "                p.pos_age.xyz += p.vel_life.xyz * dt;\n"
        "            } else if (kind == 1u) {\n"
        "                float damp = exp(clamp(-cfg.ops[o].drag * dt, -50.0, 10.0));\n"
        "                vec3 ag = cfg.ops[o].p0.xyz;\n"
        "                vec2 avxy = (unpackHalf2x16(p.pbits.z) + ag.xy * dt) * damp;\n"
        "                p.size_phase_rz.w = (p.size_phase_rz.w + ag.z * dt) * damp;\n"
        "                vec2 rxy = unpackHalf2x16(p.pbits.y) + avxy * dt;\n"
        "                p.size_phase_rz.z += p.size_phase_rz.w * dt;\n"
        "                p.pbits.y = packHalf2x16(rxy);\n"
        "                p.pbits.z = packHalf2x16(avxy);\n"
        "            } else if (kind == 2u) {\n"
        "                if (cfg.ops[o].use_lut != 0u) rgba.a *= eval_lut_s(o, t_norm);\n"
        "                else {\n"
        "                    float fit = cfg.ops[o].p3.w;\n"
        "                    float fot = cfg.ops[o].p4.w;\n"
        "                    float fi = (fit > 1e-5) ? clamp(t_norm / fit, 0.0, 1.0) : 1.0;\n"
        "                    float fo = (fot < 0.99999) ? clamp((1.0 - t_norm) / (1.0 - fot), 0.0, "
        "1.0) : 1.0;\n"
        "                    rgba.a *= (fi * fo);\n"
        "                }\n"
        "            } else if (kind == 3u) {\n"
        "                if (cfg.ops[o].use_lut != 0u) sz *= eval_lut_s(o, t_norm);\n"
        "                else {\n"
        "                    vec4 p1 = cfg.ops[o].p1; vec4 p2 = cfg.ops[o].p2;\n"
        "                    float st = p1.w, et = p2.w;\n"
        "                    float u = (et > st + 1e-5) ? clamp((t_norm - st) / (et - st), 0.0, "
        "1.0) : 1.0;\n"
        "                    sz *= mix(p1.x, p2.x, u);\n"
        "                }\n"
        "            } else if (kind == 4u) {\n"
        "                if (cfg.ops[o].use_lut != 0u) rgba.rgb *= eval_lut_rgb(o, t_norm);\n"
        "                else {\n"
        "                    vec4 p1 = cfg.ops[o].p1; vec4 p2 = cfg.ops[o].p2;\n"
        "                    float st = p1.w, et = p2.w;\n"
        "                    float u = (et > st + 1e-5) ? clamp((t_norm - st) / (et - st), 0.0, "
        "1.0) : 1.0;\n"
        "                    rgba.rgb *= mix(p1.xyz, p2.xyz, u);\n"
        "                }\n"
        "            } else if (kind == 5u || kind == 6u) {\n"
        "                vec4 p6 = cfg.ops[o].p6;\n"
        "                float freq = 0.5 * (p6.x + p6.y);\n"
        "                float sc = mix(p6.z, p6.w, 0.5 + 0.5 * sin(age * freq + rng_phase));\n"
        "                if (kind == 5u) rgba.a *= sc; else sz *= sc;\n"
        "            } else if (kind == 7u) {\n",
        "                vec4 p6 = cfg.ops[o].p6;\n"
        "                float freq = 0.5 * (p6.x + p6.y);\n"
        "                float amp = 0.5 * (p6.z + p6.w);\n"
        "                float p0 = age * freq + rng_phase;\n"
        "                float ds = sin(p0 + dt * freq) - sin(p0);\n"
        "                p.pos_age.xyz += cfg.ops[o].p5.xyz * (ds * amp);\n"
        "            } else if (kind == 8u) {\n"
        "                int cp = cfg.ops[o].cp;\n"
        "                int cidx = (cp >= 0 && cp < 8) ? cp : 0;\n"
        "                vec3 c = u_CpPos[cidx].xyz + cfg.ops[o].p4.xyz;\n"
        "                vec3 d = p.pos_age.xyz - c;\n"
        "                float dist = length(d);\n"
        "                if (dist >= 1e-4) {\n"
        "                    vec4 p7 = cfg.ops[o].p7;\n"
        "                    vec3 tdir = cross(cfg.ops[o].p3.xyz, d) / dist;\n"
        "                    float dout = max(p7.y, p7.x + 1.0);\n"
        "                    float u = clamp((dist - p7.x) / (dout - p7.x), 0.0, 1.0);\n"
        "                    float tspd = mix(p7.z, p7.w, u);\n"
        "                    p.vel_life.xyz += (tdir * tspd - d * (cfg.ops[o].p8.x / dist)) * "
        "dt;\n"
        "                }\n"
        "            } else if (kind == 9u) {\n"
        "                vec4 p5 = cfg.ops[o].p5; vec4 p8 = cfg.ops[o].p8;\n"
        "                float sc = p5.w > 0.0 ? p5.w : 0.01;\n"
        "                float t_off = mod(u_Timing.y * p8.z, 6.283185307);\n"
        "                vec3 curl = eval_curl3(p.pos_age.xyz * sc + t_off);\n"
        "                p.vel_life.xyz += curl * (p8.y * dt) * p5.xyz;\n"
        "            } else if (kind == 10u) {\n"
        "                int cp = cfg.ops[o].cp;\n"
        "                int cidx = (cp >= 0 && cp < 8) ? cp : 0;\n"
        "                vec3 c = u_CpPos[cidx].xyz + cfg.ops[o].p4.xyz;\n"
        "                vec3 d = c - p.pos_age.xyz;\n"
        "                float dist = length(d);\n"
        "                if (dist >= 1e-4) {\n"
        "                    vec4 p7 = cfg.ops[o].p7;\n"
        "                    float att = (p7.y > 0.0) ? clamp((p7.y - dist) / max(p7.y - p7.x, "
        "1e-4), 0.0, 1.0) : 1.0;\n"
        "                    p.vel_life.xyz += d * ((cfg.ops[o].p8.x * att * dt) / dist);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        p.pos_age.w = age + dt;\n"
        "        if (p.pos_age.w < p.vel_life.w) {\n"
        "            keep = true;\n"
        "            local_off = atomicAdd(s_wg_count, 1u);\n"
        "        }\n"
        "    }\n"
        "    barrier();\n"
        "    if (gl_LocalInvocationIndex == 0u && s_wg_count > 0u) {\n"
        "        s_wg_base = atomicAdd(indirect.cmds[ind_out * 8u + 0u], s_wg_count * 6u) / 6u;\n"
        "    }\n"
        "    barrier();\n"
        "    if (keep) {\n"
        "        uint dst = s_wg_base + local_off;\n"
        "        if (dst < u_Counts.x) {\n"
        "            ssbo_out.parts[dst] = p;\n"
        "            float ang = p.size_phase_rz.z;\n"
        "            float aspect = 1.0;\n"
        "            if (u_Counts2.y == 1u) {\n"
        "                float spd = length(p.vel_life.xy);\n"
        "                if (spd > 1e-4) {\n"
        "                    vec2 dir = p.vel_life.xy / spd;\n"
        "                    aspect = max(1.0, clamp(spd * u_Timing.z, u_Timing.w, "
        "u_TrailP.x));\n"
        "                    ang = atan(-dir.x, dir.y);\n"
        "                }\n"
        "            }\n"
        "            uint ru16 = 0u;\n"
        "            if (ang != 0.0) {\n"
        "                float aw = mod(ang, 6.283185307);\n"
        "                ru16 = uint(clamp(round(aw * 10430.37835047), 0.0, 65535.0)) & 0xFFFFu;\n"
        "            }\n"
        "            uint fu16 = (aspect <= 1.0001) ? 256u : uint(clamp(round(aspect * 256.0), "
        "1.0, 65535.0));\n"
        "            uint ib = dst * 3u;\n"
        "            inst_out.pairs[ib + 0u] = "
        "uvec2(floatBitsToUint(p.pos_age.x), floatBitsToUint(p.pos_age.y));\n"
        "            inst_out.pairs[ib + 1u] = "
        "uvec2(floatBitsToUint(p.pos_age.z), floatBitsToUint(sz));\n"
        "            inst_out.pairs[ib + 2u] = "
        "uvec2(packUnorm4x8(clamp(rgba, 0.0, 1.0)), ru16 | (fu16 << 16u));\n"
        "        }\n"
        "    }\n"
        "}\n"};
    return link_compute_program_parts(parts, (GLsizei)(sizeof(parts) / sizeof(parts[0])));
}

static bool ensure_cache_quad_ebo(we_part_gl_ctx_cache_t *e, uint32_t min_quads)
{
    if (!e) {
        return false;
    }
    if (e->quad_ebo != 0u && e->quad_ebo_cap >= min_quads) {
        return true;
    }
    uint32_t cap = min_quads < 4096u ? 4096u : min_quads;
    if (cap > WE_PARTICLE_MAX_CAP) {
        cap = WE_PARTICLE_MAX_CAP;
    }
    uint32_t *indices = malloc((size_t)cap * 6u * sizeof(uint32_t));
    if (!indices) {
        return false;
    }
    for (uint32_t q = 0; q < cap; q++) {
        uint32_t b = q * 4u;
        indices[q * 6u + 0u] = b + 0u;
        indices[q * 6u + 1u] = b + 1u;
        indices[q * 6u + 2u] = b + 3u;
        indices[q * 6u + 3u] = b + 0u;
        indices[q * 6u + 4u] = b + 3u;
        indices[q * 6u + 5u] = b + 2u;
    }
    if (e->quad_ebo == 0u) {
        glGenBuffers(1, &e->quad_ebo);
    }
    if (!e->quad_ebo) {
        free(indices);
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, e->quad_ebo);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)((size_t)cap * 6u * sizeof(uint32_t)), indices,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    free(indices);
    e->quad_ebo_cap = cap;
    return true;
}

static we_part_gl_ctx_cache_t *acquire_part_gl_cache(bool need_compute)
{
    EGLContext cur_ctx = eglGetCurrentContext();
    EGLDisplay cur_dpy = eglGetCurrentDisplay();
    if (cur_ctx == EGL_NO_CONTEXT) {
        return NULL;
    }

    pthread_mutex_lock(&g_part_gl_cache_mu);
    we_part_gl_ctx_cache_t *hit = NULL;
    for (uint32_t i = 0; i < 8u; i++) {
        we_part_gl_ctx_cache_t *e = &g_part_gl_cache[i];
        if (e->ctx == (void *)cur_ctx && e->dpy == (void *)cur_dpy && e->inst_prog != 0u &&
            glIsProgram(e->inst_prog) &&
            glGetUniformLocation(e->inst_prog, "u_WwalPartMagic") >= 0) {
            hit = e;
            break;
        }
    }
    if (!hit) {
        hit = &g_part_gl_cache[g_part_gl_cache_next % 8u];
        g_part_gl_cache_next++;
        memset(hit, 0, sizeof(*hit));
        hit->dpy = (void *)cur_dpy;
        hit->ctx = (void *)cur_ctx;
        hit->inst_prog = build_instanced_draw_prog();
        if (hit->inst_prog) {
            hit->inst_mvp_loc = glGetUniformLocation(hit->inst_prog, "g_ModelViewProjectionMatrix");
            hit->inst_color4_loc = glGetUniformLocation(hit->inst_prog, "g_Color4");
            hit->inst_trans_loc = glGetUniformLocation(hit->inst_prog, "g_Texture0Translation");
            hit->inst_rot_loc = glGetUniformLocation(hit->inst_prog, "g_Texture0Rotation");
            hit->inst_tex0_loc = glGetUniformLocation(hit->inst_prog, "g_Texture0");
        }
        GLuint wt = 0;
        glGenTextures(1, &wt);
        if (wt) {
            static const uint8_t white_px[4] = {255, 255, 255, 255};
            GLint prev_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
            glBindTexture(GL_TEXTURE_2D, wt);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_px);
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
            hit->white_tex = wt;
        }
    }
    if (need_compute) {
        if (!hit->cs_sim_prog || !glIsProgram(hit->cs_sim_prog) ||
            glGetUniformLocation(hit->cs_sim_prog, "u_WwalPartMagic") < 0) {
            hit->cs_sim_prog = build_cs_sim_prog();
            if (hit->cs_sim_prog) {
                hit->cs_counts_loc = glGetUniformLocation(hit->cs_sim_prog, "u_Counts");
                hit->cs_counts2_loc = glGetUniformLocation(hit->cs_sim_prog, "u_Counts2");
                hit->cs_timing_loc = glGetUniformLocation(hit->cs_sim_prog, "u_Timing");
                hit->cs_trail_loc = glGetUniformLocation(hit->cs_sim_prog, "u_TrailP");
                hit->cs_cp_loc = glGetUniformLocation(hit->cs_sim_prog, "u_CpPos");
                hit->cs_spawn_loc = glGetUniformLocation(hit->cs_sim_prog, "u_SpawnPfx");
            }
        }
    }
    pthread_mutex_unlock(&g_part_gl_cache_mu);
    return (hit->inst_prog != 0u) ? hit : NULL;
}

static bool ensure_legacy_quad_gl(we_particle_sys_t *sys)
{
    if (!sys || sys->quad_cap == 0u) {
        return false;
    }
    EGLContext cur = eglGetCurrentContext();
    if (cur == EGL_NO_CONTEXT) {
        return false;
    }
    if (sys->gl_vao && sys->gl_vbo && sys->gl_ebo && sys->gl_quad_cap >= sys->quad_cap &&
        sys->gl_owner_ctx == (void *)cur) {
        return true;
    }
    if (sys->gl_owner_ctx == (void *)cur) {
        GLuint b;
        if (sys->gl_ebo) {
            b = sys->gl_ebo;
            glDeleteBuffers(1, &b);
            sys->gl_ebo = 0u;
        }
        if (sys->gl_vbo) {
            b = sys->gl_vbo;
            glDeleteBuffers(1, &b);
            sys->gl_vbo = 0u;
        }
        if (sys->gl_vao) {
            b = sys->gl_vao;
            glDeleteVertexArrays(1, &b);
            sys->gl_vao = 0u;
        }
    } else if (sys->gl_owner_ctx != NULL) {
        return false;
    }

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    if (!vao || !vbo || !ebo) {
        if (ebo) {
            glDeleteBuffers(1, &ebo);
        }
        if (vbo) {
            glDeleteBuffers(1, &vbo);
        }
        if (vao) {
            glDeleteVertexArrays(1, &vao);
        }
        return false;
    }

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sys->quad_cap * 4u * sizeof(we_particle_vertex_t)),
                 NULL, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(we_particle_vertex_t), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(we_particle_vertex_t),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(we_particle_vertex_t),
                          (const void *)(5 * sizeof(float)));

    uint32_t *indices = malloc((size_t)sys->quad_cap * 6u * sizeof(uint32_t));
    if (!indices) {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &ebo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        return false;
    }
    for (uint32_t q = 0; q < sys->quad_cap; q++) {
        uint32_t base = q * 4u;
        indices[q * 6u + 0u] = base + 0u;
        indices[q * 6u + 1u] = base + 1u;
        indices[q * 6u + 2u] = base + 2u;
        indices[q * 6u + 3u] = base + 0u;
        indices[q * 6u + 4u] = base + 2u;
        indices[q * 6u + 5u] = base + 3u;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sys->quad_cap * 6u * sizeof(uint32_t)),
                 indices, GL_STATIC_DRAW);
    free(indices);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    sys->gl_vao = vao;
    sys->gl_vbo = vbo;
    sys->gl_ebo = ebo;
    sys->gl_quad_cap = sys->quad_cap;
    return true;
}

bool we_particle_upload_gl(we_particle_sys_t *sys)
{
    EGLContext cur = eglGetCurrentContext();
    if (!sys || sys->padded_cap == 0u || cur == EGL_NO_CONTEXT) {
        return false;
    }
    if (sys->gl_uploaded) {
        return sys->gl_owner_ctx == (void *)cur;
    }

    GLuint inst_vao = 0, inst_vbo = 0;
    glGenVertexArrays(1, &inst_vao);
    glGenBuffers(1, &inst_vbo);
    if (!inst_vao || !inst_vbo) {
        if (inst_vbo) {
            glDeleteBuffers(1, &inst_vbo);
        }
        if (inst_vao) {
            glDeleteVertexArrays(1, &inst_vao);
        }
        return false;
    }

    we_part_gl_ctx_cache_t *progs = acquire_part_gl_cache(false);
    if (progs) {
        (void)ensure_cache_quad_ebo(progs, sys->padded_cap);
    }

    glBindVertexArray(inst_vao);
    if (progs && progs->quad_ebo != 0u) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, progs->quad_ebo);
    }
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sys->padded_cap * sizeof(we_particle_instance_t)),
                 NULL, GL_DYNAMIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    sys->gl_owner_ctx = (void *)cur;
    sys->gl_inst_vao = inst_vao;
    sys->gl_inst_vbo = inst_vbo;
    sys->gl_uploaded = true;
    sys->gl_gpu_state_dirty = true;
    sys->gl_gpu_ops_dirty = true;
    return true;
}

static bool ensure_gpu_sim_buffers(we_particle_sys_t *sys)
{
    if (!we_particle_upload_gl(sys)) {
        return false;
    }
    EGLContext cur = eglGetCurrentContext();
    if (sys->gl_owner_ctx != (void *)cur) {
        return false;
    }
    if (sys->gl_ssbo[0] && sys->gl_ssbo[1] && sys->gl_indirect_bo && sys->gl_op_ssbo &&
        sys->gl_ssbo_cap >= sys->padded_cap) {
        return true;
    }
    if (sys->gl_ssbo[0]) {
        GLuint sb[2] = {sys->gl_ssbo[0], sys->gl_ssbo[1]};
        glDeleteBuffers(2, sb);
        sys->gl_ssbo[0] = 0u;
        sys->gl_ssbo[1] = 0u;
    }
    if (sys->gl_indirect_bo) {
        GLuint b = sys->gl_indirect_bo;
        glDeleteBuffers(1, &b);
        sys->gl_indirect_bo = 0u;
    }
    if (sys->gl_op_ssbo) {
        GLuint b = sys->gl_op_ssbo;
        glDeleteBuffers(1, &b);
        sys->gl_op_ssbo = 0u;
    }

    glGenBuffers(2, sys->gl_ssbo);
    glGenBuffers(1, &sys->gl_indirect_bo);
    glGenBuffers(1, &sys->gl_op_ssbo);
    if (!sys->gl_ssbo[0] || !sys->gl_ssbo[1] || !sys->gl_indirect_bo || !sys->gl_op_ssbo) {
        if (sys->gl_ssbo[0] || sys->gl_ssbo[1]) {
            GLuint sb[2] = {sys->gl_ssbo[0], sys->gl_ssbo[1]};
            glDeleteBuffers(2, sb);
            sys->gl_ssbo[0] = sys->gl_ssbo[1] = 0u;
        }
        if (sys->gl_indirect_bo) {
            GLuint b = sys->gl_indirect_bo;
            glDeleteBuffers(1, &b);
            sys->gl_indirect_bo = 0u;
        }
        if (sys->gl_op_ssbo) {
            GLuint b = sys->gl_op_ssbo;
            glDeleteBuffers(1, &b);
            sys->gl_op_ssbo = 0u;
        }
        return false;
    }

    GLsizeiptr pbytes = (GLsizeiptr)(sys->padded_cap * sizeof(we_gpu_particle_t));
    for (int k = 0; k < 2; k++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_ssbo[k]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, pbytes, NULL, GL_DYNAMIC_DRAW);
    }
    we_gpu_indirect_t init_ind;
    memset(&init_ind, 0, sizeof(init_ind));
    for (uint32_t s = 0; s < 8u; s++) {
        init_ind.cmds[s][1] = 1u;
    }
    init_ind.cmds[0][0] = sys->alive_count * 6u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_indirect_bo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sizeof(init_ind), &init_ind,
                 GL_DYNAMIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_op_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sizeof(we_gpu_ssbo_config_t), NULL,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    sys->gl_ssbo_cap = sys->padded_cap;
    sys->gl_ping_pong = 0u;
    sys->gl_indirect_idx = 0u;
    sys->gl_gpu_state_dirty = true;
    sys->gl_gpu_ops_dirty = true;
    return true;
}

static void upload_cpu_particles_to_ssbo(we_particle_sys_t *sys)
{
    uint32_t n = sys->alive_count <= sys->gl_ssbo_cap ? sys->alive_count : sys->gl_ssbo_cap;
    if (n > 0u) {
        we_gpu_particle_t *tmp = malloc((size_t)n * sizeof(we_gpu_particle_t));
        if (!tmp) {
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            tmp[i].pos_age[0] = sys->pos_x[i];
            tmp[i].pos_age[1] = sys->pos_y[i];
            tmp[i].pos_age[2] = sys->pos_z[i];
            tmp[i].pos_age[3] = sys->age[i];
            tmp[i].vel_life[0] = sys->vel_x[i];
            tmp[i].vel_life[1] = sys->vel_y[i];
            tmp[i].vel_life[2] = sys->vel_z[i];
            tmp[i].vel_life[3] = sys->max_life[i];
            tmp[i].size_phase_rz[0] = sys->init_size[i];
            tmp[i].size_phase_rz[1] = sys->rng_phase[i];
            tmp[i].size_phase_rz[2] = sys->rot_z[i];
            tmp[i].size_phase_rz[3] = sys->ang_vel_z[i];
            tmp[i].packed[0] =
                pack_rgba_u32(sys->init_r[i], sys->init_g[i], sys->init_b[i], sys->init_a[i]);
            tmp[i].packed[1] = pack_half2_u32(sys->rot_x[i], sys->rot_y[i]);
            tmp[i].packed[2] = pack_half2_u32(sys->ang_vel_x[i], sys->ang_vel_y[i]);
            tmp[i].packed[3] = pack_half2_u32(sys->acc_x[i], sys->acc_y[i]);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_ssbo[sys->gl_ping_pong]);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)((size_t)n * sizeof(we_gpu_particle_t)), tmp);
        free(tmp);
    }

    we_gpu_indirect_t ind;
    memset(&ind, 0, sizeof(ind));
    for (uint32_t s = 0; s < 8u; s++) {
        ind.cmds[s][1] = 1u;
    }
    sys->gl_indirect_idx = 0u;
    ind.cmds[0][0] = n * 6u;
    sys->gl_work_bound = n;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_indirect_bo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(ind), &ind);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static uint64_t compute_gpu_static_cfg_hash(const we_particle_sys_t *sys)
{
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = (const uint8_t *)sys->emitters;
    size_t sz = sizeof(we_particle_emitter_t) * sys->emitter_count;
    for (size_t i = 0; i < sz; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    p = (const uint8_t *)sys->initializers;
    sz = sizeof(we_particle_initializer_t) * sys->initializer_count;
    for (size_t i = 0; i < sz; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    for (uint32_t o = 0; o < sys->operator_count; o++) {
        const uint8_t *op = (const uint8_t *)&sys->operators[o];
        size_t osz = offsetof(we_particle_operator_t, ramp_lut);
        for (size_t i = 0; i < osz; i++) {
            h ^= op[i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static size_t pack_gpu_static_cfg(const we_particle_sys_t *sys, bool with_luts,
                                  we_gpu_ssbo_config_t *cfg)
{
    size_t upload_sz = with_luts ? sizeof(*cfg) : offsetof(we_gpu_ssbo_config_t, lut_s);
    memset(cfg, 0, upload_sz);

    for (uint32_t e = 0; e < sys->emitter_count && e < WE_PARTICLE_MAX_EMITTERS; e++) {
        const we_particle_emitter_t *em = &sys->emitters[e];
        cfg->emitters[e].meta[0] = (uint32_t)em->type;
        cfg->emitters[e].meta[1] = (uint32_t)(em->control_point >= 0 ? em->control_point : 0);
        cfg->emitters[e].dist_min[0] = em->distance_min[0];
        cfg->emitters[e].dist_min[1] = em->distance_min[1];
        cfg->emitters[e].dist_min[2] = em->distance_min[2];
        cfg->emitters[e].dist_min[3] = em->speed_min;
        cfg->emitters[e].dist_max[0] = em->distance_max[0];
        cfg->emitters[e].dist_max[1] = em->distance_max[1];
        cfg->emitters[e].dist_max[2] = em->distance_max[2];
        cfg->emitters[e].dist_max[3] = em->speed_max;
        cfg->emitters[e].dirs[0] = em->directions[0];
        cfg->emitters[e].dirs[1] = em->directions[1];
        cfg->emitters[e].dirs[2] = em->directions[2];
        cfg->emitters[e].origin[0] = em->origin[0];
        cfg->emitters[e].origin[1] = em->origin[1];
        cfg->emitters[e].origin[2] = em->origin[2];
    }

    for (uint32_t i = 0; i < sys->initializer_count && i < WE_PARTICLE_MAX_INITIALIZERS; i++) {
        const we_particle_initializer_t *iz = &sys->initializers[i];
        cfg->inits[i].kind = (uint32_t)iz->type;
        cfg->inits[i].exponent = iz->exponent;
        cfg->inits[i].scale = iz->scale;
        cfg->inits[i].offset = iz->offset;
        memcpy(cfg->inits[i].min_val, iz->min_val, sizeof(iz->min_val));
        memcpy(cfg->inits[i].max_val, iz->max_val, sizeof(iz->max_val));
        memcpy(cfg->inits[i].forward, iz->forward, sizeof(iz->forward));
    }

    for (uint32_t o = 0; o < sys->operator_count && o < WE_PARTICLE_MAX_OPERATORS; o++) {
        const we_particle_operator_t *op = &sys->operators[o];
        we_gpu_op_t *go = &cfg->ops[o];
        go->kind = (uint32_t)op->type;
        go->cp = op->control_point;
        go->use_lut = op->use_ramp_lut ? 1u : 0u;
        go->drag = op->drag;
        go->p0[0] = op->gravity[0];
        go->p0[1] = op->gravity[1];
        go->p0[2] = op->gravity[2];
        go->p1[0] = op->start_val[0];
        go->p1[1] = op->start_val[1];
        go->p1[2] = op->start_val[2];
        go->p1[3] = op->start_time;
        go->p2[0] = op->end_val[0];
        go->p2[1] = op->end_val[1];
        go->p2[2] = op->end_val[2];
        go->p2[3] = op->end_time;
        go->p3[0] = op->axis[0];
        go->p3[1] = op->axis[1];
        go->p3[2] = op->axis[2];
        go->p3[3] = op->fade_in_time;
        go->p4[0] = op->offset[0];
        go->p4[1] = op->offset[1];
        go->p4[2] = op->offset[2];
        go->p4[3] = op->fade_out_time;
        go->p5[0] = op->mask[0];
        go->p5[1] = op->mask[1];
        go->p5[2] = op->mask[2];
        go->p5[3] = op->turb_scale;
        go->p6[0] = op->freq_min;
        go->p6[1] = op->freq_max;
        go->p6[2] = op->scale_min;
        go->p6[3] = op->scale_max;
        go->p7[0] = op->distance_inner;
        go->p7[1] = op->distance_outer;
        go->p7[2] = op->speed_inner;
        go->p7[3] = op->speed_outer;
        go->p8[0] = op->center_force;
        go->p8[1] = op->turb_speed;
        go->p8[2] = op->turb_timescale;

        if (with_luts && op->use_ramp_lut) {
            if (op->type == WE_OP_COLOR_CHANGE) {
                for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
                    cfg->lut_s[o][k >> 2][k & 3u] = op->ramp_lut.color_lut_r[k];
                    cfg->lut_g[o][k >> 2][k & 3u] = op->ramp_lut.color_lut_g[k];
                    cfg->lut_b[o][k >> 2][k & 3u] = op->ramp_lut.color_lut_b[k];
                }
            } else {
                for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
                    cfg->lut_s[o][k >> 2][k & 3u] = op->ramp_lut.scalar_lut[k];
                }
            }
        }
    }

    return upload_sz;
}

bool we_particle_step_gpu(we_particle_sys_t *sys, float dt_sec)
{
    if (!sys || sys->paused || !isfinite(dt_sec) || dt_sec <= 0.0f) {
        return false;
    }
    if (sys->renderer_type == WE_RENDERER_ROPETRAIL) {
        return false;
    }
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }
    we_part_gl_ctx_cache_t *progs = acquire_part_gl_cache(true);
    if (!progs || !progs->cs_sim_prog) {
        return false;
    }
    if (!ensure_gpu_sim_buffers(sys)) {
        return false;
    }

    float dt = fminf(dt_sec, 0.25f);
    sys->sim_time += (double)dt;

    if (!sys->gl_gpu_sim_active || sys->gl_gpu_state_dirty) {
        upload_cpu_particles_to_ssbo(sys);
        sys->gl_gpu_state_dirty = false;
        sys->gl_gpu_ops_dirty = true;
    }

    uint8_t ping = sys->gl_ping_pong & 1u;
    uint8_t pong = (uint8_t)(ping ^ 1u);
    uint8_t ind_in = sys->gl_indirect_idx & 7u;
    uint8_t ind_out = (uint8_t)((ind_in + 1u) & 7u);
    uint8_t ind_clr = (uint8_t)((ind_in + 2u) & 7u);

    uint32_t burst_req = sys->gl_pending_burst;
    sys->gl_pending_burst = 0u;
    uint64_t cfg_hash = compute_gpu_static_cfg_hash(sys);
    bool upload_luts = sys->gl_gpu_ops_dirty;
    bool full_cfg = upload_luts || (cfg_hash != sys->gl_cfg_hash);

    if (full_cfg) {
        we_gpu_ssbo_config_t gcfg;
        size_t upload_sz = pack_gpu_static_cfg(sys, upload_luts, &gcfg);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_op_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)upload_sz, &gcfg);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        sys->gl_cfg_hash = cfg_hash;
        sys->gl_gpu_ops_dirty = false;
    }

    GLuint spawn_pfx[WE_PARTICLE_MAX_EMITTERS] = {0};
    uint32_t cum_spawn = 0u;
    for (uint32_t e = 0; e < sys->emitter_count && e < WE_PARTICLE_MAX_EMITTERS; e++) {
        const we_particle_emitter_t *em = &sys->emitters[e];
        uint32_t es = 0u;
        if (!sys->emitters[e].burst_done && em->instantaneous > 0u) {
            sys->emitters[e].burst_done = true;
            es += em->instantaneous;
        }
        if (em->rate > 0.0f && isfinite(em->rate)) {
            sys->emitters[e].emit_accum += em->rate * dt;
            if (!isfinite(sys->emitters[e].emit_accum) || sys->emitters[e].emit_accum < 0.0f) {
                sys->emitters[e].emit_accum = 0.0f;
            }
            if (sys->emitters[e].emit_accum > (float)sys->max_count) {
                sys->emitters[e].emit_accum = (float)sys->max_count;
            }
            uint32_t cs = (uint32_t)sys->emitters[e].emit_accum;
            sys->emitters[e].emit_accum -= (float)cs;
            es += cs;
        }
        cum_spawn += es;
        spawn_pfx[e] = cum_spawn;
    }
    uint32_t spawn_req = burst_req < cum_spawn ? cum_spawn : burst_req;

    uint64_t next_bound = (uint64_t)sys->gl_work_bound + (uint64_t)spawn_req;
    sys->gl_work_bound =
        (next_bound > (uint64_t)sys->max_count) ? sys->max_count : (uint32_t)next_bound;
    uint32_t groups_x = (sys->gl_work_bound + 255u) >> 8u;

    if (groups_x > 0u) {
        (void)prng_u01(&sys->rng_state);
        GLuint counts[4] = {sys->max_count, spawn_req, sys->emitter_count, sys->initializer_count};
        GLuint counts2[4] = {sys->operator_count, (uint32_t)sys->renderer_type,
                             (uint32_t)(sys->rng_state ^ (sys->rng_state >> 32)),
                             (uint32_t)ind_in | ((uint32_t)ind_out << 8u) |
                                 ((uint32_t)ind_clr << 16u)};
        float timing[4] = {dt, (float)fmod(sys->sim_time, 65536.0), sys->trail_length,
                           sys->trail_min_length};
        float trail_p[4] = {sys->trail_max_length, 0.0f, 0.0f, 0.0f};
        float cp_pos[WE_PARTICLE_MAX_CONTROL_PTS * 4];
        for (uint32_t c = 0; c < WE_PARTICLE_MAX_CONTROL_PTS; c++) {
            cp_pos[c * 4 + 0] = sys->control_points[c].pos[0];
            cp_pos[c * 4 + 1] = sys->control_points[c].pos[1];
            cp_pos[c * 4 + 2] = sys->control_points[c].pos[2];
            cp_pos[c * 4 + 3] = 0.0f;
        }

        GLint prev_prog = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sys->gl_ssbo[ping]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, sys->gl_ssbo[pong]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, sys->gl_inst_vbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, sys->gl_indirect_bo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sys->gl_op_ssbo);

        glUseProgram(progs->cs_sim_prog);
        if (progs->cs_counts_loc >= 0) {
            glUniform4uiv(progs->cs_counts_loc, 1, counts);
        }
        if (progs->cs_counts2_loc >= 0) {
            glUniform4uiv(progs->cs_counts2_loc, 1, counts2);
        }
        if (progs->cs_timing_loc >= 0) {
            glUniform4fv(progs->cs_timing_loc, 1, timing);
        }
        if (progs->cs_trail_loc >= 0) {
            glUniform4fv(progs->cs_trail_loc, 1, trail_p);
        }
        if (progs->cs_cp_loc >= 0) {
            glUniform4fv(progs->cs_cp_loc, WE_PARTICLE_MAX_CONTROL_PTS, cp_pos);
        }
        if (progs->cs_spawn_loc >= 0) {
            glUniform1uiv(progs->cs_spawn_loc, WE_PARTICLE_MAX_EMITTERS, spawn_pfx);
        }
        glDispatchCompute(groups_x, 1u, 1u);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT |
                        GL_COMMAND_BARRIER_BIT);

        for (GLuint b = 0; b <= 4u; b++) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, 0u);
        }
        glUseProgram((GLuint)prev_prog);

        sys->gl_ping_pong = pong;
        sys->gl_indirect_idx = ind_out;
    }

    sys->gl_gpu_sim_active = true;
    return true;
}

bool we_particle_gpu_sync_to_cpu(we_particle_sys_t *sys)
{
    EGLContext cur = eglGetCurrentContext();
    if (!sys || !sys->gl_gpu_sim_active || cur == EGL_NO_CONTEXT ||
        sys->gl_owner_ctx != (void *)cur || !sys->gl_indirect_bo || !sys->gl_ssbo[0]) {
        return false;
    }

    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_indirect_bo);
    const we_gpu_indirect_t *ind = (const we_gpu_indirect_t *)glMapBufferRange(
        GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(we_gpu_indirect_t), GL_MAP_READ_BIT);
    uint32_t alive = 0u;
    if (ind) {
        alive = ind->cmds[sys->gl_indirect_idx & 7u][0] / 6u;
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    if (alive > sys->max_count) {
        alive = sys->max_count;
    }
    sys->alive_count = alive;
    sys->gl_work_bound = alive;

    if (alive > 0u) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sys->gl_ssbo[sys->gl_ping_pong]);
        const we_gpu_particle_t *gp = (const we_gpu_particle_t *)glMapBufferRange(
            GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)((size_t)alive * sizeof(we_gpu_particle_t)),
            GL_MAP_READ_BIT);
        if (gp) {
            for (uint32_t i = 0; i < alive; i++) {
                sys->pos_x[i] = gp[i].pos_age[0];
                sys->pos_y[i] = gp[i].pos_age[1];
                sys->pos_z[i] = gp[i].pos_age[2];
                sys->age[i] = gp[i].pos_age[3];
                sys->vel_x[i] = gp[i].vel_life[0];
                sys->vel_y[i] = gp[i].vel_life[1];
                sys->vel_z[i] = gp[i].vel_life[2];
                sys->max_life[i] = gp[i].vel_life[3];
                sys->init_size[i] = gp[i].size_phase_rz[0];
                sys->rng_phase[i] = gp[i].size_phase_rz[1];
                sys->rot_z[i] = gp[i].size_phase_rz[2];
                sys->ang_vel_z[i] = gp[i].size_phase_rz[3];
                uint32_t rgba = gp[i].packed[0];
                sys->init_r[i] = (float)(rgba & 0xFFu) * (1.0f / 255.0f);
                sys->init_g[i] = (float)((rgba >> 8) & 0xFFu) * (1.0f / 255.0f);
                sys->init_b[i] = (float)((rgba >> 16) & 0xFFu) * (1.0f / 255.0f);
                sys->init_a[i] = (float)((rgba >> 24) & 0xFFu) * (1.0f / 255.0f);
                sys->rot_x[i] = unpack_half_lane(gp[i].packed[1], 0);
                sys->rot_y[i] = unpack_half_lane(gp[i].packed[1], 1);
                sys->ang_vel_x[i] = unpack_half_lane(gp[i].packed[2], 0);
                sys->ang_vel_y[i] = unpack_half_lane(gp[i].packed[2], 1);
                sys->acc_x[i] = unpack_half_lane(gp[i].packed[3], 0);
                sys->acc_y[i] = unpack_half_lane(gp[i].packed[3], 1);
                sys->acc_z[i] = 0.0f;
            }
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
}

void we_particle_draw_gl(we_particle_sys_t *sys, uint32_t gl_prog)
{
    EGLContext cur = eglGetCurrentContext();
    if (!sys || cur == EGL_NO_CONTEXT) {
        return;
    }
    if (sys->gl_uploaded && sys->gl_owner_ctx != (void *)cur) {
        return;
    }
    if (!sys->gl_uploaded && !we_particle_upload_gl(sys)) {
        return;
    }

    we_part_gl_ctx_cache_t *progs = acquire_part_gl_cache(false);
    GLint mvp_loc = -1;
    if (progs && gl_prog != 0u) {
        if (progs->ext_prog != gl_prog) {
            progs->ext_prog = gl_prog;
            progs->ext_mvp_loc =
                glGetUniformLocation((GLuint)gl_prog, "g_ModelViewProjectionMatrix");
            progs->ext_color4_loc = glGetUniformLocation((GLuint)gl_prog, "g_Color4");
            progs->ext_trans_loc = glGetUniformLocation((GLuint)gl_prog, "g_Texture0Translation");
            progs->ext_rot_loc = glGetUniformLocation((GLuint)gl_prog, "g_Texture0Rotation");
            progs->ext_tex0_loc = glGetUniformLocation((GLuint)gl_prog, "g_Texture0");
        }
        mvp_loc = progs->ext_mvp_loc;
    }

    bool can_use_instanced =
        (sys->renderer_type != WE_RENDERER_ROPETRAIL) && (gl_prog == 0u || mvp_loc >= 0);

    if (can_use_instanced && progs && progs->inst_prog != 0u) {
        if (!sys->gl_gpu_sim_active && sys->alive_count == 0u) {
            return;
        }
        if (!sys->gl_gpu_sim_active) {
            we_particle_instance_t *insts = NULL;
            uint32_t icnt = we_particle_build_instances(sys, &insts);
            if (icnt == 0u || !insts) {
                return;
            }
            glBindBuffer(GL_ARRAY_BUFFER, sys->gl_inst_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(icnt * sizeof(we_particle_instance_t)),
                            insts);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float col4[4] = {1, 1, 1, 1};
        float trans[2] = {0, 0};
        float rot4[4] = {1, 0, 0, 1};
        GLint tex0 = 0;

        GLint prev_prog = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
        if (gl_prog != 0u && mvp_loc >= 0) {
            glGetUniformfv((GLuint)gl_prog, mvp_loc, mvp);
            if (progs->ext_color4_loc >= 0) {
                glGetUniformfv((GLuint)gl_prog, progs->ext_color4_loc, col4);
            }
            if (progs->ext_trans_loc >= 0) {
                glGetUniformfv((GLuint)gl_prog, progs->ext_trans_loc, trans);
            }
            if (progs->ext_rot_loc >= 0) {
                glGetUniformfv((GLuint)gl_prog, progs->ext_rot_loc, rot4);
            }
            if (progs->ext_tex0_loc >= 0) {
                glGetUniformiv((GLuint)gl_prog, progs->ext_tex0_loc, &tex0);
            }
        }

        GLboolean prev_depth_mask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);
        glDepthMask(GL_FALSE);

        glUseProgram(progs->inst_prog);
        if (!progs->last_uniforms_valid || memcmp(progs->last_mvp, mvp, sizeof(mvp)) != 0) {
            if (progs->inst_mvp_loc >= 0) {
                glUniformMatrix4fv(progs->inst_mvp_loc, 1, GL_FALSE, mvp);
            }
            memcpy(progs->last_mvp, mvp, sizeof(mvp));
        }
        if (!progs->last_uniforms_valid || memcmp(progs->last_col4, col4, sizeof(col4)) != 0) {
            if (progs->inst_color4_loc >= 0) {
                glUniform4fv(progs->inst_color4_loc, 1, col4);
            }
            memcpy(progs->last_col4, col4, sizeof(col4));
        }
        if (!progs->last_uniforms_valid || memcmp(progs->last_trans, trans, sizeof(trans)) != 0) {
            if (progs->inst_trans_loc >= 0) {
                glUniform2fv(progs->inst_trans_loc, 1, trans);
            }
            memcpy(progs->last_trans, trans, sizeof(trans));
        }
        if (!progs->last_uniforms_valid || memcmp(progs->last_rot4, rot4, sizeof(rot4)) != 0) {
            if (progs->inst_rot_loc >= 0) {
                glUniform4fv(progs->inst_rot_loc, 1, rot4);
            }
            memcpy(progs->last_rot4, rot4, sizeof(rot4));
        }
        if (!progs->last_uniforms_valid || progs->last_tex0 != tex0) {
            if (progs->inst_tex0_loc >= 0) {
                glUniform1i(progs->inst_tex0_loc, tex0);
            }
            progs->last_tex0 = tex0;
        }
        progs->last_uniforms_valid = true;

        if (gl_prog == 0u && progs->white_tex != 0u) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, progs->white_tex);
        }

        (void)ensure_cache_quad_ebo(progs, sys->padded_cap);
        glBindVertexArray(sys->gl_inst_vao);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sys->gl_inst_vbo);
        if (sys->gl_gpu_sim_active && sys->gl_indirect_bo != 0u) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sys->gl_indirect_bo);
            glDrawElementsIndirect(
                GL_TRIANGLES, GL_UNSIGNED_INT,
                (const void *)(uintptr_t)((uint32_t)(sys->gl_indirect_idx & 7u) * 32u));
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        } else {
            uint32_t cnt = sys->inst_count <= sys->padded_cap ? sys->inst_count : sys->padded_cap;
            glDrawElements(GL_TRIANGLES, (GLsizei)(cnt * 6u), GL_UNSIGNED_INT, (const void *)0);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0u);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram((GLuint)prev_prog);
        glDepthMask(prev_depth_mask);
        return;
    }

    /* Fallback path for custom minimal shaders or ropetrail ribbon geometry. */
    if (sys->gl_gpu_sim_active) {
        (void)we_particle_gpu_sync_to_cpu(sys);
    }
    we_particle_vertex_t *verts = NULL;
    uint32_t vcnt = we_particle_build_vertices(sys, &verts);
    if (vcnt == 0u || sys->quad_count == 0u || !verts) {
        return;
    }
    if (!ensure_legacy_quad_gl(sys)) {
        return;
    }

    uint32_t draw_quads = sys->quad_count <= sys->gl_quad_cap ? sys->quad_count : sys->gl_quad_cap;
    uint32_t draw_verts = draw_quads * 4u;
    if (draw_quads == 0u) {
        return;
    }

    GLboolean prev_depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);
    glDepthMask(GL_FALSE);
    glBindVertexArray(sys->gl_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sys->gl_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(sys->gl_quad_cap * 4u * sizeof(we_particle_vertex_t)), NULL,
                 GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(draw_verts * sizeof(we_particle_vertex_t)),
                    verts);
    glDrawElements(GL_TRIANGLES, (GLsizei)(draw_quads * 6u), GL_UNSIGNED_INT, (const void *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDepthMask(prev_depth_mask);
}

void we_particle_destroy(we_particle_sys_t *sys)
{
    if (!sys) {
        return;
    }
    release_particle_gl_buffers(sys);
    free(sys->inst_buf);
    free(sys->vert_buf);
    free(sys->soa_block);
    free(sys);
}

/* =========================================================================
 * Ramp LUT Baking, OKLab Gradients & Clean-Room Particle Presets
 * ========================================================================= */

float we_particle_eval_scalar_lut(const float lut[WE_PARTICLE_RAMP_SAMPLES], float t)
{
    if (!lut) {
        return 1.0f;
    }
    if (!isfinite(t)) {
        return lut[0];
    }
    float tc = fminf(fmaxf(t, 0.0f), 1.0f);
    float pos = tc * (float)(WE_PARTICLE_RAMP_SAMPLES - 1u);
    uint32_t idx = (uint32_t)pos;
    if (idx >= WE_PARTICLE_RAMP_SAMPLES - 1u) {
        return lut[WE_PARTICLE_RAMP_SAMPLES - 1u];
    }
    float frac = pos - (float)idx;
    return lut[idx] + frac * (lut[idx + 1u] - lut[idx]);
}

static void sort_curve_knots(const we_particle_curve_knot_t *in_knots, uint32_t n,
                             we_particle_curve_knot_t out[WE_PARTICLE_MAX_KNOTS])
{
    for (uint32_t i = 0; i < n; i++) {
        out[i] = in_knots[i];
        out[i].t = fminf(fmaxf(out[i].t, 0.0f), 1.0f);
    }
    for (uint32_t i = 1; i < n; i++) {
        we_particle_curve_knot_t key = out[i];
        uint32_t j = i;
        while (j > 0 && out[j - 1u].t > key.t) {
            out[j] = out[j - 1u];
            j--;
        }
        out[j] = key;
    }
}

bool we_particle_bake_scalar_lut(const we_particle_curve_knot_t *knots, uint32_t knot_count,
                                 float out_lut[WE_PARTICLE_RAMP_SAMPLES])
{
    if (!knots || !out_lut || knot_count == 0u || knot_count > WE_PARTICLE_MAX_KNOTS) {
        return false;
    }
    for (uint32_t i = 0; i < knot_count; i++) {
        if (!isfinite(knots[i].t) || !isfinite(knots[i].y) || !isfinite(knots[i].in_tangent) ||
            !isfinite(knots[i].out_tangent)) {
            return false;
        }
    }
    if (knot_count == 1u) {
        float y = knots[0].y;
        for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
            out_lut[k] = y;
        }
        return true;
    }

    we_particle_curve_knot_t sk[WE_PARTICLE_MAX_KNOTS];
    sort_curve_knots(knots, knot_count, sk);

    float h[WE_PARTICLE_MAX_KNOTS];
    float delta[WE_PARTICLE_MAX_KNOTS];
    float m[WE_PARTICLE_MAX_KNOTS];
    memset(m, 0, sizeof(m));

    for (uint32_t i = 0; i + 1u < knot_count; i++) {
        h[i] = fmaxf(sk[i + 1u].t - sk[i].t, 1e-6f);
        delta[i] = (sk[i + 1u].y - sk[i].y) / h[i];
    }

    m[0] = delta[0];
    m[knot_count - 1u] = delta[knot_count - 2u];
    for (uint32_t i = 1; i + 1u < knot_count; i++) {
        if (delta[i - 1u] * delta[i] <= 0.0f) {
            m[i] = 0.0f;
        } else {
            m[i] = 0.5f * (delta[i - 1u] + delta[i]);
        }
    }

    /* Enforce Fritsch-Carlson overshoot guard (a^2 + b^2 <= 9) per monotone segment */
    for (uint32_t i = 0; i + 1u < knot_count; i++) {
        if (fabsf(delta[i]) < 1e-8f) {
            m[i] = 0.0f;
            m[i + 1u] = 0.0f;
            continue;
        }
        float a = m[i] / delta[i];
        float b = m[i + 1u] / delta[i];
        float ab2 = a * a + b * b;
        if (ab2 > 9.0f) {
            float tau = 3.0f / sqrtf(ab2);
            m[i] = tau * a * delta[i];
            m[i + 1u] = tau * b * delta[i];
        }
    }

    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
        float u = (float)k / (float)(WE_PARTICLE_RAMP_SAMPLES - 1u);
        if (u <= sk[0].t) {
            out_lut[k] = sk[0].y;
            continue;
        }
        if (u >= sk[knot_count - 1u].t) {
            out_lut[k] = sk[knot_count - 1u].y;
            continue;
        }
        uint32_t seg = 0;
        while (seg + 1u < knot_count - 1u && u > sk[seg + 1u].t) {
            seg++;
        }
        float dt = sk[seg + 1u].t - sk[seg].t;
        float s = (dt > 1e-6f) ? ((u - sk[seg].t) / dt) : 0.0f;
        s = fminf(fmaxf(s, 0.0f), 1.0f);

        uint32_t mode = sk[seg].mode;
        if (mode == WE_KNOT_HANDLE_STEP) {
            out_lut[k] = sk[seg].y;
        } else if (mode == WE_KNOT_HANDLE_LINEAR) {
            out_lut[k] = sk[seg].y + s * (sk[seg + 1u].y - sk[seg].y);
        } else {
            float m0 = (mode == WE_KNOT_HANDLE_FREE_BEZIER) ? sk[seg].out_tangent : m[seg];
            float m1 = (mode == WE_KNOT_HANDLE_FREE_BEZIER) ? sk[seg + 1u].in_tangent : m[seg + 1u];
            float s2 = s * s;
            float s3 = s2 * s;
            float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
            float h10 = s3 - 2.0f * s2 + s;
            float h01 = -2.0f * s3 + 3.0f * s2;
            float h11 = s3 - s2;
            out_lut[k] =
                h00 * sk[seg].y + h10 * h[seg] * m0 + h01 * sk[seg + 1u].y + h11 * h[seg] * m1;
        }
    }
    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
        if (!isfinite(out_lut[k])) {
            return false;
        }
    }
    return true;
}

static inline float srgb_to_linear_f32(float c)
{
    c = fminf(fmaxf(c, 0.0f), 1.0f);
    return (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float linear_to_srgb_f32(float c)
{
    c = fminf(fmaxf(c, 0.0f), 1.0f);
    return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
}

static void srgb_to_oklab(const float rgb[3], float lab[3])
{
    float r = srgb_to_linear_f32(rgb[0]);
    float g = srgb_to_linear_f32(rgb[1]);
    float b = srgb_to_linear_f32(rgb[2]);

    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static void oklab_to_srgb(const float lab[3], float rgb[3])
{
    float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    rgb[0] = linear_to_srgb_f32(r);
    rgb[1] = linear_to_srgb_f32(g);
    rgb[2] = linear_to_srgb_f32(b);
}

bool we_particle_bake_oklab_lut(const we_particle_color_stop_t *stops, uint32_t stop_count,
                                float out_r[WE_PARTICLE_RAMP_SAMPLES],
                                float out_g[WE_PARTICLE_RAMP_SAMPLES],
                                float out_b[WE_PARTICLE_RAMP_SAMPLES])
{
    if (!stops || !out_r || !out_g || !out_b || stop_count == 0u ||
        stop_count > WE_PARTICLE_MAX_KNOTS) {
        return false;
    }
    for (uint32_t i = 0; i < stop_count; i++) {
        if (!isfinite(stops[i].t) || !isfinite(stops[i].rgb[0]) || !isfinite(stops[i].rgb[1]) ||
            !isfinite(stops[i].rgb[2])) {
            return false;
        }
    }

    we_particle_color_stop_t sorted[WE_PARTICLE_MAX_KNOTS];
    float labs[WE_PARTICLE_MAX_KNOTS][3];
    for (uint32_t i = 0; i < stop_count; i++) {
        sorted[i] = stops[i];
        sorted[i].t = fminf(fmaxf(sorted[i].t, 0.0f), 1.0f);
    }
    for (uint32_t i = 1; i < stop_count; i++) {
        we_particle_color_stop_t key = sorted[i];
        uint32_t j = i;
        while (j > 0 && sorted[j - 1u].t > key.t) {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = key;
    }
    for (uint32_t i = 0; i < stop_count; i++) {
        srgb_to_oklab(sorted[i].rgb, labs[i]);
    }

    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
        float u = (float)k / (float)(WE_PARTICLE_RAMP_SAMPLES - 1u);
        float rgb[3];
        if (stop_count == 1u || u <= sorted[0].t) {
            rgb[0] = fminf(fmaxf(sorted[0].rgb[0], 0.0f), 1.0f);
            rgb[1] = fminf(fmaxf(sorted[0].rgb[1], 0.0f), 1.0f);
            rgb[2] = fminf(fmaxf(sorted[0].rgb[2], 0.0f), 1.0f);
        } else if (u >= sorted[stop_count - 1u].t) {
            rgb[0] = fminf(fmaxf(sorted[stop_count - 1u].rgb[0], 0.0f), 1.0f);
            rgb[1] = fminf(fmaxf(sorted[stop_count - 1u].rgb[1], 0.0f), 1.0f);
            rgb[2] = fminf(fmaxf(sorted[stop_count - 1u].rgb[2], 0.0f), 1.0f);
        } else {
            uint32_t seg = 0;
            while (seg + 1u < stop_count - 1u && u > sorted[seg + 1u].t) {
                seg++;
            }
            float span = sorted[seg + 1u].t - sorted[seg].t;
            float s = (span > 1e-6f) ? ((u - sorted[seg].t) / span) : 0.0f;
            s = fminf(fmaxf(s, 0.0f), 1.0f);
            float lab[3] = {
                labs[seg][0] + s * (labs[seg + 1u][0] - labs[seg][0]),
                labs[seg][1] + s * (labs[seg + 1u][1] - labs[seg][1]),
                labs[seg][2] + s * (labs[seg + 1u][2] - labs[seg][2]),
            };
            oklab_to_srgb(lab, rgb);
        }
        out_r[k] = rgb[0];
        out_g[k] = rgb[1];
        out_b[k] = rgb[2];
    }
    return true;
}

bool we_particle_apply_ramp_lut(we_particle_sys_t *sys, const we_particle_ramp_lut_t *lut)
{
    if (!sys || !lut || lut->channel_kind > 2u) {
        return false;
    }

    if (lut->channel_kind == 2u) {
        for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
            if (!isfinite(lut->color_lut_r[k]) || !isfinite(lut->color_lut_g[k]) ||
                !isfinite(lut->color_lut_b[k])) {
                return false;
            }
        }
    } else {
        for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; k++) {
            if (!isfinite(lut->scalar_lut[k])) {
                return false;
            }
        }
    }

    we_operator_type_t want_type = WE_OP_ALPHA_FADE;
    if (lut->channel_kind == 1u) {
        want_type = WE_OP_SIZE_CHANGE;
    } else if (lut->channel_kind == 2u) {
        want_type = WE_OP_COLOR_CHANGE;
    }

    we_particle_operator_t *target = NULL;
    if (lut->operator_index == UINT32_MAX) {
        for (uint32_t i = 0; i < sys->operator_count; i++) {
            if (sys->operators[i].type == want_type) {
                target = &sys->operators[i];
                break;
            }
        }
        if (!target && sys->operator_count < WE_PARTICLE_MAX_OPERATORS) {
            target = &sys->operators[sys->operator_count++];
            memset(target, 0, sizeof(*target));
            target->type = want_type;
            target->end_time = 1.0f;
        }
    } else if (lut->operator_index < sys->operator_count) {
        if (sys->operators[lut->operator_index].type != want_type) {
            return false;
        }
        target = &sys->operators[lut->operator_index];
    } else {
        return false;
    }
    if (!target) {
        return false;
    }
    target->ramp_lut = *lut;
    target->use_ramp_lut = true;
    sys->gl_gpu_ops_dirty = true;
    return true;
}

static const we_particle_preset_desc_t g_particle_presets[WE_PARTICLE_PRESET_COUNT] = {
    {WE_PARTICLE_PRESET_RAIN,
     1600u,
     WE_RENDERER_SPRITETRAIL,
     0u,
     2.0f,
     {0.0f, -420.0f, 0.0f},
     "rain",
     "Rain"},
    {WE_PARTICLE_PRESET_SNOW,
     900u,
     WE_RENDERER_SPRITE,
     0u,
     8.0f,
     {12.0f, -18.0f, 0.0f},
     "snow",
     "Snow"},
    {WE_PARTICLE_PRESET_EMBERS,
     450u,
     WE_RENDERER_SPRITETRAIL,
     1u,
     4.0f,
     {0.0f, 85.0f, 0.0f},
     "embers",
     "Embers"},
    {WE_PARTICLE_PRESET_FIREFLIES,
     140u,
     WE_RENDERER_SPRITE,
     1u,
     5.0f,
     {0.0f, 0.0f, 0.0f},
     "fireflies",
     "Fireflies"},
    {WE_PARTICLE_PRESET_CHERRY_BLOSSOMS,
     320u,
     WE_RENDERER_SPRITE,
     0u,
     7.0f,
     {-28.0f, -12.0f, 0.0f},
     "cherry_blossoms",
     "Cherry Blossoms"},
    {WE_PARTICLE_PRESET_STARFIELD,
     600u,
     WE_RENDERER_SPRITE,
     1u,
     6.0f,
     {0.0f, 0.0f, 0.0f},
     "starfield",
     "Starfield"},
    {WE_PARTICLE_PRESET_RISING_SMOKE,
     180u,
     WE_RENDERER_SPRITE,
     0u,
     6.0f,
     {8.0f, 18.0f, 0.0f},
     "rising_smoke",
     "Rising Smoke"},
    {WE_PARTICLE_PRESET_MAGIC_DUST,
     800u,
     WE_RENDERER_SPRITE,
     1u,
     4.0f,
     {0.0f, 8.0f, 0.0f},
     "magic_dust",
     "Magic Dust"},
};

const we_particle_preset_desc_t *we_particle_get_preset_desc(we_particle_preset_id_t preset_id)
{
    uint32_t idx = (uint32_t)preset_id;
    if (idx >= WE_PARTICLE_PRESET_COUNT) {
        return NULL;
    }
    return &g_particle_presets[idx];
}

static we_particle_sys_t *particle_alloc_empty(uint32_t max_count)
{
    we_particle_sys_t *sys = calloc(1, sizeof(we_particle_sys_t));
    if (!sys) {
        return NULL;
    }
    if (max_count == 0u) {
        max_count = WE_PARTICLE_DEFAULT_CAP;
    }
    if (max_count > WE_PARTICLE_MAX_CAP) {
        max_count = WE_PARTICLE_MAX_CAP;
    }
    sys->rng_state = 0x853C49E6748FEA9BULL;
    sys->max_count = max_count;
    sys->padded_cap = (max_count + 7u) & ~7u;

    size_t total_floats = (size_t)WE_PARTICLE_SOA_ARRAYS * (size_t)sys->padded_cap;
    size_t alloc_bytes = (total_floats * sizeof(float) + 31u) & ~(size_t)31u;
    sys->soa_block = (float *)aligned_alloc(32, alloc_bytes);
    if (!sys->soa_block) {
        free(sys);
        return NULL;
    }
    memset(sys->soa_block, 0, alloc_bytes);
    bind_soa_pointers(sys);

    sys->quad_cap = sys->padded_cap;
    sys->gl_gpu_ops_dirty = true;
    sys->gl_gpu_state_dirty = true;

    for (uint32_t c = 0; c < WE_PARTICLE_MAX_CONTROL_PTS; c++) {
        sys->control_points[c].id = (int32_t)c;
    }
    return sys;
}

static we_particle_initializer_t *preset_add_init(we_particle_sys_t *sys, we_initializer_type_t t)
{
    if (sys->initializer_count >= WE_PARTICLE_MAX_INITIALIZERS) {
        return NULL;
    }
    we_particle_initializer_t *iz = &sys->initializers[sys->initializer_count++];
    memset(iz, 0, sizeof(*iz));
    iz->type = t;
    iz->exponent = 1.0f;
    return iz;
}

static we_particle_operator_t *preset_add_op(we_particle_sys_t *sys, we_operator_type_t t)
{
    if (sys->operator_count >= WE_PARTICLE_MAX_OPERATORS) {
        return NULL;
    }
    we_particle_operator_t *op = &sys->operators[sys->operator_count++];
    memset(op, 0, sizeof(*op));
    op->type = t;
    op->end_time = 1.0f;
    return op;
}

static inline void preset_set_gravity(we_particle_operator_t *op,
                                      const we_particle_preset_desc_t *desc)
{
    if (op && desc) {
        op->gravity[0] = desc->gravity[0];
        op->gravity[1] = desc->gravity[1];
        op->gravity[2] = desc->gravity[2];
    }
}

static bool build_preset_rain(we_particle_sys_t *sys, we_particle_emitter_t *em,
                              const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_rain");
    sys->trail_length = 0.045f;
    sys->trail_min_length = 2.0f;
    sys->trail_max_length = 28.0f;
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 1140.0f;
    em->distance_min[0] = -1100.0f;
    em->distance_min[1] = -40.0f;
    em->distance_max[0] = 1100.0f;
    em->distance_max[1] = 40.0f;
    em->rate = 950.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 1.1f;
    iz->max_val[0] = 1.6f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 3.0f;
    iz->max_val[0] = 6.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.35f;
    iz->max_val[0] = 0.72f;

    iz = preset_add_init(sys, WE_INIT_COLOR_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.72f;
    iz->min_val[1] = 0.85f;
    iz->min_val[2] = 1.0f;
    iz->max_val[0] = 0.88f;
    iz->max_val[1] = 0.95f;
    iz->max_val[2] = 1.0f;

    iz = preset_add_init(sys, WE_INIT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = -180.0f;
    iz->min_val[1] = -1350.0f;
    iz->max_val[0] = -110.0f;
    iz->max_val[1] = -1050.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.05f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.05f;
    op->fade_out_time = 0.85f;
    return true;
}

static bool build_preset_snow(we_particle_sys_t *sys, we_particle_emitter_t *em,
                              const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_soft");
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 1120.0f;
    em->distance_min[0] = -1050.0f;
    em->distance_min[1] = -30.0f;
    em->distance_max[0] = 1050.0f;
    em->distance_max[1] = 30.0f;
    em->rate = 110.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 6.5f;
    iz->max_val[0] = 9.5f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 4.0f;
    iz->max_val[0] = 14.0f;
    iz->exponent = 2.2f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.45f;
    iz->max_val[0] = 0.90f;

    iz = preset_add_init(sys, WE_INIT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = -25.0f;
    iz->min_val[1] = -140.0f;
    iz->max_val[0] = 45.0f;
    iz->max_val[1] = -70.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.08f;

    op = preset_add_op(sys, WE_OP_TURBULENCE);
    if (!op) {
        return false;
    }
    op->turb_scale = 0.008f;
    op->turb_speed = 38.0f;
    op->turb_timescale = 0.6f;
    op->mask[0] = 1.0f;
    op->mask[1] = 1.0f;

    op = preset_add_op(sys, WE_OP_OSCILLATE_POSITION);
    if (!op) {
        return false;
    }
    op->freq_min = 0.8f;
    op->freq_max = 1.8f;
    op->scale_min = 10.0f;
    op->scale_max = 24.0f;
    op->mask[0] = 1.0f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.10f;
    op->fade_out_time = 0.80f;
    return true;
}

static bool build_preset_embers(we_particle_sys_t *sys, we_particle_emitter_t *em,
                                const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_glow");
    sys->trail_length = 0.025f;
    sys->trail_min_length = 1.0f;
    sys->trail_max_length = 6.0f;
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 40.0f;
    em->distance_min[0] = -960.0f;
    em->distance_min[1] = -30.0f;
    em->distance_max[0] = 960.0f;
    em->distance_max[1] = 30.0f;
    em->rate = 90.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 2.5f;
    iz->max_val[0] = 4.8f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 4.0f;
    iz->max_val[0] = 10.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.70f;
    iz->max_val[0] = 1.0f;

    iz = preset_add_init(sys, WE_INIT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = -45.0f;
    iz->min_val[1] = 120.0f;
    iz->max_val[0] = 45.0f;
    iz->max_val[1] = 260.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.15f;

    op = preset_add_op(sys, WE_OP_TURBULENCE);
    if (!op) {
        return false;
    }
    op->turb_scale = 0.012f;
    op->turb_speed = 65.0f;
    op->turb_timescale = 1.0f;
    op->mask[0] = 1.0f;
    op->mask[1] = 1.0f;

    op = preset_add_op(sys, WE_OP_COLOR_CHANGE);
    if (!op) {
        return false;
    }
    const we_particle_color_stop_t stops[3] = {
        {0.0f, {1.0f, 0.75f, 0.23f}},
        {0.5f, {0.98f, 0.45f, 0.09f}},
        {1.0f, {0.60f, 0.11f, 0.11f}},
    };
    op->ramp_lut.operator_index = (uint32_t)(sys->operator_count - 1u);
    op->ramp_lut.channel_kind = 2u;
    op->ramp_lut.knot_count = 3u;
    if (!we_particle_bake_oklab_lut(stops, 3u, op->ramp_lut.color_lut_r, op->ramp_lut.color_lut_g,
                                    op->ramp_lut.color_lut_b)) {
        return false;
    }
    op->use_ramp_lut = true;

    op = preset_add_op(sys, WE_OP_OSCILLATE_ALPHA);
    if (!op) {
        return false;
    }
    op->freq_min = 6.0f;
    op->freq_max = 12.0f;
    op->scale_min = 0.50f;
    op->scale_max = 1.0f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.08f;
    op->fade_out_time = 0.72f;
    return true;
}

static bool build_preset_fireflies(we_particle_sys_t *sys, we_particle_emitter_t *em)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_glow");
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 460.0f;
    em->distance_min[0] = -900.0f;
    em->distance_min[1] = -360.0f;
    em->distance_max[0] = 900.0f;
    em->distance_max[1] = 360.0f;
    em->rate = 22.0f;
    em->instantaneous = 25u;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 4.0f;
    iz->max_val[0] = 7.0f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 8.0f;
    iz->max_val[0] = 18.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.55f;
    iz->max_val[0] = 0.95f;

    iz = preset_add_init(sys, WE_INIT_COLOR_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.75f;
    iz->min_val[1] = 1.0f;
    iz->min_val[2] = 0.35f;
    iz->max_val[0] = 0.95f;
    iz->max_val[1] = 1.0f;
    iz->max_val[2] = 0.55f;

    iz = preset_add_init(sys, WE_INIT_TURBULENT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 18.0f;
    iz->max_val[0] = 55.0f;
    iz->scale = 0.01f;
    iz->forward[0] = 1.0f;
    iz->forward[1] = 1.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    op->drag = 0.25f;

    op = preset_add_op(sys, WE_OP_TURBULENCE);
    if (!op) {
        return false;
    }
    op->turb_scale = 0.01f;
    op->turb_speed = 32.0f;
    op->turb_timescale = 0.8f;
    op->mask[0] = 1.0f;
    op->mask[1] = 1.0f;

    op = preset_add_op(sys, WE_OP_OSCILLATE_ALPHA);
    if (!op) {
        return false;
    }
    op->freq_min = 1.6f;
    op->freq_max = 3.2f;
    op->scale_min = 0.15f;
    op->scale_max = 1.0f;

    op = preset_add_op(sys, WE_OP_OSCILLATE_SIZE);
    if (!op) {
        return false;
    }
    op->freq_min = 1.6f;
    op->freq_max = 3.2f;
    op->scale_min = 0.65f;
    op->scale_max = 1.25f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.15f;
    op->fade_out_time = 0.80f;
    return true;
}

static bool build_preset_cherry_blossoms(we_particle_sys_t *sys, we_particle_emitter_t *em,
                                         const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_petal");
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 1080.0f;
    em->origin[1] = 1120.0f;
    em->distance_min[0] = -1100.0f;
    em->distance_min[1] = -40.0f;
    em->distance_max[0] = 1000.0f;
    em->distance_max[1] = 40.0f;
    em->rate = 40.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 6.0f;
    iz->max_val[0] = 9.0f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 12.0f;
    iz->max_val[0] = 24.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.75f;
    iz->max_val[0] = 0.98f;

    iz = preset_add_init(sys, WE_INIT_COLOR_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.98f;
    iz->min_val[1] = 0.72f;
    iz->min_val[2] = 0.82f;
    iz->max_val[0] = 1.0f;
    iz->max_val[1] = 0.88f;
    iz->max_val[2] = 0.92f;

    iz = preset_add_init(sys, WE_INIT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = -120.0f;
    iz->min_val[1] = -140.0f;
    iz->max_val[0] = -40.0f;
    iz->max_val[1] = -80.0f;

    iz = preset_add_init(sys, WE_INIT_ROTATION_RANDOM);
    if (!iz) {
        return false;
    }
    iz->max_val[2] = 6.2831853f;

    iz = preset_add_init(sys, WE_INIT_ANGULAR_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[2] = -2.4f;
    iz->max_val[2] = 2.4f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.12f;

    if (!preset_add_op(sys, WE_OP_ANGULAR_MOVEMENT)) {
        return false;
    }

    op = preset_add_op(sys, WE_OP_OSCILLATE_POSITION);
    if (!op) {
        return false;
    }
    op->freq_min = 1.2f;
    op->freq_max = 2.5f;
    op->scale_min = 14.0f;
    op->scale_max = 32.0f;
    op->mask[0] = 1.0f;
    op->mask[1] = 0.4f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.08f;
    op->fade_out_time = 0.85f;
    return true;
}

static bool build_preset_starfield(we_particle_sys_t *sys, we_particle_emitter_t *em)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_glow");
    em->type = WE_EMITTER_SPHERERANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 540.0f;
    em->distance_min[0] = 18.0f;
    em->distance_min[1] = 18.0f;
    em->distance_max[0] = 180.0f;
    em->distance_max[1] = 180.0f;
    em->directions[0] = 1.0f;
    em->directions[1] = 0.5625f;
    em->directions[2] = 0.0f;
    em->speed_min = 45.0f;
    em->speed_max = 130.0f;
    em->rate = 110.0f;
    em->instantaneous = 80u;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 3.5f;
    iz->max_val[0] = 5.5f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 3.0f;
    iz->max_val[0] = 9.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.65f;
    iz->max_val[0] = 1.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    op->drag = -0.35f;

    op = preset_add_op(sys, WE_OP_SIZE_CHANGE);
    if (!op) {
        return false;
    }
    op->start_val[0] = 0.25f;
    op->end_val[0] = 1.65f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.12f;
    op->fade_out_time = 0.85f;
    return true;
}

static bool build_preset_rising_smoke(we_particle_sys_t *sys, we_particle_emitter_t *em,
                                      const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_smoke");
    em->type = WE_EMITTER_BOXRANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 80.0f;
    em->distance_min[0] = -720.0f;
    em->distance_min[1] = -25.0f;
    em->distance_max[0] = 720.0f;
    em->distance_max[1] = 25.0f;
    em->rate = 24.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 5.0f;
    iz->max_val[0] = 7.5f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 90.0f;
    iz->max_val[0] = 140.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.14f;
    iz->max_val[0] = 0.28f;

    iz = preset_add_init(sys, WE_INIT_VELOCITY_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = -18.0f;
    iz->min_val[1] = 70.0f;
    iz->max_val[0] = 28.0f;
    iz->max_val[1] = 120.0f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.18f;

    op = preset_add_op(sys, WE_OP_SIZE_CHANGE);
    if (!op) {
        return false;
    }
    op->start_val[0] = 0.65f;
    op->end_val[0] = 2.60f;

    op = preset_add_op(sys, WE_OP_TURBULENCE);
    if (!op) {
        return false;
    }
    op->turb_scale = 0.006f;
    op->turb_speed = 22.0f;
    op->turb_timescale = 0.5f;
    op->mask[0] = 1.0f;
    op->mask[1] = 1.0f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.18f;
    op->fade_out_time = 0.65f;
    return true;
}

static bool build_preset_magic_dust(we_particle_sys_t *sys, we_particle_emitter_t *em,
                                    const we_particle_preset_desc_t *desc)
{
    snprintf(sys->material_path, sizeof(sys->material_path), "%s", "util/particle_glow");
    sys->control_points[0].lock_to_pointer = true;
    sys->control_points[0].pos[0] = 960.0f;
    sys->control_points[0].pos[1] = 540.0f;
    em->type = WE_EMITTER_SPHERERANDOM;
    em->origin[0] = 960.0f;
    em->origin[1] = 540.0f;
    em->distance_min[0] = 40.0f;
    em->distance_min[1] = 40.0f;
    em->distance_max[0] = 580.0f;
    em->distance_max[1] = 580.0f;
    em->rate = 160.0f;

    we_particle_initializer_t *iz = preset_add_init(sys, WE_INIT_LIFETIME_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 3.0f;
    iz->max_val[0] = 5.0f;

    iz = preset_add_init(sys, WE_INIT_SIZE_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 4.0f;
    iz->max_val[0] = 12.0f;

    iz = preset_add_init(sys, WE_INIT_ALPHA_RANDOM);
    if (!iz) {
        return false;
    }
    iz->min_val[0] = 0.55f;
    iz->max_val[0] = 0.95f;

    we_particle_operator_t *op = preset_add_op(sys, WE_OP_MOVEMENT);
    if (!op) {
        return false;
    }
    preset_set_gravity(op, desc);
    op->drag = 0.35f;

    op = preset_add_op(sys, WE_OP_VORTEX);
    if (!op) {
        return false;
    }
    op->axis[2] = 1.0f;
    op->distance_inner = 30.0f;
    op->distance_outer = 550.0f;
    op->speed_inner = 140.0f;
    op->speed_outer = 35.0f;
    op->center_force = 15.0f;

    op = preset_add_op(sys, WE_OP_CONTROL_POINT_ATTRACT);
    if (!op) {
        return false;
    }
    op->distance_inner = 20.0f;
    op->distance_outer = 600.0f;
    op->center_force = 95.0f;

    op = preset_add_op(sys, WE_OP_TURBULENCE);
    if (!op) {
        return false;
    }
    op->turb_scale = 0.01f;
    op->turb_speed = 45.0f;
    op->turb_timescale = 1.0f;
    op->mask[0] = 1.0f;
    op->mask[1] = 1.0f;

    op = preset_add_op(sys, WE_OP_COLOR_CHANGE);
    if (!op) {
        return false;
    }
    const we_particle_color_stop_t stops[3] = {
        {0.0f, {0.220f, 0.741f, 0.973f}},
        {0.5f, {0.753f, 0.518f, 0.988f}},
        {1.0f, {0.957f, 0.247f, 0.369f}},
    };
    op->ramp_lut.operator_index = (uint32_t)(sys->operator_count - 1u);
    op->ramp_lut.channel_kind = 2u;
    op->ramp_lut.knot_count = 3u;
    if (!we_particle_bake_oklab_lut(stops, 3u, op->ramp_lut.color_lut_r, op->ramp_lut.color_lut_g,
                                    op->ramp_lut.color_lut_b)) {
        return false;
    }
    op->use_ramp_lut = true;

    op = preset_add_op(sys, WE_OP_OSCILLATE_ALPHA);
    if (!op) {
        return false;
    }
    op->freq_min = 3.0f;
    op->freq_max = 7.0f;
    op->scale_min = 0.35f;
    op->scale_max = 1.0f;

    op = preset_add_op(sys, WE_OP_ALPHA_FADE);
    if (!op) {
        return false;
    }
    op->fade_in_time = 0.12f;
    op->fade_out_time = 0.78f;
    return true;
}

we_particle_sys_t *we_particle_create_preset(we_particle_preset_id_t preset_id, bool prewarm)
{
    const we_particle_preset_desc_t *desc = we_particle_get_preset_desc(preset_id);
    if (!desc) {
        return NULL;
    }
    we_particle_sys_t *sys = particle_alloc_empty(desc->max_count);
    if (!sys) {
        return NULL;
    }

    sys->start_time = desc->start_time_sec;
    sys->renderer_type = (we_renderer_type_t)desc->renderer_type;
    sys->preset_blending = desc->blending;
    sys->trail_length = 0.05f;
    sys->trail_min_length = 1.0f;
    sys->trail_max_length = 100.0f;
    sys->subdivision = 1u;

    we_particle_emitter_t *em = &sys->emitters[0];
    sys->emitter_count = 1u;
    memset(em, 0, sizeof(*em));
    em->directions[0] = 1.0f;
    em->directions[1] = 1.0f;
    em->directions[2] = 1.0f;

    bool ok = false;
    switch (preset_id) {
    case WE_PARTICLE_PRESET_RAIN:
        ok = build_preset_rain(sys, em, desc);
        break;
    case WE_PARTICLE_PRESET_SNOW:
        ok = build_preset_snow(sys, em, desc);
        break;
    case WE_PARTICLE_PRESET_EMBERS:
        ok = build_preset_embers(sys, em, desc);
        break;
    case WE_PARTICLE_PRESET_FIREFLIES:
        ok = build_preset_fireflies(sys, em);
        break;
    case WE_PARTICLE_PRESET_CHERRY_BLOSSOMS:
        ok = build_preset_cherry_blossoms(sys, em, desc);
        break;
    case WE_PARTICLE_PRESET_STARFIELD:
        ok = build_preset_starfield(sys, em);
        break;
    case WE_PARTICLE_PRESET_RISING_SMOKE:
        ok = build_preset_rising_smoke(sys, em, desc);
        break;
    case WE_PARTICLE_PRESET_MAGIC_DUST:
        ok = build_preset_magic_dust(sys, em, desc);
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        we_particle_destroy(sys);
        return NULL;
    }

    if (prewarm && sys->start_time > 0.0f) {
        we_particle_reset_and_prewarm(sys);
    }
    return sys;
}

void we_particle_trigger_burst(we_particle_sys_t *sys, uint32_t count)
{
    if (!sys || count == 0u) {
        return;
    }
    if (sys->gl_gpu_sim_active) {
        uint64_t next_b = (uint64_t)sys->gl_pending_burst + (uint64_t)count;
        sys->gl_pending_burst =
            (next_b > (uint64_t)sys->max_count) ? sys->max_count : (uint32_t)next_b;
        return;
    }
    we_particle_emitter_t fallback;
    const we_particle_emitter_t *em = NULL;
    if (sys->emitter_count > 0u) {
        em = &sys->emitters[0];
    } else {
        memset(&fallback, 0, sizeof(fallback));
        fallback.type = WE_EMITTER_POINT;
        fallback.origin[0] = 960.0f;
        fallback.origin[1] = 540.0f;
        em = &fallback;
    }
    for (uint32_t i = 0; i < count && sys->alive_count < sys->max_count; i++) {
        spawn_single_particle(sys, em);
    }
    sys->gl_gpu_state_dirty = true;
}
