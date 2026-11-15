// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/ipc_proto_authoring.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/ipc_proto.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Verify ABI / wire contract invariants at compile time */
static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t must be 16 bytes");
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Legacy 72-byte SET_IMAGE ABI prefix broken");
static_assert(WAYWAL_REQ_PUPPET_SET_MESH == 0x002Bu, "Unexpected WAYWAL_REQ_PUPPET_SET_MESH");
static_assert(WAYWAL_REQ_PUPPET_SET_SKELETON == 0x002Cu,
              "Unexpected WAYWAL_REQ_PUPPET_SET_SKELETON");
static_assert(WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS == 0x002Du,
              "Unexpected WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS");
static_assert(WAYWAL_REQ_PUPPET_PAINT_WEIGHTS == 0x002Eu,
              "Unexpected WAYWAL_REQ_PUPPET_PAINT_WEIGHTS");
static_assert(WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE == 0x002Fu,
              "Unexpected WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE");
static_assert(WAYWAL_REQ_PUPPET_PREVIEW_IK == 0x0030u, "Unexpected WAYWAL_REQ_PUPPET_PREVIEW_IK");
static_assert(WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS == 0x0031u,
              "Unexpected WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS");
static_assert(WAYWAL_REQ_PUPPET_SCRUB_TIME == 0x0032u, "Unexpected WAYWAL_REQ_PUPPET_SCRUB_TIME");
static_assert(WAYWAL_REQ_EXPORT_PUPPET_MDL == 0x0033u, "Unexpected WAYWAL_REQ_EXPORT_PUPPET_MDL");
static_assert(WAYWAL_RESP_PUPPET_MESH_ACK == 0x801Au, "Unexpected WAYWAL_RESP_PUPPET_MESH_ACK");
static_assert(WAYWAL_RESP_PUPPET_WEIGHTS == 0x801Bu, "Unexpected WAYWAL_RESP_PUPPET_WEIGHTS");
static_assert(WAYWAL_RESP_PUPPET_IK_POSE == 0x801Cu, "Unexpected WAYWAL_RESP_PUPPET_IK_POSE");
static_assert(WAYWAL_RESP_EXPORT_PUPPET_MDL == 0x801Du, "Unexpected WAYWAL_RESP_EXPORT_PUPPET_MDL");

static void fill_identity_mat4(float m[16])
{
    memset(m, 0, sizeof(float) * 16u);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void test_legacy_and_helpers(void)
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

    uint64_t bytes = 0u;
    assert(!waywal_ipc_rigging_calc_mesh_bytes(2u, 1u, &bytes));
    assert(!waywal_ipc_rigging_calc_mesh_bytes(3u, 0u, &bytes));
    assert(!waywal_ipc_rigging_calc_mesh_bytes(65537u, 1u, &bytes));
    assert(!waywal_ipc_rigging_calc_mesh_bytes(3u, 131073u, &bytes));
    assert(waywal_ipc_rigging_calc_mesh_bytes(3u, 1u, &bytes));
    assert(bytes == 16u + 3u * 52u + 6u);

    assert(!waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SET_MESH));
    assert(waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SET_SKELETON));
    assert(!waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS));
    assert(waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_PAINT_WEIGHTS));
    assert(waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE));
    assert(!waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_PREVIEW_IK));
    assert(waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS));
    assert(waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_PUPPET_SCRUB_TIME));
    assert(!waywal_ipc_rigging_expects_generic_ok(WAYWAL_REQ_EXPORT_PUPPET_MDL));
}

static void test_set_mesh_wire(void)
{
    waywal_ipc_mdl_vertex_t verts[4];
    memset(verts, 0, sizeof(verts));
    for (uint32_t i = 0u; i < 4u; ++i) {
        verts[i].pos[0] = (float)(i % 2u) * 100.0f;
        verts[i].pos[1] = (float)(i / 2u) * 100.0f;
        verts[i].pos[2] = 0.0f;
        verts[i].blend_indices[0] = 0u;
        verts[i].blend_indices[1] = 1u;
        verts[i].weight[0] = 0.75f;
        verts[i].weight[1] = 0.25f;
        verts[i].texcoord[0] = (float)(i % 2u);
        verts[i].texcoord[1] = (float)(i / 2u);
    }
    const uint16_t indices[6] = {0u, 1u, 2u, 1u, 3u, 2u};

    uint8_t buf[512];
    waywal_ipc_hdr_t hdr;
    size_t total = 0u;
    assert(waywal_ipc_serialize_req_puppet_set_mesh(42u, 3u, verts, 4u, indices, 2u, &hdr, buf,
                                                    sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_REQ_PUPPET_SET_MESH);
    assert(hdr.payload_size == total);
    assert(total == 16u + 4u * 52u + 2u * 6u);

    const waywal_ipc_puppet_set_mesh_t *req = NULL;
    const waywal_ipc_mdl_vertex_t *out_v = NULL;
    const uint16_t *out_i = NULL;
    assert(waywal_ipc_validate_req_puppet_set_mesh(buf, total, &req, &out_v, &out_i));
    assert(req->object_id == 42u && req->vertex_count == 4u && req->triangle_count == 2u);
    assert(out_v[3].pos[1] == 100.0f && out_i[4] == 3u);

    /* Truncated / oversized */
    assert(!waywal_ipc_validate_req_puppet_set_mesh(buf, total - 1u, &req, &out_v, &out_i));
    assert(!waywal_ipc_validate_req_puppet_set_mesh(buf, total + 2u, &req, &out_v, &out_i));

    /* Out-of-bounds triangle index */
    uint8_t bad[512];
    memcpy(bad, buf, total);
    uint16_t bad_idx = 4u;
    memcpy(bad + 16u + 4u * 52u + 4u * sizeof(uint16_t), &bad_idx, sizeof(bad_idx));
    assert(!waywal_ipc_validate_req_puppet_set_mesh(bad, total, &req, &out_v, &out_i));

    /* NaN vertex position */
    memcpy(bad, buf, total);
    float nan_v = NAN;
    memcpy(bad + 16u, &nan_v, sizeof(nan_v));
    assert(!waywal_ipc_validate_req_puppet_set_mesh(bad, total, &req, &out_v, &out_i));

    /* Negative weight */
    memcpy(bad, buf, total);
    float neg_w = -0.1f;
    memcpy(bad + 16u + offsetof(waywal_ipc_mdl_vertex_t, weight), &neg_w, sizeof(neg_w));
    assert(!waywal_ipc_validate_req_puppet_set_mesh(bad, total, &req, &out_v, &out_i));

    /* Empty clear mesh (0 vertices, 0 triangles) */
    assert(waywal_ipc_serialize_req_puppet_set_mesh(42u, 0u, NULL, 0u, NULL, 0u, &hdr, buf,
                                                    sizeof(buf), &total));
    assert(total == sizeof(waywal_ipc_puppet_set_mesh_t));
    assert(waywal_ipc_validate_req_puppet_set_mesh(buf, total, &req, &out_v, &out_i));
    assert(req->vertex_count == 0u && req->triangle_count == 0u && out_v == NULL && out_i == NULL);

    /* Response 0x801A */
    waywal_ipc_puppet_mesh_ack_t ack;
    assert(waywal_ipc_serialize_resp_puppet_mesh_ack(0, 42u, 4u, 2u, &hdr, &ack));
    assert(hdr.opcode == WAYWAL_RESP_PUPPET_MESH_ACK && hdr.payload_size == 16u);
    const waywal_ipc_puppet_mesh_ack_t *rack = NULL;
    assert(waywal_ipc_validate_resp_puppet_mesh_ack(&ack, sizeof(ack), &rack));
    assert(rack->vertex_count == 4u && rack->triangle_count == 2u);
    assert(!waywal_ipc_validate_resp_puppet_mesh_ack(&ack, sizeof(ack) - 1u, &rack));
}

static void test_set_skeleton_wire(void)
{
    waywal_ipc_mdl_bone_t bones[2];
    memset(bones, 0, sizeof(bones));
    snprintf(bones[0].name, sizeof(bones[0].name), "root");
    bones[0].parent = UINT32_MAX;
    fill_identity_mat4(bones[0].local_trans);
    snprintf(bones[0].simulation_json, sizeof(bones[0].simulation_json), "{}");

    snprintf(bones[1].name, sizeof(bones[1].name), "arm");
    bones[1].parent = 0u;
    fill_identity_mat4(bones[1].local_trans);
    bones[1].local_trans[12] = 32.0f;
    snprintf(bones[1].simulation_json, sizeof(bones[1].simulation_json), "{\"mass\":1.0}");

    uint8_t buf[2048];
    waywal_ipc_hdr_t hdr;
    size_t total = 0u;
    assert(waywal_ipc_serialize_req_puppet_set_skeleton(7u, 1u, bones, 2u, &hdr, buf, sizeof(buf),
                                                        &total));
    assert(hdr.opcode == WAYWAL_REQ_PUPPET_SET_SKELETON);
    assert(total == 16u + 2u * 456u);

    const waywal_ipc_puppet_set_skeleton_t *req = NULL;
    const waywal_ipc_mdl_bone_t *out_b = NULL;
    assert(waywal_ipc_validate_req_puppet_set_skeleton(buf, total, &req, &out_b));
    assert(req->bone_count == 2u && out_b[1].parent == 0u);

    /* Forward parent / cycle rejection */
    bones[0].parent = 1u;
    assert(!waywal_ipc_serialize_req_puppet_set_skeleton(7u, 1u, bones, 2u, &hdr, buf, sizeof(buf),
                                                         &total));
    bones[0].parent = UINT32_MAX;

    /* Self-parent rejection */
    bones[1].parent = 1u;
    assert(!waywal_ipc_serialize_req_puppet_set_skeleton(7u, 1u, bones, 2u, &hdr, buf, sizeof(buf),
                                                         &total));
    bones[1].parent = 0u;

    /* Unterminated name rejection on wire */
    uint8_t bad[2048];
    memcpy(bad, buf, total);
    memset(bad + 16u, 'A', 128u);
    assert(!waywal_ipc_validate_req_puppet_set_skeleton(bad, total, &req, &out_b));

    /* Zero w component on matrix */
    memcpy(bad, buf, total);
    float zero_w = 0.0f;
    memcpy(bad + 16u + offsetof(waywal_ipc_mdl_bone_t, local_trans) + 15u * sizeof(float), &zero_w,
           sizeof(zero_w));
    assert(!waywal_ipc_validate_req_puppet_set_skeleton(bad, total, &req, &out_b));
}

static void test_weights_and_heatmap_wire(void)
{
    waywal_ipc_puppet_solve_weights_t sw = {
        .object_id = 9u,
        .falloff_power = 2.0f,
        .smoothness = 0.5f,
        .max_influences = 4u,
        .flags = 0xFu,
        .locked_vert_word_count = 1u,
    };
    uint64_t locked[1] = {0x3ull};
    uint8_t buf[256];
    waywal_ipc_hdr_t hdr;
    size_t total = 0u;
    assert(
        waywal_ipc_serialize_req_puppet_solve_weights(&sw, locked, &hdr, buf, sizeof(buf), &total));
    assert(total == 24u + 8u);

    const waywal_ipc_puppet_solve_weights_t *out_sw = NULL;
    const uint64_t *out_bs = NULL;
    assert(waywal_ipc_validate_req_puppet_solve_weights(buf, total, &out_sw, &out_bs));
    assert(out_bs[0] == 0x3ull);

    /* Mismatched locked_vert_word_count vs flags bit 1 */
    sw.flags = 0x1u;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights(&sw, locked, &hdr, buf, sizeof(buf),
                                                          &total));
    sw.flags = 0x2u;
    sw.locked_vert_word_count = 0u;
    assert(
        !waywal_ipc_serialize_req_puppet_solve_weights(&sw, NULL, &hdr, buf, sizeof(buf), &total));

    /* Bounds & NaN */
    sw.locked_vert_word_count = 1u;
    sw.falloff_power = 0.25f;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights(&sw, locked, &hdr, buf, sizeof(buf),
                                                          &total));
    sw.falloff_power = NAN;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights(&sw, locked, &hdr, buf, sizeof(buf),
                                                          &total));

    /* Paint & weights resp 0x002E / 0x801B */
    waywal_ipc_vertex_weight_t entries[2] = {
        {.vertex_index = 0u, .blend_indices = {0u, 1u, 0u, 0u}, .weight = {0.6f, 0.4f, 0.0f, 0.0f}},
        {.vertex_index = 3u, .blend_indices = {1u, 0u, 0u, 0u}, .weight = {1.0f, 0.0f, 0.0f, 0.0f}},
    };
    assert(waywal_ipc_serialize_req_puppet_paint_weights(9u, 1u, entries, 2u, &hdr, buf,
                                                         sizeof(buf), &total));
    assert(total == 16u + 2u * 36u);
    const waywal_ipc_puppet_paint_weights_t *out_pw = NULL;
    const waywal_ipc_vertex_weight_t *out_ent = NULL;
    assert(waywal_ipc_validate_req_puppet_paint_weights(buf, total, &out_pw, &out_ent));
    assert(out_ent[1].vertex_index == 3u);

    assert(waywal_ipc_serialize_resp_puppet_weights(0, 9u, entries, 2u, &hdr, buf, sizeof(buf),
                                                    &total));
    assert(hdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    const waywal_ipc_puppet_weights_resp_t *out_wr = NULL;
    assert(waywal_ipc_validate_resp_puppet_weights(buf, total, &out_wr, &out_ent));
    assert(out_wr->entry_count == 2u);

    /* Heatmap 0x002F */
    waywal_ipc_puppet_heatmap_mode_t hm = {
        .object_id = 9u,
        .enabled = 1u,
        .palette = 2u,
        .active_bone_idx = 1u,
        .opacity = 0.8f,
    };
    waywal_ipc_puppet_heatmap_mode_t hm_out;
    assert(waywal_ipc_serialize_req_puppet_heatmap_mode(&hm, &hdr, &hm_out));
    const waywal_ipc_puppet_heatmap_mode_t *hm_p = NULL;
    assert(waywal_ipc_validate_req_puppet_heatmap_mode(&hm_out, sizeof(hm_out), &hm_p));
    assert(hm_p->palette == 2u);

    hm.opacity = 1.5f;
    assert(!waywal_ipc_serialize_req_puppet_heatmap_mode(&hm, &hdr, &hm_out));
    hm.opacity = 0.5f;
    hm.palette = 3u;
    assert(!waywal_ipc_serialize_req_puppet_heatmap_mode(&hm, &hdr, &hm_out));
}

