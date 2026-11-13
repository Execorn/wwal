// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "waywal/we_mesh_cdt.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double tri_area_px(const we_mdl_vertex_t *a, const we_mdl_vertex_t *b,
                          const we_mdl_vertex_t *c)
{
    double ax = (double)a->pos[0];
    double ay = -(double)a->pos[1];
    double bx = (double)b->pos[0];
    double by = -(double)b->pos[1];
    double cx = (double)c->pos[0];
    double cy = -(double)c->pos[1];
    return 0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax));
}

static uint64_t pack_edge_u64(uint16_t a, uint16_t b)
{
    if (a > b) {
        uint16_t t = a;
        a = b;
        b = t;
    }
    return ((uint64_t)a << 16u) | (uint64_t)b;
}

static int cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa;
    uint64_t b = *(const uint64_t *)pb;
    return (a > b) - (a < b);
}

static void assert_valid_mesh(const we_mesh_gen_result_t *res, uint32_t tex_w, uint32_t tex_h,
                              uint32_t max_verts, int expected_euler, bool check_euler)
{
    assert(res != NULL);
    assert(res->vertices != NULL);
    assert(res->indices != NULL);
    assert(res->vertex_count >= 3u);
    assert(res->vertex_count <= max_verts);
    assert(res->vertex_count <= WE_MDL_MAX_VERTICES);
    assert(res->triangle_count >= 1u);
    assert(res->triangle_count <= WE_MDL_MAX_TRIANGLES);
    assert(res->index_count == res->triangle_count * 3u);
    assert(isfinite(res->achieved_min_angle_deg) && res->achieved_min_angle_deg > 0.0f);
    assert(isfinite(res->achieved_mean_quality) && res->achieved_mean_quality > 0.0f);

    for (uint32_t i = 0u; i < res->vertex_count; i++) {
        const we_mdl_vertex_t *v = &res->vertices[i];
        assert(isfinite(v->pos[0]) && isfinite(v->pos[1]) && isfinite(v->pos[2]));
        assert(v->pos[2] == 0.0f);
        assert(v->texcoord[0] >= 0.0f && v->texcoord[0] <= 1.0f);
        assert(v->texcoord[1] >= 0.0f && v->texcoord[1] <= 1.0f);
        assert(v->blend_indices[0] == 0u && v->blend_indices[1] == 0u &&
               v->blend_indices[2] == 0u && v->blend_indices[3] == 0u);
        assert(v->weight[0] == 1.0f && v->weight[1] == 0.0f && v->weight[2] == 0.0f &&
               v->weight[3] == 0.0f);
        double px = (double)v->pos[0] + 0.5 * (double)tex_w;
        double py = 0.5 * (double)tex_h - (double)v->pos[1];
        float exp_u = fminf(fmaxf((float)(px / (double)tex_w), 0.0f), 1.0f);
        float exp_v = fminf(fmaxf((float)(py / (double)tex_h), 0.0f), 1.0f);
        assert(fabsf(v->texcoord[0] - exp_u) <= 1e-4f);
        assert(fabsf(v->texcoord[1] - exp_v) <= 1e-4f);
    }

    uint64_t *edges = malloc((size_t)res->triangle_count * 3u * sizeof(uint64_t));
    assert(edges != NULL);
    uint32_t e_cnt = 0u;

    for (uint32_t t = 0u; t < res->triangle_count; t++) {
        uint16_t i0 = res->indices[t * 3u + 0u];
        uint16_t i1 = res->indices[t * 3u + 1u];
        uint16_t i2 = res->indices[t * 3u + 2u];
        assert(i0 < res->vertex_count && i1 < res->vertex_count && i2 < res->vertex_count);
        assert(i0 != i1 && i1 != i2 && i2 != i0);

        double area = tri_area_px(&res->vertices[i0], &res->vertices[i1], &res->vertices[i2]);
        assert(area > 1e-9);

        edges[e_cnt++] = pack_edge_u64(i0, i1);
        edges[e_cnt++] = pack_edge_u64(i1, i2);
        edges[e_cnt++] = pack_edge_u64(i2, i0);
    }

    qsort(edges, e_cnt, sizeof(uint64_t), cmp_u64);
    uint32_t unique_edges = 0u;
    for (uint32_t i = 0u; i < e_cnt;) {
        uint32_t j = i + 1u;
        while (j < e_cnt && edges[j] == edges[i]) {
            j++;
        }
        /* Manifold 2D mesh: every undirected edge belongs to 1 (boundary) or 2 (interior) tris */
        assert((j - i) == 1u || (j - i) == 2u);
        unique_edges++;
        i = j;
    }
    free(edges);

    if (check_euler) {
        int euler = (int)res->vertex_count - (int)unique_edges + (int)res->triangle_count;
        assert(euler == expected_euler);
    }
}

