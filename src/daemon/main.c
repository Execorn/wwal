// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_compute.h"
#include "daemon_internal.h"
#include "wayland_core.h"
#include "waywal/anim_engine.h"
#include "waywal/arena.h"
#include "waywal/audio_fft.h"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/mpris.h"
#include "waywal/power_governor.h"
#include "waywal/security.h"
#include "waywal/shader_engine.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"

#ifndef WAYWAL_VERSION
#define WAYWAL_VERSION "0.2.0"
#endif
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

waywal_state_store_t g_saved_state;

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

void disarm_timer_fd(int timer_fd)
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

void register_fd_in_loops(daemon_state_t *state, int fd, uring_event_type_t ev_type)
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

void unregister_fd_from_loops(daemon_state_t *state, int fd)
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

bool output_matches_target_spec(const output_node_t *out, const char *target_spec)
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
        while (end > p && end[-1] == ' ')
            end--;
        size_t len = (size_t)(end - p);
        if (len == 1 && p[0] == '*')
            return true;
        if (strlen(out->name) == len && strncmp(out->name, p, len) == 0)
            return true;
        if (out->description[0] != '\0') {
            const char *desc_tok = p;
            size_t desc_len = len;
            if (desc_len > 5 && strncasecmp(desc_tok, "desc:", 5) == 0) {
                desc_tok += 5;
                desc_len -= 5;
                while (desc_len > 0 && *desc_tok == ' ') {
                    desc_tok++;
                    desc_len--;
                }
            }
            if (desc_len > 0) {
                if (strlen(out->description) == desc_len &&
                    strncmp(out->description, desc_tok, desc_len) == 0) {
                    return true;
                }
                char sub[128];
                if (desc_len < sizeof(sub)) {
                    memcpy(sub, desc_tok, desc_len);
                    sub[desc_len] = '\0';
                    if (strstr(out->description, sub) != NULL) {
                        return true;
                    }
                }
            }
        }
        p = (*end == ',') ? (end + 1) : end;
    }
    return false;
}

bool output_matches_img_meta(const output_node_t *out, const waywal_img_metadata_t *meta)
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
        if (strcmp(ptr, "*") == 0 || strcmp(out->name, ptr) == 0 ||
            output_matches_target_spec(out, ptr))
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
            strcmp(out->name, spay->target_outputs[i]) == 0 ||
            output_matches_target_spec(out, spay->target_outputs[i])) {
            return true;
        }
    }
    return false;
}

static void detach_all_scene_audio(daemon_state_t *state)
{
    if (!state)
        return;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->we_scene) {
            we_scene_set_audio_output(out->we_scene, NULL);
        }
    }
    if (state->we_scene) {
        we_scene_set_audio_output(state->we_scene, NULL);
    }
}

void reconcile_global_engine_flags(daemon_state_t *state)
{
    if (!state)
        return;

    if (!state->outputs) {
        if (!state->shader_active && !state->workshop_active) {
            disarm_timer_fd(state->shader_timer_fd);
            if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
                detach_all_scene_audio(state);
                waywal_audio_fft_destroy(state->audio_fft);
                state->audio_fft = waywal_audio_fft_create(48000, false);
                if (state->audio_fft) {
                    waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
                    waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
                }
            }
        }
        if (state->workshop_active && state->we_scene && state->we_scene->uses_audio) {
            we_scene_set_audio_output(state->we_scene, state->audio_fft);
        } else if (state->we_scene) {
            we_scene_set_audio_output(state->we_scene, NULL);
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
                detach_all_scene_audio(state);
                waywal_audio_fft_destroy(state->audio_fft);
            }
            state->audio_fft = waywal_audio_fft_create(48000, true);
        }
        if (state->audio_fft) {
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    } else if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
        detach_all_scene_audio(state);
        waywal_audio_fft_destroy(state->audio_fft);
        state->audio_fft = waywal_audio_fft_create(48000, false);
        if (state->audio_fft) {
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    }

    bool bound_scene_audio = false;
    we_scene_t *bound_scene = NULL;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->we_scene) {
            if (!bound_scene_audio && out->workshop_active && out->we_scene->uses_audio) {
                we_scene_set_audio_output(out->we_scene, state->audio_fft);
                bound_scene_audio = true;
                bound_scene = out->we_scene;
            } else if (out->we_scene != bound_scene) {
                we_scene_set_audio_output(out->we_scene, NULL);
            }
        }
    }
    if (!bound_scene_audio && state->workshop_active && state->we_scene &&
        state->we_scene->uses_audio) {
        we_scene_set_audio_output(state->we_scene, state->audio_fft);
    } else if (state->we_scene && state->we_scene != bound_scene) {
        we_scene_set_audio_output(state->we_scene, NULL);
    }

    if (was_shader_active != any_shader) {
        layer_shell_update_input_regions(state, any_shader);
    }

    bool was_anim_active = state->anim_active;
    state->anim_active = any_anim;
    if (!any_anim) {
        disarm_timer_fd(state->anim_timer_fd);
        if (was_anim_active && !state->transitions_in_progress) {
            render_engine_release_upload_cache(&state->render_engine);
        }
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

void stop_engines_on_output(daemon_state_t *state, output_node_t *out, bool keep_shader,
                            bool keep_anim, bool keep_video, bool keep_slideshow)
{
    if (!state || !out)
        return;

    if (!keep_shader) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        if (out->shader_eng) {
            bool shared_eng = false;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->shader_active && o->shader_eng == out->shader_eng) {
                    shared_eng = true;
                    break;
                }
            }
            if (!shared_eng) {
                waywal_shader_engine_release_scratch(out->shader_eng);
            }
        }
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

    if (!keep_slideshow &&
        (!state->g_presenting_slideshow || out->slideshow_eng != state->g_presenting_slideshow)) {
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
        if (!state->in_solar_tick) {
            state->solar_owns_wallpaper = false;
            if (state->active_dynwall_path[0] != '\0' &&
                output_matches_target_spec(
                    out, state->active_dynwall_output[0] ? state->active_dynwall_output : "*")) {
                state->active_dynwall_path[0] = '\0';
                state->last_dynwall_frame_path[0] = '\0';
            }
        }
    }
}

void shader_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        if (out->shader_eng && out->shader_eng != state->shader_eng) {
            waywal_shader_engine_release_scratch(out->shader_eng);
        }
        destroy_output_we_scene(state, out);
    }
    if (state->shader_eng) {
        waywal_shader_engine_release_scratch(state->shader_eng);
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

void anim_engine_stop_playback(daemon_state_t *state)
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
    char resolved[4096];
    const char *target =
        (filepath && waywal_shader_resolve_path(filepath, resolved, sizeof(resolved))) ? resolved
                                                                                       : filepath;
    uint8_t *buf = NULL;
    size_t sz = 0;
    if (!read_file_bytes(target, &buf, &sz))
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

static void swizzle_rgba_to_bgra(uint8_t *restrict bgra, const uint8_t *restrict rgba, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        bgra[i * 4 + 0] = b;
        bgra[i * 4 + 1] = g;
        bgra[i * 4 + 2] = r;
        bgra[i * 4 + 3] = a;
    }
}

static void stop_anim_on_matched_outputs(daemon_state_t *state, const waywal_img_metadata_t *meta,
                                         const char *target_out)
{
    if (!state || !state->anim_active)
        return;
    if (!state->outputs) {
        anim_engine_stop_playback(state);
        return;
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        bool matches = (meta && meta->num_target_outputs > 0)
                           ? output_matches_img_meta(out, meta)
                           : output_matches_target_spec(out, target_out);
        if (matches && out->anim_active) {
            stop_engines_on_output(state, out, true, false, true, true);
        }
    }
    reconcile_global_engine_flags(state);
}

