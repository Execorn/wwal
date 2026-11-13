// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_mesh_rig.h"
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
#define WE_TEST_ASAN_OR_LSAN 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define WE_TEST_ASAN_OR_LSAN 1
#endif

#ifdef WE_TEST_ASAN_OR_LSAN
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
    return "leak:libEGL\n"
           "leak:libGL\n"
           "leak:libgbm\n"
           "leak:libdrm\n"
           "leak:libvulkan\n"
           "leak:libnvidia\n"
           "leak:libdbus\n"
           "leak:<unknown module>\n"
           "leak:dri\n"
           "leak:swrast\n"
           "leak:radeonsi\n"
           "leak:iris\n"
           "leak:amdgpu\n"
           "leak:libLLVM\n";
}
#endif

static void mat4_ident(float m[16])
{
    memset(m, 0, 16u * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void mat4_translate(float m[16], float x, float y, float z)
{
    mat4_ident(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void mat4_from_z_rot_trans(float m[16], float tx, float ty, float tz, float angle_z)
{
    float c = cosf(angle_z);
    float s = sinf(angle_z);
    mat4_ident(m);
    m[0] = c;
    m[1] = s;
    m[4] = -s;
    m[5] = c;
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void init_bone(we_mdl_bone_t *b, const char *name, uint32_t parent, float px, float py,
                      float pz)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->parent = parent;
    b->bind_pos[0] = px;
    b->bind_pos[1] = py;
    b->bind_pos[2] = pz;
    b->bind_scale[0] = 1.0f;
    b->bind_scale[1] = 1.0f;
    b->bind_scale[2] = 1.0f;
    mat4_translate(b->local_trans, px, py, pz);
    mat4_ident(b->offset_trans);
}

static void finalize_bone_bind_offsets(we_puppet_t *p)
{
    float world[WE_MDL_MAX_BONES][16];
    for (uint32_t i = 0; i < p->bone_count; i++) {
        if (p->bones[i].parent == 0xFFFFFFFFu) {
            memcpy(world[i], p->bones[i].local_trans, sizeof(world[i]));
        } else {
            we_mat4_mul(world[p->bones[i].parent], p->bones[i].local_trans, world[i]);
        }
        if (!we_mat4_inverse(world[i], p->bones[i].offset_trans)) {
            mat4_ident(p->bones[i].offset_trans);
        }
    }
}

static we_puppet_t *alloc_test_puppet(void)
{
    we_puppet_t *p = calloc(1u, sizeof(we_puppet_t));
    assert(p);
    return p;
}

static void build_grid_mesh(we_puppet_t *p, uint32_t nx, uint32_t ny, float x0, float y0, float x1,
                            float y1, float shear_x)
{
    uint32_t V = nx * ny;
    uint32_t T = (nx - 1u) * (ny - 1u) * 2u;
    p->vertices = calloc(V, sizeof(we_mdl_vertex_t));
    p->indices = calloc((size_t)T * 3u, sizeof(uint32_t));
    assert(p->vertices && p->indices);
    p->vertex_count = V;
    p->triangle_count = T;
    p->index_count = T * 3u;

    for (uint32_t iy = 0; iy < ny; iy++) {
        float ty = (ny > 1u) ? (float)iy / (float)(ny - 1u) : 0.0f;
        for (uint32_t ix = 0; ix < nx; ix++) {
            float tx = (nx > 1u) ? (float)ix / (float)(nx - 1u) : 0.0f;
            uint32_t idx = iy * nx + ix;
            p->vertices[idx].pos[0] = x0 + tx * (x1 - x0) + shear_x * (float)(iy & 1u);
            p->vertices[idx].pos[1] = y0 + ty * (y1 - y0);
            p->vertices[idx].pos[2] = 0.0f;
            p->vertices[idx].texcoord[0] = tx;
            p->vertices[idx].texcoord[1] = ty;
            p->vertices[idx].weight[0] = 1.0f;
        }
    }

    uint32_t ti = 0u;
    for (uint32_t iy = 0; iy < ny - 1u; iy++) {
        for (uint32_t ix = 0; ix < nx - 1u; ix++) {
            uint32_t i00 = iy * nx + ix;
            uint32_t i10 = i00 + 1u;
            uint32_t i01 = i00 + nx;
            uint32_t i11 = i01 + 1u;
            p->indices[ti * 3u + 0u] = i00;
            p->indices[ti * 3u + 1u] = i10;
            p->indices[ti * 3u + 2u] = i11;
            ti++;
            p->indices[ti * 3u + 0u] = i00;
            p->indices[ti * 3u + 1u] = i11;
            p->indices[ti * 3u + 2u] = i01;
            ti++;
        }
    }
    assert(ti == T);
}

static void assert_vertex_pou(const we_mdl_vertex_t *v, uint32_t bone_count)
{
    float sum = 0.0f;
    for (int k = 0; k < 4; k++) {
        assert(isfinite(v->weight[k]));
        assert(v->weight[k] >= -1e-6f && v->weight[k] <= 1.0f + 1e-6f);
        assert(v->blend_indices[k] < bone_count);
        sum += v->weight[k];
    }
    assert(fabsf(sum - 1.0f) < 1e-5f);
}

static float get_bone_weight(const we_mdl_vertex_t *v, uint32_t bone)
{
    float w = 0.0f;
    for (int k = 0; k < 4; k++) {
        if (v->blend_indices[k] == bone) {
            w += v->weight[k];
        }
    }
    return w;
}

static void test_skeleton_topology_and_cycles(void)
{
    we_puppet_t *p = alloc_test_puppet();
    build_grid_mesh(p, 4u, 4u, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f);

    p->bone_count = 3u;
    p->bones = calloc(3u, sizeof(we_mdl_bone_t));
    assert(p->bones);
    /* Out-of-order valid DAG: bone 1 is root, bone 0 has parent 1, bone 2 has parent 0 */
    init_bone(&p->bones[1], "root", 0xFFFFFFFFu, 0.0f, -0.5f, 0.0f);
    init_bone(&p->bones[0], "mid", 1u, 0.0f, 0.5f, 0.0f);
    init_bone(&p->bones[2], "tip", 0u, 0.0f, 0.5f, 0.0f);

    we_heat_skin_cfg_t cfg;
    we_heat_skin_cfg_default(&cfg);
    we_heat_solve_stats_t stats;
    assert(we_mesh_solve_heat_weights(p, &cfg, &stats));
    assert(stats.vertex_count == p->vertex_count);
    assert(stats.bone_count == 3u);
    for (uint32_t i = 0; i < p->vertex_count; i++) {
        assert_vertex_pou(&p->vertices[i], p->bone_count);
    }

    float out_skin[3][16];
    we_puppet_eval_skin_matrices_ex(p, NULL, 0u, NULL, 0.0, out_skin);
    for (uint32_t b = 0; b < 3u; b++) {
        for (int m = 0; m < 16; m++) {
            assert(isfinite(out_skin[b][m]));
        }
    }

    /* Inject 2-cycle: bone 0 -> bone 1 -> bone 0 */
    p->bones[1].parent = 0u;
    assert(!we_mesh_solve_heat_weights(p, &cfg, &stats));

    /* Inject out-of-range parent orphan */
    p->bones[1].parent = 99u;
    assert(!we_mesh_solve_heat_weights(p, &cfg, &stats));

    we_puppet_destroy(p);
    printf("[PASS] skeleton topology & cycle/orphan detection\n");
}

static void test_heat_skinning_visibility_islands_idt_and_perf(void)
{
    /* 1. U-shaped mesh with an open gap between left arm (x in [-3,-1]) and right arm (x in [1,3])
     * bridged only at y in [-3,-2], so upper-right vertices are separated by boundary gap from left
     * bone */
    {
        we_puppet_t *p = alloc_test_puppet();
        const uint32_t nx = 7u, ny = 6u;
        p->vertex_count = nx * ny;
        p->vertices = calloc(p->vertex_count, sizeof(we_mdl_vertex_t));
        uint32_t max_t = (nx - 1u) * (ny - 1u) * 2u;
        p->indices = calloc((size_t)max_t * 3u, sizeof(uint32_t));
        assert(p->vertices && p->indices);
        for (uint32_t iy = 0; iy < ny; iy++) {
            for (uint32_t ix = 0; ix < nx; ix++) {
                uint32_t id = iy * nx + ix;
                p->vertices[id].pos[0] = -3.0f + (float)ix;
                p->vertices[id].pos[1] = -3.0f + (float)iy;
                p->vertices[id].weight[0] = 1.0f;
            }
        }
        uint32_t t = 0u;
        for (uint32_t iy = 0; iy < ny - 1u; iy++) {
            for (uint32_t ix = 0; ix < nx - 1u; ix++) {
                if ((ix == 2u || ix == 3u) && iy >= 1u) {
                    continue;
                }
                uint32_t i00 = iy * nx + ix;
                uint32_t i10 = i00 + 1u;
                uint32_t i01 = i00 + nx;
                uint32_t i11 = i01 + 1u;
                p->indices[t * 3u + 0u] = i00;
                p->indices[t * 3u + 1u] = i10;
                p->indices[t * 3u + 2u] = i11;
                t++;
                p->indices[t * 3u + 0u] = i00;
                p->indices[t * 3u + 1u] = i11;
                p->indices[t * 3u + 2u] = i01;
                t++;
            }
        }
        p->triangle_count = t;
        p->index_count = t * 3u;

        p->bone_count = 3u;
        p->bones = calloc(3u, sizeof(we_mdl_bone_t));
        assert(p->bones);
        init_bone(&p->bones[0], "pelvis", 0xFFFFFFFFu, 0.0f, -2.5f, 0.0f);
        init_bone(&p->bones[1], "arm_L", 0u, -2.0f, 2.5f, 0.0f);
        init_bone(&p->bones[2], "arm_R", 0u, 2.0f, 2.5f, 0.0f);
        finalize_bone_bind_offsets(p);

        uint64_t locked_bs[2] = {0};
        locked_bs[0] = 1ull;
        p->vertices[0].blend_indices[0] = 2u;
        p->vertices[0].blend_indices[1] = 0u;
        p->vertices[0].weight[0] = 0.75f;
        p->vertices[0].weight[1] = 0.25f;

        we_heat_skin_cfg_t cfg;
        we_heat_skin_cfg_default(&cfg);
        cfg.locked_vert_bs = locked_bs;
        we_heat_solve_stats_t stats;
        assert(we_mesh_solve_heat_weights(p, &cfg, &stats));

        /* Locked vertex 0 must be completely unchanged */
        assert(p->vertices[0].blend_indices[0] == 2u);
        assert(p->vertices[0].blend_indices[1] == 0u);
        assert(fabsf(p->vertices[0].weight[0] - 0.75f) < 1e-6f);
        assert(fabsf(p->vertices[0].weight[1] - 0.25f) < 1e-6f);

        /* Top-right arm vertex at (1.0, 2.0) => ix=4, iy=5 */
        uint32_t right_inner_top = 5u * nx + 4u;
        assert_vertex_pou(&p->vertices[right_inner_top], p->bone_count);
        float w_right = get_bone_weight(&p->vertices[right_inner_top], 2u);
        float w_left = get_bone_weight(&p->vertices[right_inner_top], 1u);
        assert(w_right > 0.75f);
        assert(w_left < 0.15f);

        we_puppet_destroy(p);
    }

    /* 2. Disconnected floating island + obtuse mesh iDT edge flips */
    {
        we_puppet_t *p = alloc_test_puppet();
        p->vertex_count = 15u;
        p->triangle_count = 11u;
        p->index_count = 33u;
        p->vertices = calloc(p->vertex_count, sizeof(we_mdl_vertex_t));
        p->indices = calloc(p->index_count, sizeof(uint32_t));
        assert(p->vertices && p->indices);

        for (uint32_t iy = 0; iy < 2u; iy++) {
            for (uint32_t ix = 0; ix < 6u; ix++) {
                uint32_t id = iy * 6u + ix;
                p->vertices[id].pos[0] = (float)ix * 0.5f + (float)iy * 2.2f;
                p->vertices[id].pos[1] = (float)iy * 0.25f;
            }
        }
        uint32_t ti = 0u;
        for (uint32_t ix = 0; ix < 5u; ix++) {
            uint32_t i00 = ix;
            uint32_t i10 = ix + 1u;
            uint32_t i01 = 6u + ix;
            uint32_t i11 = 6u + ix + 1u;
            p->indices[ti * 3u + 0u] = i00;
            p->indices[ti * 3u + 1u] = i10;
            p->indices[ti * 3u + 2u] = i01;
            ti++;
            p->indices[ti * 3u + 0u] = i10;
            p->indices[ti * 3u + 1u] = i11;
            p->indices[ti * 3u + 2u] = i01;
            ti++;
        }
        p->vertices[12].pos[0] = 10.0f;
        p->vertices[12].pos[1] = 10.0f;
        p->vertices[13].pos[0] = 11.0f;
        p->vertices[13].pos[1] = 10.0f;
        p->vertices[14].pos[0] = 10.5f;
        p->vertices[14].pos[1] = 11.0f;
        p->indices[30] = 12u;
        p->indices[31] = 13u;
        p->indices[32] = 14u;

        p->bone_count = 2u;
        p->bones = calloc(2u, sizeof(we_mdl_bone_t));
        assert(p->bones);
        init_bone(&p->bones[0], "root", 0xFFFFFFFFu, 0.5f, 0.1f, 0.0f);
        init_bone(&p->bones[1], "tip", 0u, 2.5f, 0.1f, 0.0f);
        finalize_bone_bind_offsets(p);

        we_heat_skin_cfg_t cfg;
        we_heat_skin_cfg_default(&cfg);
        we_heat_solve_stats_t stats;
        assert(we_mesh_solve_heat_weights(p, &cfg, &stats));
        assert(stats.island_count >= 2u);
        assert(stats.idt_edge_flips > 0u);
        for (uint32_t i = 0; i < p->vertex_count; i++) {
            assert_vertex_pou(&p->vertices[i], p->bone_count);
        }
        we_puppet_destroy(p);
    }

    /* 3. 4,096-vertex solve benchmark gate (< 25.0 ms) */
    {
        we_puppet_t *p = alloc_test_puppet();
        build_grid_mesh(p, 64u, 64u, -2.0f, -2.0f, 2.0f, 2.0f, 0.0f);
        assert(p->vertex_count == 4096u);

        p->bone_count = 12u;
        p->bones = calloc(12u, sizeof(we_mdl_bone_t));
        assert(p->bones);
        init_bone(&p->bones[0], "root", 0xFFFFFFFFu, 0.0f, -1.8f, 0.0f);
        for (uint32_t b = 1u; b < 12u; b++) {
            char name[32];
            snprintf(name, sizeof(name), "bone_%u", b);
            uint32_t parent = (b <= 3u) ? (b - 1u) : (b % 4u);
            float bx = (b < 4u) ? 0.0f : ((b % 2u == 0u) ? -0.9f : 0.9f);
            float by = 0.35f;
            init_bone(&p->bones[b], name, parent, bx, by, 0.0f);
        }
        finalize_bone_bind_offsets(p);

        we_heat_skin_cfg_t cfg;
        we_heat_skin_cfg_default(&cfg);
        we_heat_solve_stats_t stats;
        assert(we_mesh_solve_heat_weights(p, &cfg, &stats));
        printf("[BENCH] 4096-vert 12-bone heat solve: %.3f ms (fill_nnz=%u, ic0=%d)\n",
               stats.solve_time_ms, stats.factor_nnz, (int)stats.used_ic0_fallback);
        for (uint32_t i = 0; i < p->vertex_count; i++) {
            assert_vertex_pou(&p->vertices[i], p->bone_count);
        }
        if (getenv("WWAL_STRICT_PERF_ASSERTS")) {
            assert(stats.solve_time_ms < 25.0);
        }
        we_puppet_destroy(p);
    }

    printf("[PASS] heat skinning visibility, disconnected islands, iDT flips, 4k benchmark\n");
}

static we_puppet_t *create_brush_test_puppet(void)
{
    we_puppet_t *p = alloc_test_puppet();
    build_grid_mesh(p, 32u, 20u, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f); /* 640 verts = 3 CoW blocks */
    p->bone_count = 3u;
    p->bones = calloc(3u, sizeof(we_mdl_bone_t));
    assert(p->bones);
    init_bone(&p->bones[0], "b0", 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f);
    init_bone(&p->bones[1], "b1", 0u, 0.5f, 0.0f, 0.0f);
    init_bone(&p->bones[2], "b2", 0u, -0.5f, 0.0f, 0.0f);
    finalize_bone_bind_offsets(p);

    for (uint32_t i = 0; i < p->vertex_count; i++) {
        p->vertices[i].blend_indices[0] = 0u;
        p->vertices[i].blend_indices[1] = 1u;
        p->vertices[i].blend_indices[2] = 2u;
        p->vertices[i].blend_indices[3] = 0u;
        p->vertices[i].weight[0] = 0.6f;
        p->vertices[i].weight[1] = 0.3f;
        p->vertices[i].weight[2] = 0.1f;
        p->vertices[i].weight[3] = 0.0f;
    }
    return p;
}

static void test_weight_renorm_and_cpu_brush(void)
{
    /* 1. Locked-bone renormalization */
    we_mdl_vertex_t v;
    memset(&v, 0, sizeof(v));
    v.blend_indices[0] = 0u;
    v.blend_indices[1] = 1u;
    v.blend_indices[2] = 2u;
    v.weight[0] = 0.5f;
    v.weight[1] = 0.3f;
    v.weight[2] = 0.2f;
    uint64_t locked_bones[2] = {(1ull << 1u), 0ull};
    we_mesh_renormalize_vertex_weights(&v, 3u, 2u, 0.5f, locked_bones);
    assert_vertex_pou(&v, 3u);
    assert(fabsf(get_bone_weight(&v, 1u) - 0.3f) < 1e-5f);
    assert(fabsf(get_bone_weight(&v, 2u) - 0.5f) < 1e-5f);
    assert(fabsf(get_bone_weight(&v, 0u) - 0.2f) < 1e-5f);

    /* 2. CPU weight brush ops + locked vertex + CoW 256-vertex dirty block tracking */
    we_puppet_t *p = create_brush_test_puppet();
    uint64_t locked_verts[10] = {0};
    locked_verts[0] = 1ull;

    uint32_t num_blocks = (p->vertex_count + WE_RIG_COW_BLOCK_VERTS - 1u) / WE_RIG_COW_BLOCK_VERTS;
    we_mdl_vertex_t *undo_snap = calloc(p->vertex_count, sizeof(we_mdl_vertex_t));
    assert(undo_snap);
    memcpy(undo_snap, p->vertices, p->vertex_count * sizeof(we_mdl_vertex_t));

    const float axy[2] = {-0.95f, -0.95f};
    const float bxy[2] = {-0.85f, -0.95f};
    uint64_t dirty_mask[8] = {0};
    uint32_t mod =
        we_mesh_apply_weight_paint_dab(p, axy, bxy, 0.20f, 0.20f, 0.40f, WE_WEIGHT_OP_ADD, 2u, 0.0f,
                                       locked_bones, locked_verts, dirty_mask);
    assert(mod > 0u);
    assert((dirty_mask[0] & 1ull) != 0ull);
    assert((dirty_mask[0] & (1ull << 2u)) == 0ull);

    /* Locked vertex 0 unchanged */
    assert(fabsf(get_bone_weight(&p->vertices[0], 2u) - 0.1f) < 1e-6f);
    assert(get_bone_weight(&p->vertices[1], 2u) > 0.1f);
    assert(fabsf(get_bone_weight(&p->vertices[1], 1u) - 0.3f) < 1e-5f);
    assert_vertex_pou(&p->vertices[1], p->bone_count);

    /* SUBTRACT */
    uint64_t dirty_sub[8] = {0};
    float before_sub = get_bone_weight(&p->vertices[1], 2u);
    mod = we_mesh_apply_weight_paint_dab(p, axy, bxy, 0.20f, 0.20f, 0.20f, WE_WEIGHT_OP_SUBTRACT,
                                         2u, 0.0f, locked_bones, locked_verts, dirty_sub);
    assert(mod > 0u);
    assert(get_bone_weight(&p->vertices[1], 2u) < before_sub);
    assert_vertex_pou(&p->vertices[1], p->bone_count);

    /* REPLACE */
    mod = we_mesh_apply_weight_paint_dab(p, axy, bxy, 0.20f, 0.20f, 1.0f, WE_WEIGHT_OP_REPLACE, 2u,
                                         0.55f, locked_bones, locked_verts, dirty_sub);
    assert(mod > 0u);
    assert(fabsf(get_bone_weight(&p->vertices[1], 2u) - 0.55f) < 0.1f);
    assert_vertex_pou(&p->vertices[1], p->bone_count);

    /* SMOOTH + NORMALIZE */
    mod = we_mesh_apply_weight_paint_dab(p, axy, bxy, 0.25f, 0.25f, 0.5f, WE_WEIGHT_OP_SMOOTH, 2u,
                                         0.0f, locked_bones, locked_verts, dirty_sub);
    assert(mod > 0u);
    mod = we_mesh_apply_weight_paint_dab(p, axy, bxy, 0.25f, 0.25f, 1.0f, WE_WEIGHT_OP_NORMALIZE,
                                         2u, 0.0f, locked_bones, locked_verts, dirty_sub);
    assert(mod > 0u);
    for (uint32_t i = 0; i < p->vertex_count; i++) {
        assert_vertex_pou(&p->vertices[i], p->bone_count);
    }

    /* CoW undo restore per dirty block */
    for (uint32_t blk = 0; blk < num_blocks; blk++) {
        if ((dirty_mask[blk >> 6u] | dirty_sub[blk >> 6u]) & (1ull << (blk & 63u))) {
            uint32_t v0 = blk * WE_RIG_COW_BLOCK_VERTS;
            uint32_t v1 = v0 + WE_RIG_COW_BLOCK_VERTS;
            if (v1 > p->vertex_count) {
                v1 = p->vertex_count;
            }
            memcpy(&p->vertices[v0], &undo_snap[v0], (size_t)(v1 - v0) * sizeof(we_mdl_vertex_t));
        }
    }
    assert(memcmp(p->vertices, undo_snap, p->vertex_count * sizeof(we_mdl_vertex_t)) == 0);

    free(undo_snap);
    we_puppet_destroy(p);
    printf("[PASS] locked-bone renormalization & CPU weight brush CoW parity\n");
}

static we_mdl_anim_t make_single_bone_anim(int32_t id, const char *name, we_mdl_anim_mode_t mode,
                                           float fps, const float *x_vals, const float *ang_vals,
                                           uint32_t kcount);

static void test_gpu_gles31_weight_brush(void)
{
    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        printf("[SKIP] /dev/dri/renderD128 unavailable, skipping GLES 3.1 compute brush test\n");
        return;
    }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        close(drm_fd);
        printf("[SKIP] gbm_create_device failed, skipping GLES 3.1 compute brush test\n");
        return;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_dpy = NULL;
    get_platform_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLint maj = 0, min = 0;
    if (get_platform_dpy) {
        dpy = get_platform_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    }
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        if (dpy != EGL_NO_DISPLAY) {
            eglTerminate(dpy);
            dpy = EGL_NO_DISPLAY;
        }
        PFNEGLQUERYDEVICESEXTPROC query_devs =
            (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        if (get_platform_dpy && query_devs) {
            EGLDeviceEXT devs[8];
            EGLint ndev = 0;
            if (query_devs(8, devs, &ndev) && ndev > 0) {
                dpy = get_platform_dpy(EGL_PLATFORM_DEVICE_EXT, devs[0], NULL);
                if (dpy != EGL_NO_DISPLAY && !eglInitialize(dpy, &maj, &min)) {
                    eglTerminate(dpy);
                    dpy = EGL_NO_DISPLAY;
                }
            }
        }
    }
    if (dpy == EGL_NO_DISPLAY) {
        gbm_device_destroy(gbm);
        close(drm_fd);
        printf("[SKIP] eglInitialize failed, skipping GLES 3.1 compute brush test\n");
        return;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLConfig egl_cfg = NULL;
    EGLint num_cfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &egl_cfg, 1, &num_cfg) || num_cfg < 1) {
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(drm_fd);
        printf("[SKIP] no ES3 EGLConfig\n");
        return;
    }

    const EGLint pbuf_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, egl_cfg, pbuf_attrs);
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1,
                                EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        if (ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(dpy, ctx);
        }
        if (surf != EGL_NO_SURFACE) {
            eglDestroySurface(dpy, surf);
        }
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(drm_fd);
        printf("[SKIP] ES 3.1 context unavailable\n");
        return;
    }

    while (glGetError() != GL_NO_ERROR) {
    }

    we_puppet_t *p_gpu = create_brush_test_puppet();
    we_puppet_t *p_cpu = create_brush_test_puppet();
    assert(we_puppet_upload_gl(p_gpu));
    assert(p_gpu->gl_uploaded);

    uint64_t locked_bones[2] = {(1ull << 1u), 0ull};
    uint64_t locked_verts[10] = {1ull};
    const float axy[2] = {-0.95f, -0.95f};
    const float bxy[2] = {-0.70f, -0.95f};
    uint64_t dirty_gpu[8] = {0};
    uint64_t dirty_cpu[8] = {0};

    uint32_t mod_gpu =
        we_mesh_apply_weight_paint_dab(p_gpu, axy, bxy, 0.25f, 0.25f, 0.35f, WE_WEIGHT_OP_ADD, 2u,
                                       0.0f, locked_bones, locked_verts, dirty_gpu);
    uint32_t mod_cpu =
        we_mesh_apply_weight_paint_dab(p_cpu, axy, bxy, 0.25f, 0.25f, 0.35f, WE_WEIGHT_OP_ADD, 2u,
                                       0.0f, locked_bones, locked_verts, dirty_cpu);
    assert(glGetError() == GL_NO_ERROR);
    assert(mod_gpu > 0u);
    assert(mod_gpu == mod_cpu);
    assert(dirty_gpu[0] == dirty_cpu[0]);

    for (uint32_t i = 0; i < p_gpu->vertex_count; i++) {
        assert_vertex_pou(&p_gpu->vertices[i], p_gpu->bone_count);
        for (uint32_t b = 0; b < p_gpu->bone_count; b++) {
            float wg = get_bone_weight(&p_gpu->vertices[i], b);
            float wc = get_bone_weight(&p_cpu->vertices[i], b);
            assert(fabsf(wg - wc) < 2e-3f);
        }
    }

    uint64_t dirty_smooth[8] = {0};
    uint32_t mod_smooth =
        we_mesh_apply_weight_paint_dab(p_gpu, axy, bxy, 0.25f, 0.25f, 0.5f, WE_WEIGHT_OP_SMOOTH, 2u,
                                       0.0f, locked_bones, locked_verts, dirty_smooth);
    assert(glGetError() == GL_NO_ERROR);
    assert(mod_smooth > 0u);
    for (uint32_t i = 0; i < p_gpu->vertex_count; i++) {
        assert_vertex_pou(&p_gpu->vertices[i], p_gpu->bone_count);
    }

    /* Verify locked-bone WE_WEIGHT_OP_NORMALIZE GPU/CPU parity and GL binding preservation */
    we_puppet_t *p_norm_gpu = create_brush_test_puppet();
    we_puppet_t *p_norm_cpu = create_brush_test_puppet();
    for (uint32_t i = 0; i < p_norm_gpu->vertex_count; i++) {
        p_norm_gpu->vertices[i].blend_indices[0] = 0;
        p_norm_gpu->vertices[i].blend_indices[1] = 1;
        p_norm_gpu->vertices[i].blend_indices[2] = 2;
        p_norm_gpu->vertices[i].blend_indices[3] = 3;
        p_norm_gpu->vertices[i].weight[0] = 0.20f;
        p_norm_gpu->vertices[i].weight[1] = 0.40f;
        p_norm_gpu->vertices[i].weight[2] = 0.10f;
        p_norm_gpu->vertices[i].weight[3] = 0.00f;
        p_norm_cpu->vertices[i] = p_norm_gpu->vertices[i];
    }
    assert(we_puppet_upload_gl(p_norm_gpu));

    GLuint sent_vao = 0, sent_vbo = 0, sent_ubo = 0, sent_ssbo[4] = {0, 0, 0, 0};
    glGenVertexArrays(1, &sent_vao);
    glGenBuffers(1, &sent_vbo);
    glGenBuffers(1, &sent_ubo);
    glGenBuffers(4, sent_ssbo);
    glBindVertexArray(sent_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sent_vbo);
    glBindBuffer(GL_UNIFORM_BUFFER, sent_ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, sent_ubo);
    for (GLuint b = 0; b < 4; b++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sent_ssbo[b]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, sent_ssbo[b]);
    }

    const float cxy[2] = {0.0f, 0.0f};
    uint64_t no_locked_verts[10] = {0};
    uint64_t dirty_norm_g[8] = {0}, dirty_norm_c[8] = {0};
    uint32_t mod_norm_g = we_mesh_apply_weight_paint_dab(
        p_norm_gpu, cxy, cxy, 2.5f, 1.0f, 1.0f, WE_WEIGHT_OP_NORMALIZE, 1u, 0.0f, locked_bones,
        no_locked_verts, dirty_norm_g);
    uint32_t mod_norm_c = we_mesh_apply_weight_paint_dab(
        p_norm_cpu, cxy, cxy, 2.5f, 1.0f, 1.0f, WE_WEIGHT_OP_NORMALIZE, 1u, 0.0f, locked_bones,
        no_locked_verts, dirty_norm_c);
    assert(glGetError() == GL_NO_ERROR);
    assert(mod_norm_g == mod_norm_c && mod_norm_g > 0u);
    for (uint32_t i = 0; i < p_norm_gpu->vertex_count; i++) {
        assert_vertex_pou(&p_norm_gpu->vertices[i], p_norm_gpu->bone_count);
        assert(fabsf(get_bone_weight(&p_norm_gpu->vertices[i], 1u) - 0.40f) < 2e-3f);
        for (uint32_t b = 0; b < p_norm_gpu->bone_count; b++) {
            assert(fabsf(get_bone_weight(&p_norm_gpu->vertices[i], b) -
                         get_bone_weight(&p_norm_cpu->vertices[i], b)) < 2e-3f);
        }
    }

    GLint got_vao = 0, got_vbo = 0, got_ubo = 0, got_ubo0 = 0, got_ssbo = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &got_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &got_vbo);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &got_ubo);
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &got_ubo0);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &got_ssbo);
    assert((GLuint)got_vao == sent_vao);
    assert((GLuint)got_vbo == sent_vbo);
    assert((GLuint)got_ubo == sent_ubo);
    assert((GLuint)got_ubo0 == sent_ubo);
    assert((GLuint)got_ssbo == sent_ssbo[3]);
    for (GLuint b = 0; b < 4; b++) {
        GLint got_b = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, b, &got_b);
        assert((GLuint)got_b == sent_ssbo[b]);
    }

    /* Verify heatmap overlay and paused rig_ctx draw preserve GL bindings */
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    static const char *k_vs =
        "#version 310 es\n"
        "layout(location=0) in vec3 a_pos;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "void main() { gl_Position = g_ModelViewProjectionMatrix * vec4(a_pos, 1.0); }\n";
    static const char *k_fs = "#version 310 es\n"
                              "precision mediump float;\n"
                              "out vec4 o_col;\n"
                              "void main() { o_col = vec4(0.2, 0.4, 0.6, 1.0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &k_vs, NULL);
    glShaderSource(fs, 1, &k_fs, NULL);
    glCompileShader(vs);
    glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    const float id4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUseProgram(prog);
    GLint loc_mvp = glGetUniformLocation(prog, "g_ModelViewProjectionMatrix");
    if (loc_mvp >= 0) {
        glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, id4);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);

    we_mesh_rig_draw_heatmap_gl(p_norm_gpu, NULL, 1u, 0u, 1u, 0.75f);
    assert(glGetError() == GL_NO_ERROR);

    GLint got_prog = 0, got_src_rgb = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &got_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &got_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &got_vbo);
    glGetIntegerv(GL_BLEND_SRC_RGB, &got_src_rgb);
    assert((GLuint)got_prog == prog);
    assert((GLuint)got_vao == sent_vao);
    assert((GLuint)got_vbo == sent_vbo);
    assert(got_src_rgb == GL_ONE);

    we_rig_pose_ctx_t *draw_rig = calloc(1u, sizeof(we_rig_pose_ctx_t));
    assert(draw_rig != NULL);
    if (!p_norm_gpu->skin_mats) {
        p_norm_gpu->skin_mats = calloc(p_norm_gpu->bone_count, sizeof(float[16]));
        assert(p_norm_gpu->skin_mats != NULL);
    }
    const float x_draw[2] = {0.0f, 1.0f};
    const float a_draw[2] = {0.0f, 0.0f};
    p_norm_gpu->anim_count = 1u;
    p_norm_gpu->anims = calloc(1u, sizeof(we_mdl_anim_t));
    assert(p_norm_gpu->anims != NULL);
    p_norm_gpu->anims[0] =
        make_single_bone_anim(7, "pause_chk", WE_ANIM_SINGLE, 1.0f, x_draw, a_draw, 2u);
    p_norm_gpu->layer_count = 1u;
    p_norm_gpu->layers[0] =
        (we_puppet_layer_t){.anim_id = 7, .rate = 1.0f, .blend = 1.0f, .visible = true};
    p_norm_gpu->rig_ctx = draw_rig;
    p_norm_gpu->playback_paused = true;
    p_norm_gpu->preview_time_sec = 0.25;
    p_norm_gpu->heatmap_mode = 2u;
    p_norm_gpu->heatmap_palette = 1u;
    p_norm_gpu->heatmap_active_bone = 0u;
    p_norm_gpu->heatmap_opacity = 0.6f;
    we_puppet_draw_gl(p_norm_gpu, prog, 99.0);
    assert(glGetError() == GL_NO_ERROR);
    assert(fabsf(p_norm_gpu->skin_mats[0][12] - 0.25f) < 1e-4f);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glDeleteProgram(prog);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteBuffers(1, &sent_vbo);
    glDeleteBuffers(1, &sent_ubo);
    glDeleteBuffers(4, sent_ssbo);
    glDeleteVertexArrays(1, &sent_vao);
    we_puppet_destroy(p_norm_gpu);
    we_puppet_destroy(p_norm_cpu);
    we_puppet_destroy(p_gpu);
    we_puppet_destroy(p_cpu);
    we_gpu_weight_brush_destroy();

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(drm_fd);
    printf("[PASS] GLES 3.1 compute weight brush GPU/CPU parity + 0 GL errors\n");
}

