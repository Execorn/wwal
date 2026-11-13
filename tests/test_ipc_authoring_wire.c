// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Verify ABI / wire contract invariants at compile time */
static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t must be 16 bytes");
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Legacy 72-byte SET_IMAGE ABI prefix broken");
static_assert(WAYWAL_REQ_SCENE_MUTATE == 0x0021u, "Unexpected WAYWAL_REQ_SCENE_MUTATE");
static_assert(WAYWAL_REQ_HOT_RELOAD_CODE == 0x0022u, "Unexpected WAYWAL_REQ_HOT_RELOAD_CODE");
static_assert(WAYWAL_REQ_QUERY_SCENE_TREE == 0x0023u, "Unexpected WAYWAL_REQ_QUERY_SCENE_TREE");
static_assert(WAYWAL_REQ_SET_SHADER_COMBO == 0x0024u, "Unexpected WAYWAL_REQ_SET_SHADER_COMBO");
static_assert(WAYWAL_REQ_BRUSH_STROKE == 0x0025u, "Unexpected WAYWAL_REQ_BRUSH_STROKE");
static_assert(WAYWAL_REQ_PARTICLE_CONFIG == 0x0026u, "Unexpected WAYWAL_REQ_PARTICLE_CONFIG");
static_assert(WAYWAL_REQ_SET_AUDIO_BINDINGS == 0x0027u, "Unexpected WAYWAL_REQ_SET_AUDIO_BINDINGS");
static_assert(WAYWAL_REQ_EXPORT_SCENE_PKG == 0x0028u, "Unexpected WAYWAL_REQ_EXPORT_SCENE_PKG");
static_assert(WAYWAL_REQ_LINT_SCENESCRIPT == 0x0029u, "Unexpected WAYWAL_REQ_LINT_SCENESCRIPT");
static_assert(WAYWAL_REQ_READBACK_TEXTURE == 0x002Au, "Unexpected WAYWAL_REQ_READBACK_TEXTURE");
static_assert(WAYWAL_RESP_SCENE_MUTATE == 0x8010u, "Unexpected WAYWAL_RESP_SCENE_MUTATE");
static_assert(WAYWAL_RESP_HOT_RELOAD_CODE == 0x8011u, "Unexpected WAYWAL_RESP_HOT_RELOAD_CODE");
static_assert(WAYWAL_RESP_QUERY_SCENE_TREE == 0x8012u, "Unexpected WAYWAL_RESP_QUERY_SCENE_TREE");
static_assert(WAYWAL_RESP_SET_SHADER_COMBO == 0x8013u, "Unexpected WAYWAL_RESP_SET_SHADER_COMBO");
static_assert(WAYWAL_RESP_BRUSH_STROKE == 0x8014u, "Unexpected WAYWAL_RESP_BRUSH_STROKE");
static_assert(WAYWAL_RESP_PARTICLE_CONFIG == 0x8015u, "Unexpected WAYWAL_RESP_PARTICLE_CONFIG");
static_assert(WAYWAL_RESP_SET_AUDIO_BINDINGS == 0x8016u,
              "Unexpected WAYWAL_RESP_SET_AUDIO_BINDINGS");
static_assert(WAYWAL_RESP_EXPORT_SCENE_PKG == 0x8017u, "Unexpected WAYWAL_RESP_EXPORT_SCENE_PKG");
static_assert(WAYWAL_RESP_LINT_SCENESCRIPT == 0x8018u, "Unexpected WAYWAL_RESP_LINT_SCENESCRIPT");
static_assert(WAYWAL_RESP_READBACK_TEXTURE == 0x8019u, "Unexpected WAYWAL_RESP_READBACK_TEXTURE");

static void test_legacy_set_image_header(void)
{
    waywal_ipc_hdr_t hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 71u,
    };
    assert(!daemon_validate_set_image_header(&hdr));

    hdr.payload_size = 72u;
    assert(daemon_validate_set_image_header(&hdr));

    hdr.payload_size = sizeof(waywal_img_metadata_t);
    assert(daemon_validate_set_image_header(&hdr));
}