bool try_start_native_animation(daemon_state_t *state, const char *filepath,
                                const waywal_img_metadata_t *meta, const char *target_out)
{
    if (!state || !filepath || !has_anim_candidate_extension(filepath)) {
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

    uint8_t *buf = NULL;
    size_t sz = 0;
    if (!read_file_bytes(filepath, &buf, &sz)) {
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

    if (!waywal_anim_probe(buf, sz)) {
        free(buf);
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

    if (!state->outputs) {
        waywal_anim_decoder_t *dec = waywal_anim_create(buf, sz);
        free(buf);
        if (!dec || waywal_anim_frame_count(dec) <= 1) {
            if (dec)
                waywal_anim_destroy(dec);
            stop_anim_on_matched_outputs(state, meta, target_out);
            return false;
        }
        const uint8_t *f_rgba = NULL;
        uint32_t fw = 0, fh = 0, delay_ms = 100;
        if (!waywal_anim_next_frame(dec, &f_rgba, &fw, &fh, &delay_ms)) {
            waywal_anim_destroy(dec);
            stop_anim_on_matched_outputs(state, meta, target_out);
            return false;
        }
        waywal_anim_reset(dec);
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
        out->displaying_color = false;
        if (filepath && filepath[0] != '\0') {
            snprintf(out->active_media_path, sizeof(out->active_media_path), "%s", filepath);
        } else {
            snprintf(out->active_media_path, sizeof(out->active_media_path), "<stdin>");
        }
        if (meta) {
            out->anim_meta = *meta;
            state->anim_meta = *meta;
        }

        if (f_rgba && fw > 0 && fh > 0 &&
            (!meta || meta->transition_type == WAYWAL_TRANSITION_NONE ||
             meta->transition_duration_ms == 0)) {
            out->transition_active = false;
            out->scaling_mode = meta ? meta->scaling_mode : WAYWAL_SCALING_FILL;
            out->filter_mode = meta ? meta->filter_mode : 0u;
            if (meta && (meta->color[0] || meta->color[1] || meta->color[2] || meta->color[3])) {
                out->fill_color = (color_rgba_t){
                    .r = meta->color[0],
                    .g = meta->color[1],
                    .b = meta->color[2],
                    .a = meta->color[3] ? meta->color[3] : 255,
                };
                out->has_fill_color = true;
            }
            if (meta) {
                out->ten_bit = meta->ten_bit;
                out->crop_gravity_x = meta->has_crop_gravity ? meta->crop_gravity_x : 0.5f;
                out->crop_gravity_y = meta->has_crop_gravity ? meta->crop_gravity_y : 0.5f;
                out->has_crop_gravity = (meta->has_crop_gravity != 0);
            }
            size_t px_bytes = (size_t)fw * (size_t)fh * 4U;
            uint8_t *bgra = (uint8_t *)arena_alloc(&state->frame_arena, px_bytes, 32U);
            bool heap_bgra = false;
            if (!bgra) {
                bgra = (uint8_t *)malloc(px_bytes);
                heap_bgra = (bgra != NULL);
            }
            if (bgra) {
                swizzle_rgba_to_bgra(bgra, f_rgba, (size_t)fw * (size_t)fh);
                render_engine_bump_upload_gen(&state->render_engine);
                output_node_render_image(state, out, bgra, fw, fh);
                if (heap_bgra)
                    free(bgra);
            }
        } else {
            waywal_anim_reset(dec);
        }

        if (out->anim_delay_ms < min_delay_ms)
            min_delay_ms = out->anim_delay_ms;
        if (!state->anim_dec)
            state->anim_dec = dec;
        started_any = true;
    }
    free(buf);

    if (!started_any) {
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

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

        swizzle_rgba_to_bgra(bgra, rgba, (size_t)w * (size_t)h);

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

waywal_theme_mode_t daemon_resolve_effective_theme_mode(daemon_state_t *state,
                                                        const waywal_img_metadata_t *meta)
{
    if (!state)
        return WAYWAL_THEME_DARK;
    bool meta_has_adn = (meta && meta->has_auto_day_night != 0);
    bool meta_adn = (meta && meta->auto_day_night != 0);
    if (meta && meta->has_theme_mode && !meta_adn) {
        return meta->theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    }
    bool auto_dn = meta_has_adn
                       ? meta_adn
                       : (meta_adn || state->config.solar.auto_day_night ||
                          state->config.theme.auto_day_night ||
                          state->config.daemon.auto_day_night || state->config.auto_day_night);
    if (auto_dn) {
        bool is_day = waywal_solar_is_daytime_at_ex(
            state->config.solar.latitude, state->config.solar.longitude,
            state->config.solar.has_coordinates, state->config.solar.sunrise_fallback_min,
            state->config.solar.sunset_fallback_min, time(NULL), NULL, NULL);
        return is_day ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    }
    if (meta && meta->has_theme_mode) {
        return meta->theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    }
    return state->config.theme.theme_mode;
}

void daemon_apply_mpris_album_art_and_theme(daemon_state_t *state,
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
    waywal_theme_mode_t tmode = daemon_resolve_effective_theme_mode(
        state, state->has_active_theme_meta ? &state->active_theme_meta : NULL);
    bool updated_theme = false;

    if (state->theme_from_media && art_bgra && aw > 0 && ah > 0 &&
        mstate->status != WAYWAL_MPRIS_STOPPED) {
        if (art_changed || force_art_upload || !state->media_palette_active) {
            if (waywal_palette_extract_seed_ex(
                    art_bgra, aw, ah, aw * 4U, true, tmode, variant, state->config.theme.contrast,
                    state->config.theme.source_color_index, false, 0U, &state->active_palette)) {
                if (state->has_saved_wallpaper_palette &&
                    state->saved_wallpaper_palette.wallpaper_path[0] != '\0') {
                    snprintf(state->active_palette.wallpaper_path,
                             sizeof(state->active_palette.wallpaper_path), "%s",
                             state->saved_wallpaper_palette.wallpaper_path);
                }
                state->has_active_palette = true;
                state->media_palette_active = true;
                updated_theme = true;
            }
        }
    } else if (state->media_palette_active) {
        state->media_palette_active = false;
        bool is_nc = (state->has_active_theme_meta && state->active_theme_meta.no_cache);
        if (state->has_saved_wallpaper_palette && !is_nc) {
            state->active_palette = state->saved_wallpaper_palette;
            state->has_active_palette = true;
            updated_theme = true;
        } else if (state->wallpaper_rgba && state->wallpaper_w > 0 && state->wallpaper_h > 0) {
            uint8_t src_idx =
                (state->has_active_theme_meta && state->active_theme_meta.has_source_color_index)
                    ? state->active_theme_meta.source_color_index
                    : state->config.theme.source_color_index;
            bool has_seed =
                (state->has_active_theme_meta && state->active_theme_meta.has_seed_color)
                    ? true
                    : state->config.theme.has_seed_color;
            uint32_t seed_rgb =
                (state->has_active_theme_meta && state->active_theme_meta.has_seed_color)
                    ? state->active_theme_meta.seed_color
                    : state->config.theme.seed_color;
            if (waywal_palette_extract_seed_ex(
                    state->wallpaper_rgba, state->wallpaper_w, state->wallpaper_h,
                    state->wallpaper_w * 4U, true, tmode, variant, state->config.theme.contrast,
                    src_idx, has_seed, seed_rgb, &state->active_palette)) {
                if (is_nc) {
                    if (state->has_saved_wallpaper_palette) {
                        memcpy(state->active_palette.wallpaper_path,
                               state->saved_wallpaper_palette.wallpaper_path,
                               sizeof(state->active_palette.wallpaper_path));
                    } else {
                        state->active_palette.wallpaper_path[0] = '\0';
                    }
                } else {
                    state->saved_wallpaper_palette = state->active_palette;
                    state->has_saved_wallpaper_palette = true;
                }
                state->has_active_palette = true;
                updated_theme = true;
            }
        }
    }

    if (updated_theme) {
        bool should_apply_theme =
            state->config.theme.enabled ||
            (state->has_active_theme_meta &&
             (state->active_theme_meta.extract_theme || state->active_theme_meta.has_seed_color));
        if (should_apply_theme &&
            !(state->has_active_theme_meta && state->active_theme_meta.no_theme &&
              !state->media_palette_active)) {
            bool no_cache_active =
                (state->has_active_theme_meta && state->active_theme_meta.no_cache &&
                 !state->media_palette_active);
            if (!no_cache_active) {
                (void)waywal_palette_export_filtered(
                    &state->active_palette, NULL, state->config.theme.caelestia_sync,
                    state->config.theme.export_css, state->config.theme.export_hyprland);
                (void)waywal_palette_apply_configured_templates(&state->active_palette, NULL, NULL,
                                                                &state->config);
            } else if (state->config.template_count > 0) {
                waywal_config_t cfg_nc = state->config;
                cfg_nc.theme.pywal_compat = false;
                (void)waywal_palette_apply_configured_templates(&state->active_palette, NULL, NULL,
                                                                &cfg_nc);
            }
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

void daemon_update_palette_from_bgra(daemon_state_t *state, const uint8_t *bgra_pixels, uint32_t w,
                                     uint32_t h, const waywal_img_metadata_t *meta,
                                     bool export_and_hooks)
{
    if (!state || !bgra_pixels || w == 0 || h == 0)
        return;

    uint32_t thumb_w = w;
    uint32_t thumb_h = h;
    if (thumb_w > 1920U || thumb_h > 1080U) {
        double scale_x = 1920.0 / (double)w;
        double scale_y = 1080.0 / (double)h;
        double scale = (scale_x < scale_y) ? scale_x : scale_y;
        thumb_w = (uint32_t)((double)w * scale + 0.5);
        thumb_h = (uint32_t)((double)h * scale + 0.5);
        if (thumb_w == 0U)
            thumb_w = 1U;
        if (thumb_h == 0U)
            thumb_h = 1U;
    }

    size_t byte_len = (size_t)thumb_w * (size_t)thumb_h * 4U;
    if (!state->wallpaper_rgba || state->wallpaper_w != thumb_w || state->wallpaper_h != thumb_h) {
        uint8_t *buf = (uint8_t *)malloc(byte_len);
        if (buf) {
            free(state->wallpaper_rgba);
            state->wallpaper_rgba = buf;
            state->wallpaper_w = thumb_w;
            state->wallpaper_h = thumb_h;
        }
    }
    if (state->wallpaper_rgba && state->wallpaper_w == thumb_w && state->wallpaper_h == thumb_h) {
        if (thumb_w == w && thumb_h == h) {
            memcpy(state->wallpaper_rgba, bgra_pixels, byte_len);
        } else {
            const uint32_t *src32 = (const uint32_t *)bgra_pixels;
            uint32_t *dst32 = (uint32_t *)state->wallpaper_rgba;
            for (uint32_t dy = 0; dy < thumb_h; ++dy) {
                uint32_t sy0 = (uint32_t)(((uint64_t)dy * h) / thumb_h);
                uint32_t sy1 = sy0 + 1U < h ? sy0 + 1U : sy0;
                const uint32_t *srow0 = src32 + (size_t)sy0 * w;
                const uint32_t *srow1 = src32 + (size_t)sy1 * w;
                uint32_t *drow = dst32 + (size_t)dy * thumb_w;
                for (uint32_t dx = 0; dx < thumb_w; ++dx) {
                    uint32_t sx0 = (uint32_t)(((uint64_t)dx * w) / thumb_w);
                    uint32_t sx1 = sx0 + 1U < w ? sx0 + 1U : sx0;
                    uint32_t p0 =
                        (((srow0[sx0] & 0xFEFEFEFEU) >> 1) + ((srow0[sx1] & 0xFEFEFEFEU) >> 1));
                    uint32_t p1 =
                        (((srow1[sx0] & 0xFEFEFEFEU) >> 1) + ((srow1[sx1] & 0xFEFEFEFEU) >> 1));
                    drow[dx] = ((p0 & 0xFEFEFEFEU) >> 1) + ((p1 & 0xFEFEFEFEU) >> 1);
                }
            }
        }
    }
    const uint8_t *sh_px = state->wallpaper_rgba ? state->wallpaper_rgba : bgra_pixels;
    uint32_t sh_w = state->wallpaper_rgba ? state->wallpaper_w : w;
    uint32_t sh_h = state->wallpaper_rgba ? state->wallpaper_h : h;
    bool uploaded_gl = false;
    if (state->shader_eng) {
        waywal_shader_engine_set_wallpaper_bgra(state->shader_eng, sh_px, sh_w, sh_h);
        uploaded_gl = true;
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && out->shader_eng != state->shader_eng) {
            waywal_shader_engine_set_wallpaper_bgra(out->shader_eng, sh_px, sh_w, sh_h);
            uploaded_gl = true;
        }
    }
    if (uploaded_gl) {
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
    }

    if (meta) {
        state->active_theme_meta = *meta;
        state->active_theme_meta.client_exported_theme = 0;
        state->has_active_theme_meta = true;
    } else {
        memset(&state->active_theme_meta, 0, sizeof(state->active_theme_meta));
        state->has_active_theme_meta = false;
    }

    if (meta && meta->no_theme) {
        state->has_saved_wallpaper_palette = false;
        return;
    }

    waywal_scheme_variant_t variant =
        waywal_scheme_variant_from_str(state->config.theme.scheme_variant);
    waywal_theme_mode_t tmode = daemon_resolve_effective_theme_mode(state, meta);
    uint8_t src_idx = (meta && meta->has_source_color_index)
                          ? meta->source_color_index
                          : state->config.theme.source_color_index;
    bool has_seed = (meta && meta->has_seed_color) ? true : state->config.theme.has_seed_color;
    uint32_t seed_rgb =
        (meta && meta->has_seed_color) ? meta->seed_color : state->config.theme.seed_color;
    uint8_t solid_opaque_px[4] = {
        bgra_pixels[0],
        bgra_pixels[1],
        bgra_pixels[2],
        255U,
    };
    const uint8_t *extract_px =
        (meta && meta->is_solid_color && bgra_pixels[3] == 0U) ? solid_opaque_px : bgra_pixels;
    uint32_t extract_w = (meta && meta->is_solid_color && bgra_pixels[3] == 0U) ? 1U : w;
    uint32_t extract_h = (meta && meta->is_solid_color && bgra_pixels[3] == 0U) ? 1U : h;
    waywal_palette_t extracted_pal;
    if (waywal_palette_extract_seed_ex(extract_px, extract_w, extract_h, extract_w * 4U, true,
                                       tmode, variant, state->config.theme.contrast, src_idx,
                                       has_seed, seed_rgb, &extracted_pal)) {
        bool is_no_cache = (meta && meta->no_cache);
        if (is_no_cache) {
            if (state->has_saved_wallpaper_palette) {
                memcpy(extracted_pal.wallpaper_path, state->saved_wallpaper_palette.wallpaper_path,
                       sizeof(extracted_pal.wallpaper_path));
            } else {
                extracted_pal.wallpaper_path[0] = '\0';
            }
        } else if (meta && meta->filepath[0] != '\0') {
            snprintf(extracted_pal.wallpaper_path, sizeof(extracted_pal.wallpaper_path), "%s",
                     meta->filepath);
        }
        if (!is_no_cache) {
            state->saved_wallpaper_palette = extracted_pal;
            state->has_saved_wallpaper_palette = true;
        }

        if (state->theme_from_media && state->media_palette_active)
            return;

        state->active_palette = extracted_pal;
        state->has_active_palette = true;

        bool client_already_exported = (meta && meta->client_exported_theme != 0);
        bool should_export = export_and_hooks && !client_already_exported &&
                             ((meta && (meta->extract_theme || meta->has_seed_color)) ||
                              state->config.theme.enabled);
        if (should_export) {
            if (!is_no_cache) {
                (void)waywal_palette_export_filtered(
                    &state->active_palette, NULL, state->config.theme.caelestia_sync,
                    state->config.theme.export_css, state->config.theme.export_hyprland);
                (void)waywal_palette_apply_configured_templates(&state->active_palette, NULL, NULL,
                                                                &state->config);
            } else if (state->config.template_count > 0) {
                waywal_config_t cfg_nc = state->config;
                cfg_nc.theme.pywal_compat = false;
                (void)waywal_palette_apply_configured_templates(&state->active_palette, NULL, NULL,
                                                                &cfg_nc);
            }
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

bool daemon_activate_depth_parallax(daemon_state_t *state, const uint8_t *bgra_pixels, uint32_t w,
                                    uint32_t h, const waywal_img_metadata_t *meta,
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
                    int ino_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
                    if (ino_fd >= 0) {
                        unregister_fd_from_loops(state, ino_fd);
                    }
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
            out->shader_path[0] = '\0';
            out->displaying_color = false;
            if (meta->filepath[0] != '\0') {
                snprintf(out->active_media_path, sizeof(out->active_media_path), "%s",
                         meta->filepath);
            } else {
                snprintf(out->active_media_path, sizeof(out->active_media_path), "<stdin>");
            }
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

void daemon_shader_render_once(daemon_state_t *state)
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

    output_node_t *donor_out = NULL;
    dmabuf_bo_t *donor_bo = NULL;
    bool any_committed = false;

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
        uint32_t out_target_fps = out->shader_fps > 0 ? out->shader_fps : 60U;
        if (out->shader_frame_idx > 0 && !out->transition_active && state->power_gov &&
            (waywal_power_governor_should_pause_output(state->power_gov, out->name) ||
             waywal_power_governor_effective_fps(state->power_gov, out->name, out_target_fps) ==
                 0U)) {
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
        bool needs_cursor =
            (out->workshop_active && wscene != NULL) ||
            (!out->workshop_active && seng != NULL && waywal_shader_engine_uses_mouse(seng));
        if (needs_cursor && daemon_query_cursor_pos(state, out, &mx, &my)) {
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

        if (out->transition_active) {
            if (out->has_source_new_bo) {
                if (out->workshop_active && wscene) {
                    (void)we_scene_render_frame(wscene, &out->source_new_bo, &fparams);
                } else if (seng && out->shader_path[0] != '\0') {
                    (void)waywal_shader_engine_render_frame(seng, &out->source_new_bo, &fparams);
                }
            }
            continue;
        }

        if (layer_shell_ensure_dmabuf_ring(state, out, out->width, out->height) &&
            out->use_dmabuf) {
            dmabuf_bo_t *bo = dmabuf_ring_acquire(&out->dmabuf_ring);
            if (bo && bo->wl_buffer) {
                bool rendered = false;
                bool can_clone =
                    (!out->workshop_active && seng != NULL && donor_out != NULL &&
                     donor_bo != NULL &&
                     (!out->shader_eng || out->shader_eng == donor_out->shader_eng ||
                      (out->shader_path[0] != '\0' &&
                       strcmp(out->shader_path, donor_out->shader_path) == 0)) &&
                     out->shader_audio_reactive == donor_out->shader_audio_reactive &&
                     !out->transition_active && !donor_out->transition_active &&
                     out->width == donor_out->width && out->height == donor_out->height &&
                     out->scaling_mode != WAYWAL_SCALING_SPAN &&
                     donor_out->scaling_mode != WAYWAL_SCALING_SPAN &&
                     !waywal_shader_engine_uses_mouse(seng));
                if (can_clone) {
                    rendered = waywal_shader_engine_blit_bo(seng, donor_bo, bo);
                }
                if (!rendered) {
                    if (out->workshop_active && wscene) {
                        rendered = we_scene_render_frame(wscene, bo, &fparams);
                    } else if (seng) {
                        rendered = waywal_shader_engine_render_frame(seng, bo, &fparams);
                        if (rendered && donor_out == NULL && !out->transition_active &&
                            out->scaling_mode != WAYWAL_SCALING_SPAN &&
                            !waywal_shader_engine_uses_mouse(seng)) {
                            donor_out = out;
                            donor_bo = bo;
                        }
                    }
                }
                if (rendered) {
                    if (out->viewport) {
                        int32_t dst_w =
                            out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
                        int32_t dst_h =
                            out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
                        if (!out->vp_configured || out->vp_dst_w != dst_w ||
                            out->vp_dst_h != dst_h) {
                            wp_viewport_set_destination(out->viewport, dst_w, dst_h);
                            wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1),
                                                   wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                                                   wl_fixed_from_int(-1));
                            out->vp_configured = true;
                            out->vp_dst_w = dst_w;
                            out->vp_dst_h = dst_h;
                        }
                    }
                    wl_surface_attach(out->surface, bo->wl_buffer, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, out->width, out->height);
                    presentation_sync_request(&out->pres_sync);
                    wl_surface_commit(out->surface);
                    out->last_presented_bo = bo;
                    out->last_committed_ring_idx = (int)(bo - out->dmabuf_ring.buffers);
                    out->has_image = true;
                    any_committed = true;
                } else {
                    bo->in_use = false;
                }
            }
        }
    }

    if (any_committed && state->display) {
        wl_display_flush(state->display);
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
    if (!state)
        return;

    bool pause_all =
        state->power_gov ? waywal_power_governor_should_pause_all(state->power_gov) : false;

    if (state->power_gov) {
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
            uint32_t eff_s_fps = 0U;
            if (!pause_all) {
                bool any_s_matched = false;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if ((out->shader_active && (out->shader_eng || state->shader_eng)) ||
                        (out->workshop_active && (out->we_scene || state->we_scene))) {
                        any_s_matched = true;
                        uint32_t out_target =
                            out->shader_fps > 0 ? out->shader_fps
                                                : (state->shader_fps > 0 ? state->shader_fps : 60U);
                        uint32_t out_eff = waywal_power_governor_effective_fps(
                            state->power_gov, out->name[0] ? out->name : "*", out_target);
                        if (out_eff > eff_s_fps)
                            eff_s_fps = out_eff;
                    }
                }
                if (!any_s_matched) {
                    const char *s_out =
                        state->shader_target_output[0] ? state->shader_target_output : "*";
                    uint32_t target_s_fps = state->shader_fps > 0 ? state->shader_fps : 60U;
                    eff_s_fps =
                        waywal_power_governor_effective_fps(state->power_gov, s_out, target_s_fps);
                }
            }
            if (eff_s_fps == 0) {
                disarm_timer_fd(state->shader_timer_fd);
                if (state->audio_fft) {
                    waywal_audio_clear_playback(state->audio_fft);
                }
            } else {
                arm_periodic_timer_fd(state->shader_timer_fd, eff_s_fps);
            }
        }

        if (state->anim_active && state->anim_timer_fd >= 0) {
            uint32_t eff_a_fps = 0U;
            if (!pause_all) {
                bool any_a_matched = false;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (out->anim_active && out->anim_dec) {
                        any_a_matched = true;
                        uint32_t out_eff = waywal_power_governor_effective_fps(
                            state->power_gov, out->name[0] ? out->name : "*", 10U);
                        if (out_eff > eff_a_fps)
                            eff_a_fps = out_eff;
                    }
                }
                if (!any_a_matched) {
                    const char *a_out =
                        state->anim_target_output[0] ? state->anim_target_output : "*";
                    eff_a_fps = waywal_power_governor_effective_fps(state->power_gov, a_out, 10U);
                }
            }
            if (eff_a_fps == 0) {
                disarm_timer_fd(state->anim_timer_fd);
            } else {
                struct itimerspec cur_its;
                if (timerfd_gettime(state->anim_timer_fd, &cur_its) < 0 ||
                    (cur_its.it_value.tv_sec == 0 && cur_its.it_value.tv_nsec == 0)) {
                    arm_oneshot_timer_ms(state->anim_timer_fd, 16U);
                }
            }
        }
    }

    if (state->solar_timer_fd >= 0) {
        bool solar_active =
            (state->active_dynwall_path[0] != '\0') ||
            (state->config.solar.manifest_path[0] != '\0') ||
            (state->config.solar.dawn_path[0] != '\0') ||
            (state->config.solar.day_path[0] != '\0') ||
            (state->config.solar.dusk_path[0] != '\0') ||
            (state->config.solar.night_path[0] != '\0') || (state->config.schedule_count > 0) ||
            state->config.solar.auto_day_night || state->config.theme.auto_day_night ||
            state->config.daemon.auto_day_night || state->config.auto_day_night ||
            (state->has_active_theme_meta && state->active_theme_meta.auto_day_night != 0);
        if (pause_all || !solar_active) {
            disarm_timer_fd(state->solar_timer_fd);
        } else {
            struct itimerspec cur_its;
            if (timerfd_gettime(state->solar_timer_fd, &cur_its) < 0 ||
                (cur_its.it_value.tv_sec == 0 && cur_its.it_value.tv_nsec == 0)) {
                arm_oneshot_timer_ms(state->solar_timer_fd, 1U);
            }
        }
    }
}

void upsert_workshop_prop_kv(char *props_buf, size_t cap, const char *key, const char *kv_pair)
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

bool daemon_activate_shader(daemon_state_t *state, const waywal_shader_payload_t *spay)
{
    if (!state || !spay || spay->shader_path[0] == '\0')
        return false;

    char resolved_shader_path[4096];
    const char *shader_file = waywal_shader_resolve_path(spay->shader_path, resolved_shader_path,
                                                         sizeof(resolved_shader_path))
                                  ? resolved_shader_path
                                  : spay->shader_path;

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
                      ? waywal_shader_engine_load_file(state->shader_eng, shader_file)
                      : validate_shader_file_headless(shader_file);
        if (!ok) {
            shader_engine_stop_playback(state);
            return false;
        }
        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            video_engine_stop(&state->video_engine);
        }
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
        anim_engine_stop_playback(state);
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
        strncpy(state->shader_path, shader_file, sizeof(state->shader_path) - 1);
        if (state->mpris) {
            daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris),
                                                   true);
        }
        gpu_compute_invalidate_state_cache((gpu_compute_ctx_t *)state->render_engine.gpu_ctx);
        daemon_reconcile_power_state(state);
        (void)waywal_ecosystem_cache_sync(
            spay->output[0] ? spay->output : "*", shader_file, "Lanczos3", NULL, 0, 0,
            state->config.general.caelestia_sync, spay->no_cache != 0);
        return true;
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

    bool first_compile_attempted = false;
    bool headless_validated = false;
    bool headless_valid = false;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!output_matches_target_spec(out, spay->output))
            continue;

        if (has_transition) {
            layer_shell_snapshot_live_output(state, out);
        }

        if (!out->shader_eng && state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            if (state->shader_eng &&
                !is_shader_eng_attached_to_any_output(state, state->shader_eng)) {
                out->shader_eng = state->shader_eng;
            } else {
                out->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
                if (!state->shader_eng) {
                    state->shader_eng = out->shader_eng;
                } else if (out->shader_eng) {
                    waywal_shader_engine_copy_custom_uniforms(out->shader_eng, state->shader_eng);
                }
            }
        }

        bool loaded = false;
        if (out->shader_eng) {
            if (state->wallpaper_rgba && state->wallpaper_w > 0 && state->wallpaper_h > 0) {
                waywal_shader_engine_set_wallpaper_bgra(out->shader_eng, state->wallpaper_rgba,
                                                        state->wallpaper_w, state->wallpaper_h);
            }
            if (state->mpris) {
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
            first_compile_attempted = true;
            if (waywal_shader_engine_load_file(out->shader_eng, shader_file)) {
                loaded = true;
                int inotify_fd = waywal_shader_engine_get_inotify_fd(out->shader_eng);
                if (inotify_fd >= 0) {
                    register_fd_in_loops(state, inotify_fd, URING_EV_SHADER_INOTIFY);
                }
            }
        } else {
            if (!headless_validated) {
                headless_valid = validate_shader_file_headless(shader_file);
                headless_validated = true;
            }
            loaded = headless_valid;
        }

        if (!loaded) {
            out->shader_active = false;
            if (!any_ok && first_compile_attempted)
                break;
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
        out->displaying_color = false;
        snprintf(out->active_media_path, sizeof(out->active_media_path), "%s", shader_file);
        memset(out->shader_path, 0, sizeof(out->shader_path));
        strncpy(out->shader_path, shader_file, sizeof(out->shader_path) - 1);

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
                if (cx < 0.0f || cy < 0.0f) {
                    double qx = 0, qy = 0;
                    double lref_w =
                        out->logical_width > 0 ? (double)out->logical_width : (double)out->width;
                    double lref_h =
                        out->logical_height > 0 ? (double)out->logical_height : (double)out->height;
                    if (daemon_query_cursor_pos(state, out, &qx, &qy) && lref_w > 0.0 &&
                        lref_h > 0.0) {
                        cx = (float)(qx / lref_w);
                        cy = (float)(qy / lref_h);
                    } else {
                        cx = 0.5f;
                        cy = 0.5f;
                    }
                } else {
                    float ref_w = out->width > 0 ? (float)out->width : 1920.0f;
                    float ref_h = out->height > 0 ? (float)out->height : 1080.0f;
                    if (cx > 1.0f) {
                        cx = cx / ref_w;
                        if (cx > 1.0f)
                            cx = 1.0f;
                    }
                    if (cy > 1.0f) {
                        cy = cy / ref_h;
                        if (cy > 1.0f)
                            cy = 1.0f;
                    }
                    if (spay->invert_y && !spay->pos_y_inverted)
                        cy = 1.0f - cy;
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
                out->transition_start_ns = 0;
                out->transition_last_tick_ns = 0;
                out->transition_duration_ns = (uint64_t)spay->transition_duration_ms * 1000000ULL;
                out->transition_progress_linear = 0.0f;
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
    strncpy(state->shader_path, shader_file, sizeof(state->shader_path) - 1);

    reconcile_global_engine_flags(state);
    if (state->mpris) {
        daemon_apply_mpris_album_art_and_theme(state, waywal_mpris_get_state(state->mpris), true);
    }
    if (!spay->no_cache) {
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, spay->output) && out->name[0] != '\0') {
                int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                 mlen > 0 ? "," : "", out->name);
                if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                    mlen += (size_t)n;
                }
            }
        }
        const char *fallback_target = spay->output[0] ? spay->output : "*";
        (void)waywal_ecosystem_cache_sync(
            matched_outputs[0] ? matched_outputs : fallback_target, shader_file, "Lanczos3", NULL,
            0, 0, state->config.general.caelestia_sync, spay->no_cache != 0);
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

static bool daemon_render_image_on_target_ex(daemon_state_t *state, const char *filepath,
                                             const char *target_output,
                                             waywal_scaling_mode_t scaling,
                                             waywal_filter_mode_t filter, bool ten_bit,
                                             uint32_t transition_type, uint32_t duration_ms,
                                             float angle_deg, bool has_angle, bool has_fill_color,
                                             uint32_t fill_color, bool has_crop_gravity,
                                             float crop_gravity_x, float crop_gravity_y)
{
    if (!state || !filepath || filepath[0] == '\0')
        return false;

    char resolved_frame_buf[4096];
    const char *load_path = filepath;
    bool is_dynwall_manifest = waywal_dynwall_is_manifest_path(filepath);
    bool man_has_theme_mode = false;
    bool man_theme_light_mode = false;
    if (is_dynwall_manifest) {
        waywal_dynwall_manifest_t man;
        waywal_dynwall_resolved_t res;
        time_t now_utc = time(NULL);
        struct tm lt;
        localtime_r(&now_utc, &lt);
        waywal_solar_state_t sstate;
        if (state->config.solar.has_coordinates) {
            waywal_solar_compute(state->config.solar.latitude, state->config.solar.longitude,
                                 now_utc, &lt, &sstate);
        } else {
            waywal_solar_synthesize_local(state->config.solar.sunrise_fallback_min,
                                          state->config.solar.sunset_fallback_min, now_utc, &lt,
                                          &sstate);
        }
        if (!waywal_dynwall_load_file(filepath, &man) ||
            !waywal_dynwall_resolve(&man, now_utc, &lt, &sstate, &res) ||
            waywal_dynwall_effective_path(&res)[0] == '\0') {
            return false;
        }
        snprintf(resolved_frame_buf, sizeof(resolved_frame_buf), "%s",
                 waywal_dynwall_effective_path(&res));
        load_path = resolved_frame_buf;
        man_has_theme_mode = res.has_theme_mode;
        man_theme_light_mode = res.theme_light_mode;
    }

    uint8_t *px = NULL;
    uint32_t w = 0, h = 0;
    if (!image_load(load_path, &px, &w, &h))
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
    if (state->in_solar_tick && state->has_active_theme_meta) {
        meta->extract_theme = state->active_theme_meta.extract_theme;
        meta->no_theme = state->active_theme_meta.no_theme;
        meta->no_cache = state->active_theme_meta.no_cache;
        meta->has_theme_mode = state->active_theme_meta.has_theme_mode;
        meta->theme_light_mode = state->active_theme_meta.theme_light_mode;
        meta->has_source_color_index = state->active_theme_meta.has_source_color_index;
        meta->source_color_index = state->active_theme_meta.source_color_index;
        meta->has_seed_color = state->active_theme_meta.has_seed_color;
        meta->seed_color = state->active_theme_meta.seed_color;
        meta->has_auto_day_night = state->active_theme_meta.has_auto_day_night;
        meta->auto_day_night = state->active_theme_meta.auto_day_night;
    } else if (man_has_theme_mode) {
        meta->has_theme_mode = 1;
        meta->theme_light_mode = man_theme_light_mode ? 1 : 0;
    }
    meta->width = w;
    meta->height = h;
    meta->transition_type = transition_type;
    meta->transition_duration_ms = duration_ms;
    meta->transition_fps = state->config.transition.fps > 0 ? state->config.transition.fps : 60;
    float eff_angle = has_angle ? angle_deg : state->config.transition.angle;
    meta->transition_angle_rad = eff_angle * (3.14159265358979323846f / 180.0f);
    meta->transition_wave_freq = state->config.transition.wave_x;
    meta->transition_wave_amp = state->config.transition.wave_y;
    meta->transition_center_x = state->config.transition.pos_x;
    meta->transition_center_y = state->config.transition.pos_y;
    meta->invert_y = state->config.transition.invert_y ? 1 : 0;
    meta->pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
    meta->bezier.p1x = state->config.transition.bezier[0];
    meta->bezier.p1y = state->config.transition.bezier[1];
    meta->bezier.p2x = state->config.transition.bezier[2];
    meta->bezier.p2y = state->config.transition.bezier[3];
    meta->has_custom_bezier = true;
    meta->scaling_mode = (uint32_t)scaling;
    meta->filter_mode = (uint32_t)filter;
    meta->ten_bit = ten_bit;
    meta->num_target_outputs = num_targets;
    meta->extra_data_len = (uint32_t)extra_len;
    strncpy(meta->filepath, filepath, sizeof(meta->filepath) - 1);
    if (num_targets == 1 && extra_len <= 64) {
        memcpy(meta_storage + sizeof(waywal_img_metadata_t), target_output, extra_len);
    }
    if (state->config.transition.has_crop_gravity) {
        meta->crop_gravity_x = state->config.transition.crop_gravity_x;
        meta->crop_gravity_y = state->config.transition.crop_gravity_y;
        meta->has_crop_gravity = 1;
    } else if (state->config.daemon.has_crop_gravity) {
        meta->crop_gravity_x = state->config.daemon.crop_gravity_x;
        meta->crop_gravity_y = state->config.daemon.crop_gravity_y;
        meta->has_crop_gravity = 1;
    }
    if (target_output && target_output[0] != '\0') {
        const waywal_output_rule_t *r = waywal_config_match_output(&state->config, target_output);
        if (r && r->has_crop_gravity) {
            meta->crop_gravity_x = r->crop_gravity_x;
            meta->crop_gravity_y = r->crop_gravity_y;
            meta->has_crop_gravity = 1;
        }
        if (r && r->has_fill_color) {
            meta->color[0] = (uint8_t)((r->fill_color >> 24) & 0xFFU);
            meta->color[1] = (uint8_t)((r->fill_color >> 16) & 0xFFU);
            meta->color[2] = (uint8_t)((r->fill_color >> 8) & 0xFFU);
            meta->color[3] = (uint8_t)(r->fill_color & 0xFFU);
        }
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (output_matches_img_meta(out, meta) && out->has_crop_gravity) {
            meta->crop_gravity_x = out->crop_gravity_x;
            meta->crop_gravity_y = out->crop_gravity_y;
            meta->has_crop_gravity = 1;
            break;
        }
    }
    if (has_crop_gravity) {
        meta->crop_gravity_x = crop_gravity_x;
        meta->crop_gravity_y = crop_gravity_y;
        meta->has_crop_gravity = 1;
    }
    if (has_fill_color) {
        meta->color[0] = (uint8_t)((fill_color >> 24) & 0xFFU);
        meta->color[1] = (uint8_t)((fill_color >> 16) & 0xFFU);
        meta->color[2] = (uint8_t)((fill_color >> 8) & 0xFFU);
        meta->color[3] = (uint8_t)(fill_color & 0xFFU);
    }

    bool has_transition = (transition_type != WAYWAL_TRANSITION_NONE && duration_ms > 0);
    bool will_transition = !has_transition;
    if (has_transition && state->outputs) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->surface && out->configured && out->width > 0 && out->height > 0 &&
                output_matches_img_meta(out, meta)) {
                will_transition = true;
                break;
            }
        }
    }
    if (will_transition) {
        const char *filt_str = waywal_filter_mode_to_swww_str(filter);
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_img_meta(out, meta) && out->name[0] != '\0') {
                int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                 mlen > 0 ? "," : "", out->name);
                if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                    mlen += (size_t)n;
                }
            }
        }
        const char *fallback_target = target_output ? target_output : "*";
        (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : fallback_target,
                                          filepath, filt_str, px, w, h,
                                          state->config.general.caelestia_sync, false);
    }

    if (state->outputs) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
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
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
    }

    daemon_update_palette_from_bgra(state, px, w, h, meta, true);
    bool ok = daemon_start_image_transition(state, px, w, h, meta);
    if (ok) {
        if (is_dynwall_manifest) {
            state->solar_owns_wallpaper = true;
            snprintf(state->active_dynwall_path, sizeof(state->active_dynwall_path), "%s",
                     filepath);
            snprintf(state->active_dynwall_output, sizeof(state->active_dynwall_output), "%s",
                     (target_output && target_output[0] != '\0') ? target_output : "*");
            snprintf(state->last_dynwall_frame_path, sizeof(state->last_dynwall_frame_path), "%s",
                     load_path);
            if (man_has_theme_mode) {
                state->sched_theme_override_active = true;
            }
        }
    }
    image_free(px);
    if (ok) {
        if (state->config.hooks.on_wallpaper_change[0] != '\0') {
            (void)waywal_palette_run_hook(state->config.hooks.on_wallpaper_change);
        }
        (void)try_start_native_animation(state, load_path, meta,
                                         target_output ? target_output : "*");
        if (is_dynwall_manifest && !state->in_solar_tick) {
            daemon_tick_solar_and_schedule(state, true);
        }
    }
    return ok;
}

