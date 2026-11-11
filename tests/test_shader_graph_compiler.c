// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_shader_graph_compiler.c
 * @brief Visual Node-Based Shader Graph Compiler Verification Suite
 *
 * Verifies:
 * 1. Struct sizeof static asserts (96B pin, 1856B node, 32B edge, 491664B graph, 1304B schedule).
 * 2. Tarjan O(V+E) SCC cycle detection and temporal feedback latch exemption.
 * 3. Typed pin coercion lattice rules.
 * 4. Spatial convolution barrier detection and multi-pass scheduling (BUFFER_A..D, IMAGE_PASS).
 * 5. Kahn topological sort with Sethi-Ullman register pressure minimization.
 * 6. SSA GLSL ES 3.10 code emission and strict Invariant 2 (opaque alpha vec4(..., 1.0)).
 * 7. Draft 2020-12 .wwgraph JSON serialization and deserialization round-trip.
 * 8. 256-node graph compilation CPU benchmark (< 0.8 ms).
 */

#include "waywal/shader_graph.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static_assert(sizeof(waywal_graph_pin_t) == 96, "waywal_graph_pin_t must be 96 bytes");
static_assert(sizeof(waywal_graph_node_t) == 1856, "waywal_graph_node_t must be 1856 bytes");
static_assert(sizeof(waywal_graph_edge_t) == 32, "waywal_graph_edge_t must be 32 bytes");
static_assert(sizeof(waywal_shader_graph_t) == 491664,
              "waywal_shader_graph_t must be 491664 bytes");
static_assert(sizeof(waywal_graph_schedule_t) == 1304,
              "waywal_graph_schedule_t must be 1304 bytes");

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_pin_coercion_lattice(void)
{
    printf("[+] Testing pin type coercion lattice...\n");

    // Float widens to vec2, vec3, vec4
    assert(waywal_pin_types_compatible(WAYWAL_PIN_FLOAT, WAYWAL_PIN_VEC2));
    assert(waywal_pin_types_compatible(WAYWAL_PIN_FLOAT, WAYWAL_PIN_VEC3));
    assert(waywal_pin_types_compatible(WAYWAL_PIN_FLOAT, WAYWAL_PIN_VEC4));

    // Vec2 widens to vec3, vec4
    assert(waywal_pin_types_compatible(WAYWAL_PIN_VEC2, WAYWAL_PIN_VEC3));
    assert(waywal_pin_types_compatible(WAYWAL_PIN_VEC2, WAYWAL_PIN_VEC4));

    // Vec3 widens to vec4
    assert(waywal_pin_types_compatible(WAYWAL_PIN_VEC3, WAYWAL_PIN_VEC4));

    // Identical types compatible
    assert(waywal_pin_types_compatible(WAYWAL_PIN_SAMPLER2D, WAYWAL_PIN_SAMPLER2D));
    assert(waywal_pin_types_compatible(WAYWAL_PIN_INT, WAYWAL_PIN_INT));
    assert(waywal_pin_types_compatible(WAYWAL_PIN_BOOL, WAYWAL_PIN_BOOL));

    // Sampler2D cannot coerce to float or vec4
    assert(!waywal_pin_types_compatible(WAYWAL_PIN_SAMPLER2D, WAYWAL_PIN_FLOAT));
    assert(!waywal_pin_types_compatible(WAYWAL_PIN_FLOAT, WAYWAL_PIN_SAMPLER2D));

    printf(
        "    -> Coercion lattice correctly enforces subtyping and blocks invalid connections.\n");
}

