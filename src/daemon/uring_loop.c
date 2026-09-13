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

    /* Try cooperative single-issuer kernel task execution */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

    int ret = io_uring_queue_init_params(qdepth, &loop->ring, &params);
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
    for (size_t i = 0; i < WAYWAL_URING_QUEUE_DEPTH; ++i) {
        if (loop->slots[i].active && loop->slots[i].fd == fd && loop->slots[i].type == type) {
            slot = &loop->slots[i];
            break;
        }
    }

    if (!slot) {
        for (size_t i = 0; i < WAYWAL_URING_QUEUE_DEPTH; ++i) {
            if (!loop->slots[i].active) {
                slot = &loop->slots[i];
                break;
            }
        }
    }

    if (!slot) {
        WAYWAL_LOG_ERR("No free io_uring event slots available (limit: %d)",
                       WAYWAL_URING_QUEUE_DEPTH);
        return false;
    }

    slot->fd = fd;
    slot->poll_mask = poll_mask;
    slot->type = type;
    slot->user_data = user_data;
    slot->active = true;
    slot->is_multishot = loop->multishot_supported;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
    if (!sqe) {
        WAYWAL_LOG_ERR("io_uring_get_sqe failed: submission queue full");
        return false;
    }

    if (slot->is_multishot) {
        io_uring_prep_poll_multishot(sqe, fd, poll_mask);
    } else {
        io_uring_prep_poll_add(sqe, fd, poll_mask);
    }
    io_uring_sqe_set_data(sqe, slot);
    int ret = io_uring_submit(&loop->ring);
    if (ret < 0) {
        WAYWAL_LOG_ERR("io_uring_submit failed: %s", strerror(-ret));
        return false;
    }

    return true;
}

int uring_loop_dispatch(uring_loop_t *loop, uring_event_handler_t handler)
{
    if (!loop || !loop->initialized || !handler)
        return -EINVAL;

    struct io_uring_cqe *cqe = NULL;
    int ret = io_uring_wait_cqe(&loop->ring, &cqe);
    if (ret < 0) {
        if (ret == -EINTR)
            return 0;
        return ret;
    }

    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&loop->ring, head, cqe)
    {
        count++;
        uring_slot_t *slot = (uring_slot_t *)io_uring_cqe_get_data(cqe);
        if (slot && slot->active) {
            handler(slot->type, slot->fd, (uint32_t)cqe->res, slot->user_data);

            bool multishot_continues =
                slot->is_multishot && ((cqe->flags & IORING_CQE_F_MORE) != 0);

            /* Re-arm poll if slot remains active and multishot didn't continue */
            if (slot->active && loop->running && !multishot_continues) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
                if (sqe) {
                    if (slot->is_multishot) {
                        io_uring_prep_poll_multishot(sqe, slot->fd, slot->poll_mask);
                    } else {
                        io_uring_prep_poll_add(sqe, slot->fd, slot->poll_mask);
                    }
                    io_uring_sqe_set_data(sqe, slot);
                }
            }
        }
    }

    io_uring_cq_advance(&loop->ring, count);
    io_uring_submit(&loop->ring);
    return (int)count;
}

void uring_loop_stop(uring_loop_t *loop)
{
    if (loop) {
        loop->running = false;
    }
}
