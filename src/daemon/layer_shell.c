#include "wayland_core.h"
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* SHM ping-pong buffer release listener: tracks when compositor releases a buffer */
static void shm_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    output_node_t *node = (output_node_t *)data;
    if (!node)
        return;
    for (int i = 0; i < 2; ++i) {
        if (node->shm_buffers[i] == wl_buffer) {
            node->shm_buffer_released[i] = true;
            break;
        }
    }
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_release,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial, uint32_t width, uint32_t height)
{
    output_node_t *node = (output_node_t *)data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    bool was_configured = node->configured;
    if (width > 0)
        node->width = (int32_t)width;
    if (height > 0)
        node->height = (int32_t)height;
    node->configured = true;

    WAYWAL_LOG_DEBUG("Output %s (%s) configured: %dx%d", node->name, node->description, node->width,
                     node->height);

    if (!was_configured) {
        output_node_render_color(node->state, node, node->state->current_color);
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)layer_surface;
    output_node_t *node = (output_node_t *)data;
    WAYWAL_LOG_INFO("Layer surface closed for output %s", node->name);
    output_node_destroy_surface(node);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

void output_node_create_surface(daemon_state_t *state, output_node_t *node)
{
    if (!state || !node || node->surface)
        return;
    if (!state->compositor || !state->layer_shell)
        return;

    node->state = state;
    node->surface = wl_compositor_create_surface(state->compositor);
    if (!node->surface) {
        WAYWAL_LOG_ERR("Failed to create wl_surface for output %s", node->name);
        return;
    }

    node->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state->layer_shell, node->surface, node->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "waywal-wallpaper");
    if (!node->layer_surface) {
        WAYWAL_LOG_ERR("Failed to get zwlr_layer_surface_v1 for output %s", node->name);
        wl_surface_destroy(node->surface);
        node->surface = NULL;
        return;
    }

    zwlr_layer_surface_v1_set_anchor(node->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                              ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                              ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                              ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(node->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        node->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_set_size(node->layer_surface, 0, 0);

    /* Input passthrough: set empty input region */
    struct wl_region *empty_region = wl_compositor_create_region(state->compositor);
    wl_surface_set_input_region(node->surface, empty_region);
    wl_region_destroy(empty_region);

    zwlr_layer_surface_v1_add_listener(node->layer_surface, &layer_surface_listener, node);

    if (state->viewporter) {
        node->viewport = wp_viewporter_get_viewport(state->viewporter, node->surface);
    }

    if (state->presentation) {
        presentation_sync_init(&node->pres_sync, state->presentation, node->surface);
    }

    wl_surface_commit(node->surface);
    wl_display_flush(state->display);
}

static inline void output_commit_frame(output_node_t *node)
{
    if (!node || !node->surface)
        return;
    if (node->pres_sync.wp_pres) {
        presentation_sync_request(&node->pres_sync);
    }
    wl_surface_commit(node->surface);
}

void output_node_destroy_surface(output_node_t *node)
{
    if (!node)
        return;

    for (uint32_t i = 0; i < 4; ++i) {
        if (node->video_wl_buffers[i]) {
            wl_buffer_destroy(node->video_wl_buffers[i]);
            node->video_wl_buffers[i] = NULL;
            node->video_buffer_in_use[i] = false;
        }
    }

    if (node->shm_old_data) {
        free(node->shm_old_data);
        node->shm_old_data = NULL;
    }
    if (node->shm_new_data) {
        free(node->shm_new_data);
        node->shm_new_data = NULL;
    }
    node->shm_buffer_cap = 0;

    if (node->use_dmabuf) {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            render_engine_release_bo(&node->state->render_engine, &node->dmabuf_ring.buffers[i]);
        }
        dmabuf_ring_destroy(&node->state->dmabuf_ctx, &node->dmabuf_ring);
        node->use_dmabuf = false;
    }
    if (node->has_source_old_bo) {
        render_engine_release_bo(&node->state->render_engine, &node->source_old_bo);
        dmabuf_bo_free(&node->source_old_bo);
        node->has_source_old_bo = false;
    }
    if (node->has_source_new_bo) {
        render_engine_release_bo(&node->state->render_engine, &node->source_new_bo);
        dmabuf_bo_free(&node->source_new_bo);
        node->has_source_new_bo = false;
    }

    for (int i = 0; i < 2; ++i) {
        if (node->shm_buffers[i]) {
            wl_buffer_destroy(node->shm_buffers[i]);
            node->shm_buffers[i] = NULL;
        }
        node->shm_buffer_released[i] = true;
    }
    if (node->shm_pool) {
        wl_shm_pool_destroy(node->shm_pool);
        node->shm_pool = NULL;
    }
    if (node->shm_data) {
        munmap(node->shm_data, node->shm_size);
        node->shm_data = NULL;
        node->shm_size = 0;
    }
    if (node->shm_fd >= 0) {
        close(node->shm_fd);
        node->shm_fd = -1;
    }


    if (node->viewport) {
        wp_viewport_destroy(node->viewport);
        node->viewport = NULL;
    }
    if (node->fract_scale) {
        wp_fractional_scale_v1_destroy(node->fract_scale);
        node->fract_scale = NULL;
    }
    if (node->layer_surface) {
        zwlr_layer_surface_v1_destroy(node->layer_surface);
        node->layer_surface = NULL;
    }
    presentation_sync_destroy(&node->pres_sync);
    if (node->surface) {
        wl_surface_destroy(node->surface);
        node->surface = NULL;
    }
    node->configured = false;
    node->transition_active = false;
    node->transition_current_frame = 0;
    node->transition_total_frames = 0;
}

static bool is_multigpu_system(void)
{
    static int s_multigpu = -1;
    if (s_multigpu != -1)
        return (s_multigpu == 1);
    DIR *d = opendir("/dev/dri");
    int count = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "renderD", 7) == 0) {
                count++;
            }
        }
        closedir(d);
    }
    s_multigpu = (count > 1) ? 1 : 0;
    return (s_multigpu == 1);
}

