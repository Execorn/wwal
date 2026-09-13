#include "waywal/slideshow.h"

#include "image_loader.h"
#include "wayland_core.h"
#include "waywal/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

static bool has_image_extension(const char *name)
{
    if (!name)
        return false;
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".bmp") == 0);
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
    ss->file_list[ss->num_files++] = strdup(filepath);
}

static void scan_path_recursive(slideshow_engine_t *ss, const char *path)
{
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
                    scan_path_recursive(ss, full_path);
                } else if (S_ISREG(child_st.st_mode) && has_image_extension(ent->d_name)) {
                    add_file_to_list(ss, full_path);
                }
            }
        }
        closedir(dir);
    }
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
        /* Pick random animated transition between FADE (2) and PAGE_CURL (19) */
        trans_type = 2 + (uint32_t)(rand() % 18);
    }

    waywal_img_metadata_t meta = {
        .width = img_w,
        .height = img_h,
        .pixel_format = 0,
        .transition_type = trans_type,
        .transition_duration_ms =
            ss->transition_duration_ms > 0 ? ss->transition_duration_ms : 1000,
        .transition_fps = ss->transition_fps > 0 ? ss->transition_fps : 60,
        .transition_angle_rad = ss->transition_angle_rad,
        .transition_wave_freq = ss->transition_wave_freq > 0.0f ? ss->transition_wave_freq : 20.0f,
        .transition_wave_amp = ss->transition_wave_amp > 0.0f ? ss->transition_wave_amp : 0.05f,
        .transition_center_x = ss->transition_center_x,
        .transition_center_y = ss->transition_center_y,
        .num_target_outputs = ss->num_target_outputs,
        .sync_mode = 0,
        .stagger_delay_ms = 0,
        .custom_shader_len = 0,
    };

    WAYWAL_LOG_INFO("Slideshow [%zu/%zu]: displaying '%s' (transition: %u)",
                    ss->current_file_idx + 1, ss->num_files, filepath, trans_type);

    daemon_start_image_transition(ss->daemon_state, pixels, img_w, img_h, &meta);
    image_free(pixels);
}

bool slideshow_init(slideshow_engine_t *ss, struct daemon_state *state)
{
    if (!ss)
        return false;

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

bool slideshow_start(slideshow_engine_t *ss, const waywal_slideshow_payload_t *payload)
{
    if (!ss || !payload)
        return false;

    /* Clean up any existing file list */
    for (size_t i = 0; i < ss->num_files; ++i) {
        free(ss->file_list[i]);
    }
    free(ss->file_list);
    ss->file_list = NULL;
    ss->num_files = 0;
    ss->capacity_files = 0;

    scan_path_recursive(ss, payload->path);
    if (ss->num_files == 0) {
        WAYWAL_LOG_ERR("Slideshow: No supported image files found in '%s'", payload->path);
        return false;
    }

    if (payload->random_order) {
        shuffle_files(ss);
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
    ss->random_order = payload->random_order;
    ss->num_target_outputs = payload->num_target_outputs;

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
    if (!ss || !ss->active || ss->paused)
        return;

    uint64_t expirations = 0;
    ssize_t s = read(ss->timer_fd, &expirations, sizeof(expirations));
    (void)s;

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
