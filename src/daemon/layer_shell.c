// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gpu_compute.h"
#include "wayland_core.h"
#include "waywal/anim_engine.h"
#include "waywal/bezier.h"
#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/shader_engine.h"
#include "waywal/uring_loop.h"
#include "waywal/we_scene.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
#include <immintrin.h>
#endif

static inline void output_commit_frame(output_node_t *node)
{
    if (!node || !node->surface)
        return;
    if (node->pres_sync.wp_pres) {
        presentation_sync_request(&node->pres_sync);
    }
    wl_surface_commit(node->surface);
}

/* SHM ping-pong buffer release listener: tracks when compositor releases a buffer */
static void shm_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    output_node_t *node = (output_node_t *)data;
    if (!node)
        return;
    for (int i = 0; i < 2; ++i) {
        if (node->shm_buffers[i] == wl_buffer) {
            node->shm_buffer_released[i] = true;
            bool video_playing =
                node->state && node->state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED;
            if (!node->transition_active && (i != node->active_buffer_idx || video_playing) &&
                node->shm_data && node->shm_size >= 8) {
                size_t frame_sz = node->shm_size / 2;
                void *inactive_ptr = (uint8_t *)node->shm_data + (size_t)i * frame_sz;
                madvise(inactive_ptr, frame_sz, MADV_REMOVE);
            }
            break;
        }
    }
}

static const struct wl_buffer_listener shm_buffer_listener = {
    .release = shm_buffer_release,
};

static bool recompute_output_dimensions(output_node_t *out)
{
    if (!out || out->logical_width == 0 || out->logical_height == 0)
        return false;

    int32_t old_w = out->width;
    int32_t old_h = out->height;

    uint32_t phys_w = out->logical_width;
    uint32_t phys_h = out->logical_height;
    if (out->fractional_scale > 0) {
        phys_w = (out->logical_width * (uint32_t)out->fractional_scale + 60u) / 120u;
        phys_h = (out->logical_height * (uint32_t)out->fractional_scale + 60u) / 120u;
    } else if (out->scale > 1) {
        phys_w = out->logical_width * (uint32_t)out->scale;
        phys_h = out->logical_height * (uint32_t)out->scale;
    } else if (out->mode_width > 0 && out->mode_height > 0) {
        bool rotated = (out->transform == WL_OUTPUT_TRANSFORM_90 ||
                        out->transform == WL_OUTPUT_TRANSFORM_270 ||
                        out->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                        out->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);
        uint32_t mw = (uint32_t)(rotated ? out->mode_height : out->mode_width);
        uint32_t mh = (uint32_t)(rotated ? out->mode_width : out->mode_height);
        if (mw >= out->logical_width && mh >= out->logical_height) {
            phys_w = mw;
            phys_h = mh;
        }
    }

    out->width = (int32_t)phys_w;
    out->height = (int32_t)phys_h;
    return (out->width != old_w || out->height != old_h);
}

static void fractional_scale_preferred_scale(void *data,
                                             struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
                                             uint32_t scale)
{
    (void)wp_fractional_scale_v1;
    output_node_t *out = (output_node_t *)data;
    if (!out || scale == 0)
        return;

    out->fractional_scale = (int32_t)scale;
    if (recompute_output_dimensions(out)) {
        out->vp_configured = false;
    }
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_preferred_scale,
};

static void backdrop_layer_surface_configure(void *data,
                                             struct zwlr_layer_surface_v1 *layer_surface,
                                             uint32_t serial, uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    output_node_t *node = (output_node_t *)data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    if (node) {
        node->backdrop_configured = true;
        if (node->has_backdrop_bo && node->backdrop_bo.wl_buffer && node->backdrop_surface) {
            if (node->backdrop_viewport) {
                int32_t dst_w =
                    node->logical_width > 0 ? (int32_t)node->logical_width : node->width;
                int32_t dst_h =
                    node->logical_height > 0 ? (int32_t)node->logical_height : node->height;
                wp_viewport_set_destination(node->backdrop_viewport, dst_w, dst_h);
            }
            wl_surface_attach(node->backdrop_surface, node->backdrop_bo.wl_buffer, 0, 0);
            wl_surface_damage_buffer(node->backdrop_surface, 0, 0, node->width, node->height);
            wl_surface_commit(node->backdrop_surface);
        }
    }
}

static void backdrop_layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)layer_surface;
    output_node_t *node = (output_node_t *)data;
    if (!node)
        return;
    node->backdrop_configured = false;
    if (node->backdrop_viewport) {
        wp_viewport_destroy(node->backdrop_viewport);
        node->backdrop_viewport = NULL;
    }
    if (node->backdrop_layer_surface) {
        zwlr_layer_surface_v1_destroy(node->backdrop_layer_surface);
        node->backdrop_layer_surface = NULL;
    }
    if (node->backdrop_surface) {
        wl_surface_destroy(node->backdrop_surface);
        node->backdrop_surface = NULL;
    }
}

static const struct zwlr_layer_surface_v1_listener backdrop_layer_surface_listener = {
    .configure = backdrop_layer_surface_configure,
    .closed = backdrop_layer_surface_closed,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial, uint32_t width, uint32_t height)
{
    output_node_t *out = (output_node_t *)data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    bool was_configured = out->configured;
    int32_t old_w = out->width;
    int32_t old_h = out->height;
    if (width > 0 && height > 0) {
        out->logical_width = width;
        out->logical_height = height;
        (void)recompute_output_dimensions(out);
    }
    if (out->viewport && out->logical_width > 0 && out->logical_height > 0) {
        wp_viewport_set_destination(out->viewport, (int32_t)out->logical_width,
                                    (int32_t)out->logical_height);
    }
    out->configured = true;
    out->vp_configured = false;

    WAYWAL_LOG_DEBUG("Output %s (%s) configured: logical=%ux%u physical=%dx%d", out->name,
                     out->description, out->logical_width, out->logical_height, out->width,
                     out->height);

    if (!was_configured) {
        output_node_render_color(out->state, out, out->state->current_color);
        if (out->state && out->state->on_output_configured) {
            out->state->on_output_configured(out->state, out);
        }
    } else {
        bool dims_changed = (out->width != old_w || out->height != old_h);
        bool anim_active =
            out->transition_active || out->shader_active || out->anim_active ||
            out->workshop_active ||
            (out->video_eng && out->video_eng->state != WAYWAL_VIDEO_STATE_STOPPED) ||
            (out->state &&
             (out->state->shader_active || out->state->anim_active || out->state->workshop_active ||
              out->state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED));
        if (dims_changed && !anim_active && out->state) {
            if (out->has_image && out->state->wallpaper_rgba) {
                output_node_render_image(out->state, out, out->state->wallpaper_rgba,
                                         out->state->wallpaper_w, out->state->wallpaper_h);
                if (out->state->on_output_configured) {
                    out->state->on_output_configured(out->state, out);
                }
            } else if (out->has_image && out->state->on_output_configured) {
                out->state->on_output_configured(out->state, out);
            } else if (!out->has_image) {
                color_rgba_t col =
                    out->has_fill_color ? out->fill_color : out->state->current_color;
                output_node_render_color(out->state, out, col);
            } else {
                output_commit_frame(out);
            }
        } else {
            output_commit_frame(out);
        }
        if (out->state && out->state->display) {
            wl_display_flush(out->state->display);
        }
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

void layer_shell_update_input_regions(daemon_state_t *state, bool interactive)
{
    if (!state || !state->compositor)
        return;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->surface)
            continue;
        if (out->layer_surface) {
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                out->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        }
        struct wl_region *region = wl_compositor_create_region(state->compositor);
        if (region) {
            if (interactive) {
                wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
            }
            wl_surface_set_input_region(out->surface, region);
            wl_region_destroy(region);
        }
        if (out->configured) {
            wl_surface_commit(out->surface);
        }
    }

    if (state->display) {
        wl_display_flush(state->display);
    }
}

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

    const char *ns = state->namespace_str[0] ? state->namespace_str : "waywal";
    uint32_t layer = state->use_bottom_layer ? ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM
                                             : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
    node->layer_surface = zwlr_layer_shell_v1_get_layer_surface(state->layer_shell, node->surface,
                                                                node->wl_output, layer, ns);
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

    /* Dynamic input region: full-surface when interactive shader/depth-map active, empty otherwise
     */
    struct wl_region *input_region = wl_compositor_create_region(state->compositor);
    if (input_region) {
        if (state->shader_active) {
            wl_region_add(input_region, 0, 0, INT32_MAX, INT32_MAX);
        }
        wl_surface_set_input_region(node->surface, input_region);
        wl_region_destroy(input_region);
    }

    struct wl_region *opaque_region = wl_compositor_create_region(state->compositor);
    if (opaque_region) {
        wl_region_add(opaque_region, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_set_opaque_region(node->surface, opaque_region);
        wl_region_destroy(opaque_region);
    }

    zwlr_layer_surface_v1_add_listener(node->layer_surface, &layer_surface_listener, node);

    if (state->viewporter) {
        node->viewport = wp_viewporter_get_viewport(state->viewporter, node->surface);
    }

    if (state->fract_manager && !node->fract_scale) {
        node->fract_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            state->fract_manager, node->surface);
        if (node->fract_scale) {
            wp_fractional_scale_v1_add_listener(node->fract_scale, &fractional_scale_listener,
                                                node);
        }
    }

    if (state->presentation) {
        presentation_sync_init(&node->pres_sync, state->presentation, node->surface);
    }

    wl_surface_commit(node->surface);

    if (state->backdrop_auto_blur && !node->backdrop_surface) {
        node->backdrop_surface = wl_compositor_create_surface(state->compositor);
        if (node->backdrop_surface) {
            node->backdrop_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
                state->layer_shell, node->backdrop_surface, node->wl_output,
                ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "waywal-backdrop");
            if (node->backdrop_layer_surface) {
                zwlr_layer_surface_v1_set_anchor(
                    node->backdrop_layer_surface,
                    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
                zwlr_layer_surface_v1_set_exclusive_zone(node->backdrop_layer_surface, -1);
                zwlr_layer_surface_v1_set_keyboard_interactivity(
                    node->backdrop_layer_surface,
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
                zwlr_layer_surface_v1_set_size(node->backdrop_layer_surface, 0, 0);
                struct wl_region *bd_empty = wl_compositor_create_region(state->compositor);
                if (bd_empty) {
                    wl_surface_set_input_region(node->backdrop_surface, bd_empty);
                    wl_region_destroy(bd_empty);
                }
                zwlr_layer_surface_v1_add_listener(node->backdrop_layer_surface,
                                                   &backdrop_layer_surface_listener, node);
                if (state->viewporter) {
                    node->backdrop_viewport =
                        wp_viewporter_get_viewport(state->viewporter, node->backdrop_surface);
                }
                wl_surface_commit(node->backdrop_surface);
            } else {
                wl_surface_destroy(node->backdrop_surface);
                node->backdrop_surface = NULL;
            }
        }
    }

    wl_display_flush(state->display);
}

