// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/slideshow.h"

#include "wayland_core.h"
#include "waywal/image_loader.h"
#include "waywal/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

extern bool try_start_native_animation(daemon_state_t *state, const char *filepath,
                                       const waywal_img_metadata_t *meta, const char *target_out)
    __attribute__((weak));

extern bool daemon_slideshow_present_media(daemon_state_t *state, slideshow_engine_t *ss,
                                           const char *filepath, uint32_t trans_type)
    __attribute__((weak));

extern void daemon_slideshow_prepare_image_output(daemon_state_t *state, slideshow_engine_t *ss)
    __attribute__((weak));

extern bool daemon_start_image_transition(daemon_state_t *state, const uint8_t *pixels,
                                          uint32_t width, uint32_t height,
                                          const waywal_img_metadata_t *meta) __attribute__((weak));

extern void daemon_broadcast_event(daemon_state_t *state, const char *event_name)
    __attribute__((weak));

static bool has_image_extension(const char *name)
{
    if (!name)
        return false;
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".gif") == 0 ||
            strcasecmp(dot, ".avif") == 0 || strcasecmp(dot, ".jxl") == 0 ||
            strcasecmp(dot, ".svg") == 0 || strcasecmp(dot, ".tga") == 0 ||
            strcasecmp(dot, ".pnm") == 0 || strcasecmp(dot, ".ppm") == 0 ||
            strcasecmp(dot, ".pgm") == 0 || strcasecmp(dot, ".qoi") == 0 ||
            strcasecmp(dot, ".ff") == 0 || strcasecmp(dot, ".hdr") == 0);
}

static bool has_video_extension(const char *name)
{
    if (!name)
        return false;
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".webm") == 0 ||
            strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".mov") == 0 ||
            strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".m4v") == 0);
}

static bool has_shader_extension(const char *name)
{
    if (!name)
        return false;
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
            strcasecmp(dot, ".comp") == 0 || strcasecmp(dot, ".shader") == 0);
}

static bool has_supported_media_extension(const char *name)
{
    return has_image_extension(name) || has_video_extension(name) || has_shader_extension(name);
}

static uint32_t slideshow_uniform_u32(uint32_t upper_bound)
{
    if (upper_bound <= 1u) {
        return 0u;
    }
    return arc4random_uniform(upper_bound);
}

static size_t slideshow_uniform_size(size_t upper_bound)
{
    if (upper_bound <= 1u) {
        return 0u;
    }
    if (upper_bound <= (size_t)UINT32_MAX) {
        return (size_t)slideshow_uniform_u32((uint32_t)upper_bound);
    }
    uint64_t ub = (uint64_t)upper_bound;
    uint64_t threshold = (0ULL - ub) % ub;
    for (;;) {
        uint64_t r = ((uint64_t)arc4random() << 32) | (uint64_t)arc4random();
        if (r >= threshold) {
            return (size_t)(r % ub);
        }
    }
}

typedef struct {
    dev_t dev;
    ino_t ino;
    bool occupied;
} slideshow_dir_slot_t;

typedef struct {
    char **files;
    bool *is_dir;
    size_t count;
    size_t cap;
    slideshow_dir_slot_t *visited;
    size_t visited_cap;
    size_t visited_count;
} slideshow_scan_ctx_t;

typedef struct {
    char path[4096];
    struct stat st;
} slideshow_subdir_t;

static bool is_workshop_marker_name(const char *name)
{
    return (strcmp(name, "project.json") == 0 || strcmp(name, "scene.pkg") == 0 ||
            strcmp(name, "gifscene.pkg") == 0 || strcmp(name, "scene.json") == 0);
}

static uint64_t hash_dev_ino(dev_t dev, ino_t ino)
{
    uint64_t h = ((uint64_t)dev * 0x9e3779b97f4a7c15ULL) ^ (uint64_t)ino;
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
}

static bool visited_dirs_grow(slideshow_scan_ctx_t *ctx)
{
    size_t old_cap = ctx->visited_cap;
    size_t new_cap = (old_cap == 0u) ? 256u : (old_cap * 2u);
    if (new_cap > (1u << 20)) {
        return false;
    }
    slideshow_dir_slot_t *tbl =
        (slideshow_dir_slot_t *)calloc(new_cap, sizeof(slideshow_dir_slot_t));
    if (!tbl) {
        return false;
    }
    size_t mask = new_cap - 1u;
    for (size_t i = 0; i < old_cap; ++i) {
        if (!ctx->visited[i].occupied) {
            continue;
        }
        size_t pos = (size_t)hash_dev_ino(ctx->visited[i].dev, ctx->visited[i].ino) & mask;
        while (tbl[pos].occupied) {
            pos = (pos + 1u) & mask;
        }
        tbl[pos] = ctx->visited[i];
    }
    free(ctx->visited);
    ctx->visited = tbl;
    ctx->visited_cap = new_cap;
    return true;
}

static bool visited_dirs_insert(slideshow_scan_ctx_t *ctx, dev_t dev, ino_t ino)
{
    if (ctx->visited_cap == 0u || (ctx->visited_count + 1u) * 2u > ctx->visited_cap) {
        if (!visited_dirs_grow(ctx)) {
            return false;
        }
    }
    size_t mask = ctx->visited_cap - 1u;
    size_t pos = (size_t)hash_dev_ino(dev, ino) & mask;
    while (ctx->visited[pos].occupied) {
        if (ctx->visited[pos].dev == dev && ctx->visited[pos].ino == ino) {
            return false;
        }
        pos = (pos + 1u) & mask;
    }
    ctx->visited[pos].dev = dev;
    ctx->visited[pos].ino = ino;
    ctx->visited[pos].occupied = true;
    ctx->visited_count++;
    return true;
}

static void scan_add_item(slideshow_scan_ctx_t *ctx, const char *filepath, bool is_dir)
{
    if (!ctx || !filepath)
        return;
    if (ctx->count >= ctx->cap) {
        size_t new_cap = (ctx->cap == 0u) ? 32u : (ctx->cap * 2u);
        char **new_files = (char **)realloc(ctx->files, new_cap * sizeof(char *));
        if (!new_files)
            return;
        ctx->files = new_files;
        bool *new_dirs = (bool *)realloc(ctx->is_dir, new_cap * sizeof(bool));
        if (!new_dirs)
            return;
        ctx->is_dir = new_dirs;
        ctx->cap = new_cap;
    }
    char *dup = strdup(filepath);
    if (!dup)
        return;
    ctx->files[ctx->count] = dup;
    ctx->is_dir[ctx->count] = is_dir;
    ctx->count++;
}

