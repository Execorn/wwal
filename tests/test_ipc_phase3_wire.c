// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_ipc_phase3_wire.c
 * @brief Shader Graph, Camera, Physics & Lighting IPC Wire Protocol & Fuzz Tests
 *
 * Verifies:
 * 1. Strict struct sizeof static asserts for all 8 IPC request/response structures:
 *    - waywal_ipc_req_compile_shader_graph_t: 1096 B
 *    - waywal_ipc_resp_compile_shader_graph_t: 280 B
 *    - waywal_ipc_req_set_camera_3d_t: 144 B
 *    - waywal_ipc_resp_set_camera_3d_t: 68 B
 *    - waywal_ipc_req_set_physics_params_t: 184 B
 *    - waywal_ipc_resp_set_physics_params_t: 12 B
 *    - waywal_ipc_req_set_scene_lights_t: 608 B
 *    - waywal_ipc_resp_set_scene_lights_t: 8 B
 * 2. Invariant 1:
 *    - sizeof(waywal_ipc_hdr_t) == 16
 *    - offsetof(waywal_img_metadata_t, filter_mode) == 72
 *    - daemon_validate_set_image_header() accepts legacy headers >= 72 bytes
 * 3. Round-trip serialization and deserialization for opcodes 0x001D..0x0020 and 0x800C..0x800F
 * 4. Fuzz rejection of truncated payloads and malformed string termination
 */

#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/os_compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ========================================================================= */
/* 1. STRUCT STATIC ASSERTS                                                  */
/* ========================================================================= */

static_assert(sizeof(waywal_ipc_hdr_t) == 16, "waywal_ipc_hdr_t must be exactly 16 bytes");
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Invariant 1: legacy prefix must end at byte 72");

static_assert(sizeof(waywal_ipc_req_compile_shader_graph_t) == 1096,
              "waywal_ipc_req_compile_shader_graph_t must be 1096 bytes");
static_assert(sizeof(waywal_ipc_resp_compile_shader_graph_t) == 280,
              "waywal_ipc_resp_compile_shader_graph_t must be 280 bytes");

static_assert(sizeof(waywal_ipc_req_set_camera_3d_t) == 144,
              "waywal_ipc_req_set_camera_3d_t must be 144 bytes");
static_assert(sizeof(waywal_ipc_resp_set_camera_3d_t) == 68,
              "waywal_ipc_resp_set_camera_3d_t must be 68 bytes");

static_assert(sizeof(waywal_ipc_req_set_physics_params_t) == 184,
              "waywal_ipc_req_set_physics_params_t must be 184 bytes");
static_assert(sizeof(waywal_ipc_resp_set_physics_params_t) == 12,
              "waywal_ipc_resp_set_physics_params_t must be 12 bytes");

static_assert(sizeof(waywal_ipc_req_set_scene_lights_t) == 608,
              "waywal_ipc_req_set_scene_lights_t must be 608 bytes");
static_assert(sizeof(waywal_ipc_resp_set_scene_lights_t) == 8,
              "waywal_ipc_resp_set_scene_lights_t must be 8 bytes");

/* ========================================================================= */
/* 2. INVARIANT 1 LEGACY VALIDATION                                          */
/* ========================================================================= */

static void test_invariant1_legacy_header_validation(void)
{
    printf("[TEST] Invariant 1 Legacy Header Validation...\n");

    waywal_ipc_hdr_t valid_72 = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 72,
    };
    assert(daemon_validate_set_image_header(&valid_72) == true);

    waywal_ipc_hdr_t valid_extended = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = sizeof(waywal_img_metadata_t),
    };
    assert(daemon_validate_set_image_header(&valid_extended) == true);

    waywal_ipc_hdr_t valid_arbitrary_large = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 128,
    };
    assert(daemon_validate_set_image_header(&valid_arbitrary_large) == true);

    // Reject smaller than 72 bytes
    waywal_ipc_hdr_t too_small = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 71,
    };
    assert(daemon_validate_set_image_header(&too_small) == false);

    // Reject bad opcode
    waywal_ipc_hdr_t bad_opcode = valid_72;
    bad_opcode.opcode = 0x9999;
    assert(daemon_validate_set_image_header(&bad_opcode) == false);

    // Null pointer
    assert(daemon_validate_set_image_header(NULL) == false);

    printf("  -> PASS: 72-byte minimum bounds and legacy compatibility verified.\n");
}

/* ========================================================================= */
/* 3. COMPILE SHADER GRAPH ROUND-TRIP & FUZZ (0x001D / 0x800C)               */
/* ========================================================================= */