static void test_scene_mutate_wire(void)
{
    waywal_ipc_req_scene_mutate_t params;
    memset(&params, 0, sizeof(params));
    snprintf(params.target_output, sizeof(params.target_output), "DP-1");
    params.mutate_op = WE_MUTATE_ADD_OBJECT;
    params.node_kind = WE_NODE_TEXT;
    params.object_id = 42;
    params.parent_id = -1;
    params.target_z_index = 0;
    params.blend_mode = 0u;
    params.flags = 0x1u;
    params.origin[0] = 100.0f;
    params.origin[1] = 200.0f;
    params.scale[0] = 1.0f;
    params.scale[1] = 1.0f;
    params.scale[2] = 1.0f;
    params.color[0] = 1.0f;
    params.color[1] = 1.0f;
    params.color[2] = 1.0f;
    params.color[3] = 0.9f;
    snprintf(params.name, sizeof(params.name), "clock_label");

    const char *json = "{\"text\":\"12:00\",\"pointsize\":48}";
    uint8_t buf[512];
    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_scene_mutate_t out_req;
    size_t total = 0u;

    assert(waywal_ipc_serialize_req_scene_mutate(&params, json, &hdr, &out_req, buf, sizeof(buf),
                                                 &total));
    assert(hdr.magic == WAYWAL_IPC_MAGIC);
    assert(hdr.version == WAYWAL_IPC_VERSION);
    assert(hdr.opcode == WAYWAL_REQ_SCENE_MUTATE);
    assert(hdr.payload_size == sizeof(params) + strlen(json));
    assert(total == hdr.payload_size);

    const char *parsed_json = NULL;
    assert(waywal_ipc_validate_req_scene_mutate((const waywal_ipc_req_scene_mutate_t *)buf, total,
                                                &parsed_json));
    assert(parsed_json != NULL);
    assert(memcmp(parsed_json, json, strlen(json)) == 0);

    /* Reject NaN origin */
    waywal_ipc_req_scene_mutate_t bad = out_req;
    bad.origin[0] = NAN;
    assert(!waywal_ipc_validate_req_scene_mutate(&bad, sizeof(bad), NULL));

    /* Reject out-of-range mutate_op */
    bad = out_req;
    bad.payload_len = 0u;
    bad.mutate_op = 99u;
    assert(!waywal_ipc_validate_req_scene_mutate(&bad, sizeof(bad), NULL));

    /* Reject embedded NUL in JSON tail */
    uint8_t bad_buf[512];
    memcpy(bad_buf, buf, total);
    bad_buf[sizeof(params) + 2u] = '\0';
    assert(!waywal_ipc_validate_req_scene_mutate((const waywal_ipc_req_scene_mutate_t *)bad_buf,
                                                 total, NULL));

    /* Response round-trip */
    waywal_ipc_resp_scene_mutate_t resp;
    assert(waywal_ipc_serialize_resp_scene_mutate(WAYWAL_STATUS_OK, 42, 7u, 3u, "", &hdr, &resp));
    assert(hdr.opcode == WAYWAL_RESP_SCENE_MUTATE);
    assert(waywal_ipc_validate_resp_scene_mutate(&resp, sizeof(resp)));
    resp.error_msg[sizeof(resp.error_msg) - 1u] = 'x';
    memset(resp.error_msg, 'a', sizeof(resp.error_msg));
    assert(!waywal_ipc_validate_resp_scene_mutate(&resp, sizeof(resp)));
}

