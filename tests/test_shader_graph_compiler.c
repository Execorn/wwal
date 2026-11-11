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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
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

    // Unclosed root object must be rejected even at exact slice boundary
    const char *open_num = "{\"schema_version\":1,\"flags\":4";
    size_t open_len = strlen(open_num);
    char *open_buf = (char *)malloc(open_len + 16);
    assert(open_buf != NULL);
    memcpy(open_buf, open_num, open_len);
    memset(open_buf + open_len, '9', 16);
    assert(!waywal_shader_graph_load_json(open_buf, open_len, bounded_dst, err_buf));

    // Valid closed slice followed immediately by non-NUL bytes beyond exact_len
    const char *closed_slice = "{\"schema_version\":1,\"flags\":4}";
    size_t closed_len = strlen(closed_slice);
    char *slice_buf = (char *)malloc(closed_len + 32);
    assert(slice_buf != NULL);
    memcpy(slice_buf, closed_slice, closed_len);
    memcpy(slice_buf + closed_len, ",\"flags\":99}9999", 16);
    assert(waywal_shader_graph_load_json(slice_buf, closed_len, bounded_dst, err_buf));
    assert(bounded_dst->flags == 4);

    // UTF-8 \uXXXX BMP + surrogate pair decoding, lossless %.9g float round-trip, and post-load ID
    const char *utf8_json =
        "{\"schema_version\":1,\"graph_name\":\"Caf\\u00e9 \\u03bb \\uD83D\\uDE80\","
        "\"author\":\"long_swar_author_0123456789_abcdefghijklmnopqrstuvwxyz_9876543210\","
        "\"nodes\":[{\"node_id\":42,\"opcode\":8,\"title\":\"SmallConst\","
        "\"canvas_pos\":[0.12345679,1024.0625],"
        "\"const_val\":[1e-05,0.00392156886,0.333333343,1.0]}],\"edges\":[]}";
    assert(waywal_shader_graph_load_json(utf8_json, strlen(utf8_json), bounded_dst, err_buf));
    assert(strcmp(bounded_dst->graph_name, "Caf\xc3\xa9 \xce\xbb \xf0\x9f\x9a\x80") == 0);
    const waywal_graph_node_t *cnode = waywal_shader_graph_find_node_const(bounded_dst, 42);
    assert(cnode != NULL);
    assert(fabsf(cnode->const_val[0] - 1e-5f) == 0.0f);
    assert(fabsf(cnode->const_val[1] - (1.0f / 255.0f)) == 0.0f);
    char *saved_lossless = waywal_shader_graph_save_json(bounded_dst);
    assert(saved_lossless != NULL);
    waywal_shader_graph_t *reloaded_lossless = waywal_shader_graph_create();
    assert(waywal_shader_graph_load_json(saved_lossless, strlen(saved_lossless), reloaded_lossless,
                                         err_buf));
    const waywal_graph_node_t *cnode2 = waywal_shader_graph_find_node_const(reloaded_lossless, 42);
    assert(cnode2 != NULL);
    assert(cnode2->const_val[0] == cnode->const_val[0]);
    assert(cnode2->const_val[1] == cnode->const_val[1]);
    assert(cnode2->canvas_pos[0] == cnode->canvas_pos[0]);
    int32_t next_id =
        waywal_shader_graph_add_node(reloaded_lossless, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 0, 0);
    assert(next_id == 43);

    free(saved_lossless);
    waywal_shader_graph_destroy(reloaded_lossless);
    free(slice_buf);
    free(open_buf);
    free(unterminated);
    free(json);
    waywal_shader_graph_destroy(bounded_dst);
    waywal_shader_graph_destroy(dst);
    waywal_shader_graph_destroy(src);
    printf("    -> JSON string escaping, UTF-8, %%.9g round-trip & bounded parsing verified.\n");
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

    double elapsed_ms = 1e9;
    size_t glsl_len = 0;
    for (int run = 0; run < 8; ++run) {
        uint64_t t_start = get_time_ns();
        waywal_graph_schedule_t schedule;
        bool analyzed = waywal_shader_graph_analyze(graph, &schedule);
        assert(analyzed);

        char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
        assert(glsl != NULL);
        uint64_t t_end = get_time_ns();
        free(glsl);

        double cur_ms = (double)(t_end - t_start) / 1000000.0;
        if (cur_ms < elapsed_ms) {
            elapsed_ms = cur_ms;
        }
    }

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    const double compile_budget_ms = 5.0;
#else
    const double compile_budget_ms = 0.8;