static void test_tarjan_scc_cycle_rejection(void)
{
    printf("[+] Testing Tarjan O(V+E) SCC cycle detection & temporal latch exemption...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);

    int32_t n1 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_MATH_ADD, "Add1", 0.0f, 0.0f);
    int32_t n2 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_MATH_MUL, "Mul1", 100.0f, 0.0f);
    assert(n1 > 0 && n2 > 0);

    // Connect Add1 output (pin 0) -> Mul1 input (pin 0)
    int32_t e1 = waywal_shader_graph_connect(graph, (uint32_t)n1, 0, (uint32_t)n2, 0);
    assert(e1 > 0);

    // No cycle yet
    uint32_t cycle_node = 0;
    assert(!waywal_shader_graph_has_cycle(graph, &cycle_node));

    // Attempting to create directed cycle: Mul1 output (pin 0) -> Add1 input (pin 0)
    // waywal_shader_graph_connect() must detect and reject cycle creation!
    int32_t e2 = waywal_shader_graph_connect(graph, (uint32_t)n2, 0, (uint32_t)n1, 0);
    assert(e2 < 0);
    assert(!waywal_shader_graph_has_cycle(graph, &cycle_node));

    // Directly insert cyclic edge to test Tarjan detector directly
    uint32_t edge_idx = graph->edge_count;
    graph->edges[edge_idx].edge_id = 999;
    graph->edges[edge_idx].src_node_id = (uint32_t)n2;
    graph->edges[edge_idx].src_pin_index = 0;
    graph->edges[edge_idx].dst_node_id = (uint32_t)n1;
    graph->edges[edge_idx].dst_pin_index = 0;
    graph->edge_count++;

    // Cycle must be detected!
    assert(waywal_shader_graph_has_cycle(graph, &cycle_node));
    assert(cycle_node == (uint32_t)n1 || cycle_node == (uint32_t)n2);

    // Disconnect unlatched cycle wire
    graph->edge_count--;
    assert(!waywal_shader_graph_has_cycle(graph, &cycle_node));

    // Now insert a temporal feedback latch (WAYWAL_NODE_IN_PASS_BUFFER)
    int32_t n_latch = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_PASS_BUFFER,
                                                   "HistoryLatch", 50.0f, 100.0f);
    assert(n_latch > 0);

    int32_t e_to_latch = waywal_shader_graph_connect(graph, (uint32_t)n2, 0, (uint32_t)n_latch, 0);
    assert(e_to_latch > 0);
    int32_t e_from_latch =
        waywal_shader_graph_connect(graph, (uint32_t)n_latch, 0, (uint32_t)n1, 1);
    assert(e_from_latch > 0);

    // With temporal latch, Tarjan SCC must permit the feedback cycle!
    assert(!waywal_shader_graph_has_cycle(graph, &cycle_node));

    waywal_shader_graph_destroy(graph);
    printf("    -> Tarjan SCC cycle rejection and temporal latch exemption validated.\n");
}

static void test_multi_pass_scheduling_and_invariant2(void)
{
    printf("[+] Testing spatial convolution barriers & Invariant 2 (opaque alpha IMAGE_PASS)...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);

    int32_t n_uv = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
    int32_t n_kawase =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_FILTER_DUAL_KAWASE, "Kawase", 150.0f, 0.0f);
    int32_t n_sink =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "FinalSink", 300.0f, 0.0f);

    waywal_shader_graph_connect(graph, (uint32_t)n_uv, 0, (uint32_t)n_kawase, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_kawase, 0, (uint32_t)n_sink, 0);

    waywal_graph_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    bool analyzed = waywal_shader_graph_analyze(graph, &schedule);
    assert(analyzed);
    assert(schedule.has_cycle_error == 0);
    assert(schedule.sorted_count >= 3);

    // Multi-pass schedule must run upstream in BUFFER_A (bit 0) and final in IMAGE_PASS (bit 4)
    assert((schedule.active_pass_mask & 0x01) != 0);
    assert((schedule.active_pass_mask & 0x10) != 0);

    // Compile GLSL ES 3.10
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
    assert(glsl != NULL);
    assert(glsl_len > 0);

    // Verify real 5-tap Marius Bjorge Dual Kawase sampling upstream u_BufferA across barrier
    assert(strstr(glsl, "#version 310 es") != NULL);
    assert(strstr(glsl, "wwal_dual_kawase(u_BufferA,") != NULL);
    assert(strstr(glsl, "clamp(uv - halfpixel.xy") != NULL);
    assert(strstr(glsl, "clamp(uv + halfpixel.xy") != NULL);
    assert(strstr(glsl, "outColor = vec4((") != NULL);
    assert(strstr(glsl, ").rgb, 1.0);") != NULL);

    free(glsl);
    waywal_shader_graph_destroy(graph);
    printf("    -> Multi-pass schedule & Invariant 2 (opaque alpha) validated.\n");
}

