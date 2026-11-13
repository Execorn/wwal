// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/ipc_proto_phase3.h"
#include "waywal/ipc_proto_authoring.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WAYWAL_MAX_INLINE_GRAPH_JSON_BYTES (16u * 1024u * 1024u)

static bool bone_sim_config_floats_finite(const we_bone_sim_config_t *cfg)
{
    return isfinite(cfg->compliance_stretch) && isfinite(cfg->compliance_bend) &&
           isfinite(cfg->damping_ratio) && isfinite(cfg->mass_kg) && isfinite(cfg->gravity_scale) &&
           isfinite(cfg->wind_drag_coeff) && isfinite(cfg->angle_limit_min_rad) &&
           isfinite(cfg->angle_limit_max_rad) && isfinite(cfg->collision_radius_px) &&
           isfinite(cfg->rest_length_px) && isfinite(cfg->audio_drag_coeff) &&
           isfinite(cfg->rayleigh_damping);
}

static bool wind_field_floats_finite(const we_wind_field_t *wind)
{
    return isfinite(wind->base_direction_rad) && isfinite(wind->base_speed_px_s) &&
           isfinite(wind->gust_frequency_hz) && isfinite(wind->gust_amplitude) &&
           isfinite(wind->curl_spatial_scale) && isfinite(wind->gravity_vec[0]) &&
           isfinite(wind->gravity_vec[1]);
}

static bool cursor_force_floats_finite(const we_cursor_force_t *cursor)
{
    return isfinite(cursor->radius_px) && isfinite(cursor->impulse_strength) &&
           isfinite(cursor->falloff_power);
}

static bool scene_light_valid(const we_scene_light_t *lt)
{
    if (lt->light_type > (uint32_t)WE_LIGHT_TUBE) {
        return false;
    }
    return isfinite(lt->color_rgb[0]) && isfinite(lt->color_rgb[1]) && isfinite(lt->color_rgb[2]) &&
           isfinite(lt->intensity) && isfinite(lt->position_or_dir[0]) &&
           isfinite(lt->position_or_dir[1]) && isfinite(lt->position_or_dir[2]) &&
           isfinite(lt->attenuation_radius) && isfinite(lt->spot_inner_cos) &&
           isfinite(lt->spot_outer_cos) && isfinite(lt->tube_length) && isfinite(lt->spot_dir[0]) &&
           isfinite(lt->spot_dir[1]) && isfinite(lt->spot_dir[2]) && isfinite(lt->specular_power);
}

/* ========================================================================= */
/* WAYWAL_REQ_COMPILE_SHADER_GRAPH (0x001D) & RESP (0x800C)                  */
/* ========================================================================= */

bool waywal_ipc_validate_req_compile_shader_graph(const waywal_ipc_req_compile_shader_graph_t *req,
                                                  size_t payload_size, const char **out_inline_json)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_compile_shader_graph_t)) {
        return false;
    }

    waywal_ipc_req_compile_shader_graph_t r;
    memcpy(&r, req, sizeof(r));

    if (r.inline_json_len > WAYWAL_MAX_INLINE_GRAPH_JSON_BYTES) {
        return false;
    }

    if (payload_size != sizeof(r) + (size_t)r.inline_json_len) {
        return false;
    }

    if (!memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }

    if (!memchr(r.graph_json_path, '\0', sizeof(r.graph_json_path))) {
        return false;
    }

    const char *json_ptr = NULL;
    if (r.inline_json_len > 0) {
        json_ptr = (const char *)req + sizeof(waywal_ipc_req_compile_shader_graph_t);
        if (memchr(json_ptr, '\0', r.inline_json_len) != NULL) {
            return false;
        }
    }

    if (out_inline_json) {
        *out_inline_json = json_ptr;
    }

    return true;
}

bool waywal_ipc_serialize_req_compile_shader_graph(
    const char *target_output, const char *graph_json_path, const char *inline_json, uint32_t flags,
    waywal_ipc_hdr_t *out_hdr, waywal_ipc_req_compile_shader_graph_t *out_req, void *out_buf,
    size_t out_buf_cap, size_t *out_total_len)
{
    if (!out_hdr) {
        return false;
    }

    uint32_t inline_len = 0;
    if (inline_json) {
        size_t raw_len = strlen(inline_json);
        if (raw_len > WAYWAL_MAX_INLINE_GRAPH_JSON_BYTES) {
            return false;
        }
        inline_len = (uint32_t)raw_len;
    }

    size_t total_payload = sizeof(waywal_ipc_req_compile_shader_graph_t) + (size_t)inline_len;

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_COMPILE_SHADER_GRAPH;
    out_hdr->payload_size = (uint64_t)total_payload;

    waywal_ipc_req_compile_shader_graph_t req_data;
    memset(&req_data, 0, sizeof(req_data));

    if (target_output) {
        strncpy(req_data.target_output, target_output, sizeof(req_data.target_output) - 1);
    } else {
        req_data.target_output[0] = '*';
    }

    if (graph_json_path) {
        strncpy(req_data.graph_json_path, graph_json_path, sizeof(req_data.graph_json_path) - 1);
    }

    req_data.inline_json_len = inline_len;
    req_data.flags = flags;

    if (out_req) {
        memcpy(out_req, &req_data, sizeof(req_data));
    }

    if (out_buf) {
        if (out_buf_cap < total_payload) {
            return false;
        }
        memcpy(out_buf, &req_data, sizeof(req_data));
        if (inline_len > 0 && inline_json) {
            memcpy((uint8_t *)out_buf + sizeof(req_data), inline_json, inline_len);
        }
    }

    if (out_total_len) {
        *out_total_len = total_payload;
    }

    return true;
}

bool waywal_ipc_validate_resp_compile_shader_graph(
    const waywal_ipc_resp_compile_shader_graph_t *resp, size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_compile_shader_graph_t)) {
        return false;
    }

    waywal_ipc_resp_compile_shader_graph_t r;
    memcpy(&r, resp, sizeof(r));

    if (!isfinite(r.compile_time_ms)) {
        return false;
    }

    if (!memchr(r.error_msg, '\0', sizeof(r.error_msg))) {
        return false;
    }

    return true;
}

bool waywal_ipc_serialize_resp_compile_shader_graph(
    uint32_t status_code, uint32_t active_pass_mask, uint32_t compiled_node_count,
    uint32_t estimated_alu_ops, float compile_time_ms, uint32_t offending_node_id,
    const char *error_msg, waywal_ipc_hdr_t *out_hdr,
    waywal_ipc_resp_compile_shader_graph_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    waywal_ipc_resp_compile_shader_graph_t resp_data;
    memset(&resp_data, 0, sizeof(resp_data));
    resp_data.status_code = status_code;
    resp_data.active_pass_mask = active_pass_mask;
    resp_data.compiled_node_count = compiled_node_count;
    resp_data.estimated_alu_ops = estimated_alu_ops;
    resp_data.compile_time_ms = isfinite(compile_time_ms) ? compile_time_ms : 0.0f;
    resp_data.offending_node_id = offending_node_id;

    if (error_msg) {
        strncpy(resp_data.error_msg, error_msg, sizeof(resp_data.error_msg) - 1);
    }
    memcpy(out_resp, &resp_data, sizeof(resp_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_COMPILE_SHADER_GRAPH;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_compile_shader_graph_t);

    return true;
}

/* ========================================================================= */
/* WAYWAL_REQ_SET_CAMERA_3D (0x001E) & RESP (0x800D)                         */
/* ========================================================================= */

bool waywal_ipc_validate_req_set_camera_3d(const waywal_ipc_req_set_camera_3d_t *req,
                                           size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_set_camera_3d_t)) {
        return false;
    }

    waywal_ipc_req_set_camera_3d_t r;
    memcpy(&r, req, sizeof(r));

    if (!memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }

    if (r.projection_mode > (uint32_t)WE_PROJ_DEPTH_PARALLAX_25D) {
        return false;
    }

    if (!isfinite(r.fov_y_deg) || !isfinite(r.near_z) || !isfinite(r.far_z) ||
        !isfinite(r.parallax_amount[0]) || !isfinite(r.parallax_amount[1]) ||
        !isfinite(r.parallax_smoothing) || !isfinite(r.dof_focal_dist) ||
        !isfinite(r.dof_focal_region) || !isfinite(r.dof_bokeh_radius_px)) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        if (!isfinite(r.eye_pos[i]) || !isfinite(r.target_pos[i]) || !isfinite(r.up_vec[i])) {
            return false;
        }
    }

    return true;
}