static void release_output_bo_refs(daemon_state_t *state, output_node_t *node, dmabuf_bo_t *bo)
{
    if (!bo) {
        return;
    }
    if (node) {
        if (node->last_presented_bo == bo) {
            node->last_presented_bo = NULL;
        }
        if (node->shader_eng) {
            waywal_shader_engine_forget_bo(node->shader_eng, bo);
        }
        if (node->we_scene) {
            waywal_we_scene_forget_bo(node->we_scene, bo);
        }
    }
    if (state) {
        if (state->shader_eng && (!node || state->shader_eng != node->shader_eng)) {
            waywal_shader_engine_forget_bo(state->shader_eng, bo);
        }
        if (state->we_scene && (!node || state->we_scene != node->we_scene)) {
            waywal_we_scene_forget_bo(state->we_scene, bo);
        }
        for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
            if (o == node) {
                continue;
            }
            if (o->shader_eng && o->shader_eng != state->shader_eng &&
                (!node || o->shader_eng != node->shader_eng)) {
                waywal_shader_engine_forget_bo(o->shader_eng, bo);
            }
            if (o->we_scene && o->we_scene != state->we_scene &&
                (!node || o->we_scene != node->we_scene)) {
                waywal_we_scene_forget_bo(o->we_scene, bo);
            }
        }
        render_engine_release_bo(&state->render_engine, bo);
    }
}

