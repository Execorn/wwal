// SPDX-License-Identifier: GPL-3.0-or-later
#include "wayland_core.h"

#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/power_governor.h"

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Declared in dmabuf_alloc.c — matches compositor dev_t to /dev/dri/renderD* */
int open_drm_node_by_devt(dev_t target_dev);

typedef struct wlr_toplevel_entry {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    daemon_state_t *state;
    struct wl_output *outputs[8];
    size_t num_outputs;
    bool maximized;
    bool fullscreen;
    bool minimized;
    struct wlr_toplevel_entry *next;
} wlr_toplevel_entry_t;

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer;
    daemon_state_t *state = (daemon_state_t *)data;
    if (!state || !surface)
        return;

    if (state->cursor_shape_dev) {
        wp_cursor_shape_device_v1_set_shape(state->cursor_shape_dev, serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->surface == surface) {
            out->cursor_x = wl_fixed_to_double(surface_x);
            out->cursor_y = wl_fixed_to_double(surface_y);
            out->has_cursor = true;
        } else {
            out->has_cursor = false;
        }
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    daemon_state_t *state = (daemon_state_t *)data;
    if (!state || !surface)
        return;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->surface == surface) {
            out->has_cursor = false;
            out->button_down = false;
            break;
        }
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer;
    (void)time;
    daemon_state_t *state = (daemon_state_t *)data;
    if (!state)
        return;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->has_cursor) {
            out->cursor_x = wl_fixed_to_double(surface_x);
            out->cursor_y = wl_fixed_to_double(surface_y);
            break;
        }
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state_val)
{
    (void)pointer;
    (void)serial;
    (void)time;
    (void)button;
    daemon_state_t *state = (daemon_state_t *)data;
    if (!state)
        return;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->has_cursor) {
            if (state_val == WL_POINTER_BUTTON_STATE_PRESSED) {
                out->button_down = true;
                out->click_pulse = true;
                out->click_x = out->cursor_x;
                out->click_y = out->cursor_y;
            } else if (state_val == WL_POINTER_BUTTON_STATE_RELEASED) {
                out->button_down = false;
            }
            break;
        }
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    daemon_state_t *state = (daemon_state_t *)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !state->pointer) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
        if (state->cursor_shape_mgr && !state->cursor_shape_dev) {
            state->cursor_shape_dev =
                wp_cursor_shape_manager_v1_get_pointer(state->cursor_shape_mgr, state->pointer);
        }
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && state->pointer) {
        if (state->cursor_shape_dev) {
            wp_cursor_shape_device_v1_destroy(state->cursor_shape_dev);
            state->cursor_shape_dev = NULL;
        }
        wl_pointer_destroy(state->pointer);
        state->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void xdg_output_handle_logical_position(void *data, struct zxdg_output_v1 *xdg_output,
                                               int32_t x, int32_t y)
{
    (void)xdg_output;
    output_node_t *out = (output_node_t *)data;
    if (out) {
        out->logical_x = x;
        out->logical_y = y;
        out->has_logical_pos = true;
    }
}

static void xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                                           int32_t width, int32_t height)
{
    (void)xdg_output;
    output_node_t *out = (output_node_t *)data;
    if (out && width > 0 && height > 0) {
        out->logical_width = (uint32_t)width;
        out->logical_height = (uint32_t)height;
    }
}

static void xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
    (void)data;
    (void)xdg_output;
}

static void xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name)
{
    (void)xdg_output;
    output_node_t *out = (output_node_t *)data;
    if (out && name && !out->name[0]) {
        snprintf(out->name, sizeof(out->name), "%s", name);
    }
}

static void xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                                          const char *description)
{
    (void)xdg_output;
    output_node_t *out = (output_node_t *)data;
    if (out && description && !out->description[0]) {
        snprintf(out->description, sizeof(out->description), "%s", description);
    }
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static void bind_xdg_output_for_node(daemon_state_t *state, output_node_t *node)
{
    if (!state || !state->xdg_output_mgr || !node || !node->wl_output || node->xdg_output)
        return;
    node->xdg_output =
        zxdg_output_manager_v1_get_xdg_output(state->xdg_output_mgr, node->wl_output);
    if (node->xdg_output) {
        zxdg_output_v1_add_listener(node->xdg_output, &xdg_output_listener, node);
    }
}

static void idle_handle_idled(void *data, struct ext_idle_notification_v1 *notification)
{
    (void)notification;
    daemon_state_t *state = (daemon_state_t *)data;
    if (state && state->power_gov) {
        waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_SESSION_IDLE, true);
        if (state->on_power_state_changed) {
            state->on_power_state_changed(state);
        }
    }
}

