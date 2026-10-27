// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_compute.h"
#include "image_loader.h"
#include "wayland_core.h"
#include "waywal/anim_engine.h"
#include "waywal/arena.h"
#include "waywal/audio_fft.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/mpris.h"
#include "waywal/power_governor.h"
#include "waywal/security.h"
#include "waywal/shader_engine.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"
#include "waywal/uring_loop.h"
#include "waywal/we_scene.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
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

static void unregister_fd_from_loops(daemon_state_t *state, int fd)
{
    if (!state || fd < 0)
        return;
    if (state->epoll_fd >= 0) {
        (void)epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
    if (state->use_uring && state->uring_loop) {
        (void)uring_loop_remove_poll(state->uring_loop, fd);
    }
}

static bool output_matches_target_spec(const output_node_t *out, const char *target_spec)
{
    if (!out)
        return false;
    if (!target_spec || target_spec[0] == '\0' || strcmp(target_spec, "*") == 0)
        return true;

    const char *p = target_spec;
    while (*p) {
        while (*p == ',' || *p == ' ')
            p++;
        if (*p == '\0')
            break;
        const char *end = p;
        while (*end != '\0' && *end != ',')
            end++;
        size_t len = (size_t)(end - p);
        if (len == 1 && p[0] == '*')
            return true;
        if (strlen(out->name) == len && strncmp(out->name, p, len) == 0)
            return true;
        p = end;
    }
    return false;
}

static bool output_matches_img_meta(const output_node_t *out, const waywal_img_metadata_t *meta)
{
    if (!out)
        return false;
    if (!meta || meta->num_target_outputs == 0)
        return true;

    const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
    uint32_t remaining = meta->extra_data_len;
    for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
        if (remaining == 0)
            break;
        size_t len = strnlen(ptr, remaining);
        if (len == remaining)
            break;
        if (strcmp(ptr, "*") == 0 || strcmp(out->name, ptr) == 0)
            return true;
        ptr += len + 1;
        remaining -= (uint32_t)(len + 1);
    }
    return false;
}

static bool output_matches_slideshow_payload(const output_node_t *out,
                                             const waywal_slideshow_payload_t *spay)
{
    if (!out)
        return false;
    if (!spay || spay->num_target_outputs == 0)
        return true;
    uint32_t max_t = spay->num_target_outputs <= 16 ? spay->num_target_outputs : 16;
    for (uint32_t i = 0; i < max_t; ++i) {
        if (strcmp(spay->target_outputs[i], "*") == 0 ||
            strcmp(out->name, spay->target_outputs[i]) == 0) {
            return true;
        }
    }
    return false;
}

void reconcile_global_engine_flags(daemon_state_t *state)
{
    if (!state)
        return;

    if (!state->outputs) {
        if (!state->shader_active && !state->workshop_active) {
            disarm_timer_fd(state->shader_timer_fd);
            if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
                waywal_audio_fft_destroy(state->audio_fft);
                state->audio_fft = waywal_audio_fft_create(48000, false);
                if (state->audio_fft) {
                    waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
                    waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
                }
            }
        }
        if (!state->anim_active) {
            disarm_timer_fd(state->anim_timer_fd);
        }
        return;
    }

    bool any_shader = false;
    bool any_workshop = false;
    bool any_audio = false;
    uint32_t max_shader_fps = 0;
    bool any_anim = false;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if ((out->shader_active && out->shader_eng) || (out->workshop_active && out->we_scene)) {
            any_shader = true;
            if (out->workshop_active)
                any_workshop = true;
            if (out->shader_audio_reactive ||
                (out->workshop_active && out->we_scene && out->we_scene->uses_audio)) {
                any_audio = true;
            }
            uint32_t fps = out->shader_fps > 0 ? out->shader_fps : 60U;
            if (fps > max_shader_fps)
                max_shader_fps = fps;
        }
        if (out->anim_active && out->anim_dec) {
            any_anim = true;
        }
    }

    bool was_shader_active = state->shader_active;
    state->shader_active = any_shader;
    state->workshop_active = any_workshop;
    if (any_shader) {
        state->shader_fps = max_shader_fps > 0 ? max_shader_fps : 60U;
    } else {
        disarm_timer_fd(state->shader_timer_fd);
    }

    if (any_audio) {
        if (!state->audio_fft || !waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
            if (state->audio_fft) {
                waywal_audio_fft_destroy(state->audio_fft);
            }
            state->audio_fft = waywal_audio_fft_create(48000, true);
        }
        if (state->audio_fft) {
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    } else if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
        waywal_audio_fft_destroy(state->audio_fft);
        state->audio_fft = waywal_audio_fft_create(48000, false);
        if (state->audio_fft) {
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    }

    if (was_shader_active != any_shader) {
        layer_shell_update_input_regions(state, any_shader);
    }

    state->anim_active = any_anim;
    if (!any_anim) {
        disarm_timer_fd(state->anim_timer_fd);
    }
}

static void destroy_output_we_scene(daemon_state_t *state, output_node_t *out)
{
    if (!out || !out->we_scene)
        return;
    we_scene_t *ws = out->we_scene;
    out->we_scene = NULL;
    bool shared = false;
    if (state) {
        for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
            if (o != out && o->we_scene == ws) {
                shared = true;
                break;
            }
        }
        if (state->we_scene == ws) {
            state->we_scene = NULL;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->we_scene) {
                    state->we_scene = o->we_scene;
                    break;
                }
            }
        }
    }
    if (!shared) {
        we_scene_destroy(ws);
    }
}

static void stop_engines_on_output(daemon_state_t *state, output_node_t *out, bool keep_shader,
                                   bool keep_anim, bool keep_video, bool keep_slideshow)
{
    if (!state || !out)
        return;

    if (!keep_shader) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        destroy_output_we_scene(state, out);
    }

    if (!keep_anim) {
        out->anim_active = false;
        if (out->anim_dec) {
            waywal_anim_decoder_t *dec = out->anim_dec;
            out->anim_dec = NULL;
            bool shared = false;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->anim_dec == dec) {
                    shared = true;
                    break;
                }
            }
            if (!shared) {
                if (state->anim_dec == dec)
                    state->anim_dec = NULL;
                waywal_anim_destroy(dec);
            }
        }
    }

    if (!keep_video) {
        video_engine_t *ve = out->video_eng;
        out->video_eng = NULL;
        out->video_targeted = false;
        if (ve) {
            bool any_other = false;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->video_eng == ve) {
                    any_other = true;
                    break;
                }
            }
            if (!any_other) {
                video_engine_stop(ve);
                if (ve != &state->video_engine) {
                    if (ve->timer_fd >= 0) {
                        unregister_fd_from_loops(state, ve->timer_fd);
                    }
                    video_engine_destroy(ve);
                    free(ve);
                }
            } else if (ve == &state->video_engine) {
                ve->has_target_filter = true;
            }
        } else if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            if (state->video_engine.target_output == out) {
                video_engine_stop(&state->video_engine);
            } else {
                state->video_engine.has_target_filter = true;
                bool any_other = false;
                for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                    if (o != out && (o->video_eng == &state->video_engine || o->video_targeted)) {
                        any_other = true;
                        break;
                    }
                }
                if (!any_other) {
                    video_engine_stop(&state->video_engine);
                }
            }
        }
    }

    if (!keep_slideshow) {
        slideshow_engine_t *ss = out->slideshow_eng;
        out->slideshow_eng = NULL;
        if (ss) {
            bool any_other = false;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->slideshow_eng == ss) {
                    any_other = true;
                    break;
                }
            }
            if (!any_other) {
                slideshow_stop(ss);
                if (ss != &state->slideshow) {
                    if (ss->timer_fd >= 0) {
                        unregister_fd_from_loops(state, ss->timer_fd);
                    }
                    slideshow_destroy(ss);
                    free(ss);
                }
            } else {
                uint32_t rem_count = 0;
                memset(ss->target_outputs, 0, sizeof(ss->target_outputs));
                for (output_node_t *o = state->outputs; o != NULL && rem_count < 16; o = o->next) {
                    if (o->slideshow_eng == ss && o->name[0] != '\0') {
                        strncpy(ss->target_outputs[rem_count], o->name,
                                sizeof(ss->target_outputs[rem_count]) - 1);
                        rem_count++;
                    }
                }
                if (rem_count > 0) {
                    ss->num_target_outputs = rem_count;
                }
            }
        }
    }
}

static void shader_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        destroy_output_we_scene(state, out);
    }
    if (state->we_scene) {
        we_scene_destroy(state->we_scene);
        state->we_scene = NULL;
    }
    bool was_active = state->shader_active;
    state->shader_active = false;
    state->workshop_active = false;
    disarm_timer_fd(state->shader_timer_fd);
    if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
        waywal_audio_fft_destroy(state->audio_fft);
        state->audio_fft = waywal_audio_fft_create(48000, false);
        if (state->audio_fft) {
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    }
    if (was_active) {
        layer_shell_update_input_regions(state, false);
    }
}

static void anim_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        stop_engines_on_output(state, out, true, false, true, true);
    }
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

    bool has_entry = (strstr(src, "mainImage") != NULL || strstr(src, "void main") != NULL);
    bool valid_syntax = true;
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

    if (!state->outputs) {
        waywal_anim_decoder_t *dec = waywal_anim_create(buf, sz);
        free(buf);
        if (!dec || waywal_anim_frame_count(dec) <= 1) {
            if (dec)
                waywal_anim_destroy(dec);
            return false;
        }
        const uint8_t *f_rgba = NULL;
        uint32_t fw = 0, fh = 0, delay_ms = 100;
        if (!waywal_anim_next_frame(dec, &f_rgba, &fw, &fh, &delay_ms)) {
            waywal_anim_destroy(dec);
            return false;
        }
        anim_engine_stop_playback(state);
        state->anim_dec = dec;
        state->anim_active = true;
        if (meta)
            state->anim_meta = *meta;
        if (!state->power_gov || !waywal_power_governor_should_pause_all(state->power_gov)) {
            arm_oneshot_timer_ms(state->anim_timer_fd, delay_ms > 0 ? delay_ms : 100);
        }
        return true;
    }

    uint64_t t_now = now_ns();
    uint32_t min_delay_ms = UINT32_MAX;
    bool started_any = false;
    uint32_t total_frames = 0;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        bool matches = (meta && meta->num_target_outputs > 0)
                           ? output_matches_img_meta(out, meta)
                           : output_matches_target_spec(out, target_out);
        if (!matches)
            continue;

        waywal_anim_decoder_t *dec = waywal_anim_create(buf, sz);
        if (!dec || waywal_anim_frame_count(dec) <= 1) {
            if (dec)
                waywal_anim_destroy(dec);
            continue;
        }
        total_frames = waywal_anim_frame_count(dec);
        const uint8_t *f_rgba = NULL;
        uint32_t fw = 0, fh = 0, delay_ms = 100;
        if (!waywal_anim_next_frame(dec, &f_rgba, &fw, &fh, &delay_ms)) {
            waywal_anim_destroy(dec);
            continue;
        }

        stop_engines_on_output(state, out, false, false, false, true);
        out->anim_dec = dec;
        out->anim_active = true;
        out->anim_delay_ms = delay_ms > 0 ? delay_ms : 100U;
        out->anim_next_ns = t_now + (uint64_t)out->anim_delay_ms * 1000000ULL;
        if (meta) {
            out->anim_meta = *meta;
            state->anim_meta = *meta;
        }
        if (out->anim_delay_ms < min_delay_ms)
            min_delay_ms = out->anim_delay_ms;
        if (!state->anim_dec)
            state->anim_dec = dec;
        started_any = true;
    }
    free(buf);

    if (!started_any)
        return false;

    state->anim_target_output[0] = '\0';
    if (target_out && target_out[0] != '\0') {
        strncpy(state->anim_target_output, target_out, sizeof(state->anim_target_output) - 1);
    }
    reconcile_global_engine_flags(state);

    if (!state->power_gov || !waywal_power_governor_should_pause_all(state->power_gov)) {
        arm_oneshot_timer_ms(state->anim_timer_fd,
                             min_delay_ms != UINT32_MAX ? min_delay_ms : 100U);
    }
    WAYWAL_LOG_INFO("Started native animation playback for '%s' (%u frames)", filepath,
                    total_frames);
    return true;
}

static void daemon_anim_dispatch_frame(daemon_state_t *state)
{
    if (!state || state->anim_timer_fd < 0)
        return;
    drain_timer_fd(state->anim_timer_fd);
    if (!state->anim_active)
        return;

    if (state->power_gov && waywal_power_governor_should_pause_all(state->power_gov)) {
        disarm_timer_fd(state->anim_timer_fd);
        return;
    }

    uint64_t t_now = now_ns();
    uint32_t next_arm_ms = UINT32_MAX;
    bool any_active = false;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->anim_active || !out->anim_dec)
            continue;
        any_active = true;

        if (state->power_gov &&
            waywal_power_governor_should_pause_output(state->power_gov, out->name)) {
            if (250U < next_arm_ms)
                next_arm_ms = 250U;
            continue;
        }

        if (out->transition_active) {
            if (33U < next_arm_ms)
                next_arm_ms = 33U;
            continue;
        }

        if (t_now + 2000000ULL < out->anim_next_ns) {
            uint32_t rem_ms = (uint32_t)((out->anim_next_ns - t_now) / 1000000ULL);
            if (rem_ms == 0)
                rem_ms = 1U;
            if (rem_ms < next_arm_ms)
                next_arm_ms = rem_ms;
            continue;
        }

        const uint8_t *rgba = NULL;
        uint32_t w = 0, h = 0, delay_ms = 100;
        if (!waywal_anim_next_frame(out->anim_dec, &rgba, &w, &h, &delay_ms) || !rgba || w == 0 ||
            h == 0) {
            continue;
        }

        size_t px_bytes = (size_t)w * (size_t)h * 4U;
        uint8_t *bgra = (uint8_t *)arena_alloc(&state->frame_arena, px_bytes, 32U);
        bool heap_bgra = false;
        if (!bgra) {
            bgra = (uint8_t *)malloc(px_bytes);
            if (!bgra)
                continue;
            heap_bgra = true;
        }

        size_t count = (size_t)w * (size_t)h;
        for (size_t i = 0; i < count; ++i) {
            bgra[i * 4 + 0] = rgba[i * 4 + 2];
            bgra[i * 4 + 1] = rgba[i * 4 + 1];
            bgra[i * 4 + 2] = rgba[i * 4 + 0];
            bgra[i * 4 + 3] = rgba[i * 4 + 3];
        }

        render_engine_bump_upload_gen(&state->render_engine);
        output_node_render_image(state, out, bgra, w, h);

        if (heap_bgra)
            free(bgra);

        uint32_t next_delay = delay_ms > 0 ? delay_ms : 100U;
        if (state->power_gov && waywal_power_governor_is_on_battery(state->power_gov) &&
            next_delay < 33U) {
            next_delay = 33U;
        }
        out->anim_delay_ms = next_delay;
        out->anim_next_ns = t_now + (uint64_t)next_delay * 1000000ULL;
        if (next_delay < next_arm_ms)
            next_arm_ms = next_delay;
    }

    if (!any_active) {
        state->anim_active = false;
        disarm_timer_fd(state->anim_timer_fd);
    } else {
        arm_oneshot_timer_ms(state->anim_timer_fd, next_arm_ms != UINT32_MAX ? next_arm_ms : 100U);
    }
}

static void daemon_shader_render_once(daemon_state_t *state);

static const char *scaling_mode_to_str(uint32_t mode)
{
    switch ((waywal_scaling_mode_t)mode) {
    case WAYWAL_SCALING_FIT:
        return "fit";
    case WAYWAL_SCALING_CENTER:
        return "center";
    case WAYWAL_SCALING_STRETCH:
        return "stretch";
    case WAYWAL_SCALING_TILE:
        return "tile";
    case WAYWAL_SCALING_SPAN:
        return "span";
    case WAYWAL_SCALING_FILL:
    default:
        return "fill";
    }
}

static double json_safe_float(float v) { return isfinite(v) ? (double)v : 0.0; }

__attribute__((format(printf, 4, 5))) static void buf_appendf(char *buf, size_t cap, size_t *off,
                                                              const char *fmt, ...)
{
    if (!buf || !off || *off + 1 >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= cap - *off) {
        *off = cap - 1;
        buf[*off] = '\0';
    } else {
        *off += (size_t)n;
    }
}

