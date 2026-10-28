// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/slideshow.h"

#include "wayland_core.h"
#include "waywal/image_loader.h"
#include "waywal/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static bool is_workshop_directory(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0')
        return false;
    char probe[4096];
    struct stat st;
    if (snprintf(probe, sizeof(probe), "%s/project.json", dir_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/scene.pkg", dir_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/gifscene.pkg", dir_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/scene.json", dir_path) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    return false;
}

static void add_file_to_list(slideshow_engine_t *ss, const char *filepath)
{
    if (!ss || !filepath)
        return;
    if (ss->num_files >= ss->capacity_files) {
        size_t new_cap = ss->capacity_files == 0 ? 32 : ss->capacity_files * 2;
        char **new_list = (char **)realloc(ss->file_list, new_cap * sizeof(char *));
        if (!new_list)
            return;
        ss->file_list = new_list;
        ss->capacity_files = new_cap;
    }
    char *dup = strdup(filepath);
    if (!dup)
        return;
    ss->file_list[ss->num_files++] = dup;
}

static void scan_path_recursive_internal(slideshow_engine_t *ss, const char *path, int depth)
{
    if (depth > 16)
        return;

    struct stat st;
    if (stat(path, &st) < 0)
        return;

    if (S_ISREG(st.st_mode)) {
        if (has_supported_media_extension(path)) {
            add_file_to_list(ss, path);
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (is_workshop_directory(path)) {
            add_file_to_list(ss, path);
            return;
        }

        DIR *dir = opendir(path);
        if (!dir)
            return;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;

            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

            struct stat child_st;
            if (stat(full_path, &child_st) == 0) {
                if (S_ISDIR(child_st.st_mode)) {
                    if (is_workshop_directory(full_path)) {
                        add_file_to_list(ss, full_path);
                    } else if (ss->recursive) {
                        scan_path_recursive_internal(ss, full_path, depth + 1);
                    }
                } else if (S_ISREG(child_st.st_mode) &&
                           has_supported_media_extension(ent->d_name)) {
                    add_file_to_list(ss, full_path);
                }
            }
        }
        closedir(dir);
    }
}

static void scan_path_recursive(slideshow_engine_t *ss, const char *path)
{
    scan_path_recursive_internal(ss, path, 0);
}

static int cmp_path_asc(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return strcmp(pa, pb);
}

static int cmp_path_desc(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return strcmp(pb, pa);
}

static int cmp_path_mtime(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    struct stat sa, sb;
    bool ok_a = (stat(pa, &sa) == 0);
    bool ok_b = (stat(pb, &sb) == 0);
    time_t sec_a = ok_a ? sa.st_mtim.tv_sec : 0;
    time_t sec_b = ok_b ? sb.st_mtim.tv_sec : 0;
    if (sec_a != sec_b) {
        return (sec_a > sec_b) ? -1 : 1;
    }
    long nsec_a = ok_a ? sa.st_mtim.tv_nsec : 0;
    long nsec_b = ok_b ? sb.st_mtim.tv_nsec : 0;
    if (nsec_a != nsec_b) {
        return (nsec_a > nsec_b) ? -1 : 1;
    }
    return strcmp(pa, pb);
}

static void shuffle_files(slideshow_engine_t *ss)
{
    if (!ss || ss->num_files < 2)
        return;
    for (size_t i = ss->num_files - 1; i > 0; --i) {
        size_t j = (size_t)rand() % (i + 1);
        char *tmp = ss->file_list[i];
        ss->file_list[i] = ss->file_list[j];
        ss->file_list[j] = tmp;
    }
    if (ss->last_played_path[0] != '\0' && ss->file_list[0] &&
        strcmp(ss->file_list[0], ss->last_played_path) == 0) {
        size_t swap_idx = 1 + ((size_t)rand() % (ss->num_files - 1));
        char *tmp = ss->file_list[0];
        ss->file_list[0] = ss->file_list[swap_idx];
        ss->file_list[swap_idx] = tmp;
    }
}

static void apply_sort_or_shuffle(slideshow_engine_t *ss)
{
    if (!ss || ss->num_files < 2)
        return;
    waywal_sort_mode_t mode = ss->has_sort_mode
                                  ? (waywal_sort_mode_t)ss->sort_mode
                                  : (ss->random_order ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING);
    switch (mode) {
    case WAYWAL_SORT_RANDOM:
        qsort(ss->file_list, ss->num_files, sizeof(char *), cmp_path_asc);
        shuffle_files(ss);
        break;
    case WAYWAL_SORT_DESCENDING:
        qsort(ss->file_list, ss->num_files, sizeof(char *), cmp_path_desc);
        break;
    case WAYWAL_SORT_MTIME:
        qsort(ss->file_list, ss->num_files, sizeof(char *), cmp_path_mtime);
        break;
    case WAYWAL_SORT_ASCENDING:
    default:
        qsort(ss->file_list, ss->num_files, sizeof(char *), cmp_path_asc);
        break;
    }
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

static void display_current_image(slideshow_engine_t *ss)
{
    if (!ss || ss->num_files == 0)
        return;

    const char *filepath = ss->file_list[ss->current_file_idx];
    if (!filepath)
        return;

    strncpy(ss->last_played_path, filepath, sizeof(ss->last_played_path) - 1);
    ss->last_played_path[sizeof(ss->last_played_path) - 1] = '\0';

    if (!ss->daemon_state)
        return;

    uint32_t trans_type = ss->transition_type;
    if (trans_type == WAYWAL_TRANSITION_RANDOM) {
        /* Pick random animated transition between SIMPLE (1) and PAGE_CURL (19) */
        trans_type = 1 + (uint32_t)(rand() % 19);
    }

    daemon_state_t *st = ss->daemon_state;
    struct stat item_st;
    bool is_dir = (stat(filepath, &item_st) == 0 && S_ISDIR(item_st.st_mode));
    if (is_dir || has_video_extension(filepath) || has_shader_extension(filepath)) {
        if (daemon_slideshow_present_media) {
            WAYWAL_LOG_INFO("Slideshow [%zu/%zu]: presenting live media '%s' (transition: %u)",
                            ss->current_file_idx + 1, ss->num_files, filepath, trans_type);
            if (daemon_slideshow_present_media(st, ss, filepath, trans_type) &&
                st->config.hooks.on_wallpaper_change[0] != '\0') {
                (void)waywal_palette_run_hook(st->config.hooks.on_wallpaper_change);
            }
        }
        return;
    }

    uint8_t *pixels = NULL;
    uint32_t img_w = 0, img_h = 0;

    if (!image_load(filepath, &pixels, &img_w, &img_h)) {
        WAYWAL_LOG_WARN("Slideshow: Failed to load image '%s', skipping", filepath);
        return;
    }

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
    strncpy(meta->filepath, filepath, sizeof(meta->filepath) - 1);

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
        if (st->config.hooks.on_wallpaper_change[0] != '\0') {
            (void)waywal_palette_run_hook(st->config.hooks.on_wallpaper_change);
        }
        if (try_start_native_animation) {
            const char *first_target = (meta->num_target_outputs > 0) ? extra_ptr : "*";
            (void)try_start_native_animation(st, filepath, meta, first_target);
        }
    }
}

bool slideshow_init(slideshow_engine_t *ss, struct daemon_state *state)
{
    if (!ss)
        return false;

    static bool s_rng_seeded = false;
    if (!s_rng_seeded) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        srand((unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ (long)getpid()));
        s_rng_seeded = true;
    }

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

void slideshow_destroy(slideshow_engine_t *ss)
{
    if (!ss)
        return;

    slideshow_stop(ss);

    if (ss->timer_fd >= 0) {
        close(ss->timer_fd);
        ss->timer_fd = -1;
    }

    for (size_t i = 0; i < ss->num_files; ++i) {
        free(ss->file_list[i]);
    }
    free(ss->file_list);
    ss->file_list = NULL;
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
        strncpy(prev_path, ss->file_list[ss->current_file_idx], sizeof(prev_path) - 1);
        prev_path[sizeof(prev_path) - 1] = '\0';
        if (ss->last_played_path[0] == '\0') {
            strncpy(ss->last_played_path, prev_path, sizeof(ss->last_played_path) - 1);
            ss->last_played_path[sizeof(ss->last_played_path) - 1] = '\0';
        }
    }

    for (size_t i = 0; i < ss->num_files; ++i) {
        free(ss->file_list[i]);
    }
    free(ss->file_list);
    ss->file_list = NULL;
    ss->num_files = 0;
    ss->capacity_files = 0;

    scan_path_recursive(ss, ss->root_path);
    if (ss->num_files == 0) {
        ss->current_file_idx = 0;
        return false;
    }

    apply_sort_or_shuffle(ss);

    ss->current_file_idx = 0;
    waywal_sort_mode_t mode = ss->has_sort_mode
                                  ? (waywal_sort_mode_t)ss->sort_mode
                                  : (ss->random_order ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING);
    if (mode != WAYWAL_SORT_RANDOM && prev_path[0] != '\0') {
        for (size_t i = 0; i < ss->num_files; ++i) {
            if (ss->file_list[i] && strcmp(ss->file_list[i], prev_path) == 0) {
                ss->current_file_idx = i;
                break;
            }
        }
    }
    return true;
}