void output_node_destroy_surface(output_node_t *node)
{
    if (!node)
        return;
    node->vp_configured = false;
    node->last_presented_bo = NULL;

    if (node->use_dmabuf) {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            release_output_bo_refs(node->state, node, &node->dmabuf_ring.buffers[i]);
        }
        dmabuf_ring_destroy(node->state ? &node->state->dmabuf_ctx : NULL, &node->dmabuf_ring);
        node->use_dmabuf = false;
    }
    if (node->has_source_old_bo) {
        release_output_bo_refs(node->state, node, &node->source_old_bo);
        dmabuf_bo_free(&node->source_old_bo);
        node->has_source_old_bo = false;
    }
    if (node->has_source_new_bo) {
        release_output_bo_refs(node->state, node, &node->source_new_bo);
        dmabuf_bo_free(&node->source_new_bo);
        node->has_source_new_bo = false;
    }
    if (node->has_backdrop_bo) {
        release_output_bo_refs(node->state, node, &node->backdrop_bo);
        dmabuf_bo_free(&node->backdrop_bo);
        node->has_backdrop_bo = false;
    }

    if (node->we_scene) {
        bool shared = false;
        if (node->state) {
            for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                if (o != node && o->we_scene == node->we_scene) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared) {
            if (node->state && node->state->we_scene == node->we_scene) {
                node->state->we_scene = NULL;
                for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                    if (o != node && o->we_scene != NULL) {
                        node->state->we_scene = o->we_scene;
                        break;
                    }
                }
            }
            we_scene_destroy(node->we_scene);
        }
        node->we_scene = NULL;
        node->workshop_active = false;
    }

    if (node->shader_eng) {
        bool shared = false;
        if (node->state) {
            for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                if (o != node && o->shader_eng == node->shader_eng) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared) {
            if (node->state && node->state->shader_eng == node->shader_eng) {
                node->state->shader_eng = NULL;
                for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                    if (o != node && o->shader_eng != NULL) {
                        node->state->shader_eng = o->shader_eng;
                        break;
                    }
                }
            }
            int ino_fd = waywal_shader_engine_get_inotify_fd(node->shader_eng);
            if (ino_fd >= 0 && node->state) {
                if (node->state->epoll_fd >= 0) {
                    (void)epoll_ctl(node->state->epoll_fd, EPOLL_CTL_DEL, ino_fd, NULL);
                }
                if (node->state->use_uring && node->state->uring_loop) {
                    (void)uring_loop_remove_poll(node->state->uring_loop, ino_fd);
                }
            }
            waywal_shader_engine_destroy(node->shader_eng);
        }
        node->shader_eng = NULL;
    }
    node->shader_active = false;

    if (node->anim_dec) {
        bool shared = false;
        if (node->state) {
            for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                if (o != node && o->anim_dec == node->anim_dec) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared) {
            if (node->state && node->state->anim_dec == node->anim_dec) {
                node->state->anim_dec = NULL;
            }
            waywal_anim_destroy(node->anim_dec);
        }
        node->anim_dec = NULL;
    }
    node->anim_active = false;

    if (node->video_eng) {
        if (node->video_eng->target_output == node) {
            node->video_eng->target_output = NULL;
        }
        bool shared = false;
        if (node->state) {
            for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                if (o != node && o->video_eng == node->video_eng) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared && node->state && node->video_eng != &node->state->video_engine) {
            if (node->video_eng->timer_fd >= 0) {
                if (node->state->epoll_fd >= 0) {
                    (void)epoll_ctl(node->state->epoll_fd, EPOLL_CTL_DEL, node->video_eng->timer_fd,
                                    NULL);
                }
                if (node->state->use_uring && node->state->uring_loop) {
                    (void)uring_loop_remove_poll(node->state->uring_loop,
                                                 node->video_eng->timer_fd);
                }
            }
            video_engine_destroy(node->video_eng);
            free(node->video_eng);
        }
        node->video_eng = NULL;
    }

    if (node->slideshow_eng) {
        bool shared = false;
        if (node->state) {
            for (output_node_t *o = node->state->outputs; o != NULL; o = o->next) {
                if (o != node && o->slideshow_eng == node->slideshow_eng) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared && node->state && node->slideshow_eng != &node->state->slideshow) {
            if (node->slideshow_eng->timer_fd >= 0) {
                if (node->state->epoll_fd >= 0) {
                    (void)epoll_ctl(node->state->epoll_fd, EPOLL_CTL_DEL,
                                    node->slideshow_eng->timer_fd, NULL);
                }
                if (node->state->use_uring && node->state->uring_loop) {
                    (void)uring_loop_remove_poll(node->state->uring_loop,
                                                 node->slideshow_eng->timer_fd);
                }
            }
            slideshow_destroy(node->slideshow_eng);
            free(node->slideshow_eng);
        }
        node->slideshow_eng = NULL;
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
    node->staging_width = 0;
    node->staging_height = 0;

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
    node->shm_width = 0;
    node->shm_height = 0;
    if (node->shm_fd >= 0) {
        close(node->shm_fd);
        node->shm_fd = -1;
    }

    node->backdrop_configured = false;
    if (node->backdrop_viewport) {
        wp_viewport_destroy(node->backdrop_viewport);
        node->backdrop_viewport = NULL;
    }
    if (node->backdrop_layer_surface) {
        zwlr_layer_surface_v1_destroy(node->backdrop_layer_surface);
        node->backdrop_layer_surface = NULL;
    }
    if (node->backdrop_surface) {
        wl_surface_destroy(node->backdrop_surface);
        node->backdrop_surface = NULL;
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

static color_rgba_t get_output_fill_color(const daemon_state_t *state, const output_node_t *node)
{
    if (node && node->has_fill_color) {
        return node->fill_color;
    }
    if (state && (state->current_color.r || state->current_color.g || state->current_color.b ||
                  state->current_color.a)) {
        return state->current_color;
    }
    return (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
}

static uint32_t pack_color_argb(color_rgba_t c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

static void teardown_output_dmabuf(daemon_state_t *state, output_node_t *node)
{
    if (!state || !node)
        return;
    node->last_presented_bo = NULL;
    if (node->use_dmabuf) {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            release_output_bo_refs(state, node, &node->dmabuf_ring.buffers[i]);
        }
        dmabuf_ring_destroy(&state->dmabuf_ctx, &node->dmabuf_ring);
        node->use_dmabuf = false;
    }
    if (node->has_source_old_bo) {
        release_output_bo_refs(state, node, &node->source_old_bo);
        dmabuf_bo_free(&node->source_old_bo);
        node->has_source_old_bo = false;
    }
    if (node->has_source_new_bo) {
        release_output_bo_refs(state, node, &node->source_new_bo);
        dmabuf_bo_free(&node->source_new_bo);
        node->has_source_new_bo = false;
    }
    if (node->has_backdrop_bo) {
        release_output_bo_refs(state, node, &node->backdrop_bo);
        dmabuf_bo_free(&node->backdrop_bo);
        node->has_backdrop_bo = false;
    }
}

static bool ensure_dmabuf_ring(daemon_state_t *state, output_node_t *node, int32_t width,
                               int32_t height)
{
    if (!state || !node || !state->dmabuf_ctx.available || !state->dmabuf_ctx.dmabuf_proto ||
        state->dmabuf_ctx.drm_fd < 0 || state->dmabuf_ctx.gbm_dev == NULL) {
        teardown_output_dmabuf(state, node);
        return false;
    }
    if (width <= 0 || height <= 0)
        return false;

    if (node->has_target_dev && state->dmabuf_ctx.has_compositor_dev &&
        node->target_dev != state->dmabuf_ctx.compositor_dev) {
        teardown_output_dmabuf(state, node);
        return false;
    }

    bool gpu_ok = state->render_engine.gpu_compute_active;
    if (!gpu_ok) {
        if (!state->dmabuf_ctx.map_supported ||
            (state->dmabuf_ctx.preferred_modifier != DRM_FORMAT_MOD_LINEAR &&
             state->dmabuf_ctx.preferred_modifier != DRM_FORMAT_MOD_INVALID)) {
            teardown_output_dmabuf(state, node);
            return false;
        }
    }

    uint32_t drm_format = DRM_FORMAT_ARGB8888;
    uint64_t modifier = gpu_ok ? state->dmabuf_ctx.preferred_modifier : DRM_FORMAT_MOD_LINEAR;
    if (node->ten_bit && state->dmabuf_ctx.has_10bit) {
        drm_format = state->dmabuf_ctx.format_10bit ? state->dmabuf_ctx.format_10bit
                                                    : DRM_FORMAT_ARGB2101010;
        modifier = gpu_ok ? state->dmabuf_ctx.modifier_10bit : DRM_FORMAT_MOD_LINEAR;
    }

    if (node->use_dmabuf && (int32_t)node->dmabuf_ring.width == width &&
        (int32_t)node->dmabuf_ring.height == height && node->dmabuf_ring.drm_format == drm_format) {
        return true;
    }

    teardown_output_dmabuf(state, node);

    if (dmabuf_ring_init(&state->dmabuf_ctx, &node->dmabuf_ring, (uint32_t)width, (uint32_t)height,
                         drm_format, modifier)) {
        node->use_dmabuf = true;
        WAYWAL_LOG_INFO("Direct Scanout active for output %s (%ux%u, fmt=0x%08x, mod=0x%016lx)",
                        node->name, width, height, drm_format, (unsigned long)modifier);
        return true;
    }

    if (drm_format != DRM_FORMAT_ARGB8888) {
        modifier = gpu_ok ? state->dmabuf_ctx.preferred_modifier : DRM_FORMAT_MOD_LINEAR;
        if (dmabuf_ring_init(&state->dmabuf_ctx, &node->dmabuf_ring, (uint32_t)width,
                             (uint32_t)height, DRM_FORMAT_ARGB8888, modifier)) {
            node->ten_bit = false;
            node->use_dmabuf = true;
            WAYWAL_LOG_INFO("Direct Scanout active for output %s (%ux%u, 8-bit fallback)",
                            node->name, width, height);
            return true;
        }
    }

    WAYWAL_LOG_WARN("Falling back to SHM rendering for output %s", node->name);
    return false;
}

bool layer_shell_ensure_dmabuf_ring(daemon_state_t *state, output_node_t *node, int32_t width,
                                    int32_t height)
{
    return ensure_dmabuf_ring(state, node, width, height);
}

static bool ensure_shm_buffer(daemon_state_t *state, output_node_t *node, int32_t width,
                              int32_t height)
{
    if (width <= 0 || height <= 0)
        return false;
    size_t stride = (size_t)width * 4;
    size_t frame_sz = stride * (size_t)height;
    size_t total_sz = frame_sz * 2; /* double-sized: two frames back-to-back */

    /* Guard: wl_shm_create_pool takes int32_t size; ensure no overflow.
     * For reference: 8K double-buf = 253MB, well below 2GB limit. */
    if (total_sz > (size_t)INT32_MAX) {
        WAYWAL_LOG_ERR("Output %s: SHM double-buffer too large (%zu bytes > INT32_MAX)", node->name,
                       total_sz);
        return false;
    }

    if (node->shm_data && node->shm_size == total_sz && node->shm_width == width &&
        node->shm_height == height && node->shm_buffers[0] && node->shm_buffers[1]) {
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
    node->shm_size = 0;
    node->shm_width = 0;
    node->shm_height = 0;
    if (node->shm_fd >= 0) {
        close(node->shm_fd);
        node->shm_fd = -1;
    }
    node->active_buffer_idx = 0;

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
        munmap(node->shm_data, node->shm_size);
        node->shm_data = NULL;
        node->shm_size = 0;
        close(node->shm_fd);
        node->shm_fd = -1;
        return false;
    }

    /* Create two wl_buffers: buf[0] at offset 0, buf[1] at offset frame_sz */
    bool buf_ok = true;
    for (int i = 0; i < 2; ++i) {
        int32_t offset = (int32_t)(i * frame_sz);
        node->shm_buffers[i] = wl_shm_pool_create_buffer(node->shm_pool, offset, width, height,
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

    node->shm_width = width;
    node->shm_height = height;
    return true;
}

void output_node_render_color(daemon_state_t *state, output_node_t *node, color_rgba_t color)
{
    if (!state || !node || !node->surface || !node->configured)
        return;
    if (node->width <= 0 || node->height <= 0)
        return;

    uint32_t pixel = ((uint32_t)color.a << 24) | ((uint32_t)color.r << 16) |
                     ((uint32_t)color.g << 8) | (uint32_t)color.b;
    node->fill_color = color;
    node->has_fill_color = true;
    int32_t dst_w = node->logical_width > 0 ? (int32_t)node->logical_width : node->width;
    int32_t dst_h = node->logical_height > 0 ? (int32_t)node->logical_height : node->height;

    if (state->single_pixel_mgr != NULL && node->viewport != NULL &&
        (node->logical_width > 0 || node->width > 0)) {
        uint32_t r32 = (uint32_t)color.r * 0x01010101u;
        uint32_t g32 = (uint32_t)color.g * 0x01010101u;
        uint32_t b32 = (uint32_t)color.b * 0x01010101u;
        uint32_t a32 = (uint32_t)color.a * 0x01010101u;
        struct wl_buffer *sp_buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            state->single_pixel_mgr, r32, g32, b32, a32);
        if (sp_buf) {
            if (node->has_source_old_bo) {
                release_output_bo_refs(state, node, &node->source_old_bo);
                dmabuf_bo_free(&node->source_old_bo);
                node->has_source_old_bo = false;
            }
            if (node->has_source_new_bo) {
                release_output_bo_refs(state, node, &node->source_new_bo);
                dmabuf_bo_free(&node->source_new_bo);
                node->has_source_new_bo = false;
            }
            node->last_presented_bo = NULL;
            node->has_image = false;
            wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                   wl_fixed_from_int(-1), wl_fixed_from_int(-1));
            wp_viewport_set_destination(node->viewport, dst_w, dst_h);
            node->vp_configured = false;
            wl_surface_attach(node->surface, sp_buf, 0, 0);
            wl_surface_damage_buffer(node->surface, 0, 0, INT32_MAX, INT32_MAX);
            output_commit_frame(node);
            wl_buffer_destroy(sp_buf);
            wl_display_flush(state->display);
            return;
        }
    }

    /* Preferred path: Hardware Direct Scanout via GBM DMA-BUF */
    if (ensure_dmabuf_ring(state, node, node->width, node->height)) {
        dmabuf_bo_t *bo = dmabuf_ring_acquire(&node->dmabuf_ring);
        if (bo && bo->wl_buffer) {
            bool uploaded = false;
            if ((bo->map_data == NULL || !state->dmabuf_ctx.map_supported ||
                 bo->modifier != DRM_FORMAT_MOD_LINEAR) &&
                state->render_engine.gpu_compute_active) {
                uploaded = render_engine_upload_static(&state->render_engine, NULL, 0, 0, bo,
                                                       WAYWAL_SCALING_FILL, pixel);
            }
            if (!uploaded && state->dmabuf_ctx.map_supported &&
                (bo->modifier == DRM_FORMAT_MOD_LINEAR || bo->modifier == DRM_FORMAT_MOD_INVALID)) {
                uint32_t stride = 0;
                void *map_data = NULL;
                void *map =
                    gbm_bo_map(bo->gbm_bo, 0, 0, (uint32_t)node->width, (uint32_t)node->height,
                               GBM_BO_TRANSFER_WRITE, &stride, &map_data);
                if (map && map != MAP_FAILED) {
                    for (int32_t y = 0; y < node->height; ++y) {
                        uint32_t *row = (uint32_t *)((uint8_t *)map + (size_t)y * stride);
                        for (int32_t x = 0; x < node->width; ++x) {
                            row[x] = pixel;
                        }
                    }
                    gbm_bo_unmap(bo->gbm_bo, map_data);
                    uploaded = true;
                }
            }
            if (uploaded) {
                if (node->has_source_old_bo && state->render_engine.gpu_compute_active) {
                    render_engine_upload_static(&state->render_engine, NULL, 0, 0,
                                                &node->source_old_bo, WAYWAL_SCALING_FILL, pixel);
                }
                if (node->viewport) {
                    wp_viewport_set_destination(node->viewport, dst_w, dst_h);
                    wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1));
                    node->vp_configured = false;
                }
                wl_surface_attach(node->surface, bo->wl_buffer, 0, 0);
                wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
                output_commit_frame(node);
                node->has_image = false;
                node->last_presented_bo = bo;
                wl_display_flush(state->display);
                return;
            }
            bo->in_use = false;
            teardown_output_dmabuf(state, node);
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
        wp_viewport_set_destination(node->viewport, dst_w, dst_h);
        wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        node->vp_configured = false;
    }

    wl_surface_attach(node->surface, node->shm_buffers[back_idx], 0, 0);
    wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
    node->shm_buffer_released[back_idx] = false;
    node->active_buffer_idx = back_idx;
    output_commit_frame(node);
    node->has_image = false;
    int inactive_idx = 1 - back_idx;
    if (node->shm_buffer_released[inactive_idx]) {
        void *inactive_ptr = (uint8_t *)node->shm_data + (size_t)inactive_idx * frame_sz;
        madvise(inactive_ptr, frame_sz, MADV_REMOVE);
    }
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

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static inline void
scale_bilinear_row_avx2(uint32_t *restrict dst_span, const uint32_t *restrict row0,
                        const uint32_t *restrict row1, const int32_t *restrict x0_tbl,
                        const int32_t *restrict x1_tbl, const uint64_t *restrict fx4_tbl,
                        const uint16_t *restrict fx_tbl, uint32_t scaled_w, uint16_t fy,
                        uint16_t ify)
{
    const __m256i v256 = _mm256_set1_epi16(256);
    const __m256i vfy = _mm256_set1_epi16((short)fy);
    const __m256i vify = _mm256_set1_epi16((short)ify);

    uint32_t x = 0;
    for (; x + 4 <= scaled_w; x += 4) {
        __m128i p00_128 = _mm_set_epi32((int)row0[x0_tbl[x + 3]], (int)row0[x0_tbl[x + 2]],
                                        (int)row0[x0_tbl[x + 1]], (int)row0[x0_tbl[x + 0]]);
        __m128i p10_128 = _mm_set_epi32((int)row0[x1_tbl[x + 3]], (int)row0[x1_tbl[x + 2]],
                                        (int)row0[x1_tbl[x + 1]], (int)row0[x1_tbl[x + 0]]);
        __m128i p01_128 = _mm_set_epi32((int)row1[x0_tbl[x + 3]], (int)row1[x0_tbl[x + 2]],
                                        (int)row1[x0_tbl[x + 1]], (int)row1[x0_tbl[x + 0]]);
        __m128i p11_128 = _mm_set_epi32((int)row1[x1_tbl[x + 3]], (int)row1[x1_tbl[x + 2]],
                                        (int)row1[x1_tbl[x + 1]], (int)row1[x1_tbl[x + 0]]);

        __m256i c00 = _mm256_cvtepu8_epi16(p00_128);
        __m256i c10 = _mm256_cvtepu8_epi16(p10_128);
        __m256i c01 = _mm256_cvtepu8_epi16(p01_128);
        __m256i c11 = _mm256_cvtepu8_epi16(p11_128);

        __m256i vfx = _mm256_loadu_si256((const __m256i *)&fx4_tbl[x]);
        __m256i vifx = _mm256_sub_epi16(v256, vfx);

        __m256i top16 = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(c00, vifx), _mm256_mullo_epi16(c10, vfx)), 8);
        __m256i bot16 = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(c01, vifx), _mm256_mullo_epi16(c11, vfx)), 8);

        __m256i res16 = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(top16, vify), _mm256_mullo_epi16(bot16, vfy)), 8);

        __m128i lo = _mm256_castsi256_si128(res16);
        __m128i hi = _mm256_extracti128_si256(res16, 1);
        __m128i packed = _mm_packus_epi16(lo, hi);
        _mm_storeu_si128((__m128i *)&dst_span[x], packed);
    }

    for (; x < scaled_w; ++x) {
        uint32_t fx = fx_tbl[x];
        uint32_t ifx = 256u - fx;
        uint32_t w00 = (ifx * ify) >> 8;
        uint32_t w10 = (fx * ify) >> 8;
        uint32_t w01 = (ifx * fy) >> 8;
        uint32_t w11 = (fx * fy) >> 8;

        uint32_t c00 = row0[x0_tbl[x]];
        uint32_t c10 = row0[x1_tbl[x]];
        uint32_t c01 = row1[x0_tbl[x]];
        uint32_t c11 = row1[x1_tbl[x]];

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

        dst_span[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}
#endif

static void scale_bilinear_rect(uint32_t *dst, uint32_t dst_w, uint32_t dst_h,
                                size_t dst_stride_bytes, const uint32_t *src, uint32_t src_w,
                                uint32_t src_h, uint32_t dst_x_start, uint32_t dst_y_start,
                                uint32_t scaled_w, uint32_t scaled_h, double off_x, double off_y,
                                double crop_w, double crop_h, uint32_t bg_packed,
                                uint32_t filter_mode)
{
    int32_t *x0_tbl = NULL;
    int32_t *x1_tbl = NULL;
    uint16_t *fx_tbl = NULL;
    uint64_t *fx4_tbl = NULL;

    if (scaled_w > 0) {
        x0_tbl = malloc((size_t)scaled_w * sizeof(int32_t));
        x1_tbl = malloc((size_t)scaled_w * sizeof(int32_t));
        fx_tbl = malloc((size_t)scaled_w * sizeof(uint16_t));
        fx4_tbl = malloc((size_t)scaled_w * sizeof(uint64_t));
    }

    const double center_shift = (filter_mode == 3u) ? 0.0 : 0.5;

    if (!x0_tbl || !x1_tbl || !fx_tbl || !fx4_tbl) {
        free(x0_tbl);
        free(x1_tbl);
        free(fx_tbl);
        free(fx4_tbl);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (uint32_t y = 0; y < dst_h; ++y) {
            uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
            if (y < dst_y_start || y >= dst_y_start + scaled_h || scaled_h == 0 || scaled_w == 0) {
                for (uint32_t x = 0; x < dst_w; ++x)
                    dst_row[x] = bg_packed;
            } else {
                double sy = off_y + ((double)(y - dst_y_start) + 0.5) * crop_h / (double)scaled_h -
                            center_shift;
                for (uint32_t x = 0; x < dst_w; ++x) {
                    if (x < dst_x_start || x >= dst_x_start + scaled_w) {
                        dst_row[x] = bg_packed;
                    } else {
                        double sx = off_x +
                                    ((double)(x - dst_x_start) + 0.5) * crop_w / (double)scaled_w -
                                    center_shift;
                        dst_row[x] = sample_bilinear(src, src_w, src_h, sx, sy);
                    }
                }
            }
        }
        return;
    }

    for (uint32_t x = 0; x < scaled_w; ++x) {
        double sx = off_x + ((double)x + 0.5) * crop_w / (double)scaled_w - center_shift;
        if (sx < 0.0)
            sx = 0.0;
        if (sx > (double)(src_w - 1))
            sx = (double)(src_w - 1);
        int32_t x0 = (int32_t)sx;
        int32_t x1 = (x0 + 1 < (int32_t)src_w) ? (x0 + 1) : x0;
        uint16_t fx = (filter_mode == 3u) ? 0u : (uint16_t)((sx - (double)x0) * 256.0);
        x0_tbl[x] = x0;
        x1_tbl[x] = x1;
        fx_tbl[x] = fx;
        fx4_tbl[x] = (uint64_t)fx * 0x0001000100010001ULL;
    }

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
    const bool use_avx2 = __builtin_cpu_supports("avx2");
#endif

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t *dst_row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride_bytes);
        if (y < dst_y_start || y >= dst_y_start + scaled_h || scaled_h == 0) {
            for (uint32_t x = 0; x < dst_w; ++x)
                dst_row[x] = bg_packed;
            continue;
        }

        for (uint32_t x = 0; x < dst_x_start; ++x)
            dst_row[x] = bg_packed;
        for (uint32_t x = dst_x_start + scaled_w; x < dst_w; ++x)
            dst_row[x] = bg_packed;

        double sy =
            off_y + ((double)(y - dst_y_start) + 0.5) * crop_h / (double)scaled_h - center_shift;
        if (sy < 0.0)
            sy = 0.0;
        if (sy > (double)(src_h - 1))
            sy = (double)(src_h - 1);
        int32_t y0 = (int32_t)sy;
        int32_t y1 = (y0 + 1 < (int32_t)src_h) ? (y0 + 1) : y0;
        uint16_t fy = (filter_mode == 3u) ? 0u : (uint16_t)((sy - (double)y0) * 256.0);
        uint16_t ify = (uint16_t)(256u - fy);

        const uint32_t *row0 = src + (size_t)y0 * src_w;
        const uint32_t *row1 = src + (size_t)y1 * src_w;
        uint32_t *dst_span = dst_row + dst_x_start;

        if (filter_mode == 3u) {
            for (uint32_t x = 0; x < scaled_w; ++x) {
                dst_span[x] = row0[x0_tbl[x]];
            }
            continue;
        }

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
        if (use_avx2) {
            scale_bilinear_row_avx2(dst_span, row0, row1, x0_tbl, x1_tbl, fx4_tbl, fx_tbl, scaled_w,
                                    fy, ify);
            continue;
        }
#endif
        for (uint32_t x = 0; x < scaled_w; ++x) {
            uint32_t fx = fx_tbl[x];
            uint32_t ifx = 256u - fx;
            uint32_t w00 = (ifx * ify) >> 8;
            uint32_t w10 = (fx * ify) >> 8;
            uint32_t w01 = (ifx * fy) >> 8;
            uint32_t w11 = (fx * fy) >> 8;

            uint32_t c00 = row0[x0_tbl[x]];
            uint32_t c10 = row0[x1_tbl[x]];
            uint32_t c01 = row1[x0_tbl[x]];
            uint32_t c11 = row1[x1_tbl[x]];

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

            dst_span[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    free(x0_tbl);
    free(x1_tbl);
    free(fx_tbl);
    free(fx4_tbl);
}

static void compute_output_span_bounds(const daemon_state_t *state, output_node_t *target)
{
    if (!target)
        return;

    int32_t t_lw = target->logical_width > 0 ? (int32_t)target->logical_width : target->width;
    int32_t t_lh = target->logical_height > 0 ? (int32_t)target->logical_height : target->height;
    if (t_lw <= 0)
        t_lw = 1;
    if (t_lh <= 0)
        t_lh = 1;

    if (!state || !state->outputs) {
        target->span_off_x = 0;
        target->span_off_y = 0;
        target->span_total_w = target->width;
        target->span_total_h = target->height;
        return;
    }

    bool any_global_pos = false;
    for (const output_node_t *o = state->outputs; o != NULL; o = o->next) {
        if (o->global_x != 0 || o->global_y != 0) {
            any_global_pos = true;
            break;
        }
    }

    int32_t min_x = INT32_MAX;
    int32_t min_y = INT32_MAX;
    int32_t max_x = INT32_MIN;
    int32_t max_y = INT32_MIN;
    int32_t accum_x = 0;
    int32_t target_ox = target->global_x;
    int32_t target_oy = target->global_y;

    for (const output_node_t *o = state->outputs; o != NULL; o = o->next) {
        int32_t lw = o->logical_width > 0 ? (int32_t)o->logical_width : o->width;
        int32_t lh = o->logical_height > 0 ? (int32_t)o->logical_height : o->height;
        if (lw <= 0 || lh <= 0)
            continue;
        int32_t ox = any_global_pos ? o->global_x : accum_x;
        int32_t oy = any_global_pos ? o->global_y : 0;
        if (o == target) {
            target_ox = ox;
            target_oy = oy;
        }
        if (ox < min_x)
            min_x = ox;
        if (oy < min_y)
            min_y = oy;
        if (ox + lw > max_x)
            max_x = ox + lw;
        if (oy + lh > max_y)
            max_y = oy + lh;
        accum_x += lw;
    }

    if (min_x >= max_x || min_y >= max_y) {
        target->span_off_x = 0;
        target->span_off_y = 0;
        target->span_total_w = target->width;
        target->span_total_h = target->height;
        return;
    }

    double sx = (double)target->width / (double)t_lw;
    double sy = (double)target->height / (double)t_lh;
    target->span_off_x = (int32_t)((double)(target_ox - min_x) * sx + 0.5);
    target->span_off_y = (int32_t)((double)(target_oy - min_y) * sy + 0.5);
    target->span_total_w = (int32_t)((double)(max_x - min_x) * sx + 0.5);
    target->span_total_h = (int32_t)((double)(max_y - min_y) * sy + 0.5);
}

void layer_shell_compute_span_bounds(daemon_state_t *state, output_node_t *target)
{
    compute_output_span_bounds(state, target);
}

void layer_shell_snapshot_live_output(daemon_state_t *state, output_node_t *out)
{
    if (!state || !out || out->width <= 0 || out->height <= 0)
        return;

    if (out->video_eng && out->video_eng->state != WAYWAL_VIDEO_STATE_STOPPED) {
        if (video_engine_snapshot_output(out->video_eng, out))
            return;
    } else if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED && !out->shader_active &&
               !out->anim_active && !out->workshop_active &&
               (state->video_engine.target_output == out ||
                (!state->video_engine.target_output &&
                 (!state->video_engine.has_target_filter || out->video_targeted)))) {
        if (video_engine_snapshot_output(&state->video_engine, out))
            return;
    }

    if (out->last_presented_bo && out->last_presented_bo != &out->source_old_bo) {
        uint32_t w = (uint32_t)out->width;
        uint32_t h = (uint32_t)out->height;
        uint64_t src_mod = state->render_engine.gpu_compute_active
                               ? state->dmabuf_ctx.preferred_modifier
                               : DRM_FORMAT_MOD_LINEAR;
        if (!out->has_source_old_bo || out->source_old_bo.width != w ||
            out->source_old_bo.height != h) {
            if (out->has_source_old_bo) {
                release_output_bo_refs(state, out, &out->source_old_bo);
                dmabuf_bo_free(&out->source_old_bo);
                out->has_source_old_bo = false;
            }
            if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_old_bo, w, h,
                                   DRM_FORMAT_ARGB8888, src_mod)) {
                out->has_source_old_bo = true;
            }
        }
        if (out->has_source_old_bo) {
            color_rgba_t fill_col = get_output_fill_color(state, out);
            uint32_t clear_color = pack_color_argb(fill_col);
            if (render_engine_copy_bo(&state->render_engine, &out->source_old_bo,
                                      out->last_presented_bo, WAYWAL_SCALING_STRETCH, clear_color,
                                      false)) {
                out->has_image = true;
                out->last_presented_bo = &out->source_old_bo;
            } else {
                release_output_bo_refs(state, out, &out->source_old_bo);
                dmabuf_bo_free(&out->source_old_bo);
                out->has_source_old_bo = false;
            }
        }
    }
}