static bool ensure_dmabuf_ring(daemon_state_t *state, output_node_t *node, int32_t width,
                               int32_t height)
{
    if (!state || !state->dmabuf_ctx.available || !state->dmabuf_ctx.dmabuf_proto) {
        return false;
    }
    if (width <= 0 || height <= 0)
        return false;

    /* Multi-GPU hybrid topologies (e.g. AMD iGPU + NVIDIA dGPU) drive different outputs
     * from different DRM devices. Cross-device DMA-BUF sharing causes compositor driver aborts;
     * use universal zero-copy wl_shm pipeline instead. */
    if (is_multigpu_system()) {
        return false;
    }

    /* CPU blitting into non-linear (tiled or DCC compressed) modifiers corrupts hardware tile
     * state and trips GPU resets. Only use direct scanout if modifier is linear. */
    if (state->dmabuf_ctx.preferred_modifier != DRM_FORMAT_MOD_LINEAR &&
        state->dmabuf_ctx.preferred_modifier != DRM_FORMAT_MOD_INVALID) {
        return false;
    }

    if (node->use_dmabuf && (int32_t)node->dmabuf_ring.width == width &&
        (int32_t)node->dmabuf_ring.height == height) {
        return true;
    }

    if (node->use_dmabuf) {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            render_engine_release_bo(&state->render_engine, &node->dmabuf_ring.buffers[i]);
        }
        dmabuf_ring_destroy(&state->dmabuf_ctx, &node->dmabuf_ring);
        node->use_dmabuf = false;
    }

    uint64_t modifier = state->dmabuf_ctx.preferred_modifier;
    if (dmabuf_ring_init(&state->dmabuf_ctx, &node->dmabuf_ring, (uint32_t)width, (uint32_t)height,
                         DRM_FORMAT_ARGB8888, modifier)) {
        node->use_dmabuf = true;
        WAYWAL_LOG_INFO("Direct Scanout active for output %s (%ux%u)", node->name, width, height);
        return true;
    }

    WAYWAL_LOG_WARN("Falling back to SHM rendering for output %s", node->name);
    return false;
}