static void test_compile_shader_graph_wire(void)
{
    printf("[TEST] Shader Graph Compiler IPC Wire (0x001D / 0x800C)...\n");

    const char *test_json = "{\"schema\":\"wwgraph-1.0\",\"nodes\":[],\"edges\":[]}";
    uint8_t buffer[2048];
    size_t total_len = 0;
    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_compile_shader_graph_t req;

    bool ok = waywal_ipc_serialize_req_compile_shader_graph(
        "DP-1", "custom/path/graph.wwgraph", test_json,
        WAYWAL_COMPILE_FLAG_VALIDATE_ONLY | WAYWAL_COMPILE_FLAG_HOT_SWAP, &hdr, &req, buffer,
        sizeof(buffer), &total_len);

    assert(ok);
    assert(hdr.magic == WAYWAL_IPC_MAGIC);
    assert(hdr.opcode == WAYWAL_REQ_COMPILE_SHADER_GRAPH);
    assert(hdr.payload_size == sizeof(waywal_ipc_req_compile_shader_graph_t) + strlen(test_json));
    assert(total_len == hdr.payload_size);

    // Deserialization / validation
    const char *out_inline_json = NULL;
    const waywal_ipc_req_compile_shader_graph_t *received_req =
        (const waywal_ipc_req_compile_shader_graph_t *)buffer;
    assert(waywal_ipc_validate_req_compile_shader_graph(received_req, total_len, &out_inline_json));
    assert(strcmp(received_req->target_output, "DP-1") == 0);
    assert(strcmp(received_req->graph_json_path, "custom/path/graph.wwgraph") == 0);
    assert(received_req->flags ==
           (WAYWAL_COMPILE_FLAG_VALIDATE_ONLY | WAYWAL_COMPILE_FLAG_HOT_SWAP));
    assert(out_inline_json != NULL);
    assert(strcmp(out_inline_json, test_json) == 0);

    // Response round-trip
    waywal_ipc_resp_compile_shader_graph_t resp;
    waywal_ipc_hdr_t resp_hdr;
    ok = waywal_ipc_serialize_resp_compile_shader_graph(
        0, 0x1F, 42, 1280, 2.75f, 0, "DAG compilation succeeded", &resp_hdr, &resp);
    assert(ok);
    assert(resp_hdr.opcode == WAYWAL_RESP_COMPILE_SHADER_GRAPH);
    assert(resp_hdr.payload_size == sizeof(resp));
    assert(waywal_ipc_validate_resp_compile_shader_graph(&resp, sizeof(resp)));
    assert(resp.status_code == 0);
    assert(resp.active_pass_mask == 0x1F);
    assert(resp.compiled_node_count == 42);
    assert(resp.estimated_alu_ops == 1280);
    assert(strcmp(resp.error_msg, "DAG compilation succeeded") == 0);

    // Fuzz truncation & malformed buffer checks
    assert(!waywal_ipc_validate_req_compile_shader_graph(received_req, total_len - 1,
                                                         &out_inline_json));
    assert(!waywal_ipc_validate_req_compile_shader_graph(received_req, sizeof(req) - 1,
                                                         &out_inline_json));
    assert(!waywal_ipc_validate_req_compile_shader_graph(NULL, total_len, &out_inline_json));
    assert(!waywal_ipc_validate_resp_compile_shader_graph(&resp, sizeof(resp) - 1));
    assert(!waywal_ipc_validate_resp_compile_shader_graph(NULL, sizeof(resp)));

    // Non-null-terminated string rejection
    uint8_t malformed_buf[sizeof(waywal_ipc_req_compile_shader_graph_t)];
    memcpy(malformed_buf, received_req, sizeof(malformed_buf));
    memset(((waywal_ipc_req_compile_shader_graph_t *)malformed_buf)->target_output, 'A', 64);
    assert(!waywal_ipc_validate_req_compile_shader_graph(
        (const waywal_ipc_req_compile_shader_graph_t *)malformed_buf, sizeof(malformed_buf), NULL));

    printf("  -> PASS: Shader Graph Compiler IPC wire round-trip & fuzzing verified.\n");
}

/* ========================================================================= */
/* 4. CAMERA 3D ROUND-TRIP & FUZZ (0x001E / 0x800D)                          */
/* ========================================================================= */

