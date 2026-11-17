// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
} dmabuf_format_table_entry_t;
#pragma pack(pop)

typedef struct feedback_state_node {
    dmabuf_context_t *ctx;
    struct zwp_linux_dmabuf_feedback_v1 *feedback;
    dmabuf_format_table_entry_t *table;
    size_t num_entries;
    size_t table_size;
    int table_fd;
    dev_t main_dev;
    bool has_main_dev;
    dev_t tranche_dev;
    bool has_tranche_dev;
    bool is_scanout_tranche;
    bool round_active;
    uint64_t pending_preferred_modifier;
    uint32_t pending_preferred_format;
    bool pending_has_rgb;
    bool pending_locked_scanout_rgb;
    bool pending_has_10bit;
    bool pending_locked_scanout_10bit;
    uint32_t pending_format_10bit;
    uint64_t pending_modifier_10bit;
    bool pending_has_nv12;
    bool pending_has_p010;
    struct feedback_state_node *next;
} feedback_state_t;

static pthread_mutex_t g_fb_registry_mtx = PTHREAD_MUTEX_INITIALIZER;
static feedback_state_t *g_fb_registry = NULL;

static void feedback_state_reset_tranche(feedback_state_t *st)
{
    st->tranche_dev = 0;
    st->has_tranche_dev = false;
    st->is_scanout_tranche = false;
}

static void feedback_state_reset_round(feedback_state_t *st)
{
    feedback_state_reset_tranche(st);
    st->round_active = false;
    st->pending_preferred_modifier = DRM_FORMAT_MOD_INVALID;
    st->pending_preferred_format = 0u;
    st->pending_has_rgb = false;
    st->pending_locked_scanout_rgb = false;
    st->pending_has_10bit = false;
    st->pending_locked_scanout_10bit = false;
    st->pending_format_10bit = 0u;
    st->pending_modifier_10bit = DRM_FORMAT_MOD_INVALID;
    st->pending_has_nv12 = false;
    st->pending_has_p010 = false;
}

static void feedback_state_release_table(feedback_state_t *st)
{
    if (st->table && st->table != MAP_FAILED) {
        munmap(st->table, st->table_size);
    }
    st->table = NULL;
    st->num_entries = 0u;
    st->table_size = 0u;
    if (st->table_fd >= 0) {
        close(st->table_fd);
        st->table_fd = -1;
    }
}

static void feedback_context_reset_caps(dmabuf_context_t *ctx)
{
    if (!ctx)
        return;
    ctx->preferred_modifier = DRM_FORMAT_MOD_INVALID;
    ctx->preferred_format = 0u;
    ctx->has_10bit = false;
    ctx->locked_scanout_10bit = false;
    ctx->format_10bit = 0u;
    ctx->modifier_10bit = DRM_FORMAT_MOD_INVALID;
    ctx->has_nv12 = false;
    ctx->has_p010 = false;
}

static feedback_state_t *feedback_state_acquire_locked(dmabuf_context_t *ctx,
                                                       struct zwp_linux_dmabuf_feedback_v1 *fb,
                                                       bool create_if_missing)
{
    feedback_state_t *fallback = NULL;
    for (feedback_state_t *cur = g_fb_registry; cur != NULL; cur = cur->next) {
        if (cur->ctx != ctx)
            continue;
        if (fb != NULL) {
            if (cur->feedback == fb)
                return cur;
            if (cur->feedback == NULL && fallback == NULL)
                fallback = cur;
        } else {
            return cur;
        }
    }
    if (fallback != NULL) {
        fallback->feedback = fb;
        return fallback;
    }
    if (!create_if_missing)
        return NULL;

    feedback_state_t *st = (feedback_state_t *)calloc(1u, sizeof(*st));
    if (!st)
        return NULL;
    st->ctx = ctx;
    st->feedback = fb;
    st->table_fd = -1;
    feedback_state_reset_round(st);
    st->next = g_fb_registry;
    g_fb_registry = st;
    return st;
}

static void feedback_state_remove_ctx_locked(const dmabuf_context_t *ctx)
{
    feedback_state_t **link = &g_fb_registry;
    while (*link != NULL) {
        feedback_state_t *cur = *link;
        if (cur->ctx == ctx) {
            *link = cur->next;
            feedback_state_release_table(cur);
            free(cur);
        } else {
            link = &cur->next;
        }
    }
}

