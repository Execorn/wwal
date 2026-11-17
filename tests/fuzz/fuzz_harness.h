#ifndef WWAL_TESTS_FUZZ_HARNESS_H
#define WWAL_TESTS_FUZZ_HARNESS_H

#include <stddef.h>
#include <stdint.h>

/* Declare the libFuzzer entrypoint upfront so -Wmissing-prototypes stays clean. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct {
    const uint8_t *data;
    size_t size;
} wwal_fuzz_sample_t;

static inline uint32_t wwal_fuzz_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    if (x == 0u) {
        x = 0x9E3779B9u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#if defined(WWAL_FUZZ_STANDALONE)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WWAL_FUZZ_MAX_SAMPLE_BYTES
#define WWAL_FUZZ_MAX_SAMPLE_BYTES (64u * 1024u * 1024u)
#endif

#ifndef WWAL_FUZZ_MAX_DIR_DEPTH
#define WWAL_FUZZ_MAX_DIR_DEPTH 16
#endif

typedef int (*wwal_fuzz_init_fn)(void);
typedef void (*wwal_fuzz_builtin_fn)(void);
typedef void (*wwal_fuzz_cleanup_fn)(void);

static inline void wwal_fuzz_run_seeds(const wwal_fuzz_sample_t *seeds, size_t seed_count)
{
    static const uint8_t empty_sentinel = 0u;
    for (size_t i = 0u; i < seed_count; ++i) {
        const uint8_t *ptr = seeds[i].data ? seeds[i].data : &empty_sentinel;
        (void)LLVMFuzzerTestOneInput(ptr, seeds[i].size);
    }
}

static inline void wwal_fuzz_run_mutations(const wwal_fuzz_sample_t *seeds, size_t seed_count,
                                           uint32_t rng_seed, uint32_t rounds,
                                           size_t min_mut_buf_cap)
{
    if (!seeds || seed_count == 0u) {
        return;
    }

    wwal_fuzz_run_seeds(seeds, seed_count);

    size_t max_seed_len = min_mut_buf_cap > 64u ? min_mut_buf_cap : 64u;
    for (size_t i = 0u; i < seed_count; ++i) {
        if (seeds[i].size > max_seed_len) {
            max_seed_len = seeds[i].size;
        }
    }

    uint8_t *mut_buf = (uint8_t *)malloc(max_seed_len);
    if (!mut_buf) {
        return;
    }

    static const uint8_t empty_sentinel = 0u;
    uint32_t rng = rng_seed;

    for (uint32_t r = 0u; r < rounds; ++r) {
        size_t base_idx = (size_t)(wwal_fuzz_xorshift32(&rng) % (uint32_t)seed_count);
        const uint8_t *base = seeds[base_idx].data;
        size_t base_len = seeds[base_idx].size;

        if (!base || base_len == 0u) {
            size_t len = (size_t)((wwal_fuzz_xorshift32(&rng) % 64u) + 1u);
            for (size_t k = 0u; k < len; ++k) {
                mut_buf[k] = (uint8_t)(wwal_fuzz_xorshift32(&rng) & 0xFFu);
            }
            (void)LLVMFuzzerTestOneInput(mut_buf, len);
            continue;
        }

        size_t len = base_len;
        memcpy(mut_buf, base, len);

        uint32_t mode = wwal_fuzz_xorshift32(&rng) % 5u;
        uint32_t flips = (wwal_fuzz_xorshift32(&rng) % 10u) + 1u;
        for (uint32_t f = 0u; f < flips; ++f) {
            size_t pos = (size_t)(wwal_fuzz_xorshift32(&rng) % (uint32_t)len);
            if (mode == 0u) {
                mut_buf[pos] ^= (uint8_t)(1u << (wwal_fuzz_xorshift32(&rng) & 7u));
            } else if (mode == 1u) {
                mut_buf[pos] = (uint8_t)(wwal_fuzz_xorshift32(&rng) & 0xFFu);
            } else if (mode == 2u) {
                static const uint8_t interesting[] = {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu};
                mut_buf[pos] = interesting[wwal_fuzz_xorshift32(&rng) % 5u];
            } else {
                mut_buf[pos] ^= (uint8_t)(wwal_fuzz_xorshift32(&rng) & 0xFFu);
            }
        }

        size_t test_len = len;
        if (mode == 4u) {
            /* Keep short truncation bias while letting large seeds keep most bytes. */
            test_len = (size_t)((wwal_fuzz_xorshift32(&rng) % (uint32_t)len) + 1u);
        } else if ((wwal_fuzz_xorshift32(&rng) & 7u) == 0u && len > 8u) {
            test_len = (size_t)((wwal_fuzz_xorshift32(&rng) % (uint32_t)(len / 2u)) + 1u);
        }

        (void)LLVMFuzzerTestOneInput(test_len > 0u ? mut_buf : &empty_sentinel, test_len);
    }

    free(mut_buf);
}