static void test_camera3d_wire(void)
{
    printf("[TEST] Camera 3D IPC Wire (0x001E / 0x800D)...\n");

    const float eye[3] = {0.0f, 1.5f, -3.0f};
    const float target[3] = {0.0f, 0.5f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float parallax[2] = {0.05f, 0.02f};

    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_set_camera_3d_t req;

    bool ok = waywal_ipc_serialize_req_set_camera_3d(
        "HDMI-A-1",
        1, // Perspective
        WAYWAL_CAMERA_FLAG_ENABLE_DOF | WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES, eye, target, up, 60.0f,
        0.1f, 100.0f, parallax, 0.85f, 3.5f, 1.2f, 8.0f, &hdr, &req);

    assert(ok);
    assert(hdr.magic == WAYWAL_IPC_MAGIC);
    assert(hdr.opcode == WAYWAL_REQ_SET_CAMERA_3D);
    assert(hdr.payload_size == sizeof(waywal_ipc_req_set_camera_3d_t));

    assert(waywal_ipc_validate_req_set_camera_3d(&req, sizeof(req)));
    assert(strcmp(req.target_output, "HDMI-A-1") == 0);
    assert(req.projection_mode == 1);
    assert(req.flags == (WAYWAL_CAMERA_FLAG_ENABLE_DOF | WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES));
    assert(req.eye_pos[1] == 1.5f);
    assert(req.fov_y_deg == 60.0f);
    assert(req.dof_bokeh_radius_px == 8.0f);

    // Response round-trip
    waywal_ipc_hdr_t resp_hdr;
    waywal_ipc_resp_set_camera_3d_t resp;
    float vp[16];
    for (int i = 0; i < 16; ++i)
        vp[i] = (float)i * 0.1f;

    ok = waywal_ipc_serialize_resp_set_camera_3d(0, vp, &resp_hdr, &resp);
    assert(ok);
    assert(resp_hdr.opcode == WAYWAL_RESP_SET_CAMERA_3D);
    assert(resp_hdr.payload_size == sizeof(resp));
    assert(waywal_ipc_validate_resp_set_camera_3d(&resp, sizeof(resp)));
    assert(resp.active_view_proj[5] == 0.5f);

    // Fuzz truncation
    assert(!waywal_ipc_validate_req_set_camera_3d(&req, sizeof(req) - 1));
    assert(!waywal_ipc_validate_req_set_camera_3d(NULL, sizeof(req)));
    assert(!waywal_ipc_validate_resp_set_camera_3d(&resp, sizeof(resp) - 1));
    assert(!waywal_ipc_validate_resp_set_camera_3d(NULL, sizeof(resp)));

    printf("  -> PASS: Camera 3D IPC wire round-trip & fuzzing verified.\n");
}

/* ========================================================================= */
/* 5. PHYSICS PARAMS ROUND-TRIP & FUZZ (0x001F / 0x800E)                     */
/* ========================================================================= */

static void test_physics_wire(void)
{
    printf("[TEST] Physics Params IPC Wire (0x001F / 0x800E)...\n");

    we_bone_sim_config_t bone_cfg = {
        .bone_index = 4,
        .flags = WE_BONE_PHYS_ENABLED | WE_BONE_PHYS_CURSOR_COLLIDE,
        .compliance_stretch = 1e-6f,
        .compliance_bend = 1e-4f,
        .damping_ratio = 0.2f,
        .mass_kg = 0.5f,
        .gravity_scale = 1.0f,
        .wind_drag_coeff = 1.5f,
        .angle_limit_min_rad = -0.5f,
        .angle_limit_max_rad = 0.5f,
        .collision_radius_px = 12.0f,
        .rest_length_px = 64.0f,
        .audio_drag_coeff = 0.8f,
        .rayleigh_damping = 0.05f,
    };

    we_wind_field_t wind = {
        .enabled = 1,
        .base_direction_rad = 0.785f,
        .base_speed_px_s = 150.0f,
        .gust_frequency_hz = 1.5f,
        .gust_amplitude = 2.0f,
        .curl_spatial_scale = 0.005f,
        .gravity_vec = {0.0f, -980.0f},
    };

    we_cursor_force_t cursor = {
        .enabled = 1,
        .radius_px = 120.0f,
        .impulse_strength = 250.0f,
        .falloff_power = 2.0f,
    };

    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_set_physics_params_t req;

    bool ok = waywal_ipc_serialize_req_set_physics_params("DP-2", 101, 4, &bone_cfg, &wind, &cursor,
                                                          &hdr, &req);

    assert(ok);
    assert(hdr.magic == WAYWAL_IPC_MAGIC);
    assert(hdr.opcode == WAYWAL_REQ_SET_PHYSICS_PARAMS);
    assert(hdr.payload_size == sizeof(waywal_ipc_req_set_physics_params_t));

    assert(waywal_ipc_validate_req_set_physics_params(&req, sizeof(req)));
    assert(strcmp(req.target_output, "DP-2") == 0);
    assert(req.object_id == 101);
    assert(req.bone_index == 4);
    assert(req.bone_cfg.collision_radius_px == 12.0f);
    assert(req.wind.base_speed_px_s == 150.0f);
    assert(req.cursor.radius_px == 120.0f);

    // Response round-trip
    waywal_ipc_hdr_t resp_hdr;
    waywal_ipc_resp_set_physics_params_t resp;
    ok = waywal_ipc_serialize_resp_set_physics_params(0, 16, 42.5f, &resp_hdr, &resp);
    assert(ok);
    assert(resp_hdr.opcode == WAYWAL_RESP_SET_PHYSICS_PARAMS);
    assert(resp_hdr.payload_size == sizeof(resp));
    assert(waywal_ipc_validate_resp_set_physics_params(&resp, sizeof(resp)));
    assert(resp.active_sim_bones == 16);
    assert(resp.last_step_time_us == 42.5f);

    // Fuzz truncation
    assert(!waywal_ipc_validate_req_set_physics_params(&req, sizeof(req) - 1));
    assert(!waywal_ipc_validate_req_set_physics_params(NULL, sizeof(req)));
    assert(!waywal_ipc_validate_resp_set_physics_params(&resp, sizeof(resp) - 1));
    assert(!waywal_ipc_validate_resp_set_physics_params(NULL, sizeof(resp)));

    printf("  -> PASS: Physics Params IPC wire round-trip & fuzzing verified.\n");
}

/* ========================================================================= */
/* 6. SCENE LIGHTS ROUND-TRIP & FUZZ (0x0020 / 0x800F)                       */
/* ========================================================================= */

static void test_scene_lights_wire(void)
{
    printf("[TEST] Scene Lights IPC Wire (0x0020 / 0x800F)...\n");

    we_lighting_uniforms_t lights;
    memset(&lights, 0, sizeof(lights));
    lights.active_light_count = 3;
    lights.ambient_color_rgb[0] = 0.2f;
    lights.ambient_color_rgb[1] = 0.2f;
    lights.ambient_color_rgb[2] = 0.25f;
    lights.normal_map_strength = 1.0f;

    lights.lights[0].light_type = WE_LIGHT_POINT;
    lights.lights[0].color_rgb[0] = 1.0f;
    lights.lights[0].intensity = 5.0f;
    lights.lights[0].position_or_dir[0] = 2.0f;
    lights.lights[0].position_or_dir[1] = 4.0f;
    lights.lights[0].position_or_dir[2] = -1.0f;
    lights.lights[0].attenuation_radius = 10.0f;

    waywal_ipc_hdr_t hdr;
    waywal_ipc_req_set_scene_lights_t req;

    bool ok = waywal_ipc_serialize_req_set_scene_lights("*", &lights, &hdr, &req);
    assert(ok);
    assert(hdr.magic == WAYWAL_IPC_MAGIC);
    assert(hdr.opcode == WAYWAL_REQ_SET_SCENE_LIGHTS);
    assert(hdr.payload_size == sizeof(waywal_ipc_req_set_scene_lights_t));

    assert(waywal_ipc_validate_req_set_scene_lights(&req, sizeof(req)));
    assert(strcmp(req.target_output, "*") == 0);
    assert(req.lighting.active_light_count == 3);
    assert(req.lighting.normal_map_strength == 1.0f);
    assert(req.lighting.lights[0].attenuation_radius == 10.0f);

    // Response round-trip
    waywal_ipc_hdr_t resp_hdr;
    waywal_ipc_resp_set_scene_lights_t resp;
    ok = waywal_ipc_serialize_resp_set_scene_lights(0, 3, &resp_hdr, &resp);
    assert(ok);
    assert(resp_hdr.opcode == WAYWAL_RESP_SET_SCENE_LIGHTS);
    assert(resp_hdr.payload_size == sizeof(resp));
    assert(waywal_ipc_validate_resp_set_scene_lights(&resp, sizeof(resp)));
    assert(resp.active_lights == 3);

    // Fuzz truncation
    assert(!waywal_ipc_validate_req_set_scene_lights(&req, sizeof(req) - 1));
    assert(!waywal_ipc_validate_req_set_scene_lights(NULL, sizeof(req)));
    assert(!waywal_ipc_validate_resp_set_scene_lights(&resp, sizeof(resp) - 1));
    assert(!waywal_ipc_validate_resp_set_scene_lights(NULL, sizeof(resp)));

    printf("  -> PASS: Scene Lights IPC wire round-trip & fuzzing verified.\n");
}

static void test_adversarial_nan_enum_and_bounds_fuzz(void)
{
    printf("[TEST] Adversarial NaN/Inf, Enum & Bounds Wire Validation...\n");

    // 1. Camera 3D NaN/Inf & enum bounds
    {
        const float eye[3] = {0.0f, 1.0f, 5.0f};
        const float target[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 1.0f, 0.0f};
        const float parallax[2] = {0.1f, 0.1f};
        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_camera_3d_t req;
        assert(waywal_ipc_serialize_req_set_camera_3d("*", 1, 0, eye, target, up, 60.0f, 0.1f,
                                                      100.0f, parallax, 0.5f, 4.0f, 1.0f, 6.0f,
                                                      &hdr, &req));
        assert(waywal_ipc_validate_req_set_camera_3d(&req, sizeof(req)));

        waywal_ipc_req_set_camera_3d_t bad = req;
        bad.projection_mode = 99u;
        assert(!waywal_ipc_validate_req_set_camera_3d(&bad, sizeof(bad)));

        bad = req;
        bad.parallax_amount[1] = NAN;
        assert(!waywal_ipc_validate_req_set_camera_3d(&bad, sizeof(bad)));

        bad = req;
        bad.dof_bokeh_radius_px = INFINITY;
        assert(!waywal_ipc_validate_req_set_camera_3d(&bad, sizeof(bad)));
    }

    // 2. Physics params NaN/Inf & bone_index bounds
    {
        we_bone_sim_config_t bone_cfg;
        memset(&bone_cfg, 0, sizeof(bone_cfg));
        bone_cfg.mass_kg = 1.0f;
        we_wind_field_t wind;
        memset(&wind, 0, sizeof(wind));
        we_cursor_force_t cursor;
        memset(&cursor, 0, sizeof(cursor));

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_physics_params_t req;
        assert(waywal_ipc_serialize_req_set_physics_params("*", -1, 0, &bone_cfg, &wind, &cursor,
                                                           &hdr, &req));
        assert(waywal_ipc_validate_req_set_physics_params(&req, sizeof(req)));

        waywal_ipc_req_set_physics_params_t bad = req;
        bad.bone_index = -2;
        assert(!waywal_ipc_validate_req_set_physics_params(&bad, sizeof(bad)));

        bad = req;
        bad.bone_index = (int32_t)WE_MDL_MAX_BONES;
        assert(!waywal_ipc_validate_req_set_physics_params(&bad, sizeof(bad)));

        bad = req;
        bad.bone_cfg.mass_kg = NAN;
        assert(!waywal_ipc_validate_req_set_physics_params(&bad, sizeof(bad)));

        bad = req;
        bad.wind.gust_amplitude = INFINITY;
        assert(!waywal_ipc_validate_req_set_physics_params(&bad, sizeof(bad)));

        bad = req;
        bad.cursor.impulse_strength = -INFINITY;
        assert(!waywal_ipc_validate_req_set_physics_params(&bad, sizeof(bad)));
    }

    // 3. Scene lights NaN/Inf & light_type bounds
    {
        we_lighting_uniforms_t lights;
        memset(&lights, 0, sizeof(lights));
        lights.active_light_count = 1;
        lights.lights[0].light_type = WE_LIGHT_SPOT;
        lights.lights[0].intensity = 2.0f;

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_scene_lights_t req;
        assert(waywal_ipc_serialize_req_set_scene_lights("*", &lights, &hdr, &req));
        assert(waywal_ipc_validate_req_set_scene_lights(&req, sizeof(req)));

        waywal_ipc_req_set_scene_lights_t bad = req;
        bad.lighting.ambient_color_rgb[0] = NAN;
        assert(!waywal_ipc_validate_req_set_scene_lights(&bad, sizeof(bad)));

        bad = req;
        bad.lighting.lights[0].light_type = 77u;
        assert(!waywal_ipc_validate_req_set_scene_lights(&bad, sizeof(bad)));

        bad = req;
        bad.lighting.lights[0].spot_dir[2] = INFINITY;
        assert(!waywal_ipc_validate_req_set_scene_lights(&bad, sizeof(bad)));
    }

    // 4. Embedded NUL in inline_json rejection
    {
        uint8_t buf[sizeof(waywal_ipc_req_compile_shader_graph_t) + 8];
        memset(buf, 0, sizeof(buf));
        waywal_ipc_req_compile_shader_graph_t req_hdr_body;
        memset(&req_hdr_body, 0, sizeof(req_hdr_body));
        req_hdr_body.target_output[0] = '*';
        req_hdr_body.inline_json_len = 8u;
        memcpy(buf, &req_hdr_body, sizeof(req_hdr_body));
        memcpy(buf + sizeof(req_hdr_body), "{}\0bad{}", 8u);
        assert(!waywal_ipc_validate_req_compile_shader_graph(
            (const waywal_ipc_req_compile_shader_graph_t *)buf, sizeof(buf), NULL));
    }

    printf("  -> PASS: Adversarial NaN/Inf, enum & bounds wire fuzzing verified.\n");
}

static bool p3_read_exact(int fd, void *buf, size_t len)
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

static void test_structured_uniform_148b_and_legacy_compat(void)
{
    printf("[Test] Structured 148 B Uniform & Legacy 64/64 Compat ...\n");

    assert(sizeof(waywal_set_uniform_structured_t) == 148u);
    assert(sizeof(waywal_set_uniform_payload_t) == 148u);

    waywal_set_uniform_payload_t pay;
    memset(&pay, 0, sizeof(pay));
    pay.structured.struct_size = sizeof(waywal_set_uniform_structured_t);
    pay.structured.flags = WAYWAL_UNIFORM_FLAG_STRUCTURED | WAYWAL_UNIFORM_FLAG_PERSIST;
    snprintf(pay.structured.target_output, sizeof(pay.structured.target_output), "%s", "DP-1");
    snprintf(pay.structured.uniform_name, sizeof(pay.structured.uniform_name), "%s", "u_exposure");
    pay.structured.uniform_components = 3u;
    pay.structured.uniform_values[0] = 1.25f;
    pay.structured.uniform_values[1] = 0.50f;
    pay.structured.uniform_values[2] = 2.00f;
    pay.structured.uniform_values[3] = 0.00f;

    assert(waywal_validate_set_uniform_structured(&pay.structured));

    char target[64] = {0};
    char uname[64] = {0};
    uint32_t comps = 0u;
    float vals[4] = {0};
    uint32_t flags = 0u;
    assert(waywal_decode_set_uniform_payload(&pay, target, sizeof(target), uname, sizeof(uname),
                                             &comps, vals, &flags));
    assert(strcmp(target, "DP-1") == 0);
    assert(strcmp(uname, "u_exposure") == 0);
    assert(comps == 3u);
    assert(fabsf(vals[0] - 1.25f) < 1e-6f);
    assert(fabsf(vals[1] - 0.50f) < 1e-6f);
    assert(fabsf(vals[2] - 2.00f) < 1e-6f);
    assert(flags == (WAYWAL_UNIFORM_FLAG_STRUCTURED | WAYWAL_UNIFORM_FLAG_PERSIST));

    /* Missing STRUCTURED flag with struct_size == 148 B must be rejected. */
    waywal_set_uniform_payload_t bad_pay = pay;
    bad_pay.structured.flags = WAYWAL_UNIFORM_FLAG_PERSIST;
    assert(!waywal_decode_set_uniform_payload(&bad_pay, target, sizeof(target), uname,
                                              sizeof(uname), &comps, vals, &flags));

    /* Unknown flag bits, component bounds, non-finite floats, and unterminated names must fail. */
    waywal_set_uniform_structured_t bad_st = pay.structured;
    bad_st.flags |= 0x8000u;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.struct_size = 144u;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.uniform_components = 0u;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.uniform_components = 5u;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.uniform_values[2] = NAN;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.uniform_values[3] = INFINITY;
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    bad_st.uniform_name[0] = '\0';
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    bad_st = pay.structured;
    memset(bad_st.target_output, 'X', sizeof(bad_st.target_output));
    assert(!waywal_validate_set_uniform_structured(&bad_st));

    /* Legacy 64/64 wire form (struct_size == 0) remains compatible. */
    waywal_set_uniform_payload_t leg;
    memset(&leg, 0, sizeof(leg));
    snprintf(leg.output, sizeof(leg.output), "%s", "HDMI-A-1");
    snprintf(leg.name, sizeof(leg.name), "%s", "u_gamma");
    leg.num_components = 1u;
    leg.values[0] = 2.2f;
    assert(waywal_decode_set_uniform_payload(&leg, target, sizeof(target), uname, sizeof(uname),
                                             &comps, vals, &flags));
    assert(strcmp(target, "HDMI-A-1") == 0);
    assert(strcmp(uname, "u_gamma") == 0);
    assert(comps == 1u);
    assert(fabsf(vals[0] - 2.2f) < 1e-6f);
    assert(flags == 0u);

    printf("  -> PASS: 148 B structured uniform and legacy 64/64 compatibility verified.\n");
}

static void test_export_0x8006_0x8007_single_sendmsg_framing(void)
{
    printf("[Test] Single-sendmsg 0x8006 TELEMETRY & 0x8007 DMABUF + F_SEAL_SHRINK ...\n");

    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

    /* 0x8006 telemetry snapshot (1208 B, 0 SCM_RIGHTS FDs) */
    waywal_telemetry_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.version = 1u;
    snap.rss_kib = 4096u;
    snap.num_outputs = 1u;
    snap.bark_bands[0] = 0.75f;

    waywal_ipc_hdr_t thdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_RESP_TELEMETRY,
        .payload_size = sizeof(snap),
    };
    assert(waywal_ipc_send_payload_fds(sv[0], &thdr, &snap, sizeof(snap), NULL, 0u));

    waywal_ipc_hdr_t rhdr = {0};
    int rx_fds[4] = {-1, -1, -1, -1};
    size_t rx_nfds = 99u;
    assert(waywal_ipc_recv_fds(sv[1], &rhdr, rx_fds, 4u, &rx_nfds));
    assert(rhdr.magic == WAYWAL_IPC_MAGIC);
    assert(rhdr.version == WAYWAL_IPC_VERSION);
    assert(rhdr.opcode == WAYWAL_RESP_TELEMETRY);
    assert(rhdr.payload_size == sizeof(snap));
    assert(rx_nfds == 0u);

    waywal_telemetry_snapshot_t snap_rx;
    memset(&snap_rx, 0, sizeof(snap_rx));
    assert(p3_read_exact(sv[1], &snap_rx, sizeof(snap_rx)));
    assert(memcmp(&snap_rx, &snap, sizeof(snap)) == 0);

    /* 0x8007 DMABUF export response (128 B + 2 shrink-sealed plane FDs) */
    int planes[2] = {-1, -1};
    planes[0] = waywal_create_memfd("waywal-test-plane0", 4096u, F_SEAL_SHRINK);
    planes[1] = waywal_create_memfd("waywal-test-plane1", 4096u, F_SEAL_SHRINK);
    assert(planes[0] >= 0 && planes[1] >= 0);

    waywal_export_dmabuf_resp_t dresp;
    memset(&dresp, 0, sizeof(dresp));
    dresp.width = 1920u;
    dresp.height = 1080u;
    dresp.drm_format = 0x34325241u;
    dresp.num_planes = 2u;
    dresp.strides[0] = 7680u;
    dresp.strides[1] = 3840u;
    snprintf(dresp.output, sizeof(dresp.output), "%s", "DP-1");
    dresp.flags = 0x2u;

    waywal_ipc_hdr_t dhdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_RESP_DMABUF,
        .payload_size = sizeof(dresp),
    };
    assert(waywal_ipc_send_payload_fds(sv[0], &dhdr, &dresp, sizeof(dresp), planes, 2u));
    close(planes[0]);
    close(planes[1]);

    memset(&rhdr, 0, sizeof(rhdr));
    rx_nfds = 0u;
    assert(waywal_ipc_recv_fds(sv[1], &rhdr, rx_fds, 4u, &rx_nfds));
    assert(rhdr.opcode == WAYWAL_RESP_DMABUF);
    assert(rhdr.payload_size == sizeof(dresp));
    assert(rx_nfds == 2u);
    assert(rx_fds[0] >= 0 && rx_fds[1] >= 0);

    for (size_t i = 0; i < rx_nfds; ++i) {
        int seals = fcntl(rx_fds[i], F_GET_SEALS);
        assert(seals >= 0 && (seals & F_SEAL_SHRINK) != 0);
        close(rx_fds[i]);
    }

    waywal_export_dmabuf_resp_t dresp_rx;
    memset(&dresp_rx, 0, sizeof(dresp_rx));
    assert(p3_read_exact(sv[1], &dresp_rx, sizeof(dresp_rx)));
    assert(memcmp(&dresp_rx, &dresp, sizeof(dresp)) == 0);

    close(sv[0]);
    close(sv[1]);
    printf("  -> PASS: 0x8006/0x8007 single-sendmsg framing & F_SEAL_SHRINK verified.\n");
}

