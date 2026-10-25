#include "wayland_core.h"
#include "gpu_compute.h"
#include "image_loader.h"
#include "waywal/anim_engine.h"
#include "waywal/arena.h"
#include "waywal/audio_fft.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/power_governor.h"
#include "waywal/security.h"
#include "waywal/shader_engine.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"
#include "waywal/uring_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static waywal_state_store_t g_saved_state;
static int g_registered_shader_inotify_fd = -1;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void drain_timer_fd(int timer_fd)
{
    if (timer_fd < 0)
        return;
    uint64_t dummy;
    while (read(timer_fd, &dummy, sizeof(dummy)) > 0) {
    }
}

static void disarm_timer_fd(int timer_fd)
{
    if (timer_fd < 0)
        return;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    (void)timerfd_settime(timer_fd, 0, &its, NULL);
    drain_timer_fd(timer_fd);
}

static void arm_periodic_timer_fd(int timer_fd, uint32_t fps)
{
    if (timer_fd < 0)
        return;
    if (fps == 0) {
        disarm_timer_fd(timer_fd);
        return;
    }
    uint64_t interval_ns = 1000000000ULL / (uint64_t)fps;
    if (interval_ns == 0)
        interval_ns = 1000000ULL;
    struct itimerspec its = {
        .it_interval =
            {
                .tv_sec = (time_t)(interval_ns / 1000000000ULL),
                .tv_nsec = (long)(interval_ns % 1000000000ULL),
            },
        .it_value =
            {
                .tv_sec = (time_t)(interval_ns / 1000000000ULL),
                .tv_nsec = (long)(interval_ns % 1000000000ULL),
            },
    };
    drain_timer_fd(timer_fd);
    (void)timerfd_settime(timer_fd, 0, &its, NULL);
}

static void arm_oneshot_timer_ms(int timer_fd, uint32_t delay_ms)
{
    if (timer_fd < 0)
        return;
    if (delay_ms == 0)
        delay_ms = 100;
    uint64_t ns = (uint64_t)delay_ms * 1000000ULL;
    struct itimerspec its = {
        .it_interval = {.tv_sec = 0, .tv_nsec = 0},
        .it_value =
            {
                .tv_sec = (time_t)(ns / 1000000000ULL),
                .tv_nsec = (long)(ns % 1000000000ULL),
            },
    };
    drain_timer_fd(timer_fd);
    (void)timerfd_settime(timer_fd, 0, &its, NULL);
}

static void shader_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    state->shader_active = false;
    disarm_timer_fd(state->shader_timer_fd);
}

static void anim_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    state->anim_active = false;
    disarm_timer_fd(state->anim_timer_fd);
    if (state->anim_dec) {
        waywal_anim_destroy(state->anim_dec);
        state->anim_dec = NULL;
    }
    if (state->anim_buf) {
        free(state->anim_buf);
        state->anim_buf = NULL;
        state->anim_buf_size = 0;
    }
}

static bool has_anim_candidate_extension(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return (strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".webp") == 0 ||
            strcasecmp(dot, ".apng") == 0 || strcasecmp(dot, ".png") == 0);
}

static bool read_file_bytes(const char *path, uint8_t **out_buf, size_t *out_size)
{
    if (!path || !out_buf || !out_size)
        return false;
    *out_buf = NULL;
    *out_size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 128L * 1024L * 1024L) {
        close(fd);
        return false;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        close(fd);
        return false;
    }
    size_t total = 0;
    while (total < sz) {
        ssize_t r = read(fd, buf + total, sz - total);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return false;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    close(fd);
    if (total != sz) {
        free(buf);
        return false;
    }
    *out_buf = buf;
    *out_size = sz;
    return true;
}

static bool validate_shader_file_headless(const char *filepath)
{
    uint8_t *buf = NULL;
    size_t sz = 0;
    if (!read_file_bytes(filepath, &buf, &sz))
        return false;
    char *src = (char *)malloc(sz + 1);
    if (!src) {
        free(buf);
        return false;
    }
    memcpy(src, buf, sz);
    src[sz] = '\0';
    free(buf);

    bool has_entry =
        (strstr(src, "mainImage") != NULL || strstr(src, "void main") != NULL);
    bool valid_syntax = true;
    /* Check balanced braces and reject obvious syntax corruption */
    int brace_depth = 0;
    for (size_t i = 0; i < sz; ++i) {
        if (src[i] == '{')
            brace_depth++;
        else if (src[i] == '}') {
            brace_depth--;
            if (brace_depth < 0) {
                valid_syntax = false;
                break;
            }
        }
    }
    if (brace_depth != 0)
        valid_syntax = false;

    char *wrapped = (has_entry && valid_syntax) ? waywal_shader_wrap_source(src) : NULL;
    free(src);
    if (!wrapped)
        return false;
    free(wrapped);
    return true;
}

static bool try_start_native_animation(daemon_state_t *state, const char *filepath,
                                       const waywal_img_metadata_t *meta, const char *target_out)
{
    if (!state || !filepath || !has_anim_candidate_extension(filepath))
        return false;

    uint8_t *buf = NULL;
    size_t sz = 0;
    if (!read_file_bytes(filepath, &buf, &sz))
        return false;

    if (!waywal_anim_probe(buf, sz)) {
        free(buf);
        return false;
    }

    waywal_anim_decoder_t *dec = waywal_anim_create(buf, sz);
    if (!dec || waywal_anim_frame_count(dec) <= 1) {
        if (dec)
            waywal_anim_destroy(dec);
        free(buf);
        return false;
    }

    const uint8_t *f_rgba = NULL;
    uint32_t fw = 0, fh = 0, delay_ms = 100;
    if (!waywal_anim_next_frame(dec, &f_rgba, &fw, &fh, &delay_ms)) {
        waywal_anim_destroy(dec);
        free(buf);
        return false;
    }

    anim_engine_stop_playback(state);
    state->anim_dec = dec;
    state->anim_buf = buf;
    state->anim_buf_size = sz;
    state->anim_active = true;
    if (meta) {
        state->anim_meta = *meta;
    } else {
        memset(&state->anim_meta, 0, sizeof(state->anim_meta));
    }
    state->anim_target_output[0] = '\0';
    if (target_out && target_out[0] != '\0') {
        strncpy(state->anim_target_output, target_out, sizeof(state->anim_target_output) - 1);
    }

    if (!state->power_gov || !waywal_power_governor_should_pause_all(state->power_gov)) {
        arm_oneshot_timer_ms(state->anim_timer_fd, delay_ms > 0 ? delay_ms : 100);
    }
    WAYWAL_LOG_INFO("Started native animation playback for '%s' (%u frames)", filepath,
                    waywal_anim_frame_count(dec));
    return true;
}

static void daemon_anim_dispatch_frame(daemon_state_t *state)
{
    if (!state || state->anim_timer_fd < 0)
        return;
    drain_timer_fd(state->anim_timer_fd);
    if (!state->anim_active || !state->anim_dec)
        return;

    if (state->power_gov && waywal_power_governor_should_pause_all(state->power_gov)) {
        disarm_timer_fd(state->anim_timer_fd);
        return;
    }

    const uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0, delay_ms = 100;
    if (!waywal_anim_next_frame(state->anim_dec, &rgba, &w, &h, &delay_ms) || !rgba || w == 0 ||
        h == 0) {
        return;
    }

    size_t px_bytes = (size_t)w * (size_t)h * 4U;
    uint8_t *bgra = (uint8_t *)arena_alloc(&state->frame_arena, px_bytes, 32U);
    bool heap_bgra = false;
    if (!bgra) {
        bgra = (uint8_t *)malloc(px_bytes);
        if (!bgra)
            return;
        heap_bgra = true;
    }

    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; ++i) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (state->anim_target_output[0] != '\0' &&
            strcmp(state->anim_target_output, "*") != 0 &&
            strcmp(state->anim_target_output, out->name) != 0) {
            continue;
        }
        if (state->power_gov &&
            waywal_power_governor_should_pause_output(state->power_gov, out->name)) {
            continue;
        }
        output_node_render_image(state, out, bgra, w, h);
    }

    if (heap_bgra)
        free(bgra);

    uint32_t eff_fps = state->power_gov
                           ? waywal_power_governor_effective_fps(
                                 state->power_gov,
                                 state->anim_target_output[0] ? state->anim_target_output : "*",
                                 delay_ms > 0 ? (1000U / delay_ms) : 10U)
                           : (delay_ms > 0 ? (1000U / delay_ms) : 10U);
    if (eff_fps == 0) {
        disarm_timer_fd(state->anim_timer_fd);
    } else {
        uint32_t next_delay = delay_ms > 0 ? delay_ms : 100U;
        if (state->power_gov && waywal_power_governor_is_on_battery(state->power_gov) &&
            next_delay < 33U) {
            next_delay = 33U;
        }
        arm_oneshot_timer_ms(state->anim_timer_fd, next_delay);
    }
}