static size_t daemon_build_status_json(daemon_state_t *state, const char *event_name, char *buf,
                                       size_t cap)
{
    if (!state || !buf || cap == 0) {
        return 0;
    }

    unsigned long total_pages = 0, rss_pages = 0;
    FILE *statm = fopen("/proc/self/statm", "r");
    if (statm) {
        (void)fscanf(statm, "%lu %lu", &total_pages, &rss_pages);
        fclose(statm);
    }
    long page_sz = sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) {
        page_sz = 4096;
    }
    unsigned long rss_kib = (rss_pages * (unsigned long)page_sz) / 1024UL;

    char esc_ns[128];
    waywal_json_escape_string(state->namespace_str, esc_ns, sizeof(esc_ns));

    bool on_battery =
        state->power_gov ? waywal_power_governor_is_on_battery(state->power_gov) : false;
    waywal_battery_policy_t bpol = state->config.daemon.battery_policy_mode;
    const char *bpol_str = "reduce-fps";
    if (bpol == WAYWAL_BATTERY_IGNORE) {
        bpol_str = "ignore";
    } else if (bpol == WAYWAL_BATTERY_PAUSE_VIDEO) {
        bpol_str = "pause-video";
    }
    uint32_t global_pmask =
        state->power_gov ? waywal_power_governor_get_output_mask(state->power_gov, NULL) : 0;

    if (!state->shader_active && state->audio_fft) {
        (void)waywal_audio_fft_update_ducking(state->audio_fft, 1.0f);
    }

    bool pw_active =
        state->audio_fft ? waywal_audio_fft_is_pipewire_active(state->audio_fft) : false;
    float vol =
        state->audio_fft ? waywal_audio_fft_get_volume(state->audio_fft) : state->audio_volume;
    float eff_gain = state->audio_fft ? waywal_audio_fft_get_effective_gain(state->audio_fft) : vol;
    bool mute_on = state->audio_fft ? waywal_audio_fft_get_mute_on_audio(state->audio_fft)
                                    : state->mute_on_audio;
    bool ext_audio =
        state->audio_fft ? waywal_audio_fft_is_external_audio_active(state->audio_fft) : false;
    float env = 0.0f;
    float beat = 0.0f;
    if (state->audio_fft) {
        const waywal_audio_frame_t *af = waywal_audio_fft_get_frame(state->audio_fft);
        if (af) {
            env = af->envelope;
            beat = af->beat_pulse;
        }
    }

    char pal_pri[16] = "#000000";
    char pal_sec[16] = "#000000";
    char pal_ter[16] = "#000000";
    char pal_bg[16] = "#000000";
    char pal_fg[16] = "#ffffff";
    if (state->has_active_palette) {
        snprintf(pal_pri, sizeof(pal_pri), "#%02x%02x%02x", state->active_palette.primary.r,
                 state->active_palette.primary.g, state->active_palette.primary.b);
        snprintf(pal_sec, sizeof(pal_sec), "#%02x%02x%02x", state->active_palette.secondary.r,
                 state->active_palette.secondary.g, state->active_palette.secondary.b);
        snprintf(pal_ter, sizeof(pal_ter), "#%02x%02x%02x", state->active_palette.tertiary.r,
                 state->active_palette.tertiary.g, state->active_palette.tertiary.b);
        snprintf(pal_bg, sizeof(pal_bg), "#%02x%02x%02x", state->active_palette.background.r,
                 state->active_palette.background.g, state->active_palette.background.b);
        snprintf(pal_fg, sizeof(pal_fg), "#%02x%02x%02x", state->active_palette.onBackground.r,
                 state->active_palette.onBackground.g, state->active_palette.onBackground.b);
    }

    const waywal_mpris_state_t *ms = state->mpris ? waywal_mpris_get_state(state->mpris) : NULL;
    const char *mstatus = "Stopped";
    bool mplaying = false;
    int64_t mlen_us = 0;
    char esc_player[256] = "";
    char esc_title[512] = "";
    char esc_artist[512] = "";
    char esc_album[512] = "";
    char esc_art_url[1024] = "";
    char esc_art_path[1024] = "";
    bool has_art_tex = false;
    if (ms) {
        mplaying = (ms->status == WAYWAL_MPRIS_PLAYING) || ms->playing;
        mlen_us = ms->length_us;
        if (ms->status == WAYWAL_MPRIS_PLAYING) {
            mstatus = "Playing";
        } else if (ms->status == WAYWAL_MPRIS_PAUSED) {
            mstatus = "Paused";
        }
        waywal_json_escape_string(ms->player_name, esc_player, sizeof(esc_player));
        waywal_json_escape_string(ms->title, esc_title, sizeof(esc_title));
        waywal_json_escape_string(ms->artist, esc_artist, sizeof(esc_artist));
        waywal_json_escape_string(ms->album, esc_album, sizeof(esc_album));
        waywal_json_escape_string(ms->art_url, esc_art_url, sizeof(esc_art_url));
        waywal_json_escape_string(ms->art_local_path, esc_art_path, sizeof(esc_art_path));
        has_art_tex = ms->has_art_pixels;
    }

    size_t off = 0;
    buf[0] = '\0';
    if (event_name && event_name[0] != '\0') {
        char esc_ev[128];
        waywal_json_escape_string(event_name, esc_ev, sizeof(esc_ev));
        buf_appendf(buf, cap, &off, "{\"event\":\"%s\",", esc_ev);
    } else {
        buf_appendf(buf, cap, &off, "{");
    }

    buf_appendf(
        buf, cap, &off,
        "\"pid\":%d,"
        "\"namespace\":\"%s\","
        "\"gpu_compute\":%s,"
        "\"dmabuf_scanout\":%s,"
        "\"ten_bit\":%s,"
        "\"nv12\":%s,"
        "\"rss_kib\":%lu,"
        "\"power\":{\"on_battery\":%s,\"battery_policy\":\"%s\",\"global_pause_mask\":%u},"
        "\"audio\":{\"pipewire_active\":%s,\"volume\":%.4f,\"effective_gain\":%.4f,"
        "\"mute_on_audio\":%s,\"external_active\":%s,\"external_audio_active\":%s,"
        "\"envelope\":%.4f,\"beat\":%.4f},"
        "\"palette\":{\"active\":%s,\"theme_from_media\":%s,\"primary\":\"%s\","
        "\"secondary\":\"%s\",\"tertiary\":\"%s\",\"background\":\"%s\",\"foreground\":\"%s\"},"
        "\"mpris\":{\"playing\":%s,\"status\":\"%s\",\"player\":\"%s\",\"title\":\"%s\","
        "\"artist\":\"%s\",\"album\":\"%s\",\"art_url\":\"%s\",\"art_path\":\"%s\","
        "\"has_art_texture\":%s,\"length_us\":%lld,\"theme_from_media\":%s},"
        "\"outputs\":[",
        (int)getpid(), esc_ns, state->render_engine.gpu_compute_active ? "true" : "false",
        state->dmabuf_ctx.available ? "true" : "false",
        state->dmabuf_ctx.has_10bit ? "true" : "false",
        state->dmabuf_ctx.has_nv12 ? "true" : "false", rss_kib, on_battery ? "true" : "false",
        bpol_str, global_pmask, pw_active ? "true" : "false", json_safe_float(vol),
        json_safe_float(eff_gain), mute_on ? "true" : "false", ext_audio ? "true" : "false",
        ext_audio ? "true" : "false", json_safe_float(env), json_safe_float(beat),
        state->has_active_palette ? "true" : "false", state->theme_from_media ? "true" : "false",
        pal_pri, pal_sec, pal_ter, pal_bg, pal_fg, mplaying ? "true" : "false", mstatus, esc_player,
        esc_title, esc_artist, esc_album, esc_art_url, esc_art_path, has_art_tex ? "true" : "false",
        (long long)mlen_us, state->theme_from_media ? "true" : "false");

    bool first_out = true;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (off + 64 >= cap) {
            break;
        }
        uint32_t pmask = state->power_gov
                             ? waywal_power_governor_get_output_mask(state->power_gov, out->name)
                             : 0;
        int32_t fscale =
            out->fractional_scale > 0 ? out->fractional_scale : (out->scale_factor * 120);
        const char *eng_kind = "static";
        const char *mpath = "";
        uint32_t fps = 0;
        if (out->workshop_active) {
            eng_kind = "workshop";
            mpath = out->workshop_dir[0] ? out->workshop_dir : state->workshop_dir;
            fps = out->shader_fps > 0 ? out->shader_fps : state->shader_fps;
        } else if (out->shader_active) {
            eng_kind = "shader";
            mpath = out->shader_path[0] ? out->shader_path : state->shader_path;
            fps = out->shader_fps > 0 ? out->shader_fps : state->shader_fps;
        } else if (out->video_eng) {
            eng_kind = "video";
            mpath = out->video_eng->filepath;
            fps = 60;
        } else if (out->slideshow_eng) {
            eng_kind = "slideshow";
            mpath = out->slideshow_eng->root_path;
        } else if (out->anim_active) {
            eng_kind = "animation";
            mpath = out->anim_meta.filepath;
        } else {
            const waywal_output_saved_state_t *sv =
                waywal_state_store_find_output(&g_saved_state, out->name);
            if (!sv) {
                sv = waywal_state_store_find_output(&g_saved_state, "*");
            }
            if (sv) {
                mpath = sv->media_path;
            }
        }
        char esc_oname[128];
        char esc_odesc[256];
        char esc_mpath[1024];
        waywal_json_escape_string(out->name[0] ? out->name : "unknown", esc_oname,
                                  sizeof(esc_oname));
        waywal_json_escape_string(out->description[0] ? out->description : "unknown", esc_odesc,
                                  sizeof(esc_odesc));
        waywal_json_escape_string(mpath, esc_mpath, sizeof(esc_mpath));

        double scale_val = json_safe_float(fscale > 0 ? ((float)fscale / 120.0f) : 1.0f);
        char out_entry[1536];
        size_t entry_off = 0;
        buf_appendf(out_entry, sizeof(out_entry), &entry_off,
                    "%s{\"name\":\"%s\",\"description\":\"%s\",\"width\":%d,\"height\":%d,"
                    "\"logical_width\":%u,\"logical_height\":%u,\"scale\":%.4f,\"scale_120\":%d,"
                    "\"vblank_ns\":%lld,\"paused\":%s,\"pause_mask\":%u,\"engine\":\"%s\","
                    "\"asset\":\"%s\",\"media_path\":\"%s\",\"fps\":%u,\"scaling\":\"%s\"}",
                    first_out ? "" : ",", esc_oname, esc_odesc, out->width, out->height,
                    out->logical_width, out->logical_height, scale_val, fscale,
                    (long long)out->pres_sync.pll.period_est_ns, (pmask != 0) ? "true" : "false",
                    pmask, eng_kind, esc_mpath, esc_mpath, fps,
                    scaling_mode_to_str(out->scaling_mode));
        if (entry_off + 1 >= sizeof(out_entry) || off + entry_off + 4 >= cap) {
            break;
        }
        memcpy(buf + off, out_entry, entry_off);
        off += entry_off;
        buf[off] = '\0';
        first_out = false;
    }

    buf_appendf(buf, cap, &off, "]}\n");
    return off;
}

static void daemon_reap_dead_subscribers(daemon_state_t *state)
{
    if (!state) {
        return;
    }
    size_t i = 0;
    while (i < state->num_subscribers) {
        int sfd = state->subscriber_fds[i];
        bool dead = (sfd < 0);
        if (!dead) {
            struct pollfd pfd = {.fd = sfd, .events = POLLIN | POLLERR | POLLHUP};
            if (poll(&pfd, 1, 0) > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    dead = true;
                } else if (pfd.revents & POLLIN) {
                    char b = 0;
                    ssize_t r = recv(sfd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (r == 0 ||
                        (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        dead = true;
                    }
                }
            }
        }
        if (dead) {
            if (sfd >= 0) {
                close(sfd);
            }
            for (size_t j = i + 1; j < state->num_subscribers; ++j) {
                state->subscriber_fds[j - 1] = state->subscriber_fds[j];
            }
            state->num_subscribers--;
        } else {
            ++i;
        }
    }
}

static void daemon_broadcast_event(daemon_state_t *state, const char *event_name)
{
    if (!state) {
        return;
    }
    daemon_reap_dead_subscribers(state);
    if (state->num_subscribers == 0) {
        return;
    }
    char json_buf[16384];
    size_t len = daemon_build_status_json(state, event_name ? event_name : "update", json_buf,
                                          sizeof(json_buf));
    if (len == 0) {
        return;
    }

    waywal_ipc_hdr_t hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_RESP_EVENT,
        .payload_size = (uint32_t)len,
    };

    size_t i = 0;
    while (i < state->num_subscribers) {
        int sfd = state->subscriber_fds[i];
        bool ok = false;
        if (sfd >= 0) {
            struct iovec iov[2] = {
                {.iov_base = &hdr, .iov_len = sizeof(hdr)},
                {.iov_base = json_buf, .iov_len = len},
            };
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = iov;
            msg.msg_iovlen = 2;
            ssize_t w = sendmsg(sfd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w == (ssize_t)(sizeof(hdr) + len)) {
                ok = true;
            }
        }
        if (!ok) {
            if (sfd >= 0) {
                close(sfd);
            }
            for (size_t j = i + 1; j < state->num_subscribers; ++j) {
                state->subscriber_fds[j - 1] = state->subscriber_fds[j];
            }
            state->num_subscribers--;
        } else {
            ++i;
        }
    }
}

static void daemon_apply_mpris_album_art_and_theme(daemon_state_t *state,
                                                   const waywal_mpris_state_t *mstate,
                                                   bool force_art_upload)
{
    if (!state || !mstate) {
        return;
    }

    bool art_changed = state->mpris ? waywal_mpris_consume_art_changed(state->mpris) : false;
    bool upload_art = art_changed || force_art_upload;
    bool uploaded_gl = false;

    float playing = (mstate->status == WAYWAL_MPRIS_PLAYING) ? 1.0f : 0.0f;
    const uint8_t *art_bgra = mstate->has_art_pixels ? mstate->art_bgra : NULL;
    uint32_t aw = mstate->has_art_pixels ? mstate->art_width : 0;
    uint32_t ah = mstate->has_art_pixels ? mstate->art_height : 0;

    if (state->shader_eng) {
        waywal_shader_engine_set_media_playing(state->shader_eng, playing);
        if (upload_art) {
            waywal_shader_engine_set_album_art_bgra(state->shader_eng, art_bgra, aw, ah);
            uploaded_gl = true;
        }
    }
    if (state->we_scene) {
        we_scene_set_media_playing(state->we_scene, playing);
        if (upload_art) {
            we_scene_set_album_art_bgra(state->we_scene, art_bgra, aw, ah);
            uploaded_gl = true;
        }
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && out->shader_eng != state->shader_eng) {
            waywal_shader_engine_set_media_playing(out->shader_eng, playing);
            if (upload_art) {
                waywal_shader_engine_set_album_art_bgra(out->shader_eng, art_bgra, aw, ah);
                uploaded_gl = true;
            }
        }
        if (out->we_scene && out->we_scene != state->we_scene) {
            we_scene_set_media_playing(out->we_scene, playing);
            if (upload_art) {
                we_scene_set_album_art_bgra(out->we_scene, art_bgra, aw, ah);
                uploaded_gl = true;
            }
        }
    }
    if (uploaded_gl) {
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    }

    waywal_scheme_variant_t variant =
        waywal_scheme_variant_from_str(state->config.theme.scheme_variant);
    waywal_theme_mode_t tmode = state->config.theme.theme_mode;
    bool updated_theme = false;

    if (state->theme_from_media && art_bgra && aw > 0 && ah > 0 &&
        mstate->status != WAYWAL_MPRIS_STOPPED) {
        if (art_changed || force_art_upload || !state->media_palette_active) {
            if (waywal_palette_extract_ex(art_bgra, aw, ah, aw * 4U, true, tmode, variant,
                                          state->config.theme.contrast, &state->active_palette)) {
                state->has_active_palette = true;
                state->media_palette_active = true;
                updated_theme = true;
            }
        }
    } else if (state->media_palette_active) {
        state->media_palette_active = false;
        if (state->wallpaper_rgba && state->wallpaper_w > 0 && state->wallpaper_h > 0) {
            if (waywal_palette_extract_ex(state->wallpaper_rgba, state->wallpaper_w,
                                          state->wallpaper_h, state->wallpaper_w * 4U, true, tmode,
                                          variant, state->config.theme.contrast,
                                          &state->active_palette)) {
                state->has_active_palette = true;
                updated_theme = true;
            }
        }
    }

    if (updated_theme) {
        if (state->config.theme.enabled) {
            (void)waywal_palette_export_filtered(
                &state->active_palette, NULL, state->config.theme.caelestia_sync,
                state->config.theme.export_css, state->config.theme.export_hyprland);
            if (state->config.theme.tty_sequences) {
                (void)waywal_palette_broadcast_tty(&state->active_palette);
            }
            if (state->config.hooks.on_theme_change[0] != '\0') {
                (void)waywal_palette_run_hook(state->config.hooks.on_theme_change);
            }
        }
        daemon_broadcast_event(state, "theme_changed");
    }
}