static bool daemon_render_image_on_target(daemon_state_t *state, const char *filepath,
                                          const char *target_output, waywal_scaling_mode_t scaling,
                                          waywal_filter_mode_t filter, bool ten_bit,
                                          uint32_t transition_type, uint32_t duration_ms)
{
    return daemon_render_image_on_target_ex(state, filepath, target_output, scaling, filter,
                                            ten_bit, transition_type, duration_ms, 0.0f, false,
                                            false, 0, false, 0.5f, 0.5f);
}

bool daemon_start_slideshow_isolated(daemon_state_t *state, const waywal_slideshow_payload_t *spay)
{
    if (!state || !spay)
        return false;

    if (waywal_dynwall_is_manifest_path(spay->path)) {
        const char *target = (spay->num_target_outputs > 0 && spay->target_outputs[0][0] != '\0')
                                 ? spay->target_outputs[0]
                                 : "*";
        return daemon_render_image_on_target(
            state, spay->path, target, (waywal_scaling_mode_t)spay->scaling_mode,
            (waywal_filter_mode_t)spay->filter_mode, spay->ten_bit != 0, spay->transition_type,
            spay->transition_duration_ms);
    }

    if (state->outputs) {
        if (spay->num_target_outputs > 0) {
            bool any_output_matched = false;
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (output_matches_slideshow_payload(out, spay)) {
                    any_output_matched = true;
                    break;
                }
            }
            if (!any_output_matched) {
                return false;
            }
        }
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
                                             const char *target, waywal_scaling_mode_t scaling,
                                             const uint8_t *preview_bgra, uint32_t preview_w,
                                             uint32_t preview_h, bool no_cache)
{
    if (!state || !filepath || filepath[0] == '\0')
        return false;

    bool has_target = (target && target[0] != '\0' && strcmp(target, "*") != 0);
    output_node_t *target_out = NULL;
    if (has_target) {
        uint32_t matched_count = 0;
        output_node_t *last_matched = NULL;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                last_matched = out;
                matched_count++;
            }
        }
        if (state->outputs && matched_count == 0) {
            return false;
        }
        if (matched_count == 1) {
            target_out = last_matched;
        }
    }

    if (has_target && state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED &&
        state->video_engine.filepath[0] != '\0' &&
        strcmp(state->video_engine.filepath, filepath) == 0) {
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                stop_engines_on_output(state, out, false, false,
                                       out->video_eng == &state->video_engine, false);
                out->video_eng = &state->video_engine;
                out->video_targeted = true;
                out->scaling_mode = (uint32_t)scaling;
                out->vp_configured = false;
                out->displaying_color = false;
                snprintf(out->active_media_path, sizeof(out->active_media_path), "%s", filepath);
                if (out->name[0] != '\0') {
                    int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                     mlen > 0 ? "," : "", out->name);
                    if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                        mlen += (size_t)n;
                    }
                }
            }
        }
        if (!no_cache && matched_outputs[0] != '\0') {
            (void)waywal_ecosystem_cache_sync(matched_outputs, filepath, "Lanczos3", preview_bgra,
                                              preview_w, preview_h,
                                              state->config.general.caelestia_sync, no_cache);
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
                if (output_matches_target_spec(out, target)) {
                    stop_engines_on_output(state, out, false, false,
                                           out->video_eng == &state->video_engine, false);
                }
            }
        } else {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                stop_engines_on_output(state, out, false, false,
                                       out->video_eng == &state->video_engine, false);
            }
            if (&state->slideshow != state->g_presenting_slideshow) {
                slideshow_stop(&state->slideshow);
            }
            shader_engine_stop_playback(state);
            anim_engine_stop_playback(state);
        }
        reconcile_global_engine_flags(state);
    } else {
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
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
            bool match = !has_target || output_matches_target_spec(out, target);
            out->video_targeted = match;
            if (!match && out->video_eng == &state->video_engine) {
                out->video_eng = NULL;
            }
        }
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!has_target || output_matches_target_spec(out, target)) {
            out->video_eng = ve;
            out->video_targeted = true;
            out->scaling_mode = (uint32_t)scaling;
            out->vp_configured = false;
        }
    }

    bool ok = video_engine_load_file(ve, filepath, target_out);
    if (!ok) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->video_eng == ve) {
                out->video_eng = NULL;
                out->video_targeted = false;
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
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->video_eng == ve) {
                out->displaying_color = false;
                snprintf(out->active_media_path, sizeof(out->active_media_path), "%s", filepath);
                if (out->name[0] != '\0') {
                    int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                     mlen > 0 ? "," : "", out->name);
                    if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                        mlen += (size_t)n;
                    }
                }
            }
        }
        if (!no_cache) {
            const char *fallback_target = target ? target : "*";
            (void)waywal_ecosystem_cache_sync(
                matched_outputs[0] ? matched_outputs : fallback_target, filepath, "Lanczos3",
                preview_bgra, preview_w, preview_h, state->config.general.caelestia_sync, no_cache);
        }
        daemon_reconcile_power_state(state);
    }
    return ok;
}

