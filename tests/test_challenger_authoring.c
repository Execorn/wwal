// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_asset_writer.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"
#include "waywal/we_tex.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_challenger_pkg_and_lz4_bounds(void)
{
    printf("[Challenger] Testing PKGV0022 writer adversarial path & duplicate rejections...\n");

    const uint8_t dummy[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t *buf = NULL;
    size_t sz = 0;
    we_asset_writer_err_t err = WE_WRITER_OK;

    const char *bad_paths[] = {
        "",
        "/",
        "\\",
        "../secret.frag",
        "shaders/../../escape.json",
        "shaders/./file.frag",
        "shaders/",
        "bad\x07path.txt",
    };

    for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
        we_pkg_write_entry_t e = {.path = bad_paths[i], .data = dummy, .size = sizeof(dummy)};
        assert(!we_pkg_write_mem(&e, 1u, &buf, &sz, &err));
        assert(buf == NULL);
        assert(err == WE_WRITER_ERR_INVALID_PATH);
    }

    /* Case-normalized duplicates must be rejected */
    we_pkg_write_entry_t dup_entries[2] = {
        {.path = "Shaders/Glow.frag", .data = dummy, .size = sizeof(dummy)},
        {.path = "shaders/glow.frag", .data = dummy, .size = sizeof(dummy)},
    };
    assert(!we_pkg_write_mem(dup_entries, 2u, &buf, &sz, &err));
    assert(err == WE_WRITER_ERR_DUPLICATE_PATH);

    /* Unsorted valid entries must sort canonically and round-trip cleanly */
    we_pkg_write_entry_t valid_entries[2] = {
        {.path = "z_last.txt", .data = (const uint8_t *)"zz", .size = 2u},
        {.path = "a_first.txt", .data = (const uint8_t *)"aa", .size = 2u},
    };
    assert(we_pkg_write_mem(valid_entries, 2u, &buf, &sz, &err));
    assert(buf != NULL && sz > 0 && err == WE_WRITER_OK);

    we_pkg_t *pkg = we_pkg_open_mem(buf, sz, NULL);
    assert(pkg != NULL);
    size_t s0 = 0, s1 = 0;
    const uint8_t *e0 = we_pkg_find(pkg, "a_first.txt", &s0);
    const uint8_t *e1 = we_pkg_find(pkg, "z_last.txt", &s1);
    assert(e0 != NULL && s0 == 2u && memcmp(e0, "aa", 2u) == 0);
    assert(e1 != NULL && s1 == 2u && memcmp(e1, "zz", 2u) == 0);
    assert(e0 < e1);
    we_pkg_close(pkg);
    free(buf);

    /* LZ4 overflow & truncated capacity checks */
    assert(we_lz4_compress_bound((size_t)WE_LZ4_MAX_INPUT_SIZE + 1u) == 0u);
    uint8_t tiny[2] = {0};
    uint8_t src[64];
    for (size_t i = 0; i < sizeof(src); ++i) {
        src[i] = (uint8_t)(i * 37u + 11u);
    }
    assert(we_lz4_compress_block(src, sizeof(src), tiny, sizeof(tiny)) == 0u);
    assert(we_lz4_compress_block(NULL, 16u, tiny, sizeof(tiny)) == 0u);

    printf("  -> PASS: PKGV0022 & LZ4 boundary checks verified.\n");
}

static void test_challenger_scene_tex_serialization_guards(void)
{
    printf("[Challenger] Testing we_tex_encode_v5 & we_scene_serialize_json NaN/range guards...\n");

    uint8_t *out = NULL;
    size_t out_sz = 0;
    we_asset_writer_err_t err = WE_WRITER_OK;

    const uint8_t px[16] = {0};
    we_tex_encode_mip_t bad_mip = {.width = 2u, .height = 2u, .pixels = px, .byte_size = 8u};
    we_tex_encode_image_t bad_img = {.mip_count = 1u, .mips = &bad_mip};
    we_tex_encode_spec_t bad_spec = {
        .format = WE_TEX_FMT_RGBA8888,
        .flags = WE_TEX_FLAG_NONE,
        .texture_width = 2u,
        .texture_height = 2u,
        .image_width = 2u,
        .image_height = 2u,
        .enable_lz4 = true,
        .image_count = 1u,
        .images = &bad_img,
    };
    assert(!we_tex_encode_v5(&bad_spec, &out, &out_sz, &err));
    assert(out == NULL);

    /* image_width exceeding texture_width must be rejected */
    we_tex_encode_mip_t ok_mip = {.width = 2u, .height = 2u, .pixels = px, .byte_size = 16u};
    we_tex_encode_image_t ok_img = {.mip_count = 1u, .mips = &ok_mip};
    we_tex_encode_spec_t ov_spec = bad_spec;
    ov_spec.images = &ok_img;
    ov_spec.image_width = 4u;
    assert(!we_tex_encode_v5(&ov_spec, &out, &out_sz, &err));

    /* Non-finite scene transforms must fail with WE_WRITER_ERR_NON_FINITE */
    we_scene_t scene = {0};
    scene.ortho_width = 1920.0f;
    scene.ortho_height = 1080.0f;
    scene.objects = calloc(1u, sizeof(we_scene_object_t));
    assert(scene.objects != NULL);
    scene.object_count = 1u;
    scene.objects[0].id = 1;
    snprintf(scene.objects[0].name, sizeof(scene.objects[0].name), "layer_nan");
    scene.objects[0].origin[0] = NAN;
    scene.objects[0].scale[0] = 1.0f;
    scene.objects[0].scale[1] = 1.0f;
    scene.objects[0].scale[2] = 1.0f;

    char *json = NULL;
    size_t json_len = 0;
    err = WE_WRITER_OK;
    assert(!we_scene_serialize_json(&scene, &json, &json_len, &err));
    assert(err == WE_WRITER_ERR_NON_FINITE);
    assert(json == NULL);

    /* Infinite layer scale must fail safely too */
    scene.objects[0].origin[0] = 10.0f;
    scene.objects[0].scale[0] = INFINITY;
    err = WE_WRITER_OK;
    assert(!we_scene_serialize_json(&scene, &json, &json_len, &err));
    assert(err == WE_WRITER_ERR_NON_FINITE);

    free(scene.objects);
    printf("  -> PASS: Non-finite transform & TEX range rejections verified.\n");
}