static void daemon_mpris_on_change(const waywal_mpris_state_t *mstate, void *userdata)
{
    daemon_state_t *state = (daemon_state_t *)userdata;
    if (!state || !mstate) {
        return;
    }
    daemon_apply_mpris_album_art_and_theme(state, mstate, false);
    daemon_broadcast_event(state, "mpris_changed");
    if (state->shader_active) {
        daemon_shader_render_once(state);
    }
}

static void daemon_update_palette_from_bgra(daemon_state_t *state, const uint8_t *bgra_pixels,
                                            uint32_t w, uint32_t h,
                                            const waywal_img_metadata_t *meta,
                                            bool export_and_hooks)
{
    if (!state || !bgra_pixels || w == 0 || h == 0)
        return;

    size_t byte_len = (size_t)w * (size_t)h * 4U;
    if (!state->wallpaper_rgba || state->wallpaper_w != w || state->wallpaper_h != h) {
        uint8_t *buf = (uint8_t *)malloc(byte_len);
        if (buf) {
            free(state->wallpaper_rgba);
            state->wallpaper_rgba = buf;
            state->wallpaper_w = w;
            state->wallpaper_h = h;
        }
    }
    if (state->wallpaper_rgba && state->wallpaper_w == w && state->wallpaper_h == h) {
        memcpy(state->wallpaper_rgba, bgra_pixels, byte_len);
    }
    bool uploaded_gl = false;
    if (state->shader_eng) {
        waywal_shader_engine_set_wallpaper_bgra(state->shader_eng, bgra_pixels, w, h);
        uploaded_gl = true;
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && out->shader_eng != state->shader_eng) {
            waywal_shader_engine_set_wallpaper_bgra(out->shader_eng, bgra_pixels, w, h);
            uploaded_gl = true;
        }
    }
    if (uploaded_gl) {
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    }

    if (meta && meta->no_theme)
        return;

    if (state->theme_from_media && state->media_palette_active)
        return;

    waywal_scheme_variant_t variant =
        waywal_scheme_variant_from_str(state->config.theme.scheme_variant);
    waywal_theme_mode_t tmode =
        (meta && meta->theme_light_mode) ? WAYWAL_THEME_LIGHT : state->config.theme.theme_mode;
    if (waywal_palette_extract_ex(bgra_pixels, w, h, w * 4U, true, tmode, variant,
                                  state->config.theme.contrast, &state->active_palette)) {
        state->has_active_palette = true;

        bool should_export =
            export_and_hooks && ((meta && meta->extract_theme) || state->config.theme.enabled);
        if (should_export) {
            (void)waywal_palette_export_filtered(
                &state->active_palette, NULL, state->config.theme.caelestia_sync,
                state->config.theme.export_css, state->config.theme.export_hyprland);
            if (state->config.theme.tty_sequences) {
                (void)waywal_palette_broadcast_tty(&state->active_palette);
            }
            if (state->config.hooks.on_theme_change[0] != '\0') {
                (void)waywal_palette_run_hook(state->config.hooks.on_theme_change);
            }
        }
        daemon_broadcast_event(state, "theme_changed");
    }
}

static void daemon_on_wallpaper_loaded(daemon_state_t *state, const uint8_t *bgra_pixels,
                                       uint32_t w, uint32_t h, const waywal_img_metadata_t *meta)
{
    daemon_update_palette_from_bgra(state, bgra_pixels, w, h, meta, true);
}

static bool is_shader_eng_attached_to_any_output(const daemon_state_t *state,
                                                 const waywal_shader_engine_t *eng)
{
    if (!state || !eng)
        return false;
    for (const output_node_t *o = state->outputs; o != NULL; o = o->next) {
        if (o->shader_eng == eng)
            return true;
    }
    return false;
}

static bool daemon_activate_depth_parallax(daemon_state_t *state, const uint8_t *bgra_pixels,
                                           uint32_t w, uint32_t h,
                                           const waywal_img_metadata_t *meta,
                                           const char *target_output)
{
    if (!state || !bgra_pixels || !meta || meta->depth_map_path[0] == '\0' || w == 0 || h == 0)
        return false;
    if (!state->dmabuf_ctx.available)
        return false;

    uint8_t *depth_bgra = NULL;
    uint32_t dw = 0, dh = 0;
    if (!image_load(meta->depth_map_path, &depth_bgra, &dw, &dh) || !depth_bgra || dw == 0 ||
        dh == 0) {
        return false;
    }

    bool any_ok = false;
    uint64_t t_now = now_ns();
    float strength = meta->depth_strength > 0.0f ? meta->depth_strength : 1.0f;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        bool matches = (meta->num_target_outputs > 0)
                           ? output_matches_img_meta(out, meta)
                           : output_matches_target_spec(out, target_output);
        if (!matches)
            continue;
        if (!out->shader_eng) {
            render_engine_make_current(&state->render_engine);
            out->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
            if (!state->shader_eng) {
                state->shader_eng = out->shader_eng;
            } else if (out->shader_eng) {
                waywal_shader_engine_copy_custom_uniforms(out->shader_eng, state->shader_eng);
                if (!is_shader_eng_attached_to_any_output(state, state->shader_eng)) {
                    waywal_shader_engine_destroy(state->shader_eng);
                    state->shader_eng = out->shader_eng;
                }
            }
        }
        if (!out->shader_eng)
            continue;
        waywal_shader_engine_set_depth_strength(out->shader_eng, strength);
        if (waywal_shader_engine_load_depth_parallax_bgra(out->shader_eng, bgra_pixels, w, h,
                                                          depth_bgra, dw, dh)) {
            out->shader_active = true;
            out->shader_audio_reactive = false;
            out->shader_fps = meta->transition_fps > 0 ? meta->transition_fps : 60U;
            out->shader_frame_idx = 0;
            out->shader_start_ns = t_now;
            out->shader_last_ns = t_now;
            any_ok = true;
        }
    }

    image_free(depth_bgra);

    if (!any_ok)
        return false;

    memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
    strncpy(state->shader_target_output, (target_output && target_output[0]) ? target_output : "*",
            sizeof(state->shader_target_output) - 1);
    reconcile_global_engine_flags(state);
    if (state->mpris) {
        daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris), true);
    }
    gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    daemon_shader_render_once(state);
    daemon_reconcile_power_state(state);
    return true;
}

static void daemon_shader_render_once(daemon_state_t *state)
{
    if (!state || !state->render_engine.gpu_compute_active)
        return;

    uint64_t t_now = now_ns();
    if (state->shader_start_ns == 0) {
        state->shader_start_ns = t_now;
        state->shader_last_ns = t_now;
    }

    float global_dt = (float)(t_now - state->shader_last_ns) * 1e-9f;
    if (global_dt <= 0.0f || global_dt > 0.25f) {
        global_dt = 1.0f / (float)(state->shader_fps > 0 ? state->shader_fps : 60);
    }
    state->shader_last_ns = t_now;

    waywal_shader_frame_params_t base_params;
    memset(&base_params, 0, sizeof(base_params));

    struct timespec wall_ts;
    clock_gettime(CLOCK_REALTIME, &wall_ts);
    struct tm tm_val;
    if (localtime_r(&wall_ts.tv_sec, &tm_val)) {
        base_params.date[0] = (float)(tm_val.tm_year + 1900);
        base_params.date[1] = (float)tm_val.tm_mon;
        base_params.date[2] = (float)tm_val.tm_mday;
        base_params.date[3] = (float)(tm_val.tm_hour * 3600 + tm_val.tm_min * 60 + tm_val.tm_sec) +
                              (float)wall_ts.tv_nsec * 1e-9f;
    }

    if (state->has_active_palette) {
        base_params.palette_primary[0] = (float)state->active_palette.primary.r / 255.0f;
        base_params.palette_primary[1] = (float)state->active_palette.primary.g / 255.0f;
        base_params.palette_primary[2] = (float)state->active_palette.primary.b / 255.0f;
        base_params.palette_secondary[0] = (float)state->active_palette.secondary.r / 255.0f;
        base_params.palette_secondary[1] = (float)state->active_palette.secondary.g / 255.0f;
        base_params.palette_secondary[2] = (float)state->active_palette.secondary.b / 255.0f;
        base_params.palette_tertiary[0] = (float)state->active_palette.tertiary.r / 255.0f;
        base_params.palette_tertiary[1] = (float)state->active_palette.tertiary.g / 255.0f;
        base_params.palette_tertiary[2] = (float)state->active_palette.tertiary.b / 255.0f;
    }

    {
        float ws_omega = 14.0f;
        float ws_exp = expf(-ws_omega * global_dt);
        float wdx = state->smooth_workspace_x - state->workspace_target_x;
        float wdy = state->smooth_workspace_y - state->workspace_target_y;
        float wtx = (state->workspace_vel_x + ws_omega * wdx) * global_dt;
        float wty = (state->workspace_vel_y + ws_omega * wdy) * global_dt;
        state->workspace_vel_x = (state->workspace_vel_x - ws_omega * wtx) * ws_exp;
        state->workspace_vel_y = (state->workspace_vel_y - ws_omega * wty) * ws_exp;
        state->smooth_workspace_x = state->workspace_target_x + (wdx + wtx) * ws_exp;
        state->smooth_workspace_y = state->workspace_target_y + (wdy + wty) * ws_exp;
        base_params.workspace_offset[0] = state->smooth_workspace_x;
        base_params.workspace_offset[1] = state->smooth_workspace_y;
    }

    if (state->mpris) {
        const waywal_mpris_state_t *ms = waywal_mpris_get_state(state->mpris);
        if (ms && ms->status == WAYWAL_MPRIS_PLAYING) {
            base_params.media_playing = 1.0f;
        }
    }

    if (state->audio_fft) {
        (void)waywal_audio_fft_update_ducking(state->audio_fft, global_dt);
        (void)waywal_audio_fft_process(state->audio_fft, NULL);
        base_params.audio = waywal_audio_fft_get_frame(state->audio_fft);
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->surface || !out->configured || out->width <= 0 || out->height <= 0)
            continue;
        if (!out->shader_active && !out->workshop_active)
            continue;
        waywal_shader_engine_t *seng = out->shader_eng ? out->shader_eng : state->shader_eng;
        we_scene_t *wscene = out->we_scene ? out->we_scene : state->we_scene;
        if (out->workshop_active) {
            if (!wscene)
                continue;
        } else {
            if (!seng)
                continue;
        }
        if (state->power_gov &&
            waywal_power_governor_should_pause_output(state->power_gov, out->name)) {
            continue;
        }

        if (out->shader_start_ns == 0) {
            out->shader_start_ns = t_now;
            out->shader_last_ns = t_now;
        }
        float dt = (float)(t_now - out->shader_last_ns) * 1e-9f;
        if (dt <= 0.0f || dt > 0.25f) {
            dt = 1.0f / (float)(out->shader_fps > 0 ? out->shader_fps : 60);
        }
        out->shader_last_ns = t_now;

        waywal_shader_frame_params_t fparams = base_params;
        fparams.time_sec = (float)(t_now - out->shader_start_ns) * 1e-9f;
        fparams.time_delta_sec = dt;
        fparams.frame_index = (int32_t)(out->shader_frame_idx++);
        if (!out->shader_audio_reactive &&
            !(out->workshop_active && wscene && wscene->uses_audio)) {
            fparams.audio = NULL;
        }

        float span_dx = 0.0f;
        float span_dy = 0.0f;
        if (out->scaling_mode == WAYWAL_SCALING_SPAN) {
            layer_shell_compute_span_bounds(state, out);
            int32_t tot_w = out->span_total_w > 0 ? out->span_total_w : out->width;
            int32_t tot_h = out->span_total_h > 0 ? out->span_total_h : out->height;
            int32_t gl_off_y = tot_h - out->span_off_y - out->height;
            if (gl_off_y < 0)
                gl_off_y = 0;
            fparams.span_rect[0] = (float)out->span_off_x;
            fparams.span_rect[1] = (float)gl_off_y;
            fparams.span_rect[2] = (float)tot_w;
            fparams.span_rect[3] = (float)tot_h;
            span_dx = (float)out->span_off_x;
            span_dy = (float)gl_off_y;
        }

        float target_mx = (float)out->width * 0.5f + span_dx;
        float target_my = (float)out->height * 0.5f + span_dy;
        double mx = 0.0, my = 0.0;
        if (daemon_query_cursor_pos(state, out, &mx, &my)) {
            double scale_x = (out->logical_width > 0 && out->width > 0)
                                 ? ((double)out->width / (double)out->logical_width)
                                 : (double)(out->scale_factor > 0 ? out->scale_factor : 1);
            double scale_y = (out->logical_height > 0 && out->height > 0)
                                 ? ((double)out->height / (double)out->logical_height)
                                 : (double)(out->scale_factor > 0 ? out->scale_factor : 1);
            target_mx = (float)(mx * scale_x) + span_dx;
            target_my = ((float)out->height - (float)(my * scale_y)) + span_dy;
            fparams.mouse[0] = target_mx;
            fparams.mouse[1] = target_my;

            float click_x = (float)(out->click_x * scale_x) + span_dx;
            float click_y = ((float)out->height - (float)(out->click_y * scale_y)) + span_dy;
            if (out->button_down) {
                fparams.mouse[2] = click_x;
                fparams.mouse[3] = click_y;
            } else {
                fparams.mouse[2] = -fabsf(click_x);
                fparams.mouse[3] = -fabsf(click_y);
            }
        }

        if (out->last_spring_ns == 0) {
            out->smooth_mouse_x = target_mx;
            out->smooth_mouse_y = target_my;
            out->mouse_vel_x = 0.0f;
            out->mouse_vel_y = 0.0f;
            out->last_spring_ns = t_now;
        } else {
            float omega = 18.0f;
            float exp_t = expf(-omega * dt);
            float dx = out->smooth_mouse_x - target_mx;
            float dy = out->smooth_mouse_y - target_my;
            float temp_x = (out->mouse_vel_x + omega * dx) * dt;
            float temp_y = (out->mouse_vel_y + omega * dy) * dt;
            out->mouse_vel_x = (out->mouse_vel_x - omega * temp_x) * exp_t;
            out->mouse_vel_y = (out->mouse_vel_y - omega * temp_y) * exp_t;
            out->smooth_mouse_x = target_mx + (dx + temp_x) * exp_t;
            out->smooth_mouse_y = target_my + (dy + temp_y) * exp_t;
            out->last_spring_ns = t_now;
        }
        state->smooth_mouse_x = out->smooth_mouse_x;
        state->smooth_mouse_y = out->smooth_mouse_y;
        state->mouse_vel_x = out->mouse_vel_x;
        state->mouse_vel_y = out->mouse_vel_y;
        fparams.mouse_smooth[0] = out->smooth_mouse_x;
        fparams.mouse_smooth[1] = out->smooth_mouse_y;
        fparams.mouse_vel[0] = out->mouse_vel_x;
        fparams.mouse_vel[1] = out->mouse_vel_y;

        if (out->transition_active && out->has_source_new_bo) {
            if (out->workshop_active && wscene) {
                (void)we_scene_render_frame(wscene, &out->source_new_bo, &fparams);
            } else if (seng) {
                (void)waywal_shader_engine_render_frame(seng, &out->source_new_bo, &fparams);
            }
            continue;
        }

        if (layer_shell_ensure_dmabuf_ring(state, out, out->width, out->height) &&
            out->use_dmabuf) {
            dmabuf_bo_t *bo = dmabuf_ring_acquire(&out->dmabuf_ring);
            if (bo && bo->wl_buffer) {
                bool rendered = false;
                if (out->workshop_active && wscene) {
                    rendered = we_scene_render_frame(wscene, bo, &fparams);
                } else if (seng) {
                    rendered = waywal_shader_engine_render_frame(seng, bo, &fparams);
                }
                if (rendered) {
                    if (out->viewport) {
                        int32_t dst_w =
                            out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
                        int32_t dst_h =
                            out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
                        wp_viewport_set_destination(out->viewport, dst_w, dst_h);
                        wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                               wl_fixed_from_int(-1));
                        out->vp_configured = false;
                    }
                    wl_surface_attach(out->surface, bo->wl_buffer, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                    wl_surface_commit(out->surface);
                    out->last_presented_bo = bo;
                    out->has_image = true;
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
    if (!state->shader_active)
        return;
    if (state->power_gov && waywal_power_governor_should_pause_all(state->power_gov)) {
        disarm_timer_fd(state->shader_timer_fd);
        return;
    }
    daemon_shader_render_once(state);
}

static void reconcile_one_video_engine_power(daemon_state_t *state, video_engine_t *ve,
                                             bool pause_all)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PLAYING || ve->timer_fd < 0)
        return;
    /* Never disarm or delay the initial 1ms first-frame decode timer */
    if (!ve->has_decoded_any)
        return;
    uint32_t target_v_fps = 60U;
    if (ve->demuxer && ve->demuxer->fps_den > 0 && ve->demuxer->fps_num > 0) {
        target_v_fps = ve->demuxer->fps_num / ve->demuxer->fps_den;
        if (target_v_fps == 0)
            target_v_fps = 30U;
    }
    uint32_t eff_v_fps = 0U;
    if (!pause_all) {
        if (ve->target_output && ve->target_output->name[0]) {
            eff_v_fps = waywal_power_governor_effective_fps(state->power_gov,
                                                            ve->target_output->name, target_v_fps);
        } else {
            bool any_matched = false;
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (out->video_eng && out->video_eng != ve)
                    continue;
                if (ve->has_target_filter && !out->video_targeted)
                    continue;
                any_matched = true;
                uint32_t out_fps = waywal_power_governor_effective_fps(
                    state->power_gov, out->name[0] ? out->name : "*", target_v_fps);
                if (out_fps > eff_v_fps)
                    eff_v_fps = out_fps;
            }
            if (!any_matched) {
                eff_v_fps =
                    waywal_power_governor_effective_fps(state->power_gov, "*", target_v_fps);
            }
        }
    }
    if (eff_v_fps == 0) {
        disarm_timer_fd(ve->timer_fd);
    } else {
        arm_oneshot_timer_ms(ve->timer_fd, 1000U / eff_v_fps);
    }
}