static void idle_handle_resumed(void *data, struct ext_idle_notification_v1 *notification)
{
    (void)notification;
    daemon_state_t *state = (daemon_state_t *)data;
    if (state && state->power_gov) {
        waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_SESSION_IDLE, false);
        if (state->on_power_state_changed) {
            state->on_power_state_changed(state);
        }
    }
}

static const struct ext_idle_notification_v1_listener idle_notification_listener = {
    .idled = idle_handle_idled,
    .resumed = idle_handle_resumed,
};

static void ensure_idle_notification(daemon_state_t *state)
{
    if (!state || state->idle_notification || !state->idle_notifier || !state->seat)
        return;
    if (!state->config.daemon.pause_on_idle && state->idle_timeout_sec == 0)
        return;
    uint32_t timeout_sec = state->idle_timeout_sec > 0 ? state->idle_timeout_sec : 300U;
    uint32_t timeout_ms = timeout_sec > 4000000U ? 4000000000U : timeout_sec * 1000U;
    state->idle_notification =
        ext_idle_notifier_v1_get_idle_notification(state->idle_notifier, timeout_ms, state->seat);
    if (state->idle_notification) {
        ext_idle_notification_v1_add_listener(state->idle_notification, &idle_notification_listener,
                                              state);
    }
}

static void recompute_wlr_toplevel_power_state(daemon_state_t *state)
{
    if (!state || !state->power_gov)
        return;

    bool changed = false;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->name[0])
            continue;
        bool any_fs = false;
        bool any_max = false;
        for (wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)state->wlr_toplevels; e != NULL;
             e = e->next) {
            if (e->minimized)
                continue;
            bool on_out = (e->num_outputs == 0);
            for (size_t i = 0; i < e->num_outputs; ++i) {
                if (e->outputs[i] == out->wl_output) {
                    on_out = true;
                    break;
                }
            }
            if (!on_out)
                continue;
            if (e->fullscreen)
                any_fs = true;
            if (e->maximized)
                any_max = true;
        }
        bool pause_fs = any_fs && state->config.daemon.auto_pause_fullscreen;
        bool pause_max = any_max && state->config.daemon.auto_pause_maximized;
        uint32_t prev_mask = waywal_power_governor_get_output_mask(state->power_gov, out->name);
        waywal_power_governor_set_output_reason(state->power_gov, out->name,
                                                WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, pause_fs);
        waywal_power_governor_set_output_reason(state->power_gov, out->name,
                                                WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED, pause_max);
        uint32_t new_mask = waywal_power_governor_get_output_mask(state->power_gov, out->name);
        if (prev_mask != new_mask)
            changed = true;
    }

    if (changed && state->on_power_state_changed) {
        state->on_power_state_changed(state);
    }
}

static void wlr_toplevel_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                      const char *title)
{
    (void)data;
    (void)toplevel;
    (void)title;
}

static void wlr_toplevel_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                       const char *app_id)
{
    (void)data;
    (void)toplevel;
    (void)app_id;
}

static void wlr_toplevel_handle_output_enter(void *data,
                                             struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                             struct wl_output *output)
{
    (void)toplevel;
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)data;
    if (!e || !output)
        return;
    for (size_t i = 0; i < e->num_outputs; ++i) {
        if (e->outputs[i] == output)
            return;
    }
    if (e->num_outputs < 8) {
        e->outputs[e->num_outputs++] = output;
    }
}

static void wlr_toplevel_handle_output_leave(void *data,
                                             struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                             struct wl_output *output)
{
    (void)toplevel;
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)data;
    if (!e || !output)
        return;
    for (size_t i = 0; i < e->num_outputs; ++i) {
        if (e->outputs[i] == output) {
            e->outputs[i] = e->outputs[e->num_outputs - 1];
            e->num_outputs--;
            return;
        }
    }
}