static void test_ik_anim_scrub_export_wire(void)
{
    uint8_t buf[1024];
    waywal_ipc_hdr_t hdr;
    size_t total = 0u;

    /* 0x0030 & 0x801C */
    waywal_ipc_puppet_preview_ik_t ik = {
        .object_id = 5u,
        .root_bone = 0u,
        .mid_bone = 1u,
        .end_bone = 2u,
        .pole_flip = 1u,
        .target_world = {10.0f, 20.0f, 0.0f},
        .override_count = 1u,
        .mode_flags = 5u,
    };
    waywal_ipc_bone_trs_t trs = {
        .bone_index = 0u,
        .local_pos = {1.0f, 2.0f, 0.0f},
        .local_angle = {0.0f, 0.0f, 0.5f},
        .local_scale = {1.0f, 1.0f, 1.0f},
    };
    assert(waywal_ipc_serialize_req_puppet_preview_ik(&ik, &trs, &hdr, buf, sizeof(buf), &total));
    assert(total == 32u + 40u);
    const waywal_ipc_puppet_preview_ik_t *out_ik = NULL;
    const waywal_ipc_bone_trs_t *out_trs = NULL;
    assert(waywal_ipc_validate_req_puppet_preview_ik(buf, total, &out_ik, &out_trs));
    assert(out_trs[0].local_angle[2] == 0.5f);

    ik.target_world[1] = INFINITY;
    assert(!waywal_ipc_serialize_req_puppet_preview_ik(&ik, &trs, &hdr, buf, sizeof(buf), &total));

    assert(
        waywal_ipc_serialize_resp_puppet_ik_pose(0, 5u, &trs, 1u, &hdr, buf, sizeof(buf), &total));
    assert(hdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE);
    const waywal_ipc_puppet_ik_pose_resp_t *out_ikr = NULL;
    assert(waywal_ipc_validate_resp_puppet_ik_pose(buf, total, &out_ikr, &out_trs));
    assert(out_ikr->bone_count == 1u);

    /* 0x0031 nested anim clips -> tracks -> keyframes */
    uint8_t blob[512];
    size_t bcur = 0u;
    waywal_ipc_anim_clip_desc_t clip;
    memset(&clip, 0, sizeof(clip));
    snprintf(clip.name, sizeof(clip.name), "idle");
    snprintf(clip.mode_str, sizeof(clip.mode_str), "loop");
    clip.id = 1;
    clip.fps = 30.0f;
    clip.length = 2;
    clip.track_count = 1u;
    clip.layer_blend = 1.0f;
    clip.layer_rate = 1.0f;
    clip.layer_visible = 1u;
    clip.layer_blend_mode = 0u;
    memcpy(blob + bcur, &clip, sizeof(clip));
    bcur += sizeof(clip);

    waywal_ipc_anim_track_desc_t tr = {.bone_index = 0, .keyframe_count = 2u};
    memcpy(blob + bcur, &tr, sizeof(tr));
    bcur += sizeof(tr);

    waywal_ipc_mdl_keyframe_t kfs[2] = {
        {.pos = {0.0f, 0.0f, 0.0f}, .angle = {0.0f, 0.0f, 0.0f}, .scale = {1.0f, 1.0f, 1.0f}},
        {.pos = {5.0f, 0.0f, 0.0f}, .angle = {0.0f, 0.0f, 0.2f}, .scale = {1.0f, 1.0f, 1.0f}},
    };
    memcpy(blob + bcur, kfs, sizeof(kfs));
    bcur += sizeof(kfs);

    assert(waywal_ipc_serialize_req_puppet_set_anim_tracks(5u, 1u, 3u, blob, bcur, &hdr, buf,
                                                           sizeof(buf), &total));
    const waywal_ipc_puppet_set_anim_tracks_t *out_anim = NULL;
    const uint8_t *out_blob = NULL;
    size_t out_blob_len = 0u;
    assert(waywal_ipc_validate_req_puppet_set_anim_tracks(buf, total, &out_anim, &out_blob,
                                                          &out_blob_len));
    assert(out_blob_len == bcur);

    /* Trailing byte rejection */
    buf[total] = 0u;
    assert(!waywal_ipc_validate_req_puppet_set_anim_tracks(buf, total + 1u, &out_anim, &out_blob,
                                                           &out_blob_len));

    /* Bad mode_str */
    snprintf(clip.mode_str, sizeof(clip.mode_str), "bogus");
    memcpy(blob, &clip, sizeof(clip));
    assert(!waywal_ipc_serialize_req_puppet_set_anim_tracks(5u, 1u, 3u, blob, bcur, &hdr, buf,
                                                            sizeof(buf), &total));

    /* 0x0032 scrub time */
    waywal_ipc_puppet_scrub_t sc = {
        .object_id = 5u, .time_sec = 1.25f, .pause_playback = 1u, .flags = 1u};
    waywal_ipc_puppet_scrub_t sc_out;
    assert(waywal_ipc_serialize_req_puppet_scrub_time(&sc, &hdr, &sc_out));
    const waywal_ipc_puppet_scrub_t *sc_p = NULL;
    assert(waywal_ipc_validate_req_puppet_scrub_time(&sc_out, sizeof(sc_out), &sc_p));
    assert(sc_p->time_sec == 1.25f);
    sc.time_sec = -0.1f;
    assert(!waywal_ipc_serialize_req_puppet_scrub_time(&sc, &hdr, &sc_out));

    /* 0x0033 export mdl & 0x801D */
    waywal_ipc_export_puppet_mdl_t exp_req;
    assert(waywal_ipc_serialize_req_export_puppet_mdl(5u, 3u, "/tmp/wwal-export-test.mdl", &hdr,
                                                      &exp_req));
    const waywal_ipc_export_puppet_mdl_t *exp_p = NULL;
    assert(waywal_ipc_validate_req_export_puppet_mdl(&exp_req, sizeof(exp_req), &exp_p));

    /* Path traversal & non-allowlisted rejection */
    assert(!waywal_ipc_serialize_req_export_puppet_mdl(5u, 3u, "/tmp/wwal-export-foo/../evil.mdl",
                                                       &hdr, &exp_req));
    assert(!waywal_ipc_serialize_req_export_puppet_mdl(5u, 3u, "/tmp/evil.mdl", &hdr, &exp_req));
    assert(!waywal_ipc_serialize_req_export_puppet_mdl(5u, 3u, "/etc/passwd", &hdr, &exp_req));
    assert(!waywal_ipc_serialize_req_export_puppet_mdl(5u, 2u, "", &hdr, &exp_req));

    /* Inline-only flag=1 with empty path is allowed */
    assert(waywal_ipc_serialize_req_export_puppet_mdl(5u, 1u, "", &hdr, &exp_req));
    assert(waywal_ipc_validate_req_export_puppet_mdl(&exp_req, sizeof(exp_req), &exp_p));

    static const uint8_t k_fake_mdl[12] = {'M', 'D', 'L', 'V', '0', '0', '2', '3', 0, 0, 0, 0};
    assert(waywal_ipc_serialize_resp_export_puppet_mdl(0, 5u, k_fake_mdl, sizeof(k_fake_mdl), &hdr,
                                                       buf, sizeof(buf), &total));
    const waywal_ipc_export_puppet_mdl_resp_t *out_er = NULL;
    const uint8_t *out_mdl = NULL;
    assert(waywal_ipc_validate_resp_export_puppet_mdl(buf, total, &out_er, &out_mdl));
    assert(out_er->mdl_byte_size == sizeof(k_fake_mdl));
    assert(memcmp(out_mdl, k_fake_mdl, sizeof(k_fake_mdl)) == 0);

    /* Write-only response (0 trailing inline bytes, byte size recorded, including >8 MiB) */
    assert(waywal_ipc_serialize_resp_export_puppet_mdl(0, 5u, NULL, 128u, &hdr, buf, sizeof(buf),
                                                       &total));
    assert(total == sizeof(waywal_ipc_export_puppet_mdl_resp_t));
    assert(waywal_ipc_validate_resp_export_puppet_mdl(buf, total, &out_er, &out_mdl));
    assert(out_er->mdl_byte_size == 128u && out_mdl == NULL);

    const uint32_t over_8mib = WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES + 4096u;
    assert(waywal_ipc_serialize_resp_export_puppet_mdl(0, 5u, NULL, over_8mib, &hdr, buf,
                                                       sizeof(buf), &total));
    assert(total == sizeof(waywal_ipc_export_puppet_mdl_resp_t));
    assert(waywal_ipc_validate_resp_export_puppet_mdl(buf, total, &out_er, &out_mdl));
    assert(out_er->mdl_byte_size == over_8mib && out_mdl == NULL);

    /* Inline blob exceeding 8 MiB payload limit must still be rejected. */
    assert(!waywal_ipc_serialize_resp_export_puppet_mdl(0, 5u, k_fake_mdl, over_8mib, &hdr, buf,
                                                        sizeof(buf), &total));
}