static void daemon_shader_render_once(daemon_state_t *state)
{
    if (!state || !state->shader_eng || !state->render_engine.gpu_compute_active)
        return;

    uint64_t t_now = now_ns();
    if (state->shader_start_ns == 0) {
        state->shader_start_ns = t_now;
        state->shader_last_ns = t_now;
    }

    waywal_shader_frame_params_t fparams;
    memset(&fparams, 0, sizeof(fparams));
    fparams.time_sec = (float)(t_now - state->shader_start_ns) * 1e-9f;
    float dt = (float)(t_now - state->shader_last_ns) * 1e-9f;
    if (dt <= 0.0f) {
        dt = 1.0f / (float)(state->shader_fps > 0 ? state->shader_fps : 60);
    }
    fparams.time_delta_sec = dt;
    fparams.frame_index = (int32_t)(state->shader_frame_idx++);
    state->shader_last_ns = t_now;

    time_t wall_sec = time(NULL);
    struct tm tm_val;
    if (localtime_r(&wall_sec, &tm_val)) {
        fparams.date[0] = (float)(tm_val.tm_year + 1900);
        fparams.date[1] = (float)tm_val.tm_mon;
        fparams.date[2] = (float)tm_val.tm_mday;
        fparams.date[3] =
            (float)(tm_val.tm_hour * 3600 + tm_val.tm_min * 60 + tm_val.tm_sec);
    }

    if (state->audio_fft) {
        (void)waywal_audio_fft_process(state->audio_fft, NULL);
        fparams.audio = waywal_audio_fft_get_frame(state->audio_fft);
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->surface || !out->configured || out->width <= 0 || out->height <= 0)
            continue;
        if (state->shader_target_output[0] != '\0' &&
            strcmp(state->shader_target_output, "*") != 0 &&
            strcmp(state->shader_target_output, out->name) != 0) {
            continue;
        }
        if (state->power_gov &&
            waywal_power_governor_should_pause_output(state->power_gov, out->name)) {
            continue;
        }

        double mx = 0.0, my = 0.0;
        if (daemon_query_cursor_pos(state, out, &mx, &my)) {
            fparams.mouse[0] = (float)mx;
            fparams.mouse[1] = (float)my;
        }

        if (layer_shell_ensure_dmabuf_ring(state, out, out->width, out->height) &&
            out->use_dmabuf) {
            dmabuf_bo_t *bo = dmabuf_ring_acquire(&out->dmabuf_ring);
            if (bo && bo->wl_buffer) {
                if (waywal_shader_engine_render_frame(state->shader_eng, bo, &fparams)) {
                    if (out->viewport) {
                        int32_t dst_w =
                            out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
                        int32_t dst_h =
                            out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
                        wp_viewport_set_destination(out->viewport, dst_w, dst_h);
                        wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1));
                    }
                    wl_surface_attach(out->surface, bo->wl_buffer, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                    wl_surface_commit(out->surface);
                    wl_display_flush(state->display);
                } else {
                    bo->in_use = false;
                }
            }
        }
    }

    if (state->render_engine.gpu_ctx) {
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    }
}

static void daemon_shader_dispatch_frame(daemon_state_t *state)
{
    if (!state || state->shader_timer_fd < 0)
        return;
    drain_timer_fd(state->shader_timer_fd);
    if (!state->shader_active || !state->shader_eng)
        return;
    if (state->power_gov && waywal_power_governor_should_pause_all(state->power_gov)) {
        disarm_timer_fd(state->shader_timer_fd);
        return;
    }
    daemon_shader_render_once(state);
}

static void daemon_reconcile_power_state(daemon_state_t *state)
{
    if (!state || !state->power_gov)
        return;

    bool pause_all = waywal_power_governor_should_pause_all(state->power_gov);

    /* Reconcile hardware video engine timer */
    if (state->video_engine.state == WAYWAL_VIDEO_STATE_PLAYING &&
        state->video_engine.timer_fd >= 0) {
        const char *v_out =
            (state->video_engine.target_output && state->video_engine.target_output->name[0])
                ? state->video_engine.target_output->name
                : "*";
        uint32_t target_v_fps = 60U;
        if (state->video_engine.demuxer && state->video_engine.demuxer->fps_den > 0 &&
            state->video_engine.demuxer->fps_num > 0) {
            target_v_fps =
                state->video_engine.demuxer->fps_num / state->video_engine.demuxer->fps_den;
            if (target_v_fps == 0)
                target_v_fps = 30U;
        }
        uint32_t eff_v_fps =
            pause_all ? 0U
                      : waywal_power_governor_effective_fps(state->power_gov, v_out, target_v_fps);
        if (eff_v_fps == 0) {
            disarm_timer_fd(state->video_engine.timer_fd);
        } else {
            arm_oneshot_timer_ms(state->video_engine.timer_fd, 1000U / eff_v_fps);
        }
    }

    /* Reconcile live shader engine timer */
    if (state->shader_active && state->shader_timer_fd >= 0) {
        const char *s_out =
            state->shader_target_output[0] ? state->shader_target_output : "*";
        uint32_t target_s_fps = state->shader_fps > 0 ? state->shader_fps : 60U;
        uint32_t eff_s_fps =
            pause_all ? 0U
                      : waywal_power_governor_effective_fps(state->power_gov, s_out, target_s_fps);
        if (eff_s_fps == 0) {
            disarm_timer_fd(state->shader_timer_fd);
        } else {
            arm_periodic_timer_fd(state->shader_timer_fd, eff_s_fps);
        }
    }

    /* Reconcile native animation engine timer */
    if (state->anim_active && state->anim_timer_fd >= 0) {
        const char *a_out = state->anim_target_output[0] ? state->anim_target_output : "*";
        uint32_t eff_a_fps =
            pause_all ? 0U : waywal_power_governor_effective_fps(state->power_gov, a_out, 10U);
        if (eff_a_fps == 0) {
            disarm_timer_fd(state->anim_timer_fd);
        } else {
            arm_oneshot_timer_ms(state->anim_timer_fd, 100U);
        }
    }
}

static void register_fd_in_loops(daemon_state_t *state, int fd, uring_event_type_t ev_type)
{
    if (!state || fd < 0)
        return;
    if (state->epoll_fd >= 0) {
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
        if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0 && errno == EEXIST) {
            (void)epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
    }
    if (state->use_uring && state->uring_loop) {
        (void)uring_loop_add_poll(state->uring_loop, fd, POLLIN, ev_type, state);
    }
}

static bool daemon_activate_shader(daemon_state_t *state, const waywal_shader_payload_t *spay)
{
    if (!state || !spay || spay->shader_path[0] == '\0')
        return false;

    if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
        video_engine_stop(&state->video_engine);
    }
    slideshow_stop(&state->slideshow);
    anim_engine_stop_playback(state);

    if (!state->shader_eng && state->dmabuf_ctx.available) {
        state->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
    }

    if (spay->audio_reactive) {
        if (!state->audio_fft) {
            state->audio_fft = waywal_audio_fft_create(48000, true);
        }
    } else if (state->audio_fft) {
        waywal_audio_fft_destroy(state->audio_fft);
        state->audio_fft = NULL;
    }

    bool ok = false;
    if (state->shader_eng) {
        if (waywal_shader_engine_load_file(state->shader_eng, spay->shader_path)) {
            ok = true;
            int inotify_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
            if (inotify_fd >= 0 && inotify_fd != g_registered_shader_inotify_fd) {
                register_fd_in_loops(state, inotify_fd, URING_EV_TIMER);
                g_registered_shader_inotify_fd = inotify_fd;
            }
        }
    } else {
        ok = validate_shader_file_headless(spay->shader_path);
    }

    if (!ok) {
        shader_engine_stop_playback(state);
        return false;
    }

    state->shader_active = true;
    state->shader_fps = spay->fps > 0 ? spay->fps : 60U;
    state->shader_frame_idx = 0;
    state->shader_start_ns = now_ns();
    state->shader_last_ns = state->shader_start_ns;
    memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
    strncpy(state->shader_target_output, spay->output[0] ? spay->output : "*",
            sizeof(state->shader_target_output) - 1);
    memset(state->shader_path, 0, sizeof(state->shader_path));
    strncpy(state->shader_path, spay->shader_path, sizeof(state->shader_path) - 1);

    daemon_shader_render_once(state);
    daemon_reconcile_power_state(state);
    return true;
}