static void feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx)
        return;

    bool log_rgb = false;
    bool log_10bit = false;
    bool log_nv12 = false;
    bool log_p010 = false;
    uint64_t pref_mod = DRM_FORMAT_MOD_INVALID;
    uint32_t pref_fmt = 0u;
    uint32_t fmt_10 = 0u;
    uint64_t mod_10 = DRM_FORMAT_MOD_INVALID;

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, false);
    if (st) {
        uint64_t prev_mod = ctx->preferred_modifier;
        uint32_t prev_fmt = ctx->preferred_format;
        bool prev_10 = ctx->has_10bit;
        uint32_t prev_fmt_10 = ctx->format_10bit;
        uint64_t prev_mod_10 = ctx->modifier_10bit;
        bool prev_nv12 = ctx->has_nv12;
        bool prev_p010 = ctx->has_p010;

        /* Commit staged round results atomically on done so renegotiation drops stale formats. */
        ctx->preferred_modifier =
            st->pending_has_rgb ? st->pending_preferred_modifier : DRM_FORMAT_MOD_INVALID;
        ctx->preferred_format = st->pending_has_rgb ? st->pending_preferred_format : 0u;
        ctx->has_10bit = st->pending_has_10bit;
        ctx->format_10bit = st->pending_has_10bit ? st->pending_format_10bit : 0u;
        ctx->modifier_10bit =
            st->pending_has_10bit ? st->pending_modifier_10bit : DRM_FORMAT_MOD_INVALID;
        ctx->locked_scanout_10bit = false;
        ctx->has_nv12 = st->pending_has_nv12;
        ctx->has_p010 = st->pending_has_p010;

        log_rgb = st->pending_has_rgb &&
                  (ctx->preferred_modifier != prev_mod || ctx->preferred_format != prev_fmt);
        pref_mod = ctx->preferred_modifier;
        pref_fmt = ctx->preferred_format;

        log_10bit = ctx->has_10bit && (!prev_10 || ctx->format_10bit != prev_fmt_10 ||
                                       ctx->modifier_10bit != prev_mod_10);
        fmt_10 = ctx->format_10bit;
        mod_10 = ctx->modifier_10bit;

        log_nv12 = ctx->has_nv12 && !prev_nv12;
        log_p010 = ctx->has_p010 && !prev_p010;

        feedback_state_reset_round(st);
    } else {
        feedback_context_reset_caps(ctx);
    }
    pthread_mutex_unlock(&g_fb_registry_mtx);

    if (log_rgb) {
        WAYWAL_LOG_INFO("Negotiated optimal DMA-BUF scanout modifier: 0x%016lx (format: 0x%08x)",
                        (unsigned long)pref_mod, pref_fmt);
    }
    if (log_10bit) {
        WAYWAL_LOG_INFO("Detected 10-bit wide gamut scanout capability: 0x%08x "
                        "(modifier: 0x%016lx)",
                        fmt_10, (unsigned long)mod_10);
    }
    if (log_nv12) {
        WAYWAL_LOG_INFO("Detected direct NV12 YUV420 DMA-BUF support");
    }
    if (log_p010) {
        WAYWAL_LOG_INFO("Detected direct P010 10-bit YUV420 DMA-BUF support");
    }
}

static void feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                  int32_t fd, uint32_t size)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, true);
    if (!st) {
        pthread_mutex_unlock(&g_fb_registry_mtx);
        if (fd >= 0) {
            close(fd);
        }
        return;
    }

    feedback_state_release_table(st);
    feedback_state_reset_tranche(st);
    st->round_active = true;

    struct stat st_buf;
    if (fd < 0 || fstat(fd, &st_buf) != 0 || !S_ISREG(st_buf.st_mode) ||
        st_buf.st_size < (off_t)size || size < sizeof(dmabuf_format_table_entry_t)) {
        if (fd >= 0) {
            close(fd);
        }
        pthread_mutex_unlock(&g_fb_registry_mtx);
        return;
    }

    void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    /* The client owns fd and must close it once mapped (mmap retains its own reference). */
    close(fd);
    st->table_fd = -1;

    if (mapped == MAP_FAILED) {
        pthread_mutex_unlock(&g_fb_registry_mtx);
        WAYWAL_LOG_ERR("Failed to mmap dmabuf feedback format table");
        return;
    }

    st->table = (dmabuf_format_table_entry_t *)mapped;
    st->table_size = size;
    st->num_entries = size / sizeof(dmabuf_format_table_entry_t);
    pthread_mutex_unlock(&g_fb_registry_mtx);
}

static void feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                 struct wl_array *device)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx || !device || device->size < sizeof(dev_t) || !device->data)
        return;

    dev_t dev = 0;
    memcpy(&dev, device->data, sizeof(dev_t));

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, true);
    if (st) {
        st->main_dev = dev;
        st->has_main_dev = true;
        st->round_active = true;
    }
    ctx->compositor_dev = dev;
    ctx->has_compositor_dev = true;
    pthread_mutex_unlock(&g_fb_registry_mtx);

    WAYWAL_LOG_INFO("Compositor main DRM device: %u:%u (dev_t %lu)", (unsigned)major(dev),
                    (unsigned)minor(dev), (unsigned long)dev);
}

static void feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx)
        return;

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, false);
    if (st) {
        feedback_state_reset_tranche(st);
    }
    pthread_mutex_unlock(&g_fb_registry_mtx);
}

