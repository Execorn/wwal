// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/os_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct open_how) == 24u,
               "Linux struct open_how must be 24 bytes for SYS_openat2");
_Static_assert((RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS) != 0u,
               "openat2 resolve flags must combine RESOLVE_BENEATH and RESOLVE_NO_SYMLINKS");

static bool validate_beneath_rel_path(const char *rel_path, int *out_errno)
{
    if (!rel_path || rel_path[0] == '\0') {
        *out_errno = EINVAL;
        return false;
    }
    if (rel_path[0] == '/') {
        *out_errno = EXDEV;
        return false;
    }

    const char *p = rel_path;
    while (*p != '\0') {
        const char *seg = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 0u || (seg_len == 1u && seg[0] == '.')) {
            *out_errno = EINVAL;
            return false;
        }
        if (seg_len == 2u && seg[0] == '.' && seg[1] == '.') {
            *out_errno = EXDEV;
            return false;
        }
        if (*p == '/') {
            p++;
            /* Reject trailing slash so empty terminal segments cannot alias directories. */
            if (*p == '\0') {
                *out_errno = EINVAL;
                return false;
            }
        }
    }
    return true;
}

int wwal_openat2_beneath(int dirfd, const char *rel_path, int flags, mode_t mode)
{
    if (dirfd < 0) {
        errno = EBADF;
        return -1;
    }

    int path_err = EINVAL;
    if (!validate_beneath_rel_path(rel_path, &path_err)) {
        errno = path_err;
        return -1;
    }

    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)flags | (uint64_t)O_CLOEXEC | (uint64_t)O_NOFOLLOW;
    /* O_TMPFILE includes O_DIRECTORY bit, so require exact mask match before supplying mode. */
    bool needs_mode = ((flags & O_CREAT) != 0) || ((flags & O_TMPFILE) == O_TMPFILE);
    how.mode = needs_mode ? ((uint64_t)mode & 07777ULL) : 0ULL;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

#ifdef SYS_openat2
    long fd;
    do {
        fd = syscall(SYS_openat2, dirfd, rel_path, &how, sizeof(how));
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        return (int)fd;
    }
    /* Fail closed on -ENOSYS rather than falling back to unconfined openat(). */
    if (errno == ENOSYS) {
        errno = ENOSYS;
    }
    return -1;
#else
    (void)how;
    errno = ENOSYS;
    return -1;
#endif
}

int waywal_create_memfd(const char *name, size_t size, unsigned int seals)
{
    if (!name)
        name = "waywal-shm";

    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }

    if (seals != 0) {
        if (fcntl(fd, F_ADD_SEALS, (int)seals) < 0) {
            close(fd);
            return -1;
        }
    }

    return fd;
}

int64_t waywal_get_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t waywal_get_monotonic_us(void) { return waywal_get_monotonic_ns() / 1000LL; }