void daemon_reconcile_power_state(daemon_state_t *state)
{
    if (!state || !state->power_gov)
        return;

    bool pause_all = waywal_power_governor_should_pause_all(state->power_gov);

    reconcile_one_video_engine_power(state, &state->video_engine, pause_all);
    video_engine_t *seen_ve[32];
    size_t num_seen_ve = 0;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->video_eng && out->video_eng != &state->video_engine) {
            bool already = false;
            for (size_t k = 0; k < num_seen_ve; ++k) {
                if (seen_ve[k] == out->video_eng) {
                    already = true;
                    break;
                }
            }
            if (!already && num_seen_ve < 32) {
                seen_ve[num_seen_ve++] = out->video_eng;
                reconcile_one_video_engine_power(state, out->video_eng, pause_all);
            }
        }
    }

    if (state->shader_active && state->shader_timer_fd >= 0) {
        const char *s_out = state->shader_target_output[0] ? state->shader_target_output : "*";
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

static void upsert_workshop_prop_kv(char *props_buf, size_t cap, const char *key,
                                    const char *kv_pair)
{
    if (!props_buf || cap == 0 || !key || key[0] == '\0' || !kv_pair)
        return;

    size_t key_len = strlen(key);
    char new_buf[2048];
    new_buf[0] = '\0';
    size_t out_off = 0;
    bool replaced = false;

    const char *p = props_buf;
    while (*p != '\0') {
        while (*p == ';')
            p++;
        if (*p == '\0')
            break;
        const char *semi = strchr(p, ';');
        size_t tok_len = semi ? (size_t)(semi - p) : strlen(p);
        if (tok_len > key_len && p[key_len] == '=' && strncasecmp(p, key, key_len) == 0) {
            if (!replaced) {
                buf_appendf(new_buf, sizeof(new_buf), &out_off, "%s%s", (out_off > 0) ? ";" : "",
                            kv_pair);
                replaced = true;
            }
        } else if (tok_len > 0) {
            buf_appendf(new_buf, sizeof(new_buf), &out_off, "%s%.*s", (out_off > 0) ? ";" : "",
                        (int)tok_len, p);
        }
        p += tok_len;
    }

    if (!replaced) {
        buf_appendf(new_buf, sizeof(new_buf), &out_off, "%s%s", (out_off > 0) ? ";" : "", kv_pair);
    }

    strncpy(props_buf, new_buf, cap - 1);
    props_buf[cap - 1] = '\0';
}

static bool daemon_activate_shader(daemon_state_t *state, const waywal_shader_payload_t *spay)
{
    if (!state || !spay || spay->shader_path[0] == '\0')
        return false;

    if (!state->outputs) {
        if (state->we_scene) {
            we_scene_destroy(state->we_scene);
            state->we_scene = NULL;
        }
        if (!state->shader_eng && state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            state->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
        }
        bool ok = (state->shader_eng && state->dmabuf_ctx.available)
                      ? waywal_shader_engine_load_file(state->shader_eng, spay->shader_path)
                      : validate_shader_file_headless(spay->shader_path);
        if (!ok) {
            shader_engine_stop_playback(state);
            return false;
        }
        if (!state->shader_eng) {
            state->shader_eng = waywal_shader_engine_create(NULL);
        }
        state->shader_active = true;
        state->workshop_active = false;
        state->shader_fps = spay->fps > 0 ? spay->fps : 60U;
        memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
        strncpy(state->shader_target_output, spay->output[0] ? spay->output : "*",
                sizeof(state->shader_target_output) - 1);
        memset(state->shader_path, 0, sizeof(state->shader_path));
        strncpy(state->shader_path, spay->shader_path, sizeof(state->shader_path) - 1);
        if (state->mpris) {
            daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris),
                                                   true);
        }
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
        daemon_reconcile_power_state(state);
        return true;
    }

    if (!validate_shader_file_headless(spay->shader_path)) {
        return false;
    }

    uint64_t t_now = now_ns();
    bool any_ok = false;
    bool any_transition_started = false;
    uint32_t trans_fps = spay->transition_fps > 0 ? spay->transition_fps : 60U;
    uint32_t num_frames = (spay->transition_duration_ms * trans_fps) / 1000U;
    if (num_frames < 2)
        num_frames = 2;
    uint32_t resolved_type = spay->transition_type;
    if (resolved_type == WAYWAL_TRANSITION_RANDOM) {
        resolved_type = 1u + ((uint32_t)rand() % 19u);
    }
    bool has_transition =
        (resolved_type != WAYWAL_TRANSITION_NONE && spay->transition_duration_ms > 0);

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!output_matches_target_spec(out, spay->output))
            continue;

        if (has_transition) {
            layer_shell_snapshot_live_output(state, out);
        }

        if (!out->shader_eng && state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            out->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
            if (!state->shader_eng) {
                state->shader_eng = out->shader_eng;
            } else if (out->shader_eng) {
                waywal_shader_engine_copy_custom_uniforms(out->shader_eng, state->shader_eng);
                if (!is_shader_eng_attached_to_any_output(state, state->shader_eng)) {
                    waywal_shader_engine_destroy(state->shader_eng);
                    state->shader_eng = out->shader_eng;
                }
            }
        }
        if (out->shader_eng && state->wallpaper_rgba && state->wallpaper_w > 0 &&
            state->wallpaper_h > 0) {
            waywal_shader_engine_set_wallpaper_bgra(out->shader_eng, state->wallpaper_rgba,
                                                    state->wallpaper_w, state->wallpaper_h);
        }
        if (out->shader_eng && state->mpris) {
            const waywal_mpris_state_t *ms = waywal_mpris_get_state(state->mpris);
            if (ms) {
                waywal_shader_engine_set_media_playing(
                    out->shader_eng, (ms->status == WAYWAL_MPRIS_PLAYING) ? 1.0f : 0.0f);
                if (ms->has_art_pixels && ms->art_bgra) {
                    waywal_shader_engine_set_album_art_bgra(out->shader_eng, ms->art_bgra,
                                                            ms->art_width, ms->art_height);
                }
            }
        }

        bool loaded = false;
        if (out->shader_eng) {
            if (waywal_shader_engine_load_file(out->shader_eng, spay->shader_path)) {
                loaded = true;
                int inotify_fd = waywal_shader_engine_get_inotify_fd(out->shader_eng);
                if (inotify_fd >= 0) {
                    register_fd_in_loops(state, inotify_fd, URING_EV_SHADER_INOTIFY);
                }
            }
        } else {
            loaded = true;
        }

        if (!loaded) {
            out->shader_active = false;
            continue;
        }

        stop_engines_on_output(state, out, true, false, false, false);
        destroy_output_we_scene(state, out);

        out->shader_active = true;
        out->workshop_active = false;
        out->shader_audio_reactive = spay->audio_reactive;
        out->shader_fps = spay->fps > 0 ? spay->fps : 60U;
        out->shader_frame_idx = 0;
        out->shader_start_ns = t_now;
        out->shader_last_ns = t_now;
        out->scaling_mode = spay->scaling_mode;
        memset(out->shader_path, 0, sizeof(out->shader_path));
        strncpy(out->shader_path, spay->shader_path, sizeof(out->shader_path) - 1);

        if (has_transition && out->has_source_old_bo && out->width > 0 && out->height > 0 &&
            layer_shell_ensure_dmabuf_ring(state, out, out->width, out->height) &&
            out->use_dmabuf) {
            uint64_t mod = state->render_engine.gpu_compute_active
                               ? state->dmabuf_ctx.preferred_modifier
                               : DRM_FORMAT_MOD_LINEAR;
            if (!out->has_source_new_bo || out->source_new_bo.width != (uint32_t)out->width ||
                out->source_new_bo.height != (uint32_t)out->height) {
                if (out->has_source_new_bo) {
                    if (out->shader_eng) {
                        waywal_shader_engine_forget_bo(out->shader_eng, &out->source_new_bo);
                    }
                    if (state->shader_eng && state->shader_eng != out->shader_eng) {
                        waywal_shader_engine_forget_bo(state->shader_eng, &out->source_new_bo);
                    }
                    if (out->we_scene) {
                        waywal_we_scene_forget_bo(out->we_scene, &out->source_new_bo);
                    }
                    if (state->we_scene && state->we_scene != out->we_scene) {
                        waywal_we_scene_forget_bo(state->we_scene, &out->source_new_bo);
                    }
                    render_engine_release_bo(&state->render_engine, &out->source_new_bo);
                    dmabuf_bo_free(&out->source_new_bo);
                    out->has_source_new_bo = false;
                }
                if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_new_bo,
                                       (uint32_t)out->width, (uint32_t)out->height,
                                       DRM_FORMAT_ARGB8888, mod)) {
                    out->has_source_new_bo = true;
                }
            }
            if (out->has_source_new_bo) {
                if (out->viewport) {
                    int32_t dst_w =
                        out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
                    int32_t dst_h =
                        out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
                    wp_viewport_set_destination(out->viewport, dst_w, dst_h);
                    wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1));
                    out->vp_configured = false;
                }
                float cx = spay->transition_center_x;
                float cy = spay->transition_center_y;
                if (cx <= 0.0f && cy <= 0.0f) {
                    cx = 0.5f;
                    cy = 0.5f;
                }
                bezier_curve_t bz = BEZIER_DEFAULT;
                if (spay->has_custom_bezier || spay->bezier.x1 != 0.0f || spay->bezier.y1 != 0.0f ||
                    spay->bezier.x2 != 0.0f || spay->bezier.y2 != 0.0f) {
                    bz = spay->bezier;
                }
                out->transition_params = (waywal_transition_params_t){
                    .type = (waywal_transition_type_t)resolved_type,
                    .progress = 0.0f,
                    .angle_rad = spay->transition_angle_rad,
                    .wave_freq = spay->transition_wave_freq,
                    .wave_amp = spay->transition_wave_amp,
                    .center_x = cx,
                    .center_y = cy,
                    .bezier = bz,
                };
                out->transition_current_frame = 0;
                out->transition_total_frames = num_frames;
                out->transition_delay_frames = 0;
                out->transition_active = true;
                any_transition_started = true;
            }
        }
        any_ok = true;
    }

    if (!any_ok) {
        reconcile_global_engine_flags(state);
        return false;
    }

    memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
    strncpy(state->shader_target_output, spay->output[0] ? spay->output : "*",
            sizeof(state->shader_target_output) - 1);
    memset(state->shader_path, 0, sizeof(state->shader_path));
    strncpy(state->shader_path, spay->shader_path, sizeof(state->shader_path) - 1);

    reconcile_global_engine_flags(state);
    if (state->mpris) {
        daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris), true);
    }
    gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    daemon_shader_render_once(state);
    if (any_transition_started && state->transition_timer_fd >= 0) {
        arm_periodic_timer_fd(state->transition_timer_fd, trans_fps);
        state->transitions_in_progress = true;
    }
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

    bool has_transition = (transition_type != WAYWAL_TRANSITION_NONE && duration_ms > 0);
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (output_matches_img_meta(out, meta)) {
            if (has_transition) {
                layer_shell_snapshot_live_output(state, out);
            }
            stop_engines_on_output(state, out, false, false, false, false);
        }
    }
    reconcile_global_engine_flags(state);

    daemon_update_palette_from_bgra(state, px, w, h, meta, true);
    bool ok = daemon_start_image_transition(state, px, w, h, meta);
    image_free(px);
    if (ok) {
        if (state->config.hooks.on_wallpaper_change[0] != '\0') {
            (void)waywal_palette_run_hook(state->config.hooks.on_wallpaper_change);
        }
        (void)try_start_native_animation(state, filepath, meta,
                                         target_output ? target_output : "*");
    }
    return ok;
}

static bool daemon_start_slideshow_isolated(daemon_state_t *state,
                                            const waywal_slideshow_payload_t *spay)
{
    if (!state || !spay)
        return false;

    if (state->outputs) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_slideshow_payload(out, spay)) {
                stop_engines_on_output(state, out, false, false, false, false);
            }
        }
        reconcile_global_engine_flags(state);
    } else {
        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            video_engine_stop(&state->video_engine);
        }
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
    }

    slideshow_engine_t *ss = &state->slideshow;
    if (state->outputs && spay->num_target_outputs > 0 && state->slideshow.active) {
        bool global_in_use = false;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!output_matches_slideshow_payload(out, spay) &&
                out->slideshow_eng == &state->slideshow) {
                global_in_use = true;
                break;
            }
        }
        if (global_in_use) {
            slideshow_engine_t *custom_ss =
                (slideshow_engine_t *)calloc(1, sizeof(slideshow_engine_t));
            if (custom_ss) {
                slideshow_init(custom_ss, state);
                if (custom_ss->timer_fd >= 0) {
                    register_fd_in_loops(state, custom_ss->timer_fd, URING_EV_TIMER);
                }
                ss = custom_ss;
            }
        }
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (output_matches_slideshow_payload(out, spay)) {
            out->slideshow_eng = ss;
        }
    }

    bool ok = slideshow_start(ss, spay);
    if (!ok) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->slideshow_eng == ss) {
                out->slideshow_eng = NULL;
            }
        }
        if (ss != &state->slideshow) {
            if (ss->timer_fd >= 0) {
                unregister_fd_from_loops(state, ss->timer_fd);
            }
            slideshow_destroy(ss);
            free(ss);
        }
    }
    return ok;
}

