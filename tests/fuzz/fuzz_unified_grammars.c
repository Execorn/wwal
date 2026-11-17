// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fuzz_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Map unquoted -DWWAL_FUZZ_TARGET=... tokens without tripping -Wundef warnings. */
#define WWAL_FUZZ_TGT_ID_0            0
#define WWAL_FUZZ_TGT_ID_1            0
#define WWAL_FUZZ_TGT_ID_default      0
#define WWAL_FUZZ_TGT_ID_shader_graph 1
#define WWAL_FUZZ_TGT_ID_toml_state   2
#define WWAL_FUZZ_TGT_ID_image_loader 3
#define WWAL_FUZZ_TGT_ID_anim_engine  4
#define WWAL_FUZZ_TGT_ID_ipc_server   5

#define WWAL_FUZZ_CAT_IMPL(a, b) a##b
#define WWAL_FUZZ_CAT(a, b)      WWAL_FUZZ_CAT_IMPL(a, b)

#if defined(WWAL_FUZZ_TARGET)
#define WWAL_FUZZ_TARGET_ID WWAL_FUZZ_CAT(WWAL_FUZZ_TGT_ID_, WWAL_FUZZ_TARGET)
#else
#define WWAL_FUZZ_TARGET_ID 0
#endif

#define WWAL_FUZZ_ENABLE_SHADER_GRAPH                                                              \
    ((WWAL_FUZZ_TARGET_ID == 0) || (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_shader_graph))
#define WWAL_FUZZ_ENABLE_TOML_STATE                                                                \
    ((WWAL_FUZZ_TARGET_ID == 0) || (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_toml_state))
#define WWAL_FUZZ_ENABLE_IMAGE_LOADER                                                              \
    ((WWAL_FUZZ_TARGET_ID == 0) || (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_image_loader))
/* anim_engine.c lives outside waywal_core, so pull it only when explicitly targeted. */
#if defined(WWAL_FUZZ_WITH_ANIM_ENGINE)
#define WWAL_FUZZ_ENABLE_ANIM_ENGINE                                                               \
    ((WWAL_FUZZ_TARGET_ID == 0) || (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_anim_engine))
#else
#define WWAL_FUZZ_ENABLE_ANIM_ENGINE (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_anim_engine)
#endif
#define WWAL_FUZZ_ENABLE_IPC_SERVER                                                                \
    ((WWAL_FUZZ_TARGET_ID == 0) || (WWAL_FUZZ_TARGET_ID == WWAL_FUZZ_TGT_ID_ipc_server))

#if WWAL_FUZZ_ENABLE_SHADER_GRAPH
#include "waywal/shader_graph.h"
#include "waywal/shader_parser.h"
#endif

#if WWAL_FUZZ_ENABLE_TOML_STATE
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"
#endif

#if WWAL_FUZZ_ENABLE_IMAGE_LOADER
#include "waywal/image_loader.h"
#endif

#if WWAL_FUZZ_ENABLE_ANIM_ENGINE
#include "waywal/anim_engine.h"
#endif

#if WWAL_FUZZ_ENABLE_IPC_SERVER
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/ipc_proto_rigging.h"
#endif