bool waywal_ipc_serialize_req_set_camera_3d(
    const char *target_output, uint32_t projection_mode, uint32_t flags, const float eye_pos[3],
    const float target_pos[3], const float up_vec[3], float fov_y_deg, float near_z, float far_z,
    const float parallax_amount[2], float parallax_smoothing, float dof_focal_dist,
    float dof_focal_region, float dof_bokeh_radius_px, waywal_ipc_hdr_t *out_hdr,
    waywal_ipc_req_set_camera_3d_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }

    waywal_ipc_req_set_camera_3d_t req_data;
    memset(&req_data, 0, sizeof(req_data));
    if (target_output) {
        strncpy(req_data.target_output, target_output, sizeof(req_data.target_output) - 1);
    } else {
        req_data.target_output[0] = '*';
    }

    req_data.projection_mode = projection_mode;
    req_data.flags = flags;

    if (eye_pos) {
        memcpy(req_data.eye_pos, eye_pos, sizeof(req_data.eye_pos));
    }
    if (target_pos) {
        memcpy(req_data.target_pos, target_pos, sizeof(req_data.target_pos));
    }
    if (up_vec) {
        memcpy(req_data.up_vec, up_vec, sizeof(req_data.up_vec));
    } else {
        req_data.up_vec[1] = 1.0f;
    }

    req_data.fov_y_deg = fov_y_deg;
    req_data.near_z = near_z;
    req_data.far_z = far_z;

    if (parallax_amount) {
        memcpy(req_data.parallax_amount, parallax_amount, sizeof(req_data.parallax_amount));
    }
    req_data.parallax_smoothing = parallax_smoothing;
    req_data.dof_focal_dist = dof_focal_dist;
    req_data.dof_focal_region = dof_focal_region;
    req_data.dof_bokeh_radius_px = dof_bokeh_radius_px;
    memcpy(out_req, &req_data, sizeof(req_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_SET_CAMERA_3D;
    out_hdr->payload_size = sizeof(waywal_ipc_req_set_camera_3d_t);

    return true;
}

bool waywal_ipc_validate_resp_set_camera_3d(const waywal_ipc_resp_set_camera_3d_t *resp,
                                            size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_set_camera_3d_t)) {
        return false;
    }

    waywal_ipc_resp_set_camera_3d_t r;
    memcpy(&r, resp, sizeof(r));
    for (int i = 0; i < 16; ++i) {
        if (!isfinite(r.active_view_proj[i])) {
            return false;
        }
    }

    return true;
}

bool waywal_ipc_serialize_resp_set_camera_3d(uint32_t status_code, const float active_view_proj[16],
                                             waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_set_camera_3d_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    waywal_ipc_resp_set_camera_3d_t resp_data;
    memset(&resp_data, 0, sizeof(resp_data));
    resp_data.status_code = status_code;
    if (active_view_proj) {
        for (int i = 0; i < 16; ++i) {
            resp_data.active_view_proj[i] =
                isfinite(active_view_proj[i]) ? active_view_proj[i] : 0.0f;
        }
    }
    memcpy(out_resp, &resp_data, sizeof(resp_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_SET_CAMERA_3D;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_set_camera_3d_t);

    return true;
}

/* ========================================================================= */
/* WAYWAL_REQ_SET_PHYSICS_PARAMS (0x001F) & RESP (0x800E)                     */
/* ========================================================================= */

bool waywal_ipc_validate_req_set_physics_params(const waywal_ipc_req_set_physics_params_t *req,
                                                size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_set_physics_params_t)) {
        return false;
    }

    waywal_ipc_req_set_physics_params_t r;
    memcpy(&r, req, sizeof(r));

    if (!memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }

    if (r.bone_index < -1 || r.bone_index >= (int32_t)WE_MDL_MAX_BONES) {
        return false;
    }

    if (!bone_sim_config_floats_finite(&r.bone_cfg) || !wind_field_floats_finite(&r.wind) ||
        !cursor_force_floats_finite(&r.cursor)) {
        return false;
    }

    return true;
}

bool waywal_ipc_serialize_req_set_physics_params(const char *target_output, int32_t object_id,
                                                 int32_t bone_index,
                                                 const we_bone_sim_config_t *bone_cfg,
                                                 const we_wind_field_t *wind,
                                                 const we_cursor_force_t *cursor,
                                                 waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_req_set_physics_params_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }

    waywal_ipc_req_set_physics_params_t req_data;
    memset(&req_data, 0, sizeof(req_data));
    if (target_output) {
        strncpy(req_data.target_output, target_output, sizeof(req_data.target_output) - 1);
    } else {
        req_data.target_output[0] = '*';
    }

    req_data.object_id = object_id;
    req_data.bone_index = bone_index;

    if (bone_cfg) {
        memcpy(&req_data.bone_cfg, bone_cfg, sizeof(req_data.bone_cfg));
    }
    if (wind) {
        memcpy(&req_data.wind, wind, sizeof(req_data.wind));
    }
    if (cursor) {
        memcpy(&req_data.cursor, cursor, sizeof(req_data.cursor));
    }
    memcpy(out_req, &req_data, sizeof(req_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_SET_PHYSICS_PARAMS;
    out_hdr->payload_size = sizeof(waywal_ipc_req_set_physics_params_t);

    return true;
}

bool waywal_ipc_validate_resp_set_physics_params(const waywal_ipc_resp_set_physics_params_t *resp,
                                                 size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_set_physics_params_t)) {
        return false;
    }

    waywal_ipc_resp_set_physics_params_t r;
    memcpy(&r, resp, sizeof(r));
    return isfinite(r.last_step_time_us);
}

bool waywal_ipc_serialize_resp_set_physics_params(uint32_t status_code, uint32_t active_sim_bones,
                                                  float last_step_time_us,
                                                  waywal_ipc_hdr_t *out_hdr,
                                                  waywal_ipc_resp_set_physics_params_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    waywal_ipc_resp_set_physics_params_t resp_data;
    memset(&resp_data, 0, sizeof(resp_data));
    resp_data.status_code = status_code;
    resp_data.active_sim_bones = active_sim_bones;
    resp_data.last_step_time_us = isfinite(last_step_time_us) ? last_step_time_us : 0.0f;
    memcpy(out_resp, &resp_data, sizeof(resp_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_SET_PHYSICS_PARAMS;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_set_physics_params_t);

    return true;
}

/* ========================================================================= */
/* WAYWAL_REQ_SET_SCENE_LIGHTS (0x0020) & RESP (0x800F)                       */
/* ========================================================================= */

bool waywal_ipc_validate_req_set_scene_lights(const waywal_ipc_req_set_scene_lights_t *req,
                                              size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_set_scene_lights_t)) {
        return false;
    }

    waywal_ipc_req_set_scene_lights_t r;
    memcpy(&r, req, sizeof(r));

    if (!memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }

    if (r.lighting.active_light_count > WE_MAX_SCENE_LIGHTS) {
        return false;
    }

    if (!isfinite(r.lighting.ambient_color_rgb[0]) || !isfinite(r.lighting.ambient_color_rgb[1]) ||
        !isfinite(r.lighting.ambient_color_rgb[2]) || !isfinite(r.lighting.sky_color_rgb[0]) ||
        !isfinite(r.lighting.sky_color_rgb[1]) || !isfinite(r.lighting.sky_color_rgb[2]) ||
        !isfinite(r.lighting.normal_map_strength)) {
        return false;
    }

    for (uint32_t i = 0; i < r.lighting.active_light_count; ++i) {
        if (!scene_light_valid(&r.lighting.lights[i])) {
            return false;
        }
    }

    return true;
}

