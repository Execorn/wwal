// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

extern EGLContext eglGetCurrentContext(void) __attribute__((weak));
extern EGLDisplay eglGetCurrentDisplay(void) __attribute__((weak));
extern __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
    __attribute__((weak));
extern void glDeleteTextures(GLsizei n, const GLuint *textures) __attribute__((weak));
extern void glBindTexture(GLenum target, GLuint texture) __attribute__((weak));
extern void glActiveTexture(GLenum texture) __attribute__((weak));
extern void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                               GLint layer, GLenum access, GLenum format) __attribute__((weak));

static inline EGLContext safe_eglGetCurrentContext(void)
{
    if (eglGetCurrentContext)
        return eglGetCurrentContext();
    typedef EGLContext (*pfn_eglGetCurrentContext)(void);
    pfn_eglGetCurrentContext fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "eglGetCurrentContext");
    return fn ? fn() : EGL_NO_CONTEXT;
}

static inline EGLDisplay safe_eglGetCurrentDisplay(void)
{
    if (eglGetCurrentDisplay)
        return eglGetCurrentDisplay();
    typedef EGLDisplay (*pfn_eglGetCurrentDisplay)(void);
    pfn_eglGetCurrentDisplay fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
    return fn ? fn() : EGL_NO_DISPLAY;
}

static inline __eglMustCastToProperFunctionPointerType safe_eglGetProcAddress(const char *procname)
{
    if (eglGetProcAddress)
        return eglGetProcAddress(procname);
    typedef __eglMustCastToProperFunctionPointerType (*pfn_eglGetProcAddress)(const char *);
    pfn_eglGetProcAddress fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    return fn ? fn(procname) : NULL;
}

static inline void safe_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    if (glDeleteTextures) {
        glDeleteTextures(n, textures);
        return;
    }
    typedef void (*pfn_glDeleteTextures)(GLsizei, const GLuint *);
    pfn_glDeleteTextures fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "glDeleteTextures");
    if (fn)
        fn(n, textures);
}

static inline void safe_glBindTexture(GLenum target, GLuint texture)
{
    if (glBindTexture) {
        glBindTexture(target, texture);
        return;
    }
    typedef void (*pfn_glBindTexture)(GLenum, GLuint);
    pfn_glBindTexture fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "glBindTexture");
    if (fn)
        fn(target, texture);
}

static inline void safe_glActiveTexture(GLenum texture)
{
    if (glActiveTexture) {
        glActiveTexture(texture);
        return;
    }
    typedef void (*pfn_glActiveTexture)(GLenum);
    pfn_glActiveTexture fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "glActiveTexture");
    if (fn)
        fn(texture);
}

