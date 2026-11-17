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
    uint64_t niri_fs_win_id;
    char niri_fs_output[64];

    ipc_backend_t ipc_backend;
    int ipc_fd;
    bool ipc_reconnected;
    bool ipc_connecting;
    uint8_t ipc_tx_buf[64];
    size_t ipc_tx_len;
    size_t ipc_tx_off;
    char ipc_buf[16384];
    size_t ipc_buf_len;
    size_t ipc_skip_bytes;
    bool ipc_drop_line;

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

static int connect_unix_nonblock(const char *sock_path, bool *out_in_progress)
{
    if (out_in_progress) {
        *out_in_progress = false;
    }
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
    int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (out_in_progress) {
        *out_in_progress = (rc < 0 && errno == EINPROGRESS);
    }
    return fd;
}

static bool flush_ipc_handshake(waywal_power_governor_t *gov)
{
    if (!gov || gov->ipc_fd < 0) {
        return false;
    }
    if (gov->ipc_connecting) {
        int so_err = 0;
        socklen_t slen = (socklen_t)sizeof(so_err);
        if (getsockopt(gov->ipc_fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0) {
            return false;
        }
        if (so_err == EINPROGRESS || so_err == EALREADY) {
            return true;
        }
        if (so_err != 0) {
            return false;
        }
        gov->ipc_connecting = false;
    }
    while (gov->ipc_tx_off < gov->ipc_tx_len) {
        ssize_t wr = send(gov->ipc_fd, gov->ipc_tx_buf + gov->ipc_tx_off,
                          gov->ipc_tx_len - gov->ipc_tx_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (wr > 0) {
            gov->ipc_tx_off += (size_t)wr;
            continue;
        }
        if (wr < 0 && errno == EINTR) {
            continue;
        }
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) {
            break;
        }
        return false;
    }
    return true;
}

static void close_ipc_socket(waywal_power_governor_t *gov)
{
    if (!gov) {
        return;
    }
    if (gov->ipc_fd >= 0) {
        close(gov->ipc_fd);
        gov->ipc_fd = -1;
    }
    gov->ipc_connecting = false;
    gov->ipc_tx_len = 0;
    gov->ipc_tx_off = 0;
    gov->ipc_buf_len = 0;
    gov->ipc_skip_bytes = 0;
    gov->ipc_drop_line = false;
}

static void init_compositor_ipc(waywal_power_governor_t *gov)
{
    gov->ipc_fd = -1;
    gov->ipc_backend = IPC_BACKEND_NONE;
    gov->ipc_connecting = false;
    gov->ipc_tx_len = 0;
    gov->ipc_tx_off = 0;

    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his && his[0] != '\0') {
        char path[512];
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && xdg[0] != '\0') {
            int n = snprintf(path, sizeof(path), "%s/hypr/%s/.socket2.sock", xdg, his);
            if (n > 0 && (size_t)n < sizeof(path)) {
                bool in_prog = false;
                gov->ipc_fd = connect_unix_nonblock(path, &in_prog);
                gov->ipc_connecting = in_prog;
            }
        }
        if (gov->ipc_fd < 0) {
            int n = snprintf(path, sizeof(path), "/tmp/hypr/%s/.socket2.sock", his);
            if (n > 0 && (size_t)n < sizeof(path)) {
                bool in_prog = false;
                gov->ipc_fd = connect_unix_nonblock(path, &in_prog);
                gov->ipc_connecting = in_prog;
            }
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
        bool in_prog = false;
        gov->ipc_fd = connect_unix_nonblock(swaysock, &in_prog);
        if (gov->ipc_fd >= 0) {
            static const char sub_payload[] = "[\"window\",\"workspace\"]";
            uint32_t len = (uint32_t)(sizeof(sub_payload) - 1);
            uint32_t type = 2u; /* IPC_SUBSCRIBE */
            size_t total = 14u + len;
            memcpy(gov->ipc_tx_buf, "i3-ipc", 6);
            memcpy(gov->ipc_tx_buf + 6, &len, 4);
            memcpy(gov->ipc_tx_buf + 10, &type, 4);
            memcpy(gov->ipc_tx_buf + 14, sub_payload, len);
            gov->ipc_tx_len = total;
            gov->ipc_tx_off = 0;
            gov->ipc_connecting = in_prog;
            if (!in_prog && !flush_ipc_handshake(gov)) {
                close_ipc_socket(gov);
                return;
            }
            gov->ipc_backend = IPC_BACKEND_SWAY;
            WAYWAL_LOG_INFO("Power governor attached to Sway/i3 IPC socket");
            return;
        }
    }

    const char *nirisock = getenv("NIRI_SOCKET");
    if (nirisock && nirisock[0] != '\0') {
        bool in_prog = false;
        gov->ipc_fd = connect_unix_nonblock(nirisock, &in_prog);
        if (gov->ipc_fd >= 0) {
            static const char niri_sub[] = "\"EventStream\"\n";
            size_t total = sizeof(niri_sub) - 1;
            memcpy(gov->ipc_tx_buf, niri_sub, total);
            gov->ipc_tx_len = total;
            gov->ipc_tx_off = 0;
            gov->ipc_connecting = in_prog;
            if (!in_prog && !flush_ipc_handshake(gov)) {
                close_ipc_socket(gov);
                return;
            }
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

#define IPC_JSON_MAX_DEPTH 32

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static bool json_parse_string(const char **pp, char *dst, size_t cap, bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    const char *p = json_skip_ws(*pp);
    if (*p != '"') {
        return false;
    }
    p++;
    size_t out = 0;
    while (*p != '\0' && *p != '"') {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20u) {
            return false;
        }
        if (ch == '\\') {
            p++;
            if (*p == '\0') {
                return false;
            }
            char esc = *p++;
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                ch = (unsigned char)esc;
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                unsigned int code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!isxdigit((unsigned char)p[i])) {
                        return false;
                    }
                    int d = isdigit((unsigned char)p[i])
                                ? (p[i] - '0')
                                : (tolower((unsigned char)p[i]) - 'a' + 10);
                    code = (code << 4) | (unsigned int)d;
                }
                p += 4;
                ch = (code >= 0x20u && code < 0x80u) ? (unsigned char)code : '?';
                break;
            }
            default:
                return false;
            }
        } else {
            p++;
        }
        if (dst && cap > 0) {
            if (out + 1u < cap) {
                dst[out++] = (char)ch;
            } else if (truncated) {
                *truncated = true;
            }
        }
    }
    if (*p != '"') {
        return false;
    }
    if (dst && cap > 0) {
        dst[out] = '\0';
    }
    *pp = p + 1;
    return true;
}

