#include "waywal/output_state.h"

#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void output_handle_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel, const char *make, const char *model,
                                   int32_t transform)
{
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    output_state_t *st = (output_state_t *)data;
    if (st) {
        st->transform = transform;
        if (make && model && !st->description[0]) {
            snprintf(st->description, sizeof(st->description), "%s %s", make, model);
        }
    }
}

static void output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                               int32_t height, int32_t refresh)
{
    (void)output;
    output_state_t *st = (output_state_t *)data;
    if (st && (flags & WL_OUTPUT_MODE_CURRENT)) {
        if (width > 0 && height > 0) {
            st->width = (uint32_t)width;
            st->height = (uint32_t)height;
        }
        if (refresh > 0) {
            st->refresh_mhz = (uint32_t)refresh;
        }
    }
}

static void output_handle_done(void *data, struct wl_output *output)
{
    (void)output;
    output_state_t *st = (output_state_t *)data;
    if (st) {
        WAYWAL_LOG_DEBUG("Output %s (%s) geometry resolved: %ux%u @ %u mHz",
                         st->name[0] ? st->name : "unnamed",
                         st->description[0] ? st->description : "unknown", st->width, st->height,
                         st->refresh_mhz);
    }
}

static void output_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    output_state_t *st = (output_state_t *)data;
    if (st) {
        st->scale = factor;
    }
}

static void output_handle_name(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    output_state_t *st = (output_state_t *)data;
    if (st && name) {
        strncpy(st->name, name, sizeof(st->name) - 1);
    }
}

static void output_handle_description(void *data, struct wl_output *output, const char *description)
{
    (void)output;
    output_state_t *st = (output_state_t *)data;
    if (st && description) {
        strncpy(st->description, description, sizeof(st->description) - 1);
    }
}

static const struct wl_output_listener g_output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

void output_manager_init(output_manager_t *om, dmabuf_context_t *ctx, struct wp_presentation *pres)
{
    if (!om)
        return;
    memset(om, 0, sizeof(*om));
    om->dmabuf_ctx = ctx;
    om->wp_pres = pres;
    WAYWAL_LOG_INFO("In-process display output manager initialized");
}

void output_manager_destroy(output_manager_t *om)
{
    if (!om)
        return;
    output_state_t *cur = om->head;
    while (cur) {
        output_state_t *next = cur->next;

        if (cur->use_dmabuf && om->dmabuf_ctx) {
            dmabuf_ring_destroy(om->dmabuf_ctx, &cur->dmabuf_ring);
        }

        presentation_sync_destroy(&cur->pres_sync);

        if (cur->output) {
            wl_output_destroy(cur->output);
        }

        free(cur);
        cur = next;
    }
    om->head = NULL;
    om->count = 0;
}

output_state_t *output_manager_add(output_manager_t *om, struct wl_registry *reg, uint32_t name,
                                   uint32_t version)
{
    if (!om || !reg)
        return NULL;

    output_state_t *st = (output_state_t *)calloc(1, sizeof(output_state_t));
    if (!st)
        return NULL;

    st->wl_name = name;
    st->scale = 1;
    st->shm_fd = -1;

    uint32_t bind_ver = (version > 4) ? 4 : version;
    st->output = (struct wl_output *)wl_registry_bind(reg, name, &wl_output_interface, bind_ver);
    if (!st->output) {
        free(st);
        return NULL;
    }

    wl_output_add_listener(st->output, &g_output_listener, st);

    /* Insert into linked list */
    st->next = om->head;
    om->head = st;
    om->count++;

    WAYWAL_LOG_INFO("Hotplug: registered display output node id %u (active displays: %u)", name,
                    om->count);
    return st;
}

void output_manager_remove(output_manager_t *om, uint32_t name)
{
    if (!om || !om->head)
        return;

    output_state_t **curr = &om->head;
    while (*curr) {
        output_state_t *entry = *curr;
        if (entry->wl_name == name) {
            *curr = entry->next;
            om->count--;

            WAYWAL_LOG_INFO("Hotplug: display unplugged id %u (%s) (remaining: %u)", name,
                            entry->name[0] ? entry->name : "unnamed", om->count);

            if (entry->use_dmabuf && om->dmabuf_ctx) {
                dmabuf_ring_destroy(om->dmabuf_ctx, &entry->dmabuf_ring);
            }

            presentation_sync_destroy(&entry->pres_sync);

            if (entry->output) {
                wl_output_destroy(entry->output);
            }

            free(entry);
            return;
        }
        curr = &entry->next;
    }
}

output_state_t *output_manager_find_by_name(output_manager_t *om, const char *name)
{
    if (!om || !name)
        return NULL;
    for (output_state_t *cur = om->head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->name, name) == 0)
            return cur;
    }
    return NULL;
}

output_state_t *output_manager_find_by_id(output_manager_t *om, uint32_t name)
{
    if (!om)
        return NULL;
    for (output_state_t *cur = om->head; cur != NULL; cur = cur->next) {
        if (cur->wl_name == name)
            return cur;
    }
    return NULL;
}