static void wlr_toplevel_handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                      struct wl_array *state_arr)
{
    (void)toplevel;
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)data;
    if (!e || !state_arr)
        return;
    e->maximized = false;
    e->fullscreen = false;
    e->minimized = false;
    const uint32_t *vals = (const uint32_t *)state_arr->data;
    size_t count = state_arr->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        if (vals[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED)
            e->maximized = true;
        else if (vals[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            e->minimized = true;
        else if (vals[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN)
            e->fullscreen = true;
    }
}

static void wlr_toplevel_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel)
{
    (void)toplevel;
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)data;
    if (e && e->state) {
        recompute_wlr_toplevel_power_state(e->state);
    }
}

static void wlr_toplevel_handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel)
{
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)data;
    daemon_state_t *state = e ? e->state : NULL;
    if (state && e) {
        wlr_toplevel_entry_t **pp = (wlr_toplevel_entry_t **)&state->wlr_toplevels;
        while (*pp) {
            if (*pp == e) {
                *pp = e->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    if (toplevel) {
        zwlr_foreign_toplevel_handle_v1_destroy(toplevel);
    }
    free(e);
    if (state) {
        recompute_wlr_toplevel_power_state(state);
    }
}

static void wlr_toplevel_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                                       struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data;
    (void)toplevel;
    (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener wlr_toplevel_handle_listener = {
    .title = wlr_toplevel_handle_title,
    .app_id = wlr_toplevel_handle_app_id,
    .output_enter = wlr_toplevel_handle_output_enter,
    .output_leave = wlr_toplevel_handle_output_leave,
    .state = wlr_toplevel_handle_state,
    .done = wlr_toplevel_handle_done,
    .closed = wlr_toplevel_handle_closed,
    .parent = wlr_toplevel_handle_parent,
};

static void wlr_toplevel_mgr_handle_toplevel(void *data,
                                             struct zwlr_foreign_toplevel_manager_v1 *manager,
                                             struct zwlr_foreign_toplevel_handle_v1 *toplevel)
{
    (void)manager;
    daemon_state_t *state = (daemon_state_t *)data;
    if (!state || !toplevel)
        return;
    wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)calloc(1, sizeof(wlr_toplevel_entry_t));
    if (!e)
        return;
    e->handle = toplevel;
    e->state = state;
    e->next = (wlr_toplevel_entry_t *)state->wlr_toplevels;
    state->wlr_toplevels = e;
    zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &wlr_toplevel_handle_listener, e);
}

static void wlr_toplevel_mgr_handle_finished(void *data,
                                             struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    daemon_state_t *state = (daemon_state_t *)data;
    if (manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(manager);
    }
    if (state && state->wlr_foreign_toplevel_mgr == manager) {
        state->wlr_foreign_toplevel_mgr = NULL;
    }
}

static const struct zwlr_foreign_toplevel_manager_v1_listener wlr_toplevel_mgr_listener = {
    .toplevel = wlr_toplevel_mgr_handle_toplevel,
    .finished = wlr_toplevel_mgr_handle_finished,
};

static void foreign_toplevel_handle_closed(void *data,
                                           struct ext_foreign_toplevel_handle_v1 *toplevel)
{
    (void)data;
    if (toplevel) {
        ext_foreign_toplevel_handle_v1_destroy(toplevel);
    }
}

static void foreign_toplevel_handle_done(void *data,
                                         struct ext_foreign_toplevel_handle_v1 *toplevel)
{
    (void)data;
    (void)toplevel;
}

static void foreign_toplevel_handle_title(void *data,
                                          struct ext_foreign_toplevel_handle_v1 *toplevel,
                                          const char *title)
{
    (void)data;
    (void)toplevel;
    (void)title;
}

static void foreign_toplevel_handle_app_id(void *data,
                                           struct ext_foreign_toplevel_handle_v1 *toplevel,
                                           const char *app_id)
{
    (void)data;
    (void)toplevel;
    (void)app_id;
}

static void foreign_toplevel_handle_identifier(void *data,
                                               struct ext_foreign_toplevel_handle_v1 *toplevel,
                                               const char *identifier)
{
    (void)data;
    (void)toplevel;
    (void)identifier;
}

static const struct ext_foreign_toplevel_handle_v1_listener foreign_toplevel_handle_listener = {
    .closed = foreign_toplevel_handle_closed,
    .done = foreign_toplevel_handle_done,
    .title = foreign_toplevel_handle_title,
    .app_id = foreign_toplevel_handle_app_id,
    .identifier = foreign_toplevel_handle_identifier,
};

static void foreign_toplevel_list_handle_toplevel(void *data,
                                                  struct ext_foreign_toplevel_list_v1 *list,
                                                  struct ext_foreign_toplevel_handle_v1 *toplevel)
{
    (void)list;
    if (toplevel) {
        ext_foreign_toplevel_handle_v1_add_listener(toplevel, &foreign_toplevel_handle_listener,
                                                    data);
    }
}

static void foreign_toplevel_list_handle_finished(void *data,
                                                  struct ext_foreign_toplevel_list_v1 *list)
{
    daemon_state_t *state = (daemon_state_t *)data;
    if (list) {
        ext_foreign_toplevel_list_v1_destroy(list);
    }
    if (state && state->foreign_toplevel_list == list) {
        state->foreign_toplevel_list = NULL;
    }
}

static const struct ext_foreign_toplevel_list_v1_listener foreign_toplevel_list_listener = {
    .toplevel = foreign_toplevel_list_handle_toplevel,
    .finished = foreign_toplevel_list_handle_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    daemon_state_t *state = (daemon_state_t *)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t ver = version >= 4 ? 4 : version;
        state->compositor =
            (struct wl_compositor *)wl_registry_bind(registry, name, &wl_compositor_interface, ver);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t ver = version >= 4 ? 4 : version;
        state->layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, ver);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->viewporter =
            (struct wp_viewporter *)wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        state->fract_manager = (struct wp_fractional_scale_manager_v1 *)wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        state->single_pixel_mgr = (struct wp_single_pixel_buffer_manager_v1 *)wl_registry_bind(
            registry, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
        WAYWAL_LOG_INFO("Bound wp_single_pixel_buffer_manager_v1");
    } else if (strcmp(interface, wp_linux_drm_syncobj_manager_v1_interface.name) == 0) {
        state->syncobj_mgr = (struct wp_linux_drm_syncobj_manager_v1 *)wl_registry_bind(
            registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1);
        WAYWAL_LOG_INFO("Bound wp_linux_drm_syncobj_manager_v1");
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        uint32_t ver = version >= 3 ? 3 : version;
        state->xdg_output_mgr = (struct zxdg_output_manager_v1 *)wl_registry_bind(
            registry, name, &zxdg_output_manager_v1_interface, ver);
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            bind_xdg_output_for_node(state, out);
        }
        WAYWAL_LOG_INFO("Bound zxdg_output_manager_v1 version %u", ver);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        state->cursor_shape_mgr = (struct wp_cursor_shape_manager_v1 *)wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
        if (state->cursor_shape_mgr && state->pointer && !state->cursor_shape_dev) {
            state->cursor_shape_dev =
                wp_cursor_shape_manager_v1_get_pointer(state->cursor_shape_mgr, state->pointer);
        }
        WAYWAL_LOG_INFO("Bound wp_cursor_shape_manager_v1");
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        uint32_t ver = version >= 3 ? 3 : version;
        state->wlr_foreign_toplevel_mgr =
            (struct zwlr_foreign_toplevel_manager_v1 *)wl_registry_bind(
                registry, name, &zwlr_foreign_toplevel_manager_v1_interface, ver);
        if (state->wlr_foreign_toplevel_mgr) {
            zwlr_foreign_toplevel_manager_v1_add_listener(state->wlr_foreign_toplevel_mgr,
                                                          &wlr_toplevel_mgr_listener, state);
        }
        WAYWAL_LOG_INFO("Bound zwlr_foreign_toplevel_manager_v1 version %u", ver);
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        uint32_t ver = version >= 2 ? 2 : 1;
        state->idle_notifier = (struct ext_idle_notifier_v1 *)wl_registry_bind(
            registry, name, &ext_idle_notifier_v1_interface, ver);
        ensure_idle_notification(state);
        WAYWAL_LOG_INFO("Bound ext_idle_notifier_v1 version %u", ver);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        state->foreign_toplevel_list = (struct ext_foreign_toplevel_list_v1 *)wl_registry_bind(
            registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        if (state->foreign_toplevel_list) {
            ext_foreign_toplevel_list_v1_add_listener(state->foreign_toplevel_list,
                                                      &foreign_toplevel_list_listener, state);
        }
        WAYWAL_LOG_INFO("Bound ext_foreign_toplevel_list_v1");
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        uint32_t ver = version >= 4 ? 4 : (version >= 3 ? 3 : version);
        state->dmabuf_ctx.dmabuf_proto = (struct zwp_linux_dmabuf_v1 *)wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface, ver);
        WAYWAL_LOG_INFO("Bound zwp_linux_dmabuf_v1 version %u", ver);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        state->presentation = (struct wp_presentation *)wl_registry_bind(
            registry, name, &wp_presentation_interface, 1);
        state->output_mgr.wp_pres = state->presentation;
        WAYWAL_LOG_INFO("Bound wp_presentation interface");
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t ver = version >= 5 ? 5 : version;
        state->seat = (struct wl_seat *)wl_registry_bind(registry, name, &wl_seat_interface, ver);
        wl_seat_add_listener(state->seat, &seat_listener, state);
        ensure_idle_notification(state);
        WAYWAL_LOG_INFO("Bound wl_seat interface version %u", ver);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        output_node_t *node = output_manager_add(&state->output_mgr, registry, name, version);
        if (node) {
            node->state = state;
            state->outputs = state->output_mgr.head;
            state->num_outputs = state->output_mgr.count;
            bind_xdg_output_for_node(state, node);
            WAYWAL_LOG_INFO("Discovered Wayland output id %u", name);
            /* Defer surface creation during initial startup until dmabuf_feedback_init
             * has negotiated the compositor's DRM device and format modifiers. */
            if (state->layer_shell && state->dmabuf_ctx.default_feedback) {
                output_node_create_surface(state, node);
            }
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry;
    daemon_state_t *state = (daemon_state_t *)data;

    output_node_t *node = output_manager_find_by_id(&state->output_mgr, name);
    if (node) {
        WAYWAL_LOG_INFO("Removing Wayland output id %u (%s)", name, node->name);
        if (state->video_engine.target_output == node) {
            state->video_engine.target_output = NULL;
        }
        output_node_destroy_surface(node);
        output_manager_remove(&state->output_mgr, name);
        state->outputs = state->output_mgr.head;
        state->num_outputs = state->output_mgr.count;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

bool daemon_wayland_init(daemon_state_t *state)
{
    if (!state)
        return false;

    state->cursor_ipc_fd = -1;
    state->display = wl_display_connect(NULL);
    if (!state->display) {
        WAYWAL_LOG_ERR(
            "Could not connect to Wayland display (check $WAYLAND_DISPLAY / $XDG_RUNTIME_DIR)");
        return false;
    }

    /* Initialize hardware DRM/GBM context */
    dmabuf_context_init(&state->dmabuf_ctx);

    /* Initialize display output manager (ARCH-05) */
    output_manager_init(&state->output_mgr, &state->dmabuf_ctx, state->presentation);
    state->outputs = state->output_mgr.head;
    state->num_outputs = state->output_mgr.count;

    state->registry = wl_display_get_registry(state->display);
    if (!state->registry) {
        WAYWAL_LOG_ERR("Failed to get Wayland registry");
        wl_display_disconnect(state->display);
        state->display = NULL;
        return false;
    }

    wl_registry_add_listener(state->registry, &registry_listener, state);

    /* Roundtrip to discover globals */
    if (wl_display_roundtrip(state->display) < 0) {
        WAYWAL_LOG_ERR("wl_display_roundtrip failed during global registry discovery");
        return false;
    }

    if (!state->compositor) {
        WAYWAL_LOG_ERR("Compositor does not support wl_compositor");
        return false;
    }
    if (!state->shm) {
        WAYWAL_LOG_ERR("Compositor does not support wl_shm");
        return false;
    }
    if (!state->layer_shell) {
        WAYWAL_LOG_ERR("Compositor does not support zwlr_layer_shell_v1 (required for wallpapers)");
        return false;
    }

    /* Roundtrip to gather initial output geometry and events */
    if (wl_display_roundtrip(state->display) < 0) {
        WAYWAL_LOG_ERR("wl_display_roundtrip failed during output discovery");
        return false;
    }

    /* Initialize DMA-BUF feedback if supported by compositor */
    if (state->dmabuf_ctx.available && state->dmabuf_ctx.dmabuf_proto) {
        dmabuf_feedback_init(&state->dmabuf_ctx);
        /* Roundtrip to receive main_device + format modifier tranches */
        (void)wl_display_roundtrip(state->display);

        /* ── Compositor device realignment ───────────────────────────────────
         * feedback_main_device() now stores the compositor's dev_t. If the GBM
         * device we opened earlier (via sysfs) doesn't match (e.g. we got
         * renderD129/NVIDIA instead of renderD128/AMD), reinitialize on the
         * correct node to avoid cross-device EGL_BAD_ALLOC. */
        if (state->dmabuf_ctx.has_compositor_dev && state->dmabuf_ctx.drm_fd >= 0) {
            struct stat st;
            if (fstat(state->dmabuf_ctx.drm_fd, &st) == 0 &&
                st.st_rdev != state->dmabuf_ctx.compositor_dev) {
                WAYWAL_LOG_INFO(
                    "GBM device mismatch (have %u:%u, compositor wants %u:%u) — reinitializing",
                    (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev),
                    (unsigned)major(state->dmabuf_ctx.compositor_dev),
                    (unsigned)minor(state->dmabuf_ctx.compositor_dev));

                /* Preserve protocol objects across GBM tear-down */
                struct zwp_linux_dmabuf_v1 *saved_proto = state->dmabuf_ctx.dmabuf_proto;
                struct zwp_linux_dmabuf_feedback_v1 *saved_fb = state->dmabuf_ctx.default_feedback;
                uint64_t saved_mod = state->dmabuf_ctx.preferred_modifier;
                uint32_t saved_fmt = state->dmabuf_ctx.preferred_format;
                dev_t saved_dev = state->dmabuf_ctx.compositor_dev;
                bool saved_hdev = state->dmabuf_ctx.has_compositor_dev;
                bool saved_10 = state->dmabuf_ctx.has_10bit;
                uint32_t saved_f10 = state->dmabuf_ctx.format_10bit;
                uint64_t saved_m10 = state->dmabuf_ctx.modifier_10bit;
                bool saved_nv12 = state->dmabuf_ctx.has_nv12;
                bool saved_p010 = state->dmabuf_ctx.has_p010;

                /* Destroy old GBM only (not protocol objects) */
                if (state->dmabuf_ctx.gbm)
                    gbm_device_destroy(state->dmabuf_ctx.gbm);
                if (state->dmabuf_ctx.drm_fd >= 0)
                    close(state->dmabuf_ctx.drm_fd);
                state->dmabuf_ctx.gbm = NULL;
                state->dmabuf_ctx.drm_fd = -1;

                /* Open compositor's actual render node */
                int new_fd = open_drm_node_by_devt(saved_dev);
                if (new_fd >= 0) {
                    struct gbm_device *new_gbm = gbm_create_device(new_fd);
                    if (new_gbm) {
                        state->dmabuf_ctx.drm_fd = new_fd;
                        state->dmabuf_ctx.gbm = new_gbm;
                        WAYWAL_LOG_INFO("GBM reinitialized on compositor device %u:%u",
                                        (unsigned)major(saved_dev), (unsigned)minor(saved_dev));
                    } else {
                        WAYWAL_LOG_WARN(
                            "gbm_create_device failed on compositor node, staying on fallback");
                        close(new_fd);
                    }
                } else {
                    WAYWAL_LOG_WARN("Could not open compositor DRM node for GBM reinit");
                }

                /* Restore protocol/modifier state */
                state->dmabuf_ctx.dmabuf_proto = saved_proto;
                state->dmabuf_ctx.default_feedback = saved_fb;
                state->dmabuf_ctx.preferred_modifier = saved_mod;
                state->dmabuf_ctx.preferred_format = saved_fmt;
                state->dmabuf_ctx.compositor_dev = saved_dev;
                state->dmabuf_ctx.has_compositor_dev = saved_hdev;
                state->dmabuf_ctx.has_10bit = saved_10;
                state->dmabuf_ctx.format_10bit = saved_f10;
                state->dmabuf_ctx.modifier_10bit = saved_m10;
                state->dmabuf_ctx.has_nv12 = saved_nv12;
                state->dmabuf_ctx.has_p010 = saved_p010;
                state->dmabuf_ctx.available = (state->dmabuf_ctx.gbm != NULL);
                state->dmabuf_ctx.map_supported = dmabuf_probe_map_support(&state->dmabuf_ctx);
            }
        }
    }

    /* Initialize GPU Compute & SIMD render engine */
    render_engine_init(&state->render_engine, &state->dmabuf_ctx);

    if (state->dmabuf_ctx.gbm && !state->dmabuf_ctx.map_supported &&
        !state->render_engine.gpu_compute_active) {
        WAYWAL_LOG_INFO("GBM bo_map and GPU compute unsupported; releasing unused GBM device "
                        "(keeping drm_fd=%d for VA-API)",
                        state->dmabuf_ctx.drm_fd);
        gbm_device_destroy(state->dmabuf_ctx.gbm);
        state->dmabuf_ctx.gbm = NULL;
        state->dmabuf_ctx.available = false;
    }

    /* Create layer surfaces for all discovered outputs */
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        output_node_create_surface(state, out);
    }

    /* Roundtrip to receive initial configure events from compositor */
    if (wl_display_roundtrip(state->display) < 0) {
        WAYWAL_LOG_ERR("wl_display_roundtrip failed during initial surface configuration");
        return false;
    }

    WAYWAL_LOG_INFO(
        "Wayland connection established successfully with %zu output(s) (Direct Scanout: %s)",
        state->num_outputs,
        (state->dmabuf_ctx.available && state->dmabuf_ctx.dmabuf_proto) ? "ENABLED" : "FALLBACK");
    return true;
}

void daemon_wayland_destroy(daemon_state_t *state)
{
    if (!state)
        return;

    if (state->cursor_ipc_fd >= 0) {
        close(state->cursor_ipc_fd);
        state->cursor_ipc_fd = -1;
    }

    while (state->wlr_toplevels) {
        wlr_toplevel_entry_t *e = (wlr_toplevel_entry_t *)state->wlr_toplevels;
        state->wlr_toplevels = e->next;
        if (e->handle) {
            zwlr_foreign_toplevel_handle_v1_destroy(e->handle);
        }
        free(e);
    }

    output_node_t *out = state->outputs;
    while (out) {
        output_node_destroy_surface(out);
        out = out->next;
    }
    output_manager_destroy(&state->output_mgr);
    state->outputs = NULL;
    state->num_outputs = 0;

    /* Destroy hardware render engine */
    render_engine_destroy(&state->render_engine);

    /* Destroy hardware scanout context */
    dmabuf_context_destroy(&state->dmabuf_ctx);

    if (state->wlr_foreign_toplevel_mgr) {
        zwlr_foreign_toplevel_manager_v1_destroy(state->wlr_foreign_toplevel_mgr);
        state->wlr_foreign_toplevel_mgr = NULL;
    }
    if (state->cursor_shape_dev) {
        wp_cursor_shape_device_v1_destroy(state->cursor_shape_dev);
        state->cursor_shape_dev = NULL;
    }
    if (state->cursor_shape_mgr) {
        wp_cursor_shape_manager_v1_destroy(state->cursor_shape_mgr);
        state->cursor_shape_mgr = NULL;
    }
    if (state->xdg_output_mgr) {
        zxdg_output_manager_v1_destroy(state->xdg_output_mgr);
        state->xdg_output_mgr = NULL;
    }
    if (state->idle_notification) {
        ext_idle_notification_v1_destroy(state->idle_notification);
        state->idle_notification = NULL;
    }
    if (state->idle_notifier) {
        ext_idle_notifier_v1_destroy(state->idle_notifier);
        state->idle_notifier = NULL;
    }
    if (state->foreign_toplevel_list) {
        ext_foreign_toplevel_list_v1_destroy(state->foreign_toplevel_list);
        state->foreign_toplevel_list = NULL;
    }
    if (state->single_pixel_mgr) {
        wp_single_pixel_buffer_manager_v1_destroy(state->single_pixel_mgr);
        state->single_pixel_mgr = NULL;
    }
    if (state->syncobj_mgr) {
        wp_linux_drm_syncobj_manager_v1_destroy(state->syncobj_mgr);
        state->syncobj_mgr = NULL;
    }
    if (state->pointer) {
        wl_pointer_destroy(state->pointer);
        state->pointer = NULL;
    }
    if (state->seat) {
        wl_seat_destroy(state->seat);
        state->seat = NULL;
    }
    if (state->viewporter) {
        wp_viewporter_destroy(state->viewporter);
        state->viewporter = NULL;
    }
    if (state->presentation) {
        wp_presentation_destroy(state->presentation);
        state->presentation = NULL;
    }
    if (state->fract_manager) {
        wp_fractional_scale_manager_v1_destroy(state->fract_manager);
        state->fract_manager = NULL;
    }
    if (state->layer_shell) {
        zwlr_layer_shell_v1_destroy(state->layer_shell);
        state->layer_shell = NULL;
    }
    if (state->shm) {
        wl_shm_destroy(state->shm);
        state->shm = NULL;
    }
    if (state->compositor) {
        wl_compositor_destroy(state->compositor);
        state->compositor = NULL;
    }
    if (state->registry) {
        wl_registry_destroy(state->registry);
        state->registry = NULL;
    }
    if (state->display) {
        wl_display_disconnect(state->display);
        state->display = NULL;
    }
}

static bool query_hyprland_cursor_global(daemon_state_t *state, double *gx, double *gy)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Throttle IPC query to 60Hz (~16.6ms) */
    if (state->has_cached_ipc_cursor && state->last_cursor_ipc_ns > 0 &&
        (now_ns - state->last_cursor_ipc_ns) < 16666666ULL) {
        *gx = state->cached_ipc_cursor_x;
        *gy = state->cached_ipc_cursor_y;
        return true;
    }

    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *xrd = getenv("XDG_RUNTIME_DIR");
    if (!his || !his[0] || !xrd || !xrd[0])
        return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (state->cursor_ipc_fd < 0) {
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return false;
            struct sockaddr_un addr = {0};
            addr.sun_family = AF_UNIX;
            int path_len =
                snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/hypr/%s/.socket.sock", xrd, his);
            if (path_len < 0 || (size_t)path_len >= sizeof(addr.sun_path)) {
                close(fd);
                return false;
            }
            struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                close(fd);
                return false;
            }
            state->cursor_ipc_fd = fd;
        }

        ssize_t w = send(state->cursor_ipc_fd, "cursorpos", 9, MSG_NOSIGNAL);
        if (w == 9) {
            char buf[128] = {0};
            ssize_t n = recv(state->cursor_ipc_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                int x = 0, y = 0;
                if (sscanf(buf, "%d, %d", &x, &y) == 2) {
                    close(state->cursor_ipc_fd);
                    state->cursor_ipc_fd = -1;
                    state->cached_ipc_cursor_x = (double)x;
                    state->cached_ipc_cursor_y = (double)y;
                    state->has_cached_ipc_cursor = true;
                    state->last_cursor_ipc_ns = now_ns;
                    *gx = (double)x;
                    *gy = (double)y;
                    return true;
                }
            }
        }
        close(state->cursor_ipc_fd);
        state->cursor_ipc_fd = -1;
    }

    return false;
}