bool daemon_activate_workshop(daemon_state_t *state, const waywal_workshop_payload_t *wpay)
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
        char probe_gifpkg[4600];
        char probe_json[4600];
        struct stat st;
        snprintf(probe_pkg, sizeof(probe_pkg), "%s/scene.pkg", ws_dir);
        snprintf(probe_gifpkg, sizeof(probe_gifpkg), "%s/gifscene.pkg", ws_dir);
        snprintf(probe_json, sizeof(probe_json), "%s/scene.json", ws_dir);
        if ((stat(probe_pkg, &st) == 0 && S_ISREG(st.st_mode)) ||
            (stat(probe_gifpkg, &st) == 0 && S_ISREG(st.st_mode)) ||
            (stat(probe_json, &st) == 0 && S_ISREG(st.st_mode))) {
            strncpy(proj.type, "scene", sizeof(proj.type) - 1);
            strncpy(proj.file, "scene.json", sizeof(proj.file) - 1);
        } else {
            return false;
        }
    }

    waywal_img_metadata_t ws_meta;
    memset(&ws_meta, 0, sizeof(ws_meta));
    snprintf(ws_meta.filepath, sizeof(ws_meta.filepath), "%s", ws_dir);
    ws_meta.no_cache = wpay->no_cache ? 1 : 0;
    ws_meta.extract_theme = wpay->extract_theme ? true : false;
    ws_meta.no_theme = wpay->no_theme ? true : false;
    ws_meta.has_theme_mode = wpay->has_theme_mode;
    ws_meta.theme_light_mode = wpay->theme_light_mode ? true : false;
    ws_meta.has_source_color_index = wpay->has_source_color_index;
    ws_meta.source_color_index = wpay->source_color_index;
    ws_meta.has_seed_color = wpay->has_seed_color;
    ws_meta.seed_color = wpay->seed_color;
    ws_meta.client_exported_theme = wpay->client_exported_theme;

    uint8_t *ws_preview_px = NULL;
    uint32_t ws_preview_w = 0, ws_preview_h = 0;
    bool ws_preview_from_image_load = false;
    char prev_path[4600];
    if (we_workshop_find_preview_file(ws_dir, prev_path, sizeof(prev_path))) {
        if (image_load(prev_path, &ws_preview_px, &ws_preview_w, &ws_preview_h) && ws_preview_px &&
            ws_preview_w > 0 && ws_preview_h > 0) {
            ws_preview_from_image_load = true;
            snprintf(ws_meta.filepath, sizeof(ws_meta.filepath), "%s", prev_path);
            daemon_update_palette_from_bgra(state, ws_preview_px, ws_preview_w, ws_preview_h,
                                            &ws_meta, true);
        } else if (ws_preview_px) {
            image_free(ws_preview_px);
            ws_preview_px = NULL;
        }
    }

    if (strcasecmp(proj.type, "video") == 0 && proj.file[0] != '\0') {
        char vpath[4600];
        snprintf(vpath, sizeof(vpath), "%s/%s", ws_dir, proj.file);
        bool vok = daemon_start_video_file_isolated(
            state, vpath, wpay->output[0] ? wpay->output : "*",
            (waywal_scaling_mode_t)wpay->scaling_mode, ws_preview_px, ws_preview_w, ws_preview_h,
            wpay->no_cache != 0);
        if (ws_preview_px) {
            if (ws_preview_from_image_load) {
                image_free(ws_preview_px);
            } else {
                free(ws_preview_px);
            }
        }
        return vok;
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
            if (ws_preview_px) {
                if (ws_preview_from_image_load) {
                    image_free(ws_preview_px);
                } else {
                    free(ws_preview_px);
                }
            }
            shader_engine_stop_playback(state);
            return false;
        }
        if (!ws_preview_px) {
            ws_preview_w = 320;
            ws_preview_h = 180;
            ws_preview_px = (uint8_t *)malloc((size_t)ws_preview_w * ws_preview_h * 4u);
            ws_preview_from_image_load = false;
            waywal_shader_frame_params_t fparams;
            memset(&fparams, 0, sizeof(fparams));
            fparams.time_sec = 0.5f;
            fparams.time_delta_sec = 1.0f / 60.0f;
            if (ws_preview_px && we_scene_render_rgba8(state->we_scene, ws_preview_w, ws_preview_h,
                                                       &fparams, ws_preview_px)) {
                size_t total_px = (size_t)ws_preview_w * ws_preview_h;
                for (size_t i = 0; i < total_px; ++i) {
                    uint8_t *p = &ws_preview_px[i * 4u];
                    uint8_t tmp = p[0];
                    p[0] = p[2];
                    p[2] = tmp;
                    p[3] = 255;
                }
                daemon_update_palette_from_bgra(state, ws_preview_px, ws_preview_w, ws_preview_h,
                                                &ws_meta, true);
            } else {
                free(ws_preview_px);
                ws_preview_px = NULL;
                ws_preview_w = 0;
                ws_preview_h = 0;
            }
        }
        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            video_engine_stop(&state->video_engine);
        }
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
        if (state->shader_eng) {
            int ino_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
            if (ino_fd >= 0) {
                unregister_fd_from_loops(state, ino_fd);
            }
            waywal_shader_engine_release_scratch(state->shader_eng);
        }
        anim_engine_stop_playback(state);
        if (wpay->audio_reactive || state->we_scene->uses_audio) {
            if (!state->audio_fft || !waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
                if (state->audio_fft) {
                    detach_all_scene_audio(state);
                    waywal_audio_fft_destroy(state->audio_fft);
                }
                state->audio_fft = waywal_audio_fft_create(48000, true);
            }
            if (state->audio_fft) {
                waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
            }
        } else if (state->audio_fft && waywal_audio_fft_is_pipewire_active(state->audio_fft)) {
            detach_all_scene_audio(state);
            waywal_audio_fft_destroy(state->audio_fft);
            state->audio_fft = waywal_audio_fft_create(48000, false);
            if (state->audio_fft) {
                waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
            }
        }
        we_scene_set_audio_output(state->we_scene,
                                  state->we_scene->uses_audio ? state->audio_fft : NULL);
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
        (void)waywal_ecosystem_cache_sync(
            wpay->output[0] ? wpay->output : "*", ws_dir, "Lanczos3", ws_preview_px, ws_preview_w,
            ws_preview_h, state->config.general.caelestia_sync, wpay->no_cache != 0);
        if (ws_preview_px) {
            if (ws_preview_from_image_load) {
                image_free(ws_preview_px);
            } else {
                free(ws_preview_px);
            }
        }
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
        out->displaying_color = false;
        snprintf(out->active_media_path, sizeof(out->active_media_path), "%s", ws_dir);
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
                if (cx < 0.0f || cy < 0.0f) {
                    double qx = 0, qy = 0;
                    double lref_w =
                        out->logical_width > 0 ? (double)out->logical_width : (double)out->width;
                    double lref_h =
                        out->logical_height > 0 ? (double)out->logical_height : (double)out->height;
                    if (daemon_query_cursor_pos(state, out, &qx, &qy) && lref_w > 0.0 &&
                        lref_h > 0.0) {
                        cx = (float)(qx / lref_w);
                        cy = (float)(qy / lref_h);
                    } else {
                        cx = 0.5f;
                        cy = 0.5f;
                    }
                } else {
                    float ref_w = out->width > 0 ? (float)out->width : 1920.0f;
                    float ref_h = out->height > 0 ? (float)out->height : 1080.0f;
                    if (cx > 1.0f) {
                        cx = cx / ref_w;
                        if (cx > 1.0f)
                            cx = 1.0f;
                    }
                    if (cy > 1.0f) {
                        cy = cy / ref_h;
                        if (cy > 1.0f)
                            cy = 1.0f;
                    }
                    if (wpay->invert_y && !wpay->pos_y_inverted)
                        cy = 1.0f - cy;
                }
                bezier_curve_t bz = BEZIER_DEFAULT;
                if (wpay->has_custom_bezier || wpay->bezier.x1 != 0.0f || wpay->bezier.y1 != 0.0f ||
                    wpay->bezier.x2 != 0.0f || wpay->bezier.y2 != 0.0f) {
                    bz = wpay->bezier;
                }
                out->transition_params = (waywal_transition_type_t)resolved_type !=
                                                 WAYWAL_TRANSITION_NONE
                                             ? (waywal_transition_params_t){
                                                   .type = (waywal_transition_type_t)resolved_type,
                                                   .progress = 0.0f,
                                                   .angle_rad = wpay->transition_angle_rad,
                                                   .wave_freq = wpay->transition_wave_freq,
                                                   .wave_amp = wpay->transition_wave_amp,
                                                   .center_x = cx,
                                                   .center_y = cy,
                                                   .bezier = bz,
                                               }
                                             : out->transition_params;
                out->transition_current_frame = 0;
                out->transition_total_frames = num_frames;
                out->transition_delay_frames = 0;
                out->transition_start_ns = 0;
                out->transition_last_tick_ns = 0;
                out->transition_duration_ns = (uint64_t)wpay->transition_duration_ms * 1000000ULL;
                out->transition_progress_linear = 0.0f;
                out->transition_active = true;
                any_transition_started = true;
            }
        }
        any_ok = true;
    }

    if (!any_ok) {
        if (ws_preview_px) {
            if (ws_preview_from_image_load) {
                image_free(ws_preview_px);
            } else {
                free(ws_preview_px);
            }
        }
        reconcile_global_engine_flags(state);
        return false;
    }

    we_scene_t *preview_scene = state->we_scene;
    if (!preview_scene) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (out->we_scene) {
                preview_scene = out->we_scene;
                break;
            }
        }
    }
    if (!ws_preview_px && preview_scene) {
        ws_preview_w = 320;
        ws_preview_h = 180;
        ws_preview_px = (uint8_t *)malloc((size_t)ws_preview_w * ws_preview_h * 4u);
        ws_preview_from_image_load = false;
        waywal_shader_frame_params_t fparams;
        memset(&fparams, 0, sizeof(fparams));
        fparams.time_sec = 0.5f;
        fparams.time_delta_sec = 1.0f / 60.0f;
        if (ws_preview_px && we_scene_render_rgba8(preview_scene, ws_preview_w, ws_preview_h,
                                                   &fparams, ws_preview_px)) {
            size_t total_px = (size_t)ws_preview_w * ws_preview_h;
            for (size_t i = 0; i < total_px; ++i) {
                uint8_t *p = &ws_preview_px[i * 4u];
                uint8_t tmp = p[0];
                p[0] = p[2];
                p[2] = tmp;
                p[3] = 255;
            }
            snprintf(ws_meta.filepath, sizeof(ws_meta.filepath), "%s", ws_dir);
            daemon_update_palette_from_bgra(state, ws_preview_px, ws_preview_w, ws_preview_h,
                                            &ws_meta, true);
        } else {
            free(ws_preview_px);
            ws_preview_px = NULL;
            ws_preview_w = 0;
            ws_preview_h = 0;
        }
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
    if (!wpay->no_cache) {
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, wpay->output) && out->name[0] != '\0') {
                int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                 mlen > 0 ? "," : "", out->name);
                if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                    mlen += (size_t)n;
                }
            }
        }
        const char *fallback_target = wpay->output[0] ? wpay->output : "*";
        (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : fallback_target,
                                          ws_dir, "Lanczos3", ws_preview_px, ws_preview_w,
                                          ws_preview_h, state->config.general.caelestia_sync,
                                          wpay->no_cache != 0);
    }
    if (ws_preview_px) {
        if (ws_preview_from_image_load) {
            image_free(ws_preview_px);
        } else {
            free(ws_preview_px);
        }
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

bool daemon_restore_output_entry(daemon_state_t *state, const waywal_output_saved_state_t *saved,
                                 const char *override_output)
{
    if (!state || !saved)
        return false;

    const char *target = (override_output && override_output[0] != '\0')
                             ? override_output
                             : (saved->output_name[0] ? saved->output_name : "*");

    if (saved->has_crop_gravity) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                out->crop_gravity_x = saved->crop_gravity_x;
                out->crop_gravity_y = saved->crop_gravity_y;
                out->has_crop_gravity = true;
            }
        }
    }

    switch (saved->kind) {
    case WAYWAL_CONTENT_SOLID_COLOR: {
        color_rgba_t col = {
            .r = (uint8_t)((saved->solid_color >> 24) & 0xFF),
            .g = (uint8_t)((saved->solid_color >> 16) & 0xFF),
            .b = (uint8_t)((saved->solid_color >> 8) & 0xFF),
            .a = (uint8_t)(saved->solid_color & 0xFF),
        };
        char matched_outputs[256] = {0};
        size_t mlen = 0;
        if (strcmp(target, "*") == 0) {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                stop_engines_on_output(state, out, false, false, false, false);
                if (out->name[0] != '\0') {
                    int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                     mlen > 0 ? "," : "", out->name);
                    if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                        mlen += (size_t)n;
                    }
                }
            }
            reconcile_global_engine_flags(state);
            daemon_clear_all_outputs(state, col);
        } else {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (output_matches_target_spec(out, target)) {
                    stop_engines_on_output(state, out, false, false, false, false);
                    output_node_render_color(state, out, col);
                    if (out->name[0] != '\0') {
                        int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen,
                                         "%s%s", mlen > 0 ? "," : "", out->name);
                        if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                            mlen += (size_t)n;
                        }
                    }
                }
            }
            reconcile_global_engine_flags(state);
        }
        uint8_t solid_bgra[64 * 64 * 4];
        for (size_t px_i = 0; px_i < 64U * 64U; ++px_i) {
            solid_bgra[px_i * 4U + 0U] = col.b;
            solid_bgra[px_i * 4U + 1U] = col.g;
            solid_bgra[px_i * 4U + 2U] = col.r;
            solid_bgra[px_i * 4U + 3U] = col.a;
        }
        (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : target, "",
                                          "Nearest", solid_bgra, 64, 64,
                                          state->config.general.caelestia_sync, false);
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
        spay.invert_y = state->config.transition.invert_y ? 1 : 0;
        spay.pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = true;
        if (saved->has_crop_gravity) {
            spay.crop_gravity_x = saved->crop_gravity_x;
            spay.crop_gravity_y = saved->crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.transition.has_crop_gravity) {
            spay.crop_gravity_x = state->config.transition.crop_gravity_x;
            spay.crop_gravity_y = state->config.transition.crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.daemon.has_crop_gravity) {
            spay.crop_gravity_x = state->config.daemon.crop_gravity_x;
            spay.crop_gravity_y = state->config.daemon.crop_gravity_y;
            spay.has_crop_gravity = 1;
        }
        const waywal_output_rule_t *orule =
            waywal_config_match_output(&state->config, strcmp(target, "*") == 0 ? "any" : target);
        if (orule && orule->has_recursive) {
            spay.no_recursive = orule->recursive ? 0 : 1;
        } else if (state->config.daemon.has_slideshow_recursive) {
            spay.no_recursive = state->config.daemon.slideshow_recursive ? 0 : 1;
        }
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
        return daemon_start_video_file_isolated(state, saved->media_path, target, saved->scaling,
                                                NULL, 0, 0, false);
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
        spay.invert_y = state->config.transition.invert_y ? 1 : 0;
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
        wpay.invert_y = state->config.transition.invert_y ? 1 : 0;
        return daemon_activate_workshop(state, &wpay);
    }
    default:
        return false;
    }
}