#if WWAL_FUZZ_ENABLE_SHADER_GRAPH
static void fuzz_target_shader_graph(const uint8_t *data, size_t size)
{
    waywal_shader_graph_t *g = waywal_shader_graph_create();
    if (!g) {
        return;
    }

    /* waywal_shader_graph_load_json calls strlen when json_len == 0. */
    const char *json_ptr = (size > 0u) ? (const char *)data : "";
    char err_buf[256];
    waywal_graph_schedule_t *sched = (waywal_graph_schedule_t *)calloc(1u, sizeof(*sched));
    if (sched) {
        if (waywal_shader_graph_load_json(json_ptr, size, g, err_buf)) {
            uint32_t cycle_node = 0u;
            (void)waywal_shader_graph_has_cycle(g, &cycle_node);
            if (waywal_shader_graph_analyze(g, sched)) {
                size_t glsl_len = 0u;
                char *glsl = waywal_shader_graph_compile_glsl(g, sched, &glsl_len);
                free(glsl);
            }
            char *saved = waywal_shader_graph_save_json(g);
            free(saved);
        }

        if (waywal_shader_graph_from_json(json_ptr, size, g, err_buf)) {
            char *saved2 = waywal_shader_graph_to_json(g);
            free(saved2);
        }

        /* Build a byte-driven DAG to hit scheduling, cycle checks, and GLSL SSA emission. */
        if (size >= 4u) {
            waywal_shader_graph_init(g, "fuzz_dag");
            uint32_t ids[16] = {0};
            uint32_t n_nodes = (uint32_t)((data[0] % 12u) + 2u);
            for (uint32_t i = 0u; i < n_nodes; ++i) {
                uint32_t op = (uint32_t)(data[(i + 1u) % size] % 51u);
                if (i + 1u == n_nodes && (data[0] & 1u) != 0u) {
                    op = WAYWAL_NODE_OUT_IMAGE_PASS;
                }
                char title[32];
                snprintf(title, sizeof(title), "n%u", i);
                int32_t nid = waywal_shader_graph_add_node(g, op, title, (float)(i * 80u), 0.0f);
                if (nid > 0) {
                    ids[i] = (uint32_t)nid;
                }
            }
            for (size_t i = 1u; i + 2u < size && i < 24u; i += 3u) {
                uint32_t s = ids[data[i] % n_nodes];
                uint32_t d = ids[data[i + 1u] % n_nodes];
                if (s != 0u && d != 0u) {
                    uint32_t sp = data[i + 2u] % WAYWAL_GRAPH_MAX_PINS_PER_NODE;
                    uint32_t dp = (data[i + 2u] >> 3u) % WAYWAL_GRAPH_MAX_PINS_PER_NODE;
                    (void)waywal_shader_graph_would_create_cycle(g, s, d);
                    int32_t eid = waywal_shader_graph_connect(g, s, sp, d, dp);
                    if (eid > 0 && ((data[i] & 0x80u) != 0u)) {
                        (void)waywal_shader_graph_disconnect(g, (uint32_t)eid);
                    }
                }
            }
            if (n_nodes > 2u && (data[0] & 2u) != 0u) {
                (void)waywal_shader_graph_remove_node(g, ids[0]);
            }
            uint32_t cycle_node = 0u;
            (void)waywal_shader_graph_has_cycle(g, &cycle_node);
            memset(sched, 0, sizeof(*sched));
            if (waywal_shader_graph_analyze(g, sched)) {
                size_t glsl_len = 0u;
                char *glsl = waywal_shader_graph_compile_glsl(g, sched, &glsl_len);
                free(glsl);
            }
            char *saved3 = waywal_shader_graph_save_json(g);
            if (saved3) {
                (void)waywal_shader_graph_load_json(saved3, strlen(saved3), g, err_buf);
                free(saved3);
            }
        }
        free(sched);
    }

    waywal_shader_meta_t *meta = (waywal_shader_meta_t *)calloc(1u, sizeof(*meta));
    if (meta) {
        waywal_shader_meta_init(meta);
        if (waywal_shader_parse_meta((const char *)data, size, meta)) {
            char decl_buf[2048];
            (void)waywal_shader_format_uniform_decls(meta, decl_buf, sizeof(decl_buf));
        }
        free(meta);
    }

    waywal_shader_graph_destroy(g);
}
#endif

#if WWAL_FUZZ_ENABLE_TOML_STATE
static char g_state_shm_dir[128] = {0};

static void cleanup_toml_state_shm(void)
{
    if (g_state_shm_dir[0] != '\0') {
        char path_in[192];
        char path_out[192];
        snprintf(path_in, sizeof(path_in), "%s/state.bin", g_state_shm_dir);
        snprintf(path_out, sizeof(path_out), "%s/state_rt.bin", g_state_shm_dir);
        (void)unlink(path_in);
        (void)unlink(path_out);
        (void)rmdir(g_state_shm_dir);
        g_state_shm_dir[0] = '\0';
    }
}

static bool ensure_toml_state_shm(void)
{
    if (g_state_shm_dir[0] != '\0') {
        return true;
    }
    snprintf(g_state_shm_dir, sizeof(g_state_shm_dir), "/dev/shm/wwal_fuzz_st_XXXXXX");
    if (!mkdtemp(g_state_shm_dir)) {
        snprintf(g_state_shm_dir, sizeof(g_state_shm_dir), "/tmp/wwal_fuzz_st_XXXXXX");
        if (!mkdtemp(g_state_shm_dir)) {
            g_state_shm_dir[0] = '\0';
            return false;
        }
    }
    (void)atexit(cleanup_toml_state_shm);
    return true;
}

