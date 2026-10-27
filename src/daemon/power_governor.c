// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/power_governor.h"

#include "waywal/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef enum {
    IPC_BACKEND_NONE = 0,
    IPC_BACKEND_HYPRLAND = 1,
    IPC_BACKEND_SWAY = 2,
    IPC_BACKEND_NIRI = 3,
} ipc_backend_t;

typedef struct {
    char name[64];
    uint32_t mask;
    bool in_use;
} governed_output_t;

#define WAYWAL_SYSFS_BAT_COUNT 2
#define WAYWAL_SYSFS_AC_COUNT  3

struct waywal_power_governor {
    waywal_battery_policy_t battery_policy;
    bool on_battery;
    uint32_t global_mask;
    bool auto_pause_fullscreen;
    bool auto_pause_maximized;
    bool pause_on_occluded_workspace;

    governed_output_t outputs[WAYWAL_MAX_GOVERNED_OUTPUTS];
    size_t output_count;
    char focused_output[64];
    char active_workspace[64];
    bool workspace_changed;

    ipc_backend_t ipc_backend;
    int ipc_fd;
    bool ipc_reconnected;
    char ipc_buf[4096];
    size_t ipc_buf_len;

    int inotify_fd;
    int bat_fds[WAYWAL_SYSFS_BAT_COUNT];
    int ac_fds[WAYWAL_SYSFS_AC_COUNT];
    int profile_fd;
    bool battery_polled_once;
};

static uint32_t policy_allowed_mask(const waywal_power_governor_t *gov)
{
    if (!gov)
        return 0xFFFFFFFFu;
    uint32_t mask = 0xFFFFFFFFu;
    if (!gov->auto_pause_fullscreen) {
        mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
    }
    if (!gov->auto_pause_maximized) {
        mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED;
    }
    if (!gov->pause_on_occluded_workspace) {
        mask &= ~(uint32_t)WAYWAL_PAUSE_WORKSPACE_OCCLUDED;
    }
    return mask;
}