static bool daemon_render_color_on_target(daemon_state_t *state, const char *target_spec,
                                          color_rgba_t col)
{
    if (!state)
        return false;
    const char *target = (target_spec && target_spec[0] != '\0') ? target_spec : "*";
    char matched_outputs[256] = {0};
    size_t mlen = 0;
    if (!state->outputs) {
        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            video_engine_stop(&state->video_engine);
        }
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
        daemon_clear_all_outputs(state, col);
    } else if (strcmp(target, "*") == 0) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            stop_engines_on_output(state, out, false, false, false, false);
            if (out->name[0] != '\0') {
                int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                 mlen > 0 ? "," : "", out->name);
                if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                    mlen += (size_t)n;
                }
            }
        }
        reconcile_global_engine_flags(state);
        daemon_clear_all_outputs(state, col);
    } else {
        bool any_matched = false;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                any_matched = true;
                stop_engines_on_output(state, out, false, false, false, false);
                output_node_render_color(state, out, col);
                if (out->name[0] != '\0') {
                    int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen, "%s%s",
                                     mlen > 0 ? "," : "", out->name);
                    if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                        mlen += (size_t)n;
                    }
                }
            }
        }
        reconcile_global_engine_flags(state);
        if (!any_matched) {
            return false;
        }
    }
    uint8_t solid_bgra[64 * 64 * 4];
    for (size_t px_i = 0; px_i < 64U * 64U; ++px_i) {
        solid_bgra[px_i * 4U + 0U] = col.b;
        solid_bgra[px_i * 4U + 1U] = col.g;
        solid_bgra[px_i * 4U + 2U] = col.r;
        solid_bgra[px_i * 4U + 3U] = col.a;
    }
    (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : target, "", "Nearest",
                                      solid_bgra, 64, 64, state->config.general.caelestia_sync,
                                      false);
    if (state->config.hooks.on_wallpaper_change[0] != '\0') {
        (void)waywal_palette_run_hook(state->config.hooks.on_wallpaper_change);
    }
    return true;
}

void daemon_slideshow_prepare_image_output(daemon_state_t *state, slideshow_engine_t *ss)
{
    if (!state || !ss)
        return;
    slideshow_engine_t *prev_pres = state->g_presenting_slideshow;
    state->g_presenting_slideshow = ss;
    uint32_t dur_ms = ss->transition_duration_ms > 0 ? ss->transition_duration_ms : 1000;
    if (state->outputs) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            bool matches = (ss->num_target_outputs == 0);
            for (uint8_t i = 0; i < ss->num_target_outputs; ++i) {
                if (output_matches_target_spec(out, ss->target_outputs[i])) {
                    matches = true;
                    break;
                }
            }
            if (matches) {
                if (ss->transition_type != WAYWAL_TRANSITION_NONE && dur_ms > 0) {
                    layer_shell_snapshot_live_output(state, out);
                }
                stop_engines_on_output(state, out, false, false, false, true);
                out->slideshow_eng = ss;
            }
        }
        reconcile_global_engine_flags(state);
    } else {
        if (state->video_engine.state != WAYWAL_VIDEO_STATE_STOPPED) {
            video_engine_stop(&state->video_engine);
        }
        if (&state->slideshow != state->g_presenting_slideshow) {
            slideshow_stop(&state->slideshow);
        }
        shader_engine_stop_playback(state);
        anim_engine_stop_playback(state);
    }
    state->g_presenting_slideshow = prev_pres;
}

bool daemon_slideshow_present_media(daemon_state_t *state, slideshow_engine_t *ss,
                                    const char *filepath, uint32_t trans_type)
{
    if (!state || !ss || !filepath || filepath[0] == '\0')
        return false;

    char target_spec[256] = {0};
    if (ss->num_target_outputs == 0) {
        strncpy(target_spec, "*", sizeof(target_spec) - 1);
    } else {
        size_t off = 0;
        for (uint8_t i = 0; i < ss->num_target_outputs; ++i) {
            if (ss->target_outputs[i][0] == '\0')
                continue;
            buf_appendf(target_spec, sizeof(target_spec), &off, "%s%s", off > 0 ? "," : "",
                        ss->target_outputs[i]);
        }
        if (target_spec[0] == '\0') {
            strncpy(target_spec, "*", sizeof(target_spec) - 1);
        }
    }

    slideshow_engine_t *prev_pres = state->g_presenting_slideshow;
    state->g_presenting_slideshow = ss;
    bool ok = false;
    uint32_t dur_ms = ss->transition_duration_ms > 0 ? ss->transition_duration_ms : 1000;

    struct stat st;
    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
        waywal_workshop_payload_t wpay;
        memset(&wpay, 0, sizeof(wpay));
        strncpy(wpay.output, target_spec, sizeof(wpay.output) - 1);
        strncpy(wpay.workshop_dir, filepath, sizeof(wpay.workshop_dir) - 1);
        wpay.fps = ss->transition_fps > 0 ? ss->transition_fps : 60;
        wpay.scaling_mode = ss->scaling_mode;
        wpay.invert_y = ss->invert_y;
        wpay.no_cache = ss->no_cache ? 1 : 0;
        wpay.extract_theme = ss->extract_theme ? 1 : 0;
        wpay.no_theme = ss->no_theme ? 1 : 0;
        wpay.has_theme_mode = ss->has_theme_mode ? 1 : 0;
        wpay.theme_light_mode = ss->theme_light_mode ? 1 : 0;
        wpay.has_source_color_index = ss->has_source_color_index ? 1 : 0;
        wpay.source_color_index = ss->source_color_index;
        wpay.has_seed_color = ss->has_seed_color ? 1 : 0;
        wpay.seed_color = ss->seed_color;
        wpay.transition_type = trans_type;
        wpay.transition_duration_ms = trans_type != WAYWAL_TRANSITION_NONE ? dur_ms : 0;
        wpay.transition_fps = ss->transition_fps;
        wpay.transition_angle_rad = ss->transition_angle_rad;
        wpay.transition_wave_freq = ss->transition_wave_freq;
        wpay.transition_wave_amp = ss->transition_wave_amp;
        wpay.transition_center_x = ss->transition_center_x;
        wpay.transition_center_y = ss->transition_center_y;
        wpay.pos_y_inverted = ss->pos_y_inverted;
        wpay.bezier = ss->bezier;
        wpay.has_custom_bezier = ss->has_custom_bezier ? 1 : 0;
        ok = daemon_activate_workshop(state, &wpay);
    } else {
        const char *ext = strrchr(filepath, '.');
        if (ext && (strcasecmp(ext, ".frag") == 0 || strcasecmp(ext, ".glsl") == 0 ||
                    strcasecmp(ext, ".comp") == 0 || strcasecmp(ext, ".shader") == 0)) {
            waywal_shader_payload_t spay;
            memset(&spay, 0, sizeof(spay));
            strncpy(spay.output, target_spec, sizeof(spay.output) - 1);
            strncpy(spay.shader_path, filepath, sizeof(spay.shader_path) - 1);
            spay.fps = ss->transition_fps > 0 ? ss->transition_fps : 60;
            spay.audio_reactive = waywal_shader_is_audio_reactive(filepath);
            spay.scaling_mode = ss->scaling_mode;
            spay.invert_y = ss->invert_y;
            spay.no_cache = ss->no_cache ? 1 : 0;
            spay.transition_type = trans_type;
            spay.transition_duration_ms = trans_type != WAYWAL_TRANSITION_NONE ? dur_ms : 0;
            spay.transition_fps = ss->transition_fps;
            spay.transition_angle_rad = ss->transition_angle_rad;
            spay.transition_wave_freq = ss->transition_wave_freq;
            spay.transition_wave_amp = ss->transition_wave_amp;
            spay.transition_center_x = ss->transition_center_x;
            spay.transition_center_y = ss->transition_center_y;
            spay.pos_y_inverted = ss->pos_y_inverted;
            spay.bezier = ss->bezier;
            spay.has_custom_bezier = ss->has_custom_bezier ? 1 : 0;
            ok = daemon_activate_shader(state, &spay);
        } else if (ext && (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".webm") == 0 ||
                           strcasecmp(ext, ".mkv") == 0 || strcasecmp(ext, ".mov") == 0 ||
                           strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".m4v") == 0)) {
            ok = daemon_start_video_file_isolated(state, filepath, target_spec,
                                                  (waywal_scaling_mode_t)ss->scaling_mode, NULL, 0,
                                                  0, ss->no_cache);
        }
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (output_matches_target_spec(out, target_spec)) {
            out->slideshow_eng = ss;
        }
    }
    state->g_presenting_slideshow = prev_pres;
    return ok;
}

static bool daemon_apply_output_config_rule(daemon_state_t *state, const waywal_output_rule_t *rule)
{
    if (!state || !rule)
        return false;
    bool is_color = (strcasecmp(rule->mode, "color") == 0 && rule->has_color);
    if (!is_color && rule->path[0] == '\0')
        return false;

    const char *target = rule->output_name[0] ? rule->output_name : "*";
    if (rule->has_crop_gravity) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                out->crop_gravity_x = rule->crop_gravity_x;
                out->crop_gravity_y = rule->crop_gravity_y;
                out->has_crop_gravity = true;
            }
        }
    }
    if (is_color) {
        color_rgba_t col = {
            .r = (uint8_t)((rule->fill_color >> 24) & 0xFFU),
            .g = (uint8_t)((rule->fill_color >> 16) & 0xFFU),
            .b = (uint8_t)((rule->fill_color >> 8) & 0xFFU),
            .a = (uint8_t)(rule->fill_color & 0xFFU),
        };
        return daemon_render_color_on_target(state, target, col);
    }
    if (strcasecmp(rule->mode, "shader") == 0) {
        waywal_shader_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        strncpy(spay.output, target, sizeof(spay.output) - 1);
        strncpy(spay.shader_path, rule->path, sizeof(spay.shader_path) - 1);
        spay.fps = rule->fps > 0 ? rule->fps : 60;
        spay.audio_reactive =
            rule->has_audio ? rule->audio : waywal_shader_is_audio_reactive(rule->path);
        spay.scaling_mode = (uint32_t)rule->scaling;
        bool eff_invert_y = rule->has_invert_y ? rule->invert_y : state->config.transition.invert_y;
        spay.invert_y = eff_invert_y ? 1 : 0;
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
        bool eff_invert_y = rule->has_invert_y ? rule->invert_y : state->config.transition.invert_y;
        wpay.invert_y = eff_invert_y ? 1 : 0;
        return daemon_activate_workshop(state, &wpay);
    }
    if (strcasecmp(rule->mode, "video") == 0) {
        return daemon_start_video_file_isolated(state, rule->path, target, rule->scaling, NULL, 0,
                                                0, false);
    }
    if (strcasecmp(rule->mode, "slideshow") == 0) {
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s = rule->interval > 0 ? rule->interval : 300;
        spay.sort_mode = (uint8_t)rule->sort_mode;
        spay.has_sort_mode = 1;
        spay.random_order = (rule->sort_mode == WAYWAL_SORT_RANDOM);
        spay.no_recursive = rule->recursive ? 0 : 1;
        spay.scaling_mode = (uint32_t)rule->scaling;
        spay.filter_mode = (uint32_t)rule->filter;
        if (rule->has_fill_color) {
            spay.color[0] = (uint8_t)((rule->fill_color >> 24) & 0xFFU);
            spay.color[1] = (uint8_t)((rule->fill_color >> 16) & 0xFFU);
            spay.color[2] = (uint8_t)((rule->fill_color >> 8) & 0xFFU);
            spay.color[3] = (uint8_t)(rule->fill_color & 0xFFU);
        }
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
        bool eff_invert_y = rule->has_invert_y ? rule->invert_y : state->config.transition.invert_y;
        spay.invert_y = eff_invert_y ? 1 : 0;
        spay.pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
        if (rule->has_invert_y && rule->invert_y != state->config.transition.invert_y &&
            spay.transition_center_y >= 0.0f && spay.transition_center_y <= 1.0f) {
            spay.transition_center_y = 1.0f - spay.transition_center_y;
            spay.pos_y_inverted = eff_invert_y ? 1 : 0;
        }
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = true;
        if (rule->has_crop_gravity) {
            spay.crop_gravity_x = rule->crop_gravity_x;
            spay.crop_gravity_y = rule->crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.transition.has_crop_gravity) {
            spay.crop_gravity_x = state->config.transition.crop_gravity_x;
            spay.crop_gravity_y = state->config.transition.crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.daemon.has_crop_gravity) {
            spay.crop_gravity_x = state->config.daemon.crop_gravity_x;
            spay.crop_gravity_y = state->config.daemon.crop_gravity_y;
            spay.has_crop_gravity = 1;
        }
        if (strcmp(target, "*") != 0) {
            spay.num_target_outputs = 1;
            strncpy(spay.target_outputs[0], target, sizeof(spay.target_outputs[0]) - 1);
        }
        strncpy(spay.path, rule->path, sizeof(spay.path) - 1);
        return daemon_start_slideshow_isolated(state, &spay);
    }
    return daemon_render_image_on_target_ex(
        state, rule->path, target, rule->scaling, rule->filter, state->config.daemon.ten_bit,
        WAYWAL_TRANSITION_NONE, 0, 0.0f, false, rule->has_fill_color, rule->fill_color,
        rule->has_crop_gravity, rule->crop_gravity_x, rule->crop_gravity_y);
}

static bool config_has_solar_or_schedule_paths(const waywal_config_t *cfg)
{
    if (!cfg)
        return false;
    if (cfg->solar.manifest_path[0] != '\0' || cfg->solar.dawn_path[0] != '\0' ||
        cfg->solar.day_path[0] != '\0' || cfg->solar.dusk_path[0] != '\0' ||
        cfg->solar.night_path[0] != '\0') {
        return true;
    }
    for (size_t i = 0; i < cfg->schedule_count; ++i) {
        if (cfg->schedule[i].path[0] != '\0') {
            return true;
        }
    }
    return false;
}