static bool daemon_render_image_on_target(daemon_state_t *state, const char *filepath,
                                          const char *target_output, waywal_scaling_mode_t scaling,
                                          waywal_filter_mode_t filter, bool ten_bit,
                                          uint32_t transition_type, uint32_t duration_ms)
{
    if (!state || !filepath || filepath[0] == '\0')
        return false;

    uint8_t *px = NULL;
    uint32_t w = 0, h = 0;
    if (!image_load(filepath, &px, &w, &h))
        return false;

    size_t extra_len = 0;
    uint32_t num_targets = 0;
    if (target_output && target_output[0] != '\0' && strcmp(target_output, "*") != 0) {
        extra_len = strlen(target_output) + 1;
        num_targets = 1;
    }

    uint8_t meta_storage[sizeof(waywal_img_metadata_t) + 128];
    memset(meta_storage, 0, sizeof(meta_storage));
    waywal_img_metadata_t *meta = (waywal_img_metadata_t *)meta_storage;
    meta->width = w;
    meta->height = h;
    meta->transition_type = transition_type;
    meta->transition_duration_ms = duration_ms;
    meta->transition_fps = state->config.transition.fps > 0 ? state->config.transition.fps : 60;
    meta->scaling_mode = (uint32_t)scaling;
    meta->filter_mode = (uint32_t)filter;
    meta->ten_bit = ten_bit;
    meta->num_target_outputs = num_targets;
    meta->extra_data_len = (uint32_t)extra_len;
    strncpy(meta->filepath, filepath, sizeof(meta->filepath) - 1);
    if (num_targets == 1 && extra_len <= 64) {
        memcpy(meta_storage + sizeof(waywal_img_metadata_t), target_output, extra_len);
    }

    bool ok = daemon_start_image_transition(state, px, w, h, meta);
    image_free(px);
    if (ok) {
        (void)try_start_native_animation(state, filepath, meta,
                                         target_output ? target_output : "*");
    }
    return ok;
}

static bool daemon_restore_output_entry(daemon_state_t *state,
                                        const waywal_output_saved_state_t *saved,
                                        const char *override_output)
{
    if (!state || !saved)
        return false;

    const char *target = (override_output && override_output[0] != '\0')
                             ? override_output
                             : (saved->output_name[0] ? saved->output_name : "*");

    switch (saved->kind) {
    case WAYWAL_CONTENT_SOLID_COLOR: {
        color_rgba_t col = {
            .r = (uint8_t)((saved->solid_color >> 24) & 0xFF),
            .g = (uint8_t)((saved->solid_color >> 16) & 0xFF),
            .b = (uint8_t)((saved->solid_color >> 8) & 0xFF),
            .a = (uint8_t)(saved->solid_color & 0xFF),
        };
        if (strcmp(target, "*") == 0) {
            daemon_clear_all_outputs(state, col);
        } else {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (strcmp(out->name, target) == 0) {
                    output_node_render_color(state, out, col);
                }
            }
        }
        return true;
    }
    case WAYWAL_CONTENT_STATIC_IMAGE:
    case WAYWAL_CONTENT_ANIMATION: {
        return daemon_render_image_on_target(state, saved->media_path, target, saved->scaling,
                                             saved->filter, saved->ten_bit,
                                             WAYWAL_TRANSITION_NONE, 0);
    }
    case WAYWAL_CONTENT_SLIDESHOW: {
        if (saved->media_path[0] == '\0')
            return false;
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s =
            saved->slideshow_interval_sec > 0 ? saved->slideshow_interval_sec : 300;
        spay.scaling_mode = saved->scaling;
        spay.filter_mode = saved->filter;
        spay.ten_bit = saved->ten_bit;
        strncpy(spay.path, saved->media_path, sizeof(spay.path) - 1);
        return slideshow_start(&state->slideshow, &spay);
    }
    case WAYWAL_CONTENT_VIDEO: {
        if (saved->media_path[0] == '\0')
            return false;
        output_node_t *target_out = NULL;
        if (strcmp(target, "*") != 0) {
            target_out = output_manager_find_by_name(&state->output_mgr, target);
        }
        return video_engine_load_file(&state->video_engine, saved->media_path, target_out);
    }
    case WAYWAL_CONTENT_SHADER: {
        if (saved->media_path[0] == '\0')
            return false;
        waywal_shader_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        strncpy(spay.output, target, sizeof(spay.output) - 1);
        strncpy(spay.shader_path, saved->media_path, sizeof(spay.shader_path) - 1);
        spay.fps = saved->target_fps > 0 ? saved->target_fps : 60;
        spay.audio_reactive = saved->audio_reactive;
        return daemon_activate_shader(state, &spay);
    }
    default:
        return false;
    }
}

static bool daemon_apply_output_config_rule(daemon_state_t *state, const waywal_output_rule_t *rule)
{
    if (!state || !rule || rule->path[0] == '\0')
        return false;
    const char *target = rule->output_name[0] ? rule->output_name : "*";
    if (strcasecmp(rule->mode, "shader") == 0) {
        waywal_shader_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        strncpy(spay.output, target, sizeof(spay.output) - 1);
        strncpy(spay.shader_path, rule->path, sizeof(spay.shader_path) - 1);
        spay.fps = rule->fps > 0 ? rule->fps : 60;
        spay.audio_reactive = rule->audio;
        return daemon_activate_shader(state, &spay);
    }
    if (strcasecmp(rule->mode, "video") == 0) {
        output_node_t *target_out =
            (strcmp(target, "*") != 0) ? output_manager_find_by_name(&state->output_mgr, target)
                                       : NULL;
        return video_engine_load_file(&state->video_engine, rule->path, target_out);
    }
    if (strcasecmp(rule->mode, "slideshow") == 0) {
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s = rule->interval > 0 ? rule->interval : 300;
        spay.random_order = rule->shuffle;
        spay.scaling_mode = (uint32_t)rule->scaling;
        spay.filter_mode = (uint32_t)rule->filter;
        strncpy(spay.path, rule->path, sizeof(spay.path) - 1);
        return slideshow_start(&state->slideshow, &spay);
    }
    return daemon_render_image_on_target(state, rule->path, target, rule->scaling, rule->filter,
                                         state->config.daemon.ten_bit, WAYWAL_TRANSITION_NONE, 0);
}

static void daemon_on_output_configured(daemon_state_t *state, output_node_t *out)
{
    if (!state || !out || out->name[0] == '\0')
        return;

    /* 1. Check per-output config rule in config.toml */
    for (size_t i = 0; i < state->config.output_count; ++i) {
        const waywal_output_rule_t *rule = &state->config.outputs[i];
        if (strcmp(rule->output_name, out->name) == 0 && rule->path[0] != '\0') {
            (void)daemon_apply_output_config_rule(state, rule);
            return;
        }
    }

    /* 2. Check persisted state_store for this output or wildcard "*" */
    const waywal_output_saved_state_t *saved =
        waywal_state_store_find_output(&g_saved_state, out->name);
    if (!saved) {
        saved = waywal_state_store_find_output(&g_saved_state, "*");
    }
    if (saved) {
        (void)daemon_restore_output_entry(state, saved, out->name);
    }
}