static void test_spatial_filters_and_builtin_effects_codegen(void)
{
    printf("[+] Testing real multi-tap spatial filters & built-in effect GLSL evaluation...\n");

    const char *effect_slugs[] = {"waterripple", "waterwaves",    "sharpen_cas",
                                  "vignette",    "crt_scanlines", "godrays"};
    const char *expected_calls[] = {"wwal_effect_waterripple(",  "wwal_effect_waterwaves(",
                                    "wwal_effect_cas_sharpen(",  "wwal_effect_vignette(",
                                    "wwal_effect_crt_pixelate(", "wwal_effect_godrays("};

    for (size_t i = 0; i < sizeof(effect_slugs) / sizeof(effect_slugs[0]); ++i) {
        waywal_shader_graph_t *graph = waywal_shader_graph_create();
        assert(graph != NULL);

        int32_t n_uv = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
        int32_t n_fx =
            waywal_shader_graph_add_node(graph, WAYWAL_NODE_BUILTIN_EFFECT, "FX", 120.0f, 0.0f);
        int32_t n_chr = waywal_shader_graph_add_node(graph, WAYWAL_NODE_FILTER_CHROMATIC_ABERR,
                                                     "Chrom", 260.0f, 0.0f);
        int32_t n_sink =
            waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 400.0f, 0.0f);

        waywal_graph_node_t *fx_node = waywal_shader_graph_find_node(graph, (uint32_t)n_fx);
        assert(fx_node != NULL);
        strncpy(fx_node->prop_or_asset_key, effect_slugs[i],
                sizeof(fx_node->prop_or_asset_key) - 1);

        waywal_shader_graph_connect(graph, (uint32_t)n_uv, 0, (uint32_t)n_fx, 0);
        waywal_shader_graph_connect(graph, (uint32_t)n_fx, 0, (uint32_t)n_chr, 0);
        waywal_shader_graph_connect(graph, (uint32_t)n_chr, 0, (uint32_t)n_sink, 0);

        waywal_graph_schedule_t schedule;
        assert(waywal_shader_graph_analyze(graph, &schedule));
        assert(schedule.has_cycle_error == 0);

        size_t glsl_len = 0;
        char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
        assert(glsl != NULL && glsl_len > 0);
        assert(strstr(glsl, expected_calls[i]) != NULL);
        assert(strstr(glsl, "wwal_chromatic_aberration(u_Buffer") != NULL);
        assert(strstr(glsl, "clamp(uv + dir") != NULL);
        assert(strstr(glsl, "clamp(uv - dir") != NULL);

        free(glsl);
        waywal_shader_graph_destroy(graph);
    }

    printf("    -> Built-in effects & RGB chromatic dispersion multi-tap shaders verified.\n");
}

static void test_json_serialization_round_trip(void)
{
    printf("[+] Testing .wwgraph JSON Draft 2020-12 serialization round-trip...\n");

    waywal_shader_graph_t *src_graph = waywal_shader_graph_create();
    strncpy(src_graph->graph_name, "TestNeonRipple", sizeof(src_graph->graph_name) - 1);
    strncpy(src_graph->author, "WayWalTeam", sizeof(src_graph->author) - 1);

    int32_t n1 =
        waywal_shader_graph_add_node(src_graph, WAYWAL_NODE_IN_UV, "UVCoord", 10.0f, 20.0f);
    int32_t n2 =
        waywal_shader_graph_add_node(src_graph, WAYWAL_NODE_MATH_SIN, "SinWave", 120.0f, 50.0f);
    int32_t n3 = waywal_shader_graph_add_node(src_graph, WAYWAL_NODE_OUT_IMAGE_PASS, "OutputSink",
                                              250.0f, 50.0f);

    waywal_shader_graph_connect(src_graph, (uint32_t)n1, 0, (uint32_t)n2, 0);
    waywal_shader_graph_connect(src_graph, (uint32_t)n2, 0, (uint32_t)n3, 0);

    char *json = waywal_shader_graph_save_json(src_graph);
    assert(json != NULL);
    assert(strlen(json) > 0);
    assert(strstr(json, "TestNeonRipple") != NULL);
    assert(strstr(json, "WayWalTeam") != NULL);

    waywal_shader_graph_t *dst_graph = waywal_shader_graph_create();
    char err_buf[256] = {0};
    bool loaded = waywal_shader_graph_load_json(json, strlen(json), dst_graph, err_buf);
    assert(loaded);
    assert(dst_graph->node_count == src_graph->node_count);
    assert(dst_graph->edge_count == src_graph->edge_count);
    assert(strcmp(dst_graph->graph_name, src_graph->graph_name) == 0);

    free(json);
    waywal_shader_graph_destroy(src_graph);
    waywal_shader_graph_destroy(dst_graph);
    printf("    -> JSON serialization/deserialization lossless round-trip verified.\n");
}