static bool ensure_shm_buffer(daemon_state_t *state, output_node_t *node, int32_t width,
                              int32_t height)
{
    if (width <= 0 || height <= 0)
        return false;
    size_t stride    = (size_t)width * 4;
    size_t frame_sz  = stride * (size_t)height;
    size_t total_sz  = frame_sz * 2; /* double-sized: two frames back-to-back */

    /* Guard: wl_shm_create_pool takes int32_t size; ensure no overflow.
     * For reference: 8K double-buf = 253MB, well below 2GB limit. */
    if (total_sz > (size_t)INT32_MAX) {
        WAYWAL_LOG_ERR("Output %s: SHM double-buffer too large (%zu bytes > INT32_MAX)", node->name,
                       total_sz);
        return false;
    }

    if (node->shm_data && node->shm_size == total_sz &&
        node->shm_buffers[0] && node->shm_buffers[1]) {
        return true;
    }

    /* Tear down existing resources */
    for (int i = 0; i < 2; ++i) {
        if (node->shm_buffers[i]) {
            wl_buffer_destroy(node->shm_buffers[i]);
            node->shm_buffers[i] = NULL;
        }
        node->shm_buffer_released[i] = true;
    }
    if (node->shm_pool) {
        wl_shm_pool_destroy(node->shm_pool);
        node->shm_pool = NULL;
    }
    if (node->shm_data) {
        munmap(node->shm_data, node->shm_size);
        node->shm_data = NULL;
    }
    if (node->shm_fd >= 0) {
        close(node->shm_fd);
        node->shm_fd = -1;
    }
    node->active_buffer_idx = 0;

    /* Guard: wl_shm_create_pool takes int32_t size; ensure no overflow BEFORE
     * any allocation. For reference: 8K double-buf = 253MB, well below 2GB. */
    if (total_sz > (size_t)INT32_MAX) {
        WAYWAL_LOG_ERR("Output %s: SHM double-buffer too large (%zu bytes > INT32_MAX)", node->name,
                       total_sz);
        return false;
    }

    /* Allocate double-sized memfd */
    node->shm_fd = waywal_create_memfd("waywal-shm-buf", total_sz, 0);
    if (node->shm_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create memfd for output %s buffer", node->name);
        return false;
    }

    node->shm_data = mmap(NULL, total_sz, PROT_READ | PROT_WRITE, MAP_SHARED, node->shm_fd, 0);
    if (node->shm_data == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for output %s buffer", node->name);
        close(node->shm_fd);
        node->shm_fd = -1;
        node->shm_data = NULL;
        return false;
    }
    node->shm_size = total_sz;

    /* Create SHM pool over entire double-sized region */
    node->shm_pool = wl_shm_create_pool(state->shm, node->shm_fd, (int32_t)total_sz);
    if (!node->shm_pool) {
        WAYWAL_LOG_ERR("wl_shm_create_pool failed");
        return false;
    }

    /* Create two wl_buffers: buf[0] at offset 0, buf[1] at offset frame_sz */
    bool buf_ok = true;
    for (int i = 0; i < 2; ++i) {
        int32_t offset = (int32_t)(i * frame_sz);
        node->shm_buffers[i] = wl_shm_pool_create_buffer(
            node->shm_pool, offset, width, height,
            (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
        if (!node->shm_buffers[i]) {
            WAYWAL_LOG_ERR("wl_shm_pool_create_buffer[%d] failed", i);
            buf_ok = false;
            break;
        }
        wl_buffer_add_listener(node->shm_buffers[i], &shm_buffer_listener, node);
        node->shm_buffer_released[i] = true;
    }

    if (!buf_ok) {
        /* Partial failure: clean up everything allocated so far */
        for (int i = 0; i < 2; ++i) {
            if (node->shm_buffers[i]) {
                wl_buffer_destroy(node->shm_buffers[i]);
                node->shm_buffers[i] = NULL;
            }
        }
        wl_shm_pool_destroy(node->shm_pool);
        node->shm_pool = NULL;
        munmap(node->shm_data, node->shm_size);
        node->shm_data = NULL;
        node->shm_size = 0;
        close(node->shm_fd);
        node->shm_fd = -1;
        return false;
    }

    return true;
}

void output_node_render_color(daemon_state_t *state, output_node_t *node, color_rgba_t color)
{
    if (!state || !node || !node->surface)
        return;
    if (node->width <= 0 || node->height <= 0)
        return;

    uint32_t pixel = ((uint32_t)color.a << 24) | ((uint32_t)color.r << 16) |
                     ((uint32_t)color.g << 8) | (uint32_t)color.b;

    /* Preferred path: Hardware Direct Scanout via GBM DMA-BUF */
    if (ensure_dmabuf_ring(state, node, node->width, node->height)) {
        dmabuf_bo_t *bo = dmabuf_ring_acquire(&node->dmabuf_ring);
        if (bo && bo->wl_buffer) {
            uint32_t stride = 0;
            void *map_data = NULL;
            void *map = gbm_bo_map(bo->gbm_bo, 0, 0, (uint32_t)node->width, (uint32_t)node->height,
                                   GBM_BO_TRANSFER_WRITE, &stride, &map_data);
            if (map && map != MAP_FAILED) {
                for (int32_t y = 0; y < node->height; ++y) {
                    uint32_t *row = (uint32_t *)((uint8_t *)map + (size_t)y * stride);
                    for (int32_t x = 0; x < node->width; ++x) {
                        row[x] = pixel;
                    }
                }
                gbm_bo_unmap(bo->gbm_bo, map_data);

                if (node->viewport) {
                    wp_viewport_set_destination(node->viewport, node->width, node->height);
                    wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1));
                }
                wl_surface_attach(node->surface, bo->wl_buffer, 0, 0);
                wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
                output_commit_frame(node);
                wl_display_flush(state->display);
                return;
            }
        }
    }

    /* Fallback path: wl_shm */
    if (!ensure_shm_buffer(state, node, node->width, node->height)) {
        return;
    }

    /* Ping-pong: write to back buffer (not the one compositor is reading) */
    int back_idx = 1 - node->active_buffer_idx;
    size_t frame_sz = (size_t)node->width * (size_t)node->height * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)((uint8_t *)node->shm_data + back_idx * frame_sz);
    size_t count = (size_t)node->width * (size_t)node->height;
    for (size_t i = 0; i < count; ++i) {
        pixels[i] = pixel;
    }

    if (node->viewport) {
        wp_viewport_set_destination(node->viewport, node->width, node->height);
        wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
    }

    wl_surface_attach(node->surface, node->shm_buffers[back_idx], 0, 0);
    wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
    node->shm_buffer_released[back_idx] = false;
    node->active_buffer_idx = back_idx;
    output_commit_frame(node);
    wl_display_flush(state->display);
}