bool daemon_query_cursor_pos(daemon_state_t *state, output_node_t *out, double *out_x,
                             double *out_y)
{
    if (!state || !out_x || !out_y)
        return false;

    /* Tier 1: Direct Wayland wl_pointer focus on wallpaper layer surface */
    if (out && out->has_cursor) {
        *out_x = out->cursor_x;
        *out_y = out->cursor_y;
        return true;
    }

    double ref_w =
        out ? (out->logical_width > 0 ? (double)out->logical_width : (double)out->width) : 1920.0;
    double ref_h =
        out ? (out->logical_height > 0 ? (double)out->logical_height : (double)out->height)
            : 1080.0;

    /* Tier 2: Compositor IPC Fallback when occluded, throttled to 60Hz */
    double gx = 0.0, gy = 0.0;
    if (query_hyprland_cursor_global(state, &gx, &gy)) {
        if (out) {
            double lx = gx - (double)out->global_x;
            double ly = gy - (double)out->global_y;
            if (ref_w > 0.0 && ref_h > 0.0) {
                if (lx < 0.0)
                    lx = 0.0;
                else if (lx > ref_w)
                    lx = ref_w;
                if (ly < 0.0)
                    ly = 0.0;
                else if (ly > ref_h)
                    ly = ref_h;
            }
            out->cursor_x = lx;
            out->cursor_y = ly;
            *out_x = lx;
            *out_y = ly;
        } else {
            *out_x = gx;
            *out_y = gy;
        }
        return true;
    }

    /* Tier 3: Last-known cached wl_pointer coordinates */
    if (out && (out->cursor_x > 0.0 || out->cursor_y > 0.0)) {
        *out_x = out->cursor_x;
        *out_y = out->cursor_y;
        return true;
    }

    /* Tier 4: Screen center fallback */
    if (ref_w > 0.0 && ref_h > 0.0) {
        *out_x = ref_w * 0.5;
        *out_y = ref_h * 0.5;
        return true;
    }

    return false;
}