void waywal_scale_image_span_cpu(uint32_t *dst, uint32_t dst_w, uint32_t dst_h,
                                 size_t dst_stride_bytes, const uint32_t *src, uint32_t src_w,
                                 uint32_t src_h, int32_t span_off_x, int32_t span_off_y,
                                 int32_t span_total_w, int32_t span_total_h, uint32_t filter_mode,
                                 color_rgba_t bg_color)
{
    if (!dst || !src || dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0)
        return;

    uint32_t bg_packed = ((uint32_t)bg_color.a << 24) | ((uint32_t)bg_color.r << 16) |
                         ((uint32_t)bg_color.g << 8) | (uint32_t)bg_color.b;

    double tot_w = (span_total_w > 0) ? (double)span_total_w : (double)dst_w;
    double tot_h = (span_total_h > 0) ? (double)span_total_h : (double)dst_h;
    double r_src = (double)src_w / (double)src_h;
    double r_dst = tot_w / tot_h;
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

    double sub_off_x = off_x + ((double)span_off_x / tot_w) * crop_w;
    double sub_off_y = off_y + ((double)span_off_y / tot_h) * crop_h;
    double sub_crop_w = ((double)dst_w / tot_w) * crop_w;
    double sub_crop_h = ((double)dst_h / tot_h) * crop_h;

    scale_bilinear_rect(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, 0, 0, dst_w, dst_h,
                        sub_off_x, sub_off_y, sub_crop_w, sub_crop_h, bg_packed, filter_mode);
}

