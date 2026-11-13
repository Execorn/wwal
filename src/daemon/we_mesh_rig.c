// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_mesh_rig.h"

#include "waywal/log.h"

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define WE_RIG_PI         3.14159265358979323846
#define WE_RIG_TWO_PI_F   6.28318530717958647692f
#define WE_RIG_PI_F       3.14159265358979323846f
#define WE_RIG_DDA_GRID_N 64u
#define WE_RIG_COW_WORDS  ((WE_MDL_MAX_VERTICES / WE_RIG_COW_BLOCK_VERTS + 63u) / 64u)
#define WE_RIG_RHS_BATCH  8u
#define WE_RIG_EMPTY_EDGE UINT32_MAX

static double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

static inline float clampf(float x, float lo, float hi)
{
    if (!isfinite(x)) {
        return lo;
    }
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline double clampd(double x, double lo, double hi)
{
    if (!isfinite(x)) {
        return lo;
    }
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ========================================================================= */
/* 1. Matrix, Quaternion & Skeleton Topology Utilities                       */
/* ========================================================================= */

static void rig_mat4_identity(float m[16])
{
    memset(m, 0, 16u * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void rig_mat4_from_trs(const float pos[3], const float angle[3], const float scale[3],
                              float out[16])
{
    float cx = cosf(angle[0]), sx = sinf(angle[0]);
    float cy = cosf(angle[1]), sy = sinf(angle[1]);
    float cz = cosf(angle[2]), sz = sinf(angle[2]);

    float scx = scale[0], scy = scale[1], scz = scale[2];

    out[0] = (cz * cy) * scx;
    out[1] = (sz * cy) * scx;
    out[2] = (-sy) * scx;
    out[3] = 0.0f;

    out[4] = (cz * sy * sx - sz * cx) * scy;
    out[5] = (sz * sy * sx + cz * cx) * scy;
    out[6] = (cy * sx) * scy;
    out[7] = 0.0f;

    out[8] = (cz * sy * cx + sz * sx) * scz;
    out[9] = (sz * sy * cx - cz * sx) * scz;
    out[10] = (cy * cx) * scz;
    out[11] = 0.0f;

    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1.0f;
}

static void rig_mat4_decompose_trs(const float m[16], float out_pos[3], float out_angle[3],
                                   float out_scale[3])
{
    out_pos[0] = m[12];
    out_pos[1] = m[13];
    out_pos[2] = m[14];

    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

    float det3 = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                 m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (det3 < 0.0f) {
        sx = -sx;
    }

    out_scale[0] = fabsf(sx) > 1e-6f ? sx : 1.0f;
    out_scale[1] = sy > 1e-6f ? sy : 1.0f;
    out_scale[2] = sz > 1e-6f ? sz : 1.0f;

    float r00 = m[0] / out_scale[0], r10 = m[1] / out_scale[0], r20 = m[2] / out_scale[0];
    float r11 = m[5] / out_scale[1], r21 = m[6] / out_scale[1];
    float r12 = m[9] / out_scale[2], r22 = m[10] / out_scale[2];

    float sy_rot = clampf(-r20, -1.0f, 1.0f);
    out_angle[1] = asinf(sy_rot);
    if (fabsf(r20) < 0.9999f) {
        out_angle[0] = atan2f(r21, r22);
        out_angle[2] = atan2f(r10, r00);
    } else {
        out_angle[0] = atan2f(-r12, r11);
        out_angle[2] = 0.0f;
    }
}

static bool validate_and_topo_sort_skeleton(const we_puppet_t *puppet,
                                            uint32_t out_topo[WE_MDL_MAX_BONES],
                                            bool *out_already_topo)
{
    if (!puppet || !puppet->bones || puppet->bone_count == 0u ||
        puppet->bone_count > WE_MDL_MAX_BONES) {
        return false;
    }

    uint32_t bcnt = puppet->bone_count;
    bool already_topo = true;
    uint8_t state[WE_MDL_MAX_BONES] = {0}; /* 0=unvisited, 1=visiting, 2=done */

    for (uint32_t i = 0; i < bcnt; i++) {
        uint32_t p = puppet->bones[i].parent;
        if (p != 0xFFFFFFFFu && p >= bcnt) {
            return false;
        }
        if (p != 0xFFFFFFFFu && p >= i) {
            already_topo = false;
        }
    }

    if (already_topo) {
        for (uint32_t i = 0; i < bcnt; i++) {
            out_topo[i] = i;
        }
        if (out_already_topo) {
            *out_already_topo = true;
        }
        return true;
    }

    /* Iterative 3-color cycle detection plus depth-ordered emission preserves parent-before-child
     * invariant even when a caller supplies bones out of index order. */
    uint8_t depth[WE_MDL_MAX_BONES] = {0};
    for (uint32_t i = 0; i < bcnt; i++) {
        if (state[i] == 2u) {
            continue;
        }
        uint32_t chain[WE_MDL_MAX_BONES];
        uint32_t chain_len = 0u;
        uint32_t cur = i;
        while (cur != 0xFFFFFFFFu) {
            if (cur >= bcnt) {
                return false;
            }
            if (state[cur] == 1u) {
                return false;
            }
            if (state[cur] == 2u) {
                break;
            }
            state[cur] = 1u;
            chain[chain_len++] = cur;
            cur = puppet->bones[cur].parent;
        }
        uint8_t base_depth = (cur != 0xFFFFFFFFu) ? (uint8_t)(depth[cur] + 1u) : 0u;
        for (uint32_t r = chain_len; r > 0u; r--) {
            uint32_t node = chain[r - 1u];
            state[node] = 2u;
            depth[node] = base_depth++;
        }
    }

    uint32_t write_idx = 0u;
    for (uint32_t d = 0u; d < bcnt && write_idx < bcnt; d++) {
        for (uint32_t i = 0u; i < bcnt; i++) {
            if (depth[i] == d) {
                out_topo[write_idx++] = i;
            }
        }
    }
    if (write_idx != bcnt) {
        return false;
    }
    if (out_already_topo) {
        *out_already_topo = false;
    }
    return true;
}

static bool mat4_is_finite(const float m[16])
{
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i])) {
            return false;
        }
    }
    return fabsf(m[15]) >= 1e-6f;
}

static void compute_bind_world_matrices(const we_puppet_t *puppet,
                                        const uint32_t topo[WE_MDL_MAX_BONES],
                                        float (*out_world_bind)[16])
{
    uint32_t bcnt = puppet->bone_count;
    for (uint32_t ti = 0; ti < bcnt; ti++) {
        uint32_t k = topo[ti];
        const we_mdl_bone_t *b = &puppet->bones[k];
        if (mat4_is_finite(b->world_bind)) {
            memcpy(out_world_bind[k], b->world_bind, sizeof(out_world_bind[k]));
            continue;
        }
        float loc[16];
        if (mat4_is_finite(b->local_trans)) {
            memcpy(loc, b->local_trans, sizeof(loc));
        } else {
            float scl[3] = {
                (isfinite(b->bind_scale[0]) && fabsf(b->bind_scale[0]) > 1e-6f) ? b->bind_scale[0]
                                                                                : 1.0f,
                (isfinite(b->bind_scale[1]) && fabsf(b->bind_scale[1]) > 1e-6f) ? b->bind_scale[1]
                                                                                : 1.0f,
                (isfinite(b->bind_scale[2]) && fabsf(b->bind_scale[2]) > 1e-6f) ? b->bind_scale[2]
                                                                                : 1.0f,
            };
            float pos[3] = {
                isfinite(b->bind_pos[0]) ? b->bind_pos[0] : 0.0f,
                isfinite(b->bind_pos[1]) ? b->bind_pos[1] : 0.0f,
                isfinite(b->bind_pos[2]) ? b->bind_pos[2] : 0.0f,
            };
            float ang[3] = {
                isfinite(b->bind_angle[0]) ? b->bind_angle[0] : 0.0f,
                isfinite(b->bind_angle[1]) ? b->bind_angle[1] : 0.0f,
                isfinite(b->bind_angle[2]) ? b->bind_angle[2] : 0.0f,
            };
            rig_mat4_from_trs(pos, ang, scl, loc);
        }
        if (b->parent == 0xFFFFFFFFu) {
            memcpy(out_world_bind[k], loc, sizeof(out_world_bind[k]));
        } else {
            we_mat4_mul(out_world_bind[b->parent], loc, out_world_bind[k]);
        }
    }
}

static bool refresh_puppet_bind_matrices(we_puppet_t *puppet, const uint32_t topo[WE_MDL_MAX_BONES])
{
    float world_bind[WE_MDL_MAX_BONES][16];
    compute_bind_world_matrices(puppet, topo, world_bind);
    uint32_t bcnt = puppet->bone_count;
    for (uint32_t i = 0; i < bcnt; i++) {
        we_mdl_bone_t *b = &puppet->bones[i];
        if (!mat4_is_finite(b->local_trans)) {
            float scl[3] = {
                (isfinite(b->bind_scale[0]) && fabsf(b->bind_scale[0]) > 1e-6f) ? b->bind_scale[0]
                                                                                : 1.0f,
                (isfinite(b->bind_scale[1]) && fabsf(b->bind_scale[1]) > 1e-6f) ? b->bind_scale[1]
                                                                                : 1.0f,
                (isfinite(b->bind_scale[2]) && fabsf(b->bind_scale[2]) > 1e-6f) ? b->bind_scale[2]
                                                                                : 1.0f,
            };
            rig_mat4_from_trs(b->bind_pos, b->bind_angle, scl, b->local_trans);
        }
        if (!mat4_is_finite(b->world_bind)) {
            memcpy(b->world_bind, world_bind[i], sizeof(b->world_bind));
        }
        if (!mat4_is_finite(b->offset_trans)) {
            if (!we_mat4_inverse(b->world_bind, b->offset_trans)) {
                rig_mat4_identity(b->offset_trans);
            }
        }
    }
    return true;
}

/* ========================================================================= */
/* 2. Cotangent iDT Mesh Operators, Visibility Raycast & Sparse Solvers      */
/* ========================================================================= */

typedef struct {
    uint32_t u;
    uint32_t v;
} boundary_edge_t;

typedef struct {
    uint32_t key_lo;
    uint32_t key_hi;
    uint32_t he0;
    uint32_t count;
} edge_hash_slot_t;

typedef struct {
    double ax, ay;
    double bx, by;
    bool is_point;
} bone_seg2d_t;

typedef struct {
    uint32_t *cell_head;
    uint32_t *cell_count;
    uint32_t *cell_edges;
    uint32_t total_refs;
    double min_x, min_y;
    double inv_cell_w, inv_cell_h;
} boundary_grid_t;

typedef struct {
    uint32_t *row_ptr;
    uint32_t *col_idx;
    double *edge_w;
    double *diag_L;
} laplacian_csr_t;

static inline uint32_t hash_u32_pair(uint32_t a, uint32_t b)
{
    uint64_t x = ((uint64_t)a << 32u) | (uint64_t)b;
    x ^= x >> 33u;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33u;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33u;
    return (uint32_t)x;
}

static inline uint32_t next_pow2_u32(uint32_t v)
{
    uint32_t p = 1u;
    while (p < v && p < (1u << 30u)) {
        p <<= 1u;
    }
    return p;
}

static bool build_intrinsic_mesh_and_boundaries(const we_puppet_t *puppet, uint32_t *tri_verts,
                                                uint32_t *opp_he, double *he_len2,
                                                boundary_edge_t **out_b_edges,
                                                uint32_t *out_b_count)
{
    uint32_t T = puppet->triangle_count;
    uint32_t he_count = T * 3u;
    uint32_t cap = next_pow2_u32(he_count * 2u + 16u);
    edge_hash_slot_t *ht = calloc(cap, sizeof(edge_hash_slot_t));
    if (!ht) {
        return false;
    }
    uint32_t mask = cap - 1u;

    for (uint32_t h = 0; h < he_count; h++) {
        opp_he[h] = WE_RIG_EMPTY_EDGE;
    }

    for (uint32_t t = 0; t < T; t++) {
        uint32_t i0 = puppet->indices[t * 3u + 0u];
        uint32_t i1 = puppet->indices[t * 3u + 1u];
        uint32_t i2 = puppet->indices[t * 3u + 2u];
        tri_verts[t * 3u + 0u] = i0;
        tri_verts[t * 3u + 1u] = i1;
        tri_verts[t * 3u + 2u] = i2;

        for (uint32_t c = 0; c < 3u; c++) {
            uint32_t h = t * 3u + c;
            uint32_t a = tri_verts[t * 3u + c];
            uint32_t b = tri_verts[t * 3u + ((c + 1u) % 3u)];
            double dx = (double)puppet->vertices[b].pos[0] - (double)puppet->vertices[a].pos[0];
            double dy = (double)puppet->vertices[b].pos[1] - (double)puppet->vertices[a].pos[1];
            double dz = (double)puppet->vertices[b].pos[2] - (double)puppet->vertices[a].pos[2];
            he_len2[h] = dx * dx + dy * dy + dz * dz;

            uint32_t lo = a < b ? a : b;
            uint32_t hi = a < b ? b : a;
            if (lo == hi) {
                continue;
            }
            uint32_t slot = hash_u32_pair(lo, hi) & mask;
            while (ht[slot].count != 0u && (ht[slot].key_lo != lo || ht[slot].key_hi != hi)) {
                slot = (slot + 1u) & mask;
            }
            if (ht[slot].count == 0u) {
                ht[slot].key_lo = lo;
                ht[slot].key_hi = hi;
                ht[slot].he0 = h;
                ht[slot].count = 1u;
            } else if (ht[slot].count == 1u) {
                uint32_t h0 = ht[slot].he0;
                opp_he[h0] = h;
                opp_he[h] = h0;
                ht[slot].count = 2u;
            } else {
                /* Non-manifold edge: unlink so iDT never flips across >2 faces */
                uint32_t h0 = ht[slot].he0;
                if (opp_he[h0] != WE_RIG_EMPTY_EDGE) {
                    opp_he[opp_he[h0]] = WE_RIG_EMPTY_EDGE;
                    opp_he[h0] = WE_RIG_EMPTY_EDGE;
                }
                opp_he[h] = WE_RIG_EMPTY_EDGE;
                ht[slot].count++;
            }
        }
    }

    uint32_t b_cap = 0u;
    for (uint32_t s = 0; s < cap; s++) {
        if (ht[s].count == 1u) {
            b_cap++;
        }
    }
    boundary_edge_t *b_edges = NULL;
    if (b_cap > 0u) {
        b_edges = malloc((size_t)b_cap * sizeof(boundary_edge_t));
        if (!b_edges) {
            free(ht);
            return false;
        }
        uint32_t w = 0u;
        for (uint32_t s = 0; s < cap; s++) {
            if (ht[s].count == 1u) {
                b_edges[w].u = ht[s].key_lo;
                b_edges[w].v = ht[s].key_hi;
                w++;
            }
        }
    }

    free(ht);
    *out_b_edges = b_edges;
    *out_b_count = b_cap;
    return true;
}

static inline double heron_area_sq16(double a2, double b2, double c2)
{
    double s = a2 * b2 + b2 * c2 + c2 * a2;
    return 2.0 * s - (a2 * a2 + b2 * b2 + c2 * c2);
}

static inline void he_neighbors(uint32_t h, uint32_t *out_next, uint32_t *out_prev)
{
    uint32_t t = h / 3u;
    uint32_t c = h % 3u;
    *out_next = t * 3u + ((c + 1u) % 3u);
    *out_prev = t * 3u + ((c + 2u) % 3u);
}

static uint32_t apply_idt_flips(uint32_t T, uint32_t *tri_verts, uint32_t *opp_he, double *he_len2)
{
    uint32_t he_count = T * 3u;
    if (he_count == 0u) {
        return 0u;
    }

    uint32_t *queue = malloc((size_t)he_count * sizeof(uint32_t));
    uint8_t *in_q = calloc(he_count, sizeof(uint8_t));
    if (!queue || !in_q) {
        free(queue);
        free(in_q);
        return 0u;
    }

    uint32_t q_head = 0u, q_tail = 0u;
    for (uint32_t h = 0; h < he_count; h++) {
        uint32_t h2 = opp_he[h];
        if (h2 != WE_RIG_EMPTY_EDGE && h < h2) {
            queue[q_tail++] = h;
            in_q[h] = 1u;
        }
    }

    uint32_t flips = 0u;
    uint32_t max_flips = T * 10u + 64u;
    while (q_head != q_tail && flips < max_flips) {
        uint32_t h = queue[q_head];
        q_head = (q_head + 1u) % he_count;
        in_q[h] = 0u;

        uint32_t h2 = opp_he[h];
        if (h2 == WE_RIG_EMPTY_EDGE || h / 3u == h2 / 3u) {
            continue;
        }

        uint32_t h_next, h_prev, h2_next, h2_prev;
        he_neighbors(h, &h_next, &h_prev);
        he_neighbors(h2, &h2_next, &h2_prev);

        /* Opposite vertices across the shared diagonal */
        uint32_t vc = tri_verts[h_prev];
        uint32_t vd = tri_verts[h2_prev];
        if (vc == vd) {
            continue;
        }

        double l_ab2 = he_len2[h];
        double l_bc2 = he_len2[h_next];
        double l_ca2 = he_len2[h_prev];
        double l_ad2 = he_len2[h2_next];
        double l_db2 = he_len2[h2_prev];

        double a1_16 = heron_area_sq16(l_ab2, l_bc2, l_ca2);
        double a2_16 = heron_area_sq16(l_ab2, l_ad2, l_db2);
        if (a1_16 <= 1e-24 || a2_16 <= 1e-24) {
            continue;
        }

        double A1 = 0.25 * sqrt(a1_16);
        double A2 = 0.25 * sqrt(a2_16);
        double cot_a = (l_ca2 + l_bc2 - l_ab2) / (4.0 * A1);
        double cot_b = (l_ad2 + l_db2 - l_ab2) / (4.0 * A2);
        if (cot_a + cot_b >= -1e-9) {
            continue;
        }

        /* Bobenko-Springborn intrinsic flip: compute diagonal l_cd^2 via corner angles at a */
        double denom1 = sqrt(l_ca2 * l_ab2);
        double denom2 = sqrt(l_ad2 * l_ab2);
        if (denom1 <= 1e-18 || denom2 <= 1e-18) {
            continue;
        }
        double cos_a1 = clampd((l_ca2 + l_ab2 - l_bc2) / (2.0 * denom1), -1.0, 1.0);
        double sin_a1 = clampd((2.0 * A1) / denom1, 0.0, 1.0);
        double cos_a2 = clampd((l_ad2 + l_ab2 - l_db2) / (2.0 * denom2), -1.0, 1.0);
        double sin_a2 = clampd((2.0 * A2) / denom2, 0.0, 1.0);

        double cos_sum = cos_a1 * cos_a2 - sin_a1 * sin_a2;
        double l_cd2 = l_ca2 + l_ad2 - 2.0 * sqrt(l_ca2 * l_ad2) * cos_sum;
        if (!isfinite(l_cd2) || l_cd2 <= 1e-18 * (l_ca2 + l_ad2)) {
            continue;
        }

        /* Flip only when the resulting intrinsic triangles form a strictly valid convex quad */
        if (heron_area_sq16(l_cd2, l_ca2, l_ad2) <= 1e-24 ||
            heron_area_sq16(l_cd2, l_db2, l_bc2) <= 1e-24) {
            continue;
        }

        uint32_t t1 = h / 3u;
        uint32_t t2 = h2 / 3u;
        uint32_t va = tri_verts[h];
        uint32_t vb = tri_verts[h2];
        uint32_t opp_bc = opp_he[h_next];
        uint32_t opp_ca = opp_he[h_prev];
        uint32_t opp_ad = opp_he[h2_next];
        uint32_t opp_db = opp_he[h2_prev];

        /* Rewire t1 = (c, a, d), t2 = (d, b, c) with shared diagonal t1:2->0 (d->c) / t2:2->0
         * (c->d)
         */
        tri_verts[t1 * 3u + 0u] = vc;
        tri_verts[t1 * 3u + 1u] = va;
        tri_verts[t1 * 3u + 2u] = vd;
        he_len2[t1 * 3u + 0u] = l_ca2;
        he_len2[t1 * 3u + 1u] = l_ad2;
        he_len2[t1 * 3u + 2u] = l_cd2;

        tri_verts[t2 * 3u + 0u] = vd;
        tri_verts[t2 * 3u + 1u] = vb;
        tri_verts[t2 * 3u + 2u] = vc;
        he_len2[t2 * 3u + 0u] = l_db2;
        he_len2[t2 * 3u + 1u] = l_bc2;
        he_len2[t2 * 3u + 2u] = l_cd2;

        uint32_t e_ca = t1 * 3u + 0u;
        uint32_t e_ad = t1 * 3u + 1u;
        uint32_t e_dc = t1 * 3u + 2u;
        uint32_t e_db = t2 * 3u + 0u;
        uint32_t e_bc = t2 * 3u + 1u;
        uint32_t e_cd = t2 * 3u + 2u;

        opp_he[e_ca] = opp_ca;
        if (opp_ca != WE_RIG_EMPTY_EDGE) {
            opp_he[opp_ca] = e_ca;
        }
        opp_he[e_ad] = opp_ad;
        if (opp_ad != WE_RIG_EMPTY_EDGE) {
            opp_he[opp_ad] = e_ad;
        }
        opp_he[e_db] = opp_db;
        if (opp_db != WE_RIG_EMPTY_EDGE) {
            opp_he[opp_db] = e_db;
        }
        opp_he[e_bc] = opp_bc;
        if (opp_bc != WE_RIG_EMPTY_EDGE) {
            opp_he[opp_bc] = e_bc;
        }
        opp_he[e_dc] = e_cd;
        opp_he[e_cd] = e_dc;

        uint32_t nbrs[4] = {e_ca, e_ad, e_db, e_bc};
        for (int k = 0; k < 4; k++) {
            uint32_t nh = nbrs[k];
            uint32_t no = opp_he[nh];
            if (no != WE_RIG_EMPTY_EDGE) {
                uint32_t rep = nh < no ? nh : no;
                if (!in_q[rep]) {
                    queue[q_tail] = rep;
                    q_tail = (q_tail + 1u) % he_count;
                    in_q[rep] = 1u;
                }
            }
        }
        flips++;
    }

    free(queue);
    free(in_q);
    return flips;
}

static void compute_mixed_voronoi_mass(const we_puppet_t *puppet, double *out_mass)
{
    uint32_t V = puppet->vertex_count;
    uint32_t T = puppet->triangle_count;
    memset(out_mass, 0, (size_t)V * sizeof(double));

    double total_area = 0.0;
    uint32_t valid_tris = 0u;

    for (uint32_t t = 0; t < T; t++) {
        uint32_t i0 = puppet->indices[t * 3u + 0u];
        uint32_t i1 = puppet->indices[t * 3u + 1u];
        uint32_t i2 = puppet->indices[t * 3u + 2u];
        const float *p0 = puppet->vertices[i0].pos;
        const float *p1 = puppet->vertices[i1].pos;
        const float *p2 = puppet->vertices[i2].pos;

        double e01x = (double)p1[0] - (double)p0[0];
        double e01y = (double)p1[1] - (double)p0[1];
        double e01z = (double)p1[2] - (double)p0[2];
        double e12x = (double)p2[0] - (double)p1[0];
        double e12y = (double)p2[1] - (double)p1[1];
        double e12z = (double)p2[2] - (double)p1[2];
        double e20x = (double)p0[0] - (double)p2[0];
        double e20y = (double)p0[1] - (double)p2[1];
        double e20z = (double)p0[2] - (double)p2[2];

        double l01_2 = e01x * e01x + e01y * e01y + e01z * e01z;
        double l12_2 = e12x * e12x + e12y * e12y + e12z * e12z;
        double l20_2 = e20x * e20x + e20y * e20y + e20z * e20z;

        double cx = e01y * (-e20z) - e01z * (-e20y);
        double cy = e01z * (-e20x) - e01x * (-e20z);
        double cz = e01x * (-e20y) - e01y * (-e20x);
        double area = 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
        if (area <= 1e-16) {
            continue;
        }
        total_area += area;
        valid_tris++;

        /* Meyer et al. (2003) mixed Voronoi region avoids negative areas on obtuse triangles */
        double d0 = l01_2 + l20_2 - l12_2;
        double d1 = l01_2 + l12_2 - l20_2;
        double d2 = l12_2 + l20_2 - l01_2;

        if (d0 < 0.0) {
            out_mass[i0] += 0.5 * area;
            out_mass[i1] += 0.25 * area;
            out_mass[i2] += 0.25 * area;
        } else if (d1 < 0.0) {
            out_mass[i1] += 0.5 * area;
            out_mass[i0] += 0.25 * area;
            out_mass[i2] += 0.25 * area;
        } else if (d2 < 0.0) {
            out_mass[i2] += 0.5 * area;
            out_mass[i0] += 0.25 * area;
            out_mass[i1] += 0.25 * area;
        } else {
            double cot0 = d0 / (4.0 * area);
            double cot1 = d1 / (4.0 * area);
            double cot2 = d2 / (4.0 * area);
            out_mass[i0] += 0.125 * (l20_2 * cot1 + l01_2 * cot2);
            out_mass[i1] += 0.125 * (l01_2 * cot2 + l12_2 * cot0);
            out_mass[i2] += 0.125 * (l12_2 * cot0 + l20_2 * cot1);
        }
    }

    double mean_area = (valid_tris > 0u) ? (total_area / (double)valid_tris) : 1.0;
    double floor_mass = fmax(1e-8 * mean_area, 1e-12);
    for (uint32_t i = 0; i < V; i++) {
        if (out_mass[i] < floor_mass || !isfinite(out_mass[i])) {
            out_mass[i] = floor_mass;
        }
    }
}

