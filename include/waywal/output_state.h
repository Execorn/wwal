// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_OUTPUT_STATE_H
#define WAYWAL_OUTPUT_STATE_H

#include "waywal/dmabuf.h"
#include "waywal/ipc_proto.h"
#include "waywal/presentation.h"
#include "waywal/render_engine.h"
#include "waywal/types.h"
#include "waywal/vaapi_dec.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daemon_state;
struct waywal_shader_engine;
struct waywal_anim_decoder;
struct video_engine;
struct slideshow_engine;
typedef struct output_node output_node_t;
typedef struct output_node output_state_t;

struct output_node {
    union {
        uint32_t global_name;
        uint32_t wl_name;
    };
    union {
        struct wl_output *wl_output;
        struct wl_output *output;
    };
    char name[64];
    char description[128];
    int32_t width;
    int32_t height;
    int32_t mode_width;
    int32_t mode_height;
    int32_t shm_width;
    int32_t shm_height;
    int32_t staging_width;
    int32_t staging_height;
    uint32_t logical_width;
    uint32_t logical_height;
    union {
        int32_t global_x;
        int32_t logical_x;
    };
    union {
        int32_t global_y;
        int32_t logical_y;
    };
    bool has_logical_pos;
    int32_t span_off_x;
    int32_t span_off_y;
    int32_t span_total_w;
    int32_t span_total_h;
    union {
        int32_t scale_factor;
        int32_t scale;
    };
    int32_t fractional_scale; /* in 120ths */
    uint32_t refresh_mhz;
    int32_t transform;

    /* Wayland layer shell surface */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fract_scale;
    struct zxdg_output_v1 *xdg_output;

    struct wl_surface *backdrop_surface;
    struct zwlr_layer_surface_v1 *backdrop_layer_surface;
    struct wp_viewport *backdrop_viewport;
    dmabuf_bo_t backdrop_bo;
    bool has_backdrop_bo;
    bool backdrop_configured;

    /* Hardware Direct Scanout ring */
    dmabuf_ring_t dmabuf_ring;
    bool use_dmabuf;

    /* Transition source buffers */
    dmabuf_bo_t source_old_bo;
    dmabuf_bo_t source_new_bo;
    bool has_source_old_bo;
    bool has_source_new_bo;
    dmabuf_bo_t *last_presented_bo;

    struct waywal_shader_engine *shader_eng;
    bool shader_active;
    bool shader_audio_reactive;
    uint32_t shader_fps;
    uint64_t shader_frame_idx;
    uint64_t shader_start_ns;
    uint64_t shader_last_ns;
    char shader_path[4096];

    struct we_scene *we_scene;
    bool workshop_active;
    char workshop_dir[4096];
    char workshop_props[2048];

    struct waywal_anim_decoder *anim_dec;
    bool anim_active;
    uint32_t anim_delay_ms;
    uint64_t anim_next_ns;
    waywal_img_metadata_t anim_meta;

    struct video_engine *video_eng;
    struct slideshow_engine *slideshow_eng;

    /* Presentation synchronization */
    presentation_sync_t pres_sync;

    /* SHM ping-pong double-buffer (eliminates compositor/CPU write races) */
    struct wl_buffer *shm_buffers[2]; /* [0]=front (compositor), [1]=back (CPU) */
    struct wl_shm_pool *shm_pool;
    void *shm_data;        /* double-sized mmap: frame_size * 2 bytes */
    void *shm_old_data;    /* CPU staging: previous frame pixels */
    void *shm_new_data;    /* CPU staging: target frame pixels */
    size_t shm_buffer_cap; /* capacity of shm_old_data / shm_new_data */
    size_t shm_size;       /* total mmap size = frame_size * 2 */
    int shm_fd;
    int active_buffer_idx;       /* 0 or 1: index currently committed to compositor */
    bool shm_buffer_released[2]; /* release listener: true when compositor released */

    /* Asynchronous Transition State */
    bool transition_active;
    uint32_t transition_current_frame;
    uint32_t transition_total_frames;
    uint32_t transition_delay_frames;
    waywal_transition_params_t transition_params;

    /* Dynamic Cursor Tracking & Spring Filter */
    double cursor_x;
    double cursor_y;
    double click_x;
    double click_y;
    bool has_cursor;
    bool button_down;
    bool click_pulse;
    float smooth_mouse_x;
    float smooth_mouse_y;
    float mouse_vel_x;
    float mouse_vel_y;
    float smooth_workspace;
    float workspace_vel;
    uint64_t last_spring_ns;

    /* Cached wp_viewport state to avoid redundant Wayland requests per frame */
    int32_t vp_dst_w;
    int32_t vp_dst_h;
    uint32_t vp_src_w;
    uint32_t vp_src_h;
    uint32_t vp_scaling_mode;
    bool vp_configured;
    bool video_targeted;

    uint32_t scaling_mode; /* waywal_scaling_mode_t */
    uint32_t filter_mode;  /* waywal_filter_mode_t */
    color_rgba_t fill_color;
    bool has_fill_color;
    bool ten_bit;
    dev_t target_dev;
    bool has_target_dev;
    bool has_image;

    bool configured;
    struct daemon_state *state;
    struct output_node *next;
};

typedef struct {
    output_node_t *head;
    uint32_t count;
    dmabuf_context_t *dmabuf_ctx;
    struct wp_presentation *wp_pres;
} output_manager_t;

void output_manager_init(output_manager_t *om, dmabuf_context_t *ctx, struct wp_presentation *pres);
void output_manager_destroy(output_manager_t *om);

output_node_t *output_manager_add(output_manager_t *om, struct wl_registry *reg, uint32_t name,
                                  uint32_t version);
void output_manager_remove(output_manager_t *om, uint32_t name);
output_node_t *output_manager_find_by_name(output_manager_t *om, const char *name);
output_node_t *output_manager_find_by_id(output_manager_t *om, uint32_t name);

bool layer_shell_ensure_dmabuf_ring(struct daemon_state *state, output_node_t *node, int32_t width,
                                    int32_t height);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_OUTPUT_STATE_H */
