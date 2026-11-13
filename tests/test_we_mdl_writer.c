// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_asset_writer.h"
#include "waywal/we_json.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_physics.h"
#include "waywal/we_pkg.h"
#include "waywal/we_puppet.h"
#include "waywal/we_scene.h"

#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_trs_col_major(float tx, float ty, float tz, float rz, float sx, float sy, float sz,
                               float out[16])
{
    float c = cosf(rz);
    float s = sinf(rz);
    memset(out, 0, sizeof(float) * 16u);
    out[0] = c * sx;
    out[1] = s * sx;
    out[4] = -s * sy;
    out[5] = c * sy;
    out[10] = sz;
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
    out[15] = 1.0f;
}

static bool approx_eq(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void assert_mat4_near(const float a[16], const float b[16], float eps)
{
    for (int i = 0; i < 16; ++i) {
        assert(approx_eq(a[i], b[i], eps));
    }
}

static void build_test_puppet(we_puppet_t *p, we_mdl_vertex_t *verts, uint16_t *indices,
                              we_mdl_bone_t *bones, we_mdl_anim_t *anims,
                              we_mdl_bone_track_t tracks[3][3], we_mdl_keyframe_t kfs[3][3][4])
{
    memset(p, 0, sizeof(*p));
    memset(verts, 0, sizeof(we_mdl_vertex_t) * 4u);
    memset(bones, 0, sizeof(we_mdl_bone_t) * 3u);
    memset(anims, 0, sizeof(we_mdl_anim_t) * 3u);

    p->mdl_flag = 2;
    p->unk_1 = 7;
    p->unk_2 = 11;
    snprintf(p->mat_json_file, sizeof(p->mat_json_file), "materials/puppet_hero.json");

    p->vertex_count = 4u;
    p->vertices = verts;
    verts[0] = (we_mdl_vertex_t){
        .pos = {-32.0f, -32.0f, 0.0f},
        .blend_indices = {0u, 1u, 0u, 0u},
        .weight = {0.75f, 0.25f, 0.0f, 0.0f},
        .texcoord = {0.0f, 0.0f},
    };
    verts[1] = (we_mdl_vertex_t){
        .pos = {32.0f, -32.0f, 0.0f},
        .blend_indices = {1u, 0u, 0u, 0u},
        .weight = {1.0f, 0.0f, 0.0f, 0.0f},
        .texcoord = {1.0f, 0.0f},
    };
    verts[2] = (we_mdl_vertex_t){
        .pos = {32.0f, 48.0f, 0.5f},
        .blend_indices = {1u, 2u, 0u, 0u},
        .weight = {0.4f, 0.6f, 0.0f, 0.0f},
        .texcoord = {1.0f, 1.0f},
    };
    verts[3] = (we_mdl_vertex_t){
        .pos = {-32.0f, 48.0f, -0.5f},
        .blend_indices = {2u, 0u, 0u, 0u},
        .weight = {1.0f, 0.0f, 0.0f, 0.0f},
        .texcoord = {0.0f, 1.0f},
    };

    p->triangle_count = 2u;
    p->index_count = 6u;
    p->indices = indices;
    indices[0] = 0u;
    indices[1] = 1u;
    indices[2] = 2u;
    indices[3] = 0u;
    indices[4] = 2u;
    indices[5] = 3u;

    p->bone_count = 3u;
    p->bones = bones;

    snprintf(bones[0].name, sizeof(bones[0].name), "root");
    bones[0].parent = 0xFFFFFFFFu;
    bones[0].bind_pos[0] = 10.0f;
    bones[0].bind_pos[1] = 20.0f;
    bones[0].bind_angle[2] = 0.25f;
    bones[0].bind_scale[0] = 1.0f;
    bones[0].bind_scale[1] = 1.0f;
    bones[0].bind_scale[2] = 1.0f;
    make_trs_col_major(10.0f, 20.0f, 0.0f, 0.25f, 1.0f, 1.0f, 1.0f, bones[0].local_trans);
    memcpy(bones[0].world_bind, bones[0].local_trans, sizeof(bones[0].world_bind));
    assert(we_mat4_inverse(bones[0].world_bind, bones[0].offset_trans));

    snprintf(bones[1].name, sizeof(bones[1].name), "arm");
    bones[1].parent = 0u;
    bones[1].bind_pos[0] = 28.0f;
    bones[1].bind_pos[1] = -6.0f;
    bones[1].bind_angle[2] = -0.35f;
    bones[1].bind_scale[0] = 1.05f;
    bones[1].bind_scale[1] = 0.95f;
    bones[1].bind_scale[2] = 1.0f;
    make_trs_col_major(28.0f, -6.0f, 0.0f, -0.35f, 1.05f, 0.95f, 1.0f, bones[1].local_trans);
    we_mat4_mul(bones[0].world_bind, bones[1].local_trans, bones[1].world_bind);
    assert(we_mat4_inverse(bones[1].world_bind, bones[1].offset_trans));
    we_bone_sim_config_t sim1 = {
        .bone_index = 1u,
        .flags = WE_BONE_PHYS_ENABLED,
        .compliance_bend = 0.008f,
        .damping_ratio = 0.35f,
        .mass_kg = 1.25f,
        .gravity_scale = 0.8f,
        .wind_drag_coeff = 0.45f,
        .rest_length_px = 36.0f,
        .audio_drag_coeff = 0.2f,
    };
    assert(we_physics_format_simulation_json(&sim1, bones[1].simulation_json));

    snprintf(bones[2].name, sizeof(bones[2].name), "tip");
    bones[2].parent = 1u;
    bones[2].bind_pos[0] = 16.0f;
    bones[2].bind_pos[1] = 12.0f;
    bones[2].bind_angle[2] = 0.15f;
    bones[2].bind_scale[0] = 1.0f;
    bones[2].bind_scale[1] = 1.0f;
    bones[2].bind_scale[2] = 1.0f;
    make_trs_col_major(16.0f, 12.0f, 0.0f, 0.15f, 1.0f, 1.0f, 1.0f, bones[2].local_trans);
    we_mat4_mul(bones[1].world_bind, bones[2].local_trans, bones[2].world_bind);
    assert(we_mat4_inverse(bones[2].world_bind, bones[2].offset_trans));
    we_bone_sim_config_t sim2 = {
        .bone_index = 2u,
        .flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE,
        .compliance_bend = 0.002f,
        .damping_ratio = 0.6f,
        .mass_kg = 0.5f,
        .gravity_scale = 2.5f,
        .wind_drag_coeff = 1.1f,
        .angle_limit_min_rad = -0.8f,
        .angle_limit_max_rad = 1.1f,
        .collision_radius_px = 18.0f,
        .rest_length_px = 24.0f,
        .audio_drag_coeff = 0.5f,
    };
    assert(we_physics_format_simulation_json(&sim2, bones[2].simulation_json));

    p->anim_count = 3u;
    p->anims = anims;
    const we_mdl_anim_mode_t modes[3] = {WE_ANIM_LOOP, WE_ANIM_MIRROR, WE_ANIM_SINGLE};
    const char *anim_names[3] = {"idle", "sway", "intro"};
    const float fps_vals[3] = {24.0f, 30.0f, 60.0f};
    /* Distinct out-of-order track bone indices guard against the parser all_same_idx quirk. */
    const int32_t bone_order[3] = {2, 0, 1};

    for (uint32_t a = 0u; a < 3u; ++a) {
        anims[a].id = (int32_t)(a + 1u);
        snprintf(anims[a].name, sizeof(anims[a].name), "%s", anim_names[a]);
        anims[a].mode = modes[a];
        anims[a].fps = fps_vals[a];
        anims[a].length = 4;
        anims[a].track_count = 3u;
        anims[a].tracks = tracks[a];
        memset(anims[a].bone_to_track, 0xFF, sizeof(anims[a].bone_to_track));
        anims[a].bone_lut_valid = true;
        for (uint32_t t = 0u; t < 3u; ++t) {
            anims[a].bone_to_track[bone_order[t]] = (int16_t)t;
            tracks[a][t].bone_index = bone_order[t];
            tracks[a][t].unk = 0;
            tracks[a][t].kf_count = 4u;
            tracks[a][t].keyframes = kfs[a][t];
            for (uint32_t k = 0u; k < 4u; ++k) {
                float f = (float)k * 0.25f + (float)a * 0.1f;
                kfs[a][t][k] = (we_mdl_keyframe_t){
                    .pos = {1.0f + f * (float)(t + 1u), -2.0f + f, 0.0f},
                    .angle = {0.0f, 0.0f, 0.1f * (float)(k + 1u) * (t == 1u ? -1.0f : 1.0f)},
                    .scale = {1.0f + 0.02f * (float)k, 1.0f - 0.01f * (float)k, 1.0f},
                };
            }
        }
    }

    p->layer_count = 2u;
    p->layers[0] = (we_puppet_layer_t){
        .anim_id = 1,
        .anim_name = "idle",
        .rate = 1.0f,
        .blend = 0.85f,
        .visible = true,
    };
    p->layers[1] = (we_puppet_layer_t){
        .anim_id = -1,
        .anim_name = "sway",
        .rate = 1.5f,
        .blend = 0.5f,
        .visible = true,
    };
}

static bool read_whole_file(const char *path, uint8_t **out_buf, size_t *out_sz)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1u);
    if (!buf) {
        fclose(fp);
        return false;
    }
    size_t nr = fread(buf, 1u, (size_t)len, fp);
    fclose(fp);
    if (nr != (size_t)len) {
        free(buf);
        return false;
    }
    buf[nr] = '\0';
    *out_buf = buf;
    *out_sz = nr;
    return true;
}