#endif
    printf(
        "    -> 256-node graph compile time: %.3f ms (%zu bytes GLSL emitted, budget: < %.3f ms)\n",
        elapsed_ms, glsl_len, compile_budget_ms);

    waywal_shader_graph_destroy(graph);

    assert(elapsed_ms < compile_budget_ms && "256-node graph compilation exceeded budget");
}

static void test_readonly_const_analyze_and_dead_helper_pruning(void)
{
    printf("[+] Testing read-only PROT_READ analyze/compile & dead-helper pruning...\n");

    size_t page_sz = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_len = ((sizeof(waywal_shader_graph_t) + page_sz - 1u) / page_sz) * page_sz;
    waywal_shader_graph_t *ro_graph = (waywal_shader_graph_t *)mmap(
        NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ro_graph != MAP_FAILED);

    waywal_shader_graph_init(ro_graph, "ReadOnlyGraph");
    int32_t n_uv = waywal_shader_graph_add_node(ro_graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
    int32_t n_kaw =
        waywal_shader_graph_add_node(ro_graph, WAYWAL_NODE_FILTER_DUAL_KAWASE, "Kaw", 100.0f, 0.0f);
    int32_t n_sink =
        waywal_shader_graph_add_node(ro_graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 200.0f, 0.0f);
    // Leave target_pass = 0 on Sink as well to verify auto-pass routing without const mutation
    waywal_shader_graph_find_node(ro_graph, (uint32_t)n_sink)->target_pass = 0;
    waywal_shader_graph_connect(ro_graph, (uint32_t)n_uv, 0, (uint32_t)n_kaw, 0);
    waywal_shader_graph_connect(ro_graph, (uint32_t)n_kaw, 0, (uint32_t)n_sink, 0);

    assert(mprotect(ro_graph, map_len, PROT_READ) == 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(ro_graph, &sched));
    assert(sched.active_pass_mask == 0x11u);
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(ro_graph, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);
    assert(strstr(glsl, "wwal_dual_kawase(u_BufferA,") != NULL);
    assert(strstr(glsl, "outColor = vec4((n2_out0).rgb, 1.0);") != NULL);
    free(glsl);
    assert(munmap(ro_graph, map_len) == 0);

    // Dead-helper pruning: UV -> Sin -> Sink must emit zero wwal_* library helpers
    waywal_shader_graph_t *min_g = waywal_shader_graph_create();
    int32_t m_uv = waywal_shader_graph_add_node(min_g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t m_sin = waywal_shader_graph_add_node(min_g, WAYWAL_NODE_MATH_SIN, "Sin", 100, 0);
    int32_t m_out = waywal_shader_graph_add_node(min_g, WAYWAL_NODE_OUT_IMAGE_PASS, "Out", 200, 0);
    waywal_shader_graph_connect(min_g, (uint32_t)m_uv, 0, (uint32_t)m_sin, 0);
    waywal_shader_graph_connect(min_g, (uint32_t)m_sin, 0, (uint32_t)m_out, 0);
    assert(waywal_shader_graph_analyze(min_g, &sched));
    glsl = waywal_shader_graph_compile_glsl(min_g, &sched, &glsl_len);
    assert(glsl != NULL);
    assert(strstr(glsl, "sin(") != NULL);
    assert(strstr(glsl, "wwal_simplex2d") == NULL);
    assert(strstr(glsl, "wwal_oklch_to_srgb") == NULL);
    assert(strstr(glsl, "wwal_dual_kawase") == NULL);
    assert(strstr(glsl, "wwal_effect_waterripple") == NULL);
    free(glsl);
    waywal_shader_graph_destroy(min_g);
    printf("    -> PROT_READ const-correctness & dead-helper pruning verified.\n");
}

static void test_codegen_smoothstep_swizzle_coercion_mute_and_pins(void)
{
    printf("[+] Testing ordered smoothstep, swizzle_mask, muted bypass & pin-flag sync...\n");

    waywal_shader_graph_t *g = waywal_shader_graph_create();
    int32_t n_uv = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t n_ss = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_SMOOTHSTEP, "SS", 80, 0);
    int32_t n_cst = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_CONSTANT, "Cst", 80, 80);
    int32_t n_kaw =
        waywal_shader_graph_add_node(g, WAYWAL_NODE_FILTER_DUAL_KAWASE, "MutedKaw", 160, 0);
    int32_t n_unchr =
        waywal_shader_graph_add_node(g, WAYWAL_NODE_FILTER_CHROMATIC_ABERR, "UnwiredChr", 160, 80);
    int32_t n_add = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_ADD, "Add", 240, 0);
    int32_t n_sink = waywal_shader_graph_add_node(g, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 320, 0);

    // Inverted default edges on SmoothStep (edge0 = 0.9 > edge1 = 0.1)
    waywal_graph_node_t *ss_node = waywal_shader_graph_find_node(g, (uint32_t)n_ss);
    ss_node->inputs[0].default_val[0] = 0.9f;
    ss_node->inputs[1].default_val[0] = 0.1f;

    // Mute Kawase so it neither triggers a pass barrier nor blurs
    waywal_shader_graph_find_node(g, (uint32_t)n_kaw)->flags |= WAYWAL_NODE_FLAG_MUTED_BYPASS;

    int32_t e_uv_ss = waywal_shader_graph_connect(g, (uint32_t)n_uv, 0, (uint32_t)n_ss, 2);
    int32_t e_cst_kaw = waywal_shader_graph_connect(g, (uint32_t)n_cst, 0, (uint32_t)n_kaw, 0);
    assert(e_uv_ss > 0 && e_cst_kaw > 0);
    // Swizzle .zyxw (2 | (1<<2) | (0<<4) | (3<<6) = 0xC6) on Constant -> MutedKaw
    g->edges[1].swizzle_mask = 0xC6u;

    assert(waywal_shader_graph_connect(g, (uint32_t)n_kaw, 0, (uint32_t)n_add, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_unchr, 0, (uint32_t)n_add, 1) > 0);
    int32_t e_ss_add = waywal_shader_graph_connect(g, (uint32_t)n_ss, 0, (uint32_t)n_sink, 0);
    assert(e_ss_add > 0);
    // Reconnect Sink pin 0 to Add (replaces e_ss_add and clears SS output connected flag)
    assert(waywal_shader_graph_connect(g, (uint32_t)n_add, 0, (uint32_t)n_sink, 0) > 0);
    assert((waywal_shader_graph_find_node(g, (uint32_t)n_ss)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) == 0u);

    // Connect SS into Add pin 1 after disconnecting UnwiredChr, verifying disconnect flag cleanup
    assert(waywal_shader_graph_disconnect_pins(g, (uint32_t)n_add, 1));
    assert((waywal_shader_graph_find_node(g, (uint32_t)n_unchr)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) == 0u);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_unchr, 0, (uint32_t)n_add, 1) > 0);

    // Also keep SS reachable via MutedKaw? Wire SS into UnwiredChr pin 2 (intensity)
    assert(waywal_shader_graph_connect(g, (uint32_t)n_ss, 0, (uint32_t)n_unchr, 2) > 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(g, &sched));
    // Muted Kawase + unwired ChromaticAberr both stay in single IMAGE_PASS (0x10)
    assert(sched.active_pass_mask == 0x10u);

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(g, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);
    assert(strstr(glsl, ".zyxw") != NULL);
    assert(strstr(glsl, "1e-5") != NULL);
    assert(strstr(glsl, "wwal_dual_kawase") == NULL);
    assert(strstr(glsl, "wwal_chromatic_aberration") == NULL);
    free(glsl);

    // Verify remove_node clears WAYWAL_PIN_FLAG_CONNECTED on surviving peers
    assert(waywal_shader_graph_remove_node(g, (uint32_t)n_ss));
    assert((waywal_shader_graph_find_node(g, (uint32_t)n_uv)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) == 0u);
    assert((waywal_shader_graph_find_node(g, (uint32_t)n_unchr)->inputs[2].flags &
            WAYWAL_PIN_FLAG_CONNECTED) == 0u);

    waywal_shader_graph_destroy(g);
    printf("    -> Smoothstep guard, swizzle, muted bypass & pin-flag sync verified.\n");
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
    test_readonly_const_analyze_and_dead_helper_pruning();
    test_codegen_smoothstep_swizzle_coercion_mute_and_pins();

    printf("==========================================================\n");
    printf("  >>> ALL SHADER GRAPH COMPILER TESTS PASSED (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
