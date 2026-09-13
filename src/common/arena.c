#include "waywal/arena.h"

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

static size_t get_page_size(void) {
    static size_t page_size = 0;
    if (page_size == 0) {
        long res = sysconf(_SC_PAGESIZE);
        page_size = (res > 0) ? (size_t)res : 4096;
    }
    return page_size;
}

bool arena_init(arena_t *arena, size_t reserve_capacity) {
    if (!arena) return false;

    size_t page_size = get_page_size();
    if (reserve_capacity == 0) {
        reserve_capacity = 4 * 1024 * 1024; /* 4 MB default */
    }
    reserve_capacity = (reserve_capacity + page_size - 1) & ~(page_size - 1);

    void *buf = mmap(NULL, reserve_capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        arena->buffer = NULL;
        arena->capacity = 0;
        arena->offset = 0;
        arena->committed = 0;
        return false;
    }

    arena->buffer = (uint8_t *)buf;
    arena->capacity = reserve_capacity;
    arena->offset = 0;
    arena->committed = reserve_capacity;
    return true;
}

void arena_destroy(arena_t *arena) {
    if (!arena || !arena->buffer) return;
    munmap(arena->buffer, arena->capacity);
    arena->buffer = NULL;
    arena->capacity = 0;
    arena->offset = 0;
    arena->committed = 0;
}

void *arena_alloc(arena_t *arena, size_t size, size_t alignment) {
    if (!arena || !arena->buffer || size == 0) return NULL;

    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        alignment = sizeof(void *);
    }

    uintptr_t curr = (uintptr_t)(arena->buffer + arena->offset);
    uintptr_t aligned = (curr + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
    size_t aligned_offset = (size_t)(aligned - (uintptr_t)arena->buffer);
    size_t new_offset = aligned_offset + size;

    if (new_offset > arena->capacity) {
        return NULL;
    }

    arena->offset = new_offset;
    return (void *)aligned;
}

void *arena_alloc_zero(arena_t *arena, size_t size, size_t alignment) {
    void *ptr = arena_alloc(arena, size, alignment);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void arena_reset(arena_t *arena) {
    if (!arena) return;
    arena->offset = 0;
}

arena_temp_t arena_temp_begin(arena_t *arena) {
    arena_temp_t temp = {
        .arena = arena,
        .offset = arena ? arena->offset : 0,
    };
    return temp;
}

void arena_temp_end(arena_temp_t temp) {
    if (temp.arena && temp.offset <= temp.arena->offset) {
        temp.arena->offset = temp.offset;
    }
}