static void rm_rf_tmp(const char *dir)
{
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    assert(n > 0 && (size_t)n < sizeof(cmd));
    int rc = system(cmd);
    (void)rc;
}

static void test_roundtrip_mesh_skeleton_and_animations(void)
{
    we_puppet_t src;
    we_mdl_vertex_t verts[4];
    uint16_t indices[6];
    we_mdl_bone_t bones[3];
    we_mdl_anim_t anims[3];
    we_mdl_bone_track_t tracks[3][3];
    we_mdl_keyframe_t kfs[3][3][4];
    build_test_puppet(&src, verts, indices, bones, anims, tracks, kfs);

    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);
    uint8_t *bytes = NULL;
    size_t sz = 0u;
    assert(we_mdl_write_mem(&src, &opts, &bytes, &sz));
    assert(bytes != NULL && sz > 0u);

    we_puppet_t *dst = we_puppet_parse_cpu(bytes, sz);
    assert(dst != NULL);
    assert(dst->mdlv_version == WE_MDL_WRITER_VERSION_MDLV);
    assert(dst->mdat_version == WE_MDL_WRITER_VERSION_MDAT);
    assert(dst->mdls_version == WE_MDL_WRITER_VERSION_MDLS);
    assert(dst->mdla_version == WE_MDL_WRITER_VERSION_MDLA);
    assert(dst->mdl_flag == src.mdl_flag);
    assert(dst->unk_1 == src.unk_1);
    assert(dst->unk_2 == src.unk_2);
    assert(strcmp(dst->mat_json_file, src.mat_json_file) == 0);
    assert(!dst->alt_mdl_format);
    assert(dst->vertex_count == src.vertex_count);
    assert(dst->triangle_count == src.triangle_count);
    assert(dst->index_count == src.index_count);
    assert(dst->bone_count == src.bone_count);
    assert(dst->anim_count == src.anim_count);

    for (uint32_t i = 0u; i < src.vertex_count; ++i) {
        for (int k = 0; k < 3; ++k) {
            assert(memcmp(&dst->vertices[i].pos[k], &src.vertices[i].pos[k], sizeof(float)) == 0);
        }
        for (int k = 0; k < 4; ++k) {
            assert(dst->vertices[i].blend_indices[k] == src.vertices[i].blend_indices[k]);
            assert(approx_eq(dst->vertices[i].weight[k], src.vertices[i].weight[k], 1e-6f));
        }
        assert(memcmp(dst->vertices[i].texcoord, src.vertices[i].texcoord, sizeof(float) * 2u) ==
               0);
    }
    assert(memcmp(dst->indices, src.indices, sizeof(uint16_t) * src.index_count) == 0);

    for (uint32_t i = 0u; i < src.bone_count; ++i) {
        assert(strcmp(dst->bones[i].name, src.bones[i].name) == 0);
        assert(dst->bones[i].parent == src.bones[i].parent);
        assert_mat4_near(dst->bones[i].local_trans, src.bones[i].local_trans, 1e-6f);
        assert_mat4_near(dst->bones[i].world_bind, src.bones[i].world_bind, 1e-5f);
        assert_mat4_near(dst->bones[i].offset_trans, src.bones[i].offset_trans, 1e-5f);
        assert(strcmp(dst->bones[i].simulation_json, src.bones[i].simulation_json) == 0);

        if (src.bones[i].simulation_json[0] != '\0') {
            we_bone_sim_config_t cfg_src = {0}, cfg_dst = {0};
            assert(we_physics_parse_simulation_json(src.bones[i].simulation_json, &cfg_src));
            assert(we_physics_parse_simulation_json(dst->bones[i].simulation_json, &cfg_dst));
            assert(cfg_src.flags == cfg_dst.flags);
            assert(approx_eq(cfg_src.compliance_bend, cfg_dst.compliance_bend, 1e-5f));
            assert(approx_eq(cfg_src.damping_ratio, cfg_dst.damping_ratio, 1e-5f));
            assert(approx_eq(cfg_src.gravity_scale, cfg_dst.gravity_scale, 1e-4f));
            assert(approx_eq(cfg_src.mass_kg, cfg_dst.mass_kg, 1e-5f));
            assert(approx_eq(cfg_src.rest_length_px, cfg_dst.rest_length_px, 1e-5f));
        }
    }

    for (uint32_t a = 0u; a < src.anim_count; ++a) {
        assert(dst->anims[a].id == src.anims[a].id);
        assert(strcmp(dst->anims[a].name, src.anims[a].name) == 0);
        assert(dst->anims[a].mode == src.anims[a].mode);
        assert(memcmp(&dst->anims[a].fps, &src.anims[a].fps, sizeof(float)) == 0);
        assert(dst->anims[a].length == src.anims[a].length);
        assert(dst->anims[a].track_count == src.anims[a].track_count);
        for (uint32_t t = 0u; t < src.anims[a].track_count; ++t) {
            const we_mdl_bone_track_t *ts = &src.anims[a].tracks[t];
            const we_mdl_bone_track_t *td = &dst->anims[a].tracks[t];
            assert(td->bone_index == ts->bone_index);
            assert(td->kf_count == ts->kf_count);
            for (uint32_t k = 0u; k < ts->kf_count; ++k) {
                assert(memcmp(&td->keyframes[k], &ts->keyframes[k], sizeof(we_mdl_keyframe_t)) ==
                       0);
            }
        }
    }

    /* Re-serialize parsed puppet and verify identical skinning + XPBD physics evaluation. */
    uint8_t *bytes2 = NULL;
    size_t sz2 = 0u;
    assert(we_mdl_write_mem(dst, &opts, &bytes2, &sz2));
    assert(sz2 == sz && memcmp(bytes2, bytes, sz) == 0);
    we_puppet_t *dst2 = we_puppet_parse_cpu(bytes2, sz2);
    assert(dst2 != NULL);

    const double sample_times[3] = {0.0, 0.05, 0.22};
    for (size_t s = 0u; s < 3u; ++s) {
        float skin_dst[3][16];
        float skin_dst2[3][16];
        we_puppet_eval_skin_matrices(dst, src.layers, src.layer_count, sample_times[s], skin_dst);
        we_puppet_eval_skin_matrices(dst2, src.layers, src.layer_count, sample_times[s], skin_dst2);
        for (uint32_t b = 0u; b < src.bone_count; ++b) {
            assert_mat4_near(skin_dst[b], skin_dst2[b], 1e-5f);
        }
    }

    /* Verify FK skinning matches raw src directly when physics simulation_json is absent. */
    bones[1].simulation_json[0] = '\0';
    bones[2].simulation_json[0] = '\0';
    uint8_t *fk_bytes = NULL;
    size_t fk_sz = 0u;
    assert(we_mdl_write_mem(&src, &opts, &fk_bytes, &fk_sz));
    we_puppet_t *fk_dst = we_puppet_parse_cpu(fk_bytes, fk_sz);
    assert(fk_dst != NULL);
    for (size_t s = 0u; s < 3u; ++s) {
        float skin_src[3][16];
        float skin_fk[3][16];
        we_puppet_eval_skin_matrices(&src, src.layers, src.layer_count, sample_times[s], skin_src);
        we_puppet_eval_skin_matrices(fk_dst, src.layers, src.layer_count, sample_times[s], skin_fk);
        for (uint32_t b = 0u; b < src.bone_count; ++b) {
            assert_mat4_near(skin_src[b], skin_fk[b], 1e-5f);
        }
    }

    we_puppet_destroy(fk_dst);
    we_mdl_write_free(fk_bytes);
    we_puppet_destroy(dst2);
    we_mdl_write_free(bytes2);
    we_puppet_destroy(dst);
    we_mdl_write_free(bytes);
}