typedef struct {
    uint32_t v;
    double w;
} adj_entry_t;

static bool assemble_cotangent_laplacian(uint32_t V, uint32_t T, const uint32_t *tri_verts,
                                         const double *he_len2, laplacian_csr_t *out_L)
{
    memset(out_L, 0, sizeof(*out_L));
    uint32_t raw_cap = T * 6u;
    uint32_t *row_cnt = calloc((size_t)V + 1u, sizeof(uint32_t));
    uint32_t *row_cur = calloc((size_t)V + 1u, sizeof(uint32_t));
    adj_entry_t *entries = raw_cap > 0u ? malloc((size_t)raw_cap * sizeof(adj_entry_t)) : NULL;
    double *tri_w = T > 0u ? malloc((size_t)T * 3u * sizeof(double)) : NULL;
    uint8_t *tri_valid = T > 0u ? calloc(T, sizeof(uint8_t)) : NULL;
    if (!row_cnt || !row_cur || (raw_cap > 0u && !entries) || (T > 0u && (!tri_w || !tri_valid))) {
        free(row_cnt);
        free(row_cur);
        free(entries);
        free(tri_w);
        free(tri_valid);
        return false;
    }

    for (uint32_t t = 0; t < T; t++) {
        uint32_t v0 = tri_verts[t * 3u + 0u];
        uint32_t v1 = tri_verts[t * 3u + 1u];
        uint32_t v2 = tri_verts[t * 3u + 2u];
        if (v0 >= V || v1 >= V || v2 >= V || v0 == v1 || v1 == v2 || v2 == v0) {
            continue;
        }
        double l01_2 = he_len2[t * 3u + 0u];
        double l12_2 = he_len2[t * 3u + 1u];
        double l20_2 = he_len2[t * 3u + 2u];

        double a16 = heron_area_sq16(l01_2, l12_2, l20_2);
        if (a16 <= 1e-24) {
            continue;
        }
        double four_A = sqrt(a16);
        if (!isfinite(four_A) || four_A <= 0.0) {
            continue;
        }
        tri_w[t * 3u + 0u] = 0.5 * (l20_2 + l12_2 - l01_2) / four_A;
        tri_w[t * 3u + 1u] = 0.5 * (l01_2 + l20_2 - l12_2) / four_A;
        tri_w[t * 3u + 2u] = 0.5 * (l12_2 + l01_2 - l20_2) / four_A;
        tri_valid[t] = 1u;

        row_cnt[v0] += 2u;
        row_cnt[v1] += 2u;
        row_cnt[v2] += 2u;
    }

    uint32_t total_raw = 0u;
    for (uint32_t u = 0; u < V; u++) {
        row_cur[u] = total_raw;
        total_raw += row_cnt[u];
    }
    row_cur[V] = total_raw;

    for (uint32_t t = 0; t < T; t++) {
        if (!tri_valid[t]) {
            continue;
        }
        uint32_t v0 = tri_verts[t * 3u + 0u];
        uint32_t v1 = tri_verts[t * 3u + 1u];
        uint32_t v2 = tri_verts[t * 3u + 2u];
        double w01 = tri_w[t * 3u + 0u];
        double w12 = tri_w[t * 3u + 1u];
        double w20 = tri_w[t * 3u + 2u];
        entries[row_cur[v0]++] = (adj_entry_t){v1, w01};
        entries[row_cur[v1]++] = (adj_entry_t){v0, w01};
        entries[row_cur[v1]++] = (adj_entry_t){v2, w12};
        entries[row_cur[v2]++] = (adj_entry_t){v1, w12};
        entries[row_cur[v2]++] = (adj_entry_t){v0, w20};
        entries[row_cur[v0]++] = (adj_entry_t){v2, w20};
    }
    free(tri_w);
    free(tri_valid);

    /* Insertion-sort each small vertex adjacency list and deduplicate in-place */
    uint32_t unique_edges = 0u;
    uint32_t row_start = 0u;
    for (uint32_t u = 0; u < V; u++) {
        uint32_t row_end = row_start + row_cnt[u];
        for (uint32_t i = row_start + 1u; i < row_end; i++) {
            adj_entry_t key = entries[i];
            uint32_t j = i;
            while (j > row_start && entries[j - 1u].v > key.v) {
                entries[j] = entries[j - 1u];
                j--;
            }
            entries[j] = key;
        }
        uint32_t u_out = unique_edges;
        for (uint32_t i = row_start; i < row_end;) {
            uint32_t nbr = entries[i].v;
            double sum_w = 0.0;
            while (i < row_end && entries[i].v == nbr) {
                sum_w += entries[i].w;
                i++;
            }
            /* Clamp residual negative cotangents on boundary/non-manifold edges so L remains an
             * M-matrix with guaranteed non-negative weights [0, 1]. */
            sum_w = fmax(0.0, sum_w);
            if (sum_w > 0.0) {
                entries[unique_edges++] = (adj_entry_t){nbr, sum_w};
            }
        }
        row_cur[u] = unique_edges - u_out;
        row_start = row_end;
    }

    out_L->row_ptr = calloc((size_t)V + 1u, sizeof(uint32_t));
    out_L->col_idx = unique_edges > 0u ? malloc((size_t)unique_edges * sizeof(uint32_t)) : NULL;
    out_L->edge_w = unique_edges > 0u ? malloc((size_t)unique_edges * sizeof(double)) : NULL;
    out_L->diag_L = calloc(V, sizeof(double));
    if (!out_L->row_ptr || !out_L->diag_L ||
        (unique_edges > 0u && (!out_L->col_idx || !out_L->edge_w))) {
        free(row_cnt);
        free(row_cur);
        free(entries);
        free(out_L->row_ptr);
        free(out_L->col_idx);
        free(out_L->edge_w);
        free(out_L->diag_L);
        memset(out_L, 0, sizeof(*out_L));
        return false;
    }

    uint32_t pos = 0u;
    for (uint32_t u = 0; u < V; u++) {
        out_L->row_ptr[u] = pos;
        uint32_t cnt = row_cur[u];
        double d_sum = 0.0;
        for (uint32_t k = 0; k < cnt; k++) {
            out_L->col_idx[pos] = entries[pos].v;
            out_L->edge_w[pos] = entries[pos].w;
            d_sum += entries[pos].w;
            pos++;
        }
        out_L->diag_L[u] = d_sum;
    }
    out_L->row_ptr[V] = pos;

    free(row_cnt);
    free(row_cur);
    free(entries);
    return true;
}

static void free_laplacian_csr(laplacian_csr_t *L)
{
    free(L->row_ptr);
    free(L->col_idx);
    free(L->edge_w);
    free(L->diag_L);
    memset(L, 0, sizeof(*L));
}

static bool build_boundary_dda_grid(const we_puppet_t *puppet, const boundary_edge_t *b_edges,
                                    uint32_t b_count, double min_x, double min_y, double max_x,
                                    double max_y, boundary_grid_t *out_grid)
{
    memset(out_grid, 0, sizeof(*out_grid));
    uint32_t num_cells = WE_RIG_DDA_GRID_N * WE_RIG_DDA_GRID_N;
    out_grid->cell_head = calloc((size_t)num_cells + 1u, sizeof(uint32_t));
    out_grid->cell_count = calloc(num_cells, sizeof(uint32_t));
    if (!out_grid->cell_head || !out_grid->cell_count) {
        free(out_grid->cell_head);
        free(out_grid->cell_count);
        return false;
    }

    double w = fmax(max_x - min_x, 1e-6);
    double h = fmax(max_y - min_y, 1e-6);
    double pad = 1e-4 * fmax(w, h);
    out_grid->min_x = min_x - pad;
    out_grid->min_y = min_y - pad;
    out_grid->inv_cell_w = (double)WE_RIG_DDA_GRID_N / (w + 2.0 * pad);
    out_grid->inv_cell_h = (double)WE_RIG_DDA_GRID_N / (h + 2.0 * pad);

    if (b_count == 0u) {
        return true;
    }

    uint32_t total_refs = 0u;
    for (uint32_t e = 0; e < b_count; e++) {
        const float *pa = puppet->vertices[b_edges[e].u].pos;
        const float *pb = puppet->vertices[b_edges[e].v].pos;
        double x0 = fmin((double)pa[0], (double)pb[0]);
        double x1 = fmax((double)pa[0], (double)pb[0]);
        double y0 = fmin((double)pa[1], (double)pb[1]);
        double y1 = fmax((double)pa[1], (double)pb[1]);

        int cx0 = (int)floor((x0 - out_grid->min_x) * out_grid->inv_cell_w);
        int cx1 = (int)floor((x1 - out_grid->min_x) * out_grid->inv_cell_w);
        int cy0 = (int)floor((y0 - out_grid->min_y) * out_grid->inv_cell_h);
        int cy1 = (int)floor((y1 - out_grid->min_y) * out_grid->inv_cell_h);

        cx0 = cx0 < 0 ? 0 : (cx0 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cx0);
        cx1 = cx1 < 0 ? 0 : (cx1 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cx1);
        cy0 = cy0 < 0 ? 0 : (cy0 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cy0);
        cy1 = cy1 < 0 ? 0 : (cy1 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cy1);

        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                out_grid->cell_count[(uint32_t)cy * WE_RIG_DDA_GRID_N + (uint32_t)cx]++;
                total_refs++;
            }
        }
    }

    out_grid->total_refs = total_refs;
    out_grid->cell_edges = total_refs > 0u ? malloc((size_t)total_refs * sizeof(uint32_t)) : NULL;
    if (total_refs > 0u && !out_grid->cell_edges) {
        free(out_grid->cell_head);
        free(out_grid->cell_count);
        memset(out_grid, 0, sizeof(*out_grid));
        return false;
    }

    for (uint32_t c = 0; c < num_cells; c++) {
        out_grid->cell_head[c + 1u] = out_grid->cell_head[c] + out_grid->cell_count[c];
        out_grid->cell_count[c] = 0u;
    }

    for (uint32_t e = 0; e < b_count; e++) {
        const float *pa = puppet->vertices[b_edges[e].u].pos;
        const float *pb = puppet->vertices[b_edges[e].v].pos;
        double x0 = fmin((double)pa[0], (double)pb[0]);
        double x1 = fmax((double)pa[0], (double)pb[0]);
        double y0 = fmin((double)pa[1], (double)pb[1]);
        double y1 = fmax((double)pa[1], (double)pb[1]);

        int cx0 = (int)floor((x0 - out_grid->min_x) * out_grid->inv_cell_w);
        int cx1 = (int)floor((x1 - out_grid->min_x) * out_grid->inv_cell_w);
        int cy0 = (int)floor((y0 - out_grid->min_y) * out_grid->inv_cell_h);
        int cy1 = (int)floor((y1 - out_grid->min_y) * out_grid->inv_cell_h);

        cx0 = cx0 < 0 ? 0 : (cx0 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cx0);
        cx1 = cx1 < 0 ? 0 : (cx1 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cx1);
        cy0 = cy0 < 0 ? 0 : (cy0 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cy0);
        cy1 = cy1 < 0 ? 0 : (cy1 >= (int)WE_RIG_DDA_GRID_N ? (int)WE_RIG_DDA_GRID_N - 1 : cy1);

        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                uint32_t cid = (uint32_t)cy * WE_RIG_DDA_GRID_N + (uint32_t)cx;
                out_grid->cell_edges[out_grid->cell_head[cid] + out_grid->cell_count[cid]++] = e;
            }
        }
    }

    return true;
}

static void free_boundary_dda_grid(boundary_grid_t *g)
{
    free(g->cell_head);
    free(g->cell_count);
    free(g->cell_edges);
    memset(g, 0, sizeof(*g));
}

static bool ray_crosses_boundary_edge(double p0x, double p0y, double p1x, double p1y, double eax,
                                      double eay, double ebx, double eby)
{
    double rx = p1x - p0x;
    double ry = p1y - p0y;
    double sx = ebx - eax;
    double sy = eby - eay;
    double denom = rx * sy - ry * sx;
    double qpx = eax - p0x;
    double qpy = eay - p0y;
    double scale = (rx * rx + ry * ry) * (sx * sx + sy * sy);
    if (scale <= 0.0 || denom * denom <= 1e-18 * scale) {
        return false;
    }
    double inv_d = 1.0 / denom;
    double t = (qpx * sy - qpy * sx) * inv_d;
    double u = (qpx * ry - qpy * rx) * inv_d;
    return (t > 1e-5 && t < 1.0 - 1e-5 && u >= -1e-5 && u <= 1.0 + 1e-5);
}

static bool is_ray_visible_dda(const we_puppet_t *puppet, uint32_t vid, double qx, double qy,
                               const boundary_edge_t *b_edges, const boundary_grid_t *grid,
                               uint32_t *edge_stamp, uint32_t stamp_id)
{
    double p0x = (double)puppet->vertices[vid].pos[0];
    double p0y = (double)puppet->vertices[vid].pos[1];
    double dx = qx - p0x;
    double dy = qy - p0y;
    if (dx * dx + dy * dy <= 1e-16) {
        return true;
    }

    /* Convert endpoints into grid coordinate space [0, N] and clip p1 if outside */
    double gx0 = (p0x - grid->min_x) * grid->inv_cell_w;
    double gy0 = (p0y - grid->min_y) * grid->inv_cell_h;
    double gx1 = (qx - grid->min_x) * grid->inv_cell_w;
    double gy1 = (qy - grid->min_y) * grid->inv_cell_h;

    double gdx = gx1 - gx0;
    double gdy = gy1 - gy0;

    int cx = (int)floor(gx0);
    int cy = (int)floor(gy0);
    int end_cx = (int)floor(gx1);
    int end_cy = (int)floor(gy1);

    int max_c = (int)WE_RIG_DDA_GRID_N - 1;
    cx = cx < 0 ? 0 : (cx > max_c ? max_c : cx);
    cy = cy < 0 ? 0 : (cy > max_c ? max_c : cy);
    end_cx = end_cx < 0 ? 0 : (end_cx > max_c ? max_c : end_cx);
    end_cy = end_cy < 0 ? 0 : (end_cy > max_c ? max_c : end_cy);

    int step_x = (gdx > 1e-12) ? 1 : ((gdx < -1e-12) ? -1 : 0);
    int step_y = (gdy > 1e-12) ? 1 : ((gdy < -1e-12) ? -1 : 0);

    double t_max_x = 1e30;
    double t_delta_x = 1e30;
    if (step_x != 0) {
        double next_bx = (step_x > 0) ? (double)(cx + 1) : (double)cx;
        t_max_x = (next_bx - gx0) / gdx;
        t_delta_x = fabs(1.0 / gdx);
    }

    double t_max_y = 1e30;
    double t_delta_y = 1e30;
    if (step_y != 0) {
        double next_by = (step_y > 0) ? (double)(cy + 1) : (double)cy;
        t_max_y = (next_by - gy0) / gdy;
        t_delta_y = fabs(1.0 / gdy);
    }

    uint32_t max_steps = WE_RIG_DDA_GRID_N * 4u;
    for (uint32_t step = 0; step < max_steps; step++) {
        uint32_t cid = (uint32_t)cy * WE_RIG_DDA_GRID_N + (uint32_t)cx;
        uint32_t start = grid->cell_head[cid];
        uint32_t end = grid->cell_head[cid + 1u];
        for (uint32_t idx = start; idx < end; idx++) {
            uint32_t e = grid->cell_edges[idx];
            if (edge_stamp[e] == stamp_id) {
                continue;
            }
            edge_stamp[e] = stamp_id;
            uint32_t eu = b_edges[e].u;
            uint32_t ev = b_edges[e].v;
            if (eu == vid || ev == vid) {
                continue;
            }
            const float *pa = puppet->vertices[eu].pos;
            const float *pb = puppet->vertices[ev].pos;
            if (ray_crosses_boundary_edge(p0x, p0y, qx, qy, (double)pa[0], (double)pa[1],
                                          (double)pb[0], (double)pb[1])) {
                return false;
            }
        }

        if (cx == end_cx && cy == end_cy) {
            break;
        }
        if (t_max_x < t_max_y) {
            t_max_x += t_delta_x;
            cx += step_x;
            if (cx < 0 || cx > max_c) {
                break;
            }
        } else {
            t_max_y += t_delta_y;
            cy += step_y;
            if (cy < 0 || cy > max_c) {
                break;
            }
        }
    }

    return true;
}

static void build_bone_segments_2d(const we_puppet_t *puppet, const uint32_t topo[WE_MDL_MAX_BONES],
                                   double min_x, double min_y, double max_x, double max_y,
                                   bone_seg2d_t out_segs[WE_MDL_MAX_BONES])
{
    float world_bind[WE_MDL_MAX_BONES][16];
    compute_bind_world_matrices(puppet, topo, world_bind);

    uint32_t bcnt = puppet->bone_count;
    double mesh_diag = hypot(max_x - min_x, max_y - min_y);
    if (mesh_diag < 1e-6) {
        mesh_diag = 1.0;
    }

    for (uint32_t b = 0; b < bcnt; b++) {
        double ax = (double)world_bind[b][12];
        double ay = (double)world_bind[b][13];
        out_segs[b].ax = ax;
        out_segs[b].ay = ay;

        double sum_cx = 0.0, sum_cy = 0.0;
        uint32_t child_cnt = 0u;
        for (uint32_t c = 0; c < bcnt; c++) {
            if (c != b && puppet->bones[c].parent == b) {
                sum_cx += (double)world_bind[c][12];
                sum_cy += (double)world_bind[c][13];
                child_cnt++;
            }
        }

        if (child_cnt >= 1u) {
            double bx = sum_cx / (double)child_cnt;
            double by = sum_cy / (double)child_cnt;
            double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
            out_segs[b].bx = bx;
            out_segs[b].by = by;
            out_segs[b].is_point = (len2 <= 1e-12);
        } else if (puppet->bones[b].parent != 0xFFFFFFFFu && puppet->bones[b].parent < bcnt) {
            uint32_t p = puppet->bones[b].parent;
            double px = (double)world_bind[p][12];
            double py = (double)world_bind[p][13];
            double dx = ax - px;
            double dy = ay - py;
            double plen = hypot(dx, dy);
            if (plen > 1e-4) {
                double ux = dx / plen;
                double uy = dy / plen;
                /* Extend leaf bone along parent direction up to the mesh extent so leaf bones
                 * placed inside a limb own the distal vertices without tying with the parent. */
                double max_ext = 0.0;
                for (uint32_t v = 0; v < puppet->vertex_count; v++) {
                    double vx = (double)puppet->vertices[v].pos[0] - ax;
                    double vy = (double)puppet->vertices[v].pos[1] - ay;
                    double proj = vx * ux + vy * uy;
                    if (proj > max_ext) {
                        max_ext = proj;
                    }
                }
                double ext = fmin(plen, fmin(max_ext, mesh_diag));
                if (ext > 1e-3 * plen) {
                    out_segs[b].bx = ax + ux * ext;
                    out_segs[b].by = ay + uy * ext;
                    out_segs[b].is_point = false;
                } else {
                    out_segs[b].bx = ax;
                    out_segs[b].by = ay;
                    out_segs[b].is_point = true;
                }
            } else {
                out_segs[b].bx = ax;
                out_segs[b].by = ay;
                out_segs[b].is_point = true;
            }
        } else {
            out_segs[b].bx = ax;
            out_segs[b].by = ay;
            out_segs[b].is_point = true;
        }
    }
}

static inline void closest_pt_seg2d(double px, double py, const bone_seg2d_t *s, double *out_qx,
                                    double *out_qy, double *out_dist)
{
    if (s->is_point) {
        *out_qx = s->ax;
        *out_qy = s->ay;
        *out_dist = hypot(px - s->ax, py - s->ay);
        return;
    }
    double abx = s->bx - s->ax;
    double aby = s->by - s->ay;
    double len2 = abx * abx + aby * aby;
    double t =
        (len2 > 1e-18) ? clampd(((px - s->ax) * abx + (py - s->ay) * aby) / len2, 0.0, 1.0) : 0.0;
    *out_qx = s->ax + t * abx;
    *out_qy = s->ay + t * aby;
    *out_dist = hypot(px - *out_qx, py - *out_qy);
}

static uint32_t find_mesh_components(uint32_t V, const laplacian_csr_t *L, uint32_t *out_comp_id)
{
    for (uint32_t i = 0; i < V; i++) {
        out_comp_id[i] = UINT32_MAX;
    }
    uint32_t *bfs = malloc((size_t)V * sizeof(uint32_t));
    if (!bfs) {
        for (uint32_t i = 0; i < V; i++) {
            out_comp_id[i] = 0u;
        }
        return 1u;
    }

    uint32_t comp_count = 0u;
    for (uint32_t root = 0; root < V; root++) {
        if (out_comp_id[root] != UINT32_MAX) {
            continue;
        }
        uint32_t head = 0u, tail = 0u;
        bfs[tail++] = root;
        out_comp_id[root] = comp_count;
        while (head < tail) {
            uint32_t u = bfs[head++];
            uint32_t start = L->row_ptr[u];
            uint32_t end = L->row_ptr[u + 1u];
            for (uint32_t p = start; p < end; p++) {
                uint32_t v = L->col_idx[p];
                if (out_comp_id[v] == UINT32_MAX) {
                    out_comp_id[v] = comp_count;
                    bfs[tail++] = v;
                }
            }
        }
        comp_count++;
    }
    free(bfs);
    return comp_count;
}

