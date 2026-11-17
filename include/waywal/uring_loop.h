// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_URING_LOOP_H
#define WAYWAL_URING_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define WWAL_URING_ALIGN16 alignas(16)
#else
#define WWAL_URING_ALIGN16 _Alignas(16)
#endif

#define WAYWAL_URING_QUEUE_DEPTH  128
#define WAYWAL_URING_MAX_HANDLERS 128

/* Opaque backing for struct io_uring so ISO C23 headers avoid pulling
 * POSIX/GNU-only definitions (sigset_t, idtype_t) from <liburing.h>. */
typedef struct {
    WWAL_URING_ALIGN16 uint8_t opaque[320];
} waywal_io_uring_storage_t;

typedef enum {
    URING_EV_WAYLAND_READ = 1,
    URING_EV_WAYLAND = URING_EV_WAYLAND_READ,
    URING_EV_IPC_ACCEPT = 2,
    URING_EV_IPC_READ = 3,
    URING_EV_TIMER = 4,
    URING_EV_TRANSITION_TIMER = URING_EV_TIMER,
    URING_EV_SIGNAL = 5,
    URING_EV_INOTIFY = 6,
    URING_EV_POWER_IPC = 7,
    URING_EV_POWER_INOTIFY = 8,
    URING_EV_SHADER_TIMER = 9,
    URING_EV_SHADER_INOTIFY = 10,
    URING_EV_ANIM_TIMER = 11,
    URING_EV_SOLAR_TIMER = 12,
    EVENT_TYPE_SOLAR_TIMER = URING_EV_SOLAR_TIMER,
    URING_EV_MPRIS = 13,
    URING_EV_GPU_FENCE = 14,
} uring_event_type_t;

typedef struct {
    int fd;
    uint32_t poll_mask;
    uring_event_type_t type;
    void *user_data;
    uint32_t gen;
    uint64_t in_flight_tag;
    bool active;
    bool is_multishot;
    bool in_flight;
} uring_slot_t;

typedef struct {
    waywal_io_uring_storage_t ring;
    uring_slot_t slots[WAYWAL_URING_MAX_HANDLERS];
    size_t num_slots;
    uint32_t queue_depth;
    uint64_t owner_tid;
    bool owner_bound;
    bool single_issuer;
    bool poll_first_send_supported;
    bool initialized;
    bool running;
    union {
        bool multishot_supported;
        bool multishot_poll_supported;
    };
} uring_loop_t;
typedef uring_loop_t waywal_uring_loop_t;

typedef void (*uring_event_handler_t)(uring_event_type_t type, int fd, uint32_t res,
                                      void *user_data);

bool uring_loop_init(uring_loop_t *loop, uint32_t depth);
void uring_loop_destroy(uring_loop_t *loop);

/* Registers an fd with poll notifications inside the io_uring instance */
bool uring_loop_add_poll(uring_loop_t *loop, int fd, uint32_t poll_mask, uring_event_type_t type,
                         void *user_data);

/* Cancels and deactivates any active poll slot for fd */
bool uring_loop_remove_poll(waywal_uring_loop_t *loop, int fd);

/* Dispatches pending completions; blocks until at least 1 event is ready */
int uring_loop_dispatch(uring_loop_t *loop, uring_event_handler_t handler);

void uring_loop_stop(uring_loop_t *loop);

/* Explicitly flushes/submits any queued SQEs */
int uring_loop_submit(waywal_uring_loop_t *loop);

bool uring_loop_check_owner_thread(const uring_loop_t *loop);
bool uring_loop_poll_first_send_supported(const uring_loop_t *loop);
int uring_loop_submit_send_multishot(uring_loop_t *loop, int fd, const void *buf, size_t len,
                                     int msg_flags, uint64_t tag);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_URING_LOOP_H */