static void fuzz_target_toml_state(const uint8_t *data, size_t size)
{
    size_t toml_len = size <= (256u * 1024u) ? size : (256u * 1024u);
    char *toml_str = (char *)malloc(toml_len + 1u);
    waywal_config_t *cfg = (waywal_config_t *)calloc(1u, sizeof(*cfg));
    if (toml_str && cfg) {
        memcpy(toml_str, data, toml_len);
        toml_str[toml_len] = '\0';

        char err_buf[256];
        waywal_config_init_defaults(cfg);
        if (waywal_config_parse_string(toml_str, cfg, err_buf, sizeof(err_buf))) {
            (void)waywal_config_match_output(cfg, "DP-1");
        }
        uint32_t dur_sec = 0u;
        (void)waywal_parse_duration_seconds(toml_str, &dur_sec);
        waywal_sort_mode_t sort_m = WAYWAL_SORT_RANDOM;
        (void)waywal_parse_sort_mode(toml_str, &sort_m);
        float gx = 0.0f;
        float gy = 0.0f;
        (void)waywal_parse_crop_gravity(toml_str, &gx, &gy);
    }
    free(cfg);
    free(toml_str);

    if (size > 0u && size <= (1024u * 1024u) && ensure_toml_state_shm()) {
        char state_path[192];
        snprintf(state_path, sizeof(state_path), "%s/state.bin", g_state_shm_dir);
        int fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            size_t off = 0u;
            while (off < size) {
                ssize_t w = write(fd, data + off, size - off);
                if (w <= 0) {
                    break;
                }
                off += (size_t)w;
            }
            close(fd);

            if (off == size) {
                waywal_state_store_t *store = (waywal_state_store_t *)calloc(1u, sizeof(*store));
                if (store) {
                    waywal_state_store_init(store);
                    if (waywal_state_store_load_at(store, state_path)) {
                        (void)waywal_state_store_find_output(store, "DP-1");
                        char rt_path[192];
                        snprintf(rt_path, sizeof(rt_path), "%s/state_rt.bin", g_state_shm_dir);
                        if (waywal_state_store_save_at(store, rt_path)) {
                            (void)waywal_state_store_load_at(store, rt_path);
                        }
                        (void)unlink(rt_path);
                    }
                    free(store);
                }
            }
            (void)unlink(state_path);
        }
    }

    if (size > 0u) {
        (void)waywal_filter_mode_to_swww_str((waywal_filter_mode_t)(data[0] & 7u));
    }
}
#endif