static governed_output_t *find_or_create_output(waywal_power_governor_t *gov, const char *name,
                                                bool create)
{
    if (!gov || !name || name[0] == '\0') {
        return NULL;
    }
    governed_output_t *free_slot = NULL;
    for (size_t i = 0; i < gov->output_count; ++i) {
        if (gov->outputs[i].in_use) {
            if (strcmp(gov->outputs[i].name, name) == 0) {
                return &gov->outputs[i];
            }
        } else if (!free_slot) {
            free_slot = &gov->outputs[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (!free_slot) {
        if (gov->output_count >= WAYWAL_MAX_GOVERNED_OUTPUTS) {
            return NULL;
        }
        free_slot = &gov->outputs[gov->output_count++];
    }
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", name);
    free_slot->in_use = true;
    return free_slot;
}

static const governed_output_t *find_output_const(const waywal_power_governor_t *gov,
                                                  const char *name)
{
    if (!gov || !name || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < gov->output_count; ++i) {
        if (gov->outputs[i].in_use && strcmp(gov->outputs[i].name, name) == 0) {
            return &gov->outputs[i];
        }
    }
    return NULL;
}

static void sync_battery_mask(waywal_power_governor_t *gov)
{
    if (!gov) {
        return;
    }
    if (gov->on_battery && gov->battery_policy == WAYWAL_BATTERY_PAUSE_VIDEO) {
        gov->global_mask |= WAYWAL_PAUSE_BATTERY_SAVER;
    } else {
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_BATTERY_SAVER;
    }
}

static int connect_unix_nonblock(const char *sock_path)
{
    if (!sock_path || sock_path[0] == '\0') {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static void init_compositor_ipc(waywal_power_governor_t *gov)
{
    gov->ipc_fd = -1;
    gov->ipc_backend = IPC_BACKEND_NONE;

    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his && his[0] != '\0') {
        char path[512];
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && xdg[0] != '\0') {
            snprintf(path, sizeof(path), "%s/hypr/%s/.socket2.sock", xdg, his);
            gov->ipc_fd = connect_unix_nonblock(path);
        }
        if (gov->ipc_fd < 0) {
            snprintf(path, sizeof(path), "/tmp/hypr/%s/.socket2.sock", his);
            gov->ipc_fd = connect_unix_nonblock(path);
        }
        if (gov->ipc_fd >= 0) {
            gov->ipc_backend = IPC_BACKEND_HYPRLAND;
            WAYWAL_LOG_INFO("Power governor attached to Hyprland IPC event socket");
            return;
        }
    }

    const char *swaysock = getenv("SWAYSOCK");
    if (!swaysock || swaysock[0] == '\0') {
        swaysock = getenv("I3SOCK");
    }
    if (swaysock && swaysock[0] != '\0') {
        gov->ipc_fd = connect_unix_nonblock(swaysock);
        if (gov->ipc_fd >= 0) {
            static const char sub_payload[] = "[\"window\",\"workspace\"]";
            uint8_t pkt[14 + sizeof(sub_payload) - 1];
            memcpy(pkt, "i3-ipc", 6);
            uint32_t len = (uint32_t)(sizeof(sub_payload) - 1);
            uint32_t type = 2u; /* IPC_SUBSCRIBE */
            memcpy(pkt + 6, &len, 4);
            memcpy(pkt + 10, &type, 4);
            memcpy(pkt + 14, sub_payload, len);
            ssize_t wr = send(gov->ipc_fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
            (void)wr;
            gov->ipc_backend = IPC_BACKEND_SWAY;
            WAYWAL_LOG_INFO("Power governor attached to Sway/i3 IPC socket");
            return;
        }
    }

    const char *nirisock = getenv("NIRI_SOCKET");
    if (nirisock && nirisock[0] != '\0') {
        gov->ipc_fd = connect_unix_nonblock(nirisock);
        if (gov->ipc_fd >= 0) {
            static const char niri_sub[] = "\"EventStream\"\n";
            ssize_t wr = send(gov->ipc_fd, niri_sub, sizeof(niri_sub) - 1, MSG_NOSIGNAL);
            (void)wr;
            gov->ipc_backend = IPC_BACKEND_NIRI;
            WAYWAL_LOG_INFO("Power governor attached to Niri IPC event socket");
            return;
        }
    }
}

static bool pread_first_line(int fd, char *out, size_t out_sz)
{
    if (fd < 0 || !out || out_sz == 0) {
        return false;
    }
    ssize_t n = pread(fd, out, out_sz - 1, 0);
    if (n <= 0) {
        return false;
    }
    out[n] = '\0';
    for (ssize_t i = 0; i < n; ++i) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = '\0';
            break;
        }
    }
    return true;
}

static void init_battery_inotify(waywal_power_governor_t *gov)
{
    for (size_t i = 0; i < WAYWAL_SYSFS_BAT_COUNT; ++i) {
        gov->bat_fds[i] = -1;
    }
    for (size_t i = 0; i < WAYWAL_SYSFS_AC_COUNT; ++i) {
        gov->ac_fds[i] = -1;
    }
    gov->profile_fd = -1;

    static const char *const bat_paths[WAYWAL_SYSFS_BAT_COUNT] = {
        "/sys/class/power_supply/BAT0/status",
        "/sys/class/power_supply/BAT1/status",
    };
    static const char *const ac_paths[WAYWAL_SYSFS_AC_COUNT] = {
        "/sys/class/power_supply/AC/online",
        "/sys/class/power_supply/ACAD/online",
        "/sys/class/power_supply/ADP1/online",
    };
    static const char *const profile_path = "/sys/firmware/acpi/platform_profile";

    gov->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    for (size_t i = 0; i < WAYWAL_SYSFS_BAT_COUNT; ++i) {
        gov->bat_fds[i] = open(bat_paths[i], O_RDONLY | O_CLOEXEC);
        if (gov->bat_fds[i] >= 0 && gov->inotify_fd >= 0) {
            inotify_add_watch(gov->inotify_fd, bat_paths[i], IN_MODIFY | IN_CLOSE_WRITE);
        }
    }
    for (size_t i = 0; i < WAYWAL_SYSFS_AC_COUNT; ++i) {
        gov->ac_fds[i] = open(ac_paths[i], O_RDONLY | O_CLOEXEC);
        if (gov->ac_fds[i] >= 0 && gov->inotify_fd >= 0) {
            inotify_add_watch(gov->inotify_fd, ac_paths[i], IN_MODIFY | IN_CLOSE_WRITE);
        }
    }
    gov->profile_fd = open(profile_path, O_RDONLY | O_CLOEXEC);
    if (gov->profile_fd >= 0 && gov->inotify_fd >= 0) {
        inotify_add_watch(gov->inotify_fd, profile_path, IN_MODIFY | IN_CLOSE_WRITE);
    }
}

waywal_power_governor_t *waywal_power_governor_create(waywal_battery_policy_t policy)
{
    waywal_power_governor_t *gov =
        (waywal_power_governor_t *)calloc(1, sizeof(waywal_power_governor_t));
    if (!gov) {
        return NULL;
    }
    gov->battery_policy = policy;
    gov->auto_pause_fullscreen = true;
    gov->auto_pause_maximized = true;
    gov->pause_on_occluded_workspace = true;
    gov->ipc_fd = -1;
    gov->inotify_fd = -1;

    init_compositor_ipc(gov);
    init_battery_inotify(gov);
    waywal_power_governor_poll_battery(gov);
    return gov;
}

void waywal_power_governor_destroy(waywal_power_governor_t *gov)
{
    if (!gov) {
        return;
    }
    if (gov->ipc_fd >= 0) {
        close(gov->ipc_fd);
    }
    if (gov->inotify_fd >= 0) {
        close(gov->inotify_fd);
    }
    for (size_t i = 0; i < WAYWAL_SYSFS_BAT_COUNT; ++i) {
        if (gov->bat_fds[i] >= 0) {
            close(gov->bat_fds[i]);
        }
    }
    for (size_t i = 0; i < WAYWAL_SYSFS_AC_COUNT; ++i) {
        if (gov->ac_fds[i] >= 0) {
            close(gov->ac_fds[i]);
        }
    }
    if (gov->profile_fd >= 0) {
        close(gov->profile_fd);
    }
    free(gov);
}

int waywal_power_governor_get_ipc_fd(const waywal_power_governor_t *gov)
{
    return gov ? gov->ipc_fd : -1;
}

int waywal_power_governor_get_inotify_fd(const waywal_power_governor_t *gov)
{
    return gov ? gov->inotify_fd : -1;
}

void waywal_power_governor_feed_hyprland_event(waywal_power_governor_t *gov, const char *line)
{
    if (!gov || !line) {
        return;
    }

    const char *sep = strstr(line, ">>");
    if (!sep) {
        return;
    }
    size_t ev_len = (size_t)(sep - line);
    const char *arg = sep + 2;

    if ((ev_len == 10 && memcmp(line, "focusedmon", 10) == 0) ||
        (ev_len == 12 && memcmp(line, "focusedmonv2", 12) == 0)) {
        const char *comma = strchr(arg, ',');
        size_t mon_len = comma ? (size_t)(comma - arg) : strlen(arg);
        if (mon_len > 0 && mon_len < sizeof(gov->focused_output)) {
            memcpy(gov->focused_output, arg, mon_len);
            gov->focused_output[mon_len] = '\0';
            governed_output_t *out = find_or_create_output(gov, gov->focused_output, true);
            if (out && (gov->global_mask &
                        (WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN | WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED))) {
                /* Migrate any early global fullscreen/maximized bit onto the newly known output */
                out->mask |= gov->global_mask &
                             (WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN | WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED);
                gov->global_mask &=
                    ~(uint32_t)(WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN | WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED);
            }
        }
        if (comma && comma[1] != '\0') {
            snprintf(gov->active_workspace, sizeof(gov->active_workspace), "%s", comma + 1);
            gov->workspace_changed = true;
        }
    } else if (ev_len == 10 && memcmp(line, "fullscreen", 10) == 0) {
        bool is_fs = (arg[0] == '1');
        if (!is_fs) {
            gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
        }
        if (gov->focused_output[0] != '\0') {
            waywal_power_governor_set_output_reason(gov, gov->focused_output,
                                                    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, is_fs);
        } else {
            waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, is_fs);
        }
    } else if (ev_len == 9 && memcmp(line, "workspace", 9) == 0) {
        if (arg[0] != '\0') {
            snprintf(gov->active_workspace, sizeof(gov->active_workspace), "%s", arg);
            gov->workspace_changed = true;
        }
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
        if (gov->focused_output[0] != '\0') {
            waywal_power_governor_set_output_reason(gov, gov->focused_output,
                                                    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
        }
    } else if (ev_len == 11 && memcmp(line, "workspacev2", 11) == 0) {
        const char *comma = strchr(arg, ',');
        const char *ws_name = (comma && comma[1] != '\0') ? (comma + 1) : arg;
        if (ws_name[0] != '\0') {
            snprintf(gov->active_workspace, sizeof(gov->active_workspace), "%s", ws_name);
            gov->workspace_changed = true;
        }
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
        if (gov->focused_output[0] != '\0') {
            waywal_power_governor_set_output_reason(gov, gov->focused_output,
                                                    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
        }
    }
}

void waywal_power_governor_feed_sway_payload(waywal_power_governor_t *gov, const char *json)
{
    if (!gov || !json) {
        return;
    }

    char target_output[64];
    target_output[0] = '\0';

    const char *cur_obj = strstr(json, "\"current\"");
    const char *search_base = cur_obj ? cur_obj : json;

    const char *out_key = strstr(search_base, "\"output\"");
    if (!out_key && cur_obj) {
        out_key = strstr(json, "\"output\"");
    }
    if (out_key) {
        const char *colon = strchr(out_key + 8, ':');
        if (colon) {
            const char *q1 = strchr(colon + 1, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && (size_t)(q2 - (q1 + 1)) < sizeof(target_output)) {
                    size_t len = (size_t)(q2 - (q1 + 1));
                    memcpy(target_output, q1 + 1, len);
                    target_output[len] = '\0';
                }
            }
        }
    }

    if (target_output[0] != '\0') {
        snprintf(gov->focused_output, sizeof(gov->focused_output), "%s", target_output);
        find_or_create_output(gov, gov->focused_output, true);
    }

    if (cur_obj || strstr(json, "\"change\":\"focus\"") || strstr(json, "\"change\": \"focus\"")) {
        const char *name_key = strstr(search_base, "\"name\"");
        if (name_key) {
            const char *colon = strchr(name_key + 6, ':');
            if (colon) {
                const char *q1 = strchr(colon + 1, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && q2 > q1 + 1 &&
                        (size_t)(q2 - (q1 + 1)) < sizeof(gov->active_workspace)) {
                        size_t len = (size_t)(q2 - (q1 + 1));
                        memcpy(gov->active_workspace, q1 + 1, len);
                        gov->active_workspace[len] = '\0';
                        gov->workspace_changed = true;
                    }
                }
            }
        }
    }

    const char *fs_key = strstr(json, "\"fullscreen_mode\"");
    if (!fs_key) {
        return;
    }
    const char *colon = strchr(fs_key + 17, ':');
    if (!colon) {
        return;
    }
    const char *p = colon + 1;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    bool is_fs = (*p == '1' || *p == '2');
    if (!is_fs) {
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
    }

    const char *out_name = target_output[0] != '\0' ? target_output : gov->focused_output;
    if (out_name[0] != '\0') {
        waywal_power_governor_set_output_reason(gov, out_name, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN,
                                                is_fs);
    } else {
        waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, is_fs);
    }
}

