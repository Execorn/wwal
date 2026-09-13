#ifndef WAYWAL_ARENA_H
#define WAYWAL_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
    size_t committed;
} arena_t;

typedef struct {
    arena_t *arena;
    size_t offset;
} arena_temp_t;

#define WAYWAL_PERSISTENT_ARENA_CAPACITY (16 * 1024 * 1024)
#define WAYWAL_FRAME_ARENA_CAPACITY      (4 * 1024 * 1024)

/* Initialize arena using virtual memory reserve + demand-paging */
bool arena_init(arena_t *arena, size_t reserve_capacity);
void arena_destroy(arena_t *arena);

/* Allocate aligned memory from the arena */
void *arena_alloc(arena_t *arena, size_t size, size_t alignment);
void *arena_alloc_zero(arena_t *arena, size_t size, size_t alignment);

/* Reset entire arena to zero offset */
void arena_reset(arena_t *arena);

/* Scoped temporary scratchpad allocation */
arena_temp_t arena_temp_begin(arena_t *arena);
void arena_temp_end(arena_temp_t temp);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_ARENA_H */