bool waywal_ipc_serialize_req_set_scene_lights(const char *target_output,
                                               const we_lighting_uniforms_t *lighting,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_set_scene_lights_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }

    waywal_ipc_req_set_scene_lights_t req_data;
    memset(&req_data, 0, sizeof(req_data));
    if (target_output) {
        strncpy(req_data.target_output, target_output, sizeof(req_data.target_output) - 1);
    } else {
        req_data.target_output[0] = '*';
    }

    if (lighting) {
        memcpy(&req_data.lighting, lighting, sizeof(req_data.lighting));
    }
    memcpy(out_req, &req_data, sizeof(req_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_SET_SCENE_LIGHTS;
    out_hdr->payload_size = sizeof(waywal_ipc_req_set_scene_lights_t);

    return true;
}

bool waywal_ipc_validate_resp_set_scene_lights(const waywal_ipc_resp_set_scene_lights_t *resp,
                                               size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_set_scene_lights_t)) {
        return false;
    }

    waywal_ipc_resp_set_scene_lights_t r;
    memcpy(&r, resp, sizeof(r));
    return r.active_lights <= WE_MAX_SCENE_LIGHTS;
}

bool waywal_ipc_serialize_resp_set_scene_lights(uint32_t status_code, uint32_t active_lights,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_set_scene_lights_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    waywal_ipc_resp_set_scene_lights_t resp_data;
    memset(&resp_data, 0, sizeof(resp_data));
    resp_data.status_code = status_code;
    resp_data.active_lights = active_lights;
    memcpy(out_resp, &resp_data, sizeof(resp_data));

    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_SET_SCENE_LIGHTS;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_set_scene_lights_t);

    return true;
}

bool daemon_validate_set_image_header(const waywal_ipc_hdr_t *hdr)
{
    if (!hdr || hdr->opcode != WAYWAL_REQ_SET_IMAGE) {
        return false;
    }
    const size_t min_legacy_prefix = 72u;
    return hdr->payload_size >= min_legacy_prefix;
}

/* ========================================================================= */
/* Authoring IPC Wire Validation & Serialization (0x0021..0x002A)            */
/* ========================================================================= */

static bool ipc_packed_floats_finite(const void *src, size_t count)
{
    const uint8_t *bytes = (const uint8_t *)src;
    for (size_t i = 0; i < count; ++i) {
        float val = 0.0f;
        memcpy(&val, bytes + i * sizeof(float), sizeof(val));
        if (!isfinite(val)) {
            return false;
        }
    }
    return true;
}

static void ipc_copy_bounded_str(char *dst, size_t dst_cap, const char *src)
{
    if (!dst || dst_cap == 0u) {
        return;
    }
    memset(dst, 0, dst_cap);
    if (!src) {
        return;
    }
    size_t len = strnlen(src, dst_cap - 1u);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void ipc_copy_target_output(char *dst, size_t dst_cap, const char *target_output)
{
    if (target_output) {
        ipc_copy_bounded_str(dst, dst_cap, target_output);
    } else {
        ipc_copy_bounded_str(dst, dst_cap, "*");
    }
}

static void ipc_fill_hdr(waywal_ipc_hdr_t *out_hdr, uint16_t opcode, size_t payload_size)
{
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = opcode;
    out_hdr->payload_size = (uint64_t)payload_size;
}

static bool ipc_brush_params_valid(const we_brush_params_t *params)
{
    we_brush_params_t p;
    memcpy(&p, params, sizeof(p));
    if (p.channel > (uint32_t)WE_BRUSH_CHAN_SMUDGE ||
        p.op_mode > (uint32_t)WE_BRUSH_OP_SMUDGE_ADV) {
        return false;
    }
    if (!ipc_packed_floats_finite((const uint8_t *)&p + offsetof(we_brush_params_t, radius_px),
                                  14u)) {
        return false;
    }
    if (p.radius_px < 0.0f || p.radius_px > 4096.0f || p.hardness < 0.0f || p.hardness > 1.0f ||
        p.opacity < 0.0f || p.opacity > 1.0f || p.falloff_power < 0.0f || p.falloff_power > 64.0f ||
        p.blur_radius_px < 0.0f || p.blur_radius_px > 512.0f || p.target_value < 0.0f ||
        p.target_value > 1.0f || p.flow_strength < 0.0f || p.flow_strength > 16.0f) {
        return false;
    }
    return true;
}

static bool ipc_brush_dab_valid(const we_brush_dab_t *dab)
{
    if (!ipc_packed_floats_finite(dab, 11u)) {
        return false;
    }
    we_brush_dab_t d;
    memcpy(&d, dab, sizeof(d));
    if (d.radius_a < 0.0f || d.radius_b < 0.0f || d.radius_a > 4096.0f || d.radius_b > 4096.0f ||
        d.opacity_eff < 0.0f || d.opacity_eff > 1.0f || fabsf(d.ax) > 1e6f || fabsf(d.ay) > 1e6f ||
        fabsf(d.bx) > 1e6f || fabsf(d.by) > 1e6f) {
        return false;
    }
    return true;
}

static bool ipc_particle_ramp_lut_valid(const we_particle_ramp_lut_t *lut)
{
    we_particle_ramp_lut_t l;
    memcpy(&l, lut, sizeof(l));
    if (l.channel_kind > 2u || l.knot_count < 2u || l.knot_count > WE_PARTICLE_MAX_KNOTS) {
        return false;
    }
    return ipc_packed_floats_finite((const uint8_t *)&l +
                                        offsetof(we_particle_ramp_lut_t, scalar_lut),
                                    (size_t)WE_PARTICLE_RAMP_SAMPLES * 4u);
}

static bool ipc_audio_binding_valid(const we_audio_binding_t *binding)
{
    we_audio_binding_t b;
    memcpy(&b, binding, sizeof(b));
    if (b.target_kind > (uint32_t)WE_AUDIO_TGT_PARTICLE_BURST_TRIG || b.target_component > 3u ||
        b.src_kind > (uint32_t)WE_AUDIO_SRC_BEAT_PULSE ||
        b.agg_mode > (uint32_t)WE_AUDIO_AGG_PEAK || b.band_end >= 64u ||
        b.band_start > b.band_end || b.curve_shape > (uint32_t)WE_AUDIO_CURVE_TRIG_EDGE ||
        b.blend_mode > (uint32_t)WE_AUDIO_BLEND_REPLACE) {
        return false;
    }
    if (!memchr(b.uniform_name, '\0', sizeof(b.uniform_name))) {
        return false;
    }
    if (!isfinite(b.input_gain) || !isfinite(b.threshold_open) || !isfinite(b.hysteresis) ||
        !isfinite(b.attack_sec) || !isfinite(b.release_sec) || !isfinite(b.out_min) ||
        !isfinite(b.out_max) || !isfinite(b.base_value) || !isfinite(b.env_state) ||
        !isfinite(b.last_output) || !isfinite(b.cooldown_rem_sec)) {
        return false;
    }
    if (b.input_gain < 0.0f || b.input_gain > 100.0f || b.threshold_open < 0.0f ||
        b.threshold_open > 1.0f || b.hysteresis < 0.0f || b.hysteresis > 1.0f ||
        b.attack_sec < 0.0f || b.attack_sec > 60.0f || b.release_sec < 0.0f ||
        b.release_sec > 60.0f || b.cooldown_rem_sec < 0.0f || b.cooldown_rem_sec > 60.0f ||
        fabsf(b.out_min) > 1e6f || fabsf(b.out_max) > 1e6f || fabsf(b.base_value) > 1e6f ||
        fabsf(b.env_state) > 1e6f || fabsf(b.last_output) > 1e6f) {
        return false;
    }
    return true;
}

static bool ipc_serialize_var_msg(uint16_t opcode, const void *fixed_hdr, size_t fixed_size,
                                  const void *tail, size_t tail_size, waywal_ipc_hdr_t *out_hdr,
                                  void *out_struct, void *out_buf, size_t out_buf_cap,
                                  size_t *out_total_len)
{
    if (!out_hdr || !fixed_hdr) {
        return false;
    }
    if (tail_size > 0u && !tail) {
        return false;
    }
    size_t total_payload = fixed_size + tail_size;
    if (out_buf && out_buf_cap < total_payload) {
        return false;
    }

    ipc_fill_hdr(out_hdr, opcode, total_payload);
    if (out_struct) {
        memcpy(out_struct, fixed_hdr, fixed_size);
    }
    if (out_buf) {
        memcpy(out_buf, fixed_hdr, fixed_size);
        if (tail_size > 0u) {
            memcpy((uint8_t *)out_buf + fixed_size, tail, tail_size);
        }
    }
    if (out_total_len) {
        *out_total_len = total_payload;
    }
    return true;
}

bool waywal_ipc_validate_req_scene_mutate(const waywal_ipc_req_scene_mutate_t *req,
                                          size_t payload_size, const char **out_payload_json)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_scene_mutate_t)) {
        return false;
    }
    waywal_ipc_req_scene_mutate_t r;
    memcpy(&r, req, sizeof(r));

    if (r.payload_len > WAYWAL_IPC_MAX_MUTATE_PAYLOAD_BYTES ||
        payload_size != sizeof(r) + (size_t)r.payload_len) {
        return false;
    }
    if (r.mutate_op > (uint32_t)WE_MUTATE_SET_SOUND_PROPS ||
        r.node_kind > (uint32_t)WE_NODE_CAMERA3D) {
        return false;
    }
    if (!memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.name, '\0', sizeof(r.name))) {
        return false;
    }
    if (!ipc_packed_floats_finite(
            (const uint8_t *)&r + offsetof(waywal_ipc_req_scene_mutate_t, origin), 17u)) {
        return false;
    }

    const char *json_ptr = NULL;
    if (r.payload_len > 0u) {
        json_ptr = (const char *)req + sizeof(r);
        if (memchr(json_ptr, '\0', r.payload_len) != NULL) {
            return false;
        }
    }
    if (out_payload_json) {
        *out_payload_json = json_ptr;
    }
    return true;
}

