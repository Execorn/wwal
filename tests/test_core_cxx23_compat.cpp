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
#include "waywal/we_vdf.h"
#include "waywal/we_appworkshop.h"
#include "waywal/wwpkg.h"
#include "waywal/we_compat_scanner.h"

// ----------------------------------------------------------------------------
// Compile-time Struct Packing & Offset Assertions
// ----------------------------------------------------------------------------
static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t must be exactly 16 bytes packed");
static_assert(sizeof(waywal_img_metadata_t) == 4815,
              "waywal_img_metadata_t must be exactly 4815 bytes packed");
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Legacy 72-byte prefix of waywal_img_metadata_t must remain frozen at offset 72");

// Workshop, Open Hub & Compatibility IPC Wire Struct Sizes and Alignment Invariants
static_assert(sizeof(waywal_ipc_req_workshop_sync_t) == 1028, "Must be 1028 bytes");
static_assert(sizeof(waywal_ipc_resp_workshop_sync_t) == 24, "Must be 24 bytes");
static_assert(offsetof(waywal_ipc_resp_workshop_sync_t, total_size_bytes) == 16,
              "total_size_bytes must be at offset 16");

static_assert(sizeof(waywal_ipc_req_workshop_download_t) == 16, "Must be 16 bytes");
static_assert(sizeof(waywal_ipc_resp_workshop_download_t) == 280, "Must be 280 bytes");

static_assert(sizeof(waywal_ipc_req_wwpkg_install_t) == 24, "Must be 24 bytes header");
static_assert(sizeof(waywal_ipc_resp_wwpkg_install_t) == 24, "Must be 24 bytes header");

static_assert(sizeof(waywal_ipc_req_scan_compat_t) == 1028, "Must be 1028 bytes");
static_assert(sizeof(waywal_ipc_resp_scan_compat_t) == 1052, "Must be 1052 bytes");

static_assert(sizeof(wwpkg_signature_t) == 216, "Must be 216 bytes with zero hidden padding");

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

static void test_workshop_and_wwpkg_cxx23_interop()
{
    std::cout << "[Core C++23] Testing Workshop & WWPKG C23 Modules (VDF, AppWorkshop, WWPKG, "
                 "Compat Scanner)..."
              << std::endl;

    // 1. VDF Parser interop
    const char sample_vdf[] = "\"libraryfolders\"\n"
                              "{\n"
                              "    \"0\"\n"
                              "    {\n"
                              "        \"path\" \"/home/user/.local/share/Steam\"\n"
                              "        \"label\" \"\"\n"
                              "        \"contentid\" \"12345\"\n"
                              "        \"total_size\" \"500000000000\"\n"
                              "        \"apps\"\n"
                              "        {\n"
                              "            \"431960\" \"104857600\"\n"
                              "        }\n"
                              "    }\n"
                              "}\n";

    waywal_vdf_doc_t *vdf = waywal_vdf_parse(sample_vdf, sizeof(sample_vdf) - 1);
    assert(vdf != nullptr && "waywal_vdf_parse must succeed on valid VDF");
    assert(vdf->root != nullptr);
    const waywal_vdf_node_t *node0 = waywal_vdf_find_child(vdf->root, "0");
    assert(node0 != nullptr);
    const char *path = waywal_vdf_get_string(node0, "path", "");
    assert(std::string_view(path) == "/home/user/.local/share/Steam");
    waywal_vdf_free(vdf);

    // 2. WWPKG Crypto interop (Ed25519 keypair and SHA-256)
    uint8_t pub[32]{};
    uint8_t priv[64]{};
    wwpkg_generate_keypair(pub, priv);
    bool has_nonzero_pub = false;
    for (uint8_t b : pub) {
        if (b != 0)
            has_nonzero_pub = true;
    }
    assert(has_nonzero_pub && "Generated pubkey must be non-zero");

    const uint8_t test_msg[] = "WayWal Open Hub RFC 6962 Merkle Test";
    uint8_t test_hash[32]{};
    wwpkg_sha256(test_msg, sizeof(test_msg) - 1, test_hash);
    uint8_t sig[64]{};
    int sign_rc = wwpkg_ed25519_sign(sig, test_hash, 32, priv);
    assert(sign_rc == 0 && "wwpkg_ed25519_sign must succeed");
    int verify_rc = wwpkg_ed25519_verify(sig, test_hash, 32, pub);
    assert(verify_rc == 0 && "wwpkg_ed25519_verify must verify signature");

    // 3. Compat Scanner heap report allocation interop
    we_compat_report_t *report = we_compat_report_alloc();
    assert(report != nullptr && "we_compat_report_alloc must allocate 246 KB report on heap");
    assert(report->tier == WE_TIER_UNKNOWN);
    we_compat_report_free(report);

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    assert(scanner != nullptr && "we_compat_scanner_create must succeed");
    we_compat_scanner_destroy(scanner);

    std::cout << "  -> Workshop & WWPKG C23 Modules interop passed." << std::endl;
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
    test_workshop_and_wwpkg_cxx23_interop();

    std::cout << "==========================================================" << std::endl;
    std::cout << "  >>> CORE C++23 COMPATIBILITY PASSED CLEANLY (100%) <<<  " << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