static void test_nonblocking_eagain_and_sigpipe_resilience(void)
{
    printf("[Test] Non-blocking SOCK_NONBLOCK EAGAIN & SIGPIPE=SIG_IGN Resilience ...\n");

    struct sigaction sa_ign = {0};
    struct sigaction sa_prev = {0};
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    assert(sigaction(SIGPIPE, &sa_ign, &sa_prev) == 0);

    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
    close(sv[1]);

    /* Disconnected peer must yield EPIPE without killing the process. */
    const uint8_t probe = 0xAAu;
    ssize_t dead_w = send(sv[0], &probe, 1u, MSG_NOSIGNAL | MSG_DONTWAIT);
    assert(dead_w < 0 && errno == EPIPE);
    close(sv[0]);

    /* Exercise full EAGAIN + short-write completion across a constrained UNIX socket buffer. */
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
    int small_buf = 8192;
    (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small_buf, sizeof(small_buf));
    (void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small_buf, sizeof(small_buf));

    const size_t total_len = 256u * 1024u;
    uint8_t *tx_buf = malloc(total_len);
    assert(tx_buf != NULL);
    for (size_t i = 0; i < total_len; ++i) {
        tx_buf[i] = (uint8_t)((i * 131u + 17u) & 0xFFu);
    }

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        uint8_t chunk[2048];
        size_t got = 0u;
        while (got < total_len) {
            struct pollfd pfd = {.fd = sv[1], .events = POLLIN, .revents = 0};
            if (poll(&pfd, 1, 3000) <= 0) {
                _exit(2);
            }
            ssize_t r = recv(sv[1], chunk, sizeof(chunk), 0);
            if (r < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                _exit(3);
            }
            if (r == 0) {
                _exit(4);
            }
            for (ssize_t k = 0; k < r; ++k) {
                uint8_t expected = (uint8_t)(((got + (size_t)k) * 131u + 17u) & 0xFFu);
                if (chunk[k] != expected) {
                    _exit(5);
                }
            }
            got += (size_t)r;
        }
        close(sv[1]);
        _exit(0);
    }

    close(sv[1]);
    size_t sent = 0u;
    while (sent < total_len) {
        ssize_t n = send(sv[0], tx_buf + sent, total_len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = sv[0], .events = POLLOUT, .revents = 0};
                assert(poll(&pfd, 1, 3000) > 0);
                continue;
            }
            assert(0 && "Unexpected send failure in non-blocking test");
        }
        assert(n > 0);
        sent += (size_t)n;
    }

    close(sv[0]);
    free(tx_buf);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    (void)sigaction(SIGPIPE, &sa_prev, NULL);

    printf("  -> PASS: SOCK_NONBLOCK EAGAIN short-write completion & EPIPE resilience verified.\n");
}