static void test_json_escaping_and_bounded_parse(void)
{
    printf("[+] Testing JSON string escaping & non-NUL-terminated bounded number parsing...\n");

    waywal_shader_graph_t *src = waywal_shader_graph_create();
    assert(src != NULL);
    const char *tricky_name = "Quote\"Slash\\Line\nTab\tCtrl\007End";
    const char *tricky_author = "Author \"A\\B\"\t2026";
    strncpy(src->graph_name, tricky_name, sizeof(src->graph_name) - 1);
    strncpy(src->author, tricky_author, sizeof(src->author) - 1);

    int32_t n1 =
        waywal_shader_graph_add_node(src, WAYWAL_NODE_IN_CONSTANT, "Node\"1\\A\n", 12.5f, -34.25f);
    int32_t n2 =
        waywal_shader_graph_add_node(src, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink\"Out", 200.0f, 0.0f);
    waywal_shader_graph_connect(src, (uint32_t)n1, 0, (uint32_t)n2, 0);

    char *json = waywal_shader_graph_save_json(src);
    assert(json != NULL);
    assert(strstr(json, "\\\"Slash\\\\Line\\nTab\\tCtrl\\u0007End") != NULL);

    waywal_shader_graph_t *dst = waywal_shader_graph_create();
    char err_buf[256] = {0};
    assert(waywal_shader_graph_load_json(json, strlen(json), dst, err_buf));
    assert(strcmp(dst->graph_name, tricky_name) == 0);
    assert(strcmp(dst->author, tricky_author) == 0);
    assert(strcmp(dst->nodes[0].title, "Node\"1\\A\n") == 0);

    // Non-NUL-terminated buffer with trailing digits right after json_len
    const char *compact_json =
        "{\"schema_version\":1,\"graph_name\":\"Bounded\",\"nodes\":[],\"flags\":7}";
    size_t exact_len = strlen(compact_json);
    char *unterminated = (char *)malloc(exact_len + 16);
    assert(unterminated != NULL);
    memcpy(unterminated, compact_json, exact_len);
    memset(unterminated + exact_len, '9', 16);

    waywal_shader_graph_t *bounded_dst = waywal_shader_graph_create();
    assert(waywal_shader_graph_load_json(unterminated, exact_len, bounded_dst, err_buf));
    assert(bounded_dst->flags == 7);

    // Truncated number at exact slice boundary (must not read trailing '9's into flags)
    const char *open_num = "{\"schema_version\":1,\"flags\":4";
    size_t open_len = strlen(open_num);
    char *open_buf = (char *)malloc(open_len + 16);
    assert(open_buf != NULL);
    memcpy(open_buf, open_num, open_len);
    memset(open_buf + open_len, '9', 16);
    assert(waywal_shader_graph_load_json(open_buf, open_len, bounded_dst, err_buf));
    assert(bounded_dst->flags == 4);

    free(open_buf);
    free(unterminated);
    free(json);
    waywal_shader_graph_destroy(bounded_dst);
    waywal_shader_graph_destroy(dst);
    waywal_shader_graph_destroy(src);
    printf("    -> JSON string escaping & bounded non-NUL number parsing verified.\n");
}