static bool schedule_phase_matches(const char *rule_phase, const waywal_solar_state_t *solar_ptr,
                                   waywal_solar_phase_t cur_phase, uint32_t sec_of_day, bool is_day,
                                   bool require_fine_subphase)
{
    waywal_solar_phase_t rp;
    if (!waywal_solar_phase_from_string(rule_phase, &rp))
        return false;
    bool is_fine =
        (rp == WAYWAL_SOLAR_MORNING || rp == WAYWAL_SOLAR_NOON || rp == WAYWAL_SOLAR_AFTERNOON ||
         rp == WAYWAL_SOLAR_SUNSET || rp == WAYWAL_SOLAR_EVENING);
    if (require_fine_subphase && !is_fine)
        return false;
    if (!require_fine_subphase && is_fine)
        return false;

    double ha = solar_ptr ? solar_ptr->hour_angle_deg : (((double)sec_of_day / 60.0) / 4.0 - 180.0);
    if (rp == cur_phase)
        return true;
    if (cur_phase == WAYWAL_SOLAR_DAY) {
        if (rp == WAYWAL_SOLAR_MORNING && ha < -15.0)
            return true;
        if (rp == WAYWAL_SOLAR_NOON && ha >= -15.0 && ha <= 15.0)
            return true;
        if (rp == WAYWAL_SOLAR_AFTERNOON && ha > 15.0)
            return true;
    }
    if (cur_phase == WAYWAL_SOLAR_DUSK) {
        if (rp == WAYWAL_SOLAR_SUNSET && is_day)
            return true;
        if (rp == WAYWAL_SOLAR_EVENING && !is_day)
            return true;
    }
    return false;
}

void daemon_tick_solar_and_schedule(daemon_state_t *state, bool initial_sync)
{
    if (!state)
        return;

    drain_timer_fd(state->solar_timer_fd);

    time_t now_utc = time(NULL);
    struct tm lt;
    memset(&lt, 0, sizeof(lt));
    localtime_r(&now_utc, &lt);
    uint32_t sec_of_day = (uint32_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);

    bool is_day = false;
    waywal_solar_phase_t cur_phase = WAYWAL_SOLAR_NIGHT;
    waywal_solar_state_t sstate;
    memset(&sstate, 0, sizeof(sstate));
    const waywal_solar_state_t *solar_ptr = NULL;
    if (state->config.solar.has_coordinates) {
        waywal_solar_compute(state->config.solar.latitude, state->config.solar.longitude, now_utc,
                             &lt, &sstate);
        is_day = sstate.is_daytime;
        cur_phase = sstate.phase;
        solar_ptr = &sstate;
    } else {
        waywal_solar_synthesize_local(state->config.solar.sunrise_fallback_min,
                                      state->config.solar.sunset_fallback_min, now_utc, &lt,
                                      &sstate);
        is_day = sstate.is_daylight;
        cur_phase = sstate.phase;
        solar_ptr = &sstate;
    }

    uint32_t trans_type = initial_sync
                              ? WAYWAL_TRANSITION_NONE
                              : (state->config.transition.type_enum != WAYWAL_TRANSITION_NONE
                                     ? state->config.transition.type_enum
                                     : WAYWAL_TRANSITION_FADE);
    uint32_t trans_dur = initial_sync ? 0U
                                      : (uint32_t)(state->config.transition.duration > 0.0f
                                                       ? state->config.transition.duration * 1000.0f
                                                       : 1000.0f);
    uint32_t next_wait_ms = 60000U;
    bool wallpaper_rendered_this_tick = false;
    bool has_sched_theme_override = false;
    bool sched_light_mode = is_day;
    bool prev_sched_override_active = state->sched_theme_override_active;
    bool any_sched_theme_rule = false;

    const waywal_schedule_rule_t *chosen_sched = NULL;
    if (state->config.schedule_count > 0) {
        const waywal_schedule_rule_t *best_time_before = NULL;
        const waywal_schedule_rule_t *best_time_wrap = NULL;
        const waywal_schedule_rule_t *best_fine_phase = NULL;
        const waywal_schedule_rule_t *best_coarse_phase = NULL;
        bool any_phase_rule = false;

        for (size_t i = 0; i < state->config.schedule_count; ++i) {
            const waywal_schedule_rule_t *r = &state->config.schedule[i];
            if (r->has_theme_mode) {
                any_sched_theme_rule = true;
            }
            if (r->path[0] == '\0' && !r->has_theme_mode)
                continue;
            if (r->has_time && r->time_minutes >= 0) {
                uint32_t r_sec = (uint32_t)r->time_minutes * 60U;
                if (r_sec <= sec_of_day) {
                    if (!best_time_before || r->time_minutes >= best_time_before->time_minutes) {
                        best_time_before = r;
                    }
                }
                if (!best_time_wrap || r->time_minutes >= best_time_wrap->time_minutes) {
                    best_time_wrap = r;
                }
            } else if (r->phase[0] != '\0') {
                any_phase_rule = true;
                if (schedule_phase_matches(r->phase, solar_ptr, cur_phase, sec_of_day, is_day,
                                           true)) {
                    best_fine_phase = r;
                } else if (schedule_phase_matches(r->phase, solar_ptr, cur_phase, sec_of_day,
                                                  is_day, false)) {
                    best_coarse_phase = r;
                }
            }
        }

        const waywal_schedule_rule_t *best_phase =
            best_fine_phase ? best_fine_phase : best_coarse_phase;
        bool allow_wrap = !any_phase_rule || (!is_day && cur_phase == WAYWAL_SOLAR_NIGHT &&
                                              sstate.true_solar_time_min < sstate.sunrise_min);
        chosen_sched = best_phase ? best_phase
                                  : (best_time_before ? best_time_before
                                                      : (allow_wrap ? best_time_wrap : NULL));
        if (chosen_sched && chosen_sched->has_theme_mode) {
            has_sched_theme_override = true;
            sched_light_mode = chosen_sched->theme_light_mode;
        }
    }

    bool cfg_auto_theme = state->config.solar.auto_day_night ||
                          state->config.theme.auto_day_night ||
                          state->config.daemon.auto_day_night || state->config.auto_day_night;

    const char *man_path =
        state->active_dynwall_path[0] != '\0'
            ? state->active_dynwall_path
            : (state->solar_owns_wallpaper ? state->config.solar.manifest_path : "");
    const char *man_out =
        (state->active_dynwall_path[0] != '\0' && state->active_dynwall_output[0] != '\0')
            ? state->active_dynwall_output
            : "*";
    waywal_dynwall_resolved_t man_res;
    memset(&man_res, 0, sizeof(man_res));
    bool have_man_res = false;
    if (man_path[0] != '\0') {
        waywal_dynwall_manifest_t man;
        if (waywal_dynwall_load_file(man_path, &man) &&
            waywal_dynwall_resolve(&man, now_utc, &lt, solar_ptr, &man_res) &&
            waywal_dynwall_effective_path(&man_res)[0] != '\0') {
            have_man_res = true;
            if (man_res.next_change_sec > 0 && man_res.next_change_sec < 60U) {
                next_wait_ms = (uint32_t)man_res.next_change_sec * 1000U;
                if (next_wait_ms < 1000U)
                    next_wait_ms = 1000U;
            }
            if (man_res.has_theme_mode && !has_sched_theme_override && !cfg_auto_theme &&
                !(state->has_active_theme_meta && state->active_theme_meta.auto_day_night != 0)) {
                has_sched_theme_override = true;
                sched_light_mode = man_res.theme_light_mode;
            }
        }
    }

    if (has_sched_theme_override) {
        state->has_active_theme_meta = true;
        state->active_theme_meta.has_theme_mode = 1;
        state->active_theme_meta.theme_light_mode = sched_light_mode ? 1 : 0;
        state->active_theme_meta.auto_day_night = 0;
        state->sched_theme_override_active = true;
    } else {
        if (state->sched_theme_override_active) {
            state->active_theme_meta.has_theme_mode = 0;
            state->sched_theme_override_active = false;
        }
        if (cfg_auto_theme && state->has_active_theme_meta &&
            !(state->active_theme_meta.has_auto_day_night &&
              state->active_theme_meta.auto_day_night == 0)) {
            state->active_theme_meta.has_theme_mode = 0;
            state->active_theme_meta.auto_day_night = 1;
        }
    }

    if (have_man_res) {
        const char *eff_frame = waywal_dynwall_effective_path(&man_res);
        if (strcmp(state->last_dynwall_frame_path, eff_frame) != 0) {
            state->in_solar_tick = true;
            if (daemon_render_image_on_target(state, eff_frame, man_out, WAYWAL_SCALING_FILL,
                                              WAYWAL_FILTER_LANCZOS3, state->config.daemon.ten_bit,
                                              trans_type, trans_dur)) {
                snprintf(state->last_dynwall_frame_path, sizeof(state->last_dynwall_frame_path),
                         "%s", eff_frame);
                wallpaper_rendered_this_tick = true;
            }
            state->in_solar_tick = false;
        }
    } else if (man_path[0] == '\0' && state->solar_owns_wallpaper) {
        const char *phase_path = NULL;
        switch (cur_phase) {
        case WAYWAL_SOLAR_DAWN:
            phase_path = state->config.solar.dawn_path[0] != '\0' ? state->config.solar.dawn_path
                                                                  : state->config.solar.day_path;
            break;
        case WAYWAL_SOLAR_DAY:
            phase_path = state->config.solar.day_path;
            break;
        case WAYWAL_SOLAR_DUSK:
            phase_path = state->config.solar.dusk_path[0] != '\0' ? state->config.solar.dusk_path
                                                                  : state->config.solar.night_path;
            break;
        case WAYWAL_SOLAR_NIGHT:
        default:
            phase_path = state->config.solar.night_path;
            break;
        }
        if (phase_path && phase_path[0] != '\0' &&
            strcmp(state->last_dynwall_frame_path, phase_path) != 0) {
            state->in_solar_tick = true;
            if (daemon_render_image_on_target(state, phase_path, "*", WAYWAL_SCALING_FILL,
                                              WAYWAL_FILTER_LANCZOS3, state->config.daemon.ten_bit,
                                              trans_type, trans_dur)) {
                snprintf(state->last_dynwall_frame_path, sizeof(state->last_dynwall_frame_path),
                         "%s", phase_path);
                wallpaper_rendered_this_tick = true;
            }
            state->in_solar_tick = false;
        }
    }

    if (state->solar_owns_wallpaper && chosen_sched && chosen_sched->path[0] != '\0' &&
        strcmp(state->last_schedule_frame_path, chosen_sched->path) != 0) {
        const char *sched_out = chosen_sched->output[0] ? chosen_sched->output : "*";
        state->in_solar_tick = true;
        if (daemon_render_image_on_target(state, chosen_sched->path, sched_out, WAYWAL_SCALING_FILL,
                                          WAYWAL_FILTER_LANCZOS3, state->config.daemon.ten_bit,
                                          trans_type, trans_dur)) {
            snprintf(state->last_schedule_frame_path, sizeof(state->last_schedule_frame_path), "%s",
                     chosen_sched->path);
            wallpaper_rendered_this_tick = true;
        }
        state->in_solar_tick = false;
    }

    bool auto_theme = cfg_auto_theme || (state->has_active_theme_meta &&
                                         state->active_theme_meta.auto_day_night != 0);
    bool should_apply_theme =
        state->config.theme.enabled ||
        (state->has_active_theme_meta &&
         (state->active_theme_meta.extract_theme || state->active_theme_meta.has_seed_color));
    if (auto_theme || has_sched_theme_override || prev_sched_override_active) {
        bool target_light =
            has_sched_theme_override
                ? sched_light_mode
                : (auto_theme ? is_day : (state->config.theme.theme_mode == WAYWAL_THEME_LIGHT));
        bool phase_changed =
            state->has_last_solar_state && (state->last_solar_is_daytime != target_light);
        state->last_solar_is_daytime = target_light;
        state->has_last_solar_state = true;

        bool mode_mismatch =
            state->has_active_palette && (state->active_palette.is_light_mode != target_light);
        if ((phase_changed || mode_mismatch) && !wallpaper_rendered_this_tick &&
            should_apply_theme && state->has_active_palette && state->wallpaper_rgba &&
            state->wallpaper_w > 0 && state->wallpaper_h > 0 &&
            !(state->has_active_theme_meta && state->active_theme_meta.no_theme)) {
            waywal_scheme_variant_t variant =
                waywal_scheme_variant_from_str(state->config.theme.scheme_variant);
            waywal_theme_mode_t tmode = target_light ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
            uint8_t src_idx =
                (state->has_active_theme_meta && state->active_theme_meta.has_source_color_index)
                    ? state->active_theme_meta.source_color_index
                    : state->config.theme.source_color_index;
            bool has_seed =
                (state->has_active_theme_meta && state->active_theme_meta.has_seed_color)
                    ? true
                    : state->config.theme.has_seed_color;
            uint32_t seed_rgb =
                (state->has_active_theme_meta && state->active_theme_meta.has_seed_color)
                    ? state->active_theme_meta.seed_color
                    : state->config.theme.seed_color;
            if (state->media_palette_active && state->has_saved_wallpaper_palette) {
                char saved_wp[sizeof(state->saved_wallpaper_palette.wallpaper_path)];
                memcpy(saved_wp, state->saved_wallpaper_palette.wallpaper_path, sizeof(saved_wp));
                if (waywal_palette_extract_seed_ex(
                        state->wallpaper_rgba, state->wallpaper_w, state->wallpaper_h,
                        state->wallpaper_w * 4U, true, tmode, variant, state->config.theme.contrast,
                        src_idx, has_seed, seed_rgb, &state->saved_wallpaper_palette)) {
                    memcpy(state->saved_wallpaper_palette.wallpaper_path, saved_wp,
                           sizeof(saved_wp));
                }
            } else if (!state->media_palette_active) {
                bool is_no_cache =
                    (state->has_active_theme_meta && state->active_theme_meta.no_cache);
                char saved_wp[sizeof(state->active_palette.wallpaper_path)];
                memcpy(saved_wp, state->active_palette.wallpaper_path, sizeof(saved_wp));
                if (waywal_palette_extract_seed_ex(
                        state->wallpaper_rgba, state->wallpaper_w, state->wallpaper_h,
                        state->wallpaper_w * 4U, true, tmode, variant, state->config.theme.contrast,
                        src_idx, has_seed, seed_rgb, &state->active_palette)) {
                    memcpy(state->active_palette.wallpaper_path, saved_wp, sizeof(saved_wp));
                    if (!is_no_cache) {
                        state->saved_wallpaper_palette = state->active_palette;
                        state->has_saved_wallpaper_palette = true;
                        (void)waywal_palette_export_filtered(
                            &state->active_palette, NULL, state->config.theme.caelestia_sync,
                            state->config.theme.export_css, state->config.theme.export_hyprland);
                        (void)waywal_palette_apply_configured_templates(&state->active_palette,
                                                                        NULL, NULL, &state->config);
                    } else if (state->config.template_count > 0) {
                        waywal_config_t cfg_nc = state->config;
                        cfg_nc.theme.pywal_compat = false;
                        (void)waywal_palette_apply_configured_templates(&state->active_palette,
                                                                        NULL, NULL, &cfg_nc);
                    }
                    if (state->config.theme.tty_sequences) {
                        (void)waywal_palette_broadcast_tty(&state->active_palette);
                    }
                    if (state->config.hooks.on_theme_change[0] != '\0') {
                        (void)waywal_palette_run_hook(state->config.hooks.on_theme_change);
                    }
                    daemon_broadcast_event(state, "theme_changed");
                }
            }
        }
    }

    if (wallpaper_rendered_this_tick) {
        daemon_broadcast_event(state, "wallpaper_changed");
    }

    bool solar_active =
        (state->active_dynwall_path[0] != '\0') ||
        (state->solar_owns_wallpaper && config_has_solar_or_schedule_paths(&state->config)) ||
        any_sched_theme_rule || auto_theme;
    bool pause_all =
        state->power_gov ? waywal_power_governor_should_pause_all(state->power_gov) : false;
    if (state->solar_timer_fd >= 0) {
        if (solar_active && !pause_all) {
            arm_oneshot_timer_ms(state->solar_timer_fd, next_wait_ms);
        } else {
            disarm_timer_fd(state->solar_timer_fd);
        }
    }
}