static we_puppet_t *create_ik_arm_puppet(float l1, float l2, bool with_xpbd_tip)
{
    we_puppet_t *p = alloc_test_puppet();
    build_grid_mesh(p, 4u, 2u, 0.0f, -0.1f, l1 + l2, 0.1f, 0.0f);

    p->bone_count = with_xpbd_tip ? 4u : 3u;
    p->bones = calloc(p->bone_count, sizeof(we_mdl_bone_t));
    assert(p->bones);
    init_bone(&p->bones[0], "upper_arm", 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f);
    init_bone(&p->bones[1], "forearm", 0u, l1, 0.0f, 0.0f);
    init_bone(&p->bones[2], "hand", 1u, l2, 0.0f, 0.0f);
    if (with_xpbd_tip) {
        init_bone(&p->bones[3], "ribbon", 2u, 0.5f, 0.0f, 0.0f);
    }
    finalize_bone_bind_offsets(p);
    return p;
}

static void eval_pose_world(const we_puppet_t *p, float (*local)[16], float (*world)[16])
{
    for (uint32_t i = 0; i < p->bone_count; i++) {
        if (p->bones[i].parent == 0xFFFFFFFFu) {
            memcpy(world[i], local[i], 16u * sizeof(float));
        } else {
            we_mat4_mul(world[p->bones[i].parent], local[i], world[i]);
        }
    }
}