bool slideshow_start(slideshow_engine_t *ss, const waywal_slideshow_payload_t *payload)
{
    if (!ss || !payload)
        return false;

    memset(ss->root_path, 0, sizeof(ss->root_path));
    strncpy(ss->root_path, payload->path, sizeof(ss->root_path) - 1);
    ss->recursive = (payload->no_recursive == 0);
    ss->random_order = payload->random_order;
    bool explicit_sort =
        (payload->has_sort_mode != 0) || (payload->sort_mode > (uint8_t)WAYWAL_SORT_RANDOM &&
                                          payload->sort_mode <= (uint8_t)WAYWAL_SORT_MTIME);
    ss->has_sort_mode = explicit_sort;
    ss->sort_mode = explicit_sort ? payload->sort_mode
                                  : (payload->random_order ? (uint8_t)WAYWAL_SORT_RANDOM
                                                           : (uint8_t)WAYWAL_SORT_ASCENDING);
    if (ss->has_sort_mode) {
        ss->random_order = (ss->sort_mode == (uint8_t)WAYWAL_SORT_RANDOM);
    }
    ss->current_file_idx = 0;
    ss->last_played_path[0] = '\0';

    /* Clean up any existing file list before fresh start */
    for (size_t i = 0; i < ss->num_files; ++i) {
        free(ss->file_list[i]);
    }
    free(ss->file_list);
    ss->file_list = NULL;
    ss->num_files = 0;
    ss->capacity_files = 0;

    if (!slideshow_rescan(ss)) {
        WAYWAL_LOG_ERR("Slideshow: No supported media files found in '%s'", payload->path);
        slideshow_stop(ss);
        return false;
    }

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

    display_current_image(ss);
    arm_timer(ss);
    return true;
}