/* ========================================================================= */
/* 7. LIVE SOCKET WIRE TEST                                                  */
/* ========================================================================= */

static int try_connect_socket(const char *path)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int find_or_spawn_daemon(const char *ns, pid_t *out_pid, char *out_sock_path,
                                size_t sock_cap)
{
    *out_pid = -1;
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir)
        runtime_dir = "/tmp";
    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    if (!wl_disp)
        wl_disp = "wayland-1";

    snprintf(out_sock_path, sock_cap, "%s/%s-wwald.%s.sock", runtime_dir, wl_disp, ns);

    int s = try_connect_socket(out_sock_path);
    if (s >= 0) {
        close(s);
        return 0;
    }

    const char *candidates[] = {"./wwald",        "build_worker/wwald",
                                "build/wwald",    "build/src/daemon/wwald",
                                "../build/wwald", NULL};
    const char *daemon_bin = NULL;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            daemon_bin = candidates[i];
            break;
        }
    }

    if (!daemon_bin) {
        return -2;
    }

    printf("[TEST] Spawning temporary daemon: %s --namespace %s\n", daemon_bin, ns);
    pid_t pid = fork();
    if (pid == 0) {
        execl(daemon_bin, daemon_bin, "--namespace", ns, (char *)NULL);
        _exit(127);
    }

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    *out_pid = pid;

    for (int i = 0; i < 40; ++i) {
        usleep(50000);
        s = try_connect_socket(out_sock_path);
        if (s >= 0) {
            close(s);
            return 0;
        }
        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            return -1;
        }
    }
    return -1;
}