static void test_challenger_particles_audio_and_script_lint(void)
{
    printf("[Challenger] Testing particle capacity bounds, zero-dt audio step & script lint...\n");

    float lut[WE_PARTICLE_RAMP_SAMPLES];
    assert(!we_particle_bake_scalar_lut(NULL, 0u, lut));

    assert(!we_particle_realloc_capacity(NULL, 16u));
    we_particle_sys_t *sys = we_particle_create_preset(WE_PARTICLE_PRESET_RAIN, false);
    assert(sys != NULL);
    assert(we_particle_realloc_capacity(sys, UINT32_MAX));
    assert(sys->max_count <= WE_PARTICLE_MAX_CAP);
    we_particle_trigger_burst(sys, 24u);
    assert(sys->alive_count == 24u);
    assert(we_particle_realloc_capacity(sys, 4u));
    assert(sys->max_count == 4u);
    assert(sys->alive_count == 24u);

    const we_particle_curve_knot_t nan_knot[1] = {{0.0f, NAN, 0.0f, 0.0f, 0u, 0u}};
    assert(!we_particle_bake_scalar_lut(nan_knot, 1u, lut));
    const we_particle_color_stop_t inf_stops[2] = {
        {0.0f, {0.0f, 0.0f, 0.0f}},
        {1.0f, {INFINITY, 1.0f, 1.0f}},
    };
    float ok_r[WE_PARTICLE_RAMP_SAMPLES], ok_g[WE_PARTICLE_RAMP_SAMPLES],
        ok_b[WE_PARTICLE_RAMP_SAMPLES];
    assert(!we_particle_bake_oklab_lut(inf_stops, 2u, ok_r, ok_g, ok_b));

    const we_particle_curve_knot_t valid_knots[2] = {
        {0.0f, 0.5f, 0.0f, 0.0f, 0u, 0u},
        {1.0f, 1.0f, 0.0f, 0.0f, 0u, 0u},
    };
    assert(we_particle_bake_scalar_lut(valid_knots, 2u, lut));
    sys->operators[0].type = WE_OP_TURBULENCE;
    if (sys->operator_count < 1u)
        sys->operator_count = 1u;
    we_particle_ramp_lut_t bad_ramp;
    memset(&bad_ramp, 0, sizeof(bad_ramp));
    memcpy(bad_ramp.scalar_lut, lut, sizeof(lut));
    bad_ramp.operator_index = 0u;
    bad_ramp.channel_kind = 0u; /* WE_OP_ALPHA_FADE */
    assert(!we_particle_apply_ramp_lut(sys, &bad_ramp));
    we_particle_destroy(sys);

    static const char *adv_json =
        "{\"maxcount\": 1e30, \"flags\": -1, \"starttime\": 1e30, "
        "\"renderer\": [{\"name\": \"sprite\", \"subdivision\": 1e30}], "
        "\"emitter\": [{\"name\": \"sphererandom\", \"rate\": 5.0, \"instantaneous\": 1e30}]}";
    we_particle_sys_t *adv_sys = we_particle_create_from_json(adv_json, strlen(adv_json));
    assert(adv_sys != NULL);
    assert(adv_sys->max_count <= WE_PARTICLE_MAX_CAP && adv_sys->max_count > 0u);
    assert(isfinite(adv_sys->start_time));
    we_particle_destroy(adv_sys);

    static const char *neg_json = "{\"maxcount\": -1, \"starttime\": 0.0}";
    we_particle_sys_t *neg_sys = we_particle_create_from_json(neg_json, strlen(neg_json));
    assert(neg_sys != NULL);
    assert(neg_sys->max_count >= 1u && neg_sys->max_count <= WE_PARTICLE_MAX_CAP);
    we_particle_destroy(neg_sys);

    /* Zero dt and non-finite FFT inputs must not poison audio envelope state */
    we_scene_t scene = {0};
    scene.objects = calloc(1u, sizeof(we_scene_object_t));
    assert(scene.objects != NULL);
    scene.object_count = 1u;
    scene.objects[0].id = 5;
    scene.objects[0].alpha = 0.75f;
    scene.objects[0].scale[0] = 1.0f;
    scene.objects[0].scale[1] = 1.0f;
    scene.objects[0].scale[2] = 1.0f;

    we_audio_binding_t b = {0};
    b.binding_id = 1u;
    b.target_object_id = 5;
    b.target_kind = WE_AUDIO_TGT_LAYER_ALPHA;
    b.src_kind = WE_AUDIO_SRC_ENVELOPE;
    b.input_gain = 1.0f;
    b.out_min = 0.25f;
    b.out_max = 1.0f;
    b.attack_sec = 0.05f;
    b.release_sec = 0.15f;
    b.enabled = 1u;
    assert(we_scene_apply_audio_bindings(&scene, WE_AUDIO_BIND_OP_REPLACE_ALL, &b, 1u, 0u));

    waywal_audio_frame_t frame = {0};
    frame.envelope = NAN;
    frame.bands[0] = INFINITY;
    we_scene_step_audio_bindings(&scene, &frame, 0.0f);
    assert(isfinite(scene.objects[0].alpha));
    we_scene_step_audio_bindings(&scene, &frame, 0.016f);
    assert(isfinite(scene.objects[0].alpha));

    free(scene.objects);

    /* Script linter must diagnose nesting depth overflow cleanly */
    char deep[256];
    for (int i = 0; i < 80; ++i) {
        deep[i] = '(';
    }
    deep[80] = 'x';
    for (int i = 0; i < 80; ++i) {
        deep[81 + i] = ')';
    }
    deep[161] = '\0';
    we_script_diag_t diag = {0};
    assert(!we_script_lint(deep, strlen(deep), 0u, &diag));
    assert(diag.error_line >= 1u);

    printf("  -> PASS: Particle, audio non-finite/dt, and script lint guards verified.\n");
}