static void test_simulation_json_and_locale_independence(void)
{
    char old_loc[128] = {0};
    const char *cur = setlocale(LC_NUMERIC, NULL);
    if (cur) {
        snprintf(old_loc, sizeof(old_loc), "%s", cur);
    }
    setlocale(LC_NUMERIC, "de_DE.UTF-8");

    we_bone_sim_config_t cfg = {
        .bone_index = 0u,
        .flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_ANGULAR_LIMIT | WE_BONE_PHYS_CURSOR_COLLIDE,
        .compliance_bend = 0.0125f,
        .damping_ratio = 0.42f,
        .mass_kg = 1.75f,
        .gravity_scale = 2.5f,
        .wind_drag_coeff = 0.85f,
        .angle_limit_min_rad = -1.25f,
        .angle_limit_max_rad = 1.25f,
        .collision_radius_px = 22.5f,
        .rest_length_px = 45.25f,
        .audio_drag_coeff = 0.33f,
    };
    char json[256] = {0};
    assert(we_physics_format_simulation_json(&cfg, json));
    assert(strchr(json, ',') != NULL);
    /* Every floating-point value must use dot decimals even under de_DE.UTF-8. */
    assert(strstr(json, "0,") == NULL);

    we_bone_sim_config_t parsed = {0};
    assert(we_physics_parse_simulation_json(json, &parsed));
    assert(parsed.flags == cfg.flags);
    assert(approx_eq(parsed.compliance_bend, cfg.compliance_bend, 1e-4f));
    assert(approx_eq(parsed.damping_ratio, cfg.damping_ratio, 1e-4f));
    assert(approx_eq(parsed.gravity_scale, cfg.gravity_scale, 1e-4f));
    assert(approx_eq(parsed.mass_kg, cfg.mass_kg, 1e-4f));
    assert(approx_eq(parsed.rest_length_px, cfg.rest_length_px, 1e-4f));
    assert(approx_eq(parsed.wind_drag_coeff, cfg.wind_drag_coeff, 1e-4f));
    assert(approx_eq(parsed.audio_drag_coeff, cfg.audio_drag_coeff, 1e-4f));
    assert(approx_eq(parsed.collision_radius_px, cfg.collision_radius_px, 1e-4f));
    assert(approx_eq(parsed.angle_limit_max_rad, cfg.angle_limit_max_rad, 1e-4f));

    cfg.flags = 0u;
    assert(we_physics_format_simulation_json(&cfg, json));
    assert(json[0] == '\0');

    cfg.flags = WE_BONE_PHYS_ENABLED;
    cfg.damping_ratio = NAN;
    assert(!we_physics_format_simulation_json(&cfg, json));
    assert(!we_physics_format_simulation_json(NULL, json));
    assert(!we_physics_format_simulation_json(&cfg, NULL));

    if (old_loc[0] != '\0') {
        setlocale(LC_NUMERIC, old_loc);
    }
}