static void daemon_handle_power_gov_events(daemon_state_t *state, int fd)
{
    if (!state || !state->power_gov)
        return;

    int ipc_fd = waywal_power_governor_get_ipc_fd(state->power_gov);
    int inotify_fd = waywal_power_governor_get_inotify_fd(state->power_gov);

    if (fd == ipc_fd) {
        waywal_power_governor_dispatch_ipc(state->power_gov);
        char ws[64];
        char mon[64];
        while (waywal_power_governor_pop_workspace_change(state->power_gov, ws, sizeof(ws), mon,
                                                          sizeof(mon))) {
            for (size_t i = 0; i < state->config.workspace_count; ++i) {
                const waywal_workspace_rule_t *wrule = &state->config.workspaces[i];
                if (strcmp(wrule->workspace_name, ws) == 0 && wrule->path[0] != '\0') {
                    const char *target =
                        wrule->output_name[0] ? wrule->output_name : (mon[0] ? mon : "*");
                    uint32_t dur_ms =
                        (uint32_t)(state->config.transition.duration > 0.0f
                                       ? state->config.transition.duration * 1000.0f
                                       : 500.0f);
                    (void)daemon_render_image_on_target(
                        state, wrule->path, target, WAYWAL_SCALING_FILL, WAYWAL_FILTER_LANCZOS3,
                        state->config.daemon.ten_bit, wrule->transition_type_enum, dur_ms);
                    break;
                }
            }
        }
    } else if (fd == inotify_fd) {
        (void)waywal_power_governor_poll_battery(state->power_gov);
    }

    daemon_reconcile_power_state(state);
}

static void print_usage(const char *prog)
{
    printf("wwald - High-performance Wayland wallpaper daemon (C23)\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -n, --namespace <NAME>       Socket namespace (default: \"default\")\n");
    printf("      --layer <LAYER>          Layer target: background or bottom\n");
    printf("      --backdrop <MODE>        Backdrop mode: none, auto-blur, or path\n");
    printf("      --battery-policy <POL>   Battery policy: ignore, reduce-fps, pause-video\n");
    printf("  -v, --verbose                Enable debug logging\n");
    printf("  -h, --help                   Print help information\n");
}

static bool read_exact(int fd, void *buf, size_t len)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < len) {
        ssize_t r = read(fd, p + total, len - total);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        total += (size_t)r;
    }
    return true;
}

static bool write_exact(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < len) {
        ssize_t w = write(fd, p + total, len - total);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        total += (size_t)w;
    }
    return true;
}

