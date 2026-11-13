// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_translation_mat(float tx, float ty, float tz, float out[16])
{
    memset(out, 0, sizeof(float) * 16u);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
    out[15] = 1.0f;
}

static void build_character_rgba8(uint8_t *rgba, uint32_t w, uint32_t h)
{
    memset(rgba, 0, (size_t)w * (size_t)h * 4u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            bool inside_torso = (x >= 44u && x <= 84u && y >= 28u && y <= 100u);
            bool inside_arm = (x >= 20u && x <= 108u && y >= 48u && y <= 66u);
            bool inside_ribbon = (x >= 56u && x <= 72u && y >= 96u && y <= 118u);
            if (inside_torso || inside_arm || inside_ribbon) {
                size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                rgba[idx + 0] = 180u;
                rgba[idx + 1] = 210u;
                rgba[idx + 2] = 250u;
                rgba[idx + 3] = 255u;
            }
        }
    }
}

static void setup_rig_skeleton(we_puppet_t *puppet, we_mdl_bone_t bones[5], we_mdl_anim_t anims[2],
                               we_mdl_bone_track_t tracks[2][5], we_mdl_keyframe_t kfs[2][5][3])
{
    memset(bones, 0, sizeof(we_mdl_bone_t) * 5u);
    memset(anims, 0, sizeof(we_mdl_anim_t) * 2u);
    memset(tracks, 0, sizeof(we_mdl_bone_track_t) * 2u * 5u);
    memset(kfs, 0, sizeof(we_mdl_keyframe_t) * 2u * 5u * 3u);

    const float rest_xy[5][2] = {
        {0.0f, 0.0f},   /* 0: pelvis (px 64, 64) */
        {0.0f, 10.0f},  /* 1: arm_root (px 64, 54) */
        {20.0f, 10.0f}, /* 2: arm_mid (px 84, 54) */
        {40.0f, 10.0f}, /* 3: arm_end (px 104, 54) */
        {0.0f, -32.0f}, /* 4: ribbon_xpbd (px 64, 96) */
    };
    const uint32_t parents[5] = {0xFFFFFFFFu, 0u, 1u, 2u, 0u};
    const char *names[5] = {"pelvis", "arm_root", "arm_mid", "arm_end", "ribbon_xpbd"};

    float world_rest[5][16];
    for (uint32_t i = 0; i < 5u; ++i) {
        snprintf(bones[i].name, sizeof(bones[i].name), "%s", names[i]);
        bones[i].parent = parents[i];
        float px = (parents[i] == 0xFFFFFFFFu) ? 0.0f : rest_xy[parents[i]][0];
        float py = (parents[i] == 0xFFFFFFFFu) ? 0.0f : rest_xy[parents[i]][1];
        float dx = rest_xy[i][0] - px;
        float dy = rest_xy[i][1] - py;
        make_translation_mat(dx, dy, 0.0f, bones[i].local_trans);
        if (parents[i] == 0xFFFFFFFFu) {
            memcpy(world_rest[i], bones[i].local_trans, sizeof(float) * 16u);
        } else {
            we_mat4_mul(world_rest[parents[i]], bones[i].local_trans, world_rest[i]);
        }
        assert(we_mat4_inverse(world_rest[i], bones[i].offset_trans));
    }

    we_bone_sim_config_t xpbd_cfg;
    memset(&xpbd_cfg, 0, sizeof(xpbd_cfg));
    we_physics_apply_preset(&xpbd_cfg, WE_PHYS_PRESET_SILK);
    xpbd_cfg.flags |= WE_BONE_PHYS_ENABLED;
    xpbd_cfg.bone_index = 4u;
    assert(we_physics_format_simulation_json(&xpbd_cfg, bones[4].simulation_json));

    puppet->mdl_flag = 2;
    puppet->unk_1 = 3;
    puppet->unk_2 = 5;
    snprintf(puppet->mat_json_file, sizeof(puppet->mat_json_file), "materials/challenger.json");
    puppet->bone_count = 5u;
    puppet->bones = bones;

    /* Clip 0: override idle cycle */
    anims[0].id = 100;
    snprintf(anims[0].name, sizeof(anims[0].name), "idle");
    snprintf(anims[0].mode_str, sizeof(anims[0].mode_str), "loop");
    anims[0].mode = WE_ANIM_LOOP;
    anims[0].fps = 30.0f;
    anims[0].length = 3u;
    anims[0].track_count = 5u;
    anims[0].tracks = tracks[0];

    /* Clip 1: additive breath micro-motion */
    anims[1].id = 101;
    snprintf(anims[1].name, sizeof(anims[1].name), "breath_add");
    snprintf(anims[1].mode_str, sizeof(anims[1].mode_str), "loop");
    anims[1].mode = WE_ANIM_LOOP;
    anims[1].fps = 30.0f;
    anims[1].length = 3u;
    anims[1].track_count = 5u;
    anims[1].tracks = tracks[1];

    for (uint32_t c = 0; c < 2u; ++c) {
        for (uint32_t b = 0; b < 5u; ++b) {
            tracks[c][b].bone_index = b;
            tracks[c][b].kf_count = 3u;
            tracks[c][b].keyframes = kfs[c][b];
            for (uint32_t k = 0; k < 3u; ++k) {
                if (c == 0u) {
                    kfs[c][b][k].pos[0] = bones[b].local_trans[12];
                    kfs[c][b][k].pos[1] = bones[b].local_trans[13];
                    kfs[c][b][k].pos[2] = 0.0f;
                    kfs[c][b][k].angle[2] = (b == 0u && k == 1u) ? 0.08f : 0.0f;
                } else {
                    kfs[c][b][k].pos[0] = 0.0f;
                    kfs[c][b][k].pos[1] = (b == 1u && k == 1u) ? 2.5f : 0.0f;
                    kfs[c][b][k].pos[2] = 0.0f;
                    kfs[c][b][k].angle[2] = 0.0f;
                }
                kfs[c][b][k].scale[0] = 1.0f;
                kfs[c][b][k].scale[1] = 1.0f;
                kfs[c][b][k].scale[2] = 1.0f;
            }
        }
    }

    puppet->anim_count = 2u;
    puppet->anims = anims;
}