static void test_ik_aim_limits_and_xpbd_exclusion(void)
{
    we_puppet_t *p = create_ik_arm_puppet(1.0f, 1.0f, false);
    float anim_local[3][16];
    float world[3][16];

    /* 1. Reachable target + pole direction */
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    we_ik_chain_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.root_bone = 0u;
    ch.mid_bone = 1u;
    ch.end_bone = 2u;
    ch.enabled = true;
    ch.blend_weight = 1.0f;
    ch.soft_zone_ratio = 0.10f;
    ch.max_stretch_ratio = 1.0f;
    ch.target_world[0] = 1.1f;
    ch.target_world[1] = 0.8f;
    ch.pole_world[0] = 0.0f;
    ch.pole_world[1] = 2.0f;
    assert(we_ik_solve_two_bone(p, &ch, anim_local));
    eval_pose_world(p, anim_local, world);
    float dx = world[2][12] - ch.target_world[0];
    float dy = world[2][13] - ch.target_world[1];
    assert(sqrtf(dx * dx + dy * dy) < 1e-3f);
    assert(world[1][13] > 0.4f);

    /* 2. C1 soft-stretch continuity across d_soft boundary (non-stretch and stretch 1.25x) */
    for (int stretch_pass = 0; stretch_pass < 2; stretch_pass++) {
        ch.max_stretch_ratio = (stretch_pass == 0) ? 1.0f : 1.25f;
        ch.soft_zone_ratio = 0.10f;
        float d_soft = 2.0f * (1.0f - 0.10f);
        float prev_x = 0.0f;
        for (int s = 0; s <= 12; s++) {
            float d = (d_soft - 0.12f) + (float)s * 0.04f;
            for (int i = 0; i < 3; i++) {
                memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
            }
            ch.target_world[0] = d;
            ch.target_world[1] = 0.0f;
            assert(we_ik_solve_two_bone(p, &ch, anim_local));
            eval_pose_world(p, anim_local, world);
            if (s > 0) {
                float step = world[2][12] - prev_x;
                assert(step > 0.0f && step < 0.06f);
            }
            prev_x = world[2][12];
        }
        if (stretch_pass == 1) {
            assert(prev_x > 2.0f && prev_x <= 2.5f + 1e-4f);
        } else {
            assert(prev_x <= 2.0f + 1e-4f);
        }
    }

    /* 3. Near-|L1-L2| zero-distance singularity check (non-NaN & bounded) */
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    ch.target_world[0] = 0.001f;
    ch.target_world[1] = 0.001f;
    assert(we_ik_solve_two_bone(p, &ch, anim_local));
    eval_pose_world(p, anim_local, world);
    for (int i = 0; i < 16; i++) {
        assert(isfinite(world[2][i]));
    }

    /* 4. Joint angle limits clamping */
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    ch.max_stretch_ratio = 1.0f;
    ch.target_world[0] = -1.0f;
    ch.target_world[1] = 1.2f;
    ch.root_angle_min_rad = -0.35f;
    ch.root_angle_max_rad = 0.35f;
    assert(we_ik_solve_two_bone(p, &ch, anim_local));
    float root_ang = atan2f(anim_local[0][1], anim_local[0][0]);
    assert(root_ang >= -0.351f && root_ang <= 0.351f);

    /* 5. Single-bone Look-At / Aim constraint with cone limit */
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    we_aim_constraint_t aim;
    memset(&aim, 0, sizeof(aim));
    aim.bone_index = 0u;
    aim.enabled = true;
    aim.blend_weight = 1.0f;
    aim.aim_axis_local[0] = 1.0f;
    aim.target_world[0] = 0.0f;
    aim.target_world[1] = 2.0f;
    aim.max_cone_rad = 0.5f;
    assert(we_ik_solve_aim(p, &aim, anim_local));
    float aimed_ang = atan2f(anim_local[0][1], anim_local[0][0]);
    assert(fabsf(aimed_ang - 0.5f) < 1e-3f);

    we_puppet_destroy(p);

    /* 6. BoneDriveMode exclusion & XPBD trailing child on solved IK tip */
    we_puppet_t *px = create_ik_arm_puppet(1.0f, 1.0f, true);
    we_puppet_physics_ctx_t phys_ctx;
    we_physics_init_from_puppet(&phys_ctx, px);
    assert(phys_ctx.initialized);
    we_physics_apply_preset(&phys_ctx.configs[0], WE_PHYS_PRESET_SILK);
    phys_ctx.configs[0].flags |= WE_BONE_PHYS_ENABLED;
    we_physics_apply_preset(&phys_ctx.configs[3], WE_PHYS_PRESET_SILK);
    phys_ctx.configs[3].flags |= WE_BONE_PHYS_ENABLED;
    phys_ctx.active_bone_count = px->bone_count;
    px->physics_ctx = &phys_ctx;

    we_rig_pose_ctx_t rig;
    memset(&rig, 0, sizeof(rig));
    rig.ik_chain_count = 1u;
    rig.ik_chains[0].root_bone = 0u;
    rig.ik_chains[0].mid_bone = 1u;
    rig.ik_chains[0].end_bone = 2u;
    rig.ik_chains[0].enabled = true;
    rig.ik_chains[0].blend_weight = 1.0f;
    rig.ik_chains[0].target_world[0] = 1.0f;
    rig.ik_chains[0].target_world[1] = 1.0f;
    rig.ik_chains[0].pole_world[1] = 2.0f;
    rig.bone_drive_modes[0] = WE_BONE_DRIVE_IK;
    rig.bone_drive_modes[1] = WE_BONE_DRIVE_IK;
    rig.bone_drive_modes[3] = WE_BONE_DRIVE_XPBD;

    float test_local[4][16];
    for (int i = 0; i < 4; i++) {
        memcpy(test_local[i], px->bones[i].local_trans, 16u * sizeof(float));
    }
    we_rig_pose_ctx_t rig_skip = rig;
    rig_skip.bone_drive_modes[0] = WE_BONE_DRIVE_XPBD;
    we_rig_apply_constraints(px, &rig_skip, test_local);
    assert(memcmp(test_local[0], px->bones[0].local_trans, sizeof(test_local[0])) == 0);

    float skin[4][16];
    for (int step = 0; step < 6; step++) {
        we_puppet_eval_skin_matrices_ex(px, NULL, 0u, &rig, 0.016 * (double)(step + 1), skin);
    }
    assert((phys_ctx.configs[0].flags & WE_BONE_PHYS_ENABLED) != 0u);
    float hand_world[16];
    float bind_hand[16];
    mat4_translate(bind_hand, 2.0f, 0.0f, 0.0f);
    we_mat4_mul(skin[2], bind_hand, hand_world);
    assert(fabsf(hand_world[12] - 1.0f) < 0.05f && fabsf(hand_world[13] - 1.0f) < 0.05f);
    for (int m = 0; m < 16; m++) {
        assert(isfinite(skin[3][m]));
    }

    px->physics_ctx = NULL;
    we_puppet_destroy(px);
    printf("[PASS] 2-bone IK soft-stretch, singularity, limits, Aim, XPBD exclusion\n");
}

