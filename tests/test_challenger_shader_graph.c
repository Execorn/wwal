// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_challenger_shader_graph.c
 * @brief Adversarial Verification & Stress Test Suite for Shader Graph & IPC Wire
 *
 * Reproduces and verifies edge-case failure modes:
 * 1. Tarjan SCC self-loop exemption on WAYWAL_NODE_IN_PASS_BUFFER.
 * 2. Infinite recursion stack overflow in compute_sethi_ullman_weights on feedback loops.
 * 3. Invalid GLSL ES 3.10 'const' qualifiers on SSA variables with runtime initializers.
 * 4. Cross-pass undeclared identifier emission (n<id>_out0) in multi-pass & barrier shaders.
 * 5. IPC wire length mismatch rejection in daemon dispatch.
 */

#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/shader_graph.h"

#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Challenge 1: Self-loop detection on Pass Buffer */
static void challenge_pass_buffer_self_loop(void)
{
    printf("[CHALLENGE 1] Testing self-loop on WAYWAL_NODE_IN_PASS_BUFFER...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);

    int32_t latch =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_PASS_BUFFER, "Latch", 0.0f, 0.0f);
    assert(latch > 0);

    // Self-loop: connect output 0 to input 0
    int32_t eid = waywal_shader_graph_connect(graph, (uint32_t)latch, 0, (uint32_t)latch, 0);
    assert(eid < 0 && "waywal_shader_graph_connect must reject self-loop on PASS_BUFFER");
    printf("  [*] Self-loop correctly rejected.\n");

    waywal_shader_graph_destroy(graph);
}

/* Challenge 2: Stack overflow on feedback loop in compute_sethi_ullman_weights */
static void challenge_feedback_loop_stack_overflow(void)
{
    printf("[CHALLENGE 2] Testing latched feedback loop in compute_sethi_ullman_weights...\n");

    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        waywal_shader_graph_t *graph = waywal_shader_graph_create();
        int32_t n1 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_MATH_ADD, "Add", 0.0f, 0.0f);
        int32_t n2 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_MATH_MUL, "Mul", 100.0f, 0.0f);
        int32_t n_latch =
            waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_PASS_BUFFER, "Latch", 200.0f, 0.0f);
        int32_t n_sink =
            waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 300.0f, 0.0f);

        waywal_shader_graph_connect(graph, (uint32_t)n1, 0, (uint32_t)n2, 0);
        waywal_shader_graph_connect(graph, (uint32_t)n2, 0, (uint32_t)n_latch, 0);
        waywal_shader_graph_connect(graph, (uint32_t)n_latch, 0, (uint32_t)n1, 1);
        waywal_shader_graph_connect(graph, (uint32_t)n2, 0, (uint32_t)n_sink, 0);

        waywal_graph_schedule_t schedule;
        bool ok = waywal_shader_graph_analyze(graph, &schedule);
        size_t glsl_len = 0;
        char *glsl = ok ? waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len) : NULL;
        bool pass = (ok && schedule.has_cycle_error == 0 && schedule.sorted_count == 4 &&
                     glsl != NULL && glsl_len > 0);
        free(glsl);
        waywal_shader_graph_destroy(graph);
        _exit(pass ? 0 : 1);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(!WIFSIGNALED(status) && "compute_sethi_ullman_weights crashed on latched feedback loop");
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           "Latched feedback loop failed schedule/codegen");
    printf("  [*] No crash on latched feedback loop; schedule & codegen succeeded.\n");
}

/* Challenge 3: GLSL ES 3.10 const qualifier syntax error */
static void challenge_glsl_const_qualifier(void)
{
    printf("[CHALLENGE 3] Testing GLSL ES 3.10 SSA 'const' qualifier validity...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    int32_t n1 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
    int32_t n2 = waywal_shader_graph_add_node(graph, WAYWAL_NODE_MATH_SIN, "Sin", 100.0f, 0.0f);
    int32_t n3 =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 200.0f, 0.0f);

    waywal_shader_graph_connect(graph, (uint32_t)n1, 0, (uint32_t)n2, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n2, 0, (uint32_t)n3, 0);

    waywal_graph_schedule_t schedule;
    assert(waywal_shader_graph_analyze(graph, &schedule));

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);

    bool has_illegal_const_v_texcoord = (strstr(glsl, "const vec2 n1_out0 = v_TexCoord;") != NULL);
    bool has_illegal_const_u_time =
        (strstr(glsl, "const float n") != NULL && strstr(glsl, "= u_Time;") != NULL);

    assert(!has_illegal_const_v_texcoord && "Generated GLSL contains illegal const on v_TexCoord");
    assert(!has_illegal_const_u_time && "Generated GLSL contains illegal const on u_Time");
    printf("  [*] No illegal const qualifiers found.\n");

    free(glsl);
    waywal_shader_graph_destroy(graph);
}