int main(void)
{
    printf("[Challenger Rigging] Running E2E RGBA8 -> CDT -> Heat -> IK -> MDL -> XPBD...\n");

    const uint32_t w = 128u;
    const uint32_t h = 128u;
    uint8_t *rgba = calloc((size_t)w * (size_t)h * 4u, 1u);
    assert(rgba != NULL);
    build_character_rgba8(rgba, w, h);

    we_mesh_gen_cfg_t cdt_cfg;
    we_mesh_gen_cfg_default(&cdt_cfg);
    cdt_cfg.dilation_px = 1.0f;
    cdt_cfg.interior_spacing = 12.0f;
    cdt_cfg.min_angle_deg = 22.0f;
    cdt_cfg.max_vertices = 2048u;

    const we_cdt_pin_t pins[2] = {
        {.px = 64.0, .py = 54.0, .flags = 1u},
        {.px = 84.0, .py = 54.0, .flags = 1u},
    };
    we_mesh_gen_result_t mesh;
    memset(&mesh, 0, sizeof(mesh));
    assert(we_mesh_gen_from_rgba8(rgba, w, h, w * 4u, &cdt_cfg, pins, 2u, &mesh));
    free(rgba);

    assert(mesh.vertex_count >= 12u);
    assert(mesh.triangle_count >= 10u);
    assert(mesh.index_count == mesh.triangle_count * 3u);
    assert(mesh.achieved_min_angle_deg > 10.0f);

    /* Ensure at least one vertex matches each hard Steiner pin cleanly (centered world coords) */
    for (uint32_t p = 0; p < 2u; ++p) {
        double wx = pins[p].px - 0.5 * (double)w;
        double wy = 0.5 * (double)h - pins[p].py;
        bool found = false;
        for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
            double dx = (double)mesh.vertices[v].pos[0] - wx;
            double dy = (double)mesh.vertices[v].pos[1] - wy;
            if (dx * dx + dy * dy < 1e-2) {
                found = true;
                break;
            }
        }
        assert(found);
    }

    we_puppet_t puppet;
    memset(&puppet, 0, sizeof(puppet));
    puppet.vertex_count = mesh.vertex_count;
    puppet.vertices = mesh.vertices;
    puppet.triangle_count = mesh.triangle_count;
    puppet.index_count = mesh.index_count;
    puppet.indices = mesh.indices;

    we_mdl_bone_t bones[5];
    we_mdl_anim_t anims[2];
    we_mdl_bone_track_t tracks[2][5];
    we_mdl_keyframe_t kfs[2][5][3];
    setup_rig_skeleton(&puppet, bones, anims, tracks, kfs);

    /* 1. Geodesic / Laplacian heat weight solve */
    we_heat_skin_cfg_t heat_cfg;
    we_heat_skin_cfg_default(&heat_cfg);
    heat_cfg.use_intrinsic_delaunay = true;
    we_heat_solve_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    assert(we_mesh_solve_heat_weights(&puppet, &heat_cfg, &stats));
    assert(stats.vertex_count == puppet.vertex_count);
    assert(stats.bone_count == puppet.bone_count);

    for (uint32_t v = 0; v < puppet.vertex_count; ++v) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            assert(isfinite(puppet.vertices[v].weight[k]));
            assert(puppet.vertices[v].weight[k] >= 0.0f);
            sum += puppet.vertices[v].weight[k];
        }
        assert(fabsf(sum - 1.0f) < 1e-3f);
    }

    /* 2. Interactive weight brush dab preserves partition of unity (centered Y-up on ribbon) */
    uint64_t dirty_blocks = 0u;
    const float dab_a[2] = {0.0f, -38.0f};
    const float dab_b[2] = {2.0f, -44.0f};
    uint32_t touched =
        we_mesh_apply_weight_paint_dab(&puppet, dab_a, dab_b, 14.0f, 14.0f, 0.75f, WE_WEIGHT_OP_ADD,
                                       4u, 1.0f, NULL, NULL, &dirty_blocks);
    assert(touched > 0u);
    assert(dirty_blocks != 0u);
    for (uint32_t v = 0; v < puppet.vertex_count; ++v) {
        float sum = puppet.vertices[v].weight[0] + puppet.vertices[v].weight[1] +
                    puppet.vertices[v].weight[2] + puppet.vertices[v].weight[3];
        assert(fabsf(sum - 1.0f) < 1e-3f);
    }

    /* 3. Two-bone IK + XPBD priority evaluation */
    we_rig_pose_ctx_t rig_ctx;
    memset(&rig_ctx, 0, sizeof(rig_ctx));
    rig_ctx.bone_drive_modes[0] = WE_BONE_DRIVE_FK;
    rig_ctx.bone_drive_modes[1] = WE_BONE_DRIVE_IK;
    rig_ctx.bone_drive_modes[2] = WE_BONE_DRIVE_IK;
    rig_ctx.bone_drive_modes[3] = WE_BONE_DRIVE_IK;
    rig_ctx.bone_drive_modes[4] = WE_BONE_DRIVE_XPBD;
    rig_ctx.ik_chain_count = 1u;
    rig_ctx.ik_chains[0] = (we_ik_chain_t){
        .root_bone = 1u,
        .mid_bone = 2u,
        .end_bone = 3u,
        .bend_sign = 1,
        .enabled = 1u,
        .target_world = {24.0f, -18.0f, 0.0f},
        .pole_world = {10.0f, -24.0f, 0.0f},
        .soft_zone_ratio = 0.05f,
        .max_stretch_ratio = 1.15f,
        .root_angle_min_rad = -3.0f,
        .root_angle_max_rad = 3.0f,
        .mid_angle_min_rad = -3.0f,
        .mid_angle_max_rad = 3.0f,
        .blend_weight = 1.0f,
    };

    we_puppet_layer_t layers[2];
    memset(layers, 0, sizeof(layers));
    layers[0] = (we_puppet_layer_t){.anim_id = 100, .rate = 1.0f, .blend = 1.0f, .visible = true};
    layers[1] = (we_puppet_layer_t){.anim_id = 101, .rate = 1.0f, .blend = 0.5f, .visible = true};
    rig_ctx.layer_blend_modes[0] = WE_ANIM_BLEND_OVERRIDE;
    rig_ctx.layer_blend_modes[1] = WE_ANIM_BLEND_ADDITIVE;

    float skin_mats[5][16];
    we_puppet_eval_skin_matrices_ex(&puppet, layers, 2u, &rig_ctx, 0.025, skin_mats);
    for (uint32_t b = 0; b < 5u; ++b) {
        for (int i = 0; i < 16; ++i) {
            assert(isfinite(skin_mats[b][i]));
        }
    }

    /* 4. Serialize to canonical MDL memory buffer and round-trip via we_puppet_parse_cpu() */
    we_mdl_write_opts_t wopts;
    we_mdl_write_opts_default(&wopts);
    uint8_t *mdl_bytes = NULL;
    size_t mdl_size = 0u;
    assert(we_mdl_write_mem(&puppet, &wopts, &mdl_bytes, &mdl_size));
    assert(mdl_bytes != NULL && mdl_size > 256u);

    we_puppet_t *parsed = we_puppet_parse_cpu(mdl_bytes, mdl_size);
    assert(parsed != NULL);
    assert(parsed->vertex_count == puppet.vertex_count);
    assert(parsed->triangle_count == puppet.triangle_count);
    assert(parsed->bone_count == puppet.bone_count);
    assert(parsed->anim_count == puppet.anim_count);
    assert(strstr(parsed->bones[4].simulation_json, "\"stiffness\"") != NULL);

    /* 5. Step XPBD physics on the parsed puppet and deform vertices on CPU */
    we_puppet_physics_ctx_t phys_ctx;
    we_physics_init_from_puppet(&phys_ctx, parsed);
    phys_ctx.wind.enabled = 1u;
    phys_ctx.wind.base_speed_px_s = 320.0f;
    assert(phys_ctx.active_bone_count >= 1u);

    float parsed_skin[5][16];
    for (int step = 0; step < 10; ++step) {
        we_puppet_eval_skin_matrices(parsed, NULL, 0u, step * (1.0 / 60.0), parsed_skin);
        we_physics_step_puppet(&phys_ctx, parsed, 1.0f / 60.0f, parsed_skin);
    }

    we_skinned_vertex_t *deformed = calloc(parsed->vertex_count, sizeof(we_skinned_vertex_t));
    assert(deformed != NULL);
    we_puppet_skin_vertices_cpu(parsed, parsed_skin, deformed);
    for (uint32_t v = 0; v < parsed->vertex_count; ++v) {
        assert(isfinite(deformed[v].pos[0]) && isfinite(deformed[v].pos[1]));
    }

    /* 6. Heap-owned rig_ctx + playback_paused preview evaluation + [0,0] locked IK check */
    we_rig_pose_ctx_t *heap_rig = calloc(1u, sizeof(we_rig_pose_ctx_t));
    assert(heap_rig != NULL);
    *heap_rig = rig_ctx;
    heap_rig->ik_chains[0].enabled = 0x3u;
    heap_rig->ik_chains[0].root_angle_min_rad = 0.0f;
    heap_rig->ik_chains[0].root_angle_max_rad = 0.0f;
    heap_rig->ik_chains[0].mid_angle_min_rad = 0.0f;
    heap_rig->ik_chains[0].mid_angle_max_rad = 0.0f;
    parsed->rig_ctx = heap_rig;
    parsed->playback_paused = true;
    parsed->preview_time_sec = 0.025;
    we_puppet_eval_skin_matrices_ex(parsed, layers, 2u, (const we_rig_pose_ctx_t *)parsed->rig_ctx,
                                    parsed->preview_time_sec, parsed_skin);
    for (uint32_t b = 0; b < 5u; ++b) {
        for (int i = 0; i < 16; ++i) {
            assert(isfinite(parsed_skin[b][i]));
        }
    }

    free(deformed);
    we_puppet_destroy(parsed);
    we_mdl_write_free(mdl_bytes);
    we_mesh_gen_result_free(&mesh);

    printf("[Challenger Rigging] ALL E2E CHECKS PASSED.\n");
    return 0;
}