static void test_options_flags_and_sanitization(void)
{
    we_puppet_t src;
    we_mdl_vertex_t verts[4];
    uint16_t indices[6];
    we_mdl_bone_t bones[3];
    we_mdl_anim_t anims[3];
    we_mdl_bone_track_t tracks[3][3];
    we_mdl_keyframe_t kfs[3][3][4];
    build_test_puppet(&src, verts, indices, bones, anims, tracks, kfs);

    /* mdl_flag == 9 must be rejected unless override_mdl_flag > 0 is explicitly requested. */
    src.mdl_flag = 9;
    snprintf(bones[0].name, sizeof(bones[0].name), "root\x01\x7f");
    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);
    uint8_t *bytes = NULL;
    size_t sz = 0u;
    assert(!we_mdl_write_mem(&src, &opts, &bytes, &sz));
    assert(!we_mdl_write_mem(&src, NULL, &bytes, &sz));

    opts.override_mdl_flag = 1;
    assert(we_mdl_write_mem(&src, &opts, &bytes, &sz));
    we_puppet_t *dst = we_puppet_parse_cpu(bytes, sz);
    assert(dst != NULL);
    assert(dst->mdl_flag == 1);
    assert(strcmp(dst->bones[0].name, "root__") == 0);
    we_puppet_destroy(dst);
    we_mdl_write_free(bytes);

    /* Strict ASCII mode rejects control characters. */
    src.mdl_flag = 2;
    opts.override_mdl_flag = 0;
    opts.sanitize_ascii_strings = false;
    assert(!we_mdl_write_mem(&src, &opts, &bytes, &sz));

    /* Omit attachments/skeleton/animations + override mdl_flag. */
    snprintf(bones[0].name, sizeof(bones[0].name), "root");
    opts.emit_attachments = false;
    opts.emit_skeleton = false;
    opts.emit_animations = false;
    opts.override_mdl_flag = 5;
    assert(we_mdl_write_mem(&src, &opts, &bytes, &sz));
    dst = we_puppet_parse_cpu(bytes, sz);
    assert(dst != NULL);
    assert(dst->mdl_flag == 5);
    assert(dst->mdat_version == 0u);
    assert(dst->bone_count == 0u);
    assert(dst->anim_count == 0u);
    assert(dst->vertex_count == src.vertex_count);
    we_puppet_destroy(dst);
    we_mdl_write_free(bytes);
}