static bool json_parse_uint64(const char **pp, uint64_t *out_val)
{
    const char *p = json_skip_ws(*pp);
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE) {
        return false;
    }
    if (out_val) {
        *out_val = (uint64_t)val;
    }
    *pp = end;
    return true;
}

static bool json_parse_int64(const char **pp, int64_t *out_val)
{
    const char *p = json_skip_ws(*pp);
    if (*p != '-' && !isdigit((unsigned char)*p)) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long val = strtoll(p, &end, 10);
    if (end == p || errno == ERANGE) {
        return false;
    }
    if (out_val) {
        *out_val = (int64_t)val;
    }
    *pp = end;
    return true;
}

static bool json_skip_value(const char **pp, int depth)
{
    if (depth >= IPC_JSON_MAX_DEPTH) {
        return false;
    }
    const char *p = json_skip_ws(*pp);
    if (*p == '"') {
        return json_parse_string(&p, NULL, 0, NULL) ? (*pp = p, true) : false;
    }
    if (*p == '{') {
        p = json_skip_ws(p + 1);
        if (*p == '}') {
            *pp = p + 1;
            return true;
        }
        for (;;) {
            if (!json_parse_string(&p, NULL, 0, NULL)) {
                return false;
            }
            p = json_skip_ws(p);
            if (*p != ':') {
                return false;
            }
            p++;
            if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
            p = json_skip_ws(p);
            if (*p == ',') {
                p = json_skip_ws(p + 1);
                continue;
            }
            if (*p == '}') {
                *pp = p + 1;
                return true;
            }
            return false;
        }
    }
    if (*p == '[') {
        p = json_skip_ws(p + 1);
        if (*p == ']') {
            *pp = p + 1;
            return true;
        }
        for (;;) {
            if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
            p = json_skip_ws(p);
            if (*p == ',') {
                p = json_skip_ws(p + 1);
                continue;
            }
            if (*p == ']') {
                *pp = p + 1;
                return true;
            }
            return false;
        }
    }
    if (strncmp(p, "true", 4) == 0) {
        *pp = p + 4;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *pp = p + 5;
        return true;
    }
    if (strncmp(p, "null", 4) == 0) {
        *pp = p + 4;
        return true;
    }
    if (*p == '-' || isdigit((unsigned char)*p)) {
        if (*p == '-') {
            p++;
        }
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        if (*p == '.') {
            p++;
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
            while (isdigit((unsigned char)*p)) {
                p++;
            }
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') {
                p++;
            }
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
            while (isdigit((unsigned char)*p)) {
                p++;
            }
        }
        *pp = p;
        return true;
    }
    return false;
}