static void daemon_process_ipc_client(daemon_state_t *state, int client_fd)
{
    /* Verify peer credentials (SO_PEERCRED) to prevent unauthorized local hijack */
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0 ||
        cred.uid != geteuid()) {
        WAYWAL_LOG_WARN("Rejected unauthorized IPC connection from UID %u (expected %u)", cred.uid,
                        geteuid());
        close(client_fd);
        return;
    }

    /* Set socket timeouts to prevent stalled client from hanging daemon event loop */
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    waywal_ipc_hdr_t req_hdr;
    int attached_fd = -1;
    if (!waywal_ipc_recv(client_fd, &req_hdr, &attached_fd)) {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        close(client_fd);
        return;
    }

    waywal_ipc_hdr_t resp_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_RESP_OK,
        .payload_size = 0,
    };

    switch (req_hdr.opcode) {
    case WAYWAL_REQ_PING: {
        resp_hdr.opcode = WAYWAL_RESP_PONG;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_QUERY: {
        char query_buf[4096];
        size_t off = 0;
        off += (size_t)snprintf(query_buf + off, sizeof(query_buf) - off,
                                "Connected outputs (%zu):\n", state->num_outputs);

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (off < sizeof(query_buf)) {
                off += (size_t)snprintf(query_buf + off, sizeof(query_buf) - off,
                                        "  - %s (%s): %dx%d (scale: %d)\n",
                                        out->name[0] ? out->name : "unknown",
                                        out->description[0] ? out->description : "unknown",
                                        out->width, out->height, out->scale_factor);
            }
        }

        resp_hdr.opcode = WAYWAL_RESP_INFO;
        resp_hdr.payload_size = off;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        (void)write_exact(client_fd, query_buf, off);
        break;
    }

    case WAYWAL_REQ_CLEAR: {
        waywal_clear_payload_t clear_payload;
        if (req_hdr.payload_size >= sizeof(clear_payload)) {
            if (read_exact(client_fd, &clear_payload, sizeof(clear_payload))) {
                color_rgba_t color = {
                    .r = clear_payload.color[0],
                    .g = clear_payload.color[1],
                    .b = clear_payload.color[2],
                    .a = clear_payload.color[3],
                };
                char extra_buf[4096];
                size_t extra_len = req_hdr.payload_size - sizeof(clear_payload);
                bool has_targets = false;
                if (clear_payload.num_target_outputs > 0 &&
                    clear_payload.num_target_outputs <= 64 && extra_len > 0 &&
                    extra_len <= sizeof(extra_buf)) {
                    if (read_exact(client_fd, extra_buf, extra_len)) {
                        extra_buf[extra_len - 1] = '\0';
                        has_targets = true;
                    }
                }

                bool had_video = (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED ||
                                  state->video_engine.decoder.initialized);
                state->video_engine.state = WAYWAL_VIDEO_STATE_STOPPED;
                if (had_video) {
                    video_engine_stop(&state->video_engine);
                }
                slideshow_stop(&state->slideshow);
                shader_engine_stop_playback(state);
                anim_engine_stop_playback(state);

                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                s_entry.kind = WAYWAL_CONTENT_SOLID_COLOR;
                s_entry.solid_color = ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
                                      ((uint32_t)color.b << 8) | (uint32_t)color.a;

                if (has_targets) {
                    const char *ptr = extra_buf;
                    size_t rem = extra_len;
                    for (uint32_t i = 0; i < clear_payload.num_target_outputs && rem > 0; ++i) {
                        const char *end = (const char *)memchr(ptr, '\0', rem);
                        if (!end)
                            break;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (strcmp(out->name, ptr) == 0) {
                                output_node_render_color(state, out, color);
                            }
                        }
                        memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                        strncpy(s_entry.output_name, ptr, sizeof(s_entry.output_name) - 1);
                        (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                        size_t step = (size_t)(end - ptr) + 1;
                        ptr += step;
                        rem -= step;
                    }
                } else {
                    daemon_clear_all_outputs(state, color);
                    strncpy(s_entry.output_name, "*", sizeof(s_entry.output_name) - 1);
                    (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (out->name[0] != '\0') {
                            memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                            strncpy(s_entry.output_name, out->name,
                                    sizeof(s_entry.output_name) - 1);
                            (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                        }
                    }
                }
                (void)waywal_state_store_save(&g_saved_state, NULL);
                WAYWAL_LOG_INFO("Cleared screen with color: #%02x%02x%02x%02x", color.r, color.g,
                                color.b, color.a);
            }
        }
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_SET_IMAGE: {
        struct stat st;
        if (attached_fd >= 0 && fstat(attached_fd, &st) == 0 &&
            (size_t)st.st_size >= sizeof(waywal_img_metadata_t) &&
            req_hdr.payload_size >= sizeof(waywal_img_metadata_t) &&
            (size_t)st.st_size >= req_hdr.payload_size) {
            void *mapped =
                mmap(NULL, req_hdr.payload_size, PROT_READ, MAP_PRIVATE, attached_fd, 0);
            if (mapped != MAP_FAILED) {
                waywal_img_metadata_t *meta = (waywal_img_metadata_t *)mapped;
                uint64_t expected_size = sizeof(waywal_img_metadata_t) +
                                         (uint64_t)meta->extra_data_len +
                                         (uint64_t)meta->width * (uint64_t)meta->height * 4ULL;
                bool ok = false;
                if (meta->width > 0 && meta->height > 0 && meta->width <= 16384 &&
                    meta->height <= 16384 && req_hdr.payload_size >= expected_size &&
                    (size_t)st.st_size >= expected_size) {
                    bool valid_strings = true;
                    if (meta->num_target_outputs > 64) {
                        valid_strings = false;
                    } else {
                        const char *ptr = (const char *)mapped + sizeof(waywal_img_metadata_t);
                        size_t remaining = meta->extra_data_len;
                        for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
                            const char *end = (const char *)memchr(ptr, '\0', remaining);
                            if (!end) {
                                valid_strings = false;
                                break;
                            }
                            size_t len = (size_t)(end - ptr) + 1;
                            ptr += len;
                            remaining -= len;
                        }
                        if (valid_strings && meta->custom_shader_len > 0) {
                            if (meta->custom_shader_len > remaining) {
                                valid_strings = false;
                            } else {
                                const char *end =
                                    (const char *)memchr(ptr, '\0', meta->custom_shader_len);
                                if (!end) {
                                    valid_strings = false;
                                }
                            }
                        }
                    }

                    if (valid_strings) {
                        const uint8_t *pixels = (const uint8_t *)mapped +
                                                sizeof(waywal_img_metadata_t) +
                                                meta->extra_data_len;
                        WAYWAL_LOG_INFO("Rendering received image: %ux%u", meta->width,
                                        meta->height);
                        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
                            video_engine_stop(&state->video_engine);
                        }
                        slideshow_stop(&state->slideshow);
                        shader_engine_stop_playback(state);
                        anim_engine_stop_playback(state);

                        ok = daemon_start_image_transition(state, pixels, meta->width,
                                                           meta->height, meta);
                        if (ok) {
                            const char *first_target = "*";
                            if (meta->num_target_outputs > 0) {
                                first_target =
                                    (const char *)mapped + sizeof(waywal_img_metadata_t);
                            }
                            bool is_anim = try_start_native_animation(state, meta->filepath, meta,
                                                                      first_target);

                            waywal_output_saved_state_t s_entry;
                            memset(&s_entry, 0, sizeof(s_entry));
                            s_entry.kind = is_anim ? WAYWAL_CONTENT_ANIMATION
                                                   : WAYWAL_CONTENT_STATIC_IMAGE;
                            snprintf(s_entry.media_path, sizeof(s_entry.media_path), "%.*s",
                                     (int)(sizeof(meta->filepath) - 1), meta->filepath);
                            s_entry.scaling = (waywal_scaling_mode_t)meta->scaling_mode;
                            s_entry.filter = (waywal_filter_mode_t)meta->filter_mode;
                            s_entry.ten_bit = meta->ten_bit;
                            s_entry.solid_color = ((uint32_t)meta->color[0] << 24) |
                                                  ((uint32_t)meta->color[1] << 16) |
                                                  ((uint32_t)meta->color[2] << 8) |
                                                  (uint32_t)meta->color[3];

                            if (meta->num_target_outputs > 0) {
                                const char *ptr =
                                    (const char *)mapped + sizeof(waywal_img_metadata_t);
                                for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
                                    memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                                    strncpy(s_entry.output_name, ptr,
                                            sizeof(s_entry.output_name) - 1);
                                    (void)waywal_state_store_update_output(&g_saved_state,
                                                                           &s_entry);
                                    ptr += strlen(ptr) + 1;
                                }
                            } else {
                                strncpy(s_entry.output_name, "*", sizeof(s_entry.output_name) - 1);
                                (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                                for (output_node_t *out = state->outputs; out != NULL;
                                     out = out->next) {
                                    if (out->name[0] != '\0') {
                                        memset(s_entry.output_name, 0,
                                               sizeof(s_entry.output_name));
                                        strncpy(s_entry.output_name, out->name,
                                                sizeof(s_entry.output_name) - 1);
                                        (void)waywal_state_store_update_output(&g_saved_state,
                                                                               &s_entry);
                                    }
                                }
                            }
                            (void)waywal_state_store_save(&g_saved_state, NULL);
                        }
                    } else {
                        WAYWAL_LOG_ERR("IPC image payload string validation failed");
                    }
                } else {
                    WAYWAL_LOG_ERR(
                        "IPC image payload bounds check failed: %ux%u, size=%zu, expected=%lu",
                        meta->width, meta->height, req_hdr.payload_size,
                        (unsigned long)expected_size);
                }
                resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
                munmap(mapped, req_hdr.payload_size);
            } else {
                WAYWAL_LOG_ERR("mmap attached memfd failed: %s", strerror(errno));
                resp_hdr.opcode = WAYWAL_RESP_ERR;
            }
        } else {
            resp_hdr.opcode = WAYWAL_RESP_ERR;
        }

        if (attached_fd >= 0) {
            close(attached_fd);
            attached_fd = -1;
        }
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_SET_VIDEO: {
        slideshow_stop(&state->slideshow);
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
        bool ok = false;
        waywal_video_payload_t vpay;
        bool has_vpay = false;
        output_node_t *target_out = NULL;
        char extra_buf[4096];
        size_t extra_len = 0;
        bool has_extra_targets = false;
        state->video_engine.has_target_filter = false;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            out->video_targeted = true;
        }

        if (req_hdr.payload_size >= sizeof(waywal_video_payload_t)) {
            if (read_exact(client_fd, &vpay, sizeof(vpay))) {
                vpay.filepath[sizeof(vpay.filepath) - 1] = '\0';
                state->video_engine.loop_count = vpay.loop_count;
                state->video_engine.playback_speed =
                    vpay.playback_speed > 0.01f ? vpay.playback_speed : 1.0f;
                has_vpay = true;

                extra_len = req_hdr.payload_size - sizeof(waywal_video_payload_t);
                if (vpay.num_target_outputs > 0 && vpay.num_target_outputs <= 64 && extra_len > 0 &&
                    extra_len <= sizeof(extra_buf)) {
                    if (read_exact(client_fd, extra_buf, extra_len)) {
                        extra_buf[extra_len - 1] = '\0';
                        has_extra_targets = true;
                        state->video_engine.has_target_filter = true;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            out->video_targeted = false;
                        }
                        const char *ptr = extra_buf;
                        size_t remaining = extra_len;
                        uint32_t matched_count = 0;
                        output_node_t *last_matched = NULL;
                        for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0; ++i) {
                            const char *end = (const char *)memchr(ptr, '\0', remaining);
                            if (!end)
                                break;
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (strcmp(out->name, ptr) == 0) {
                                    out->video_targeted = true;
                                    last_matched = out;
                                    matched_count++;
                                }
                            }
                            size_t step = (size_t)(end - ptr) + 1;
                            ptr += step;
                            remaining -= step;
                        }
                        if (matched_count == 1) {
                            target_out = last_matched;
                        }
                    }
                }
            }
        }
        if (attached_fd >= 0) {
            struct stat st;
            if (fstat(attached_fd, &st) == 0 && st.st_size > 0) {
                ok = video_engine_load_mem(&state->video_engine, attached_fd, (size_t)st.st_size,
                                           target_out);
                attached_fd = -1;
            } else {
                close(attached_fd);
                attached_fd = -1;
            }
        } else if (has_vpay) {
            ok = video_engine_load_file(&state->video_engine, vpay.filepath, target_out);
        }
        if (ok && has_vpay) {
            daemon_reconcile_power_state(state);
            waywal_output_saved_state_t s_entry;
            memset(&s_entry, 0, sizeof(s_entry));
            s_entry.kind = WAYWAL_CONTENT_VIDEO;
            strncpy(s_entry.media_path, vpay.filepath, sizeof(s_entry.media_path) - 1);
            if (has_extra_targets) {
                const char *ptr = extra_buf;
                size_t rem = extra_len;
                for (uint32_t i = 0; i < vpay.num_target_outputs && rem > 0; ++i) {
                    const char *end = (const char *)memchr(ptr, '\0', rem);
                    if (!end)
                        break;
                    memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                    strncpy(s_entry.output_name, ptr, sizeof(s_entry.output_name) - 1);
                    (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                    size_t step = (size_t)(end - ptr) + 1;
                    ptr += step;
                    rem -= step;
                }
            } else {
                strncpy(s_entry.output_name, "*", sizeof(s_entry.output_name) - 1);
                (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
            }
            (void)waywal_state_store_save(&g_saved_state, NULL);
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_PAUSE: {
        video_engine_pause(&state->video_engine);
        if (state->shader_active) {
            disarm_timer_fd(state->shader_timer_fd);
        }
        if (state->anim_active) {
            disarm_timer_fd(state->anim_timer_fd);
        }
        if (state->power_gov) {
            waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_USER_MANUAL,
                                                    true);
        }
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_UNPAUSE: {
        if (state->power_gov) {
            waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_USER_MANUAL,
                                                    false);
        }
        video_engine_resume(&state->video_engine);
        daemon_reconcile_power_state(state);
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_TOGGLE: {
        if (state->video_engine.state == WAYWAL_VIDEO_STATE_PLAYING) {
            video_engine_pause(&state->video_engine);
        } else if (state->video_engine.state == WAYWAL_VIDEO_STATE_PAUSED) {
            video_engine_resume(&state->video_engine);
        }
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_SLIDESHOW: {
        waywal_slideshow_payload_t spay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(spay)) {
            if (read_exact(client_fd, &spay, sizeof(spay))) {
                spay.path[sizeof(spay.path) - 1] = '\0';
                if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
                    video_engine_stop(&state->video_engine);
                }
                shader_engine_stop_playback(state);
                anim_engine_stop_playback(state);
                ok = slideshow_start(&state->slideshow, &spay);
                if (ok) {
                    waywal_output_saved_state_t s_entry;
                    memset(&s_entry, 0, sizeof(s_entry));
                    strncpy(s_entry.output_name, "*", sizeof(s_entry.output_name) - 1);
                    s_entry.kind = WAYWAL_CONTENT_SLIDESHOW;
                    strncpy(s_entry.media_path, spay.path, sizeof(s_entry.media_path) - 1);
                    s_entry.slideshow_interval_sec = spay.interval_s;
                    s_entry.scaling = (waywal_scaling_mode_t)spay.scaling_mode;
                    s_entry.filter = (waywal_filter_mode_t)spay.filter_mode;
                    s_entry.ten_bit = spay.ten_bit;
                    (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                    (void)waywal_state_store_save(&g_saved_state, NULL);
                }
            }
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_SLIDESHOW_CTRL: {
        waywal_slideshow_ctrl_t sctrl;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(sctrl)) {
            if (read_exact(client_fd, &sctrl, sizeof(sctrl))) {
                ok = slideshow_control(&state->slideshow, sctrl.action);
            }
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    case WAYWAL_REQ_SHADER: {
        waywal_shader_payload_t spay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(spay) && read_exact(client_fd, &spay, sizeof(spay))) {
            spay.output[sizeof(spay.output) - 1] = '\0';
            spay.shader_path[sizeof(spay.shader_path) - 1] = '\0';

            ok = daemon_activate_shader(state, &spay);
            if (ok) {
                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                strncpy(s_entry.output_name, spay.output[0] ? spay.output : "*",
                        sizeof(s_entry.output_name) - 1);
                s_entry.kind = WAYWAL_CONTENT_SHADER;
                strncpy(s_entry.media_path, spay.shader_path, sizeof(s_entry.media_path) - 1);
                s_entry.target_fps = spay.fps > 0 ? spay.fps : 60;
                s_entry.audio_reactive = spay.audio_reactive;
                (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                (void)waywal_state_store_save(&g_saved_state, NULL);
            }
        }

        if (ok) {
            const char *msg = "OK: Shader active\n";
            resp_hdr.opcode = WAYWAL_RESP_OK;
            resp_hdr.payload_size = strlen(msg);
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            (void)write_exact(client_fd, msg, strlen(msg));
        } else {
            resp_hdr.opcode = WAYWAL_RESP_ERR;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
        }
        break;
    }

    case WAYWAL_REQ_STATS: {
        unsigned long total_pages = 0, rss_pages = 0;
        FILE *statm = fopen("/proc/self/statm", "r");
        if (statm) {
            (void)fscanf(statm, "%lu %lu", &total_pages, &rss_pages);
            fclose(statm);
        }
        long page_sz = sysconf(_SC_PAGESIZE);
        if (page_sz <= 0)
            page_sz = 4096;
        unsigned long rss_kib = (rss_pages * (unsigned long)page_sz) / 1024UL;

        char stats_buf[4096];
        size_t off = 0;
        off += (size_t)snprintf(
            stats_buf + off, sizeof(stats_buf) - off,
            "=== WayWal Daemon Telemetry ===\n"
            "RSS Memory:     %lu KiB (%.2f MiB)\n"
            "GPU Compute:    %s\n"
            "DMA-BUF Scanout:%s (modifier=0x%016lx, 10bit=%s, nv12=%s)\n"
            "Outputs (%zu):\n",
            rss_kib, (double)rss_kib / 1024.0,
            state->render_engine.gpu_compute_active ? "active" : "cpu-simd-fallback",
            state->dmabuf_ctx.available ? "enabled" : "shm-fallback",
            (unsigned long)state->dmabuf_ctx.preferred_modifier,
            state->dmabuf_ctx.has_10bit ? "yes" : "no",
            state->dmabuf_ctx.has_nv12 ? "yes" : "no", state->num_outputs);

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (off >= sizeof(stats_buf))
                break;
            uint32_t pmask =
                state->power_gov ? waywal_power_governor_get_output_mask(state->power_gov, out->name)
                                 : 0;
            int32_t fscale =
                out->fractional_scale > 0 ? out->fractional_scale : (out->scale_factor * 120);
            off += (size_t)snprintf(
                stats_buf + off, sizeof(stats_buf) - off,
                "  - %s: physical=%dx%d logical=%ux%u scale=%d/120 vblank_period=%lldns pause_mask=0x%02x\n",
                out->name[0] ? out->name : "unknown", out->width, out->height, out->logical_width,
                out->logical_height, fscale, (long long)out->pres_sync.pll.period_est_ns, pmask);
        }

        resp_hdr.opcode = WAYWAL_RESP_OK;
        resp_hdr.payload_size = off;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        (void)write_exact(client_fd, stats_buf, off);
        break;
    }

    case WAYWAL_REQ_RESTORE: {
        (void)waywal_state_store_load(&g_saved_state, NULL);
        bool restored_any = false;

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->name[0] == '\0')
                continue;
            const waywal_output_saved_state_t *out_saved =
                waywal_state_store_find_output(&g_saved_state, out->name);
            if (out_saved) {
                if (daemon_restore_output_entry(state, out_saved, out->name)) {
                    restored_any = true;
                }
            }
        }

        if (!restored_any) {
            const waywal_output_saved_state_t *saved =
                waywal_state_store_find_output(&g_saved_state, "*");
            if (!saved && g_saved_state.count > 0) {
                saved = &g_saved_state.outputs[0];
            }
            if (saved) {
                (void)daemon_restore_output_entry(state, saved, "*");
            }
        }

        const char *msg = "OK: State restored\n";
        resp_hdr.opcode = WAYWAL_RESP_OK;
        resp_hdr.payload_size = strlen(msg);
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        (void)write_exact(client_fd, msg, strlen(msg));
        break;
    }

    case WAYWAL_REQ_KILL: {
        WAYWAL_LOG_INFO("Received kill request from client");
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        state->running = false;
        if (state->uring_loop) {
            uring_loop_stop(state->uring_loop);
        }
        break;
    }

    default:
        resp_hdr.opcode = WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        break;
    }

    if (attached_fd >= 0) {
        close(attached_fd);
        attached_fd = -1;
    }
    close(client_fd);
}

void daemon_handle_ipc_connection(daemon_state_t *state)
{
    if (!state || state->server_socket_fd < 0)
        return;

    while (true) {
        int client_fd = accept4(state->server_socket_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            WAYWAL_LOG_ERR("accept4 failed: %s", strerror(errno));
            break;
        }

        daemon_process_ipc_client(state, client_fd);

        if (!state->running) {
            break;
        }
    }
}

static bool g_daemon_wl_ok = true;

static bool daemon_pump_wayland_events(daemon_state_t *st)
{
    if (!st || !st->display)
        return false;
    while (wl_display_prepare_read(st->display) != 0) {
        wl_display_dispatch_pending(st->display);
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(st->display), .events = POLLIN};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP))) {
        if (wl_display_read_events(st->display) < 0 && errno != EAGAIN) {
            g_daemon_wl_ok = false;
            return false;
        }
    } else {
        wl_display_cancel_read(st->display);
    }
    wl_display_dispatch_pending(st->display);
    if (wl_display_get_error(st->display) != 0) {
        g_daemon_wl_ok = false;
        return false;
    }
    return true;
}