static void copy_or_scale_image_ex(const output_node_t *node, uint32_t *dst, uint32_t dst_w,
                                   uint32_t dst_h, size_t dst_stride_bytes, const uint32_t *src,
                                   uint32_t src_w, uint32_t src_h, uint32_t scaling_mode,
                                   uint32_t filter_mode, color_rgba_t bg_color)
{
    if (!dst || !src || dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0)
        return;

    uint32_t bg_packed = ((uint32_t)bg_color.a << 24) | ((uint32_t)bg_color.r << 16) |
                         ((uint32_t)bg_color.g << 8) | (uint32_t)bg_color.b;

    if (dst_w == src_w && dst_h == src_h && scaling_mode != WAYWAL_SCALING_TILE &&
        scaling_mode != WAYWAL_SCALING_SPAN) {
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

        scale_bilinear_rect(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, 0, 0, dst_w,
                            dst_h, off_x, off_y, crop_w, crop_h, bg_packed, filter_mode);
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

            scale_bilinear_rect(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, 0, pad_y,
                                dst_w, scaled_h, 0.0, 0.0, (double)src_w, (double)src_h, bg_packed,
                                filter_mode);
        } else {
            /* Taller than screen: full height, pillarbox left/right */
            uint32_t scaled_w = (uint32_t)((double)dst_h * r_src + 0.5);
            if (scaled_w > dst_w)
                scaled_w = dst_w;
            uint32_t pad_x = (dst_w - scaled_w) / 2;

            scale_bilinear_rect(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, pad_x, 0,
                                scaled_w, dst_h, 0.0, 0.0, (double)src_w, (double)src_h, bg_packed,
                                filter_mode);
        }
        break;
    }

    case WAYWAL_SCALING_STRETCH: {
        scale_bilinear_rect(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, 0, 0, dst_w,
                            dst_h, 0.0, 0.0, (double)src_w, (double)src_h, bg_packed, filter_mode);
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

    case WAYWAL_SCALING_SPAN: {
        int32_t off_x = node ? node->span_off_x : 0;
        int32_t off_y = node ? node->span_off_y : 0;
        int32_t tot_w = (node && node->span_total_w > 0) ? node->span_total_w : (int32_t)dst_w;
        int32_t tot_h = (node && node->span_total_h > 0) ? node->span_total_h : (int32_t)dst_h;
        waywal_scale_image_span_cpu(dst, dst_w, dst_h, dst_stride_bytes, src, src_w, src_h, off_x,
                                    off_y, tot_w, tot_h, filter_mode, bg_color);
        break;
    }
    }
}

