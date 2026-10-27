// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_POWER_GOVERNOR_H
#define WAYWAL_POWER_GOVERNOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_MAX_GOVERNED_OUTPUTS 16

typedef enum {
    WAYWAL_PAUSE_NONE = 0,
    WAYWAL_PAUSE_DPMS_OR_UNMAPPED = (1u << 0),
    WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN = (1u << 1),
    WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED = (1u << 2),
    WAYWAL_PAUSE_WORKSPACE_OCCLUDED = (1u << 3),
    WAYWAL_PAUSE_SESSION_IDLE = (1u << 4),
    WAYWAL_PAUSE_BATTERY_SAVER = (1u << 5),
    WAYWAL_PAUSE_USER_MANUAL = (1u << 6),
} waywal_pause_reason_t;

#ifndef WAYWAL_BATTERY_POLICY_DEFINED
#define WAYWAL_BATTERY_POLICY_DEFINED
typedef enum : uint32_t {
    WAYWAL_BATTERY_IGNORE = 0,
    WAYWAL_BATTERY_REDUCE_FPS = 1,
    WAYWAL_BATTERY_PAUSE_VIDEO = 2,
} waywal_battery_policy_t;
#endif

typedef struct waywal_power_governor waywal_power_governor_t;

waywal_power_governor_t *waywal_power_governor_create(waywal_battery_policy_t policy);
void waywal_power_governor_destroy(waywal_power_governor_t *gov);

int waywal_power_governor_get_ipc_fd(const waywal_power_governor_t *gov);
int waywal_power_governor_get_inotify_fd(const waywal_power_governor_t *gov);
bool waywal_power_governor_consume_ipc_reconnected(waywal_power_governor_t *gov);

void waywal_power_governor_dispatch_ipc(waywal_power_governor_t *gov);
void waywal_power_governor_feed_hyprland_event(waywal_power_governor_t *gov, const char *line);
void waywal_power_governor_feed_sway_payload(waywal_power_governor_t *gov, const char *json);
void waywal_power_governor_feed_niri_event(waywal_power_governor_t *gov, const char *json);
bool waywal_power_governor_pop_workspace_change(waywal_power_governor_t *gov, char *out_ws,
                                                size_t ws_cap, char *out_mon, size_t mon_cap);

bool waywal_power_governor_poll_battery(waywal_power_governor_t *gov);
void waywal_power_governor_set_on_battery(waywal_power_governor_t *gov, bool on_battery);
bool waywal_power_governor_is_on_battery(const waywal_power_governor_t *gov);
void waywal_power_governor_set_battery_policy(waywal_power_governor_t *gov,
                                              waywal_battery_policy_t policy);
void waywal_power_governor_set_pause_policies(waywal_power_governor_t *gov,
                                              bool auto_pause_fullscreen, bool auto_pause_maximized,
                                              bool pause_on_occluded_workspace);

void waywal_power_governor_set_output_reason(waywal_power_governor_t *gov, const char *output_name,
                                             waywal_pause_reason_t reason, bool active);
void waywal_power_governor_remove_output(waywal_power_governor_t *gov, const char *output_name);
void waywal_power_governor_set_global_reason(waywal_power_governor_t *gov,
                                             waywal_pause_reason_t reason, bool active);

uint32_t waywal_power_governor_get_output_mask(const waywal_power_governor_t *gov,
                                               const char *output_name);
bool waywal_power_governor_should_pause_output(const waywal_power_governor_t *gov,
                                               const char *output_name);
bool waywal_power_governor_should_pause_all(const waywal_power_governor_t *gov);
uint32_t waywal_power_governor_effective_fps(const waywal_power_governor_t *gov,
                                             const char *output_name, uint32_t target_fps);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_POWER_GOVERNOR_H */
