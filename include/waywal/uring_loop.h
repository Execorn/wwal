// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_URING_LOOP_H
#define WAYWAL_URING_LOOP_H

#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_URING_QUEUE_DEPTH 64

typedef enum {
    URING_EV_WAYLAND_READ = 1,
    URING_EV_IPC_ACCEPT = 2,
    URING_EV_IPC_READ = 3,
    URING_EV_TIMER = 4,
    URING_EV_SIGNAL = 5,
    URING_EV_INOTIFY = 6,
    URING_EV_POWER_IPC = 7,
    URING_EV_POWER_INOTIFY = 8,
    URING_EV_SHADER_TIMER = 9,
    URING_EV_SHADER_INOTIFY = 10,
    URING_EV_ANIM_TIMER = 11,
} uring_event_type_t;

typedef struct {
    int fd;
    uint32_t poll_mask;
    uring_event_type_t type;
    void *user_data;
    bool active;
    bool is_multishot;
} uring_slot_t;

typedef struct {
    struct io_uring ring;
    uring_slot_t slots[WAYWAL_URING_QUEUE_DEPTH];
    size_t num_slots;
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

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_URING_LOOP_H */