/* Reverse Cuthill-McKee ordering minimizes fill-in of the 2D planar stiffness matrix */
static bool compute_rcm_permutation(uint32_t n, const uint32_t *row_ptr, const uint32_t *col_idx,
                                    uint32_t *out_perm, uint32_t *out_inv_perm)
{
    if (n == 0u) {
        return true;
    }
    uint8_t *visited = calloc(n, sizeof(uint8_t));
    uint32_t *queue = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *nbrs = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *dist = malloc((size_t)n * sizeof(uint32_t));
    if (!visited || !queue || !nbrs || !dist) {
        free(visited);
        free(queue);
        free(nbrs);
        free(dist);
        return false;
    }

    uint32_t cm_pos = 0u;
    for (uint32_t seed = 0; seed < n; seed++) {
        if (visited[seed]) {
            continue;
        }
        /* Two-pass George-Liu pseudo-peripheral start node search */
        uint32_t start = seed;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t qh = 0u, qt = 0u;
            queue[qt++] = start;
            dist[start] = 1u;
            uint32_t far_node = start;
            uint32_t far_deg = row_ptr[start + 1u] - row_ptr[start];
            uint32_t max_d = 1u;
            while (qh < qt) {
                uint32_t u = queue[qh++];
                uint32_t d = dist[u];
                uint32_t udeg = row_ptr[u + 1u] - row_ptr[u];
                if (d > max_d || (d == max_d && udeg < far_deg)) {
                    max_d = d;
                    far_node = u;
                    far_deg = udeg;
                }
                for (uint32_t p = row_ptr[u]; p < row_ptr[u + 1u]; p++) {
                    uint32_t v = col_idx[p];
                    if (dist[v] == 0u && !visited[v]) {
                        dist[v] = d + 1u;
                        queue[qt++] = v;
                    }
                }
            }
            for (uint32_t i = 0; i < qt; i++) {
                dist[queue[i]] = 0u;
            }
            start = far_node;
        }

        uint32_t qh = 0u, qt = 0u;
        queue[qt++] = start;
        visited[start] = 1u;
        while (qh < qt) {
            uint32_t u = queue[qh++];
            out_perm[cm_pos++] = u;

            uint32_t n_cnt = 0u;
            for (uint32_t p = row_ptr[u]; p < row_ptr[u + 1u]; p++) {
                uint32_t v = col_idx[p];
                if (!visited[v]) {
                    visited[v] = 1u;
                    nbrs[n_cnt++] = v;
                }
            }
            /* Insertion sort neighbors by degree (degrees on planar meshes are small) */
            for (uint32_t i = 1u; i < n_cnt; i++) {
                uint32_t key = nbrs[i];
                uint32_t kdeg = row_ptr[key + 1u] - row_ptr[key];
                uint32_t j = i;
                while (j > 0u) {
                    uint32_t prev = nbrs[j - 1u];
                    uint32_t pdeg = row_ptr[prev + 1u] - row_ptr[prev];
                    if (pdeg <= kdeg) {
                        break;
                    }
                    nbrs[j] = prev;
                    j--;
                }
                nbrs[j] = key;
            }
            for (uint32_t i = 0; i < n_cnt; i++) {
                queue[qt++] = nbrs[i];
            }
        }
    }

    for (uint32_t i = 0; i < n / 2u; i++) {
        uint32_t tmp = out_perm[i];
        out_perm[i] = out_perm[n - 1u - i];
        out_perm[n - 1u - i] = tmp;
    }
    for (uint32_t i = 0; i < n; i++) {
        out_inv_perm[out_perm[i]] = i;
    }

    free(visited);
    free(queue);
    free(nbrs);
    free(dist);
    return true;
}

/* Solves A * W = R for all B columns in 8-wide batches using IC(0)-preconditioned CG */
static bool solve_ic0_pcg_batched(uint32_t n, const uint32_t *row_ptr, const uint32_t *col_idx,
                                  const double *val, const double *diag, uint32_t B,
                                  const double *rhs, double *out_sol)
{
    uint32_t nnz = row_ptr[n];
    double *ic_l = nnz > 0u ? malloc((size_t)nnz * sizeof(double)) : NULL;
    double *ic_d = malloc((size_t)n * sizeof(double));
    uint32_t *diag_pos = malloc((size_t)n * sizeof(uint32_t));
    if (!ic_d || !diag_pos || (nnz > 0u && !ic_l)) {
        free(ic_l);
        free(ic_d);
        free(diag_pos);
        return false;
    }

    /* Row-by-row IC(0) on symmetric CSR (col_idx sorted ascending per row) */
    for (uint32_t i = 0; i < n; i++) {
        diag_pos[i] = row_ptr[i + 1u];
        for (uint32_t p = row_ptr[i]; p < row_ptr[i + 1u]; p++) {
            if (col_idx[p] > i) {
                diag_pos[i] = p;
                break;
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        double d_i = diag[i];
        for (uint32_t p = row_ptr[i]; p < diag_pos[i]; p++) {
            uint32_t j = col_idx[p];
            double s = val[p];
            uint32_t pi = row_ptr[i];
            uint32_t pj = row_ptr[j];
            while (pi < p && pj < diag_pos[j]) {
                uint32_t ci = col_idx[pi];
                uint32_t cj = col_idx[pj];
                if (ci == cj) {
                    s -= ic_l[pi] * ic_l[pj] * ic_d[ci];
                    pi++;
                    pj++;
                } else if (ci < cj) {
                    pi++;
                } else {
                    pj++;
                }
            }
            double lij = s / ic_d[j];
            ic_l[p] = lij;
            d_i -= lij * lij * ic_d[j];
        }
        ic_d[i] = (d_i > 1e-14) ? d_i : 1e-14;
    }

    size_t stride = (size_t)n * WE_RIG_RHS_BATCH;
    double *r = malloc(stride * sizeof(double));
    double *z = malloc(stride * sizeof(double));
    double *pvec = malloc(stride * sizeof(double));
    double *ap = malloc(stride * sizeof(double));
    if (!r || !z || !pvec || !ap) {
        free(r);
        free(z);
        free(pvec);
        free(ap);
        free(ic_l);
        free(ic_d);
        free(diag_pos);
        return false;
    }

    for (uint32_t b0 = 0; b0 < B; b0 += WE_RIG_RHS_BATCH) {
        uint32_t nb = (B - b0 < WE_RIG_RHS_BATCH) ? (B - b0) : WE_RIG_RHS_BATCH;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t k = 0; k < nb; k++) {
                double rv = rhs[(size_t)(b0 + k) * n + i];
                r[(size_t)i * WE_RIG_RHS_BATCH + k] = rv;
                out_sol[(size_t)(b0 + k) * n + i] = 0.0;
            }
            for (uint32_t k = nb; k < WE_RIG_RHS_BATCH; k++) {
                r[(size_t)i * WE_RIG_RHS_BATCH + k] = 0.0;
            }
        }

        /* Forward + back IC(0) solve: z = M^{-1} r */
        memcpy(z, r, stride * sizeof(double));
        for (uint32_t i = 0; i < n; i++) {
            double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t p = row_ptr[i]; p < diag_pos[i]; p++) {
                double lij = ic_l[p];
                const double *zj = &z[(size_t)col_idx[p] * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    zi[k] -= lij * zj[k];
                }
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            double inv_d = 1.0 / ic_d[i];
            double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                zi[k] *= inv_d;
            }
        }
        for (uint32_t i = n; i > 0u; i--) {
            uint32_t row = i - 1u;
            const double *zr = &z[(size_t)row * WE_RIG_RHS_BATCH];
            for (uint32_t p = row_ptr[row]; p < diag_pos[row]; p++) {
                double lij = ic_l[p];
                double *zj = &z[(size_t)col_idx[p] * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    zj[k] -= lij * zr[k];
                }
            }
        }

        memcpy(pvec, z, stride * sizeof(double));
        double rz_old[WE_RIG_RHS_BATCH] = {0};
        double r0_norm2[WE_RIG_RHS_BATCH] = {0};
        for (uint32_t i = 0; i < n; i++) {
            const double *ri = &r[(size_t)i * WE_RIG_RHS_BATCH];
            const double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t k = 0; k < nb; k++) {
                rz_old[k] += ri[k] * zi[k];
                r0_norm2[k] += ri[k] * ri[k];
            }
        }

        const uint32_t max_iters = 600u;
        for (uint32_t iter = 0; iter < max_iters; iter++) {
            bool all_conv = true;
            for (uint32_t k = 0; k < nb; k++) {
                if (rz_old[k] > 1e-18 && r0_norm2[k] > 1e-24) {
                    all_conv = false;
                    break;
                }
            }
            if (all_conv) {
                break;
            }

            for (uint32_t i = 0; i < n; i++) {
                const double *pi_v = &pvec[(size_t)i * WE_RIG_RHS_BATCH];
                double di = diag[i];
                double acc[WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    acc[k] = di * pi_v[k];
                }
                for (uint32_t p = row_ptr[i]; p < row_ptr[i + 1u]; p++) {
                    double aij = val[p];
                    const double *pj_v = &pvec[(size_t)col_idx[p] * WE_RIG_RHS_BATCH];
                    for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                        acc[k] += aij * pj_v[k];
                    }
                }
                double *api = &ap[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    api[k] = acc[k];
                }
            }

            double pAp[WE_RIG_RHS_BATCH] = {0};
            for (uint32_t i = 0; i < n; i++) {
                const double *pi_v = &pvec[(size_t)i * WE_RIG_RHS_BATCH];
                const double *api = &ap[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < nb; k++) {
                    pAp[k] += pi_v[k] * api[k];
                }
            }

            double alpha[WE_RIG_RHS_BATCH] = {0};
            for (uint32_t k = 0; k < nb; k++) {
                alpha[k] = (pAp[k] > 1e-24) ? (rz_old[k] / pAp[k]) : 0.0;
            }

            double r_norm2[WE_RIG_RHS_BATCH] = {0};
            for (uint32_t i = 0; i < n; i++) {
                double *ri = &r[(size_t)i * WE_RIG_RHS_BATCH];
                const double *pi_v = &pvec[(size_t)i * WE_RIG_RHS_BATCH];
                const double *api = &ap[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < nb; k++) {
                    out_sol[(size_t)(b0 + k) * n + i] += alpha[k] * pi_v[k];
                    ri[k] -= alpha[k] * api[k];
                    r_norm2[k] += ri[k] * ri[k];
                }
            }

            bool done = true;
            for (uint32_t k = 0; k < nb; k++) {
                if (r_norm2[k] > 1e-12 * fmax(r0_norm2[k], 1.0)) {
                    done = false;
                    break;
                }
            }
            if (done) {
                break;
            }

            memcpy(z, r, stride * sizeof(double));
            for (uint32_t i = 0; i < n; i++) {
                double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t p = row_ptr[i]; p < diag_pos[i]; p++) {
                    double lij = ic_l[p];
                    const double *zj = &z[(size_t)col_idx[p] * WE_RIG_RHS_BATCH];
                    for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                        zi[k] -= lij * zj[k];
                    }
                }
            }
            for (uint32_t i = 0; i < n; i++) {
                double inv_d = 1.0 / ic_d[i];
                double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    zi[k] *= inv_d;
                }
            }
            for (uint32_t i = n; i > 0u; i--) {
                uint32_t row = i - 1u;
                const double *zr = &z[(size_t)row * WE_RIG_RHS_BATCH];
                for (uint32_t p = row_ptr[row]; p < diag_pos[row]; p++) {
                    double lij = ic_l[p];
                    double *zj = &z[(size_t)col_idx[p] * WE_RIG_RHS_BATCH];
                    for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                        zj[k] -= lij * zr[k];
                    }
                }
            }

            double rz_new[WE_RIG_RHS_BATCH] = {0};
            for (uint32_t i = 0; i < n; i++) {
                const double *ri = &r[(size_t)i * WE_RIG_RHS_BATCH];
                const double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < nb; k++) {
                    rz_new[k] += ri[k] * zi[k];
                }
            }
            double beta[WE_RIG_RHS_BATCH] = {0};
            for (uint32_t k = 0; k < nb; k++) {
                beta[k] = (rz_old[k] > 1e-24) ? (rz_new[k] / rz_old[k]) : 0.0;
                rz_old[k] = rz_new[k];
            }
            for (uint32_t i = 0; i < n; i++) {
                double *pi_v = &pvec[(size_t)i * WE_RIG_RHS_BATCH];
                const double *zi = &z[(size_t)i * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < nb; k++) {
                    pi_v[k] = zi[k] + beta[k] * pi_v[k];
                }
            }
        }
    }

    free(r);
    free(z);
    free(pvec);
    free(ap);
    free(ic_l);
    free(ic_d);
    free(diag_pos);
    return true;
}