static bool daemon_start_video_file_isolated(daemon_state_t *state, const char *filepath,
                                             const char *target, waywal_scaling_mode_t scaling)
{
    if (!state || !filepath || filepath[0] == '\0')
        return false;

    bool has_target = (target && target[0] != '\0' && strcmp(target, "*") != 0);
    output_node_t *target_out = NULL;
    if (has_target) {
        target_out = output_manager_find_by_name(&state->output_mgr, target);
    }

    if (has_target && state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED &&
        state->video_engine.filepath[0] != '\0' &&
        strcmp(state->video_engine.filepath, filepath) == 0) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (strcmp(out->name, target) == 0) {
                stop_engines_on_output(state, out, false, false,
                                       out->video_eng == &state->video_engine, false);
                out->video_eng = &state->video_engine;
                out->video_targeted = true;
                out->scaling_mode = (uint32_t)scaling;
                out->vp_configured = false;
            }
        }
        if (state->video_engine.target_output != target_out) {
            state->video_engine.target_output = NULL;
        }
        reconcile_global_engine_flags(state);
        daemon_reconcile_power_state(state);
        return true;
    }

    if (state->outputs) {
        if (has_target) {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (strcmp(out->name, target) == 0) {
                    stop_engines_on_output(state, out, false, false,
                                           out->video_eng == &state->video_engine, false);
                }
            }
        } else {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                stop_engines_on_output(state, out, false, false,
                                       out->video_eng == &state->video_engine, false);
            }
            slideshow_stop(&state->slideshow);
            shader_engine_stop_playback(state);
            anim_engine_stop_playback(state);
        }
        reconcile_global_engine_flags(state);
    } else {
        slideshow_stop(&state->slideshow);
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
    }

    video_engine_t *ve = &state->video_engine;
    if (has_target && state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED &&
        state->video_engine.target_output != target_out) {
        video_engine_t *custom_ve = (video_engine_t *)calloc(1, sizeof(video_engine_t));
        if (custom_ve && video_engine_init(custom_ve, state)) {
            if (custom_ve->timer_fd >= 0) {
                register_fd_in_loops(state, custom_ve->timer_fd, URING_EV_TIMER);
            }
            ve = custom_ve;
        } else {
            free(custom_ve);
        }
    }

    ve->has_target_filter = has_target;
    if (ve == &state->video_engine) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            bool match = !has_target || (strcmp(out->name, target) == 0);
            out->video_targeted = match;
            if (!match && out->video_eng == &state->video_engine) {
                out->video_eng = NULL;
            }
        }
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!has_target || strcmp(out->name, target) == 0) {
            out->video_eng = ve;
            out->scaling_mode = (uint32_t)scaling;
            out->vp_configured = false;
        }
    }

    bool ok = video_engine_load_file(ve, filepath, target_out);
    if (!ok) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->video_eng == ve) {
                out->video_eng = NULL;
            }
        }
        if (ve != &state->video_engine) {
            if (ve->timer_fd >= 0) {
                unregister_fd_from_loops(state, ve->timer_fd);
            }
            video_engine_destroy(ve);
            free(ve);
        }
    } else {
        daemon_reconcile_power_state(state);
    }
    return ok;
}