static void test_invalid_inputs_and_fuzz_rejection(void)
{
    we_puppet_t base;
    we_mdl_vertex_t verts[4];
    uint16_t indices[6];
    we_mdl_bone_t bones[3];
    we_mdl_anim_t anims[3];
    we_mdl_bone_track_t tracks[3][3];
    we_mdl_keyframe_t kfs[3][3][4];
    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);
    uint8_t *bytes = NULL;
    size_t sz = 0u;

    assert(!we_mdl_write_mem(NULL, &opts, &bytes, &sz));
    build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
    assert(!we_mdl_write_mem(&base, &opts, NULL, &sz));
    assert(!we_mdl_write_mem(&base, &opts, &bytes, NULL));

    /* Zero vertices / excessive vertices */
    {
        we_puppet_t bad = base;
        bad.vertex_count = 0u;
        assert(!we_mdl_write_mem(&bad, &opts, &bytes, &sz));
        bad.vertex_count = WE_MDL_MAX_VERTICES + 1u;
        assert(!we_mdl_write_mem(&bad, &opts, &bytes, &sz));
    }
    /* Non-finite vertex pos */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        verts[1].pos[0] = NAN;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
        verts[1].pos[0] = 32.0f;
        verts[1].weight[0] = -0.1f;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
    /* Index bounds */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        indices[2] = 4u;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
        indices[2] = 2u;
        base.index_count = 5u;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
    /* Out-of-bounds blend index */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        verts[0].blend_indices[1] = 3u;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
    /* Bone cycle / non-topological parent / singular matrix */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        bones[0].parent = 1u;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));

        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        bones[1].parent = 2u;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));

        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        bones[1].local_trans[15] = 0.0f;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));

        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        bones[1].local_trans[0] = 0.0f;
        bones[1].local_trans[1] = 0.0f;
        bones[1].local_trans[4] = 0.0f;
        bones[1].local_trans[5] = 0.0f;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
    /* Duplicate track bone_index, bad fps, bad keyframe floats */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        tracks[0][1].bone_index = tracks[0][0].bone_index;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));

        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        anims[0].fps = 0.0f;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
        anims[0].fps = 1001.0f;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));

        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        kfs[1][0][2].angle[2] = INFINITY;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
    /* Invalid mdl_flag bounds */
    {
        build_test_puppet(&base, verts, indices, bones, anims, tracks, kfs);
        base.mdl_flag = -1;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
        base.mdl_flag = 257;
        assert(!we_mdl_write_mem(&base, &opts, &bytes, &sz));
    }
}