static bool solve_sparse_ldlt_or_ic0(uint32_t n, const uint32_t *perm_row_ptr,
                                     const uint32_t *perm_col_idx, const double *perm_val,
                                     const double *perm_diag, uint32_t B, const double *perm_rhs,
                                     double *out_perm_sol, uint32_t *out_factor_nnz,
                                     bool *out_used_ic0)
{
    *out_factor_nnz = 0u;
    *out_used_ic0 = false;
    if (n == 0u) {
        return true;
    }

    /* Build upper-triangular CSC (row <= k for column k) from symmetric CSR */
    uint32_t *Ap = calloc((size_t)n + 1u, sizeof(uint32_t));
    uint32_t *parent = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *ancestor = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *flag = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *pattern = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *tmp_stack = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *col_nnz = calloc(n, sizeof(uint32_t));
    if (!Ap || !parent || !ancestor || !flag || !pattern || !tmp_stack || !col_nnz) {
        free(Ap);
        free(parent);
        free(ancestor);
        free(flag);
        free(pattern);
        free(tmp_stack);
        free(col_nnz);
        return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        Ap[i + 1u]++; /* diagonal */
        for (uint32_t p = perm_row_ptr[i]; p < perm_row_ptr[i + 1u]; p++) {
            uint32_t j = perm_col_idx[p];
            if (i < j) {
                Ap[j + 1u]++;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        Ap[i + 1u] += Ap[i];
    }

    uint32_t nnz_u = Ap[n];
    uint32_t *Ai = malloc((size_t)nnz_u * sizeof(uint32_t));
    double *Ax = malloc((size_t)nnz_u * sizeof(double));
    uint32_t *ap_cur = malloc((size_t)n * sizeof(uint32_t));
    if (!Ai || !Ax || !ap_cur) {
        free(Ap);
        free(parent);
        free(ancestor);
        free(flag);
        free(pattern);
        free(tmp_stack);
        free(col_nnz);
        free(Ai);
        free(Ax);
        free(ap_cur);
        return false;
    }
    memcpy(ap_cur, Ap, (size_t)n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t p = perm_row_ptr[i]; p < perm_row_ptr[i + 1u]; p++) {
            uint32_t j = perm_col_idx[p];
            if (i < j) {
                uint32_t dst = ap_cur[j]++;
                Ai[dst] = i;
                Ax[dst] = perm_val[p];
            }
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t dst = ap_cur[k]++;
        Ai[dst] = k;
        Ax[dst] = perm_diag[k];
    }
    free(ap_cur);

    /* Elimination tree (CSparse cs_etree) */
    for (uint32_t k = 0; k < n; k++) {
        parent[k] = UINT32_MAX;
        ancestor[k] = UINT32_MAX;
        for (uint32_t p = Ap[k]; p < Ap[k + 1u]; p++) {
            uint32_t i = Ai[p];
            while (i < k) {
                uint32_t inext = ancestor[i];
                ancestor[i] = k;
                if (inext == UINT32_MAX) {
                    parent[i] = k;
                    break;
                }
                i = inext;
            }
        }
    }

    /* Symbolic fill-in count */
    uint64_t fill_nnz64 = 0u;
    for (uint32_t i = 0; i < n; i++) {
        flag[i] = UINT32_MAX;
    }
    for (uint32_t k = 0; k < n; k++) {
        flag[k] = k;
        for (uint32_t p = Ap[k]; p < Ap[k + 1u]; p++) {
            uint32_t i = Ai[p];
            while (i < k && flag[i] != k) {
                flag[i] = k;
                col_nnz[i]++;
                fill_nnz64++;
                i = parent[i];
            }
        }
    }

    if (fill_nnz64 > (uint64_t)WE_RIG_HEAT_MAX_FILL_NNZ) {
        free(Ai);
        free(Ax);
        free(Ap);
        free(parent);
        free(ancestor);
        free(flag);
        free(pattern);
        free(tmp_stack);
        free(col_nnz);
        *out_used_ic0 = true;
        *out_factor_nnz = perm_row_ptr[n];
        return solve_ic0_pcg_batched(n, perm_row_ptr, perm_col_idx, perm_val, perm_diag, B,
                                     perm_rhs, out_perm_sol);
    }

    uint32_t fill_nnz = (uint32_t)fill_nnz64;
    *out_factor_nnz = fill_nnz + n;

    uint32_t *Lp = calloc((size_t)n + 1u, sizeof(uint32_t));
    uint32_t *col_cur = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *Li = fill_nnz > 0u ? malloc((size_t)fill_nnz * sizeof(uint32_t)) : NULL;
    double *Lx = fill_nnz > 0u ? malloc((size_t)fill_nnz * sizeof(double)) : NULL;
    double *D = malloc((size_t)n * sizeof(double));
    double *Y = calloc(n, sizeof(double));
    double *X = malloc((size_t)n * WE_RIG_RHS_BATCH * sizeof(double));
    if (!Lp || !col_cur || !D || !Y || !X || (fill_nnz > 0u && (!Li || !Lx))) {
        free(Lp);
        free(col_cur);
        free(Li);
        free(Lx);
        free(D);
        free(Y);
        free(X);
        free(Ai);
        free(Ax);
        free(Ap);
        free(parent);
        free(ancestor);
        free(flag);
        free(pattern);
        free(tmp_stack);
        free(col_nnz);
        return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        Lp[i + 1u] = Lp[i] + col_nnz[i];
        col_cur[i] = Lp[i];
        flag[i] = UINT32_MAX;
    }

    /* Numeric simplicial LDL^T factorization (CSparse cs_ldl) */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t top = n;
        flag[k] = k;
        Y[k] = 0.0;
        for (uint32_t p = Ap[k]; p < Ap[k + 1u]; p++) {
            uint32_t i = Ai[p];
            if (i <= k) {
                Y[i] += Ax[p];
                uint32_t len = 0u;
                while (i < k && flag[i] != k) {
                    tmp_stack[len++] = i;
                    flag[i] = k;
                    i = parent[i];
                }
                while (len > 0u) {
                    pattern[--top] = tmp_stack[--len];
                }
            }
        }
        double d = Y[k];
        Y[k] = 0.0;

        for (uint32_t p = top; p < n; p++) {
            uint32_t i = pattern[p];
            double yi = Y[i];
            Y[i] = 0.0;
            double l_ki = yi / D[i];
            for (uint32_t q = Lp[i]; q < col_cur[i]; q++) {
                Y[Li[q]] -= Lx[q] * yi;
            }
            uint32_t dst = col_cur[i]++;
            Li[dst] = k;
            Lx[dst] = l_ki;
            d -= l_ki * yi;
        }
        D[k] = (d > 1e-18) ? d : 1e-18;
    }

    /* 8-RHS batched forward / diagonal / backward triangular solves */
    for (uint32_t b0 = 0; b0 < B; b0 += WE_RIG_RHS_BATCH) {
        uint32_t nb = (B - b0 < WE_RIG_RHS_BATCH) ? (B - b0) : WE_RIG_RHS_BATCH;
        for (uint32_t i = 0; i < n; i++) {
            double *xi = &X[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t k = 0; k < nb; k++) {
                xi[k] = perm_rhs[(size_t)(b0 + k) * n + i];
            }
            for (uint32_t k = nb; k < WE_RIG_RHS_BATCH; k++) {
                xi[k] = 0.0;
            }
        }

        for (uint32_t i = 0; i < n; i++) {
            const double *xi = &X[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t p = Lp[i]; p < Lp[i + 1u]; p++) {
                double lval = Lx[p];
                double *xr = &X[(size_t)Li[p] * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    xr[k] -= lval * xi[k];
                }
            }
        }

        for (uint32_t i = 0; i < n; i++) {
            double inv_d = 1.0 / D[i];
            double *xi = &X[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                xi[k] *= inv_d;
            }
        }

        for (uint32_t i = n; i > 0u; i--) {
            uint32_t row = i - 1u;
            double *xr = &X[(size_t)row * WE_RIG_RHS_BATCH];
            for (uint32_t p = Lp[row]; p < Lp[row + 1u]; p++) {
                double lval = Lx[p];
                const double *xd = &X[(size_t)Li[p] * WE_RIG_RHS_BATCH];
                for (uint32_t k = 0; k < WE_RIG_RHS_BATCH; k++) {
                    xr[k] -= lval * xd[k];
                }
            }
        }

        for (uint32_t i = 0; i < n; i++) {
            const double *xi = &X[(size_t)i * WE_RIG_RHS_BATCH];
            for (uint32_t k = 0; k < nb; k++) {
                out_perm_sol[(size_t)(b0 + k) * n + i] = xi[k];
            }
        }
    }

    free(Lp);
    free(col_cur);
    free(Li);
    free(Lx);
    free(D);
    free(Y);
    free(X);
    free(Ai);
    free(Ax);
    free(Ap);
    free(parent);
    free(ancestor);
    free(flag);
    free(pattern);
    free(tmp_stack);
    free(col_nnz);
    return true;
}

void we_heat_skin_cfg_default(we_heat_skin_cfg_t *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->conductivity_scale = 1.0f;
    out_cfg->falloff_power = 2.0f;
    out_cfg->smooth_factor = 0.0f;
    out_cfg->max_influences = 4u;
    out_cfg->prune_epsilon = 1e-4f;
    out_cfg->use_visibility_raycast = true;
    out_cfg->use_intrinsic_delaunay = true;
    out_cfg->locked_vert_bs = NULL;
}

static inline bool vert_is_locked(uint32_t vid, const uint64_t *locked_bs)
{
    if (!locked_bs) {
        return false;
    }
    return ((locked_bs[vid >> 6u] >> (vid & 63u)) & 1ull) != 0ull;
}

static void finalize_vertex_top4_weights(we_mdl_vertex_t *vert, const double *w_row, uint32_t B,
                                         uint32_t max_inf, float prune_eps, uint32_t fallback_bone)
{
    uint32_t top_b[4] = {0u, 0u, 0u, 0u};
    double top_w[4] = {0.0, 0.0, 0.0, 0.0};
    uint32_t count = 0u;

    for (uint32_t b = 0; b < B; b++) {
        double w = w_row[b];
        if (!isfinite(w) || w <= 0.0) {
            continue;
        }
        if (count < max_inf) {
            uint32_t ins = count;
            while (ins > 0u && top_w[ins - 1u] < w) {
                top_w[ins] = top_w[ins - 1u];
                top_b[ins] = top_b[ins - 1u];
                ins--;
            }
            top_w[ins] = w;
            top_b[ins] = b;
            count++;
        } else if (w > top_w[max_inf - 1u]) {
            uint32_t ins = max_inf - 1u;
            while (ins > 0u && top_w[ins - 1u] < w) {
                top_w[ins] = top_w[ins - 1u];
                top_b[ins] = top_b[ins - 1u];
                ins--;
            }
            top_w[ins] = w;
            top_b[ins] = b;
        }
    }

    while (count > 1u && top_w[count - 1u] < (double)prune_eps) {
        count--;
    }

    double sum = 0.0;
    for (uint32_t k = 0; k < count; k++) {
        sum += top_w[k];
    }

    for (uint32_t k = 0; k < 4u; k++) {
        vert->blend_indices[k] = 0u;
        vert->weight[k] = 0.0f;
    }

    if (count == 0u || sum <= 1e-12) {
        vert->blend_indices[0] = (fallback_bone < B) ? fallback_bone : 0u;
        vert->weight[0] = 1.0f;
        return;
    }

    float rem = 1.0f;
    for (uint32_t k = 1u; k < count; k++) {
        float wk = clampf((float)(top_w[k] / sum), 0.0f, 1.0f);
        vert->blend_indices[k] = top_b[k];
        vert->weight[k] = wk;
        rem -= wk;
    }
    vert->blend_indices[0] = top_b[0];
    vert->weight[0] = rem > 0.0f ? rem : 0.0f;
}

bool we_mesh_solve_heat_weights(we_puppet_t *puppet, const we_heat_skin_cfg_t *cfg_in,
                                we_heat_solve_stats_t *out_stats)
{
    double t_start = monotonic_ms();
    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
    if (!puppet || !puppet->vertices || !puppet->indices || puppet->vertex_count < 3u ||
        puppet->vertex_count > WE_MDL_MAX_VERTICES || puppet->triangle_count < 1u ||
        puppet->triangle_count > WE_MDL_MAX_TRIANGLES) {
        return false;
    }

    uint32_t topo[WE_MDL_MAX_BONES];
    if (!validate_and_topo_sort_skeleton(puppet, topo, NULL)) {
        return false;
    }
    refresh_puppet_bind_matrices(puppet, topo);

    uint32_t V = puppet->vertex_count;
    uint32_t T = puppet->triangle_count;
    uint32_t B = puppet->bone_count;

    double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
    for (uint32_t i = 0; i < V; i++) {
        const float *p = puppet->vertices[i].pos;
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
            return false;
        }
        if ((double)p[0] < min_x) {
            min_x = (double)p[0];
        }
        if ((double)p[0] > max_x) {
            max_x = (double)p[0];
        }
        if ((double)p[1] < min_y) {
            min_y = (double)p[1];
        }
        if ((double)p[1] > max_y) {
            max_y = (double)p[1];
        }
    }
    for (uint32_t i = 0; i < T * 3u; i++) {
        if (puppet->indices[i] >= V) {
            return false;
        }
    }

    we_heat_skin_cfg_t cfg;
    if (cfg_in) {
        cfg = *cfg_in;
    } else {
        we_heat_skin_cfg_default(&cfg);
    }
    double c_heat = (isfinite(cfg.conductivity_scale) && cfg.conductivity_scale > 0.0f)
                        ? (double)cfg.conductivity_scale
                        : 1.0;
    double falloff_p = (double)clampf(cfg.falloff_power, 0.5f, 8.0f);
    float smooth_factor = clampf(cfg.smooth_factor, 0.0f, 1.0f);
    uint32_t max_inf = cfg.max_influences;
    if (max_inf < 1u) {
        max_inf = 1u;
    } else if (max_inf > 4u) {
        max_inf = 4u;
    }
    float prune_eps = clampf(cfg.prune_epsilon, 0.0f, 0.5f);

    uint32_t *tri_verts = malloc((size_t)T * 3u * sizeof(uint32_t));
    uint32_t *opp_he = malloc((size_t)T * 3u * sizeof(uint32_t));
    double *he_len2 = malloc((size_t)T * 3u * sizeof(double));
    double *mass = malloc((size_t)V * sizeof(double));
    if (!tri_verts || !opp_he || !he_len2 || !mass) {
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        free(mass);
        return false;
    }

    boundary_edge_t *b_edges = NULL;
    uint32_t b_edge_cnt = 0u;
    if (!build_intrinsic_mesh_and_boundaries(puppet, tri_verts, opp_he, he_len2, &b_edges,
                                             &b_edge_cnt)) {
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        free(mass);
        return false;
    }

    uint32_t idt_flips = 0u;
    if (cfg.use_intrinsic_delaunay) {
        idt_flips = apply_idt_flips(T, tri_verts, opp_he, he_len2);
    }

    compute_mixed_voronoi_mass(puppet, mass);

    laplacian_csr_t L;
    if (!assemble_cotangent_laplacian(V, T, tri_verts, he_len2, &L)) {
        free(b_edges);
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        free(mass);
        return false;
    }
    free(tri_verts);
    free(opp_he);
    free(he_len2);

    bone_seg2d_t bone_segs[WE_MDL_MAX_BONES];
    build_bone_segments_2d(puppet, topo, min_x, min_y, max_x, max_y, bone_segs);

    boundary_grid_t b_grid = {0};
    if (cfg.use_visibility_raycast && b_edge_cnt > 0u) {
        if (!build_boundary_dda_grid(puppet, b_edges, b_edge_cnt, min_x, min_y, max_x, max_y,
                                     &b_grid)) {
            free_laplacian_csr(&L);
            free(b_edges);
            free(mass);
            return false;
        }
    }

    uint32_t *comp_id = malloc((size_t)V * sizeof(uint32_t));
    double *H_diag = calloc(V, sizeof(double));
    double *P_rhs = calloc((size_t)V * B, sizeof(double));
    uint32_t *closest_bone = calloc(V, sizeof(uint32_t));
    uint32_t *edge_stamp = (b_edge_cnt > 0u) ? calloc(b_edge_cnt, sizeof(uint32_t)) : NULL;
    if (!comp_id || !H_diag || !P_rhs || !closest_bone || (b_edge_cnt > 0u && !edge_stamp)) {
        free(comp_id);
        free(H_diag);
        free(P_rhs);
        free(closest_bone);
        free(edge_stamp);
        free_boundary_dda_grid(&b_grid);
        free_laplacian_csr(&L);
        free(b_edges);
        free(mass);
        return false;
    }

    uint32_t island_count = find_mesh_components(V, &L, comp_id);
    double *comp_H_sum = calloc(island_count, sizeof(double));
    if (!comp_H_sum) {
        free(comp_H_sum);
        free(comp_id);
        free(H_diag);
        free(P_rhs);
        free(closest_bone);
        free(edge_stamp);
        free_boundary_dda_grid(&b_grid);
        free_laplacian_csr(&L);
        free(b_edges);
        free(mass);
        return false;
    }

    double diag_mesh = hypot(max_x - min_x, max_y - min_y);
    if (diag_mesh < 1e-6) {
        diag_mesh = 1.0;
    }
    double mean_mass = 0.0;
    for (uint32_t i = 0; i < V; i++) {
        mean_mass += mass[i];
    }
    mean_mass = fmax(mean_mass / (double)V, 1e-12);

    uint32_t stamp_id = 1u;
    double reg_p = pow((double)WE_RIG_HEAT_DEFAULT_REG, falloff_p);

    typedef struct {
        uint32_t bone;
        double dist;
        double qx, qy;
    } bone_cand_t;
    bone_cand_t cands[WE_MDL_MAX_BONES];

    for (uint32_t i = 0; i < V; i++) {
        double vx = (double)puppet->vertices[i].pos[0];
        double vy = (double)puppet->vertices[i].pos[1];

        for (uint32_t b = 0; b < B; b++) {
            cands[b].bone = b;
            closest_pt_seg2d(vx, vy, &bone_segs[b], &cands[b].qx, &cands[b].qy, &cands[b].dist);
        }
        closest_bone[i] = 0u;
        double best_euclid = 1e30;
        for (uint32_t b = 0; b < B; b++) {
            if (cands[b].dist < best_euclid) {
                best_euclid = cands[b].dist;
                closest_bone[i] = b;
            }
        }

        /* Sort candidates by Euclidean distance so visibility raycasts stop immediately past
         * (1 + 1e-3) * d_min + 1e-4 * D_mesh once a visible bone is found. */
        for (uint32_t a = 1u; a < B; a++) {
            bone_cand_t key = cands[a];
            uint32_t j = a;
            while (j > 0u && cands[j - 1u].dist > key.dist) {
                cands[j] = cands[j - 1u];
                j--;
            }
            cands[j] = key;
        }

        bool has_vis = false;
        double d_min = 1e30;
        uint32_t near_bones[WE_MDL_MAX_BONES];
        double near_dists[WE_MDL_MAX_BONES];
        uint32_t near_cnt = 0u;

        for (uint32_t idx = 0; idx < B; idx++) {
            double tol = (1.0 + 1e-3) * d_min + 1e-4 * diag_mesh;
            if (has_vis && cands[idx].dist > tol) {
                break;
            }
            bool vis = true;
            double vis_dist = cands[idx].dist;
            if (cfg.use_visibility_raycast && b_edge_cnt > 0u) {
                vis = is_ray_visible_dda(puppet, i, cands[idx].qx, cands[idx].qy, b_edges, &b_grid,
                                         edge_stamp, stamp_id++);
                if (!vis && !bone_segs[cands[idx].bone].is_point) {
                    /* Probe quarter-points along the bone segment to avoid false occlusion at
                     * tight concave joint vertices. */
                    static const double k_t[3] = {0.25, 0.5, 0.75};
                    double best_alt = 1e30;
                    for (int s = 0; s < 3; s++) {
                        double sx = bone_segs[cands[idx].bone].ax +
                                    k_t[s] * (bone_segs[cands[idx].bone].bx -
                                              bone_segs[cands[idx].bone].ax);
                        double sy = bone_segs[cands[idx].bone].ay +
                                    k_t[s] * (bone_segs[cands[idx].bone].by -
                                              bone_segs[cands[idx].bone].ay);
                        double sd = hypot(vx - sx, vy - sy);
                        if (has_vis && sd > tol) {
                            continue;
                        }
                        if (is_ray_visible_dda(puppet, i, sx, sy, b_edges, &b_grid, edge_stamp,
                                               stamp_id++)) {
                            if (sd < best_alt) {
                                best_alt = sd;
                                vis = true;
                            }
                        }
                    }
                    if (vis) {
                        vis_dist = best_alt;
                    }
                }
            }
            if (vis) {
                if (!has_vis) {
                    has_vis = true;
                    d_min = vis_dist;
                    near_bones[near_cnt] = cands[idx].bone;
                    near_dists[near_cnt] = vis_dist;
                    near_cnt++;
                } else {
                    if (vis_dist < d_min) {
                        d_min = vis_dist;
                        uint32_t new_cnt = 0u;
                        double new_tol = (1.0 + 1e-3) * d_min + 1e-4 * diag_mesh;
                        for (uint32_t q = 0; q < near_cnt; q++) {
                            if (near_dists[q] <= new_tol) {
                                near_bones[new_cnt] = near_bones[q];
                                near_dists[new_cnt] = near_dists[q];
                                new_cnt++;
                            }
                        }
                        near_cnt = new_cnt;
                    }
                    double cur_tol = (1.0 + 1e-3) * d_min + 1e-4 * diag_mesh;
                    if (vis_dist <= cur_tol) {
                        near_bones[near_cnt] = cands[idx].bone;
                        near_dists[near_cnt] = vis_dist;
                        near_cnt++;
                    }
                }
            }
        }

        if (has_vis && near_cnt > 0u) {
            double d_norm = d_min / diag_mesh;
            double h_val = (mass[i] / mean_mass) * c_heat / (pow(d_norm, falloff_p) + reg_p);
            H_diag[i] = h_val;
            comp_H_sum[comp_id[i]] += h_val;
            double inv_n = 1.0 / (double)near_cnt;
            for (uint32_t k = 0; k < near_cnt; k++) {
                P_rhs[(size_t)i * B + near_bones[k]] = inv_n;
            }
            closest_bone[i] = near_bones[0];
        }
    }

    /* Recover disconnected floating islands that had zero visible bones */
    for (uint32_t c = 0; c < island_count; c++) {
        if (comp_H_sum[c] > 0.0) {
            continue;
        }
        for (uint32_t i = 0; i < V; i++) {
            if (comp_id[i] != c) {
                continue;
            }
            double vx = (double)puppet->vertices[i].pos[0];
            double vy = (double)puppet->vertices[i].pos[1];
            double d_min = 1e30;
            uint32_t best_b = 0u;
            for (uint32_t b = 0; b < B; b++) {
                double qx, qy, d;
                closest_pt_seg2d(vx, vy, &bone_segs[b], &qx, &qy, &d);
                if (d < d_min) {
                    d_min = d;
                    best_b = b;
                }
            }
            double d_norm = d_min / diag_mesh;
            H_diag[i] = (mass[i] / mean_mass) * c_heat / (pow(d_norm, falloff_p) + reg_p);
            P_rhs[(size_t)i * B + best_b] = 1.0;
            closest_bone[i] = best_b;
        }
    }

    free_boundary_dda_grid(&b_grid);
    free(b_edges);
    free(edge_stamp);
    free(comp_H_sum);
    free(comp_id);

    double tr_LH = 0.0;
    double tr_M = 0.0;
    for (uint32_t i = 0; i < V; i++) {
        tr_LH += L.diag_L[i] + H_diag[i];
        tr_M += mass[i];
    }
    double tikhonov_eps = 1e-8 * tr_LH / fmax(tr_M, 1e-12);
    if (!isfinite(tikhonov_eps) || tikhonov_eps <= 0.0) {
        tikhonov_eps = 1e-8;
    }

    /* Map free vertices (eliminating locked vertices via Dirichlet condensation) */
    uint32_t *v_to_free = malloc((size_t)V * sizeof(uint32_t));
    uint32_t *free_to_v = malloc((size_t)V * sizeof(uint32_t));
    if (!v_to_free || !free_to_v) {
        free(v_to_free);
        free(free_to_v);
        free_laplacian_csr(&L);
        free(H_diag);
        free(P_rhs);
        free(closest_bone);
        free(mass);
        return false;
    }

    uint32_t n_free = 0u;
    for (uint32_t i = 0; i < V; i++) {
        if (vert_is_locked(i, cfg.locked_vert_bs)) {
            v_to_free[i] = UINT32_MAX;
        } else {
            v_to_free[i] = n_free;
            free_to_v[n_free] = i;
            n_free++;
        }
    }

    uint32_t factor_nnz = 0u;
    bool used_ic0 = false;
    double *W_full = calloc((size_t)V * B, sizeof(double));
    if (!W_full) {
        free(v_to_free);
        free(free_to_v);
        free_laplacian_csr(&L);
        free(H_diag);
        free(P_rhs);
        free(closest_bone);
        free(mass);
        return false;
    }

    for (uint32_t i = 0; i < V; i++) {
        if (v_to_free[i] == UINT32_MAX) {
            const we_mdl_vertex_t *lv = &puppet->vertices[i];
            for (int k = 0; k < 4; k++) {
                if (lv->weight[k] > 0.0f && lv->blend_indices[k] < B) {
                    W_full[(size_t)i * B + lv->blend_indices[k]] += (double)lv->weight[k];
                }
            }
        }
    }

    if (n_free > 0u) {
        uint32_t *f_row_ptr = calloc((size_t)n_free + 1u, sizeof(uint32_t));
        double *f_diag = malloc((size_t)n_free * sizeof(double));
        if (!f_row_ptr || !f_diag) {
            free(f_row_ptr);
            free(f_diag);
            free(W_full);
            free(v_to_free);
            free(free_to_v);
            free_laplacian_csr(&L);
            free(H_diag);
            free(P_rhs);
            free(closest_bone);
            free(mass);
            return false;
        }

        uint32_t f_nnz = 0u;
        for (uint32_t fi = 0; fi < n_free; fi++) {
            uint32_t i = free_to_v[fi];
            f_diag[fi] = L.diag_L[i] + H_diag[i] + tikhonov_eps * mass[i];
            for (uint32_t p = L.row_ptr[i]; p < L.row_ptr[i + 1u]; p++) {
                if (v_to_free[L.col_idx[p]] != UINT32_MAX) {
                    f_row_ptr[fi + 1u]++;
                    f_nnz++;
                }
            }
        }
        for (uint32_t fi = 0; fi < n_free; fi++) {
            f_row_ptr[fi + 1u] += f_row_ptr[fi];
        }

        uint32_t *f_col_idx = f_nnz > 0u ? malloc((size_t)f_nnz * sizeof(uint32_t)) : NULL;
        double *f_val = f_nnz > 0u ? malloc((size_t)f_nnz * sizeof(double)) : NULL;
        uint32_t *perm = malloc((size_t)n_free * sizeof(uint32_t));
        uint32_t *inv_perm = malloc((size_t)n_free * sizeof(uint32_t));
        if ((f_nnz > 0u && (!f_col_idx || !f_val)) || !perm || !inv_perm) {
            free(f_row_ptr);
            free(f_diag);
            free(f_col_idx);
            free(f_val);
            free(perm);
            free(inv_perm);
            free(W_full);
            free(v_to_free);
            free(free_to_v);
            free_laplacian_csr(&L);
            free(H_diag);
            free(P_rhs);
            free(closest_bone);
            free(mass);
            return false;
        }

        uint32_t wpos = 0u;
        for (uint32_t fi = 0; fi < n_free; fi++) {
            uint32_t i = free_to_v[fi];
            for (uint32_t p = L.row_ptr[i]; p < L.row_ptr[i + 1u]; p++) {
                uint32_t fj = v_to_free[L.col_idx[p]];
                if (fj != UINT32_MAX) {
                    f_col_idx[wpos] = fj;
                    f_val[wpos] = -L.edge_w[p];
                    wpos++;
                }
            }
        }

        if (!compute_rcm_permutation(n_free, f_row_ptr, f_col_idx, perm, inv_perm)) {
            for (uint32_t i = 0; i < n_free; i++) {
                perm[i] = i;
                inv_perm[i] = i;
            }
        }

        /* Build permuted CSR (sorted ascending col_idx per row) and permuted RHS */
        uint32_t *p_row_ptr = calloc((size_t)n_free + 1u, sizeof(uint32_t));
        uint32_t *p_col_idx = f_nnz > 0u ? malloc((size_t)f_nnz * sizeof(uint32_t)) : NULL;
        double *p_val = f_nnz > 0u ? malloc((size_t)f_nnz * sizeof(double)) : NULL;
        double *p_diag = malloc((size_t)n_free * sizeof(double));
        double *p_rhs = calloc((size_t)B * n_free, sizeof(double));
        double *p_sol = calloc((size_t)B * n_free, sizeof(double));
        if (!p_row_ptr || !p_diag || !p_rhs || !p_sol || (f_nnz > 0u && (!p_col_idx || !p_val))) {
            free(p_row_ptr);
            free(p_col_idx);
            free(p_val);
            free(p_diag);
            free(p_rhs);
            free(p_sol);
            free(f_row_ptr);
            free(f_diag);
            free(f_col_idx);
            free(f_val);
            free(perm);
            free(inv_perm);
            free(W_full);
            free(v_to_free);
            free(free_to_v);
            free_laplacian_csr(&L);
            free(H_diag);
            free(P_rhs);
            free(closest_bone);
            free(mass);
            return false;
        }

        for (uint32_t pi = 0; pi < n_free; pi++) {
            uint32_t fi = perm[pi];
            p_diag[pi] = f_diag[fi];
            p_row_ptr[pi + 1u] = p_row_ptr[pi] + (f_row_ptr[fi + 1u] - f_row_ptr[fi]);
        }

        for (uint32_t pi = 0; pi < n_free; pi++) {
            uint32_t fi = perm[pi];
            uint32_t orig_v = free_to_v[fi];
            uint32_t dst = p_row_ptr[pi];
            uint32_t cnt = f_row_ptr[fi + 1u] - f_row_ptr[fi];
            for (uint32_t p = f_row_ptr[fi]; p < f_row_ptr[fi + 1u]; p++) {
                p_col_idx[dst] = inv_perm[f_col_idx[p]];
                p_val[dst] = f_val[p];
                dst++;
            }
            /* Insertion sort row entries by permuted column index */
            uint32_t r0 = p_row_ptr[pi];
            for (uint32_t a = 1u; a < cnt; a++) {
                uint32_t kc = p_col_idx[r0 + a];
                double kv = p_val[r0 + a];
                uint32_t j = a;
                while (j > 0u && p_col_idx[r0 + j - 1u] > kc) {
                    p_col_idx[r0 + j] = p_col_idx[r0 + j - 1u];
                    p_val[r0 + j] = p_val[r0 + j - 1u];
                    j--;
                }
                p_col_idx[r0 + j] = kc;
                p_val[r0 + j] = kv;
            }

            double hi = H_diag[orig_v];
            for (uint32_t b = 0; b < B; b++) {
                p_rhs[(size_t)b * n_free + pi] = hi * P_rhs[(size_t)orig_v * B + b];
            }
            /* Subtract Dirichlet contribution A_{orig_v, locked} * W_locked from RHS */
            for (uint32_t p = L.row_ptr[orig_v]; p < L.row_ptr[orig_v + 1u]; p++) {
                uint32_t nbr = L.col_idx[p];
                if (v_to_free[nbr] == UINT32_MAX) {
                    double w_edge = L.edge_w[p];
                    for (uint32_t b = 0; b < B; b++) {
                        p_rhs[(size_t)b * n_free + pi] += w_edge * W_full[(size_t)nbr * B + b];
                    }
                }
            }
        }

        free(f_row_ptr);
        free(f_diag);
        free(f_col_idx);
        free(f_val);

        bool solved = solve_sparse_ldlt_or_ic0(n_free, p_row_ptr, p_col_idx, p_val, p_diag, B,
                                               p_rhs, p_sol, &factor_nnz, &used_ic0);

        if (solved) {
            for (uint32_t pi = 0; pi < n_free; pi++) {
                uint32_t orig_v = free_to_v[perm[pi]];
                for (uint32_t b = 0; b < B; b++) {
                    W_full[(size_t)orig_v * B + b] = p_sol[(size_t)b * n_free + pi];
                }
            }
        }

        free(p_row_ptr);
        free(p_col_idx);
        free(p_val);
        free(p_diag);
        free(p_rhs);
        free(p_sol);
        free(perm);
        free(inv_perm);

        if (!solved) {
            free(W_full);
            free(v_to_free);
            free(free_to_v);
            free_laplacian_csr(&L);
            free(H_diag);
            free(P_rhs);
            free(closest_bone);
            free(mass);
            return false;
        }
    }

    if (smooth_factor > 1e-6f && n_free > 0u) {
        double *W_smooth = malloc((size_t)V * B * sizeof(double));
        if (W_smooth) {
            memcpy(W_smooth, W_full, (size_t)V * B * sizeof(double));
            double sf = (double)smooth_factor;
            for (uint32_t i = 0; i < V; i++) {
                if (v_to_free[i] == UINT32_MAX || L.diag_L[i] <= 1e-18) {
                    continue;
                }
                double inv_deg = 1.0 / L.diag_L[i];
                for (uint32_t b = 0; b < B; b++) {
                    double avg = 0.0;
                    for (uint32_t p = L.row_ptr[i]; p < L.row_ptr[i + 1u]; p++) {
                        avg += L.edge_w[p] * W_full[(size_t)L.col_idx[p] * B + b];
                    }
                    W_smooth[(size_t)i * B + b] =
                        (1.0 - sf) * W_full[(size_t)i * B + b] + sf * avg * inv_deg;
                }
            }
            memcpy(W_full, W_smooth, (size_t)V * B * sizeof(double));
            free(W_smooth);
        }
    }

    for (uint32_t i = 0; i < V; i++) {
        if (v_to_free[i] != UINT32_MAX) {
            finalize_vertex_top4_weights(&puppet->vertices[i], &W_full[(size_t)i * B], B, max_inf,
                                         prune_eps, closest_bone[i]);
        }
    }

    free(W_full);
    free(v_to_free);
    free(free_to_v);
    free_laplacian_csr(&L);
    free(H_diag);
    free(P_rhs);
    free(closest_bone);
    free(mass);

    if (out_stats) {
        out_stats->vertex_count = V;
        out_stats->bone_count = B;
        out_stats->island_count = island_count;
        out_stats->idt_edge_flips = idt_flips;
        out_stats->factor_nnz = factor_nnz;
        out_stats->used_ic0_fallback = used_ic0;
        out_stats->solve_time_ms = monotonic_ms() - t_start;
    }
    return true;
}

/* ========================================================================= */
/* 3. Locked-Bone Renormalization & GLES 3.1 Compute Brush / Heatmap Shaders */
/* ========================================================================= */

static inline bool is_bone_locked_mask(uint32_t bone_idx, const uint64_t locked_bone_mask[2])
{
    if (!locked_bone_mask || bone_idx >= WE_MDL_MAX_BONES) {
        return false;
    }
    return ((locked_bone_mask[bone_idx >> 6u] >> (bone_idx & 63u)) & 1ull) != 0ull;
}

