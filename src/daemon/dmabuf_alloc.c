#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static void dmabuf_release_handler(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    dmabuf_bo_t *bo = (dmabuf_bo_t *)data;
    if (bo) {
        bo->in_use = false;
    }
}

static const struct wl_buffer_listener dmabuf_buffer_listener = {
    .release = dmabuf_release_handler,
};

#include <dirent.h>
#include <stdio.h>

static bool find_active_drm_render_node(char *out_path, size_t max_len) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) return false;

    struct dirent *entry;
    char path[512];
    char status[64];
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        /* Look for connector directories like card0-HDMI-A-1, card1-eDP-1 */
        char *dash = strchr(entry->d_name, '-');
        if (!dash) continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (fgets(status, sizeof(status), f)) {
            /* Trim trailing newline / whitespace */
            size_t len = strlen(status);
            while (len > 0 && (status[len - 1] == '\n' || status[len - 1] == '\r' || status[len - 1] == ' ')) {
                status[--len] = '\0';
            }
            if (strcmp(status, "connected") == 0) {
                /* Extract card name e.g. card1 */
                char card_name[64] = {0};
                size_t card_len = (size_t)(dash - entry->d_name);
                if (card_len < sizeof(card_name)) {
                    memcpy(card_name, entry->d_name, card_len);
                    card_name[card_len] = '\0';

                    char drm_dir_path[512];
                    snprintf(drm_dir_path, sizeof(drm_dir_path), "/sys/class/drm/%s/device/drm", card_name);
                    DIR *drm_dir = opendir(drm_dir_path);
                    if (drm_dir) {
                        struct dirent *drm_entry;
                        while ((drm_entry = readdir(drm_dir)) != NULL) {
                            if (strncmp(drm_entry->d_name, "renderD", 7) == 0) {
                                snprintf(out_path, max_len, "/dev/dri/%s", drm_entry->d_name);
                                found = true;
                                break;
                            }
                        }
                        closedir(drm_dir);
                    }
                }
            }
        }
        fclose(f);
        if (found) break;
    }
    closedir(dir);
    return found;
}

bool dmabuf_context_init(dmabuf_context_t *ctx) {
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->drm_fd = -1;
    ctx->preferred_modifier = DRM_FORMAT_MOD_INVALID;

    /* Discover DRM render node:
     * 1. Query sysfs for active display connectors to find the GPU driving the display.
     * 2. Fallback to /dev/dri/renderD128 if accessible.
     * 3. Fallback to drmGetDevices2.
     */
    char active_node[256] = {0};
    int drm_fd = -1;

    if (find_active_drm_render_node(active_node, sizeof(active_node))) {
        drm_fd = open(active_node, O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) {
            WAYWAL_LOG_INFO("Discovered active display DRM render node: %s", active_node);
        }
    }

    if (drm_fd < 0) {
        drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) {
            WAYWAL_LOG_INFO("Discovered fallback primary DRM render node: /dev/dri/renderD128");
        }
    }

    if (drm_fd < 0) {
        drmDevicePtr devices[8];
        int num_devices = drmGetDevices2(0, devices, 8);
        if (num_devices > 0) {
            for (int i = 0; i < num_devices; i++) {
                if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
                    drm_fd = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
                    if (drm_fd >= 0) {
                        WAYWAL_LOG_INFO("Discovered DRM render node: %s", devices[i]->nodes[DRM_NODE_RENDER]);
                        break;
                    }
                }
            }
            drmFreeDevices(devices, num_devices);
        }
    }

    if (drm_fd < 0) {
        WAYWAL_LOG_WARN("No DRM render node accessible: %s", strerror(errno));
        return false;
    }

    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        WAYWAL_LOG_WARN("gbm_create_device failed: %s", strerror(errno));
        close(drm_fd);
        return false;
    }

    ctx->drm_fd = drm_fd;
    ctx->gbm = gbm;
    ctx->available = true;
    WAYWAL_LOG_INFO("GBM device initialized successfully on DRM fd %d", drm_fd);
    return true;
}

void dmabuf_context_destroy(dmabuf_context_t *ctx) {
    if (!ctx) return;
    if (ctx->default_feedback) {
        dmabuf_feedback_destroy(ctx);
    }
    if (ctx->dmabuf_proto) {
        zwp_linux_dmabuf_v1_destroy(ctx->dmabuf_proto);
        ctx->dmabuf_proto = NULL;
    }
    if (ctx->gbm) {
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
    }
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }
    ctx->available = false;
}

