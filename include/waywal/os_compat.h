#ifndef WAYWAL_OS_COMPAT_H
#define WAYWAL_OS_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates an anonymous shared memory descriptor (using memfd_create with seals) */
int waywal_create_memfd(const char *name, size_t size, unsigned int seals);

/* Monotonic time helpers */
int64_t waywal_get_monotonic_ns(void);
int64_t waywal_get_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_OS_COMPAT_H */