void we_mesh_renormalize_vertex_weights(we_mdl_vertex_t *vert, uint32_t bone_count,
                                        uint32_t active_bone, float target_weight,
                                        const uint64_t locked_bone_mask[2])
{
    if (!vert || bone_count == 0u) {
        return;
    }
    if (bone_count > WE_MDL_MAX_BONES) {
        bone_count = WE_MDL_MAX_BONES;
    }

    /* Sanitize slots and merge duplicate bone indices */
    for (int k = 0; k < 4; k++) {
        if (!isfinite(vert->weight[k]) || vert->weight[k] <= 0.0f ||
            vert->blend_indices[k] >= bone_count) {
            vert->weight[k] = 0.0f;
            vert->blend_indices[k] = 0u;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (vert->weight[i] <= 0.0f) {
            continue;
        }
        for (int j = i + 1; j < 4; j++) {
            if (vert->weight[j] > 0.0f && vert->blend_indices[j] == vert->blend_indices[i]) {
                vert->weight[i] += vert->weight[j];
                vert->weight[j] = 0.0f;
                vert->blend_indices[j] = 0u;
            }
        }
    }

    float locked_sum = 0.0f;
    for (int k = 0; k < 4; k++) {
        if (vert->weight[k] > 0.0f &&
            is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask)) {
            locked_sum += vert->weight[k];
        }
    }
    if (locked_sum > 1.0f) {
        float inv_l = 1.0f / locked_sum;
        for (int k = 0; k < 4; k++) {
            if (vert->weight[k] > 0.0f &&
                is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask)) {
                vert->weight[k] *= inv_l;
            }
        }
        locked_sum = 1.0f;
    }

    float unlocked_budget = fmaxf(0.0f, 1.0f - locked_sum);
    bool active_valid =
        (active_bone < bone_count) && !is_bone_locked_mask(active_bone, locked_bone_mask);

    int active_slot = -1;
    int evict_slot = -1;
    float min_unlocked = 2.0f;
    float other_unlocked_sum = 0.0f;

    for (int k = 0; k < 4; k++) {
        bool is_locked = (vert->weight[k] > 0.0f) &&
                         is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask);
        if (vert->weight[k] > 0.0f && vert->blend_indices[k] == active_bone) {
            active_slot = k;
        }
        if (!is_locked) {
            if (vert->weight[k] > 0.0f && vert->blend_indices[k] != active_bone) {
                other_unlocked_sum += vert->weight[k];
            }
            if (vert->weight[k] < min_unlocked) {
                min_unlocked = vert->weight[k];
                evict_slot = k;
            }
        }
    }

    if (active_valid) {
        float new_w = clampf(target_weight, 0.0f, unlocked_budget);
        if (active_slot < 0) {
            if (new_w > 1e-6f && evict_slot >= 0) {
                active_slot = evict_slot;
                if (vert->weight[active_slot] > 0.0f &&
                    !is_bone_locked_mask(vert->blend_indices[active_slot], locked_bone_mask)) {
                    other_unlocked_sum =
                        fmaxf(0.0f, other_unlocked_sum - vert->weight[active_slot]);
                }
                vert->blend_indices[active_slot] = active_bone;
                vert->weight[active_slot] = new_w;
            }
        } else {
            vert->weight[active_slot] = new_w;
        }

        if (active_slot >= 0) {
            float rem = fmaxf(0.0f, unlocked_budget - vert->weight[active_slot]);
            if (other_unlocked_sum > 1e-6f) {
                float scale = rem / other_unlocked_sum;
                for (int k = 0; k < 4; k++) {
                    if (k != active_slot && vert->weight[k] > 0.0f &&
                        !is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask)) {
                        vert->weight[k] *= scale;
                    }
                }
            } else if (rem > 1e-6f) {
                int fallback_slot = -1;
                for (int k = 0; k < 4; k++) {
                    if (k != active_slot &&
                        !(vert->weight[k] > 0.0f &&
                          is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask))) {
                        fallback_slot = k;
                        break;
                    }
                }
                uint32_t fb_bone = active_bone;
                for (uint32_t b = 0; b < bone_count; b++) {
                    if (b != active_bone && !is_bone_locked_mask(b, locked_bone_mask)) {
                        fb_bone = b;
                        break;
                    }
                }
                if (fallback_slot >= 0 && fb_bone != active_bone) {
                    vert->blend_indices[fallback_slot] = fb_bone;
                    vert->weight[fallback_slot] = rem;
                } else {
                    vert->weight[active_slot] = unlocked_budget;
                }
            }
        }
    } else {
        /* Active bone is locked or out of range: preserve locked slots and normalize unlocked */
        if (other_unlocked_sum > 1e-6f) {
            float scale = unlocked_budget / other_unlocked_sum;
            for (int k = 0; k < 4; k++) {
                if (vert->weight[k] > 0.0f &&
                    !is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask)) {
                    vert->weight[k] *= scale;
                }
            }
        }
    }

    for (int k = 0; k < 4; k++) {
        if (vert->weight[k] <= 1e-7f) {
            vert->weight[k] = 0.0f;
            vert->blend_indices[k] = 0u;
        }
    }

    /* Enforce exact 1-ULP sum == 1.0f on the largest unlocked positive slot */
    float total = vert->weight[0] + vert->weight[1] + vert->weight[2] + vert->weight[3];
    if (total <= 1e-6f) {
        uint32_t fb = (active_bone < bone_count) ? active_bone : 0u;
        vert->blend_indices[0] = fb;
        vert->weight[0] = 1.0f;
        return;
    }

    int fix_slot = -1;
    float best_w = -1.0f;
    for (int k = 0; k < 4; k++) {
        if (vert->weight[k] > best_w &&
            !is_bone_locked_mask(vert->blend_indices[k], locked_bone_mask)) {
            best_w = vert->weight[k];
            fix_slot = k;
        }
    }
    if (fix_slot >= 0) {
        float others = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (k != fix_slot) {
                others += vert->weight[k];
            }
        }
        vert->weight[fix_slot] = fmaxf(0.0f, 1.0f - others);
    }
}

typedef struct {
    float pos_lock[4];
    uint32_t blend_indices[4];
    float weight[4];
} gpu_weight_vert_t;
static_assert(sizeof(gpu_weight_vert_t) == 48, "gpu_weight_vert_t must be 48 bytes");

typedef struct {
    float a[2];
    float b[2];
    float ra;
    float rb;
    float hardness;
    float strength;
    uint32_t vertex_count;
    uint32_t active_bone;
    uint32_t op_mode;
    float target_value;
    uint32_t locked_bones[4];
    float prune_eps;
    uint32_t bone_count;
    float pad[2];
} gpu_brush_params_ubo_t;
static_assert(sizeof(gpu_brush_params_ubo_t) == 80, "gpu_brush_params_ubo_t must be 80 bytes");

typedef struct {
    float blend_indices_f[4];
    uint32_t blend_indices_u[4];
    float weight[4];
} gpu_skin_vbo_attrib_t;
static_assert(sizeof(gpu_skin_vbo_attrib_t) == 48, "gpu_skin_vbo_attrib_t must be 48 bytes");

static const char *const k_weight_brush_comp_parts[3] = {
    "#version 310 es\n"
    "layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;\n"
    "struct WeightVert {\n"
    "    vec4  pos_lock;\n"
    "    uvec4 blend_indices;\n"
    "    vec4  weight;\n"
    "};\n"
    "layout(std430, binding = 0) readonly buffer WeightsIn { WeightVert v_in[]; };\n"
    "layout(std430, binding = 1) readonly buffer MeshAdjacency {\n"
    "    uint row_ptr_offset;\n"
    "    uint col_idx_offset;\n"
    "    uint cot_weight_offset;\n"
    "    uint vertex_count;\n"
    "    uint data[];\n"
    "};\n"
    "layout(std430, binding = 2) writeonly buffer WeightsOut { WeightVert v_out[]; };\n"
    "layout(std430, binding = 3) buffer DirtyBlockBits { uint dirty_words[8]; };\n"
    "layout(std140, binding = 0) uniform BrushParams {\n"
    "    vec2  u_DabA;\n"
    "    vec2  u_DabB;\n"
    "    float u_RadiusA;\n"
    "    float u_RadiusB;\n"
    "    float u_Hardness;\n"
    "    float u_Strength;\n"
    "    uint  u_VertexCount;\n"
    "    uint  u_ActiveBone;\n"
    "    uint  u_OpMode;\n"
    "    float u_TargetValue;\n"
    "    uvec4 u_LockedBones;\n"
    "    float u_PruneEps;\n"
    "    uint  u_BoneCount;\n"
    "    vec2  u_Pad;\n"
    "};\n"
    "bool is_bone_locked(uint b) {\n"
    "    uint word = b >> 5u;\n"
    "    uint bit  = b & 31u;\n"
    "    return (word < 4u) && ((u_LockedBones[word] & (1u << bit)) != 0u);\n"
    "}\n"
    "float sd_capsule_dab(vec2 p, vec2 pa, vec2 pb, float ra, float rb) {\n"
    "    vec2  ba = pb - pa;\n"
    "    float l2 = dot(ba, ba);\n"
    "    float h  = (l2 < 1e-8) ? 0.0 : clamp(dot(p - pa, ba) / l2, 0.0, 1.0);\n"
    "    float r  = (l2 < 1e-8) ? max(ra, rb) : mix(ra, rb, h);\n"
    "    return length((p - pa) - ba * h) - r;\n"
    "}\n"
    "void renormalize_with_locks(inout uvec4 idx, inout vec4 w, uint active_bone, float target_w) "
    "{\n"
    "    for (int i = 0; i < 4; ++i) {\n"
    "        if (w[i] <= 0.0 || idx[i] >= u_BoneCount) {\n"
    "            w[i] = 0.0;\n"
    "            idx[i] = 0u;\n"
    "            continue;\n"
    "        }\n"
    "        for (int j = i + 1; j < 4; ++j) {\n"
    "            if (w[j] > 0.0 && idx[j] == idx[i]) {\n"
    "                w[i] += w[j];\n"
    "                w[j] = 0.0;\n"
    "                idx[j] = 0u;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    float locked_sum   = 0.0;\n"
    "    for (int k = 0; k < 4; ++k) {\n"
    "        if (w[k] > 0.0 && is_bone_locked(idx[k])) {\n"
    "            locked_sum += w[k];\n"
    "        }\n"
    "    }\n"
    "    if (locked_sum > 1.0) {\n"
    "        float inv_l = 1.0 / locked_sum;\n"
    "        for (int k = 0; k < 4; ++k) {\n"
    "            if (w[k] > 0.0 && is_bone_locked(idx[k])) {\n"
    "                w[k] *= inv_l;\n"
    "            }\n"
    "        }\n"
    "        locked_sum = 1.0;\n"
    "    }\n"
    "    float unlocked_budget = max(0.0, 1.0 - locked_sum);\n"
    "    bool  active_valid    = (active_bone < u_BoneCount) && !is_bone_locked(active_bone);\n"
    "    float other_sum       = 0.0;\n"
    "    int   active_slot     = -1;\n"
    "    int   evict_slot      = -1;\n"
    "    float min_unlocked    = 2.0;\n"
    "    for (int k = 0; k < 4; ++k) {\n"
    "        bool locked = (w[k] > 0.0) && is_bone_locked(idx[k]);\n"
    "        if (w[k] > 0.0 && idx[k] == active_bone) {\n"
    "            active_slot = k;\n"
    "        }\n"
    "        if (!locked) {\n"
    "            if (w[k] > 0.0 && idx[k] != active_bone) {\n"
    "                other_sum += w[k];\n"
    "            }\n"
    "            if (w[k] < min_unlocked) {\n"
    "                min_unlocked = w[k];\n"
    "                evict_slot   = k;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    if (active_valid) {\n"
    "        float new_w = clamp(target_w, 0.0, unlocked_budget);\n"
    "        if (active_slot < 0) {\n"
    "            if (new_w > 1e-6 && evict_slot >= 0) {\n"
    "                active_slot = evict_slot;\n"
    "                if (w[active_slot] > 0.0 && !is_bone_locked(idx[active_slot])) {\n"
    "                    other_sum = max(0.0, other_sum - w[active_slot]);\n"
    "                }\n"
    "                idx[active_slot] = active_bone;\n"
    "                w[active_slot]   = new_w;\n"
    "            }\n"
    "        } else {\n"
    "            w[active_slot] = new_w;\n"
    "        }\n",
    "        if (active_slot >= 0) {\n"
    "            float rem = max(0.0, unlocked_budget - w[active_slot]);\n"
    "            if (other_sum > 1e-6) {\n"
    "                float scale = rem / other_sum;\n"
    "                for (int k = 0; k < 4; ++k) {\n"
    "                    if (k != active_slot && w[k] > 0.0 && !is_bone_locked(idx[k])) {\n"
    "                        w[k] *= scale;\n"
    "                    }\n"
    "                }\n"
    "            } else if (rem > 1e-6) {\n"
    "                int fallback_slot = -1;\n"
    "                for (int k = 0; k < 4; ++k) {\n"
    "                    if (k != active_slot && !(w[k] > 0.0 && is_bone_locked(idx[k]))) {\n"
    "                        fallback_slot = k;\n"
    "                        break;\n"
    "                    }\n"
    "                }\n"
    "                uint fb_bone = active_bone;\n"
    "                for (uint b = 0u; b < u_BoneCount; ++b) {\n"
    "                    if (b != active_bone && !is_bone_locked(b)) {\n"
    "                        fb_bone = b;\n"
    "                        break;\n"
    "                    }\n"
    "                }\n"
    "                if (fallback_slot >= 0 && fb_bone != active_bone) {\n"
    "                    idx[fallback_slot] = fb_bone;\n"
    "                    w[fallback_slot]   = rem;\n"
    "                } else {\n"
    "                    w[active_slot] = unlocked_budget;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    } else {\n"
    "        if (other_sum > 1e-6) {\n"
    "            float scale = unlocked_budget / other_sum;\n"
    "            for (int k = 0; k < 4; ++k) {\n"
    "                if (w[k] > 0.0 && !is_bone_locked(idx[k])) {\n"
    "                    w[k] *= scale;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    for (int k = 0; k < 4; ++k) {\n"
    "        if (w[k] <= 1e-7) {\n"
    "            w[k] = 0.0;\n"
    "            idx[k] = 0u;\n"
    "        }\n"
    "    }\n"
    "}\n",
    "void main() {\n"
    "    uint vid = gl_GlobalInvocationID.x;\n"
    "    if (vid >= u_VertexCount) return;\n"
    "    WeightVert v = v_in[vid];\n"
    "    if (v.pos_lock.w > 0.5) {\n"
    "        v_out[vid] = v;\n"
    "        return;\n"
    "    }\n"
    "    vec2  ba     = u_DabB - u_DabA;\n"
    "    float l2     = dot(ba, ba);\n"
    "    float h_t    = (l2 < 1e-8) ? 0.0 : clamp(dot(v.pos_lock.xy - u_DabA, ba) / l2, 0.0, "
    "1.0);\n"
    "    float r_loc  = (l2 < 1e-8) ? max(u_RadiusA, u_RadiusB) : mix(u_RadiusA, u_RadiusB, h_t);\n"
    "    float sd     = sd_capsule_dab(v.pos_lock.xy, u_DabA, u_DabB, u_RadiusA, u_RadiusB);\n"
    "    if (sd >= 0.0) {\n"
    "        v_out[vid] = v;\n"
    "        return;\n"
    "    }\n"
    "    float r_norm = clamp(-sd / max(r_loc, 1e-4), 0.0, 1.0);\n"
    "    float f      = smoothstep(0.0, max(1.0 - u_Hardness, 1e-3), r_norm);\n"
    "    float effect = clamp(f * u_Strength, 0.0, 1.0);\n"
    "    if (effect <= 1e-6) {\n"
    "        v_out[vid] = v;\n"
    "        return;\n"
    "    }\n"
    "    uvec4 idx = v.blend_indices;\n"
    "    vec4  w   = v.weight;\n"
    "    float cur = 0.0;\n"
    "    for (int k = 0; k < 4; ++k) {\n"
    "        if (idx[k] == u_ActiveBone) cur += w[k];\n"
    "    }\n"
    "    if (u_OpMode == 0u) {\n"
    "        renormalize_with_locks(idx, w, u_ActiveBone, cur + (1.0 - cur) * effect);\n"
    "    } else if (u_OpMode == 1u) {\n"
    "        renormalize_with_locks(idx, w, u_ActiveBone, max(0.0, cur - effect));\n"
    "    } else if (u_OpMode == 2u) {\n"
    "        renormalize_with_locks(idx, w, u_ActiveBone, mix(cur, u_TargetValue, effect));\n"
    "    } else if (u_OpMode == 3u) {\n"
    "        uint  r0       = data[row_ptr_offset + vid];\n"
    "        uint  r1       = data[row_ptr_offset + vid + 1u];\n"
    "        float avg_bone = 0.0;\n"
    "        float w_sum    = 0.0;\n"
    "        for (uint e = r0; e < r1; ++e) {\n"
    "            uint       nbr = data[col_idx_offset + e];\n"
    "            float      cw  = uintBitsToFloat(data[cot_weight_offset + e]);\n"
    "            WeightVert nv  = v_in[nbr];\n"
    "            float      nw  = 0.0;\n"
    "            for (int k = 0; k < 4; ++k) {\n"
    "                if (nv.blend_indices[k] == u_ActiveBone) nw += nv.weight[k];\n"
    "            }\n"
    "            avg_bone += cw * nw;\n"
    "            w_sum    += cw;\n"
    "        }\n"
    "        if (w_sum > 1e-6) {\n"
    "            renormalize_with_locks(idx, w, u_ActiveBone, mix(cur, avg_bone / w_sum, "
    "effect));\n"
    "        }\n"
    "    } else {\n"
    "        for (int k = 0; k < 4; ++k) {\n"
    "            if (w[k] < u_PruneEps && !is_bone_locked(idx[k])) w[k] = 0.0;\n"
    "        }\n"
    "        renormalize_with_locks(idx, w, u_ActiveBone, cur);\n"
    "    }\n"
    "    v.blend_indices = idx;\n"
    "    v.weight        = w;\n"
    "    v_out[vid]      = v;\n"
    "    uint blk = vid >> 8u;\n"
    "    atomicOr(dirty_words[blk >> 5u], 1u << (blk & 31u));\n"
    "}\n",
};

static const char *const k_heatmap_vert_glsl =
    "#version 310 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec3 a_Position;\n"
    "layout(location = 1) in vec2 a_TexCoord;\n"
    "layout(location = 5) in vec4 a_BlendIndices;\n"
    "layout(location = 6) in vec4 a_BlendWeights;\n"
    "uniform mat4  u_MVP;\n"
    "uniform int   u_ActiveBone;\n"
    "out vec2  v_TexCoord;\n"
    "out float v_ActiveWeight;\n"
    "void main() {\n"
    "    gl_Position = u_MVP * vec4(a_Position, 1.0);\n"
    "    v_TexCoord  = a_TexCoord;\n"
    "    float w = 0.0;\n"
    "    ivec4 bi = ivec4(a_BlendIndices + 0.5);\n"
    "    if (bi.x == u_ActiveBone) w += a_BlendWeights.x;\n"
    "    if (bi.y == u_ActiveBone) w += a_BlendWeights.y;\n"
    "    if (bi.z == u_ActiveBone) w += a_BlendWeights.z;\n"
    "    if (bi.w == u_ActiveBone) w += a_BlendWeights.w;\n"
    "    v_ActiveWeight = clamp(w, 0.0, 1.0);\n"
    "}\n";

static const char *const k_heatmap_frag_glsl =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2  v_TexCoord;\n"
    "in float v_ActiveWeight;\n"
    "uniform sampler2D u_Texture0;\n"
    "uniform int       u_PaletteId;\n"
    "uniform float     u_OverlayAlpha;\n"
    "out vec4 fragColor;\n"
    "vec3 heatmap_turbo(float t) {\n"
    "    t = clamp(t, 0.0, 1.0);\n"
    "    const vec4 kR4 = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);\n"
    "    const vec4 kG4 = vec4(0.09140261, 2.19418839,   4.84296658, -14.18503333);\n"
    "    const vec4 kB4 = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);\n"
    "    const vec2 kR2 = vec2(-152.94239396, 59.28637943);\n"
    "    const vec2 kG2 = vec2(   4.27729857,  2.82956604);\n"
    "    const vec2 kB2 = vec2( -89.90310912, 27.34824973);\n"
    "    vec4 v4 = vec4(1.0, t, t * t, t * t * t);\n"
    "    vec2 v2 = vec2(v4.w * t, v4.w * t * t);\n"
    "    return clamp(vec3(dot(v4, kR4) + dot(v2, kR2),\n"
    "                      dot(v4, kG4) + dot(v2, kG2),\n"
    "                      dot(v4, kB4) + dot(v2, kB2)), 0.0, 1.0);\n"
    "}\n"
    "vec3 heatmap_viridis(float t) {\n"
    "    t = clamp(t, 0.0, 1.0);\n"
    "    const vec3 c0 = vec3(0.277727327, 0.005407345, 0.334099805);\n"
    "    const vec3 c1 = vec3(0.105093043, 1.404613529, 1.384590163);\n"
    "    const vec3 c2 = vec3(-0.330861829, 0.214847559, 0.095095163);\n"
    "    const vec3 c3 = vec3(-4.634230499, -5.799100973, -19.332440956);\n"
    "    const vec3 c4 = vec3(6.228269936, 14.179933367, 56.690552601);\n"
    "    const vec3 c5 = vec3(4.776384998, -13.745145377, -65.353032633);\n"
    "    const vec3 c6 = vec3(-5.435455856, 4.645852612, 26.312435249);\n"
    "    return clamp(c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6))))), 0.0, "
    "1.0);\n"
    "}\n"
    "vec3 heatmap_inferno(float t) {\n"
    "    t = clamp(t, 0.0, 1.0);\n"
    "    const vec3 c0 = vec3(0.000218940, 0.001651005, -0.019480898);\n"
    "    const vec3 c1 = vec3(0.106513419, 0.563956437,  3.932712390);\n"
    "    const vec3 c2 = vec3(11.60249310, -3.972853960, -15.94239410);\n"
    "    const vec3 c3 = vec3(-41.7039961, 17.43639890, 44.35414520);\n"
    "    const vec3 c4 = vec3(77.16293570, -33.40235890, -81.80730930);\n"
    "    const vec3 c5 = vec3(-71.3194282, 32.62606430, 73.20951990);\n"
    "    const vec3 c6 = vec3(25.13112620, -12.24266890, -23.07032500);\n"
    "    return clamp(c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6))))), 0.0, "
    "1.0);\n"
    "}\n"
    "void main() {\n"
    "    vec4  albedo = texture(u_Texture0, v_TexCoord);\n"
    "    vec3  ramp   = (u_PaletteId == 2) ? heatmap_inferno(v_ActiveWeight)\n"
    "                 : (u_PaletteId == 1) ? heatmap_viridis(v_ActiveWeight)\n"
    "                                      : heatmap_turbo(v_ActiveWeight);\n"
    "    if (v_ActiveWeight < 0.001) {\n"
    "        ramp = vec3(0.08, 0.10, 0.14);\n"
    "    }\n"
    "    float fw      = max(fwidth(v_ActiveWeight), 1e-4);\n"
    "    float iso     = abs(fract(v_ActiveWeight * 4.0 + 0.5) - 0.5) / (4.0 * fw);\n"
    "    float isoLine = 1.0 - clamp(iso, 0.0, 1.0);\n"
    "    vec3 tinted   = mix(albedo.rgb, ramp, u_OverlayAlpha);\n"
    "    tinted        = mix(tinted, vec3(0.95), isoLine * 0.35 * u_OverlayAlpha);\n"
    "    fragColor     = vec4(tinted, albedo.a);\n"
    "}\n";

typedef struct {
    EGLContext owner_ctx;
    GLuint compute_prog;
    GLuint heatmap_prog;
    GLuint ssbo_ping;
    GLuint ssbo_pong;
    GLuint ssbo_adj;
    GLuint ssbo_dirty;
    GLuint ubo_params;
    GLuint heatmap_vao;
    GLuint heatmap_vbo_dyn;
    GLuint heatmap_vbo_skin;
    GLuint heatmap_ebo;
    GLuint heatmap_white_tex;
    uint32_t ssbo_vert_cap;
    size_t adj_bytes_cap;
    uint32_t heatmap_vert_cap;
    uint32_t heatmap_skin_cap;
    uint32_t heatmap_idx_cap;
    bool ready;
} gpu_weight_brush_state_t;

static gpu_weight_brush_state_t g_gpu_brush = {0};

