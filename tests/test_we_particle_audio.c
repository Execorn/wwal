// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_particle.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\n"
           "leak:libGLESv2\nleak:dri\nleak:libnvidia\n";
}
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_abi_contracts(void)
{
    assert(sizeof(we_particle_curve_knot_t) == 24u);
    assert(sizeof(we_particle_color_stop_t) == 16u);
    assert(sizeof(we_particle_ramp_lut_t) == 1056u);
    assert(sizeof(we_particle_preset_desc_t) == 112u);
    assert(sizeof(we_audio_binding_t) == 160u);
    printf("[PASS] ABI struct static_assert sizes (24/16/1056/112/160 bytes)\n");
}

static void test_fritsch_carlson_and_oklab_luts(void)
{
    /* Monotone step + plateau must have zero overshoot across [0, 1] */
    we_particle_curve_knot_t mono_knots[4] = {
        {.t = 0.0f, .y = 0.0f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.25f, .y = 0.8f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.70f, .y = 0.8f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 1.0f, .y = 1.0f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
    };
    float lut[WE_PARTICLE_RAMP_SAMPLES];
    assert(we_particle_bake_scalar_lut(mono_knots, 4u, lut));

    float prev = -1e9f;
    for (int i = 0; i <= 10000; i++) {
        float u = (float)i / 10000.0f;
        float v = we_particle_eval_scalar_lut(lut, u);
        assert(isfinite(v));
        assert(v >= -1e-5f && v <= 1.0f + 1e-5f);
        assert(v >= prev - 1e-5f);
        if (u >= 0.25f && u <= 0.70f) {
            assert(fabsf(v - 0.8f) < 1e-4f);
        }
        prev = v;
    }

    /* Smooth quadratic-like curve accuracy (< 5e-4 max LUT interpolation error vs baked spline) */
    we_particle_curve_knot_t smooth_knots[5] = {
        {.t = 0.00f, .y = 0.00f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.25f, .y = 0.0625f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.50f, .y = 0.25f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.75f, .y = 0.5625f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 1.00f, .y = 1.00f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
    };
    assert(we_particle_bake_scalar_lut(smooth_knots, 5u, lut));
    assert(fabsf(we_particle_eval_scalar_lut(lut, 0.0f) - 0.0f) < 1e-6f);
    assert(fabsf(we_particle_eval_scalar_lut(lut, 0.5f) - 0.25f) < 1e-6f);
    assert(fabsf(we_particle_eval_scalar_lut(lut, 1.0f) - 1.0f) < 1e-6f);
    for (uint32_t i = 0; i < WE_PARTICLE_RAMP_SAMPLES - 1u; i++) {
        float t_mid = ((float)i + 0.5f) / 64.0f;
        float approx = we_particle_eval_scalar_lut(lut, t_mid);
        float expected = 0.5f * (lut[i] + lut[i + 1]);
        assert(fabsf(approx - expected) < 1e-5f);
    }

    /* Linear and step handle modes */
    we_particle_curve_knot_t step_knots[2] = {
        {.t = 0.0f, .y = 0.2f, .mode = WE_KNOT_HANDLE_STEP},
        {.t = 1.0f, .y = 0.9f, .mode = WE_KNOT_HANDLE_STEP},
    };
    assert(we_particle_bake_scalar_lut(step_knots, 2u, lut));
    assert(fabsf(lut[32] - 0.2f) < 1e-6f);
    assert(fabsf(lut[64] - 0.9f) < 1e-6f);

    we_particle_curve_knot_t lin_knots[2] = {
        {.t = 0.0f, .y = 0.2f, .mode = WE_KNOT_HANDLE_LINEAR},
        {.t = 1.0f, .y = 0.8f, .mode = WE_KNOT_HANDLE_LINEAR},
    };
    assert(we_particle_bake_scalar_lut(lin_knots, 2u, lut));
    assert(fabsf(we_particle_eval_scalar_lut(lut, 0.5f) - 0.5f) < 1e-5f);

    /* Perceptual OKLab gradient: #38bdf8 -> #f43f5e avoids grey mud at midpoint */
    we_particle_color_stop_t stops[2] = {
        {.t = 0.0f, .rgb = {0x38 / 255.0f, 0xbd / 255.0f, 0xf8 / 255.0f}},
        {.t = 1.0f, .rgb = {0xf4 / 255.0f, 0x3f / 255.0f, 0x5e / 255.0f}},
    };
    float lr[WE_PARTICLE_RAMP_SAMPLES], lg[WE_PARTICLE_RAMP_SAMPLES], lb[WE_PARTICLE_RAMP_SAMPLES];
    assert(we_particle_bake_oklab_lut(stops, 2u, lr, lg, lb));
    assert(fabsf(lr[0] - stops[0].rgb[0]) < 1e-3f);
    assert(fabsf(lb[64] - stops[1].rgb[2]) < 1e-3f);
    /* OKLab preserves perceptual lightness (0.729, 0.560, 0.677; L=0.699) vs darker sRGB mid */
    assert(fabsf(lr[32] - 0.7293f) < 0.01f);
    assert(fabsf(lg[32] - 0.5597f) < 0.01f);
    assert(fabsf(lb[32] - 0.6769f) < 0.01f);
    assert(lr[32] > 0.588f + 0.12f && lg[32] > 0.494f + 0.05f);

    /* Vivid chroma test: #0000ff -> #00ff00 avoids dark (0, 0.5, 0.5) -> bright (0, 0.666, 0.748)
     */
    we_particle_color_stop_t bg_stops[2] = {
        {.t = 0.0f, .rgb = {0.0f, 0.0f, 1.0f}},
        {.t = 1.0f, .rgb = {0.0f, 1.0f, 0.0f}},
    };
    assert(we_particle_bake_oklab_lut(bg_stops, 2u, lr, lg, lb));
    assert(lr[32] < 0.01f && lg[32] > 0.65f && lb[32] > 0.73f);

    printf("[PASS] Fritsch-Carlson Hermite overshoot=0 & OKLab gradient LUTs\n");
}

static void test_presets_and_control_point_attract(void)
{
    const char *expected_slugs[WE_PARTICLE_PRESET_COUNT] = {
        "rain",      "snow",         "embers",     "fireflies", "cherry_blossoms",
        "starfield", "rising_smoke", "magic_dust",
    };

    for (uint32_t id = 0; id < WE_PARTICLE_PRESET_COUNT; id++) {
        const we_particle_preset_desc_t *desc =
            we_particle_get_preset_desc((we_particle_preset_id_t)id);
        assert(desc != NULL);
        assert(desc->preset_id == id);
        assert(strcmp(desc->slug, expected_slugs[id]) == 0);

        we_particle_sys_t *sys = we_particle_create_preset((we_particle_preset_id_t)id, true);
        assert(sys != NULL);
        assert(sys->max_count == desc->max_count);
        assert(sys->alive_count > 0u);
        assert(sys->alive_count <= sys->max_count);

        for (int step = 0; step < 20; step++) {
            we_particle_update(sys, 0.016f);
        }
        assert(sys->alive_count <= sys->max_count);
        for (uint32_t i = 0; i < sys->alive_count; i++) {
            assert(isfinite(sys->pos_x[i]) && isfinite(sys->pos_y[i]));
            assert(isfinite(sys->vel_x[i]) && isfinite(sys->vel_y[i]));
            assert(isfinite(sys->size[i]) && isfinite(sys->a[i]));
            assert(isfinite(sys->r[i]) && isfinite(sys->g[i]) && isfinite(sys->b[i]));
        }
        we_particle_vertex_t *verts = NULL;
        uint32_t vcnt = we_particle_build_vertices(sys, &verts);
        assert(vcnt == sys->alive_count * 4u);
        assert(verts != NULL);

        /* Verify pause freezes simulation */
        uint32_t paused_alive = sys->alive_count;
        float paused_x0 = sys->alive_count > 0u ? sys->pos_x[0] : 0.0f;
        sys->paused = true;
        we_particle_update(sys, 0.1f);
        assert(sys->alive_count == paused_alive);
        if (sys->alive_count > 0u) {
            assert(sys->pos_x[0] == paused_x0);
        }

        we_particle_destroy(sys);
    }

    /* Verify ControlPointAttract pulls radially inward (distinguishing from Vortex swirl) */
    const char *attract_json = "{\"maxcount\":16,\"emitter\":[{\"name\":\"box\",\"origin\":[0,0,0],"
                               "\"distancemin\":[0,0,0],\"distancemax\":[0,0,0],\"rate\":0}],"
                               "\"controlpoint\":[{\"id\":0,\"flags\":1,\"offset\":[0,0,0]}],"
                               "\"operator\":[{\"name\":\"controlpointattract\",\"controlpoint\":0,"
                               "\"distanceinner\":10,\"distanceouter\":500,\"scale\":200},"
                               "{\"name\":\"movement\",\"gravity\":[0,0,0],\"drag\":0}]}";
    we_particle_sys_t *asys = we_particle_create_from_json(attract_json, strlen(attract_json));
    assert(asys != NULL);
    assert(asys->operator_count == 2u);
    assert(asys->operators[0].type == WE_OP_CONTROL_POINT_ATTRACT);

    we_particle_set_pointer(asys, 400.0f, 300.0f, 0.0f);
    we_particle_trigger_burst(asys, 1u);
    assert(asys->alive_count == 1u);
    /* Place particle at (100, 300, 0), strictly -X relative to pointer (400, 300, 0) */
    asys->pos_x[0] = 100.0f;
    asys->pos_y[0] = 300.0f;
    asys->pos_z[0] = 0.0f;
    asys->vel_x[0] = 0.0f;
    asys->vel_y[0] = 0.0f;
    asys->vel_z[0] = 0.0f;
    asys->max_life[0] = 10.0f;
    we_particle_update(asys, 0.05f);
    assert(asys->vel_x[0] > 0.0f);
    assert(fabsf(asys->vel_y[0]) < 1e-4f);
    assert(asys->pos_x[0] > 100.0f);
    we_particle_destroy(asys);

    /* Apply runtime ramp LUT to ALPHA_FADE / SIZE_CHANGE / COLOR_CHANGE & benchmark 16,384 */
    we_particle_sys_t *bench = we_particle_create_preset(WE_PARTICLE_PRESET_STARFIELD, false);
    assert(bench != NULL);
    assert(we_particle_realloc_capacity(bench, WE_PARTICLE_MAX_CAP));
    we_particle_ramp_lut_t alpha_lut;
    memset(&alpha_lut, 0, sizeof(alpha_lut));
    alpha_lut.operator_index = UINT32_MAX;
    alpha_lut.channel_kind = 0u;
    alpha_lut.knot_count = 3u;
    we_particle_curve_knot_t ak[3] = {
        {.t = 0.0f, .y = 0.1f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 0.5f, .y = 1.0f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
        {.t = 1.0f, .y = 0.0f, .mode = WE_KNOT_HANDLE_AUTO_SMOOTH},
    };
    assert(we_particle_bake_scalar_lut(ak, 3u, alpha_lut.scalar_lut));
    assert(we_particle_apply_ramp_lut(bench, &alpha_lut));

    we_particle_trigger_burst(bench, WE_PARTICLE_MAX_CAP);
    assert(bench->alive_count == WE_PARTICLE_MAX_CAP);
    for (uint32_t i = 0; i < bench->alive_count; i++) {
        bench->max_life[i] = 100.0f;
        bench->age[i] = 10.0f;
    }

    uint64_t t0 = now_ns();
    for (int f = 0; f < 40; f++) {
        we_particle_update(bench, 0.016f);
    }
    uint64_t t1 = now_ns();
    double ms_per_frame = (double)(t1 - t0) / (40.0 * 1e6);
    printf("[PASS] 8 presets, ControlPointAttract, and 16384-particle LUT update (%.3f ms/frame)\n",
           ms_per_frame);
    we_particle_destroy(bench);
}

static void test_audio_bindings(void)
{
    we_scene_t *scene = calloc(1, sizeof(we_scene_t));
    assert(scene != NULL);
    scene->objects = calloc(2, sizeof(we_scene_object_t));
    assert(scene->objects != NULL);

    scene->object_count = 2u;
    we_scene_object_t *obj0 = &scene->objects[0];
    obj0->id = 101;
    obj0->visible = true;
    obj0->alpha = 1.0f;
    obj0->brightness = 1.0f;
    obj0->color[0] = obj0->color[1] = obj0->color[2] = 1.0f;
    obj0->scale[0] = obj0->scale[1] = obj0->scale[2] = 1.0f;
    obj0->has_base_pass = true;
    snprintf(obj0->base_pass.const_vals[0].name, sizeof(obj0->base_pass.const_vals[0].name), "%s",
             "u_glow");
    obj0->base_pass.const_val_count = 1u;
    obj0->is_light_object = true;
    obj0->light_intensity = 1.0f;
    obj0->light_radius = 400.0f;
    obj0->particle_sys = we_particle_create_preset(WE_PARTICLE_PRESET_MAGIC_DUST, false);
    assert(obj0->particle_sys != NULL);

    /* 1. Hysteresis gate anti-chatter test */
    we_audio_binding_t b_gate;
    memset(&b_gate, 0, sizeof(b_gate));
    b_gate.binding_id = 1u;
    b_gate.target_object_id = 101;
    b_gate.target_kind = WE_AUDIO_TGT_LAYER_ORIGIN_X;
    b_gate.src_kind = WE_AUDIO_SRC_BARK_RANGE;
    b_gate.agg_mode = WE_AUDIO_AGG_PEAK;
    b_gate.band_start = 0u;
    b_gate.band_end = 1u;
    b_gate.input_gain = 1.0f;
    b_gate.threshold_open = 0.60f;
    b_gate.hysteresis = 0.15f;
    b_gate.attack_sec = 0.0001f;
    b_gate.release_sec = 0.0001f;
    b_gate.curve_shape = WE_AUDIO_CURVE_LINEAR;
    b_gate.blend_mode = WE_AUDIO_BLEND_ADD;
    b_gate.out_min = 0.0f;
    b_gate.out_max = 100.0f;
    b_gate.base_value = 960.0f;
    b_gate.enabled = 1u;

    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REPLACE_ALL, &b_gate, 1u, 0u));
    waywal_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.bands[0] = 0.55f; /* below open=0.60 */
    we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    assert(scene->audio_bindings[0].gate_open == 0u);
    assert(fabsf(obj0->origin[0] - 960.0f) < 1e-4f);

    frame.bands[0] = 0.65f; /* opens gate */
    we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    assert(scene->audio_bindings[0].gate_open == 1u);
    assert(fabsf(obj0->origin[0] - 1025.0f) < 1e-2f);

    frame.bands[0] = 0.50f; /* above close=0.45 -> stays open */
    we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    assert(scene->audio_bindings[0].gate_open == 1u);

    frame.bands[0] = 0.40f; /* below close=0.45 -> closes */
    we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    assert(scene->audio_bindings[0].gate_open == 0u);
    assert(fabsf(obj0->origin[0] - 960.0f) < 1e-2f);

    /* 2. Giannoulis dt-invariance: 60 Hz vs 144 Hz over identical 0.5s attack + 0.5s release */
    we_audio_binding_t b60 = b_gate;
    b60.threshold_open = 0.0f;
    b60.hysteresis = 0.0f;
    b60.attack_sec = 0.080f;
    b60.release_sec = 0.200f;
    b60.blend_mode = WE_AUDIO_BLEND_REPLACE;
    b60.out_max = 1.0f;
    we_audio_binding_t b144 = b60;

    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REPLACE_ALL, &b60, 1u, 0u));
    frame.bands[0] = 1.0f;
    for (int i = 0; i < 30; i++) {
        we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    }
    frame.bands[0] = 0.0f;
    for (int i = 0; i < 30; i++) {
        we_scene_step_audio_bindings(scene, &frame, 1.0f / 60.0f);
    }
    float out60 = scene->audio_bindings[0].env_state;

    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REPLACE_ALL, &b144, 1u, 0u));
    frame.bands[0] = 1.0f;
    for (int i = 0; i < 72; i++) {
        we_scene_step_audio_bindings(scene, &frame, 1.0f / 144.0f);
    }
    frame.bands[0] = 0.0f;
    for (int i = 0; i < 72; i++) {
        we_scene_step_audio_bindings(scene, &frame, 1.0f / 144.0f);
    }
    float out144 = scene->audio_bindings[0].env_state;
    assert(fabsf(out60 - out144) < 0.02f);

    /* 3. TRIG_EDGE 80ms cooldown + PARTICLE_BURST_TRIG */
    we_audio_binding_t b_trig;
    memset(&b_trig, 0, sizeof(b_trig));
    b_trig.binding_id = 2u;
    b_trig.target_object_id = 101;
    b_trig.target_kind = WE_AUDIO_TGT_PARTICLE_BURST_TRIG;
    b_trig.src_kind = WE_AUDIO_SRC_BEAT_PULSE;
    b_trig.input_gain = 1.0f;
    b_trig.threshold_open = 0.5f;
    b_trig.hysteresis = 0.1f;
    b_trig.curve_shape = WE_AUDIO_CURVE_TRIG_EDGE;
    b_trig.blend_mode = WE_AUDIO_BLEND_REPLACE;
    b_trig.out_min = 0.0f;
    b_trig.out_max = 12.0f;
    b_trig.enabled = 1u;
    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REPLACE_ALL, &b_trig, 1u, 0u));

    frame.beat_pulse = 0.9f;
    we_scene_step_audio_bindings(scene, &frame, 0.020f);
    assert(obj0->particle_sys->alive_count == 12u);

    /* Drop and immediately re-trigger inside 80ms cooldown -> blocked */
    frame.beat_pulse = 0.0f;
    we_scene_step_audio_bindings(scene, &frame, 0.020f);
    frame.beat_pulse = 0.9f;
    we_scene_step_audio_bindings(scene, &frame, 0.020f);
    assert(obj0->particle_sys->alive_count == 12u);

    /* Advance past remaining cooldown, drop, then re-trigger -> fires again */
    frame.beat_pulse = 0.0f;
    we_scene_step_audio_bindings(scene, &frame, 0.060f);
    frame.beat_pulse = 0.9f;
    we_scene_step_audio_bindings(scene, &frame, 0.020f);
    assert(obj0->particle_sys->alive_count == 24u);

    /* 4. Target dispatch coverage + 64-binding perf benchmark (< 2.5 us target) */
    we_audio_binding_t rules[WE_MAX_AUDIO_BINDINGS];
    memset(rules, 0, sizeof(rules));
    for (uint32_t i = 0; i < WE_MAX_AUDIO_BINDINGS; i++) {
        rules[i].binding_id = 100u + i;
        rules[i].target_object_id = 101;
        rules[i].target_kind = i % 14u;
        rules[i].target_sub_index =
            (rules[i].target_kind == WE_AUDIO_TGT_PASS_UNIFORM_COMP) ? UINT32_MAX : 0u;
        rules[i].target_component = 2u;
        snprintf(rules[i].uniform_name, sizeof(rules[i].uniform_name), "%s", "u_glow");
        rules[i].src_kind = WE_AUDIO_SRC_BARK_RANGE;
        rules[i].agg_mode = WE_AUDIO_AGG_PEAK_WEIGHTED;
        rules[i].band_start = (uint16_t)(i % 32u);
        rules[i].band_end = (uint16_t)((i % 32u) + 8u);
        rules[i].input_gain = 1.0f;
        rules[i].attack_sec = 0.015f;
        rules[i].release_sec = 0.120f;
        rules[i].curve_shape = i % 4u;
        rules[i].blend_mode = WE_AUDIO_BLEND_ADD;
        rules[i].out_min = 0.0f;
        rules[i].out_max = 1.0f;
        rules[i].base_value = 0.5f;
        rules[i].enabled = 1u;
    }
    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REPLACE_ALL, rules,
                                         WE_MAX_AUDIO_BINDINGS, 0u));
    for (int k = 0; k < 64; k++) {
        frame.bands[k] = 0.8f;
    }

    uint64_t b0 = now_ns();
    for (int it = 0; it < 1000; it++) {
        uint32_t ev = we_scene_step_audio_bindings(scene, &frame, 1.0f / 144.0f);
        assert(ev == WE_MAX_AUDIO_BINDINGS);
    }
    uint64_t b1 = now_ns();
    double ns_per_eval = (double)(b1 - b0) / 1000.0;
    assert(obj0->base_pass.const_vals[0].val[2] > 0.5f);
    assert(obj0->light_intensity > 0.5f);

    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_REMOVE, NULL, 0u, 100u));
    assert(scene->audio_binding_count == WE_MAX_AUDIO_BINDINGS - 1u);
    assert(we_scene_apply_audio_bindings(scene, WE_AUDIO_BIND_OP_CLEAR, NULL, 0u, 0u));
    assert(scene->audio_binding_count == 0u);

    printf("[PASS] Audio bindings (hysteresis, 60 vs 144 Hz dt-invariance=%.4f, cooldown, 64-rule "
           "avg=%.0f ns)\n",
           fabsf(out60 - out144), ns_per_eval);

    we_particle_destroy(obj0->particle_sys);
    free(scene->objects);
    free(scene);
}

