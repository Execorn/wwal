// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_OS_COMPAT_H
#define WAYWAL_OS_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates an anonymous shared memory descriptor (using memfd_create with seals) */
int waywal_create_memfd(const char *name, size_t size, unsigned int seals);

/* Opens rel_path strictly beneath anchored dirfd with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS.
 * Rejects absolute paths, empty segments, '.' and '..' components while permitting '..' inside
 * filenames. Preserves mode for O_CREAT / O_TMPFILE and fails closed with ENOSYS if unsupported. */
int wwal_openat2_beneath(int dirfd, const char *rel_path, int flags, mode_t mode);

/* Monotonic time helpers */
int64_t waywal_get_monotonic_ns(void);
int64_t waywal_get_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_OS_COMPAT_H */
