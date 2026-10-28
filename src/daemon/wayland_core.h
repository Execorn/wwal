// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WAYLAND_CORE_H
#define WAYWAL_WAYLAND_CORE_H

#include "cursor-shape-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "waywal/arena.h"
#include "waywal/dmabuf.h"
#include "waywal/ipc_proto.h"
#include "waywal/palette_hct.h"
#include "waywal/presentation.h"
#include "waywal/render_engine.h"
#include "waywal/types.h"
#include "waywal/uring_loop.h"
#include "waywal/video_engine.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "waywal/output_state.h"
#include "waywal/slideshow.h"
#include "waywal/toml_parser.h"

struct daemon_state;
struct waywal_power_governor;
struct waywal_shader_engine;
struct waywal_audio_fft;
struct waywal_anim_decoder;
struct waywal_mpris;

typedef struct daemon_state {
    arena_t frame_arena;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fract_manager;
    struct wp_presentation *presentation;
    struct wp_single_pixel_buffer_manager_v1 *single_pixel_mgr;
    struct wp_linux_drm_syncobj_manager_v1 *syncobj_mgr;
    struct ext_idle_notifier_v1 *idle_notifier;
    struct ext_idle_notification_v1 *idle_notification;
    struct ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
    struct zxdg_output_manager_v1 *xdg_output_mgr;
    struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
    union {
        struct wp_cursor_shape_device_v1 *cursor_shape_dev;
        struct wp_cursor_shape_device_v1 *cursor_shape_device;
    };
    struct zwlr_foreign_toplevel_manager_v1 *wlr_foreign_toplevel_mgr;
    void *wlr_toplevels;

    int cursor_ipc_fd;
    uint64_t last_cursor_ipc_ns;
    double cached_ipc_cursor_x;
    double cached_ipc_cursor_y;
    bool has_cached_ipc_cursor;

    /* Hardware Direct Scanout context */
    dmabuf_context_t dmabuf_ctx;

    /* Hardware Render Engine (Compute Shaders & SIMD) */
    render_engine_t render_engine;

    /* Hardware Video Wallpaper Engine (VA-API & Zero-Copy PRIME) */
    video_engine_t video_engine;

    /* Built-in Daemon Slideshow Engine */
    slideshow_engine_t slideshow;

    struct waywal_power_governor *power_gov;
    struct waywal_shader_engine *shader_eng;
    struct we_scene *we_scene;
    bool workshop_active;
    char workshop_dir[4096];
    char workshop_props[2048];
    struct waywal_audio_fft *audio_fft;
    int shader_timer_fd;
    bool shader_active;
    uint32_t shader_fps;
    uint64_t shader_frame_idx;
    uint64_t shader_start_ns;
    uint64_t shader_last_ns;
    char shader_target_output[64];
    char shader_path[4096];
    char shader_depth_map_path[256];
    uint8_t *wallpaper_rgba;
    uint32_t wallpaper_w;
    uint32_t wallpaper_h;
    float smooth_mouse_x;
    float smooth_mouse_y;
    float mouse_vel_x;
    float mouse_vel_y;
    float smooth_workspace_x;
    float smooth_workspace_y;
    float workspace_vel_x;
    float workspace_vel_y;
    float workspace_target_x;
    float workspace_target_y;

    struct waywal_anim_decoder *anim_dec;
    int anim_timer_fd;
    bool anim_active;
    uint8_t *anim_buf;
    size_t anim_buf_size;
    waywal_img_metadata_t anim_meta;
    char anim_target_output[64];

    uint32_t idle_timeout_sec;
    waywal_config_t config;
    waywal_palette_t active_palette;
    bool has_active_palette;
    waywal_palette_t saved_wallpaper_palette;
    bool has_saved_wallpaper_palette;
    waywal_img_metadata_t active_theme_meta;
    bool has_active_theme_meta;
    void (*on_output_configured)(struct daemon_state *state, output_node_t *out);
    void (*on_power_state_changed)(struct daemon_state *state);
    void (*on_wallpaper_loaded)(struct daemon_state *state, const uint8_t *bgra_pixels, uint32_t w,
                                uint32_t h, const waywal_img_metadata_t *meta);

    /* Modern Linux io_uring asynchronous event loop */
    uring_loop_t *uring_loop;
    bool use_uring;

    /* Unified display output manager */
    output_manager_t output_mgr;
    output_node_t *outputs;
    size_t num_outputs;

    /* Asynchronous transitions timer state */
    int transition_timer_fd;
    bool transitions_in_progress;

    int epoll_fd;
    int server_socket_fd;
    int signal_fd;
    bool running;

    bool use_bottom_layer;
    bool backdrop_auto_blur;
    char backdrop_path[4096];
    char namespace_str[64];
    color_rgba_t current_color;

    struct waywal_mpris *mpris;
    int subscriber_fds[16];
    size_t num_subscribers;
    bool theme_from_media;
    bool media_palette_active;
    float audio_volume;
    bool mute_on_audio;
    slideshow_engine_t *g_presenting_slideshow;
    union {
        int last_workspace_id;
        int last_workspace_idx;
    };
    char last_workspace_name[64];

    int solar_timer_fd;
    bool last_solar_is_daytime;
    bool has_last_solar_state;
    bool in_solar_tick;
    bool solar_owns_wallpaper;
    bool sched_theme_override_active;
    char active_dynwall_path[4096];
    char active_dynwall_output[64];
    char last_dynwall_frame_path[4096];
    char last_schedule_frame_path[4096];
} daemon_state_t;