static void test_challenger_ipc_authoring_wire_fuzz(void)
{
    printf("[Challenger] Testing 0x0021..0x002A authoring wire validators against malformed "
           "sizes...\n");

    waywal_ipc_req_scene_mutate_t mutate = {0};
    const char *json_ptr = NULL;
    /* Truncated packet smaller than fixed header must fail */
    assert(!waywal_ipc_validate_req_scene_mutate(&mutate, sizeof(mutate) - 1u, &json_ptr));
    /* Trailing length mismatch must fail */
    mutate.payload_len = 32u;
    assert(!waywal_ipc_validate_req_scene_mutate(&mutate, sizeof(mutate) + 16u, &json_ptr));

    /* Brush stroke dab overflow & NaN check */
    waywal_ipc_req_brush_stroke_t brush = {0};
    brush.action = WE_BRUSH_IPC_DAB_BATCH;
    brush.dab_count = WE_BRUSH_MAX_STROKE_DABS + 1u;
    const we_brush_dab_t *dabs_ptr = NULL;
    uint32_t dab_count = 0;
    assert(!waywal_ipc_validate_req_brush_stroke(
        &brush, sizeof(brush) + (size_t)brush.dab_count * sizeof(we_brush_dab_t), &dabs_ptr,
        &dab_count));

    brush.dab_count = 0u;
    brush.action = 99u;
    assert(!waywal_ipc_validate_req_brush_stroke(&brush, sizeof(brush), &dabs_ptr, &dab_count));

    /* Audio binding overflow check */
    waywal_ipc_req_set_audio_bindings_t ab = {0};
    ab.bind_op = WE_AUDIO_BIND_OP_REPLACE_ALL;
    ab.binding_count = WE_MAX_AUDIO_BINDINGS + 1u;
    const we_audio_binding_t *bindings_ptr = NULL;
    uint32_t bcount = 0;
    assert(!waywal_ipc_validate_req_set_audio_bindings(
        &ab, sizeof(ab) + (size_t)ab.binding_count * sizeof(we_audio_binding_t), &bindings_ptr,
        &bcount));

    /* Readback oversized length check */
    waywal_ipc_resp_readback_texture_t rb = {0};
    rb.data_len = WAYWAL_IPC_MAX_READBACK_BYTES + 1u;
    const void *rb_data = NULL;
    uint32_t rb_len = 0;
    assert(!waywal_ipc_validate_resp_readback_texture(&rb, sizeof(rb) + rb.data_len, &rb_data,
                                                      &rb_len));

    printf("  -> PASS: Authoring IPC malformed wire rejection checks passed.\n");
}

int main(void)
{
    printf("=================================================================\n");
    printf("Starting Authoring Challenger Adversarial Test Suite\n");
    printf("=================================================================\n");

    test_challenger_pkg_and_lz4_bounds();
    test_challenger_scene_tex_serialization_guards();
    test_challenger_particles_audio_and_script_lint();
    test_challenger_ipc_authoring_wire_fuzz();

    printf("=================================================================\n");
    printf("All Authoring Challenger Tests PASSED!\n");
    printf("=================================================================\n");
    return 0;
}
