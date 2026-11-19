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
#include "waywal/bezier.h"
#include "waywal/shader_parser.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
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

static void test_connect_cycle_preserves_prior_edge(void)
{
    printf("[+] Testing AUD-047 cycle-preserving connect() & would_create_cycle()...\n");

    waywal_shader_graph_t *g = waywal_shader_graph_create();
    assert(g != NULL);

    int32_t n1 = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t n2 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_SIN, "Sin", 100, 0);
    int32_t n3 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_ADD, "Add", 200, 0);
    int32_t n4 = waywal_shader_graph_add_node(g, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 300, 0);
    int32_t n_latch =
        waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_PASS_BUFFER, "Latch", 200, 120);

    int32_t e_keep = waywal_shader_graph_connect(g, (uint32_t)n1, 0, (uint32_t)n2, 0);
    int32_t e23 = waywal_shader_graph_connect(g, (uint32_t)n2, 0, (uint32_t)n3, 0);
    int32_t e34 = waywal_shader_graph_connect(g, (uint32_t)n3, 0, (uint32_t)n4, 0);
    int32_t e3l = waywal_shader_graph_connect(g, (uint32_t)n3, 0, (uint32_t)n_latch, 0);
    assert(e_keep > 0 && e23 > 0 && e34 > 0 && e3l > 0);

    assert(waywal_shader_graph_would_create_cycle(g, (uint32_t)n3, (uint32_t)n2));
    assert(waywal_shader_graph_would_create_cycle(g, (uint32_t)n1, (uint32_t)n1));
    assert(!waywal_shader_graph_would_create_cycle(g, (uint32_t)n_latch, (uint32_t)n2));

    uint32_t edge_cnt_before = g->edge_count;
    int32_t bad_e = waywal_shader_graph_connect(g, (uint32_t)n3, 0, (uint32_t)n2, 0);
    assert(bad_e < 0);
    assert(g->edge_count == edge_cnt_before);

    bool found_kept = false;
    for (uint32_t i = 0; i < g->edge_count; ++i) {
        if (g->edges[i].edge_id == (uint32_t)e_keep) {
            assert(g->edges[i].src_node_id == (uint32_t)n1);
            assert(g->edges[i].dst_node_id == (uint32_t)n2);
            assert(g->edges[i].dst_pin_index == 0u);
            found_kept = true;
        }
    }
    assert(found_kept);
    assert((waywal_shader_graph_find_node(g, (uint32_t)n1)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) != 0u);
    assert((waywal_shader_graph_find_node(g, (uint32_t)n2)->inputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) != 0u);

    waywal_shader_graph_destroy(g);
    printf("    -> Cyclic connect() safely preserves existing incoming wire & pin flags.\n");
}