static inline uint32_t sample_bilinear(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                                       double sx, double sy)
{
    if (sx < 0.0)
        sx = 0.0;
    if (sy < 0.0)
        sy = 0.0;
    if (sx > (double)(src_w - 1))
        sx = (double)(src_w - 1);
    if (sy > (double)(src_h - 1))
        sy = (double)(src_h - 1);

    int32_t x0 = (int32_t)sx;
    int32_t y0 = (int32_t)sy;
    int32_t x1 = (x0 + 1 < (int32_t)src_w) ? (x0 + 1) : x0;
    int32_t y1 = (y0 + 1 < (int32_t)src_h) ? (y0 + 1) : y0;

    uint32_t fx = (uint32_t)((sx - (double)x0) * 256.0);
    uint32_t fy = (uint32_t)((sy - (double)y0) * 256.0);
    uint32_t ifx = 256 - fx;
    uint32_t ify = 256 - fy;

    uint32_t w00 = (ifx * ify) >> 8;
    uint32_t w10 = (fx * ify) >> 8;
    uint32_t w01 = (ifx * fy) >> 8;
    uint32_t w11 = (fx * fy) >> 8;

    uint32_t c00 = src[(size_t)y0 * src_w + (size_t)x0];
    uint32_t c10 = src[(size_t)y0 * src_w + (size_t)x1];
    uint32_t c01 = src[(size_t)y1 * src_w + (size_t)x0];
    uint32_t c11 = src[(size_t)y1 * src_w + (size_t)x1];

    uint32_t b = (((c00 & 0xFF) * w00) + ((c10 & 0xFF) * w10) + ((c01 & 0xFF) * w01) +
                  ((c11 & 0xFF) * w11)) >>
                 8;
    uint32_t g = ((((c00 >> 8) & 0xFF) * w00) + (((c10 >> 8) & 0xFF) * w10) +
                  (((c01 >> 8) & 0xFF) * w01) + (((c11 >> 8) & 0xFF) * w11)) >>
                 8;
    uint32_t r = ((((c00 >> 16) & 0xFF) * w00) + (((c10 >> 16) & 0xFF) * w10) +
                  (((c01 >> 16) & 0xFF) * w01) + (((c11 >> 16) & 0xFF) * w11)) >>
                 8;
    uint32_t a = ((((c00 >> 24) & 0xFF) * w00) + (((c10 >> 24) & 0xFF) * w10) +
                  (((c01 >> 24) & 0xFF) * w01) + (((c11 >> 24) & 0xFF) * w11)) >>
                 8;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void copy_or_scale_image(uint32_t *dst, uint32_t dst_w, uint32_t dst_h,
                                size_t dst_stride_bytes, const uint32_t *src, uint32_t src_w,
                                uint32_t src_h, uint32_t scaling_mode, color_rgba_t bg_color)
{
    if (!dst || !src || dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0)
        return;

    uint32_t bg_packed = ((uint32_t)bg_color.a << 24) | ((uint32_t)bg_color.r << 16) |
                         ((uint32_t)bg_color.g << 8) | (uint32_t)bg_color.b;

    if (dst_w == src_w && dst_h == src_h && scaling_mode != WAYWAL_SCALING_TILE) {
        for (uint32_t y = 0; y < dst_h; ++y) {
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            memcpy(dst_row, src + (size_t)y * src_w, (size_t)src_w * sizeof(uint32_t));
        }
        return;
    }

    switch ((waywal_scaling_mode_t)scaling_mode) {
    case WAYWAL_SCALING_FILL: {
        /* Crop to fill / cover preserving aspect ratio, centered */
        double r_src = (double)src_w / (double)src_h;
        double r_dst = (double)dst_w / (double)dst_h;
        double crop_w, crop_h, off_x, off_y;

        if (r_src > r_dst) {
            crop_h = (double)src_h;
            crop_w = crop_h * r_dst;
            off_x = ((double)src_w - crop_w) * 0.5;
            off_y = 0.0;
        } else {
            crop_w = (double)src_w;
            crop_h = crop_w / r_dst;
            off_x = 0.0;
            off_y = ((double)src_h - crop_h) * 0.5;
        }

        for (uint32_t y = 0; y < dst_h; ++y) {
            double sy = off_y + ((double)y + 0.5) * crop_h / (double)dst_h;
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            for (uint32_t x = 0; x < dst_w; ++x) {
                double sx = off_x + ((double)x + 0.5) * crop_w / (double)dst_w;
                dst_row[x] = sample_bilinear(src, src_w, src_h, sx, sy);
            }
        }
        break;
    }

    case WAYWAL_SCALING_FIT: {
        /* Fit entire image preserving aspect ratio with letterbox/pillarbox */
        double r_src = (double)src_w / (double)src_h;
        double r_dst = (double)dst_w / (double)dst_h;

        if (r_src > r_dst) {
            /* Wider than screen: full width, letterbox top/bottom */
            uint32_t scaled_h = (uint32_t)((double)dst_w / r_src + 0.5);
            if (scaled_h > dst_h)
                scaled_h = dst_h;
            uint32_t pad_y = (dst_h - scaled_h) / 2;

            for (uint32_t y = 0; y < dst_h; ++y) {
                uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
                if (y < pad_y || y >= pad_y + scaled_h) {
                    for (uint32_t x = 0; x < dst_w; ++x)
                        dst_row[x] = bg_packed;
                } else {
                    double sy = ((double)(y - pad_y) + 0.5) * (double)src_h / (double)scaled_h;
                    for (uint32_t x = 0; x < dst_w; ++x) {
                        double sx = ((double)x + 0.5) * (double)src_w / (double)dst_w;
                        dst_row[x] = sample_bilinear(src, src_w, src_h, sx, sy);
                    }
                }
            }
        } else {
            /* Taller than screen: full height, pillarbox left/right */
            uint32_t scaled_w = (uint32_t)((double)dst_h * r_src + 0.5);
            if (scaled_w > dst_w)
                scaled_w = dst_w;
            uint32_t pad_x = (dst_w - scaled_w) / 2;

            for (uint32_t y = 0; y < dst_h; ++y) {
                uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
                double sy = ((double)y + 0.5) * (double)src_h / (double)dst_h;
                for (uint32_t x = 0; x < dst_w; ++x) {
                    if (x < pad_x || x >= pad_x + scaled_w) {
                        dst_row[x] = bg_packed;
                    } else {
                        double sx = ((double)(x - pad_x) + 0.5) * (double)src_w / (double)scaled_w;
                        dst_row[x] = sample_bilinear(src, src_w, src_h, sx, sy);
                    }
                }
            }
        }
        break;
    }

    case WAYWAL_SCALING_STRETCH: {
        for (uint32_t y = 0; y < dst_h; ++y) {
            double sy = ((double)y + 0.5) * (double)src_h / (double)dst_h;
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            for (uint32_t x = 0; x < dst_w; ++x) {
                double sx = ((double)x + 0.5) * (double)src_w / (double)dst_w;
                dst_row[x] = sample_bilinear(src, src_w, src_h, sx, sy);
            }
        }
        break;
    }

    case WAYWAL_SCALING_CENTER: {
        int32_t off_x = ((int32_t)dst_w - (int32_t)src_w) / 2;
        int32_t off_y = ((int32_t)dst_h - (int32_t)src_h) / 2;
        for (uint32_t y = 0; y < dst_h; ++y) {
            int32_t sy = (int32_t)y - off_y;
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            for (uint32_t x = 0; x < dst_w; ++x) {
                int32_t sx = (int32_t)x - off_x;
                if (sx >= 0 && sx < (int32_t)src_w && sy >= 0 && sy < (int32_t)src_h) {
                    dst_row[x] = src[(size_t)sy * src_w + (size_t)sx];
                } else {
                    dst_row[x] = bg_packed;
                }
            }
        }
        break;
    }

    case WAYWAL_SCALING_TILE: {
        for (uint32_t y = 0; y < dst_h; ++y) {
            uint32_t sy = y % src_h;
            const uint32_t *src_row = src + (size_t)sy * src_w;
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            for (uint32_t x = 0; x < dst_w; ++x) {
                uint32_t sx = x % src_w;
                dst_row[x] = src_row[sx];
            }
        }
        break;
    }
    }
}

void output_node_render_image(daemon_state_t *state, output_node_t *node, const uint8_t *src_pixels,
                              uint32_t img_w, uint32_t img_h)
{
    if (!state || !node || !node->surface || !src_pixels)
        return;
    if (node->width <= 0 || node->height <= 0 || img_w == 0 || img_h == 0)
        return;

    const uint32_t *src = (const uint32_t *)src_pixels;

    if (node->viewport) {
        wp_viewport_set_destination(node->viewport, node->width, node->height);
        wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
    }

    /* Preferred path: Hardware Direct Scanout & Plane Scaling */
    if (ensure_dmabuf_ring(state, node, node->width, node->height)) {
        dmabuf_bo_t *bo = dmabuf_ring_acquire(&node->dmabuf_ring);
        if (bo && bo->wl_buffer) {
            uint32_t stride = 0;
            void *map_data = NULL;
            void *map = gbm_bo_map(bo->gbm_bo, 0, 0, (uint32_t)node->width, (uint32_t)node->height,
                                   GBM_BO_TRANSFER_WRITE, &stride, &map_data);
            if (map && map != MAP_FAILED) {
                copy_or_scale_image((uint32_t *)map, (uint32_t)node->width, (uint32_t)node->height,
                                    stride, src, img_w, img_h, node->scaling_mode,
                                    state->current_color);
                gbm_bo_unmap(bo->gbm_bo, map_data);

                wl_surface_attach(node->surface, bo->wl_buffer, 0, 0);
                wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
                output_commit_frame(node);
                node->has_image = true;
                wl_display_flush(state->display);
                return;
            }
        }
    }

    /* Fallback SHM rendering path */
    if (ensure_shm_buffer(state, node, node->width, node->height)) {
        int back_idx = 1 - node->active_buffer_idx;
        size_t frame_sz = (size_t)node->width * (size_t)node->height * sizeof(uint32_t);
        void *back_ptr  = (uint8_t *)node->shm_data + back_idx * frame_sz;
        copy_or_scale_image((uint32_t *)back_ptr, (uint32_t)node->width,
                            (uint32_t)node->height, (size_t)node->width * sizeof(uint32_t), src,
                            img_w, img_h, node->scaling_mode, state->current_color);
        wl_surface_attach(node->surface, node->shm_buffers[back_idx], 0, 0);
        wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
        node->shm_buffer_released[back_idx] = false;
        node->active_buffer_idx = back_idx;
        output_commit_frame(node);
        node->has_image = true;
        wl_display_flush(state->display);
    }
}

void daemon_clear_all_outputs(daemon_state_t *state, color_rgba_t color)
{
    if (!state)
        return;
    state->current_color = color;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        output_node_render_color(state, out, color);
    }
}

