#include "wayland_core.h"
#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    daemon_state_t *state = (daemon_state_t *)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t ver = version >= 4 ? 4 : version;
        state->compositor = (struct wl_compositor *)wl_registry_bind(
            registry, name, &wl_compositor_interface, ver);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = (struct wl_shm *)wl_registry_bind(
            registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t ver = version >= 4 ? 4 : version;
        state->layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, ver);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->viewporter = (struct wp_viewporter *)wl_registry_bind(
            registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        state->fract_manager = (struct wp_fractional_scale_manager_v1 *)wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
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
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        output_node_t *node = output_manager_add(&state->output_mgr, registry, name, version);
        if (node) {
            node->state = state;
            state->outputs = state->output_mgr.head;
            state->num_outputs = state->output_mgr.count;
            WAYWAL_LOG_INFO("Discovered Wayland output id %u", name);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    daemon_state_t *state = (daemon_state_t *)data;

    output_node_t *node = output_manager_find_by_id(&state->output_mgr, name);
    if (node) {
        WAYWAL_LOG_INFO("Removing Wayland output id %u (%s)", name, node->name);
        output_node_destroy_surface(node);
        output_manager_remove(&state->output_mgr, name);
        state->outputs = state->output_mgr.head;
        state->num_outputs = state->output_mgr.count;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

bool daemon_wayland_init(daemon_state_t *state) {
    if (!state) return false;

    state->display = wl_display_connect(NULL);
    if (!state->display) {
        WAYWAL_LOG_ERR("Could not connect to Wayland display (check $WAYLAND_DISPLAY / $XDG_RUNTIME_DIR)");
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
        /* Roundtrip to receive format modifier tranches */
        (void)wl_display_roundtrip(state->display);
    }

    /* Initialize GPU Compute & SIMD render engine */
    render_engine_init(&state->render_engine, &state->dmabuf_ctx);

    /* Create layer surfaces for all discovered outputs */
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        output_node_create_surface(state, out);
    }

    /* Roundtrip to receive initial configure events from compositor */
    if (wl_display_roundtrip(state->display) < 0) {
        WAYWAL_LOG_ERR("wl_display_roundtrip failed during initial surface configuration");
        return false;
    }

    WAYWAL_LOG_INFO("Wayland connection established successfully with %zu output(s) (Direct Scanout: %s)",
                  state->num_outputs,
                  (state->dmabuf_ctx.available && state->dmabuf_ctx.dmabuf_proto) ? "ENABLED" : "FALLBACK");
    return true;
}

void daemon_wayland_destroy(daemon_state_t *state) {
    if (!state) return;

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