void waywal_power_governor_feed_niri_event(waywal_power_governor_t *gov, const char *json)
{
    if (!gov || !json) {
        return;
    }

    if (strstr(json, "\"WorkspaceActivated\"") || strstr(json, "\"WorkspaceFocused\"")) {
        const char *id_key = strstr(json, "\"id\"");
        if (id_key) {
            const char *colon = strchr(id_key + 4, ':');
            if (colon) {
                const char *p = colon + 1;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                size_t len = 0;
                while (p[len] && isdigit((unsigned char)p[len]) &&
                       len < sizeof(gov->active_workspace) - 1) {
                    gov->active_workspace[len] = p[len];
                    len++;
                }
                if (len > 0) {
                    gov->active_workspace[len] = '\0';
                    gov->workspace_changed = true;
                }
            }
        }
    }
}

bool waywal_power_governor_pop_workspace_change(waywal_power_governor_t *gov, char *out_ws,
                                                size_t ws_cap, char *out_mon, size_t mon_cap)
{
    if (!gov || !gov->workspace_changed) {
        return false;
    }
    gov->workspace_changed = false;
    if (out_ws && ws_cap > 0) {
        snprintf(out_ws, ws_cap, "%s", gov->active_workspace);
    }
    if (out_mon && mon_cap > 0) {
        snprintf(out_mon, mon_cap, "%s", gov->focused_output);
    }
    return gov->active_workspace[0] != '\0';
}

