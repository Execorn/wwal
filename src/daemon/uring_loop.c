// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/uring_loop.h"

#include "waywal/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool uring_loop_init(uring_loop_t *loop, uint32_t depth)
{
    if (!loop)
        return false;
    memset(loop, 0, sizeof(*loop));

    uint32_t qdepth = depth > 0 ? depth : WAYWAL_URING_QUEUE_DEPTH;

    /* Try cooperative single-issuer + deferred kernel task execution */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
#ifdef IORING_SETUP_DEFER_TASKRUN
    params.flags =
        IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#else
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
#endif

    int ret = io_uring_queue_init_params(qdepth, &loop->ring, &params);
    if (ret < 0) {
        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
        ret = io_uring_queue_init_params(qdepth, &loop->ring, &params);
    }
    if (ret < 0) {
        /* Fall back to standard io_uring setup if advanced flags unsupported */
        ret = io_uring_queue_init(qdepth, &loop->ring, 0);
    }

    if (ret < 0) {
        WAYWAL_LOG_ERR("io_uring_queue_init failed: %s (%d)", strerror(-ret), ret);
        return false;
    }

    loop->initialized = true;
    loop->running = true;
    loop->multishot_supported = true;
    WAYWAL_LOG_INFO("Linux io_uring event loop initialized (depth: %u, multishot poll: ENABLED)",
                    qdepth);
    return true;
}

void uring_loop_destroy(uring_loop_t *loop)
{
    if (!loop || !loop->initialized)
        return;

    loop->running = false;
    io_uring_queue_exit(&loop->ring);
    loop->initialized = false;
    WAYWAL_LOG_INFO("io_uring event loop terminated");
}

bool uring_loop_add_poll(uring_loop_t *loop, int fd, uint32_t poll_mask, uring_event_type_t type,
                         void *user_data)
{
    if (!loop || !loop->initialized || fd < 0)
        return false;

    /* Find existing or free slot */
    uring_slot_t *slot = NULL;
    size_t slot_idx = 0;
    for (size_t i = 0; i < WAYWAL_URING_MAX_HANDLERS; ++i) {
        if (loop->slots[i].active && loop->slots[i].fd == fd && loop->slots[i].type == type) {
            slot = &loop->slots[i];
            slot_idx = i;
            slot->user_data = user_data;
            if (slot->poll_mask == poll_mask && slot->in_flight) {
                return true;
            }
            if (slot->in_flight && slot->in_flight_tag != 0) {
                struct io_uring_sqe *rm_sqe = io_uring_get_sqe(&loop->ring);
                if (!rm_sqe) {
                    (void)io_uring_submit(&loop->ring);
                    rm_sqe = io_uring_get_sqe(&loop->ring);
                }
                if (rm_sqe) {
                    io_uring_prep_poll_remove(rm_sqe, (__u64)slot->in_flight_tag);
                    io_uring_sqe_set_data64(rm_sqe, 0);
                    (void)io_uring_submit(&loop->ring);
                }
                slot->in_flight = false;
                slot->in_flight_tag = 0;
            }
            break;
        }
    }

    if (!slot) {
        for (size_t i = 0; i < WAYWAL_URING_MAX_HANDLERS; ++i) {
            if (!loop->slots[i].active) {
                slot = &loop->slots[i];
                slot_idx = i;
                break;
            }
        }
    }

    if (!slot) {
        WAYWAL_LOG_ERR("No free io_uring event slots available (limit: %d)",
                       WAYWAL_URING_MAX_HANDLERS);
        return false;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
    if (!sqe) {
        (void)io_uring_submit(&loop->ring);
        sqe = io_uring_get_sqe(&loop->ring);
    }
    if (!sqe) {
        WAYWAL_LOG_ERR("io_uring_get_sqe failed: submission queue full");
        return false;
    }

    slot->fd = fd;
    slot->poll_mask = poll_mask;
    slot->type = type;
    slot->user_data = user_data;
    slot->gen++;
    if (slot->gen == 0)
        slot->gen = 1;
    slot->in_flight_tag = ((uint64_t)slot->gen << 32) | (uint64_t)(slot_idx + 1U);
    slot->active = true;
    slot->is_multishot = loop->multishot_supported;
    slot->in_flight = true;

    if (slot->is_multishot) {
        io_uring_prep_poll_multishot(sqe, fd, poll_mask);
    } else {
        io_uring_prep_poll_add(sqe, fd, poll_mask);
    }
    io_uring_sqe_set_data64(sqe, slot->in_flight_tag);
    int ret = io_uring_submit(&loop->ring);
    if (ret < 0) {
        WAYWAL_LOG_ERR("io_uring_submit failed: %s", strerror(-ret));
        uint32_t saved_gen = slot->gen;
        memset(slot, 0, sizeof(*slot));
        slot->gen = saved_gen;
        return false;
    }

    return true;
}

bool uring_loop_remove_poll(waywal_uring_loop_t *loop, int fd)
{
    if (!loop || !loop->initialized || fd < 0)
        return false;

    bool removed = false;
    for (size_t i = 0; i < WAYWAL_URING_MAX_HANDLERS; ++i) {
        if (loop->slots[i].active && loop->slots[i].fd == fd) {
            uring_slot_t *slot = &loop->slots[i];
            bool was_in_flight = slot->in_flight;
            uint64_t old_tag = slot->in_flight_tag;
            slot->active = false;
            slot->in_flight = false;
            slot->in_flight_tag = 0;
            slot->fd = -1;
            if (was_in_flight && old_tag != 0) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
                if (!sqe) {
                    (void)io_uring_submit(&loop->ring);
                    sqe = io_uring_get_sqe(&loop->ring);
                }
                if (sqe) {
                    io_uring_prep_poll_remove(sqe, (__u64)old_tag);
                    io_uring_sqe_set_data64(sqe, 0);
                    (void)io_uring_submit(&loop->ring);
                }
            }
            removed = true;
        }
    }
    return removed;
}