bool waywal_ipc_serialize_req_scene_mutate(const waywal_ipc_req_scene_mutate_t *params,
                                           const char *payload_json, waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_req_scene_mutate_t *out_req, void *out_buf,
                                           size_t out_buf_cap, size_t *out_total_len)
{
    if (!params || !out_hdr) {
        return false;
    }
    size_t tail_len = payload_json ? strlen(payload_json) : 0u;
    if (tail_len > WAYWAL_IPC_MAX_MUTATE_PAYLOAD_BYTES) {
        return false;
    }
    waywal_ipc_req_scene_mutate_t r;
    memcpy(&r, params, sizeof(r));
    r.payload_len = (uint32_t)tail_len;
    r._pad = 0u;
    if (!memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.name, '\0', sizeof(r.name)) ||
        r.mutate_op > (uint32_t)WE_MUTATE_SET_SOUND_PROPS ||
        r.node_kind > (uint32_t)WE_NODE_CAMERA3D ||
        !ipc_packed_floats_finite(
            (const uint8_t *)&r + offsetof(waywal_ipc_req_scene_mutate_t, origin), 17u)) {
        return false;
    }
    return ipc_serialize_var_msg(WAYWAL_REQ_SCENE_MUTATE, &r, sizeof(r), payload_json, tail_len,
                                 out_hdr, out_req, out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_scene_mutate(const waywal_ipc_resp_scene_mutate_t *resp,
                                           size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_scene_mutate_t)) {
        return false;
    }
    waywal_ipc_resp_scene_mutate_t r;
    memcpy(&r, resp, sizeof(r));
    return memchr(r.error_msg, '\0', sizeof(r.error_msg)) != NULL;
}

bool waywal_ipc_serialize_resp_scene_mutate(uint32_t status_code, int32_t object_id,
                                            uint32_t scene_revision, uint32_t object_count,
                                            const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_resp_scene_mutate_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_SCENE_MUTATE, sizeof(*out_resp));
    waywal_ipc_resp_scene_mutate_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.object_id = object_id;
    r.scene_revision = scene_revision;
    r.object_count = object_count;
    ipc_copy_bounded_str(r.error_msg, sizeof(r.error_msg), error_msg);
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_hot_reload_code(const waywal_ipc_req_hot_reload_code_t *req,
                                             size_t payload_size, const char **out_source)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_hot_reload_code_t)) {
        return false;
    }
    waywal_ipc_req_hot_reload_code_t r;
    memcpy(&r, req, sizeof(r));
    if (r.code_kind > 2u || r.source_len > WAYWAL_IPC_MAX_CODE_BYTES ||
        payload_size != sizeof(r) + (size_t)r.source_len) {
        return false;
    }
    if (!memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.uniform_name, '\0', sizeof(r.uniform_name))) {
        return false;
    }
    const char *src_ptr = NULL;
    if (r.source_len > 0u) {
        src_ptr = (const char *)req + sizeof(r);
        if (memchr(src_ptr, '\0', r.source_len) != NULL) {
            return false;
        }
    }
    if (out_source) {
        *out_source = src_ptr;
    }
    return true;
}

bool waywal_ipc_serialize_req_hot_reload_code(
    const char *target_output, uint32_t code_kind, uint32_t flags, int32_t object_id,
    uint32_t pass_index, uint32_t script_slot, const char *uniform_name, const char *source_utf8,
    size_t source_len, waywal_ipc_hdr_t *out_hdr, waywal_ipc_req_hot_reload_code_t *out_req,
    void *out_buf, size_t out_buf_cap, size_t *out_total_len)
{
    if (!out_hdr || code_kind > 2u || source_len > WAYWAL_IPC_MAX_CODE_BYTES) {
        return false;
    }
    if (source_len > 0u && (!source_utf8 || memchr(source_utf8, '\0', source_len) != NULL)) {
        return false;
    }
    waywal_ipc_req_hot_reload_code_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.code_kind = code_kind;
    r.flags = flags;
    r.object_id = object_id;
    r.pass_index = pass_index;
    r.script_slot = script_slot;
    r.source_len = (uint32_t)source_len;
    ipc_copy_bounded_str(r.uniform_name, sizeof(r.uniform_name), uniform_name);
    return ipc_serialize_var_msg(WAYWAL_REQ_HOT_RELOAD_CODE, &r, sizeof(r), source_utf8, source_len,
                                 out_hdr, out_req, out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_hot_reload_code(const waywal_ipc_resp_hot_reload_code_t *resp,
                                              size_t payload_size, const char **out_meta_json)
{
    if (!resp || payload_size < sizeof(waywal_ipc_resp_hot_reload_code_t)) {
        return false;
    }
    waywal_ipc_resp_hot_reload_code_t r;
    memcpy(&r, resp, sizeof(r));
    if (r.meta_json_len > WAYWAL_IPC_MAX_META_JSON_BYTES ||
        payload_size != sizeof(r) + (size_t)r.meta_json_len) {
        return false;
    }
    if (!memchr(r.error_msg, '\0', sizeof(r.error_msg))) {
        return false;
    }
    const char *meta_ptr = NULL;
    if (r.meta_json_len > 0u) {
        meta_ptr = (const char *)resp + sizeof(r);
        if (memchr(meta_ptr, '\0', r.meta_json_len) != NULL) {
            return false;
        }
    }
    if (out_meta_json) {
        *out_meta_json = meta_ptr;
    }
    return true;
}

bool waywal_ipc_serialize_resp_hot_reload_code(
    uint32_t status_code, uint32_t error_line, uint32_t error_col, uint32_t uniform_count,
    uint32_t combo_count, uint32_t compile_time_us, const char *error_msg, const char *meta_json,
    size_t meta_json_len, waywal_ipc_hdr_t *out_hdr, waywal_ipc_resp_hot_reload_code_t *out_resp,
    void *out_buf, size_t out_buf_cap, size_t *out_total_len)
{
    if (!out_hdr || meta_json_len > WAYWAL_IPC_MAX_META_JSON_BYTES) {
        return false;
    }
    if (meta_json_len > 0u && (!meta_json || memchr(meta_json, '\0', meta_json_len) != NULL)) {
        return false;
    }
    waywal_ipc_resp_hot_reload_code_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.error_line = error_line;
    r.error_col = error_col;
    r.uniform_count = uniform_count;
    r.combo_count = combo_count;
    r.compile_time_us = compile_time_us;
    r.meta_json_len = (uint32_t)meta_json_len;
    ipc_copy_bounded_str(r.error_msg, sizeof(r.error_msg), error_msg);
    return ipc_serialize_var_msg(WAYWAL_RESP_HOT_RELOAD_CODE, &r, sizeof(r), meta_json,
                                 meta_json_len, out_hdr, out_resp, out_buf, out_buf_cap,
                                 out_total_len);
}

bool waywal_ipc_validate_req_query_scene_tree(const waywal_ipc_req_query_scene_tree_t *req,
                                              size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_query_scene_tree_t)) {
        return false;
    }
    waywal_ipc_req_query_scene_tree_t r;
    memcpy(&r, req, sizeof(r));
    return memchr(r.target_output, '\0', sizeof(r.target_output)) != NULL;
}

