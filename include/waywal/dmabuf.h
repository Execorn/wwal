#ifndef WAYWAL_DMABUF_H
#define WAYWAL_DMABUF_H

#include "linux-dmabuf-v1-client-protocol.h"

#include <drm_fourcc.h>
#include <gbm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_MAX_BUFFER_PLANES 4
#define WAYWAL_DMABUF_RING_SIZE  2 /* Double buffering */

typedef struct dmabuf_bo {
    int fd[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t stride[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t offset[WAYWAL_MAX_BUFFER_PLANES];
    uint64_t modifier;
    uint32_t num_planes;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;

    struct gbm_bo *gbm_bo;
    struct wl_buffer *wl_buffer;
    void *egl_image;
    uint32_t gl_tex;
    bool in_use;
} dmabuf_bo_t;

typedef struct dmabuf_context {
    int drm_fd;
    struct gbm_device *gbm;
    struct zwp_linux_dmabuf_v1 *dmabuf_proto;
    struct zwp_linux_dmabuf_feedback_v1 *default_feedback;
    uint64_t preferred_modifier;
    bool available;
} dmabuf_context_t;

typedef struct dmabuf_ring {
    dmabuf_bo_t buffers[WAYWAL_DMABUF_RING_SIZE];
    size_t current_idx;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint64_t modifier;
    bool initialized;
} dmabuf_ring_t;

/* Initialize DRM node and GBM device */
bool dmabuf_context_init(dmabuf_context_t *ctx);
void dmabuf_context_destroy(dmabuf_context_t *ctx);

/* Standalone buffer allocation */
bool dmabuf_bo_allocate(dmabuf_context_t *ctx, dmabuf_bo_t *bo, uint32_t width, uint32_t height,
                        uint32_t drm_format, uint64_t modifier);
void dmabuf_bo_free(dmabuf_bo_t *bo);

/* Ring buffer lifecycle */
bool dmabuf_ring_init(dmabuf_context_t *ctx, dmabuf_ring_t *ring, uint32_t width, uint32_t height,
                      uint32_t drm_format, uint64_t modifier);
void dmabuf_ring_destroy(dmabuf_context_t *ctx, dmabuf_ring_t *ring);

/* Acquire next available hardware buffer for rendering/display */
dmabuf_bo_t *dmabuf_ring_acquire(dmabuf_ring_t *ring);

/* Release buffer when compositor fires wl_buffer::release */
void dmabuf_bo_mark_released(dmabuf_ring_t *ring, struct wl_buffer *buffer);

/* Setup feedback */
void dmabuf_feedback_init(dmabuf_context_t *ctx);
void dmabuf_feedback_destroy(dmabuf_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_DMABUF_H */
