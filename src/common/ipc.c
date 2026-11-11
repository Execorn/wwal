// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/ipc_proto_phase3.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