typedef struct {
    bool has_output;
    char output[64];
    bool has_ws_name;
    char ws_name[64];
    bool has_fs;
    bool is_fs;
} sway_parsed_event_t;

static bool parse_sway_object(const char **pp, sway_parsed_event_t *ev, bool in_current,
                              bool in_container, int depth)
{
    if (depth >= IPC_JSON_MAX_DEPTH) {
        return false;
    }
    const char *p = json_skip_ws(*pp);
    if (*p != '{') {
        return json_skip_value(pp, depth);
    }
    p = json_skip_ws(p + 1);
    if (*p == '}') {
        *pp = p + 1;
        return true;
    }
    for (;;) {
        char key[64];
        bool key_trunc = false;
        if (!json_parse_string(&p, key, sizeof(key), &key_trunc)) {
            return false;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = json_skip_ws(p + 1);

        if (!key_trunc && strcmp(key, "current") == 0 && !in_current && !in_container &&
            *p == '{') {
            if (!parse_sway_object(&p, ev, true, false, depth + 1)) {
                return false;
            }
        } else if (!key_trunc && strcmp(key, "container") == 0 && !in_current && !in_container &&
                   *p == '{') {
            if (!parse_sway_object(&p, ev, false, true, depth + 1)) {
                return false;
            }
        } else if (!key_trunc && strcmp(key, "output") == 0 && *p == '"') {
            char val[64];
            bool val_trunc = false;
            if (!json_parse_string(&p, val, sizeof(val), &val_trunc)) {
                return false;
            }
            if (!val_trunc && val[0] != '\0' && (in_current || in_container || !ev->has_output)) {
                snprintf(ev->output, sizeof(ev->output), "%s", val);
                ev->has_output = true;
            }
        } else if (!key_trunc && in_current && strcmp(key, "name") == 0 && *p == '"') {
            char val[64];
            bool val_trunc = false;
            if (!json_parse_string(&p, val, sizeof(val), &val_trunc)) {
                return false;
            }
            if (!val_trunc && val[0] != '\0') {
                snprintf(ev->ws_name, sizeof(ev->ws_name), "%s", val);
                ev->has_ws_name = true;
            }
        } else if (!key_trunc && strcmp(key, "fullscreen_mode") == 0 &&
                   (in_container || (!in_current && depth == 0)) &&
                   (*p == '-' || isdigit((unsigned char)*p))) {
            int64_t mode = 0;
            if (!json_parse_int64(&p, &mode)) {
                return false;
            }
            ev->has_fs = true;
            ev->is_fs = (mode == 1 || mode == 2);
        } else {
            if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
        }

        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return true;
        }
        return false;
    }
}