static GLuint compile_gl_shader_parts(GLenum stage, const char *const *parts, GLsizei count)
{
    GLuint sh = glCreateShader(stage);
    if (!sh) {
        return 0u;
    }
    glShaderSource(sh, count, parts, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log_buf[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_WARN("we_mesh_rig shader compile error: %s", log_buf);
        glDeleteShader(sh);
        return 0u;
    }
    return sh;
}

static void delete_gpu_brush_gl_objects(void)
{
    if (g_gpu_brush.compute_prog) {
        glDeleteProgram(g_gpu_brush.compute_prog);
    }
    if (g_gpu_brush.heatmap_prog) {
        glDeleteProgram(g_gpu_brush.heatmap_prog);
    }
    if (g_gpu_brush.ssbo_ping) {
        glDeleteBuffers(1, &g_gpu_brush.ssbo_ping);
    }
    if (g_gpu_brush.ssbo_pong) {
        glDeleteBuffers(1, &g_gpu_brush.ssbo_pong);
    }
    if (g_gpu_brush.ssbo_adj) {
        glDeleteBuffers(1, &g_gpu_brush.ssbo_adj);
    }
    if (g_gpu_brush.ssbo_dirty) {
        glDeleteBuffers(1, &g_gpu_brush.ssbo_dirty);
    }
    if (g_gpu_brush.ubo_params) {
        glDeleteBuffers(1, &g_gpu_brush.ubo_params);
    }
    if (g_gpu_brush.heatmap_vbo_dyn) {
        glDeleteBuffers(1, &g_gpu_brush.heatmap_vbo_dyn);
    }
    if (g_gpu_brush.heatmap_vbo_skin) {
        glDeleteBuffers(1, &g_gpu_brush.heatmap_vbo_skin);
    }
    if (g_gpu_brush.heatmap_ebo) {
        glDeleteBuffers(1, &g_gpu_brush.heatmap_ebo);
    }
    if (g_gpu_brush.heatmap_vao) {
        glDeleteVertexArrays(1, &g_gpu_brush.heatmap_vao);
    }
    if (g_gpu_brush.heatmap_white_tex) {
        glDeleteTextures(1, &g_gpu_brush.heatmap_white_tex);
    }
}

void we_gpu_weight_brush_destroy(void)
{
    EGLContext cur_ctx = eglGetCurrentContext();
    if (cur_ctx != EGL_NO_CONTEXT && g_gpu_brush.owner_ctx == cur_ctx) {
        delete_gpu_brush_gl_objects();
    }
    memset(&g_gpu_brush, 0, sizeof(g_gpu_brush));
}

static bool gpu_heatmap_compile_program(void)
{
    if (g_gpu_brush.heatmap_prog && glIsProgram(g_gpu_brush.heatmap_prog)) {
        return true;
    }
    GLuint hvs = compile_gl_shader_parts(GL_VERTEX_SHADER, &k_heatmap_vert_glsl, 1);
    GLuint hfs = compile_gl_shader_parts(GL_FRAGMENT_SHADER, &k_heatmap_frag_glsl, 1);
    if (hvs && hfs) {
        GLuint hprog = glCreateProgram();
        glAttachShader(hprog, hvs);
        glAttachShader(hprog, hfs);
        glLinkProgram(hprog);
        GLint hok = 0;
        glGetProgramiv(hprog, GL_LINK_STATUS, &hok);
        if (hok) {
            g_gpu_brush.heatmap_prog = hprog;
        } else {
            glDeleteProgram(hprog);
        }
    }
    if (hvs) {
        glDeleteShader(hvs);
    }
    if (hfs) {
        glDeleteShader(hfs);
    }
    return g_gpu_brush.heatmap_prog != 0u;
}

static bool gpu_weight_brush_ensure_ready(void)
{
    EGLContext cur_ctx = eglGetCurrentContext();
    if (cur_ctx == EGL_NO_CONTEXT) {
        return false;
    }
    if (g_gpu_brush.ready && g_gpu_brush.owner_ctx == cur_ctx &&
        glIsProgram(g_gpu_brush.compute_prog)) {
        return true;
    }

    GLint major = 0, minor = 0, ssbo_blocks = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &ssbo_blocks);
    if (major < 3 || (major == 3 && minor < 1) || ssbo_blocks < 4) {
        return false;
    }

    if (g_gpu_brush.owner_ctx == cur_ctx) {
        delete_gpu_brush_gl_objects();
    }
    memset(&g_gpu_brush, 0, sizeof(g_gpu_brush));
    g_gpu_brush.owner_ctx = cur_ctx;

    GLuint cs = compile_gl_shader_parts(GL_COMPUTE_SHADER, k_weight_brush_comp_parts, 3);
    if (!cs) {
        return false;
    }
    GLuint cprog = glCreateProgram();
    glAttachShader(cprog, cs);
    glLinkProgram(cprog);
    glDeleteShader(cs);
    GLint ok = 0;
    glGetProgramiv(cprog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(cprog);
        return false;
    }
    g_gpu_brush.compute_prog = cprog;
    (void)gpu_heatmap_compile_program();

    GLint prev_ssbo = 0, prev_ubo = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev_ssbo);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prev_ubo);

    glGenBuffers(1, &g_gpu_brush.ssbo_ping);
    glGenBuffers(1, &g_gpu_brush.ssbo_pong);
    glGenBuffers(1, &g_gpu_brush.ssbo_adj);
    glGenBuffers(1, &g_gpu_brush.ssbo_dirty);
    glGenBuffers(1, &g_gpu_brush.ubo_params);

    uint32_t zero_dirty[8] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_dirty);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero_dirty), zero_dirty, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, g_gpu_brush.ubo_params);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(gpu_brush_params_ubo_t), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)prev_ssbo);
    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prev_ubo);

    if (glGetError() != GL_NO_ERROR) {
        delete_gpu_brush_gl_objects();
        memset(&g_gpu_brush, 0, sizeof(g_gpu_brush));
        return false;
    }

    g_gpu_brush.ready = true;
    return true;
}

static bool build_packed_csr_adjacency(const we_puppet_t *puppet, uint32_t **out_buf,
                                       size_t *out_words)
{
    uint32_t V = puppet->vertex_count;
    uint32_t T = puppet->triangle_count;
    uint32_t *tri_verts = malloc((size_t)T * 3u * sizeof(uint32_t));
    uint32_t *opp_he = malloc((size_t)T * 3u * sizeof(uint32_t));
    double *he_len2 = malloc((size_t)T * 3u * sizeof(double));
    if (!tri_verts || !opp_he || !he_len2) {
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        return false;
    }
    boundary_edge_t *b_edges = NULL;
    uint32_t b_cnt = 0u;
    if (!build_intrinsic_mesh_and_boundaries(puppet, tri_verts, opp_he, he_len2, &b_edges,
                                             &b_cnt)) {
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        return false;
    }
    free(b_edges);

    laplacian_csr_t L;
    if (!assemble_cotangent_laplacian(V, T, tri_verts, he_len2, &L)) {
        free(tri_verts);
        free(opp_he);
        free(he_len2);
        return false;
    }
    free(tri_verts);
    free(opp_he);
    free(he_len2);

    uint32_t E = L.row_ptr[V];
    size_t row_off = 0u;
    size_t col_off = (size_t)V + 1u;
    size_t w_off = col_off + (size_t)E;
    size_t data_words = w_off + (size_t)E;
    size_t total_words = 4u + data_words;

    uint32_t *packed = malloc(total_words * sizeof(uint32_t));
    if (!packed) {
        free_laplacian_csr(&L);
        return false;
    }
    packed[0] = (uint32_t)row_off;
    packed[1] = (uint32_t)col_off;
    packed[2] = (uint32_t)w_off;
    packed[3] = V;

    memcpy(packed + 4u + row_off, L.row_ptr, ((size_t)V + 1u) * sizeof(uint32_t));
    if (E > 0u) {
        memcpy(packed + 4u + col_off, L.col_idx, (size_t)E * sizeof(uint32_t));
        for (uint32_t e = 0; e < E; e++) {
            float fw = (float)L.edge_w[e];
            uint32_t bits = 0u;
            memcpy(&bits, &fw, sizeof(bits));
            packed[4u + w_off + e] = bits;
        }
    }

    free_laplacian_csr(&L);
    *out_buf = packed;
    *out_words = total_words;
    return true;
}

static void normalize_readback_vertex_with_locks(we_mdl_vertex_t *dv,
                                                 const uint64_t locked_bone_mask[2])
{
    float locked_sum = 0.0f;
    float unlocked_sum = 0.0f;
    for (int k = 0; k < 4; k++) {
        if (dv->weight[k] <= 0.0f) {
            continue;
        }
        if (is_bone_locked_mask(dv->blend_indices[k], locked_bone_mask)) {
            locked_sum += dv->weight[k];
        } else {
            unlocked_sum += dv->weight[k];
        }
    }
    if (unlocked_sum > 1e-6f) {
        float budget = fmaxf(0.0f, 1.0f - locked_sum);
        float inv = budget / unlocked_sum;
        int fix_slot = -1;
        float best_w = -1.0f;
        for (int k = 0; k < 4; k++) {
            if (dv->weight[k] > 0.0f &&
                !is_bone_locked_mask(dv->blend_indices[k], locked_bone_mask)) {
                dv->weight[k] *= inv;
                if (dv->weight[k] > best_w) {
                    best_w = dv->weight[k];
                    fix_slot = k;
                }
            }
        }
        if (fix_slot >= 0) {
            float others = 0.0f;
            for (int k = 0; k < 4; k++) {
                if (k != fix_slot) {
                    others += dv->weight[k];
                }
            }
            dv->weight[fix_slot] = fmaxf(0.0f, 1.0f - others);
        }
    }
}

static void sync_puppet_gl_vbo_skin_block(const we_puppet_t *puppet, uint32_t v0, uint32_t v1)
{
    if (!puppet || puppet->gl_vbo_skin == 0u || v1 <= v0) {
        return;
    }
    gpu_skin_vbo_attrib_t skin_blk[WE_RIG_COW_BLOCK_VERTS];
    uint32_t cnt = v1 - v0;
    if (cnt > WE_RIG_COW_BLOCK_VERTS) {
        cnt = WE_RIG_COW_BLOCK_VERTS;
    }
    for (uint32_t j = 0; j < cnt; j++) {
        const we_mdl_vertex_t *sv = &puppet->vertices[v0 + j];
        for (int k = 0; k < 4; k++) {
            uint32_t bidx = sv->blend_indices[k];
            if (puppet->bone_count > 0u && bidx >= puppet->bone_count) {
                bidx = 0u;
            }
            skin_blk[j].blend_indices_f[k] = (float)bidx;
            skin_blk[j].blend_indices_u[k] = bidx;
            skin_blk[j].weight[k] = sv->weight[k];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, puppet->gl_vbo_skin);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(v0 * sizeof(gpu_skin_vbo_attrib_t)),
                    (GLsizeiptr)(cnt * sizeof(gpu_skin_vbo_attrib_t)), skin_blk);
}

static bool gpu_weight_brush_dispatch(we_puppet_t *puppet, const float axy[2], const float bxy[2],
                                      float radius_a, float radius_b, float strength,
                                      we_weight_brush_op_t op, uint32_t active_bone,
                                      float target_value, const uint64_t locked_bone_mask[2],
                                      const uint64_t *locked_vert_bs,
                                      uint64_t *inout_dirty_block_mask, uint32_t *out_modified_cnt)
{
    if (!puppet->gl_uploaded || !gpu_weight_brush_ensure_ready()) {
        return false;
    }

    uint32_t V = puppet->vertex_count;
    size_t vert_bytes = (size_t)V * sizeof(gpu_weight_vert_t);
    gpu_weight_vert_t *staging = malloc(vert_bytes);
    if (!staging) {
        return false;
    }

    for (uint32_t i = 0; i < V; i++) {
        const we_mdl_vertex_t *sv = &puppet->vertices[i];
        staging[i].pos_lock[0] = sv->pos[0];
        staging[i].pos_lock[1] = sv->pos[1];
        staging[i].pos_lock[2] = sv->pos[2];
        staging[i].pos_lock[3] = vert_is_locked(i, locked_vert_bs) ? 1.0f : 0.0f;
        for (int k = 0; k < 4; k++) {
            staging[i].blend_indices[k] = sv->blend_indices[k];
            staging[i].weight[k] = sv->weight[k];
        }
    }

    uint32_t *adj_buf = NULL;
    size_t adj_words = 0u;
    if (op == WE_WEIGHT_OP_SMOOTH) {
        if (!build_packed_csr_adjacency(puppet, &adj_buf, &adj_words)) {
            free(staging);
            return false;
        }
    } else {
        adj_words = 6u;
        adj_buf = calloc(adj_words, sizeof(uint32_t));
        if (!adj_buf) {
            free(staging);
            return false;
        }
    }

    GLint prev_prog = 0;
    GLint prev_vbo = 0;
    GLint prev_ssbo = 0;
    GLint prev_ubo = 0;
    GLint prev_ssbo_idx[4] = {0, 0, 0, 0};
    GLint prev_ubo_idx0 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev_ssbo);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prev_ubo);
    for (GLuint b = 0; b < 4u; b++) {
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, b, &prev_ssbo_idx[b]);
    }
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0u, &prev_ubo_idx0);

    bool ok = false;
    uint32_t modified_cnt = 0u;

    if (V > g_gpu_brush.ssbo_vert_cap) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_ping);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)vert_bytes, staging, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_pong);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)vert_bytes, NULL, GL_DYNAMIC_READ);
        g_gpu_brush.ssbo_vert_cap = V;
    } else {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_ping);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)vert_bytes, staging);
    }

    size_t adj_bytes = adj_words * sizeof(uint32_t);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_adj);
    if (adj_bytes > g_gpu_brush.adj_bytes_cap) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)adj_bytes, adj_buf, GL_DYNAMIC_DRAW);
        g_gpu_brush.adj_bytes_cap = adj_bytes;
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)adj_bytes, adj_buf);
    }
    free(adj_buf);
    adj_buf = NULL;

    uint32_t zero_dirty[8] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_dirty);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero_dirty), zero_dirty);

    gpu_brush_params_ubo_t ubo = {0};
    ubo.a[0] = axy[0];
    ubo.a[1] = axy[1];
    ubo.b[0] = bxy[0];
    ubo.b[1] = bxy[1];
    ubo.ra = radius_a;
    ubo.rb = radius_b;
    ubo.hardness = 0.25f;
    ubo.strength = strength;
    ubo.vertex_count = V;
    ubo.active_bone = active_bone;
    ubo.op_mode = (uint32_t)op;
    ubo.target_value = target_value;
    if (locked_bone_mask) {
        ubo.locked_bones[0] = (uint32_t)(locked_bone_mask[0] & 0xFFFFFFFFull);
        ubo.locked_bones[1] = (uint32_t)(locked_bone_mask[0] >> 32u);
        ubo.locked_bones[2] = (uint32_t)(locked_bone_mask[1] & 0xFFFFFFFFull);
        ubo.locked_bones[3] = (uint32_t)(locked_bone_mask[1] >> 32u);
    }
    ubo.prune_eps = 1e-4f;
    ubo.bone_count = puppet->bone_count;

    glBindBuffer(GL_UNIFORM_BUFFER, g_gpu_brush.ubo_params);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ubo), &ubo);

    glUseProgram(g_gpu_brush.compute_prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_gpu_brush.ssbo_ping);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_gpu_brush.ssbo_adj);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_gpu_brush.ssbo_pong);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g_gpu_brush.ssbo_dirty);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_gpu_brush.ubo_params);

    GLuint groups = (V + 63u) / 64u;
    glDispatchCompute(groups, 1u, 1u);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t dirty_words[8] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_dirty);
    void *dptr =
        glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(dirty_words), GL_MAP_READ_BIT);
    if (!dptr) {
        goto restore_gl;
    }
    memcpy(dirty_words, dptr, sizeof(dirty_words));
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_pong);
    const gpu_weight_vert_t *out_ptr = (const gpu_weight_vert_t *)glMapBufferRange(
        GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)vert_bytes, GL_MAP_READ_BIT);
    if (!out_ptr) {
        goto restore_gl;
    }

    if (glGetError() != GL_NO_ERROR) {
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        goto restore_gl;
    }

    uint32_t num_blocks = (V + WE_RIG_COW_BLOCK_VERTS - 1u) / WE_RIG_COW_BLOCK_VERTS;
    for (uint32_t blk = 0; blk < num_blocks; blk++) {
        if (((dirty_words[blk >> 5u] >> (blk & 31u)) & 1u) == 0u) {
            continue;
        }
        if (inout_dirty_block_mask) {
            inout_dirty_block_mask[blk >> 6u] |= (1ull << (blk & 63u));
        }
        uint32_t v0 = blk * WE_RIG_COW_BLOCK_VERTS;
        uint32_t v1 = v0 + WE_RIG_COW_BLOCK_VERTS;
        if (v1 > V) {
            v1 = V;
        }
        for (uint32_t i = v0; i < v1; i++) {
            we_mdl_vertex_t *dv = &puppet->vertices[i];
            bool changed = false;
            for (int k = 0; k < 4; k++) {
                if (dv->blend_indices[k] != out_ptr[i].blend_indices[k] ||
                    fabsf(dv->weight[k] - out_ptr[i].weight[k]) > 1e-6f) {
                    changed = true;
                }
                dv->blend_indices[k] = out_ptr[i].blend_indices[k];
                dv->weight[k] = out_ptr[i].weight[k];
            }
            if (changed) {
                normalize_readback_vertex_with_locks(dv, locked_bone_mask);
                modified_cnt++;
            }
        }
        sync_puppet_gl_vbo_skin_block(puppet, v0, v1);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_gpu_brush.ssbo_pong);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    ok = (glGetError() == GL_NO_ERROR);

restore_gl:
    for (GLuint b = 0; b < 4u; b++) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, (GLuint)prev_ssbo_idx[b]);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0u, (GLuint)prev_ubo_idx0);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)prev_ssbo);
    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prev_ubo);
    glUseProgram((GLuint)prev_prog);
    free(adj_buf);
    free(staging);

    if (ok) {
        *out_modified_cnt = modified_cnt;
    }
    return ok;
}