static void scan_dir_at(slideshow_scan_ctx_t *ctx, const char *dir_path, const struct stat *dir_st,
                        bool recursive, bool collect_files, int depth)
{
    if (depth > 32 || !dir_path || !dir_st)
        return;
    if (!visited_dirs_insert(ctx, dir_st->st_dev, dir_st->st_ino)) {
        return;
    }

    int dfd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return;

    DIR *dir = fdopendir(dfd);
    if (!dir) {
        close(dfd);
        return;
    }

    size_t start_count = ctx->count;
    size_t dir_len = strlen(dir_path);
    const char *sep = (dir_len > 0 && dir_path[dir_len - 1] == '/') ? "" : "/";

    slideshow_subdir_t *subdirs = NULL;
    size_t num_subdirs = 0;
    size_t cap_subdirs = 0;
    bool is_workshop = false;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        if (is_workshop_marker_name(ent->d_name)) {
            bool is_reg = false;
#ifdef _DIRENT_HAVE_D_TYPE
            if (ent->d_type == DT_REG) {
                is_reg = true;
            } else if (ent->d_type == DT_LNK || ent->d_type == DT_UNKNOWN)
#endif
            {
                struct stat cst;
                if (fstatat(dfd, ent->d_name, &cst, 0) == 0 && S_ISREG(cst.st_mode)) {
                    is_reg = true;
                }
            }
            if (is_reg) {
                is_workshop = true;
                break;
            }
            continue;
        }

        char full_path[4096];
        int n = snprintf(full_path, sizeof(full_path), "%s%s%s", dir_path, sep, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full_path)) {
            continue;
        }

        bool child_is_reg = false;
        bool child_is_dir = false;
        struct stat child_st;
        bool have_st = false;

#ifdef _DIRENT_HAVE_D_TYPE
        if (ent->d_type == DT_REG) {
            child_is_reg = true;
        } else if (ent->d_type == DT_DIR) {
            child_is_dir = true;
        } else if (ent->d_type == DT_LNK || ent->d_type == DT_UNKNOWN)
#endif
        {
            if (fstatat(dfd, ent->d_name, &child_st, 0) == 0) {
                have_st = true;
                child_is_reg = S_ISREG(child_st.st_mode);
                child_is_dir = S_ISDIR(child_st.st_mode);
            }
        }

        if (child_is_reg) {
            if (collect_files && has_supported_media_extension(ent->d_name)) {
                scan_add_item(ctx, full_path, false);
            }
        } else if (child_is_dir && (recursive || depth == 0)) {
            if (!have_st && fstatat(dfd, ent->d_name, &child_st, 0) != 0) {
                continue;
            }
            if (num_subdirs >= cap_subdirs) {
                size_t nc = (cap_subdirs == 0u) ? 16u : (cap_subdirs * 2u);
                slideshow_subdir_t *ns =
                    (slideshow_subdir_t *)realloc(subdirs, nc * sizeof(slideshow_subdir_t));
                if (!ns) {
                    continue;
                }
                subdirs = ns;
                cap_subdirs = nc;
            }
            memcpy(subdirs[num_subdirs].path, full_path, (size_t)n + 1u);
            subdirs[num_subdirs].st = child_st;
            num_subdirs++;
        }
    }

    closedir(dir);

    if (is_workshop) {
        for (size_t i = start_count; i < ctx->count; ++i) {
            free(ctx->files[i]);
        }
        ctx->count = start_count;
        free(subdirs);
        scan_add_item(ctx, dir_path, true);
        return;
    }

    for (size_t i = 0; i < num_subdirs; ++i) {
        scan_dir_at(ctx, subdirs[i].path, &subdirs[i].st, recursive, recursive, depth + 1);
    }
    free(subdirs);
}