static void test_rigging_ext_wire(void)
{
    uint8_t buf[1024];
    waywal_ipc_hdr_t hdr;
    size_t total = 0u;

    /* SOLVE_WEIGHTS_EXT: legacy path returns *out_ext == NULL */
    waywal_ipc_puppet_solve_weights_t sw = {.object_id = 7u,
                                            .falloff_power = 2.0f,
                                            .smoothness = 0.25f,
                                            .max_influences = 4u,
                                            .flags = 0x1u | WAYWAL_IPC_SOLVE_WEIGHTS_FLAG_EXT,
                                            .locked_vert_word_count = 0u};
    const waywal_ipc_puppet_solve_weights_t *out_sw = NULL;
    const uint64_t *out_bs = NULL;
    const waywal_ipc_puppet_solve_weights_ext_t *out_sw_ext = NULL;
    assert(waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, NULL, NULL, &hdr, buf,
                                                             sizeof(buf), &total));
    assert(total == sizeof(waywal_ipc_puppet_solve_weights_t));
    assert(waywal_ipc_validate_req_puppet_solve_weights_ext(buf, total, &out_sw, &out_bs,
                                                            &out_sw_ext));
    assert((out_sw->flags & WAYWAL_IPC_SOLVE_WEIGHTS_FLAG_EXT) == 0u);
    assert(out_sw_ext == NULL);

    /* SOLVE_WEIGHTS_EXT: _ext block + locked bitmap round-trip */
    sw.flags = 0x2u | 0x4u;
    sw.locked_vert_word_count = 1u;
    uint64_t locked_mask = 0x55AAu;
    waywal_ipc_puppet_solve_weights_ext_t sw_ext = {.conductivity_scale = 3.25f};
    assert(waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, &locked_mask, &sw_ext, &hdr, buf,
                                                             sizeof(buf), &total));
    assert(total == sizeof(waywal_ipc_puppet_solve_weights_t) + sizeof(uint64_t) +
                        sizeof(waywal_ipc_puppet_solve_weights_ext_t));
    assert(!waywal_ipc_validate_req_puppet_solve_weights(buf, total, &out_sw, &out_bs));
    assert(waywal_ipc_validate_req_puppet_solve_weights_ext(buf, total, &out_sw, &out_bs,
                                                            &out_sw_ext));
    assert((out_sw->flags & WAYWAL_IPC_SOLVE_WEIGHTS_FLAG_EXT) != 0u);
    assert(out_bs && out_bs[0] == 0x55AAu);
    assert(out_sw_ext != NULL && fabsf(out_sw_ext->conductivity_scale - 3.25f) < 1e-6f);

    /* Reject truncated _ext, bad flag pairing, and out-of-range conductivity */
    assert(!waywal_ipc_validate_req_puppet_solve_weights_ext(buf, total - 1u, &out_sw, &out_bs,
                                                             &out_sw_ext));
    buf[total] = 0u;
    assert(!waywal_ipc_validate_req_puppet_solve_weights_ext(buf, total + 1u, &out_sw, &out_bs,
                                                             &out_sw_ext));
    sw_ext.conductivity_scale = -0.1f;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, &locked_mask, &sw_ext, &hdr, buf,
                                                              sizeof(buf), &total));
    sw_ext.conductivity_scale = 101.0f;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, &locked_mask, &sw_ext, &hdr, buf,
                                                              sizeof(buf), &total));
    sw_ext.conductivity_scale = NAN;
    assert(!waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, &locked_mask, &sw_ext, &hdr, buf,
                                                              sizeof(buf), &total));

    /* PREVIEW_IK_EXT: legacy path returns *out_ext == NULL */
    waywal_ipc_puppet_preview_ik_t ik = {.object_id = 5u,
                                         .root_bone = 1u,
                                         .mid_bone = 2u,
                                         .end_bone = 3u,
                                         .pole_flip = 0u,
                                         .target_world = {20.0f, 10.0f, 0.0f},
                                         .override_count = 1u,
                                         .mode_flags =
                                             WAYWAL_IPC_IK_FLAG_ENABLE |
                                             WAYWAL_IPC_IK_FLAG_HAS_POLE_EXT};
    waywal_ipc_bone_trs_t trs = {.bone_index = 0u,
                                 .local_pos = {0.0f, 0.0f, 0.0f},
                                 .local_angle = {0.0f, 0.0f, 0.2f},
                                 .local_scale = {1.0f, 1.0f, 1.0f}};
    const waywal_ipc_puppet_preview_ik_t *out_ik = NULL;
    const waywal_ipc_bone_trs_t *out_ov = NULL;
    const waywal_ipc_puppet_preview_ik_ext_t *out_ik_ext = NULL;
    assert(
        waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, NULL, &hdr, buf, sizeof(buf),
                                                       &total));
    assert(total == sizeof(ik) + sizeof(trs));
    assert(waywal_ipc_validate_req_puppet_preview_ik_ext(buf, total, &out_ik, &out_ov, &out_ik_ext));
    assert((out_ik->mode_flags & WAYWAL_IPC_IK_FLAG_HAS_POLE_EXT) == 0u);
    assert(out_ik_ext == NULL);

    /* PREVIEW_IK_EXT: full _ext block round-trip + rejection checks */
    waywal_ipc_puppet_preview_ik_ext_t ik_ext = {
        .pole_world = {-12.0f, 34.0f, 5.0f},
        .soft_zone_ratio = 0.20f,
        .max_stretch_ratio = 1.40f,
        .root_angle_min_rad = -0.6f,
        .root_angle_max_rad = 0.8f,
        .mid_angle_min_rad = -1.2f,
        .mid_angle_max_rad = 0.3f,
        .aim_axis_local = {0.0f, 1.0f, 0.0f},
        .aim_max_cone_rad = 0.5f,
        .aim_blend_weight = 0.75f,
        .ik_blend_weight = 0.85f,
        .ext_flags = WAYWAL_IPC_IK_EXT_USE_POLE | WAYWAL_IPC_IK_EXT_SOFT_IK |
                     WAYWAL_IPC_IK_EXT_ALLOW_STRETCH | WAYWAL_IPC_IK_EXT_LIMIT_ANGLES |
                     WAYWAL_IPC_IK_EXT_AIM_ENABLED,
    };
    ik.mode_flags = WAYWAL_IPC_IK_FLAG_ENABLE;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, &ik_ext, &hdr, buf,
                                                          sizeof(buf), &total));
    assert(total == sizeof(ik) + sizeof(trs) + sizeof(ik_ext));
    assert(!waywal_ipc_validate_req_puppet_preview_ik(buf, total, &out_ik, &out_ov));
    assert(waywal_ipc_validate_req_puppet_preview_ik_ext(buf, total, &out_ik, &out_ov, &out_ik_ext));
    assert((out_ik->mode_flags & WAYWAL_IPC_IK_FLAG_HAS_POLE_EXT) != 0u);
    assert(out_ik_ext != NULL);
    assert(fabsf(out_ik_ext->pole_world[0] - (-12.0f)) < 1e-6f);
    assert(fabsf(out_ik_ext->pole_world[1] - 34.0f) < 1e-6f);
    assert(fabsf(out_ik_ext->soft_zone_ratio - 0.20f) < 1e-6f);
    assert(fabsf(out_ik_ext->max_stretch_ratio - 1.40f) < 1e-6f);
    assert(fabsf(out_ik_ext->root_angle_min_rad - (-0.6f)) < 1e-6f);
    assert(fabsf(out_ik_ext->mid_angle_max_rad - 0.3f) < 1e-6f);
    assert(fabsf(out_ik_ext->aim_axis_local[1] - 1.0f) < 1e-6f);
    assert(fabsf(out_ik_ext->aim_max_cone_rad - 0.5f) < 1e-6f);
    assert(fabsf(out_ik_ext->aim_blend_weight - 0.75f) < 1e-6f);
    assert(fabsf(out_ik_ext->ik_blend_weight - 0.85f) < 1e-6f);
    assert(out_ik_ext->ext_flags == ik_ext.ext_flags);

    assert(!waywal_ipc_validate_req_puppet_preview_ik_ext(buf, total - 4u, &out_ik, &out_ov,
                                                          &out_ik_ext));
    waywal_ipc_puppet_preview_ik_ext_t bad_ext = ik_ext;
    bad_ext.ext_flags = 0x20u;
    assert(!waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, &bad_ext, &hdr, buf,
                                                           sizeof(buf), &total));
    bad_ext = ik_ext;
    bad_ext.root_angle_min_rad = 1.0f;
    bad_ext.root_angle_max_rad = -1.0f;
    assert(!waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, &bad_ext, &hdr, buf,
                                                           sizeof(buf), &total));
    bad_ext = ik_ext;
    bad_ext.max_stretch_ratio = 0.5f;
    assert(!waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, &bad_ext, &hdr, buf,
                                                           sizeof(buf), &total));
    bad_ext = ik_ext;
    bad_ext.aim_max_cone_rad = 3.5f;
    assert(!waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, &trs, &bad_ext, &hdr, buf,
                                                           sizeof(buf), &total));
}