bool waywal_power_governor_consume_ipc_reconnected(waywal_power_governor_t *gov)
{
    if (!gov) {
        return false;
    }
    bool reconnected = gov->ipc_reconnected;
    gov->ipc_reconnected = false;
    return reconnected;
}

void waywal_power_governor_dispatch_ipc(waywal_power_governor_t *gov)
{
    if (!gov) {
        return;
    }
    if (gov->ipc_fd < 0) {
        init_compositor_ipc(gov);
        if (gov->ipc_fd < 0) {
            return;
        }
        gov->ipc_reconnected = true;
    }

    for (;;) {
        if (gov->ipc_buf_len >= sizeof(gov->ipc_buf) - 1) {
            gov->ipc_buf_len = 0;
        }
        ssize_t n = recv(gov->ipc_fd, gov->ipc_buf + gov->ipc_buf_len,
                         sizeof(gov->ipc_buf) - 1 - gov->ipc_buf_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(gov->ipc_fd);
            gov->ipc_fd = -1;
            gov->ipc_buf_len = 0;
            init_compositor_ipc(gov);
            gov->ipc_reconnected = (gov->ipc_fd >= 0);
            break;
        }
        if (n < 0) {
            break;
        }
        gov->ipc_buf_len += (size_t)n;
        gov->ipc_buf[gov->ipc_buf_len] = '\0';

        if (gov->ipc_backend == IPC_BACKEND_HYPRLAND || gov->ipc_backend == IPC_BACKEND_NIRI) {
            char *start = gov->ipc_buf;
            char *nl = NULL;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                if (gov->ipc_backend == IPC_BACKEND_HYPRLAND) {
                    waywal_power_governor_feed_hyprland_event(gov, start);
                } else {
                    waywal_power_governor_feed_niri_event(gov, start);
                }
                start = nl + 1;
            }
            size_t rem = (size_t)(gov->ipc_buf + gov->ipc_buf_len - start);
            if (rem > 0 && start != gov->ipc_buf) {
                memmove(gov->ipc_buf, start, rem);
            }
            gov->ipc_buf_len = rem;
        } else if (gov->ipc_backend == IPC_BACKEND_SWAY) {
            size_t offset = 0;
            while (gov->ipc_buf_len - offset >= 14) {
                if (memcmp(gov->ipc_buf + offset, "i3-ipc", 6) != 0) {
                    offset++;
                    continue;
                }
                uint32_t msg_len = 0;
                memcpy(&msg_len, gov->ipc_buf + offset + 6, 4);
                if (msg_len > sizeof(gov->ipc_buf) - 16) {
                    gov->ipc_buf_len = 0;
                    return;
                }
                if (gov->ipc_buf_len - offset < 14u + msg_len) {
                    break;
                }
                char saved = gov->ipc_buf[offset + 14u + msg_len];
                gov->ipc_buf[offset + 14u + msg_len] = '\0';
                waywal_power_governor_feed_sway_payload(gov, gov->ipc_buf + offset + 14u);
                gov->ipc_buf[offset + 14u + msg_len] = saved;
                offset += 14u + msg_len;
            }
            if (offset > 0) {
                size_t rem = gov->ipc_buf_len - offset;
                if (rem > 0) {
                    memmove(gov->ipc_buf, gov->ipc_buf + offset, rem);
                }
                gov->ipc_buf_len = rem;
            }
        }
    }
}