static bool daemon_activate_workshop(daemon_state_t *state, const waywal_workshop_payload_t *wpay)
{
    if (!state || !wpay || wpay->workshop_dir[0] == '\0')
        return false;

    char resolved_dir[4096];
    const char *ws_dir = wpay->workshop_dir;
    if (we_workshop_resolve_dir(wpay->workshop_dir, resolved_dir, sizeof(resolved_dir))) {
        ws_dir = resolved_dir;
    }

    we_project_t proj;
    memset(&proj, 0, sizeof(proj));
    if (!we_project_load_dir(ws_dir, &proj)) {
        char probe_pkg[4600];
        char probe_json[4600];
        struct stat st;
        snprintf(probe_pkg, sizeof(probe_pkg), "%s/scene.pkg", ws_dir);
        snprintf(probe_json, sizeof(probe_json), "%s/scene.json", ws_dir);
        if ((stat(probe_pkg, &st) == 0 && S_ISREG(st.st_mode)) ||
            (stat(probe_json, &st) == 0 && S_ISREG(st.st_mode))) {
            strncpy(proj.type, "scene", sizeof(proj.type) - 1);
            strncpy(proj.file, "scene.json", sizeof(proj.file) - 1);
        } else {
            return false;
        }
    }

    if (proj.preview[0] != '\0') {
        char prev_path[4600];
        snprintf(prev_path, sizeof(prev_path), "%s/%s", ws_dir, proj.preview);
        uint8_t *px = NULL;
        uint32_t pw = 0, ph = 0;
        if (image_load(prev_path, &px, &pw, &ph) && px && pw > 0 && ph > 0) {
            daemon_update_palette_from_bgra(state, px, pw, ph, NULL, true);
        }
        if (px)
            image_free(px);
    }

    if (strcasecmp(proj.type, "video") == 0 && proj.file[0] != '\0') {
        char vpath[4600];
        snprintf(vpath, sizeof(vpath), "%s/%s", ws_dir, proj.file);
        return daemon_start_video_file_isolated(state, vpath, wpay->output[0] ? wpay->output : "*",
                                                (waywal_scaling_mode_t)wpay->scaling_mode);
    }

    if (!state->outputs) {
        if (state->we_scene) {
            we_scene_destroy(state->we_scene);
            state->we_scene = NULL;
        }
        if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            state->we_scene = we_scene_create(&state->dmabuf_ctx, ws_dir, wpay->prop_overrides);
        } else {
            state->we_scene = we_scene_load_cpu(ws_dir, wpay->prop_overrides);
        }
        if (!state->we_scene) {
            shader_engine_stop_playback(state);
            return false;
        }
        state->shader_active = true;
        state->workshop_active = true;
        state->shader_fps = wpay->fps > 0 ? wpay->fps : 60U;
        memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
        strncpy(state->shader_target_output, wpay->output[0] ? wpay->output : "*",
                sizeof(state->shader_target_output) - 1);
        memset(state->workshop_dir, 0, sizeof(state->workshop_dir));
        strncpy(state->workshop_dir, ws_dir, sizeof(state->workshop_dir) - 1);
        memset(state->workshop_props, 0, sizeof(state->workshop_props));
        strncpy(state->workshop_props, wpay->prop_overrides, sizeof(state->workshop_props) - 1);
        if (state->mpris) {
            daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris),
                                                   true);
        }
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
        daemon_reconcile_power_state(state);
        return true;
    }

    uint64_t t_now = now_ns();
    bool any_ok = false;
    bool any_transition_started = false;
    uint32_t trans_fps = wpay->transition_fps > 0 ? wpay->transition_fps : 60U;
    uint32_t num_frames = (wpay->transition_duration_ms * trans_fps) / 1000U;
    if (num_frames < 2)
        num_frames = 2;
    uint32_t resolved_type = wpay->transition_type;
    if (resolved_type == WAYWAL_TRANSITION_RANDOM) {
        resolved_type = 1u + ((uint32_t)rand() % 19u);
    }
    bool has_transition =
        (resolved_type != WAYWAL_TRANSITION_NONE && wpay->transition_duration_ms > 0);

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!output_matches_target_spec(out, wpay->output))
            continue;

        if (has_transition) {
            layer_shell_snapshot_live_output(state, out);
        }

        destroy_output_we_scene(state, out);

        if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            out->we_scene = we_scene_create(&state->dmabuf_ctx, ws_dir, wpay->prop_overrides);
        } else {
            out->we_scene = we_scene_load_cpu(ws_dir, wpay->prop_overrides);
        }
        if (!state->we_scene && out->we_scene) {
            state->we_scene = out->we_scene;
        }

        if (!out->we_scene) {
            out->workshop_active = false;
            continue;
        }

        stop_engines_on_output(state, out, true, false, false, false);

        out->shader_active = false;
        out->workshop_active = true;
        out->shader_audio_reactive = wpay->audio_reactive;
        out->shader_fps = wpay->fps > 0 ? wpay->fps : 60U;
        out->shader_frame_idx = 0;
        out->shader_start_ns = t_now;
        out->shader_last_ns = t_now;
        out->scaling_mode = wpay->scaling_mode;
        memset(out->workshop_dir, 0, sizeof(out->workshop_dir));
        strncpy(out->workshop_dir, ws_dir, sizeof(out->workshop_dir) - 1);
        memset(out->workshop_props, 0, sizeof(out->workshop_props));
        strncpy(out->workshop_props, wpay->prop_overrides, sizeof(out->workshop_props) - 1);

        if (has_transition && out->has_source_old_bo && out->width > 0 && out->height > 0 &&
            layer_shell_ensure_dmabuf_ring(state, out, out->width, out->height) &&
            out->use_dmabuf) {
            uint64_t mod = state->render_engine.gpu_compute_active
                               ? state->dmabuf_ctx.preferred_modifier
                               : DRM_FORMAT_MOD_LINEAR;
            if (!out->has_source_new_bo || out->source_new_bo.width != (uint32_t)out->width ||
                out->source_new_bo.height != (uint32_t)out->height) {
                if (out->has_source_new_bo) {
                    if (out->shader_eng) {
                        waywal_shader_engine_forget_bo(out->shader_eng, &out->source_new_bo);
                    }
                    if (state->shader_eng && state->shader_eng != out->shader_eng) {
                        waywal_shader_engine_forget_bo(state->shader_eng, &out->source_new_bo);
                    }
                    if (out->we_scene) {
                        waywal_we_scene_forget_bo(out->we_scene, &out->source_new_bo);
                    }
                    if (state->we_scene && state->we_scene != out->we_scene) {
                        waywal_we_scene_forget_bo(state->we_scene, &out->source_new_bo);
                    }
                    render_engine_release_bo(&state->render_engine, &out->source_new_bo);
                    dmabuf_bo_free(&out->source_new_bo);
                    out->has_source_new_bo = false;
                }
                if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_new_bo,
                                       (uint32_t)out->width, (uint32_t)out->height,
                                       DRM_FORMAT_ARGB8888, mod)) {
                    out->has_source_new_bo = true;
                }
            }
            if (out->has_source_new_bo) {
                if (out->viewport) {
                    int32_t dst_w =
                        out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
                    int32_t dst_h =
                        out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
                    wp_viewport_set_destination(out->viewport, dst_w, dst_h);
                    wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                           wl_fixed_from_int(-1));
                    out->vp_configured = false;
                }
                float cx = wpay->transition_center_x;
                float cy = wpay->transition_center_y;
                if (cx <= 0.0f && cy <= 0.0f) {
                    cx = 0.5f;
                    cy = 0.5f;
                }
                bezier_curve_t bz = BEZIER_DEFAULT;
                if (wpay->has_custom_bezier || wpay->bezier.x1 != 0.0f || wpay->bezier.y1 != 0.0f ||
                    wpay->bezier.x2 != 0.0f || wpay->bezier.y2 != 0.0f) {
                    bz = wpay->bezier;
                }
                out->transition_params = (waywal_transition_params_t){
                    .type = (waywal_transition_type_t)resolved_type,
                    .progress = 0.0f,
                    .angle_rad = wpay->transition_angle_rad,
                    .wave_freq = wpay->transition_wave_freq,
                    .wave_amp = wpay->transition_wave_amp,
                    .center_x = cx,
                    .center_y = cy,
                    .bezier = bz,
                };
                out->transition_current_frame = 0;
                out->transition_total_frames = num_frames;
                out->transition_delay_frames = 0;
                out->transition_active = true;
                any_transition_started = true;
            }
        }
        any_ok = true;
    }

    if (!any_ok) {
        reconcile_global_engine_flags(state);
        return false;
    }

    memset(state->shader_target_output, 0, sizeof(state->shader_target_output));
    strncpy(state->shader_target_output, wpay->output[0] ? wpay->output : "*",
            sizeof(state->shader_target_output) - 1);
    memset(state->workshop_dir, 0, sizeof(state->workshop_dir));
    strncpy(state->workshop_dir, ws_dir, sizeof(state->workshop_dir) - 1);
    memset(state->workshop_props, 0, sizeof(state->workshop_props));
    strncpy(state->workshop_props, wpay->prop_overrides, sizeof(state->workshop_props) - 1);

    reconcile_global_engine_flags(state);
    if (state->mpris) {
        daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris), true);
    }
    gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    daemon_shader_render_once(state);
    if (any_transition_started && state->transition_timer_fd >= 0) {
        arm_periodic_timer_fd(state->transition_timer_fd, trans_fps);
        state->transitions_in_progress = true;
    }
    daemon_reconcile_power_state(state);
    return true;
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
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                stop_engines_on_output(state, out, false, false, false, false);
            }
            reconcile_global_engine_flags(state);
            daemon_clear_all_outputs(state, col);
        } else {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (strcmp(out->name, target) == 0) {
                    stop_engines_on_output(state, out, false, false, false, false);
                    output_node_render_color(state, out, col);
                }
            }
            reconcile_global_engine_flags(state);
        }
        return true;
    }
    case WAYWAL_CONTENT_STATIC_IMAGE:
    case WAYWAL_CONTENT_ANIMATION: {
        return daemon_render_image_on_target(state, saved->media_path, target, saved->scaling,
                                             saved->filter, saved->ten_bit, WAYWAL_TRANSITION_NONE,
                                             0);
    }
    case WAYWAL_CONTENT_SLIDESHOW: {
        if (saved->media_path[0] == '\0')
            return false;
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s = saved->slideshow_interval_sec > 0 ? saved->slideshow_interval_sec : 300;
        spay.scaling_mode = saved->scaling;
        spay.filter_mode = saved->filter;
        spay.ten_bit = saved->ten_bit;
        spay.transition_type = state->config.transition.type_enum;
        spay.transition_duration_ms = (uint32_t)(state->config.transition.duration * 1000.0f);
        spay.transition_fps = state->config.transition.fps;
        spay.transition_angle_rad =
            state->config.transition.angle * (3.14159265358979323846f / 180.0f);
        spay.transition_wave_freq = state->config.transition.wave_x;
        spay.transition_wave_amp = state->config.transition.wave_y;
        spay.transition_center_x = state->config.transition.pos_x;
        spay.transition_center_y = state->config.transition.pos_y;
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = true;
        if (strcmp(target, "*") != 0) {
            spay.num_target_outputs = 1;
            strncpy(spay.target_outputs[0], target, sizeof(spay.target_outputs[0]) - 1);
        }
        strncpy(spay.path, saved->media_path, sizeof(spay.path) - 1);
        return daemon_start_slideshow_isolated(state, &spay);
    }
    case WAYWAL_CONTENT_VIDEO: {
        if (saved->media_path[0] == '\0')
            return false;
        return daemon_start_video_file_isolated(state, saved->media_path, target, saved->scaling);
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
        spay.scaling_mode = (uint32_t)saved->scaling;
        return daemon_activate_shader(state, &spay);
    }
    case WAYWAL_CONTENT_WORKSHOP: {
        if (saved->media_path[0] == '\0')
            return false;
        waywal_workshop_payload_t wpay;
        memset(&wpay, 0, sizeof(wpay));
        strncpy(wpay.output, target, sizeof(wpay.output) - 1);
        strncpy(wpay.workshop_dir, saved->media_path, sizeof(wpay.workshop_dir) - 1);
        strncpy(wpay.prop_overrides, saved->prop_overrides, sizeof(wpay.prop_overrides) - 1);
        wpay.fps = saved->target_fps > 0 ? saved->target_fps : 60;
        wpay.audio_reactive = saved->audio_reactive;
        wpay.scaling_mode = (uint32_t)saved->scaling;
        return daemon_activate_workshop(state, &wpay);
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
        spay.scaling_mode = (uint32_t)rule->scaling;
        return daemon_activate_shader(state, &spay);
    }
    if (strcasecmp(rule->mode, "workshop") == 0) {
        waywal_workshop_payload_t wpay;
        memset(&wpay, 0, sizeof(wpay));
        strncpy(wpay.output, target, sizeof(wpay.output) - 1);
        strncpy(wpay.workshop_dir, rule->path, sizeof(wpay.workshop_dir) - 1);
        wpay.fps = rule->fps > 0 ? rule->fps : 60;
        wpay.audio_reactive = rule->audio;
        wpay.scaling_mode = (uint32_t)rule->scaling;
        return daemon_activate_workshop(state, &wpay);
    }
    if (strcasecmp(rule->mode, "video") == 0) {
        return daemon_start_video_file_isolated(state, rule->path, target, rule->scaling);
    }
    if (strcasecmp(rule->mode, "slideshow") == 0) {
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s = rule->interval > 0 ? rule->interval : 300;
        spay.random_order = rule->shuffle;
        spay.scaling_mode = (uint32_t)rule->scaling;
        spay.filter_mode = (uint32_t)rule->filter;
        spay.ten_bit = state->config.daemon.ten_bit;
        spay.transition_type = state->config.transition.type_enum;
        spay.transition_duration_ms = (uint32_t)(state->config.transition.duration * 1000.0f);
        spay.transition_fps = state->config.transition.fps;
        spay.transition_angle_rad =
            state->config.transition.angle * (3.14159265358979323846f / 180.0f);
        spay.transition_wave_freq = state->config.transition.wave_x;
        spay.transition_wave_amp = state->config.transition.wave_y;
        spay.transition_center_x = state->config.transition.pos_x;
        spay.transition_center_y = state->config.transition.pos_y;
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = true;
        if (strcmp(target, "*") != 0) {
            spay.num_target_outputs = 1;
            strncpy(spay.target_outputs[0], target, sizeof(spay.target_outputs[0]) - 1);
        }
        strncpy(spay.path, rule->path, sizeof(spay.path) - 1);
        return daemon_start_slideshow_isolated(state, &spay);
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
        int new_ipc_fd = waywal_power_governor_get_ipc_fd(state->power_gov);
        bool reconnected = waywal_power_governor_consume_ipc_reconnected(state->power_gov);
        if (new_ipc_fd != ipc_fd || reconnected) {
            if (ipc_fd >= 0) {
                if (state->epoll_fd >= 0 && new_ipc_fd != ipc_fd) {
                    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, ipc_fd, NULL);
                }
                if (state->use_uring && state->uring_loop) {
                    (void)uring_loop_remove_poll(state->uring_loop, ipc_fd);
                }
            }
            if (new_ipc_fd >= 0) {
                register_fd_in_loops(state, new_ipc_fd, URING_EV_TIMER);
            }
        }
        char ws[64];
        char mon[64];
        while (waywal_power_governor_pop_workspace_change(state->power_gov, ws, sizeof(ws), mon,
                                                          sizeof(mon))) {
            int ws_idx = atoi(ws);
            if (ws_idx > 0) {
                state->workspace_target_x = (float)((ws_idx - 1) % 10) * 0.1f;
            } else {
                state->workspace_target_x += 0.1f;
                if (state->workspace_target_x > 1.0f)
                    state->workspace_target_x -= 1.0f;
            }
            for (size_t i = 0; i < state->config.workspace_count; ++i) {
                const waywal_workspace_rule_t *wrule = &state->config.workspaces[i];
                if (strcmp(wrule->workspace_name, ws) == 0 && wrule->path[0] != '\0') {
                    const char *target =
                        wrule->output_name[0] ? wrule->output_name : (mon[0] ? mon : "*");
                    uint32_t dur_ms = (uint32_t)(state->config.transition.duration > 0.0f
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLIN};
                if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
                    continue;
                }
            }
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLOUT)) {
                    continue;
                }
            }
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
        buf_appendf(query_buf, sizeof(query_buf), &off, "Connected outputs (%zu):\n",
                    state->num_outputs);

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            buf_appendf(query_buf, sizeof(query_buf), &off, "  - %s (%s): %dx%d (scale: %d)\n",
                        out->name[0] ? out->name : "unknown",
                        out->description[0] ? out->description : "unknown", out->width, out->height,
                        out->scale_factor);
        }

        resp_hdr.opcode = WAYWAL_RESP_INFO;
        resp_hdr.payload_size = (uint32_t)off;
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

                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                s_entry.kind = WAYWAL_CONTENT_SOLID_COLOR;
                s_entry.solid_color = ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
                                      ((uint32_t)color.b << 8) | (uint32_t)color.a;

                if (has_targets && state->outputs) {
                    const char *ptr = extra_buf;
                    size_t rem = extra_len;
                    for (uint32_t i = 0; i < clear_payload.num_target_outputs && rem > 0; ++i) {
                        const char *end = (const char *)memchr(ptr, '\0', rem);
                        if (!end)
                            break;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (strcmp(out->name, ptr) == 0) {
                                stop_engines_on_output(state, out, false, false, false, false);
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
                    reconcile_global_engine_flags(state);
                } else {
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        stop_engines_on_output(state, out, false, false, false, false);
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
        daemon_broadcast_event(state, "wallpaper_changed");
        break;
    }

    case WAYWAL_REQ_SET_IMAGE: {
        struct stat st;
        const size_t min_hdr_size = 72u; /* Legacy v0.x metadata header prefix */
        if (attached_fd >= 0 && fstat(attached_fd, &st) == 0 &&
            (size_t)st.st_size >= min_hdr_size && req_hdr.payload_size >= min_hdr_size &&
            (size_t)st.st_size >= req_hdr.payload_size) {
            void *mapped = mmap(NULL, req_hdr.payload_size, PROT_READ, MAP_PRIVATE, attached_fd, 0);
            if (mapped != MAP_FAILED) {
                const waywal_img_metadata_t *raw_meta = (const waywal_img_metadata_t *)mapped;
                uint64_t pixel_bytes =
                    (uint64_t)raw_meta->width * (uint64_t)raw_meta->height * 4ULL;
                uint64_t tail_bytes = (uint64_t)raw_meta->extra_data_len + pixel_bytes;
                size_t hdr_size = sizeof(waywal_img_metadata_t);
                if (req_hdr.payload_size >= tail_bytes) {
                    uint64_t inferred_hdr = req_hdr.payload_size - tail_bytes;
                    if (inferred_hdr >= min_hdr_size &&
                        inferred_hdr < sizeof(waywal_img_metadata_t)) {
                        hdr_size = (size_t)inferred_hdr;
                    }
                }
                uint64_t expected_size = (uint64_t)hdr_size + tail_bytes;
                bool ok = false;
                if (raw_meta->width > 0 && raw_meta->height > 0 && raw_meta->width <= 16384 &&
                    raw_meta->height <= 16384 && req_hdr.payload_size >= expected_size &&
                    (size_t)st.st_size >= expected_size) {
                    uint8_t *compat_buf = NULL;
                    waywal_img_metadata_t *meta = (waywal_img_metadata_t *)mapped;
                    if (hdr_size < sizeof(waywal_img_metadata_t)) {
                        compat_buf = (uint8_t *)calloc(1, sizeof(waywal_img_metadata_t) +
                                                              raw_meta->extra_data_len + 1u);
                        if (compat_buf) {
                            memcpy(compat_buf, mapped, hdr_size);
                            if (raw_meta->extra_data_len > 0) {
                                memcpy(compat_buf + sizeof(waywal_img_metadata_t),
                                       (const uint8_t *)mapped + hdr_size,
                                       raw_meta->extra_data_len);
                            }
                            meta = (waywal_img_metadata_t *)compat_buf;
                        }
                    }

                    bool valid_strings = true;
                    if (meta->num_target_outputs > 64) {
                        valid_strings = false;
                    } else {
                        const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
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
                        const uint8_t *pixels =
                            (const uint8_t *)mapped + hdr_size + meta->extra_data_len;
                        WAYWAL_LOG_INFO("Rendering received image: %ux%u", meta->width,
                                        meta->height);
                        if (state->outputs) {
                            bool has_transition =
                                (meta->transition_type != WAYWAL_TRANSITION_NONE &&
                                 meta->transition_duration_ms > 0);
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (output_matches_img_meta(out, meta)) {
                                    if (has_transition) {
                                        layer_shell_snapshot_live_output(state, out);
                                    }
                                    stop_engines_on_output(state, out, false, false, false, false);
                                }
                            }
                            reconcile_global_engine_flags(state);
                        } else {
                            if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
                                video_engine_stop(&state->video_engine);
                            }
                            slideshow_stop(&state->slideshow);
                            shader_engine_stop_playback(state);
                            anim_engine_stop_playback(state);
                        }
                        if (state->power_gov) {
                            waywal_power_governor_set_global_reason(
                                state->power_gov, WAYWAL_PAUSE_USER_MANUAL, false);
                        }

                        if (meta->backdrop[0] != '\0') {
                            state->backdrop_auto_blur =
                                (strcasecmp(meta->backdrop, "auto-blur") == 0);
                            if (strcasecmp(meta->backdrop, "none") == 0) {
                                state->backdrop_path[0] = '\0';
                            } else if (!state->backdrop_auto_blur) {
                                strncpy(state->backdrop_path, meta->backdrop,
                                        sizeof(state->backdrop_path) - 1);
                                state->backdrop_path[sizeof(state->backdrop_path) - 1] = '\0';
                            }
                        }
                        if (meta->layer_name[0] != '\0') {
                            state->use_bottom_layer = (strcasecmp(meta->layer_name, "bottom") == 0);
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (out->layer_surface && out->surface) {
                                    zwlr_layer_surface_v1_set_layer(
                                        out->layer_surface,
                                        state->use_bottom_layer
                                            ? ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM
                                            : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
                                }
                            }
                        }
                        if (meta->battery_policy[0] != '\0' && state->power_gov) {
                            waywal_battery_policy_t bpol = WAYWAL_BATTERY_REDUCE_FPS;
                            if (strcasecmp(meta->battery_policy, "ignore") == 0) {
                                bpol = WAYWAL_BATTERY_IGNORE;
                            } else if (strcasecmp(meta->battery_policy, "pause-video") == 0) {
                                bpol = WAYWAL_BATTERY_PAUSE_VIDEO;
                            }
                            waywal_power_governor_set_battery_policy(state->power_gov, bpol);
                        }

                        daemon_update_palette_from_bgra(state, pixels, meta->width, meta->height,
                                                        meta, false);

                        const char *first_target = "*";
                        if (meta->num_target_outputs > 0) {
                            first_target = (const char *)meta + sizeof(waywal_img_metadata_t);
                        }
                        bool is_anim =
                            try_start_native_animation(state, meta->filepath, meta, first_target);
                        bool direct_depth_parallax = false;
                        if (!is_anim && meta->depth_map_path[0] != '\0' &&
                            (meta->transition_type == WAYWAL_TRANSITION_NONE ||
                             meta->transition_duration_ms == 0)) {
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (!output_matches_img_meta(out, meta))
                                    continue;
                                out->scaling_mode = meta->scaling_mode;
                                out->filter_mode = meta->filter_mode;
                                out->ten_bit = meta->ten_bit;
                            }
                            direct_depth_parallax = daemon_activate_depth_parallax(
                                state, pixels, meta->width, meta->height, meta, first_target);
                        }

                        if (direct_depth_parallax) {
                            ok = true;
                        } else {
                            ok = daemon_start_image_transition(state, pixels, meta->width,
                                                               meta->height, meta);
                            if (ok && !is_anim && meta->depth_map_path[0] != '\0') {
                                (void)daemon_activate_depth_parallax(
                                    state, pixels, meta->width, meta->height, meta, first_target);
                            }
                        }

                        if (ok) {
                            waywal_output_saved_state_t s_entry;
                            memset(&s_entry, 0, sizeof(s_entry));
                            s_entry.kind =
                                is_anim ? WAYWAL_CONTENT_ANIMATION : WAYWAL_CONTENT_STATIC_IMAGE;
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
                                    (const char *)meta + sizeof(waywal_img_metadata_t);
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
                                        memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
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
                    free(compat_buf);
                } else {
                    WAYWAL_LOG_ERR(
                        "IPC image payload bounds check failed: %ux%u, size=%zu, expected=%lu",
                        raw_meta->width, raw_meta->height, req_hdr.payload_size,
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
        if (resp_hdr.opcode == WAYWAL_RESP_OK) {
            daemon_broadcast_event(state, "wallpaper_changed");
        }
        break;
    }

    case WAYWAL_REQ_SET_VIDEO: {
        bool ok = false;
        waywal_video_payload_t vpay;
        memset(&vpay, 0, sizeof(vpay));
        bool has_vpay = false;
        output_node_t *target_out = NULL;
        char extra_buf[4096];
        size_t extra_len = 0;
        bool has_extra_targets = false;

        if (req_hdr.payload_size >= sizeof(waywal_video_payload_t)) {
            if (read_exact(client_fd, &vpay, sizeof(vpay))) {
                vpay.filepath[sizeof(vpay.filepath) - 1] = '\0';
                has_vpay = true;

                extra_len = req_hdr.payload_size - sizeof(waywal_video_payload_t);
                if (vpay.num_target_outputs > 0 && vpay.num_target_outputs <= 64 && extra_len > 0 &&
                    extra_len <= sizeof(extra_buf)) {
                    if (read_exact(client_fd, extra_buf, extra_len)) {
                        extra_buf[extra_len - 1] = '\0';
                        has_extra_targets = true;
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

        bool shared_existing_ve = (has_extra_targets && has_vpay && attached_fd < 0 &&
                                   state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED &&
                                   state->video_engine.filepath[0] != '\0' &&
                                   strcmp(state->video_engine.filepath, vpay.filepath) == 0);

        if (shared_existing_ve) {
            const char *ptr = extra_buf;
            size_t remaining = extra_len;
            for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0; ++i) {
                const char *end = (const char *)memchr(ptr, '\0', remaining);
                if (!end)
                    break;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (strcmp(out->name, ptr) == 0) {
                        stop_engines_on_output(state, out, false, false,
                                               out->video_eng == &state->video_engine, false);
                        out->video_eng = &state->video_engine;
                        out->video_targeted = true;
                        out->scaling_mode = vpay.scaling_mode;
                        out->vp_configured = false;
                    }
                }
                size_t step = (size_t)(end - ptr) + 1;
                ptr += step;
                remaining -= step;
            }
            state->video_engine.loop_count = vpay.loop_count;
            state->video_engine.playback_speed =
                vpay.playback_speed > 0.01f ? vpay.playback_speed : 1.0f;
            if (state->video_engine.target_output != target_out) {
                state->video_engine.target_output = NULL;
            }
            reconcile_global_engine_flags(state);
            ok = true;
        } else {
            if (state->outputs) {
                if (has_extra_targets) {
                    const char *ptr = extra_buf;
                    size_t remaining = extra_len;
                    for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0; ++i) {
                        const char *end = (const char *)memchr(ptr, '\0', remaining);
                        if (!end)
                            break;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (strcmp(out->name, ptr) == 0) {
                                stop_engines_on_output(state, out, false, false,
                                                       out->video_eng == &state->video_engine,
                                                       false);
                            }
                        }
                        size_t step = (size_t)(end - ptr) + 1;
                        ptr += step;
                        remaining -= step;
                    }
                } else {
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        stop_engines_on_output(state, out, false, false,
                                               out->video_eng == &state->video_engine, false);
                    }
                    slideshow_stop(&state->slideshow);
                    shader_engine_stop_playback(state);
                    anim_engine_stop_playback(state);
                }
                reconcile_global_engine_flags(state);
            } else {
                slideshow_stop(&state->slideshow);
                shader_engine_stop_playback(state);
                anim_engine_stop_playback(state);
            }

            video_engine_t *ve = &state->video_engine;
            if (has_extra_targets && state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED &&
                (!target_out || state->video_engine.target_output != target_out)) {
                video_engine_t *custom_ve = (video_engine_t *)calloc(1, sizeof(video_engine_t));
                if (custom_ve && video_engine_init(custom_ve, state)) {
                    if (custom_ve->timer_fd >= 0) {
                        register_fd_in_loops(state, custom_ve->timer_fd, URING_EV_TIMER);
                    }
                    ve = custom_ve;
                } else {
                    free(custom_ve);
                }
            }

            if (has_vpay) {
                ve->loop_count = vpay.loop_count;
                ve->playback_speed = vpay.playback_speed > 0.01f ? vpay.playback_speed : 1.0f;
            }
            ve->has_target_filter = has_extra_targets;
            if (ve == &state->video_engine) {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    out->video_targeted = !has_extra_targets;
                }
                if (has_extra_targets) {
                    const char *ptr = extra_buf;
                    size_t remaining = extra_len;
                    for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0; ++i) {
                        const char *end = (const char *)memchr(ptr, '\0', remaining);
                        if (!end)
                            break;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (strcmp(out->name, ptr) == 0) {
                                out->video_targeted = true;
                            }
                        }
                        size_t step = (size_t)(end - ptr) + 1;
                        ptr += step;
                        remaining -= step;
                    }
                }
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (!out->video_targeted && out->video_eng == &state->video_engine) {
                        out->video_eng = NULL;
                    }
                }
            }

            if (has_extra_targets) {
                const char *ptr = extra_buf;
                size_t remaining = extra_len;
                for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0; ++i) {
                    const char *end = (const char *)memchr(ptr, '\0', remaining);
                    if (!end)
                        break;
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (strcmp(out->name, ptr) == 0) {
                            out->video_eng = ve;
                            if (has_vpay) {
                                out->scaling_mode = vpay.scaling_mode;
                                out->vp_configured = false;
                            }
                        }
                    }
                    size_t step = (size_t)(end - ptr) + 1;
                    ptr += step;
                    remaining -= step;
                }
            } else {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    out->video_eng = ve;
                    if (has_vpay) {
                        out->scaling_mode = vpay.scaling_mode;
                        out->vp_configured = false;
                    }
                }
            }

            if (attached_fd >= 0) {
                struct stat st;
                if (fstat(attached_fd, &st) == 0 && st.st_size > 0) {
                    ok = video_engine_load_mem(ve, attached_fd, (size_t)st.st_size, target_out);
                    attached_fd = -1;
                } else {
                    close(attached_fd);
                    attached_fd = -1;
                }
            } else if (has_vpay) {
                ok = video_engine_load_file(ve, vpay.filepath, target_out);
            }

            if (!ok) {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (out->video_eng == ve) {
                        out->video_eng = NULL;
                    }
                }
                if (ve != &state->video_engine) {
                    if (ve->timer_fd >= 0) {
                        unregister_fd_from_loops(state, ve->timer_fd);
                    }
                    video_engine_destroy(ve);
                    free(ve);
                }
            }
        }

        if (ok && has_vpay) {
            daemon_reconcile_power_state(state);
            waywal_output_saved_state_t s_entry;
            memset(&s_entry, 0, sizeof(s_entry));
            s_entry.kind = WAYWAL_CONTENT_VIDEO;
            strncpy(s_entry.media_path, vpay.filepath, sizeof(s_entry.media_path) - 1);
            s_entry.scaling = (waywal_scaling_mode_t)vpay.scaling_mode;
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
        if (ok) {
            daemon_broadcast_event(state, "wallpaper_changed");
        }
        break;
    }

    case WAYWAL_REQ_PAUSE: {
        video_engine_pause(&state->video_engine);
        video_engine_t *seen_ve[32];
        size_t num_seen_ve = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->video_eng && out->video_eng != &state->video_engine) {
                bool already = false;
                for (size_t k = 0; k < num_seen_ve; ++k) {
                    if (seen_ve[k] == out->video_eng) {
                        already = true;
                        break;
                    }
                }
                if (!already && num_seen_ve < 32) {
                    seen_ve[num_seen_ve++] = out->video_eng;
                    video_engine_pause(out->video_eng);
                }
            }
        }
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
        daemon_broadcast_event(state, "power_changed");
        break;
    }

    case WAYWAL_REQ_UNPAUSE: {
        if (state->power_gov) {
            waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_USER_MANUAL,
                                                    false);
        }
        video_engine_resume(&state->video_engine);
        video_engine_t *seen_ve[32];
        size_t num_seen_ve = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->video_eng && out->video_eng != &state->video_engine) {
                bool already = false;
                for (size_t k = 0; k < num_seen_ve; ++k) {
                    if (seen_ve[k] == out->video_eng) {
                        already = true;
                        break;
                    }
                }
                if (!already && num_seen_ve < 32) {
                    seen_ve[num_seen_ve++] = out->video_eng;
                    video_engine_resume(out->video_eng);
                }
            }
        }
        daemon_reconcile_power_state(state);
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        daemon_broadcast_event(state, "power_changed");
        break;
    }

    case WAYWAL_REQ_TOGGLE: {
        bool manual_paused =
            (state->video_engine.state == WAYWAL_VIDEO_STATE_PAUSED) ||
            (state->power_gov && (waywal_power_governor_get_output_mask(state->power_gov, NULL) &
                                  WAYWAL_PAUSE_USER_MANUAL) != 0);
        for (output_node_t *out = state->outputs; out != NULL && !manual_paused; out = out->next) {
            if (out->video_eng && out->video_eng->state == WAYWAL_VIDEO_STATE_PAUSED) {
                manual_paused = true;
            }
        }
        video_engine_t *seen_ve[32];
        size_t num_seen_ve = 0;
        if (manual_paused) {
            if (state->power_gov) {
                waywal_power_governor_set_global_reason(state->power_gov, WAYWAL_PAUSE_USER_MANUAL,
                                                        false);
            }
            if (state->video_engine.state == WAYWAL_VIDEO_STATE_PAUSED) {
                video_engine_resume(&state->video_engine);
            }
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (out->video_eng && out->video_eng != &state->video_engine &&
                    out->video_eng->state == WAYWAL_VIDEO_STATE_PAUSED) {
                    bool already = false;
                    for (size_t k = 0; k < num_seen_ve; ++k) {
                        if (seen_ve[k] == out->video_eng) {
                            already = true;
                            break;
                        }
                    }
                    if (!already && num_seen_ve < 32) {
                        seen_ve[num_seen_ve++] = out->video_eng;
                        video_engine_resume(out->video_eng);
                    }
                }
            }
            daemon_reconcile_power_state(state);
        } else {
            if (state->video_engine.state == WAYWAL_VIDEO_STATE_PLAYING) {
                video_engine_pause(&state->video_engine);
            }
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (out->video_eng && out->video_eng != &state->video_engine &&
                    out->video_eng->state == WAYWAL_VIDEO_STATE_PLAYING) {
                    bool already = false;
                    for (size_t k = 0; k < num_seen_ve; ++k) {
                        if (seen_ve[k] == out->video_eng) {
                            already = true;
                            break;
                        }
                    }
                    if (!already && num_seen_ve < 32) {
                        seen_ve[num_seen_ve++] = out->video_eng;
                        video_engine_pause(out->video_eng);
                    }
                }
            }
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
        }
        resp_hdr.opcode = WAYWAL_RESP_OK;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        daemon_broadcast_event(state, "power_changed");
        break;
    }

    case WAYWAL_REQ_SLIDESHOW: {
        waywal_slideshow_payload_t spay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(spay)) {
            if (read_exact(client_fd, &spay, sizeof(spay))) {
                spay.path[sizeof(spay.path) - 1] = '\0';
                ok = daemon_start_slideshow_isolated(state, &spay);
                if (ok) {
                    waywal_output_saved_state_t s_entry;
                    memset(&s_entry, 0, sizeof(s_entry));
                    s_entry.kind = WAYWAL_CONTENT_SLIDESHOW;
                    strncpy(s_entry.media_path, spay.path, sizeof(s_entry.media_path) - 1);
                    s_entry.slideshow_interval_sec = spay.interval_s;
                    s_entry.scaling = (waywal_scaling_mode_t)spay.scaling_mode;
                    s_entry.filter = (waywal_filter_mode_t)spay.filter_mode;
                    s_entry.ten_bit = spay.ten_bit;
                    if (spay.num_target_outputs > 0) {
                        uint32_t max_t =
                            spay.num_target_outputs <= 16 ? spay.num_target_outputs : 16;
                        for (uint32_t i = 0; i < max_t; ++i) {
                            if (spay.target_outputs[i][0] != '\0') {
                                memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                                strncpy(s_entry.output_name, spay.target_outputs[i],
                                        sizeof(s_entry.output_name) - 1);
                                (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                            }
                        }
                    } else {
                        strncpy(s_entry.output_name, "*", sizeof(s_entry.output_name) - 1);
                        (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                    }
                    (void)waywal_state_store_save(&g_saved_state, NULL);
                }
            }
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (ok) {
            daemon_broadcast_event(state, "wallpaper_changed");
        }
        break;
    }

    case WAYWAL_REQ_SLIDESHOW_CTRL: {
        waywal_slideshow_ctrl_t sctrl;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(sctrl)) {
            if (read_exact(client_fd, &sctrl, sizeof(sctrl))) {
                ok = slideshow_control(&state->slideshow, sctrl.action);
                if (sctrl.action == WAYWAL_SLIDESHOW_STOP) {
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (out->slideshow_eng == &state->slideshow) {
                            out->slideshow_eng = NULL;
                        }
                    }
                }
                slideshow_engine_t *seen[32];
                size_t num_seen = 0;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    slideshow_engine_t *ss = out->slideshow_eng;
                    if (!ss || ss == &state->slideshow) {
                        continue;
                    }
                    bool already = false;
                    for (size_t k = 0; k < num_seen; ++k) {
                        if (seen[k] == ss) {
                            already = true;
                            break;
                        }
                    }
                    if (!already && num_seen < 32) {
                        seen[num_seen++] = ss;
                        if (slideshow_control(ss, sctrl.action)) {
                            ok = true;
                        }
                    }
                }
                if (sctrl.action == WAYWAL_SLIDESHOW_STOP) {
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        out->slideshow_eng = NULL;
                    }
                    for (size_t k = 0; k < num_seen; ++k) {
                        if (seen[k]->timer_fd >= 0) {
                            unregister_fd_from_loops(state, seen[k]->timer_fd);
                        }
                        slideshow_destroy(seen[k]);
                        free(seen[k]);
                    }
                }
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
                s_entry.kind = WAYWAL_CONTENT_SHADER;
                strncpy(s_entry.media_path, spay.shader_path, sizeof(s_entry.media_path) - 1);
                s_entry.target_fps = spay.fps > 0 ? spay.fps : 60;
                s_entry.audio_reactive = spay.audio_reactive;
                s_entry.scaling = (waywal_scaling_mode_t)spay.scaling_mode;
                if (spay.output[0] != '\0' && strchr(spay.output, ',') != NULL) {
                    char out_copy[sizeof(spay.output)];
                    strncpy(out_copy, spay.output, sizeof(out_copy) - 1);
                    out_copy[sizeof(out_copy) - 1] = '\0';
                    char *saveptr = NULL;
                    for (char *tok = strtok_r(out_copy, ",", &saveptr); tok != NULL;
                         tok = strtok_r(NULL, ",", &saveptr)) {
                        while (*tok == ' ')
                            tok++;
                        if (*tok != '\0') {
                            memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                            strncpy(s_entry.output_name, tok, sizeof(s_entry.output_name) - 1);
                            (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                        }
                    }
                } else {
                    strncpy(s_entry.output_name, spay.output[0] ? spay.output : "*",
                            sizeof(s_entry.output_name) - 1);
                    (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                }
                (void)waywal_state_store_save(&g_saved_state, NULL);
            }
        }

        if (ok) {
            const char *msg = "OK: Shader active\n";
            resp_hdr.opcode = WAYWAL_RESP_OK;
            resp_hdr.payload_size = strlen(msg);
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            (void)write_exact(client_fd, msg, strlen(msg));
            daemon_broadcast_event(state, "wallpaper_changed");
        } else {
            resp_hdr.opcode = WAYWAL_RESP_ERR;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
        }
        break;
    }

    case WAYWAL_REQ_WORKSHOP: {
        waywal_workshop_payload_t wpay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(wpay) && read_exact(client_fd, &wpay, sizeof(wpay))) {
            wpay.output[sizeof(wpay.output) - 1] = '\0';
            wpay.workshop_dir[sizeof(wpay.workshop_dir) - 1] = '\0';
            wpay.prop_overrides[sizeof(wpay.prop_overrides) - 1] = '\0';

            ok = daemon_activate_workshop(state, &wpay);
            if (ok) {
                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                s_entry.kind = WAYWAL_CONTENT_WORKSHOP;
                const char *saved_ws_dir =
                    state->workshop_dir[0] != '\0' ? state->workshop_dir : wpay.workshop_dir;
                strncpy(s_entry.media_path, saved_ws_dir, sizeof(s_entry.media_path) - 1);
                strncpy(s_entry.prop_overrides, wpay.prop_overrides,
                        sizeof(s_entry.prop_overrides) - 1);
                s_entry.target_fps = wpay.fps > 0 ? wpay.fps : 60;
                s_entry.audio_reactive = wpay.audio_reactive;
                s_entry.scaling = (waywal_scaling_mode_t)wpay.scaling_mode;
                if (wpay.output[0] != '\0' && strchr(wpay.output, ',') != NULL) {
                    char out_copy[sizeof(wpay.output)];
                    strncpy(out_copy, wpay.output, sizeof(out_copy) - 1);
                    out_copy[sizeof(out_copy) - 1] = '\0';
                    char *saveptr = NULL;
                    for (char *tok = strtok_r(out_copy, ",", &saveptr); tok != NULL;
                         tok = strtok_r(NULL, ",", &saveptr)) {
                        while (*tok == ' ')
                            tok++;
                        if (*tok != '\0') {
                            memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                            strncpy(s_entry.output_name, tok, sizeof(s_entry.output_name) - 1);
                            (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                        }
                    }
                } else {
                    strncpy(s_entry.output_name, wpay.output[0] ? wpay.output : "*",
                            sizeof(s_entry.output_name) - 1);
                    (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                }
                (void)waywal_state_store_save(&g_saved_state, NULL);
            }
        }

        if (ok) {
            const char *msg = "OK: Workshop active\n";
            resp_hdr.opcode = WAYWAL_RESP_OK;
            resp_hdr.payload_size = strlen(msg);
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            (void)write_exact(client_fd, msg, strlen(msg));
            daemon_broadcast_event(state, "wallpaper_changed");
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
        buf_appendf(stats_buf, sizeof(stats_buf), &off,
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
            if (off + 1 >= sizeof(stats_buf))
                break;
            uint32_t pmask =
                state->power_gov
                    ? waywal_power_governor_get_output_mask(state->power_gov, out->name)
                    : 0;
            int32_t fscale =
                out->fractional_scale > 0 ? out->fractional_scale : (out->scale_factor * 120);
            const char *eng_kind = "static";
            if (out->workshop_active) {
                eng_kind = "workshop";
            } else if (out->shader_active) {
                eng_kind = "shader";
            } else if (out->video_eng) {
                eng_kind = "video";
            } else if (out->slideshow_eng) {
                eng_kind = "slideshow";
            }
            buf_appendf(stats_buf, sizeof(stats_buf), &off,
                        "  - %s: physical=%dx%d logical=%ux%u scale=%d/120 "
                        "vblank_period=%lldns pause_mask=0x%02x engine=%s\n",
                        out->name[0] ? out->name : "unknown", out->width, out->height,
                        out->logical_width, out->logical_height, fscale,
                        (long long)out->pres_sync.pll.period_est_ns, pmask, eng_kind);
        }

        resp_hdr.opcode = WAYWAL_RESP_OK;
        resp_hdr.payload_size = (uint32_t)off;
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
        daemon_broadcast_event(state, "state_restored");
        break;
    }

    case WAYWAL_REQ_QUERY_JSON: {
        char json_buf[16384];
        size_t len = daemon_build_status_json(state, NULL, json_buf, sizeof(json_buf));
        resp_hdr.opcode = WAYWAL_RESP_INFO;
        resp_hdr.payload_size = len;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (len > 0) {
            (void)write_exact(client_fd, json_buf, len);
        }
        break;
    }

    case WAYWAL_REQ_SET_UNIFORM: {
        waywal_set_uniform_payload_t upay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(upay) && read_exact(client_fd, &upay, sizeof(upay))) {
            upay.output[sizeof(upay.output) - 1] = '\0';
            upay.name[sizeof(upay.name) - 1] = '\0';
            if (upay.name[0] != '\0' && upay.num_components >= 1 && upay.num_components <= 4) {
                bool valid_floats = true;
                for (uint32_t i = 0; i < upay.num_components; ++i) {
                    if (!isfinite(upay.values[i])) {
                        valid_floats = false;
                        break;
                    }
                }
                if (valid_floats) {
                    char prop_kv[160];
                    size_t kv_off = 0;
                    buf_appendf(prop_kv, sizeof(prop_kv), &kv_off, "%s=", upay.name);
                    for (uint32_t i = 0; i < upay.num_components; ++i) {
                        buf_appendf(prop_kv, sizeof(prop_kv), &kv_off, "%s%.6g", (i > 0) ? "," : "",
                                    (double)upay.values[i]);
                    }

                    bool apply_global = (!state->outputs || upay.output[0] == '\0' ||
                                         strcmp(upay.output, "*") == 0);
                    if (apply_global) {
                        if (!state->shader_eng && !state->we_scene) {
                            if (state->dmabuf_ctx.available) {
                                render_engine_make_current(&state->render_engine);
                                state->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
                            } else {
                                state->shader_eng = waywal_shader_engine_create(NULL);
                            }
                        }
                        if (state->shader_eng) {
                            if (waywal_shader_engine_set_custom_uniform(state->shader_eng,
                                                                        upay.name, upay.values,
                                                                        upay.num_components)) {
                                ok = true;
                            }
                        }
                        if (state->we_scene) {
                            if (we_scene_set_uniform_override(state->we_scene, upay.name,
                                                              upay.values, upay.num_components)) {
                                ok = true;
                                upsert_workshop_prop_kv(state->workshop_props,
                                                        sizeof(state->workshop_props), upay.name,
                                                        prop_kv);
                            }
                        }
                    }
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (!output_matches_target_spec(out, upay.output)) {
                            continue;
                        }
                        if (out->shader_eng &&
                            (!apply_global || out->shader_eng != state->shader_eng)) {
                            if (waywal_shader_engine_set_custom_uniform(
                                    out->shader_eng, upay.name, upay.values, upay.num_components)) {
                                ok = true;
                            }
                        }
                        if (out->we_scene) {
                            bool scene_ok = true;
                            if (!apply_global || out->we_scene != state->we_scene) {
                                scene_ok = we_scene_set_uniform_override(
                                    out->we_scene, upay.name, upay.values, upay.num_components);
                            }
                            if (scene_ok) {
                                ok = true;
                                upsert_workshop_prop_kv(out->workshop_props,
                                                        sizeof(out->workshop_props), upay.name,
                                                        prop_kv);
                            }
                        }
                    }
                    if (ok) {
                        bool saved_dirty = false;
                        for (size_t k = 0; k < g_saved_state.count; ++k) {
                            waywal_output_saved_state_t *se = &g_saved_state.outputs[k];
                            if (se->kind != WAYWAL_CONTENT_WORKSHOP)
                                continue;
                            if (apply_global || strcmp(se->output_name, upay.output) == 0 ||
                                strcmp(se->output_name, "*") == 0) {
                                upsert_workshop_prop_kv(se->prop_overrides,
                                                        sizeof(se->prop_overrides), upay.name,
                                                        prop_kv);
                                saved_dirty = true;
                            }
                        }
                        if (saved_dirty) {
                            (void)waywal_state_store_save(&g_saved_state, NULL);
                        }
                    }
                }
            }
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (ok) {
            if (state->shader_active) {
                daemon_shader_render_once(state);
            }
            daemon_broadcast_event(state, "uniform_changed");
        }
        break;
    }

    case WAYWAL_REQ_AUDIO_CTRL: {
        waywal_audio_ctrl_payload_t apay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(apay) && read_exact(client_fd, &apay, sizeof(apay))) {
            if (!state->audio_fft) {
                state->audio_fft = waywal_audio_fft_create(48000, false);
            }
            if (isfinite(apay.volume) && apay.volume >= 0.0f) {
                float clamped = apay.volume > 1.0f ? 1.0f : apay.volume;
                state->audio_volume = clamped;
                if (state->audio_fft) {
                    waywal_audio_fft_set_volume(state->audio_fft, clamped);
                }
            }
            if (apay.mute_on_audio == 0 || apay.mute_on_audio == 1) {
                state->mute_on_audio = (apay.mute_on_audio == 1);
                if (state->audio_fft) {
                    waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
                }
            }
            if (apay.theme_from_media == 0 || apay.theme_from_media == 1) {
                bool prev_tfm = state->theme_from_media;
                state->theme_from_media = (apay.theme_from_media == 1);
                if (state->theme_from_media != prev_tfm) {
                    const waywal_mpris_state_t *ms =
                        state->mpris ? waywal_mpris_get_state(state->mpris) : NULL;
                    daemon_apply_mpris_album_art_and_theme(state, ms, true);
                }
            }
            ok = true;
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (ok) {
            daemon_broadcast_event(state, "audio_changed");
        }
        break;
    }

    case WAYWAL_REQ_SUBSCRIBE: {
        char json_buf[16384];
        size_t len = daemon_build_status_json(state, "initial", json_buf, sizeof(json_buf));
        resp_hdr.opcode = WAYWAL_RESP_OK;
        resp_hdr.payload_size = (uint32_t)len;
        if (waywal_ipc_send(client_fd, &resp_hdr, -1) &&
            (len == 0 || write_exact(client_fd, json_buf, len))) {
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags >= 0) {
                (void)fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            }
            daemon_reap_dead_subscribers(state);
            if (state->num_subscribers >= 16) {
                close(state->subscriber_fds[0]);
                for (size_t j = 1; j < 16; ++j) {
                    state->subscriber_fds[j - 1] = state->subscriber_fds[j];
                }
                state->num_subscribers = 15;
            }
            state->subscriber_fds[state->num_subscribers++] = client_fd;
            if (attached_fd >= 0) {
                close(attached_fd);
            }
            return;
        }
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
        bool prev_tfm = st->theme_from_media;
        st->use_bottom_layer = (st->config.daemon.layer_pos == WAYWAL_LAYER_BOTTOM);
        st->backdrop_auto_blur = (strcasecmp(st->config.daemon.backdrop, "auto-blur") == 0);
        st->idle_timeout_sec = st->config.daemon.idle_timeout_sec;
        st->audio_volume = st->config.daemon.volume;
        st->mute_on_audio = st->config.daemon.mute_on_audio;
        st->theme_from_media = st->config.theme.theme_from_media;
        if (st->theme_from_media != prev_tfm) {
            const waywal_mpris_state_t *ms = st->mpris ? waywal_mpris_get_state(st->mpris) : NULL;
            daemon_apply_mpris_album_art_and_theme(st, ms, true);
        }
        if (st->audio_fft) {
            waywal_audio_fft_set_volume(st->audio_fft, st->audio_volume);
            waywal_audio_fft_set_mute_on_audio(st->audio_fft, st->mute_on_audio);
        }
        if (st->power_gov) {
            waywal_power_governor_set_battery_policy(st->power_gov,
                                                     st->config.daemon.battery_policy_mode);
            waywal_power_governor_set_pause_policies(st->power_gov,
                                                     st->config.daemon.auto_pause_fullscreen,
                                                     st->config.daemon.auto_pause_maximized,
                                                     st->config.daemon.pause_on_occluded_workspace);
        }
        bool slideshow_rule_applied = false;
        for (size_t i = 0; i < st->config.output_count; ++i) {
            if (strcasecmp(st->config.outputs[i].mode, "slideshow") == 0) {
                slideshow_rule_applied = true;
            }
            (void)daemon_apply_output_config_rule(st, &st->config.outputs[i]);
        }
        if (st->slideshow.active && !slideshow_rule_applied) {
            st->slideshow.transition_type = st->config.transition.type_enum;
            st->slideshow.transition_duration_ms =
                (uint32_t)(st->config.transition.duration * 1000.0f);
            st->slideshow.transition_fps = st->config.transition.fps;
            st->slideshow.transition_angle_rad =
                st->config.transition.angle * (3.14159265358979323846f / 180.0f);
            st->slideshow.transition_wave_freq = st->config.transition.wave_x;
            st->slideshow.transition_wave_amp = st->config.transition.wave_y;
            st->slideshow.transition_center_x = st->config.transition.pos_x;
            st->slideshow.transition_center_y = st->config.transition.pos_y;
            st->slideshow.bezier.p1x = st->config.transition.bezier[0];
            st->slideshow.bezier.p1y = st->config.transition.bezier[1];
            st->slideshow.bezier.p2x = st->config.transition.bezier[2];
            st->slideshow.bezier.p2y = st->config.transition.bezier[3];
            st->slideshow.has_custom_bezier = true;
            st->slideshow.ten_bit = st->config.daemon.ten_bit;
            (void)slideshow_rescan(&st->slideshow);
        }
        if (st->config.theme.enabled && st->has_active_palette) {
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
        daemon_reconcile_power_state(st);
        daemon_broadcast_event(st, "config_reloaded");
        WAYWAL_LOG_INFO("Reloaded configuration on SIGHUP");
    }
}

