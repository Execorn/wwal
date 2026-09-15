#ifndef WAYWAL_OUTPUT_STATE_H
#define WAYWAL_OUTPUT_STATE_H

#include "waywal/dmabuf.h"
#include "waywal/presentation.h"
#include "waywal/render_engine.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daemon_state;
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

    /* Hardware Direct Scanout ring */
    dmabuf_ring_t dmabuf_ring;
    bool use_dmabuf;

    /* Transition source buffers */
    dmabuf_bo_t source_old_bo;
    dmabuf_bo_t source_new_bo;
    bool has_source_old_bo;
    bool has_source_new_bo;

    /* Presentation synchronization */
    presentation_sync_t pres_sync;

    /* SHM ping-pong double-buffer (eliminates compositor/CPU write races) */
    struct wl_buffer *shm_buffers[2];   /* [0]=front (compositor), [1]=back (CPU) */
    struct wl_shm_pool *shm_pool;
    void *shm_data;                     /* double-sized mmap: frame_size * 2 bytes */
    void *shm_old_data;                 /* CPU staging: previous frame pixels */
    void *shm_new_data;                 /* CPU staging: target frame pixels */
    size_t shm_buffer_cap;              /* capacity of shm_old_data / shm_new_data */
    size_t shm_size;                    /* total mmap size = frame_size * 2 */
    int shm_fd;
    int active_buffer_idx;              /* 0 or 1: index currently committed to compositor */
    bool shm_buffer_released[2];        /* release listener: true when compositor released */

    /* Asynchronous Transition State */
    bool transition_active;
    uint32_t transition_current_frame;
    uint32_t transition_total_frames;
    uint32_t transition_delay_frames;
    waywal_transition_params_t transition_params;

    /* Dynamic Cursor Tracking */
    double cursor_x;
    double cursor_y;
    bool has_cursor;

    /* Persistent Video Buffers Pool */
    struct wl_buffer *video_wl_buffers[4];
    bool video_buffer_in_use[4];

    uint32_t scaling_mode; /* waywal_scaling_mode_t */
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

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_OUTPUT_STATE_H */