static bool ensure_shm_transition_buffers(output_node_t *node)
{
    size_t needed = (size_t)node->width * (size_t)node->height * sizeof(uint32_t);
    if (node->shm_buffer_cap < needed) {
        free(node->shm_old_data);
        free(node->shm_new_data);
        node->shm_old_data = malloc(needed);
        node->shm_new_data = malloc(needed);
        if (!node->shm_old_data || !node->shm_new_data) {
            free(node->shm_old_data);
            free(node->shm_new_data);
            node->shm_old_data = NULL;
            node->shm_new_data = NULL;
            node->shm_buffer_cap = 0;
            return false;
        }
        node->shm_buffer_cap = needed;
    }
    return true;
}

bool transition_engine_init(daemon_state_t *state)
{
    if (!state)
        return false;
    state->transition_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state->transition_timer_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create transition timerfd: %s", strerror(errno));
        return false;
    }
    state->transitions_in_progress = false;
    WAYWAL_LOG_INFO("Asynchronous transition engine initialized (timerfd=%d)",
                    state->transition_timer_fd);
    return true;
}

void transition_engine_destroy(daemon_state_t *state)
{
    if (!state)
        return;
    if (state->transition_timer_fd >= 0) {
        close(state->transition_timer_fd);
        state->transition_timer_fd = -1;
    }
    state->transitions_in_progress = false;
}