#if WWAL_FUZZ_ENABLE_IMAGE_LOADER
static void fuzz_target_image_loader(const uint8_t *data, size_t size)
{
    raw_image_t img;
    memset(&img, 0, sizeof(img));
    if ((image_load_from_memory)(data, size, &img)) {
        raw_image_free(&img);
    }

    uint8_t *rgba_px = NULL;
    uint32_t w = 0u;
    uint32_t h = 0u;
    if (image_load_from_memory_rgba(data, size, &rgba_px, &w, &h)) {
        image_free(rgba_px);
    }

    if (size > 0u && size <= (2u * 1024u * 1024u)) {
        int mfd = memfd_create("wwal_fuzz_img", MFD_CLOEXEC);
        if (mfd >= 0) {
            size_t off = 0u;
            while (off < size) {
                ssize_t n = write(mfd, data + off, size - off);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            if (off == size) {
                (void)lseek(mfd, 0, SEEK_SET);
                memset(&img, 0, sizeof(img));
                if ((image_load_fd)(mfd, &img)) {
                    raw_image_free(&img);
                }
                (void)lseek(mfd, 0, SEEK_SET);
                rgba_px = NULL;
                w = 0u;
                h = 0u;
                if (image_load_fd_rgba(mfd, &rgba_px, &w, &h)) {
                    image_free(rgba_px);
                }
            }
            close(mfd);
        }
    }
}
#endif

#if WWAL_FUZZ_ENABLE_ANIM_ENGINE
static void fuzz_target_anim_engine(const uint8_t *data, size_t size)
{
    (void)waywal_anim_probe(data, size);
    (void)waywal_anim_detect_format(data, size);

    waywal_anim_decoder_t *dec = waywal_anim_create(data, size);
    if (!dec) {
        return;
    }

    (void)waywal_anim_frame_count(dec);
    (void)waywal_anim_get_format(dec);

    const uint8_t *rgba = NULL;
    uint32_t w = 0u;
    uint32_t h = 0u;
    uint32_t delay_ms = 0u;
    for (uint32_t frame = 0u; frame < 16u; ++frame) {
        if (!waywal_anim_next_frame(dec, &rgba, &w, &h, &delay_ms)) {
            break;
        }
    }

    waywal_anim_reset(dec);
    (void)waywal_anim_next_frame(dec, &rgba, &w, &h, &delay_ms);
    waywal_anim_destroy(dec);
}
#endif

#if WWAL_FUZZ_ENABLE_IPC_SERVER
static void fuzz_ipc_socket_framing(const uint8_t *data, size_t size)
{
    if (size == 0u || size > 16384u) {
        return;
    }

    /* Raw wire framing over non-blocking Unix socketpair with SHUT_WR so recvmsg never blocks. */
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0) {
        int attached_src = -1;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        struct iovec iov = {.iov_base = (void *)(uintptr_t)data, .iov_len = size};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1u;

        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsg_u;

        if ((data[0] & 1u) != 0u) {
            attached_src = memfd_create("wwal_ipc_fuzz", MFD_CLOEXEC);
            if (attached_src >= 0) {
                memset(&cmsg_u, 0, sizeof(cmsg_u));
                msg.msg_control = cmsg_u.buf;
                msg.msg_controllen = sizeof(cmsg_u.buf);
                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(cmsg), &attached_src, sizeof(int));
            }
        }

        (void)sendmsg(sv[0], &msg, MSG_NOSIGNAL);
        if (attached_src >= 0) {
            close(attached_src);
        }
        (void)shutdown(sv[0], SHUT_WR);

        waywal_ipc_hdr_t recvd_hdr;
        int recvd_fds[4] = {-1, -1, -1, -1};
        size_t num_fds = 0u;
        (void)waywal_ipc_recv_fds(sv[1], &recvd_hdr, recvd_fds, 4u, &num_fds);
        for (size_t i = 0u; i < 4u; ++i) {
            if (recvd_fds[i] >= 0) {
                close(recvd_fds[i]);
            }
        }
        close(sv[0]);
        close(sv[1]);
    }

    /* Framed header + payload roundtrip via waywal_ipc_send_payload_fds -> waywal_ipc_recv. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0) {
        waywal_ipc_hdr_t send_hdr;
        memset(&send_hdr, 0, sizeof(send_hdr));
        send_hdr.magic = (data[0] & 2u) ? WAYWAL_IPC_MAGIC : (uint32_t)data[0];
        send_hdr.version = (size >= 2u && (data[0] & 4u)) ? WAYWAL_IPC_VERSION : (uint16_t)data[0];
        send_hdr.opcode = (uint16_t)(data[0] & 0x3Fu);
        send_hdr.payload_size = size;
        (void)waywal_ipc_send_payload_fds(sv[0], &send_hdr, data, size <= 4096u ? size : 4096u,
                                          NULL, 0u);
        (void)shutdown(sv[0], SHUT_WR);

        waywal_ipc_hdr_t recvd_hdr;
        int recvd_fd = -1;
        (void)waywal_ipc_recv(sv[1], &recvd_hdr, &recvd_fd);
        if (recvd_fd >= 0) {
            close(recvd_fd);
        }
        close(sv[0]);
        close(sv[1]);
    }
}

static void fuzz_ipc_wire_validators(const void *buf, size_t len)
{
    if (len >= sizeof(waywal_ipc_hdr_t)) {
        waywal_ipc_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        (void)daemon_validate_set_image_header(&hdr);
    }

    if (len >= sizeof(waywal_img_metadata_t)) {
        waywal_img_metadata_t meta;
        memcpy(&meta, buf, sizeof(meta));
        uint64_t pixel_bytes = 0u;
        uint64_t shm_sz = (len >= 8u) ? ((const uint64_t *)buf)[0] : (uint64_t)len;
        (void)waywal_validate_img_metadata(&meta, shm_sz, &pixel_bytes);
    }

    if (len >= sizeof(waywal_set_uniform_payload_t)) {
        waywal_set_uniform_payload_t uni;
        memcpy(&uni, buf, sizeof(uni));
        char target[64];
        char name[64];
        uint32_t comps = 0u;
        float vals[4] = {0};
        uint32_t flags = 0u;
        (void)waywal_decode_set_uniform_payload(&uni, target, sizeof(target), name, sizeof(name),
                                                &comps, vals, &flags);
    }

    /* Workshop / Hub validators (0x0019..0x001C, 0x8008..0x800B) */
    const char *out_str = NULL;
    (void)waywal_ipc_validate_req_workshop_sync((const waywal_ipc_req_workshop_sync_t *)buf, len);
    (void)waywal_ipc_validate_resp_workshop_sync((const waywal_ipc_resp_workshop_sync_t *)buf, len);
    (void)waywal_ipc_validate_req_workshop_download((const waywal_ipc_req_workshop_download_t *)buf,
                                                    len);
    (void)waywal_ipc_validate_resp_workshop_download(
        (const waywal_ipc_resp_workshop_download_t *)buf, len);
    (void)waywal_ipc_validate_req_wwpkg_install((const waywal_ipc_req_wwpkg_install_t *)buf, len,
                                                &out_str);
    (void)waywal_ipc_validate_resp_wwpkg_install((const waywal_ipc_resp_wwpkg_install_t *)buf, len,
                                                 &out_str);
    (void)waywal_ipc_validate_req_scan_compat((const waywal_ipc_req_scan_compat_t *)buf, len);
    (void)waywal_ipc_validate_resp_scan_compat((const waywal_ipc_resp_scan_compat_t *)buf, len);

    /* Camera, physics, lighting & shader-graph validators (0x001D..0x0020, 0x800C..0x800F) */
    (void)waywal_ipc_validate_req_compile_shader_graph(
        (const waywal_ipc_req_compile_shader_graph_t *)buf, len, &out_str);
    (void)waywal_ipc_validate_resp_compile_shader_graph(
        (const waywal_ipc_resp_compile_shader_graph_t *)buf, len);
    (void)waywal_ipc_validate_req_set_camera_3d((const waywal_ipc_req_set_camera_3d_t *)buf, len);
    (void)waywal_ipc_validate_resp_set_camera_3d((const waywal_ipc_resp_set_camera_3d_t *)buf, len);
    (void)waywal_ipc_validate_req_set_physics_params(
        (const waywal_ipc_req_set_physics_params_t *)buf, len);
    (void)waywal_ipc_validate_resp_set_physics_params(
        (const waywal_ipc_resp_set_physics_params_t *)buf, len);
    (void)waywal_ipc_validate_req_set_scene_lights((const waywal_ipc_req_set_scene_lights_t *)buf,
                                                   len);
    (void)waywal_ipc_validate_resp_set_scene_lights((const waywal_ipc_resp_set_scene_lights_t *)buf,
                                                    len);

    /* Authoring validators (0x0021..0x002A) */
    const we_brush_dab_t *dabs = NULL;
    uint32_t dab_count = 0u;
    const void *part_payload = NULL;
    uint32_t part_len = 0u;
    const we_audio_binding_t *bindings = NULL;
    uint32_t bind_count = 0u;
    (void)waywal_ipc_validate_req_scene_mutate((const waywal_ipc_req_scene_mutate_t *)buf, len,
                                               &out_str);
    (void)waywal_ipc_validate_req_hot_reload_code((const waywal_ipc_req_hot_reload_code_t *)buf,
                                                  len, &out_str);
    (void)waywal_ipc_validate_req_query_scene_tree((const waywal_ipc_req_query_scene_tree_t *)buf,
                                                   len);
    (void)waywal_ipc_validate_req_set_shader_combo((const waywal_ipc_req_set_shader_combo_t *)buf,
                                                   len);
    (void)waywal_ipc_validate_req_brush_stroke((const waywal_ipc_req_brush_stroke_t *)buf, len,
                                               &dabs, &dab_count);
    (void)waywal_ipc_validate_req_particle_config((const waywal_ipc_req_particle_config_t *)buf,
                                                  len, &part_payload, &part_len);
    (void)waywal_ipc_validate_req_set_audio_bindings(
        (const waywal_ipc_req_set_audio_bindings_t *)buf, len, &bindings, &bind_count);
    (void)waywal_ipc_validate_req_export_scene_pkg((const waywal_ipc_req_export_scene_pkg_t *)buf,
                                                   len);
    (void)waywal_ipc_validate_req_lint_scenescript((const waywal_ipc_req_lint_scenescript_t *)buf,
                                                   len, &out_str);
    (void)waywal_ipc_validate_req_readback_texture((const waywal_ipc_req_readback_texture_t *)buf,
                                                   len);

    /* Rigging validators (0x002B..0x0033) */
    const waywal_ipc_puppet_set_mesh_t *mesh_req = NULL;
    const waywal_ipc_mdl_vertex_t *mesh_verts = NULL;
    const uint16_t *mesh_idx = NULL;
    (void)waywal_ipc_validate_req_puppet_set_mesh(buf, len, &mesh_req, &mesh_verts, &mesh_idx);

    const waywal_ipc_puppet_set_skeleton_t *skel_req = NULL;
    const waywal_ipc_mdl_bone_t *skel_bones = NULL;
    (void)waywal_ipc_validate_req_puppet_set_skeleton(buf, len, &skel_req, &skel_bones);

    const waywal_ipc_puppet_solve_weights_t *solve_req = NULL;
    const uint64_t *locked_bs = NULL;
    (void)waywal_ipc_validate_req_puppet_solve_weights(buf, len, &solve_req, &locked_bs);

    const waywal_ipc_puppet_paint_weights_t *paint_req = NULL;
    const waywal_ipc_vertex_weight_t *paint_entries = NULL;
    (void)waywal_ipc_validate_req_puppet_paint_weights(buf, len, &paint_req, &paint_entries);

    const waywal_ipc_puppet_heatmap_mode_t *hm_req = NULL;
    (void)waywal_ipc_validate_req_puppet_heatmap_mode(buf, len, &hm_req);

    const waywal_ipc_puppet_preview_ik_t *ik_req = NULL;
    const waywal_ipc_bone_trs_t *ik_ovr = NULL;
    (void)waywal_ipc_validate_req_puppet_preview_ik(buf, len, &ik_req, &ik_ovr);

    const waywal_ipc_puppet_set_anim_tracks_t *anim_req = NULL;
    const uint8_t *clips_blob = NULL;
    size_t clips_len = 0u;
    (void)waywal_ipc_validate_req_puppet_set_anim_tracks(buf, len, &anim_req, &clips_blob,
                                                         &clips_len);

    const waywal_ipc_puppet_scrub_t *scrub_req = NULL;
    (void)waywal_ipc_validate_req_puppet_scrub_time(buf, len, &scrub_req);

    const waywal_ipc_export_puppet_mdl_t *exp_req = NULL;
    (void)waywal_ipc_validate_req_export_puppet_mdl(buf, len, &exp_req);

    const waywal_ipc_puppet_mesh_ack_t *ack_resp = NULL;
    (void)waywal_ipc_validate_resp_puppet_mesh_ack(buf, len, &ack_resp);

    const waywal_ipc_puppet_weights_resp_t *w_resp = NULL;
    const waywal_ipc_vertex_weight_t *w_entries = NULL;
    (void)waywal_ipc_validate_resp_puppet_weights(buf, len, &w_resp, &w_entries);

    const waywal_ipc_puppet_ik_pose_resp_t *ik_resp = NULL;
    const waywal_ipc_bone_trs_t *ik_bones = NULL;
    (void)waywal_ipc_validate_resp_puppet_ik_pose(buf, len, &ik_resp, &ik_bones);

    const waywal_ipc_export_puppet_mdl_resp_t *exp_resp = NULL;
    const uint8_t *exp_blob = NULL;
    (void)waywal_ipc_validate_resp_export_puppet_mdl(buf, len, &exp_resp, &exp_blob);
}