static void feedback_tranche_target_device(void *data,
                                           struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                           struct wl_array *device)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx)
        return;

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, true);
    if (st) {
        st->round_active = true;
        if (!device || device->size < sizeof(dev_t) || !device->data) {
            st->tranche_dev = 0;
            st->has_tranche_dev = false;
        } else {
            memcpy(&st->tranche_dev, device->data, sizeof(dev_t));
            st->has_tranche_dev = true;
        }
    }
    pthread_mutex_unlock(&g_fb_registry_mtx);
}

static void feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                     struct wl_array *indices)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx || !indices || (indices->size > 0 && !indices->data))
        return;

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, false);
    if (!st || !st->table) {
        pthread_mutex_unlock(&g_fb_registry_mtx);
        return;
    }

    st->round_active = true;
    dev_t main_dev = st->has_main_dev ? st->main_dev : ctx->compositor_dev;
    bool have_main = st->has_main_dev || ctx->has_compositor_dev;
    if (have_main && st->has_tranche_dev && st->tranche_dev != main_dev) {
        pthread_mutex_unlock(&g_fb_registry_mtx);
        return;
    }

    const uint16_t *idx_ptr = (const uint16_t *)indices->data;
    size_t count = indices->size / sizeof(uint16_t);

    for (size_t i = 0; i < count; ++i) {
        uint16_t idx = idx_ptr[i];
        if ((size_t)idx >= st->num_entries) {
            continue;
        }
        uint32_t fmt = st->table[idx].format;
        uint64_t mod = st->table[idx].modifier;

        if (fmt == DRM_FORMAT_ARGB8888 || fmt == DRM_FORMAT_XRGB8888) {
            if (!st->pending_locked_scanout_rgb &&
                (!st->pending_has_rgb || st->pending_preferred_modifier == DRM_FORMAT_MOD_INVALID ||
                 st->is_scanout_tranche)) {
                st->pending_preferred_modifier = mod;
                st->pending_preferred_format = fmt;
                st->pending_has_rgb = true;
                if (st->is_scanout_tranche) {
                    st->pending_locked_scanout_rgb = true;
                }
            }
        } else if (fmt == DRM_FORMAT_XRGB2101010 || fmt == DRM_FORMAT_ARGB2101010) {
            if (!st->pending_locked_scanout_10bit &&
                (!st->pending_has_10bit || st->is_scanout_tranche)) {
                st->pending_has_10bit = true;
                st->pending_format_10bit = fmt;
                st->pending_modifier_10bit = mod;
                if (st->is_scanout_tranche) {
                    st->pending_locked_scanout_10bit = true;
                }
            }
        } else if (fmt == DRM_FORMAT_NV12) {
            st->pending_has_nv12 = true;
        } else if (fmt == DRM_FORMAT_P010) {
            st->pending_has_p010 = true;
        }
    }
    pthread_mutex_unlock(&g_fb_registry_mtx);
}

static void feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                   uint32_t flags)
{
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx)
        return;

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_t *st = feedback_state_acquire_locked(ctx, feedback, true);
    if (st) {
        st->round_active = true;
        st->is_scanout_tranche = (flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT) != 0u;
    }
    pthread_mutex_unlock(&g_fb_registry_mtx);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
    .done = feedback_done,
    .format_table = feedback_format_table,
    .main_device = feedback_main_device,
    .tranche_done = feedback_tranche_done,
    .tranche_target_device = feedback_tranche_target_device,
    .tranche_formats = feedback_tranche_formats,
    .tranche_flags = feedback_tranche_flags,
};

void dmabuf_feedback_init(dmabuf_context_t *ctx)
{
    if (!ctx || !ctx->dmabuf_proto)
        return;

    if (ctx->default_feedback) {
        dmabuf_feedback_destroy(ctx);
    }

    if (zwp_linux_dmabuf_v1_get_version(ctx->dmabuf_proto) >= 4) {
        feedback_context_reset_caps(ctx);
        struct zwp_linux_dmabuf_feedback_v1 *fb =
            zwp_linux_dmabuf_v1_get_default_feedback(ctx->dmabuf_proto);
        if (fb) {
            pthread_mutex_lock(&g_fb_registry_mtx);
            feedback_state_t *st = feedback_state_acquire_locked(ctx, fb, true);
            if (st) {
                feedback_state_reset_round(st);
            }
            pthread_mutex_unlock(&g_fb_registry_mtx);
            if (!st) {
                zwp_linux_dmabuf_feedback_v1_destroy(fb);
                return;
            }
            ctx->default_feedback = fb;
            zwp_linux_dmabuf_feedback_v1_add_listener(fb, &feedback_listener, ctx);
            WAYWAL_LOG_INFO("DMA-BUF feedback listener initialized");
        }
    }
}

void dmabuf_feedback_destroy(dmabuf_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->default_feedback) {
        zwp_linux_dmabuf_feedback_v1_destroy(ctx->default_feedback);
        ctx->default_feedback = NULL;
    }

    pthread_mutex_lock(&g_fb_registry_mtx);
    feedback_state_remove_ctx_locked(ctx);
    pthread_mutex_unlock(&g_fb_registry_mtx);

    feedback_context_reset_caps(ctx);
}