static void test_write_file_atomic_and_symlink_rejection(void)
{
    we_puppet_t src;
    we_mdl_vertex_t verts[4];
    uint16_t indices[6];
    we_mdl_bone_t bones[3];
    we_mdl_anim_t anims[3];
    we_mdl_bone_track_t tracks[3][3];
    we_mdl_keyframe_t kfs[3][3][4];
    build_test_puppet(&src, verts, indices, bones, anims, tracks, kfs);

    char tmpl[] = "/tmp/we_mdl_file_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);

    char out_path[256], victim_path[256], link_path[256], sub_dir[256], dir_link[256],
        linked_file[256];
    snprintf(out_path, sizeof(out_path), "%s/char.mdl", dir);
    snprintf(victim_path, sizeof(victim_path), "%s/victim.txt", dir);
    snprintf(link_path, sizeof(link_path), "%s/link.mdl", dir);
    snprintf(sub_dir, sizeof(sub_dir), "%s/real_sub", dir);
    snprintf(dir_link, sizeof(dir_link), "%s/link_sub", dir);
    snprintf(linked_file, sizeof(linked_file), "%s/link_sub/out.mdl", dir);

    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);
    assert(we_mdl_write_file(&src, &opts, out_path));
    /* Atomic overwrite succeeds cleanly. */
    src.unk_1 = 99;
    assert(we_mdl_write_file(&src, &opts, out_path));

    uint8_t *disk = NULL;
    size_t disk_sz = 0u;
    assert(read_whole_file(out_path, &disk, &disk_sz));
    we_puppet_t *parsed = we_puppet_parse_cpu(disk, disk_sz);
    assert(parsed != NULL);
    assert(parsed->unk_1 == 99);
    we_puppet_destroy(parsed);
    free(disk);

    FILE *vf = fopen(victim_path, "wb");
    assert(vf != NULL);
    fwrite("KEEP_INTACT", 1u, 11u, vf);
    fclose(vf);
    assert(symlink(victim_path, link_path) == 0);
    assert(!we_mdl_write_file(&src, &opts, link_path));

    assert(read_whole_file(victim_path, &disk, &disk_sz));
    assert(disk_sz == 11u && memcmp(disk, "KEEP_INTACT", 11u) == 0);
    free(disk);

    assert(mkdir(sub_dir, 0755) == 0);
    assert(symlink(sub_dir, dir_link) == 0);
    assert(!we_mdl_write_file(&src, &opts, linked_file));

    char nested_dir[512], nested_file[512];
    int n_dir = snprintf(nested_dir, sizeof(nested_dir), "%s/nested", sub_dir);
    assert(n_dir > 0 && (size_t)n_dir < sizeof(nested_dir));
    assert(mkdir(nested_dir, 0755) == 0);
    int n_file = snprintf(nested_file, sizeof(nested_file), "%s/link_sub/nested/deep.mdl", dir);
    assert(n_file > 0 && (size_t)n_file < sizeof(nested_file));
    assert(!we_mdl_write_file(&src, &opts, nested_file));

    rm_rf_tmp(dir);
}