static void scan_path_tree(slideshow_scan_ctx_t *ctx, const char *path, bool recursive)
{
    if (!ctx || !path || path[0] == '\0')
        return;

    struct stat st;
    if (stat(path, &st) < 0)
        return;

    if (S_ISREG(st.st_mode)) {
        if (has_supported_media_extension(path)) {
            scan_add_item(ctx, path, false);
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        scan_dir_at(ctx, path, &st, recursive, true, 0);
    }
}

typedef struct {
    char *path;
    bool is_dir;
    time_t sec;
    long nsec;
} sort_entry_t;

static int cmp_entry_asc(const void *a, const void *b)
{
    const sort_entry_t *ea = (const sort_entry_t *)a;
    const sort_entry_t *eb = (const sort_entry_t *)b;
    return strcmp(ea->path, eb->path);
}

static int cmp_entry_desc(const void *a, const void *b)
{
    const sort_entry_t *ea = (const sort_entry_t *)a;
    const sort_entry_t *eb = (const sort_entry_t *)b;
    return strcmp(eb->path, ea->path);
}

static int cmp_entry_mtime(const void *a, const void *b)
{
    const sort_entry_t *ea = (const sort_entry_t *)a;
    const sort_entry_t *eb = (const sort_entry_t *)b;
    if (ea->sec != eb->sec) {
        return (ea->sec > eb->sec) ? -1 : 1;
    }
    if (ea->nsec != eb->nsec) {
        return (ea->nsec > eb->nsec) ? -1 : 1;
    }
    return strcmp(ea->path, eb->path);
}

static void swap_slide_items(char **files, bool *is_dir, size_t a, size_t b)
{
    if (a == b)
        return;
    char *tmp_p = files[a];
    files[a] = files[b];
    files[b] = tmp_p;
    if (is_dir) {
        bool tmp_d = is_dir[a];
        is_dir[a] = is_dir[b];
        is_dir[b] = tmp_d;
    }
}

static void shuffle_slide_arrays(char **files, bool *is_dir, size_t num_files,
                                 const char *last_played_path)
{
    if (!files || num_files < 2)
        return;
    for (size_t i = num_files - 1; i > 0; --i) {
        size_t j = slideshow_uniform_size(i + 1u);
        swap_slide_items(files, is_dir, i, j);
    }
    if (last_played_path && last_played_path[0] != '\0' && files[0] &&
        strcmp(files[0], last_played_path) == 0) {
        size_t swap_idx = 1u + slideshow_uniform_size(num_files - 1u);
        swap_slide_items(files, is_dir, 0u, swap_idx);
    }
}

static void shuffle_files(slideshow_engine_t *ss)
{
    if (!ss || ss->num_files < 2)
        return;
    shuffle_slide_arrays(ss->file_list, ss->is_dir_list, ss->num_files, ss->last_played_path);
}

static void sort_or_shuffle_arrays(char **files, bool *is_dir, size_t count,
                                   waywal_sort_mode_t mode, const char *last_played_path)
{
    if (!files || count < 2)
        return;

    sort_entry_t *entries = (sort_entry_t *)malloc(count * sizeof(sort_entry_t));
    if (!entries)
        return;

    for (size_t i = 0; i < count; ++i) {
        entries[i].path = files[i];
        entries[i].is_dir = is_dir ? is_dir[i] : false;
        entries[i].sec = 0;
        entries[i].nsec = 0;
        if (mode == WAYWAL_SORT_MTIME) {
            struct stat st;
            if (stat(files[i], &st) == 0) {
                entries[i].sec = st.st_mtim.tv_sec;
                entries[i].nsec = st.st_mtim.tv_nsec;
            }
        }
    }

    switch (mode) {
    case WAYWAL_SORT_RANDOM:
        qsort(entries, count, sizeof(sort_entry_t), cmp_entry_asc);
        break;
    case WAYWAL_SORT_DESCENDING:
        qsort(entries, count, sizeof(sort_entry_t), cmp_entry_desc);
        break;
    case WAYWAL_SORT_MTIME:
        qsort(entries, count, sizeof(sort_entry_t), cmp_entry_mtime);
        break;
    case WAYWAL_SORT_ASCENDING:
    default:
        qsort(entries, count, sizeof(sort_entry_t), cmp_entry_asc);
        break;
    }

    for (size_t i = 0; i < count; ++i) {
        files[i] = entries[i].path;
        if (is_dir) {
            is_dir[i] = entries[i].is_dir;
        }
    }
    free(entries);

    if (mode == WAYWAL_SORT_RANDOM) {
        shuffle_slide_arrays(files, is_dir, count, last_played_path);
    }
}

static void free_slide_arrays(char **files, bool *is_dir, size_t count)
{
    if (files) {
        for (size_t i = 0; i < count; ++i) {
            free(files[i]);
        }
        free(files);
    }
    free(is_dir);
}

static bool scan_and_build_list(const char *root_path, bool recursive, waywal_sort_mode_t mode,
                                const char *last_played_path, const char *preserve_path,
                                char ***out_files, bool **out_is_dir, size_t *out_count,
                                size_t *out_cap, size_t *out_idx)
{
    slideshow_scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    scan_path_tree(&ctx, root_path, recursive);
    free(ctx.visited);
    ctx.visited = NULL;

    if (ctx.count == 0) {
        free_slide_arrays(ctx.files, ctx.is_dir, ctx.count);
        return false;
    }

    sort_or_shuffle_arrays(ctx.files, ctx.is_dir, ctx.count, mode, last_played_path);

    size_t idx = 0;
    if (mode != WAYWAL_SORT_RANDOM && preserve_path && preserve_path[0] != '\0') {
        for (size_t i = 0; i < ctx.count; ++i) {
            if (ctx.files[i] && strcmp(ctx.files[i], preserve_path) == 0) {
                idx = i;
                break;
            }
        }
    }

    *out_files = ctx.files;
    *out_is_dir = ctx.is_dir;
    *out_count = ctx.count;
    *out_cap = ctx.cap;
    if (out_idx) {
        *out_idx = idx;
    }
    return true;
}

typedef struct slideshow_async {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool sync_init;
    bool thread_started;
    bool stop;
    int timer_fd;

    uint64_t req_gen;
    bool req_pending;
    char req_path[4096];
    int req_dir;
    bool req_from_tick;
    size_t req_fail_streak;

    bool has_done;
    uint64_t done_gen;
    char done_path[4096];
    bool done_ok;
    uint8_t *done_pixels;
    uint32_t done_w;
    uint32_t done_h;
    int done_dir;
    bool done_from_tick;
    size_t done_fail_streak;

    bool has_prefetch_req;
    char prefetch_req_path[4096];

    bool has_prefetch;
    char prefetch_path[4096];
    bool prefetch_ok;
    uint8_t *prefetch_pixels;
    uint32_t prefetch_w;
    uint32_t prefetch_h;
} slideshow_async_t;

typedef enum {
    SLIDE_PRESENT_ASYNC_PENDING = 0,
    SLIDE_PRESENT_SYNC_OK = 1,
    SLIDE_PRESENT_SYNC_FAIL = 2,
} slide_present_res_t;

static void copy_cstr_trunc(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, cap - 1u);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void arm_timer(slideshow_engine_t *ss)
{
    if (!ss || ss->timer_fd < 0 || ss->interval_s == 0)
        return;

    uint64_t dummy;
    while (read(ss->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;

    struct itimerspec its = {
        .it_interval = {.tv_sec = (time_t)ss->interval_s, .tv_nsec = 0},
        .it_value = {.tv_sec = (time_t)ss->interval_s, .tv_nsec = 0},
    };
    timerfd_settime(ss->timer_fd, 0, &its, NULL);
}

static void disarm_timer(slideshow_engine_t *ss)
{
    if (!ss || ss->timer_fd < 0)
        return;

    struct itimerspec its = {0};
    timerfd_settime(ss->timer_fd, 0, &its, NULL);
    uint64_t dummy;
    while (read(ss->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;
}

static void trigger_timer_async_wakeup(int timer_fd)
{
    if (timer_fd < 0)
        return;
    struct itimerspec its = {
        .it_interval = {0, 0},
        .it_value = {.tv_sec = 0, .tv_nsec = 1},
    };
    (void)timerfd_settime(timer_fd, 0, &its, NULL);
}

static void *slideshow_async_worker_main(void *arg)
{
    slideshow_async_t *async = (slideshow_async_t *)arg;
    for (;;) {
        char path[4096];
        bool is_primary = false;
        uint64_t gen = 0;
        int dir = 1;
        bool from_tick = false;
        size_t fail_streak = 0;

        pthread_mutex_lock(&async->mu);
        while (!async->stop && !async->req_pending && !async->has_prefetch_req) {
            pthread_cond_wait(&async->cv, &async->mu);
        }
        if (async->stop) {
            pthread_mutex_unlock(&async->mu);
            break;
        }
        if (async->req_pending) {
            is_primary = true;
            copy_cstr_trunc(path, sizeof(path), async->req_path);
            gen = async->req_gen;
            dir = async->req_dir;
            from_tick = async->req_from_tick;
            fail_streak = async->req_fail_streak;
            async->req_pending = false;
        } else {
            is_primary = false;
            copy_cstr_trunc(path, sizeof(path), async->prefetch_req_path);
            async->has_prefetch_req = false;
        }
        pthread_mutex_unlock(&async->mu);

        uint8_t *pixels = NULL;
        uint32_t img_w = 0, img_h = 0;
        bool ok = image_load(path, &pixels, &img_w, &img_h);
        if (!ok || !pixels || img_w == 0 || img_h == 0) {
            image_free(pixels);
            pixels = NULL;
            img_w = 0;
            img_h = 0;
            ok = false;
        }

        pthread_mutex_lock(&async->mu);
        if (async->stop) {
            pthread_mutex_unlock(&async->mu);
            image_free(pixels);
            break;
        }
        if (is_primary) {
            if (gen == async->req_gen && !async->req_pending) {
                image_free(async->done_pixels);
                async->done_pixels = pixels;
                async->done_w = img_w;
                async->done_h = img_h;
                async->done_ok = ok;
                async->done_gen = gen;
                async->done_dir = dir;
                async->done_from_tick = from_tick;
                async->done_fail_streak = fail_streak;
                copy_cstr_trunc(async->done_path, sizeof(async->done_path), path);
                async->has_done = true;
                trigger_timer_async_wakeup(async->timer_fd);
            } else {
                image_free(pixels);
            }
        } else {
            if (!async->has_prefetch_req && !async->req_pending) {
                image_free(async->prefetch_pixels);
                async->prefetch_pixels = pixels;
                async->prefetch_w = img_w;
                async->prefetch_h = img_h;
                async->prefetch_ok = ok;
                copy_cstr_trunc(async->prefetch_path, sizeof(async->prefetch_path), path);
                async->has_prefetch = true;
            } else {
                image_free(pixels);
            }
        }
        pthread_mutex_unlock(&async->mu);
    }
    return NULL;
}

static slideshow_async_t *ensure_slideshow_async(slideshow_engine_t *ss)
{
    if (!ss || ss->timer_fd < 0)
        return NULL;
    if (ss->async)
        return ss->async;

    slideshow_async_t *async = (slideshow_async_t *)calloc(1, sizeof(slideshow_async_t));
    if (!async)
        return NULL;
    async->timer_fd = ss->timer_fd;
    if (pthread_mutex_init(&async->mu, NULL) != 0) {
        free(async);
        return NULL;
    }
    if (pthread_cond_init(&async->cv, NULL) != 0) {
        pthread_mutex_destroy(&async->mu);
        free(async);
        return NULL;
    }
    async->sync_init = true;
    if (pthread_create(&async->thread, NULL, slideshow_async_worker_main, async) != 0) {
        pthread_cond_destroy(&async->cv);
        pthread_mutex_destroy(&async->mu);
        free(async);
        return NULL;
    }
    async->thread_started = true;
    ss->async = async;
    return async;
}

static void destroy_slideshow_async(slideshow_engine_t *ss)
{
    if (!ss || !ss->async)
        return;
    slideshow_async_t *async = ss->async;
    ss->async = NULL;

    if (async->sync_init) {
        if (async->thread_started) {
            pthread_mutex_lock(&async->mu);
            async->stop = true;
            pthread_cond_signal(&async->cv);
            pthread_mutex_unlock(&async->mu);
            (void)pthread_join(async->thread, NULL);
            async->thread_started = false;
        }
        image_free(async->done_pixels);
        image_free(async->prefetch_pixels);
        pthread_cond_destroy(&async->cv);
        pthread_mutex_destroy(&async->mu);
    }
    free(async);
}

static void cancel_async_requests(slideshow_engine_t *ss, bool clear_prefetch)
{
    if (!ss || !ss->async)
        return;
    slideshow_async_t *async = ss->async;
    pthread_mutex_lock(&async->mu);
    async->req_gen++;
    async->req_pending = false;
    if (async->has_done) {
        image_free(async->done_pixels);
        async->done_pixels = NULL;
        async->has_done = false;
    }
    if (clear_prefetch) {
        async->has_prefetch_req = false;
        if (async->has_prefetch) {
            image_free(async->prefetch_pixels);
            async->prefetch_pixels = NULL;
            async->has_prefetch = false;
        }
    }
    pthread_mutex_unlock(&async->mu);
}

static waywal_sort_mode_t effective_sort_mode(const slideshow_engine_t *ss)
{
    return ss->has_sort_mode ? (waywal_sort_mode_t)ss->sort_mode
                             : (ss->random_order ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING);
}

static void schedule_prefetch_next(slideshow_engine_t *ss)
{
    if (!ss || !ss->daemon_state || ss->num_files < 2 || !ss->file_list)
        return;

    size_t next_idx = ss->current_file_idx + 1u;
    if (next_idx >= ss->num_files) {
        if (effective_sort_mode(ss) == WAYWAL_SORT_RANDOM) {
            return;
        }
        next_idx = 0u;
    }
    const char *next_path = ss->file_list[next_idx];
    bool next_is_dir = ss->is_dir_list ? ss->is_dir_list[next_idx] : false;
    if (!next_path || next_is_dir || has_video_extension(next_path) ||
        has_shader_extension(next_path)) {
        return;
    }

    slideshow_async_t *async = ensure_slideshow_async(ss);
    if (!async)
        return;

    pthread_mutex_lock(&async->mu);
    if ((!async->has_prefetch || strcmp(async->prefetch_path, next_path) != 0) &&
        !async->req_pending) {
        copy_cstr_trunc(async->prefetch_req_path, sizeof(async->prefetch_req_path), next_path);
        async->has_prefetch_req = true;
        pthread_cond_signal(&async->cv);
    }
    pthread_mutex_unlock(&async->mu);
}

static uint32_t choose_transition_type(const slideshow_engine_t *ss)
{
    uint32_t trans_type = ss->transition_type;
    if (trans_type == WAYWAL_TRANSITION_RANDOM) {
        /* Pick random animated transition between SIMPLE (1) and PAGE_CURL (19). */
        trans_type = 1u + slideshow_uniform_u32(19u);
    }
    return trans_type;
}

static bool present_decoded_image(slideshow_engine_t *ss, const char *filepath, uint8_t *pixels,
                                  uint32_t img_w, uint32_t img_h)
{
    if (!ss || !ss->daemon_state || !filepath || !pixels || img_w == 0 || img_h == 0) {
        image_free(pixels);
        return false;
    }

    daemon_state_t *st = ss->daemon_state;
    uint32_t trans_type = choose_transition_type(ss);

    if (daemon_slideshow_prepare_image_output) {
        daemon_slideshow_prepare_image_output(st, ss);
    }

    uint8_t meta_buf[sizeof(waywal_img_metadata_t) + 16 * 64];
    memset(meta_buf, 0, sizeof(meta_buf));
    waywal_img_metadata_t *meta = (waywal_img_metadata_t *)meta_buf;

    meta->width = img_w;
    meta->height = img_h;
    meta->pixel_format = ss->ten_bit ? 1 : 0;
    meta->transition_type = trans_type;
    meta->transition_duration_ms =
        ss->transition_duration_ms > 0 ? ss->transition_duration_ms : 1000;
    meta->transition_fps = ss->transition_fps > 0 ? ss->transition_fps : 60;
    meta->transition_angle_rad = ss->transition_angle_rad;
    meta->transition_wave_freq = ss->transition_wave_freq;
    meta->transition_wave_amp = ss->transition_wave_amp;
    meta->transition_center_x = ss->transition_center_x;
    meta->transition_center_y = ss->transition_center_y;
    memcpy(meta->color, ss->color, sizeof(meta->color));
    meta->sync_mode = 0;
    meta->stagger_delay_ms = 0;
    meta->custom_shader_len = 0;
    meta->scaling_mode = ss->scaling_mode;
    meta->filter_mode = ss->filter_mode;
    meta->bezier = ss->bezier;
    meta->has_custom_bezier = ss->has_custom_bezier;
    meta->ten_bit = ss->ten_bit;
    meta->crop_gravity_x = ss->crop_gravity_x;
    meta->crop_gravity_y = ss->crop_gravity_y;
    meta->has_crop_gravity = ss->has_crop_gravity;
    meta->invert_y = ss->invert_y;
    meta->pos_y_inverted = ss->pos_y_inverted ? 1 : 0;
    meta->no_cache = ss->no_cache ? 1 : 0;
    meta->extract_theme = ss->extract_theme;
    meta->no_theme = ss->no_theme;
    meta->has_theme_mode = ss->has_theme_mode ? 1 : 0;
    meta->theme_light_mode = ss->theme_light_mode;
    meta->has_source_color_index = ss->has_source_color_index ? 1 : 0;
    meta->source_color_index = ss->source_color_index;
    meta->has_seed_color = ss->has_seed_color ? 1 : 0;
    meta->seed_color = ss->seed_color;
    if (ss->auto_day_night != 0) {
        meta->has_auto_day_night = 1;
        meta->auto_day_night = (ss->auto_day_night == 1) ? 1 : 0;
    }
    copy_cstr_trunc(meta->filepath, sizeof(meta->filepath), filepath);

    uint32_t valid_targets = 0;
    size_t extra_off = 0;
    char *extra_ptr = (char *)(meta_buf + sizeof(waywal_img_metadata_t));
    uint32_t max_targets = ss->num_target_outputs <= 16 ? ss->num_target_outputs : 16;
    for (uint32_t i = 0; i < max_targets; ++i) {
        if (ss->target_outputs[i][0] != '\0') {
            size_t len = strnlen(ss->target_outputs[i], 63) + 1;
            if (extra_off + len <= 16 * 64) {
                memcpy(extra_ptr + extra_off, ss->target_outputs[i], len - 1);
                extra_ptr[extra_off + len - 1] = '\0';
                extra_off += len;
                valid_targets++;
            }
        }
    }
    meta->num_target_outputs = valid_targets;
    meta->extra_data_len = (uint32_t)extra_off;

    WAYWAL_LOG_INFO("Slideshow [%zu/%zu]: displaying '%s' (transition: %u)",
                    ss->current_file_idx + 1, ss->num_files, filepath, trans_type);

    if (st->on_wallpaper_loaded) {
        st->on_wallpaper_loaded(st, pixels, img_w, img_h, meta);
    }

    if (daemon_start_image_transition && !ss->no_cache) {
        const char *filt_str =
            waywal_filter_mode_to_swww_str((waywal_filter_mode_t)ss->filter_mode);
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = st->outputs; out != NULL; out = out->next) {
            if (out->slideshow_eng == ss && out->name[0] != '\0') {
                int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                 mlen > 0 ? "," : "", out->name);
                if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                    mlen += (size_t)n;
                }
            }
        }
        const char *first_target = (meta->num_target_outputs > 0) ? extra_ptr : "*";
        (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : first_target,
                                          filepath, filt_str, pixels, img_w, img_h,
                                          st->config.general.caelestia_sync, ss->no_cache);
    }

    bool ok = daemon_start_image_transition
                  ? daemon_start_image_transition(st, pixels, img_w, img_h, meta)
                  : false;
    image_free(pixels);
    if (ok) {
        copy_cstr_trunc(ss->last_played_path, sizeof(ss->last_played_path), filepath);
        if (st->config.hooks.on_wallpaper_change[0] != '\0') {
            (void)waywal_palette_run_hook(st->config.hooks.on_wallpaper_change);
        }
        if (try_start_native_animation) {
            const char *first_target = (meta->num_target_outputs > 0) ? extra_ptr : "*";
            (void)try_start_native_animation(st, filepath, meta, first_target);
        }
    }
    return ok;
}

static void advance_slideshow_index_dir(slideshow_engine_t *ss, int step_dir)
{
    if (!ss || ss->num_files == 0)
        return;
    if (step_dir < 0) {
        if (ss->current_file_idx == 0)
            ss->current_file_idx = ss->num_files - 1u;
        else
            ss->current_file_idx--;
        return;
    }
    if (ss->current_file_idx + 1u >= ss->num_files) {
        ss->current_file_idx = 0u;
        if (effective_sort_mode(ss) == WAYWAL_SORT_RANDOM && ss->num_files > 1u) {
            shuffle_files(ss);
        }
    } else {
        ss->current_file_idx++;
    }
}

static slide_present_res_t request_or_present_slide(slideshow_engine_t *ss, int step_dir,
                                                    bool from_tick, size_t fail_streak)
{
    if (!ss || ss->num_files == 0 || !ss->file_list)
        return SLIDE_PRESENT_SYNC_FAIL;

    for (size_t attempt = fail_streak; attempt < ss->num_files; ++attempt) {
        const char *filepath = ss->file_list[ss->current_file_idx];
        if (!filepath) {
            advance_slideshow_index_dir(ss, step_dir);
            continue;
        }

        if (!ss->daemon_state) {
            copy_cstr_trunc(ss->last_played_path, sizeof(ss->last_played_path), filepath);
            return SLIDE_PRESENT_SYNC_OK;
        }

        bool is_dir = ss->is_dir_list ? ss->is_dir_list[ss->current_file_idx] : false;
        if (is_dir || has_video_extension(filepath) || has_shader_extension(filepath)) {
            cancel_async_requests(ss, false);
            bool ok = false;
            if (daemon_slideshow_present_media) {
                uint32_t trans_type = choose_transition_type(ss);
                WAYWAL_LOG_INFO("Slideshow [%zu/%zu]: presenting live media '%s' (transition: %u)",
                                ss->current_file_idx + 1, ss->num_files, filepath, trans_type);
                ok = daemon_slideshow_present_media(ss->daemon_state, ss, filepath, trans_type);
                if (ok) {
                    copy_cstr_trunc(ss->last_played_path, sizeof(ss->last_played_path), filepath);
                    if (ss->daemon_state->config.hooks.on_wallpaper_change[0] != '\0') {
                        (void)waywal_palette_run_hook(
                            ss->daemon_state->config.hooks.on_wallpaper_change);
                    }
                }
            }
            if (ok) {
                schedule_prefetch_next(ss);
                return SLIDE_PRESENT_SYNC_OK;
            }
            WAYWAL_LOG_WARN("Slideshow: Failed to present media '%s', skipping", filepath);
            advance_slideshow_index_dir(ss, step_dir);
            continue;
        }

        slideshow_async_t *async = ensure_slideshow_async(ss);
        if (!async) {
            uint8_t *pixels = NULL;
            uint32_t img_w = 0, img_h = 0;
            if (image_load(filepath, &pixels, &img_w, &img_h) && pixels && img_w > 0 && img_h > 0) {
                if (present_decoded_image(ss, filepath, pixels, img_w, img_h)) {
                    return SLIDE_PRESENT_SYNC_OK;
                }
            } else {
                image_free(pixels);
                WAYWAL_LOG_WARN("Slideshow: Failed to load image '%s', skipping", filepath);
            }
            advance_slideshow_index_dir(ss, step_dir);
            continue;
        }

        uint8_t *hit_pixels = NULL;
        uint32_t hit_w = 0, hit_h = 0;
        bool cache_hit = false;
        bool cache_ok = false;

        pthread_mutex_lock(&async->mu);
        if (async->has_prefetch && strcmp(async->prefetch_path, filepath) == 0) {
            cache_hit = true;
            cache_ok = async->prefetch_ok;
            hit_pixels = async->prefetch_pixels;
            hit_w = async->prefetch_w;
            hit_h = async->prefetch_h;
            async->prefetch_pixels = NULL;
            async->has_prefetch = false;
            async->req_gen++;
            async->req_pending = false;
        } else {
            if (async->has_done) {
                image_free(async->done_pixels);
                async->done_pixels = NULL;
                async->has_done = false;
            }
            async->req_gen++;
            async->req_pending = true;
            copy_cstr_trunc(async->req_path, sizeof(async->req_path), filepath);
            async->req_dir = step_dir;
            async->req_from_tick = from_tick;
            async->req_fail_streak = attempt;
            async->has_prefetch_req = false;
            pthread_cond_signal(&async->cv);
        }
        pthread_mutex_unlock(&async->mu);

        if (!cache_hit) {
            return SLIDE_PRESENT_ASYNC_PENDING;
        }

        if (cache_ok && present_decoded_image(ss, filepath, hit_pixels, hit_w, hit_h)) {
            schedule_prefetch_next(ss);
            return SLIDE_PRESENT_SYNC_OK;
        }
        image_free(hit_pixels);
        WAYWAL_LOG_WARN("Slideshow: Failed to load image '%s', skipping", filepath);
        advance_slideshow_index_dir(ss, step_dir);
    }

    WAYWAL_LOG_WARN("Slideshow: All %zu candidate items failed to load", ss->num_files);
    return SLIDE_PRESENT_SYNC_FAIL;
}

bool slideshow_init(slideshow_engine_t *ss, struct daemon_state *state)
{
    if (!ss)
        return false;

    memset(ss, 0, sizeof(*ss));
    ss->daemon_state = state;
    ss->recursive = true;
    ss->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ss->timer_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create slideshow timerfd: %s", strerror(errno));
        return false;
    }

    WAYWAL_LOG_INFO("Slideshow engine initialized (timer_fd: %d)", ss->timer_fd);
    return true;
}

bool slideshow_clone(slideshow_engine_t *dst, const slideshow_engine_t *src)
{
    if (!dst || !src)
        return false;

    if (!slideshow_init(dst, src->daemon_state))
        return false;

    int new_timer_fd = dst->timer_fd;
    *dst = *src;
    dst->timer_fd = new_timer_fd;
    dst->file_list = NULL;
    dst->is_dir_list = NULL;
    dst->async = NULL;
    dst->num_files = 0;
    dst->capacity_files = 0;

    if (src->num_files > 0 && src->file_list) {
        dst->file_list = (char **)calloc(src->num_files, sizeof(char *));
        dst->is_dir_list = (bool *)calloc(src->num_files, sizeof(bool));
        if (!dst->file_list || !dst->is_dir_list) {
            slideshow_destroy(dst);
            return false;
        }
        dst->capacity_files = src->num_files;
        for (size_t i = 0; i < src->num_files; ++i) {
            if (!src->file_list[i])
                continue;
            char *dup = strdup(src->file_list[i]);
            if (!dup) {
                slideshow_destroy(dst);
                return false;
            }
            dst->is_dir_list[dst->num_files] = src->is_dir_list ? src->is_dir_list[i] : false;
            dst->file_list[dst->num_files++] = dup;
        }
    }

    if (dst->num_files == 0) {
        dst->current_file_idx = 0;
    } else if (dst->current_file_idx >= dst->num_files) {
        dst->current_file_idx = dst->num_files - 1u;
    }

    if (dst->active && !dst->paused && dst->interval_s > 0) {
        arm_timer(dst);
    }
    return true;
}

void slideshow_destroy(slideshow_engine_t *ss)
{
    if (!ss)
        return;

    slideshow_stop(ss);
    destroy_slideshow_async(ss);

    if (ss->timer_fd >= 0) {
        close(ss->timer_fd);
        ss->timer_fd = -1;
    }

    free_slide_arrays(ss->file_list, ss->is_dir_list, ss->num_files);
    ss->file_list = NULL;
    ss->is_dir_list = NULL;
    ss->num_files = 0;
    ss->capacity_files = 0;
}

bool slideshow_rescan(slideshow_engine_t *ss)
{
    if (!ss || ss->root_path[0] == '\0')
        return false;

    char prev_path[4096];
    prev_path[0] = '\0';
    if (ss->file_list && ss->current_file_idx < ss->num_files &&
        ss->file_list[ss->current_file_idx]) {
        copy_cstr_trunc(prev_path, sizeof(prev_path), ss->file_list[ss->current_file_idx]);
        if (ss->last_played_path[0] == '\0') {
            copy_cstr_trunc(ss->last_played_path, sizeof(ss->last_played_path), prev_path);
        }
    }

    char **new_files = NULL;
    bool *new_dirs = NULL;
    size_t new_count = 0;
    size_t new_cap = 0;
    size_t new_idx = 0;
    waywal_sort_mode_t mode = effective_sort_mode(ss);

    if (!scan_and_build_list(ss->root_path, ss->recursive, mode, ss->last_played_path, prev_path,
                             &new_files, &new_dirs, &new_count, &new_cap, &new_idx)) {
        return false;
    }

    cancel_async_requests(ss, true);
    free_slide_arrays(ss->file_list, ss->is_dir_list, ss->num_files);
    ss->file_list = new_files;
    ss->is_dir_list = new_dirs;
    ss->num_files = new_count;
    ss->capacity_files = new_cap;
    ss->current_file_idx = new_idx;
    return true;
}

bool slideshow_start(slideshow_engine_t *ss, const waywal_slideshow_payload_t *payload)
{
    if (!ss || !payload || payload->path[0] == '\0')
        return false;

    bool explicit_sort =
        (payload->has_sort_mode != 0) || (payload->sort_mode > (uint8_t)WAYWAL_SORT_RANDOM &&
                                          payload->sort_mode <= (uint8_t)WAYWAL_SORT_MTIME);
    uint32_t new_sort = explicit_sort ? payload->sort_mode
                                      : (payload->random_order ? (uint8_t)WAYWAL_SORT_RANDOM
                                                               : (uint8_t)WAYWAL_SORT_ASCENDING);
    bool new_random =
        explicit_sort ? (new_sort == (uint8_t)WAYWAL_SORT_RANDOM) : (payload->random_order != 0);
    bool new_recursive = (payload->no_recursive == 0);

    char root_buf[4096];
    copy_cstr_trunc(root_buf, sizeof(root_buf), payload->path);

    char **new_files = NULL;
    bool *new_dirs = NULL;
    size_t new_count = 0;
    size_t new_cap = 0;
    if (!scan_and_build_list(root_buf, new_recursive, (waywal_sort_mode_t)new_sort, "", NULL,
                             &new_files, &new_dirs, &new_count, &new_cap, NULL)) {
        WAYWAL_LOG_ERR("Slideshow: No supported media files found in '%s'", payload->path);
        return false;
    }

    cancel_async_requests(ss, true);
    free_slide_arrays(ss->file_list, ss->is_dir_list, ss->num_files);

    memset(ss->root_path, 0, sizeof(ss->root_path));
    memcpy(ss->root_path, root_buf, strlen(root_buf));
    ss->recursive = new_recursive;
    ss->random_order = new_random;
    ss->has_sort_mode = explicit_sort;
    ss->sort_mode = new_sort;
    ss->file_list = new_files;
    ss->is_dir_list = new_dirs;
    ss->num_files = new_count;
    ss->capacity_files = new_cap;
    ss->current_file_idx = 0;
    ss->last_played_path[0] = '\0';

    ss->interval_s = payload->interval_s > 0 ? payload->interval_s : 300;
    ss->transition_type = payload->transition_type;
    ss->transition_duration_ms = payload->transition_duration_ms;
    ss->transition_fps = payload->transition_fps;
    ss->transition_angle_rad = payload->transition_angle_rad;
    ss->transition_wave_freq = payload->transition_wave_freq;
    ss->transition_wave_amp = payload->transition_wave_amp;
    ss->transition_center_x = payload->transition_center_x;
    ss->transition_center_y = payload->transition_center_y;
    ss->num_target_outputs = payload->num_target_outputs <= 16 ? payload->num_target_outputs : 16;
    memcpy(ss->target_outputs, payload->target_outputs, sizeof(ss->target_outputs));
    ss->scaling_mode = payload->scaling_mode;
    ss->filter_mode = payload->filter_mode;
    ss->bezier = payload->bezier;
    ss->has_custom_bezier = payload->has_custom_bezier;
    memcpy(ss->color, payload->color, sizeof(ss->color));
    ss->ten_bit = payload->ten_bit;
    ss->crop_gravity_x = payload->crop_gravity_x;
    ss->crop_gravity_y = payload->crop_gravity_y;
    ss->has_crop_gravity = payload->has_crop_gravity;
    ss->invert_y = payload->invert_y;
    ss->pos_y_inverted = payload->pos_y_inverted;
    ss->no_cache = (payload->no_cache != 0);
    ss->extract_theme = (payload->extract_theme != 0);
    ss->no_theme = (payload->no_theme != 0);
    ss->has_theme_mode = (payload->has_theme_mode != 0);
    ss->theme_light_mode = (payload->theme_light_mode != 0);
    ss->has_source_color_index = (payload->has_source_color_index != 0);
    ss->source_color_index = payload->source_color_index;
    ss->has_seed_color = (payload->has_seed_color != 0);
    ss->seed_color = payload->seed_color;
    ss->auto_day_night = payload->auto_day_night;

    ss->active = true;
    ss->paused = false;
    ss->current_file_idx = 0;

    WAYWAL_LOG_INFO(
        "Slideshow started with %zu media items (interval: %us, sort: %u, transition: %u)",
        ss->num_files, ss->interval_s, (unsigned)ss->sort_mode, ss->transition_type);

    if (request_or_present_slide(ss, +1, false, 0u) != SLIDE_PRESENT_ASYNC_PENDING) {
        arm_timer(ss);
    }
    return true;
}

void slideshow_stop(slideshow_engine_t *ss)
{
    if (!ss || !ss->active)
        return;

    cancel_async_requests(ss, true);
    disarm_timer(ss);
    ss->active = false;
    ss->paused = false;
    WAYWAL_LOG_INFO("Slideshow stopped");
}

void slideshow_pause(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->paused)
        return;

    bool rearm_done = false;
    if (ss->async) {
        pthread_mutex_lock(&ss->async->mu);
        rearm_done = ss->async->has_done;
        pthread_mutex_unlock(&ss->async->mu);
    }
    disarm_timer(ss);
    if (rearm_done) {
        trigger_timer_async_wakeup(ss->timer_fd);
    }
    ss->paused = true;
    WAYWAL_LOG_INFO("Slideshow paused");
}