int uring_loop_dispatch(uring_loop_t *loop, uring_event_handler_t handler)
{
    if (!loop || !loop->initialized || !handler)
        return -EINVAL;

    int dispatched_events = 0;
    while (dispatched_events == 0 && loop->running) {
        struct io_uring_cqe *cqe = NULL;
        int ret = io_uring_wait_cqe(&loop->ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR)
                return 0;
            return ret;
        }

        unsigned head;
        unsigned count = 0;

        for (int pass = 0; pass < 2; ++pass) {
            unsigned cqe_idx = 0;
            io_uring_for_each_cqe(&loop->ring, head, cqe)
            {
                if (pass == 0) {
                    count++;
                } else {
                    if (cqe_idx >= count) {
                        break;
                    }
                    cqe_idx++;
                }
                uint64_t tag = io_uring_cqe_get_data64(cqe);
                if (tag == 0) {
                    continue;
                }
                uint32_t idx = (uint32_t)(tag & 0xFFFFFFFFU) - 1U;
                if (idx >= WAYWAL_URING_MAX_HANDLERS) {
                    continue;
                }
                uring_slot_t *slot = &loop->slots[idx];
                if (!slot->active || slot->in_flight_tag != tag) {
                    continue;
                }
                bool is_wayland = (slot->type == URING_EV_WAYLAND_READ);
                if ((pass == 0 && !is_wayland) || (pass == 1 && is_wayland)) {
                    continue;
                }

                bool multishot_continues = ((cqe->flags & IORING_CQE_F_MORE) != 0);
                if (cqe->res < 0) {
                    if (cqe->res == -ECANCELED) {
                        slot->in_flight = false;
                        slot->in_flight_tag = 0;
                        continue;
                    }
                    if (cqe->res == -EINTR || cqe->res == -EAGAIN) {
                        if (!multishot_continues) {
                            slot->in_flight = false;
                        }
                    } else {
                        if (cqe->res == -EINVAL && slot->is_multishot) {
                            loop->multishot_supported = false;
                            slot->is_multishot = false;
                            slot->in_flight = false;
                        } else {
                            WAYWAL_LOG_WARN("uring poll error: fd=%d res=%d", slot->fd, cqe->res);
                            if (!multishot_continues) {
                                slot->in_flight = false;
                                slot->in_flight_tag = 0;
                                slot->active = false;
                            }
                            continue;
                        }
                    }
                } else {
                    if (!multishot_continues) {
                        slot->in_flight = false;
                    }
                    dispatched_events++;
                    handler(slot->type, slot->fd, (uint32_t)cqe->res, slot->user_data);
                }

                /* Re-arm poll if the same slot generation remains active and no SQE is in flight */
                if (slot->active && slot->in_flight_tag == tag && loop->running &&
                    !slot->in_flight) {
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
                    if (!sqe) {
                        io_uring_submit(&loop->ring);
                        sqe = io_uring_get_sqe(&loop->ring);
                    }
                    if (sqe) {
                        slot->gen++;
                        if (slot->gen == 0)
                            slot->gen = 1;
                        slot->in_flight_tag = ((uint64_t)slot->gen << 32) | (uint64_t)(idx + 1U);
                        slot->in_flight = true;
                        if (slot->is_multishot) {
                            io_uring_prep_poll_multishot(sqe, slot->fd, slot->poll_mask);
                        } else {
                            io_uring_prep_poll_add(sqe, slot->fd, slot->poll_mask);
                        }
                        io_uring_sqe_set_data64(sqe, slot->in_flight_tag);
                    } else {
                        WAYWAL_LOG_ERR("uring_loop_dispatch: SQ ring full, failed to re-arm fd %d",
                                       slot->fd);
                    }
                }
            }
        }

        io_uring_cq_advance(&loop->ring, count);
        io_uring_submit(&loop->ring);
    }
    return dispatched_events;
}

void uring_loop_stop(uring_loop_t *loop)
{
    if (loop) {
        loop->running = false;
    }
}
