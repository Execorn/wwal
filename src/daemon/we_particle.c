// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_particle.h"

#include "waywal/log.h"

#include <GLES3/gl31.h>
#include <ctype.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* =========================================================================
 * Lightweight JSON DOM Helper for Particle System Definitions
 * ========================================================================= */

typedef enum {
    PJ_NULL = 0,
    PJ_BOOL,
    PJ_NUM,
    PJ_STR,
    PJ_ARR,
    PJ_OBJ,
} pj_type_t;

typedef struct pj_val pj_val_t;

typedef struct {
    char *key;
    pj_val_t *val;
} pj_kv_t;

struct pj_val {
    pj_type_t type;
    bool bval;
    double num;
    char *str;
    struct {
        pj_val_t **items;
        size_t count;
    } arr;
    struct {
        pj_kv_t *entries;
        size_t count;
    } obj;
};

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} pj_lexer_t;

static void pj_free(pj_val_t *v)
{
    if (!v) {
        return;
    }
    if (v->type == PJ_STR) {
        free(v->str);
    } else if (v->type == PJ_ARR) {
        for (size_t i = 0; i < v->arr.count; i++) {
            pj_free(v->arr.items[i]);
        }
        free(v->arr.items);
    } else if (v->type == PJ_OBJ) {
        for (size_t i = 0; i < v->obj.count; i++) {
            free(v->obj.entries[i].key);
            pj_free(v->obj.entries[i].val);
        }
        free(v->obj.entries);
    }
    free(v);
}

static void pj_skip_ws(pj_lexer_t *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->pos++;
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
        } else if (c == '/' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                lx->pos++;
            }
            lx->pos = (lx->pos + 1 < lx->len) ? (lx->pos + 2) : lx->len;
        } else {
            break;
        }
    }
}

static char *pj_parse_raw_str(pj_lexer_t *lx)
{
    pj_skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
        return NULL;
    }
    lx->pos++;
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos++];
        if (c == '"') {
            out[len] = '\0';
            return out;
        }
        if (c == '\\' && lx->pos < lx->len) {
            char esc = lx->src[lx->pos++];
            switch (esc) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            default:
                c = esc;
                break;
            }
        }
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(out, ncap);
            if (!nb) {
                free(out);
                return NULL;
            }
            out = nb;
            cap = ncap;
        }
        out[len++] = c;
    }
    free(out);
    return NULL;
}

static pj_val_t *pj_parse_val(pj_lexer_t *lx, int depth);

static pj_val_t *pj_parse_arr(pj_lexer_t *lx, int depth)
{
    lx->pos++;
    pj_val_t *arr = calloc(1, sizeof(pj_val_t));
    if (!arr) {
        return NULL;
    }
    arr->type = PJ_ARR;
    size_t cap = 0;
    while (lx->pos < lx->len) {
        pj_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
            lx->pos++;
            return arr;
        }
        pj_val_t *el = pj_parse_val(lx, depth + 1);
        if (!el) {
            pj_free(arr);
            return NULL;
        }
        if (arr->arr.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            pj_val_t **ni = realloc(arr->arr.items, ncap * sizeof(pj_val_t *));
            if (!ni) {
                pj_free(el);
                pj_free(arr);
                return NULL;
            }
            arr->arr.items = ni;
            cap = ncap;
        }
        arr->arr.items[arr->arr.count++] = el;
        pj_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
        }
    }
    pj_free(arr);
    return NULL;
}

static pj_val_t *pj_parse_obj(pj_lexer_t *lx, int depth)
{
    lx->pos++;
    pj_val_t *obj = calloc(1, sizeof(pj_val_t));
    if (!obj) {
        return NULL;
    }
    obj->type = PJ_OBJ;
    size_t cap = 0;
    while (lx->pos < lx->len) {
        pj_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
            lx->pos++;
            return obj;
        }
        char *k = pj_parse_raw_str(lx);
        if (!k) {
            pj_free(obj);
            return NULL;
        }
        pj_skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
            free(k);
            pj_free(obj);
            return NULL;
        }
        lx->pos++;
        pj_val_t *v = pj_parse_val(lx, depth + 1);
        if (!v) {
            free(k);
            pj_free(obj);
            return NULL;
        }
        if (obj->obj.count + 1 > cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            pj_kv_t *ne = realloc(obj->obj.entries, ncap * sizeof(pj_kv_t));
            if (!ne) {
                free(k);
                pj_free(v);
                pj_free(obj);
                return NULL;
            }
            obj->obj.entries = ne;
            cap = ncap;
        }
        obj->obj.entries[obj->obj.count].key = k;
        obj->obj.entries[obj->obj.count].val = v;
        obj->obj.count++;
        pj_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
        }
    }
    pj_free(obj);
    return NULL;
}

