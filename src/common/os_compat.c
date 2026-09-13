#include "waywal/os_compat.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

int waywal_create_memfd(const char *name, size_t size, unsigned int seals) {
    if (!name) name = "waywal-shm";

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

int64_t waywal_get_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

int64_t waywal_get_monotonic_us(void) {
    return waywal_get_monotonic_ns() / 1000LL;
}