static void daemon_on_output_configured(daemon_state_t *state, output_node_t *out)
{
    if (!state || !out || out->name[0] == '\0')
        return;

    /* 1. Check per-output config rule in config.toml (exact match first, then [default]/[any]) */
    const waywal_output_rule_t *rule = waywal_config_match_output(&state->config, out->name);
    if (rule && (rule->path[0] != '\0' || rule->has_color)) {
        waywal_output_rule_t scoped = *rule;
        if (scoped.output_name[0] == '\0' || strcmp(scoped.output_name, "*") == 0) {
            strncpy(scoped.output_name, out->name, sizeof(scoped.output_name) - 1);
            scoped.output_name[sizeof(scoped.output_name) - 1] = '\0';
        }
        (void)daemon_apply_output_config_rule(state, &scoped);
        return;
    }

    /* 2. Check persisted state_store for this output or wildcard "*" */
    const waywal_output_saved_state_t *saved =
        waywal_state_store_find_output(&g_saved_state, out->name);
    if (!saved) {
        saved = waywal_state_store_find_output(&g_saved_state, "*");
    }
    if (saved) {
        (void)daemon_restore_output_entry(state, saved, out->name);
        return;
    }

    if (state->active_dynwall_path[0] != '\0' || state->config.solar.manifest_path[0] != '\0' ||
        state->config.solar.dawn_path[0] != '\0' || state->config.solar.day_path[0] != '\0' ||
        state->config.solar.dusk_path[0] != '\0' || state->config.solar.night_path[0] != '\0' ||
        state->config.schedule_count > 0) {
        state->solar_owns_wallpaper = true;
        state->last_dynwall_frame_path[0] = '\0';
        state->last_schedule_frame_path[0] = '\0';
        daemon_tick_solar_and_schedule(state, true);
    }
}

static bool daemon_apply_workspace_rule(daemon_state_t *state, const waywal_workspace_rule_t *wrule,
                                        const char *monitor_name, int delta)
{
    if (!state || !wrule)
        return false;
    bool is_color = (strcasecmp(wrule->mode, "color") == 0 && wrule->has_color);
    if (!is_color && wrule->path[0] == '\0')
        return false;

    const char *target = wrule->output_name[0]
                             ? wrule->output_name
                             : (monitor_name && monitor_name[0] ? monitor_name : "*");
    uint32_t trans_type =
        wrule->has_transition ? (uint32_t)wrule->transition_type_enum : WAYWAL_TRANSITION_SLIDE;
    float dur_sec =
        wrule->has_transition_duration
            ? wrule->transition_duration
            : (state->config.transition.duration > 0.0f ? state->config.transition.duration : 0.4f);
    uint32_t dur_ms = (uint32_t)(dur_sec * 1000.0f);
    float default_angle =
        wrule->directional ? (delta >= 0 ? 0.0f : 180.0f) : state->config.transition.angle;
    float angle_deg = wrule->has_transition_angle ? wrule->transition_angle : default_angle;
    float angle_rad = angle_deg * (3.14159265358979323846f / 180.0f);
    uint32_t trans_fps = state->config.transition.fps > 0 ? state->config.transition.fps : 60;
    uint32_t rule_fps = wrule->fps > 0 ? wrule->fps : trans_fps;
    waywal_filter_mode_t filt = wrule->has_filter ? wrule->filter : WAYWAL_FILTER_LANCZOS3;

    if (wrule->has_crop_gravity) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_target_spec(out, target)) {
                out->crop_gravity_x = wrule->crop_gravity_x;
                out->crop_gravity_y = wrule->crop_gravity_y;
                out->has_crop_gravity = true;
            }
        }
    }

    if (is_color) {
        color_rgba_t col = {
            .r = (uint8_t)((wrule->fill_color >> 24) & 0xFFU),
            .g = (uint8_t)((wrule->fill_color >> 16) & 0xFFU),
            .b = (uint8_t)((wrule->fill_color >> 8) & 0xFFU),
            .a = (uint8_t)(wrule->fill_color & 0xFFU),
        };
        if (state->outputs && trans_type != WAYWAL_TRANSITION_NONE && dur_ms > 0) {
            uint8_t meta_storage[sizeof(waywal_img_metadata_t) + 128];
            memset(meta_storage, 0, sizeof(meta_storage));
            waywal_img_metadata_t *meta = (waywal_img_metadata_t *)meta_storage;
            meta->width = 1;
            meta->height = 1;
            meta->is_solid_color = 1;
            meta->color[0] = col.r;
            meta->color[1] = col.g;
            meta->color[2] = col.b;
            meta->color[3] = col.a;
            meta->transition_type = trans_type;
            meta->transition_duration_ms = dur_ms;
            meta->transition_fps = trans_fps;
            meta->transition_angle_rad = angle_rad;
            meta->transition_wave_freq = state->config.transition.wave_x;
            meta->transition_wave_amp = state->config.transition.wave_y;
            meta->transition_center_x = state->config.transition.pos_x;
            meta->transition_center_y = state->config.transition.pos_y;
            meta->invert_y = state->config.transition.invert_y ? 1 : 0;
            meta->pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
            meta->bezier.p1x = state->config.transition.bezier[0];
            meta->bezier.p1y = state->config.transition.bezier[1];
            meta->bezier.p2x = state->config.transition.bezier[2];
            meta->bezier.p2y = state->config.transition.bezier[3];
            meta->has_custom_bezier = true;
            if (strcmp(target, "*") != 0) {
                size_t extra_len = strlen(target) + 1;
                if (extra_len <= 64) {
                    meta->num_target_outputs = 1;
                    meta->extra_data_len = (uint32_t)extra_len;
                    memcpy(meta_storage + sizeof(waywal_img_metadata_t), target, extra_len);
                }
            }
            bool can_transition_color = false;
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (out->surface && out->configured && out->width > 0 && out->height > 0 &&
                    output_matches_img_meta(out, meta)) {
                    can_transition_color = true;
                    break;
                }
            }
            if (can_transition_color) {
                char matched_outputs[256] = {0};
                size_t mlen = 0;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (output_matches_img_meta(out, meta) && out->name[0] != '\0') {
                        int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen,
                                         "%s%s", mlen > 0 ? "," : "", out->name);
                        if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                            mlen += (size_t)n;
                        }
                    }
                }
                uint8_t solid_bgra[64 * 64 * 4];
                for (size_t px_i = 0; px_i < 64U * 64U; ++px_i) {
                    solid_bgra[px_i * 4U + 0U] = col.b;
                    solid_bgra[px_i * 4U + 1U] = col.g;
                    solid_bgra[px_i * 4U + 2U] = col.r;
                    solid_bgra[px_i * 4U + 3U] = col.a;
                }
                (void)waywal_ecosystem_cache_sync(matched_outputs[0] ? matched_outputs : target, "",
                                                  "Nearest", solid_bgra, 64, 64,
                                                  state->config.general.caelestia_sync, false);
            }
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (output_matches_img_meta(out, meta)) {
                    layer_shell_snapshot_live_output(state, out);
                    stop_engines_on_output(state, out, false, false, false, false);
                }
            }
            reconcile_global_engine_flags(state);
            uint8_t px_bgra[4] = {col.b, col.g, col.r, col.a};
            daemon_update_palette_from_bgra(state, px_bgra, 1, 1, meta, true);
            if (daemon_start_image_transition(state, px_bgra, 1, 1, meta)) {
                if (state->config.hooks.on_wallpaper_change[0] != '\0') {
                    (void)waywal_palette_run_hook(state->config.hooks.on_wallpaper_change);
                }
                return true;
            }
        }
        return daemon_render_color_on_target(state, target, col);
    }
    if (strcasecmp(wrule->mode, "shader") == 0) {
        waywal_shader_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        strncpy(spay.output, target, sizeof(spay.output) - 1);
        strncpy(spay.shader_path, wrule->path, sizeof(spay.shader_path) - 1);
        spay.fps = rule_fps;
        spay.audio_reactive =
            wrule->has_audio ? wrule->audio : waywal_shader_is_audio_reactive(wrule->path);
        spay.scaling_mode = (uint32_t)wrule->scaling;
        spay.invert_y = state->config.transition.invert_y ? 1 : 0;
        spay.transition_type = trans_type;
        spay.transition_duration_ms = dur_ms;
        spay.transition_fps = trans_fps;
        spay.transition_angle_rad = angle_rad;
        spay.transition_wave_freq = state->config.transition.wave_x;
        spay.transition_wave_amp = state->config.transition.wave_y;
        spay.transition_center_x = state->config.transition.pos_x;
        spay.transition_center_y = state->config.transition.pos_y;
        spay.pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = 1;
        return daemon_activate_shader(state, &spay);
    }
    if (strcasecmp(wrule->mode, "workshop") == 0) {
        waywal_workshop_payload_t wpay;
        memset(&wpay, 0, sizeof(wpay));
        strncpy(wpay.output, target, sizeof(wpay.output) - 1);
        strncpy(wpay.workshop_dir, wrule->path, sizeof(wpay.workshop_dir) - 1);
        strncpy(wpay.prop_overrides, wrule->prop_overrides, sizeof(wpay.prop_overrides) - 1);
        wpay.fps = rule_fps;
        wpay.audio_reactive = wrule->audio;
        wpay.scaling_mode = (uint32_t)wrule->scaling;
        wpay.invert_y = state->config.transition.invert_y ? 1 : 0;
        wpay.transition_type = trans_type;
        wpay.transition_duration_ms = dur_ms;
        wpay.transition_fps = trans_fps;
        wpay.transition_angle_rad = angle_rad;
        wpay.transition_wave_freq = state->config.transition.wave_x;
        wpay.transition_wave_amp = state->config.transition.wave_y;
        wpay.transition_center_x = state->config.transition.pos_x;
        wpay.transition_center_y = state->config.transition.pos_y;
        wpay.pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
        wpay.bezier.p1x = state->config.transition.bezier[0];
        wpay.bezier.p1y = state->config.transition.bezier[1];
        wpay.bezier.p2x = state->config.transition.bezier[2];
        wpay.bezier.p2y = state->config.transition.bezier[3];
        wpay.has_custom_bezier = 1;
        return daemon_activate_workshop(state, &wpay);
    }
    if (strcasecmp(wrule->mode, "video") == 0) {
        return daemon_start_video_file_isolated(state, wrule->path, target, wrule->scaling, NULL, 0,
                                                0, false);
    }
    if (strcasecmp(wrule->mode, "slideshow") == 0) {
        waywal_slideshow_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        spay.interval_s = wrule->interval > 0 ? wrule->interval
                                              : (state->config.daemon.slideshow_interval > 0
                                                     ? state->config.daemon.slideshow_interval
                                                     : 300);
        spay.sort_mode = (uint8_t)wrule->sort_mode;
        spay.has_sort_mode = 1;
        spay.random_order = (wrule->sort_mode == WAYWAL_SORT_RANDOM);
        spay.no_recursive = wrule->recursive ? 0 : 1;
        spay.scaling_mode = (uint32_t)wrule->scaling;
        spay.filter_mode = (uint32_t)filt;
        if (wrule->has_fill_color) {
            spay.color[0] = (uint8_t)((wrule->fill_color >> 24) & 0xFFU);
            spay.color[1] = (uint8_t)((wrule->fill_color >> 16) & 0xFFU);
            spay.color[2] = (uint8_t)((wrule->fill_color >> 8) & 0xFFU);
            spay.color[3] = (uint8_t)(wrule->fill_color & 0xFFU);
        }
        spay.ten_bit = state->config.daemon.ten_bit;
        spay.transition_type = trans_type;
        spay.transition_duration_ms = dur_ms;
        spay.transition_fps = trans_fps;
        spay.transition_angle_rad = angle_rad;
        spay.transition_wave_freq = state->config.transition.wave_x;
        spay.transition_wave_amp = state->config.transition.wave_y;
        spay.transition_center_x = state->config.transition.pos_x;
        spay.transition_center_y = state->config.transition.pos_y;
        spay.invert_y = state->config.transition.invert_y ? 1 : 0;
        spay.pos_y_inverted = state->config.transition.pos_y_inverted ? 1 : 0;
        spay.bezier.p1x = state->config.transition.bezier[0];
        spay.bezier.p1y = state->config.transition.bezier[1];
        spay.bezier.p2x = state->config.transition.bezier[2];
        spay.bezier.p2y = state->config.transition.bezier[3];
        spay.has_custom_bezier = true;
        if (wrule->has_crop_gravity) {
            spay.crop_gravity_x = wrule->crop_gravity_x;
            spay.crop_gravity_y = wrule->crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.transition.has_crop_gravity) {
            spay.crop_gravity_x = state->config.transition.crop_gravity_x;
            spay.crop_gravity_y = state->config.transition.crop_gravity_y;
            spay.has_crop_gravity = 1;
        } else if (state->config.daemon.has_crop_gravity) {
            spay.crop_gravity_x = state->config.daemon.crop_gravity_x;
            spay.crop_gravity_y = state->config.daemon.crop_gravity_y;
            spay.has_crop_gravity = 1;
        }
        if (strcmp(target, "*") != 0) {
            spay.num_target_outputs = 1;
            strncpy(spay.target_outputs[0], target, sizeof(spay.target_outputs[0]) - 1);
        }
        strncpy(spay.path, wrule->path, sizeof(spay.path) - 1);
        return daemon_start_slideshow_isolated(state, &spay);
    }
    return daemon_render_image_on_target_ex(
        state, wrule->path, target, wrule->scaling, filt, state->config.daemon.ten_bit, trans_type,
        dur_ms, angle_deg, true, wrule->has_fill_color, wrule->fill_color, wrule->has_crop_gravity,
        wrule->crop_gravity_x, wrule->crop_gravity_y);
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
            if (ws[0] != '\0' && strcmp(state->last_workspace_name, ws) == 0) {
                continue;
            }
            strncpy(state->last_workspace_name, ws, sizeof(state->last_workspace_name) - 1);
            state->last_workspace_name[sizeof(state->last_workspace_name) - 1] = '\0';
            int ws_idx = atoi(ws);
            int delta = (ws_idx > 0 && state->last_workspace_id > 0)
                            ? (ws_idx - state->last_workspace_id)
                            : 1;
            if (ws_idx > 0) {
                state->last_workspace_id = ws_idx;
                state->workspace_target_x = (float)((ws_idx - 1) % 10) * 0.1f;
            } else {
                state->workspace_target_x += 0.1f;
                if (state->workspace_target_x > 1.0f)
                    state->workspace_target_x -= 1.0f;
            }
            for (size_t i = 0; i < state->config.workspace_count; ++i) {
                const waywal_workspace_rule_t *wrule = &state->config.workspaces[i];
                if (strcmp(wrule->workspace_name, ws) == 0 &&
                    (wrule->path[0] != '\0' || wrule->has_color)) {
                    if (daemon_apply_workspace_rule(state, wrule, mon, delta)) {
                        daemon_broadcast_event(state, "wallpaper_changed");
                    }
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
    printf("  -d, --daemon                 Fork into background daemon\n");
    printf("      --no-daemon              Stay in foreground (swww-daemon compatibility)\n");
    printf("  -f, --format <FMT>           Pixel format override (xrgb, argb, bgr, rgb)\n");
    printf("  -q, --quiet                  Suppress informational logs\n");
    printf("      --layer <LAYER>          Layer target: background or bottom\n");
    printf("      --backdrop <MODE>        Backdrop mode: none, auto-blur, or path\n");
    printf("      --battery-policy <POL>   Battery policy: ignore, reduce-fps, pause-video\n");
    printf("  -v, --verbose                Enable debug logging\n");
    printf("  -V, --version                Print version information\n");
    printf("  -h, --help                   Print help information\n");
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
        wl_display_dispatch_pending(st->display);
    } else {
        wl_display_cancel_read(st->display);
        wl_display_dispatch_pending(st->display);
    }
    if (wl_display_get_error(st->display) != 0) {
        g_daemon_wl_ok = false;
        return false;
    }
    return true;
}