static void test_json_deep_alternating_skipper(void)
{
    printf(
        "[+] Testing AUD-241 bounded iterative JSON skipper on deep alternating containers...\n");

    waywal_shader_graph_t *dst = waywal_shader_graph_create();
    assert(dst != NULL);
    char err_buf[256] = {0};

    /* 32 alternating levels of {[{"k":...}]} should skip cleanly */
    size_t valid_depth = 32u;
    size_t valid_cap = valid_depth * 8u + 128u;
    char *valid_json = (char *)malloc(valid_cap);
    assert(valid_json != NULL);
    size_t pos = 0;
    pos += (size_t)snprintf(valid_json + pos, valid_cap - pos, "{\"schema_version\":1,\"extra\":");
    for (size_t d = 0; d < valid_depth; ++d) {
        memcpy(valid_json + pos, "{\"a\":[", 6u);
        pos += 6u;
    }
    memcpy(valid_json + pos, "123", 3u);
    pos += 3u;
    for (size_t d = 0; d < valid_depth; ++d) {
        memcpy(valid_json + pos, "]}", 2u);
        pos += 2u;
    }
    pos += (size_t)snprintf(valid_json + pos, valid_cap - pos,
                            ",\"nodes\":[{\"node_id\":1,\"opcode\":0}],\"edges\":[]}");
    assert(waywal_shader_graph_load_json(valid_json, pos, dst, err_buf));
    assert(dst->node_count == 1u);
    free(valid_json);

    /* 8192 alternating levels must fail cleanly without stack overflow */
    size_t dos_depth = 8192u;
    size_t dos_cap = dos_depth * 8u + 128u;
    char *dos_json = (char *)malloc(dos_cap);
    assert(dos_json != NULL);
    pos = 0;
    pos += (size_t)snprintf(dos_json + pos, dos_cap - pos, "{\"schema_version\":1,\"dos\":");
    for (size_t d = 0; d < dos_depth; ++d) {
        memcpy(dos_json + pos, "{\"a\":[", 6u);
        pos += 6u;
    }
    memcpy(dos_json + pos, "0", 1u);
    pos += 1u;
    for (size_t d = 0; d < dos_depth; ++d) {
        memcpy(dos_json + pos, "]}", 2u);
        pos += 2u;
    }
    memcpy(dos_json + pos, "}", 2u);
    pos += 1u;
    assert(!waywal_shader_graph_load_json(dos_json, pos, dst, err_buf));
    free(dos_json);

    /* Mismatched closing delimiter inside skipped value must fail */
    const char *mismatched = "{\"schema_version\":1,\"bad\":[1, 2}}";
    assert(!waywal_shader_graph_load_json(mismatched, strlen(mismatched), dst, err_buf));

    waywal_shader_graph_destroy(dst);
    printf("    -> Deep alternating JSON skipper bounded & stack-safe.\n");
}

static void test_safe_pow_oklab_iec_and_pin_sync(void)
{
    printf("[+] Testing AUD-383 wwal_safe_pow, IEC 61966-2-1 OKLab & O(V+E) pin sync...\n");

    waywal_shader_graph_t *g = waywal_shader_graph_create();
    assert(g != NULL);

    int32_t n_uv = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t n_pow = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_POW, "Pow", 100, 0);
    int32_t n_ok = waywal_shader_graph_add_node(g, WAYWAL_NODE_COLOR_SRGB_TO_OKLCH, "ToOK", 200, 0);
    int32_t n_srgb =
        waywal_shader_graph_add_node(g, WAYWAL_NODE_COLOR_OKLCH_TO_SRGB, "ToSRGB", 300, 0);
    int32_t n_hue = waywal_shader_graph_add_node(g, WAYWAL_NODE_COLOR_HUE_SHIFT, "Hue", 400, 0);
    int32_t n_sink = waywal_shader_graph_add_node(g, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 500, 0);

    waywal_shader_graph_find_node(g, (uint32_t)n_pow)->inputs[1].default_val[0] = 0.0f;

    assert(waywal_shader_graph_connect(g, (uint32_t)n_uv, 0, (uint32_t)n_pow, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_pow, 0, (uint32_t)n_ok, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_ok, 0, (uint32_t)n_srgb, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_srgb, 0, (uint32_t)n_hue, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_hue, 0, (uint32_t)n_sink, 0) > 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(g, &sched));
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(g, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);

    assert(strstr(glsl, "vec4 wwal_safe_pow(") != NULL);
    assert(strstr(glsl, "_out0 = wwal_safe_pow(") != NULL);
    assert(strstr(glsl, "0.0001") == NULL);
    assert(strstr(glsl, "wwal_srgb_to_linear(") != NULL);
    assert(strstr(glsl, "0.04045") != NULL);
    assert(strstr(glsl, "12.92") != NULL);
    assert(strstr(glsl, "2.4") != NULL);
    assert(strstr(glsl, "wwal_linear_to_srgb(") != NULL);
    assert(strstr(glsl, "0.0031308") != NULL);
    assert(strstr(glsl, "abs(a) < 1e-6 && abs(b) < 1e-6") != NULL);
    free(glsl);
    waywal_shader_graph_destroy(g);

    /* Sparse node IDs (> 1024) pin flag sync test */
    const char *sparse_json =
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":5000,\"opcode\":0},{\"node_id\":6000,"
        "\"opcode\":50}],\"edges\":[{\"edge_id\":1,\"src_node_id\":5000,\"src_pin_index\":0,"
        "\"dst_node_id\":6000,\"dst_pin_index\":0}]}";
    waywal_shader_graph_t *sg = waywal_shader_graph_create();
    char err_buf[256] = {0};
    assert(waywal_shader_graph_load_json(sparse_json, strlen(sparse_json), sg, err_buf));
    assert((waywal_shader_graph_find_node(sg, 5000)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) != 0u);
    assert((waywal_shader_graph_find_node(sg, 6000)->inputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) !=
           0u);
    assert(waywal_shader_graph_disconnect(sg, 1));
    assert((waywal_shader_graph_find_node(sg, 5000)->outputs[0].flags &
            WAYWAL_PIN_FLAG_CONNECTED) == 0u);
    waywal_shader_graph_destroy(sg);

    printf("    -> wwal_safe_pow, IEC 61966-2-1 OKLab & O(V+E) pin sync verified.\n");
}