static we_mdl_anim_t make_single_bone_anim(int32_t id, const char *name, we_mdl_anim_mode_t mode,
                                           float fps, const float *x_vals, const float *ang_vals,
                                           uint32_t kcount)
{
    we_mdl_anim_t a;
    memset(&a, 0, sizeof(a));
    a.id = id;
    snprintf(a.name, sizeof(a.name), "%s", name);
    a.fps = fps;
    a.length = (int32_t)kcount;
    a.mode = mode;
    a.track_count = 1u;
    a.tracks = calloc(1u, sizeof(we_mdl_bone_track_t));
    assert(a.tracks);
    a.tracks[0].bone_index = 0;
    a.tracks[0].kf_count = kcount;
    a.tracks[0].keyframes = calloc(kcount, sizeof(we_mdl_keyframe_t));
    assert(a.tracks[0].keyframes);
    for (uint32_t k = 0; k < kcount; k++) {
        a.tracks[0].keyframes[k].pos[0] = x_vals[k];
        a.tracks[0].keyframes[k].angle[2] = ang_vals[k];
        a.tracks[0].keyframes[k].scale[0] = 1.0f;
        a.tracks[0].keyframes[k].scale[1] = 1.0f;
        a.tracks[0].keyframes[k].scale[2] = 1.0f;
    }
    return a;
}