static void daemon_apply_all_output_config_rules(daemon_state_t *st, bool *out_slideshow_applied,
                                                 bool *out_theme_updated)
{
    if (!st)
        return;
    bool has_specific = false;
    for (size_t i = 0; i < st->config.output_count; ++i) {
        const waywal_output_rule_t *r = &st->config.outputs[i];
        if (r->output_name[0] != '\0' && strcmp(r->output_name, "*") != 0) {
            has_specific = true;
            break;
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < st->config.output_count; ++i) {
            const waywal_output_rule_t *rule = &st->config.outputs[i];
            bool is_wild = (rule->output_name[0] == '\0' || strcmp(rule->output_name, "*") == 0);
            if ((pass == 0 && !is_wild) || (pass == 1 && is_wild))
                continue;
            if (!is_wild && st->outputs != NULL) {
                bool matched_connected = false;
                for (output_node_t *out = st->outputs; out != NULL; out = out->next) {
                    if (output_matches_target_spec(out, rule->output_name)) {
                        matched_connected = true;
                        break;
                    }
                }
                if (!matched_connected) {
                    continue;
                }
            }
            if (is_wild && has_specific && st->outputs != NULL) {
                for (output_node_t *out = st->outputs; out != NULL; out = out->next) {
                    if (out->name[0] == '\0')
                        continue;
                    if (waywal_config_match_output(&st->config, out->name) == rule) {
                        waywal_output_rule_t scoped = *rule;
                        strncpy(scoped.output_name, out->name, sizeof(scoped.output_name) - 1);
                        scoped.output_name[sizeof(scoped.output_name) - 1] = '\0';
                        if (strcasecmp(scoped.mode, "slideshow") == 0 && out_slideshow_applied) {
                            *out_slideshow_applied = true;
                        }
                        if (daemon_apply_output_config_rule(st, &scoped) &&
                            strcasecmp(scoped.mode, "video") != 0 && out_theme_updated) {
                            *out_theme_updated = true;
                        }
                    }
                }
                continue;
            }
            if (strcasecmp(rule->mode, "slideshow") == 0 && out_slideshow_applied) {
                *out_slideshow_applied = true;
            }
            if (daemon_apply_output_config_rule(st, rule) && strcasecmp(rule->mode, "video") != 0 &&
                out_theme_updated) {
                *out_theme_updated = true;
            }
        }
    }
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
        bool rule_updated_theme = false;
        daemon_apply_all_output_config_rules(st, &slideshow_rule_applied, &rule_updated_theme);
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
            st->slideshow.invert_y = st->config.transition.invert_y ? 1 : 0;
            st->slideshow.pos_y_inverted = st->config.transition.pos_y_inverted ? 1 : 0;
            st->slideshow.bezier.p1x = st->config.transition.bezier[0];
            st->slideshow.bezier.p1y = st->config.transition.bezier[1];
            st->slideshow.bezier.p2x = st->config.transition.bezier[2];
            st->slideshow.bezier.p2y = st->config.transition.bezier[3];
            st->slideshow.has_custom_bezier = true;
            st->slideshow.ten_bit = st->config.daemon.ten_bit;
            if (st->config.daemon.has_slideshow_interval &&
                st->config.daemon.slideshow_interval > 0) {
                uint32_t new_interval = st->config.daemon.slideshow_interval;
                bool interval_changed = (st->slideshow.interval_s != new_interval);
                st->slideshow.interval_s = new_interval;
                if (interval_changed && !st->slideshow.paused && st->slideshow.timer_fd >= 0) {
                    uint64_t exp = 0;
                    while (read(st->slideshow.timer_fd, &exp, sizeof(exp)) > 0) {
                    }
                    struct itimerspec its;
                    memset(&its, 0, sizeof(its));
                    its.it_value.tv_sec = (time_t)new_interval;
                    its.it_interval.tv_sec = (time_t)new_interval;
                    (void)timerfd_settime(st->slideshow.timer_fd, 0, &its, NULL);
                }
            }
            if (st->config.daemon.has_slideshow_sort) {
                st->slideshow.sort_mode = (uint8_t)st->config.daemon.slideshow_sort;
                st->slideshow.has_sort_mode = true;
                st->slideshow.random_order = st->config.daemon.slideshow_shuffle;
            } else if (st->config.daemon.has_slideshow_shuffle) {
                st->slideshow.random_order = st->config.daemon.slideshow_shuffle;
                st->slideshow.sort_mode = st->config.daemon.slideshow_shuffle
                                              ? (uint8_t)WAYWAL_SORT_RANDOM
                                              : (uint8_t)WAYWAL_SORT_ASCENDING;
            }
            if (st->config.daemon.has_slideshow_recursive) {
                st->slideshow.recursive = st->config.daemon.slideshow_recursive;
            }
            if (st->config.transition.has_crop_gravity) {
                st->slideshow.crop_gravity_x = st->config.transition.crop_gravity_x;
                st->slideshow.crop_gravity_y = st->config.transition.crop_gravity_y;
                st->slideshow.has_crop_gravity = true;
            } else if (st->config.daemon.has_crop_gravity) {
                st->slideshow.crop_gravity_x = st->config.daemon.crop_gravity_x;
                st->slideshow.crop_gravity_y = st->config.daemon.crop_gravity_y;
                st->slideshow.has_crop_gravity = true;
            } else {
                st->slideshow.crop_gravity_x = 0.5f;
                st->slideshow.crop_gravity_y = 0.5f;
                st->slideshow.has_crop_gravity = false;
            }
            (void)slideshow_rescan(&st->slideshow);
        }
        bool should_reload_theme =
            st->config.theme.enabled ||
            (st->has_active_theme_meta &&
             (st->active_theme_meta.extract_theme || st->active_theme_meta.has_seed_color));
        if (!rule_updated_theme && should_reload_theme && st->has_active_palette &&
            !(st->has_active_theme_meta && st->active_theme_meta.no_theme &&
              !st->media_palette_active)) {
            bool is_no_cache = (st->has_active_theme_meta && st->active_theme_meta.no_cache &&
                                !st->media_palette_active);
            if (!st->media_palette_active && st->wallpaper_rgba && st->wallpaper_w > 0 &&
                st->wallpaper_h > 0) {
                char saved_wp[sizeof(st->active_palette.wallpaper_path)];
                memcpy(saved_wp, st->active_palette.wallpaper_path, sizeof(saved_wp));
                waywal_scheme_variant_t variant =
                    waywal_scheme_variant_from_str(st->config.theme.scheme_variant);
                waywal_theme_mode_t tmode = daemon_resolve_effective_theme_mode(
                    st, st->has_active_theme_meta ? &st->active_theme_meta : NULL);
                uint8_t src_idx =
                    (st->has_active_theme_meta && st->active_theme_meta.has_source_color_index)
                        ? st->active_theme_meta.source_color_index
                        : st->config.theme.source_color_index;
                bool has_seed = (st->has_active_theme_meta && st->active_theme_meta.has_seed_color)
                                    ? true
                                    : st->config.theme.has_seed_color;
                uint32_t seed_rgb =
                    (st->has_active_theme_meta && st->active_theme_meta.has_seed_color)
                        ? st->active_theme_meta.seed_color
                        : st->config.theme.seed_color;
                if (waywal_palette_extract_seed_ex(
                        st->wallpaper_rgba, st->wallpaper_w, st->wallpaper_h, st->wallpaper_w * 4U,
                        true, tmode, variant, st->config.theme.contrast, src_idx, has_seed,
                        seed_rgb, &st->active_palette)) {
                    if (is_no_cache) {
                        if (st->has_saved_wallpaper_palette) {
                            memcpy(st->active_palette.wallpaper_path,
                                   st->saved_wallpaper_palette.wallpaper_path,
                                   sizeof(st->active_palette.wallpaper_path));
                        } else {
                            st->active_palette.wallpaper_path[0] = '\0';
                        }
                    } else {
                        memcpy(st->active_palette.wallpaper_path, saved_wp, sizeof(saved_wp));
                        st->saved_wallpaper_palette = st->active_palette;
                    }
                }
            }
            if (!is_no_cache) {
                (void)waywal_palette_export_filtered(
                    &st->active_palette, NULL, st->config.theme.caelestia_sync,
                    st->config.theme.export_css, st->config.theme.export_hyprland);
                (void)waywal_palette_apply_configured_templates(&st->active_palette, NULL, NULL,
                                                                &st->config);
            } else if (st->config.template_count > 0) {
                waywal_config_t cfg_nc = st->config;
                cfg_nc.theme.pywal_compat = false;
                (void)waywal_palette_apply_configured_templates(&st->active_palette, NULL, NULL,
                                                                &cfg_nc);
            }
            if (st->config.theme.tty_sequences) {
                (void)waywal_palette_broadcast_tty(&st->active_palette);
            }
            if (st->config.hooks.on_theme_change[0] != '\0') {
                (void)waywal_palette_run_hook(st->config.hooks.on_theme_change);
            }
        }
        if (!st->has_active_palette && st->config.output_count == 0 && !st->slideshow.active &&
            config_has_solar_or_schedule_paths(&st->config)) {
            st->solar_owns_wallpaper = true;
        }
        st->last_dynwall_frame_path[0] = '\0';
        st->last_schedule_frame_path[0] = '\0';
        daemon_tick_solar_and_schedule(st, false);
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
    case URING_EV_SOLAR_TIMER:
        if ((res & POLLIN) == 0)
            break;
        daemon_pump_wayland_events(st);
        daemon_tick_solar_and_schedule(st, false);
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
        } else if (fd == st->solar_timer_fd) {
            daemon_tick_solar_and_schedule(st, false);
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
    state.solar_timer_fd = -1;
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
    const char *env_ns = getenv("AWWW_NAMESPACE");
    if (!env_ns || env_ns[0] == '\0') {
        env_ns = getenv("SWWW_NAMESPACE");
    }
    if (env_ns && env_ns[0] != '\0') {
        strncpy(state.namespace_str, env_ns, sizeof(state.namespace_str) - 1);
        state.namespace_str[sizeof(state.namespace_str) - 1] = '\0';
    }
    waywal_battery_policy_t battery_policy = state.config.daemon.battery_policy_mode;
    bool daemonize_bg = false;

    static const struct option long_options[] = {{"namespace", required_argument, NULL, 'n'},
                                                 {"daemon", no_argument, NULL, 'd'},
                                                 {"no-daemon", no_argument, NULL, 1009},
                                                 {"format", required_argument, NULL, 'f'},
                                                 {"quiet", no_argument, NULL, 'q'},
                                                 {"layer", required_argument, NULL, 1001},
                                                 {"backdrop", required_argument, NULL, 1002},
                                                 {"battery-policy", required_argument, NULL, 1003},
                                                 {"volume", required_argument, NULL, 1004},
                                                 {"mute-on-audio", no_argument, NULL, 1005},
                                                 {"no-mute-on-audio", no_argument, NULL, 1006},
                                                 {"theme-from-media", no_argument, NULL, 1007},
                                                 {"no-theme-from-media", no_argument, NULL, 1008},
                                                 {"verbose", no_argument, NULL, 'v'},
                                                 {"version", no_argument, NULL, 'V'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "n:df:qvhV", long_options, NULL)) != -1) {
        switch (opt) {
        case 'n':
            strncpy(state.namespace_str, optarg, sizeof(state.namespace_str) - 1);
            state.namespace_str[sizeof(state.namespace_str) - 1] = '\0';
            break;
        case 'd':
            daemonize_bg = true;
            break;
        case 1009:
            daemonize_bg = false;
            break;
        case 'f':
            break;
        case 'q':
            waywal_log_set_level(WAYWAL_LOG_LEVEL_ERR);
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
        case 'V':
            printf("wwald %s\n", WAYWAL_VERSION);
            return 0;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (waywal_ipc_namespace_is_alive(state.namespace_str)) {
        WAYWAL_LOG_INFO("wwald daemon is already running for namespace '%s'", state.namespace_str);
        return 0;
    }

    int ready_pipe[2] = {-1, -1};
    if (daemonize_bg) {
        if (pipe2(ready_pipe, O_CLOEXEC) < 0) {
            WAYWAL_LOG_ERR("pipe2() failed: %s", strerror(errno));
            return 1;
        }
        pid_t bg_pid = fork();
        if (bg_pid < 0) {
            WAYWAL_LOG_ERR("fork() failed: %s", strerror(errno));
            close(ready_pipe[0]);
            close(ready_pipe[1]);
            return 1;
        }
        if (bg_pid > 0) {
            close(ready_pipe[1]);
            uint8_t ok_byte = 0;
            ssize_t rd = 0;
            do {
                rd = read(ready_pipe[0], &ok_byte, 1);
            } while (rd < 0 && errno == EINTR);
            close(ready_pipe[0]);
            return (rd == 1 && ok_byte == 1) ? 0 : 1;
        }
        close(ready_pipe[0]);
        ready_pipe[0] = -1;
        (void)setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
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
    waywal_state_store_set_namespace(state.namespace_str);
    waywal_state_store_init(&g_saved_state);
    (void)waywal_state_store_load(&g_saved_state, NULL);

    if (!arena_init(&state.frame_arena, WAYWAL_FRAME_ARENA_CAPACITY)) {
        fprintf(stderr, "Fatal: failed to initialize frame memory arena\n");
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
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
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
        return 1;
    }

    state.signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (state.signal_fd < 0) {
        WAYWAL_LOG_ERR("signalfd failed: %s", strerror(errno));
        arena_destroy(&state.frame_arena);
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
        return 1;
    }

    if (!daemon_wayland_init(&state)) {
        WAYWAL_LOG_ERR("Failed to initialize Wayland subsystem");
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
        return 1;
    }

    state.server_socket_fd = waywal_ipc_server_create(state.namespace_str);
    if (state.server_socket_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create IPC server socket");
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
        return 1;
    }

    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        WAYWAL_LOG_ERR("epoll_create1 failed: %s", strerror(errno));
        close(state.server_socket_fd);
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        if (state.mpris)
            waywal_mpris_destroy(state.mpris);
        if (state.power_gov)
            waywal_power_governor_destroy(state.power_gov);
        if (state.audio_fft)
            waywal_audio_fft_destroy(state.audio_fft);
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
    state.solar_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

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
    if (state.solar_timer_fd >= 0) {
        struct epoll_event ev_solar = {.events = EPOLLIN, .data.fd = state.solar_timer_fd};
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.solar_timer_fd, &ev_solar);
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
        if (state.solar_timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.solar_timer_fd, POLLIN, URING_EV_SOLAR_TIMER,
                                &state);
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
    security_sandbox_register_config(&state.config);
    security_sandbox_apply(NULL);

    state.solar_owns_wallpaper = config_has_solar_or_schedule_paths(&state.config);
    state.on_output_configured = daemon_on_output_configured;
    state.on_wallpaper_loaded = daemon_on_wallpaper_loaded;
    if (state.config.output_count > 0) {
        daemon_apply_all_output_config_rules(&state, NULL, NULL);
    } else if (state.config.daemon.restore_on_start) {
        for (output_node_t *out = state.outputs; out != NULL; out = out->next) {
            daemon_on_output_configured(&state, out);
        }
    }
    daemon_tick_solar_and_schedule(&state, true);

    WAYWAL_LOG_INFO("wwald daemon running successfully (namespace: %s)", state.namespace_str);
    if (ready_pipe[1] >= 0) {
        uint8_t ok_byte = 1;
        (void)write(ready_pipe[1], &ok_byte, 1);
        close(ready_pipe[1]);
        ready_pipe[1] = -1;
    }

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
                } else if (ev_fd == state.solar_timer_fd) {
                    daemon_pump_wayland_events(&state);
                    daemon_tick_solar_and_schedule(&state, false);
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
        int ino_fd = waywal_shader_engine_get_inotify_fd(state.shader_eng);
        if (ino_fd >= 0) {
            unregister_fd_from_loops(&state, ino_fd);
        }
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
    if (state.solar_timer_fd >= 0) {
        close(state.solar_timer_fd);
        state.solar_timer_fd = -1;
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

    WAYWAL_LOG_INFO("Clean shutdown complete.");
    return 0;
}