void slideshow_resume(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || !ss->paused)
        return;

    ss->paused = false;
    bool async_pending = false;
    if (ss->async) {
        pthread_mutex_lock(&ss->async->mu);
        async_pending = ss->async->req_pending || ss->async->has_done;
        pthread_mutex_unlock(&ss->async->mu);
    }
    if (!async_pending) {
        arm_timer(ss);
    }
    WAYWAL_LOG_INFO("Slideshow resumed");
}

void slideshow_toggle(slideshow_engine_t *ss)
{
    if (!ss || !ss->active)
        return;
    if (ss->paused)
        slideshow_resume(ss);
    else
        slideshow_pause(ss);
}

void slideshow_next(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->num_files == 0)
        return;

    advance_slideshow_index_dir(ss, +1);
    if (request_or_present_slide(ss, +1, false, 0u) != SLIDE_PRESENT_ASYNC_PENDING && !ss->paused) {
        arm_timer(ss);
    }
}

void slideshow_prev(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->num_files == 0)
        return;

    advance_slideshow_index_dir(ss, -1);
    if (request_or_present_slide(ss, -1, false, 0u) != SLIDE_PRESENT_ASYNC_PENDING && !ss->paused) {
        arm_timer(ss);
    }
}

void slideshow_dispatch_tick(slideshow_engine_t *ss)
{
    if (!ss || ss->timer_fd < 0)
        return;

    uint64_t expirations = 0;
    ssize_t s = read(ss->timer_fd, &expirations, sizeof(expirations));
    if (s <= 0 || expirations == 0)
        return;

    if (!ss->active || ss->num_files == 0)
        return;

    if (ss->async) {
        bool has_done = false;
        uint64_t done_gen = 0;
        uint64_t cur_gen = 0;
        bool req_pending = false;
        char done_path[4096];
        bool done_ok = false;
        uint8_t *done_pixels = NULL;
        uint32_t done_w = 0, done_h = 0;
        int done_dir = 1;
        bool done_from_tick = false;
        size_t done_fail_streak = 0;

        pthread_mutex_lock(&ss->async->mu);
        cur_gen = ss->async->req_gen;
        req_pending = ss->async->req_pending;
        if (ss->async->has_done) {
            has_done = true;
            done_gen = ss->async->done_gen;
            copy_cstr_trunc(done_path, sizeof(done_path), ss->async->done_path);
            done_ok = ss->async->done_ok;
            done_pixels = ss->async->done_pixels;
            done_w = ss->async->done_w;
            done_h = ss->async->done_h;
            done_dir = ss->async->done_dir;
            done_from_tick = ss->async->done_from_tick;
            done_fail_streak = ss->async->done_fail_streak;
            ss->async->done_pixels = NULL;
            ss->async->has_done = false;
        }
        pthread_mutex_unlock(&ss->async->mu);

        if (has_done) {
            if (done_gen != cur_gen) {
                image_free(done_pixels);
                return;
            }
            bool presented_ok = false;
            if (done_ok) {
                presented_ok = present_decoded_image(ss, done_path, done_pixels, done_w, done_h);
            } else {
                image_free(done_pixels);
                WAYWAL_LOG_WARN("Slideshow: Failed to load image '%s', skipping", done_path);
            }
            if (presented_ok) {
                if (!ss->paused) {
                    arm_timer(ss);
                }
                schedule_prefetch_next(ss);
                if (done_from_tick && ss->daemon_state && daemon_broadcast_event) {
                    daemon_broadcast_event(ss->daemon_state, "wallpaper_changed");
                }
                return;
            }
            size_t next_streak = done_fail_streak + 1u;
            if (next_streak >= ss->num_files) {
                WAYWAL_LOG_WARN("Slideshow: All %zu candidate items failed to load", ss->num_files);
                if (!ss->paused) {
                    arm_timer(ss);
                }
                return;
            }
            advance_slideshow_index_dir(ss, done_dir);
            slide_present_res_t res =
                request_or_present_slide(ss, done_dir, done_from_tick, next_streak);
            if (res != SLIDE_PRESENT_ASYNC_PENDING) {
                if (!ss->paused) {
                    arm_timer(ss);
                }
                if (done_from_tick && res == SLIDE_PRESENT_SYNC_OK && ss->daemon_state &&
                    daemon_broadcast_event) {
                    daemon_broadcast_event(ss->daemon_state, "wallpaper_changed");
                }
            }
            return;
        }

        if (req_pending) {
            return;
        }
    }

    if (ss->paused)
        return;

    advance_slideshow_index_dir(ss, +1);
    slide_present_res_t res = request_or_present_slide(ss, +1, true, 0u);
    if (res == SLIDE_PRESENT_SYNC_OK && ss->daemon_state && daemon_broadcast_event) {
        daemon_broadcast_event(ss->daemon_state, "wallpaper_changed");
    }
}

bool slideshow_control(slideshow_engine_t *ss, uint32_t action)
{
    if (!ss)
        return false;
    switch (action) {
    case WAYWAL_SLIDESHOW_STOP:
        slideshow_stop(ss);
        return true;
    case WAYWAL_SLIDESHOW_PAUSE:
        if (!ss->active)
            return false;
        slideshow_pause(ss);
        return true;
    case WAYWAL_SLIDESHOW_RESUME:
        if (!ss->active)
            return false;
        slideshow_resume(ss);
        return true;
    case WAYWAL_SLIDESHOW_TOGGLE:
        if (!ss->active)
            return false;
        slideshow_toggle(ss);
        return true;
    case WAYWAL_SLIDESHOW_NEXT:
        if (!ss->active || ss->num_files == 0)
            return false;
        slideshow_next(ss);
        return true;
    case WAYWAL_SLIDESHOW_PREV:
        if (!ss->active || ss->num_files == 0)
            return false;
        slideshow_prev(ss);
        return true;
    default:
        return false;
    }
}