static bool is_valid_utf8_cstr(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0u) {
        unsigned char c = *p;
        if (c <= 0x7Fu) {
            p++;
        } else if ((c & 0xE0u) == 0xC0u) {
            if (c < 0xC2u || (p[1] & 0xC0u) != 0x80u)
                return false;
            p += 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            if ((p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u)
                return false;
            p += 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            if (c > 0xF4u || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u ||
                (p[3] & 0xC0u) != 0x80u)
                return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

static void test_utf8_truncation_safety(void)
{
    printf("[+] Testing AUD-441 UTF-8 boundary truncation in SWAR JSON & node titles...\n");

    waywal_shader_graph_t *dst = waywal_shader_graph_create();
    char err_buf[256] = {0};

    /* 62 ASCII bytes + 3-byte Euro sign (65 total > 63 byte payload cap of graph_name[64]) */
    char json_buf[512];
    char pad62[63];
    memset(pad62, 'A', 62u);
    pad62[62] = '\0';

    snprintf(json_buf, sizeof(json_buf), "{\"schema_version\":1,\"graph_name\":\"%s\xe2\x82\xac\"}",
             pad62);
    assert(waywal_shader_graph_load_json(json_buf, strlen(json_buf), dst, err_buf));
    assert(strlen(dst->graph_name) == 62u);
    assert(is_valid_utf8_cstr(dst->graph_name));

    /* 61 ASCII bytes + 4-byte rocket emoji (65 total > 63 cap, SWAR lands inside emoji) */
    pad62[61] = '\0';
    snprintf(json_buf, sizeof(json_buf),
             "{\"schema_version\":1,\"graph_name\":\"%s\xf0\x9f\x9a\x80\"}", pad62);
    assert(waywal_shader_graph_load_json(json_buf, strlen(json_buf), dst, err_buf));
    assert(strlen(dst->graph_name) == 61u);
    assert(is_valid_utf8_cstr(dst->graph_name));

    /* 60 ASCII bytes + 3-byte Euro sign = 63 bytes (fits completely) */
    pad62[60] = '\0';
    snprintf(json_buf, sizeof(json_buf), "{\"schema_version\":1,\"graph_name\":\"%s\xe2\x82\xac\"}",
             pad62);
    assert(waywal_shader_graph_load_json(json_buf, strlen(json_buf), dst, err_buf));
    assert(strlen(dst->graph_name) == 63u);
    assert(is_valid_utf8_cstr(dst->graph_name));

    /* Escape truncation: 61 ASCII bytes + \uD83D\uDE80 + tail must stop at 61 */
    memset(pad62, 'A', 61u);
    pad62[61] = '\0';
    snprintf(json_buf, sizeof(json_buf),
             "{\"schema_version\":1,\"graph_name\":\"%s\\uD83D\\uDE80tail\"}", pad62);
    assert(waywal_shader_graph_load_json(json_buf, strlen(json_buf), dst, err_buf));
    assert(strlen(dst->graph_name) == 61u);
    assert(is_valid_utf8_cstr(dst->graph_name));

    /* Direct add_node title truncation */
    char long_title[80];
    memset(long_title, 'B', 62u);
    memcpy(long_title + 62u, "\xf0\x9f\x9a\x80", 5u);
    int32_t nid = waywal_shader_graph_add_node(dst, WAYWAL_NODE_IN_UV, long_title, 0.0f, 0.0f);
    assert(nid > 0);
    const waywal_graph_node_t *n = waywal_shader_graph_find_node_const(dst, (uint32_t)nid);
    assert(n != NULL && strlen(n->title) == 62u && is_valid_utf8_cstr(n->title));

    waywal_shader_graph_destroy(dst);
    printf("    -> UTF-8 boundary alignment verified across SWAR, escapes & node titles.\n");
}

static void test_shader_parser_nonfinite_guards(void)
{
    printf("[+] Testing AUD-442 shader_parser non-finite float guards & bounded slices...\n");

    const char *shader_src = "// @scale: NaN\n"
                             "// @uniform float u_nan = NaN; [0.0, 1.0]\n"
                             "// @uniform float u_inf = inf; [-inf, inf, nan]\n"
                             "// @uniform float u_ovf = 1e50; [0.0, 1e50]\n"
                             "// @uniform bool  u_bool = NaN;\n"
                             "// @uniform vec3  u_vec = vec3(1.0, NaN, 0.5);\n"
                             "// [UNIFORM]\n"
                             "// type: float\n"
                             "// name: u_blk\n"
                             "// default: inf\n"
                             "// min: nan\n"
                             "// max: 1e60\n"
                             "// step: nan\n"
                             "void mainImage(out vec4 fragColor, in vec2 fragCoord) {}\n";

    size_t slen = strlen(shader_src);
    char *unterminated = (char *)malloc(slen);
    assert(unterminated != NULL);
    memcpy(unterminated, shader_src, slen);

    waywal_shader_meta_t meta;
    assert(waywal_shader_parse_meta(unterminated, slen, &meta));
    free(unterminated);

    assert(isfinite(meta.global_scale) && meta.global_scale == 1.0f);
    assert(meta.uniform_count == 6u);
    for (uint32_t i = 0; i < meta.uniform_count; ++i) {
        const waywal_shader_uniform_def_t *u = &meta.uniforms[i];
        assert(isfinite(u->step) && u->step >= 0.0f);
        for (uint32_t c = 0; c < 4u; ++c) {
            assert(isfinite(u->default_val[c]));
            assert(isfinite(u->min_val[c]));
            assert(isfinite(u->max_val[c]));
            assert(u->min_val[c] <= u->max_val[c]);
        }
    }
    const waywal_shader_uniform_def_t *ub = waywal_shader_find_uniform(&meta, "u_bool");
    assert(ub != NULL && ub->default_val[0] == 0.0f);

    printf("    -> Non-finite shader uniform/scale values safely sanitized.\n");
}

static float eval_cubic_x(const bezier_curve_t *c, float t)
{
    double u = 1.0 - (double)t;
    double tt = (double)t;
    return (float)(3.0 * u * u * tt * (double)c->p1x + 3.0 * u * tt * tt * (double)c->p2x +
                   tt * tt * tt);
}

static void test_bezier_safeguarded_solver(void)
{
    printf("[+] Testing AUD-233/234/437 safeguarded double-precision cubic Bezier...\n");

    /* AUD-233: BEZIER_LINEAR (0,0,1,1) has x(t) = 3t^2 - 2t^3 != t, NOT diagonal shortcut */
    float t_lin25 = waywal_cubic_bezier_solve_t(&BEZIER_LINEAR, 0.25f);
    float x_lin25 = eval_cubic_x(&BEZIER_LINEAR, t_lin25);
    assert(fabsf(x_lin25 - 0.25f) < 1e-6f);
    assert(fabsf(t_lin25 - 0.25f) > 0.05f);
    assert(fabsf(bezier_eval(&BEZIER_LINEAR, 0.25f) - 0.25f) < 1e-6f);

    bezier_curve_t c_zero = bezier_create(0.0f, 0.0f, 0.0f, 0.0f);
    float t_z = waywal_cubic_bezier_solve_t(&c_zero, 0.125f);
    assert(fabsf(t_z - 0.5f) < 1e-6f);
    assert(fabsf(bezier_eval(&c_zero, 0.125f) - 0.125f) < 1e-6f);

    /* AUD-234: Flat derivative near origin (p1x=0, p2x=0) at tiny progress */
    bezier_curve_t c_flat = bezier_create(0.0f, 0.5f, 0.0f, 0.5f);
    const float tiny_ps[] = {5e-6f, 1e-5f, 1e-4f, 0.001f, 0.25f, 0.5f, 0.999f};
    for (size_t i = 0; i < sizeof(tiny_ps) / sizeof(tiny_ps[0]); ++i) {
        float p = tiny_ps[i];
        float t = waywal_cubic_bezier_solve_t(&c_flat, p);
        float x = eval_cubic_x(&c_flat, t);
        assert(t >= 0.0f && t <= 1.0f);
        assert(fabsf(x - p) <= 1e-6f);
    }

    /* Steep S-curve monotonicity and sub-1e-6 residual */
    bezier_curve_t c_steep = bezier_create(0.95f, 0.0f, 0.05f, 1.0f);
    float prev_t = 0.0f;
    for (int i = 1; i < 100; ++i) {
        float p = (float)i * 0.01f;
        float t = waywal_cubic_bezier_solve_t(&c_steep, p);
        float x = eval_cubic_x(&c_steep, t);
        assert(t >= prev_t && t <= 1.0f);
        assert(fabsf(x - p) <= 1e-6f);
        prev_t = t;
    }

    /* AUD-437: NaN / Inf guards before clamping */
    bezier_curve_t c_nan = bezier_create(NAN, NAN, INFINITY, -INFINITY);
    assert(isfinite(c_nan.p1x) && isfinite(c_nan.p1y) && isfinite(c_nan.p2x) &&
           isfinite(c_nan.p2y));
    assert(waywal_cubic_bezier_solve_t(&BEZIER_DEFAULT, NAN) == 0.0f);
    assert(waywal_cubic_bezier_solve_t(&BEZIER_DEFAULT, INFINITY) == 0.0f);
    assert(waywal_cubic_bezier_solve_t(&BEZIER_DEFAULT, -INFINITY) == 0.0f);
    assert(bezier_eval(&BEZIER_DEFAULT, NAN) == 0.0f);
    bezier_curve_t raw_bad = {.p1x = NAN, .p1y = NAN, .p2x = 0.5f, .p2y = INFINITY};
    assert(waywal_cubic_bezier_solve_t(&raw_bad, 0.5f) == 0.0f);
    assert(bezier_eval(&raw_bad, 0.5f) == 0.0f);

    printf("    -> Safeguarded Newton-Halley-Bisection & NaN guards verified.\n");
}

static void test_ssa_gvn_deduplication(void)
{
    printf("[+] Testing PERF-039 hash-consed SSA GVN deduplication...\n");

    waywal_shader_graph_t *g = waywal_shader_graph_create();
    int32_t n_uv = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t n_sin1 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_SIN, "Sin1", 100, 0);
    int32_t n_sin2 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_SIN, "Sin2", 100, 80);
    int32_t n_add = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_ADD, "Add", 200, 0);
    int32_t n_c1 = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_CONSTANT, "C1", 200, 120);
    int32_t n_c2 = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_CONSTANT, "C2", 200, 180);
    int32_t n_add2 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_ADD, "Add2", 300, 0);
    int32_t n_add3 = waywal_shader_graph_add_node(g, WAYWAL_NODE_MATH_ADD, "Add3", 380, 0);
    int32_t n_sink = waywal_shader_graph_add_node(g, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 460, 0);

    assert(waywal_shader_graph_connect(g, (uint32_t)n_uv, 0, (uint32_t)n_sin1, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_uv, 0, (uint32_t)n_sin2, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_sin1, 0, (uint32_t)n_add, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_sin2, 0, (uint32_t)n_add, 1) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_add, 0, (uint32_t)n_add2, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_c1, 0, (uint32_t)n_add2, 1) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_add2, 0, (uint32_t)n_add3, 0) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_c2, 0, (uint32_t)n_add3, 1) > 0);
    assert(waywal_shader_graph_connect(g, (uint32_t)n_add3, 0, (uint32_t)n_sink, 0) > 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(g, &sched));
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(g, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);

    const char *first_sin = strstr(glsl, " = sin(");
    assert(first_sin != NULL);
    assert(strstr(first_sin + 7, " = sin(") == NULL);
    assert(strstr(glsl, "u_Node5_Const") != NULL);
    assert(strstr(glsl, "u_Node6_Const") != NULL);

    free(glsl);
    waywal_shader_graph_destroy(g);
    printf("    -> Duplicate pure SSA Sin(UV) deduplicated while constants stayed distinct.\n");
}