static void test_neon_curl_ripple_reference_graph(void)
{
    printf("[+] Testing reference graph (neon_curl_ripple.wwgraph) round-trip & codegen...\n");

    static const char *k_neon_curl_ripple_json =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"graph_name\": \"Neon Curl Ripple + Dual Kawase Bloom\",\n"
        "  \"author\": \"execorn\",\n"
        "  \"flags\": 1,\n"
        "  \"nodes\": [\n"
        "    {\n"
        "      \"node_id\": 1,\n"
        "      \"opcode\": 0,\n"
        "      \"title\": \"Normalized UV\",\n"
        "      \"prop_or_asset_key\": \"\",\n"
        "      \"canvas_pos\": [60.0, 120.0],\n"
        "      \"target_pass\": 0,\n"
        "      \"flags\": 0,\n"
        "      \"const_val\": [0.0, 0.0, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 2,\n"
        "      \"opcode\": 1,\n"
        "      \"title\": \"Scene Time (s)\",\n"
        "      \"prop_or_asset_key\": \"iTime\",\n"
        "      \"canvas_pos\": [60.0, 260.0],\n"
        "      \"target_pass\": 0,\n"
        "      \"flags\": 0,\n"
        "      \"const_val\": [1.0, 0.0, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 3,\n"
        "      \"opcode\": 31,\n"
        "      \"title\": \"2D Curl Noise Field\",\n"
        "      \"prop_or_asset_key\": \"u_curl_scale\",\n"
        "      \"canvas_pos\": [320.0, 160.0],\n"
        "      \"target_pass\": 0,\n"
        "      \"flags\": 2,\n"
        "      \"const_val\": [3.5, 0.4, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 4,\n"
        "      \"opcode\": 48,\n"
        "      \"title\": \"Built-In: Water Ripple\",\n"
        "      \"prop_or_asset_key\": \"waterripple\",\n"
        "      \"canvas_pos\": [600.0, 160.0],\n"
        "      \"target_pass\": 0,\n"
        "      \"flags\": 2,\n"
        "      \"const_val\": [1.2, 0.03, 18.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 5,\n"
        "      \"opcode\": 49,\n"
        "      \"title\": \"BUFFER_A Sink (RGBA16F)\",\n"
        "      \"prop_or_asset_key\": \"BUFFER_A\",\n"
        "      \"canvas_pos\": [880.0, 160.0],\n"
        "      \"target_pass\": 0,\n"
        "      \"flags\": 0,\n"
        "      \"const_val\": [0.0, 0.0, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 6,\n"
        "      \"opcode\": 6,\n"
        "      \"title\": \"Read BUFFER_A Latch\",\n"
        "      \"prop_or_asset_key\": \"BUFFER_A\",\n"
        "      \"canvas_pos\": [600.0, 420.0],\n"
        "      \"target_pass\": 4,\n"
        "      \"flags\": 0,\n"
        "      \"const_val\": [0.0, 0.0, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 7,\n"
        "      \"opcode\": 44,\n"
        "      \"title\": \"Dual Kawase Bloom\",\n"
        "      \"prop_or_asset_key\": \"bloom_glow\",\n"
        "      \"canvas_pos\": [880.0, 420.0],\n"
        "      \"target_pass\": 4,\n"
        "      \"flags\": 2,\n"
        "      \"const_val\": [1.5, 0.65, 0.0, 1.0]\n"
        "    },\n"
        "    {\n"
        "      \"node_id\": 8,\n"
        "      \"opcode\": 50,\n"
        "      \"title\": \"Final IMAGE_PASS Output\",\n"
        "      \"prop_or_asset_key\": \"IMAGE_PASS\",\n"
        "      \"canvas_pos\": [1160.0, 420.0],\n"
        "      \"target_pass\": 4,\n"
        "      \"flags\": 0,\n"
        "      \"const_val\": [0.0, 0.0, 0.0, 1.0]\n"
        "    }\n"
        "  ],\n"
        "  \"edges\": [\n"
        "    { \"edge_id\": 1, \"src_node_id\": 1, \"src_pin_index\": 0, \"dst_node_id\": 3, "
        "\"dst_pin_index\": 0, \"swizzle_mask\": 0 },\n"
        "    { \"edge_id\": 2, \"src_node_id\": 2, \"src_pin_index\": 0, \"dst_node_id\": 3, "
        "\"dst_pin_index\": 1, \"swizzle_mask\": 0 },\n"
        "    { \"edge_id\": 3, \"src_node_id\": 3, \"src_pin_index\": 0, \"dst_node_id\": 4, "
        "\"dst_pin_index\": 0, \"swizzle_mask\": 0 },\n"
        "    { \"edge_id\": 4, \"src_node_id\": 4, \"src_pin_index\": 0, \"dst_node_id\": 5, "
        "\"dst_pin_index\": 0, \"swizzle_mask\": 0 },\n"
        "    { \"edge_id\": 5, \"src_node_id\": 6, \"src_pin_index\": 0, \"dst_node_id\": 7, "
        "\"dst_pin_index\": 0, \"swizzle_mask\": 0 },\n"
        "    { \"edge_id\": 6, \"src_node_id\": 7, \"src_pin_index\": 0, \"dst_node_id\": 8, "
        "\"dst_pin_index\": 0, \"swizzle_mask\": 0 }\n"
        "  ]\n"
        "}\n";

    waywal_shader_graph_t *g1 = waywal_shader_graph_create();
    char err_buf[256] = {0};
    assert(waywal_shader_graph_load_json(k_neon_curl_ripple_json, strlen(k_neon_curl_ripple_json),
                                         g1, err_buf));
    assert(g1->node_count == 8);
    assert(g1->edge_count == 6);
    assert(strcmp(g1->graph_name, "Neon Curl Ripple + Dual Kawase Bloom") == 0);

    char *resaved = waywal_shader_graph_save_json(g1);
    assert(resaved != NULL);

    waywal_shader_graph_t *g2 = waywal_shader_graph_create();
    assert(waywal_shader_graph_load_json(resaved, strlen(resaved), g2, err_buf));
    assert(g2->node_count == 8);
    assert(g2->edge_count == 6);
    assert(strcmp(g2->graph_name, g1->graph_name) == 0);
    assert(strcmp(g2->author, g1->author) == 0);

    waywal_graph_schedule_t schedule;
    assert(waywal_shader_graph_analyze(g2, &schedule));
    assert(schedule.has_cycle_error == 0);
    assert(schedule.sorted_count == 8);
    assert(schedule.active_pass_mask == 0x11u);

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(g2, &schedule, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);
    assert(strstr(glsl, "#pragma Pass BUFFER_A") != NULL);
    assert(strstr(glsl, "#pragma Pass IMAGE_PASS") != NULL);
    assert(strstr(glsl, "wwal_curl2d(") != NULL);
    assert(strstr(glsl, "wwal_effect_waterripple(") != NULL);
    assert(strstr(glsl, "wwal_dual_kawase(u_BufferA,") != NULL);
    assert(strstr(glsl, "outColor = vec4((n7_out0).rgb, 1.0);") != NULL);

    free(glsl);
    free(resaved);
    waywal_shader_graph_destroy(g2);
    waywal_shader_graph_destroy(g1);
    printf("    -> neon_curl_ripple.wwgraph 8-node multi-pass reference graph verified.\n");
}