static inline void safe_glBindImageTexture(GLuint unit, GLuint texture, GLint level,
                                           GLboolean layered, GLint layer, GLenum access,
                                           GLenum format)
{
    if (glBindImageTexture) {
        glBindImageTexture(unit, texture, level, layered, layer, access, format);
        return;
    }
    typedef void (*pfn_glBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    pfn_glBindImageTexture fn = NULL;
    *(void **)(&fn) = dlsym(RTLD_DEFAULT, "glBindImageTexture");
    if (fn)
        fn(unit, texture, level, layered, layer, access, format);
}

static void dmabuf_release_handler(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    dmabuf_bo_t *bo = (dmabuf_bo_t *)data;
    if (bo) {
        bo->in_use = false;
        if (bo->on_release) {
            bo->on_release(bo->release_user_data, bo);
        }
    }
}

static const struct wl_buffer_listener dmabuf_buffer_listener = {
    .release = dmabuf_release_handler,
};

#include <dirent.h>
#include <stdio.h>

static bool find_active_drm_render_node(char *out_path, size_t max_len)
{
    DIR *dir = opendir("/sys/class/drm");
    if (!dir)
        return false;

    struct dirent *entry;
    char path[512];
    char status[64];
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        /* Look for connector directories like card0-HDMI-A-1, card1-eDP-1 */
        char *dash = strchr(entry->d_name, '-');
        if (!dash)
            continue;

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        if (fgets(status, sizeof(status), f)) {
            /* Trim trailing newline / whitespace */
            size_t len = strlen(status);
            while (len > 0 &&
                   (status[len - 1] == '\n' || status[len - 1] == '\r' || status[len - 1] == ' ')) {
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
                    snprintf(drm_dir_path, sizeof(drm_dir_path), "/sys/class/drm/%s/device/drm",
                             card_name);
                    DIR *drm_dir = opendir(drm_dir_path);
                    if (drm_dir) {
                        struct dirent *drm_entry;
                        while ((drm_entry = readdir(drm_dir)) != NULL) {
                            if (strncmp(drm_entry->d_name, "renderD", 7) == 0) {
                                snprintf(out_path, max_len, "/dev/dri/%.64s", drm_entry->d_name);
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
        if (found)
            break;
    }
    closedir(dir);
    return found;
}

/* Try to find the DRM render node matching a specific dev_t reported by the compositor */
int open_drm_node_by_devt(dev_t target_dev)
{
    /* 1. Query libdrm directly: converts primary KMS or render dev_t to render node */
    drmDevicePtr dev_ptr = NULL;
    if (drmGetDeviceFromDevId(target_dev, 0, &dev_ptr) == 0 && dev_ptr) {
        if ((dev_ptr->available_nodes & (1 << DRM_NODE_RENDER)) &&
            dev_ptr->nodes[DRM_NODE_RENDER]) {
            int fd = open(dev_ptr->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                WAYWAL_LOG_INFO("Matched compositor DRM device %u:%u via libdrm → %s",
                                (unsigned)major(target_dev), (unsigned)minor(target_dev),
                                dev_ptr->nodes[DRM_NODE_RENDER]);
                drmFreeDevice(&dev_ptr);
                return fd;
            }
        }
        drmFreeDevice(&dev_ptr);
    }

    DIR *dir = opendir("/dev/dri");
    if (!dir)
        return -1;

    struct dirent *de;
    int found_fd = -1;

    while ((de = readdir(dir)) != NULL) {
        bool is_render = (strncmp(de->d_name, "renderD", 7) == 0);
        bool is_card = (strncmp(de->d_name, "card", 4) == 0);
        if (!is_render && !is_card)
            continue;

        char path[300];
        snprintf(path, sizeof(path), "/dev/dri/%s", de->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && st.st_rdev == target_dev) {
            if (is_render) {
                found_fd = open(path, O_RDWR | O_CLOEXEC);
                if (found_fd >= 0) {
                    WAYWAL_LOG_INFO("Matched compositor DRM render device %u:%u → %s",
                                    (unsigned)major(target_dev), (unsigned)minor(target_dev), path);
                    break;
                }
            } else if (is_card) {
                /* When st.st_rdev == target_dev matches a primary KMS node (card*), derive
                 * corresponding render node via /sys/class/drm/<cardN>/device/drm/renderD* */
                char sys_path[512];
                snprintf(sys_path, sizeof(sys_path), "/sys/class/drm/%s/device/drm", de->d_name);
                DIR *sys_dir = opendir(sys_path);
                if (sys_dir) {
                    struct dirent *sde;
                    while ((sde = readdir(sys_dir)) != NULL) {
                        if (strncmp(sde->d_name, "renderD", 7) == 0) {
                            char render_path[300];
                            snprintf(render_path, sizeof(render_path), "/dev/dri/%s", sde->d_name);
                            found_fd = open(render_path, O_RDWR | O_CLOEXEC);
                            if (found_fd >= 0) {
                                WAYWAL_LOG_INFO(
                                    "Derived render node for compositor KMS card %s (%u:%u) → %s",
                                    de->d_name, (unsigned)major(target_dev),
                                    (unsigned)minor(target_dev), render_path);
                                break;
                            }
                        }
                    }
                    closedir(sys_dir);
                }

                /* Fallback 1: renderD minor = card minor + 128 */
                if (found_fd < 0) {
                    char render_path[300];
                    snprintf(render_path, sizeof(render_path), "/dev/dri/renderD%d",
                             (int)minor(target_dev) + 128);
                    found_fd = open(render_path, O_RDWR | O_CLOEXEC);
                    if (found_fd >= 0) {
                        WAYWAL_LOG_INFO("Derived render node via minor offset for %s → %s",
                                        de->d_name, render_path);
                    }
                }

                /* Fallback 2: open matched KMS node directly */
                if (found_fd < 0) {
                    found_fd = open(path, O_RDWR | O_CLOEXEC);
                    if (found_fd >= 0) {
                        WAYWAL_LOG_INFO("Fallback opened matched KMS card device %u:%u → %s",
                                        (unsigned)major(target_dev), (unsigned)minor(target_dev),
                                        path);
                    }
                }
                break;
            }
        }
    }
    closedir(dir);
    return found_fd;
}

/* NVIDIA's proprietary GBM backend (nvidia-drm_gbm.so) allocates BOs but leaves
 * bo_map unimplemented, which would cause CPU pixel uploads to fail after
 * full-screen DMA-BUFs have already been imported into the compositor. */
bool dmabuf_probe_map_support(dmabuf_context_t *ctx)
{
    if (!ctx || !ctx->gbm)
        return false;

    struct gbm_bo *bo = gbm_bo_create(ctx->gbm, 4, 4, DRM_FORMAT_ARGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) {
        bo = gbm_bo_create(ctx->gbm, 4, 4, DRM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT);
    }
    if (!bo) {
        bo = gbm_bo_create(ctx->gbm, 4, 4, DRM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);
    }
    if (!bo) {
        return false;
    }

    uint32_t stride = 0;
    void *map_data = NULL;
    void *map = gbm_bo_map(bo, 0, 0, 4, 4, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    bool supported = (map != NULL && map != MAP_FAILED);
    if (supported) {
        gbm_bo_unmap(bo, map_data);
    }
    gbm_bo_destroy(bo);
    return supported;
}

bool dmabuf_context_init(dmabuf_context_t *ctx)
{
    if (!ctx)
        return false;
    dev_t saved_compositor_dev = ctx->compositor_dev;
    bool saved_has_compositor_dev = ctx->has_compositor_dev;
    memset(ctx, 0, sizeof(*ctx));
    ctx->drm_fd = -1;
    ctx->preferred_modifier = DRM_FORMAT_MOD_INVALID;
    ctx->compositor_dev = saved_compositor_dev;
    ctx->has_compositor_dev = saved_has_compositor_dev;

    int drm_fd = -1;
    char active_node[256] = {0};

    /* Priority 0: If compositor dev_t already known (feedback arrived before init),
     * match it directly. This path is not yet reachable at init time because Wayland
     * roundtrip hasn't happened; the fallback paths below handle first-time init. */
    if (ctx->has_compositor_dev) {
        drm_fd = open_drm_node_by_devt(ctx->compositor_dev);
        if (drm_fd >= 0) {
            WAYWAL_LOG_INFO("Priority 0: Opened compositor-matched DRM render node");
        }
    }

    /* Priority 1: Active connector sysfs scan */
    if (drm_fd < 0 && find_active_drm_render_node(active_node, sizeof(active_node))) {
        drm_fd = open(active_node, O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) {
            WAYWAL_LOG_INFO("Priority 1: Discovered active display DRM render node: %s",
                            active_node);
        }
    }

    /* Priority 2: Fallback to renderD128 (AMD compositor default) */
    if (drm_fd < 0) {
        drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) {
            WAYWAL_LOG_INFO("Priority 2: Fallback DRM render node: /dev/dri/renderD128");
        }
    }

    /* Priority 3: drmGetDevices2 enumeration */
    if (drm_fd < 0) {
        drmDevicePtr devices[8];
        int num_devices = drmGetDevices2(0, devices, 8);
        if (num_devices > 0) {
            for (int i = 0; i < num_devices; i++) {
                if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
                    drm_fd = open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
                    if (drm_fd >= 0) {
                        WAYWAL_LOG_INFO("Priority 3: DRM render node: %s",
                                        devices[i]->nodes[DRM_NODE_RENDER]);
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
    ctx->map_supported = dmabuf_probe_map_support(ctx);
    WAYWAL_LOG_INFO("GBM device initialized on DRM fd %d (bo_map: %s)", drm_fd,
                    ctx->map_supported ? "supported" : "unsupported");
    return true;
}

void dmabuf_context_destroy(dmabuf_context_t *ctx)
{
    if (!ctx)
        return;
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
    ctx->map_supported = false;
}

bool dmabuf_bo_allocate(dmabuf_context_t *ctx, dmabuf_bo_t *bo, uint32_t width, uint32_t height,
                        uint32_t drm_format, uint64_t modifier)
{
    if (!ctx || !ctx->gbm || !bo || width == 0 || height == 0)
        return false;
    memset(bo, 0, sizeof(*bo));
    for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
        bo->fd[p] = -1;
    }

    struct gbm_bo *gbm_bo = NULL;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        const uint64_t mods[] = {modifier};
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
    bo->is_10bit = (drm_format == DRM_FORMAT_XRGB2101010 || drm_format == DRM_FORMAT_ARGB2101010 ||
                    drm_format == DRM_FORMAT_XBGR2101010 || drm_format == DRM_FORMAT_ABGR2101010);

    struct zwp_linux_buffer_params_v1 *params = NULL;
    if (ctx->dmabuf_proto) {
        params = zwp_linux_dmabuf_v1_create_params(ctx->dmabuf_proto);
    }

    for (int p = 0; p < num_planes; p++) {
        int plane_fd = gbm_bo_get_fd_for_plane(gbm_bo, p);
        if (plane_fd < 0) {
            plane_fd = gbm_bo_get_fd(gbm_bo);
        }
        if (plane_fd < 0) {
            WAYWAL_LOG_ERR("Failed to export DMA-BUF fd for plane %d", p);
            if (params) {
                zwp_linux_buffer_params_v1_destroy(params);
            }
            for (int j = 0; j < p; ++j) {
                int fd_to_close = bo->fd[j];
                if (fd_to_close >= 0) {
                    close(fd_to_close);
                    for (int k = j + 1; k < p; ++k) {
                        if (bo->fd[k] == fd_to_close) {
                            bo->fd[k] = -1;
                        }
                    }
                    bo->fd[j] = -1;
                }
            }
            gbm_bo_destroy(gbm_bo);
            bo->gbm_bo = NULL;
            return false;
        }
        bo->fd[p] = plane_fd;
        bo->stride[p] = gbm_bo_get_stride_for_plane(gbm_bo, p);
        bo->offset[p] = gbm_bo_get_offset(gbm_bo, p);

        if (params) {
            zwp_linux_buffer_params_v1_add(params, plane_fd, (uint32_t)p, bo->offset[p],
                                           bo->stride[p], (uint32_t)(actual_mod >> 32),
                                           (uint32_t)(actual_mod & 0xFFFFFFFF));
        }
    }

    if (params) {
        bo->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)width,
                                                                (int32_t)height, drm_format, 0);
        zwp_linux_buffer_params_v1_destroy(params);

        if (bo->wl_buffer) {
            wl_buffer_add_listener(bo->wl_buffer, &dmabuf_buffer_listener, bo);
        }
    }

    return true;
}

void dmabuf_bo_free(dmabuf_bo_t *bo)
{
    if (!bo)
        return;

    /* Release GL texture and EGLImage before destroying buffer if EGL context is active */
    if (bo->gl_tex != 0 || bo->egl_image != NULL) {
        EGLContext cur_ctx = safe_eglGetCurrentContext();
        if (cur_ctx != EGL_NO_CONTEXT) {
            EGLDisplay cur_dpy = safe_eglGetCurrentDisplay();
            if (bo->gl_tex != 0) {
                /* Invariant: Ensure gl_tex is unbound from texture and image units before deleting
                 */
                safe_glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                safe_glActiveTexture(GL_TEXTURE2);
                safe_glBindTexture(GL_TEXTURE_2D, 0);
                safe_glActiveTexture(GL_TEXTURE1);
                safe_glBindTexture(GL_TEXTURE_2D, 0);
                safe_glActiveTexture(GL_TEXTURE0);
                safe_glBindTexture(GL_TEXTURE_2D, 0);

                safe_glDeleteTextures(1, &bo->gl_tex);
                bo->gl_tex = 0;
            }
            if (bo->egl_image != NULL) {
                PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR =
                    (PFNEGLDESTROYIMAGEKHRPROC)safe_eglGetProcAddress("eglDestroyImageKHR");
                if (p_eglDestroyImageKHR && cur_dpy != EGL_NO_DISPLAY) {
                    p_eglDestroyImageKHR(cur_dpy, bo->egl_image);
                }
                bo->egl_image = NULL;
            }
        }
    }
    bo->egl_image = NULL;
    bo->gl_tex = 0;
    if (!bo->gbm_bo && !bo->wl_buffer) {
        for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
            bo->fd[p] = -1;
        }
        bo->in_use = false;
        return;
    }
    if (bo->wl_buffer) {
        wl_buffer_destroy(bo->wl_buffer);
        bo->wl_buffer = NULL;
    }
    for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
        int fd = bo->fd[p];
        if (fd >= 0) {
            close(fd);
            for (int j = p + 1; j < WAYWAL_MAX_BUFFER_PLANES; ++j) {
                if (bo->fd[j] == fd) {
                    bo->fd[j] = -1;
                }
            }
            bo->fd[p] = -1;
        }
    }
    if (bo->gbm_bo) {
        gbm_bo_destroy(bo->gbm_bo);
        bo->gbm_bo = NULL;
    }
    bo->in_use = false;
}

bool dmabuf_ring_init(dmabuf_context_t *ctx, dmabuf_ring_t *ring, uint32_t width, uint32_t height,
                      uint32_t drm_format, uint64_t modifier)
{
    if (!ctx || !ctx->available || !ring || width == 0 || height == 0)
        return false;
    memset(ring, 0, sizeof(*ring));
    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p) {
            ring->buffers[i].fd[p] = -1;
        }
    }
    ring->ctx = ctx;
    ring->width = width;
    ring->height = height;
    ring->drm_format = drm_format;
    ring->modifier = modifier;

    if (!dmabuf_bo_allocate(ctx, &ring->buffers[0], width, height, drm_format, modifier)) {
        WAYWAL_LOG_ERR("Failed to allocate initial ring buffer 0");
        return false;
    }

    ring->initialized = true;
    WAYWAL_LOG_DEBUG("Initialized lazy DMA-BUF ring for %ux%u (mod: 0x%016lx, 1/%d allocated)",
                     width, height, (unsigned long)modifier, WAYWAL_DMABUF_RING_SIZE);
    return true;
}

void dmabuf_ring_destroy(dmabuf_context_t *ctx, dmabuf_ring_t *ring)
{
    (void)ctx;
    if (!ring || !ring->initialized)
        return;

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        ring->buffers[i].on_release = NULL;
        ring->buffers[i].release_user_data = NULL;
        dmabuf_bo_free(&ring->buffers[i]);
    }
    ring->initialized = false;
}

void dmabuf_ring_trim_idle(dmabuf_ring_t *ring)
{
    if (!ring || !ring->initialized)
        return;

    int keep_idx = -1;
    size_t last_idx = (ring->current_idx + WAYWAL_DMABUF_RING_SIZE - 1) % WAYWAL_DMABUF_RING_SIZE;
    if (ring->buffers[last_idx].gbm_bo != NULL) {
        keep_idx = (int)last_idx;
    } else {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            if (ring->buffers[i].gbm_bo != NULL && ring->buffers[i].in_use) {
                keep_idx = (int)i;
                break;
            }
        }
        if (keep_idx < 0) {
            for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
                if (ring->buffers[i].gbm_bo != NULL) {
                    keep_idx = (int)i;
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        if (ring->buffers[i].gbm_bo != NULL && !ring->buffers[i].in_use && (int)i != keep_idx) {
            dmabuf_bo_free(&ring->buffers[i]);
        }
    }
}

dmabuf_bo_t *dmabuf_ring_acquire(dmabuf_ring_t *ring)
{
    if (!ring || !ring->initialized)
        return NULL;

    for (size_t attempt = 0; attempt < WAYWAL_DMABUF_RING_SIZE; ++attempt) {
        size_t idx = (ring->current_idx + attempt) % WAYWAL_DMABUF_RING_SIZE;
        bool slot_allocated =
            (ring->buffers[idx].gbm_bo != NULL || ring->buffers[idx].wl_buffer != NULL);
        if (slot_allocated && !ring->buffers[idx].in_use) {
            ring->current_idx = (idx + 1) % WAYWAL_DMABUF_RING_SIZE;
            ring->buffers[idx].in_use = true;
            return &ring->buffers[idx];
        }
    }

    if (ring->ctx) {
        for (size_t attempt = 0; attempt < WAYWAL_DMABUF_RING_SIZE; ++attempt) {
            size_t idx = (ring->current_idx + attempt) % WAYWAL_DMABUF_RING_SIZE;
            bool slot_allocated =
                (ring->buffers[idx].gbm_bo != NULL || ring->buffers[idx].wl_buffer != NULL);
            if (!slot_allocated) {
                void (*saved_on_release)(void *user_data, struct dmabuf_bo *bo) =
                    ring->buffers[idx].on_release;
                void *saved_release_user_data = ring->buffers[idx].release_user_data;
                if (dmabuf_bo_allocate(ring->ctx, &ring->buffers[idx], ring->width, ring->height,
                                       ring->drm_format, ring->modifier)) {
                    ring->buffers[idx].on_release = saved_on_release;
                    ring->buffers[idx].release_user_data = saved_release_user_data;
                    ring->current_idx = (idx + 1) % WAYWAL_DMABUF_RING_SIZE;
                    ring->buffers[idx].in_use = true;
                    return &ring->buffers[idx];
                }
                ring->buffers[idx].on_release = saved_on_release;
                ring->buffers[idx].release_user_data = saved_release_user_data;
                break;
            }
        }
    }

    WAYWAL_LOG_DEBUG("All DMA-BUF scanout buffers in ring are currently busy; throttling frame");
    return NULL;
}

void dmabuf_bo_mark_released(dmabuf_ring_t *ring, struct wl_buffer *buffer)
{
    if (!ring || !ring->initialized || !buffer)
        return;

    for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
        if (ring->buffers[i].wl_buffer == buffer) {
            ring->buffers[i].in_use = false;
            return;
        }
    }
}