bool waywal_ipc_serialize_req_query_scene_tree(const char *target_output, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_query_scene_tree_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_REQ_QUERY_SCENE_TREE, sizeof(*out_req));
    waywal_ipc_req_query_scene_tree_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.flags = flags;
    memcpy(out_req, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_resp_query_scene_tree(const waywal_ipc_resp_query_scene_tree_t *resp,
                                               size_t payload_size, const char **out_json)
{
    if (!resp || payload_size < sizeof(waywal_ipc_resp_query_scene_tree_t)) {
        return false;
    }
    waywal_ipc_resp_query_scene_tree_t r;
    memcpy(&r, resp, sizeof(r));
    if (r.json_len > WAYWAL_IPC_MAX_SCENE_JSON || payload_size != sizeof(r) + (size_t)r.json_len) {
        return false;
    }
    const char *json_ptr = NULL;
    if (r.json_len > 0u) {
        json_ptr = (const char *)resp + sizeof(r);
        if (memchr(json_ptr, '\0', r.json_len) != NULL) {
            return false;
        }
    }
    if (out_json) {
        *out_json = json_ptr;
    }
    return true;
}

bool waywal_ipc_serialize_resp_query_scene_tree(uint32_t status_code, uint32_t scene_revision,
                                                uint32_t object_count, const char *json,
                                                size_t json_len, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_query_scene_tree_t *out_resp,
                                                void *out_buf, size_t out_buf_cap,
                                                size_t *out_total_len)
{
    if (!out_hdr || json_len > WAYWAL_IPC_MAX_SCENE_JSON) {
        return false;
    }
    if (json_len > 0u && (!json || memchr(json, '\0', json_len) != NULL)) {
        return false;
    }
    waywal_ipc_resp_query_scene_tree_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.scene_revision = scene_revision;
    r.object_count = object_count;
    r.json_len = (uint32_t)json_len;
    return ipc_serialize_var_msg(WAYWAL_RESP_QUERY_SCENE_TREE, &r, sizeof(r), json, json_len,
                                 out_hdr, out_resp, out_buf, out_buf_cap, out_total_len);
}

static bool ipc_combo_name_valid(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    unsigned char c0 = (unsigned char)name[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) {
        return false;
    }
    for (size_t i = 1u; name[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

bool waywal_ipc_validate_req_set_shader_combo(const waywal_ipc_req_set_shader_combo_t *req,
                                              size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_set_shader_combo_t)) {
        return false;
    }
    waywal_ipc_req_set_shader_combo_t r;
    memcpy(&r, req, sizeof(r));
    if (!memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.combo_name, '\0', sizeof(r.combo_name))) {
        return false;
    }
    return ipc_combo_name_valid(r.combo_name);
}

bool waywal_ipc_serialize_req_set_shader_combo(const char *target_output, int32_t object_id,
                                               uint32_t pass_index, const char *combo_name,
                                               int32_t combo_value, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_set_shader_combo_t *out_req)
{
    if (!out_hdr || !out_req || !ipc_combo_name_valid(combo_name)) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_REQ_SET_SHADER_COMBO, sizeof(*out_req));
    waywal_ipc_req_set_shader_combo_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.object_id = object_id;
    r.pass_index = pass_index;
    ipc_copy_bounded_str(r.combo_name, sizeof(r.combo_name), combo_name);
    r.combo_value = combo_value;
    r.flags = flags;
    memcpy(out_req, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_resp_set_shader_combo(const waywal_ipc_resp_set_shader_combo_t *resp,
                                               size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_set_shader_combo_t)) {
        return false;
    }
    waywal_ipc_resp_set_shader_combo_t r;
    memcpy(&r, resp, sizeof(r));
    return memchr(r.error_msg, '\0', sizeof(r.error_msg)) != NULL;
}

bool waywal_ipc_serialize_resp_set_shader_combo(uint32_t status_code, uint32_t recompile_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_set_shader_combo_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_SET_SHADER_COMBO, sizeof(*out_resp));
    waywal_ipc_resp_set_shader_combo_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.recompile_time_us = recompile_time_us;
    ipc_copy_bounded_str(r.error_msg, sizeof(r.error_msg), error_msg);
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_brush_stroke(const waywal_ipc_req_brush_stroke_t *req,
                                          size_t payload_size, const we_brush_dab_t **out_dabs,
                                          uint32_t *out_dab_count)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_brush_stroke_t)) {
        return false;
    }
    waywal_ipc_req_brush_stroke_t r;
    memcpy(&r, req, sizeof(r));
    if (r.action > (uint32_t)WE_BRUSH_IPC_REDO_STROKE || r.dab_count > WE_BRUSH_MAX_STROKE_DABS ||
        !memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !ipc_brush_params_valid(&r.params)) {
        return false;
    }
    size_t expected = sizeof(r) + (size_t)r.dab_count * sizeof(we_brush_dab_t);
    if (payload_size != expected) {
        return false;
    }
    const we_brush_dab_t *dabs_ptr = NULL;
    if (r.dab_count > 0u) {
        const uint8_t *tail = (const uint8_t *)req + sizeof(r);
        for (uint32_t i = 0; i < r.dab_count; ++i) {
            we_brush_dab_t dab;
            memcpy(&dab, tail + (size_t)i * sizeof(we_brush_dab_t), sizeof(dab));
            if (!ipc_brush_dab_valid(&dab)) {
                return false;
            }
        }
        dabs_ptr = (const we_brush_dab_t *)(const void *)tail;
    }
    if (out_dabs) {
        *out_dabs = dabs_ptr;
    }
    if (out_dab_count) {
        *out_dab_count = r.dab_count;
    }
    return true;
}