void waywal_power_governor_feed_sway_payload(waywal_power_governor_t *gov, const char *json)
{
    if (!gov || !json) {
        return;
    }

    sway_parsed_event_t ev;
    memset(&ev, 0, sizeof(ev));
    const char *p = json;
    if (!parse_sway_object(&p, &ev, false, false, 0)) {
        return;
    }

    if (ev.has_output && ev.output[0] != '\0') {
        snprintf(gov->focused_output, sizeof(gov->focused_output), "%s", ev.output);
        find_or_create_output(gov, gov->focused_output, true);
    }

    if (ev.has_ws_name && ev.ws_name[0] != '\0') {
        snprintf(gov->active_workspace, sizeof(gov->active_workspace), "%s", ev.ws_name);
        gov->workspace_changed = true;
    }

    if (!ev.has_fs) {
        return;
    }
    if (!ev.is_fs) {
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
    }

    const char *out_name =
        (ev.has_output && ev.output[0] != '\0') ? ev.output : gov->focused_output;
    if (out_name[0] != '\0') {
        waywal_power_governor_set_output_reason(gov, out_name, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN,
                                                ev.is_fs);
    } else {
        waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, ev.is_fs);
    }
}

typedef struct {
    bool has_id;
    uint64_t id;
    bool has_output;
    char output[64];
    bool has_fs;
    bool is_fs;
    bool has_focused;
    bool focused;
} niri_obj_fields_t;

static bool parse_niri_object(const char **pp, niri_obj_fields_t *out, int depth);

static bool parse_niri_fields_in_object(const char **pp, niri_obj_fields_t *out, int depth,
                                        bool check_window_sub)
{
    if (depth >= IPC_JSON_MAX_DEPTH) {
        return false;
    }
    const char *p = json_skip_ws(*pp);
    if (*p != '{') {
        return json_skip_value(pp, depth);
    }
    p = json_skip_ws(p + 1);
    if (*p == '}') {
        *pp = p + 1;
        return true;
    }
    for (;;) {
        char key[64];
        bool key_trunc = false;
        if (!json_parse_string(&p, key, sizeof(key), &key_trunc)) {
            return false;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = json_skip_ws(p + 1);

        if (!key_trunc && check_window_sub && strcmp(key, "window") == 0 && *p == '{') {
            if (!parse_niri_object(&p, out, depth + 1)) {
                return false;
            }
        } else if (!key_trunc && strcmp(key, "id") == 0 && isdigit((unsigned char)*p)) {
            if (!json_parse_uint64(&p, &out->id)) {
                return false;
            }
            out->has_id = true;
        } else if (!key_trunc && strcmp(key, "output") == 0 && *p == '"') {
            char val[64];
            bool val_trunc = false;
            if (!json_parse_string(&p, val, sizeof(val), &val_trunc)) {
                return false;
            }
            if (!val_trunc && val[0] != '\0') {
                snprintf(out->output, sizeof(out->output), "%s", val);
                out->has_output = true;
            }
        } else if (!key_trunc &&
                   (strcmp(key, "is_fullscreen") == 0 || strcmp(key, "fullscreen") == 0)) {
            if (strncmp(p, "true", 4) == 0) {
                p += 4;
                out->has_fs = true;
                out->is_fs = true;
            } else if (strncmp(p, "false", 5) == 0) {
                p += 5;
                out->has_fs = true;
                out->is_fs = false;
            } else if (*p == '1' || *p == '0') {
                out->has_fs = true;
                out->is_fs = (*p == '1');
                p++;
            } else if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
        } else if (!key_trunc && strcmp(key, "focused") == 0) {
            if (strncmp(p, "true", 4) == 0) {
                p += 4;
                out->has_focused = true;
                out->focused = true;
            } else if (strncmp(p, "false", 5) == 0) {
                p += 5;
                out->has_focused = true;
                out->focused = false;
            } else if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
        } else {
            if (!json_skip_value(&p, depth + 1)) {
                return false;
            }
        }

        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return true;
        }
        return false;
    }
}