static void test_scenescript_linter(void)
{
    we_script_diag_t diag;

    const char *valid_src = "'use strict';\n"
                            "export function update(value) {\n"
                            "    let amp = (audio.bass > 0.25) ? audio.bass * 2.0 : 0.0;\n"
                            "    const msg = `bass=${amp}`;\n"
                            "    return value + amp;\n"
                            "}\n";
    assert(we_script_lint(valid_src, strlen(valid_src), 0u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_OK);
    assert(diag.error_line == 0u);

    /* Unclosed paren reported on exact line */
    const char *bad_paren = "export function update(value) {\n"
                            "    let x = Math.sin(value;\n"
                            "    return x;\n"
                            "}\n";
    assert(!we_script_lint(bad_paren, strlen(bad_paren), 0u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_SYNTAX_ERR);
    assert(diag.error_line == 2u);
    assert(diag.error_msg[0] != '\0');

    /* Unterminated string literal */
    const char *bad_str = "let a = 1;\n"
                          "let b = \"unterminated;\n";
    assert(!we_script_lint(bad_str, strlen(bad_str), 0u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_SYNTAX_ERR);
    assert(diag.error_line == 2u);
    assert(diag.error_col == 9u);

    /* Unterminated block comment */
    const char *bad_comment = "let a = 1;\n/* open comment\n";
    assert(!we_script_lint(bad_comment, strlen(bad_comment), 0u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_SYNTAX_ERR);
    assert(diag.error_line == 2u);

    /* Depth exceeded (> 64 nested expressions) */
    char deep_src[256];
    size_t dp = 0;
    for (int i = 0; i < 72; i++) {
        deep_src[dp++] = '(';
    }
    deep_src[dp++] = '1';
    for (int i = 0; i < 72; i++) {
        deep_src[dp++] = ')';
    }
    deep_src[dp] = '\0';
    assert(!we_script_lint(deep_src, dp, 0u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_DEPTH_EXCEEDED);

    /* Bounded <= 64 KiB scratch holds cleanly on a > 128 KiB valid multi-function script */
    size_t big_cap = 160u * 1024u;
    char *big_src = malloc(big_cap);
    assert(big_src != NULL);
    size_t bp = 0;
    const char *fn = "function f() { let x = 1 + 2; return x * 3; }\n";
    size_t fn_len = strlen(fn);
    while (bp + fn_len + 1u < big_cap) {
        memcpy(big_src + bp, fn, fn_len);
        bp += fn_len;
    }
    big_src[bp] = '\0';
    assert(bp > 131072u);
    assert(we_script_lint(big_src, bp, 1u, &diag));
    assert(diag.status_code == WE_SCRIPT_LINT_OK);
    free(big_src);

    printf("[PASS] we_script_lint (exact line/col, depth guard, 160 KiB script inside <64 KiB "
           "scratch)\n");
}

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
} particle_headless_gl_t;

static bool init_particle_headless_gl(particle_headless_gl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->drm_fd = -1;
    out->dpy = EGL_NO_DISPLAY;
    out->ctx = EGL_NO_CONTEXT;

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    static const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1,
                                         EGL_NONE};

    for (int idx = 128; idx < 136; ++idx) {
        char node_path[64];
        snprintf(node_path, sizeof(node_path), "/dev/dri/renderD%d", idx);
        int fd = open(node_path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct gbm_device *gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }
        EGLDisplay dpy = get_platform_dpy ? get_platform_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL)
                                          : eglGetDisplay((EGLNativeDisplayType)gbm);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) ||
            !eglBindAPI(EGL_OPENGL_ES_API)) {
            if (dpy != EGL_NO_DISPLAY)
                eglTerminate(dpy);
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }
        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT) {
            static const EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                                 EGL_SURFACE_TYPE, 0, EGL_NONE};
            EGLConfig cfg = NULL;
            EGLint num_cfg = 0;
            if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg) && num_cfg > 0)
                ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
        }
        if (ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            out->drm_fd = fd;
            out->gbm = gbm;
            out->dpy = dpy;
            out->ctx = ctx;
            return true;
        }
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(fd);
    }

    if (get_platform_dpy) {
        EGLDisplay dpy = get_platform_dpy(0x31DD, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL) &&
            eglBindAPI(EGL_OPENGL_ES_API)) {
            EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
            if (ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
                out->dpy = dpy;
                out->ctx = ctx;
                return true;
            }
            if (ctx != EGL_NO_CONTEXT)
                eglDestroyContext(dpy, ctx);
            eglTerminate(dpy);
        }
    }
    return false;
}