static void test_multilayer_blending_and_modes(void)
{
    we_puppet_t *p = alloc_test_puppet();
    build_grid_mesh(p, 4u, 2u, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
    p->bone_count = 1u;
    p->bones = calloc(1u, sizeof(we_mdl_bone_t));
    assert(p->bones);
    init_bone(&p->bones[0], "root", 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f);
    finalize_bone_bind_offsets(p);

    const float x_loop[3] = {0.0f, 2.0f, 4.0f};
    const float a_zero[3] = {0.0f, 0.0f, 0.0f};
    const float x_single[3] = {1.0f, 2.0f, 5.0f};
    const float a_add[2] = {0.5f, 0.5f};
    const float x_add[2] = {1.5f, 1.5f};

    p->anim_count = 4u;
    p->anims = calloc(4u, sizeof(we_mdl_anim_t));
    assert(p->anims);
    p->anims[0] = make_single_bone_anim(0, "loop", WE_ANIM_LOOP, 1.0f, x_loop, a_zero, 3u);
    p->anims[1] = make_single_bone_anim(1, "mirror", WE_ANIM_MIRROR, 1.0f, x_loop, a_zero, 3u);
    p->anims[2] = make_single_bone_anim(2, "single", WE_ANIM_SINGLE, 1.0f, x_single, a_zero, 3u);
    p->anims[3] = make_single_bone_anim(3, "add", WE_ANIM_LOOP, 1.0f, x_add, a_add, 2u);

    float skin[1][16];
    we_puppet_layer_t lay[2];
    memset(lay, 0, sizeof(lay));
    lay[0].anim_id = 0;
    lay[0].rate = 1.0f;
    lay[0].blend = 1.0f;
    lay[0].visible = true;

    we_puppet_eval_skin_matrices_ex(p, lay, 1u, NULL, 3.5, skin);
    assert(fabsf(skin[0][12] - 1.0f) < 1e-4f);

    lay[0].anim_id = 1;
    we_puppet_eval_skin_matrices_ex(p, lay, 1u, NULL, 3.0, skin);
    assert(fabsf(skin[0][12] - 2.0f) < 1e-4f);

    lay[0].anim_id = 2;
    we_puppet_eval_skin_matrices_ex(p, lay, 1u, NULL, 10.0, skin);
    assert(fabsf(skin[0][12] - 5.0f) < 1e-4f);

    lay[1].anim_id = 3;
    lay[1].rate = 1.0f;
    lay[1].blend = 0.5f;
    lay[1].visible = true;
    we_rig_pose_ctx_t rig;
    memset(&rig, 0, sizeof(rig));
    rig.layer_blend_modes[0] = WE_ANIM_BLEND_OVERRIDE;
    rig.layer_blend_modes[1] = WE_ANIM_BLEND_ADDITIVE;
    we_puppet_eval_skin_matrices_ex(p, lay, 2u, &rig, 10.0, skin);
    assert(fabsf(skin[0][12] - 5.75f) < 1e-4f);
    assert(fabsf(atan2f(skin[0][1], skin[0][0]) - 0.25f) < 1e-4f);

    float dummy_m[16];
    mat4_from_z_rot_trans(dummy_m, 0.0f, 0.0f, 0.0f, 0.0f);
    assert(fabsf(dummy_m[15] - 1.0f) < 1e-6f);

    we_puppet_destroy(p);
    printf("[PASS] multi-layer blending (OVERRIDE + ADDITIVE) & animation loop modes\n");
}

static void test_obtuse_cotangent_and_small_mesh_heat(void)
{
    /* Obtuse triangle whose apex angle yields exact cot == -2.0:
     * vertices (0,0), (2,0), (1, sqrt(5)-2). */
    we_puppet_t *p = alloc_test_puppet();
    const float y_apex = sqrtf(5.0f) - 2.0f;
    p->vertex_count = 3u;
    p->triangle_count = 1u;
    p->index_count = 3u;
    p->vertices = calloc(3u, sizeof(we_mdl_vertex_t));
    p->skinned_verts = calloc(3u, sizeof(we_skinned_vertex_t));
    p->indices = calloc(3u, sizeof(uint16_t));
    assert(p->vertices && p->skinned_verts && p->indices);
    p->vertices[0].pos[0] = 0.0f;
    p->vertices[0].pos[1] = 0.0f;
    p->vertices[1].pos[0] = 2.0f;
    p->vertices[1].pos[1] = 0.0f;
    p->vertices[2].pos[0] = 1.0f;
    p->vertices[2].pos[1] = y_apex;
    p->indices[0] = 0;
    p->indices[1] = 1;
    p->indices[2] = 2;

    p->bone_count = 2u;
    p->bones = calloc(2u, sizeof(we_mdl_bone_t));
    assert(p->bones);
    init_bone(&p->bones[0], "b0", 0xFFFFFFFFu, 0.2f, 0.02f, 0.0f);
    init_bone(&p->bones[1], "b1", 0u, 1.6f, 0.0f, 0.0f);
    finalize_bone_bind_offsets(p);

    we_heat_skin_cfg_t cfg;
    we_heat_skin_cfg_default(&cfg);
    assert(we_mesh_solve_heat_weights(p, &cfg, NULL));
    for (uint32_t v = 0; v < 3u; v++) {
        assert_vertex_pou(&p->vertices[v], 2u);
    }

    const float pos[2] = {1.0f, y_apex};
    uint64_t dirty[2] = {0};
    uint32_t sm = we_mesh_apply_weight_paint_dab(p, pos, pos, 3.0f, 1.0f, 0.5f, WE_WEIGHT_OP_SMOOTH,
                                                 0u, 0.0f, NULL, NULL, dirty);
    assert(sm > 0u);
    for (uint32_t v = 0; v < 3u; v++) {
        assert_vertex_pou(&p->vertices[v], 2u);
    }
    we_puppet_destroy(p);

    /* Scale-invariant visibility ray at 1e-5 coordinates with separated quads */
    we_puppet_t *ps = alloc_test_puppet();
    ps->vertex_count = 6u;
    ps->triangle_count = 2u;
    ps->index_count = 6u;
    ps->vertices = calloc(6u, sizeof(we_mdl_vertex_t));
    ps->skinned_verts = calloc(6u, sizeof(we_skinned_vertex_t));
    ps->indices = calloc(6u, sizeof(uint16_t));
    assert(ps->vertices && ps->skinned_verts && ps->indices);
    const float sc = 1.0e-5f;
    const float coords[6][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}, {3.0f, 0.0f}, {4.0f, 0.0f}, {3.5f, 1.0f},
    };
    for (uint32_t i = 0; i < 6u; i++) {
        ps->vertices[i].pos[0] = coords[i][0] * sc;
        ps->vertices[i].pos[1] = coords[i][1] * sc;
        ps->indices[i] = (uint16_t)i;
    }
    ps->bone_count = 2u;
    ps->bones = calloc(2u, sizeof(we_mdl_bone_t));
    assert(ps->bones);
    init_bone(&ps->bones[0], "left", 0xFFFFFFFFu, 0.5f * sc, 0.3f * sc, 0.0f);
    init_bone(&ps->bones[1], "right", 0xFFFFFFFFu, 3.5f * sc, 0.3f * sc, 0.0f);
    finalize_bone_bind_offsets(ps);

    assert(we_mesh_solve_heat_weights(ps, &cfg, NULL));
    for (uint32_t v = 0; v < 6u; v++) {
        assert_vertex_pou(&ps->vertices[v], 2u);
    }
    assert(get_bone_weight(&ps->vertices[0], 0u) > 0.99f);
    assert(get_bone_weight(&ps->vertices[3], 1u) > 0.99f);
    we_puppet_destroy(ps);
    printf("[PASS] obtuse cot == -2.0 triangle & 1e-5 scale visibility heat skinning\n");
}