static void test_hot_reload_and_combo_wire(void)
{
    const char *glsl = "// [COMBO] {\"material\":\"UI\",\"combo\":\"MODE\",\"default\":1}\n"
                       "uniform float u_Speed;\nvoid main() {}\n";
    uint8_t buf[1024];
    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_hot_reload_code_t req;
    size_t total = 0u;
    assert(waywal_ipc_serialize_req_hot_reload_code("HDMI-A-1", 0u, 0x6u, 3, UINT32_MAX, 0u,
                                                    "u_Speed", glsl, strlen(glsl), &hdr, &req, buf,
                                                    sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_REQ_HOT_RELOAD_CODE);
    const char *out_src = NULL;
    assert(waywal_ipc_validate_req_hot_reload_code((const waywal_ipc_req_hot_reload_code_t *)buf,
                                                   total, &out_src));
    assert(memcmp(out_src, glsl, strlen(glsl)) == 0);

    /* Reject invalid code_kind and oversized source_len */
    req.code_kind = 9u;
    req.source_len = 0u;
    assert(!waywal_ipc_validate_req_hot_reload_code(&req, sizeof(req), NULL));

    const char *meta = "{\"uniforms\":[],\"combos\":[]}";
    waywal_ipc_resp_hot_reload_code_t resp;
    assert(waywal_ipc_serialize_resp_hot_reload_code(WAYWAL_STATUS_OK, 0u, 0u, 1u, 1u, 250u, "",
                                                     meta, strlen(meta), &hdr, &resp, buf,
                                                     sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_RESP_HOT_RELOAD_CODE);
    const char *out_meta = NULL;
    assert(waywal_ipc_validate_resp_hot_reload_code((const waywal_ipc_resp_hot_reload_code_t *)buf,
                                                    total, &out_meta));
    assert(memcmp(out_meta, meta, strlen(meta)) == 0);

    /* Shader combo */
    waywal_ipc_req_set_shader_combo_t creq;
    assert(
        waywal_ipc_serialize_req_set_shader_combo("*", 1, UINT32_MAX, "MODE", 2, 0u, &hdr, &creq));
    assert(hdr.opcode == WAYWAL_REQ_SET_SHADER_COMBO);
    assert(waywal_ipc_validate_req_set_shader_combo(&creq, sizeof(creq)));
    assert(!waywal_ipc_validate_req_set_shader_combo(&creq, sizeof(creq) - 1u));
    creq.combo_name[0] = '\0';
    assert(!waywal_ipc_validate_req_set_shader_combo(&creq, sizeof(creq)));

    /* Reject GLSL directive injection and invalid identifiers in combo_name */
    assert(!waywal_ipc_serialize_req_set_shader_combo("*", 1, UINT32_MAX, "MODE\n#define EVIL 1", 1,
                                                      0u, &hdr, &creq));
    assert(!waywal_ipc_serialize_req_set_shader_combo("*", 1, UINT32_MAX, "1MODE", 1, 0u, &hdr,
                                                      &creq));
    memset(&creq, 0, sizeof(creq));
    snprintf(creq.target_output, sizeof(creq.target_output), "*");
    snprintf(creq.combo_name, sizeof(creq.combo_name), "BAD-COMBO");
    assert(!waywal_ipc_validate_req_set_shader_combo(&creq, sizeof(creq)));

    waywal_ipc_resp_set_shader_combo_t cresp;
    assert(waywal_ipc_serialize_resp_set_shader_combo(WAYWAL_STATUS_OK, 120u, "", &hdr, &cresp));
    assert(hdr.opcode == WAYWAL_RESP_SET_SHADER_COMBO);
    assert(waywal_ipc_validate_resp_set_shader_combo(&cresp, sizeof(cresp)));
}

static void test_query_tree_export_lint_readback_wire(void)
{
    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_query_scene_tree_t qreq;
    assert(waywal_ipc_serialize_req_query_scene_tree("*", 0xFu, &hdr, &qreq));
    assert(hdr.opcode == WAYWAL_REQ_QUERY_SCENE_TREE);
    assert(waywal_ipc_validate_req_query_scene_tree(&qreq, sizeof(qreq)));

    const char *tree = "{\"scene_revision\":3,\"objects\":[]}";
    uint8_t buf[512];
    waywal_ipc_resp_query_scene_tree_t qresp;
    size_t total = 0u;
    assert(waywal_ipc_serialize_resp_query_scene_tree(WAYWAL_STATUS_OK, 3u, 0u, tree, strlen(tree),
                                                      &hdr, &qresp, buf, sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_RESP_QUERY_SCENE_TREE);
    const char *out_tree = NULL;
    assert(waywal_ipc_validate_resp_query_scene_tree(
        (const waywal_ipc_resp_query_scene_tree_t *)buf, total, &out_tree));
    assert(memcmp(out_tree, tree, strlen(tree)) == 0);

    /* Export scene pkg: allowlist & flag enforcement */
    waywal_ipc_req_export_scene_pkg_t ereq;
    assert(waywal_ipc_serialize_req_export_scene_pkg(
        "DP-1", "/tmp/wwal-export-test/out.pkg",
        WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG | WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE, &hdr, &ereq));
    assert(hdr.opcode == WAYWAL_REQ_EXPORT_SCENE_PKG);
    assert(waywal_ipc_validate_req_export_scene_pkg(&ereq, sizeof(ereq)));
    assert(!waywal_ipc_serialize_req_export_scene_pkg("DP-1", "", 0u, &hdr, &ereq));

    /* Direct .pkg flag without pack flag must fail validation */
    assert(waywal_ipc_serialize_req_export_scene_pkg(
        "DP-1", "/tmp/wwal-export-test/out.pkg", WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE, &hdr, &ereq));
    assert(!waywal_ipc_validate_req_export_scene_pkg(&ereq, sizeof(ereq)));

    /* Unsafe or non-allowlisted export destinations must be rejected */
    const char *bad_export_paths[] = {
        "~/.config/evil",    "/home/user/.config/wwal/evil",
        "/tmp/out.pkg",      "/tmp/wwal-export-test/../evil",
        "/tmp/wwal-export-", "relative/export",
    };
    for (size_t i = 0; i < sizeof(bad_export_paths) / sizeof(bad_export_paths[0]); ++i) {
        assert(waywal_ipc_serialize_req_export_scene_pkg("DP-1", bad_export_paths[i], 0u, &hdr,
                                                         &ereq));
        assert(!waywal_ipc_validate_req_export_scene_pkg(&ereq, sizeof(ereq)));
    }

    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    const char *home_dir = getenv("HOME");
    char cache_export[512];
    if (xdg_cache && xdg_cache[0] == '/') {
        snprintf(cache_export, sizeof(cache_export), "%s/wwal/exports/unit_test_pkg", xdg_cache);
    } else if (home_dir && home_dir[0] == '/') {
        snprintf(cache_export, sizeof(cache_export), "%s/.cache/wwal/exports/unit_test_pkg",
                 home_dir);
    } else {
        snprintf(cache_export, sizeof(cache_export), "/tmp/wwal-export-unit/unit_test_pkg");
    }
    assert(waywal_ipc_serialize_req_export_scene_pkg(
        "DP-1", cache_export, WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG, &hdr, &ereq));
    assert(waywal_ipc_validate_req_export_scene_pkg(&ereq, sizeof(ereq)));

    waywal_ipc_resp_export_scene_pkg_t eresp;
    assert(waywal_ipc_serialize_resp_export_scene_pkg(WAYWAL_STATUS_OK, 5u, 4096ULL, 900ULL, "",
                                                      &hdr, &eresp));
    assert(hdr.opcode == WAYWAL_RESP_EXPORT_SCENE_PKG);
    assert(waywal_ipc_validate_resp_export_scene_pkg(&eresp, sizeof(eresp)));

    /* Lint scenescript */
    const char *js = "export function update(v) { return v; }";
    waywal_ipc_req_lint_scenescript_t lreq;
    assert(waywal_ipc_serialize_req_lint_scenescript(js, strlen(js), 1u, &hdr, &lreq, buf,
                                                     sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_REQ_LINT_SCENESCRIPT);
    const char *out_js = NULL;
    assert(waywal_ipc_validate_req_lint_scenescript((const waywal_ipc_req_lint_scenescript_t *)buf,
                                                    total, &out_js));
    assert(memcmp(out_js, js, strlen(js)) == 0);

    waywal_ipc_resp_lint_scenescript_t lresp;
    assert(waywal_ipc_serialize_resp_lint_scenescript(WAYWAL_STATUS_OK, 0u, 0u, 15u, "", &hdr,
                                                      &lresp));
    assert(hdr.opcode == WAYWAL_RESP_LINT_SCENESCRIPT);
    assert(waywal_ipc_validate_resp_lint_scenescript(&lresp, sizeof(lresp)));

    /* Readback texture */
    waywal_ipc_req_readback_texture_t rreq;
    assert(waywal_ipc_serialize_req_readback_texture("*", 1, UINT32_MAX, 0u, 0u, &hdr, &rreq));
    assert(hdr.opcode == WAYWAL_REQ_READBACK_TEXTURE);
    assert(waywal_ipc_validate_req_readback_texture(&rreq, sizeof(rreq)));
    rreq.format_mode = 2u;
    assert(!waywal_ipc_validate_req_readback_texture(&rreq, sizeof(rreq)));

    const uint8_t px[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    waywal_ipc_resp_readback_texture_t rresp;
    assert(waywal_ipc_serialize_resp_readback_texture(
        WAYWAL_STATUS_OK, 0u, 2u, 2u, 0u, px, sizeof(px), &hdr, &rresp, buf, sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_RESP_READBACK_TEXTURE);
    const void *out_px = NULL;
    uint32_t out_px_len = 0u;
    assert(waywal_ipc_validate_resp_readback_texture(
        (const waywal_ipc_resp_readback_texture_t *)buf, total, &out_px, &out_px_len));
    assert(out_px_len == sizeof(px));
    assert(memcmp(out_px, px, sizeof(px)) == 0);
}

static void test_brush_particle_audio_wire(void)
{
    /* Brush stroke */
    we_brush_params_t bp = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 16.0f,
        .hardness = 0.8f,
        .falloff_power = 1.0f,
        .opacity = 0.75f,
        .target_value = 1.0f,
        .flow_strength = 0.0f,
        .blur_radius_px = 4.0f,
        .pressure_size_weight = 0.5f,
        .pressure_alpha_weight = 0.5f,
        .velocity_damp_weight = 0.1f,
        .color_rgba = {1.0f, 1.0f, 1.0f, 1.0f},
    };
    we_brush_dab_t dabs[2] = {
        {.ax = 10.0f,
         .ay = 20.0f,
         .bx = 14.0f,
         .by = 24.0f,
         .radius_a = 8.0f,
         .radius_b = 8.0f,
         .opacity_eff = 0.8f},
        {.ax = 14.0f,
         .ay = 24.0f,
         .bx = 18.0f,
         .by = 28.0f,
         .radius_a = 8.0f,
         .radius_b = 8.0f,
         .opacity_eff = 0.8f},
    };

    uint8_t buf[512];
    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_brush_stroke_t breq;
    size_t total = 0u;
    assert(waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                 99ULL, &bp, dabs, 2u, &hdr, &breq, buf,
                                                 sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_REQ_BRUSH_STROKE);
    const we_brush_dab_t *out_dabs = NULL;
    uint32_t out_dab_cnt = 0u;
    assert(waywal_ipc_validate_req_brush_stroke((const waywal_ipc_req_brush_stroke_t *)buf, total,
                                                &out_dabs, &out_dab_cnt));
    assert(out_dab_cnt == 2u);
    assert(out_dabs != NULL);

    /* Reject INF in dab and negative radii/hardness/opacity */
    we_brush_dab_t bad_dab = dabs[0];
    bad_dab.ax = INFINITY;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bp, &bad_dab, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    bad_dab = dabs[0];
    bad_dab.radius_a = -1.0f;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bp, &bad_dab, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    bad_dab = dabs[0];
    bad_dab.radius_b = -0.5f;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bp, &bad_dab, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    we_brush_params_t bad_bp = bp;
    bad_bp.radius_px = -2.0f;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bad_bp, dabs, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    bad_bp = bp;
    bad_bp.hardness = -0.25f;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bad_bp, dabs, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    bad_bp = bp;
    bad_bp.opacity = -0.1f;
    assert(!waywal_ipc_serialize_req_brush_stroke("DP-1", WE_BRUSH_IPC_DAB_BATCH, 1, UINT32_MAX, 0u,
                                                  99ULL, &bad_bp, dabs, 1u, &hdr, &breq, buf,
                                                  sizeof(buf), &total));

    we_brush_dirty_rect_t rect = {.x = 2, .y = 12, .width = 24, .height = 24};
    we_brush_stats_t stats = {.last_dab_gpu_us = 45u,
                              .max_pool_slots = 4096u,
                              .canvas_width = 1920u,
                              .canvas_height = 1080u};
    waywal_ipc_resp_brush_stroke_t bresp;
    assert(waywal_ipc_serialize_resp_brush_stroke(WAYWAL_STATUS_OK, 99ULL, &rect, &stats, 4u, &hdr,
                                                  &bresp));
    assert(hdr.opcode == WAYWAL_RESP_BRUSH_STROKE);
    assert(waywal_ipc_validate_resp_brush_stroke(&bresp, sizeof(bresp)));

    /* Particle config */
    we_particle_ramp_lut_t lut;
    memset(&lut, 0, sizeof(lut));
    lut.operator_index = 0u;
    lut.channel_kind = 0u;
    lut.knot_count = 2u;
    for (uint32_t i = 0; i < WE_PARTICLE_RAMP_SAMPLES; ++i) {
        lut.scalar_lut[i] = (float)i / (float)(WE_PARTICLE_RAMP_SAMPLES - 1u);
    }

    uint8_t *pbuf = (uint8_t *)malloc(2048u);
    assert(pbuf != NULL);
    waywal_ipc_req_particle_config_t preq;
    assert(waywal_ipc_serialize_req_particle_config("*", 5, WE_PARTICLE_CFG_SET_RAMP_LUT, 0u, 0u,
                                                    &lut, (uint32_t)sizeof(lut), &hdr, &preq, pbuf,
                                                    2048u, &total));
    assert(hdr.opcode == WAYWAL_REQ_PARTICLE_CONFIG);
    const void *ppayload = NULL;
    uint32_t plen = 0u;
    assert(waywal_ipc_validate_req_particle_config((const waywal_ipc_req_particle_config_t *)pbuf,
                                                   total, &ppayload, &plen));
    assert(plen == sizeof(lut));

    /* Reject knot_count < 2 */
    we_particle_ramp_lut_t bad_lut = lut;
    bad_lut.knot_count = 1u;
    assert(!waywal_ipc_serialize_req_particle_config("*", 5, WE_PARTICLE_CFG_SET_RAMP_LUT, 0u, 0u,
                                                     &bad_lut, (uint32_t)sizeof(bad_lut), &hdr,
                                                     &preq, pbuf, 2048u, &total));
    bad_lut.knot_count = 0u;
    assert(!waywal_ipc_serialize_req_particle_config("*", 5, WE_PARTICLE_CFG_SET_RAMP_LUT, 0u, 0u,
                                                     &bad_lut, (uint32_t)sizeof(bad_lut), &hdr,
                                                     &preq, pbuf, 2048u, &total));
    free(pbuf);

    waywal_ipc_resp_particle_config_t presp;
    assert(waywal_ipc_serialize_resp_particle_config(WAYWAL_STATUS_OK, 5, 128u, 512u, 320u, &hdr,
                                                     &presp));
    assert(hdr.opcode == WAYWAL_RESP_PARTICLE_CONFIG);
    assert(waywal_ipc_validate_resp_particle_config(&presp, sizeof(presp)));

    /* Audio bindings */
    we_audio_binding_t bind;
    memset(&bind, 0, sizeof(bind));
    bind.binding_id = 7u;
    bind.target_object_id = 1;
    bind.target_kind = WE_AUDIO_TGT_PASS_UNIFORM_COMP;
    bind.target_component = 0u;
    snprintf(bind.uniform_name, sizeof(bind.uniform_name), "u_Pulse");
    bind.src_kind = WE_AUDIO_SRC_BASS_ENERGY;
    bind.agg_mode = WE_AUDIO_AGG_PEAK_WEIGHTED;
    bind.band_start = 0u;
    bind.band_end = 7u;
    bind.input_gain = 1.5f;
    bind.threshold_open = 0.2f;
    bind.hysteresis = 0.05f;
    bind.attack_sec = 0.01f;
    bind.release_sec = 0.15f;
    bind.curve_shape = WE_AUDIO_CURVE_SMOOTHSTEP;
    bind.blend_mode = WE_AUDIO_BLEND_ADD;
    bind.out_min = 0.0f;
    bind.out_max = 1.0f;
    bind.enabled = 1u;

    waywal_ipc_req_set_audio_bindings_t areq;
    assert(waywal_ipc_serialize_req_set_audio_bindings("*", WE_AUDIO_BIND_OP_REPLACE_ALL, 0u, &bind,
                                                       1u, &hdr, &areq, buf, sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_REQ_SET_AUDIO_BINDINGS);
    const we_audio_binding_t *out_binds = NULL;
    uint32_t out_cnt = 0u;
    assert(waywal_ipc_validate_req_set_audio_bindings(
        (const waywal_ipc_req_set_audio_bindings_t *)buf, total, &out_binds, &out_cnt));
    assert(out_cnt == 1u);

    /* Reject band_start > band_end and out-of-range audio floats */
    we_audio_binding_t bad_bind = bind;
    bad_bind.band_start = 10u;
    bad_bind.band_end = 5u;
    assert(!waywal_ipc_serialize_req_set_audio_bindings("*", WE_AUDIO_BIND_OP_UPSERT, 0u, &bad_bind,
                                                        1u, &hdr, &areq, buf, sizeof(buf), &total));

    bad_bind = bind;
    bad_bind.input_gain = -1.0f;
    assert(!waywal_ipc_serialize_req_set_audio_bindings("*", WE_AUDIO_BIND_OP_UPSERT, 0u, &bad_bind,
                                                        1u, &hdr, &areq, buf, sizeof(buf), &total));

    bad_bind = bind;
    bad_bind.out_max = 1e9f;
    assert(!waywal_ipc_serialize_req_set_audio_bindings("*", WE_AUDIO_BIND_OP_UPSERT, 0u, &bad_bind,
                                                        1u, &hdr, &areq, buf, sizeof(buf), &total));

    bad_bind = bind;
    bad_bind.attack_sec = 999.0f;
    assert(!waywal_ipc_serialize_req_set_audio_bindings("*", WE_AUDIO_BIND_OP_UPSERT, 0u, &bad_bind,
                                                        1u, &hdr, &areq, buf, sizeof(buf), &total));

    waywal_ipc_resp_set_audio_bindings_t aresp;
    assert(waywal_ipc_serialize_resp_set_audio_bindings(WAYWAL_STATUS_OK, 1u, 1200u, &hdr, &aresp));
    assert(hdr.opcode == WAYWAL_RESP_SET_AUDIO_BINDINGS);
    assert(waywal_ipc_validate_resp_set_audio_bindings(&aresp, sizeof(aresp)));
}

static void test_authoring_framing_0x0021_to_0x002A(void)
{
    static const uint16_t opcodes[10] = {
        WAYWAL_REQ_SCENE_MUTATE,       WAYWAL_REQ_HOT_RELOAD_CODE,  WAYWAL_REQ_QUERY_SCENE_TREE,
        WAYWAL_REQ_SET_SHADER_COMBO,   WAYWAL_REQ_BRUSH_STROKE,     WAYWAL_REQ_PARTICLE_CONFIG,
        WAYWAL_REQ_SET_AUDIO_BINDINGS, WAYWAL_REQ_EXPORT_SCENE_PKG, WAYWAL_REQ_LINT_SCENESCRIPT,
        WAYWAL_REQ_READBACK_TEXTURE,
    };
    static const size_t min_sizes[10] = {
        sizeof(waywal_ipc_req_scene_mutate_t),       sizeof(waywal_ipc_req_hot_reload_code_t),
        sizeof(waywal_ipc_req_query_scene_tree_t),   sizeof(waywal_ipc_req_set_shader_combo_t),
        sizeof(waywal_ipc_req_brush_stroke_t),       sizeof(waywal_ipc_req_particle_config_t),
        sizeof(waywal_ipc_req_set_audio_bindings_t), sizeof(waywal_ipc_req_export_scene_pkg_t),
        sizeof(waywal_ipc_req_lint_scenescript_t),   sizeof(waywal_ipc_req_readback_texture_t),
    };
    for (size_t i = 0; i < 10u; ++i) {
        assert(opcodes[i] == (uint16_t)(0x0021u + i));
        assert(min_sizes[i] >= sizeof(waywal_ipc_hdr_t));
    }
}

int main(void)
{
    test_legacy_set_image_header();
    test_scene_mutate_wire();
    test_hot_reload_and_combo_wire();
    test_query_tree_export_lint_readback_wire();
    test_brush_particle_audio_wire();
    test_authoring_framing_0x0021_to_0x002A();
    printf("test_ipc_authoring_wire: all tests passed!\n");
    return 0;
}
