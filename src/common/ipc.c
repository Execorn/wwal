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

/* ========================================================================= */
/* WAYWAL_REQ_COMPILE_SHADER_GRAPH (0x001D) & RESP (0x800C)                  */
/* ========================================================================= */

bool waywal_ipc_validate_req_compile_shader_graph(const waywal_ipc_req_compile_shader_graph_t *req,
                                                  size_t payload_size, const char **out_inline_json)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_compile_shader_graph_t)) {
        return false;
    }

    if (payload_size !=
        sizeof(waywal_ipc_req_compile_shader_graph_t) + (size_t)req->inline_json_len) {
        return false;
    }

    if (!memchr(req->target_output, '\0', sizeof(req->target_output))) {
        return false;
    }

    if (!memchr(req->graph_json_path, '\0', sizeof(req->graph_json_path))) {
        return false;
    }

    if (out_inline_json) {
        if (req->inline_json_len > 0) {
            *out_inline_json = (const char *)req + sizeof(waywal_ipc_req_compile_shader_graph_t);
        } else {
            *out_inline_json = NULL;
        }
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
        inline_len = (uint32_t)strlen(inline_json);
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
        *out_req = req_data;
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

    if (!memchr(resp->error_msg, '\0', sizeof(resp->error_msg))) {
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

    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    out_resp->active_pass_mask = active_pass_mask;
    out_resp->compiled_node_count = compiled_node_count;
    out_resp->estimated_alu_ops = estimated_alu_ops;
    out_resp->compile_time_ms = compile_time_ms;
    out_resp->offending_node_id = offending_node_id;

    if (error_msg) {
        strncpy(out_resp->error_msg, error_msg, sizeof(out_resp->error_msg) - 1);
    }

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

    if (!memchr(req->target_output, '\0', sizeof(req->target_output))) {
        return false;
    }

    if (!isfinite(req->fov_y_deg) || !isfinite(req->near_z) || !isfinite(req->far_z)) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        if (!isfinite(req->eye_pos[i]) || !isfinite(req->target_pos[i]) ||
            !isfinite(req->up_vec[i])) {
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

    memset(out_req, 0, sizeof(*out_req));
    if (target_output) {
        strncpy(out_req->target_output, target_output, sizeof(out_req->target_output) - 1);
    } else {
        out_req->target_output[0] = '*';
    }

    out_req->projection_mode = projection_mode;
    out_req->flags = flags;

    if (eye_pos) {
        memcpy(out_req->eye_pos, eye_pos, sizeof(out_req->eye_pos));
    }
    if (target_pos) {
        memcpy(out_req->target_pos, target_pos, sizeof(out_req->target_pos));
    }
    if (up_vec) {
        memcpy(out_req->up_vec, up_vec, sizeof(out_req->up_vec));
    } else {
        out_req->up_vec[1] = 1.0f; /* Default {0, 1, 0} */
    }

    out_req->fov_y_deg = fov_y_deg;
    out_req->near_z = near_z;
    out_req->far_z = far_z;

    if (parallax_amount) {
        memcpy(out_req->parallax_amount, parallax_amount, sizeof(out_req->parallax_amount));
    }
    out_req->parallax_smoothing = parallax_smoothing;
    out_req->dof_focal_dist = dof_focal_dist;
    out_req->dof_focal_region = dof_focal_region;
    out_req->dof_bokeh_radius_px = dof_bokeh_radius_px;

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

    return true;
}

bool waywal_ipc_serialize_resp_set_camera_3d(uint32_t status_code, const float active_view_proj[16],
                                             waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_set_camera_3d_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    if (active_view_proj) {
        memcpy(out_resp->active_view_proj, active_view_proj, sizeof(out_resp->active_view_proj));
    }

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

    if (!memchr(req->target_output, '\0', sizeof(req->target_output))) {
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

    memset(out_req, 0, sizeof(*out_req));
    if (target_output) {
        strncpy(out_req->target_output, target_output, sizeof(out_req->target_output) - 1);
    } else {
        out_req->target_output[0] = '*';
    }

    out_req->object_id = object_id;
    out_req->bone_index = bone_index;

    if (bone_cfg) {
        out_req->bone_cfg = *bone_cfg;
    }
    if (wind) {
        out_req->wind = *wind;
    }
    if (cursor) {
        out_req->cursor = *cursor;
    }

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

    return true;
}

bool waywal_ipc_serialize_resp_set_physics_params(uint32_t status_code, uint32_t active_sim_bones,
                                                  float last_step_time_us,
                                                  waywal_ipc_hdr_t *out_hdr,
                                                  waywal_ipc_resp_set_physics_params_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    out_resp->active_sim_bones = active_sim_bones;
    out_resp->last_step_time_us = last_step_time_us;

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

    if (!memchr(req->target_output, '\0', sizeof(req->target_output))) {
        return false;
    }

    if (req->lighting.active_light_count > WE_MAX_SCENE_LIGHTS) {
        return false;
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

    memset(out_req, 0, sizeof(*out_req));
    if (target_output) {
        strncpy(out_req->target_output, target_output, sizeof(out_req->target_output) - 1);
    } else {
        out_req->target_output[0] = '*';
    }

    if (lighting) {
        out_req->lighting = *lighting;
    }

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

    return true;
}

bool waywal_ipc_serialize_resp_set_scene_lights(uint32_t status_code, uint32_t active_lights,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_resp_set_scene_lights_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }

    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    out_resp->active_lights = active_lights;

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