static bool is_target_output(const output_node_t *node, const waywal_img_metadata_t *meta)
{
    if (!meta || meta->num_target_outputs == 0)
        return true;

    const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
    for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
        if (strcmp(node->name, ptr) == 0)
            return true;
        ptr += strlen(ptr) + 1;
    }
    return false;
}

bool daemon_start_image_transition(daemon_state_t *state, const uint8_t *pixels, uint32_t img_w,
                                   uint32_t img_h, const waywal_img_metadata_t *meta)
{
    if (!state || !pixels || img_w == 0 || img_h == 0)
        return false;

    /* If custom shader provided, load it into compute engine */
    if (meta && meta->transition_type == WAYWAL_TRANSITION_CUSTOM && meta->custom_shader_len > 0) {
        const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
        for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
            ptr += strlen(ptr) + 1;
        }
        render_engine_load_custom_shader(&state->render_engine, ptr);
    }

    /* ── Priority 5: Preempt any in-progress transition cleanly ─────────────────
     * Snapshot the last rendered pixel data into shm_old_data so the new
     * transition starts from the visible frame, not from stale shm_new_data. */
    for (output_node_t *pout = state->outputs; pout != NULL; pout = pout->next) {
        if (!pout->transition_active)
            continue;
        /* Copy the currently committed (back) buffer into old staging */
        if (pout->shm_data && pout->shm_old_data && pout->width > 0 && pout->height > 0) {
            size_t fsz = (size_t)pout->width * (size_t)pout->height * sizeof(uint32_t);
            void *displayed = (uint8_t *)pout->shm_data + pout->active_buffer_idx * fsz;
            memcpy(pout->shm_old_data, displayed, fsz);
        }
        pout->transition_active = false;
    }
    /* Drain any stale timerfd expirations left over from the cancelled transition */
    if (state->transition_timer_fd >= 0) {
        uint64_t dummy;
        while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
    }
    state->transitions_in_progress = false;

    if (!meta || meta->transition_type == WAYWAL_TRANSITION_NONE ||
        meta->transition_type == WAYWAL_TRANSITION_SIMPLE || meta->transition_duration_ms == 0) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!is_target_output(out, meta))
                continue;
            out->scaling_mode = meta ? meta->scaling_mode : WAYWAL_SCALING_FILL;
            output_node_render_image(state, out, pixels, img_w, img_h);
        }
        return true;
    }

    uint32_t fps = meta->transition_fps > 0 ? meta->transition_fps : 60;
    uint32_t num_frames = (meta->transition_duration_ms * fps) / 1000;
    if (num_frames < 2)
        num_frames = 2;
    uint64_t frame_interval_ns = 1000000000ULL / fps;

    uint32_t stagger_delay_ms = meta->stagger_delay_ms > 0 ? meta->stagger_delay_ms : 150;
    uint32_t stagger_frames = (stagger_delay_ms * fps) / 1000;

    const uint32_t *src = (const uint32_t *)pixels;
    int started_count = 0;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->width <= 0 || out->height <= 0)
            continue;
        if (!is_target_output(out, meta))
            continue;

        out->scaling_mode = meta ? meta->scaling_mode : WAYWAL_SCALING_FILL;

        if (out->viewport) {
            wp_viewport_set_destination(out->viewport, out->width, out->height);
            wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                   wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        }

        bool dmabuf_ok = false;
        if (ensure_dmabuf_ring(state, out, out->width, out->height)) {
            uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
            if (!out->has_source_new_bo || out->source_new_bo.width != (uint32_t)out->width ||
                out->source_new_bo.height != (uint32_t)out->height) {
                if (out->has_source_new_bo) {
                    render_engine_release_bo(&state->render_engine, &out->source_new_bo);
                    dmabuf_bo_free(&out->source_new_bo);
                    out->has_source_new_bo = false;
                }
                if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_new_bo,
                                       (uint32_t)out->width, (uint32_t)out->height,
                                       DRM_FORMAT_ARGB8888, modifier)) {
                    out->has_source_new_bo = true;
                }
            }

            if (out->has_source_new_bo) {
                uint32_t stride_new = 0;
                void *map_data_new = NULL;
                void *map_new = gbm_bo_map(out->source_new_bo.gbm_bo, 0, 0, (uint32_t)out->width,
                                           (uint32_t)out->height, GBM_BO_TRANSFER_WRITE,
                                           &stride_new, &map_data_new);
                if (map_new && map_new != MAP_FAILED) {
                    copy_or_scale_image((uint32_t *)map_new, (uint32_t)out->width,
                                        (uint32_t)out->height, stride_new, src, img_w, img_h,
                                        out->scaling_mode, state->current_color);
                    gbm_bo_unmap(out->source_new_bo.gbm_bo, map_data_new);

                    if (!out->has_source_old_bo ||
                        out->source_old_bo.width != (uint32_t)out->width ||
                        out->source_old_bo.height != (uint32_t)out->height) {
                        if (out->has_source_old_bo) {
                            render_engine_release_bo(&state->render_engine, &out->source_old_bo);
                            dmabuf_bo_free(&out->source_old_bo);
                            out->has_source_old_bo = false;
                        }
                        if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_old_bo,
                                               (uint32_t)out->width, (uint32_t)out->height,
                                               DRM_FORMAT_ARGB8888, modifier)) {
                            out->has_source_old_bo = true;
                            uint32_t stride_old = 0;
                            void *map_data_old = NULL;
                            void *map_old =
                                gbm_bo_map(out->source_old_bo.gbm_bo, 0, 0, (uint32_t)out->width,
                                           (uint32_t)out->height, GBM_BO_TRANSFER_WRITE,
                                           &stride_old, &map_data_old);
                            if (map_old && map_old != MAP_FAILED) {
                                uint32_t col = ((uint32_t)state->current_color.a << 24) |
                                               ((uint32_t)state->current_color.r << 16) |
                                               ((uint32_t)state->current_color.g << 8) |
                                               (uint32_t)state->current_color.b;
                                for (int32_t y = 0; y < out->height; ++y) {
                                    uint32_t *dst_row =
                                        (uint32_t *)((uint8_t *)map_old + (size_t)y * stride_old);
                                    for (int32_t x = 0; x < out->width; ++x) {
                                        dst_row[x] = col;
                                    }
                                }
                                gbm_bo_unmap(out->source_old_bo.gbm_bo, map_data_old);
                            }
                        }
                    }
                    dmabuf_ok = true;
                }
            }
        }

        if (!dmabuf_ok) {
            out->use_dmabuf = false;
            if (!ensure_shm_buffer(state, out, out->width, out->height) ||
                !ensure_shm_transition_buffers(out)) {
                output_node_render_image(state, out, pixels, img_w, img_h);
                continue;
            }
            if (out->has_image && out->shm_data) {
                /* Snapshot the currently committed (front) buffer as the old frame */
                size_t fsz = (size_t)out->width * (size_t)out->height * sizeof(uint32_t);
                void *front_ptr = (uint8_t *)out->shm_data + out->active_buffer_idx * fsz;
                memcpy(out->shm_old_data, front_ptr, fsz);
            } else {
                uint32_t col = ((uint32_t)state->current_color.a << 24) |
                               ((uint32_t)state->current_color.r << 16) |
                               ((uint32_t)state->current_color.g << 8) |
                               (uint32_t)state->current_color.b;
                uint32_t *old_u32 = (uint32_t *)out->shm_old_data;
                size_t total_px = (size_t)out->width * (size_t)out->height;
                for (size_t p = 0; p < total_px; ++p) {
                    old_u32[p] = col;
                }
            }
            copy_or_scale_image((uint32_t *)out->shm_new_data, (uint32_t)out->width,
                                (uint32_t)out->height, (size_t)out->width * sizeof(uint32_t), src,
                                img_w, img_h, out->scaling_mode, state->current_color);
            /* GPU-SHM path removed: OpenMP CPU engine is 50x faster than glReadPixels */
        }

        float cx = 0.5f;
        float cy = 0.5f;
        if (meta->transition_center_x < 0.0f || meta->transition_center_y < 0.0f) {
            /* Dynamic cursor tracking */
            double qx = 0, qy = 0;
            if (daemon_query_cursor_pos(state, out, &qx, &qy) && out->width > 0 &&
                out->height > 0) {
                cx = (float)(qx / (double)out->width);
                cy = (float)(qy / (double)out->height);
            }
        } else {
            cx = meta->transition_center_x;
            cy = meta->transition_center_y;
        }

        float wfreq = meta->transition_wave_freq > 0.0f ? meta->transition_wave_freq : 20.0f;
        float wamp = meta->transition_wave_amp > 0.0f ? meta->transition_wave_amp : 0.05f;

        out->transition_params = (waywal_transition_params_t){
            .type = (waywal_transition_type_t)meta->transition_type,
            .progress = 0.0f,
            .angle_rad = meta->transition_angle_rad,
            .wave_freq = wfreq,
            .wave_amp = wamp,
            .center_x = cx,
            .center_y = cy,
            .bezier = BEZIER_DEFAULT,
        };
        out->transition_current_frame = 0;
        out->transition_total_frames = num_frames;
        out->transition_delay_frames =
            (meta->sync_mode == 1) ? (started_count * stagger_frames) : 0;
        out->transition_active = true;
        started_count++;
    }

    if (started_count == 0)
        return false;

    struct itimerspec its = {.it_interval = {.tv_sec = (time_t)(frame_interval_ns / 1000000000ULL),
                                             .tv_nsec = (long)(frame_interval_ns % 1000000000ULL)},
                             .it_value = {.tv_sec = (time_t)(frame_interval_ns / 1000000000ULL),
                                          .tv_nsec = (long)(frame_interval_ns % 1000000000ULL)}};
    if (state->transition_timer_fd >= 0) {
        uint64_t dummy;
        while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0);
        timerfd_settime(state->transition_timer_fd, 0, &its, NULL);
    }
    state->transitions_in_progress = true;
    WAYWAL_LOG_INFO("Transition started: type=%u, duration=%ums, frames=%u, fps=%u",
                    meta->transition_type, meta->transition_duration_ms, num_frames, fps);
    return true;
}