static pj_val_t *pj_parse_val(pj_lexer_t *lx, int depth)
{
    if (depth > 64) {
        return NULL;
    }
    pj_skip_ws(lx);
    if (lx->pos >= lx->len) {
        return NULL;
    }
    char c = lx->src[lx->pos];
    if (c == '{') {
        return pj_parse_obj(lx, depth);
    }
    if (c == '[') {
        return pj_parse_arr(lx, depth);
    }
    if (c == '"') {
        char *s = pj_parse_raw_str(lx);
        if (!s) {
            return NULL;
        }
        pj_val_t *v = calloc(1, sizeof(pj_val_t));
        if (!v) {
            free(s);
            return NULL;
        }
        v->type = PJ_STR;
        v->str = s;
        return v;
    }
    if (lx->len - lx->pos >= 4 && strncmp(lx->src + lx->pos, "true", 4) == 0) {
        lx->pos += 4;
        pj_val_t *v = calloc(1, sizeof(pj_val_t));
        if (v) {
            v->type = PJ_BOOL;
            v->bval = true;
        }
        return v;
    }
    if (lx->len - lx->pos >= 5 && strncmp(lx->src + lx->pos, "false", 5) == 0) {
        lx->pos += 5;
        pj_val_t *v = calloc(1, sizeof(pj_val_t));
        if (v) {
            v->type = PJ_BOOL;
            v->bval = false;
        }
        return v;
    }
    if (lx->len - lx->pos >= 4 && strncmp(lx->src + lx->pos, "null", 4) == 0) {
        lx->pos += 4;
        pj_val_t *v = calloc(1, sizeof(pj_val_t));
        if (v) {
            v->type = PJ_NULL;
        }
        return v;
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.') {
        size_t rem = lx->len - lx->pos;
        size_t clen = rem < 63 ? rem : 63;
        char nbuf[64];
        memcpy(nbuf, lx->src + lx->pos, clen);
        nbuf[clen] = '\0';
        char *endptr = NULL;
        double num = strtod(nbuf, &endptr);
        if (endptr == nbuf) {
            return NULL;
        }
        lx->pos += (size_t)(endptr - nbuf);
        pj_val_t *v = calloc(1, sizeof(pj_val_t));
        if (v) {
            v->type = PJ_NUM;
            v->num = num;
        }
        return v;
    }
    return NULL;
}

static const pj_val_t *pj_get(const pj_val_t *obj, const char *key)
{
    if (!obj || obj->type != PJ_OBJ || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (strcasecmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return NULL;
}

static const char *pj_str(const pj_val_t *v, const char *def)
{
    if (!v) {
        return def;
    }
    if (v->type == PJ_STR && v->str) {
        return v->str;
    }
    if (v->type == PJ_OBJ) {
        const pj_val_t *in = pj_get(v, "value");
        if (in && in->type == PJ_STR && in->str) {
            return in->str;
        }
    }
    return def;
}

static uint32_t pj_floats(const pj_val_t *v, float *out, uint32_t max_n)
{
    if (!v || !out || max_n == 0) {
        return 0;
    }
    if (v->type == PJ_OBJ) {
        const pj_val_t *in = pj_get(v, "value");
        return in ? pj_floats(in, out, max_n) : 0;
    }
    if (v->type == PJ_NUM) {
        out[0] = (float)v->num;
        return 1;
    }
    if (v->type == PJ_BOOL) {
        out[0] = v->bval ? 1.0f : 0.0f;
        return 1;
    }
    if (v->type == PJ_STR && v->str) {
        if (strcasecmp(v->str, "true") == 0) {
            out[0] = 1.0f;
            return 1;
        }
        if (strcasecmp(v->str, "false") == 0) {
            out[0] = 0.0f;
            return 1;
        }
        const char *p = v->str;
        uint32_t cnt = 0;
        while (*p && cnt < max_n) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) {
                p++;
            }
            if (!*p) {
                break;
            }
            char *endptr = NULL;
            float f = strtof(p, &endptr);
            if (endptr == p) {
                break;
            }
            out[cnt++] = f;
            p = endptr;
        }
        return cnt;
    }
    if (v->type == PJ_ARR) {
        uint32_t cnt = (uint32_t)v->arr.count < max_n ? (uint32_t)v->arr.count : max_n;
        for (uint32_t i = 0; i < cnt; i++) {
            float tmp = 0.0f;
            pj_floats(v->arr.items[i], &tmp, 1);
            out[i] = tmp;
        }
        return cnt;
    }
    return 0;
}