bool waywal_ipc_serialize_req_brush_stroke(const char *target_output, uint32_t action,
                                           int32_t object_id, uint32_t pass_index,
                                           uint32_t tex_slot, uint64_t stroke_id,
                                           const we_brush_params_t *params,
                                           const we_brush_dab_t *dabs, uint32_t dab_count,
                                           waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_req_brush_stroke_t *out_req, void *out_buf,
                                           size_t out_buf_cap, size_t *out_total_len)
{
    if (!out_hdr || action > (uint32_t)WE_BRUSH_IPC_REDO_STROKE ||
        dab_count > WE_BRUSH_MAX_STROKE_DABS) {
        return false;
    }
    if (dab_count > 0u && !dabs) {
        return false;
    }
    waywal_ipc_req_brush_stroke_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.action = action;
    r.object_id = object_id;
    r.pass_index = pass_index;
    r.tex_slot = tex_slot;
    r.stroke_id = stroke_id;
    r.dab_count = dab_count;
    if (params) {
        memcpy(&r.params, params, sizeof(r.params));
        if (!ipc_brush_params_valid(&r.params)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < dab_count; ++i) {
        if (!ipc_brush_dab_valid(&dabs[i])) {
            return false;
        }
    }
    return ipc_serialize_var_msg(WAYWAL_REQ_BRUSH_STROKE, &r, sizeof(r), dabs,
                                 (size_t)dab_count * sizeof(we_brush_dab_t), out_hdr, out_req,
                                 out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_brush_stroke(const waywal_ipc_resp_brush_stroke_t *resp,
                                           size_t payload_size)
{
    return resp != NULL && payload_size == sizeof(waywal_ipc_resp_brush_stroke_t);
}

bool waywal_ipc_serialize_resp_brush_stroke(uint32_t status_code, uint64_t stroke_id,
                                            const we_brush_dirty_rect_t *dirty_rect,
                                            const we_brush_stats_t *stats,
                                            uint32_t captured_tile_count, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_resp_brush_stroke_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_BRUSH_STROKE, sizeof(*out_resp));
    waywal_ipc_resp_brush_stroke_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.stroke_id = stroke_id;
    if (dirty_rect) {
        memcpy(&r.dirty_rect, dirty_rect, sizeof(r.dirty_rect));
    }
    if (stats) {
        memcpy(&r.stats, stats, sizeof(r.stats));
    }
    r.captured_tile_count = captured_tile_count;
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_particle_config(const waywal_ipc_req_particle_config_t *req,
                                             size_t payload_size, const void **out_payload,
                                             uint32_t *out_payload_len)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_particle_config_t)) {
        return false;
    }
    waywal_ipc_req_particle_config_t r;
    memcpy(&r, req, sizeof(r));
    if (r.config_op > (uint32_t)WE_PARTICLE_CFG_PAUSE_TOGGLE ||
        !memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }

    switch (r.config_op) {
    case WE_PARTICLE_CFG_APPLY_PRESET:
        if (r.preset_id >= WE_PARTICLE_PRESET_COUNT || r.payload_len != 0u) {
            return false;
        }
        break;
    case WE_PARTICLE_CFG_SET_SYSTEM_JSON:
        if (r.payload_len == 0u || r.payload_len > WAYWAL_IPC_MAX_SCENE_JSON) {
            return false;
        }
        break;
    case WE_PARTICLE_CFG_SET_RAMP_LUT:
        if (r.payload_len != sizeof(we_particle_ramp_lut_t)) {
            return false;
        }
        break;
    case WE_PARTICLE_CFG_TRIGGER_BURST:
    case WE_PARTICLE_CFG_PAUSE_TOGGLE:
        if (r.payload_len != 0u) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (payload_size != sizeof(r) + (size_t)r.payload_len) {
        return false;
    }

    const void *tail = NULL;
    if (r.payload_len > 0u) {
        tail = (const uint8_t *)req + sizeof(r);
        if (r.config_op == WE_PARTICLE_CFG_SET_SYSTEM_JSON) {
            if (memchr(tail, '\0', r.payload_len) != NULL) {
                return false;
            }
        } else if (r.config_op == WE_PARTICLE_CFG_SET_RAMP_LUT) {
            if (!ipc_particle_ramp_lut_valid((const we_particle_ramp_lut_t *)tail)) {
                return false;
            }
        }
    }

    if (out_payload) {
        *out_payload = tail;
    }
    if (out_payload_len) {
        *out_payload_len = r.payload_len;
    }
    return true;
}

bool waywal_ipc_serialize_req_particle_config(const char *target_output, int32_t object_id,
                                              uint32_t config_op, uint32_t preset_id,
                                              uint32_t flags, const void *payload,
                                              uint32_t payload_len, waywal_ipc_hdr_t *out_hdr,
                                              waywal_ipc_req_particle_config_t *out_req,
                                              void *out_buf, size_t out_buf_cap,
                                              size_t *out_total_len)
{
    if (!out_hdr || config_op > (uint32_t)WE_PARTICLE_CFG_PAUSE_TOGGLE) {
        return false;
    }
    if (config_op == WE_PARTICLE_CFG_APPLY_PRESET &&
        (preset_id >= WE_PARTICLE_PRESET_COUNT || payload_len != 0u)) {
        return false;
    }
    if (config_op == WE_PARTICLE_CFG_SET_SYSTEM_JSON &&
        (payload_len == 0u || payload_len > WAYWAL_IPC_MAX_SCENE_JSON || !payload ||
         memchr(payload, '\0', payload_len) != NULL)) {
        return false;
    }
    if (config_op == WE_PARTICLE_CFG_SET_RAMP_LUT &&
        (payload_len != sizeof(we_particle_ramp_lut_t) || !payload ||
         !ipc_particle_ramp_lut_valid((const we_particle_ramp_lut_t *)payload))) {
        return false;
    }
    if ((config_op == WE_PARTICLE_CFG_TRIGGER_BURST || config_op == WE_PARTICLE_CFG_PAUSE_TOGGLE) &&
        payload_len != 0u) {
        return false;
    }

    waywal_ipc_req_particle_config_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.object_id = object_id;
    r.config_op = config_op;
    r.preset_id = preset_id;
    r.flags = flags;
    r.payload_len = payload_len;
    return ipc_serialize_var_msg(WAYWAL_REQ_PARTICLE_CONFIG, &r, sizeof(r), payload, payload_len,
                                 out_hdr, out_req, out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_particle_config(const waywal_ipc_resp_particle_config_t *resp,
                                              size_t payload_size)
{
    return resp != NULL && payload_size == sizeof(waywal_ipc_resp_particle_config_t);
}

bool waywal_ipc_serialize_resp_particle_config(uint32_t status_code, int32_t object_id,
                                               uint32_t alive_count, uint32_t max_count,
                                               uint32_t sim_time_us, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_resp_particle_config_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_PARTICLE_CONFIG, sizeof(*out_resp));
    waywal_ipc_resp_particle_config_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.object_id = object_id;
    r.alive_count = alive_count;
    r.max_count = max_count;
    r.sim_time_us = sim_time_us;
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_set_audio_bindings(const waywal_ipc_req_set_audio_bindings_t *req,
                                                size_t payload_size,
                                                const we_audio_binding_t **out_bindings,
                                                uint32_t *out_count)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_set_audio_bindings_t)) {
        return false;
    }
    waywal_ipc_req_set_audio_bindings_t r;
    memcpy(&r, req, sizeof(r));
    if (r.bind_op > (uint32_t)WE_AUDIO_BIND_OP_CLEAR || r.binding_count > WE_MAX_AUDIO_BINDINGS ||
        !memchr(r.target_output, '\0', sizeof(r.target_output))) {
        return false;
    }
    if ((r.bind_op == WE_AUDIO_BIND_OP_CLEAR || r.bind_op == WE_AUDIO_BIND_OP_REMOVE) &&
        r.binding_count != 0u) {
        return false;
    }
    size_t expected = sizeof(r) + (size_t)r.binding_count * sizeof(we_audio_binding_t);
    if (payload_size != expected) {
        return false;
    }

    const we_audio_binding_t *arr = NULL;
    if (r.binding_count > 0u) {
        const uint8_t *tail = (const uint8_t *)req + sizeof(r);
        for (uint32_t i = 0; i < r.binding_count; ++i) {
            if (!ipc_audio_binding_valid(
                    (const we_audio_binding_t *)(const void *)(tail +
                                                               (size_t)i *
                                                                   sizeof(we_audio_binding_t)))) {
                return false;
            }
        }
        arr = (const we_audio_binding_t *)(const void *)tail;
    }
    if (out_bindings) {
        *out_bindings = arr;
    }
    if (out_count) {
        *out_count = r.binding_count;
    }
    return true;
}