bool waywal_power_governor_poll_battery(waywal_power_governor_t *gov)
{
    if (!gov) {
        return false;
    }

    if (gov->inotify_fd >= 0) {
        uint8_t ev_buf[256];
        bool changed = false;
        while (read(gov->inotify_fd, ev_buf, sizeof(ev_buf)) > 0) {
            changed = true;
        }
        if (gov->battery_polled_once && !changed) {
            return gov->on_battery;
        }
    }

    bool discharging = false;
    char buf[64];

    for (size_t i = 0; i < WAYWAL_SYSFS_BAT_COUNT; ++i) {
        if (pread_first_line(gov->bat_fds[i], buf, sizeof(buf))) {
            if (strcasecmp(buf, "Discharging") == 0) {
                discharging = true;
                break;
            }
        }
    }

    if (!discharging) {
        for (size_t i = 0; i < WAYWAL_SYSFS_AC_COUNT; ++i) {
            if (pread_first_line(gov->ac_fds[i], buf, sizeof(buf))) {
                if (buf[0] == '0') {
                    discharging = true;
                }
                break;
            }
        }
    }

    if (!discharging) {
        if (pread_first_line(gov->profile_fd, buf, sizeof(buf))) {
            if (strcasecmp(buf, "low-power") == 0) {
                discharging = true;
            }
        }
    }

    gov->battery_polled_once = true;
    gov->on_battery = discharging;
    sync_battery_mask(gov);
    return gov->on_battery;
}

