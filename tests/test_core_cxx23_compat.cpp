// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_core_cxx23_compat.cpp
 * @brief C++23 Compatibility & Core Header Verification Suite
 *
 * Compiles cleanly under -std=c++23 -Wall -Wextra -Werror, including every single
 * waywal header without symbol collision or language friction.
 * Empirically tests Cubic Bezier solver, OkLab/HCT palette extraction, JSON DOM parser,
 * and GLSL shader metadata parser, while asserting critical struct layout invariants.
 */

#include <iostream>
#include <vector>
#include <string_view>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>

// Include every public waywal/*.h header to verify C++23 compatibility
#include "waywal/anim_engine.h"
#include "waywal/arena.h"
#include "waywal/audio_fft.h"
#include "waywal/bezier.h"
#include "waywal/demuxer.h"
#include "waywal/dmabuf.h"
#include "waywal/gif_transcode.h"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/mpris.h"
#include "waywal/os_compat.h"
#include "waywal/output_state.h"
#include "waywal/palette.h"
#include "waywal/palette_hct.h"
#include "waywal/path.h"
#include "waywal/power_governor.h"
#include "waywal/presentation.h"
#include "waywal/render_engine.h"
#include "waywal/security.h"
#include "waywal/shader_engine.h"
#include "waywal/shader_parser.h"
#include "waywal/slideshow.h"
#include "waywal/solar_schedule.h"
#include "waywal/state_store.h"
#include "waywal/thumb_cache.h"
#include "waywal/toml_parser.h"
#include "waywal/types.h"
#include "waywal/uring_loop.h"
#include "waywal/vaapi_dec.h"
#include "waywal/video_decoder.h"
#include "waywal/video_engine.h"
#include "waywal/we_json.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"
#include "waywal/we_puppet.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"
#include "waywal/we_shader.h"
#include "waywal/we_tex.h"

// ----------------------------------------------------------------------------
// Compile-time Struct Packing & Offset Assertions
// ----------------------------------------------------------------------------
static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t must be exactly 16 bytes packed");
static_assert(sizeof(waywal_img_metadata_t) == 4815,
              "waywal_img_metadata_t must be exactly 4815 bytes packed");
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Legacy 72-byte prefix of waywal_img_metadata_t must remain frozen at offset 72");

static void test_bezier_solver()
{
    std::cout << "[Core C++23] Testing Cubic Bezier Solver (bezier_solve_t & bezier_eval)..."
              << std::endl;

    bezier_curve_t curve = bezier_create(0.25f, 0.1f, 0.25f, 1.0f); // Default CSS ease
    assert(curve.p1x == 0.25f && curve.p1y == 0.1f);
    assert(curve.p2x == 0.25f && curve.p2y == 1.0f);

    // Boundary conditions
    float t0 = bezier_solve_t(&curve, 0.0f);
    assert(std::fabs(t0 - 0.0f) < 1e-5f);
    float t1 = bezier_solve_t(&curve, 1.0f);
    assert(std::fabs(t1 - 1.0f) < 1e-5f);

    float y0 = bezier_eval(&curve, 0.0f);
    assert(std::fabs(y0 - 0.0f) < 1e-5f);
    float y1 = bezier_eval(&curve, 1.0f);
    assert(std::fabs(y1 - 1.0f) < 1e-5f);

    // Midpoint check: monotonic progress
    float t_mid = bezier_solve_t(&curve, 0.5f);
    assert(t_mid > 0.0f && t_mid < 1.0f);
    float y_mid = bezier_eval(&curve, 0.5f);
    assert(y_mid > 0.0f && y_mid < 1.0f);

    // Out-of-bounds safety checks
    assert(bezier_solve_t(&curve, -0.1f) == 0.0f);
    assert(bezier_solve_t(&curve, 1.5f) == 1.0f);
    assert(bezier_eval(&curve, -0.1f) == 0.0f);
    assert(bezier_eval(&curve, 1.5f) == 1.0f);

    // Linear curve identity
    bezier_curve_t lin = BEZIER_LINEAR;
    assert(std::fabs(bezier_eval(&lin, 0.333f) - 0.333f) < 1e-5f);

    std::cout << "  -> Cubic Bezier solver passed." << std::endl;
}

static void test_palette_hct_extraction()
{
    std::cout << "[Core C++23] Testing Material You OkLab/HCT Palette Extraction..." << std::endl;

    constexpr uint32_t width = 128;
    constexpr uint32_t height = 128;
    constexpr uint32_t stride = width * 4;
    std::vector<uint8_t> pixels(height * stride);

    // Generate vibrant gradient pattern with rich chromatic seeds
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = (y * width + x) * 4;
            pixels[idx + 0] = static_cast<uint8_t>((x * 255) / width);             // R
            pixels[idx + 1] = static_cast<uint8_t>((y * 255) / height);            // G
            pixels[idx + 2] = static_cast<uint8_t>(((x + y) * 255) / (2 * width)); // B
            pixels[idx + 3] = 255;                                                 // A
        }
    }

    waywal_palette_t palette{};
    bool ok = waywal_palette_hct_extract(pixels.data(), width, height, stride, WAYWAL_THEME_DARK,
                                         &palette);
    assert(ok && "waywal_palette_hct_extract must succeed on synthetic buffer");

    // Validate that tokens were populated
    assert(palette.primary.a == 255);
    assert(palette.background.a == 255);
    assert(palette.surface.a == 255);
    assert(palette.seed_count > 0);

    // Validate dual mode opposite derivation
    waywal_palette_t light_palette{};
    waywal_palette_derive_opposite(&palette, &light_palette);
    assert(light_palette.is_light_mode == true);
    assert(light_palette.primary.a == 255);

    std::cout << "  -> Material You OkLab/HCT palette extraction passed." << std::endl;
}

