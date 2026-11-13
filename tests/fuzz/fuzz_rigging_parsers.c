// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__asan_default_options(void);
__attribute__((visibility("default"))) const char *__asan_default_options(void)
{
    return "allocator_may_return_null=1:detect_leaks=1";
}
#endif

static float fuzz_read_f32_bounded(const uint8_t *data, size_t size, size_t off, float fallback,
                                   float lo, float hi)
{
    if (off + sizeof(float) > size) {
        return fallback;
    }
    float v = 0.0f;
    memcpy(&v, data + off, sizeof(float));
    if (!isfinite(v)) {
        return fallback;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void fuzz_mdl_parser_and_writer(const uint8_t *data, size_t size)
{
    we_puppet_t *puppet = we_puppet_parse_cpu(data, size);
    if (puppet) {
        we_mdl_write_opts_t opts;
        we_mdl_write_opts_default(&opts);
        opts.sanitize_ascii_strings = (data[0] & 0x01u) != 0u;
        opts.emit_attachments = (data[0] & 0x02u) != 0u;
        opts.emit_skeleton = (data[0] & 0x04u) != 0u;
        opts.emit_animations = (data[0] & 0x08u) != 0u;

        uint8_t *written = NULL;
        size_t written_sz = 0u;
        if (we_mdl_write_mem(puppet, &opts, &written, &written_sz)) {
            we_puppet_t *reparsed = we_puppet_parse_cpu(written, written_sz);
            if (reparsed) {
                we_puppet_destroy(reparsed);
            }
            we_mdl_write_free(written);
        }
        we_puppet_destroy(puppet);
    }

    if (size >= 16u) {
        we_mdl_vertex_t verts[3];
        memset(verts, 0, sizeof(verts));
        verts[0].pos[0] = 0.0f;
        verts[0].pos[1] = 0.0f;
        verts[0].weight[0] = 1.0f;
        verts[1].pos[0] = 64.0f;
        verts[1].pos[1] = 0.0f;
        verts[1].weight[0] = 1.0f;
        verts[2].pos[0] = 32.0f;
        verts[2].pos[1] = 64.0f;
        verts[2].weight[0] = 1.0f;
        uint16_t indices[3] = {0u, 1u, 2u};

        we_mdl_bone_t bone;
        memset(&bone, 0, sizeof(bone));
        size_t name_copy = size < 32u ? size : 31u;
        memcpy(bone.name, data, name_copy);
        bone.name[name_copy] = '\0';
        bone.parent = UINT32_MAX;
        bone.local_trans[0] = 1.0f;
        bone.local_trans[5] = 1.0f;
        bone.local_trans[10] = 1.0f;
        bone.local_trans[15] = 1.0f;
        memcpy(bone.world_bind, bone.local_trans, sizeof(bone.world_bind));
        memcpy(bone.offset_trans, bone.local_trans, sizeof(bone.offset_trans));
        bone.bind_scale[0] = 1.0f;
        bone.bind_scale[1] = 1.0f;
        bone.bind_scale[2] = 1.0f;

        we_puppet_t synth;
        memset(&synth, 0, sizeof(synth));
        synth.mdl_flag = (int32_t)(data[0] & 0x0Fu);
        synth.vertex_count = 3u;
        synth.vertices = verts;
        synth.triangle_count = 1u;
        synth.index_count = 3u;
        synth.indices = indices;
        synth.bone_count = 1u;
        synth.bones = &bone;

        we_mdl_write_opts_t opts;
        we_mdl_write_opts_default(&opts);
        opts.sanitize_ascii_strings = (data[1] & 1u) != 0u;

        uint8_t *buf = NULL;
        size_t buf_sz = 0u;
        if (we_mdl_write_mem(&synth, &opts, &buf, &buf_sz)) {
            we_puppet_t *rt = we_puppet_parse_cpu(buf, buf_sz);
            if (rt) {
                we_puppet_destroy(rt);
            }
            we_mdl_write_free(buf);
        }
    }
}

static void fuzz_cdt_meshing(const uint8_t *data, size_t size)
{
    const uint32_t w = 24u;
    const uint32_t h = 24u;
    uint8_t alpha[24u * 24u];
    for (size_t i = 0u; i < sizeof(alpha); ++i) {
        alpha[i] = data[i % size];
    }

    we_mesh_gen_cfg_t cfg;
    we_mesh_gen_cfg_default(&cfg);
    cfg.alpha_threshold = (float)((data[0] % 200u) + 16u) / 255.0f;
    cfg.dilation_px = (float)(data[size > 1u ? 1u : 0u] & 3u);
    cfg.simplify_eps_px = 0.5f + (float)(data[size > 2u ? 2u : 0u] & 7u) * 0.5f;
    cfg.interior_spacing = 6.0f + (float)(data[size > 3u ? 3u : 0u] & 15u);
    cfg.min_angle_deg = 12.0f + (float)(data[size > 4u ? 4u : 0u] % 18u);
    cfg.min_island_area = (float)(data[size > 5u ? 5u : 0u] & 15u);
    cfg.max_vertices = 128u;

    we_cdt_pin_t pins[2];
    pins[0] = (we_cdt_pin_t){
        .px = (double)(data[0] % w),
        .py = (double)(data[size > 1u ? 1u : 0u] % h),
        .flags = (uint32_t)(data[0] & 3u),
    };
    pins[1] = (we_cdt_pin_t){
        .px = fuzz_read_f32_bounded(data, size, 2u, 12.0f, -8.0f, 32.0f),
        .py = fuzz_read_f32_bounded(data, size, 6u, 12.0f, -8.0f, 32.0f),
        .flags = 1u,
    };
    uint32_t pin_count = (uint32_t)(data[0] % 3u);

    we_cdt_loop_t *loops = NULL;
    uint32_t loop_count = 0u;
    if (we_mesh_extract_contours(alpha, w, h, w, &cfg, &loops, &loop_count)) {
        we_mesh_loops_free(loops, loop_count);
    }

    we_mesh_gen_result_t res;
    if (we_mesh_gen_from_alpha(alpha, w, h, w, &cfg, pins, pin_count, &res)) {
        we_mesh_gen_result_free(&res);
    }

    /* Exercise untrusted polygon loops (convex, self-intersecting, degenerate, and hole loops). */
    we_cdt_vec2_t outer_pts[8];
    we_cdt_vec2_t hole_pts[4];
    uint32_t n_outer = 3u + (uint32_t)(data[0] % 6u);
    for (uint32_t i = 0u; i < n_outer; ++i) {
        size_t o = (size_t)i * 2u;
        outer_pts[i].x = (double)(data[o % size]) * 0.5;
        outer_pts[i].y = (double)(data[(o + 1u) % size]) * 0.5;
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        size_t o = (size_t)i * 2u + 3u;
        hole_pts[i].x = 40.0 + (double)(data[o % size] & 0x3Fu);
        hole_pts[i].y = 40.0 + (double)(data[(o + 1u) % size] & 0x3Fu);
    }

    we_cdt_loop_t raw_loops[2] = {
        {.points = outer_pts, .point_count = n_outer, .is_hole = false, .signed_area_px2 = 0.0},
        {.points = hole_pts,
         .point_count = 4u,
         .is_hole = (data[0] & 0x10u) != 0u,
         .signed_area_px2 = 0.0},
    };
    uint32_t raw_loop_count = (data[0] & 0x20u) ? 2u : 1u;
    if (we_mesh_gen_from_loops(raw_loops, raw_loop_count, 256u, 256u, &cfg, pins, pin_count,
                               &res)) {
        we_mesh_gen_result_free(&res);
    }
}

static void fuzz_simulation_json(const uint8_t *data, size_t size)
{
    char json_buf[513];
    size_t copy_len = size < (sizeof(json_buf) - 1u) ? size : (sizeof(json_buf) - 1u);
    memcpy(json_buf, data, copy_len);
    json_buf[copy_len] = '\0';

    we_bone_sim_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (we_physics_parse_simulation_json(json_buf, &cfg)) {
        char formatted[256];
        if (we_physics_format_simulation_json(&cfg, formatted)) {
            we_bone_sim_config_t reparsed;
            memset(&reparsed, 0, sizeof(reparsed));
            (void)we_physics_parse_simulation_json(formatted, &reparsed);
        }
    }
}

static void fuzz_rigging_ipc_validators(const uint8_t *data, size_t size)
{
    uint8_t aligned_buf[4096];
    size_t copy_len = size < sizeof(aligned_buf) ? size : sizeof(aligned_buf);
    memset(aligned_buf, 0, sizeof(aligned_buf));
    memcpy(aligned_buf, data, copy_len);

    const uint8_t *bufs[2] = {data, aligned_buf};
    const size_t lens[2] = {size, sizeof(aligned_buf)};

    for (size_t k = 0u; k < 2u; ++k) {
        const uint8_t *b = bufs[k];
        size_t l = lens[k];

        const waywal_ipc_puppet_set_mesh_t *mesh_req = NULL;
        const waywal_ipc_mdl_vertex_t *mesh_verts = NULL;
        const uint16_t *mesh_indices = NULL;
        (void)waywal_ipc_validate_req_puppet_set_mesh(b, l, &mesh_req, &mesh_verts, &mesh_indices);

        const waywal_ipc_puppet_set_skeleton_t *skel_req = NULL;
        const waywal_ipc_mdl_bone_t *skel_bones = NULL;
        (void)waywal_ipc_validate_req_puppet_set_skeleton(b, l, &skel_req, &skel_bones);

        const waywal_ipc_puppet_solve_weights_t *solve_req = NULL;
        const uint64_t *locked_bs = NULL;
        (void)waywal_ipc_validate_req_puppet_solve_weights(b, l, &solve_req, &locked_bs);

        const waywal_ipc_puppet_paint_weights_t *paint_req = NULL;
        const waywal_ipc_vertex_weight_t *paint_entries = NULL;
        (void)waywal_ipc_validate_req_puppet_paint_weights(b, l, &paint_req, &paint_entries);

        const waywal_ipc_puppet_heatmap_mode_t *hm_req = NULL;
        (void)waywal_ipc_validate_req_puppet_heatmap_mode(b, l, &hm_req);

        const waywal_ipc_puppet_preview_ik_t *ik_req = NULL;
        const waywal_ipc_bone_trs_t *ik_overrides = NULL;
        (void)waywal_ipc_validate_req_puppet_preview_ik(b, l, &ik_req, &ik_overrides);

        const waywal_ipc_puppet_set_anim_tracks_t *anim_req = NULL;
        const uint8_t *clips_blob = NULL;
        size_t clips_blob_len = 0u;
        (void)waywal_ipc_validate_req_puppet_set_anim_tracks(b, l, &anim_req, &clips_blob,
                                                             &clips_blob_len);

        const waywal_ipc_puppet_scrub_t *scrub_req = NULL;
        (void)waywal_ipc_validate_req_puppet_scrub_time(b, l, &scrub_req);

        const waywal_ipc_export_puppet_mdl_t *exp_req = NULL;
        (void)waywal_ipc_validate_req_export_puppet_mdl(b, l, &exp_req);

        const waywal_ipc_puppet_mesh_ack_t *ack_resp = NULL;
        (void)waywal_ipc_validate_resp_puppet_mesh_ack(b, l, &ack_resp);

        const waywal_ipc_puppet_weights_resp_t *w_resp = NULL;
        const waywal_ipc_vertex_weight_t *w_entries = NULL;
        (void)waywal_ipc_validate_resp_puppet_weights(b, l, &w_resp, &w_entries);

        const waywal_ipc_puppet_ik_pose_resp_t *ik_resp = NULL;
        const waywal_ipc_bone_trs_t *ik_bones = NULL;
        (void)waywal_ipc_validate_resp_puppet_ik_pose(b, l, &ik_resp, &ik_bones);

        const waywal_ipc_export_puppet_mdl_resp_t *exp_resp = NULL;
        const uint8_t *mdl_blob = NULL;
        (void)waywal_ipc_validate_resp_export_puppet_mdl(b, l, &exp_resp, &mdl_blob);
    }

    (void)waywal_ipc_validate_req_puppet_heatmap_mode(
        (const waywal_ipc_puppet_heatmap_mode_t *)aligned_buf,
        sizeof(waywal_ipc_puppet_heatmap_mode_t),
        &(const waywal_ipc_puppet_heatmap_mode_t *){NULL});
    (void)waywal_ipc_validate_req_puppet_scrub_time((const waywal_ipc_puppet_scrub_t *)aligned_buf,
                                                    sizeof(waywal_ipc_puppet_scrub_t),
                                                    &(const waywal_ipc_puppet_scrub_t *){NULL});
    (void)waywal_ipc_validate_req_export_puppet_mdl(
        (const waywal_ipc_export_puppet_mdl_t *)aligned_buf, sizeof(waywal_ipc_export_puppet_mdl_t),
        &(const waywal_ipc_export_puppet_mdl_t *){NULL});
    (void)waywal_ipc_validate_resp_puppet_mesh_ack(
        (const waywal_ipc_puppet_mesh_ack_t *)aligned_buf, sizeof(waywal_ipc_puppet_mesh_ack_t),
        &(const waywal_ipc_puppet_mesh_ack_t *){NULL});
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0u) {
        return 0;
    }

    fuzz_mdl_parser_and_writer(data, size);
    fuzz_cdt_meshing(data, size);
    fuzz_simulation_json(data, size);
    fuzz_rigging_ipc_validators(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE

typedef struct {
    const uint8_t *bytes;
    size_t len;
} fuzz_seed_t;

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void build_seed_mdl(uint8_t **out_bytes, size_t *out_len)
{
    we_mdl_vertex_t verts[4];
    memset(verts, 0, sizeof(verts));
    for (uint32_t i = 0u; i < 4u; ++i) {
        verts[i].pos[0] = (float)(i % 2u) * 40.0f;
        verts[i].pos[1] = (float)(i / 2u) * 40.0f;
        verts[i].blend_indices[0] = i % 2u;
        verts[i].weight[0] = 1.0f;
        verts[i].texcoord[0] = (float)(i % 2u);
        verts[i].texcoord[1] = (float)(i / 2u);
    }
    uint16_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};

    we_mdl_bone_t bones[2];
    memset(bones, 0, sizeof(bones));
    snprintf(bones[0].name, sizeof(bones[0].name), "root");
    bones[0].parent = UINT32_MAX;
    bones[0].local_trans[0] = 1.0f;
    bones[0].local_trans[5] = 1.0f;
    bones[0].local_trans[10] = 1.0f;
    bones[0].local_trans[15] = 1.0f;
    memcpy(bones[0].world_bind, bones[0].local_trans, sizeof(bones[0].world_bind));
    memcpy(bones[0].offset_trans, bones[0].local_trans, sizeof(bones[0].offset_trans));
    bones[0].bind_scale[0] = 1.0f;
    bones[0].bind_scale[1] = 1.0f;
    bones[0].bind_scale[2] = 1.0f;

    snprintf(bones[1].name, sizeof(bones[1].name), "tail");
    bones[1].parent = 0u;
    memcpy(bones[1].local_trans, bones[0].local_trans, sizeof(bones[1].local_trans));
    bones[1].local_trans[13] = 24.0f;
    bones[1].bind_pos[1] = 24.0f;
    bones[1].bind_scale[0] = 1.0f;
    bones[1].bind_scale[1] = 1.0f;
    bones[1].bind_scale[2] = 1.0f;
    memcpy(bones[1].world_bind, bones[1].local_trans, sizeof(bones[1].world_bind));
    (void)we_mat4_inverse(bones[1].world_bind, bones[1].offset_trans);
    snprintf(bones[1].simulation_json, sizeof(bones[1].simulation_json),
             "{\"stiffness\":0.8,\"damping\":0.3,\"mass\":1.0,\"length\":24.0}");

    we_mdl_keyframe_t kfs[2] = {
        {.pos = {0.0f, 24.0f, 0.0f}, .angle = {0.0f, 0.0f, 0.0f}, .scale = {1.0f, 1.0f, 1.0f}},
        {.pos = {0.0f, 24.0f, 0.0f}, .angle = {0.0f, 0.0f, 0.35f}, .scale = {1.0f, 1.0f, 1.0f}},
    };
    we_mdl_bone_track_t track = {
        .bone_index = 1,
        .unk = 0,
        .kf_count = 2u,
        .keyframes = kfs,
    };
    we_mdl_anim_t anim;
    memset(&anim, 0, sizeof(anim));
    anim.id = 1;
    snprintf(anim.name, sizeof(anim.name), "idle");
    snprintf(anim.mode_str, sizeof(anim.mode_str), "loop");
    anim.mode = WE_ANIM_LOOP;
    anim.fps = 24.0f;
    anim.length = 2;
    anim.track_count = 1u;
    anim.tracks = &track;

    we_puppet_t p;
    memset(&p, 0, sizeof(p));
    p.mdl_flag = 2;
    snprintf(p.mat_json_file, sizeof(p.mat_json_file), "materials/hero.json");
    p.vertex_count = 4u;
    p.vertices = verts;
    p.triangle_count = 2u;
    p.index_count = 6u;
    p.indices = indices;
    p.bone_count = 2u;
    p.bones = bones;
    p.anim_count = 1u;
    p.anims = &anim;

    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);
    if (!we_mdl_write_mem(&p, &opts, out_bytes, out_len)) {
        *out_bytes = NULL;
        *out_len = 0u;
    }
}

int main(void)
{
    printf("[FUZZ] Starting fuzz_rigging_parsers standalone fuzzing harness...\n");

    uint8_t *mdl_seed = NULL;
    size_t mdl_seed_len = 0u;
    build_seed_mdl(&mdl_seed, &mdl_seed_len);

    waywal_ipc_hdr_t hdr;
    uint8_t ipc_mesh_buf[512];
    size_t ipc_mesh_len = 0u;
    waywal_ipc_mdl_vertex_t iverts[3];
    memset(iverts, 0, sizeof(iverts));
    for (uint32_t i = 0u; i < 3u; ++i) {
        iverts[i].pos[0] = (float)i * 10.0f;
        iverts[i].weight[0] = 1.0f;
    }
    const uint16_t iidx[3] = {0u, 1u, 2u};
    (void)waywal_ipc_serialize_req_puppet_set_mesh(1u, 3u, iverts, 3u, iidx, 1u, &hdr, ipc_mesh_buf,
                                                   sizeof(ipc_mesh_buf), &ipc_mesh_len);

    uint8_t ipc_skel_buf[1024];
    size_t ipc_skel_len = 0u;
    waywal_ipc_mdl_bone_t ibone;
    memset(&ibone, 0, sizeof(ibone));
    snprintf(ibone.name, sizeof(ibone.name), "root");
    ibone.parent = UINT32_MAX;
    ibone.local_trans[0] = 1.0f;
    ibone.local_trans[5] = 1.0f;
    ibone.local_trans[10] = 1.0f;
    ibone.local_trans[15] = 1.0f;
    snprintf(ibone.simulation_json, sizeof(ibone.simulation_json), "{}");
    (void)waywal_ipc_serialize_req_puppet_set_skeleton(1u, 1u, &ibone, 1u, &hdr, ipc_skel_buf,
                                                       sizeof(ipc_skel_buf), &ipc_skel_len);

    static const uint8_t seed_json0[] =
        "{\"stiffness\":0.65,\"damping\":0.25,\"gravity\":1.5,\"mass\":0.8,\"length\":32.0,"
        "\"max_angle\":1.1,\"wind\":0.5,\"audio\":0.2,\"collide\":true,\"radius\":12.0}";
    static const uint8_t seed_json1[] =
        "{\"stiffness\":\"bad\",\"mass\":1e99,\"nested\":[[[[1,2,3]]]]}";
    static const uint8_t seed_mdl_hdr[] = "MDLV0023\x00\x02\x00\x00\x00\xff\xff\xff\xff";
    static const uint8_t seed_mdl_alt[] = "MDLV0013\x00\x00\x00\x00\x00\x00\x00\x00\x00";
    static const uint8_t seed_zeros[64] = {0};

    fuzz_seed_t corpus[8];
    size_t corpus_count = 0u;
    if (mdl_seed && mdl_seed_len > 0u) {
        corpus[corpus_count++] = (fuzz_seed_t){mdl_seed, mdl_seed_len};
    }
    if (ipc_mesh_len > 0u) {
        corpus[corpus_count++] = (fuzz_seed_t){ipc_mesh_buf, ipc_mesh_len};
    }
    if (ipc_skel_len > 0u) {
        corpus[corpus_count++] = (fuzz_seed_t){ipc_skel_buf, ipc_skel_len};
    }
    corpus[corpus_count++] = (fuzz_seed_t){seed_json0, sizeof(seed_json0) - 1u};
    corpus[corpus_count++] = (fuzz_seed_t){seed_json1, sizeof(seed_json1) - 1u};
    corpus[corpus_count++] = (fuzz_seed_t){seed_mdl_hdr, sizeof(seed_mdl_hdr) - 1u};
    corpus[corpus_count++] = (fuzz_seed_t){seed_mdl_alt, sizeof(seed_mdl_alt) - 1u};
    corpus[corpus_count++] = (fuzz_seed_t){seed_zeros, sizeof(seed_zeros)};

    for (size_t i = 0u; i < corpus_count; ++i) {
        LLVMFuzzerTestOneInput(corpus[i].bytes, corpus[i].len);
    }

    const int MUTATION_ROUNDS = 2500;
    uint32_t rng = 0x51A9F00DU;
    uint8_t mut_buf[4096];

    for (int r = 0; r < MUTATION_ROUNDS; ++r) {
        size_t base_idx = xorshift32(&rng) % corpus_count;
        size_t base_len = corpus[base_idx].len;
        size_t len = base_len < sizeof(mut_buf) ? base_len : sizeof(mut_buf);
        memcpy(mut_buf, corpus[base_idx].bytes, len);

        uint32_t flips = (xorshift32(&rng) % 10u) + 1u;
        for (uint32_t f = 0u; f < flips; ++f) {
            if (len > 0u) {
                size_t p = xorshift32(&rng) % len;
                mut_buf[p] ^= (uint8_t)(xorshift32(&rng) & 0xFFu);
            }
        }

        size_t test_len = (len > 0u) ? ((xorshift32(&rng) % len) + 1u) : 0u;
        LLVMFuzzerTestOneInput(mut_buf, test_len);
    }

    we_mdl_write_free(mdl_seed);
    printf("[FUZZ] fuzz_rigging_parsers (%d mutation rounds) PASSED CLEANLY (100%%).\n",
           MUTATION_ROUNDS);
    return 0;
}
#endif