/* Challenge 4: Cross-pass undeclared variable emission */
static void challenge_cross_pass_undeclared_variables(void)
{
    printf("[CHALLENGE 4] Testing cross-pass SSA variable leakage (undeclared identifiers)...\n");

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    int32_t n_uv = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
    int32_t n_kawase =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_FILTER_DUAL_KAWASE, "Kawase", 150.0f, 0.0f);
    int32_t n_sink =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "FinalSink", 300.0f, 0.0f);

    waywal_shader_graph_connect(graph, (uint32_t)n_uv, 0, (uint32_t)n_kawase, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_kawase, 0, (uint32_t)n_sink, 0);

    waywal_graph_schedule_t schedule;
    assert(waywal_shader_graph_analyze(graph, &schedule));

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);

    char *image_pass_ptr = strstr(glsl, "#pragma Pass IMAGE_PASS");
    assert(image_pass_ptr != NULL);
    char *main_ptr = strstr(image_pass_ptr, "void main()");
    assert(main_ptr != NULL);
    bool leaked_ssa =
        (strstr(main_ptr, "n2_out0") != NULL && strstr(image_pass_ptr, "n2_out0 =") == NULL);
    assert(!leaked_ssa && "IMAGE_PASS references undeclared cross-pass SSA variable n2_out0");
    printf("  [*] Cross-pass SSA isolation verified.\n");

    free(glsl);
    waywal_shader_graph_destroy(graph);
}

/* Challenge 5: IPC wire framing validation */
static void challenge_ipc_wire_framing(void)
{
    printf("[CHALLENGE 5] Testing IPC wire framing validation...\n");

    waywal_ipc_req_compile_shader_graph_t req;
    memset(&req, 0, sizeof(req));
    req.inline_json_len = 50000; // Claims 50KB inline JSON

    // Payload size is only header (1096 bytes), does not match claimed inline_json_len
    const char *inline_json = NULL;
    bool valid = waywal_ipc_validate_req_compile_shader_graph(&req, sizeof(req), &inline_json);
    assert(!valid && "waywal_ipc_validate_req_compile_shader_graph must reject truncated payload");
    printf("  [*] waywal_ipc_validate_req_compile_shader_graph correctly rejected mismatched "
           "inline_json_len.\n");
}