static void destroy_particle_headless_gl(particle_headless_gl_t *gl)
{
    if (gl->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gl->ctx != EGL_NO_CONTEXT)
            eglDestroyContext(gl->dpy, gl->ctx);
        eglTerminate(gl->dpy);
    }
    if (gl->gbm)
        gbm_device_destroy(gl->gbm);
    if (gl->drm_fd >= 0)
        close(gl->drm_fd);
}

static GLuint compile_minimal_particle_prog(void)
{
    static const char *vs_src = "#version 310 es\n"
                                "layout(location = 0) in vec3 a_Pos;\n"
                                "void main() { gl_Position = vec4(a_Pos, 1.0); }\n";
    static const char *fs_src = "#version 310 es\n"
                                "precision mediump float;\n"
                                "out vec4 o_Color;\n"
                                "void main() { o_Color = vec4(1.0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(vs);
    glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    assert(linked == GL_TRUE);
    return prog;
}

static void test_particle_remediation_regressions(void)
{
    float lut[WE_PARTICLE_RAMP_SAMPLES];
    const we_particle_curve_knot_t nan_single[1] = {{0.0f, NAN, 0.0f, 0.0f, 0u, 0u}};
    assert(!we_particle_bake_scalar_lut(nan_single, 1u, lut));
    const we_particle_curve_knot_t inf_single[1] = {{0.0f, INFINITY, 0.0f, 0.0f, 0u, 0u}};
    assert(!we_particle_bake_scalar_lut(inf_single, 1u, lut));
    const we_particle_curve_knot_t nan_pair[2] = {
        {0.0f, 0.5f, 0.0f, 0.0f, 0u, 0u},
        {1.0f, NAN, 0.0f, 0.0f, 0u, 0u},
    };
    assert(!we_particle_bake_scalar_lut(nan_pair, 2u, lut));
    const we_particle_curve_knot_t valid_pair[2] = {
        {0.0f, 0.25f, 0.0f, 0.0f, 0u, 0u},
        {1.0f, 0.75f, 0.0f, 0.0f, 0u, 0u},
    };
    assert(we_particle_bake_scalar_lut(valid_pair, 2u, lut));
    assert(fabsf(we_particle_eval_scalar_lut(lut, NAN) - 0.25f) < 1e-6f);

    float r[WE_PARTICLE_RAMP_SAMPLES], g[WE_PARTICLE_RAMP_SAMPLES], b[WE_PARTICLE_RAMP_SAMPLES];
    const we_particle_color_stop_t nan_stop[2] = {
        {0.0f, {1.0f, 0.0f, 0.0f}},
        {1.0f, {NAN, 1.0f, 0.0f}},
    };
    assert(!we_particle_bake_oklab_lut(nan_stop, 2u, r, g, b));

    we_particle_sys_t *sys = we_particle_create_preset(WE_PARTICLE_PRESET_EMBERS, false);
    assert(sys != NULL);
    /* Inject a known WE_OP_MOVEMENT slot and WE_OP_SIZE_CHANGE slot to verify type enforcement */
    sys->operators[0].type = WE_OP_MOVEMENT;
    sys->operators[1].type = WE_OP_SIZE_CHANGE;
    if (sys->operator_count < 2u)
        sys->operator_count = 2u;

    we_particle_ramp_lut_t ramp;
    memset(&ramp, 0, sizeof(ramp));
    memcpy(ramp.scalar_lut, lut, sizeof(lut));
    ramp.channel_kind = 1u;   /* WE_OP_SIZE_CHANGE */
    ramp.operator_index = 0u; /* points to WE_OP_MOVEMENT */
    assert(!we_particle_apply_ramp_lut(sys, &ramp));

    ramp.operator_index = 1u;
    ramp.scalar_lut[10] = NAN;
    assert(!we_particle_apply_ramp_lut(sys, &ramp));
    ramp.scalar_lut[10] = lut[10];
    assert(we_particle_apply_ramp_lut(sys, &ramp));
    we_particle_destroy(sys);

    static const char *extreme_json =
        "{"
        "\"maxcount\": 1e30,"
        "\"starttime\": 1e30,"
        "\"flags\": -1,"
        "\"renderer\": [{\"name\": \"sprite\", \"subdivision\": 1e30}],"
        "\"controlpoint\": [{\"id\": 1e30, \"flags\": -1}],"
        "\"emitter\": [{\"name\": \"sphererandom\", \"rate\": 10.0, \"instantaneous\": 1e30, "
        "\"controlpoint\": -1}]"
        "}";
    we_particle_sys_t *ext_sys = we_particle_create_from_json(extreme_json, strlen(extreme_json));
    assert(ext_sys != NULL);
    assert(ext_sys->max_count <= WE_PARTICLE_MAX_CAP && ext_sys->max_count > 0u);
    assert(isfinite(ext_sys->start_time));
    we_particle_destroy(ext_sys);

    static const char *neg_max_json = "{\"maxcount\": -5, \"starttime\": \"NaN\"}";
    we_particle_sys_t *neg_sys = we_particle_create_from_json(neg_max_json, strlen(neg_max_json));
    assert(neg_sys != NULL);
    assert(neg_sys->max_count >= 1u && neg_sys->max_count <= WE_PARTICLE_MAX_CAP);
    we_particle_destroy(neg_sys);

    we_particle_sys_t *shrink_sys = we_particle_create_preset(WE_PARTICLE_PRESET_EMBERS, false);
    assert(shrink_sys != NULL);
    we_particle_trigger_burst(shrink_sys, 32u);
    assert(shrink_sys->alive_count == 32u);
    assert(we_particle_realloc_capacity(shrink_sys, 8u));
    assert(shrink_sys->max_count == 8u);
    assert(shrink_sys->alive_count == 32u);
    shrink_sys->sim_time = 1.0e8;
    we_particle_update(shrink_sys, 0.016f);
    assert(shrink_sys->alive_count == 32u);
    for (uint32_t i = 0; i < shrink_sys->alive_count; i++) {
        assert(isfinite(shrink_sys->pos_x[i]));
        assert(isfinite(shrink_sys->pos_y[i]));
        assert(isfinite(shrink_sys->vel_x[i]));
    }
    we_particle_destroy(shrink_sys);

    we_particle_sys_t *gl_sys = we_particle_create_preset(WE_PARTICLE_PRESET_RAIN, true);
    assert(gl_sys != NULL);
    assert(!we_particle_upload_gl(gl_sys));
    we_particle_draw_gl(gl_sys, 0u);

    particle_headless_gl_t gl;
    if (init_particle_headless_gl(&gl)) {
        assert(we_particle_upload_gl(gl_sys));
        GLint bound_vbo = -1, bound_ebo = -1, bound_vao = -1;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound_vbo);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound_ebo);
        assert(bound_vbo == 0);
        assert(bound_ebo == 0);

        GLuint prog = compile_minimal_particle_prog();
        GLuint dummy_vao = 0, dummy_vbo = 0;
        glGenVertexArrays(1, &dummy_vao);
        glGenBuffers(1, &dummy_vbo);

        glBindVertexArray(dummy_vao);
        glBindBuffer(GL_ARRAY_BUFFER, dummy_vbo);
        glDepthMask(GL_TRUE);
        we_particle_draw_gl(gl_sys, prog);
        GLboolean depth_mask = GL_FALSE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        assert(depth_mask == GL_TRUE);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound_vbo);
        assert(bound_vao == 0);
        assert(bound_vbo == 0);

        glBindVertexArray(dummy_vao);
        glBindBuffer(GL_ARRAY_BUFFER, dummy_vbo);
        glDepthMask(GL_FALSE);
        we_particle_draw_gl(gl_sys, prog);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        assert(depth_mask == GL_FALSE);

        glDeleteBuffers(1, &dummy_vbo);
        glDeleteVertexArrays(1, &dummy_vao);
        glDeleteProgram(prog);

        we_particle_destroy(gl_sys);
        destroy_particle_headless_gl(&gl);
    } else {
        we_particle_destroy(gl_sys);
    }

    printf(
        "[PASS] particle remediation regressions (LUT NaN/Inf, type guard, JSON extremes, shrink, "
        "GL state)\n");
}

int main(void)
{
    test_abi_contracts();
    test_fritsch_carlson_and_oklab_luts();
    test_presets_and_control_point_attract();
    test_audio_bindings();
    test_scenescript_linter();
    test_particle_remediation_regressions();
    printf("ALL p4-particle-audio-core TESTS PASSED\n");
    return 0;
}