static inline int wwal_fuzz_replay_file(const char *path, size_t *out_count)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[FUZZ] Failed to open sample '%s': %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "[FUZZ] Failed to stat sample '%s': %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return 0;
    }

    if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)WWAL_FUZZ_MAX_SAMPLE_BYTES) {
        fprintf(stderr, "[FUZZ] Sample '%s' exceeds size cap (%u bytes)\n", path,
                (unsigned)WWAL_FUZZ_MAX_SAMPLE_BYTES);
        close(fd);
        return -1;
    }

    size_t cap = (st.st_size > 0) ? (size_t)st.st_size : 64u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }

    size_t total = 0u;
    while (total < (size_t)WWAL_FUZZ_MAX_SAMPLE_BYTES) {
        if (total == cap) {
            size_t next_cap = cap * 2u;
            if (next_cap > (size_t)WWAL_FUZZ_MAX_SAMPLE_BYTES) {
                next_cap = (size_t)WWAL_FUZZ_MAX_SAMPLE_BYTES;
            }
            if (next_cap <= cap) {
                break;
            }
            uint8_t *grown = (uint8_t *)realloc(buf, next_cap);
            if (!grown) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = grown;
            cap = next_cap;
        }
        ssize_t n = read(fd, buf + total, cap - total);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[FUZZ] Read failed on '%s': %s\n", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        total += (size_t)n;
    }
    close(fd);

    static const uint8_t empty_sentinel = 0u;
    (void)LLVMFuzzerTestOneInput(total > 0u ? buf : &empty_sentinel, total);
    free(buf);
    if (out_count) {
        (*out_count)++;
    }
    return 0;
}

static inline int wwal_fuzz_replay_path(const char *path, int depth, size_t *out_count)
{
    if (!path || depth > WWAL_FUZZ_MAX_DIR_DEPTH) {
        return 0;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[FUZZ] Cannot stat path '%s': %s\n", path, strerror(errno));
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        return wwal_fuzz_replay_file(path, out_count);
    }

    if (!S_ISDIR(st.st_mode)) {
        return 0;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "[FUZZ] Cannot open directory '%s': %s\n", path, strerror(errno));
        return -1;
    }

    int rc = 0;
    size_t dir_len = strlen(path);
    bool needs_slash = dir_len > 0u && path[dir_len - 1u] != '/';

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        size_t name_len = strlen(ent->d_name);
        size_t full_len = dir_len + (needs_slash ? 1u : 0u) + name_len + 1u;
        char *child = (char *)malloc(full_len);
        if (!child) {
            rc = -1;
            break;
        }
        snprintf(child, full_len, "%s%s%s", path, needs_slash ? "/" : "", ent->d_name);
        if (wwal_fuzz_replay_path(child, depth + 1, out_count) != 0) {
            rc = -1;
        }
        free(child);
        if (rc != 0) {
            break;
        }
    }

    closedir(dir);
    return rc;
}

static inline int wwal_fuzz_standalone_driver(int argc, char **argv, const char *target_name,
                                              wwal_fuzz_init_fn init_fn,
                                              wwal_fuzz_builtin_fn builtin_fn,
                                              wwal_fuzz_cleanup_fn cleanup_fn)
{
    if (init_fn && init_fn() != 0) {
        if (cleanup_fn) {
            cleanup_fn();
        }
        return 1;
    }

    int rc = 0;
    if (argc > 1) {
        size_t replayed = 0u;
        for (int i = 1; i < argc; ++i) {
            if (wwal_fuzz_replay_path(argv[i], 0, &replayed) != 0) {
                rc = 1;
                break;
            }
        }
        if (rc == 0) {
            printf("[FUZZ] %s replayed %zu sample(s) CLEANLY.\n", target_name, replayed);
        }
    } else {
        printf("[FUZZ] Starting %s standalone fuzzing harness...\n", target_name);
        if (builtin_fn) {
            builtin_fn();
        }
        printf("[FUZZ] %s PASSED CLEANLY (100%%).\n", target_name);
    }

    if (cleanup_fn) {
        cleanup_fn();
    }
    return rc;
}

#define WWAL_FUZZ_HARNESS_MAIN(target_name, init_fn, builtin_fn, cleanup_fn)                       \
    int main(int argc, char **argv)                                                                \
    {                                                                                              \
        return wwal_fuzz_standalone_driver(argc, argv, (target_name), (init_fn), (builtin_fn),     \
                                           (cleanup_fn));                                          \
    }

#else

#define WWAL_FUZZ_HARNESS_MAIN(target_name, init_fn, builtin_fn, cleanup_fn)

#endif /* defined(WWAL_FUZZ_STANDALONE) */

#endif /* WWAL_TESTS_FUZZ_HARNESS_H */