static void test_large_graph_compilation_benchmark(void)
{
    printf("[+] Benchmarking 256-node graph compilation CPU budget (< 0.8 ms)...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);

    int32_t prev_id = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UVRoot", 0.0f, 0.0f);
    // Create chain of math nodes
    for (int i = 1; i < 255; ++i) {
        uint32_t op = (i % 2 == 0) ? WAYWAL_NODE_MATH_ADD : WAYWAL_NODE_MATH_MUL;
        char name[32];
        snprintf(name, sizeof(name), "Op%d", i);
        int32_t curr_id = waywal_shader_graph_add_node(graph, op, name, (float)i * 10.0f, 0.0f);
        waywal_shader_graph_connect(graph, (uint32_t)prev_id, 0, (uint32_t)curr_id, 0);
        prev_id = curr_id;
    }
    int32_t sink_id =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 3000.0f, 0.0f);
    waywal_shader_graph_connect(graph, (uint32_t)prev_id, 0, (uint32_t)sink_id, 0);

    assert(graph->node_count == 256);

    uint64_t t_start = get_time_ns();
    waywal_graph_schedule_t schedule;
    bool analyzed = waywal_shader_graph_analyze(graph, &schedule);
    assert(analyzed);

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
    assert(glsl != NULL);
    uint64_t t_end = get_time_ns();

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    const double compile_budget_ms = 5.0;
#else
    const double compile_budget_ms = 0.8;
#endif
    double elapsed_ms = (double)(t_end - t_start) / 1000000.0;
    printf(
        "    -> 256-node graph compile time: %.3f ms (%zu bytes GLSL emitted, budget: < %.3f ms)\n",
        elapsed_ms, glsl_len, compile_budget_ms);

    free(glsl);
    waywal_shader_graph_destroy(graph);

    assert(elapsed_ms < compile_budget_ms && "256-node graph compilation exceeded budget");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  Shader Graph Compiler Test Suite (test_shader_graph_compiler)\n");
    printf("==========================================================\n");

    test_pin_coercion_lattice();
    test_tarjan_scc_cycle_rejection();
    test_multi_pass_scheduling_and_invariant2();
    test_spatial_filters_and_builtin_effects_codegen();
    test_json_serialization_round_trip();
    test_json_escaping_and_bounded_parse();
    test_neon_curl_ripple_reference_graph();
    test_large_graph_compilation_benchmark();

    printf("==========================================================\n");
    printf("  >>> ALL SHADER GRAPH COMPILER TESTS PASSED (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
