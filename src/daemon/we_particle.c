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

static void pj_vec3(const pj_val_t *v, float out[3], float d0, float d1, float d2)
{
    out[0] = d0;
    out[1] = d1;
    out[2] = d2;
    uint32_t n = pj_floats(v, out, 3);
    if (n == 1) {
        out[1] = out[0];
        out[2] = out[0];
    }
}

static void normalize_color_vec3(float c[3])
{
    if (c[0] > 1.001f || c[1] > 1.001f || c[2] > 1.001f) {
        c[0] /= 255.0f;
        c[1] /= 255.0f;
        c[2] /= 255.0f;
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

    we_particle_vertex_t *new_vbuf = calloc((size_t)new_padded * 4u, sizeof(we_particle_vertex_t));
    if (!new_vbuf) {
        free(new_soa);
        return false;
    }

    if (sys->soa_block && sys->alive_count > 0u) {
        uint32_t copy_n = sys->alive_count <= sys->padded_cap ? sys->alive_count : sys->padded_cap;
        for (uint32_t a = 0; a < WE_PARTICLE_SOA_ARRAYS; a++) {
            memcpy(new_soa + (size_t)a * new_padded, sys->soa_block + (size_t)a * sys->padded_cap,
                   copy_n * sizeof(float));
        }
    }

    free(sys->soa_block);
    free(sys->vert_buf);
    sys->soa_block = new_soa;
    sys->vert_buf = new_vbuf;
    sys->max_count = new_max_count;
    sys->padded_cap = new_padded;
    sys->quad_cap = new_padded;
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

static void parse_json_renderer(we_particle_sys_t *sys, const pj_val_t *root)
{
    sys->renderer_type = WE_RENDERER_SPRITE;
    sys->trail_length = 0.05f;
    sys->trail_min_length = 1.0f;
    sys->trail_max_length = 100.0f;
    const pj_val_t *rends = pj_get(root, "renderer");
    if (!rends || rends->type != PJ_ARR || rends->arr.count == 0) {
        return;
    }
    const pj_val_t *rn = rends->arr.items[0];
    const char *rname = pj_str(pj_get(rn, "name"), "sprite");
    if (strcasecmp(rname, "spritetrail") == 0) {
        sys->renderer_type = WE_RENDERER_SPRITETRAIL;
    } else if (strcasecmp(rname, "ropetrail") == 0 || strcasecmp(rname, "rope") == 0) {
        sys->renderer_type = WE_RENDERER_ROPETRAIL;
    } else {
        sys->renderer_type = WE_RENDERER_SPRITE;
    }
    sys->trail_length = pj_float(pj_get(rn, "length"), 0.05f);
    sys->trail_min_length = pj_float(pj_get(rn, "minlength"), 1.0f);
    sys->trail_max_length = pj_float(pj_get(rn, "maxlength"), 100.0f);
    sys->subdivision = safe_f32_to_u32(pj_float(pj_get(rn, "subdivision"), 1.0f), UINT32_MAX);
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
        int32_t cid = safe_f32_to_i32(pj_float(pj_get(cn, "id"), (float)i), (int32_t)i);
        if (cid < 0 || cid >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
            cid = (int32_t)i;
        }
        we_particle_cp_t *cp = &sys->control_points[cid];
        cp->id = cid;
        pj_vec3(pj_get(cn, "offset"), cp->offset, 0.0f, 0.0f, 0.0f);
        memcpy(cp->pos, cp->offset, sizeof(cp->pos));
        int32_t cp_flags = safe_f32_to_i32(pj_float(pj_get(cn, "flags"), 0.0f), 0);
        cp->lock_to_pointer = (cp_flags & 1) != 0 ||
                              (pj_float(pj_get(cn, "locktopointer"), 0.0f) != 0.0f) ||
                              (pj_float(pj_get(cn, "link_mouse"), 0.0f) != 0.0f);
        cp->world_space = (cp_flags & 2) != 0 || (pj_float(pj_get(cn, "worldspace"), 0.0f) != 0.0f);
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
        pj_vec3(pj_get(en, "directions"), em->directions, 1.0f, 1.0f, 1.0f);
        pj_vec3(pj_get(en, "origin"), em->origin, 0.0f, 0.0f, 0.0f);
        float base_speed = pj_float(pj_get(en, "speed"), 0.0f);
        const pj_val_t *esmin = pj_get(en, "speedmin");
        if (!esmin) {
            esmin = pj_get(en, "minspeed");
        }
        const pj_val_t *esmax = pj_get(en, "speedmax");
        if (!esmax) {
            esmax = pj_get(en, "maxspeed");
        }
        em->speed_min = pj_float(esmin, base_speed);
        em->speed_max = pj_float(esmax, em->speed_min > base_speed ? em->speed_min : base_speed);
        em->rate = pj_float(pj_get(en, "rate"), 10.0f);
        em->instantaneous =
            safe_f32_to_u32(pj_float(pj_get(en, "instantaneous"), 0.0f), WE_PARTICLE_MAX_CAP);
        em->control_point = safe_f32_to_i32(pj_float(pj_get(en, "controlpoint"), 0.0f), 0);
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
        iz->exponent = pj_float(pj_get(in, "exponent"), 1.0f);
        if (!isfinite(iz->exponent) || iz->exponent <= 0.0f) {
            iz->exponent = 1.0f;
        }

        if (strcasecmp(iname, "lifetimerandom") == 0) {
            iz->type = WE_INIT_LIFETIME_RANDOM;
            iz->min_val[0] = pj_float(pj_get(in, "min"), 1.0f);
            iz->max_val[0] = pj_float(pj_get(in, "max"), 2.0f);
        } else if (strcasecmp(iname, "sizerandom") == 0) {
            iz->type = WE_INIT_SIZE_RANDOM;
            iz->min_val[0] = pj_float(pj_get(in, "min"), 10.0f);
            iz->max_val[0] = pj_float(pj_get(in, "max"), 20.0f);
        } else if (strcasecmp(iname, "alpharandom") == 0) {
            iz->type = WE_INIT_ALPHA_RANDOM;
            iz->min_val[0] = pj_float(pj_get(in, "min"), 0.5f);
            iz->max_val[0] = pj_float(pj_get(in, "max"), 1.0f);
        } else if (strcasecmp(iname, "colorrandom") == 0) {
            iz->type = WE_INIT_COLOR_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 1.0f, 1.0f, 1.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 1.0f, 1.0f, 1.0f);
            normalize_color_vec3(iz->min_val);
            normalize_color_vec3(iz->max_val);
        } else if (strcasecmp(iname, "velocityrandom") == 0) {
            iz->type = WE_INIT_VELOCITY_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, -10.0f, -10.0f, 0.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 10.0f, 10.0f, 0.0f);
        } else if (strcasecmp(iname, "rotationrandom") == 0) {
            iz->type = WE_INIT_ROTATION_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 0.0f, 0.0f, 0.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 0.0f, 0.0f, 6.2831853f);
        } else if (strcasecmp(iname, "angularvelocityrandom") == 0) {
            iz->type = WE_INIT_ANGULAR_VELOCITY_RANDOM;
            pj_vec3(pj_get(in, "min"), iz->min_val, 0.0f, 0.0f, -1.0f);
            pj_vec3(pj_get(in, "max"), iz->max_val, 0.0f, 0.0f, 1.0f);
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
            iz->min_val[0] = pj_float(tsmin, 10.0f);
            iz->max_val[0] = pj_float(tsmax, 50.0f);
            iz->scale = pj_float(pj_get(in, "scale"), 0.01f);
            iz->offset = pj_float(pj_get(in, "offset"), 0.0f);
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
            op->drag = pj_float(pj_get(on, "drag"), 0.0f);
        } else if (strcasecmp(oname, "angularmovement") == 0) {
            op->type = WE_OP_ANGULAR_MOVEMENT;
            op->drag = pj_float(pj_get(on, "drag"), 0.0f);
            pj_vec3(pj_get(on, "force"), op->gravity, 0.0f, 0.0f, 0.0f);
        } else if (strcasecmp(oname, "alphafade") == 0) {
            op->type = WE_OP_ALPHA_FADE;
            op->fade_in_time = pj_float(pj_get(on, "fadeintime"), 0.1f);
            op->fade_out_time = pj_float(pj_get(on, "fadeouttime"), 0.8f);
        } else if (strcasecmp(oname, "sizechange") == 0) {
            op->type = WE_OP_SIZE_CHANGE;
            op->start_time = pj_float(pj_get(on, "starttime"), 0.0f);
            op->end_time = pj_float(pj_get(on, "endtime"), 1.0f);
            op->start_val[0] = pj_float(pj_get(on, "startvalue"), 1.0f);
            op->end_val[0] = pj_float(pj_get(on, "endvalue"), 0.0f);
        } else if (strcasecmp(oname, "colorchange") == 0) {
            op->type = WE_OP_COLOR_CHANGE;
            op->start_time = pj_float(pj_get(on, "starttime"), 0.0f);
            op->end_time = pj_float(pj_get(on, "endtime"), 1.0f);
            pj_vec3(pj_get(on, "startvalue"), op->start_val, 1.0f, 1.0f, 1.0f);
            pj_vec3(pj_get(on, "endvalue"), op->end_val, 1.0f, 1.0f, 1.0f);
            normalize_color_vec3(op->start_val);
            normalize_color_vec3(op->end_val);
        } else if (strcasecmp(oname, "oscillatealpha") == 0) {
            op->type = WE_OP_OSCILLATE_ALPHA;
            op->freq_min = pj_float(pj_get(on, "frequencymin"), 1.0f);
            op->freq_max = pj_float(pj_get(on, "frequencymax"), 3.0f);
            op->scale_min = pj_float(pj_get(on, "scalemin"), 0.2f);
            op->scale_max = pj_float(pj_get(on, "scalemax"), 1.0f);
        } else if (strcasecmp(oname, "oscillatesize") == 0) {
            op->type = WE_OP_OSCILLATE_SIZE;
            op->freq_min = pj_float(pj_get(on, "frequencymin"), 1.0f);
            op->freq_max = pj_float(pj_get(on, "frequencymax"), 3.0f);
            op->scale_min = pj_float(pj_get(on, "scalemin"), 0.8f);
            op->scale_max = pj_float(pj_get(on, "scalemax"), 1.2f);
        } else if (strcasecmp(oname, "oscillateposition") == 0) {
            op->type = WE_OP_OSCILLATE_POSITION;
            op->freq_min = pj_float(pj_get(on, "frequencymin"), 1.0f);
            op->freq_max = pj_float(pj_get(on, "frequencymax"), 3.0f);
            op->scale_min = pj_float(pj_get(on, "scalemin"), 5.0f);
            op->scale_max = pj_float(pj_get(on, "scalemax"), 15.0f);
            pj_vec3(pj_get(on, "mask"), op->mask, 1.0f, 1.0f, 0.0f);
        } else if (strcasecmp(oname, "vortex") == 0) {
            op->type = WE_OP_VORTEX;
            op->control_point = safe_f32_to_i32(pj_float(pj_get(on, "controlpoint"), 0.0f), 0);
            pj_vec3(pj_get(on, "axis"), op->axis, 0.0f, 0.0f, 1.0f);
            float alen = sqrtf(op->axis[0] * op->axis[0] + op->axis[1] * op->axis[1] +
                               op->axis[2] * op->axis[2]);
            if (alen > 1e-5f) {
                op->axis[0] /= alen;
                op->axis[1] /= alen;
                op->axis[2] /= alen;
            } else {
                op->axis[0] = 0.0f;
                op->axis[1] = 0.0f;
                op->axis[2] = 1.0f;
            }
            pj_vec3(pj_get(on, "offset"), op->offset, 0.0f, 0.0f, 0.0f);
            op->distance_inner = pj_float(pj_get(on, "distanceinner"), 50.0f);
            op->distance_outer = pj_float(pj_get(on, "distanceouter"), 300.0f);
            op->speed_inner = pj_float(pj_get(on, "speedinner"), 200.0f);
            op->speed_outer = pj_float(pj_get(on, "speedouter"), 50.0f);
            op->center_force =
                pj_float(pj_get(on, "centerforce"), pj_float(pj_get(on, "scale"), 50.0f));
        } else if (strcasecmp(oname, "controlpointattract") == 0) {
            op->type = WE_OP_CONTROL_POINT_ATTRACT;
            op->control_point = safe_f32_to_i32(pj_float(pj_get(on, "controlpoint"), 0.0f), 0);
            pj_vec3(pj_get(on, "offset"), op->offset, 0.0f, 0.0f, 0.0f);
            op->distance_inner = pj_float(pj_get(on, "distanceinner"), 0.0f);
            op->distance_outer = pj_float(pj_get(on, "distanceouter"), 300.0f);
            op->center_force =
                pj_float(pj_get(on, "scale"), pj_float(pj_get(on, "centerforce"), 50.0f));
        } else if (strcasecmp(oname, "turbulence") == 0) {
            op->type = WE_OP_TURBULENCE;
            op->turb_scale = pj_float(pj_get(on, "scale"), 0.01f);
            const pj_val_t *tsmin = pj_get(on, "speedmin");
            if (!tsmin) {
                tsmin = pj_get(on, "minspeed");
            }
            const pj_val_t *tsmax = pj_get(on, "speedmax");
            if (!tsmax) {
                tsmax = pj_get(on, "maxspeed");
            }
            float sp_min = pj_float(tsmin, 50.0f);
            float sp_max = pj_float(tsmax, sp_min);
            op->turb_speed = pj_float(pj_get(on, "speed"), 0.5f * (sp_min + sp_max));
            op->turb_timescale = pj_float(pj_get(on, "timescale"), 1.0f);
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
        safe_f32_to_i32(pj_float(pj_get(root, "maxcount"), (float)WE_PARTICLE_DEFAULT_CAP),
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

    float raw_start = pj_float(pj_get(root, "starttime"), 0.0f);
    sys->start_time = isfinite(raw_start) ? raw_start : 0.0f;
    snprintf(sys->material_path, sizeof(sys->material_path), "%s",
             pj_str(pj_get(root, "material"), ""));

    int32_t flags = safe_f32_to_i32(pj_float(pj_get(root, "flags"), 0.0f), 0);
    sys->world_space = (flags & 1) != 0;
    sys->perspective = (flags & 4) != 0;

    parse_json_renderer(sys, root);
    parse_json_control_points(sys, root);
    parse_json_emitters(sys, root);
    parse_json_initializers(sys, root);
    parse_json_operators(sys, root);

    pj_free(root);

    sys->quad_cap = sys->padded_cap;
    sys->vert_buf = calloc((size_t)sys->quad_cap * 4u, sizeof(we_particle_vertex_t));
    if (!sys->vert_buf) {
        we_particle_destroy(sys);
        return NULL;
    }

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
    for (uint32_t i = 0; i < WE_PARTICLE_MAX_CONTROL_PTS; i++) {
        if (sys->control_points[i].lock_to_pointer) {
            sys->control_points[i].pos[0] = world_x + sys->control_points[i].offset[0];
            sys->control_points[i].pos[1] = world_y + sys->control_points[i].offset[1];
            sys->control_points[i].pos[2] = world_z + sys->control_points[i].offset[2];
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
            float dmin = em->distance_min[k];
            float dmax = em->distance_max[k];
            float off = 0.0f;
            if (fabsf(dmin) > 1e-5f && dmin <= dmax) {
                off = prng_range(rng, dmin, dmax) * em->directions[k];
            } else if (fabsf(dmin) <= 1e-5f) {
                off = prng_sym(rng) * dmax * em->directions[k];
            } else {
                float hi = dmax >= dmin ? dmax : dmin;
                float sign = (prng_u01(rng) >= 0.5f) ? 1.0f : -1.0f;
                float mag = prng_range(rng, dmin, hi);
                off = sign * mag * em->directions[k];
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
        float r_min = em->distance_min[0];
        float r_max = em->distance_max[0];
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
            float rspd = prng_range(rng, em->speed_min, em->speed_max);
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

static void op_movement_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t n,
                               float dt)
{
    float damp = expf(-op->drag * dt);
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
    float damp = expf(-op->drag * dt);
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

static void op_alpha_fade_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                 uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            sys->a[i] *= we_particle_eval_scalar_lut(op->ramp_lut.scalar_lut, t);
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
        sys->a[i] *= (fi * fo);
    }
}

static void op_size_change_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                  uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            sys->size[i] *= we_particle_eval_scalar_lut(op->ramp_lut.scalar_lut, t);
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
        sys->size[i] *= (sv + u * dv);
    }
}

static void op_color_change_scalar(we_particle_sys_t *sys, const we_particle_operator_t *op,
                                   uint32_t n)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < n; i++) {
            float mlife = fmaxf(sys->max_life[i], 1e-5f);
            float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
            sys->r[i] *= we_particle_eval_scalar_lut(op->ramp_lut.color_lut_r, t);
            sys->g[i] *= we_particle_eval_scalar_lut(op->ramp_lut.color_lut_g, t);
            sys->b[i] *= we_particle_eval_scalar_lut(op->ramp_lut.color_lut_b, t);
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
        sys->r[i] *= (op->start_val[0] + u * (op->end_val[0] - op->start_val[0]));
        sys->g[i] *= (op->start_val[1] + u * (op->end_val[1] - op->start_val[1]));
        sys->b[i] *= (op->start_val[2] + u * (op->end_val[2] - op->start_val[2]));
    }
}

