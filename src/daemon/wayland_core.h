#ifndef WAYWAL_WAYLAND_CORE_H
#define WAYWAL_WAYLAND_CORE_H

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
#include "waywal/presentation.h"
#include "waywal/render_engine.h"
#include "waywal/types.h"
#include "waywal/uring_loop.h"
#include "waywal/video_engine.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

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

typedef struct daemon_state {
    arena_t persistent_arena;
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
    struct waywal_audio_fft *audio_fft;
    int shader_timer_fd;
    bool shader_active;
    uint32_t shader_fps;
    uint64_t shader_frame_idx;
    uint64_t shader_start_ns;
    uint64_t shader_last_ns;
    char shader_target_output[64];
    char shader_path[4096];

    struct waywal_anim_decoder *anim_dec;
    int anim_timer_fd;
    bool anim_active;
    uint8_t *anim_buf;
    size_t anim_buf_size;
    waywal_img_metadata_t anim_meta;
    char anim_target_output[64];

    uint32_t idle_timeout_sec;
    waywal_config_t config;
    void (*on_output_configured)(struct daemon_state *state, output_node_t *out);
    void (*on_power_state_changed)(struct daemon_state *state);

    /* Modern Linux io_uring asynchronous event loop */
    uring_loop_t *uring_loop;
    bool use_uring;

    /* Unified Display Output Manager (ARCH-05) */
    output_manager_t output_mgr;
    output_node_t *outputs;
    size_t num_outputs;

    /* Asynchronous Transitions Engine (BUG-03, BUG-04) */
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

void daemon_clear_all_outputs(daemon_state_t *state, color_rgba_t color);
void daemon_handle_ipc_connection(daemon_state_t *state);
void layer_shell_blur_rgba_cpu(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WAYLAND_CORE_H */