uint32_t we_mesh_apply_weight_paint_dab(we_puppet_t *puppet, const float axy[2], const float bxy[2],
                                        float radius_a, float radius_b, float strength,
                                        we_weight_brush_op_t op, uint32_t active_bone,
                                        float target_value, const uint64_t locked_bone_mask[2],
                                        const uint64_t *locked_vert_bs,
                                        uint64_t *inout_dirty_block_mask)
{
    if (!puppet || !puppet->vertices || puppet->vertex_count == 0u || puppet->bone_count == 0u ||
        !axy || !bxy) {
        return 0u;
    }
    if (!isfinite(axy[0]) || !isfinite(axy[1]) || !isfinite(bxy[0]) || !isfinite(bxy[1]) ||
        !isfinite(radius_a) || !isfinite(radius_b) || !isfinite(strength)) {
        return 0u;
    }

    float ra = fmaxf(radius_a, 0.0f);
    float rb = fmaxf(radius_b, 0.0f);
    float str = clampf(strength, 0.0f, 1.0f);
    if (fmaxf(ra, rb) <= 1e-6f || str <= 1e-6f) {
        return 0u;
    }

    uint32_t gpu_modified = 0u;
    if (gpu_weight_brush_dispatch(puppet, axy, bxy, ra, rb, str, op, active_bone, target_value,
                                  locked_bone_mask, locked_vert_bs, inout_dirty_block_mask,
                                  &gpu_modified)) {
        return gpu_modified;
    }

    uint32_t V = puppet->vertex_count;
    uint32_t B = puppet->bone_count;

    /* Snapshot weights & build 1-ring cotangent adjacency for order-independent SMOOTH */
    laplacian_csr_t smooth_L = {0};
    float *snap_active_w = NULL;
    if (op == WE_WEIGHT_OP_SMOOTH && puppet->indices && puppet->triangle_count > 0u) {
        uint32_t T = puppet->triangle_count;
        uint32_t *tri_verts = malloc((size_t)T * 3u * sizeof(uint32_t));
        uint32_t *opp_he = malloc((size_t)T * 3u * sizeof(uint32_t));
        double *he_len2 = malloc((size_t)T * 3u * sizeof(double));
        if (tri_verts && opp_he && he_len2) {
            boundary_edge_t *b_edges = NULL;
            uint32_t b_cnt = 0u;
            if (build_intrinsic_mesh_and_boundaries(puppet, tri_verts, opp_he, he_len2, &b_edges,
                                                    &b_cnt)) {
                (void)assemble_cotangent_laplacian(V, T, tri_verts, he_len2, &smooth_L);
                free(b_edges);
            }
        }
        free(tri_verts);
        free(opp_he);
        free(he_len2);

        snap_active_w = calloc(V, sizeof(float));
        if (snap_active_w) {
            for (uint32_t i = 0; i < V; i++) {
                for (int k = 0; k < 4; k++) {
                    if (puppet->vertices[i].blend_indices[k] == active_bone) {
                        snap_active_w[i] += puppet->vertices[i].weight[k];
                    }
                }
            }
        }
    }

    float ba_x = bxy[0] - axy[0];
    float ba_y = bxy[1] - axy[1];
    float l2 = ba_x * ba_x + ba_y * ba_y;
    const float hardness = 0.25f;
    const float edge1 = fmaxf(1.0f - hardness, 1e-3f);

    uint64_t local_dirty[WE_RIG_COW_WORDS] = {0};
    uint32_t modified_cnt = 0u;
    for (uint32_t vid = 0; vid < V; vid++) {
        if (vert_is_locked(vid, locked_vert_bs)) {
            continue;
        }
        we_mdl_vertex_t *v = &puppet->vertices[vid];
        float pa_x = v->pos[0] - axy[0];
        float pa_y = v->pos[1] - axy[1];
        float h = (l2 < 1e-8f) ? 0.0f : clampf((pa_x * ba_x + pa_y * ba_y) / l2, 0.0f, 1.0f);
        float r_loc = (l2 < 1e-8f) ? fmaxf(ra, rb) : (ra + h * (rb - ra));
        float dx = pa_x - ba_x * h;
        float dy = pa_y - ba_y * h;
        float sd = sqrtf(dx * dx + dy * dy) - r_loc;
        if (sd >= 0.0f) {
            continue;
        }

        float r_norm = clampf(-sd / fmaxf(r_loc, 1e-4f), 0.0f, 1.0f);
        float st = clampf(r_norm / edge1, 0.0f, 1.0f);
        float falloff = st * st * (3.0f - 2.0f * st);
        float effect = clampf(falloff * str, 0.0f, 1.0f);
        if (effect <= 1e-6f) {
            continue;
        }

        float cur = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (v->blend_indices[k] == active_bone) {
                cur += v->weight[k];
            }
        }

        float target = cur;
        bool do_renorm = true;
        if (op == WE_WEIGHT_OP_ADD) {
            target = cur + (1.0f - cur) * effect;
        } else if (op == WE_WEIGHT_OP_SUBTRACT) {
            target = fmaxf(0.0f, cur - effect);
        } else if (op == WE_WEIGHT_OP_REPLACE) {
            float tv = clampf(target_value, 0.0f, 1.0f);
            target = cur + (tv - cur) * effect;
        } else if (op == WE_WEIGHT_OP_SMOOTH) {
            if (smooth_L.row_ptr && snap_active_w) {
                double avg_w = 0.0;
                double w_sum = 0.0;
                for (uint32_t p = smooth_L.row_ptr[vid]; p < smooth_L.row_ptr[vid + 1u]; p++) {
                    double cw = smooth_L.edge_w[p];
                    avg_w += cw * (double)snap_active_w[smooth_L.col_idx[p]];
                    w_sum += cw;
                }
                if (w_sum > 1e-9) {
                    target = cur + ((float)(avg_w / w_sum) - cur) * effect;
                } else {
                    do_renorm = false;
                }
            } else {
                do_renorm = false;
            }
        } else {
            for (int k = 0; k < 4; k++) {
                if (v->weight[k] < 1e-4f &&
                    !is_bone_locked_mask(v->blend_indices[k], locked_bone_mask)) {
                    v->weight[k] = 0.0f;
                    v->blend_indices[k] = 0u;
                }
            }
        }

        if (do_renorm) {
            we_mesh_renormalize_vertex_weights(v, B, active_bone, target, locked_bone_mask);
            uint32_t blk = vid / WE_RIG_COW_BLOCK_VERTS;
            if ((blk >> 6u) < WE_RIG_COW_WORDS) {
                local_dirty[blk >> 6u] |= (1ull << (blk & 63u));
            }
            if (inout_dirty_block_mask) {
                inout_dirty_block_mask[blk >> 6u] |= (1ull << (blk & 63u));
            }
            modified_cnt++;
        }
    }

    if (modified_cnt > 0u && puppet->gl_uploaded && puppet->gl_vbo_skin != 0u &&
        eglGetCurrentContext() != EGL_NO_CONTEXT) {
        GLint prev_vbo = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
        uint32_t num_blocks = (V + WE_RIG_COW_BLOCK_VERTS - 1u) / WE_RIG_COW_BLOCK_VERTS;
        for (uint32_t blk = 0; blk < num_blocks; blk++) {
            if ((blk >> 6u) < WE_RIG_COW_WORDS &&
                ((local_dirty[blk >> 6u] >> (blk & 63u)) & 1ull) != 0ull) {
                uint32_t v0 = blk * WE_RIG_COW_BLOCK_VERTS;
                uint32_t v1 = v0 + WE_RIG_COW_BLOCK_VERTS;
                if (v1 > V) {
                    v1 = V;
                }
                sync_puppet_gl_vbo_skin_block(puppet, v0, v1);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    }

    free(snap_active_w);
    free_laplacian_csr(&smooth_L);
    return modified_cnt;
}

bool we_mesh_rig_draw_heatmap_gl(const we_puppet_t *puppet, const float (*skin_mats)[16],
                                 uint8_t mode, uint8_t palette, uint16_t active_bone, float opacity)
{
    if (!puppet || !puppet->vertices || !puppet->indices || puppet->vertex_count == 0u ||
        puppet->index_count == 0u || mode == 0u) {
        return false;
    }

    EGLContext cur_ctx = eglGetCurrentContext();
    if (cur_ctx == EGL_NO_CONTEXT) {
        return false;
    }

    if (!gpu_weight_brush_ensure_ready()) {
        GLint major = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        if (major < 3) {
            return false;
        }
        if (g_gpu_brush.owner_ctx == cur_ctx && g_gpu_brush.heatmap_prog == 0u) {
            if (!gpu_heatmap_compile_program()) {
                return false;
            }
        } else if (g_gpu_brush.owner_ctx != cur_ctx) {
            memset(&g_gpu_brush, 0, sizeof(g_gpu_brush));
            g_gpu_brush.owner_ctx = cur_ctx;
            if (!gpu_heatmap_compile_program()) {
                return false;
            }
        }
    }
    if (g_gpu_brush.heatmap_prog == 0u || !glIsProgram(g_gpu_brush.heatmap_prog)) {
        return false;
    }

    uint32_t V = puppet->vertex_count;
    we_skinned_vertex_t *skinned = malloc((size_t)V * sizeof(we_skinned_vertex_t));
    if (!skinned) {
        return false;
    }
    we_puppet_skin_vertices_cpu(puppet, skin_mats, skinned);

    gpu_skin_vbo_attrib_t *skin_buf = NULL;
    if (puppet->gl_vbo_skin == 0u) {
        skin_buf = calloc(V, sizeof(gpu_skin_vbo_attrib_t));
        if (!skin_buf) {
            free(skinned);
            return false;
        }
        for (uint32_t i = 0; i < V; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t bidx = puppet->vertices[i].blend_indices[j];
                if (puppet->bone_count > 0u && bidx >= puppet->bone_count) {
                    bidx = 0u;
                }
                skin_buf[i].blend_indices_f[j] = (float)bidx;
                skin_buf[i].blend_indices_u[j] = bidx;
                skin_buf[i].weight[j] = puppet->vertices[i].weight[j];
            }
        }
    }

    GLint prev_prog = 0;
    GLint prev_vao = 0;
    GLint prev_vbo = 0;
    GLint prev_active_tex = GL_TEXTURE0;
    GLint prev_tex0 = 0;
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    GLint prev_src_rgb = GL_ONE, prev_dst_rgb = GL_ZERO;
    GLint prev_src_a = GL_ONE, prev_dst_a = GL_ZERO;
    GLint prev_eq_rgb = GL_FUNC_ADD, prev_eq_a = GL_FUNC_ADD;
    GLint prev_depth_func = GL_LESS;
    GLboolean prev_depth_mask = GL_TRUE;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prev_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prev_eq_a);
    glGetIntegerv(GL_DEPTH_FUNC, &prev_depth_func);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);

    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    if (prev_prog > 0 && glIsProgram((GLuint)prev_prog)) {
        GLint mvp_loc = glGetUniformLocation((GLuint)prev_prog, "g_ModelViewProjectionMatrix");
        if (mvp_loc < 0) {
            mvp_loc = glGetUniformLocation((GLuint)prev_prog, "u_MVP");
        }
        if (mvp_loc >= 0) {
            glGetUniformfv((GLuint)prev_prog, mvp_loc, mvp);
        }
    }

    if (g_gpu_brush.heatmap_vao == 0u) {
        glGenVertexArrays(1, &g_gpu_brush.heatmap_vao);
    }
    if (g_gpu_brush.heatmap_vbo_dyn == 0u) {
        glGenBuffers(1, &g_gpu_brush.heatmap_vbo_dyn);
    }
    if (g_gpu_brush.heatmap_white_tex == 0u) {
        uint8_t white[4] = {255u, 255u, 255u, 255u};
        glGenTextures(1, &g_gpu_brush.heatmap_white_tex);
        glBindTexture(GL_TEXTURE_2D, g_gpu_brush.heatmap_white_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    }

    glBindVertexArray(g_gpu_brush.heatmap_vao);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu_brush.heatmap_vbo_dyn);
    if (V > g_gpu_brush.heatmap_vert_cap) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(V * sizeof(we_skinned_vertex_t)), skinned,
                     GL_DYNAMIC_DRAW);
        g_gpu_brush.heatmap_vert_cap = V;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(V * sizeof(we_skinned_vertex_t)), skinned);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(we_skinned_vertex_t), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(we_skinned_vertex_t),
                          (const void *)(3u * sizeof(float)));

    if (puppet->gl_vbo_skin != 0u) {
        glBindBuffer(GL_ARRAY_BUFFER, puppet->gl_vbo_skin);
    } else {
        if (g_gpu_brush.heatmap_vbo_skin == 0u) {
            glGenBuffers(1, &g_gpu_brush.heatmap_vbo_skin);
        }
        glBindBuffer(GL_ARRAY_BUFFER, g_gpu_brush.heatmap_vbo_skin);
        if (V > g_gpu_brush.heatmap_skin_cap) {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(V * sizeof(gpu_skin_vbo_attrib_t)), skin_buf,
                         GL_DYNAMIC_DRAW);
            g_gpu_brush.heatmap_skin_cap = V;
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(V * sizeof(gpu_skin_vbo_attrib_t)),
                            skin_buf);
        }
    }
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(gpu_skin_vbo_attrib_t),
                          (const void *)offsetof(gpu_skin_vbo_attrib_t, blend_indices_f));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(gpu_skin_vbo_attrib_t),
                          (const void *)offsetof(gpu_skin_vbo_attrib_t, weight));

    if (puppet->gl_ebo != 0u) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, puppet->gl_ebo);
    } else {
        if (g_gpu_brush.heatmap_ebo == 0u) {
            glGenBuffers(1, &g_gpu_brush.heatmap_ebo);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gpu_brush.heatmap_ebo);
        if (puppet->index_count > g_gpu_brush.heatmap_idx_cap) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizeiptr)(puppet->index_count * sizeof(uint16_t)), puppet->indices,
                         GL_DYNAMIC_DRAW);
            g_gpu_brush.heatmap_idx_cap = puppet->index_count;
        } else {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                            (GLsizeiptr)(puppet->index_count * sizeof(uint16_t)), puppet->indices);
        }
    }

    glUseProgram(g_gpu_brush.heatmap_prog);
    GLint loc_mvp = glGetUniformLocation(g_gpu_brush.heatmap_prog, "u_MVP");
    GLint loc_bone = glGetUniformLocation(g_gpu_brush.heatmap_prog, "u_ActiveBone");
    GLint loc_tex = glGetUniformLocation(g_gpu_brush.heatmap_prog, "u_Texture0");
    GLint loc_pal = glGetUniformLocation(g_gpu_brush.heatmap_prog, "u_PaletteId");
    GLint loc_alpha = glGetUniformLocation(g_gpu_brush.heatmap_prog, "u_OverlayAlpha");

    if (loc_mvp >= 0) {
        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
    }
    if (loc_bone >= 0) {
        glUniform1i(loc_bone, (GLint)active_bone);
    }
    if (prev_tex0 == 0) {
        glBindTexture(GL_TEXTURE_2D, g_gpu_brush.heatmap_white_tex);
    }
    if (loc_tex >= 0) {
        glUniform1i(loc_tex, 0);
    }
    if (loc_pal >= 0) {
        glUniform1i(loc_pal, (GLint)palette);
    }
    if (loc_alpha >= 0) {
        float ov_alpha = isfinite(opacity) ? clampf(opacity, 0.0f, 1.0f) : 0.75f;
        glUniform1f(loc_alpha, ov_alpha);
    }

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    glDrawElements(GL_TRIANGLES, (GLsizei)puppet->index_count, GL_UNSIGNED_SHORT, (const void *)0);

    glDepthFunc((GLenum)prev_depth_func);
    glDepthMask(prev_depth_mask);
    if (!prev_blend) {
        glDisable(GL_BLEND);
    }
    glBlendEquationSeparate((GLenum)prev_eq_rgb, (GLenum)prev_eq_a);
    glBlendFuncSeparate((GLenum)prev_src_rgb, (GLenum)prev_dst_rgb, (GLenum)prev_src_a,
                        (GLenum)prev_dst_a);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    glActiveTexture((GLenum)prev_active_tex);
    glBindVertexArray((GLuint)prev_vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    glUseProgram((GLuint)prev_prog);

    free(skin_buf);
    free(skinned);
    return glGetError() == GL_NO_ERROR;
}

/* ========================================================================= */
/* 4. Analytic 2-Bone Soft-Stretch IK, Look-At Aim & Multi-Layer Evaluation  */
/* ========================================================================= */

static inline void vec3_norm(float v[3])
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    } else {
        v[0] = 1.0f;
        v[1] = 0.0f;
        v[2] = 0.0f;
    }
}

static inline float vec3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void vec3_cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void mat3_shortest_arc_rotate(const float from_u[3], const float to_u[3], float M[16])
{
    float d = clampf(vec3_dot(from_u, to_u), -1.0f, 1.0f);
    if (d > 1.0f - 1e-6f) {
        return;
    }

    float axis[3];
    if (d < -1.0f + 1e-6f) {
        /* 180-degree turnaround: prefer Z axis in 2D puppet space */
        if (fabsf(from_u[2]) < 0.9f) {
            axis[0] = 0.0f;
            axis[1] = 0.0f;
            axis[2] = 1.0f;
        } else {
            axis[0] = 1.0f;
            axis[1] = 0.0f;
            axis[2] = 0.0f;
        }
    } else {
        vec3_cross(from_u, to_u, axis);
        vec3_norm(axis);
    }

    float angle = acosf(d);
    float c = cosf(angle);
    float s = sinf(angle);
    float omc = 1.0f - c;
    float x = axis[0], y = axis[1], z = axis[2];

    float R[9] = {
        c + x * x * omc,     y * x * omc + z * s, z * x * omc - y * s,
        x * y * omc - z * s, c + y * y * omc,     z * y * omc + x * s,
        x * z * omc + y * s, y * z * omc - x * s, c + z * z * omc,
    };

    for (int col = 0; col < 3; col++) {
        float vx = M[col * 4 + 0];
        float vy = M[col * 4 + 1];
        float vz = M[col * 4 + 2];
        M[col * 4 + 0] = R[0] * vx + R[3] * vy + R[6] * vz;
        M[col * 4 + 1] = R[1] * vx + R[4] * vy + R[7] * vz;
        M[col * 4 + 2] = R[2] * vx + R[5] * vy + R[8] * vz;
    }
}

static bool bone_is_descendant(const we_puppet_t *puppet, uint32_t child, uint32_t ancestor)
{
    uint32_t cur = puppet->bones[child].parent;
    uint32_t steps = 0u;
    while (cur != 0xFFFFFFFFu && steps < puppet->bone_count) {
        if (cur == ancestor) {
            return true;
        }
        cur = puppet->bones[cur].parent;
        steps++;
    }
    return false;
}

static void eval_world_mats_topo(const we_puppet_t *puppet, const uint32_t topo[WE_MDL_MAX_BONES],
                                 float (*const anim_local)[16], float (*out_world)[16])
{
    uint32_t bcnt = puppet->bone_count;
    for (uint32_t ti = 0; ti < bcnt; ti++) {
        uint32_t k = topo[ti];
        uint32_t p = puppet->bones[k].parent;
        if (p == 0xFFFFFFFFu) {
            memcpy(out_world[k], anim_local[k], sizeof(out_world[k]));
        } else {
            we_mat4_mul(out_world[p], anim_local[k], out_world[k]);
        }
    }
}

static void blend_local_trs(const float orig[16], const float solved[16], float w, float out[16])
{
    if (w >= 0.9999f) {
        memcpy(out, solved, 16u * sizeof(float));
        return;
    }
    if (w <= 1e-6f) {
        memcpy(out, orig, 16u * sizeof(float));
        return;
    }
    float p0[3], a0[3], s0[3], p1[3], a1[3], s1[3];
    rig_mat4_decompose_trs(orig, p0, a0, s0);
    rig_mat4_decompose_trs(solved, p1, a1, s1);

    float pos[3], ang[3], scl[3];
    for (int c = 0; c < 3; c++) {
        pos[c] = p0[c] + w * (p1[c] - p0[c]);
        float da = remainderf(a1[c] - a0[c], WE_RIG_TWO_PI_F);
        ang[c] = a0[c] + w * da;
        scl[c] = s0[c] + w * (s1[c] - s0[c]);
    }
    rig_mat4_from_trs(pos, ang, scl, out);
}

static void clamp_bone_z_angle_limit(const float bind_local[16], float min_rad, float max_rad,
                                     bool allow_zero_lock, float inout_local[16])
{
    if (!isfinite(min_rad) || !isfinite(max_rad) || max_rad < min_rad ||
        (!allow_zero_lock && min_rad == 0.0f && max_rad == 0.0f) || !mat4_is_finite(inout_local) ||
        !mat4_is_finite(bind_local)) {
        return;
    }
    float pos[3], ang[3], scl[3], bpos[3], bang[3], bscl[3];
    rig_mat4_decompose_trs(inout_local, pos, ang, scl);
    rig_mat4_decompose_trs(bind_local, bpos, bang, bscl);

    float rel = remainderf(ang[2] - bang[2], WE_RIG_TWO_PI_F);
    if (!isfinite(rel)) {
        return;
    }
    float clamped = clampf(rel, min_rad, max_rad);
    if (fabsf(clamped - rel) > 1e-6f) {
        ang[2] = bang[2] + clamped;
        rig_mat4_from_trs(pos, ang, scl, inout_local);
    }
}

bool we_ik_solve_two_bone(const we_puppet_t *puppet, const we_ik_chain_t *chain,
                          float (*inout_anim_local)[16])
{
    if (!puppet || !chain || !inout_anim_local || !chain->enabled) {
        return false;
    }
    uint32_t topo[WE_MDL_MAX_BONES];
    if (!validate_and_topo_sort_skeleton(puppet, topo, NULL)) {
        return false;
    }

    uint32_t bcnt = puppet->bone_count;
    uint32_t r = chain->root_bone;
    uint32_t m = chain->mid_bone;
    uint32_t e = chain->end_bone;
    if (r >= bcnt || m >= bcnt || e >= bcnt || r == m || m == e || r == e) {
        return false;
    }
    if (!bone_is_descendant(puppet, m, r) || !bone_is_descendant(puppet, e, m)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(chain->target_world[i]) || !isfinite(chain->pole_world[i])) {
            return false;
        }
    }
    if (!isfinite(chain->soft_zone_ratio) || !isfinite(chain->max_stretch_ratio) ||
        !isfinite(chain->blend_weight)) {
        return false;
    }

    float w_blend = clampf(chain->blend_weight, 0.0f, 1.0f);
    if (w_blend <= 1e-6f) {
        return true;
    }

    float bind_world[WE_MDL_MAX_BONES][16];
    compute_bind_world_matrices(puppet, topo, bind_world);
    float L1 =
        sqrtf((bind_world[m][12] - bind_world[r][12]) * (bind_world[m][12] - bind_world[r][12]) +
              (bind_world[m][13] - bind_world[r][13]) * (bind_world[m][13] - bind_world[r][13]) +
              (bind_world[m][14] - bind_world[r][14]) * (bind_world[m][14] - bind_world[r][14]));
    float L2 =
        sqrtf((bind_world[e][12] - bind_world[m][12]) * (bind_world[e][12] - bind_world[m][12]) +
              (bind_world[e][13] - bind_world[m][13]) * (bind_world[e][13] - bind_world[m][13]) +
              (bind_world[e][14] - bind_world[m][14]) * (bind_world[e][14] - bind_world[m][14]));
    if (!isfinite(L1) || !isfinite(L2) || L1 <= 1e-5f || L2 <= 1e-5f) {
        return false;
    }

    float orig_r[16], orig_m[16], orig_e[16];
    memcpy(orig_r, inout_anim_local[r], sizeof(orig_r));
    memcpy(orig_m, inout_anim_local[m], sizeof(orig_m));
    memcpy(orig_e, inout_anim_local[e], sizeof(orig_e));
    /* Bit 1 (0x2) opts into strict [0, 0] angle locking while keeping zero-initialized limits
     * unconstrained. */
    bool allow_zero_lock = (chain->enabled & 0x2u) != 0u;

    float world[WE_MDL_MAX_BONES][16];
    eval_world_mats_topo(puppet, topo, inout_anim_local, world);

    float p0[3] = {world[r][12], world[r][13], world[r][14]};
    float p1[3] = {world[m][12], world[m][13], world[m][14]};
    float p2[3] = {world[e][12], world[e][13], world[e][14]};

    float to_tgt[3] = {chain->target_world[0] - p0[0], chain->target_world[1] - p0[1],
                       chain->target_world[2] - p0[2]};
    float d_raw = sqrtf(vec3_dot(to_tgt, to_tgt));
    float d_hat[3];
    if (d_raw > 1e-6f) {
        d_hat[0] = to_tgt[0] / d_raw;
        d_hat[1] = to_tgt[1] / d_raw;
        d_hat[2] = to_tgt[2] / d_raw;
    } else {
        d_hat[0] = p2[0] - p0[0];
        d_hat[1] = p2[1] - p0[1];
        d_hat[2] = p2[2] - p0[2];
        vec3_norm(d_hat);
    }

    float L_max = L1 + L2;
    float soft_ratio = clampf(chain->soft_zone_ratio, 0.0f, 0.25f);
    float delta = soft_ratio * L_max;
    float d_soft = L_max - delta;
    float max_stretch = clampf(chain->max_stretch_ratio, 1.0f, 1.5f);

    float d_low_hard = fabsf(L1 - L2) + 1e-4f * L_max;
    float low_zone = fmaxf(0.02f * L_max, 1e-4f);
    float d_low_thresh = d_low_hard + low_zone;

    float L1_eff = L1;
    float L2_eff = L2;
    float d_eff = d_raw;

    if (d_raw < d_low_thresh) {
        d_eff = d_low_hard + low_zone * expf((d_raw - d_low_thresh) / low_zone);
    } else if (d_raw > d_soft) {
        float excess = d_raw - d_soft;
        if (max_stretch > 1.0f + 1e-6f) {
            float sigma = 1.0f + (max_stretch - 1.0f) *
                                     tanhf(excess / (delta * (max_stretch - 1.0f) + 1e-5f));
            L1_eff = sigma * L1;
            L2_eff = sigma * L2;
            float stretch_span = max_stretch * L_max - d_soft;
            d_eff = d_soft + stretch_span * tanhf(excess / (stretch_span + 1e-6f));
            float max_lim = (L1_eff + L2_eff) * (1.0f - 1e-5f);
            if (d_eff > max_lim) {
                d_eff = max_lim;
            }
        } else if (delta > 1e-6f) {
            d_eff = d_soft + delta * (1.0f - expf(-excess / delta));
        } else {
            d_eff = L_max;
        }
    }

    float cos_alpha =
        clampf((L1_eff * L1_eff + d_eff * d_eff - L2_eff * L2_eff) / (2.0f * L1_eff * d_eff),
               -1.0f + 1e-6f, 1.0f - 1e-6f);
    float alpha = acosf(cos_alpha);

    /* Orthonormal pole plane (d_hat, n_perp) */
    float n_perp[3] = {0.0f, 0.0f, 0.0f};
    bool has_explicit_pole = (fabsf(chain->pole_world[0]) + fabsf(chain->pole_world[1]) +
                              fabsf(chain->pole_world[2])) > 1e-6f;
    if (has_explicit_pole) {
        float pv[3] = {chain->pole_world[0] - p0[0], chain->pole_world[1] - p0[1],
                       chain->pole_world[2] - p0[2]};
        float proj = vec3_dot(pv, d_hat);
        n_perp[0] = pv[0] - proj * d_hat[0];
        n_perp[1] = pv[1] - proj * d_hat[1];
        n_perp[2] = pv[2] - proj * d_hat[2];
    }
    float n_len = sqrtf(vec3_dot(n_perp, n_perp));
    if (n_len > 1e-5f) {
        n_perp[0] /= n_len;
        n_perp[1] /= n_len;
        n_perp[2] /= n_len;
    } else {
        float sgn = (chain->bend_sign < 0) ? -1.0f : 1.0f;
        if (fabsf(d_hat[0]) + fabsf(d_hat[1]) > 1e-4f) {
            n_perp[0] = -d_hat[1] * sgn;
            n_perp[1] = d_hat[0] * sgn;
            n_perp[2] = 0.0f;
        } else {
            n_perp[0] = sgn;
            n_perp[1] = 0.0f;
            n_perp[2] = 0.0f;
        }
        vec3_norm(n_perp);
    }

    float u1_desired[3] = {
        cosf(alpha) * d_hat[0] + sinf(alpha) * n_perp[0],
        cosf(alpha) * d_hat[1] + sinf(alpha) * n_perp[1],
        cosf(alpha) * d_hat[2] + sinf(alpha) * n_perp[2],
    };
    vec3_norm(u1_desired);

    float u1_cur[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    vec3_norm(u1_cur);

    float new_world_r[16];
    memcpy(new_world_r, world[r], sizeof(new_world_r));
    mat3_shortest_arc_rotate(u1_cur, u1_desired, new_world_r);

    if (puppet->bones[r].parent == 0xFFFFFFFFu) {
        memcpy(inout_anim_local[r], new_world_r, sizeof(new_world_r));
    } else {
        float inv_p[16];
        if (we_mat4_inverse(world[puppet->bones[r].parent], inv_p)) {
            we_mat4_mul(inv_p, new_world_r, inout_anim_local[r]);
        }
    }
    clamp_bone_z_angle_limit(puppet->bones[r].local_trans, chain->root_angle_min_rad,
                             chain->root_angle_max_rad, allow_zero_lock, inout_anim_local[r]);

    /* Scale segment translations smoothly when soft-stretch elongates L1/L2 */
    float sigma_stretch = L1_eff / L1;
    if (fabsf(sigma_stretch - 1.0f) > 1e-5f) {
        inout_anim_local[m][12] *= sigma_stretch;
        inout_anim_local[m][13] *= sigma_stretch;
        inout_anim_local[m][14] *= sigma_stretch;
        inout_anim_local[e][12] *= sigma_stretch;
        inout_anim_local[e][13] *= sigma_stretch;
        inout_anim_local[e][14] *= sigma_stretch;
    }

    /* Recompute world matrices after root rotation so mid-bone aims cleanly at target */
    eval_world_mats_topo(puppet, topo, inout_anim_local, world);
    float p1_upd[3] = {world[m][12], world[m][13], world[m][14]};
    float p2_upd[3] = {world[e][12], world[e][13], world[e][14]};
    float target_eff[3] = {
        p0[0] + d_eff * d_hat[0],
        p0[1] + d_eff * d_hat[1],
        p0[2] + d_eff * d_hat[2],
    };

    float u2_cur[3] = {p2_upd[0] - p1_upd[0], p2_upd[1] - p1_upd[1], p2_upd[2] - p1_upd[2]};
    float u2_desired[3] = {target_eff[0] - p1_upd[0], target_eff[1] - p1_upd[1],
                           target_eff[2] - p1_upd[2]};
    vec3_norm(u2_cur);
    vec3_norm(u2_desired);

    float new_world_m[16];
    memcpy(new_world_m, world[m], sizeof(new_world_m));
    mat3_shortest_arc_rotate(u2_cur, u2_desired, new_world_m);

    float inv_pm[16];
    if (we_mat4_inverse(world[puppet->bones[m].parent], inv_pm)) {
        we_mat4_mul(inv_pm, new_world_m, inout_anim_local[m]);
    }
    clamp_bone_z_angle_limit(puppet->bones[m].local_trans, chain->mid_angle_min_rad,
                             chain->mid_angle_max_rad, allow_zero_lock, inout_anim_local[m]);

    blend_local_trs(orig_r, inout_anim_local[r], w_blend, inout_anim_local[r]);
    blend_local_trs(orig_m, inout_anim_local[m], w_blend, inout_anim_local[m]);
    for (int c = 12; c < 15; c++) {
        inout_anim_local[e][c] = orig_e[c] + w_blend * (inout_anim_local[e][c] - orig_e[c]);
    }
    return true;
}