static bool has_pin_vertex(const we_mesh_gen_result_t *res, double px, double py, uint32_t w,
                           uint32_t h)
{
    float wx = (float)(px - 0.5 * (double)w);
    float wy = (float)(0.5 * (double)h - py);
    for (uint32_t i = 0u; i < res->vertex_count; i++) {
        float dx = res->vertices[i].pos[0] - wx;
        float dy = res->vertices[i].pos[1] - wy;
        if (dx * dx + dy * dy <= 1e-6f) {
            return true;
        }
    }
    return false;
}

static void test_config_and_validation(void)
{
    printf("[+] Testing config defaults and invalid input validation...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    assert(fabsf(cfg.alpha_threshold - 0.5f) < 1e-6f);
    assert(fabsf(cfg.dilation_px - 2.0f) < 1e-6f);
    assert(fabsf(cfg.simplify_eps_px - 1.5f) < 1e-6f);
    assert(fabsf(cfg.interior_spacing - 24.0f) < 1e-6f);
    assert(fabsf(cfg.min_angle_deg - 25.0f) < 1e-6f);
    assert(fabsf(cfg.min_island_area - 4.0f) < 1e-6f);
    assert(cfg.max_vertices == 8192u);

    uint8_t dummy[16] = {255};
    we_cdt_loop_t *loops = NULL;
    uint32_t lcnt = 99u;
    assert(!we_mesh_extract_contours(NULL, 4, 4, 4, &cfg, &loops, &lcnt));
    assert(loops == NULL && lcnt == 0u);
    assert(!we_mesh_extract_contours(dummy, 0, 4, 4, &cfg, &loops, &lcnt));
    assert(!we_mesh_extract_contours(dummy, 4, 0, 4, &cfg, &loops, &lcnt));
    assert(!we_mesh_extract_contours(dummy, WE_CDT_MAX_IMAGE_DIM + 1u, 4, 4, &cfg, &loops, &lcnt));
    assert(!we_mesh_extract_contours(dummy, 4, 4, 2, &cfg, &loops, &lcnt));

    we_mesh_gen_result_t res;
    assert(!we_mesh_gen_from_alpha(dummy, 0, 0, 0, &cfg, NULL, 0, &res));
    assert(!we_mesh_gen_from_rgba8(dummy, 2, 2, 4, &cfg, NULL, 0, &res));

    we_mesh_gen_cfg_t bad_budget = cfg;
    bad_budget.max_vertices = 2u;
    assert(!we_mesh_gen_from_alpha(dummy, 4, 4, 4, &bad_budget, NULL, 0, &res));
}

static void test_alpha_edge_cases(void)
{
    printf("[+] Testing 1x1, uniform alpha, stride, and RGBA8 cutouts...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);

    /* Uniform zero alpha */
    uint8_t zeros[64 * 64];
    memset(zeros, 0, sizeof(zeros));
    we_cdt_loop_t *loops = NULL;
    uint32_t lcnt = 0u;
    assert(we_mesh_extract_contours(zeros, 64, 64, 0, &cfg, &loops, &lcnt));
    assert(lcnt == 0u && loops == NULL);
    we_mesh_loops_free(loops, lcnt);

    we_mesh_gen_result_t res;
    assert(!we_mesh_gen_from_alpha(zeros, 64, 64, 0, &cfg, NULL, 0, &res));

    /* 1x1 opaque pixel with default dilation (2 px -> ~5x5 square) */
    uint8_t one_px = 255u;
    assert(we_mesh_gen_from_alpha(&one_px, 1, 1, 1, &cfg, NULL, 0, &res));
    assert_valid_mesh(&res, 1, 1, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);

    /* 1x1 opaque pixel with 0 dilation and 0 min_island_area */
    we_mesh_gen_cfg_t tight_cfg = cfg;
    tight_cfg.dilation_px = 0.0f;
    tight_cfg.min_island_area = 0.0f;
    assert(we_mesh_gen_from_alpha(&one_px, 1, 1, 1, &tight_cfg, NULL, 0, &res));
    assert_valid_mesh(&res, 1, 1, tight_cfg.max_vertices, 1, true);
    we_mesh_gen_result_free(&res);

    /* Uniform opaque 64x64 */
    uint8_t full[64 * 64];
    memset(full, 255, sizeof(full));
    assert(we_mesh_gen_from_alpha(full, 64, 64, 64, &cfg, NULL, 0, &res));
    assert_valid_mesh(&res, 64, 64, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);

    /* RGBA8 with padded stride */
    uint32_t w = 64u;
    uint32_t h = 64u;
    uint32_t stride = w * 4u + 16u;
    uint8_t *rgba = calloc((size_t)h * stride, 1u);
    assert(rgba != NULL);
    for (uint32_t y = 8u; y < 56u; y++) {
        for (uint32_t x = 8u; x < 56u; x++) {
            double dx = (double)x - 31.5;
            double dy = (double)y - 31.5;
            double r = hypot(dx, dy);
            uint8_t a = (r <= 20.0) ? 255u : (r <= 22.0 ? (uint8_t)((22.0 - r) * 127.0) : 0u);
            rgba[(size_t)y * stride + x * 4u + 0u] = 200u;
            rgba[(size_t)y * stride + x * 4u + 1u] = 100u;
            rgba[(size_t)y * stride + x * 4u + 2u] = 50u;
            rgba[(size_t)y * stride + x * 4u + 3u] = a;
        }
    }
    we_cdt_pin_t pin = {.px = 31.5, .py = 31.5, .flags = 1u};
    assert(we_mesh_gen_from_rgba8(rgba, w, h, stride, &cfg, &pin, 1u, &res));
    assert_valid_mesh(&res, w, h, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    assert(has_pin_vertex(&res, 31.5, 31.5, w, h));
    we_mesh_gen_result_free(&res);
    free(rgba);
}

static void test_asymptotic_decider_saddles(void)
{
    printf("[+] Testing Marching Squares Asymptotic Decider saddle disambiguation...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.dilation_px = 0.0f;
    cfg.min_island_area = 0.0f;
    cfg.alpha_threshold = 0.5f;

    /* 4x4 grid with TL=(1,1), BR=(2,2) high (>128), TR=(2,1), BL=(1,2) low (<128):
       When high=255, low=100: s00=+255, s11=+255, s10=-55, s01=-55 -> N = 255^2 - 55^2 > 0
       => center inside => 1 connected loop! */
    uint8_t buf_connected[4 * 4] = {0};
    buf_connected[1 * 4 + 1] = 255u;
    buf_connected[2 * 4 + 2] = 255u;
    buf_connected[1 * 4 + 2] = 100u;
    buf_connected[2 * 4 + 1] = 100u;

    we_cdt_loop_t *loops = NULL;
    uint32_t lcnt = 0u;
    assert(we_mesh_extract_contours(buf_connected, 4, 4, 4, &cfg, &loops, &lcnt));
    assert(lcnt == 1u);
    assert(!loops[0].is_hole && loops[0].signed_area_px2 > 0.0);
    we_mesh_loops_free(loops, lcnt);

    /* When high=150, low=0: s00=+45, s11=+45, s10=-255, s01=-255 -> N = 45^2 - 255^2 < 0
       => center outside => 2 disconnected loops! */
    uint8_t buf_split[4 * 4] = {0};
    buf_split[1 * 4 + 1] = 150u;
    buf_split[2 * 4 + 2] = 150u;
    buf_split[1 * 4 + 2] = 0u;
    buf_split[2 * 4 + 1] = 0u;

    assert(we_mesh_extract_contours(buf_split, 4, 4, 4, &cfg, &loops, &lcnt));
    assert(lcnt == 2u);
    assert(!loops[0].is_hole && !loops[1].is_hole);
    we_mesh_loops_free(loops, lcnt);

    /* Exact N == 0 tie-break (255/0 checkerboard at alpha_threshold = 0.5f => s=+/-255, det=0) */
    uint8_t buf_tie_5[4 * 4] = {0};
    buf_tie_5[1 * 4 + 1] = 255u;
    buf_tie_5[2 * 4 + 2] = 255u;
    assert(we_mesh_extract_contours(buf_tie_5, 4, 4, 4, &cfg, &loops, &lcnt));
    assert(lcnt == 2u);
    assert(!loops[0].is_hole && !loops[1].is_hole);
    assert(loops[0].signed_area_px2 > 0.0 && loops[1].signed_area_px2 > 0.0);
    we_mesh_loops_free(loops, lcnt);

    uint8_t buf_tie_10[4 * 4] = {0};
    buf_tie_10[1 * 4 + 2] = 255u;
    buf_tie_10[2 * 4 + 1] = 255u;
    assert(we_mesh_extract_contours(buf_tie_10, 4, 4, 4, &cfg, &loops, &lcnt));
    assert(lcnt == 2u);
    assert(!loops[0].is_hole && !loops[1].is_hole);
    assert(loops[0].signed_area_px2 > 0.0 && loops[1].signed_area_px2 > 0.0);
    we_mesh_loops_free(loops, lcnt);
}

static void test_star_and_concave_polygons(void)
{
    printf("[+] Testing star & concave polygons with hard pins and min-angle >= 25 deg...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.interior_spacing = 18.0f;
    cfg.min_angle_deg = 25.0f;
    cfg.simplify_eps_px = 0.5f;

    /* 5-pointed star with ~54 deg tip angle (>= 50 deg, so full 25 deg achievable) */
    we_cdt_vec2_t star_pts[10];
    double cx = 150.0;
    double cy = 150.0;
    for (int k = 0; k < 10; k++) {
        double ang = (double)k * (M_PI / 5.0) - M_PI / 2.0;
        double r = (k % 2 == 0) ? 120.0 : 62.0;
        star_pts[k] = (we_cdt_vec2_t){cx + r * cos(ang), cy + r * sin(ang)};
    }
    we_cdt_loop_t star_loop = {.points = star_pts, .point_count = 10u, .is_hole = false};
    we_cdt_pin_t pins[2] = {
        {.px = 150.0, .py = 150.0, .flags = 1u},
        {.px = 150.0, .py = 110.0, .flags = 1u},
    };

    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_loops(&star_loop, 1u, 300u, 300u, &cfg, pins, 2u, &res));
    assert_valid_mesh(&res, 300u, 300u, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    assert(has_pin_vertex(&res, 150.0, 150.0, 300u, 300u));
    assert(has_pin_vertex(&res, 150.0, 110.0, 300u, 300u));
    we_mesh_gen_result_free(&res);

    /* Deep concave U-shape */
    we_cdt_vec2_t u_pts[8] = {
        {20.0, 20.0},  {80.0, 20.0},  {80.0, 140.0},  {160.0, 140.0},
        {160.0, 20.0}, {220.0, 20.0}, {220.0, 200.0}, {20.0, 200.0},
    };
    we_cdt_loop_t u_loop = {.points = u_pts, .point_count = 8u, .is_hole = false};
    assert(we_mesh_gen_from_loops(&u_loop, 1u, 256u, 256u, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);
}

static void test_holes_and_nested_islands(void)
{
    printf("[+] Testing holes and nested islands (Euler & winding invariants)...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.interior_spacing = 20.0f;
    cfg.min_angle_deg = 25.0f;

    we_cdt_vec2_t outer_pts[4] = {
        {10.0, 10.0},
        {240.0, 10.0},
        {240.0, 240.0},
        {10.0, 240.0},
    };
    we_cdt_vec2_t hole_pts[4] = {
        {60.0, 60.0},
        {60.0, 190.0},
        {190.0, 190.0},
        {190.0, 60.0},
    };
    we_cdt_vec2_t island_pts[4] = {
        {100.0, 100.0},
        {150.0, 100.0},
        {150.0, 150.0},
        {100.0, 150.0},
    };

    /* Single hole => Euler characteristic V - E + F = 0 */
    we_cdt_loop_t loops_1hole[2] = {
        {.points = outer_pts, .point_count = 4u, .is_hole = false},
        {.points = hole_pts, .point_count = 4u, .is_hole = true},
    };
    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_loops(loops_1hole, 2u, 256u, 256u, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, cfg.max_vertices, 0, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);

    /* Outer + Hole + Nested Island => Euler V - E + F = 0 + 1 = 1 */
    we_cdt_loop_t loops_island[3] = {
        {.points = outer_pts, .point_count = 4u, .is_hole = false},
        {.points = hole_pts, .point_count = 4u, .is_hole = true},
        {.points = island_pts, .point_count = 4u, .is_hole = false},
    };
    assert(we_mesh_gen_from_loops(loops_island, 3u, 256u, 256u, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);
}

static void test_thin_tentacles_and_budget(void)
{
    printf("[+] Testing thin tentacles and vertex budget capping...\n");
    uint32_t w = 256u;
    uint32_t h = 64u;
    uint8_t *alpha = calloc((size_t)w * h, 1u);
    assert(alpha != NULL);

    /* Body [10..50] x [12..52] + 2px-tall tentacle [50..240] x [31..33] */
    for (uint32_t y = 12u; y < 52u; y++) {
        for (uint32_t x = 10u; x < 50u; x++) {
            alpha[y * w + x] = 255u;
        }
    }
    for (uint32_t y = 31u; y < 33u; y++) {
        for (uint32_t x = 50u; x < 240u; x++) {
            alpha[y * w + x] = 255u;
        }
    }

    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.dilation_px = 0.0f;
    cfg.simplify_eps_px = 0.5f;
    cfg.interior_spacing = 16.0f;
    cfg.min_angle_deg = 25.0f;

    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_alpha(alpha, w, h, w, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, w, h, cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);

    /* Vertex budget clamping test */
    cfg.max_vertices = 40u;
    assert(we_mesh_gen_from_alpha(alpha, w, h, w, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, w, h, 40u, 1, true);
    assert(res.hit_vertex_budget);
    we_mesh_gen_result_free(&res);
    free(alpha);
}

static uint32_t fuzz_rng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *s = x;
    return x;
}

static void test_adversarial_fuzz_inputs(void)
{
    printf("[+] Running adversarial fuzz & invalid topology tests...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);

    /* Self-intersecting bowtie loop must fail cleanly without leaking */
    we_cdt_vec2_t bowtie[4] = {{0.0, 0.0}, {100.0, 100.0}, {100.0, 0.0}, {0.0, 100.0}};
    we_cdt_loop_t bad_loop = {.points = bowtie, .point_count = 4u, .is_hole = false};
    we_mesh_gen_result_t res;
    assert(!we_mesh_gen_from_loops(&bad_loop, 1u, 128u, 128u, &cfg, NULL, 0u, &res));

    /* NaN coordinate must fail cleanly */
    we_cdt_vec2_t nan_pts[3] = {{10.0, 10.0}, {NAN, 20.0}, {30.0, 10.0}};
    we_cdt_loop_t nan_loop = {.points = nan_pts, .point_count = 3u, .is_hole = false};
    assert(!we_mesh_gen_from_loops(&nan_loop, 1u, 64u, 64u, &cfg, NULL, 0u, &res));

    /* Random alpha fuzzing across 20 seeds (checkerboards, blobs, noise) */
    uint32_t seed = 0xDEADBEEFu;
    uint8_t alpha[32 * 32];
    uint32_t success_count = 0u;
    for (uint32_t iter = 0u; iter < 20u; iter++) {
        for (size_t i = 0u; i < sizeof(alpha); i++) {
            uint32_t r = fuzz_rng(&seed);
            alpha[i] = (iter % 2u == 0u) ? (uint8_t)(r & 0xFFu) : ((r & 1u) ? 255u : 0u);
        }
        we_mesh_gen_cfg_t fcfg = cfg;
        fcfg.dilation_px = (float)(iter % 4u);
        fcfg.min_island_area = (iter % 3u == 0u) ? 0.0f : 4.0f;
        if (we_mesh_gen_from_alpha(alpha, 32u, 32u, 32u, &fcfg, NULL, 0u, &res)) {
            assert_valid_mesh(&res, 32u, 32u, fcfg.max_vertices, 0, false);
            we_mesh_gen_result_free(&res);
            success_count++;
        }
    }
    assert(success_count > 0u);
}

static void test_acute_corners_multi_island_and_uv(void)
{
    printf("[+] Testing acute corners, multi-island alpha, and ortho UV alignment...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.min_angle_deg = 25.0f;

    /* Sharp 20-deg acute wedge to verify concentric shell termination */
    we_cdt_vec2_t wedge[3] = {{20.0, 100.0}, {220.0, 82.0}, {220.0, 118.0}};
    we_cdt_loop_t wedge_loop = {.points = wedge, .point_count = 3u, .is_hole = false};
    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_loops(&wedge_loop, 1u, 256u, 256u, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, cfg.max_vertices, 1, true);
    we_mesh_gen_result_free(&res);

    /* Multi-island alpha cutout (3 disconnected blobs => Euler V - E + F = 3) */
    uint32_t w = 256u;
    uint32_t h = 256u;
    uint8_t *alpha = calloc((size_t)w * h, 1u);
    assert(alpha != NULL);
    const uint32_t centers[3][2] = {{48u, 48u}, {192u, 64u}, {120u, 192u}};
    for (int c = 0; c < 3; c++) {
        for (uint32_t y = centers[c][1] - 24u; y <= centers[c][1] + 24u; y++) {
            for (uint32_t x = centers[c][0] - 24u; x <= centers[c][0] + 24u; x++) {
                double r =
                    hypot((double)x - (double)centers[c][0], (double)y - (double)centers[c][1]);
                if (r <= 18.0) {
                    alpha[y * w + x] = 255u;
                }
            }
        }
    }
    assert(we_mesh_gen_from_alpha(alpha, w, h, w, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, w, h, cfg.max_vertices, 3, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    we_mesh_gen_result_free(&res);
    free(alpha);

    /* Exact pos / texcoord correspondence for a known box [20..80] x [10..50] on 100x100 */
    we_cdt_vec2_t box[4] = {{20.0, 10.0}, {80.0, 10.0}, {80.0, 50.0}, {20.0, 50.0}};
    we_cdt_loop_t box_loop = {.points = box, .point_count = 4u, .is_hole = false};
    we_mesh_gen_cfg_t box_cfg = cfg;
    box_cfg.interior_spacing = 200.0f;
    assert(we_mesh_gen_from_loops(&box_loop, 1u, 100u, 100u, &box_cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 100u, 100u, box_cfg.max_vertices, 1, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    bool found_tl = false;
    for (uint32_t i = 0u; i < res.vertex_count; i++) {
        if (fabsf(res.vertices[i].pos[0] - (20.0f - 50.0f)) < 1e-4f &&
            fabsf(res.vertices[i].pos[1] - (50.0f - 10.0f)) < 1e-4f) {
            assert(fabsf(res.vertices[i].texcoord[0] - 0.20f) < 1e-5f);
            assert(fabsf(res.vertices[i].texcoord[1] - 0.10f) < 1e-5f);
            found_tl = true;
        }
    }
    assert(found_tl);
    we_mesh_gen_result_free(&res);
}

static void test_pin_edge_cases_and_budget_clamping(void)
{
    printf("[+] Testing boundary/exterior/close pins, budget clamping, reflex channels & 1e8 "
           "coords...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.min_angle_deg = 25.0f;

    /* Boundary, exterior, flags=0, and 1e-5 px close pins at 8192x8192 texture scale */
    we_cdt_vec2_t big_box[4] = {
        {100.0, 100.0},
        {8092.0, 100.0},
        {8092.0, 8092.0},
        {100.0, 8092.0},
    };
    we_cdt_loop_t big_loop = {.points = big_box, .point_count = 4u, .is_hole = false};
    we_cdt_pin_t pins[6] = {
        {.px = 4096.0, .py = 100.0, .flags = 1u},         /* Directly on constrained edge */
        {.px = 50000.0, .py = 50000.0, .flags = 1u},      /* Exterior past super-triangle */
        {.px = -10.0, .py = -10.0, .flags = 1u},          /* Exterior inside super-triangle */
        {.px = 2000.0, .py = 2000.0, .flags = 0u},        /* flags=0 must be ignored */
        {.px = 4096.0, .py = 4096.0, .flags = 1u},        /* Close pair A */
        {.px = 4096.0 + 1e-5, .py = 4096.0, .flags = 1u}, /* Close pair B (1e-5 px) */
    };
    we_mesh_gen_cfg_t big_cfg = cfg;
    big_cfg.interior_spacing = 512.0f;
    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_loops(&big_loop, 1u, 8192u, 8192u, &big_cfg, pins, 6u, &res));
    assert_valid_mesh(&res, 8192u, 8192u, big_cfg.max_vertices, 1, true);
    assert(has_pin_vertex(&res, 4096.0, 100.0, 8192u, 8192u));
    assert(has_pin_vertex(&res, 4096.0, 4096.0, 8192u, 8192u));
    assert(!has_pin_vertex(&res, 50000.0, 50000.0, 8192u, 8192u));
    assert(!has_pin_vertex(&res, -10.0, -10.0, 8192u, 8192u));
    assert(!has_pin_vertex(&res, 2000.0, 2000.0, 8192u, 8192u));
    we_mesh_gen_result_free(&res);

    /* Adaptive TP-RDP budget clamping when raw loop vertices exceed max_vertices */
    we_cdt_vec2_t circle_pts[128];
    for (uint32_t k = 0u; k < 128u; k++) {
        double a = (double)k * (2.0 * M_PI / 128.0);
        circle_pts[k] = (we_cdt_vec2_t){128.0 + 96.0 * cos(a), 128.0 + 96.0 * sin(a)};
    }
    we_cdt_loop_t circle_loop = {.points = circle_pts, .point_count = 128u, .is_hole = false};
    we_mesh_gen_cfg_t tight_budget = cfg;
    tight_budget.simplify_eps_px = 0.25f;
    tight_budget.max_vertices = 32u;
    assert(we_mesh_gen_from_loops(&circle_loop, 1u, 256u, 256u, &tight_budget, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, 32u, 1, true);
    assert(res.hit_vertex_budget);
    we_mesh_gen_result_free(&res);

    /* Deep curved reflex channel (120 vertices) stressing Sloan recovery queue */
    we_cdt_vec2_t reflex_pts[122];
    for (uint32_t k = 0u; k < 60u; k++) {
        double t = (double)k / 59.0;
        double ang = M_PI * (0.1 + 0.8 * t);
        reflex_pts[k] = (we_cdt_vec2_t){256.0 + 180.0 * cos(ang), 256.0 + 180.0 * sin(ang)};
    }
    reflex_pts[60] = (we_cdt_vec2_t){60.0, 260.0};
    for (uint32_t k = 0u; k < 60u; k++) {
        double t = (double)(59u - k) / 59.0;
        double ang = M_PI * (0.1 + 0.8 * t);
        reflex_pts[61u + k] = (we_cdt_vec2_t){256.0 + 140.0 * cos(ang), 256.0 + 140.0 * sin(ang)};
    }
    reflex_pts[121] = (we_cdt_vec2_t){452.0, 260.0};
    we_cdt_loop_t reflex_loop = {.points = reflex_pts, .point_count = 122u, .is_hole = false};
    we_mesh_gen_cfg_t reflex_cfg = cfg;
    reflex_cfg.simplify_eps_px = 0.25f;
    assert(we_mesh_gen_from_loops(&reflex_loop, 1u, 512u, 512u, &reflex_cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 512u, 512u, reflex_cfg.max_vertices, 1, true);
    we_mesh_gen_result_free(&res);

    /* Large-coordinate (1e8) TP-RDP + CDT robustness */
    const double base = 1e8;
    we_cdt_vec2_t large_pts[8] = {
        {base + 0.0, base + 0.0},     {base + 100.0, base + 0.1},   {base + 200.0, base + 0.0},
        {base + 200.1, base + 100.0}, {base + 200.0, base + 200.0}, {base + 100.0, base + 199.9},
        {base + 0.0, base + 200.0},   {base - 0.1, base + 100.0},
    };
    we_cdt_loop_t large_loop = {.points = large_pts, .point_count = 8u, .is_hole = false};
    we_mesh_gen_cfg_t large_cfg = cfg;
    large_cfg.interior_spacing = 120.0f;
    assert(we_mesh_gen_from_loops(&large_loop, 1u, 256u, 256u, &large_cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, 256u, 256u, large_cfg.max_vertices, 1, true);
    we_mesh_gen_result_free(&res);
}

static void test_high_res_and_strict_perf(void)
{
    printf("[+] Testing 1024x1024 CDT perf & 4096x4096 bounded contour raster...\n");
    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.min_angle_deg = 25.0f;
    cfg.interior_spacing = 32.0f;

    /* 1024x1024 alpha ring */
    uint32_t w = 1024u;
    uint32_t h = 1024u;
    uint8_t *alpha = calloc((size_t)w * h, 1u);
    assert(alpha != NULL);
    for (uint32_t y = 64u; y < 960u; y++) {
        for (uint32_t x = 64u; x < 960u; x++) {
            double r = hypot((double)x - 511.5, (double)y - 511.5);
            if (r <= 420.0 && r >= 120.0) {
                alpha[(size_t)y * w + x] = 255u;
            }
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    we_mesh_gen_result_t res;
    assert(we_mesh_gen_from_alpha(alpha, w, h, w, &cfg, NULL, 0u, &res));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms =
        (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;
    printf("[BENCH] 1024x1024 ring CDT: %.3f ms (verts=%u, tris=%u, min_ang=%.2f deg)\n",
           elapsed_ms, res.vertex_count, res.triangle_count, (double)res.achieved_min_angle_deg);

    assert_valid_mesh(&res, w, h, cfg.max_vertices, 0, true);
    assert(res.achieved_min_angle_deg >= 25.0f);
    if (getenv("WWAL_STRICT_PERF_ASSERTS")) {
        assert(elapsed_ms < 500.0);
    }
    we_mesh_gen_result_free(&res);
    free(alpha);

    /* 4096x4096 contour extraction verifies downscale-and-scale-back coordinate accuracy */
    uint32_t w4 = 4096u;
    uint32_t h4 = 4096u;
    uint8_t *alpha4 = calloc((size_t)w4 * h4, 1u);
    assert(alpha4 != NULL);
    for (uint32_t y = 1024u; y < 3072u; y++) {
        memset(alpha4 + (size_t)y * w4 + 1024u, 255, 2048u);
    }
    we_cdt_loop_t *loops4 = NULL;
    uint32_t lcnt4 = 0u;
    assert(we_mesh_extract_contours(alpha4, w4, h4, w4, &cfg, &loops4, &lcnt4));
    assert(lcnt4 == 1u);
    assert(loops4[0].signed_area_px2 > 2000.0 * 2000.0);
    assert(we_mesh_gen_from_loops(loops4, lcnt4, w4, h4, &cfg, NULL, 0u, &res));
    assert_valid_mesh(&res, w4, h4, cfg.max_vertices, 1, true);
    we_mesh_gen_result_free(&res);
    we_mesh_loops_free(loops4, lcnt4);
    free(alpha4);
}

int main(void)
{
    test_config_and_validation();
    test_alpha_edge_cases();
    test_asymptotic_decider_saddles();
    test_star_and_concave_polygons();
    test_holes_and_nested_islands();
    test_thin_tentacles_and_budget();
    test_acute_corners_multi_island_and_uv();
    test_adversarial_fuzz_inputs();
    test_pin_edge_cases_and_budget_clamping();
    test_high_res_and_strict_perf();
    printf("[PASS] All we_mesh_cdt tests passed.\n");
    return 0;
}