bool dmabuf_bo_allocate(dmabuf_context_t *ctx, dmabuf_bo_t *bo,
                        uint32_t width, uint32_t height,
                        uint32_t drm_format, uint64_t modifier) {
    if (!ctx || !ctx->gbm || !bo || width == 0 || height == 0) return false;
    memset(bo, 0, sizeof(*bo));
    for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
        bo->fd[p] = -1;
    }

    struct gbm_bo *gbm_bo = NULL;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        const uint64_t mods[] = { modifier };
        gbm_bo = gbm_bo_create_with_modifiers(ctx->gbm, width, height, drm_format, mods, 1);
    }

    if (!gbm_bo) {
        gbm_bo = gbm_bo_create(ctx->gbm, width, height, drm_format,
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!gbm_bo) {
        gbm_bo = gbm_bo_create(ctx->gbm, width, height, drm_format, GBM_BO_USE_SCANOUT);
    }

    if (!gbm_bo) {
        WAYWAL_LOG_ERR("gbm_bo_create failed for %ux%u format 0x%08x", width, height, drm_format);
        return false;
    }

    int num_planes = gbm_bo_get_plane_count(gbm_bo);
    if (num_planes <= 0 || num_planes > WAYWAL_MAX_BUFFER_PLANES) {
        num_planes = 1;
    }
    uint64_t actual_mod = gbm_bo_get_modifier(gbm_bo);

    bo->gbm_bo = gbm_bo;
    bo->width = width;
    bo->height = height;
    bo->drm_format = drm_format;
    bo->modifier = actual_mod;
    bo->num_planes = (uint32_t)num_planes;

    struct zwp_linux_buffer_params_v1 *params = NULL;
    if (ctx->dmabuf_proto) {
        params = zwp_linux_dmabuf_v1_create_params(ctx->dmabuf_proto);
    }

    for (int p = 0; p < num_planes; p++) {
        int plane_fd = gbm_bo_get_fd_for_plane(gbm_bo, p);
        if (plane_fd < 0) {
            plane_fd = gbm_bo_get_fd(gbm_bo);
        }
        bo->fd[p] = plane_fd;
        bo->stride[p] = gbm_bo_get_stride_for_plane(gbm_bo, p);
        bo->offset[p] = gbm_bo_get_offset(gbm_bo, p);

        if (params && plane_fd >= 0) {
            zwp_linux_buffer_params_v1_add(params, plane_fd, (uint32_t)p,
                                           bo->offset[p], bo->stride[p],
                                           (uint32_t)(actual_mod >> 32),
                                           (uint32_t)(actual_mod & 0xFFFFFFFF));
        }
    }

    if (params) {
        bo->wl_buffer = zwp_linux_buffer_params_v1_create_immed(
            params, (int32_t)width, (int32_t)height, drm_format, 0);
        zwp_linux_buffer_params_v1_destroy(params);

        if (bo->wl_buffer) {
            wl_buffer_add_listener(bo->wl_buffer, &dmabuf_buffer_listener, bo);
        }
    }

    return true;
}

void dmabuf_bo_free(dmabuf_bo_t *bo) {
    if (!bo) return;
    bo->egl_image = NULL;
    bo->gl_tex = 0;
    if (bo->wl_buffer) {
        wl_buffer_destroy(bo->wl_buffer);
        bo->wl_buffer = NULL;
    }
    for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
        if (bo->fd[p] >= 0) {
            close(bo->fd[p]);
            bo->fd[p] = -1;
        }
    }
    if (bo->gbm_bo) {
        gbm_bo_destroy(bo->gbm_bo);
        bo->gbm_bo = NULL;
    }
    bo->in_use = false;
}

bool dmabuf_ring_init(dmabuf_context_t *ctx, dmabuf_ring_t *ring,
                      uint32_t width, uint32_t height,
                      uint32_t drm_format, uint64_t modifier) {
    if (!ctx || !ctx->available || !ring || width == 0 || height == 0) return false;
    memset(ring, 0, sizeof(*ring));
    ring->width = width;
    ring->height = height;
    ring->drm_format = drm_format;
    ring->modifier = modifier;

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        if (!dmabuf_bo_allocate(ctx, &ring->buffers[i], width, height, drm_format, modifier)) {
            WAYWAL_LOG_ERR("Failed to allocate ring buffer %zu", i);
            for (size_t j = 0; j < i; ++j) {
                dmabuf_bo_free(&ring->buffers[j]);
            }
            return false;
        }
    }

    ring->initialized = true;
    WAYWAL_LOG_DEBUG("Initialized DMA-BUF ring for %ux%u (mod: 0x%016lx)",
                   width, height, (unsigned long)modifier);
    return true;
}

void dmabuf_ring_destroy(dmabuf_context_t *ctx, dmabuf_ring_t *ring) {
    (void)ctx;
    if (!ring || !ring->initialized) return;

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        dmabuf_bo_free(&ring->buffers[i]);
    }
    ring->initialized = false;
}

dmabuf_bo_t *dmabuf_ring_acquire(dmabuf_ring_t *ring) {
    if (!ring || !ring->initialized) return NULL;

    /* Check next buffer */
    dmabuf_bo_t *candidate = &ring->buffers[ring->current_idx];
    ring->current_idx = (ring->current_idx + 1) % WAYWAL_DMABUF_RING_SIZE;
    candidate->in_use = true;
    return candidate;
}

void dmabuf_bo_mark_released(dmabuf_ring_t *ring, struct wl_buffer *buffer) {
    if (!ring || !ring->initialized || !buffer) return;

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        if (ring->buffers[i].wl_buffer == buffer) {
            ring->buffers[i].in_use = false;
            return;
        }
    }
}