typedef struct {
    const waywal_shader_graph_t *graph;
    const waywal_graph_schedule_t *schedule;
    pthread_barrier_t *barrier;
    uint32_t iters;
    double *latencies_ms;
} bench_16t_worker_t;

typedef struct {
    pthread_barrier_t *barrier;
    _Atomic bool stop;
} malloc_churn_ctx_t;

static uint64_t get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_double_asc(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void *malloc_churn_thread(void *arg)
{
    malloc_churn_ctx_t *ctx = (malloc_churn_ctx_t *)arg;
    pthread_barrier_wait(ctx->barrier);
    void *ring[32] = {0};
    uint32_t idx = 0;
    uint32_t seed = 0x9e3779b9u;
    uint32_t ops = 0;
    while (!atomic_load_explicit(&ctx->stop, memory_order_acquire)) {
        seed = seed * 1664525u + 1013904223u;
        size_t sz = 16u + (seed % 32768u);
        free(ring[idx]);
        ring[idx] = malloc(sz);
        if (ring[idx]) {
            ((volatile char *)ring[idx])[0] = 1;
        }
        idx = (idx + 1u) & 31u;
        if ((++ops & 7u) == 0u) {
            struct timespec pause = {0, 25000L}; /* 25 us */
            nanosleep(&pause, NULL);
        }
    }
    for (size_t i = 0; i < 32u; ++i) {
        free(ring[i]);
    }
    return NULL;
}

static void *bench_16t_compile_worker(void *arg)
{
    bench_16t_worker_t *w = (bench_16t_worker_t *)arg;
    pthread_barrier_wait(w->barrier);
    /* Warm up TLS slab once before timing. */
    size_t warm_len = 0;
    char *warm = waywal_shader_graph_compile_glsl(w->graph, w->schedule, &warm_len);
    assert(warm != NULL && warm_len > 0);
    free(warm);

    for (uint32_t i = 0; i < w->iters; ++i) {
        uint64_t t0 = get_monotonic_ns();
        size_t glsl_len = 0;
        char *glsl = waywal_shader_graph_compile_glsl(w->graph, w->schedule, &glsl_len);
        uint64_t t1 = get_monotonic_ns();
        assert(glsl != NULL && glsl_len > 0);
        free(glsl);
        w->latencies_ms[i] = (double)(t1 - t0) / 1000000.0;
        /* 250 us frame pacing across 16 workers avoids pure OS CFS timeslice starvation on
         * <=12-core hosts while sustaining ~64k contended compiles/s alongside malloc churn. */
        struct timespec pause = {0, 250000L};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void test_multithreaded_16t_compile_p99_slo(void)
{
    printf(
        "[+] Benchmarking PERF-039 (VD-08) 16-thread 256-node compile wall p99 (< 0.50 ms)...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);

    int32_t prev_id = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UVRoot", 0.0f, 0.0f);
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

    waywal_graph_schedule_t schedule;
    assert(waywal_shader_graph_analyze(graph, &schedule));

    enum { kThreads = 16, kChurnThreads = 4, kIters = 100 };
    double *all_lat = (double *)calloc((size_t)(kThreads * kIters), sizeof(double));
    assert(all_lat != NULL);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, kThreads + kChurnThreads);

    malloc_churn_ctx_t churn = {.barrier = &barrier, .stop = false};
    pthread_t churn_tids[kChurnThreads];
    for (int c = 0; c < kChurnThreads; ++c) {
        assert(pthread_create(&churn_tids[c], NULL, malloc_churn_thread, &churn) == 0);
    }

    bench_16t_worker_t workers[kThreads];
    pthread_t tids[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        workers[t] = (bench_16t_worker_t){
            .graph = graph,
            .schedule = &schedule,
            .barrier = &barrier,
            .iters = kIters,
            .latencies_ms = all_lat + (size_t)t * kIters,
        };
        assert(pthread_create(&tids[t], NULL, bench_16t_compile_worker, &workers[t]) == 0);
    }

    for (int t = 0; t < kThreads; ++t) {
        pthread_join(tids[t], NULL);
    }
    atomic_store_explicit(&churn.stop, true, memory_order_release);
    for (int c = 0; c < kChurnThreads; ++c) {
        pthread_join(churn_tids[c], NULL);
    }
    pthread_barrier_destroy(&barrier);

    const size_t total = (size_t)kThreads * kIters;
    qsort(all_lat, total, sizeof(double), cmp_double_asc);
    double p50_ms = all_lat[total / 2u];
    double p99_ms = all_lat[(total * 99u) / 100u];
    double max_ms = all_lat[total - 1u];

    printf("    -> 16T contended 256-node compile wall latency: p50=%.3f ms, p99=%.3f ms, "
           "max=%.3f ms (SLO: < 0.500 ms)\n",
           p50_ms, p99_ms, max_ms);

    const char *strict_env = getenv("WWAL_STRICT_PERF_ASSERTS");
    bool strict_perf = (strict_env != NULL && strcmp(strict_env, "1") == 0);
#if defined(WWAL_PERF_GATES)
    strict_perf = true;
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer) ||                           \
    defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    strict_perf = false;
#endif
    if (strict_perf) {
        assert(p99_ms < 0.50 && "16T shader_graph compile wall p99 exceeded 0.50 ms SLO (VD-08)");
    }

    free(all_lat);
    waywal_shader_graph_destroy(graph);
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
    test_connect_cycle_preserves_prior_edge();
    test_json_deep_alternating_skipper();
    test_safe_pow_oklab_iec_and_pin_sync();
    test_utf8_truncation_safety();
    test_shader_parser_nonfinite_guards();
    test_bezier_safeguarded_solver();
    test_ssa_gvn_deduplication();
    test_multithreaded_16t_compile_p99_slo();

    printf("==========================================================\n");
    printf("  >>> ALL SHADER GRAPH COMPILER TESTS PASSED (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