void transition_engine_dispatch_tick(daemon_state_t *state)
{
    if (!state || state->transition_timer_fd < 0)
        return;

    uint64_t expirations = 0;
    ssize_t s = read(state->transition_timer_fd, &expirations, sizeof(expirations));
    if (s <= 0 || expirations == 0)
        return;

    int active_count = 0;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->transition_active)
            continue;

        if (out->transition_delay_frames > 0) {
            out->transition_delay_frames--;
            active_count++;
            continue;
        }

        out->transition_current_frame++;
        float t = (float)out->transition_current_frame / (float)out->transition_total_frames;
        if (t > 1.0f)
            t = 1.0f;
        out->transition_params.progress = bezier_eval(&out->transition_params.bezier, t);

        if (out->use_dmabuf) {
            dmabuf_bo_t *target_bo = dmabuf_ring_acquire(&out->dmabuf_ring);
            if (target_bo && target_bo->wl_buffer) {
                if (render_engine_execute_transition(&state->render_engine, target_bo,
                                                     &out->source_old_bo, &out->source_new_bo,
                                                     &out->transition_params)) {
                    wl_surface_attach(out->surface, target_bo->wl_buffer, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                    output_commit_frame(out);
                }
            }
        } else {
            /* SHM ping-pong path: render into back buffer, never touch the front */
            if (out->shm_buffers[0] && out->shm_buffers[1] &&
                out->shm_data && out->shm_old_data && out->shm_new_data) {
                int back_idx = 1 - out->active_buffer_idx;

                /* Safety: if compositor hasn't released the back buffer yet, skip frame */
                if (!out->shm_buffer_released[back_idx]) {
                    active_count++;
                    continue;
                }

                struct timespec ts0, ts1;
                clock_gettime(CLOCK_MONOTONIC, &ts0);

                size_t frame_sz = (size_t)out->width * (size_t)out->height * sizeof(uint32_t);
                void *back_ptr  = (uint8_t *)out->shm_data + back_idx * frame_sz;

                /* OpenMP-parallelized CPU transition (16 threads, ~5–18ms for complex effects) */
                render_engine_execute_cpu_transition(
                    (uint32_t *)back_ptr,
                    (const uint32_t *)out->shm_old_data,
                    (const uint32_t *)out->shm_new_data,
                    (uint32_t)out->width, (uint32_t)out->height,
                    (uint32_t)out->width * sizeof(uint32_t),
                    &out->transition_params);

                clock_gettime(CLOCK_MONOTONIC, &ts1);
                double render_ms = ((ts1.tv_sec - ts0.tv_sec) * 1000.0) +
                                   ((ts1.tv_nsec - ts0.tv_nsec) / 1000000.0);

                /* Commit back buffer; swap front/back indices */
                wl_surface_attach(out->surface, out->shm_buffers[back_idx], 0, 0);
                wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                out->shm_buffer_released[back_idx] = false;
                out->active_buffer_idx = back_idx;
                output_commit_frame(out);

                if (out->transition_current_frame <= 2 || out->transition_current_frame % 20 == 0) {
                    WAYWAL_LOG_INFO("Tick frame %u out %s: render=%.2fms (OpenMP CPU)",
                                    out->transition_current_frame, out->name, render_ms);
                }
            }
        }

        if (out->transition_current_frame >= out->transition_total_frames) {
            out->transition_active = false;
            if (out->use_dmabuf) {
                dmabuf_bo_t tmp = out->source_old_bo;
                out->source_old_bo = out->source_new_bo;
                out->source_new_bo = tmp;
            } else if (out->shm_old_data && out->shm_new_data) {
                /* Promote new frame -> old so next transition starts from correct state */
                void *tmp = out->shm_old_data;
                out->shm_old_data = out->shm_new_data;
                out->shm_new_data = tmp;
            }
            out->has_image = true;
        } else {
            active_count++;
        }
    }

    wl_display_flush(state->display);

    if (active_count == 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        timerfd_settime(state->transition_timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0);
        state->transitions_in_progress = false;
        WAYWAL_LOG_INFO("Transition completed successfully across active outputs");
    }
}

void output_node_render_image_with_transition(daemon_state_t *state, output_node_t *node,
                                              const uint8_t *src_pixels, uint32_t img_w,
                                              uint32_t img_h, const waywal_img_metadata_t *meta)
{
    (void)node;
    daemon_start_image_transition(state, src_pixels, img_w, img_h, meta);
}