static bool parse_niri_object(const char **pp, niri_obj_fields_t *out, int depth)
{
    memset(out, 0, sizeof(*out));
    return parse_niri_fields_in_object(pp, out, depth, true);
}

static void apply_niri_window_fullscreen(waywal_power_governor_t *gov, const niri_obj_fields_t *f)
{
    if (!gov || !f || !f->has_fs) {
        return;
    }
    const char *out_name =
        (f->has_output && f->output[0] != '\0') ? f->output : gov->focused_output;
    if (f->is_fs) {
        if (gov->niri_fs_output[0] != '\0' && out_name[0] != '\0' &&
            strcmp(gov->niri_fs_output, out_name) != 0 &&
            (!f->has_id || f->id == 0 || gov->niri_fs_win_id == 0 ||
             f->id == gov->niri_fs_win_id)) {
            waywal_power_governor_set_output_reason(gov, gov->niri_fs_output,
                                                    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
        }
        if (out_name[0] != '\0') {
            snprintf(gov->niri_fs_output, sizeof(gov->niri_fs_output), "%s", out_name);
            gov->niri_fs_win_id = f->has_id ? f->id : 0;
            waywal_power_governor_set_output_reason(gov, out_name, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN,
                                                    true);
        } else {
            waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, true);
        }
    } else {
        gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
        if (gov->niri_fs_output[0] != '\0' &&
            (!f->has_id || f->id == 0 || gov->niri_fs_win_id == 0 ||
             f->id == gov->niri_fs_win_id)) {
            waywal_power_governor_set_output_reason(gov, gov->niri_fs_output,
                                                    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
            gov->niri_fs_output[0] = '\0';
            gov->niri_fs_win_id = 0;
        }
        if (out_name[0] != '\0') {
            waywal_power_governor_set_output_reason(gov, out_name, WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN,
                                                    false);
        }
    }
}

static bool handle_niri_windows_changed(waywal_power_governor_t *gov, const char **pp, int depth)
{
    const char *p = json_skip_ws(*pp);
    if (*p != '{') {
        return json_skip_value(pp, depth);
    }

    gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
    for (size_t i = 0; i < gov->output_count; ++i) {
        if (gov->outputs[i].in_use) {
            gov->outputs[i].mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
        }
    }
    gov->niri_fs_output[0] = '\0';
    gov->niri_fs_win_id = 0;

    p = json_skip_ws(p + 1);
    if (*p == '}') {
        *pp = p + 1;
        return true;
    }
    for (;;) {
        char key[64];
        bool key_trunc = false;
        if (!json_parse_string(&p, key, sizeof(key), &key_trunc)) {
            return false;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = json_skip_ws(p + 1);

        if (!key_trunc && strcmp(key, "windows") == 0 && *p == '[') {
            p = json_skip_ws(p + 1);
            if (*p == ']') {
                p++;
            } else {
                for (;;) {
                    niri_obj_fields_t win;
                    if (!parse_niri_object(&p, &win, depth + 2)) {
                        return false;
                    }
                    if (win.has_output && win.output[0] != '\0') {
                        find_or_create_output(gov, win.output, true);
                        if (win.has_focused && win.focused) {
                            snprintf(gov->focused_output, sizeof(gov->focused_output), "%s",
                                     win.output);
                        }
                    }
                    if (win.has_fs && win.is_fs) {
                        apply_niri_window_fullscreen(gov, &win);
                    }
                    p = json_skip_ws(p);
                    if (*p == ',') {
                        p = json_skip_ws(p + 1);
                        continue;
                    }
                    if (*p == ']') {
                        p++;
                        break;
                    }
                    return false;
                }
            }
        } else if (!json_skip_value(&p, depth + 1)) {
            return false;
        }

        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return true;
        }
        return false;
    }
}

void waywal_power_governor_feed_niri_event(waywal_power_governor_t *gov, const char *json)
{
    if (!gov || !json) {
        return;
    }

    const char *p = json_skip_ws(json);
    if (*p != '{') {
        return;
    }
    p = json_skip_ws(p + 1);
    if (*p == '}') {
        return;
    }

    niri_obj_fields_t flat;
    memset(&flat, 0, sizeof(flat));

    for (;;) {
        char key[64];
        bool key_trunc = false;
        if (!json_parse_string(&p, key, sizeof(key), &key_trunc)) {
            return;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return;
        }
        p = json_skip_ws(p + 1);

        if (!key_trunc &&
            (strcmp(key, "WorkspaceActivated") == 0 || strcmp(key, "WorkspaceFocused") == 0) &&
            *p == '{') {
            niri_obj_fields_t ev;
            if (!parse_niri_object(&p, &ev, 1)) {
                return;
            }
            if (ev.has_output && ev.output[0] != '\0') {
                snprintf(gov->focused_output, sizeof(gov->focused_output), "%s", ev.output);
                find_or_create_output(gov, gov->focused_output, true);
            }
            if (ev.has_id) {
                snprintf(gov->active_workspace, sizeof(gov->active_workspace), "%llu",
                         (unsigned long long)ev.id);
                gov->workspace_changed = true;
            }
            gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
            const char *out_name =
                (ev.has_output && ev.output[0] != '\0') ? ev.output : gov->focused_output;
            if (out_name[0] != '\0') {
                waywal_power_governor_set_output_reason(gov, out_name,
                                                        WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
            }
        } else if (!key_trunc && strcmp(key, "WindowClosed") == 0 && *p == '{') {
            niri_obj_fields_t ev;
            if (!parse_niri_object(&p, &ev, 1)) {
                return;
            }
            gov->global_mask &= ~(uint32_t)WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN;
            if (gov->niri_fs_output[0] != '\0' &&
                (!ev.has_id || ev.id == 0 || gov->niri_fs_win_id == 0 ||
                 ev.id == gov->niri_fs_win_id)) {
                waywal_power_governor_set_output_reason(gov, gov->niri_fs_output,
                                                        WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
                gov->niri_fs_output[0] = '\0';
                gov->niri_fs_win_id = 0;
            }
            if (gov->focused_output[0] != '\0') {
                waywal_power_governor_set_output_reason(gov, gov->focused_output,
                                                        WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
            }
        } else if (!key_trunc && strcmp(key, "WindowsChanged") == 0 && *p == '{') {
            if (!handle_niri_windows_changed(gov, &p, 1)) {
                return;
            }
        } else if (!key_trunc &&
                   (strcmp(key, "WindowOpenedOrChanged") == 0 ||
                    strcmp(key, "WindowFocusChanged") == 0) &&
                   *p == '{') {
            niri_obj_fields_t ev;
            if (!parse_niri_object(&p, &ev, 1)) {
                return;
            }
            if (ev.has_output && ev.output[0] != '\0') {
                snprintf(gov->focused_output, sizeof(gov->focused_output), "%s", ev.output);
                find_or_create_output(gov, gov->focused_output, true);
            }
            apply_niri_window_fullscreen(gov, &ev);
        } else if (!key_trunc && strcmp(key, "id") == 0 && isdigit((unsigned char)*p)) {
            if (!json_parse_uint64(&p, &flat.id)) {
                return;
            }
            flat.has_id = true;
        } else if (!key_trunc && strcmp(key, "output") == 0 && *p == '"') {
            char val[64];
            bool val_trunc = false;
            if (!json_parse_string(&p, val, sizeof(val), &val_trunc)) {
                return;
            }
            if (!val_trunc && val[0] != '\0') {
                snprintf(flat.output, sizeof(flat.output), "%s", val);
                flat.has_output = true;
            }
        } else if (!key_trunc &&
                   (strcmp(key, "is_fullscreen") == 0 || strcmp(key, "fullscreen") == 0)) {
            if (strncmp(p, "true", 4) == 0) {
                p += 4;
                flat.has_fs = true;
                flat.is_fs = true;
            } else if (strncmp(p, "false", 5) == 0) {
                p += 5;
                flat.has_fs = true;
                flat.is_fs = false;
            } else if (*p == '1' || *p == '0') {
                flat.has_fs = true;
                flat.is_fs = (*p == '1');
                p++;
            } else if (!json_skip_value(&p, 1)) {
                return;
            }
        } else {
            if (!json_skip_value(&p, 1)) {
                return;
            }
        }

        p = json_skip_ws(p);
        if (*p == ',') {
            p = json_skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            break;
        }
        return;
    }

    if (flat.has_output && flat.output[0] != '\0') {
        snprintf(gov->focused_output, sizeof(gov->focused_output), "%s", flat.output);
        find_or_create_output(gov, gov->focused_output, true);
    }
    if (flat.has_fs) {
        apply_niri_window_fullscreen(gov, &flat);
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
        if (gov->ipc_connecting || gov->ipc_tx_off < gov->ipc_tx_len) {
            if (!flush_ipc_handshake(gov)) {
                close_ipc_socket(gov);
                init_compositor_ipc(gov);
                gov->ipc_reconnected = (gov->ipc_fd >= 0);
                break;
            }
            if (gov->ipc_connecting) {
                break;
            }
        }
        if (gov->ipc_buf_len >= sizeof(gov->ipc_buf) - 1) {
            if (gov->ipc_backend == IPC_BACKEND_HYPRLAND || gov->ipc_backend == IPC_BACKEND_NIRI) {
                gov->ipc_drop_line = true;
            }
            gov->ipc_buf_len = 0;
        }
        ssize_t n = recv(gov->ipc_fd, gov->ipc_buf + gov->ipc_buf_len,
                         sizeof(gov->ipc_buf) - 1 - gov->ipc_buf_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close_ipc_socket(gov);
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
                if (gov->ipc_drop_line) {
                    gov->ipc_drop_line = false;
                } else if (gov->ipc_backend == IPC_BACKEND_HYPRLAND) {
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
            if (gov->ipc_skip_bytes > 0) {
                size_t skip =
                    gov->ipc_buf_len < gov->ipc_skip_bytes ? gov->ipc_buf_len : gov->ipc_skip_bytes;
                offset += skip;
                gov->ipc_skip_bytes -= skip;
            }
            while (gov->ipc_skip_bytes == 0 && gov->ipc_buf_len - offset >= 14) {
                if (memcmp(gov->ipc_buf + offset, "i3-ipc", 6) != 0) {
                    offset++;
                    continue;
                }
                uint32_t msg_len = 0;
                memcpy(&msg_len, gov->ipc_buf + offset + 6, 4);
                if (msg_len > sizeof(gov->ipc_buf) - 16) {
                    offset += 14u;
                    size_t avail = gov->ipc_buf_len - offset;
                    size_t skip = avail < (size_t)msg_len ? avail : (size_t)msg_len;
                    offset += skip;
                    gov->ipc_skip_bytes = (size_t)msg_len - skip;
                    break;
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
        bool any_ac_readable = false;
        bool any_ac_online = false;
        for (size_t i = 0; i < WAYWAL_SYSFS_AC_COUNT; ++i) {
            if (pread_first_line(gov->ac_fds[i], buf, sizeof(buf))) {
                any_ac_readable = true;
                if (buf[0] == '1') {
                    any_ac_online = true;
                    break;
                }
            }
        }
        if (any_ac_readable && !any_ac_online) {
            discharging = true;
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
    if (strcmp(gov->focused_output, output_name) == 0) {
        gov->focused_output[0] = '\0';
    }
    if (strcmp(gov->niri_fs_output, output_name) == 0) {
        gov->niri_fs_output[0] = '\0';
        gov->niri_fs_win_id = 0;
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