static bool daemon_dispatch_per_output_fd(daemon_state_t *st, int fd)
{
    if (!st || fd < 0)
        return false;
    for (output_node_t *out = st->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && fd == waywal_shader_engine_get_inotify_fd(out->shader_eng)) {
            daemon_pump_wayland_events(st);
            if (waywal_shader_engine_poll_hot_reload(out->shader_eng) && st->shader_active) {
                daemon_shader_render_once(st);
            }
            return true;
        }
        if (out->video_eng && out->video_eng != &st->video_engine &&
            fd == out->video_eng->timer_fd) {
            daemon_pump_wayland_events(st);
            video_engine_dispatch_frame(out->video_eng);
            return true;
        }
        if (out->slideshow_eng && out->slideshow_eng != &st->slideshow &&
            fd == out->slideshow_eng->timer_fd) {
            daemon_pump_wayland_events(st);
            slideshow_dispatch_tick(out->slideshow_eng);
            return true;
        }
    }
    return false;
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
    case URING_EV_SHADER_INOTIFY:
    case URING_EV_INOTIFY:
        if ((res & POLLIN) == 0)
            break;
        daemon_pump_wayland_events(st);
        if (!daemon_dispatch_per_output_fd(st, fd) && st->shader_eng &&
            fd == waywal_shader_engine_get_inotify_fd(st->shader_eng)) {
            if (waywal_shader_engine_poll_hot_reload(st->shader_eng) && st->shader_active) {
                daemon_shader_render_once(st);
            }
        }
        break;
    case URING_EV_POWER_IPC:
    case URING_EV_POWER_INOTIFY:
        if ((res & POLLIN) == 0)
            break;
        daemon_handle_power_gov_events(st, fd);
        break;
    case URING_EV_TIMER:
    case URING_EV_SHADER_TIMER:
    case URING_EV_ANIM_TIMER:
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
        } else if (daemon_dispatch_per_output_fd(st, fd)) {
            /* Dispatched per-output engine FD */
        } else if (st->shader_eng && fd == waywal_shader_engine_get_inotify_fd(st->shader_eng)) {
            if (waywal_shader_engine_poll_hot_reload(st->shader_eng) && st->shader_active) {
                daemon_shader_render_once(st);
            }
        } else if (st->power_gov && (fd == waywal_power_governor_get_ipc_fd(st->power_gov) ||
                                     fd == waywal_power_governor_get_inotify_fd(st->power_gov))) {
            daemon_handle_power_gov_events(st, fd);
        } else if (st->mpris && fd == waywal_mpris_get_fd(st->mpris)) {
            waywal_mpris_dispatch(st->mpris);
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
    state.audio_volume = state.config.daemon.volume;
    state.mute_on_audio = state.config.daemon.mute_on_audio;
    state.theme_from_media = state.config.theme.theme_from_media;
    if (state.config.daemon.backdrop[0] != '\0' &&
        strcasecmp(state.config.daemon.backdrop, "none") != 0 && !state.backdrop_auto_blur) {
        strncpy(state.backdrop_path, state.config.daemon.backdrop, sizeof(state.backdrop_path) - 1);
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
                                                 {"volume", required_argument, NULL, 1004},
                                                 {"mute-on-audio", no_argument, NULL, 1005},
                                                 {"no-mute-on-audio", no_argument, NULL, 1006},
                                                 {"theme-from-media", no_argument, NULL, 1007},
                                                 {"no-theme-from-media", no_argument, NULL, 1008},
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
        case 1004: {
            float v = strtof(optarg, NULL);
            if (isfinite(v)) {
                if (v < 0.0f)
                    v = 0.0f;
                else if (v > 1.0f)
                    v = 1.0f;
                state.audio_volume = v;
            }
            break;
        }
        case 1005:
            state.mute_on_audio = true;
            break;
        case 1006:
            state.mute_on_audio = false;
            break;
        case 1007:
            state.theme_from_media = true;
            break;
        case 1008:
            state.theme_from_media = false;
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

    state.audio_fft = waywal_audio_fft_create(48000, false);
    if (state.audio_fft) {
        waywal_audio_fft_set_volume(state.audio_fft, state.audio_volume);
        waywal_audio_fft_set_mute_on_audio(state.audio_fft, state.mute_on_audio);
    }

    state.power_gov = waywal_power_governor_create(battery_policy);
    if (state.power_gov) {
        waywal_power_governor_set_pause_policies(state.power_gov,
                                                 state.config.daemon.auto_pause_fullscreen,
                                                 state.config.daemon.auto_pause_maximized,
                                                 state.config.daemon.pause_on_occluded_workspace);
    }
    state.mpris = waywal_mpris_create(daemon_mpris_on_change, &state);
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

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = SIG_DFL;
    sa_chld.sa_flags = SA_NOCLDWAIT | SA_RESTART;
    sigemptyset(&sa_chld.sa_mask);
    (void)sigaction(SIGCHLD, &sa_chld, NULL);

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
    int mpris_fd = state.mpris ? waywal_mpris_get_fd(state.mpris) : -1;

    if (state.transition_timer_fd >= 0) {
        struct epoll_event ev_trans = {.events = EPOLLIN, .data.fd = state.transition_timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.transition_timer_fd, &ev_trans);
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
    if (mpris_fd >= 0) {
        struct epoll_event ev_mp = {.events = EPOLLIN, .data.fd = mpris_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, mpris_fd, &ev_mp);
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
            uring_loop_add_poll(&uring_loop, state.shader_timer_fd, POLLIN, URING_EV_TIMER, &state);
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
        if (mpris_fd >= 0) {
            uring_loop_add_poll(&uring_loop, mpris_fd, POLLIN, URING_EV_TIMER, &state);
        }
        WAYWAL_LOG_INFO("Event loop running via Linux io_uring (zero-syscall polling)");
    } else {
        WAYWAL_LOG_INFO("Event loop running via epoll fallback");
    }

    /* Apply security hardening (PR_SET_NO_NEW_PRIVS & Landlock LSM) */
    security_sandbox_apply(NULL);

    state.on_output_configured = daemon_on_output_configured;
    state.on_wallpaper_loaded = daemon_on_wallpaper_loaded;
    if (state.config.output_count > 0) {
        for (size_t i = 0; i < state.config.output_count; ++i) {
            (void)daemon_apply_output_config_rule(&state, &state.config.outputs[i]);
        }
    } else if (state.config.daemon.restore_on_start) {
        for (output_node_t *out = state.outputs; out != NULL; out = out->next) {
            daemon_on_output_configured(&state, out);
        }
    }

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
                } else if (daemon_dispatch_per_output_fd(&state, ev_fd)) {
                    /* Dispatched per-output engine FD */
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
                } else if (state.mpris && ev_fd == waywal_mpris_get_fd(state.mpris)) {
                    waywal_mpris_dispatch(state.mpris);
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

    for (size_t i = 0; i < state.num_subscribers; ++i) {
        if (state.subscriber_fds[i] >= 0) {
            close(state.subscriber_fds[i]);
            state.subscriber_fds[i] = -1;
        }
    }
    state.num_subscribers = 0;

    for (output_node_t *out = state.outputs; out != NULL; out = out->next) {
        stop_engines_on_output(&state, out, false, false, false, false);
        if (out->shader_eng == state.shader_eng) {
            out->shader_eng = NULL;
        }
        if (out->we_scene == state.we_scene) {
            out->we_scene = NULL;
        }
    }
    shader_engine_stop_playback(&state);
    anim_engine_stop_playback(&state);
    transition_engine_destroy(&state);
    video_engine_destroy(&state.video_engine);
    slideshow_destroy(&state.slideshow);
    if (state.shader_eng) {
        waywal_shader_engine_destroy(state.shader_eng);
        state.shader_eng = NULL;
    }
    if (state.we_scene) {
        we_scene_destroy(state.we_scene);
        state.we_scene = NULL;
    }
    free(state.wallpaper_rgba);
    state.wallpaper_rgba = NULL;
    if (state.audio_fft) {
        waywal_audio_fft_destroy(state.audio_fft);
        state.audio_fft = NULL;
    }
    if (state.mpris) {
        waywal_mpris_destroy(state.mpris);
        state.mpris = NULL;
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
