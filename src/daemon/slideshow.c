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
            strcasecmp(dot, ".svg") == 0);
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
        if (has_image_extension(path)) {
            add_file_to_list(ss, path);
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir)
            return;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

            struct stat child_st;
            if (stat(full_path, &child_st) == 0) {
                if (S_ISDIR(child_st.st_mode)) {
                    scan_path_recursive_internal(ss, full_path, depth + 1);
                } else if (S_ISREG(child_st.st_mode) && has_image_extension(ent->d_name)) {
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
    if (!ss || ss->num_files == 0 || !ss->daemon_state)
        return;

    const char *filepath = ss->file_list[ss->current_file_idx];
    uint8_t *pixels = NULL;
    uint32_t img_w = 0, img_h = 0;

    if (!image_load(filepath, &pixels, &img_w, &img_h)) {
        WAYWAL_LOG_WARN("Slideshow: Failed to load image '%s', skipping", filepath);
        return;
    }

    uint32_t trans_type = ss->transition_type;
    if (trans_type == WAYWAL_TRANSITION_RANDOM) {
        /* Pick random animated transition between SIMPLE (1) and PAGE_CURL (19) */
        trans_type = 1 + (uint32_t)(rand() % 19);
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

    daemon_state_t *st = ss->daemon_state;
    if (st->on_wallpaper_loaded) {
        st->on_wallpaper_loaded(st, pixels, img_w, img_h, meta);
    } else if (img_w > 0 && img_h > 0) {
        uint32_t thumb_w = img_w;
        uint32_t thumb_h = img_h;
        if (thumb_w > 1920U || thumb_h > 1080U) {
            double scale_x = 1920.0 / (double)img_w;
            double scale_y = 1080.0 / (double)img_h;
            double scale = (scale_x < scale_y) ? scale_x : scale_y;
            thumb_w = (uint32_t)((double)img_w * scale + 0.5);
            thumb_h = (uint32_t)((double)img_h * scale + 0.5);
            if (thumb_w == 0U)
                thumb_w = 1U;
            if (thumb_h == 0U)
                thumb_h = 1U;
        }
        size_t byte_len = (size_t)thumb_w * (size_t)thumb_h * 4U;
        if (!st->wallpaper_rgba || st->wallpaper_w != thumb_w || st->wallpaper_h != thumb_h) {
            uint8_t *buf = (uint8_t *)malloc(byte_len);
            if (buf) {
                free(st->wallpaper_rgba);
                st->wallpaper_rgba = buf;
                st->wallpaper_w = thumb_w;
                st->wallpaper_h = thumb_h;
            }
        }
        if (st->wallpaper_rgba && st->wallpaper_w == thumb_w && st->wallpaper_h == thumb_h) {
            if (thumb_w == img_w && thumb_h == img_h) {
                memcpy(st->wallpaper_rgba, pixels, byte_len);
            } else {
                const uint32_t *src32 = (const uint32_t *)pixels;
                uint32_t *dst32 = (uint32_t *)st->wallpaper_rgba;
                for (uint32_t dy = 0; dy < thumb_h; ++dy) {
                    uint32_t sy0 = (uint32_t)(((uint64_t)dy * img_h) / thumb_h);
                    uint32_t sy1 = sy0 + 1U < img_h ? sy0 + 1U : sy0;
                    const uint32_t *srow0 = src32 + (size_t)sy0 * img_w;
                    const uint32_t *srow1 = src32 + (size_t)sy1 * img_w;
                    uint32_t *drow = dst32 + (size_t)dy * thumb_w;
                    for (uint32_t dx = 0; dx < thumb_w; ++dx) {
                        uint32_t sx0 = (uint32_t)(((uint64_t)dx * img_w) / thumb_w);
                        uint32_t sx1 = sx0 + 1U < img_w ? sx0 + 1U : sx0;
                        uint32_t p0 =
                            (((srow0[sx0] & 0xFEFEFEFEU) >> 1) + ((srow0[sx1] & 0xFEFEFEFEU) >> 1));
                        uint32_t p1 =
                            (((srow1[sx0] & 0xFEFEFEFEU) >> 1) + ((srow1[sx1] & 0xFEFEFEFEU) >> 1));
                        drow[dx] = ((p0 & 0xFEFEFEFEU) >> 1) + ((p1 & 0xFEFEFEFEU) >> 1);
                    }
                }
            }
        }
        waywal_scheme_variant_t variant =
            waywal_scheme_variant_from_str(st->config.theme.scheme_variant);
        if (waywal_palette_extract_ex(pixels, img_w, img_h, img_w * 4U, true,
                                      st->config.theme.theme_mode, variant,
                                      st->config.theme.contrast, &st->active_palette)) {
            st->saved_wallpaper_palette = st->active_palette;
            st->has_saved_wallpaper_palette = true;
            st->has_active_palette = true;
            if (st->config.theme.enabled) {
                (void)waywal_palette_export_filtered(
                    &st->active_palette, NULL, st->config.theme.caelestia_sync,
                    st->config.theme.export_css, st->config.theme.export_hyprland);
                if (st->config.theme.tty_sequences) {
                    (void)waywal_palette_broadcast_tty(&st->active_palette);
                }
                if (st->config.hooks.on_theme_change[0] != '\0') {
                    (void)waywal_palette_run_hook(st->config.hooks.on_theme_change);
                }
            }
        }
    }

    bool ok = daemon_start_image_transition(st, pixels, img_w, img_h, meta);
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

    if (ss->random_order) {
        shuffle_files(ss);
    }

    ss->current_file_idx = 0;
    if (prev_path[0] != '\0') {
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
    ss->random_order = payload->random_order;
    ss->current_file_idx = 0;

    /* Clean up any existing file list before fresh start */
    for (size_t i = 0; i < ss->num_files; ++i) {
        free(ss->file_list[i]);
    }
    free(ss->file_list);
    ss->file_list = NULL;
    ss->num_files = 0;
    ss->capacity_files = 0;

    if (!slideshow_rescan(ss)) {
        WAYWAL_LOG_ERR("Slideshow: No supported image files found in '%s'", payload->path);
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

    ss->active = true;
    ss->paused = false;
    ss->current_file_idx = 0;

    WAYWAL_LOG_INFO("Slideshow started with %zu images (interval: %us, transition: %u)",
                    ss->num_files, ss->interval_s, ss->transition_type);

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

void slideshow_next(slideshow_engine_t *ss)
{
    if (!ss || !ss->active || ss->num_files == 0)
        return;

    ss->current_file_idx = (ss->current_file_idx + 1) % ss->num_files;
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

    ss->current_file_idx = (ss->current_file_idx + 1) % ss->num_files;
    display_current_image(ss);
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
        slideshow_pause(ss);
        return true;
    case WAYWAL_SLIDESHOW_RESUME:
        slideshow_resume(ss);
        return true;
    case WAYWAL_SLIDESHOW_TOGGLE:
        slideshow_toggle(ss);
        return true;
    case WAYWAL_SLIDESHOW_NEXT:
        slideshow_next(ss);
        return true;
    case WAYWAL_SLIDESHOW_PREV:
        slideshow_prev(ss);
        return true;
    default:
        return false;
    }
}