static void daemon_reload_config(daemon_state_t *st)
{
    if (!st)
        return;
    if (waywal_config_load_default(&st->config)) {
        st->use_bottom_layer = (st->config.daemon.layer_pos == WAYWAL_LAYER_BOTTOM);
        st->backdrop_auto_blur = (strcasecmp(st->config.daemon.backdrop, "auto-blur") == 0);
        st->idle_timeout_sec = st->config.daemon.idle_timeout_sec;
        if (st->power_gov) {
            waywal_power_governor_set_battery_policy(st->power_gov,
                                                     st->config.daemon.battery_policy_mode);
        }
        daemon_reconcile_power_state(st);
        WAYWAL_LOG_INFO("Reloaded configuration on SIGHUP");
    }
}

static void daemon_uring_event_callback(uring_event_type_t type, int fd, uint32_t res,
                                        void *user_data)
{
    daemon_state_t *st = (daemon_state_t *)user_data;
    if (!st)
        return;

    switch (type) {
    case URING_EV_WAYLAND_READ:
        daemon_pump_wayland_events(st);
        break;
    case URING_EV_IPC_ACCEPT:
        daemon_handle_ipc_connection(st);
        break;
    case URING_EV_TIMER:
        if ((res & POLLIN) == 0)
            break;
        daemon_pump_wayland_events(st);
        if (fd == st->transition_timer_fd) {
            transition_engine_dispatch_tick(st);
        } else if (fd == st->video_engine.timer_fd) {
            video_engine_dispatch_frame(&st->video_engine);
        } else if (fd == st->slideshow.timer_fd) {
            slideshow_dispatch_tick(&st->slideshow);
        } else if (fd == st->shader_timer_fd) {
            daemon_shader_dispatch_frame(st);
        } else if (fd == st->anim_timer_fd) {
            daemon_anim_dispatch_frame(st);
        } else if (st->shader_eng && fd == waywal_shader_engine_get_inotify_fd(st->shader_eng)) {
            if (waywal_shader_engine_poll_hot_reload(st->shader_eng) && st->shader_active) {
                daemon_shader_render_once(st);
            }
        } else if (st->power_gov &&
                   (fd == waywal_power_governor_get_ipc_fd(st->power_gov) ||
                    fd == waywal_power_governor_get_inotify_fd(st->power_gov))) {
            daemon_handle_power_gov_events(st, fd);
        }
        break;
    case URING_EV_SIGNAL: {
        struct signalfd_siginfo fdsi;
        ssize_t s = read(fd, &fdsi, sizeof(fdsi));
        if (s == (ssize_t)sizeof(fdsi)) {
            if (fdsi.ssi_signo == SIGHUP) {
                daemon_reload_config(st);
                break;
            }
            WAYWAL_LOG_INFO("Received signal %u, shutting down cleanly", fdsi.ssi_signo);
            st->running = false;
            if (st->uring_loop) {
                uring_loop_stop(st->uring_loop);
            }
        }
        break;
    }
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    daemon_state_t state;
    memset(&state, 0, sizeof(state));
    state.shader_timer_fd = -1;
    state.anim_timer_fd = -1;
    strncpy(state.namespace_str, "default", sizeof(state.namespace_str) - 1);
    state.current_color = (color_rgba_t){.r = 0, .g = 0, .b = 0, .a = 255};
    state.running = true;
    state.on_power_state_changed = daemon_reconcile_power_state;

    (void)waywal_config_load_default(&state.config);
    state.use_bottom_layer = (state.config.daemon.layer_pos == WAYWAL_LAYER_BOTTOM);
    state.backdrop_auto_blur = (strcasecmp(state.config.daemon.backdrop, "auto-blur") == 0);
    state.idle_timeout_sec = state.config.daemon.idle_timeout_sec;
    if (state.config.daemon.backdrop[0] != '\0' &&
        strcasecmp(state.config.daemon.backdrop, "none") != 0 && !state.backdrop_auto_blur) {
        strncpy(state.backdrop_path, state.config.daemon.backdrop,
                sizeof(state.backdrop_path) - 1);
    }
    if (state.config.daemon.namespace_str[0] != '\0' &&
        strcmp(state.config.daemon.namespace_str, "waywal") != 0) {
        strncpy(state.namespace_str, state.config.daemon.namespace_str,
                sizeof(state.namespace_str) - 1);
    }
    waywal_battery_policy_t battery_policy = state.config.daemon.battery_policy_mode;

    static const struct option long_options[] = {{"namespace", required_argument, NULL, 'n'},
                                                 {"layer", required_argument, NULL, 1001},
                                                 {"backdrop", required_argument, NULL, 1002},
                                                 {"battery-policy", required_argument, NULL, 1003},
                                                 {"verbose", no_argument, NULL, 'v'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "n:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'n':
            strncpy(state.namespace_str, optarg, sizeof(state.namespace_str) - 1);
            state.namespace_str[sizeof(state.namespace_str) - 1] = '\0';
            break;
        case 1001:
            state.use_bottom_layer = (strcasecmp(optarg, "bottom") == 0);
            break;
        case 1002:
            state.backdrop_auto_blur = (strcasecmp(optarg, "auto-blur") == 0);
            if (strcasecmp(optarg, "none") != 0 && !state.backdrop_auto_blur) {
                strncpy(state.backdrop_path, optarg, sizeof(state.backdrop_path) - 1);
            }
            break;
        case 1003:
            if (strcasecmp(optarg, "ignore") == 0) {
                battery_policy = WAYWAL_BATTERY_IGNORE;
            } else if (strcasecmp(optarg, "pause-video") == 0) {
                battery_policy = WAYWAL_BATTERY_PAUSE_VIDEO;
            } else {
                battery_policy = WAYWAL_BATTERY_REDUCE_FPS;
            }
            break;
        case 'v':
            waywal_log_set_level(WAYWAL_LOG_LEVEL_DEBUG);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    state.power_gov = waywal_power_governor_create(battery_policy);
    waywal_state_store_init(&g_saved_state);
    (void)waywal_state_store_load(&g_saved_state, NULL);

    if (!arena_init(&state.persistent_arena, WAYWAL_PERSISTENT_ARENA_CAPACITY)) {
        fprintf(stderr, "Fatal: failed to initialize persistent memory arena\n");
        return 1;
    }
    if (!arena_init(&state.frame_arena, WAYWAL_FRAME_ARENA_CAPACITY)) {
        fprintf(stderr, "Fatal: failed to initialize frame memory arena\n");
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGHUP);
    sigaddset(&sigmask, SIGPIPE);
    signal(SIGPIPE, SIG_IGN);
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        WAYWAL_LOG_ERR("sigprocmask failed: %s", strerror(errno));
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    state.signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (state.signal_fd < 0) {
        WAYWAL_LOG_ERR("signalfd failed: %s", strerror(errno));
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    if (!daemon_wayland_init(&state)) {
        WAYWAL_LOG_ERR("Failed to initialize Wayland subsystem");
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    state.server_socket_fd = waywal_ipc_server_create(state.namespace_str);
    if (state.server_socket_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create IPC server socket");
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        WAYWAL_LOG_ERR("epoll_create1 failed: %s", strerror(errno));
        close(state.server_socket_fd);
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    int wl_fd = wl_display_get_fd(state.display);

    struct epoll_event ev_wl = {.events = EPOLLIN, .data.fd = wl_fd};
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev_wl);

    struct epoll_event ev_ipc = {.events = EPOLLIN, .data.fd = state.server_socket_fd};
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.server_socket_fd, &ev_ipc);

    struct epoll_event ev_sig = {.events = EPOLLIN, .data.fd = state.signal_fd};
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.signal_fd, &ev_sig);

    if (!video_engine_init(&state.video_engine, &state)) {
        WAYWAL_LOG_WARN(
            "Hardware video engine initialization failed (video playback will be unavailable)");
    }

    transition_engine_init(&state);
    slideshow_init(&state.slideshow, &state);
    state.shader_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    state.anim_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    int gov_ipc_fd = state.power_gov ? waywal_power_governor_get_ipc_fd(state.power_gov) : -1;
    int gov_inotify_fd =
        state.power_gov ? waywal_power_governor_get_inotify_fd(state.power_gov) : -1;

    if (state.transition_timer_fd >= 0) {
        struct epoll_event ev_trans = {.events = EPOLLIN, .data.fd = state.transition_timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.transition_timer_fd, &ev_trans);
    }
    if (state.video_engine.timer_fd >= 0) {
        struct epoll_event ev_vid = {.events = EPOLLIN, .data.fd = state.video_engine.timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.video_engine.timer_fd, &ev_vid);
    }
    if (state.slideshow.timer_fd >= 0) {
        struct epoll_event ev_slide = {.events = EPOLLIN, .data.fd = state.slideshow.timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.slideshow.timer_fd, &ev_slide);
    }
    if (state.shader_timer_fd >= 0) {
        struct epoll_event ev_sh = {.events = EPOLLIN, .data.fd = state.shader_timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.shader_timer_fd, &ev_sh);
    }
    if (state.anim_timer_fd >= 0) {
        struct epoll_event ev_anim = {.events = EPOLLIN, .data.fd = state.anim_timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.anim_timer_fd, &ev_anim);
    }
    if (gov_ipc_fd >= 0) {
        struct epoll_event ev_gipc = {.events = EPOLLIN, .data.fd = gov_ipc_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, gov_ipc_fd, &ev_gipc);
    }
    if (gov_inotify_fd >= 0) {
        struct epoll_event ev_gino = {.events = EPOLLIN, .data.fd = gov_inotify_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, gov_inotify_fd, &ev_gino);
    }

    uring_loop_t uring_loop;
    state.use_uring = uring_loop_init(&uring_loop, WAYWAL_URING_QUEUE_DEPTH);
    if (state.use_uring) {
        state.uring_loop = &uring_loop;
        uring_loop_add_poll(&uring_loop, wl_fd, POLLIN, URING_EV_WAYLAND_READ, &state);
        uring_loop_add_poll(&uring_loop, state.server_socket_fd, POLLIN, URING_EV_IPC_ACCEPT,
                            &state);
        uring_loop_add_poll(&uring_loop, state.signal_fd, POLLIN, URING_EV_SIGNAL, &state);
        if (state.transition_timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.transition_timer_fd, POLLIN, URING_EV_TIMER,
                                &state);
        }
        if (state.video_engine.timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.video_engine.timer_fd, POLLIN, URING_EV_TIMER,
                                &state);
        }
        if (state.slideshow.timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.slideshow.timer_fd, POLLIN, URING_EV_TIMER,
                                &state);
        }
        if (state.shader_timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.shader_timer_fd, POLLIN, URING_EV_TIMER,
                                &state);
        }
        if (state.anim_timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.anim_timer_fd, POLLIN, URING_EV_TIMER, &state);
        }
        if (gov_ipc_fd >= 0) {
            uring_loop_add_poll(&uring_loop, gov_ipc_fd, POLLIN, URING_EV_TIMER, &state);
        }
        if (gov_inotify_fd >= 0) {
            uring_loop_add_poll(&uring_loop, gov_inotify_fd, POLLIN, URING_EV_TIMER, &state);
        }
        WAYWAL_LOG_INFO("Event loop running via Linux io_uring (zero-syscall polling)");
    } else {
        WAYWAL_LOG_INFO("Event loop running via epoll fallback");
    }

    state.on_output_configured = daemon_on_output_configured;
    if (state.config.output_count > 0) {
        for (size_t i = 0; i < state.config.output_count; ++i) {
            (void)daemon_apply_output_config_rule(&state, &state.config.outputs[i]);
        }
    } else if (state.config.daemon.restore_on_start) {
        for (output_node_t *out = state.outputs; out != NULL; out = out->next) {
            daemon_on_output_configured(&state, out);
        }
    }

    /* Apply security hardening (PR_SET_NO_NEW_PRIVS & Landlock LSM) */
    security_sandbox_apply(NULL);

    WAYWAL_LOG_INFO("wwald daemon running successfully (namespace: %s)", state.namespace_str);

    /* Main event loop */
    while (state.running) {
        wl_display_dispatch_pending(state.display);
        wl_display_flush(state.display);
        g_daemon_wl_ok = true;

        if (state.use_uring) {
            int ret = uring_loop_dispatch(&uring_loop, daemon_uring_event_callback);
            if (ret < 0 && ret != -EINTR) {
                WAYWAL_LOG_ERR("io_uring dispatch failed: %s", strerror(-ret));
                break;
            }
        } else {
            struct epoll_event events[16];
            int nfds = epoll_wait(state.epoll_fd, events, 16, -1);
            if (nfds < 0) {
                if (errno == EINTR)
                    continue;
                WAYWAL_LOG_ERR("epoll_wait failed: %s", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == wl_fd) {
                    daemon_pump_wayland_events(&state);
                    break;
                }
            }

            for (int i = 0; i < nfds; ++i) {
                int ev_fd = events[i].data.fd;
                if (ev_fd == wl_fd) {
                    continue;
                } else if (ev_fd == state.server_socket_fd) {
                    daemon_handle_ipc_connection(&state);
                } else if (ev_fd == state.transition_timer_fd) {
                    daemon_pump_wayland_events(&state);
                    transition_engine_dispatch_tick(&state);
                } else if (ev_fd == state.video_engine.timer_fd) {
                    daemon_pump_wayland_events(&state);
                    video_engine_dispatch_frame(&state.video_engine);
                } else if (ev_fd == state.slideshow.timer_fd) {
                    daemon_pump_wayland_events(&state);
                    slideshow_dispatch_tick(&state.slideshow);
                } else if (ev_fd == state.shader_timer_fd) {
                    daemon_pump_wayland_events(&state);
                    daemon_shader_dispatch_frame(&state);
                } else if (ev_fd == state.anim_timer_fd) {
                    daemon_pump_wayland_events(&state);
                    daemon_anim_dispatch_frame(&state);
                } else if (state.shader_eng &&
                           ev_fd == waywal_shader_engine_get_inotify_fd(state.shader_eng)) {
                    if (waywal_shader_engine_poll_hot_reload(state.shader_eng) &&
                        state.shader_active) {
                        daemon_shader_render_once(&state);
                    }
                } else if (state.power_gov &&
                           (ev_fd == waywal_power_governor_get_ipc_fd(state.power_gov) ||
                            ev_fd == waywal_power_governor_get_inotify_fd(state.power_gov))) {
                    daemon_handle_power_gov_events(&state, ev_fd);
                } else if (ev_fd == state.signal_fd) {
                    struct signalfd_siginfo fdsi;
                    ssize_t s = read(state.signal_fd, &fdsi, sizeof(fdsi));
                    if (s == (ssize_t)sizeof(fdsi)) {
                        if (fdsi.ssi_signo == SIGHUP) {
                            daemon_reload_config(&state);
                            continue;
                        }
                        WAYWAL_LOG_INFO("Received signal %u, shutting down cleanly",
                                        fdsi.ssi_signo);
                        state.running = false;
                    }
                }
            }
        }

        if (!g_daemon_wl_ok || wl_display_get_error(state.display) != 0) {
            WAYWAL_LOG_ERR("Wayland display encountered fatal error");
            break;
        }
        wl_display_dispatch_pending(state.display);

        arena_reset(&state.frame_arena);
    }

    WAYWAL_LOG_INFO("Shutting down wwald...");

    shader_engine_stop_playback(&state);
    anim_engine_stop_playback(&state);
    transition_engine_destroy(&state);
    video_engine_destroy(&state.video_engine);
    slideshow_destroy(&state.slideshow);
    if (state.shader_eng) {
        waywal_shader_engine_destroy(state.shader_eng);
        state.shader_eng = NULL;
    }
    if (state.audio_fft) {
        waywal_audio_fft_destroy(state.audio_fft);
        state.audio_fft = NULL;
    }
    if (state.power_gov) {
        waywal_power_governor_destroy(state.power_gov);
        state.power_gov = NULL;
    }
    if (state.shader_timer_fd >= 0) {
        close(state.shader_timer_fd);
        state.shader_timer_fd = -1;
    }
    if (state.anim_timer_fd >= 0) {
        close(state.anim_timer_fd);
        state.anim_timer_fd = -1;
    }

    if (state.use_uring) {
        uring_loop_destroy(&uring_loop);
        state.uring_loop = NULL;
    }

    if (!waywal_ipc_is_socket_activated()) {
        path_buf_t sock_path;
        if (waywal_ipc_resolve_socket_path(&sock_path, state.namespace_str)) {
            unlink(path_buf_cstr(&sock_path));
        }
    }

    close(state.server_socket_fd);
    close(state.epoll_fd);
    close(state.signal_fd);
    daemon_wayland_destroy(&state);
    arena_destroy(&state.frame_arena);
    arena_destroy(&state.persistent_arena);

    WAYWAL_LOG_INFO("Clean shutdown complete.");
    return 0;
}