static bool wire_full_read(int fd, void *buf, size_t count, int timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return false;
        ssize_t n = read(fd, p + total, count - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

static bool wire_full_write(int fd, const void *buf, size_t count, int timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return false;
        ssize_t n = write(fd, p + total, count - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

static void test_live_socket_round_trip(void)
{
    printf("[TEST] Live Socket IPC Dispatch (0x001D..0x0020 -> 0x800C..0x800F)...\n");

    const char *ns = "test_phase3_wire";
    char sock_path[1024] = {0};
    pid_t daemon_pid = -1;

    int ready = find_or_spawn_daemon(ns, &daemon_pid, sock_path, sizeof(sock_path));
    if (ready == -2) {
        printf("[TEST] Notice: wwald binary not found. Skipping live socket test.\n");
        return;
    }
    if (ready < 0) {
        /* wwald requires a Wayland compositor; skip live test only when WAYLAND_DISPLAY is absent
         */
        if (getenv("WAYLAND_DISPLAY") == NULL) {
            printf("[TEST] Notice: Headless environment without Wayland display. Skipping live "
                   "test.\n");
            return;
        }
        fprintf(stderr, "[-] Failed to spawn/connect to wwald daemon at %s\n", sock_path);
        if (daemon_pid > 0)
            kill(daemon_pid, SIGTERM);
        assert(false && "Failed to spawn/connect to daemon for live wire test");
        return;
    }

    // --- 1. Test WAYWAL_REQ_COMPILE_SHADER_GRAPH (0x001D -> 0x800C) ---
    {
        int fd = try_connect_socket(sock_path);
        assert(fd >= 0);

        const char *test_graph_json =
            "{\"schema_version\":1,\"graph_name\":\"LiveWireTest\",\"nodes\":["
            "{\"node_id\":1,\"opcode\":0,\"title\":\"UV\",\"canvas_pos\":[0,0]},"
            "{\"node_id\":2,\"opcode\":50,\"title\":\"Sink\",\"canvas_pos\":[200,0]}"
            "],\"edges\":["
            "{\"edge_id\":1,\"src_node_id\":1,\"src_pin_index\":0,\"dst_node_id\":2,\"dst_pin_"
            "index\":0}"
            "]}";
        uint32_t json_len = (uint32_t)strlen(test_graph_json);

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_compile_shader_graph_t req;
        memset(&hdr, 0, sizeof(hdr));
        memset(&req, 0, sizeof(req));

        hdr.magic = WAYWAL_IPC_MAGIC;
        hdr.version = WAYWAL_IPC_VERSION;
        hdr.opcode = WAYWAL_REQ_COMPILE_SHADER_GRAPH;
        hdr.payload_size = sizeof(req) + json_len;

        strncpy(req.target_output, "*", sizeof(req.target_output) - 1);
        req.inline_json_len = json_len;
        req.flags = WAYWAL_COMPILE_FLAG_VALIDATE_ONLY;

        assert(wire_full_write(fd, &hdr, sizeof(hdr), 1000));
        assert(wire_full_write(fd, &req, sizeof(req), 1000));
        assert(wire_full_write(fd, test_graph_json, json_len, 1000));

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_resp_compile_shader_graph_t resp;
        assert(wire_full_read(fd, &resp_hdr, sizeof(resp_hdr), 2000));
        assert(resp_hdr.magic == WAYWAL_IPC_MAGIC);
        assert(resp_hdr.opcode == WAYWAL_RESP_COMPILE_SHADER_GRAPH);
        assert(resp_hdr.payload_size == sizeof(resp));

        assert(wire_full_read(fd, &resp, sizeof(resp), 2000));
        assert(resp.status_code == WAYWAL_STATUS_OK);
        assert(resp.compiled_node_count >= 2);
        assert((resp.active_pass_mask & 0x10) != 0);

        close(fd);
        printf("  -> Live 0x001D (COMPILE_SHADER_GRAPH) OK -> 0x800C (nodes=%u, status=%u)\n",
               resp.compiled_node_count, resp.status_code);
    }

    // --- 2. Test WAYWAL_REQ_SET_CAMERA_3D (0x001E -> 0x800D) ---
    {
        int fd = try_connect_socket(sock_path);
        assert(fd >= 0);

        const float eye[3] = {0.0f, 1.5f, -3.0f};
        const float target[3] = {0.0f, 0.5f, 0.0f};
        const float up[3] = {0.0f, 1.0f, 0.0f};
        const float parallax[2] = {0.05f, 0.02f};

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_camera_3d_t req;
        assert(waywal_ipc_serialize_req_set_camera_3d("*", 1, 0, eye, target, up, 60.0f, 0.1f,
                                                      1000.0f, parallax, 0.85f, 3.5f, 1.2f, 8.0f,
                                                      &hdr, &req));

        assert(wire_full_write(fd, &hdr, sizeof(hdr), 1000));
        assert(wire_full_write(fd, &req, sizeof(req), 1000));

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_resp_set_camera_3d_t resp;
        assert(wire_full_read(fd, &resp_hdr, sizeof(resp_hdr), 2000));
        assert(resp_hdr.magic == WAYWAL_IPC_MAGIC);
        assert(resp_hdr.opcode == WAYWAL_RESP_SET_CAMERA_3D);
        assert(resp_hdr.payload_size == sizeof(resp));

        assert(wire_full_read(fd, &resp, sizeof(resp), 2000));
        assert(resp.status_code == WAYWAL_STATUS_OK);

        close(fd);
        printf("  -> Live 0x001E (SET_CAMERA_3D) OK -> 0x800D (status=%u)\n", resp.status_code);
    }

    // --- 3. Test WAYWAL_REQ_SET_PHYSICS_PARAMS (0x001F -> 0x800E) ---
    {
        int fd = try_connect_socket(sock_path);
        assert(fd >= 0);

        we_bone_sim_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.compliance_stretch = 0.001f;
        cfg.compliance_bend = 0.005f;

        we_wind_field_t wind;
        memset(&wind, 0, sizeof(wind));
        wind.enabled = 1;
        wind.base_speed_px_s = 50.0f;

        we_cursor_force_t cursor;
        memset(&cursor, 0, sizeof(cursor));
        cursor.enabled = 1;
        cursor.radius_px = 120.0f;

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_physics_params_t req;
        assert(waywal_ipc_serialize_req_set_physics_params("*", -1, 0, &cfg, &wind, &cursor, &hdr,
                                                           &req));

        assert(wire_full_write(fd, &hdr, sizeof(hdr), 1000));
        assert(wire_full_write(fd, &req, sizeof(req), 1000));

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_resp_set_physics_params_t resp;
        assert(wire_full_read(fd, &resp_hdr, sizeof(resp_hdr), 2000));
        assert(resp_hdr.magic == WAYWAL_IPC_MAGIC);
        assert(resp_hdr.opcode == WAYWAL_RESP_SET_PHYSICS_PARAMS);
        assert(resp_hdr.payload_size == sizeof(resp));

        assert(wire_full_read(fd, &resp, sizeof(resp), 2000));
        assert(resp.status_code == WAYWAL_STATUS_OK);

        close(fd);
        printf("  -> Live 0x001F (SET_PHYSICS_PARAMS) OK -> 0x800E (active_sim_bones=%u)\n",
               resp.active_sim_bones);
    }

    // --- 4. Test WAYWAL_REQ_SET_SCENE_LIGHTS (0x0020 -> 0x800F) ---
    {
        int fd = try_connect_socket(sock_path);
        assert(fd >= 0);

        we_lighting_uniforms_t lights;
        memset(&lights, 0, sizeof(lights));
        lights.active_light_count = 2;
        lights.lights[0].light_type = WE_LIGHT_POINT;
        lights.lights[0].intensity = 4.0f;

        waywal_ipc_hdr_t hdr;
        waywal_ipc_req_set_scene_lights_t req;
        assert(waywal_ipc_serialize_req_set_scene_lights("*", &lights, &hdr, &req));

        assert(wire_full_write(fd, &hdr, sizeof(hdr), 1000));
        assert(wire_full_write(fd, &req, sizeof(req), 1000));

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_resp_set_scene_lights_t resp;
        assert(wire_full_read(fd, &resp_hdr, sizeof(resp_hdr), 2000));
        assert(resp_hdr.magic == WAYWAL_IPC_MAGIC);
        assert(resp_hdr.opcode == WAYWAL_RESP_SET_SCENE_LIGHTS);
        assert(resp_hdr.payload_size == sizeof(resp));

        assert(wire_full_read(fd, &resp, sizeof(resp), 2000));
        assert(resp.status_code == WAYWAL_STATUS_OK);
        assert(resp.active_lights == 2);

        close(fd);
        printf("  -> Live 0x0020 (SET_SCENE_LIGHTS) OK -> 0x800F (active_lights=%u)\n",
               resp.active_lights);
    }

    if (daemon_pid > 0) {
        kill(daemon_pid, SIGTERM);
        int status;
        waitpid(daemon_pid, &status, 0);
    }

    printf("  -> PASS: All IPC live wire requests/responses verified successfully.\n");
}

int main(void)
{
    printf("=================================================================\n");
    printf("Starting IPC Wire Protocol & Struct Test Suite\n");
    printf("=================================================================\n");

    test_invariant1_legacy_header_validation();
    test_compile_shader_graph_wire();
    test_camera3d_wire();
    test_physics_wire();
    test_scene_lights_wire();
    test_adversarial_nan_enum_and_bounds_fuzz();
    test_structured_uniform_148b_and_legacy_compat();
    test_export_0x8006_0x8007_single_sendmsg_framing();
    test_nonblocking_eagain_and_sigpipe_resilience();
    test_live_socket_round_trip();

    printf("=================================================================\n");
    printf("All IPC Wire & Struct Tests PASSED successfully!\n");
    printf("=================================================================\n");
    return 0;
}
