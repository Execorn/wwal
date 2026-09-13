#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
} dmabuf_format_table_entry_t;
#pragma pack(pop)

typedef struct {
    dmabuf_format_table_entry_t *table;
    size_t num_entries;
    int table_fd;
    bool is_scanout_tranche;
} feedback_state_t;

static feedback_state_t g_fb_state = {0};

static void feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    (void)data;
    (void)feedback;
    if (g_fb_state.table && g_fb_state.table != MAP_FAILED) {
        munmap(g_fb_state.table, g_fb_state.num_entries * sizeof(dmabuf_format_table_entry_t));
        g_fb_state.table = NULL;
    }
    if (g_fb_state.table_fd >= 0) {
        close(g_fb_state.table_fd);
        g_fb_state.table_fd = -1;
    }
    g_fb_state.num_entries = 0;
}

static void feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                  int32_t fd, uint32_t size)
{
    (void)data;
    (void)feedback;
    if (g_fb_state.table && g_fb_state.table != MAP_FAILED) {
        munmap(g_fb_state.table, g_fb_state.num_entries * sizeof(dmabuf_format_table_entry_t));
    }
    if (g_fb_state.table_fd >= 0) {
        close(g_fb_state.table_fd);
    }

    g_fb_state.table_fd = fd;
    g_fb_state.num_entries = size / sizeof(dmabuf_format_table_entry_t);
    g_fb_state.table =
        (dmabuf_format_table_entry_t *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (g_fb_state.table == MAP_FAILED) {
        WAYWAL_LOG_ERR("Failed to mmap dmabuf feedback format table");
        g_fb_state.table = NULL;
        close(fd);
        g_fb_state.table_fd = -1;
    }
}

static void feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                 struct wl_array *device)
{
    (void)data;
    (void)feedback;
    (void)device;
}

static void feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    (void)data;
    (void)feedback;
    g_fb_state.is_scanout_tranche = false;
}

static void feedback_tranche_target_device(void *data,
                                           struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                           struct wl_array *device)
{
    (void)data;
    (void)feedback;
    (void)device;
}

static void feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                     struct wl_array *indices)
{
    (void)feedback;
    dmabuf_context_t *ctx = (dmabuf_context_t *)data;
    if (!ctx || !g_fb_state.table)
        return;

    const uint16_t *idx_ptr = (const uint16_t *)indices->data;
    size_t count = indices->size / sizeof(uint16_t);

    for (size_t i = 0; i < count; ++i) {
        uint16_t idx = idx_ptr[i];
        if (idx < g_fb_state.num_entries) {
            uint32_t fmt = g_fb_state.table[idx].format;
            uint64_t mod = g_fb_state.table[idx].modifier;

            if (fmt == DRM_FORMAT_ARGB8888 || fmt == DRM_FORMAT_XRGB8888) {
                if (ctx->preferred_modifier == DRM_FORMAT_MOD_INVALID ||
                    g_fb_state.is_scanout_tranche) {
                    ctx->preferred_modifier = mod;
                    ctx->preferred_format = fmt;
                    WAYWAL_LOG_INFO(
                        "Negotiated optimal DMA-BUF scanout modifier: 0x%016lx (format: 0x%08x)",
                        (unsigned long)mod, fmt);
                    if (g_fb_state.is_scanout_tranche) {
                        break;
                    }
                }
            } else if (fmt == DRM_FORMAT_XRGB2101010 || fmt == DRM_FORMAT_ARGB2101010 ||
                       fmt == DRM_FORMAT_XBGR2101010 || fmt == DRM_FORMAT_ABGR2101010) {
                if (!ctx->has_10bit || g_fb_state.is_scanout_tranche) {
                    ctx->has_10bit = true;
                    ctx->format_10bit = fmt;
                    ctx->modifier_10bit = mod;
                    WAYWAL_LOG_INFO("Detected 10-bit wide gamut scanout capability: 0x%08x "
                                    "(modifier: 0x%016lx)",
                                    fmt, (unsigned long)mod);
                }
            }
        }
    }
}

static void feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback,
                                   uint32_t flags)
{
    (void)data;
    (void)feedback;
    if (flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT) {
        g_fb_state.is_scanout_tranche = true;
    }
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

    if (zwp_linux_dmabuf_v1_get_version(ctx->dmabuf_proto) >= 4) {
        ctx->default_feedback = zwp_linux_dmabuf_v1_get_default_feedback(ctx->dmabuf_proto);
        if (ctx->default_feedback) {
            zwp_linux_dmabuf_feedback_v1_add_listener(ctx->default_feedback, &feedback_listener,
                                                      ctx);
            WAYWAL_LOG_INFO("DMA-BUF feedback listener initialized");
        }
    }
}

void dmabuf_feedback_destroy(dmabuf_context_t *ctx)
{
    if (!ctx || !ctx->default_feedback)
        return;
    zwp_linux_dmabuf_feedback_v1_destroy(ctx->default_feedback);
    ctx->default_feedback = NULL;
}