static void op_oscillate_scalar_channel(float *dst, const float *age, const float *rng_phase,
                                        uint32_t n, const we_particle_operator_t *op)
{
    float freq = 0.5f * (op->freq_min + op->freq_max);
    float half_range = 0.5f * (op->scale_max - op->scale_min);
    float base = op->scale_min + half_range;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] *= (base + sinf(age[i] * freq + rng_phase[i]) * half_range);
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

static void particle_reset_and_ops_scalar(we_particle_sys_t *sys, float dt)
{
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        return;
    }

    /* Reset modulated channels to spawn baselines so operators do not compound across ticks. */
    memcpy(sys->size, sys->init_size, n * sizeof(float));
    memcpy(sys->a, sys->init_a, n * sizeof(float));
    memcpy(sys->r, sys->init_r, n * sizeof(float));
    memcpy(sys->g, sys->init_g, n * sizeof(float));
    memcpy(sys->b, sys->init_b, n * sizeof(float));

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
            op_alpha_fade_scalar(sys, op, n);
            break;
        case WE_OP_SIZE_CHANGE:
            op_size_change_scalar(sys, op, n);
            break;
        case WE_OP_COLOR_CHANGE:
            op_color_change_scalar(sys, op, n);
            break;
        case WE_OP_OSCILLATE_ALPHA:
            op_oscillate_scalar_channel(sys->a, sys->age, sys->rng_phase, n, op);
            break;
        case WE_OP_OSCILLATE_SIZE:
            op_oscillate_scalar_channel(sys->size, sys->age, sys->rng_phase, n, op);
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

    for (uint32_t i = 0; i < n; i++) {
        sys->age[i] += dt;
    }
}

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx2,fma"))) static inline __m256 vsin256_ps(__m256 x)
{
    const __m256 v_inv_2pi = _mm256_set1_ps(0.15915494309189535f);
    const __m256 v_2pi = _mm256_set1_ps(6.28318530717958647692f);
    const __m256 v_pi = _mm256_set1_ps(3.14159265358979323846f);
    const __m256 v_neg_pi = _mm256_set1_ps(-3.14159265358979323846f);
    const __m256 v_half_pi = _mm256_set1_ps(1.57079632679489661923f);
    const __m256 v_neg_half_pi = _mm256_set1_ps(-1.57079632679489661923f);

    __m256 k =
        _mm256_round_ps(_mm256_mul_ps(x, v_inv_2pi), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 r = _mm256_fnmadd_ps(k, v_2pi, x);

    __m256 gt_half = _mm256_cmp_ps(r, v_half_pi, _CMP_GT_OQ);
    r = _mm256_blendv_ps(r, _mm256_sub_ps(v_pi, r), gt_half);
    __m256 lt_neg_half = _mm256_cmp_ps(r, v_neg_half_pi, _CMP_LT_OQ);
    r = _mm256_blendv_ps(r, _mm256_sub_ps(v_neg_pi, r), lt_neg_half);

    const __m256 c3 = _mm256_set1_ps(-0.16666667f);
    const __m256 c5 = _mm256_set1_ps(0.0083333310f);
    const __m256 c7 = _mm256_set1_ps(-0.00019840874f);

    __m256 r2 = _mm256_mul_ps(r, r);
    __m256 poly = _mm256_fmadd_ps(c7, r2, c5);
    poly = _mm256_fmadd_ps(poly, r2, c3);
    return _mm256_fmadd_ps(poly, _mm256_mul_ps(r2, r), r);
}

__attribute__((target("avx2,fma"))) static inline __m256 vcos256_ps(__m256 x)
{
    return vsin256_ps(_mm256_add_ps(x, _mm256_set1_ps(1.57079632679489661923f)));
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
    __m256 v_damp = _mm256_set1_ps(expf(-op->drag * dt));
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
    __m256 v_damp = _mm256_set1_ps(expf(-op->drag * dt));
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

__attribute__((target("avx2,fma"))) static void op_alpha_fade_avx2(we_particle_sys_t *sys,
                                                                   const we_particle_operator_t *op,
                                                                   uint32_t vec_n, __m256 v_zero,
                                                                   __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 val = v_eval_lut65_avx2(op->ramp_lut.scalar_lut, t);
            _mm256_store_ps(sys->a + i, _mm256_mul_ps(_mm256_load_ps(sys->a + i), val));
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
        __m256 a = _mm256_load_ps(sys->a + i);
        _mm256_store_ps(sys->a + i, _mm256_mul_ps(a, _mm256_mul_ps(fi, fo)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_size_change_avx2(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t vec_n,
                    __m256 v_zero, __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 val = v_eval_lut65_avx2(op->ramp_lut.scalar_lut, t);
            _mm256_store_ps(sys->size + i, _mm256_mul_ps(_mm256_load_ps(sys->size + i), val));
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
        _mm256_store_ps(sys->size + i, _mm256_mul_ps(_mm256_load_ps(sys->size + i), mult));
    }
}

__attribute__((target("avx2,fma"))) static void
op_color_change_avx2(we_particle_sys_t *sys, const we_particle_operator_t *op, uint32_t vec_n,
                     __m256 v_zero, __m256 v_one)
{
    if (op->use_ramp_lut) {
        for (uint32_t i = 0; i < vec_n; i += 8u) {
            __m256 age = _mm256_load_ps(sys->age + i);
            __m256 mlife = _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
            __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
            __m256 vr = v_eval_lut65_avx2(op->ramp_lut.color_lut_r, t);
            __m256 vg = v_eval_lut65_avx2(op->ramp_lut.color_lut_g, t);
            __m256 vb = v_eval_lut65_avx2(op->ramp_lut.color_lut_b, t);
            _mm256_store_ps(sys->r + i, _mm256_mul_ps(_mm256_load_ps(sys->r + i), vr));
            _mm256_store_ps(sys->g + i, _mm256_mul_ps(_mm256_load_ps(sys->g + i), vg));
            _mm256_store_ps(sys->b + i, _mm256_mul_ps(_mm256_load_ps(sys->b + i), vb));
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
        _mm256_store_ps(sys->r + i,
                        _mm256_mul_ps(_mm256_load_ps(sys->r + i), _mm256_fmadd_ps(u, dr, sr)));
        _mm256_store_ps(sys->g + i,
                        _mm256_mul_ps(_mm256_load_ps(sys->g + i), _mm256_fmadd_ps(u, dg, sg)));
        _mm256_store_ps(sys->b + i,
                        _mm256_mul_ps(_mm256_load_ps(sys->b + i), _mm256_fmadd_ps(u, db, sb)));
    }
}

__attribute__((target("avx2,fma"))) static void
op_oscillate_avx2_channel(float *dst, const float *age, const float *rng_phase, uint32_t vec_n,
                          const we_particle_operator_t *op)
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
        _mm256_store_ps(dst + i, _mm256_mul_ps(_mm256_load_ps(dst + i), scale));
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

        __m256 sx = vsin256_ps(x);
        __m256 cx = vcos256_ps(x);
        __m256 sy = vsin256_ps(y);
        __m256 cy = vcos256_ps(y);
        __m256 sz = vsin256_ps(z);
        __m256 cz = vcos256_ps(z);

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

__attribute__((target("avx2,fma"))) static void particle_reset_and_ops_avx2(we_particle_sys_t *sys,
                                                                            float dt)
{
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        return;
    }
    uint32_t vec_n = (n + 7u) & ~7u;
    if (vec_n > sys->padded_cap) {
        vec_n = sys->padded_cap;
    }

    for (uint32_t tail = n; tail < vec_n; tail++) {
        sys->age[tail] = 0.0f;
        sys->max_life[tail] = 1.0f;
    }

    /* Reset modulated channels across 32B-aligned SoA arrays before evaluating operators. */
    for (uint32_t i = 0; i < vec_n; i += 8u) {
        _mm256_store_ps(sys->size + i, _mm256_load_ps(sys->init_size + i));
        _mm256_store_ps(sys->a + i, _mm256_load_ps(sys->init_a + i));
        _mm256_store_ps(sys->r + i, _mm256_load_ps(sys->init_r + i));
        _mm256_store_ps(sys->g + i, _mm256_load_ps(sys->init_g + i));
        _mm256_store_ps(sys->b + i, _mm256_load_ps(sys->init_b + i));
    }

    __m256 v_dt = _mm256_set1_ps(dt);
    __m256 v_zero = _mm256_setzero_ps();
    __m256 v_one = _mm256_set1_ps(1.0f);

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
            op_alpha_fade_avx2(sys, op, vec_n, v_zero, v_one);
            break;
        case WE_OP_SIZE_CHANGE:
            op_size_change_avx2(sys, op, vec_n, v_zero, v_one);
            break;
        case WE_OP_COLOR_CHANGE:
            op_color_change_avx2(sys, op, vec_n, v_zero, v_one);
            break;
        case WE_OP_OSCILLATE_ALPHA:
            op_oscillate_avx2_channel(sys->a, sys->age, sys->rng_phase, vec_n, op);
            break;
        case WE_OP_OSCILLATE_SIZE:
            op_oscillate_avx2_channel(sys->size, sys->age, sys->rng_phase, vec_n, op);
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

    for (uint32_t i = 0; i < vec_n; i += 8u) {
        _mm256_store_ps(sys->age + i, _mm256_add_ps(_mm256_load_ps(sys->age + i), v_dt));
    }
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

#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        particle_reset_and_ops_avx2(sys, dt);
    } else
#endif
    {
        particle_reset_and_ops_scalar(sys, dt);
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
    if (!sys || !sys->vert_buf) {
        if (out_verts) {
            *out_verts = NULL;
        }
        return 0;
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

bool we_particle_upload_gl(we_particle_sys_t *sys)
{
    if (!sys || sys->quad_cap == 0u || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }
    if (sys->gl_uploaded) {
        return true;
    }

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    if (vao == 0 || vbo == 0 || ebo == 0) {
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

    /* 0: a_Position (vec3), 1: a_TexCoord (vec2), 2: a_Color (vec4) */
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
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
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
    sys->gl_uploaded = true;
    return true;
}

void we_particle_draw_gl(we_particle_sys_t *sys, uint32_t gl_prog)
{
    if (!sys || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return;
    }
    (void)gl_prog;
    we_particle_vertex_t *verts = NULL;
    uint32_t vcnt = we_particle_build_vertices(sys, &verts);
    if (vcnt == 0u || sys->quad_count == 0u || !verts) {
        return;
    }
    if (sys->gl_uploaded && sys->quad_cap > sys->gl_quad_cap) {
        if (sys->gl_ebo) {
            GLuint b = sys->gl_ebo;
            glDeleteBuffers(1, &b);
            sys->gl_ebo = 0;
        }
        if (sys->gl_vbo) {
            GLuint b = sys->gl_vbo;
            glDeleteBuffers(1, &b);
            sys->gl_vbo = 0;
        }
        if (sys->gl_vao) {
            GLuint v = sys->gl_vao;
            glDeleteVertexArrays(1, &v);
            sys->gl_vao = 0;
        }
        sys->gl_uploaded = false;
    }
    if (!sys->gl_uploaded && !we_particle_upload_gl(sys)) {
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
    if (sys->gl_uploaded && eglGetCurrentContext() != EGL_NO_CONTEXT) {
        if (sys->gl_ebo) {
            GLuint b = sys->gl_ebo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_vbo) {
            GLuint b = sys->gl_vbo;
            glDeleteBuffers(1, &b);
        }
        if (sys->gl_vao) {
            GLuint v = sys->gl_vao;
            glDeleteVertexArrays(1, &v);
        }
    }
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
    if (lut->operator_index < sys->operator_count) {
        if (sys->operators[lut->operator_index].type != want_type) {
            return false;
        }
        target = &sys->operators[lut->operator_index];
    } else {
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
    }
    if (!target) {
        return false;
    }
    target->ramp_lut = *lut;
    target->use_ramp_lut = true;
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
    sys->vert_buf = calloc((size_t)sys->quad_cap * 4u, sizeof(we_particle_vertex_t));
    if (!sys->vert_buf) {
        free(sys->soa_block);
        free(sys);
        return NULL;
    }

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
}