static void render_backdrop_surface(daemon_state_t *state, output_node_t *node,
                                    const uint8_t *src_pixels, uint32_t img_w, uint32_t img_h)
{
    if (!state || !node || !state->backdrop_auto_blur || !node->backdrop_surface || !src_pixels)
        return;
    if (!state->render_engine.gpu_compute_active || !state->render_engine.gpu_ctx)
        return;

    uint64_t mod = state->dmabuf_ctx.preferred_modifier;
    if (!node->has_backdrop_bo || node->backdrop_bo.width != (uint32_t)node->width ||
        node->backdrop_bo.height != (uint32_t)node->height) {
        if (node->has_backdrop_bo) {
            release_output_bo_refs(state, node, &node->backdrop_bo);
            dmabuf_bo_free(&node->backdrop_bo);
            node->has_backdrop_bo = false;
        }
        if (dmabuf_bo_allocate(&state->dmabuf_ctx, &node->backdrop_bo, (uint32_t)node->width,
                               (uint32_t)node->height, DRM_FORMAT_ARGB8888, mod)) {
            node->has_backdrop_bo = true;
        }
    }

    if (node->has_backdrop_bo && node->backdrop_bo.wl_buffer) {
        if (gpu_compute_render_kawase_blur((gpu_compute_ctx_t *)state->render_engine.gpu_ctx,
                                           src_pixels, img_w, img_h, &node->backdrop_bo, 12)) {
            if (node->backdrop_viewport) {
                int32_t dst_w =
                    node->logical_width > 0 ? (int32_t)node->logical_width : node->width;
                int32_t dst_h =
                    node->logical_height > 0 ? (int32_t)node->logical_height : node->height;
                wp_viewport_set_destination(node->backdrop_viewport, dst_w, dst_h);
            }
            if (node->backdrop_configured) {
                wl_surface_attach(node->backdrop_surface, node->backdrop_bo.wl_buffer, 0, 0);
                wl_surface_damage_buffer(node->backdrop_surface, 0, 0, node->width, node->height);
                wl_surface_commit(node->backdrop_surface);
            }
        }
    }
}

void output_node_render_image(daemon_state_t *state, output_node_t *node, const uint8_t *src_pixels,
                              uint32_t img_w, uint32_t img_h)
{
    if (!state || !node || !node->surface || !node->configured || !src_pixels)
        return;
    if (node->width <= 0 || node->height <= 0 || img_w == 0 || img_h == 0)
        return;

    compute_output_span_bounds(state, node);
    render_engine_set_span_bounds(&state->render_engine, node->span_off_x, node->span_off_y,
                                  node->span_total_w, node->span_total_h);

    const uint32_t *src = (const uint32_t *)src_pixels;
    color_rgba_t fill_col = get_output_fill_color(state, node);
    uint32_t clear_color = pack_color_argb(fill_col);
    int32_t dst_w = node->logical_width > 0 ? (int32_t)node->logical_width : node->width;
    int32_t dst_h = node->logical_height > 0 ? (int32_t)node->logical_height : node->height;

    if (node->viewport) {
        wp_viewport_set_destination(node->viewport, dst_w, dst_h);
        wp_viewport_set_source(node->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        node->vp_configured = false;
    }

    render_backdrop_surface(state, node, src_pixels, img_w, img_h);

    /* Preferred path: Hardware Direct Scanout & Plane Scaling */
    if (ensure_dmabuf_ring(state, node, node->width, node->height)) {
        dmabuf_bo_t *bo = dmabuf_ring_acquire(&node->dmabuf_ring);
        if (bo && bo->wl_buffer) {
            bool uploaded = false;
            if ((bo->map_data == NULL || !state->dmabuf_ctx.map_supported ||
                 bo->modifier != DRM_FORMAT_MOD_LINEAR) &&
                state->render_engine.gpu_compute_active) {
                uploaded = render_engine_upload_static(&state->render_engine, src_pixels, img_w,
                                                       img_h, bo, node->scaling_mode, clear_color);
            }
            if (!uploaded && state->dmabuf_ctx.map_supported &&
                (bo->modifier == DRM_FORMAT_MOD_LINEAR || bo->modifier == DRM_FORMAT_MOD_INVALID)) {
                uint32_t stride = 0;
                void *map_data = NULL;
                void *map =
                    gbm_bo_map(bo->gbm_bo, 0, 0, (uint32_t)node->width, (uint32_t)node->height,
                               GBM_BO_TRANSFER_WRITE, &stride, &map_data);
                if (map && map != MAP_FAILED) {
                    copy_or_scale_image_ex(node, (uint32_t *)map, (uint32_t)node->width,
                                           (uint32_t)node->height, stride, src, img_w, img_h,
                                           node->scaling_mode, node->filter_mode, fill_col);
                    gbm_bo_unmap(bo->gbm_bo, map_data);
                    uploaded = true;
                }
            }
            if (uploaded) {
                if (!state->transitions_in_progress && !state->anim_active &&
                    state->num_outputs <= 1) {
                    render_engine_release_upload_cache(&state->render_engine);
                }
                wl_surface_attach(node->surface, bo->wl_buffer, 0, 0);
                wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
                output_commit_frame(node);
                node->has_image = true;
                node->last_presented_bo = bo;
                wl_display_flush(state->display);
                return;
            }
            bo->in_use = false;
            teardown_output_dmabuf(state, node);
        }
    }

    /* Fallback SHM rendering path */
    if (ensure_shm_buffer(state, node, node->width, node->height)) {
        int back_idx = 1 - node->active_buffer_idx;
        size_t frame_sz = (size_t)node->width * (size_t)node->height * sizeof(uint32_t);
        void *back_ptr = (uint8_t *)node->shm_data + back_idx * frame_sz;
        copy_or_scale_image_ex(node, (uint32_t *)back_ptr, (uint32_t)node->width,
                               (uint32_t)node->height, (size_t)node->width * sizeof(uint32_t), src,
                               img_w, img_h, node->scaling_mode, node->filter_mode, fill_col);
        wl_surface_attach(node->surface, node->shm_buffers[back_idx], 0, 0);
        wl_surface_damage_buffer(node->surface, 0, 0, node->width, node->height);
        node->shm_buffer_released[back_idx] = false;
        node->active_buffer_idx = back_idx;
        output_commit_frame(node);
        node->has_image = true;
        int inactive_idx = 1 - back_idx;
        if (node->shm_buffer_released[inactive_idx]) {
            void *inactive_ptr = (uint8_t *)node->shm_data + (size_t)inactive_idx * frame_sz;
            madvise(inactive_ptr, frame_sz, MADV_REMOVE);
        }
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
    if (node->shm_buffer_cap < needed || !node->shm_old_data || !node->shm_new_data) {
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
            node->staging_width = 0;
            node->staging_height = 0;
            return false;
        }
        node->shm_buffer_cap = needed;
    }
    node->staging_width = node->width;
    node->staging_height = node->height;
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
    uint32_t remaining = meta->extra_data_len;

    for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
        if (remaining == 0)
            break;
        size_t len = strnlen(ptr, remaining);
        if (len == remaining)
            break;
        if (strcmp(node->name, ptr) == 0)
            return true;
        ptr += len + 1;
        remaining -= (uint32_t)(len + 1);
    }
    return false;
}