bool waywal_ipc_serialize_req_set_audio_bindings(const char *target_output, uint32_t bind_op,
                                                 uint32_t remove_id,
                                                 const we_audio_binding_t *bindings,
                                                 uint32_t binding_count, waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_req_set_audio_bindings_t *out_req,
                                                 void *out_buf, size_t out_buf_cap,
                                                 size_t *out_total_len)
{
    if (!out_hdr || bind_op > (uint32_t)WE_AUDIO_BIND_OP_CLEAR ||
        binding_count > WE_MAX_AUDIO_BINDINGS) {
        return false;
    }
    if ((bind_op == WE_AUDIO_BIND_OP_CLEAR || bind_op == WE_AUDIO_BIND_OP_REMOVE) &&
        binding_count != 0u) {
        return false;
    }
    if (binding_count > 0u && !bindings) {
        return false;
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (!ipc_audio_binding_valid(&bindings[i])) {
            return false;
        }
    }

    waywal_ipc_req_set_audio_bindings_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.bind_op = bind_op;
    r.binding_count = binding_count;
    r.remove_id = remove_id;
    return ipc_serialize_var_msg(WAYWAL_REQ_SET_AUDIO_BINDINGS, &r, sizeof(r), bindings,
                                 (size_t)binding_count * sizeof(we_audio_binding_t), out_hdr,
                                 out_req, out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_set_audio_bindings(const waywal_ipc_resp_set_audio_bindings_t *resp,
                                                 size_t payload_size)
{
    return resp != NULL && payload_size == sizeof(waywal_ipc_resp_set_audio_bindings_t);
}

bool waywal_ipc_serialize_resp_set_audio_bindings(uint32_t status_code,
                                                  uint32_t active_binding_count,
                                                  uint32_t eval_time_ns, waywal_ipc_hdr_t *out_hdr,
                                                  waywal_ipc_resp_set_audio_bindings_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_SET_AUDIO_BINDINGS, sizeof(*out_resp));
    waywal_ipc_resp_set_audio_bindings_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.active_binding_count = active_binding_count;
    r.eval_time_ns = eval_time_ns;
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

static bool ipc_path_components_safe(const char *path, char *out_clean, size_t clean_cap)
{
    if (!path || path[0] != '/') {
        return false;
    }
    size_t len = strnlen(path, clean_cap);
    if (len <= 1u || len >= clean_cap) {
        return false;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 0x20u || ch == 0x7Fu || ch == '\\' || ch == ':') {
            return false;
        }
    }
    while (len > 1u && path[len - 1u] == '/') {
        --len;
    }
    if (len <= 1u) {
        return false;
    }
    memcpy(out_clean, path, len);
    out_clean[len] = '\0';

    const char *p = out_clean + 1u;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);
        if (comp_len == 0u || (comp_len == 1u && p[0] == '.') ||
            (comp_len == 2u && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1u;
    }
    return true;
}

static bool ipc_canonicalize_candidate(const char *clean, char *out_resolved, size_t out_cap)
{
    char probe[PATH_MAX];
    size_t len = strlen(clean);
    if (len >= sizeof(probe)) {
        return false;
    }
    memcpy(probe, clean, len + 1u);

    for (;;) {
        struct stat st;
        if (lstat(probe, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                return false;
            }
            const char *suffix = clean + strlen(probe);
            if (*suffix != '\0' && !S_ISDIR(st.st_mode)) {
                return false;
            }
            if (*suffix == '\0' && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
                return false;
            }
            char resolved_anc[PATH_MAX];
            if (!realpath(probe, resolved_anc)) {
                return false;
            }
            /* Reject symlinks on any existing intermediate path component as well. */
            char check[PATH_MAX];
            memcpy(check, probe, strlen(probe) + 1u);
            while (strlen(check) > 1u) {
                struct stat cst;
                if (lstat(check, &cst) == 0 && S_ISLNK(cst.st_mode)) {
                    return false;
                }
                char *sep = strrchr(check, '/');
                if (!sep || sep == check) {
                    break;
                }
                *sep = '\0';
            }
            int w = snprintf(out_resolved, out_cap, "%s%s", resolved_anc, suffix);
            return w > 0 && (size_t)w < out_cap;
        }
        if (errno != ENOENT) {
            return false;
        }
        char *sep = strrchr(probe, '/');
        if (!sep) {
            return false;
        }
        if (sep == probe) {
            probe[1] = '\0';
        } else {
            *sep = '\0';
        }
    }
}

static bool ipc_path_within_root(const char *candidate, const char *root, bool require_subpath)
{
    size_t root_len = strlen(root);
    if (strncmp(candidate, root, root_len) != 0) {
        return false;
    }
    if (candidate[root_len] == '\0') {
        return !require_subpath;
    }
    return candidate[root_len] == '/';
}

static bool ipc_build_cache_export_root(char *out_root, size_t out_cap)
{
    char raw_root[PATH_MAX];
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] == '/') {
        size_t xlen = strlen(xdg);
        while (xlen > 1u && xdg[xlen - 1u] == '/') {
            --xlen;
        }
        int n = snprintf(raw_root, sizeof(raw_root), "%.*s/wwal/exports", (int)xlen, xdg);
        if (n <= 0 || (size_t)n >= sizeof(raw_root)) {
            return false;
        }
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            return false;
        }
        size_t hlen = strlen(home);
        while (hlen > 1u && home[hlen - 1u] == '/') {
            --hlen;
        }
        int n = snprintf(raw_root, sizeof(raw_root), "%.*s/.cache/wwal/exports", (int)hlen, home);
        if (n <= 0 || (size_t)n >= sizeof(raw_root)) {
            return false;
        }
    }
    char clean_root[PATH_MAX];
    if (!ipc_path_components_safe(raw_root, clean_root, sizeof(clean_root))) {
        return false;
    }
    return ipc_canonicalize_candidate(clean_root, out_root, out_cap);
}

bool waywal_ipc_export_path_is_allowed(const char *path, char *out_resolved, size_t out_cap)
{
    char clean[PATH_MAX];
    if (!ipc_path_components_safe(path, clean, sizeof(clean))) {
        return false;
    }
    char resolved[PATH_MAX];
    if (!ipc_canonicalize_candidate(clean, resolved, sizeof(resolved))) {
        return false;
    }
    bool allowed = false;
    char cache_root[PATH_MAX];
    if (ipc_build_cache_export_root(cache_root, sizeof(cache_root)) &&
        ipc_path_within_root(resolved, cache_root, true)) {
        allowed = true;
    }
    if (!allowed) {
        char tmp_resolved[PATH_MAX];
        const char *tmp_base = realpath("/tmp", tmp_resolved) ? tmp_resolved : "/tmp";
        size_t tlen = strlen(tmp_base);
        static const char k_prefix[] = "/wwal-export-";
        size_t plen = sizeof(k_prefix) - 1u;
        if (strncmp(resolved, tmp_base, tlen) == 0 &&
            strncmp(resolved + tlen, k_prefix, plen) == 0 && resolved[tlen + plen] != '\0' &&
            resolved[tlen + plen] != '/') {
            allowed = true;
        }
    }
    if (!allowed) {
        return false;
    }
    if (out_resolved && out_cap > 0u) {
        size_t rlen = strlen(resolved);
        if (rlen >= out_cap) {
            return false;
        }
        memcpy(out_resolved, resolved, rlen + 1u);
    }
    return true;
}

