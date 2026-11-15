// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/os_compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

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

static bool auth_send_msg_nb(int fd, const waywal_ipc_hdr_t *hdr, const void *payload,
                             size_t payload_len)
{
    struct iovec iov[2];
    iov[0].iov_base = (void *)hdr;
    iov[0].iov_len = sizeof(*hdr);
    size_t iov_cnt = 1u;
    if (payload_len > 0u) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = payload_len;
        iov_cnt = 2u;
    }

    size_t idx = 0u;
    while (idx < iov_cnt) {
        struct msghdr msg = {0};
        msg.msg_iov = &iov[idx];
        msg.msg_iovlen = iov_cnt - idx;
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
                if (poll(&pfd, 1, 2000) <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        size_t adv = (size_t)n;
        while (idx < iov_cnt && adv >= iov[idx].iov_len) {
            adv -= iov[idx].iov_len;
            idx++;
        }
        if (idx < iov_cnt && adv > 0u) {
            iov[idx].iov_base = (uint8_t *)iov[idx].iov_base + adv;
            iov[idx].iov_len -= adv;
        }
    }
    return true;
}

static bool auth_recv_exact_nb(int fd, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0u;
    while (got < len) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 2000) <= 0) {
            return false;
        }
        ssize_t n = recv(fd, dst + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void test_authoring_wire_dispatch_framing_and_bounds(void)
{
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);

    /* Mixed legacy 72 B SET_IMAGE + F_SEAL_SHRINK memfd framing */
    int shm_fd = waywal_create_memfd("waywal-auth-img", 4096u, F_SEAL_SHRINK);
    assert(shm_fd >= 0);

    uint8_t leg72[72] = {0};
    waywal_img_metadata_t leg_meta;
    memset(&leg_meta, 0, sizeof(leg_meta));
    leg_meta.width = 16u;
    leg_meta.height = 16u;
    memcpy(leg72, &leg_meta, sizeof(leg72));

    waywal_ipc_hdr_t leg_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = sizeof(leg72),
    };
    assert(waywal_ipc_send_payload_fds(sv[0], &leg_hdr, leg72, sizeof(leg72), &shm_fd, 1u));
    close(shm_fd);

    waywal_ipc_hdr_t rx_hdr = {0};
    int rx_fd = -1;
    size_t rx_nfds = 0u;
    assert(waywal_ipc_recv_fds(sv[1], &rx_hdr, &rx_fd, 1u, &rx_nfds));
    assert(rx_hdr.opcode == WAYWAL_REQ_SET_IMAGE);
    assert(rx_hdr.payload_size == 72u);
    assert(rx_nfds == 1u && rx_fd >= 0);
    assert((fcntl(rx_fd, F_GET_SEALS) & F_SEAL_SHRINK) != 0);
    close(rx_fd);

    uint8_t rx_leg72[72] = {0};
    assert(auth_recv_exact_nb(sv[1], rx_leg72, sizeof(rx_leg72)));

    waywal_img_metadata_t zero_filled;
    memset(&zero_filled, 0, sizeof(zero_filled));
    memcpy(&zero_filled, rx_leg72, sizeof(rx_leg72));
    uint64_t px_bytes = 0u;
    assert(waywal_validate_img_metadata(&zero_filled, 4096u, &px_bytes));
    assert(px_bytes == 1024u);

    /* Truncated / inconsistent extended metadata lengths must fail validation */
    waywal_img_metadata_t bad_meta = zero_filled;
    bad_meta.extra_data_len = 16u;
    bad_meta.custom_shader_len = 17u;
    assert(!waywal_validate_img_metadata(&bad_meta, 4096u, NULL));
    bad_meta.extra_data_len = WWAL_MAX_EXTRA_IPC_BYTES + 1u;
    bad_meta.custom_shader_len = 0u;
    assert(!waywal_validate_img_metadata(&bad_meta, 4096u, NULL));

    /* Structured 148 B uniform wire packet */
    waywal_set_uniform_payload_t u_pay;
    memset(&u_pay, 0, sizeof(u_pay));
    u_pay.structured.struct_size = sizeof(waywal_set_uniform_structured_t);
    u_pay.structured.flags = WAYWAL_UNIFORM_FLAG_STRUCTURED;
    snprintf(u_pay.structured.target_output, sizeof(u_pay.structured.target_output), "%s", "DP-1");
    snprintf(u_pay.structured.uniform_name, sizeof(u_pay.structured.uniform_name), "%s",
             "u_authoring_time");
    u_pay.structured.uniform_components = 1u;
    u_pay.structured.uniform_values[0] = 3.5f;

    waywal_ipc_hdr_t uhdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_UNIFORM,
        .payload_size = sizeof(u_pay),
    };
    assert(auth_send_msg_nb(sv[0], &uhdr, &u_pay, sizeof(u_pay)));
    assert(auth_recv_exact_nb(sv[1], &rx_hdr, sizeof(rx_hdr)));
    assert(rx_hdr.opcode == WAYWAL_REQ_SET_UNIFORM);
    assert(rx_hdr.payload_size == sizeof(waywal_set_uniform_structured_t));
    waywal_set_uniform_payload_t u_rx;
    assert(auth_recv_exact_nb(sv[1], &u_rx, sizeof(u_rx)));

    char out_name[64] = {0};
    char uniform_name[64] = {0};
    uint32_t comps = 0u;
    float vals[4] = {0};
    uint32_t uflags = 0u;
    assert(waywal_decode_set_uniform_payload(&u_rx, out_name, sizeof(out_name), uniform_name,
                                             sizeof(uniform_name), &comps, vals, &uflags));
    assert(strcmp(out_name, "DP-1") == 0 && strcmp(uniform_name, "u_authoring_time") == 0);
    assert(comps == 1u && fabsf(vals[0] - 3.5f) < 1e-6f);

    /* Single-sendmsg SCENE_MUTATE + HOT_RELOAD_CODE + SET_SHADER_COMBO wire round-trip */
    waywal_ipc_req_scene_mutate_t mut_params;
    memset(&mut_params, 0, sizeof(mut_params));
    snprintf(mut_params.target_output, sizeof(mut_params.target_output), "%s", "DP-1");
    mut_params.mutate_op = WE_MUTATE_SET_TRANSFORM;
    mut_params.object_id = 42;
    mut_params.parent_id = -1;
    mut_params.scale[0] = 1.0f;
    mut_params.scale[1] = 1.0f;
    mut_params.scale[2] = 1.0f;
    mut_params.color[3] = 1.0f;
    snprintf(mut_params.name, sizeof(mut_params.name), "%s", "layer_a");

    uint8_t pkt_buf[4096];
    waywal_ipc_hdr_t mh = {0};
    waywal_ipc_req_scene_mutate_t mreq = {0};
    size_t mlen = 0u;
    assert(waywal_ipc_serialize_req_scene_mutate(&mut_params, "{\"x\":1}", &mh, &mreq, pkt_buf,
                                                 sizeof(pkt_buf), &mlen));
    assert(auth_send_msg_nb(sv[0], &mh, pkt_buf, mlen));

    waywal_ipc_hdr_t rx_mh = {0};
    assert(auth_recv_exact_nb(sv[1], &rx_mh, sizeof(rx_mh)));
    assert(rx_mh.opcode == WAYWAL_REQ_SCENE_MUTATE && rx_mh.payload_size == mlen);
    uint8_t rx_buf[4096];
    assert(rx_mh.payload_size <= sizeof(rx_buf));
    assert(auth_recv_exact_nb(sv[1], rx_buf, rx_mh.payload_size));
    const char *mut_json = NULL;
    assert(waywal_ipc_validate_req_scene_mutate((const waywal_ipc_req_scene_mutate_t *)rx_buf,
                                                rx_mh.payload_size, &mut_json));
    assert(mut_json != NULL && strcmp(mut_json, "{\"x\":1}") == 0);

    /* Oversized mutate payload_len mismatch must be rejected deterministically */
    waywal_ipc_req_scene_mutate_t bad_mut = mreq;
    bad_mut.payload_len = 128u;
    assert(!waywal_ipc_validate_req_scene_mutate(&bad_mut, sizeof(bad_mut), &mut_json));

    /* HOT_RELOAD_CODE packet + status reply framing */
    const char *frag_src = "void main() { gl_FragColor = vec4(1.0); }";
    waywal_ipc_hdr_t hh = {0};
    waywal_ipc_req_hot_reload_code_t hreq = {0};
    size_t hlen = 0u;
    assert(waywal_ipc_serialize_req_hot_reload_code("DP-1", 0u, 0x2u, 42, 0u, 0u, "", frag_src,
                                                    strlen(frag_src), &hh, &hreq, pkt_buf,
                                                    sizeof(pkt_buf), &hlen));
    assert(auth_send_msg_nb(sv[0], &hh, pkt_buf, hlen));
    assert(auth_recv_exact_nb(sv[1], &rx_hdr, sizeof(rx_hdr)));
    assert(rx_hdr.opcode == WAYWAL_REQ_HOT_RELOAD_CODE && rx_hdr.payload_size == hlen);
    assert(rx_hdr.payload_size <= sizeof(rx_buf));
    assert(auth_recv_exact_nb(sv[1], rx_buf, rx_hdr.payload_size));
    const char *rx_src = NULL;
    assert(waywal_ipc_validate_req_hot_reload_code((const waywal_ipc_req_hot_reload_code_t *)rx_buf,
                                                   rx_hdr.payload_size, &rx_src));
    assert(rx_src != NULL && strcmp(rx_src, frag_src) == 0);

    /* SET_SHADER_COMBO packet framing */
    waywal_ipc_hdr_t ch = {0};
    waywal_ipc_req_set_shader_combo_t creq = {0};
    assert(waywal_ipc_serialize_req_set_shader_combo("DP-1", 42, 0u, "QUALITY", 2, 0u, &ch, &creq));
    assert(auth_send_msg_nb(sv[0], &ch, &creq, sizeof(creq)));
    assert(auth_recv_exact_nb(sv[1], &rx_hdr, sizeof(rx_hdr)));
    assert(rx_hdr.opcode == WAYWAL_REQ_SET_SHADER_COMBO && rx_hdr.payload_size == sizeof(creq));
    waywal_ipc_req_set_shader_combo_t creq_rx;
    assert(auth_recv_exact_nb(sv[1], &creq_rx, sizeof(creq_rx)));
    assert(waywal_ipc_validate_req_set_shader_combo(&creq_rx, sizeof(creq_rx)));

    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    test_legacy_set_image_header();
    test_scene_mutate_wire();
    test_hot_reload_and_combo_wire();
    test_query_tree_export_lint_readback_wire();
    test_brush_particle_audio_wire();
    test_authoring_framing_0x0021_to_0x002A();
    test_authoring_wire_dispatch_framing_and_bounds();
    printf("test_ipc_authoring_wire: all tests passed!\n");
    return 0;
}