static void test_we_json_parser()
{
    std::cout << "[Core C++23] Testing Wallpaper Engine Lightweight DOM JSON Parser..."
              << std::endl;

    const char sample_json[] = "{\n"
                               "  \"title\": \"Cyberpunk Neon Alley\",\n"
                               "  \"type\": \"scene\",\n"
                               "  \"fps\": 60,\n"
                               "  \"preview\": \"preview.gif\",\n"
                               "  \"general\": {\n"
                               "    \"properties\": {\n"
                               "      \"schemecolor\": { \"value\": \"0.1 0.8 0.3\" },\n"
                               "      \"rain_speed\": { \"value\": 1.75 },\n"
                               "      \"bloom_enabled\": { \"value\": true }\n"
                               "    }\n"
                               "  },\n"
                               "  \"tags\": [\"Anime\", \"Cyberpunk\", \"Rain\"]\n"
                               "}";

    we_jval_t *root = we_json_parse(sample_json, sizeof(sample_json) - 1);
    assert(root != nullptr && "we_json_parse must parse valid JSON string");
    assert(root->type == JVAL_OBJ);

    const we_jval_t *title_val = we_jobj_get(root, "title");
    assert(title_val != nullptr && title_val->type == JVAL_STR);
    assert(std::string_view(title_val->str) == "Cyberpunk Neon Alley");

    const we_jval_t *fps_val = we_jobj_get(root, "fps");
    assert(fps_val != nullptr && fps_val->type == JVAL_NUM);
    assert(fps_val->num == 60.0);

    const we_jval_t *gen_val = we_jobj_get(root, "general");
    assert(gen_val != nullptr && gen_val->type == JVAL_OBJ);

    const we_jval_t *props_val = we_jobj_get(gen_val, "properties");
    assert(props_val != nullptr && props_val->type == JVAL_OBJ);

    const we_jval_t *rain_val = we_jobj_get(props_val, "rain_speed");
    assert(rain_val != nullptr && rain_val->type == JVAL_OBJ);
    const we_jval_t *rain_num = we_jobj_get(rain_val, "value");
    assert(rain_num != nullptr && rain_num->type == JVAL_NUM);
    assert(std::fabs(rain_num->num - 1.75) < 1e-4);

    const we_jval_t *tags_val = we_jobj_get(root, "tags");
    assert(tags_val != nullptr && tags_val->type == JVAL_ARR);
    assert(tags_val->arr.count == 3);
    assert(std::string_view(tags_val->arr.items[1]->str) == "Cyberpunk");

    we_jval_free(root);
    std::cout << "  -> DOM JSON parser passed." << std::endl;
}

static void test_shader_parser()
{
    std::cout << "[Core C++23] Testing GLSL Shader Metadata & Uniform Directive Parser..."
              << std::endl;

    const char sample_glsl[] =
        "// [COMMON]\n"
        "// @title: Audio Reactive Hyperboloid\n"
        "// @author: Inigo Quilez\n"
        "// @description: Raymarched fractal reacting to PipeWire 64-band Bark FFT\n"
        "// @license: MIT\n"
        "//\n"
        "// [BUFFER A]\n"
        "// #pragma waywal iChannel0 = audio\n"
        "// #pragma waywal iChannel1 = bufferA\n"
        "// @uniform float u_speed = 1.2; [0.1, 5.0, 0.05]\n"
        "// @uniform vec3 u_neon_color = vec3(0.1, 0.95, 0.85); // Neon tint\n"
        "// @uniform bool u_enable_bloom = true;\n"
        "\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    fragColor = vec4(1.0);\n"
        "}\n";

    waywal_shader_meta_t meta{};
    waywal_shader_meta_init(&meta);

    bool ok = waywal_shader_parse_meta(sample_glsl, sizeof(sample_glsl) - 1, &meta);
    assert(ok && "waywal_shader_parse_meta must parse shader directives");

    assert(std::string_view(meta.title) == "Audio Reactive Hyperboloid");
    assert(std::string_view(meta.author) == "Inigo Quilez");
    assert(meta.has_multi_pass == true);

    // Verify parsed uniform definitions
    assert(meta.uniform_count >= 1);
    const waywal_shader_uniform_def_t *speed_u = waywal_shader_find_uniform(&meta, "u_speed");
    assert(speed_u != nullptr);
    assert(speed_u->type == WAYWAL_UNIFORM_TYPE_FLOAT);
    assert(std::fabs(speed_u->default_val[0] - 1.2f) < 1e-4f);
    assert(speed_u->has_range == true);
    assert(std::fabs(speed_u->min_val[0] - 0.1f) < 1e-4f);
    assert(std::fabs(speed_u->max_val[0] - 5.0f) < 1e-4f);

    std::cout << "  -> GLSL shader parser passed." << std::endl;
}

int main()
{
    std::cout << "==========================================================" << std::endl;
    std::cout << "  C++23 Compatibility & Core Library Test Suite           " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_bezier_solver();
    test_palette_hct_extraction();
    test_we_json_parser();
    test_shader_parser();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> CORE C++23 COMPATIBILITY PASSED CLEANLY (100%) <<<  " << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