static float pj_float(const pj_val_t *v, float def)
{
    float tmp = def;
    return (pj_floats(v, &tmp, 1) >= 1) ? tmp : def;
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
        sys->max_count = new_max_count;
        if (sys->alive_count > sys->max_count) {
            sys->alive_count = sys->max_count;
        }
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
    if (sys->start_time > 0.0f) {
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

we_particle_sys_t *we_particle_create_from_json(const char *json_str, size_t len)
{
    if (!json_str || len == 0) {
        return NULL;
    }

    pj_lexer_t lx = {.src = json_str, .len = len, .pos = 0};
    pj_val_t *root = pj_parse_val(&lx, 0);
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

    int32_t raw_max = (int32_t)pj_float(pj_get(root, "maxcount"), (float)WE_PARTICLE_DEFAULT_CAP);
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

    sys->start_time = pj_float(pj_get(root, "starttime"), 0.0f);
    snprintf(sys->material_path, sizeof(sys->material_path), "%s",
             pj_str(pj_get(root, "material"), ""));

    int32_t flags = (int32_t)pj_float(pj_get(root, "flags"), 0.0f);
    sys->world_space = (flags & 1) != 0;
    sys->perspective = (flags & 4) != 0;

    /* Parse renderer[] */
    sys->renderer_type = WE_RENDERER_SPRITE;
    sys->trail_length = 0.05f;
    sys->trail_min_length = 1.0f;
    sys->trail_max_length = 100.0f;
    const pj_val_t *rends = pj_get(root, "renderer");
    if (rends && rends->type == PJ_ARR && rends->arr.count > 0) {
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
        sys->subdivision = (uint32_t)pj_float(pj_get(rn, "subdivision"), 1.0f);
    }

    /* Parse controlpoint[] */
    for (uint32_t c = 0; c < WE_PARTICLE_MAX_CONTROL_PTS; c++) {
        sys->control_points[c].id = (int32_t)c;
    }
    const pj_val_t *cps = pj_get(root, "controlpoint");
    if (cps && cps->type == PJ_ARR) {
        for (size_t i = 0; i < cps->arr.count && i < WE_PARTICLE_MAX_CONTROL_PTS; i++) {
            const pj_val_t *cn = cps->arr.items[i];
            if (!cn || cn->type != PJ_OBJ) {
                continue;
            }
            int32_t cid = (int32_t)pj_float(pj_get(cn, "id"), (float)i);
            if (cid < 0 || cid >= (int32_t)WE_PARTICLE_MAX_CONTROL_PTS) {
                cid = (int32_t)i;
            }
            we_particle_cp_t *cp = &sys->control_points[cid];
            cp->id = cid;
            pj_vec3(pj_get(cn, "offset"), cp->offset, 0.0f, 0.0f, 0.0f);
            memcpy(cp->pos, cp->offset, sizeof(cp->pos));
            int32_t cp_flags = (int32_t)pj_float(pj_get(cn, "flags"), 0.0f);
            cp->lock_to_pointer = (cp_flags & 1) != 0 ||
                                  (pj_float(pj_get(cn, "locktopointer"), 0.0f) != 0.0f) ||
                                  (pj_float(pj_get(cn, "link_mouse"), 0.0f) != 0.0f);
            cp->world_space =
                (cp_flags & 2) != 0 || (pj_float(pj_get(cn, "worldspace"), 0.0f) != 0.0f);
            if ((uint32_t)(cid + 1) > sys->control_point_count) {
                sys->control_point_count = (uint32_t)(cid + 1);
            }
        }
    }

    /* Parse emitter[] */
    const pj_val_t *emits = pj_get(root, "emitter");
    if (emits && emits->type == PJ_ARR) {
        for (size_t i = 0; i < emits->arr.count && sys->emitter_count < WE_PARTICLE_MAX_EMITTERS;
             i++) {
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
            em->speed_max =
                pj_float(esmax, em->speed_min > base_speed ? em->speed_min : base_speed);
            em->rate = pj_float(pj_get(en, "rate"), 10.0f);
            em->instantaneous = (uint32_t)fmaxf(0.0f, pj_float(pj_get(en, "instantaneous"), 0.0f));
            em->control_point = (int32_t)pj_float(pj_get(en, "controlpoint"), 0.0f);
        }
    }

    /* Parse initializer[] */
    const pj_val_t *inits = pj_get(root, "initializer");
    if (inits && inits->type == PJ_ARR) {
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
            if (iz->exponent <= 0.0f) {
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

    /* Parse operator[] */
    const pj_val_t *ops = pj_get(root, "operator");
    if (ops && ops->type == PJ_ARR) {
        for (size_t i = 0; i < ops->arr.count && sys->operator_count < WE_PARTICLE_MAX_OPERATORS;
             i++) {
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
            } else if (strcasecmp(oname, "vortex") == 0 ||
                       strcasecmp(oname, "controlpointattract") == 0) {
                op->type = WE_OP_VORTEX;
                op->control_point = (int32_t)pj_float(pj_get(on, "controlpoint"), 0.0f);
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
                if (strcasecmp(oname, "controlpointattract") == 0) {
                    op->speed_inner = 0.0f;
                    op->speed_outer = 0.0f;
                }
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

    pj_free(root);

    sys->quad_cap = sys->padded_cap;
    sys->vert_buf = calloc((size_t)sys->quad_cap * 4u, sizeof(we_particle_vertex_t));
    if (!sys->vert_buf) {
        we_particle_destroy(sys);
        return NULL;
    }

    /* Pre-warm simulation if starttime > 0 */
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

static void particle_reset_and_ops_scalar(we_particle_sys_t *sys, float dt)
{
    uint32_t n = sys->alive_count;
    if (n == 0u) {
        return;
    }

    /* Pre-Operator Reset: prevent multiplicative compounding across ticks */
    memcpy(sys->size, sys->init_size, n * sizeof(float));
    memcpy(sys->a, sys->init_a, n * sizeof(float));
    memcpy(sys->r, sys->init_r, n * sizeof(float));
    memcpy(sys->g, sys->init_g, n * sizeof(float));
    memcpy(sys->b, sys->init_b, n * sizeof(float));

    for (uint32_t o = 0; o < sys->operator_count; o++) {
        const we_particle_operator_t *op = &sys->operators[o];
        switch (op->type) {
        case WE_OP_MOVEMENT: {
            float damp = expf(-op->drag * dt);
            float gx = op->gravity[0], gy = op->gravity[1], gz = op->gravity[2];
            for (uint32_t i = 0; i < n; i++) {
                float vx = (sys->vel_x[i] + (gx + sys->acc_x[i]) * dt) * damp;
                float vy = (sys->vel_y[i] + (gy + sys->acc_y[i]) * dt) * damp;
                float vz = (sys->vel_z[i] + (gz + sys->acc_z[i]) * dt) * damp;
                sys->vel_x[i] = vx;
                sys->vel_y[i] = vy;
                sys->vel_z[i] = vz;
                sys->pos_x[i] += vx * dt;
                sys->pos_y[i] += vy * dt;
                sys->pos_z[i] += vz * dt;
            }
            break;
        }
        case WE_OP_ANGULAR_MOVEMENT: {
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
            break;
        }
        case WE_OP_ALPHA_FADE: {
            float fin = op->fade_in_time;
            float fout = op->fade_out_time;
            float inv_fin = (fin > 1e-5f) ? (1.0f / fin) : 0.0f;
            float inv_fout = (fout < 0.99999f) ? (1.0f / (1.0f - fout)) : 0.0f;
            for (uint32_t i = 0; i < n; i++) {
                float mlife = fmaxf(sys->max_life[i], 1e-5f);
                float t = fminf(fmaxf(sys->age[i] / mlife, 0.0f), 1.0f);
                float fi = (fin > 1e-5f) ? fminf(fmaxf(t * inv_fin, 0.0f), 1.0f) : 1.0f;
                float fo =
                    (fout < 0.99999f) ? fminf(fmaxf((1.0f - t) * inv_fout, 0.0f), 1.0f) : 1.0f;
                sys->a[i] *= (fi * fo);
            }
            break;
        }
        case WE_OP_SIZE_CHANGE: {
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
            break;
        }
        case WE_OP_COLOR_CHANGE: {
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
            break;
        }
        case WE_OP_OSCILLATE_ALPHA: {
            float freq = 0.5f * (op->freq_min + op->freq_max);
            for (uint32_t i = 0; i < n; i++) {
                float w = 0.5f + 0.5f * sinf(sys->age[i] * freq + sys->rng_phase[i]);
                sys->a[i] *= (op->scale_min + w * (op->scale_max - op->scale_min));
            }
            break;
        }
        case WE_OP_OSCILLATE_SIZE: {
            float freq = 0.5f * (op->freq_min + op->freq_max);
            for (uint32_t i = 0; i < n; i++) {
                float w = 0.5f + 0.5f * sinf(sys->age[i] * freq + sys->rng_phase[i]);
                sys->size[i] *= (op->scale_min + w * (op->scale_max - op->scale_min));
            }
            break;
        }
        case WE_OP_OSCILLATE_POSITION: {
            float freq = 0.5f * (op->freq_min + op->freq_max);
            float amp = 0.5f * (op->scale_min + op->scale_max);
            for (uint32_t i = 0; i < n; i++) {
                float p0 = sys->age[i] * freq + sys->rng_phase[i];
                float p1 = (sys->age[i] + dt) * freq + sys->rng_phase[i];
                float s = (sinf(p1) - sinf(p0)) * amp;
                sys->pos_x[i] += s * op->mask[0];
                sys->pos_y[i] += s * op->mask[1];
                sys->pos_z[i] += s * op->mask[2];
            }
            break;
        }
        case WE_OP_VORTEX: {
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
                /* Tangential swirl = cross(axis, d) / dist */
                float tx = (ay * dz - az * dy) * inv_dist;
                float ty = (az * dx - ax * dz) * inv_dist;
                float tz = (ax * dy - ay * dx) * inv_dist;
                /* Centripetal pull = -d / dist */
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
            break;
        }
        case WE_OP_TURBULENCE: {
            float sc = op->turb_scale > 0.0f ? op->turb_scale : 0.01f;
            float t_off = (float)sys->sim_time * op->turb_timescale;
            float spd = op->turb_speed * dt;
            for (uint32_t i = 0; i < n; i++) {
                float curl[3];
                eval_curl_noise_3d(sys->pos_x[i] * sc + t_off, sys->pos_y[i] * sc + t_off,
                                   sys->pos_z[i] * sc + t_off, curl);
                sys->vel_x[i] += curl[0] * spd * op->mask[0];
                sys->vel_y[i] += curl[1] * spd * op->mask[1];
                sys->vel_z[i] += curl[2] * spd * op->mask[2];
            }
            break;
        }
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

__attribute__((unused, target("avx2,fma"))) static inline __m256 vcos256_ps(__m256 x)
{
    return vsin256_ps(_mm256_add_ps(x, _mm256_set1_ps(1.57079632679489661923f)));
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

    /* 1. AVX2 Pre-Operator Reset across 32B-aligned SoA arrays */
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
        case WE_OP_MOVEMENT: {
            float damp = expf(-op->drag * dt);
            __m256 v_damp = _mm256_set1_ps(damp);
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

                __m256 px = _mm256_fmadd_ps(vx, v_dt, _mm256_load_ps(sys->pos_x + i));
                __m256 py = _mm256_fmadd_ps(vy, v_dt, _mm256_load_ps(sys->pos_y + i));
                __m256 pz = _mm256_fmadd_ps(vz, v_dt, _mm256_load_ps(sys->pos_z + i));

                _mm256_store_ps(sys->pos_x + i, px);
                _mm256_store_ps(sys->pos_y + i, py);
                _mm256_store_ps(sys->pos_z + i, pz);
            }
            break;
        }
        case WE_OP_ANGULAR_MOVEMENT: {
            float damp = expf(-op->drag * dt);
            __m256 v_damp = _mm256_set1_ps(damp);
            __m256 v_fx = _mm256_set1_ps(op->gravity[0]);
            __m256 v_fy = _mm256_set1_ps(op->gravity[1]);
            __m256 v_fz = _mm256_set1_ps(op->gravity[2]);

            for (uint32_t i = 0; i < vec_n; i += 8u) {
                __m256 avx = _mm256_mul_ps(
                    _mm256_fmadd_ps(v_fx, v_dt, _mm256_load_ps(sys->ang_vel_x + i)), v_damp);
                __m256 avy = _mm256_mul_ps(
                    _mm256_fmadd_ps(v_fy, v_dt, _mm256_load_ps(sys->ang_vel_y + i)), v_damp);
                __m256 avz = _mm256_mul_ps(
                    _mm256_fmadd_ps(v_fz, v_dt, _mm256_load_ps(sys->ang_vel_z + i)), v_damp);

                _mm256_store_ps(sys->ang_vel_x + i, avx);
                _mm256_store_ps(sys->ang_vel_y + i, avy);
                _mm256_store_ps(sys->ang_vel_z + i, avz);

                _mm256_store_ps(sys->rot_x + i,
                                _mm256_fmadd_ps(avx, v_dt, _mm256_load_ps(sys->rot_x + i)));
                _mm256_store_ps(sys->rot_y + i,
                                _mm256_fmadd_ps(avy, v_dt, _mm256_load_ps(sys->rot_y + i)));
                _mm256_store_ps(sys->rot_z + i,
                                _mm256_fmadd_ps(avz, v_dt, _mm256_load_ps(sys->rot_z + i)));
            }
            break;
        }
        case WE_OP_ALPHA_FADE: {
            float fin = op->fade_in_time;
            float fout = op->fade_out_time;
            __m256 v_inv_fin = _mm256_set1_ps((fin > 1e-5f) ? (1.0f / fin) : 1e6f);
            __m256 v_inv_fout = _mm256_set1_ps((fout < 0.99999f) ? (1.0f / (1.0f - fout)) : 1e6f);

            for (uint32_t i = 0; i < vec_n; i += 8u) {
                __m256 age = _mm256_load_ps(sys->age + i);
                __m256 mlife =
                    _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
                __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
                __m256 fi =
                    (fin > 1e-5f)
                        ? _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(t, v_inv_fin), v_zero), v_one)
                        : v_one;
                __m256 fo = (fout < 0.99999f)
                                ? _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(v_one, t),
                                                                            v_inv_fout),
                                                              v_zero),
                                                v_one)
                                : v_one;
                __m256 a = _mm256_load_ps(sys->a + i);
                _mm256_store_ps(sys->a + i, _mm256_mul_ps(a, _mm256_mul_ps(fi, fo)));
            }
            break;
        }
        case WE_OP_SIZE_CHANGE: {
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
                __m256 mlife =
                    _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
                __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
                __m256 u = (inv_span > 0.0f)
                               ? _mm256_min_ps(
                                     _mm256_max_ps(
                                         _mm256_mul_ps(_mm256_sub_ps(t, v_st), v_inv_span), v_zero),
                                     v_one)
                               : v_one;
                __m256 mult = _mm256_fmadd_ps(u, v_dv, v_sv);
                _mm256_store_ps(sys->size + i, _mm256_mul_ps(_mm256_load_ps(sys->size + i), mult));
            }
            break;
        }
        case WE_OP_COLOR_CHANGE: {
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
                __m256 mlife =
                    _mm256_max_ps(_mm256_load_ps(sys->max_life + i), _mm256_set1_ps(1e-5f));
                __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_div_ps(age, mlife), v_zero), v_one);
                __m256 u = (inv_span > 0.0f)
                               ? _mm256_min_ps(
                                     _mm256_max_ps(
                                         _mm256_mul_ps(_mm256_sub_ps(t, v_st), v_inv_span), v_zero),
                                     v_one)
                               : v_one;
                _mm256_store_ps(sys->r + i, _mm256_mul_ps(_mm256_load_ps(sys->r + i),
                                                          _mm256_fmadd_ps(u, dr, sr)));
                _mm256_store_ps(sys->g + i, _mm256_mul_ps(_mm256_load_ps(sys->g + i),
                                                          _mm256_fmadd_ps(u, dg, sg)));
                _mm256_store_ps(sys->b + i, _mm256_mul_ps(_mm256_load_ps(sys->b + i),
                                                          _mm256_fmadd_ps(u, db, sb)));
            }
            break;
        }
        case WE_OP_OSCILLATE_ALPHA: {
            float freq = 0.5f * (op->freq_min + op->freq_max);
            __m256 v_freq = _mm256_set1_ps(freq);
            __m256 v_half = _mm256_set1_ps(0.5f);
            __m256 v_smin = _mm256_set1_ps(op->scale_min);
            __m256 v_sdiff = _mm256_set1_ps(op->scale_max - op->scale_min);
            for (uint32_t i = 0; i < vec_n; i += 8u) {
                __m256 phase = _mm256_fmadd_ps(_mm256_load_ps(sys->age + i), v_freq,
                                               _mm256_load_ps(sys->rng_phase + i));
                __m256 w = _mm256_fmadd_ps(v_half, vsin256_ps(phase), v_half);
                __m256 scale = _mm256_fmadd_ps(w, v_sdiff, v_smin);
                _mm256_store_ps(sys->a + i, _mm256_mul_ps(_mm256_load_ps(sys->a + i), scale));
            }
            break;
        }
        case WE_OP_OSCILLATE_SIZE: {
            float freq = 0.5f * (op->freq_min + op->freq_max);
            __m256 v_freq = _mm256_set1_ps(freq);
            __m256 v_half = _mm256_set1_ps(0.5f);
            __m256 v_smin = _mm256_set1_ps(op->scale_min);
            __m256 v_sdiff = _mm256_set1_ps(op->scale_max - op->scale_min);
            for (uint32_t i = 0; i < vec_n; i += 8u) {
                __m256 phase = _mm256_fmadd_ps(_mm256_load_ps(sys->age + i), v_freq,
                                               _mm256_load_ps(sys->rng_phase + i));
                __m256 w = _mm256_fmadd_ps(v_half, vsin256_ps(phase), v_half);
                __m256 scale = _mm256_fmadd_ps(w, v_sdiff, v_smin);
                _mm256_store_ps(sys->size + i, _mm256_mul_ps(_mm256_load_ps(sys->size + i), scale));
            }
            break;
        }
        case WE_OP_OSCILLATE_POSITION: {
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
            break;
        }
        case WE_OP_VORTEX: {
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

                __m256 tx =
                    _mm256_mul_ps(_mm256_fmsub_ps(v_ay, dz, _mm256_mul_ps(v_az, dy)), inv_dist);
                __m256 ty =
                    _mm256_mul_ps(_mm256_fmsub_ps(v_az, dx, _mm256_mul_ps(v_ax, dz)), inv_dist);
                __m256 tz =
                    _mm256_mul_ps(_mm256_fmsub_ps(v_ax, dy, _mm256_mul_ps(v_ay, dx)), inv_dist);

                __m256 u = _mm256_min_ps(
                    _mm256_max_ps(_mm256_mul_ps(_mm256_sub_ps(dist, v_din), v_inv_d), v_zero),
                    v_one);
                __m256 t_spd = _mm256_fmadd_ps(u, v_sdiff, v_sin);
                __m256 rad_scale = _mm256_mul_ps(v_pull, inv_dist);

                __m256 fx = _mm256_and_ps(active_mask,
                                          _mm256_fmadd_ps(tx, t_spd, _mm256_mul_ps(dx, rad_scale)));
                __m256 fy = _mm256_and_ps(active_mask,
                                          _mm256_fmadd_ps(ty, t_spd, _mm256_mul_ps(dy, rad_scale)));
                __m256 fz = _mm256_and_ps(active_mask,
                                          _mm256_fmadd_ps(tz, t_spd, _mm256_mul_ps(dz, rad_scale)));

                _mm256_store_ps(sys->vel_x + i,
                                _mm256_fmadd_ps(fx, v_dt, _mm256_load_ps(sys->vel_x + i)));
                _mm256_store_ps(sys->vel_y + i,
                                _mm256_fmadd_ps(fy, v_dt, _mm256_load_ps(sys->vel_y + i)));
                _mm256_store_ps(sys->vel_z + i,
                                _mm256_fmadd_ps(fz, v_dt, _mm256_load_ps(sys->vel_z + i)));
            }
            break;
        }
        case WE_OP_TURBULENCE: {
            float sc = op->turb_scale > 0.0f ? op->turb_scale : 0.01f;
            float t_off = (float)sys->sim_time * op->turb_timescale;
            float spd = op->turb_speed * dt;
            for (uint32_t i = 0; i < n; i++) {
                float curl[3];
                eval_curl_noise_3d(sys->pos_x[i] * sc + t_off, sys->pos_y[i] * sc + t_off,
                                   sys->pos_z[i] * sc + t_off, curl);
                sys->vel_x[i] += curl[0] * spd * op->mask[0];
                sys->vel_y[i] += curl[1] * spd * op->mask[1];
                sys->vel_z[i] += curl[2] * spd * op->mask[2];
            }
            break;
        }
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
    uint32_t stride = sys->padded_cap;
    for (uint32_t a = 0; a < WE_PARTICLE_SOA_ARRAYS; a++) {
        sys->soa_block[a * stride + dst] = sys->soa_block[a * stride + src];
    }
}