void waywal_power_governor_set_on_battery(waywal_power_governor_t *gov, bool on_battery)
{
    if (!gov) {
        return;
    }
    gov->on_battery = on_battery;
    sync_battery_mask(gov);
}

bool waywal_power_governor_is_on_battery(const waywal_power_governor_t *gov)
{
    return gov ? gov->on_battery : false;
}

void waywal_power_governor_set_battery_policy(waywal_power_governor_t *gov,
                                              waywal_battery_policy_t policy)
{
    if (!gov) {
        return;
    }
    gov->battery_policy = policy;
    sync_battery_mask(gov);
}

void waywal_power_governor_set_pause_policies(waywal_power_governor_t *gov,
                                              bool auto_pause_fullscreen, bool auto_pause_maximized,
                                              bool pause_on_occluded_workspace)
{
    if (!gov) {
        return;
    }
    gov->auto_pause_fullscreen = auto_pause_fullscreen;
    gov->auto_pause_maximized = auto_pause_maximized;
    gov->pause_on_occluded_workspace = pause_on_occluded_workspace;
}

void waywal_power_governor_set_output_reason(waywal_power_governor_t *gov, const char *output_name,
                                             waywal_pause_reason_t reason, bool active)
{
    if (!gov) {
        return;
    }
    if (!output_name || output_name[0] == '\0') {
        waywal_power_governor_set_global_reason(gov, reason, active);
        return;
    }
    governed_output_t *out = find_or_create_output(gov, output_name, true);
    if (!out) {
        return;
    }
    if (active) {
        out->mask |= (uint32_t)reason;
    } else {
        out->mask &= ~(uint32_t)reason;
        /* Also clear any stale early global reason bit for window state */
        if (reason == WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN ||
            reason == WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED) {
            gov->global_mask &= ~(uint32_t)reason;
        }
    }
}

void waywal_power_governor_remove_output(waywal_power_governor_t *gov, const char *output_name)
{
    if (!gov || !output_name || output_name[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < gov->output_count; ++i) {
        if (gov->outputs[i].in_use && strcmp(gov->outputs[i].name, output_name) == 0) {
            gov->outputs[i].in_use = false;
            gov->outputs[i].mask = 0u;
            gov->outputs[i].name[0] = '\0';
        }
    }
    while (gov->output_count > 0 && !gov->outputs[gov->output_count - 1].in_use) {
        gov->output_count--;
    }
}

void waywal_power_governor_set_global_reason(waywal_power_governor_t *gov,
                                             waywal_pause_reason_t reason, bool active)
{
    if (!gov) {
        return;
    }
    if (active) {
        gov->global_mask |= (uint32_t)reason;
    } else {
        gov->global_mask &= ~(uint32_t)reason;
    }
}

uint32_t waywal_power_governor_get_output_mask(const waywal_power_governor_t *gov,
                                               const char *output_name)
{
    if (!gov) {
        return 0;
    }
    const governed_output_t *out = find_output_const(gov, output_name);
    uint32_t out_mask = out ? out->mask : 0u;
    uint32_t allowed = policy_allowed_mask(gov);
    return (gov->global_mask | out_mask) & allowed;
}

bool waywal_power_governor_should_pause_output(const waywal_power_governor_t *gov,
                                               const char *output_name)
{
    return waywal_power_governor_get_output_mask(gov, output_name) != 0u;
}

bool waywal_power_governor_should_pause_all(const waywal_power_governor_t *gov)
{
    if (!gov) {
        return false;
    }
    uint32_t allowed = policy_allowed_mask(gov);
    if ((gov->global_mask & allowed) != 0u) {
        return true;
    }
    size_t active_outputs = 0;
    for (size_t i = 0; i < gov->output_count; ++i) {
        if (gov->outputs[i].in_use) {
            active_outputs++;
            if ((gov->outputs[i].mask & allowed) == 0u) {
                return false;
            }
        }
    }
    return active_outputs > 0;
}

uint32_t waywal_power_governor_effective_fps(const waywal_power_governor_t *gov,
                                             const char *output_name, uint32_t target_fps)
{
    uint32_t base_fps = (target_fps == 0) ? 60u : target_fps;
    if (!gov) {
        return base_fps;
    }
    if (waywal_power_governor_should_pause_output(gov, output_name)) {
        return 0u;
    }
    if (gov->on_battery && gov->battery_policy == WAYWAL_BATTERY_REDUCE_FPS && base_fps > 24u) {
        return 24u;
    }
    return base_fps;
}