static void fuzz_target_ipc_server(const uint8_t *data, size_t size)
{
    fuzz_ipc_socket_framing(data, size);

    uint8_t *aligned = (uint8_t *)calloc(1u, 4096u);
    if (aligned) {
        size_t copy_len = size < 4096u ? size : 4096u;
        if (copy_len > 0u) {
            memcpy(aligned, data, copy_len);
        }
        fuzz_ipc_wire_validators(data, size);
        fuzz_ipc_wire_validators(aligned, 4096u);
        free(aligned);
    }
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data) {
        return 0;
    }

#if WWAL_FUZZ_ENABLE_SHADER_GRAPH
    fuzz_target_shader_graph(data, size);
#endif
#if WWAL_FUZZ_ENABLE_TOML_STATE
    fuzz_target_toml_state(data, size);
#endif
#if WWAL_FUZZ_ENABLE_IMAGE_LOADER
    fuzz_target_image_loader(data, size);
#endif
#if WWAL_FUZZ_ENABLE_ANIM_ENGINE
    fuzz_target_anim_engine(data, size);
#endif
#if WWAL_FUZZ_ENABLE_IPC_SERVER
    fuzz_target_ipc_server(data, size);
#endif

    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE
static void run_builtin_unified_corpus(void)
{
    wwal_fuzz_sample_t seeds[16];
    size_t n = 0u;

#if WWAL_FUZZ_ENABLE_SHADER_GRAPH
    static const char sg_valid[] =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"graph_name\": \"fuzz_smoke\",\n"
        "  \"nodes\": [\n"
        "    {\"node_id\": 1, \"opcode\": 0, \"title\": \"UV\", \"output_pin_count\": 1},\n"
        "    {\"node_id\": 2, \"opcode\": 50, \"title\": \"Image\", \"input_pin_count\": 1}\n"
        "  ],\n"
        "  \"edges\": [\n"
        "    {\"edge_id\": 1, \"src_node_id\": 1, \"src_pin_index\": 0, "
        "\"dst_node_id\": 2, \"dst_pin_index\": 0}\n"
        "  ]\n"
        "}";
    static const char sg_glsl_meta[] = "// @title Fuzz Plasma\n"
                                       "// @uniform float u_speed 1.5 0.0 10.0\n"
                                       "// @pass BufferA scale=0.5\n"
                                       "void mainImage(out vec4 fragColor, in vec2 fragCoord) {}\n";
    seeds[n++] = (wwal_fuzz_sample_t){(const uint8_t *)sg_valid, sizeof(sg_valid) - 1u};
    seeds[n++] = (wwal_fuzz_sample_t){(const uint8_t *)sg_glsl_meta, sizeof(sg_glsl_meta) - 1u};
#endif

#if WWAL_FUZZ_ENABLE_TOML_STATE
    static const char ts_toml[] =
        "[daemon]\nlayer = \"background\"\n[transition]\ntype = \"fade\"\nduration = 0.5\n"
        "crop_gravity = \"center\"\n[output.\"DP-1\"]\npath = \"/tmp/w.png\"\n";
    static const uint8_t ts_state_hdr[] = {
        0x54u, 0x53u, 0x57u, 0x57u, 0x06u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    seeds[n++] = (wwal_fuzz_sample_t){(const uint8_t *)ts_toml, sizeof(ts_toml) - 1u};
    seeds[n++] = (wwal_fuzz_sample_t){ts_state_hdr, sizeof(ts_state_hdr)};
#endif

#if WWAL_FUZZ_ENABLE_IMAGE_LOADER
    static const uint8_t il_ppm[] = "P6\n1 1\n255\n\xFF\x00\x80";
    static const uint8_t il_qoi[] = {
        'q', 'o',  'i',  'f',  0,    0, 0, 1, 0, 0, 0, 1, 4,
        0,   0xFF, 0xAA, 0x55, 0xFF, 0, 0, 0, 0, 0, 0, 0, 1,
    };
    static const uint8_t il_png_hdr[] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    seeds[n++] = (wwal_fuzz_sample_t){il_ppm, sizeof(il_ppm) - 1u};
    seeds[n++] = (wwal_fuzz_sample_t){il_qoi, sizeof(il_qoi)};
    seeds[n++] = (wwal_fuzz_sample_t){il_png_hdr, sizeof(il_png_hdr)};
#endif

#if WWAL_FUZZ_ENABLE_ANIM_ENGINE
    /* Minimal valid 1x1 2-frame animated GIF89a with NETSCAPE2.0 extension and GCEs. */
    static const uint8_t ae_gif89a[] = {
        'G',  'I',  'F',  '8',  '9',  'a',  0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xFF, 0xFF, 0xFF, 0x21, 0xFF, 0x0B, 'N',  'E',  'T',  'S',  'C',  'A',  'P',  'E',
        '2',  '.',  '0',  0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xF9, 0x04, 0x04, 0x05, 0x00, 0x00,
        0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01,
        0x00, 0x21, 0xF9, 0x04, 0x04, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x4C, 0x01, 0x00, 0x3B,
    };
    static const uint8_t ae_apng[] = {
        0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0, 0, 0, 0x08u, 'a', 'c', 'T', 'L',
    };
    seeds[n++] = (wwal_fuzz_sample_t){ae_gif89a, sizeof(ae_gif89a)};
    seeds[n++] = (wwal_fuzz_sample_t){ae_apng, sizeof(ae_apng)};
#endif

#if WWAL_FUZZ_ENABLE_IPC_SERVER
    uint8_t is_req_sync[sizeof(waywal_ipc_hdr_t) + sizeof(waywal_ipc_req_workshop_sync_t)];
    memset(is_req_sync, 0, sizeof(is_req_sync));
    waywal_ipc_hdr_t shdr;
    waywal_ipc_req_workshop_sync_t sreq;
    if (waywal_ipc_serialize_req_workshop_sync(1u, "/tmp/ws", &shdr, &sreq)) {
        memcpy(is_req_sync, &shdr, sizeof(shdr));
        memcpy(is_req_sync + sizeof(shdr), &sreq, sizeof(sreq));
        seeds[n++] = (wwal_fuzz_sample_t){is_req_sync, sizeof(is_req_sync)};
    }
    static const uint8_t is_raw_hdr[] = {'W', 'W', 'A', 'L', 0x01, 0x00, 0x11, 0x00};
    seeds[n++] = (wwal_fuzz_sample_t){is_raw_hdr, sizeof(is_raw_hdr)};
#endif

    if (n > 0u) {
        wwal_fuzz_run_mutations(seeds, n, 0xD15EA5E5u, 512u, 2048u);
    }

    /* Short deterministic regression assertions per enabled grammar target. */
#if WWAL_FUZZ_ENABLE_SHADER_GRAPH
    {
        waywal_shader_graph_t *g = waywal_shader_graph_create();
        waywal_graph_schedule_t *sched = (waywal_graph_schedule_t *)calloc(1u, sizeof(*sched));
        char err[256];
        if (!g || !sched ||
            !waywal_shader_graph_load_json(sg_valid, sizeof(sg_valid) - 1u, g, err) ||
            !waywal_shader_graph_analyze(g, sched)) {
            free(sched);
            waywal_shader_graph_destroy(g);
            exit(1);
        }
        size_t glsl_len = 0u;
        char *glsl = waywal_shader_graph_compile_glsl(g, sched, &glsl_len);
        if (!glsl || glsl_len == 0u) {
            free(glsl);
            free(sched);
            waywal_shader_graph_destroy(g);
            exit(1);
        }
        free(glsl);
        free(sched);
        waywal_shader_graph_destroy(g);
    }
#endif

#if WWAL_FUZZ_ENABLE_TOML_STATE
    if (ensure_toml_state_shm()) {
        waywal_state_store_t *st = (waywal_state_store_t *)calloc(1u, sizeof(*st));
        waywal_output_saved_state_t *out_st =
            (waywal_output_saved_state_t *)calloc(1u, sizeof(*out_st));
        if (!st || !out_st) {
            free(st);
            free(out_st);
            exit(1);
        }
        waywal_state_store_init(st);
        snprintf(out_st->output_name, sizeof(out_st->output_name), "DP-1");
        out_st->kind = WAYWAL_CONTENT_SOLID_COLOR;
        out_st->solid_color = 0x112233FFu;
        out_st->video_speed = 1.0f;
        out_st->volume = 1.0f;
        char p[192];
        snprintf(p, sizeof(p), "%s/state_rt.bin", g_state_shm_dir);
        bool ok = waywal_state_store_update_output(st, out_st) &&
                  waywal_state_store_save_at(st, p) && waywal_state_store_load_at(st, p) &&
                  waywal_state_store_find_output(st, "DP-1") != NULL;
        (void)unlink(p);
        free(out_st);
        free(st);
        if (!ok) {
            exit(1);
        }
    }
#endif

#if WWAL_FUZZ_ENABLE_IMAGE_LOADER
    {
        raw_image_t img;
        memset(&img, 0, sizeof(img));
        if (!(image_load_from_memory)(il_ppm, sizeof(il_ppm) - 1u, &img) || img.width != 1u ||
            img.height != 1u) {
            raw_image_free(&img);
            exit(1);
        }
        raw_image_free(&img);
    }
#endif

#if WWAL_FUZZ_ENABLE_ANIM_ENGINE
    if (!waywal_anim_probe(ae_gif89a, sizeof(ae_gif89a)) ||
        waywal_anim_detect_format(ae_gif89a, sizeof(ae_gif89a)) != WAYWAL_ANIM_FORMAT_GIF) {
        exit(1);
    }
#endif

#if WWAL_FUZZ_ENABLE_IPC_SERVER
    {
        waywal_ipc_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.magic = WAYWAL_IPC_MAGIC;
        h.version = WAYWAL_IPC_VERSION;
        h.opcode = WAYWAL_REQ_SET_IMAGE;
        h.payload_size = sizeof(waywal_img_metadata_t);
        if (!daemon_validate_set_image_header(&h)) {
            exit(1);
        }
    }
#endif
}

static void cleanup_unified_grammars(void)
{
#if WWAL_FUZZ_ENABLE_TOML_STATE
    cleanup_toml_state_shm();
#endif
}

WWAL_FUZZ_HARNESS_MAIN("fuzz_unified_grammars", NULL, run_builtin_unified_corpus,
                       cleanup_unified_grammars)
#endif