void we_particle_update(we_particle_sys_t *sys, float dt_sec)
{
    if (!sys || !isfinite(dt_sec) || dt_sec <= 0.0f) {
        return;
    }
    float dt = fminf(dt_sec, 0.25f);
    sys->sim_time += (double)dt;

    /* 1. Run emitters */
    for (uint32_t e = 0; e < sys->emitter_count; e++) {
        we_particle_emitter_t *em = &sys->emitters[e];
        if (!em->burst_done && em->instantaneous > 0u) {
            em->burst_done = true;
            for (uint32_t b = 0; b < em->instantaneous && sys->alive_count < sys->max_count; b++) {
                spawn_single_particle(sys, em);
            }
        }
        if (em->rate > 0.0f) {
            em->emit_accum += em->rate * dt;
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

    /* 2. Run Pre-Operator Reset + Operators + Age advance */
#if !defined(WWAL_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64))
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        particle_reset_and_ops_avx2(sys, dt);
    } else
#endif
    {
        particle_reset_and_ops_scalar(sys, dt);
    }

    /* 3. Compact dead particles (age[i] >= max_life[i]) */
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
        for (uint32_t i = 0; i < count; i++) {
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
    if (!sys || sys->quad_cap == 0u) {
        return false;
    }
    if (sys->gl_uploaded) {
        return true;
    }

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

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

    sys->gl_vao = vao;
    sys->gl_vbo = vbo;
    sys->gl_ebo = ebo;
    sys->gl_quad_cap = sys->quad_cap;
    sys->gl_uploaded = true;
    return true;
}

void we_particle_draw_gl(we_particle_sys_t *sys, uint32_t gl_prog)
{
    if (!sys) {
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
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(draw_verts * sizeof(we_particle_vertex_t)),
                    verts);
    glDrawElements(GL_TRIANGLES, (GLsizei)(draw_quads * 6u), GL_UNSIGNED_INT, (const void *)0);
    glDepthMask(prev_depth_mask);
}

void we_particle_destroy(we_particle_sys_t *sys)
{
    if (!sys) {
        return;
    }
    if (sys->gl_uploaded) {
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