bool daemon_start_image_transition(daemon_state_t *state, const uint8_t *pixels, uint32_t img_w,
                                   uint32_t img_h, const waywal_img_metadata_t *meta)
{
    if (!state || !pixels || img_w == 0 || img_h == 0)
        return false;

    render_engine_bump_upload_gen(&state->render_engine);

    /* If custom shader provided, load it into compute engine */
    if (meta && meta->transition_type == WAYWAL_TRANSITION_CUSTOM && meta->custom_shader_len > 0) {
        const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
        size_t rem = meta->extra_data_len;
        bool valid = true;
        for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
            const char *nul = (const char *)memchr(ptr, '\0', rem);
            if (!nul) {
                valid = false;
                break;
            }
            size_t step = (size_t)(nul - ptr) + 1u;
            ptr += step;
            rem -= step;
        }
        if (valid && rem >= meta->custom_shader_len && memchr(ptr, '\0', rem) != NULL) {
            render_engine_load_custom_shader(&state->render_engine, ptr);
        }
    }

    /* Snapshot outgoing live frames (shader, animation, video, or mid-transition) on targeted
     * outputs so transitions blend smoothly from the visible frame. */
    bool any_other_transition = false;
    bool has_transition =
        meta && meta->transition_type != WAYWAL_TRANSITION_NONE && meta->transition_duration_ms > 0;
    for (output_node_t *pout = state->outputs; pout != NULL; pout = pout->next) {
        if (!is_target_output(pout, meta)) {
            if (pout->transition_active) {
                any_other_transition = true;
            }
            continue;
        }
        if (has_transition) {
            layer_shell_snapshot_live_output(state, pout);
        }
        if (!pout->transition_active)
            continue;
        /* Copy the currently committed (back) buffer into old staging */
        if (pout->shm_data && pout->shm_old_data && pout->width > 0 && pout->height > 0) {
            size_t fsz = (size_t)pout->width * (size_t)pout->height * sizeof(uint32_t);
            if (pout->shm_size == fsz * 2 && pout->shm_buffer_cap >= fsz &&
                pout->shm_width == pout->width && pout->shm_height == pout->height &&
                pout->staging_width == pout->width && pout->staging_height == pout->height) {
                void *displayed = (uint8_t *)pout->shm_data + (size_t)pout->active_buffer_idx * fsz;
                memcpy(pout->shm_old_data, displayed, fsz);
            }
        }
        pout->transition_active = false;
    }
    if (!any_other_transition) {
        if (state->transition_timer_fd >= 0) {
            uint64_t dummy;
            while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0)
                ;
        }
        state->transitions_in_progress = false;
    }

    if (!meta || meta->transition_type == WAYWAL_TRANSITION_NONE ||
        meta->transition_duration_ms == 0) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!is_target_output(out, meta))
                continue;
            out->scaling_mode = meta ? meta->scaling_mode : WAYWAL_SCALING_FILL;
            out->filter_mode = meta ? meta->filter_mode : 0u;
            if (meta && (meta->color[0] || meta->color[1] || meta->color[2] || meta->color[3])) {
                out->fill_color = (color_rgba_t){
                    .r = meta->color[0],
                    .g = meta->color[1],
                    .b = meta->color[2],
                    .a = meta->color[3] ? meta->color[3] : 255,
                };
                out->has_fill_color = true;
            }
            if (meta) {
                out->ten_bit = meta->ten_bit;
            }
            output_node_render_image(state, out, pixels, img_w, img_h);
        }
        render_engine_release_upload_cache(&state->render_engine);
        return true;
    }

    uint32_t resolved_type = meta->transition_type;
    if (resolved_type == WAYWAL_TRANSITION_RANDOM) {
        resolved_type = 1u + ((uint32_t)rand() % 19u);
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
        if (!out->surface || !out->configured || out->width <= 0 || out->height <= 0)
            continue;
        if (!is_target_output(out, meta))
            continue;

        out->scaling_mode = meta ? meta->scaling_mode : WAYWAL_SCALING_FILL;
        out->filter_mode = meta ? meta->filter_mode : 0u;
        color_rgba_t prev_fill_col = get_output_fill_color(state, out);
        uint32_t prev_clear_color = pack_color_argb(prev_fill_col);
        if (meta && (meta->color[0] || meta->color[1] || meta->color[2] || meta->color[3])) {
            out->fill_color = (color_rgba_t){
                .r = meta->color[0],
                .g = meta->color[1],
                .b = meta->color[2],
                .a = meta->color[3] ? meta->color[3] : 255,
            };
            out->has_fill_color = true;
        }
        out->ten_bit = meta ? meta->ten_bit : false;
        color_rgba_t fill_col = get_output_fill_color(state, out);
        uint32_t clear_color = pack_color_argb(fill_col);

        compute_output_span_bounds(state, out);
        render_engine_set_span_bounds(&state->render_engine, out->span_off_x, out->span_off_y,
                                      out->span_total_w, out->span_total_h);

        if (out->viewport) {
            int32_t dst_w = out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
            int32_t dst_h = out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
            wp_viewport_set_destination(out->viewport, dst_w, dst_h);
            wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                   wl_fixed_from_int(-1), wl_fixed_from_int(-1));
            out->vp_configured = false;
        }

        render_backdrop_surface(state, out, pixels, img_w, img_h);

        bool dmabuf_ok = false;
        if (ensure_dmabuf_ring(state, out, out->width, out->height)) {
            bool gpu_ok = state->render_engine.gpu_compute_active;
            uint64_t modifier =
                gpu_ok ? state->dmabuf_ctx.preferred_modifier : DRM_FORMAT_MOD_LINEAR;
            if (!out->has_source_new_bo || out->source_new_bo.width != (uint32_t)out->width ||
                out->source_new_bo.height != (uint32_t)out->height) {
                if (out->has_source_new_bo) {
                    release_output_bo_refs(state, out, &out->source_new_bo);
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
                bool new_uploaded = false;
                if ((out->source_new_bo.map_data == NULL || !state->dmabuf_ctx.map_supported ||
                     out->source_new_bo.modifier != DRM_FORMAT_MOD_LINEAR) &&
                    gpu_ok) {
                    new_uploaded = render_engine_upload_static(&state->render_engine, pixels, img_w,
                                                               img_h, &out->source_new_bo,
                                                               out->scaling_mode, clear_color);
                }
                if (!new_uploaded && state->dmabuf_ctx.map_supported &&
                    (out->source_new_bo.modifier == DRM_FORMAT_MOD_LINEAR ||
                     out->source_new_bo.modifier == DRM_FORMAT_MOD_INVALID)) {
                    uint32_t stride_new = 0;
                    void *map_data_new = NULL;
                    void *map_new = gbm_bo_map(out->source_new_bo.gbm_bo, 0, 0,
                                               (uint32_t)out->width, (uint32_t)out->height,
                                               GBM_BO_TRANSFER_WRITE, &stride_new, &map_data_new);
                    if (map_new && map_new != MAP_FAILED) {
                        copy_or_scale_image_ex(out, (uint32_t *)map_new, (uint32_t)out->width,
                                               (uint32_t)out->height, stride_new, src, img_w, img_h,
                                               out->scaling_mode, out->filter_mode, fill_col);
                        gbm_bo_unmap(out->source_new_bo.gbm_bo, map_data_new);
                        new_uploaded = true;
                    }
                }

                if (new_uploaded) {
                    bool copied_old =
                        (out->has_source_old_bo && out->last_presented_bo == &out->source_old_bo &&
                         out->source_old_bo.width == (uint32_t)out->width &&
                         out->source_old_bo.height == (uint32_t)out->height);
                    if (!out->has_source_old_bo ||
                        out->source_old_bo.width != (uint32_t)out->width ||
                        out->source_old_bo.height != (uint32_t)out->height) {
                        if (out->has_source_old_bo) {
                            release_output_bo_refs(state, out, &out->source_old_bo);
                            dmabuf_bo_free(&out->source_old_bo);
                            out->has_source_old_bo = false;
                        }
                        if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_old_bo,
                                               (uint32_t)out->width, (uint32_t)out->height,
                                               DRM_FORMAT_ARGB8888, modifier)) {
                            out->has_source_old_bo = true;
                        }
                    }
                    if (out->has_source_old_bo && !copied_old) {
                        bool old_uploaded = false;
                        if ((out->source_old_bo.map_data == NULL ||
                             !state->dmabuf_ctx.map_supported ||
                             out->source_old_bo.modifier != DRM_FORMAT_MOD_LINEAR) &&
                            gpu_ok) {
                            old_uploaded = render_engine_upload_static(
                                &state->render_engine, NULL, 0, 0, &out->source_old_bo,
                                WAYWAL_SCALING_FILL, prev_clear_color);
                        }
                        if (!old_uploaded && state->dmabuf_ctx.map_supported) {
                            uint32_t stride_old = 0;
                            void *map_data_old = NULL;
                            void *map_old =
                                gbm_bo_map(out->source_old_bo.gbm_bo, 0, 0, (uint32_t)out->width,
                                           (uint32_t)out->height, GBM_BO_TRANSFER_WRITE,
                                           &stride_old, &map_data_old);
                            if (map_old && map_old != MAP_FAILED) {
                                for (int32_t y = 0; y < out->height; ++y) {
                                    uint32_t *dst_row =
                                        (uint32_t *)((uint8_t *)map_old + (size_t)y * stride_old);
                                    for (int32_t x = 0; x < out->width; ++x) {
                                        dst_row[x] = prev_clear_color;
                                    }
                                }
                                gbm_bo_unmap(out->source_old_bo.gbm_bo, map_data_old);
                            }
                        }
                    }
                    if (out->has_source_old_bo) {
                        dmabuf_ok = true;
                    }
                }
            }
        }

        if (!dmabuf_ok) {
            teardown_output_dmabuf(state, out);
            if (!ensure_shm_buffer(state, out, out->width, out->height) ||
                !ensure_shm_transition_buffers(out)) {
                output_node_render_image(state, out, pixels, img_w, img_h);
                continue;
            }
            size_t fsz = (size_t)out->width * (size_t)out->height * sizeof(uint32_t);
            if (out->has_image && out->shm_data && out->shm_size == fsz * 2 &&
                out->shm_width == out->width && out->shm_height == out->height) {
                /* Snapshot the currently committed (front) buffer as the old frame */
                void *front_ptr = (uint8_t *)out->shm_data + (size_t)out->active_buffer_idx * fsz;
                memcpy(out->shm_old_data, front_ptr, fsz);
            } else {
                uint32_t *old_u32 = (uint32_t *)out->shm_old_data;
                size_t total_px = (size_t)out->width * (size_t)out->height;
                for (size_t p = 0; p < total_px; ++p) {
                    old_u32[p] = prev_clear_color;
                }
            }
            copy_or_scale_image_ex(out, (uint32_t *)out->shm_new_data, (uint32_t)out->width,
                                   (uint32_t)out->height, (size_t)out->width * sizeof(uint32_t),
                                   src, img_w, img_h, out->scaling_mode, out->filter_mode,
                                   fill_col);
        }

        float cx = 0.5f;
        float cy = 0.5f;
        if (meta->transition_center_x < 0.0f || meta->transition_center_y < 0.0f) {
            /* Dynamic cursor tracking: daemon_query_cursor_pos returns logical coordinates */
            double qx = 0, qy = 0;
            double ref_w = out->logical_width > 0 ? (double)out->logical_width : (double)out->width;
            double ref_h =
                out->logical_height > 0 ? (double)out->logical_height : (double)out->height;
            if (daemon_query_cursor_pos(state, out, &qx, &qy) && ref_w > 0.0 && ref_h > 0.0) {
                cx = (float)(qx / ref_w);
                cy = (float)(qy / ref_h);
            }
        } else {
            cx = meta->transition_center_x;
            cy = meta->transition_center_y;
        }

        float wfreq = meta->transition_wave_freq;
        float wamp = meta->transition_wave_amp;

        bezier_curve_t bz = BEZIER_DEFAULT;
        if (meta &&
            (meta->has_custom_bezier || meta->bezier.x1 != 0.0f || meta->bezier.y1 != 0.0f ||
             meta->bezier.x2 != 0.0f || meta->bezier.y2 != 0.0f)) {
            bz = meta->bezier;
        }

        out->transition_params = (waywal_transition_params_t){
            .type = (waywal_transition_type_t)resolved_type,
            .progress = 0.0f,
            .angle_rad = meta->transition_angle_rad,
            .wave_freq = wfreq,
            .wave_amp = wamp,
            .center_x = cx,
            .center_y = cy,
            .bezier = bz,
        };
        out->transition_current_frame = 0;
        out->transition_total_frames = num_frames;
        out->transition_delay_frames =
            (meta->sync_mode == 1) ? (started_count * stagger_frames) : 0;
        out->transition_active = true;
        started_count++;
    }
    render_engine_release_upload_cache(&state->render_engine);

    if (started_count == 0)
        return false;

    struct itimerspec its = {.it_interval = {.tv_sec = (time_t)(frame_interval_ns / 1000000000ULL),
                                             .tv_nsec = (long)(frame_interval_ns % 1000000000ULL)},
                             .it_value = {.tv_sec = (time_t)(frame_interval_ns / 1000000000ULL),
                                          .tv_nsec = (long)(frame_interval_ns % 1000000000ULL)}};
    if (state->transition_timer_fd >= 0) {
        uint64_t dummy;
        while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
        timerfd_settime(state->transition_timer_fd, 0, &its, NULL);
    }
    state->transitions_in_progress = true;
    WAYWAL_LOG_INFO("Transition started: type=%u, duration=%ums, frames=%u, fps=%u", resolved_type,
                    meta->transition_duration_ms, num_frames, fps);
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
        out->transition_params.progress = (out->transition_params.type == WAYWAL_TRANSITION_SIMPLE)
                                              ? t
                                              : bezier_eval(&out->transition_params.bezier, t);

        if (out->use_dmabuf) {
            if ((int32_t)out->dmabuf_ring.width != out->width ||
                (int32_t)out->dmabuf_ring.height != out->height ||
                (int32_t)out->source_old_bo.width != out->width ||
                (int32_t)out->source_old_bo.height != out->height ||
                (int32_t)out->source_new_bo.width != out->width ||
                (int32_t)out->source_new_bo.height != out->height) {
                out->transition_current_frame = out->transition_total_frames;
            } else {
                bool committed = false;
                dmabuf_bo_t *target_bo = dmabuf_ring_acquire(&out->dmabuf_ring);
                if (target_bo && target_bo->wl_buffer) {
                    if (render_engine_execute_transition(&state->render_engine, target_bo,
                                                         &out->source_old_bo, &out->source_new_bo,
                                                         &out->transition_params)) {
                        wl_surface_attach(out->surface, target_bo->wl_buffer, 0, 0);
                        wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                        presentation_sync_request(&out->pres_sync);
                        output_commit_frame(out);
                        out->last_presented_bo = target_bo;
                        committed = true;
                    } else {
                        target_bo->in_use = false;
                    }
                }
                if (!committed && out->transition_current_frame >= out->transition_total_frames) {
                    out->transition_current_frame = out->transition_total_frames - 1;
                    active_count++;
                    continue;
                }
            }
        } else {
            /* SHM ping-pong path: render into back buffer, never touch the front */
            size_t frame_sz = (size_t)out->width * (size_t)out->height * sizeof(uint32_t);
            if (out->width <= 0 || out->height <= 0 || !out->shm_buffers[0] ||
                !out->shm_buffers[1] || !out->shm_data || !out->shm_old_data ||
                !out->shm_new_data || out->shm_size != frame_sz * 2 ||
                out->shm_buffer_cap < frame_sz || out->shm_width != out->width ||
                out->shm_height != out->height || out->staging_width != out->width ||
                out->staging_height != out->height) {
                out->transition_current_frame = out->transition_total_frames;
            } else {
                int back_idx = 1 - out->active_buffer_idx;

                /* Safety: if compositor hasn't released the back buffer yet, skip frame */
                if (!out->shm_buffer_released[back_idx]) {
                    active_count++;
                    continue;
                }

                struct timespec ts0, ts1;
                clock_gettime(CLOCK_MONOTONIC, &ts0);

                void *back_ptr = (uint8_t *)out->shm_data + (size_t)back_idx * frame_sz;

                /* OpenMP-parallelized CPU transition (16 threads, ~5–18ms for complex effects) */
                render_engine_execute_cpu_transition(
                    (uint32_t *)back_ptr, (const uint32_t *)out->shm_old_data,
                    (const uint32_t *)out->shm_new_data, (uint32_t)out->width,
                    (uint32_t)out->height, (uint32_t)out->width * sizeof(uint32_t),
                    &out->transition_params);

                clock_gettime(CLOCK_MONOTONIC, &ts1);
                double render_ms = ((ts1.tv_sec - ts0.tv_sec) * 1000.0) +
                                   ((ts1.tv_nsec - ts0.tv_nsec) / 1000000.0);

                /* Commit back buffer; swap front/back indices */
                wl_surface_attach(out->surface, out->shm_buffers[back_idx], 0, 0);
                wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                out->shm_buffer_released[back_idx] = false;
                out->active_buffer_idx = back_idx;
                presentation_sync_request(&out->pres_sync);
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
                if (out->has_source_old_bo) {
                    release_output_bo_refs(state, out, &out->source_old_bo);
                    dmabuf_bo_free(&out->source_old_bo);
                    out->has_source_old_bo = false;
                }
                if (out->has_source_new_bo) {
                    release_output_bo_refs(state, out, &out->source_new_bo);
                    dmabuf_bo_free(&out->source_new_bo);
                    out->has_source_new_bo = false;
                }
            } else {
                free(out->shm_old_data);
                free(out->shm_new_data);
                out->shm_old_data = NULL;
                out->shm_new_data = NULL;
                out->shm_buffer_cap = 0;
                out->staging_width = 0;
                out->staging_height = 0;
                int inactive_idx = 1 - out->active_buffer_idx;
                if (out->shm_buffer_released[inactive_idx] && out->shm_data && out->shm_size >= 8) {
                    size_t frame_sz = out->shm_size / 2;
                    void *inactive_ptr = (uint8_t *)out->shm_data + (size_t)inactive_idx * frame_sz;
                    madvise(inactive_ptr, frame_sz, MADV_REMOVE);
                }
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
        while (read(state->transition_timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
        state->transitions_in_progress = false;
        malloc_trim(0);
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