static void test_scene_writer_puppet_and_sidecar_roundtrip(void)
{
    we_puppet_t puppet;
    we_mdl_vertex_t verts[4];
    uint16_t indices[6];
    we_mdl_bone_t bones[3];
    we_mdl_anim_t anims[3];
    we_mdl_bone_track_t tracks[3][3];
    we_mdl_keyframe_t kfs[3][3][4];
    build_test_puppet(&puppet, verts, indices, bones, anims, tracks, kfs);
    /* Clear mat_json_file so scene writer synthesizes materials/<stem>.json automatically. */
    puppet.mat_json_file[0] = '\0';

    /* Attach non-empty rig_ctx so sidecar serialization exercises IK/Aim/drive modes. */
    we_rig_pose_ctx_t rig_ctx;
    memset(&rig_ctx, 0, sizeof(rig_ctx));
    rig_ctx.ik_chain_count = 1u;
    rig_ctx.ik_chains[0] = (we_ik_chain_t){
        .root_bone = 0u,
        .mid_bone = 1u,
        .end_bone = 2u,
        .bend_sign = 1,
        .enabled = 1u,
        .target_world = {10.0f, 20.0f, 0.0f},
        .pole_world = {0.0f, 1.0f, 0.0f},
        .soft_zone_ratio = 0.05f,
        .max_stretch_ratio = 1.0f,
        .root_angle_min_rad = -1.5f,
        .root_angle_max_rad = 1.5f,
        .mid_angle_min_rad = -1.5f,
        .mid_angle_max_rad = 1.5f,
        .blend_weight = 1.0f,
    };
    rig_ctx.aim_count = 1u;
    rig_ctx.aim_constraints[0] = (we_aim_constraint_t){
        .bone_index = 2u,
        .enabled = 1u,
        .aim_axis_local = {1.0f, 0.0f, 0.0f},
        .target_world = {30.0f, 5.0f, 0.0f},
        .max_cone_rad = 1.0f,
        .blend_weight = 0.75f,
    };
    rig_ctx.bone_drive_modes[0] = WE_BONE_DRIVE_FK;
    rig_ctx.bone_drive_modes[1] = WE_BONE_DRIVE_IK;
    rig_ctx.bone_drive_modes[2] = WE_BONE_DRIVE_AIM;
    puppet.rig_ctx = &rig_ctx;

    we_scene_object_t obj = {0};
    obj.id = 42;
    obj.parent_id = -1;
    snprintf(obj.name, sizeof(obj.name), "Hero Character!");
    snprintf(obj.image_path, sizeof(obj.image_path), "models/legacy.json");
    obj.scale[0] = 1.0f;
    obj.scale[1] = 1.0f;
    obj.scale[2] = 1.0f;
    obj.size[0] = 64.0f;
    obj.size[1] = 80.0f;
    obj.alpha = 1.0f;
    obj.brightness = 1.0f;
    obj.color[0] = 1.0f;
    obj.color[1] = 1.0f;
    obj.color[2] = 1.0f;
    obj.visible = true;
    obj.puppet = &puppet;

    we_scene_t scene = {0};
    snprintf(scene.project.title, sizeof(scene.project.title), "Puppet Export Scene");
    snprintf(scene.project.type, sizeof(scene.project.type), "scene");
    snprintf(scene.project.file, sizeof(scene.project.file), "scene.json");
    scene.ortho_width = 1920.0f;
    scene.ortho_height = 1080.0f;
    scene.object_count = 1u;
    scene.objects = &obj;

    we_asset_writer_err_t err = WE_WRITER_OK;
    char *sjson = NULL;
    size_t slen = 0u;
    assert(we_scene_serialize_json(&scene, &sjson, &slen, &err));
    assert(strstr(sjson, "\"puppet\":\"models/hero_character_42.mdl\"") != NULL);
    assert(strstr(sjson, "\"puppet_layers\":[") != NULL);
    assert(strstr(sjson, "\"image\":") == NULL);
    free(sjson);

    char tmpl_loose[] = "/tmp/we_mdl_scene_loose_XXXXXX";
    char tmpl_pkg[] = "/tmp/we_mdl_scene_pkg_XXXXXX";
    char *dir_loose = mkdtemp(tmpl_loose);
    char *dir_pkg = mkdtemp(tmpl_pkg);
    assert(dir_loose != NULL && dir_pkg != NULL);

    assert(we_scene_export_dir(&scene, dir_loose, false, &err) && err == WE_WRITER_OK);
    assert(we_scene_export_dir(&scene, dir_pkg, true, &err) && err == WE_WRITER_OK);

    /* Verify studio_rig_42.json exists alongside project.json and stays out of scene.pkg. */
    char sidecar_path[256];
    snprintf(sidecar_path, sizeof(sidecar_path), "%s/studio_rig_42.json", dir_pkg);
    uint8_t *sc_bytes = NULL;
    size_t sc_sz = 0u;
    assert(read_whole_file(sidecar_path, &sc_bytes, &sc_sz));
    we_jval_t *sc_root = we_json_parse_root((const char *)sc_bytes, sc_sz);
    assert(sc_root != NULL);
    const we_jval_t *v_node = we_jobj_get(sc_root, "version");
    const we_jval_t *id_node = we_jobj_get(sc_root, "object_id");
    assert(v_node && v_node->type == JVAL_NUM && (int)v_node->num == 1);
    assert(id_node && id_node->type == JVAL_NUM && (int)id_node->num == 42);
    assert(strcmp(we_jval_str(we_jobj_get(sc_root, "puppet"), ""),
                  "models/hero_character_42.mdl") == 0);
    const we_jval_t *drives = we_jobj_get(sc_root, "bone_drive_modes");
    assert(drives != NULL && drives->type == JVAL_ARR && drives->arr.count == 3u);
    assert(strcmp(we_jval_str(we_jobj_get(drives->arr.items[1], "drive_mode"), ""), "IK") == 0);
    assert(strcmp(we_jval_str(we_jobj_get(drives->arr.items[2], "drive_mode"), ""), "Aim") == 0);
    const we_jval_t *iks = we_jobj_get(sc_root, "ik_chains");
    assert(iks != NULL && iks->type == JVAL_ARR && iks->arr.count == 1u);
    const we_jval_t *aims = we_jobj_get(sc_root, "aim_constraints");
    assert(aims != NULL && aims->type == JVAL_ARR && aims->arr.count == 1u);
    const we_jval_t *curves = we_jobj_get(sc_root, "animation_curves");
    assert(curves != NULL && curves->type == JVAL_ARR && curves->arr.count == 3u);
    we_jval_free(sc_root);
    free(sc_bytes);

    char pkg_path[256];
    snprintf(pkg_path, sizeof(pkg_path), "%s/scene.pkg", dir_pkg);
    we_pkg_t *pkg = we_pkg_open_file(pkg_path, dir_pkg);
    assert(pkg != NULL);
    size_t entry_sz = 0u;
    assert(we_pkg_find(pkg, "models/hero_character_42.mdl", &entry_sz) != NULL);
    assert(we_pkg_find(pkg, "materials/hero_character_42.json", &entry_sz) != NULL);
    assert(we_pkg_find(pkg, "models/hero_character_42.json", &entry_sz) != NULL);
    assert(we_pkg_find(pkg, "studio_rig_42.json", &entry_sz) == NULL);
    we_pkg_close(pkg);

    const char *dirs[2] = {dir_loose, dir_pkg};
    for (int d = 0; d < 2; ++d) {
        we_scene_t *loaded = we_scene_load_cpu(dirs[d], NULL);
        assert(loaded != NULL);
        assert(loaded->object_count == 1u);
        const we_scene_object_t *lo = &loaded->objects[0];
        assert(lo->id == 42);
        assert(lo->has_base_pass);
        assert(strcmp(lo->base_pass.shader_name, "genericimage2") == 0);
        assert(lo->puppet != NULL);
        assert(strcmp(lo->puppet->mat_json_file, "materials/hero_character_42.json") == 0);
        assert(lo->puppet->vertex_count == puppet.vertex_count);
        assert(lo->puppet->triangle_count == puppet.triangle_count);
        assert(lo->puppet->bone_count == puppet.bone_count);
        assert(lo->puppet->anim_count == puppet.anim_count);
        assert(lo->puppet->layer_count == puppet.layer_count);
        assert(lo->puppet->layers[0].anim_id == 1);
        assert(approx_eq(lo->puppet->layers[0].blend, 0.85f, 1e-5f));
        assert(strcmp(lo->puppet->layers[1].anim_name, "sway") == 0);
        assert(approx_eq(lo->puppet->layers[1].rate, 1.5f, 1e-5f));
        we_scene_destroy(loaded);
    }

    rm_rf_tmp(dir_loose);
    rm_rf_tmp(dir_pkg);
}

int main(void)
{
    test_roundtrip_mesh_skeleton_and_animations();
    test_simulation_json_and_locale_independence();
    test_options_flags_and_sanitization();
    test_invalid_inputs_and_fuzz_rejection();
    test_write_file_atomic_and_symlink_rejection();
    test_scene_writer_puppet_and_sidecar_roundtrip();
    printf("All we_mdl_writer tests passed.\n");
    return 0;
}