void slideshow_stop(slideshow_engine_t *ss)
{
    if (!ss || !ss->active)
        return;

    disarm_timer(ss);
    ss->active = false;
    ss->paused = false;
    WAYWAL_LOG_INFO("Slideshow stopped");
}

void slideshow_pause(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->paused)
        return;

    disarm_timer(ss);
    ss->paused = true;
    WAYWAL_LOG_INFO("Slideshow paused");
}

void slideshow_resume(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || !ss->paused)
        return;

    ss->paused = false;
    arm_timer(ss);
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

static void advance_slideshow_index(slideshow_engine_t *ss)
{
    if (!ss || ss->num_files == 0)
        return;
    if (ss->current_file_idx + 1 >= ss->num_files) {
        ss->current_file_idx = 0;
        waywal_sort_mode_t mode =
            ss->has_sort_mode ? (waywal_sort_mode_t)ss->sort_mode
                              : (ss->random_order ? WAYWAL_SORT_RANDOM : WAYWAL_SORT_ASCENDING);
        if (mode == WAYWAL_SORT_RANDOM && ss->num_files > 1) {
            shuffle_files(ss);
        }
    } else {
        ss->current_file_idx++;
    }
}

void slideshow_next(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->num_files == 0)
        return;

    advance_slideshow_index(ss);
    display_current_image(ss);
    if (!ss->paused)
        arm_timer(ss);
}

void slideshow_prev(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->num_files == 0)
        return;

    if (ss->current_file_idx == 0)
        ss->current_file_idx = ss->num_files - 1;
    else
        ss->current_file_idx--;

    display_current_image(ss);
    if (!ss->paused)
        arm_timer(ss);
}

void slideshow_dispatch_tick(slideshow_engine_t *ss)
{
    if (!ss || ss->timer_fd < 0)
        return;

    uint64_t expirations = 0;
    ssize_t s = read(ss->timer_fd, &expirations, sizeof(expirations));
    if (s <= 0 || expirations == 0)
        return;

    if (!ss->active || ss->paused || ss->num_files == 0)
        return;

    advance_slideshow_index(ss);
    display_current_image(ss);
    if (ss->daemon_state && daemon_broadcast_event) {
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