static void test_ik_zero_lock_stretch_blend_and_immutable_physics(void)
{
    /* 1. [0,0] strict angle lock via enabled=0x3u vs legacy unconstrained enabled=0x1u */
    we_puppet_t *p = create_ik_arm_puppet(1.0f, 1.0f, false);
    float anim_local[3][16];
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    we_ik_chain_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.root_bone = 0u;
    ch.mid_bone = 1u;
    ch.end_bone = 2u;
    ch.enabled = 0x3u;
    ch.blend_weight = 1.0f;
    ch.target_world[0] = 0.0f;
    ch.target_world[1] = 1.5f;
    ch.pole_world[0] = -1.0f;
    ch.pole_world[1] = 0.5f;
    assert(we_ik_solve_two_bone(p, &ch, anim_local));
    assert(fabsf(atan2f(anim_local[0][1], anim_local[0][0])) < 1e-5f);
    assert(fabsf(atan2f(anim_local[1][1], anim_local[1][0])) < 1e-5f);

    /* 2. Stretch end_bone blend at blend_weight=0.5f */
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    float full_stretch[3][16];
    for (int i = 0; i < 3; i++) {
        memcpy(full_stretch[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    ch.enabled = 0x1u;
    ch.soft_zone_ratio = 0.1f;
    ch.max_stretch_ratio = 1.25f;
    ch.target_world[0] = 3.0f;
    ch.target_world[1] = 0.0f;
    ch.blend_weight = 1.0f;
    assert(we_ik_solve_two_bone(p, &ch, full_stretch));
    assert(full_stretch[2][12] > 1.01f);

    ch.blend_weight = 0.5f;
    assert(we_ik_solve_two_bone(p, &ch, anim_local));
    float expected_e_x = 0.5f * (p->bones[2].local_trans[12] + full_stretch[2][12]);
    assert(fabsf(anim_local[2][12] - expected_e_x) < 1e-4f);

    /* 3. XPBD end_bone protection inside we_rig_apply_constraints */
    we_rig_pose_ctx_t rig;
    memset(&rig, 0, sizeof(rig));
    rig.ik_chain_count = 1u;
    rig.ik_chains[0] = ch;
    rig.ik_chains[0].blend_weight = 1.0f;
    rig.bone_drive_modes[0] = WE_BONE_DRIVE_IK;
    rig.bone_drive_modes[1] = WE_BONE_DRIVE_IK;
    rig.bone_drive_modes[2] = WE_BONE_DRIVE_XPBD;
    for (int i = 0; i < 3; i++) {
        memcpy(anim_local[i], p->bones[i].local_trans, 16u * sizeof(float));
    }
    anim_local[2][12] = 1.42f;
    we_rig_apply_constraints(p, &rig, anim_local);
    assert(fabsf(anim_local[2][12] - 1.42f) < 1e-6f);
    we_puppet_destroy(p);

    /* 4. Non-topological skeleton (child idx 0, root idx 1) + immutable physics configs.flags */
    we_puppet_t *pn = alloc_test_puppet();
    build_grid_mesh(pn, 4u, 2u, 0.0f, -0.1f, 2.0f, 0.1f, 0.0f);
    pn->bone_count = 4u;
    pn->bones = calloc(4u, sizeof(we_mdl_bone_t));
    assert(pn->bones);
    init_bone(&pn->bones[1], "root", 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f);
    init_bone(&pn->bones[2], "mid", 1u, 1.0f, 0.0f, 0.0f);
    init_bone(&pn->bones[3], "end", 2u, 1.0f, 0.0f, 0.0f);
    init_bone(&pn->bones[0], "tail", 3u, 0.5f, 0.0f, 0.0f);
    finalize_bone_bind_offsets(pn);

    pn->physics_ctx = calloc(1u, sizeof(we_puppet_physics_ctx_t));
    assert(pn->physics_ctx);
    we_physics_init_from_puppet(pn->physics_ctx, pn);
    for (uint32_t b = 0; b < 4u; b++) {
        pn->physics_ctx->configs[b].flags = WE_BONE_PHYS_ENABLED;
        pn->physics_ctx->configs[b].mass_kg = 1.0f;
        pn->physics_ctx->configs[b].rest_length_px = 0.5f;
    }
    pn->physics_ctx->configs[1].flags |= WE_BONE_PHYS_PINNED_ROOT;
    pn->physics_ctx->active_bone_count = 4u;

    uint32_t orig_flags[4];
    for (uint32_t b = 0; b < 4u; b++) {
        orig_flags[b] = pn->physics_ctx->configs[b].flags;
    }

    we_rig_pose_ctx_t non_topo_rig;
    memset(&non_topo_rig, 0, sizeof(non_topo_rig));
    non_topo_rig.bone_drive_modes[1] = WE_BONE_DRIVE_IK;
    non_topo_rig.bone_drive_modes[2] = WE_BONE_DRIVE_IK;
    non_topo_rig.bone_drive_modes[3] = WE_BONE_DRIVE_IK;
    non_topo_rig.bone_drive_modes[0] = WE_BONE_DRIVE_XPBD;
    non_topo_rig.ik_chain_count = 1u;
    non_topo_rig.ik_chains[0] = (we_ik_chain_t){
        .root_bone = 1u,
        .mid_bone = 2u,
        .end_bone = 3u,
        .enabled = 1u,
        .blend_weight = 1.0f,
        .target_world = {1.5f, 0.5f, 0.0f},
        .pole_world = {0.5f, 1.5f, 0.0f},
    };

    float skin[4][16];
    for (int f = 0; f < 5; f++) {
        we_puppet_eval_skin_matrices_ex(pn, NULL, 0u, &non_topo_rig, 0.016 * (double)(f + 1), skin);
        for (uint32_t b = 0; b < 4u; b++) {
            assert(pn->physics_ctx->configs[b].flags == orig_flags[b]);
            for (int m = 0; m < 16; m++) {
                assert(isfinite(skin[b][m]));
            }
        }
    }
    we_puppet_destroy(pn);
    printf("[PASS] [0,0] IK lock, stretch blend, end_bone XPBD guard & immutable physics flags\n");
}

int main(void)
{
    test_skeleton_topology_and_cycles();
    test_heat_skinning_visibility_islands_idt_and_perf();
    test_weight_renorm_and_cpu_brush();
    test_gpu_gles31_weight_brush();
    test_ik_aim_limits_and_xpbd_exclusion();
    test_multilayer_blending_and_modes();
    test_obtuse_cotangent_and_small_mesh_heat();
    test_ik_zero_lock_stretch_blend_and_immutable_physics();
    printf("ALL test_we_mesh_rig CHECKS PASSED\n");
    return 0;
}