static bool wire_read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0u;
    while (got < len) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 2000) <= 0) {
            return false;
        }
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
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

static bool wire_write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0u;
    while (sent < len) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
        if (poll(&pfd, 1, 2000) <= 0) {
            return false;
        }
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool wire_send_packet(int fd, const waywal_ipc_hdr_t *hdr, const void *payload,
                             size_t payload_len)
{
    return wire_write_exact(fd, hdr, sizeof(*hdr)) &&
           (payload_len == 0u || wire_write_exact(fd, payload, payload_len));
}

static bool wire_recv_packet(int fd, waywal_ipc_hdr_t *out_hdr, uint8_t *out_payload,
                             size_t payload_cap)
{
    if (!wire_read_exact(fd, out_hdr, sizeof(*out_hdr))) {
        return false;
    }
    if (out_hdr->magic != WAYWAL_IPC_MAGIC || out_hdr->version != WAYWAL_IPC_VERSION ||
        out_hdr->payload_size > payload_cap) {
        return false;
    }
    if (out_hdr->payload_size > 0u && !wire_read_exact(fd, out_payload, out_hdr->payload_size)) {
        return false;
    }
    return true;
}

static void run_mock_rig_server(int fd)
{
    uint8_t req_buf[8192];
    uint8_t resp_buf[8192];
    for (int step = 0; step < 11; ++step) {
        waywal_ipc_hdr_t hdr;
        if (!wire_recv_packet(fd, &hdr, req_buf, sizeof(req_buf))) {
            _exit(2);
        }
        waywal_ipc_hdr_t rh = {0};
        size_t rlen = 0u;
        bool ok = false;

        switch (hdr.opcode) {
        case WAYWAL_REQ_PUPPET_SET_MESH: {
            const waywal_ipc_puppet_set_mesh_t *r = NULL;
            const waywal_ipc_mdl_vertex_t *v = NULL;
            const uint16_t *idx = NULL;
            ok = waywal_ipc_validate_req_puppet_set_mesh(req_buf, hdr.payload_size, &r, &v, &idx);
            if (ok) {
                waywal_ipc_puppet_mesh_ack_t ack;
                ok = waywal_ipc_serialize_resp_puppet_mesh_ack(0, r->object_id, r->vertex_count,
                                                               r->triangle_count, &rh, &ack);
                memcpy(resp_buf, &ack, sizeof(ack));
                rlen = sizeof(ack);
            }
            break;
        }
        case WAYWAL_REQ_PUPPET_SET_SKELETON: {
            const waywal_ipc_puppet_set_skeleton_t *r = NULL;
            const waywal_ipc_mdl_bone_t *b = NULL;
            ok = waywal_ipc_validate_req_puppet_set_skeleton(req_buf, hdr.payload_size, &r, &b);
            break;
        }
        case WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS: {
            const waywal_ipc_puppet_solve_weights_t *r = NULL;
            const uint64_t *bs = NULL;
            const waywal_ipc_puppet_solve_weights_ext_t *ext = NULL;
            ok = waywal_ipc_validate_req_puppet_solve_weights_ext(req_buf, hdr.payload_size, &r,
                                                                 &bs, &ext);
            if (ok) {
                float w0 = (ext && ext->conductivity_scale > 1.5f) ? 0.25f : 1.0f;
                waywal_ipc_vertex_weight_t ent = {
                    .vertex_index = 0u,
                    .blend_indices = {0u, 1u, 0u, 0u},
                    .weight = {w0, 1.0f - w0, 0.0f, 0.0f},
                };
                ok = waywal_ipc_serialize_resp_puppet_weights(0, r->object_id, &ent, 1u, &rh,
                                                              resp_buf, sizeof(resp_buf), &rlen);
            }
            break;
        }
        case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS: {
            const waywal_ipc_puppet_paint_weights_t *r = NULL;
            const waywal_ipc_vertex_weight_t *ent = NULL;
            ok = waywal_ipc_validate_req_puppet_paint_weights(req_buf, hdr.payload_size, &r, &ent);
            break;
        }
        case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE: {
            const waywal_ipc_puppet_heatmap_mode_t *r = NULL;
            ok = waywal_ipc_validate_req_puppet_heatmap_mode(req_buf, hdr.payload_size, &r);
            break;
        }
        case WAYWAL_REQ_PUPPET_PREVIEW_IK: {
            const waywal_ipc_puppet_preview_ik_t *r = NULL;
            const waywal_ipc_bone_trs_t *ov = NULL;
            const waywal_ipc_puppet_preview_ik_ext_t *ext = NULL;
            ok = waywal_ipc_validate_req_puppet_preview_ik_ext(req_buf, hdr.payload_size, &r, &ov,
                                                             &ext);
            if (ok) {
                waywal_ipc_bone_trs_t pose = {.bone_index = 0u, .local_scale = {1.0f, 1.0f, 1.0f}};
                if (ext && (ext->ext_flags & WAYWAL_IPC_IK_EXT_USE_POLE) != 0u) {
                    memcpy(pose.local_pos, ext->pole_world, sizeof(pose.local_pos));
                }
                ok = waywal_ipc_serialize_resp_puppet_ik_pose(0, r->object_id, &pose, 1u, &rh,
                                                              resp_buf, sizeof(resp_buf), &rlen);
            }
            break;
        }
        case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS: {
            const waywal_ipc_puppet_set_anim_tracks_t *r = NULL;
            const uint8_t *blob = NULL;
            size_t blen = 0u;
            ok = waywal_ipc_validate_req_puppet_set_anim_tracks(req_buf, hdr.payload_size, &r,
                                                                &blob, &blen);
            break;
        }
        case WAYWAL_REQ_PUPPET_SCRUB_TIME: {
            const waywal_ipc_puppet_scrub_t *r = NULL;
            ok = waywal_ipc_validate_req_puppet_scrub_time(req_buf, hdr.payload_size, &r);
            break;
        }
        case WAYWAL_REQ_EXPORT_PUPPET_MDL: {
            const waywal_ipc_export_puppet_mdl_t *r = NULL;
            ok = waywal_ipc_validate_req_export_puppet_mdl(req_buf, hdr.payload_size, &r);
            if (ok) {
                static const uint8_t k_blob[12] = {'M', 'D', 'L', 'V', '0', '0',
                                                   '2', '3', 0,   0,   0,   0};
                ok = waywal_ipc_serialize_resp_export_puppet_mdl(0, r->object_id, k_blob,
                                                                 sizeof(k_blob), &rh, resp_buf,
                                                                 sizeof(resp_buf), &rlen);
            }
            break;
        }
        default:
            ok = false;
            break;
        }

        if (!ok) {
            _exit(3);
        }
        if (waywal_ipc_rigging_expects_generic_ok(hdr.opcode)) {
            rh.magic = WAYWAL_IPC_MAGIC;
            rh.version = WAYWAL_IPC_VERSION;
            rh.opcode = WAYWAL_RESP_OK;
            rh.payload_size = 0u;
            rlen = 0u;
        }
        if (!wire_send_packet(fd, &rh, resp_buf, rlen)) {
            _exit(4);
        }
    }
    _exit(0);
}