bool waywal_ipc_validate_req_export_scene_pkg(const waywal_ipc_req_export_scene_pkg_t *req,
                                              size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_export_scene_pkg_t)) {
        return false;
    }
    waywal_ipc_req_export_scene_pkg_t r;
    memcpy(&r, req, sizeof(r));
    if (!memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.dest_dir_or_pkg, '\0', sizeof(r.dest_dir_or_pkg))) {
        return false;
    }
    if ((r.flags & WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE) != 0u &&
        (r.flags & WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG) == 0u) {
        return false;
    }
    return waywal_ipc_export_path_is_allowed(r.dest_dir_or_pkg, NULL, 0u);
}

bool waywal_ipc_serialize_req_export_scene_pkg(const char *target_output,
                                               const char *dest_dir_or_pkg, uint32_t flags,
                                               waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_export_scene_pkg_t *out_req)
{
    if (!out_hdr || !out_req || !dest_dir_or_pkg || dest_dir_or_pkg[0] == '\0') {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_REQ_EXPORT_SCENE_PKG, sizeof(*out_req));
    waywal_ipc_req_export_scene_pkg_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    ipc_copy_bounded_str(r.dest_dir_or_pkg, sizeof(r.dest_dir_or_pkg), dest_dir_or_pkg);
    r.flags = flags;
    memcpy(out_req, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_resp_export_scene_pkg(const waywal_ipc_resp_export_scene_pkg_t *resp,
                                               size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_export_scene_pkg_t)) {
        return false;
    }
    waywal_ipc_resp_export_scene_pkg_t r;
    memcpy(&r, resp, sizeof(r));
    return memchr(r.error_msg, '\0', sizeof(r.error_msg)) != NULL;
}

bool waywal_ipc_serialize_resp_export_scene_pkg(uint32_t status_code, uint32_t entry_count,
                                                uint64_t total_bytes, uint64_t export_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_export_scene_pkg_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_EXPORT_SCENE_PKG, sizeof(*out_resp));
    waywal_ipc_resp_export_scene_pkg_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.entry_count = entry_count;
    r.total_bytes = total_bytes;
    r.export_time_us = export_time_us;
    ipc_copy_bounded_str(r.error_msg, sizeof(r.error_msg), error_msg);
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_lint_scenescript(const waywal_ipc_req_lint_scenescript_t *req,
                                              size_t payload_size, const char **out_source)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_lint_scenescript_t)) {
        return false;
    }
    waywal_ipc_req_lint_scenescript_t r;
    memcpy(&r, req, sizeof(r));
    if (r.source_len > WAYWAL_IPC_MAX_CODE_BYTES ||
        payload_size != sizeof(r) + (size_t)r.source_len) {
        return false;
    }
    const char *src_ptr = NULL;
    if (r.source_len > 0u) {
        src_ptr = (const char *)req + sizeof(r);
        if (memchr(src_ptr, '\0', r.source_len) != NULL) {
            return false;
        }
    }
    if (out_source) {
        *out_source = src_ptr;
    }
    return true;
}

bool waywal_ipc_serialize_req_lint_scenescript(const char *source_utf8, size_t source_len,
                                               uint32_t flags, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_lint_scenescript_t *out_req,
                                               void *out_buf, size_t out_buf_cap,
                                               size_t *out_total_len)
{
    if (!out_hdr || source_len > WAYWAL_IPC_MAX_CODE_BYTES) {
        return false;
    }
    if (source_len > 0u && (!source_utf8 || memchr(source_utf8, '\0', source_len) != NULL)) {
        return false;
    }
    waywal_ipc_req_lint_scenescript_t r;
    memset(&r, 0, sizeof(r));
    r.source_len = (uint32_t)source_len;
    r.flags = flags;
    return ipc_serialize_var_msg(WAYWAL_REQ_LINT_SCENESCRIPT, &r, sizeof(r), source_utf8,
                                 source_len, out_hdr, out_req, out_buf, out_buf_cap, out_total_len);
}

bool waywal_ipc_validate_resp_lint_scenescript(const waywal_ipc_resp_lint_scenescript_t *resp,
                                               size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_lint_scenescript_t)) {
        return false;
    }
    waywal_ipc_resp_lint_scenescript_t r;
    memcpy(&r, resp, sizeof(r));
    return memchr(r.error_msg, '\0', sizeof(r.error_msg)) != NULL;
}

bool waywal_ipc_serialize_resp_lint_scenescript(uint32_t status_code, uint32_t error_line,
                                                uint32_t error_col, uint32_t lint_time_us,
                                                const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_lint_scenescript_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_RESP_LINT_SCENESCRIPT, sizeof(*out_resp));
    waywal_ipc_resp_lint_scenescript_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.error_line = error_line;
    r.error_col = error_col;
    r.lint_time_us = lint_time_us;
    ipc_copy_bounded_str(r.error_msg, sizeof(r.error_msg), error_msg);
    memcpy(out_resp, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_req_readback_texture(const waywal_ipc_req_readback_texture_t *req,
                                              size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_readback_texture_t)) {
        return false;
    }
    waywal_ipc_req_readback_texture_t r;
    memcpy(&r, req, sizeof(r));
    return r.format_mode <= 1u && memchr(r.target_output, '\0', sizeof(r.target_output)) != NULL;
}

bool waywal_ipc_serialize_req_readback_texture(const char *target_output, int32_t object_id,
                                               uint32_t pass_index, uint32_t tex_slot,
                                               uint32_t format_mode, waywal_ipc_hdr_t *out_hdr,
                                               waywal_ipc_req_readback_texture_t *out_req)
{
    if (!out_hdr || !out_req || format_mode > 1u) {
        return false;
    }
    ipc_fill_hdr(out_hdr, WAYWAL_REQ_READBACK_TEXTURE, sizeof(*out_req));
    waywal_ipc_req_readback_texture_t r;
    memset(&r, 0, sizeof(r));
    ipc_copy_target_output(r.target_output, sizeof(r.target_output), target_output);
    r.object_id = object_id;
    r.pass_index = pass_index;
    r.tex_slot = tex_slot;
    r.format_mode = format_mode;
    memcpy(out_req, &r, sizeof(r));
    return true;
}

bool waywal_ipc_validate_resp_readback_texture(const waywal_ipc_resp_readback_texture_t *resp,
                                               size_t payload_size, const void **out_data,
                                               uint32_t *out_data_len)
{
    if (!resp || payload_size < sizeof(waywal_ipc_resp_readback_texture_t)) {
        return false;
    }
    waywal_ipc_resp_readback_texture_t r;
    memcpy(&r, resp, sizeof(r));
    if (r.data_len > WAYWAL_IPC_MAX_READBACK_BYTES ||
        payload_size != sizeof(r) + (size_t)r.data_len) {
        return false;
    }
    const void *data_ptr = NULL;
    if (r.data_len > 0u) {
        data_ptr = (const uint8_t *)resp + sizeof(r);
    }
    if (out_data) {
        *out_data = data_ptr;
    }
    if (out_data_len) {
        *out_data_len = r.data_len;
    }
    return true;
}

bool waywal_ipc_serialize_resp_readback_texture(uint32_t status_code, uint32_t tex_format,
                                                uint32_t width, uint32_t height, uint32_t flags,
                                                const void *data, uint32_t data_len,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_readback_texture_t *out_resp,
                                                void *out_buf, size_t out_buf_cap,
                                                size_t *out_total_len)
{
    if (!out_hdr || data_len > WAYWAL_IPC_MAX_READBACK_BYTES) {
        return false;
    }
    if (data_len > 0u && !data) {
        return false;
    }
    waywal_ipc_resp_readback_texture_t r;
    memset(&r, 0, sizeof(r));
    r.status_code = status_code;
    r.tex_format = tex_format;
    r.width = width;
    r.height = height;
    r.flags = flags;
    r.data_len = data_len;
    return ipc_serialize_var_msg(WAYWAL_RESP_READBACK_TEXTURE, &r, sizeof(r), data, data_len,
                                 out_hdr, out_resp, out_buf, out_buf_cap, out_total_len);
}