bool daemon_wayland_init(daemon_state_t *state);
void daemon_wayland_dispatch(daemon_state_t *state);
void daemon_wayland_destroy(daemon_state_t *state);
bool daemon_query_cursor_pos(daemon_state_t *state, output_node_t *out, double *out_x,
                             double *out_y);

/* Asynchronous Transitions Engine API */
bool transition_engine_init(daemon_state_t *state);
void transition_engine_destroy(daemon_state_t *state);
void transition_engine_dispatch_tick(daemon_state_t *state);
bool daemon_start_image_transition(daemon_state_t *state, const uint8_t *pixels, uint32_t img_w,
                                   uint32_t img_h, const waywal_img_metadata_t *meta);

void output_node_create_surface(daemon_state_t *state, output_node_t *node);
void output_node_destroy_surface(output_node_t *node);
void output_node_render_color(daemon_state_t *state, output_node_t *node, color_rgba_t color);
void output_node_render_image(daemon_state_t *state, output_node_t *node, const uint8_t *pixels,
                              uint32_t img_w, uint32_t img_h);
void output_node_render_image_with_transition(daemon_state_t *state, output_node_t *node,
                                              const uint8_t *pixels, uint32_t img_w, uint32_t img_h,
                                              const waywal_img_metadata_t *meta);

void layer_shell_update_input_regions(daemon_state_t *state, bool interactive);
void layer_shell_compute_span_bounds(daemon_state_t *state, output_node_t *target);
void layer_shell_snapshot_live_output(daemon_state_t *state, output_node_t *out);
void waywal_scale_image_span_cpu(uint32_t *dst, uint32_t dst_w, uint32_t dst_h,
                                 size_t dst_stride_bytes, const uint32_t *src, uint32_t src_w,
                                 uint32_t src_h, int32_t span_off_x, int32_t span_off_y,
                                 int32_t span_total_w, int32_t span_total_h, uint32_t filter_mode,
                                 color_rgba_t bg_color);

void daemon_clear_all_outputs(daemon_state_t *state, color_rgba_t color);
void daemon_handle_ipc_connection(daemon_state_t *state);
void reconcile_global_engine_flags(daemon_state_t *state);
void daemon_reconcile_power_state(daemon_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WAYLAND_CORE_H */