bool we_ik_solve_aim(const we_puppet_t *puppet, const we_aim_constraint_t *aim,
                     float (*inout_anim_local)[16])
{
    if (!puppet || !aim || !inout_anim_local || !aim->enabled) {
        return false;
    }
    uint32_t topo[WE_MDL_MAX_BONES];
    if (!validate_and_topo_sort_skeleton(puppet, topo, NULL)) {
        return false;
    }
    uint32_t k = aim->bone_index;
    if (k >= puppet->bone_count) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(aim->target_world[i]) || !isfinite(aim->aim_axis_local[i])) {
            return false;
        }
    }

    float w_blend = clampf(aim->blend_weight, 0.0f, 1.0f);
    if (w_blend <= 1e-6f) {
        return true;
    }

    float world[WE_MDL_MAX_BONES][16];
    eval_world_mats_topo(puppet, topo, inout_anim_local, world);

    float to_tgt[3] = {
        aim->target_world[0] - world[k][12],
        aim->target_world[1] - world[k][13],
        aim->target_world[2] - world[k][14],
    };
    float dist = sqrtf(vec3_dot(to_tgt, to_tgt));
    if (dist <= 1e-6f) {
        return true;
    }
    float dir_tgt[3] = {to_tgt[0] / dist, to_tgt[1] / dist, to_tgt[2] / dist};

    float local_ax[3] = {aim->aim_axis_local[0], aim->aim_axis_local[1], aim->aim_axis_local[2]};
    if (vec3_dot(local_ax, local_ax) <= 1e-10f) {
        local_ax[0] = 1.0f;
        local_ax[1] = 0.0f;
        local_ax[2] = 0.0f;
    } else {
        vec3_norm(local_ax);
    }

    float dir_cur[3] = {
        world[k][0] * local_ax[0] + world[k][4] * local_ax[1] + world[k][8] * local_ax[2],
        world[k][1] * local_ax[0] + world[k][5] * local_ax[1] + world[k][9] * local_ax[2],
        world[k][2] * local_ax[0] + world[k][6] * local_ax[1] + world[k][10] * local_ax[2],
    };
    vec3_norm(dir_cur);

    float cos_th = clampf(vec3_dot(dir_cur, dir_tgt), -1.0f, 1.0f);
    float theta = acosf(cos_th);
    if (theta <= 1e-6f) {
        return true;
    }

    float max_cone = isfinite(aim->max_cone_rad) ? aim->max_cone_rad : 0.0f;
    float eff_theta = (max_cone > 1e-6f && theta > max_cone) ? max_cone : theta;
    eff_theta *= w_blend;

    float axis[3];
    if (cos_th < -1.0f + 1e-6f) {
        axis[0] = 0.0f;
        axis[1] = 0.0f;
        axis[2] = 1.0f;
    } else {
        vec3_cross(dir_cur, dir_tgt, axis);
        vec3_norm(axis);
    }

    float c = cosf(eff_theta);
    float s = sinf(eff_theta);
    float omc = 1.0f - c;
    float x = axis[0], y = axis[1], z = axis[2];
    float R[9] = {
        c + x * x * omc,     y * x * omc + z * s, z * x * omc - y * s,
        x * y * omc - z * s, c + y * y * omc,     z * y * omc + x * s,
        x * z * omc + y * s, y * z * omc - x * s, c + z * z * omc,
    };

    float new_world[16];
    memcpy(new_world, world[k], sizeof(new_world));
    for (int col = 0; col < 3; col++) {
        float vx = new_world[col * 4 + 0];
        float vy = new_world[col * 4 + 1];
        float vz = new_world[col * 4 + 2];
        new_world[col * 4 + 0] = R[0] * vx + R[3] * vy + R[6] * vz;
        new_world[col * 4 + 1] = R[1] * vx + R[4] * vy + R[7] * vz;
        new_world[col * 4 + 2] = R[2] * vx + R[5] * vy + R[8] * vz;
    }

    if (puppet->bones[k].parent == 0xFFFFFFFFu) {
        memcpy(inout_anim_local[k], new_world, sizeof(new_world));
    } else {
        float inv_p[16];
        if (we_mat4_inverse(world[puppet->bones[k].parent], inv_p)) {
            we_mat4_mul(inv_p, new_world, inout_anim_local[k]);
        }
    }
    return true;
}

void we_rig_apply_constraints(const we_puppet_t *puppet, const we_rig_pose_ctx_t *rig,
                              float (*inout_anim_local)[16])
{
    if (!puppet || !rig || !inout_anim_local || puppet->bone_count == 0u) {
        return;
    }
    uint32_t bcnt = puppet->bone_count < WE_MDL_MAX_BONES ? puppet->bone_count : WE_MDL_MAX_BONES;
    uint32_t ik_cnt =
        rig->ik_chain_count < WE_RIG_MAX_IK_CHAINS ? rig->ik_chain_count : WE_RIG_MAX_IK_CHAINS;
    for (uint32_t c = 0; c < ik_cnt; c++) {
        const we_ik_chain_t *ch = &rig->ik_chains[c];
        if (!ch->enabled || ch->root_bone >= bcnt || ch->mid_bone >= bcnt) {
            continue;
        }
        if (rig->bone_drive_modes[ch->root_bone] == WE_BONE_DRIVE_XPBD ||
            rig->bone_drive_modes[ch->mid_bone] == WE_BONE_DRIVE_XPBD ||
            rig->bone_drive_modes[ch->root_bone] == WE_BONE_DRIVE_AIM ||
            rig->bone_drive_modes[ch->mid_bone] == WE_BONE_DRIVE_AIM) {
            continue;
        }
        bool protect_end_xpbd =
            (ch->end_bone < bcnt && rig->bone_drive_modes[ch->end_bone] == WE_BONE_DRIVE_XPBD);
        float saved_end[16];
        if (protect_end_xpbd) {
            memcpy(saved_end, inout_anim_local[ch->end_bone], sizeof(saved_end));
        }
        we_ik_solve_two_bone(puppet, ch, inout_anim_local);
        if (protect_end_xpbd) {
            memcpy(inout_anim_local[ch->end_bone], saved_end, sizeof(saved_end));
        }
    }

    uint32_t aim_cnt =
        rig->aim_count < WE_RIG_MAX_AIM_CONSTRAINTS ? rig->aim_count : WE_RIG_MAX_AIM_CONSTRAINTS;
    for (uint32_t a = 0; a < aim_cnt; a++) {
        const we_aim_constraint_t *aim = &rig->aim_constraints[a];
        if (!aim->enabled || aim->bone_index >= bcnt) {
            continue;
        }
        if (rig->bone_drive_modes[aim->bone_index] == WE_BONE_DRIVE_XPBD ||
            rig->bone_drive_modes[aim->bone_index] == WE_BONE_DRIVE_IK) {
            continue;
        }
        we_ik_solve_aim(puppet, aim, inout_anim_local);
    }
}

static const we_mdl_anim_t *rig_find_anim(const we_puppet_t *puppet, const we_puppet_layer_t *layer)
{
    if (!puppet || !layer || puppet->anim_count == 0u || !puppet->anims) {
        return NULL;
    }
    if (layer->anim_name[0] != '\0') {
        for (uint32_t i = 0; i < puppet->anim_count; i++) {
            if (strcasecmp(puppet->anims[i].name, layer->anim_name) == 0) {
                return &puppet->anims[i];
            }
        }
    }
    for (uint32_t i = 0; i < puppet->anim_count; i++) {
        if (puppet->anims[i].id == layer->anim_id) {
            return &puppet->anims[i];
        }
    }
    if (layer->anim_id >= 0 && (uint32_t)layer->anim_id < puppet->anim_count) {
        return &puppet->anims[layer->anim_id];
    }
    return NULL;
}

static void rig_eval_track_at_time(const we_mdl_anim_t *anim, const we_mdl_bone_track_t *tr,
                                   double time_sec, float out_pos[3], float out_angle[3],
                                   float out_scale[3])
{
    if (!tr || tr->kf_count == 0u || !tr->keyframes) {
        return;
    }
    if (tr->kf_count == 1u) {
        memcpy(out_pos, tr->keyframes[0].pos, 3u * sizeof(float));
        memcpy(out_angle, tr->keyframes[0].angle, 3u * sizeof(float));
        memcpy(out_scale, tr->keyframes[0].scale, 3u * sizeof(float));
        return;
    }

    double fps = (anim->fps > 0.0f) ? (double)anim->fps : 30.0;
    double frame_pos = (isfinite(time_sec) ? time_sec : 0.0) * fps;
    uint32_t f0 = 0u, f1 = 0u;
    float alpha = 0.0f;

    if (anim->mode == WE_ANIM_SINGLE) {
        double max_f = (double)(tr->kf_count - 1u);
        double clamped = fmin(fmax(frame_pos, 0.0), max_f);
        double fl = floor(clamped);
        f0 = (uint32_t)fl;
        f1 = (f0 + 1u < tr->kf_count) ? (f0 + 1u) : f0;
        alpha = (float)(clamped - fl);
    } else if (anim->mode == WE_ANIM_MIRROR) {
        double span = (double)(anim->length > 1 ? (anim->length - 1) : 1);
        double period = 2.0 * span;
        double wrapped = fmod(frame_pos, period);
        if (wrapped < 0.0) {
            wrapped += period;
        }
        if (wrapped > span) {
            wrapped = period - wrapped;
        }
        double fl = floor(wrapped);
        f0 = (uint32_t)fl;
        if (f0 >= tr->kf_count) {
            f0 = tr->kf_count - 1u;
        }
        f1 = (f0 + 1u < tr->kf_count) ? (f0 + 1u) : f0;
        alpha = (float)(wrapped - fl);
    } else {
        double cycle = (double)(anim->length > 0 ? anim->length : (int32_t)tr->kf_count);
        double wrapped = fmod(frame_pos, cycle);
        if (wrapped < 0.0) {
            wrapped += cycle;
        }
        double fl = floor(wrapped);
        f0 = ((uint32_t)fl) % tr->kf_count;
        f1 = (f0 + 1u) % tr->kf_count;
        alpha = (float)(wrapped - fl);
    }

    const we_mdl_keyframe_t *k0 = &tr->keyframes[f0];
    const we_mdl_keyframe_t *k1 = &tr->keyframes[f1];
    for (int c = 0; c < 3; c++) {
        out_pos[c] = k0->pos[c] + alpha * (k1->pos[c] - k0->pos[c]);
        float da = remainderf(k1->angle[c] - k0->angle[c], WE_RIG_TWO_PI_F);
        out_angle[c] = k0->angle[c] + alpha * da;
        out_scale[c] = k0->scale[c] + alpha * (k1->scale[c] - k0->scale[c]);
    }
}

static const we_mdl_bone_track_t *find_bone_track(const we_mdl_anim_t *anim, uint32_t bone_idx)
{
    if (!anim || !anim->tracks || anim->track_count == 0u) {
        return NULL;
    }
    if (bone_idx < WE_MDL_MAX_BONES && anim->bone_lut_valid) {
        int16_t tidx = anim->bone_to_track[bone_idx];
        if (tidx >= 0 && (uint32_t)tidx < anim->track_count &&
            anim->tracks[tidx].bone_index == (int32_t)bone_idx &&
            anim->tracks[tidx].kf_count > 0u) {
            return &anim->tracks[tidx];
        }
    }
    for (uint32_t t = 0; t < anim->track_count; t++) {
        if (anim->tracks[t].bone_index == (int32_t)bone_idx && anim->tracks[t].kf_count > 0u) {
            return &anim->tracks[t];
        }
    }
    return NULL;
}

static void eval_masked_topo_physics_skin_mats(const we_puppet_t *puppet, const uint32_t *topo,
                                               const float (*anim_local)[16],
                                               const uint8_t ik_owned[WE_MDL_MAX_BONES],
                                               double time_sec, float (*out_skin_mats)[16])
{
    uint32_t bcnt = puppet->bone_count < WE_MDL_MAX_BONES ? puppet->bone_count : WE_MDL_MAX_BONES;
    we_puppet_physics_ctx_t *phys = puppet->physics_ctx;
    uint32_t substeps = we_physics_advance_wall_clock(phys, time_sec);
    float world_mats[WE_MDL_MAX_BONES][16];

    if (substeps > 0u) {
        for (uint32_t s = 0; s < substeps; s++) {
            double cur_time = phys->total_sim_time_sec;
            for (uint32_t t = 0; t < bcnt; t++) {
                uint32_t k = topo[t];
                uint32_t p = puppet->bones[k].parent;
                if (p == 0xFFFFFFFFu || p >= bcnt) {
                    memcpy(world_mats[k], anim_local[k], sizeof(world_mats[k]));
                } else {
                    we_mat4_mul(world_mats[p], anim_local[k], world_mats[k]);
                }
                if (!ik_owned[k] && (phys->configs[k].flags & WE_BONE_PHYS_ENABLED)) {
                    float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                    we_physics_substep_bone(phys, k, pivot, world_mats[k], WE_PHYSICS_SUBSTEP_DT,
                                            cur_time);
                }
            }
            phys->time_accumulator_sec =
                fmaxf(0.0f, phys->time_accumulator_sec - WE_PHYSICS_SUBSTEP_DT);
            phys->total_sim_time_sec += (double)WE_PHYSICS_SUBSTEP_DT;
        }
        float bass = 0.0f;
        for (int b = 0; b < 8; b++) {
            bass += phys->audio_bands[b];
        }
        phys->prev_audio_bass = bass * 0.125f;
    } else {
        for (uint32_t t = 0; t < bcnt; t++) {
            uint32_t k = topo[t];
            uint32_t p = puppet->bones[k].parent;
            if (p == 0xFFFFFFFFu || p >= bcnt) {
                memcpy(world_mats[k], anim_local[k], sizeof(world_mats[k]));
            } else {
                we_mat4_mul(world_mats[p], anim_local[k], world_mats[k]);
            }
            if (!ik_owned[k] && (phys->configs[k].flags & WE_BONE_PHYS_ENABLED)) {
                float pivot[3] = {world_mats[k][12], world_mats[k][13], world_mats[k][14]};
                we_physics_apply_bone_rotation(phys, k, pivot, world_mats[k]);
            }
        }
    }

    float bind_world[WE_MDL_MAX_BONES][16];
    compute_bind_world_matrices(puppet, topo, bind_world);
    for (uint32_t k = 0; k < bcnt; k++) {
        float off[16];
        if (mat4_is_finite(puppet->bones[k].offset_trans)) {
            memcpy(off, puppet->bones[k].offset_trans, sizeof(off));
        } else if (!we_mat4_inverse(bind_world[k], off)) {
            rig_mat4_identity(off);
        }
        we_mat4_mul(world_mats[k], off, out_skin_mats[k]);
    }
}

void we_puppet_eval_skin_matrices_ex(const we_puppet_t *puppet, const we_puppet_layer_t *layers,
                                     uint32_t layer_count, const we_rig_pose_ctx_t *rig,
                                     double time_sec, float (*out_skin_mats)[16])
{
    if (!puppet || !out_skin_mats || puppet->bone_count == 0u || !puppet->bones) {
        return;
    }

    uint32_t topo[WE_MDL_MAX_BONES];
    bool already_topo = true;
    if (!validate_and_topo_sort_skeleton(puppet, topo, &already_topo)) {
        return;
    }

    we_puppet_layer_t default_layer;
    const we_puppet_layer_t *active_layers = layers;
    uint32_t active_count = layer_count;
    if ((!active_layers || active_count == 0u) && puppet->layer_count > 0u) {
        active_layers = puppet->layers;
        active_count = puppet->layer_count;
    }
    if ((!active_layers || active_count == 0u) && puppet->anim_count > 0u && puppet->anims) {
        memset(&default_layer, 0, sizeof(default_layer));
        default_layer.anim_id = puppet->anims[0].id;
        default_layer.rate = 1.0f;
        default_layer.blend = 1.0f;
        default_layer.visible = true;
        active_layers = &default_layer;
        active_count = 1u;
    }
    if (active_count > WE_MDL_MAX_LAYERS) {
        active_count = WE_MDL_MAX_LAYERS;
    }

    const we_mdl_anim_t *layer_anims[WE_MDL_MAX_LAYERS];
    for (uint32_t l = 0; l < active_count; l++) {
        const we_puppet_layer_t *lay = &active_layers[l];
        if (!lay->visible || lay->blend <= 0.0f) {
            layer_anims[l] = NULL;
        } else {
            layer_anims[l] = rig_find_anim(puppet, lay);
        }
    }

    float anim_local[WE_MDL_MAX_BONES][16];
    uint32_t bcnt = puppet->bone_count;

    for (uint32_t k = 0; k < bcnt; k++) {
        const we_mdl_bone_t *bone = &puppet->bones[k];
        float bpos[3] = {bone->bind_pos[0], bone->bind_pos[1], bone->bind_pos[2]};
        float bang[3] = {bone->bind_angle[0], bone->bind_angle[1], bone->bind_angle[2]};
        float bscl[3] = {bone->bind_scale[0], bone->bind_scale[1], bone->bind_scale[2]};
        if (!mat4_is_finite(bone->local_trans)) {
            if (fabsf(bscl[0]) < 1e-6f) {
                bscl[0] = 1.0f;
            }
            if (fabsf(bscl[1]) < 1e-6f) {
                bscl[1] = 1.0f;
            }
            if (fabsf(bscl[2]) < 1e-6f) {
                bscl[2] = 1.0f;
            }
        } else if (fabsf(bscl[0]) < 1e-6f && fabsf(bscl[1]) < 1e-6f && fabsf(bscl[2]) < 1e-6f) {
            rig_mat4_decompose_trs(bone->local_trans, bpos, bang, bscl);
        }

        float pos[3] = {bpos[0], bpos[1], bpos[2]};
        float angle[3] = {bang[0], bang[1], bang[2]};
        float scale[3] = {bscl[0], bscl[1], bscl[2]};
        bool animated = false;

        /* Apply OVERRIDE layers first, then ADDITIVE delta layers (Delta = anim - bind) */
        for (int pass = 0; pass < 2; pass++) {
            we_anim_blend_mode_t target_mode =
                (pass == 0) ? WE_ANIM_BLEND_OVERRIDE : WE_ANIM_BLEND_ADDITIVE;
            for (uint32_t l = 0; l < active_count; l++) {
                we_anim_blend_mode_t mode =
                    rig ? rig->layer_blend_modes[l] : WE_ANIM_BLEND_OVERRIDE;
                if (mode != target_mode) {
                    continue;
                }
                const we_puppet_layer_t *lay = &active_layers[l];
                const we_mdl_anim_t *anim = layer_anims[l];
                if (!anim) {
                    continue;
                }
                const we_mdl_bone_track_t *tr = find_bone_track(anim, k);
                if (!tr) {
                    continue;
                }
                float t_pos[3], t_ang[3], t_scl[3];
                float rate = isfinite(lay->rate) ? lay->rate : 1.0f;
                rig_eval_track_at_time(anim, tr, time_sec * (double)rate, t_pos, t_ang, t_scl);
                float w = clampf(lay->blend, 0.0f, 1.0f);

                if (mode == WE_ANIM_BLEND_OVERRIDE) {
                    for (int c = 0; c < 3; c++) {
                        pos[c] = pos[c] + w * (t_pos[c] - pos[c]);
                        if (w >= 0.9999f) {
                            angle[c] = t_ang[c];
                        } else {
                            float da = remainderf(t_ang[c] - angle[c], WE_RIG_TWO_PI_F);
                            angle[c] = angle[c] + w * da;
                        }
                        scale[c] = scale[c] + w * (t_scl[c] - scale[c]);
                    }
                } else {
                    for (int c = 0; c < 3; c++) {
                        pos[c] += w * (t_pos[c] - bpos[c]);
                        float da = remainderf(t_ang[c] - bang[c], WE_RIG_TWO_PI_F);
                        angle[c] += w * da;
                        scale[c] += w * (t_scl[c] - bscl[c]);
                    }
                }
                animated = true;
            }
        }

        if (animated) {
            rig_mat4_from_trs(pos, angle, scale, anim_local[k]);
        } else if (mat4_is_finite(bone->local_trans)) {
            memcpy(anim_local[k], bone->local_trans, sizeof(anim_local[k]));
        } else {
            rig_mat4_from_trs(bpos, bang, bscl, anim_local[k]);
        }
    }

    if (rig) {
        we_rig_apply_constraints(puppet, rig, anim_local);
    }

    uint8_t ik_owned[WE_MDL_MAX_BONES] = {0};
    bool need_mask = false;
    bool phys_active = (puppet->physics_ctx && puppet->physics_ctx->initialized &&
                        puppet->physics_ctx->active_bone_count > 0u);
    if (rig && phys_active) {
        for (uint32_t k = 0; k < bcnt; k++) {
            if (rig->bone_drive_modes[k] == WE_BONE_DRIVE_IK ||
                rig->bone_drive_modes[k] == WE_BONE_DRIVE_AIM) {
                ik_owned[k] = 1u;
            }
        }
        uint32_t ik_cnt =
            rig->ik_chain_count < WE_RIG_MAX_IK_CHAINS ? rig->ik_chain_count : WE_RIG_MAX_IK_CHAINS;
        for (uint32_t c = 0; c < ik_cnt; c++) {
            const we_ik_chain_t *ch = &rig->ik_chains[c];
            if (!ch->enabled) {
                continue;
            }
            if (ch->root_bone < bcnt &&
                rig->bone_drive_modes[ch->root_bone] != WE_BONE_DRIVE_XPBD) {
                ik_owned[ch->root_bone] = 1u;
            }
            if (ch->mid_bone < bcnt && rig->bone_drive_modes[ch->mid_bone] != WE_BONE_DRIVE_XPBD) {
                ik_owned[ch->mid_bone] = 1u;
            }
            if (ch->end_bone < bcnt && rig->bone_drive_modes[ch->end_bone] != WE_BONE_DRIVE_XPBD) {
                ik_owned[ch->end_bone] = 1u;
            }
        }
        uint32_t aim_cnt = rig->aim_count < WE_RIG_MAX_AIM_CONSTRAINTS ? rig->aim_count
                                                                       : WE_RIG_MAX_AIM_CONSTRAINTS;
        for (uint32_t a = 0; a < aim_cnt; a++) {
            const we_aim_constraint_t *aim = &rig->aim_constraints[a];
            if (aim->enabled && aim->bone_index < bcnt &&
                rig->bone_drive_modes[aim->bone_index] != WE_BONE_DRIVE_XPBD) {
                ik_owned[aim->bone_index] = 1u;
            }
        }
        for (uint32_t k = 0; k < bcnt; k++) {
            if (ik_owned[k] && (puppet->physics_ctx->configs[k].flags & WE_BONE_PHYS_ENABLED)) {
                need_mask = true;
                break;
            }
        }
    }

    if (phys_active && already_topo && !need_mask) {
        uint32_t substeps = we_physics_advance_wall_clock(puppet->physics_ctx, time_sec);
        we_physics_solve_hierarchy_skin_matrices(
            puppet->physics_ctx, puppet, (const float (*)[16])anim_local, substeps, out_skin_mats);
    } else if (phys_active) {
        eval_masked_topo_physics_skin_mats(puppet, topo, (const float (*)[16])anim_local, ik_owned,
                                           time_sec, out_skin_mats);
    } else {
        float world_mats[WE_MDL_MAX_BONES][16];
        eval_world_mats_topo(puppet, topo, anim_local, world_mats);
        float bind_world[WE_MDL_MAX_BONES][16];
        compute_bind_world_matrices(puppet, topo, bind_world);
        for (uint32_t k = 0; k < bcnt; k++) {
            float off[16];
            if (mat4_is_finite(puppet->bones[k].offset_trans)) {
                memcpy(off, puppet->bones[k].offset_trans, sizeof(off));
            } else if (!we_mat4_inverse(bind_world[k], off)) {
                rig_mat4_identity(off);
            }
            we_mat4_mul(world_mats[k], off, out_skin_mats[k]);
        }
    }
}