/* Challenge 6: GLSL comment breakout sanitization & adversarial JSON fuzzing */
static void challenge_glsl_comment_injection_and_json_adversarial(void)
{
    printf("[CHALLENGE 6] Testing GLSL comment breakout & adversarial JSON rejection...\n");

    waywal_shader_graph_t *g = waywal_shader_graph_create();
    int32_t n_uv = waywal_shader_graph_add_node(g, WAYWAL_NODE_IN_UV, "UV", 0, 0);
    int32_t n_fx = waywal_shader_graph_add_node(g, WAYWAL_NODE_BUILTIN_EFFECT, "FX", 100, 0);
    int32_t n_out = waywal_shader_graph_add_node(g, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 200, 0);
    waywal_graph_node_t *fx = waywal_shader_graph_find_node(g, (uint32_t)n_fx);
    strncpy(fx->prop_or_asset_key, "waterripple\\\n#error INJECTED_CODE\r\n\\",
            sizeof(fx->prop_or_asset_key) - 1);
    waywal_shader_graph_connect(g, (uint32_t)n_uv, 0, (uint32_t)n_fx, 0);
    waywal_shader_graph_connect(g, (uint32_t)n_fx, 0, (uint32_t)n_out, 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(g, &sched));
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(g, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);
    assert(strstr(glsl, "\n#error INJECTED_CODE") == NULL);
    assert(strstr(glsl, "    vec4 n2_out0 = ") != NULL);
    free(glsl);
    waywal_shader_graph_destroy(g);

    waywal_shader_graph_t *dst = waywal_shader_graph_create();
    char err_buf[256] = {0};

    const char *bad_docs[] = {
        "{\"schema_version\":1,\"flags\":1e30}",
        "{\"schema_version\":1,\"flags\":-1}",
        "{\"schema_version\":1,\"flags\":1.5}",
        "{\"schema_version\":1,\"flags\":4294967296}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":8,\"const_val\":[1e300,0,0,1]}]"
        "}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":8,\"const_val\":[1e999,0,0,1]}]"
        "}",
        "{\"schema_version\":1,\"graph_name\":\"bad\\u12\"}",
        "{\"schema_version\":1,\"graph_name\":\"bad\nraw_newline\"}",
        "{\"schema_version\":1} trailing_garbage",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":0,\"opcode\":0}]}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":999}]}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":0},{\"node_id\":1,\"opcode\":1}"
        "]}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":0}],\"edges\":[{\"edge_id\":1,"
        "\"src_node_id\":1,\"src_pin_index\":0,\"dst_node_id\":99,\"dst_pin_index\":0}]}",
        "{\"schema_version\":1,\"nodes\":[{\"node_id\":1,\"opcode\":10},{\"node_id\":2,\"opcode\":"
        "10}],\"edges\":[{\"edge_id\":1,\"src_node_id\":1,\"src_pin_index\":0,\"dst_node_id\":2,"
        "\"dst_pin_index\":0},{\"edge_id\":2,\"src_node_id\":2,\"src_pin_index\":0,\"dst_node_id\":"
        "1,\"dst_pin_index\":0}]}",
    };

    for (size_t i = 0; i < sizeof(bad_docs) / sizeof(bad_docs[0]); ++i) {
        assert(!waywal_shader_graph_load_json(bad_docs[i], strlen(bad_docs[i]), dst, err_buf));
    }

    // Truncated \u escape right at exact slice end (ASAN out-of-bounds read guard)
    const char *trunc_u = "{\"schema_version\":1,\"graph_name\":\"\\u12";
    size_t trunc_len = strlen(trunc_u);
    char *trunc_buf = (char *)malloc(trunc_len);
    assert(trunc_buf != NULL);
    memcpy(trunc_buf, trunc_u, trunc_len);
    assert(!waywal_shader_graph_load_json(trunc_buf, trunc_len, dst, err_buf));
    free(trunc_buf);

    // Unordered JSON keys ("edges" before "nodes") must still sync WAYWAL_PIN_FLAG_CONNECTED
    const char *edges_first =
        "{\"schema_version\":1,"
        "\"edges\":[{\"edge_id\":1,\"src_node_id\":1,\"src_pin_index\":0,\"dst_node_id\":2,\"dst_"
        "pin_index\":0}],"
        "\"nodes\":[{\"node_id\":1,\"opcode\":0},{\"node_id\":2,\"opcode\":50}]}";
    assert(waywal_shader_graph_load_json(edges_first, strlen(edges_first), dst, err_buf));
    assert((waywal_shader_graph_find_node(dst, 1)->outputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) !=
           0u);
    assert((waywal_shader_graph_find_node(dst, 2)->inputs[0].flags & WAYWAL_PIN_FLAG_CONNECTED) !=
           0u);

    waywal_shader_graph_destroy(dst);
    printf("  [*] Comment injection neutralized and all adversarial JSON inputs rejected.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  Adversarial Shader Graph & IPC Framing Stress Suite     \n");
    printf("==========================================================\n");

    challenge_pass_buffer_self_loop();
    challenge_feedback_loop_stack_overflow();
    challenge_glsl_const_qualifier();
    challenge_cross_pass_undeclared_variables();
    challenge_ipc_wire_framing();
    challenge_glsl_comment_injection_and_json_adversarial();

    printf("==========================================================\n");
    printf("  >>> ALL CHALLENGER SHADER GRAPH TESTS PASSED (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