static void test_unix_socketpair_rigging_roundtrip(void)
{
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        run_mock_rig_server(sv[1]);
        _exit(1);
    }
    close(sv[1]);
    int cfd = sv[0];

    uint8_t tx[8192];
    uint8_t rx[8192];
    waywal_ipc_hdr_t hdr, rhdr;
    size_t total = 0u;

    /* 0x002B SET_MESH -> 0x801A */
    waywal_ipc_mdl_vertex_t verts[3];
    memset(verts, 0, sizeof(verts));
    verts[1].pos[0] = 10.0f;
    verts[2].pos[1] = 10.0f;
    for (int i = 0; i < 3; ++i) {
        verts[i].weight[0] = 1.0f;
    }
    const uint16_t indices[3] = {0u, 1u, 2u};
    assert(waywal_ipc_serialize_req_puppet_set_mesh(7u, 1u, verts, 3u, indices, 1u, &hdr, tx,
                                                    sizeof(tx), &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_MESH_ACK);
    const waywal_ipc_puppet_mesh_ack_t *ack = NULL;
    assert(waywal_ipc_validate_resp_puppet_mesh_ack(rx, rhdr.payload_size, &ack));
    assert(ack->object_id == 7u && ack->vertex_count == 3u && ack->triangle_count == 1u);

    /* 0x002C SET_SKELETON -> RESP_OK */
    waywal_ipc_mdl_bone_t bone;
    memset(&bone, 0, sizeof(bone));
    snprintf(bone.name, sizeof(bone.name), "root");
    bone.parent = UINT32_MAX;
    fill_identity_mat4(bone.local_trans);
    assert(waywal_ipc_serialize_req_puppet_set_skeleton(7u, 1u, &bone, 1u, &hdr, tx, sizeof(tx),
                                                        &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 0x002D SOLVE_WEIGHTS (legacy) -> 0x801B */
    waywal_ipc_puppet_solve_weights_t sw = {.object_id = 7u,
                                            .falloff_power = 2.0f,
                                            .smoothness = 0.25f,
                                            .max_influences = 4u,
                                            .flags = 1u};
    assert(waywal_ipc_serialize_req_puppet_solve_weights(&sw, NULL, &hdr, tx, sizeof(tx), &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    const waywal_ipc_puppet_weights_resp_t *wr = NULL;
    const waywal_ipc_vertex_weight_t *we = NULL;
    assert(waywal_ipc_validate_resp_puppet_weights(rx, rhdr.payload_size, &wr, &we));
    assert(wr->object_id == 7u && wr->entry_count == 1u);
    assert(fabsf(we[0].weight[0] - 1.0f) < 1e-6f);

    /* 0x002D SOLVE_WEIGHTS (_ext) -> 0x801B */
    waywal_ipc_puppet_solve_weights_ext_t sw_ext = {.conductivity_scale = 2.0f};
    assert(waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw, NULL, &sw_ext, &hdr, tx,
                                                             sizeof(tx), &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    assert(waywal_ipc_validate_resp_puppet_weights(rx, rhdr.payload_size, &wr, &we));
    assert(wr->object_id == 7u && wr->entry_count == 1u);
    assert(fabsf(we[0].weight[0] - 0.25f) < 1e-6f);

    /* 0x002E PAINT_WEIGHTS -> RESP_OK */
    waywal_ipc_vertex_weight_t paint = {
        .vertex_index = 0u, .blend_indices = {0u, 0u, 0u, 0u}, .weight = {1.0f, 0.0f, 0.0f, 0.0f}};
    assert(waywal_ipc_serialize_req_puppet_paint_weights(7u, 1u, &paint, 1u, &hdr, tx, sizeof(tx),
                                                         &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 0x002F SET_HEATMAP_MODE -> RESP_OK */
    waywal_ipc_puppet_heatmap_mode_t hm = {
        .object_id = 7u, .enabled = 1u, .palette = 0u, .active_bone_idx = 0u, .opacity = 0.7f};
    waywal_ipc_puppet_heatmap_mode_t hm_pay;
    assert(waywal_ipc_serialize_req_puppet_heatmap_mode(&hm, &hdr, &hm_pay));
    assert(wire_send_packet(cfd, &hdr, &hm_pay, hdr.payload_size));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 0x0030 PREVIEW_IK (legacy) -> 0x801C */
    waywal_ipc_puppet_preview_ik_t ik = {.object_id = 7u,
                                         .root_bone = 0u,
                                         .mid_bone = 0xFFFFu,
                                         .end_bone = 0xFFFFu,
                                         .mode_flags = 1u};
    assert(waywal_ipc_serialize_req_puppet_preview_ik(&ik, NULL, &hdr, tx, sizeof(tx), &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE);
    const waywal_ipc_puppet_ik_pose_resp_t *ikr = NULL;
    const waywal_ipc_bone_trs_t *ikb = NULL;
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->object_id == 7u && ikr->bone_count == 1u);

    /* 0x0030 PREVIEW_IK (_ext) -> 0x801C */
    waywal_ipc_puppet_preview_ik_ext_t ik_ext = {
        .pole_world = {-10.0f, 25.0f, 0.0f},
        .soft_zone_ratio = 0.1f,
        .max_stretch_ratio = 1.2f,
        .ik_blend_weight = 1.0f,
        .ext_flags = WAYWAL_IPC_IK_EXT_USE_POLE | WAYWAL_IPC_IK_EXT_SOFT_IK |
                     WAYWAL_IPC_IK_EXT_ALLOW_STRETCH,
    };
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik, NULL, &ik_ext, &hdr, tx, sizeof(tx),
                                                          &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE);
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->object_id == 7u && ikr->bone_count == 1u);
    assert(fabsf(ikb[0].local_pos[0] - (-10.0f)) < 1e-6f);
    assert(fabsf(ikb[0].local_pos[1] - 25.0f) < 1e-6f);

    /* 0x0031 SET_ANIM_TRACKS -> RESP_OK */
    assert(waywal_ipc_serialize_req_puppet_set_anim_tracks(7u, 0u, 1u, NULL, 0u, &hdr, tx,
                                                           sizeof(tx), &total));
    assert(wire_send_packet(cfd, &hdr, tx, total));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 0x0032 SCRUB_TIME -> RESP_OK */
    waywal_ipc_puppet_scrub_t sc = {
        .object_id = 7u, .time_sec = 0.5f, .pause_playback = 1u, .flags = 2u};
    waywal_ipc_puppet_scrub_t sc_pay;
    assert(waywal_ipc_serialize_req_puppet_scrub_time(&sc, &hdr, &sc_pay));
    assert(wire_send_packet(cfd, &hdr, &sc_pay, hdr.payload_size));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 0x0033 EXPORT_PUPPET_MDL -> 0x801D */
    waywal_ipc_export_puppet_mdl_t exp_pay;
    assert(waywal_ipc_serialize_req_export_puppet_mdl(7u, 1u, "", &hdr, &exp_pay));
    assert(wire_send_packet(cfd, &hdr, &exp_pay, hdr.payload_size));
    assert(wire_recv_packet(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_EXPORT_PUPPET_MDL);
    const waywal_ipc_export_puppet_mdl_resp_t *er = NULL;
    const uint8_t *eb = NULL;
    assert(waywal_ipc_validate_resp_export_puppet_mdl(rx, rhdr.payload_size, &er, &eb));
    assert(er->object_id == 7u && er->mdl_byte_size == 12u && eb != NULL);

    close(cfd);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static bool rig_read_exact_nb(int fd, void *buf, size_t len)
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

static bool rig_recv_packet_nb(int fd, waywal_ipc_hdr_t *out_hdr, uint8_t *out_payload,
                               size_t payload_cap)
{
    if (!rig_read_exact_nb(fd, out_hdr, sizeof(*out_hdr))) {
        return false;
    }
    if (out_hdr->magic != WAYWAL_IPC_MAGIC || out_hdr->version != WAYWAL_IPC_VERSION ||
        out_hdr->payload_size > payload_cap) {
        return false;
    }
    if (out_hdr->payload_size > 0u && !rig_read_exact_nb(fd, out_payload, out_hdr->payload_size)) {
        return false;
    }
    return true;
}

static void run_scrub_parity_server(int fd)
{
    uint8_t req_buf[1024];
    uint8_t resp_buf[1024];
    for (int step = 0; step < 5; ++step) {
        waywal_ipc_hdr_t hdr = {0};
        if (!rig_recv_packet_nb(fd, &hdr, req_buf, sizeof(req_buf))) {
            _exit(2);
        }

        waywal_ipc_hdr_t rh = {
            .magic = WAYWAL_IPC_MAGIC,
            .version = WAYWAL_IPC_VERSION,
            .opcode = WAYWAL_RESP_ERR,
            .payload_size = 0u,
        };
        const void *rpay = NULL;
        size_t rlen = 0u;

        switch (hdr.opcode) {
        case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS: {
            const waywal_ipc_puppet_set_anim_tracks_t *r = NULL;
            const uint8_t *blob = NULL;
            size_t blen = 0u;
            bool ok = waywal_ipc_validate_req_puppet_set_anim_tracks(req_buf, hdr.payload_size, &r,
                                                                     &blob, &blen);
            rh.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
            if (!ok) {
                static const char err[] = "Invalid anim tracks";
                rpay = err;
                rlen = sizeof(err);
                rh.payload_size = (uint32_t)rlen;
            }
            break;
        }
        case WAYWAL_REQ_PUPPET_SCRUB_TIME: {
            /* Always emit OK/ERR parity (never silent) so client reply queue never desyncs. */
            const waywal_ipc_puppet_scrub_t *r = NULL;
            bool valid = waywal_ipc_validate_req_puppet_scrub_time(req_buf, hdr.payload_size, &r);
            bool found = valid && r->object_id == 7u;
            if (found) {
                rh.opcode = WAYWAL_RESP_OK;
            } else {
                static const char err_invalid[] = "Invalid scrub_time payload";
                static const char err_missing[] = "Failed to scrub puppet time";
                const char *msg = valid ? err_missing : err_invalid;
                rh.opcode = WAYWAL_RESP_ERR;
                rpay = msg;
                rlen = strlen(msg) + 1u;
                rh.payload_size = (uint32_t)rlen;
            }
            break;
        }
        case WAYWAL_REQ_EXPORT_PUPPET_MDL: {
            const waywal_ipc_export_puppet_mdl_t *r = NULL;
            if (!waywal_ipc_validate_req_export_puppet_mdl(req_buf, hdr.payload_size, &r)) {
                _exit(3);
            }
            static const uint8_t mdl_magic[8] = {'M', 'D', 'L', 'P', 1, 0, 0, 0};
            if (!waywal_ipc_serialize_resp_export_puppet_mdl(0, r->object_id, mdl_magic,
                                                             sizeof(mdl_magic), &rh, resp_buf,
                                                             sizeof(resp_buf), &rlen)) {
                _exit(4);
            }
            rpay = resp_buf;
            break;
        }
        default:
            _exit(5);
        }

        if (!waywal_ipc_send_payload_fds(fd, &rh, rpay, rlen, NULL, 0u)) {
            _exit(6);
        }
    }
    close(fd);
    _exit(0);
}

static void test_scrub_time_error_parity_no_desync(void)
{
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        run_scrub_parity_server(sv[1]);
    }

    close(sv[1]);
    int cfd = sv[0];
    waywal_ipc_hdr_t hdr = {0};
    waywal_ipc_hdr_t rhdr = {0};
    uint8_t tx[512];
    uint8_t rx[512];
    size_t total = 0u;

    /* 1. !replace_all && sync_layers (flags = 0x2u) anim track sync -> RESP_OK */
    assert(waywal_ipc_serialize_req_puppet_set_anim_tracks(7u, 0u, 0x2u, NULL, 0u, &hdr, tx,
                                                           sizeof(tx), &total));
    assert(waywal_ipc_send_payload_fds(cfd, &hdr, tx, total, NULL, 0u));
    assert(rig_recv_packet_nb(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 2. Valid 0x0032 SCRUB_TIME -> mandatory WAYWAL_RESP_OK */
    waywal_ipc_puppet_scrub_t sc_ok = {
        .object_id = 7u, .time_sec = 0.25f, .pause_playback = 1u, .flags = 1u};
    waywal_ipc_puppet_scrub_t sc_pay;
    assert(waywal_ipc_serialize_req_puppet_scrub_time(&sc_ok, &hdr, &sc_pay));
    assert(waywal_ipc_send_payload_fds(cfd, &hdr, &sc_pay, hdr.payload_size, NULL, 0u));
    assert(rig_recv_packet_nb(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 3. Wire-invalid 0x0032 SCRUB_TIME (negative time) -> mandatory WAYWAL_RESP_ERR */
    waywal_ipc_puppet_scrub_t sc_bad = {
        .object_id = 7u, .time_sec = -1.0f, .pause_playback = 0u, .flags = 0u};
    waywal_ipc_hdr_t bad_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_PUPPET_SCRUB_TIME,
        .payload_size = sizeof(sc_bad),
    };
    assert(waywal_ipc_send_payload_fds(cfd, &bad_hdr, &sc_bad, sizeof(sc_bad), NULL, 0u));
    assert(rig_recv_packet_nb(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_ERR);

    /* 4. Unknown-object 0x0032 SCRUB_TIME -> mandatory WAYWAL_RESP_ERR (no silent drop) */
    waywal_ipc_puppet_scrub_t sc_miss = {
        .object_id = 9999u, .time_sec = 1.0f, .pause_playback = 0u, .flags = 0u};
    assert(waywal_ipc_serialize_req_puppet_scrub_time(&sc_miss, &hdr, &sc_pay));
    assert(waywal_ipc_send_payload_fds(cfd, &hdr, &sc_pay, hdr.payload_size, NULL, 0u));
    assert(rig_recv_packet_nb(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_ERR);

    /* 5. Subsequent EXPORT_PUPPET_MDL must still receive 0x801D cleanly without desync */
    waywal_ipc_export_puppet_mdl_t exp_pay;
    assert(waywal_ipc_serialize_req_export_puppet_mdl(7u, 1u, NULL, &hdr, &exp_pay));
    assert(waywal_ipc_send_payload_fds(cfd, &hdr, &exp_pay, hdr.payload_size, NULL, 0u));
    assert(rig_recv_packet_nb(cfd, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_EXPORT_PUPPET_MDL);
    const waywal_ipc_export_puppet_mdl_resp_t *exp_resp = NULL;
    const uint8_t *mdl_blob = NULL;
    assert(waywal_ipc_validate_resp_export_puppet_mdl(rx, rhdr.payload_size, &exp_resp, &mdl_blob));
    assert(exp_resp->status == 0 && exp_resp->object_id == 7u && exp_resp->mdl_byte_size == 8u);
    assert(memcmp(mdl_blob, "MDLP\1\0\0\0", 8u) == 0);

    close(cfd);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void rig_remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        (void)unlink(path);
        return;
    }
    struct dirent *ent;
    char child[4096];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n > 0 && (size_t)n < sizeof(child)) {
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                rig_remove_tree(child);
            } else {
                (void)unlink(child);
            }
        }
    }
    closedir(dir);
    (void)rmdir(path);
}

static int rig_try_connect_unix(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool rig_daemon_roundtrip(const char *sock_path, const waywal_ipc_hdr_t *hdr,
                                 const void *payload, size_t payload_len,
                                 waywal_ipc_hdr_t *out_rhdr, uint8_t *out_rx, size_t rx_cap)
{
    int fd = rig_try_connect_unix(sock_path);
    if (fd < 0) {
        return false;
    }
    bool ok = wire_send_packet(fd, hdr, payload, payload_len) &&
              wire_recv_packet(fd, out_rhdr, out_rx, rx_cap);
    close(fd);
    return ok;
}

static waywal_ipc_puppet_preview_ik_ext_t rig_default_ik_ext(void)
{
    waywal_ipc_puppet_preview_ik_ext_t ext;
    memset(&ext, 0, sizeof(ext));
    /* Wire validator rejects stretch ratios outside [1.0, 4.0]. */
    ext.max_stretch_ratio = 1.0f;
    return ext;
}

static void rig_fill_bone(waywal_ipc_mdl_bone_t *b, const char *name, uint32_t parent, float tx,
                          float ty)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->parent = parent;
    fill_identity_mat4(b->local_trans);
    b->local_trans[12] = tx;
    b->local_trans[13] = ty;
}

static void test_live_daemon_rigging_ext_dispatch(void)
{
    if (getenv("WAYLAND_DISPLAY") == NULL) {
        printf("[TEST] Notice: Headless environment without WAYLAND_DISPLAY. Skipping live daemon "
               "rigging dispatch test.\n");
        return;
    }
    const char *candidates[] = {"./wwald",
                                "build_worker/wwald",
                                "build/wwald",
                                "build/src/daemon/wwald",
                                "../build/wwald",
                                NULL};
    const char *daemon_bin = NULL;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            daemon_bin = candidates[i];
            break;
        }
    }
    if (!daemon_bin) {
        printf("[TEST] Notice: wwald binary not found. Skipping live daemon rigging dispatch "
               "test.\n");
        return;
    }

    char tmp_template[] = "/tmp/wwal_rig_wire_XXXXXX";
    char *tmp_root = mkdtemp(tmp_template);
    assert(tmp_root != NULL);

    char cache_dir[4096], state_dir[4096], proj_path[4096], scene_path[4096];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", tmp_root);
    snprintf(state_dir, sizeof(state_dir), "%s/state", tmp_root);
    snprintf(proj_path, sizeof(proj_path), "%s/project.json", tmp_root);
    snprintf(scene_path, sizeof(scene_path), "%s/scene.json", tmp_root);
    assert(mkdir(cache_dir, 0755) == 0);
    assert(mkdir(state_dir, 0755) == 0);

    FILE *fp = fopen(proj_path, "wb");
    assert(fp != NULL);
    fputs("{\"title\":\"RigWireTest\",\"type\":\"scene\",\"file\":\"scene.json\"}\n", fp);
    fclose(fp);
    fp = fopen(scene_path, "wb");
    assert(fp != NULL);
    fputs("{\"general\":{\"orthogonalprojection\":{\"width\":128,\"height\":128}},"
          "\"objects\":[]}\n",
          fp);
    fclose(fp);

    char ns[64];
    snprintf(ns, sizeof(ns), "rigwire_%d", (int)getpid());
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        runtime_dir = "/tmp";
    }
    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    char sock_path[1024];
    snprintf(sock_path, sizeof(sock_path), "%s/%s-wwald.%s.sock", runtime_dir, wl_disp, ns);
    (void)unlink(sock_path);

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent_pid) {
            _exit(0);
        }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        setenv("WWAL_WORKSHOP_DIR", tmp_root, 1);
        setenv("WWAL_CACHE_DIR", cache_dir, 1);
        setenv("WWAL_STATE_DIR", state_dir, 1);
        setenv("XDG_CACHE_HOME", cache_dir, 1);
        setenv("XDG_STATE_HOME", state_dir, 1);
        execl(daemon_bin, daemon_bin, "--namespace", ns, "-q", (char *)NULL);
        _exit(127);
    }

    bool sock_ready = false;
    for (int i = 0; i < 60; ++i) {
        usleep(50000);
        int s = rig_try_connect_unix(sock_path);
        if (s >= 0) {
            close(s);
            sock_ready = true;
            break;
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            break;
        }
    }
    if (!sock_ready) {
        kill(pid, SIGTERM);
        int status = 0;
        (void)waitpid(pid, &status, 0);
        rig_remove_tree(tmp_root);
        assert(false && "Failed to start wwald daemon for live rigging dispatch test");
    }

    uint8_t tx[16384];
    uint8_t rx[16384];
    waywal_ipc_hdr_t hdr, rhdr;
    size_t total = 0u;

    /* 1. Load minimal scene workspace so outputs hold an active we_scene_t */
    waywal_workshop_payload_t wpay;
    memset(&wpay, 0, sizeof(wpay));
    snprintf(wpay.output, sizeof(wpay.output), "*");
    snprintf(wpay.workshop_dir, sizeof(wpay.workshop_dir), "%s", tmp_root);
    wpay.fps = 30u;
    wpay.no_cache = 1u;
    wpay.no_theme = 1u;
    wpay.volume = -1.0f;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = WAYWAL_IPC_MAGIC;
    hdr.version = WAYWAL_IPC_VERSION;
    hdr.opcode = WAYWAL_REQ_WORKSHOP;
    hdr.payload_size = sizeof(wpay);
    assert(rig_daemon_roundtrip(sock_path, &hdr, &wpay, sizeof(wpay), &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 2. Add object_id=1 to the live scene */
    waywal_ipc_req_scene_mutate_t mut;
    memset(&mut, 0, sizeof(mut));
    snprintf(mut.target_output, sizeof(mut.target_output), "*");
    mut.mutate_op = WE_MUTATE_ADD_OBJECT;
    mut.node_kind = WE_NODE_IMAGE;
    mut.object_id = 1;
    mut.parent_id = -1;
    mut.flags = 0x1u;
    mut.scale[0] = 1.0f;
    mut.scale[1] = 1.0f;
    mut.scale[2] = 1.0f;
    mut.size[0] = 64.0f;
    mut.size[1] = 64.0f;
    mut.color[0] = 1.0f;
    mut.color[1] = 1.0f;
    mut.color[2] = 1.0f;
    mut.color[3] = 1.0f;
    snprintf(mut.name, sizeof(mut.name), "rig_puppet");
    assert(waywal_ipc_serialize_req_scene_mutate(&mut, NULL, &hdr, NULL, tx, sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_SCENE_MUTATE);
    waywal_ipc_resp_scene_mutate_t mut_resp;
    memcpy(&mut_resp, rx, sizeof(mut_resp));
    assert(waywal_ipc_validate_resp_scene_mutate(&mut_resp, rhdr.payload_size));
    assert(mut_resp.status_code == WAYWAL_STATUS_OK && mut_resp.object_id == 1);

    /* 3. Upload 10-vertex / 8-triangle strip mesh spanning X=[0..40], Y=[-4..4] */
    static const float k_x[5] = {0.0f, 10.0f, 20.0f, 30.0f, 40.0f};
    waywal_ipc_mdl_vertex_t verts[10];
    memset(verts, 0, sizeof(verts));
    for (uint32_t c = 0u; c < 5u; ++c) {
        verts[c * 2u + 0u].pos[0] = k_x[c];
        verts[c * 2u + 0u].pos[1] = -4.0f;
        verts[c * 2u + 0u].texcoord[0] = k_x[c] / 40.0f;
        verts[c * 2u + 0u].texcoord[1] = 0.0f;
        verts[c * 2u + 0u].weight[0] = 1.0f;

        verts[c * 2u + 1u].pos[0] = k_x[c];
        verts[c * 2u + 1u].pos[1] = 4.0f;
        verts[c * 2u + 1u].texcoord[0] = k_x[c] / 40.0f;
        verts[c * 2u + 1u].texcoord[1] = 1.0f;
        verts[c * 2u + 1u].weight[0] = 1.0f;
    }
    uint16_t indices[24];
    for (uint16_t seg = 0u; seg < 4u; ++seg) {
        uint16_t v0 = (uint16_t)(seg * 2u);
        uint16_t v1 = (uint16_t)(v0 + 1u);
        uint16_t v2 = (uint16_t)(v0 + 2u);
        uint16_t v3 = (uint16_t)(v0 + 3u);
        indices[seg * 6u + 0u] = v0;
        indices[seg * 6u + 1u] = v2;
        indices[seg * 6u + 2u] = v3;
        indices[seg * 6u + 3u] = v0;
        indices[seg * 6u + 4u] = v3;
        indices[seg * 6u + 5u] = v1;
    }
    assert(waywal_ipc_serialize_req_puppet_set_mesh(1u, 1u, verts, 10u, indices, 8u, &hdr, tx,
                                                    sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_MESH_ACK);
    const waywal_ipc_puppet_mesh_ack_t *ack = NULL;
    assert(waywal_ipc_validate_resp_puppet_mesh_ack(rx, rhdr.payload_size, &ack));
    assert(ack->status == (int32_t)WAYWAL_STATUS_OK && ack->vertex_count == 10u);

    /* 4. Upload 4-bone arm chain: 0(pelvis)->1(arm_root 0,0)->2(arm_mid +20,0)->3(arm_end +20,0) */
    waywal_ipc_mdl_bone_t bones[4];
    rig_fill_bone(&bones[0], "pelvis", UINT32_MAX, 0.0f, 0.0f);
    rig_fill_bone(&bones[1], "arm_root", 0u, 0.0f, 0.0f);
    rig_fill_bone(&bones[2], "arm_mid", 1u, 20.0f, 0.0f);
    rig_fill_bone(&bones[3], "arm_end", 2u, 20.0f, 0.0f);
    assert(waywal_ipc_serialize_req_puppet_set_skeleton(1u, 1u, bones, 4u, &hdr, tx, sizeof(tx),
                                                        &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_OK);

    /* 5. SOLVE_WEIGHTS: legacy & _ext conductivity_scale differentiation */
    waywal_ipc_puppet_solve_weights_t sw_req = {.object_id = 1u,
                                                .falloff_power = 2.0f,
                                                .smoothness = 0.0f,
                                                .max_influences = 4u,
                                                .flags = 0x1u | 0x4u | 0x8u};
    assert(waywal_ipc_serialize_req_puppet_solve_weights(&sw_req, NULL, &hdr, tx, sizeof(tx),
                                                         &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    const waywal_ipc_puppet_weights_resp_t *wr = NULL;
    const waywal_ipc_vertex_weight_t *w_ent = NULL;
    assert(waywal_ipc_validate_resp_puppet_weights(rx, rhdr.payload_size, &wr, &w_ent));
    assert(wr->status == (int32_t)WAYWAL_STATUS_OK && wr->entry_count == 10u);

    float w_low[10] = {0};
    waywal_ipc_puppet_solve_weights_ext_t sw_low = {.conductivity_scale = 0.02f};
    assert(waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw_req, NULL, &sw_low, &hdr, tx,
                                                             sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    assert(waywal_ipc_validate_resp_puppet_weights(rx, rhdr.payload_size, &wr, &w_ent));
    assert(wr->status == (int32_t)WAYWAL_STATUS_OK && wr->entry_count == 10u);
    for (uint32_t v = 0u; v < 10u; ++v) {
        for (uint32_t k = 0u; k < 4u; ++k) {
            if (w_ent[v].blend_indices[k] == 1u) {
                w_low[v] += w_ent[v].weight[k];
            }
        }
    }

    float max_diff = 0.0f;
    waywal_ipc_puppet_solve_weights_ext_t sw_high = {.conductivity_scale = 80.0f};
    assert(waywal_ipc_serialize_req_puppet_solve_weights_ext(&sw_req, NULL, &sw_high, &hdr, tx,
                                                             sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS);
    assert(waywal_ipc_validate_resp_puppet_weights(rx, rhdr.payload_size, &wr, &w_ent));
    assert(wr->status == (int32_t)WAYWAL_STATUS_OK && wr->entry_count == 10u);
    for (uint32_t v = 0u; v < 10u; ++v) {
        float wh = 0.0f;
        for (uint32_t k = 0u; k < 4u; ++k) {
            if (w_ent[v].blend_indices[k] == 1u) {
                wh += w_ent[v].weight[k];
            }
        }
        float d = fabsf(wh - w_low[v]);
        if (d > max_diff) {
            max_diff = d;
        }
    }
    assert(max_diff > 1e-3f);

    /* 6. PREVIEW_IK: legacy path */
    waywal_ipc_puppet_preview_ik_t ik_req = {.object_id = 1u,
                                             .root_bone = 1u,
                                             .mid_bone = 2u,
                                             .end_bone = 3u,
                                             .pole_flip = 0u,
                                             .target_world = {20.0f, 20.0f, 0.0f},
                                             .override_count = 0u,
                                             .mode_flags = WAYWAL_IPC_IK_FLAG_ENABLE};
    assert(waywal_ipc_serialize_req_puppet_preview_ik(&ik_req, NULL, &hdr, tx, sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(rhdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE);
    const waywal_ipc_puppet_ik_pose_resp_t *ikr = NULL;
    const waywal_ipc_bone_trs_t *ikb = NULL;
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK && ikr->bone_count == 4u);
    assert(fabsf(ikb[1].local_angle[2]) > 0.1f || fabsf(ikb[2].local_angle[2]) > 0.1f);

    /* 7. PREVIEW_IK_EXT: pole_world (!= target_world) flips bend plane */
    ik_req.target_world[0] = 20.0f;
    ik_req.target_world[1] = 0.0f;
    waywal_ipc_puppet_preview_ik_ext_t ik_ext = rig_default_ik_ext();
    ik_ext.pole_world[0] = 10.0f;
    ik_ext.pole_world[1] = 30.0f;
    ik_ext.ik_blend_weight = 1.0f;
    ik_ext.ext_flags = WAYWAL_IPC_IK_EXT_USE_POLE;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK && ikr->bone_count == 4u);
    float root_ang_pos_pole = ikb[1].local_angle[2];

    ik_ext.pole_world[1] = -30.0f;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK && ikr->bone_count == 4u);
    float root_ang_neg_pole = ikb[1].local_angle[2];
    assert(root_ang_pos_pole > 0.5f && root_ang_neg_pole < -0.5f);

    /* 8. PREVIEW_IK_EXT: soft_zone_ratio softens near-max reach vs hard clamp */
    ik_req.target_world[0] = 39.5f;
    ik_req.target_world[1] = 0.0f;
    ik_ext = rig_default_ik_ext();
    ik_ext.pole_world[0] = 20.0f;
    ik_ext.pole_world[1] = 30.0f;
    ik_ext.soft_zone_ratio = 0.0f;
    ik_ext.ik_blend_weight = 1.0f;
    ik_ext.ext_flags = WAYWAL_IPC_IK_EXT_USE_POLE;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    float hard_mid_abs = fabsf(ikb[2].local_angle[2]);

    ik_ext.soft_zone_ratio = 0.25f;
    ik_ext.ext_flags = WAYWAL_IPC_IK_EXT_USE_POLE | WAYWAL_IPC_IK_EXT_SOFT_IK;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    float soft_mid_abs = fabsf(ikb[2].local_angle[2]);
    assert(soft_mid_abs > hard_mid_abs + 0.1f);

    /* 9. PREVIEW_IK_EXT: max_stretch_ratio elongates segments on over-extended target */
    ik_req.target_world[0] = 56.0f;
    ik_req.target_world[1] = 0.0f;
    ik_ext = rig_default_ik_ext();
    ik_ext.max_stretch_ratio = 1.5f;
    ik_ext.soft_zone_ratio = 0.2f;
    ik_ext.ik_blend_weight = 1.0f;
    ik_ext.ext_flags = WAYWAL_IPC_IK_EXT_ALLOW_STRETCH | WAYWAL_IPC_IK_EXT_SOFT_IK;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(ikb[2].local_pos[0] > 23.0f);

    /* 10. PREVIEW_IK_EXT: joint angle limits (bounded + strict [0, 0] lock) */
    ik_req.target_world[0] = 0.0f;
    ik_req.target_world[1] = 20.0f;
    ik_ext = rig_default_ik_ext();
    ik_ext.root_angle_min_rad = -0.30f;
    ik_ext.root_angle_max_rad = 0.30f;
    ik_ext.mid_angle_min_rad = -0.20f;
    ik_ext.mid_angle_max_rad = 0.20f;
    ik_ext.ik_blend_weight = 1.0f;
    ik_ext.ext_flags = WAYWAL_IPC_IK_EXT_LIMIT_ANGLES;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2]) <= 0.305f && fabsf(ikb[2].local_angle[2]) <= 0.205f);

    ik_ext.root_angle_min_rad = 0.0f;
    ik_ext.root_angle_max_rad = 0.0f;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&ik_req, NULL, &ik_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2]) < 1e-4f);

    /* 11. PREVIEW_IK_EXT: 1-bone Aim (aim_axis_local, aim_max_cone_rad, aim_blend_weight, disabled) */
    waywal_ipc_puppet_preview_ik_t aim_req = {.object_id = 1u,
                                              .root_bone = 1u,
                                              .mid_bone = 0xFFFFu,
                                              .end_bone = 0xFFFFu,
                                              .target_world = {0.0f, 20.0f, 0.0f},
                                              .mode_flags = WAYWAL_IPC_IK_FLAG_ENABLE};
    waywal_ipc_puppet_preview_ik_ext_t aim_ext = rig_default_ik_ext();
    aim_ext.aim_axis_local[0] = 1.0f;
    aim_ext.aim_max_cone_rad = 0.25f;
    aim_ext.aim_blend_weight = 1.0f;
    aim_ext.ext_flags = WAYWAL_IPC_IK_EXT_AIM_ENABLED;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&aim_req, NULL, &aim_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2] - 0.25f) < 0.02f);

    aim_ext.aim_axis_local[0] = 0.0f;
    aim_ext.aim_axis_local[1] = 1.0f;
    aim_ext.aim_max_cone_rad = 3.14f;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&aim_req, NULL, &aim_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2]) < 0.02f);

    aim_ext.aim_axis_local[0] = 1.0f;
    aim_ext.aim_axis_local[1] = 0.0f;
    aim_ext.aim_blend_weight = 0.5f;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&aim_req, NULL, &aim_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2] - 0.785398f) < 0.02f);

    aim_ext.ext_flags = 0u;
    assert(waywal_ipc_serialize_req_puppet_preview_ik_ext(&aim_req, NULL, &aim_ext, &hdr, tx,
                                                          sizeof(tx), &total));
    assert(rig_daemon_roundtrip(sock_path, &hdr, tx, total, &rhdr, rx, sizeof(rx)));
    assert(waywal_ipc_validate_resp_puppet_ik_pose(rx, rhdr.payload_size, &ikr, &ikb));
    assert(ikr->status == (int32_t)WAYWAL_STATUS_OK);
    assert(fabsf(ikb[1].local_angle[2]) < 1e-4f);

    kill(pid, SIGTERM);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    (void)unlink(sock_path);
    rig_remove_tree(tmp_root);
}

int main(void)
{
    test_legacy_and_helpers();
    test_set_mesh_wire();
    test_set_skeleton_wire();
    test_weights_and_heatmap_wire();
    test_ik_anim_scrub_export_wire();
    test_rigging_ext_wire();
    test_unix_socketpair_rigging_roundtrip();
    test_scrub_time_error_parity_no_desync();
    test_live_daemon_rigging_ext_dispatch();
    printf("test_ipc_rigging_wire: PASS\n");
    return 0;
}
