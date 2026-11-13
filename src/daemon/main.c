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
#include "waywal/we_script.h"
#include "waywal/we_json.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/shader_graph.h"
#include "waywal/we_mdl_writer.h"
#include "waywal/we_mesh_rig.h"
#if __has_include("waywal/we_asset_writer.h")
#include "waywal/we_asset_writer.h"
#define WAYWAL_HAVE_ASSET_WRITER 1
#else
#define WAYWAL_HAVE_ASSET_WRITER 0
#endif
#if __has_include("waywal/we_gpu_brush.h")
#include "waywal/we_gpu_brush.h"
#define WAYWAL_HAVE_GPU_BRUSH 1
#else
#define WAYWAL_HAVE_GPU_BRUSH 0
#endif

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

float waywal_shader_engine_get_render_scale(const waywal_shader_engine_t *eng);
uint32_t waywal_shader_engine_get_target_fps(const waywal_shader_engine_t *eng);
bool waywal_shader_engine_uses_wallpaper(const waywal_shader_engine_t *eng);
bool waywal_shader_engine_uses_album_art(const waywal_shader_engine_t *eng);
void waywal_audio_fft_set_capture_active(waywal_audio_fft_t *fft, bool active);
void dmabuf_ring_trim_idle(dmabuf_ring_t *ring);

static void rig_invalidate_scene_slots(const we_scene_t *sc);
static void rig_clear_all_slots(void);

static void authoring_invalidate_scene(const we_scene_t *sc);
static void daemon_authoring_cleanup(void);

static void daemon_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    if (dst == src) {
        dst[cap - 1] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

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
    time_t exp_sec = (time_t)(interval_ns / 1000000000ULL);
    long exp_nsec = (long)(interval_ns % 1000000000ULL);

    struct itimerspec cur_its;
    if (timerfd_gettime(timer_fd, &cur_its) == 0) {
        if (cur_its.it_interval.tv_sec == exp_sec && cur_its.it_interval.tv_nsec == exp_nsec &&
            (cur_its.it_value.tv_sec != 0 || cur_its.it_value.tv_nsec != 0)) {
            return;
        }
    }

    struct itimerspec its = {
        .it_interval =
            {
                .tv_sec = exp_sec,
                .tv_nsec = exp_nsec,
            },
        .it_value =
            {
                .tv_sec = exp_sec,
                .tv_nsec = exp_nsec,
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
    for (uint32_t i = 0; i < spay->num_target_outputs && i < 16; ++i) {
        if (strncmp(spay->target_outputs[i], "*", sizeof(spay->target_outputs[i])) == 0 ||
            strncmp(spay->target_outputs[i], out->name, sizeof(spay->target_outputs[i])) == 0) {
            return true;
        }
        char target_buf[sizeof(spay->target_outputs[i]) + 1];
        memcpy(target_buf, spay->target_outputs[i], sizeof(spay->target_outputs[i]));
        target_buf[sizeof(spay->target_outputs[i])] = '\0';
        if (output_matches_target_spec(out, target_buf)) {
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
            if (state->audio_fft) {
                detach_all_scene_audio(state);
                waywal_audio_fft_set_capture_active(state->audio_fft, false);
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
            waywal_audio_fft_set_capture_active(state->audio_fft, true);
            waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
            waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
        }
    } else if (state->audio_fft) {
        detach_all_scene_audio(state);
        waywal_audio_fft_set_capture_active(state->audio_fft, false);
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

static void release_output_transition_bos(daemon_state_t *state, output_node_t *out, bool free_old,
                                          bool free_new)
{
    if (!state || !out)
        return;
    if (free_old && out->has_source_old_bo) {
        layer_shell_release_output_bo_refs(state, out, &out->source_old_bo);
        dmabuf_bo_free(&out->source_old_bo);
        out->has_source_old_bo = false;
    }
    if (free_new && out->has_source_new_bo) {
        layer_shell_release_output_bo_refs(state, out, &out->source_new_bo);
        dmabuf_bo_free(&out->source_new_bo);
        out->has_source_new_bo = false;
    }
}

static void destroy_output_shader_eng(daemon_state_t *state, output_node_t *out)
{
    if (!out || !out->shader_eng)
        return;
    waywal_shader_engine_t *seng = out->shader_eng;
    out->shader_eng = NULL;
    bool shared = false;
    if (state) {
        for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
            if (o != out && o->shader_eng == seng) {
                shared = true;
                break;
            }
        }
        if (state->shader_eng == seng && !shared) {
            state->shader_eng = NULL;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o != out && o->shader_eng) {
                    state->shader_eng = o->shader_eng;
                    break;
                }
            }
        }
    }
    if (!shared) {
        if (state) {
            int ino_fd = waywal_shader_engine_get_inotify_fd(seng);
            if (ino_fd >= 0) {
                unregister_fd_from_loops(state, ino_fd);
            }
            if (state->dmabuf_ctx.available) {
                render_engine_make_current(&state->render_engine);
            }
        }
        waywal_shader_engine_destroy(seng);
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
        if (state->we_scene == ws && !shared) {
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
        if (state && state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
        }
        authoring_invalidate_scene(ws);
        we_scene_destroy(ws);
    }
}

void stop_engines_on_output(daemon_state_t *state, output_node_t *out, bool keep_shader,
                            bool keep_anim, bool keep_video, bool keep_slideshow)
{
    if (!state || !out)
        return;

    release_output_transition_bos(state, out, false, true);

    if (!keep_shader) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        if (out->has_source_old_bo) {
            if (out->shader_eng)
                waywal_shader_engine_forget_bo(out->shader_eng, &out->source_old_bo);
            if (out->we_scene)
                waywal_we_scene_forget_bo(out->we_scene, &out->source_old_bo);
        }
        destroy_output_shader_eng(state, out);
        destroy_output_we_scene(state, out);
        if (state->shader_eng) {
            bool any_attached = false;
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o->shader_eng == state->shader_eng) {
                    any_attached = true;
                    break;
                }
            }
            if (!any_attached) {
                int ino_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
                if (ino_fd >= 0) {
                    unregister_fd_from_loops(state, ino_fd);
                }
                if (state->dmabuf_ctx.available) {
                    render_engine_make_current(&state->render_engine);
                }
                waywal_shader_engine_destroy(state->shader_eng);
                state->shader_eng = NULL;
            }
        }
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
                        daemon_copy_cstr(ss->target_outputs[rem_count],
                                         sizeof(ss->target_outputs[rem_count]), o->name);
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

    layer_shell_trim_output_dmabuf_ring(state, out);
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

void shader_engine_stop_playback(daemon_state_t *state)
{
    if (!state)
        return;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        out->shader_active = false;
        out->workshop_active = false;
        out->shader_audio_reactive = false;
        release_output_transition_bos(state, out, true, true);
        destroy_output_shader_eng(state, out);
        destroy_output_we_scene(state, out);
        layer_shell_trim_output_dmabuf_ring(state, out);
    }
    if (state->shader_eng) {
        int ino_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
        if (ino_fd >= 0) {
            unregister_fd_from_loops(state, ino_fd);
        }
        if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
        }
        waywal_shader_engine_destroy(state->shader_eng);
        state->shader_eng = NULL;
    }
    if (state->we_scene) {
        if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
        }
        authoring_invalidate_scene(state->we_scene);
        we_scene_destroy(state->we_scene);
        state->we_scene = NULL;
    }
    bool was_active = state->shader_active;
    state->shader_active = false;
    state->workshop_active = false;
    disarm_timer_fd(state->shader_timer_fd);
    if (state->audio_fft) {
        detach_all_scene_audio(state);
        waywal_audio_fft_set_capture_active(state->audio_fft, false);
    }
    if (was_active) {
        layer_shell_update_input_regions(state, false);
    }
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
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

    waywal_anim_decoder_t *dec = waywal_anim_create(buf, sz);
    free(buf);
    if (!dec || waywal_anim_frame_count(dec) <= 1) {
        if (dec)
            waywal_anim_destroy(dec);
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }
    uint32_t total_frames = waywal_anim_frame_count(dec);
    const uint8_t *f_rgba = NULL;
    uint32_t fw = 0, fh = 0, delay_ms = 100;
    if (!waywal_anim_next_frame(dec, &f_rgba, &fw, &fh, &delay_ms)) {
        waywal_anim_destroy(dec);
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

    uint64_t t_now = now_ns();
    uint32_t min_delay_ms = UINT32_MAX;
    bool started_any = false;
    bool need_reset_dec = false;
    uint8_t *shared_bgra = NULL;
    bool heap_bgra = false;

    if (f_rgba && fw > 0 && fh > 0 &&
        (!meta || meta->transition_type == WAYWAL_TRANSITION_NONE ||
         meta->transition_duration_ms == 0)) {
        size_t px_bytes = (size_t)fw * (size_t)fh * 4U;
        shared_bgra = (uint8_t *)arena_alloc(&state->frame_arena, px_bytes, 32U);
        if (!shared_bgra) {
            shared_bgra = (uint8_t *)malloc(px_bytes);
            heap_bgra = (shared_bgra != NULL);
        }
        if (shared_bgra) {
            swizzle_rgba_to_bgra(shared_bgra, f_rgba, (size_t)fw * (size_t)fh);
            render_engine_bump_upload_gen(&state->render_engine);
        }
    } else {
        need_reset_dec = true;
    }

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        bool matches = (meta && meta->num_target_outputs > 0)
                           ? output_matches_img_meta(out, meta)
                           : output_matches_target_spec(out, target_out);
        if (!matches)
            continue;

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

        if (shared_bgra) {
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
            output_node_render_image(state, out, shared_bgra, fw, fh);
        }

        if (out->anim_delay_ms < min_delay_ms)
            min_delay_ms = out->anim_delay_ms;
        if (!state->anim_dec)
            state->anim_dec = dec;
        started_any = true;
    }

    if (heap_bgra)
        free(shared_bgra);

    if (!started_any) {
        waywal_anim_destroy(dec);
        stop_anim_on_matched_outputs(state, meta, target_out);
        return false;
    }

    if (need_reset_dec) {
        waywal_anim_reset(dec);
    }

    state->anim_target_output[0] = '\0';
    if (target_out && target_out[0] != '\0') {
        daemon_copy_cstr(state->anim_target_output, sizeof(state->anim_target_output), target_out);
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

    struct {
        waywal_anim_decoder_t *dec;
        uint8_t *bgra;
        uint32_t w;
        uint32_t h;
        uint32_t delay_ms;
        bool heap_bgra;
        bool ok;
    } stepped_dec[8];
    size_t num_stepped = 0;

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

        size_t slot = num_stepped;
        for (size_t k = 0; k < num_stepped; ++k) {
            if (stepped_dec[k].dec == out->anim_dec) {
                slot = k;
                break;
            }
        }

        if (slot == num_stepped && t_now + 2000000ULL < out->anim_next_ns) {
            uint32_t rem_ms = (uint32_t)((out->anim_next_ns - t_now) / 1000000ULL);
            if (rem_ms == 0)
                rem_ms = 1U;
            if (rem_ms < next_arm_ms)
                next_arm_ms = rem_ms;
            continue;
        }

        if (slot == num_stepped) {
            if (slot >= 8)
                continue;
            num_stepped++;
            stepped_dec[slot].dec = out->anim_dec;
            stepped_dec[slot].bgra = NULL;
            stepped_dec[slot].w = 0;
            stepped_dec[slot].h = 0;
            stepped_dec[slot].delay_ms = 100U;
            stepped_dec[slot].heap_bgra = false;
            stepped_dec[slot].ok = false;

            const uint8_t *rgba = NULL;
            uint32_t w = 0, h = 0, delay_ms = 100;
            if (waywal_anim_next_frame(out->anim_dec, &rgba, &w, &h, &delay_ms) && rgba && w > 0 &&
                h > 0) {
                size_t px_bytes = (size_t)w * (size_t)h * 4U;
                uint8_t *bgra = (uint8_t *)arena_alloc(&state->frame_arena, px_bytes, 32U);
                bool heap_bgra = false;
                if (!bgra) {
                    bgra = (uint8_t *)malloc(px_bytes);
                    heap_bgra = (bgra != NULL);
                }
                if (bgra) {
                    swizzle_rgba_to_bgra(bgra, rgba, (size_t)w * (size_t)h);
                    render_engine_bump_upload_gen(&state->render_engine);
                    stepped_dec[slot].bgra = bgra;
                    stepped_dec[slot].w = w;
                    stepped_dec[slot].h = h;
                    stepped_dec[slot].delay_ms = delay_ms;
                    stepped_dec[slot].heap_bgra = heap_bgra;
                    stepped_dec[slot].ok = true;
                }
            }
        }

        if (!stepped_dec[slot].ok)
            continue;

        output_node_render_image(state, out, stepped_dec[slot].bgra, stepped_dec[slot].w,
                                 stepped_dec[slot].h);

        uint32_t next_delay = stepped_dec[slot].delay_ms > 0 ? stepped_dec[slot].delay_ms : 100U;
        if (state->power_gov && waywal_power_governor_is_on_battery(state->power_gov) &&
            next_delay < 33U) {
            next_delay = 33U;
        }
        out->anim_delay_ms = next_delay;
        out->anim_next_ns = t_now + (uint64_t)next_delay * 1000000ULL;
        if (next_delay < next_arm_ms)
            next_arm_ms = next_delay;
    }

    for (size_t k = 0; k < num_stepped; ++k) {
        if (stepped_dec[k].heap_bgra)
            free(stepped_dec[k].bgra);
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
        if (upload_art && state->shader_active &&
            waywal_shader_engine_uses_album_art(state->shader_eng)) {
            waywal_shader_engine_set_album_art_bgra(state->shader_eng, art_bgra, aw, ah);
            uploaded_gl = true;
        }
    }
    if (state->we_scene) {
        we_scene_set_media_playing(state->we_scene, playing);
        if (upload_art && state->workshop_active) {
            we_scene_set_album_art_bgra(state->we_scene, art_bgra, aw, ah);
            uploaded_gl = true;
        }
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && out->shader_eng != state->shader_eng) {
            waywal_shader_engine_set_media_playing(out->shader_eng, playing);
            if (upload_art && out->shader_active &&
                waywal_shader_engine_uses_album_art(out->shader_eng)) {
                waywal_shader_engine_set_album_art_bgra(out->shader_eng, art_bgra, aw, ah);
                uploaded_gl = true;
            }
        }
        if (out->we_scene && out->we_scene != state->we_scene) {
            we_scene_set_media_playing(out->we_scene, playing);
            if (upload_art && out->workshop_active) {
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

    if (!meta || meta->transition_type == WAYWAL_TRANSITION_NONE ||
        meta->transition_duration_ms == 0) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!meta || output_matches_img_meta(out, meta)) {
                release_output_transition_bos(state, out, true, true);
            }
        }
    }

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
    if (state->shader_eng && state->shader_active &&
        waywal_shader_engine_uses_wallpaper(state->shader_eng)) {
        waywal_shader_engine_set_wallpaper_bgra(state->shader_eng, sh_px, sh_w, sh_h);
        uploaded_gl = true;
    }
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (out->shader_eng && out->shader_eng != state->shader_eng && out->shader_active &&
            waywal_shader_engine_uses_wallpaper(out->shader_eng)) {
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
    bool is_solid_zero_alpha = (meta && meta->is_solid_color && bgra_pixels[3] == 0U);
    const uint8_t *extract_px = is_solid_zero_alpha ? solid_opaque_px : sh_px;
    uint32_t extract_w = is_solid_zero_alpha ? 1U : sh_w;
    uint32_t extract_h = is_solid_zero_alpha ? 1U : sh_h;
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
            daemon_copy_cstr(extracted_pal.wallpaper_path, sizeof(extracted_pal.wallpaper_path),
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

    daemon_copy_cstr(state->shader_target_output, sizeof(state->shader_target_output),
                     (target_output && target_output[0]) ? target_output : "*");
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

    bool need_audio_this_frame = false;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!out->surface || !out->configured || out->width <= 0 || out->height <= 0)
            continue;
        if (!out->shader_active && !out->workshop_active)
            continue;
        we_scene_t *wscene = out->we_scene ? out->we_scene : state->we_scene;
        if (out->shader_audio_reactive || (out->workshop_active && wscene && wscene->uses_audio)) {
            need_audio_this_frame = true;
            break;
        }
    }
    if (state->audio_fft && need_audio_this_frame) {
        (void)waywal_audio_fft_update_ducking(state->audio_fft, global_dt);
        (void)waywal_audio_fft_process(state->audio_fft, NULL);
        base_params.audio = waywal_audio_fft_get_frame(state->audio_fft);
    }

    output_node_t *donor_outs[8];
    dmabuf_bo_t *donor_bos[8];
    float donor_norm_mx[8];
    float donor_norm_my[8];
    size_t num_donors = 0;
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
        output_node_t *cursor_src = out;
        bool have_cursor = false;
        if (needs_cursor) {
            if (daemon_query_cursor_pos(state, out, &mx, &my)) {
                have_cursor = true;
            } else if (out->workshop_active && wscene != NULL &&
                       out->scaling_mode != WAYWAL_SCALING_SPAN) {
                for (output_node_t *o2 = state->outputs; o2 != NULL; o2 = o2->next) {
                    if (o2 != out && o2->workshop_active &&
                        (o2->we_scene ? o2->we_scene : state->we_scene) == wscene &&
                        o2->width == out->width && o2->height == out->height &&
                        daemon_query_cursor_pos(state, o2, &mx, &my)) {
                        cursor_src = o2;
                        have_cursor = true;
                        break;
                    }
                }
            }
        }
        if (have_cursor) {
            double scale_x =
                (cursor_src->logical_width > 0 && cursor_src->width > 0)
                    ? ((double)cursor_src->width / (double)cursor_src->logical_width)
                    : (double)(cursor_src->scale_factor > 0 ? cursor_src->scale_factor : 1);
            double scale_y =
                (cursor_src->logical_height > 0 && cursor_src->height > 0)
                    ? ((double)cursor_src->height / (double)cursor_src->logical_height)
                    : (double)(cursor_src->scale_factor > 0 ? cursor_src->scale_factor : 1);
            target_mx = (float)(mx * scale_x) + span_dx;
            target_my = ((float)out->height - (float)(my * scale_y)) + span_dy;
            fparams.mouse[0] = target_mx;
            fparams.mouse[1] = target_my;

            float click_x = (float)(cursor_src->click_x * scale_x) + span_dx;
            float click_y = ((float)out->height - (float)(cursor_src->click_y * scale_y)) + span_dy;
            if (cursor_src->button_down) {
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

        float rscale = (!out->workshop_active && seng != NULL && out->viewport != NULL)
                           ? waywal_shader_engine_get_render_scale(seng)
                           : 1.0f;
        if (rscale < 0.25f)
            rscale = 0.25f;
        else if (rscale > 1.0f)
            rscale = 1.0f;
        int32_t rw = (rscale < 0.999f) ? (int32_t)((float)out->width * rscale + 0.5f) : out->width;
        int32_t rh =
            (rscale < 0.999f) ? (int32_t)((float)out->height * rscale + 0.5f) : out->height;
        rw &= ~1;
        rh &= ~1;
        if (rw < 64)
            rw = out->width;
        if (rh < 64)
            rh = out->height;

        if (out->dmabuf_ring.initialized &&
            (out->dmabuf_ring.width != (uint32_t)rw || out->dmabuf_ring.height != (uint32_t)rh)) {
            for (size_t bi = 0; bi < WAYWAL_DMABUF_RING_SIZE; ++bi) {
                if (seng)
                    waywal_shader_engine_forget_bo(seng, &out->dmabuf_ring.buffers[bi]);
                if (wscene)
                    waywal_we_scene_forget_bo(wscene, &out->dmabuf_ring.buffers[bi]);
            }
        }

        waywal_shader_frame_params_t render_params = fparams;
        if (rw != out->width || rh != out->height) {
            float sx = (float)rw / (float)out->width;
            float sy = (float)rh / (float)out->height;
            render_params.mouse[0] *= sx;
            render_params.mouse[1] *= sy;
            render_params.mouse[2] *= sx;
            render_params.mouse[3] *= sy;
            render_params.mouse_smooth[0] *= sx;
            render_params.mouse_smooth[1] *= sy;
            render_params.mouse_vel[0] *= sx;
            render_params.mouse_vel[1] *= sy;
            render_params.span_rect[0] *= sx;
            render_params.span_rect[1] *= sy;
            render_params.span_rect[2] *= sx;
            render_params.span_rect[3] *= sy;
        }

        float norm_mx = out->width > 0 ? (out->smooth_mouse_x / (float)out->width) : 0.5f;
        float norm_my = out->height > 0 ? (out->smooth_mouse_y / (float)out->height) : 0.5f;

        if (layer_shell_ensure_dmabuf_ring(state, out, rw, rh) && out->use_dmabuf) {
            dmabuf_bo_t *bo = dmabuf_ring_acquire(&out->dmabuf_ring);
            if (bo && bo->wl_buffer) {
                bool rendered = false;
                if (!out->transition_active && out->scaling_mode != WAYWAL_SCALING_SPAN) {
                    for (size_t d = 0; d < num_donors; ++d) {
                        output_node_t *d_out = donor_outs[d];
                        dmabuf_bo_t *d_bo = donor_bos[d];
                        if (!d_out || !d_bo || d_out->transition_active ||
                            d_out->scaling_mode == WAYWAL_SCALING_SPAN ||
                            d_bo->width != (uint32_t)rw || d_bo->height != (uint32_t)rh ||
                            out->shader_audio_reactive != d_out->shader_audio_reactive) {
                            continue;
                        }
                        if (out->workshop_active && d_out->workshop_active && wscene != NULL) {
                            we_scene_t *d_wscene =
                                d_out->we_scene ? d_out->we_scene : state->we_scene;
                            if (wscene == d_wscene) {
                                rendered = we_scene_blit_last_frame(wscene, bo);
                                if (rendered)
                                    break;
                            }
                        } else if (!out->workshop_active && !d_out->workshop_active &&
                                   seng != NULL) {
                            bool same_shader =
                                (!out->shader_eng || out->shader_eng == d_out->shader_eng ||
                                 (out->shader_path[0] != '\0' &&
                                  strcmp(out->shader_path, d_out->shader_path) == 0));
                            bool mouse_match = !waywal_shader_engine_uses_mouse(seng) ||
                                               (fabsf(norm_mx - donor_norm_mx[d]) < 1e-3f &&
                                                fabsf(norm_my - donor_norm_my[d]) < 1e-3f &&
                                                !out->button_down && !d_out->button_down);
                            if (same_shader && mouse_match) {
                                rendered = waywal_shader_engine_blit_bo(seng, d_bo, bo);
                                if (rendered)
                                    break;
                            }
                        }
                    }
                }
                if (!rendered) {
                    if (out->workshop_active && wscene) {
                        rendered = we_scene_render_frame(wscene, bo, &render_params);
                    } else if (seng) {
                        rendered = waywal_shader_engine_render_frame(seng, bo, &render_params);
                    }
                    if (rendered && num_donors < 8 && !out->transition_active &&
                        out->scaling_mode != WAYWAL_SCALING_SPAN) {
                        donor_outs[num_donors] = out;
                        donor_bos[num_donors] = bo;
                        donor_norm_mx[num_donors] = norm_mx;
                        donor_norm_my[num_donors] = norm_my;
                        num_donors++;
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
                    wl_surface_damage_buffer(out->surface, 0, 0, rw, rh);
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

typedef struct {
    uint32_t global_name;
    char name[64];
    bool in_use;
    bool had_fullscreen_unpresented;
} fullscreen_remap_slot_t;

static fullscreen_remap_slot_t g_fs_remap_slots[32];

static fullscreen_remap_slot_t *find_or_alloc_fs_remap_slot(const output_node_t *out)
{
    if (!out)
        return NULL;
    size_t free_idx = sizeof(g_fs_remap_slots) / sizeof(g_fs_remap_slots[0]);
    for (size_t i = 0; i < sizeof(g_fs_remap_slots) / sizeof(g_fs_remap_slots[0]); ++i) {
        if (g_fs_remap_slots[i].in_use) {
            if ((out->global_name != 0 && g_fs_remap_slots[i].global_name == out->global_name) ||
                (out->name[0] != '\0' && strcmp(g_fs_remap_slots[i].name, out->name) == 0)) {
                g_fs_remap_slots[i].global_name = out->global_name;
                if (out->name[0] != '\0') {
                    snprintf(g_fs_remap_slots[i].name, sizeof(g_fs_remap_slots[i].name), "%s",
                             out->name);
                }
                return &g_fs_remap_slots[i];
            }
        } else if (free_idx == sizeof(g_fs_remap_slots) / sizeof(g_fs_remap_slots[0])) {
            free_idx = i;
        }
    }
    if (free_idx >= sizeof(g_fs_remap_slots) / sizeof(g_fs_remap_slots[0]))
        return NULL;
    fullscreen_remap_slot_t *slot = &g_fs_remap_slots[free_idx];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->global_name = out->global_name;
    if (out->name[0] != '\0') {
        snprintf(slot->name, sizeof(slot->name), "%s", out->name);
    }
    return slot;
}

static void record_fullscreen_unpresented_state(const daemon_state_t *state)
{
    if (!state || !state->power_gov)
        return;
    for (const output_node_t *out = state->outputs; out != NULL; out = out->next) {
        fullscreen_remap_slot_t *slot = find_or_alloc_fs_remap_slot(out);
        if (!slot)
            continue;
        if (!out->configured || out->pres_sync.pll.frame_seq > 0) {
            slot->had_fullscreen_unpresented = false;
            continue;
        }
        uint32_t mask = waywal_power_governor_get_output_mask(state->power_gov, out->name);
        if ((mask & WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN) != 0 || out->wlr_has_fullscreen) {
            slot->had_fullscreen_unpresented = true;
        }
    }
}

/* Only recreate a layer surface when leaving a fullscreen-occluded workspace where the compositor
 * never presented the initial commit, preventing repeated surface churn on normal tiled workspaces
 * where static or occluded surfaces keep frame_seq == 0. */
static bool consume_fullscreen_remap_needed(const daemon_state_t *state, const output_node_t *out)
{
    if (!state || !state->power_gov || !out || !out->configured || !out->pres_sync.wp_pres ||
        out->pres_sync.pll.frame_seq != 0 || out->pres_sync.pending_feedback) {
        return false;
    }
    fullscreen_remap_slot_t *slot = find_or_alloc_fs_remap_slot(out);
    if (!slot || !slot->had_fullscreen_unpresented)
        return false;

    uint32_t mask = waywal_power_governor_get_output_mask(state->power_gov, out->name);
    if ((mask & (WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN | WAYWAL_PAUSE_DPMS_OR_UNMAPPED |
                 WAYWAL_PAUSE_SESSION_IDLE)) != 0) {
        return false;
    }
    slot->had_fullscreen_unpresented = false;
    return true;
}

void daemon_reconcile_power_state(daemon_state_t *state)
{
    if (!state)
        return;

    record_fullscreen_unpresented_state(state);

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

    daemon_copy_cstr(props_buf, cap, new_buf);
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
            authoring_invalidate_scene(state->we_scene);
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
        uint32_t hint_fps =
            state->shader_eng ? waywal_shader_engine_get_target_fps(state->shader_eng) : 60U;
        state->shader_fps = spay->fps > 0 ? spay->fps : (hint_fps > 0 ? hint_fps : 60U);
        daemon_copy_cstr(state->shader_target_output, sizeof(state->shader_target_output),
                         spay->output[0] ? spay->output : "*");
        daemon_copy_cstr(state->shader_path, sizeof(state->shader_path), shader_file);
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
        } else {
            release_output_transition_bos(state, out, true, true);
        }

        waywal_shader_engine_t *shared_donor_eng = NULL;
        if (state->dmabuf_ctx.available) {
            for (output_node_t *prev = state->outputs; prev != NULL && prev != out;
                 prev = prev->next) {
                if (prev->shader_active && prev->shader_eng && prev->width == out->width &&
                    prev->height == out->height && strcmp(prev->shader_path, shader_file) == 0) {
                    shared_donor_eng = prev->shader_eng;
                    break;
                }
            }
        }

        if (shared_donor_eng) {
            if (out->shader_eng && out->shader_eng != shared_donor_eng) {
                destroy_output_shader_eng(state, out);
            }
            out->shader_eng = shared_donor_eng;
        } else if (!out->shader_eng && state->dmabuf_ctx.available) {
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
        if (shared_donor_eng) {
            loaded = true;
        } else if (out->shader_eng) {
            first_compile_attempted = true;
            if (waywal_shader_engine_load_file(out->shader_eng, shader_file)) {
                loaded = true;
                if (state->wallpaper_rgba && state->wallpaper_w > 0 && state->wallpaper_h > 0 &&
                    waywal_shader_engine_uses_wallpaper(out->shader_eng)) {
                    waywal_shader_engine_set_wallpaper_bgra(out->shader_eng, state->wallpaper_rgba,
                                                            state->wallpaper_w, state->wallpaper_h);
                }
                if (state->mpris) {
                    const waywal_mpris_state_t *ms = waywal_mpris_get_state(state->mpris);
                    if (ms) {
                        waywal_shader_engine_set_media_playing(
                            out->shader_eng, (ms->status == WAYWAL_MPRIS_PLAYING) ? 1.0f : 0.0f);
                        if (ms->has_art_pixels && ms->art_bgra &&
                            waywal_shader_engine_uses_album_art(out->shader_eng)) {
                            waywal_shader_engine_set_album_art_bgra(out->shader_eng, ms->art_bgra,
                                                                    ms->art_width, ms->art_height);
                        }
                    }
                }
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

        uint32_t hint_fps =
            out->shader_eng ? waywal_shader_engine_get_target_fps(out->shader_eng) : 60U;
        out->shader_active = true;
        out->workshop_active = false;
        out->shader_audio_reactive = spay->audio_reactive;
        out->shader_fps = spay->fps > 0 ? spay->fps : (hint_fps > 0 ? hint_fps : 60U);
        out->shader_frame_idx = 0;
        out->shader_start_ns = t_now;
        out->shader_last_ns = t_now;
        out->scaling_mode = spay->scaling_mode;
        out->displaying_color = false;
        daemon_copy_cstr(out->active_media_path, sizeof(out->active_media_path), shader_file);
        daemon_copy_cstr(out->shader_path, sizeof(out->shader_path), shader_file);

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

    daemon_copy_cstr(state->shader_target_output, sizeof(state->shader_target_output),
                     spay->output[0] ? spay->output : "*");
    daemon_copy_cstr(state->shader_path, sizeof(state->shader_path), shader_file);
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
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
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
    daemon_copy_cstr(meta->filepath, sizeof(meta->filepath), filepath);
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
                } else {
                    release_output_transition_bos(state, out, true, true);
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
            daemon_copy_cstr(proj.type, sizeof(proj.type), "scene");
            daemon_copy_cstr(proj.file, sizeof(proj.file), "scene.json");
        } else {
            return false;
        }
    }

    waywal_img_metadata_t ws_meta;
    memset(&ws_meta, 0, sizeof(ws_meta));
    daemon_copy_cstr(ws_meta.filepath, sizeof(ws_meta.filepath), ws_dir);
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
            daemon_copy_cstr(ws_meta.filepath, sizeof(ws_meta.filepath), prev_path);
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
            authoring_invalidate_scene(state->we_scene);
            we_scene_destroy(state->we_scene);
            state->we_scene = NULL;
        }
        if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            state->we_scene = we_scene_create(&state->dmabuf_ctx, ws_dir, wpay->prop_overrides);
        } else {
            state->we_scene = we_scene_load_cpu(ws_dir, wpay->prop_overrides);
        }
        if (state->we_scene) {
            static uint64_t s_authoring_scene_seq = 1u;
            if (state->we_scene->authoring_revision == 0u) {
                state->we_scene->authoring_revision = s_authoring_scene_seq++;
            }
            state->we_scene->on_destroy_cb = authoring_invalidate_scene;
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
            for (output_node_t *o = state->outputs; o != NULL; o = o->next) {
                if (o->shader_eng == state->shader_eng) {
                    o->shader_eng = NULL;
                }
            }
            int ino_fd = waywal_shader_engine_get_inotify_fd(state->shader_eng);
            if (ino_fd >= 0) {
                unregister_fd_from_loops(state, ino_fd);
            }
            if (state->dmabuf_ctx.available) {
                render_engine_make_current(&state->render_engine);
            }
            waywal_shader_engine_destroy(state->shader_eng);
            state->shader_eng = NULL;
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
                waywal_audio_fft_set_capture_active(state->audio_fft, true);
                waywal_audio_fft_set_volume(state->audio_fft, state->audio_volume);
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, state->mute_on_audio);
            }
        } else if (state->audio_fft) {
            detach_all_scene_audio(state);
            waywal_audio_fft_set_capture_active(state->audio_fft, false);
        }
        we_scene_set_audio_output(state->we_scene,
                                  state->we_scene->uses_audio ? state->audio_fft : NULL);
        state->shader_active = true;
        state->workshop_active = true;
        state->shader_fps = wpay->fps > 0 ? wpay->fps : 60U;
        daemon_copy_cstr(state->shader_target_output, sizeof(state->shader_target_output),
                         wpay->output[0] ? wpay->output : "*");
        daemon_copy_cstr(state->workshop_dir, sizeof(state->workshop_dir), ws_dir);
        daemon_copy_cstr(state->workshop_props, sizeof(state->workshop_props),
                         wpay->prop_overrides);
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
        } else {
            release_output_transition_bos(state, out, true, true);
        }

        destroy_output_we_scene(state, out);

        we_scene_t *shared_donor_scene = NULL;
        for (output_node_t *prev = state->outputs; prev != NULL && prev != out; prev = prev->next) {
            if (prev->workshop_active && prev->we_scene && prev->width == out->width &&
                prev->height == out->height && strcmp(prev->workshop_dir, ws_dir) == 0 &&
                strcmp(prev->workshop_props, wpay->prop_overrides) == 0) {
                shared_donor_scene = prev->we_scene;
                break;
            }
        }

        if (shared_donor_scene) {
            out->we_scene = shared_donor_scene;
        } else if (state->dmabuf_ctx.available) {
            render_engine_make_current(&state->render_engine);
            out->we_scene = we_scene_create(&state->dmabuf_ctx, ws_dir, wpay->prop_overrides);
        } else {
            out->we_scene = we_scene_load_cpu(ws_dir, wpay->prop_overrides);
        }
        if (out->we_scene) {
            static uint64_t s_out_scene_seq = 1000000u;
            if (out->we_scene->authoring_revision == 0u) {
                out->we_scene->authoring_revision = s_out_scene_seq++;
            }
            out->we_scene->on_destroy_cb = authoring_invalidate_scene;
        }
        if (!state->we_scene && out->we_scene) {
            state->we_scene = out->we_scene;
        }

        if (!out->we_scene) {
            out->workshop_active = false;
            continue;
        }

        destroy_output_shader_eng(state, out);
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
        daemon_copy_cstr(out->active_media_path, sizeof(out->active_media_path), ws_dir);
        daemon_copy_cstr(out->workshop_dir, sizeof(out->workshop_dir), ws_dir);
        daemon_copy_cstr(out->workshop_props, sizeof(out->workshop_props), wpay->prop_overrides);

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

    daemon_copy_cstr(state->shader_target_output, sizeof(state->shader_target_output),
                     wpay->output[0] ? wpay->output : "*");
    daemon_copy_cstr(state->workshop_dir, sizeof(state->workshop_dir), ws_dir);
    daemon_copy_cstr(state->workshop_props, sizeof(state->workshop_props), wpay->prop_overrides);
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
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
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
        if (saved->backdrop[0] != '\0') {
            state->backdrop_auto_blur = (strcasecmp(saved->backdrop, "auto-blur") == 0);
            if (strcasecmp(saved->backdrop, "none") == 0) {
                state->backdrop_path[0] = '\0';
            } else if (!state->backdrop_auto_blur) {
                daemon_copy_cstr(state->backdrop_path, sizeof(state->backdrop_path),
                                 saved->backdrop);
            }
        }
        if (saved->kind == WAYWAL_CONTENT_STATIC_IMAGE && saved->depth_map_path[0] != '\0' &&
            state->dmabuf_ctx.available) {
            uint8_t *px = NULL;
            uint32_t w = 0, h = 0;
            if (image_load(saved->media_path, &px, &w, &h) && px && w > 0 && h > 0) {
                waywal_img_metadata_t meta;
                memset(&meta, 0, sizeof(meta));
                meta.width = w;
                meta.height = h;
                meta.scaling_mode = (uint32_t)saved->scaling;
                meta.filter_mode = (uint32_t)saved->filter;
                meta.ten_bit = saved->ten_bit;
                meta.crop_gravity_x = saved->has_crop_gravity ? saved->crop_gravity_x : 0.5f;
                meta.crop_gravity_y = saved->has_crop_gravity ? saved->crop_gravity_y : 0.5f;
                meta.has_crop_gravity = saved->has_crop_gravity ? 1 : 0;
                daemon_copy_cstr(meta.filepath, sizeof(meta.filepath), saved->media_path);
                daemon_copy_cstr(meta.depth_map_path, sizeof(meta.depth_map_path),
                                 saved->depth_map_path);
                meta.depth_strength = saved->depth_strength > 0.0f ? saved->depth_strength : 1.0f;
                daemon_copy_cstr(meta.backdrop, sizeof(meta.backdrop), saved->backdrop);

                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (output_matches_target_spec(out, target)) {
                        stop_engines_on_output(state, out, false, false, false, false);
                        out->scaling_mode = meta.scaling_mode;
                        out->filter_mode = meta.filter_mode;
                        out->ten_bit = meta.ten_bit;
                        out->crop_gravity_x = meta.crop_gravity_x;
                        out->crop_gravity_y = meta.crop_gravity_y;
                        out->has_crop_gravity = (meta.has_crop_gravity != 0);
                    }
                }
                bool ok = daemon_activate_depth_parallax(state, px, w, h, &meta, target);
                image_free(px);
                if (ok) {
                    return true;
                }
            }
        }
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
            daemon_copy_cstr(spay.target_outputs[0], sizeof(spay.target_outputs[0]), target);
        }
        daemon_copy_cstr(spay.path, sizeof(spay.path), saved->media_path);
        return daemon_start_slideshow_isolated(state, &spay);
    }
    case WAYWAL_CONTENT_VIDEO: {
        if (saved->media_path[0] == '\0')
            return false;
        if (isfinite(saved->volume) && saved->volume >= 0.0f) {
            float clamped = saved->volume > 1.0f ? 1.0f : saved->volume;
            state->audio_volume = clamped;
            if (state->audio_fft) {
                waywal_audio_fft_set_volume(state->audio_fft, clamped);
            }
        }
        if (saved->mute_on_audio) {
            state->mute_on_audio = true;
            if (state->audio_fft) {
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, true);
            }
        }
        bool ok = daemon_start_video_file_isolated(state, saved->media_path, target, saved->scaling,
                                                   NULL, 0, 0, false);
        if (ok) {
            float eff_speed = saved->video_speed > 0.01f ? saved->video_speed : 1.0f;
            state->video_engine.playback_speed = eff_speed;
            state->video_engine.loop_count = saved->loop_count;
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (output_matches_target_spec(out, target) && out->video_eng) {
                    out->video_eng->playback_speed = eff_speed;
                    out->video_eng->loop_count = saved->loop_count;
                }
            }
        }
        return ok;
    }
    case WAYWAL_CONTENT_SHADER: {
        if (saved->media_path[0] == '\0')
            return false;
        if (isfinite(saved->volume) && saved->volume >= 0.0f) {
            float clamped = saved->volume > 1.0f ? 1.0f : saved->volume;
            state->audio_volume = clamped;
            if (state->audio_fft) {
                waywal_audio_fft_set_volume(state->audio_fft, clamped);
            }
        }
        if (saved->mute_on_audio) {
            state->mute_on_audio = true;
            if (state->audio_fft) {
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, true);
            }
        }
        if (saved->backdrop[0] != '\0') {
            state->backdrop_auto_blur = (strcasecmp(saved->backdrop, "auto-blur") == 0);
            if (strcasecmp(saved->backdrop, "none") == 0) {
                state->backdrop_path[0] = '\0';
            } else if (!state->backdrop_auto_blur) {
                daemon_copy_cstr(state->backdrop_path, sizeof(state->backdrop_path),
                                 saved->backdrop);
            }
        }
        waywal_shader_payload_t spay;
        memset(&spay, 0, sizeof(spay));
        daemon_copy_cstr(spay.output, sizeof(spay.output), target);
        daemon_copy_cstr(spay.shader_path, sizeof(spay.shader_path), saved->media_path);
        spay.fps = saved->target_fps > 0 ? saved->target_fps : 60;
        spay.audio_reactive = saved->audio_reactive;
        spay.scaling_mode = (uint32_t)saved->scaling;
        spay.invert_y = state->config.transition.invert_y ? 1 : 0;
        spay.volume = saved->volume;
        spay.mute_on_audio = saved->mute_on_audio;
        spay.speed = saved->video_speed > 0.01f ? saved->video_speed : 1.0f;
        daemon_copy_cstr(spay.backdrop, sizeof(spay.backdrop), saved->backdrop);
        return daemon_activate_shader(state, &spay);
    }
    case WAYWAL_CONTENT_WORKSHOP: {
        if (saved->media_path[0] == '\0')
            return false;
        if (isfinite(saved->volume) && saved->volume >= 0.0f) {
            float clamped = saved->volume > 1.0f ? 1.0f : saved->volume;
            state->audio_volume = clamped;
            if (state->audio_fft) {
                waywal_audio_fft_set_volume(state->audio_fft, clamped);
            }
        }
        if (saved->mute_on_audio) {
            state->mute_on_audio = true;
            if (state->audio_fft) {
                waywal_audio_fft_set_mute_on_audio(state->audio_fft, true);
            }
        }
        if (saved->backdrop[0] != '\0') {
            state->backdrop_auto_blur = (strcasecmp(saved->backdrop, "auto-blur") == 0);
            if (strcasecmp(saved->backdrop, "none") == 0) {
                state->backdrop_path[0] = '\0';
            } else if (!state->backdrop_auto_blur) {
                daemon_copy_cstr(state->backdrop_path, sizeof(state->backdrop_path),
                                 saved->backdrop);
            }
        }
        waywal_workshop_payload_t wpay;
        memset(&wpay, 0, sizeof(wpay));
        daemon_copy_cstr(wpay.output, sizeof(wpay.output), target);
        daemon_copy_cstr(wpay.workshop_dir, sizeof(wpay.workshop_dir), saved->media_path);
        daemon_copy_cstr(wpay.prop_overrides, sizeof(wpay.prop_overrides), saved->prop_overrides);
        wpay.fps = saved->target_fps > 0 ? saved->target_fps : 60;
        wpay.audio_reactive = saved->audio_reactive;
        wpay.scaling_mode = (uint32_t)saved->scaling;
        wpay.invert_y = state->config.transition.invert_y ? 1 : 0;
        wpay.volume = saved->volume;
        wpay.mute_on_audio = saved->mute_on_audio;
        daemon_copy_cstr(wpay.backdrop, sizeof(wpay.backdrop), saved->backdrop);
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
            release_output_transition_bos(state, out, true, true);
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
                release_output_transition_bos(state, out, true, true);
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
                } else {
                    release_output_transition_bos(state, out, true, true);
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
        daemon_copy_cstr(target_spec, sizeof(target_spec), "*");
    } else {
        size_t off = 0;
        for (uint8_t i = 0; i < ss->num_target_outputs; ++i) {
            if (ss->target_outputs[i][0] == '\0')
                continue;
            buf_appendf(target_spec, sizeof(target_spec), &off, "%s%s", off > 0 ? "," : "",
                        ss->target_outputs[i]);
        }
        if (target_spec[0] == '\0') {
            daemon_copy_cstr(target_spec, sizeof(target_spec), "*");
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
        daemon_copy_cstr(wpay.output, sizeof(wpay.output), target_spec);
        daemon_copy_cstr(wpay.workshop_dir, sizeof(wpay.workshop_dir), filepath);
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
            daemon_copy_cstr(spay.output, sizeof(spay.output), target_spec);
            daemon_copy_cstr(spay.shader_path, sizeof(spay.shader_path), filepath);
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
        daemon_copy_cstr(spay.output, sizeof(spay.output), target);
        daemon_copy_cstr(spay.shader_path, sizeof(spay.shader_path), rule->path);
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
        daemon_copy_cstr(wpay.output, sizeof(wpay.output), target);
        daemon_copy_cstr(wpay.workshop_dir, sizeof(wpay.workshop_dir), rule->path);
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
            daemon_copy_cstr(spay.target_outputs[0], sizeof(spay.target_outputs[0]), target);
        }
        daemon_copy_cstr(spay.path, sizeof(spay.path), rule->path);
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
            daemon_copy_cstr(scoped.output_name, sizeof(scoped.output_name), out->name);
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
        daemon_copy_cstr(spay.output, sizeof(spay.output), target);
        daemon_copy_cstr(spay.shader_path, sizeof(spay.shader_path), wrule->path);
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
        daemon_copy_cstr(wpay.output, sizeof(wpay.output), target);
        daemon_copy_cstr(wpay.workshop_dir, sizeof(wpay.workshop_dir), wrule->path);
        daemon_copy_cstr(wpay.prop_overrides, sizeof(wpay.prop_overrides), wrule->prop_overrides);
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
            daemon_copy_cstr(spay.target_outputs[0], sizeof(spay.target_outputs[0]), target);
        }
        daemon_copy_cstr(spay.path, sizeof(spay.path), wrule->path);
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
        record_fullscreen_unpresented_state(state);
        waywal_power_governor_dispatch_ipc(state->power_gov);
        int new_ipc_fd = waywal_power_governor_get_ipc_fd(state->power_gov);
        bool reconnected = waywal_power_governor_consume_ipc_reconnected(state->power_gov);
        if (new_ipc_fd != ipc_fd || reconnected) {
            if (ipc_fd >= 0) {
                unregister_fd_from_loops(state, ipc_fd);
            }
            if (new_ipc_fd >= 0) {
                register_fd_in_loops(state, new_ipc_fd, URING_EV_POWER_IPC);
            }
        }
        char ws[64];
        char mon[64];
        while (waywal_power_governor_pop_workspace_change(state->power_gov, ws, sizeof(ws), mon,
                                                          sizeof(mon))) {
            if (mon[0] != '\0') {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (strcmp(out->name, mon) == 0 &&
                        consume_fullscreen_remap_needed(state, out)) {
                        output_node_recreate_layer_surface(state, out);
                    }
                }
            }
            if (ws[0] != '\0' && strcmp(state->last_workspace_name, ws) == 0) {
                continue;
            }
            daemon_copy_cstr(state->last_workspace_name, sizeof(state->last_workspace_name), ws);
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
                        daemon_copy_cstr(scoped.output_name, sizeof(scoped.output_name), out->name);
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
    case URING_EV_IPC_READ:
        if (fd == daemon_ipc_async_get_fd()) {
            if ((res & POLLIN) != 0) {
                daemon_ipc_async_dispatch(st);
                if (st->display) {
                    wl_display_flush(st->display);
                }
            }
        } else if (daemon_ipc_dispatch_subscriber_fd(st, fd)) {
            if (st->display) {
                wl_display_flush(st->display);
            }
        }
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
    case URING_EV_MPRIS:
        if (st->mpris && fd == waywal_mpris_get_fd(st->mpris)) {
            if ((res & (POLLERR | POLLHUP)) != 0 || !waywal_mpris_dispatch(st->mpris)) {
                unregister_fd_from_loops(st, fd);
            }
        }
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

/* ========================================================================= */
/* Shader Graph, 3D Camera, Physics & Scene Lighting IPC Dispatch Handlers   */
/* ========================================================================= */

#define WAYWAL_MAX_GRAPH_JSON_FILE_BYTES (16u * 1024u * 1024u)
#define WAYWAL_MAX_PHASE3_DRAIN_BYTES    (64ULL * 1024ULL * 1024ULL)

static bool output_matches_phase3_target(const output_node_t *out, const char *target_spec)
{
    if (!out) {
        return false;
    }
    if (!target_spec || target_spec[0] == '\0' || strcmp(target_spec, "*") == 0 ||
        strcasecmp(target_spec, "all") == 0) {
        return true;
    }
    return output_matches_target_spec(out, target_spec);
}

static bool target_spec_is_wildcard(const char *target_spec)
{
    return !target_spec || target_spec[0] == '\0' || strcmp(target_spec, "*") == 0 ||
           strcasecmp(target_spec, "all") == 0;
}

static bool scene_already_visited(we_scene_t *const *visited, size_t count, const we_scene_t *sc)
{
    for (size_t i = 0; i < count; ++i) {
        if (visited[i] == sc) {
            return true;
        }
    }
    return false;
}

static void upload_graph_constants_to_shader_eng(waywal_shader_engine_t *eng,
                                                 const waywal_shader_graph_t *graph,
                                                 const waywal_graph_schedule_t *schedule)
{
    if (!eng || !graph || !schedule) {
        return;
    }
    for (uint32_t k = 0; k < schedule->sorted_count; ++k) {
        uint32_t idx = schedule->sorted_node_indices[k];
        if (idx >= graph->node_count) {
            continue;
        }
        const waywal_graph_node_t *node = &graph->nodes[idx];
        if (node->opcode == WAYWAL_NODE_IN_CONSTANT || node->opcode == WAYWAL_NODE_IN_USER_PROP) {
            float vals[4];
            for (int c = 0; c < 4; ++c) {
                vals[c] = isfinite(node->const_val[c]) ? node->const_val[c] : 0.0f;
            }
            char uname[48];
            snprintf(uname, sizeof(uname), "u_Node%u_Const", node->node_id);
            (void)waywal_shader_engine_set_custom_uniform(eng, uname, vals, 4u);
        } else if (node->opcode == WAYWAL_NODE_NOISE_CURL2D) {
            float scale = isfinite(node->const_val[0]) ? node->const_val[0] : 1.0f;
            char uname[48];
            snprintf(uname, sizeof(uname), "u_Node%u_Scale", node->node_id);
            (void)waywal_shader_engine_set_custom_uniform(eng, uname, &scale, 1u);
        }
    }
}

static void apply_ipc_camera_to_camera3d(we_camera3d_t *cam,
                                         const waywal_ipc_req_set_camera_3d_t *r, double time_sec,
                                         float vp_w, float vp_h, float out_vp[16])
{
    if (!cam || !r) {
        return;
    }
    float safe_w = vp_w > 1.0f ? vp_w : 1920.0f;
    float safe_h = vp_h > 1.0f ? vp_h : 1080.0f;
    float aspect = safe_w / safe_h;

    if ((r->flags & WAYWAL_CAMERA_FLAG_RESET) != 0u) {
        we_camera3d_init_default(cam, safe_w, safe_h);
    } else {
        cam->projection_mode = r->projection_mode;
        cam->path_enabled = (r->flags & WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES) ? 1u : 0u;
        cam->dof_enabled = (r->flags & WAYWAL_CAMERA_FLAG_ENABLE_DOF) ? 1.0f : 0.0f;
        memcpy(cam->eye_pos, r->eye_pos, sizeof(cam->eye_pos));
        memcpy(cam->target_pos, r->target_pos, sizeof(cam->target_pos));
        memcpy(cam->up_vec, r->up_vec, sizeof(cam->up_vec));
        if (r->fov_y_deg > 0.0f) {
            cam->fov_y_deg = r->fov_y_deg;
        }
        if (r->near_z > 0.0f) {
            cam->near_z = r->near_z;
        }
        if (r->far_z > cam->near_z) {
            cam->far_z = r->far_z;
        }
        cam->parallax_amount[0] = r->parallax_amount[0];
        cam->parallax_amount[1] = r->parallax_amount[1];
        if (r->parallax_smoothing > 0.0f) {
            cam->parallax_smoothing = r->parallax_smoothing;
        }
        if (r->dof_focal_dist > 0.0f) {
            cam->dof_focal_dist = r->dof_focal_dist;
        }
        if (r->dof_focal_region > 0.0f) {
            cam->dof_focal_region = r->dof_focal_region;
        }
        if (r->dof_bokeh_radius_px > 0.0f) {
            cam->dof_bokeh_radius_px = r->dof_bokeh_radius_px;
        }
    }

    we_camera3d_update(cam, time_sec, 0.0f, 0.0f, aspect);
    if (out_vp) {
        memcpy(out_vp, cam->view_proj_matrix, sizeof(cam->view_proj_matrix));
    }
}

static void apply_ipc_camera_to_scene(we_scene_t *sc, const waywal_ipc_req_set_camera_3d_t *r,
                                      float vp_w, float vp_h, float out_vp[16])
{
    if (!sc || !r) {
        return;
    }
    apply_ipc_camera_to_camera3d(&sc->camera, r, sc->sim_time, vp_w, vp_h, out_vp);
    sc->is_3d = (sc->camera.projection_mode != WE_PROJ_ORTHOGRAPHIC_2D);
    memcpy(sc->camera_eye, sc->camera.eye_pos, sizeof(sc->camera_eye));
    memcpy(sc->camera_center, sc->camera.target_pos, sizeof(sc->camera_center));
    memcpy(sc->camera_up, sc->camera.up_vec, sizeof(sc->camera_up));
    sc->camera_parallax =
        (sc->camera.parallax_amount[0] != 0.0f || sc->camera.parallax_amount[1] != 0.0f ||
         sc->camera.projection_mode == WE_PROJ_DEPTH_PARALLAX_25D);
    sc->camera_parallax_amount = sc->camera.parallax_amount[0];
}

static void apply_bone_cfg_preserving_rest(we_bone_sim_config_t *dst,
                                           const we_bone_sim_config_t *src, uint32_t bone_idx)
{
    float prev_rest = dst->rest_length_px;
    *dst = *src;
    dst->bone_index = bone_idx;
    if (!(src->rest_length_px > 0.0f) && prev_rest > 0.0f) {
        dst->rest_length_px = prev_rest;
    }
}

static uint32_t apply_ipc_physics_to_scene(we_scene_t *sc,
                                           const waywal_ipc_req_set_physics_params_t *r)
{
    if (!sc || !r) {
        return 0;
    }
    uint32_t sim_bones = 0;
    for (uint32_t i = 0; i < sc->object_count; ++i) {
        we_scene_object_t *obj = &sc->objects[i];
        if (!obj->puppet) {
            continue;
        }
        if (r->object_id >= 0 && obj->id != r->object_id) {
            continue;
        }
        if (!obj->puppet->physics_ctx && obj->puppet->bone_count > 0 &&
            (r->bone_cfg.flags & WE_BONE_PHYS_ENABLED) != 0u) {
            obj->puppet->physics_ctx = calloc(1, sizeof(we_puppet_physics_ctx_t));
            if (obj->puppet->physics_ctx) {
                we_physics_init_from_puppet(obj->puppet->physics_ctx, obj->puppet);
            }
        }
        we_puppet_physics_ctx_t *pctx = obj->puppet->physics_ctx;
        if (!pctx) {
            continue;
        }
        pctx->wind = r->wind;
        pctx->cursor = r->cursor;

        uint32_t bone_limit = obj->puppet->bone_count <= WE_MDL_MAX_BONES ? obj->puppet->bone_count
                                                                          : WE_MDL_MAX_BONES;
        if (r->bone_index >= 0 && (uint32_t)r->bone_index < WE_MDL_MAX_BONES) {
            apply_bone_cfg_preserving_rest(&pctx->configs[r->bone_index], &r->bone_cfg,
                                           (uint32_t)r->bone_index);
        } else if (r->bone_index < 0) {
            for (uint32_t b = 0; b < bone_limit; ++b) {
                apply_bone_cfg_preserving_rest(&pctx->configs[b], &r->bone_cfg, b);
            }
        }

        uint32_t active = 0;
        for (uint32_t b = 0; b < bone_limit; ++b) {
            if ((pctx->configs[b].flags & WE_BONE_PHYS_ENABLED) != 0u) {
                active++;
            }
        }
        pctx->active_bone_count = active;
        sim_bones += active;
        if ((r->bone_cfg.flags & WE_BONE_PHYS_ENABLED) != 0u &&
            r->bone_cfg.audio_drag_coeff > 0.0f) {
            sc->uses_audio = true;
        }
    }
    return sim_bones;
}

bool daemon_dispatch_ipc_req_compile_shader_graph(daemon_state_t *state,
                                                  const waywal_ipc_req_compile_shader_graph_t *req,
                                                  const char *inline_json,
                                                  waywal_ipc_resp_compile_shader_graph_t *resp)
{
    if (!req || !resp) {
        return false;
    }

    uint64_t t_start = now_ns();
    waywal_ipc_resp_compile_shader_graph_t resp_local;
    memset(&resp_local, 0, sizeof(resp_local));

    waywal_ipc_req_compile_shader_graph_t req_local;
    memcpy(&req_local, req, sizeof(req_local));

    if (!memchr(req_local.target_output, '\0', sizeof(req_local.target_output)) ||
        !memchr(req_local.graph_json_path, '\0', sizeof(req_local.graph_json_path)) ||
        req_local.inline_json_len > WAYWAL_MAX_GRAPH_JSON_FILE_BYTES) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                 "Malformed shader graph compile request");
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    char *file_json = NULL;
    const char *json_data = inline_json;
    size_t json_len = req_local.inline_json_len;

    if (req_local.graph_json_path[0] != '\0') {
        int fd = open(req_local.graph_json_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (fd >= 0) {
                close(fd);
            }
            resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                     "Cannot open regular graph JSON file at path: %.200s",
                     req_local.graph_json_path);
            resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
            memcpy(resp, &resp_local, sizeof(resp_local));
            return true;
        }
        if (st.st_size <= 0 || (uint64_t)st.st_size > WAYWAL_MAX_GRAPH_JSON_FILE_BYTES) {
            close(fd);
            resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                     "Invalid graph JSON file size (%lld bytes)", (long long)st.st_size);
            resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
            memcpy(resp, &resp_local, sizeof(resp_local));
            return true;
        }
        size_t sz = (size_t)st.st_size;
        file_json = (char *)malloc(sz + 1u);
        if (!file_json) {
            close(fd);
            resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                     "Out of memory reading graph JSON");
            resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
            memcpy(resp, &resp_local, sizeof(resp_local));
            return true;
        }
        size_t total_rd = 0;
        while (total_rd < sz) {
            ssize_t r = read(fd, file_json + total_rd, sz - total_rd);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (r == 0) {
                break;
            }
            total_rd += (size_t)r;
        }
        close(fd);
        if (total_rd != sz) {
            free(file_json);
            resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                     "Failed to read graph JSON from: %.200s", req_local.graph_json_path);
            resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
            memcpy(resp, &resp_local, sizeof(resp_local));
            return true;
        }
        file_json[sz] = '\0';
        json_data = file_json;
        json_len = sz;
    } else if (!json_data || json_len == 0) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                 "No graph_json_path or inline_json provided in request");
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    if (!graph) {
        free(file_json);
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                 "Out of memory allocating shader graph");
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    char parse_err[256] = {0};
    if (!waywal_shader_graph_load_json(json_data, json_len, graph, parse_err)) {
        free(file_json);
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg), "Graph JSON syntax error: %s",
                 parse_err);
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        waywal_shader_graph_destroy(graph);
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    free(file_json);

    waywal_graph_schedule_t schedule;
    memset(&schedule, 0, sizeof(schedule));
    if (!waywal_shader_graph_analyze(graph, &schedule)) {
        resp_local.status_code =
            schedule.has_cycle_error ? WAYWAL_STATUS_ERR_CYCLE : WAYWAL_STATUS_ERR_SYNTAX;
        resp_local.offending_node_id = schedule.cycle_node_id;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg), "%s", schedule.error_msg);
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        waywal_shader_graph_destroy(graph);
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glsl_len);
    if (!glsl) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        snprintf(resp_local.error_msg, sizeof(resp_local.error_msg), "GLSL ES 3.10 codegen failed");
        resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
        waywal_shader_graph_destroy(graph);
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    resp_local.status_code = WAYWAL_STATUS_OK;
    resp_local.active_pass_mask = schedule.active_pass_mask;
    resp_local.compiled_node_count = schedule.sorted_count;
    resp_local.estimated_alu_ops = schedule.estimated_alu_ops;
    snprintf(resp_local.error_msg, sizeof(resp_local.error_msg), "Graph compiled successfully");

    if (state && (req_local.flags & WAYWAL_COMPILE_FLAG_HOT_SWAP) != 0u &&
        (req_local.flags & WAYWAL_COMPILE_FLAG_VALIDATE_ONLY) == 0u) {
        bool swapped = false;
        bool attempted_gpu_swap = false;
        waywal_shader_engine_t *loaded_engs[16];
        size_t loaded_eng_count = 0;

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!output_matches_phase3_target(out, req_local.target_output)) {
                continue;
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
                    }
                }
            }
            waywal_shader_engine_t *eng = out->shader_eng ? out->shader_eng : state->shader_eng;
            if (!eng) {
                continue;
            }
            bool already_loaded = false;
            for (size_t e = 0; e < loaded_eng_count; ++e) {
                if (loaded_engs[e] == eng) {
                    already_loaded = true;
                    break;
                }
            }
            bool ok_eng = already_loaded;
            if (!already_loaded) {
                attempted_gpu_swap = true;
                if (waywal_shader_engine_load_source(eng, glsl, "studio_shader_graph")) {
                    upload_graph_constants_to_shader_eng(eng, graph, &schedule);
                    if (loaded_eng_count < 16u) {
                        loaded_engs[loaded_eng_count++] = eng;
                    }
                    ok_eng = true;
                }
            }
            if (ok_eng) {
                stop_engines_on_output(state, out, true, false, false, false);
                destroy_output_we_scene(state, out);
                out->shader_active = true;
                out->workshop_active = false;
                out->shader_audio_reactive = (strstr(glsl, "u_AudioBands") != NULL);
                if (out->shader_fps == 0) {
                    uint32_t hint_fps = waywal_shader_engine_get_target_fps(eng);
                    out->shader_fps = hint_fps > 0 ? hint_fps : 60U;
                }
                out->displaying_color = false;
                snprintf(out->active_media_path, sizeof(out->active_media_path),
                         "studio_shader_graph");
                swapped = true;
            }
        }

        if (!swapped && !state->outputs) {
            if (!state->shader_eng && state->dmabuf_ctx.available) {
                render_engine_make_current(&state->render_engine);
                state->shader_eng = waywal_shader_engine_create(&state->dmabuf_ctx);
            }
            if (state->shader_eng) {
                attempted_gpu_swap = true;
                if (waywal_shader_engine_load_source(state->shader_eng, glsl,
                                                     "studio_shader_graph")) {
                    upload_graph_constants_to_shader_eng(state->shader_eng, graph, &schedule);
                    state->shader_active = true;
                    state->workshop_active = false;
                    swapped = true;
                }
            }
        }

        if (attempted_gpu_swap && !swapped) {
            resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            snprintf(resp_local.error_msg, sizeof(resp_local.error_msg),
                     "Live shader engine failed to compile or link generated GLSL");
        } else if (swapped) {
            reconcile_global_engine_flags(state);
            daemon_reconcile_power_state(state);
            if (state->shader_active) {
                daemon_shader_render_once(state);
            }
        }
    }

    free(glsl);
    waywal_shader_graph_destroy(graph);
    resp_local.compile_time_ms = (float)(now_ns() - t_start) / 1000000.0f;
    memcpy(resp, &resp_local, sizeof(resp_local));
    return true;
}

bool daemon_dispatch_ipc_req_set_camera_3d(daemon_state_t *state,
                                           const waywal_ipc_req_set_camera_3d_t *req,
                                           waywal_ipc_resp_set_camera_3d_t *resp)
{
    if (!req || !resp) {
        return false;
    }

    waywal_ipc_resp_set_camera_3d_t resp_local;
    memset(&resp_local, 0, sizeof(resp_local));

    if (!waywal_ipc_validate_req_set_camera_3d(req, sizeof(*req))) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    waywal_ipc_req_set_camera_3d_t req_local;
    memcpy(&req_local, req, sizeof(req_local));

    float vp_w = 1920.0f;
    float vp_h = 1080.0f;
    if (state) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (output_matches_phase3_target(out, req_local.target_output) && out->width > 0 &&
                out->height > 0) {
                vp_w = (float)out->width;
                vp_h = (float)out->height;
                break;
            }
        }
    }

    bool computed_vp = false;
    bool updated_any_scene = false;
    we_scene_t *visited[16];
    size_t visited_count = 0;

    if (state) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!output_matches_phase3_target(out, req_local.target_output) || !out->we_scene) {
                continue;
            }
            if (scene_already_visited(visited, visited_count, out->we_scene)) {
                continue;
            }
            float out_w = (out->width > 0 && out->height > 0) ? (float)out->width : vp_w;
            float out_h = (out->width > 0 && out->height > 0) ? (float)out->height : vp_h;
            apply_ipc_camera_to_scene(out->we_scene, &req_local, out_w, out_h,
                                      computed_vp ? NULL : resp_local.active_view_proj);
            computed_vp = true;
            updated_any_scene = true;
            if (visited_count < 16u) {
                visited[visited_count++] = out->we_scene;
            }
        }

        if (state->we_scene && !scene_already_visited(visited, visited_count, state->we_scene) &&
            (!state->outputs || target_spec_is_wildcard(req_local.target_output))) {
            apply_ipc_camera_to_scene(state->we_scene, &req_local, vp_w, vp_h,
                                      computed_vp ? NULL : resp_local.active_view_proj);
            computed_vp = true;
            updated_any_scene = true;
        }
    }

    if (!computed_vp) {
        we_camera3d_t tmp_cam;
        we_camera3d_init_default(&tmp_cam, vp_w, vp_h);
        apply_ipc_camera_to_camera3d(&tmp_cam, &req_local, 0.0, vp_w, vp_h,
                                     resp_local.active_view_proj);
    }

    resp_local.status_code = WAYWAL_STATUS_OK;
    if (state && updated_any_scene && state->shader_active) {
        daemon_shader_render_once(state);
    }

    memcpy(resp, &resp_local, sizeof(resp_local));
    return true;
}

bool daemon_dispatch_ipc_req_set_physics_params(daemon_state_t *state,
                                                const waywal_ipc_req_set_physics_params_t *req,
                                                waywal_ipc_resp_set_physics_params_t *resp)
{
    if (!req || !resp) {
        return false;
    }

    uint64_t t_start = now_ns();
    waywal_ipc_resp_set_physics_params_t resp_local;
    memset(&resp_local, 0, sizeof(resp_local));

    if (!waywal_ipc_validate_req_set_physics_params(req, sizeof(*req))) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        resp_local.last_step_time_us = (float)(now_ns() - t_start) / 1000.0f;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    waywal_ipc_req_set_physics_params_t req_local;
    memcpy(&req_local, req, sizeof(req_local));

    uint32_t sim_bones = 0;
    bool updated_any_scene = false;
    we_scene_t *visited[16];
    size_t visited_count = 0;

    if (state) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!output_matches_phase3_target(out, req_local.target_output) || !out->we_scene) {
                continue;
            }
            if (scene_already_visited(visited, visited_count, out->we_scene)) {
                continue;
            }
            sim_bones += apply_ipc_physics_to_scene(out->we_scene, &req_local);
            updated_any_scene = true;
            if (visited_count < 16u) {
                visited[visited_count++] = out->we_scene;
            }
        }

        if (state->we_scene && !scene_already_visited(visited, visited_count, state->we_scene) &&
            (!state->outputs || target_spec_is_wildcard(req_local.target_output))) {
            sim_bones += apply_ipc_physics_to_scene(state->we_scene, &req_local);
            updated_any_scene = true;
        }

        if (updated_any_scene) {
            reconcile_global_engine_flags(state);
            if (state->shader_active) {
                daemon_shader_render_once(state);
            }
        }
    }

    resp_local.status_code = WAYWAL_STATUS_OK;
    resp_local.active_sim_bones = sim_bones;
    resp_local.last_step_time_us = (float)(now_ns() - t_start) / 1000.0f;
    memcpy(resp, &resp_local, sizeof(resp_local));
    return true;
}

bool daemon_dispatch_ipc_req_set_scene_lights(daemon_state_t *state,
                                              const waywal_ipc_req_set_scene_lights_t *req,
                                              waywal_ipc_resp_set_scene_lights_t *resp)
{
    if (!req || !resp) {
        return false;
    }

    waywal_ipc_resp_set_scene_lights_t resp_local;
    memset(&resp_local, 0, sizeof(resp_local));

    if (!waywal_ipc_validate_req_set_scene_lights(req, sizeof(*req))) {
        resp_local.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        memcpy(resp, &resp_local, sizeof(resp_local));
        return true;
    }

    waywal_ipc_req_set_scene_lights_t req_local;
    memcpy(&req_local, req, sizeof(req_local));

    we_lighting_uniforms_t packed;
    we_lighting_pack_ubo(req_local.lighting.lights, req_local.lighting.active_light_count,
                         req_local.lighting.ambient_color_rgb, req_local.lighting.sky_color_rgb,
                         req_local.lighting.normal_map_strength, &packed);

    bool updated_any_scene = false;
    we_scene_t *visited[16];
    size_t visited_count = 0;

    if (state) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!output_matches_phase3_target(out, req_local.target_output) || !out->we_scene) {
                continue;
            }
            if (scene_already_visited(visited, visited_count, out->we_scene)) {
                continue;
            }
            out->we_scene->lighting = packed;
            out->we_scene->light_count = packed.active_light_count;
            out->we_scene->lighting_ipc_override = true;
            updated_any_scene = true;
            if (visited_count < 16u) {
                visited[visited_count++] = out->we_scene;
            }
        }

        if (state->we_scene && !scene_already_visited(visited, visited_count, state->we_scene) &&
            (!state->outputs || target_spec_is_wildcard(req_local.target_output))) {
            state->we_scene->lighting = packed;
            state->we_scene->light_count = packed.active_light_count;
            state->we_scene->lighting_ipc_override = true;
            updated_any_scene = true;
        }

        if (updated_any_scene && state->shader_active) {
            daemon_shader_render_once(state);
        }
    }

    resp_local.status_code = WAYWAL_STATUS_OK;
    resp_local.active_lights = packed.active_light_count;
    memcpy(resp, &resp_local, sizeof(resp_local));
    return true;
}

static bool ipc_dispatch_read_full(int fd, void *buf, size_t count, int timeout_ms)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            return false;
        }
        ssize_t n = read(fd, p + total, count - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

static bool ipc_dispatch_write_full(int fd, const void *buf, size_t count, int timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            return false;
        }
        ssize_t n = write(fd, p + total, count - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

static bool ipc_drain_phase3_payload(int fd, uint64_t rem_bytes, int timeout_ms)
{
    if (rem_bytes == 0) {
        return true;
    }
    if (rem_bytes > WAYWAL_MAX_PHASE3_DRAIN_BYTES) {
        (void)shutdown(fd, SHUT_RDWR);
        return false;
    }
    uint8_t scratch[4096];
    uint64_t left = rem_bytes;
    while (left > 0) {
        size_t chunk = left < sizeof(scratch) ? (size_t)left : sizeof(scratch);
        if (!ipc_dispatch_read_full(fd, scratch, chunk, timeout_ms)) {
            (void)shutdown(fd, SHUT_RDWR);
            return false;
        }
        left -= chunk;
    }
    return true;
}

static bool ipc_send_compile_graph_error(int client_fd, uint32_t status_code, const char *msg)
{
    waywal_ipc_hdr_t resp_hdr;
    waywal_ipc_resp_compile_shader_graph_t resp;
    waywal_ipc_serialize_resp_compile_shader_graph(status_code, 0u, 0u, 0u, 0.0f, 0u, msg,
                                                   &resp_hdr, &resp);
    if (!ipc_dispatch_write_full(client_fd, &resp_hdr, sizeof(resp_hdr), 500) ||
        !ipc_dispatch_write_full(client_fd, &resp, sizeof(resp), 500)) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }
    return true;
}

bool daemon_dispatch_phase3_ipc(daemon_state_t *state, int client_fd, const waywal_ipc_hdr_t *hdr,
                                int attached_fd)
{
    if (!hdr || client_fd < 0) {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        return false;
    }

    if (hdr->magic != WAYWAL_IPC_MAGIC || hdr->version != WAYWAL_IPC_VERSION) {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }

    switch (hdr->opcode) {
    case WAYWAL_REQ_COMPILE_SHADER_GRAPH: {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        if (hdr->payload_size < sizeof(waywal_ipc_req_compile_shader_graph_t)) {
            if (!ipc_drain_phase3_payload(client_fd, hdr->payload_size, 500)) {
                return false;
            }
            return ipc_send_compile_graph_error(client_fd, WAYWAL_STATUS_ERR_SYNTAX,
                                                "Truncated shader graph request");
        }
        waywal_ipc_req_compile_shader_graph_t req;
        if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        uint64_t rem = hdr->payload_size - sizeof(req);
        if (req.inline_json_len > WAYWAL_MAX_GRAPH_JSON_FILE_BYTES ||
            rem != (uint64_t)req.inline_json_len ||
            !memchr(req.target_output, '\0', sizeof(req.target_output)) ||
            !memchr(req.graph_json_path, '\0', sizeof(req.graph_json_path))) {
            if (!ipc_drain_phase3_payload(client_fd, rem, 500)) {
                return false;
            }
            return ipc_send_compile_graph_error(client_fd, WAYWAL_STATUS_ERR_SYNTAX,
                                                "Invalid shader graph request framing");
        }
        char *inline_json = NULL;
        if (req.inline_json_len > 0) {
            inline_json = (char *)malloc((size_t)req.inline_json_len + 1u);
            if (!inline_json) {
                if (!ipc_drain_phase3_payload(client_fd, rem, 500)) {
                    return false;
                }
                return ipc_send_compile_graph_error(client_fd, WAYWAL_STATUS_ERR_SYNTAX,
                                                    "Out of memory reading inline shader graph");
            }
            if (!ipc_dispatch_read_full(client_fd, inline_json, req.inline_json_len, 500)) {
                free(inline_json);
                (void)shutdown(client_fd, SHUT_RDWR);
                return false;
            }
            if (memchr(inline_json, '\0', req.inline_json_len) != NULL) {
                free(inline_json);
                return ipc_send_compile_graph_error(client_fd, WAYWAL_STATUS_ERR_SYNTAX,
                                                    "Embedded NUL in inline shader graph JSON");
            }
            inline_json[req.inline_json_len] = '\0';
        }

        waywal_ipc_resp_compile_shader_graph_t resp;
        daemon_dispatch_ipc_req_compile_shader_graph(state, &req, inline_json, &resp);
        free(inline_json);

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_serialize_resp_compile_shader_graph(
            resp.status_code, resp.active_pass_mask, resp.compiled_node_count,
            resp.estimated_alu_ops, resp.compile_time_ms, resp.offending_node_id, resp.error_msg,
            &resp_hdr, &resp);

        if (!ipc_dispatch_write_full(client_fd, &resp_hdr, sizeof(resp_hdr), 500) ||
            !ipc_dispatch_write_full(client_fd, &resp, sizeof(resp), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        return true;
    }

    case WAYWAL_REQ_SET_CAMERA_3D: {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        waywal_ipc_resp_set_camera_3d_t resp;
        memset(&resp, 0, sizeof(resp));
        if (hdr->payload_size != sizeof(waywal_ipc_req_set_camera_3d_t)) {
            if (!ipc_drain_phase3_payload(client_fd, hdr->payload_size, 500)) {
                return false;
            }
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        } else {
            waywal_ipc_req_set_camera_3d_t req;
            if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
                (void)shutdown(client_fd, SHUT_RDWR);
                return false;
            }
            daemon_dispatch_ipc_req_set_camera_3d(state, &req, &resp);
        }

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_serialize_resp_set_camera_3d(resp.status_code, resp.active_view_proj, &resp_hdr,
                                                &resp);

        if (!ipc_dispatch_write_full(client_fd, &resp_hdr, sizeof(resp_hdr), 500) ||
            !ipc_dispatch_write_full(client_fd, &resp, sizeof(resp), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        return true;
    }

    case WAYWAL_REQ_SET_PHYSICS_PARAMS: {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        waywal_ipc_resp_set_physics_params_t resp;
        memset(&resp, 0, sizeof(resp));
        if (hdr->payload_size != sizeof(waywal_ipc_req_set_physics_params_t)) {
            if (!ipc_drain_phase3_payload(client_fd, hdr->payload_size, 500)) {
                return false;
            }
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        } else {
            waywal_ipc_req_set_physics_params_t req;
            if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
                (void)shutdown(client_fd, SHUT_RDWR);
                return false;
            }
            daemon_dispatch_ipc_req_set_physics_params(state, &req, &resp);
        }

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_serialize_resp_set_physics_params(resp.status_code, resp.active_sim_bones,
                                                     resp.last_step_time_us, &resp_hdr, &resp);

        if (!ipc_dispatch_write_full(client_fd, &resp_hdr, sizeof(resp_hdr), 500) ||
            !ipc_dispatch_write_full(client_fd, &resp, sizeof(resp), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        return true;
    }

    case WAYWAL_REQ_SET_SCENE_LIGHTS: {
        if (attached_fd >= 0) {
            close(attached_fd);
        }
        waywal_ipc_resp_set_scene_lights_t resp;
        memset(&resp, 0, sizeof(resp));
        if (hdr->payload_size != sizeof(waywal_ipc_req_set_scene_lights_t)) {
            if (!ipc_drain_phase3_payload(client_fd, hdr->payload_size, 500)) {
                return false;
            }
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        } else {
            waywal_ipc_req_set_scene_lights_t req;
            if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
                (void)shutdown(client_fd, SHUT_RDWR);
                return false;
            }
            daemon_dispatch_ipc_req_set_scene_lights(state, &req, &resp);
        }

        waywal_ipc_hdr_t resp_hdr;
        waywal_ipc_serialize_resp_set_scene_lights(resp.status_code, resp.active_lights, &resp_hdr,
                                                   &resp);

        if (!ipc_dispatch_write_full(client_fd, &resp_hdr, sizeof(resp_hdr), 500) ||
            !ipc_dispatch_write_full(client_fd, &resp, sizeof(resp), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        return true;
    }

    case WAYWAL_REQ_SCENE_MUTATE:
    case WAYWAL_REQ_HOT_RELOAD_CODE:
    case WAYWAL_REQ_QUERY_SCENE_TREE:
    case WAYWAL_REQ_SET_SHADER_COMBO:
    case WAYWAL_REQ_BRUSH_STROKE:
    case WAYWAL_REQ_PARTICLE_CONFIG:
    case WAYWAL_REQ_SET_AUDIO_BINDINGS:
    case WAYWAL_REQ_EXPORT_SCENE_PKG:
    case WAYWAL_REQ_LINT_SCENESCRIPT:
    case WAYWAL_REQ_READBACK_TEXTURE:
        return daemon_dispatch_authoring_ipc(state, client_fd, hdr, attached_fd);

    case WAYWAL_REQ_PUPPET_SET_MESH:
    case WAYWAL_REQ_PUPPET_SET_SKELETON:
    case WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS:
    case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS:
    case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE:
    case WAYWAL_REQ_PUPPET_PREVIEW_IK:
    case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS:
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
    case WAYWAL_REQ_EXPORT_PUPPET_MDL:
        return daemon_dispatch_rigging_ipc(state, client_fd, hdr, attached_fd);

    default:
        break;
    }

    if (attached_fd >= 0) {
        close(attached_fd);
    }
    if (!ipc_drain_phase3_payload(client_fd, hdr->payload_size, 500)) {
        return false;
    }
    return false;
}

/* ========================================================================= */
/* Authoring IPC Dispatch Helpers & Handlers (0x0021..0x002A)                */
/* ========================================================================= */

#define WAYWAL_AUTHORING_SCENE_SLOTS 16u
#define WAYWAL_AUTHORING_MAX_OBJECTS 1024u

typedef struct {
    const we_scene_t *scene;
    uint64_t authoring_revision;
    uint32_t revision;
} authoring_scene_slot_t;

static authoring_scene_slot_t g_authoring_slots[WAYWAL_AUTHORING_SCENE_SLOTS];
static uint32_t g_authoring_slot_cursor = 0u;
#if WAYWAL_HAVE_GPU_BRUSH
static we_gpu_brush_ctx_t *g_authoring_brush_ctx = NULL;
#endif

static bool authoring_matches_target(const output_node_t *out, const char *target_spec)
{
    if (!out) {
        return false;
    }
    if (!target_spec || target_spec[0] == '\0' || strcmp(target_spec, "*") == 0 ||
        strcasecmp(target_spec, "all") == 0) {
        return true;
    }
    return output_matches_target_spec(out, target_spec);
}

static bool authoring_drain_payload(int fd, uint64_t rem_bytes, int timeout_ms)
{
    if (rem_bytes == 0u) {
        return true;
    }
    if (rem_bytes > WAYWAL_MAX_PHASE3_DRAIN_BYTES) {
        (void)shutdown(fd, SHUT_RDWR);
        return false;
    }
    uint8_t scratch[4096];
    uint64_t left = rem_bytes;
    while (left > 0u) {
        size_t chunk = left < sizeof(scratch) ? (size_t)left : sizeof(scratch);
        if (!ipc_dispatch_read_full(fd, scratch, chunk, timeout_ms)) {
            (void)shutdown(fd, SHUT_RDWR);
            return false;
        }
        left -= chunk;
    }
    return true;
}

static authoring_scene_slot_t *authoring_get_scene_slot(const we_scene_t *sc, bool create)
{
    if (!sc) {
        return NULL;
    }
    for (size_t i = 0; i < WAYWAL_AUTHORING_SCENE_SLOTS; ++i) {
        if (g_authoring_slots[i].scene == sc &&
            g_authoring_slots[i].authoring_revision == sc->authoring_revision) {
            return &g_authoring_slots[i];
        }
        if (g_authoring_slots[i].scene == sc) {
            memset(&g_authoring_slots[i], 0, sizeof(g_authoring_slots[i]));
        }
    }
    if (!create) {
        return NULL;
    }
    for (size_t i = 0; i < WAYWAL_AUTHORING_SCENE_SLOTS; ++i) {
        if (!g_authoring_slots[i].scene) {
            memset(&g_authoring_slots[i], 0, sizeof(g_authoring_slots[i]));
            g_authoring_slots[i].scene = sc;
            g_authoring_slots[i].authoring_revision = sc->authoring_revision;
            g_authoring_slots[i].revision = 1u;
            return &g_authoring_slots[i];
        }
    }
    authoring_scene_slot_t *slot = &g_authoring_slots[g_authoring_slot_cursor];
    g_authoring_slot_cursor = (g_authoring_slot_cursor + 1u) % WAYWAL_AUTHORING_SCENE_SLOTS;
    memset(slot, 0, sizeof(*slot));
    slot->scene = sc;
    slot->authoring_revision = sc->authoring_revision;
    slot->revision = 1u;
    return slot;
}

static void authoring_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0u) {
        return;
    }
    memset(dst, 0, cap);
    if (!src) {
        return;
    }
    for (size_t i = 0u; i + 1u < cap && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} authoring_strbuf_t;

static bool authoring_sb_reserve(authoring_strbuf_t *sb, size_t extra)
{
    if (sb->oom) {
        return false;
    }
    if (extra > SIZE_MAX - sb->len - 1u) {
        sb->oom = true;
        return false;
    }
    size_t needed = sb->len + extra + 1u;
    if (needed <= sb->cap) {
        return true;
    }
    size_t new_cap = sb->cap ? sb->cap : 512u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    char *grown = (char *)realloc(sb->data, new_cap);
    if (!grown) {
        sb->oom = true;
        return false;
    }
    sb->data = grown;
    sb->cap = new_cap;
    return true;
}

static void authoring_sb_append_raw(authoring_strbuf_t *sb, const char *s, size_t n)
{
    if (!s || n == 0u || !authoring_sb_reserve(sb, n)) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void authoring_sb_append_cstr(authoring_strbuf_t *sb, const char *s)
{
    if (s) {
        authoring_sb_append_raw(sb, s, strlen(s));
    }
}

static void authoring_sb_appendf(authoring_strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void authoring_sb_appendf(authoring_strbuf_t *sb, const char *fmt, ...)
{
    if (sb->oom || !fmt) {
        return;
    }
    char stack_buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(stack_buf)) {
        authoring_sb_append_raw(sb, stack_buf, (size_t)n);
        return;
    }
    if (!authoring_sb_reserve(sb, (size_t)n)) {
        return;
    }
    va_list ap2;
    va_start(ap2, fmt);
    (void)vsnprintf(sb->data + sb->len, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

static void authoring_sb_append_json_str(authoring_strbuf_t *sb, const char *s)
{
    authoring_sb_append_raw(sb, "\"", 1u);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
            unsigned char ch = *p;
            if (ch == '"') {
                authoring_sb_append_raw(sb, "\\\"", 2u);
            } else if (ch == '\\') {
                authoring_sb_append_raw(sb, "\\\\", 2u);
            } else if (ch == '\n') {
                authoring_sb_append_raw(sb, "\\n", 2u);
            } else if (ch == '\r') {
                authoring_sb_append_raw(sb, "\\r", 2u);
            } else if (ch == '\t') {
                authoring_sb_append_raw(sb, "\\t", 2u);
            } else if (ch < 0x20u) {
                authoring_sb_appendf(sb, "\\u%04x", (unsigned)ch);
            } else {
                authoring_sb_append_raw(sb, (const char *)p, 1u);
            }
        }
    }
    authoring_sb_append_raw(sb, "\"", 1u);
}

static we_scene_object_t *authoring_find_object(we_scene_t *sc, int32_t object_id, size_t *out_idx)
{
    if (!sc || !sc->objects) {
        return NULL;
    }
    for (uint32_t i = 0; i < sc->object_count; ++i) {
        if (sc->objects[i].id == object_id) {
            if (out_idx) {
                *out_idx = i;
            }
            return &sc->objects[i];
        }
    }
    return NULL;
}

static bool authoring_reparent_creates_cycle(const we_scene_t *sc, int32_t object_id,
                                             int32_t new_parent_id)
{
    if (!sc || new_parent_id < 0) {
        return false;
    }
    if (new_parent_id == object_id) {
        return true;
    }
    int32_t cur = new_parent_id;
    for (uint32_t hop = 0; hop < sc->object_count; ++hop) {
        const we_scene_object_t *parent_obj = NULL;
        for (uint32_t i = 0; i < sc->object_count; ++i) {
            if (sc->objects[i].id == cur) {
                parent_obj = &sc->objects[i];
                break;
            }
        }
        if (!parent_obj || parent_obj->parent_id < 0) {
            return false;
        }
        if (parent_obj->parent_id == object_id) {
            return true;
        }
        cur = parent_obj->parent_id;
    }
    return true;
}

static we_material_pass_t *authoring_resolve_pass(we_scene_object_t *obj, uint32_t pass_index)
{
    if (!obj) {
        return NULL;
    }
    if (pass_index == UINT32_MAX) {
        return obj->has_base_pass ? &obj->base_pass : NULL;
    }
    uint32_t cursor = 0u;
    for (uint32_t e = 0; e < obj->effect_count; ++e) {
        we_effect_t *eff = &obj->effects[e];
        if (!eff->passes) {
            continue;
        }
        if (pass_index < cursor + eff->pass_count) {
            return &eff->passes[pass_index - cursor];
        }
        cursor += eff->pass_count;
    }
    return NULL;
}

static void authoring_ensure_scene_egl(const we_scene_t *sc)
{
    if (sc && sc->gl_initialized && sc->egl_display && sc->egl_context &&
        eglGetCurrentContext() != sc->egl_context) {
        (void)eglMakeCurrent(sc->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, sc->egl_context);
    }
}

static void authoring_invalidate_scene(const we_scene_t *sc)
{
    if (!sc) {
        return;
    }
    for (size_t i = 0; i < WAYWAL_AUTHORING_SCENE_SLOTS; ++i) {
        if (g_authoring_slots[i].scene == sc) {
            memset(&g_authoring_slots[i], 0, sizeof(g_authoring_slots[i]));
        }
    }
    rig_invalidate_scene_slots(sc);
#if WAYWAL_HAVE_GPU_BRUSH
    if (g_authoring_brush_ctx) {
        authoring_ensure_scene_egl(sc);
        we_gpu_brush_unbind_scene(g_authoring_brush_ctx, sc);
    }
#endif
}

static void daemon_authoring_cleanup(void)
{
    memset(g_authoring_slots, 0, sizeof(g_authoring_slots));
    rig_clear_all_slots();
#if WAYWAL_HAVE_GPU_BRUSH
    if (g_authoring_brush_ctx) {
        we_gpu_brush_destroy(g_authoring_brush_ctx);
        g_authoring_brush_ctx = NULL;
    }
#endif
}

static void authoring_free_material_pass(we_material_pass_t *pass, bool gl_init)
{
    if (!pass) {
        return;
    }
    for (uint32_t i = 0; i < WE_MAX_PASS_TEXTURES; ++i) {
        if (pass->textures[i]) {
            we_tex_destroy(pass->textures[i]);
            pass->textures[i] = NULL;
        }
    }
    for (uint32_t i = 0; i < pass->const_val_count; ++i) {
        free(pass->const_vals[i].script_src);
        pass->const_vals[i].script_src = NULL;
    }
    we_shader_preprocessed_free(&pass->prep);
    if (gl_init && pass->gl_prog != 0u) {
        glDeleteProgram(pass->gl_prog);
        pass->gl_prog = 0u;
    }
}

static void authoring_free_object_resources(const we_scene_t *sc, we_scene_object_t *obj)
{
    if (!obj) {
        return;
    }
    bool gl_init = (sc && sc->gl_initialized);
    if (gl_init) {
        authoring_ensure_scene_egl(sc);
    }
    if (obj->has_base_pass) {
        authoring_free_material_pass(&obj->base_pass, gl_init);
    }
    if (obj->effects) {
        for (uint32_t e = 0; e < obj->effect_count; ++e) {
            we_effect_t *eff = &obj->effects[e];
            if (eff->passes) {
                for (uint32_t p = 0; p < eff->pass_count; ++p) {
                    authoring_free_material_pass(&eff->passes[p], gl_init);
                }
                free(eff->passes);
                eff->passes = NULL;
            }
            for (uint32_t f = 0; f < eff->fbo_count; ++f) {
                if (gl_init && eff->fbos[f].gl_fbo != 0u) {
                    glDeleteFramebuffers(1, &eff->fbos[f].gl_fbo);
                }
                if (gl_init && eff->fbos[f].gl_tex != 0u) {
                    glDeleteTextures(1, &eff->fbos[f].gl_tex);
                }
                eff->fbos[f].gl_fbo = 0u;
                eff->fbos[f].gl_tex = 0u;
            }
        }
        free(obj->effects);
        obj->effects = NULL;
    }
    if (obj->puppet) {
        we_puppet_destroy(obj->puppet);
        obj->puppet = NULL;
    }
    if (obj->particle_sys) {
        we_particle_destroy(obj->particle_sys);
        obj->particle_sys = NULL;
    }
    for (uint32_t s = 0; s < obj->script_count; ++s) {
        if (obj->scripts[s].vm) {
            we_script_destroy(obj->scripts[s].vm);
            obj->scripts[s].vm = NULL;
        }
    }
    for (uint32_t t = 0; t < obj->sound_track_count; ++t) {
        free(obj->sound_tracks_pcm[t]);
        obj->sound_tracks_pcm[t] = NULL;
    }
}

static bool authoring_apply_text_json(we_scene_object_t *obj, const char *payload_json)
{
    if (!obj || !payload_json || payload_json[0] == '\0') {
        return true;
    }
    we_jval_t *root = we_json_parse_root(payload_json, strlen(payload_json));
    if (!root || root->type != JVAL_OBJ) {
        we_jval_free(root);
        return false;
    }
    const char *text = we_jval_str(we_jobj_get(root, "text"), NULL);
    if (text) {
        authoring_copy_cstr(obj->text_str, sizeof(obj->text_str), text);
    }
    const char *font = we_jval_str(we_jobj_get(root, "font"), NULL);
    if (font) {
        authoring_copy_cstr(obj->text_font, sizeof(obj->text_font), font);
    }
    const char *halign = we_jval_str(we_jobj_get(root, "halign"), NULL);
    if (halign) {
        authoring_copy_cstr(obj->text_halign, sizeof(obj->text_halign), halign);
    }
    const char *valign = we_jval_str(we_jobj_get(root, "valign"), NULL);
    if (valign) {
        authoring_copy_cstr(obj->text_valign, sizeof(obj->text_valign), valign);
    }
    const we_jval_t *ps = we_jobj_get(root, "pointsize");
    if (ps && ps->type == JVAL_NUM && isfinite(ps->num) && ps->num > 0.0) {
        double clamped = ps->num < 1.0 ? 1.0 : (ps->num > 4096.0 ? 4096.0 : ps->num);
        obj->text_pointsize = (int32_t)clamped;
    }
    const we_jval_t *pad = we_jobj_get(root, "padding");
    if (pad && pad->type == JVAL_NUM && isfinite(pad->num) && pad->num >= 0.0) {
        double clamped = pad->num > 4096.0 ? 4096.0 : pad->num;
        obj->text_padding = (int32_t)clamped;
    }
    const we_jval_t *opbg = we_jobj_get(root, "opaquebackground");
    if (opbg && opbg->type == JVAL_BOOL) {
        obj->text_opaque_bg = opbg->bval;
    }
    obj->is_text_object = true;
    obj->text_dirty = true;
    we_jval_free(root);
    return true;
}

static bool authoring_apply_sound_json(we_scene_object_t *obj, const char *payload_json)
{
    if (!obj || !payload_json || payload_json[0] == '\0') {
        return true;
    }
    we_jval_t *root = we_json_parse_root(payload_json, strlen(payload_json));
    if (!root || root->type != JVAL_OBJ) {
        we_jval_free(root);
        return false;
    }
    const we_jval_t *vol = we_jobj_get(root, "volume");
    if (vol && vol->type == JVAL_NUM && isfinite(vol->num)) {
        float v = (float)vol->num;
        obj->sound_volume = v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v);
    }
    const we_jval_t *mint = we_jobj_get(root, "mintime");
    if (mint && mint->type == JVAL_NUM && isfinite(mint->num) && mint->num >= 0.0) {
        obj->sound_mintime = (float)mint->num;
    }
    const we_jval_t *maxt = we_jobj_get(root, "maxtime");
    if (maxt && maxt->type == JVAL_NUM && isfinite(maxt->num) && maxt->num >= 0.0) {
        obj->sound_maxtime = (float)maxt->num;
    }
    const we_jval_t *ss = we_jobj_get(root, "startsilent");
    if (ss && ss->type == JVAL_BOOL) {
        obj->sound_startsilent = ss->bval;
    }
    const we_jval_t *paused = we_jobj_get(root, "paused");
    if (paused && paused->type == JVAL_BOOL) {
        obj->sound_paused = paused->bval;
    }
    const char *mode = we_jval_str(we_jobj_get(root, "mode"), NULL);
    if (mode) {
        if (strcasecmp(mode, "single") == 0) {
            obj->sound_mode = WE_SOUND_MODE_SINGLE;
        } else if (strcasecmp(mode, "random") == 0) {
            obj->sound_mode = WE_SOUND_MODE_RANDOM;
        } else {
            obj->sound_mode = WE_SOUND_MODE_LOOP;
        }
    }
    obj->is_sound_object = true;
    we_jval_free(root);
    return true;
}

#define WE_SCENE_MAX_PARENT_DEPTH 16u

static uint32_t authoring_parent_chain_depth(const we_scene_t *sc, int32_t parent_id)
{
    if (!sc || parent_id < 0) {
        return 0u;
    }
    uint32_t depth = 0u;
    int32_t cur = parent_id;
    while (cur >= 0 && depth <= WE_SCENE_MAX_PARENT_DEPTH + 1u) {
        const we_scene_object_t *p = NULL;
        for (uint32_t i = 0; i < sc->object_count; ++i) {
            if (sc->objects[i].id == cur) {
                p = &sc->objects[i];
                break;
            }
        }
        if (!p) {
            break;
        }
        depth++;
        cur = p->parent_id;
    }
    return depth;
}

static uint32_t authoring_subtree_depth(const we_scene_t *sc, int32_t root_id, uint32_t depth)
{
    if (!sc || depth > WE_SCENE_MAX_PARENT_DEPTH) {
        return depth;
    }
    uint32_t max_d = depth;
    for (uint32_t i = 0; i < sc->object_count; ++i) {
        if (sc->objects[i].parent_id == root_id && sc->objects[i].id != root_id) {
            uint32_t d = authoring_subtree_depth(sc, sc->objects[i].id, depth + 1u);
            if (d > max_d) {
                max_d = d;
            }
        }
    }
    return max_d;
}

static bool authoring_mutate_scene(we_scene_t *sc, const waywal_ipc_req_scene_mutate_t *r,
                                   const char *payload_json, uint32_t *out_status,
                                   int32_t *out_obj_id, char *err_buf, size_t err_cap)
{
    authoring_scene_slot_t *slot = authoring_get_scene_slot(sc, true);

    if (r->mutate_op == WE_MUTATE_ADD_OBJECT) {
        if (sc->object_count >= WAYWAL_AUTHORING_MAX_OBJECTS) {
            *out_status = 3u;
            authoring_copy_cstr(err_buf, err_cap, "Scene object limit exceeded");
            return false;
        }
        if (r->blend_mode > (uint32_t)WE_BLEND_DISABLED) {
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Invalid blend mode");
            return false;
        }
        int32_t new_id = r->object_id;
        if (new_id <= 0 || authoring_find_object(sc, new_id, NULL) != NULL) {
            int32_t max_id = 0;
            for (uint32_t i = 0; i < sc->object_count; ++i) {
                if (sc->objects[i].id > max_id) {
                    max_id = sc->objects[i].id;
                }
            }
            new_id = max_id + 1;
        }
        if (r->parent_id >= 0) {
            if (r->parent_id == new_id) {
                *out_status = 2u;
                authoring_copy_cstr(err_buf, err_cap, "Parent cycle detected");
                return false;
            }
            if (!authoring_find_object(sc, r->parent_id, NULL)) {
                *out_status = 1u;
                authoring_copy_cstr(err_buf, err_cap, "Parent scene object not found");
                return false;
            }
            if (authoring_parent_chain_depth(sc, r->parent_id) > WE_SCENE_MAX_PARENT_DEPTH) {
                *out_status = 2u;
                authoring_copy_cstr(err_buf, err_cap, "Parent hierarchy depth exceeds maximum");
                return false;
            }
        }
        we_scene_object_t *grown = (we_scene_object_t *)realloc(
            sc->objects, ((size_t)sc->object_count + 1u) * sizeof(we_scene_object_t));
        if (!grown) {
            *out_status = 3u;
            authoring_copy_cstr(err_buf, err_cap, "Out of memory appending scene object");
            return false;
        }
        sc->objects = grown;
        we_scene_object_t *obj = &sc->objects[sc->object_count];
        memset(obj, 0, sizeof(*obj));
        obj->id = new_id;
        obj->parent_id = r->parent_id;
        authoring_copy_cstr(obj->name, sizeof(obj->name), r->name);
        memcpy(obj->origin, r->origin, sizeof(obj->origin));
        memcpy(obj->scale, r->scale, sizeof(obj->scale));
        memcpy(obj->angles, r->angles, sizeof(obj->angles));
        memcpy(obj->size, r->size, sizeof(obj->size));
        memcpy(obj->parallax_depth, r->parallax_depth, sizeof(obj->parallax_depth));
        obj->color[0] = r->color[0];
        obj->color[1] = r->color[1];
        obj->color[2] = r->color[2];
        obj->alpha = r->color[3];
        obj->brightness = 1.0f;
        obj->color_blend_mode = (int32_t)r->blend_mode;
        obj->visible = (r->flags & 0x1u) != 0u;
        obj->fullscreen = (r->flags & 0x4u) != 0u;
        obj->passthrough = (r->flags & 0x8u) != 0u;
        obj->is_sound_object = (r->node_kind == WE_NODE_SOUND);
        obj->is_text_object = (r->node_kind == WE_NODE_TEXT);
        obj->is_light_object = (r->node_kind == WE_NODE_LIGHT);
        if (obj->is_light_object) {
            obj->light_type = 0u;
            obj->light_radius = 400.0f;
            obj->light_intensity = 1.0f;
            obj->light_specular_power = 16.0f;
        }
        bool payload_ok = true;
        if (obj->is_text_object) {
            obj->text_dirty = true;
            obj->text_pointsize = 24;
            authoring_copy_cstr(obj->text_halign, sizeof(obj->text_halign), "center");
            authoring_copy_cstr(obj->text_valign, sizeof(obj->text_valign), "center");
            payload_ok = authoring_apply_text_json(obj, payload_json);
        } else if (obj->is_sound_object) {
            obj->sound_volume = 1.0f;
            payload_ok = authoring_apply_sound_json(obj, payload_json);
        } else if (payload_json && payload_json[0] != '\0') {
            we_jval_t *probe = we_json_parse_root(payload_json, strlen(payload_json));
            payload_ok = (probe != NULL && probe->type == JVAL_OBJ);
            we_jval_free(probe);
        }
        if (!payload_ok) {
            authoring_free_object_resources(sc, obj);
            memset(obj, 0, sizeof(*obj));
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Invalid object payload JSON");
            return false;
        }
        sc->object_count++;
        if (slot) {
            slot->revision++;
        }
        *out_obj_id = new_id;
        return true;
    }

    size_t idx = 0u;
    we_scene_object_t *obj = authoring_find_object(sc, r->object_id, &idx);
    if (!obj) {
        *out_status = 1u;
        authoring_copy_cstr(err_buf, err_cap, "Scene object not found");
        return false;
    }

    switch (r->mutate_op) {
    case WE_MUTATE_REMOVE_OBJECT: {
        int32_t removed_id = obj->id;
        for (uint32_t i = 0; i < sc->object_count; ++i) {
            if (sc->objects[i].parent_id == removed_id) {
                sc->objects[i].parent_id = -1;
            }
        }
        authoring_free_object_resources(sc, obj);
        if (idx + 1u < sc->object_count) {
            memmove(&sc->objects[idx], &sc->objects[idx + 1u],
                    (size_t)(sc->object_count - idx - 1u) * sizeof(we_scene_object_t));
        }
        sc->object_count--;
        break;
    }
    case WE_MUTATE_REORDER_Z: {
        if (r->target_z_index < 0 || (uint32_t)r->target_z_index >= sc->object_count) {
            *out_status = 3u;
            authoring_copy_cstr(err_buf, err_cap, "Target Z index out of range");
            return false;
        }
        size_t dst_idx = (size_t)r->target_z_index;
        if (dst_idx != idx) {
            we_scene_object_t moved = sc->objects[idx];
            if (dst_idx < idx) {
                memmove(&sc->objects[dst_idx + 1u], &sc->objects[dst_idx],
                        (idx - dst_idx) * sizeof(we_scene_object_t));
            } else {
                memmove(&sc->objects[idx], &sc->objects[idx + 1u],
                        (dst_idx - idx) * sizeof(we_scene_object_t));
            }
            sc->objects[dst_idx] = moved;
        }
        break;
    }
    case WE_MUTATE_REPARENT: {
        if (r->parent_id >= 0 && !authoring_find_object(sc, r->parent_id, NULL)) {
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Parent scene object not found");
            return false;
        }
        if (authoring_reparent_creates_cycle(sc, obj->id, r->parent_id)) {
            *out_status = 2u;
            authoring_copy_cstr(err_buf, err_cap, "Reparent rejected: hierarchy cycle detected");
            return false;
        }
        if (r->parent_id >= 0) {
            uint32_t parent_hops = authoring_parent_chain_depth(sc, r->parent_id);
            uint32_t sub_depth = authoring_subtree_depth(sc, obj->id, 0u);
            if (parent_hops + sub_depth > WE_SCENE_MAX_PARENT_DEPTH) {
                *out_status = 2u;
                authoring_copy_cstr(err_buf, err_cap,
                                    "Reparent rejected: hierarchy depth exceeds maximum");
                return false;
            }
        }
        obj->parent_id = r->parent_id;
        break;
    }
    case WE_MUTATE_SET_TRANSFORM:
        memcpy(obj->origin, r->origin, sizeof(obj->origin));
        memcpy(obj->scale, r->scale, sizeof(obj->scale));
        memcpy(obj->angles, r->angles, sizeof(obj->angles));
        memcpy(obj->size, r->size, sizeof(obj->size));
        memcpy(obj->parallax_depth, r->parallax_depth, sizeof(obj->parallax_depth));
        break;
    case WE_MUTATE_SET_BLEND_VIS:
        if (r->blend_mode > (uint32_t)WE_BLEND_DISABLED) {
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Invalid blend mode");
            return false;
        }
        obj->color_blend_mode = (int32_t)r->blend_mode;
        obj->visible = (r->flags & 0x1u) != 0u;
        obj->fullscreen = (r->flags & 0x4u) != 0u;
        obj->passthrough = (r->flags & 0x8u) != 0u;
        obj->color[0] = r->color[0];
        obj->color[1] = r->color[1];
        obj->color[2] = r->color[2];
        obj->alpha = r->color[3];
        if (obj->has_base_pass) {
            obj->base_pass.blending = (we_blend_mode_t)r->blend_mode;
        }
        break;
    case WE_MUTATE_SET_TEXT_PROPS:
        if (!authoring_apply_text_json(obj, payload_json)) {
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Invalid JSON payload for text properties");
            return false;
        }
        break;
    case WE_MUTATE_SET_SOUND_PROPS:
        if (!authoring_apply_sound_json(obj, payload_json)) {
            *out_status = 1u;
            authoring_copy_cstr(err_buf, err_cap, "Invalid JSON payload for sound properties");
            return false;
        }
        break;
    default:
        *out_status = 1u;
        authoring_copy_cstr(err_buf, err_cap, "Unsupported scene mutation opcode");
        return false;
    }

    if (slot) {
        slot->revision++;
    }
    *out_obj_id = r->object_id;
    return true;
}

bool daemon_dispatch_ipc_req_scene_mutate(daemon_state_t *state,
                                          const waywal_ipc_req_scene_mutate_t *req,
                                          const char *payload_json,
                                          waywal_ipc_resp_scene_mutate_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "NULL scene mutate request");
        return false;
    }

    waywal_ipc_req_scene_mutate_t r;
    memcpy(&r, req, sizeof(r));
    if (r.mutate_op > (uint32_t)WE_MUTATE_SET_SOUND_PROPS ||
        r.node_kind > (uint32_t)WE_NODE_CAMERA3D ||
        !memchr(r.target_output, '\0', sizeof(r.target_output)) ||
        !memchr(r.name, '\0', sizeof(r.name))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Malformed scene mutate args");
        return false;
    }

    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        resp->object_id = r.object_id;
        return true;
    }

    we_scene_t *visited[64];
    size_t visited_count = 0u;
    bool found_scene = false;
    uint32_t last_rev = 0u;
    uint32_t last_count = 0u;
    int32_t out_obj_id = r.object_id;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!authoring_matches_target(out, r.target_output) || !out->we_scene) {
            continue;
        }
        if (scene_already_visited(visited, visited_count, out->we_scene)) {
            continue;
        }
        if (visited_count < 64u) {
            visited[visited_count++] = out->we_scene;
        }
        found_scene = true;
        uint32_t err_status = 0u;
        if (!authoring_mutate_scene(out->we_scene, &r, payload_json, &err_status, &out_obj_id,
                                    resp->error_msg, sizeof(resp->error_msg))) {
            resp->status_code = err_status ? err_status : WAYWAL_STATUS_ERR_SYNTAX;
            resp->object_id = r.object_id;
            return false;
        }
        authoring_scene_slot_t *slot = authoring_get_scene_slot(out->we_scene, false);
        last_rev = slot ? slot->revision : 1u;
        last_count = out->we_scene->object_count;
    }

    if (!found_scene) {
        resp->status_code = 1u;
        resp->object_id = r.object_id;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "No active scene matching target output");
        return false;
    }

    resp->status_code = WAYWAL_STATUS_OK;
    resp->object_id = out_obj_id;
    resp->scene_revision = last_rev;
    resp->object_count = last_count;
    return true;
}

static char *authoring_build_shader_meta_json(const we_shader_meta_t *meta, size_t *out_len)
{
    authoring_strbuf_t sb = {0};
    authoring_sb_append_cstr(&sb, "{\"uniforms\":[");
    for (uint32_t i = 0; i < meta->uniform_count; ++i) {
        const we_shader_uniform_meta_t *u = &meta->uniforms[i];
        if (i > 0u) {
            authoring_sb_append_raw(&sb, ",", 1u);
        }
        authoring_sb_append_cstr(&sb, "{\"name\":");
        authoring_sb_append_json_str(&sb, u->name);
        authoring_sb_append_cstr(&sb, ",\"type\":");
        authoring_sb_append_json_str(&sb, u->type);
        authoring_sb_append_cstr(&sb, ",\"material\":");
        authoring_sb_append_json_str(&sb, u->material_key);
        authoring_sb_appendf(&sb, ",\"components\":%u,\"sampler_slot\":%d}", u->num_components,
                             u->sampler_slot);
    }
    authoring_sb_append_cstr(&sb, "],\"combos\":[");
    for (uint32_t i = 0; i < meta->combo_count; ++i) {
        if (i > 0u) {
            authoring_sb_append_raw(&sb, ",", 1u);
        }
        authoring_sb_append_cstr(&sb, "{\"name\":");
        authoring_sb_append_json_str(&sb, meta->combos[i].name);
        authoring_sb_appendf(&sb, ",\"value\":%d}", meta->combos[i].value);
    }
    authoring_sb_append_cstr(&sb, "]}");
    if (sb.oom) {
        free(sb.data);
        if (out_len) {
            *out_len = 0u;
        }
        return NULL;
    }
    if (out_len) {
        *out_len = sb.len;
    }
    return sb.data;
}

bool daemon_dispatch_ipc_req_hot_reload_code(daemon_state_t *state,
                                             const waywal_ipc_req_hot_reload_code_t *req,
                                             const char *source_utf8,
                                             waywal_ipc_resp_hot_reload_code_t *resp,
                                             char **out_meta_json, size_t *out_meta_len)
{
    if (out_meta_json) {
        *out_meta_json = NULL;
    }
    if (out_meta_len) {
        *out_meta_len = 0u;
    }
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->code_kind > 2u || req->source_len > WAYWAL_IPC_MAX_CODE_BYTES ||
        !memchr(req->target_output, '\0', sizeof(req->target_output)) ||
        !memchr(req->uniform_name, '\0', sizeof(req->uniform_name))) {
        resp->status_code = 1u;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Invalid hot-reload header");
        return false;
    }
    if (req->source_len > 0u && !source_utf8) {
        resp->status_code = 1u;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Missing code payload");
        return false;
    }

    uint64_t t0 = now_ns();
    size_t src_len = req->source_len;
    char *src_copy = (char *)malloc(src_len + 1u);
    if (!src_copy) {
        resp->status_code = 1u;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Out of memory");
        return false;
    }
    if (src_len > 0u) {
        memcpy(src_copy, source_utf8, src_len);
    }
    src_copy[src_len] = '\0';

    if (req->code_kind == 2u) {
        we_script_diag_t diag;
        memset(&diag, 0, sizeof(diag));
        bool ok = we_script_lint(src_copy, src_len, 1u, &diag);
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), diag.error_msg);
        if (ok && (req->flags & 0x2u) != 0u && state != NULL) {
            we_scene_t *visited[64];
            size_t visited_count = 0u;
            bool applied = false;
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (!authoring_matches_target(out, req->target_output) || !out->we_scene) {
                    continue;
                }
                if (scene_already_visited(visited, visited_count, out->we_scene)) {
                    continue;
                }
                if (visited_count < 64u) {
                    visited[visited_count++] = out->we_scene;
                }
                we_scene_object_t *obj = authoring_find_object(out->we_scene, req->object_id, NULL);
                if (!obj) {
                    continue;
                }
                we_script_t *vm =
                    we_script_create(src_copy, src_len, &out->we_scene->script_shared);
                if (!vm || we_script_was_aborted(vm)) {
                    if (vm) {
                        we_script_destroy(vm);
                    }
                    free(src_copy);
                    resp->status_code = 3u;
                    resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
                    authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                        "SceneScript initialization aborted by watchdog");
                    return false;
                }
                if (req->script_slot < WE_MAX_OBJECT_SCRIPTS) {
                    if (obj->scripts[req->script_slot].vm) {
                        we_script_destroy(obj->scripts[req->script_slot].vm);
                    }
                    obj->scripts[req->script_slot].vm = vm;
                    if (req->uniform_name[0] != '\0') {
                        authoring_copy_cstr(obj->scripts[req->script_slot].uniform_name,
                                            sizeof(obj->scripts[req->script_slot].uniform_name),
                                            req->uniform_name);
                    }
                    if (req->script_slot >= obj->script_count) {
                        obj->script_count = req->script_slot + 1u;
                    }
                } else {
                    we_script_destroy(vm);
                }
                applied = true;
            }
            if (!applied) {
                free(src_copy);
                resp->status_code = 4u;
                authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                    "Target scene object not found for script hot-swap");
                return false;
            }
        }
        free(src_copy);
        resp->status_code = ok ? WAYWAL_STATUS_OK : (diag.status_code ? diag.status_code : 1u);
        resp->error_line = diag.error_line;
        resp->error_col = diag.error_col;
        resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
        return ok;
    }

    we_shader_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    we_shader_extract_metadata(src_copy, &meta);
    bool is_frag = (req->code_kind == 0u);
    bool do_hot_swap = (req->flags & 0x2u) != 0u;
    bool do_validate = (req->flags & 0x1u) != 0u;
    bool swapped = false;
    bool validated_pass = false;

    if (state != NULL && (do_hot_swap || do_validate)) {
        we_scene_t *visited_scenes[64];
        size_t visited_scene_count = 0u;
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (!authoring_matches_target(out, req->target_output)) {
                continue;
            }
            if (do_hot_swap && req->object_id < 0 && out->shader_eng && is_frag) {
                if (waywal_shader_engine_load_source(out->shader_eng, src_copy, "hot_reload")) {
                    swapped = true;
                }
            } else if (out->we_scene && req->object_id >= 0) {
                if (scene_already_visited(visited_scenes, visited_scene_count, out->we_scene)) {
                    continue;
                }
                if (visited_scene_count < 64u) {
                    visited_scenes[visited_scene_count++] = out->we_scene;
                }
                we_scene_object_t *obj = authoring_find_object(out->we_scene, req->object_id, NULL);
                we_material_pass_t *pass = authoring_resolve_pass(obj, req->pass_index);
                if (!pass) {
                    continue;
                }
                we_shader_meta_t pass_meta;
                memset(&pass_meta, 0, sizeof(pass_meta));
                we_shader_extract_metadata(src_copy, &pass_meta);
                bool tex0_is_gif =
                    (pass->textures[0] && (pass->textures[0]->flags & WE_TEX_FLAG_IS_GIF) != 0u);
                char *pass_prep = we_shader_preprocess_stage(
                    src_copy, is_frag, out->we_scene->pkg, pass->combos, pass->combo_count,
                    pass->bound_tex_mask, tex0_is_gif, &pass_meta);
                if (!pass_prep) {
                    free(src_copy);
                    resp->status_code = 1u;
                    resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
                    authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                        "GLSL preprocessing failed");
                    return false;
                }

                if (out->we_scene->gl_initialized) {
                    static const char k_val_vert_300es[] =
                        "#version 300 es\nlayout(location=0) in vec2 a_pos;"
                        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
                    static const char k_val_frag_300es[] =
                        "#version 300 es\nprecision mediump float; out vec4 fragColor;"
                        "void main() { fragColor = vec4(1.0); }\n";
                    bool have_both_real =
                        (is_frag ? (pass->prep.vert_glsl != NULL) : (pass->prep.frag_glsl != NULL));
                    const char *cand_vert =
                        is_frag ? (pass->prep.vert_glsl ? pass->prep.vert_glsl : k_val_vert_300es)
                                : pass_prep;
                    const char *cand_frag =
                        is_frag ? pass_prep
                                : (pass->prep.frag_glsl ? pass->prep.frag_glsl : k_val_frag_300es);
                    authoring_ensure_scene_egl(out->we_scene);
                    uint32_t prog =
                        we_shader_compile_program(cand_vert, cand_frag, "hot_reload_pass");
                    if (prog == 0u) {
                        free(pass_prep);
                        free(src_copy);
                        resp->status_code = 2u;
                        resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
                        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                            "GLSL program link error");
                        return false;
                    }
                    if (do_hot_swap && have_both_real) {
                        if (pass->gl_prog != 0u) {
                            glDeleteProgram(pass->gl_prog);
                        }
                        pass->gl_prog = prog;
                        pass->ulocs.initialized = false;
                    } else {
                        glDeleteProgram(prog);
                    }
                }

                if (do_hot_swap) {
                    if (is_frag) {
                        free(pass->prep.frag_glsl);
                        pass->prep.frag_glsl = pass_prep;
                    } else {
                        free(pass->prep.vert_glsl);
                        pass->prep.vert_glsl = pass_prep;
                    }
                    pass->prep.meta = pass_meta;
                    swapped = true;
                } else {
                    free(pass_prep);
                }
                meta = pass_meta;
                validated_pass = true;
            }
        }
        if (do_hot_swap && !swapped) {
            free(src_copy);
            resp->status_code = 4u;
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "Target shader pass not found");
            return false;
        }
    }

    if (!validated_pass) {
        char *prep =
            we_shader_preprocess_stage(src_copy, is_frag, NULL, NULL, 0u, 0u, false, &meta);
        if (!prep) {
            free(src_copy);
            resp->status_code = 1u;
            resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "GLSL preprocessing failed");
            return false;
        }
        free(prep);
    }

    free(src_copy);

    if ((req->flags & 0x4u) != 0u && out_meta_json && out_meta_len) {
        size_t mlen = 0u;
        char *mjs = authoring_build_shader_meta_json(&meta, &mlen);
        if (mjs && mlen <= WAYWAL_IPC_MAX_META_JSON_BYTES) {
            *out_meta_json = mjs;
            *out_meta_len = mlen;
            resp->meta_json_len = (uint32_t)mlen;
        } else {
            free(mjs);
        }
    }

    resp->status_code = WAYWAL_STATUS_OK;
    resp->uniform_count = meta.uniform_count;
    resp->combo_count = meta.combo_count;
    resp->compile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
    return true;
}

bool daemon_dispatch_ipc_req_query_scene_tree(daemon_state_t *state,
                                              const waywal_ipc_req_query_scene_tree_t *req,
                                              waywal_ipc_resp_query_scene_tree_t *resp,
                                              char **out_json, size_t *out_json_len)
{
    if (out_json) {
        *out_json = NULL;
    }
    if (out_json_len) {
        *out_json_len = 0u;
    }
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || !memchr(req->target_output, '\0', sizeof(req->target_output))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    we_scene_t *sc = NULL;
    if (state) {
        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            if (authoring_matches_target(out, req->target_output) && out->we_scene) {
                sc = out->we_scene;
                break;
            }
        }
    }

    authoring_scene_slot_t *slot = sc ? authoring_get_scene_slot(sc, true) : NULL;
    uint32_t rev = slot ? slot->revision : 0u;
    uint32_t obj_count = sc ? sc->object_count : 0u;

    authoring_strbuf_t sb = {0};
    authoring_sb_appendf(&sb, "{\"scene_revision\":%u,\"object_count\":%u,\"objects\":[", rev,
                         obj_count);

    if (sc && sc->objects) {
        for (uint32_t i = 0; i < sc->object_count; ++i) {
            const we_scene_object_t *o = &sc->objects[i];
            if (i > 0u) {
                authoring_sb_append_raw(&sb, ",", 1u);
            }
            const char *kind =
                o->is_text_object
                    ? "text"
                    : (o->is_sound_object
                           ? "sound"
                           : (o->is_light_object ? "light"
                                                 : (o->particle_sys ? "particle" : "image")));
            authoring_sb_appendf(&sb,
                                 "{\"id\":%d,\"parent_id\":%d,\"kind\":\"%s\",\"name\":", o->id,
                                 o->parent_id, kind);
            authoring_sb_append_json_str(&sb, o->name);
            authoring_sb_appendf(
                &sb,
                ",\"visible\":%s,\"blend_mode\":%d,\"alpha\":%.6g,"
                "\"origin\":[%.6g,%.6g,%.6g],\"scale\":[%.6g,%.6g,%.6g],\"size\":[%.6g,%.6g]",
                o->visible ? "true" : "false", o->color_blend_mode, (double)o->alpha,
                (double)o->origin[0], (double)o->origin[1], (double)o->origin[2],
                (double)o->scale[0], (double)o->scale[1], (double)o->scale[2], (double)o->size[0],
                (double)o->size[1]);
            if ((req->flags & 0x1u) != 0u && o->has_base_pass) {
                authoring_sb_appendf(&sb, ",\"combo_count\":%u,\"const_val_count\":%u",
                                     o->base_pass.combo_count, o->base_pass.const_val_count);
            }
            if ((req->flags & 0x2u) != 0u && o->particle_sys) {
                authoring_sb_appendf(&sb, ",\"particle\":{\"alive\":%u,\"max\":%u}",
                                     o->particle_sys->alive_count, o->particle_sys->max_count);
            }
            if ((req->flags & 0x4u) != 0u) {
                authoring_sb_appendf(&sb, ",\"script_count\":%u", o->script_count);
            }
            if ((req->flags & 0x8u) != 0u && o->is_sound_object) {
                authoring_sb_appendf(&sb, ",\"sound_volume\":%.6g,\"sound_paused\":%s",
                                     (double)o->sound_volume, o->sound_paused ? "true" : "false");
            }
            authoring_sb_append_raw(&sb, "}", 1u);
        }
    }

    if ((req->flags & 0x8u) != 0u && sc) {
        authoring_sb_appendf(&sb, "],\"audio_binding_count\":%u}", sc->audio_binding_count);
    } else {
        authoring_sb_append_cstr(&sb, "]}");
    }

    if (sb.oom || sb.len > WAYWAL_IPC_MAX_SCENE_JSON) {
        free(sb.data);
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    if (out_json) {
        *out_json = sb.data;
    } else {
        free(sb.data);
    }
    if (out_json_len) {
        *out_json_len = sb.len;
    }
    resp->status_code = WAYWAL_STATUS_OK;
    resp->scene_revision = rev;
    resp->object_count = obj_count;
    resp->json_len = (uint32_t)sb.len;
    return true;
}

static bool authoring_combo_name_valid(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    unsigned char c0 = (unsigned char)name[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) {
        return false;
    }
    for (size_t i = 1u; name[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

static char *authoring_patch_combo_define(const char *stage_glsl, const char *combo_name,
                                          int32_t combo_value)
{
    if (!stage_glsl || !authoring_combo_name_valid(combo_name)) {
        return NULL;
    }
    char def_line[160];
    int n = snprintf(def_line, sizeof(def_line), "#define %s %d\n", combo_name, combo_value);
    if (n <= 0 || (size_t)n >= sizeof(def_line)) {
        return NULL;
    }
    size_t def_len = (size_t)n;
    size_t name_len = strlen(combo_name);
    size_t src_len = strlen(stage_glsl);
    size_t cap = src_len + def_len + 64u;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }

    size_t used = 0u;
    size_t insert_off = 0u;
    bool saw_version = false;
    bool replaced = false;
    const char *p = stage_glsl;

    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p + 1) : strlen(p);
        const char *q = p;
        while ((size_t)(q - p) < line_len && (*q == ' ' || *q == '\t')) {
            q++;
        }
        size_t rem = line_len - (size_t)(q - p);
        bool is_define = false;
        if (rem >= 8u && strncmp(q, "#define", 7) == 0 && (q[7] == ' ' || q[7] == '\t')) {
            const char *r = q + 7;
            while ((size_t)(r - p) < line_len && (*r == ' ' || *r == '\t')) {
                r++;
            }
            size_t r_rem = line_len - (size_t)(r - p);
            if (r_rem >= name_len && strncmp(r, combo_name, name_len) == 0) {
                char next_c = (r_rem > name_len) ? r[name_len] : '\0';
                if (next_c == ' ' || next_c == '\t' || next_c == '\r' || next_c == '\n' ||
                    next_c == '\0') {
                    is_define = true;
                }
            }
        }
        bool is_version = (!saw_version && rem >= 8u && strncmp(q, "#version", 8) == 0);
        const char *emit = is_define ? def_line : p;
        size_t emit_len = is_define ? def_len : line_len;
        if (used + emit_len + def_len + 2u > cap) {
            size_t new_cap = (used + emit_len + def_len + 64u) * 2u;
            char *grown = (char *)realloc(out, new_cap);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
            cap = new_cap;
        }
        memcpy(out + used, emit, emit_len);
        used += emit_len;
        if (is_define) {
            replaced = true;
        }
        if (is_version) {
            saw_version = true;
            insert_off = used;
        }
        p += line_len;
    }

    if (!replaced) {
        if (used + def_len + 2u > cap) {
            char *grown = (char *)realloc(out, used + def_len + 2u);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        memmove(out + insert_off + def_len, out + insert_off, used - insert_off);
        memcpy(out + insert_off, def_line, def_len);
        used += def_len;
    }

    if (used > 0u && out[used - 1u] != '\n') {
        out[used++] = '\n';
    }
    out[used] = '\0';
    return out;
}

bool daemon_dispatch_ipc_req_set_shader_combo(daemon_state_t *state,
                                              const waywal_ipc_req_set_shader_combo_t *req,
                                              waywal_ipc_resp_set_shader_combo_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || !memchr(req->target_output, '\0', sizeof(req->target_output)) ||
        !memchr(req->combo_name, '\0', sizeof(req->combo_name)) ||
        !authoring_combo_name_valid(req->combo_name)) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Invalid shader combo request");
        return false;
    }

    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        return true;
    }

    uint64_t t0 = now_ns();
    we_scene_t *visited[64];
    size_t visited_count = 0u;
    bool updated = false;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!authoring_matches_target(out, req->target_output) || !out->we_scene) {
            continue;
        }
        if (scene_already_visited(visited, visited_count, out->we_scene)) {
            continue;
        }
        if (visited_count < 64u) {
            visited[visited_count++] = out->we_scene;
        }
        we_scene_object_t *obj = authoring_find_object(out->we_scene, req->object_id, NULL);
        we_material_pass_t *pass = authoring_resolve_pass(obj, req->pass_index);
        if (!pass) {
            continue;
        }
        bool found_combo = false;
        size_t combo_idx = 0u;
        for (uint32_t c = 0; c < pass->combo_count; ++c) {
            if (strcmp(pass->combos[c].name, req->combo_name) == 0) {
                combo_idx = c;
                found_combo = true;
                break;
            }
        }
        if (!found_combo && pass->combo_count >= WE_SHADER_MAX_COMBOS) {
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "Shader combo limit exceeded");
            return false;
        }

        char *cand_vert = NULL;
        char *cand_frag = NULL;
        if (pass->prep.vert_glsl) {
            cand_vert = authoring_patch_combo_define(pass->prep.vert_glsl, req->combo_name,
                                                     req->combo_value);
            if (!cand_vert) {
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                    "Out of memory patching vertex shader combo");
                return false;
            }
        }
        if (pass->prep.frag_glsl) {
            cand_frag = authoring_patch_combo_define(pass->prep.frag_glsl, req->combo_name,
                                                     req->combo_value);
            if (!cand_frag) {
                free(cand_vert);
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                    "Out of memory patching fragment shader combo");
                return false;
            }
        }

        if (out->we_scene->gl_initialized && cand_vert && cand_frag) {
            authoring_ensure_scene_egl(out->we_scene);
            uint32_t prog = we_shader_compile_program(cand_vert, cand_frag, pass->shader_name);
            if (prog == 0u) {
                free(cand_vert);
                free(cand_frag);
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                resp->recompile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
                authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                    "Shader recompile failed");
                return false;
            }
            if (pass->gl_prog != 0u) {
                glDeleteProgram(pass->gl_prog);
            }
            pass->gl_prog = prog;
            pass->ulocs.initialized = false;
        }

        if (cand_vert) {
            free(pass->prep.vert_glsl);
            pass->prep.vert_glsl = cand_vert;
        }
        if (cand_frag) {
            free(pass->prep.frag_glsl);
            pass->prep.frag_glsl = cand_frag;
        }
        if (found_combo) {
            pass->combos[combo_idx].value = req->combo_value;
        } else {
            authoring_copy_cstr(pass->combos[pass->combo_count].name,
                                sizeof(pass->combos[pass->combo_count].name), req->combo_name);
            pass->combos[pass->combo_count].value = req->combo_value;
            pass->combo_count++;
        }
        updated = true;
    }

    if (!updated) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Target shader pass not found");
        return false;
    }

    resp->status_code = WAYWAL_STATUS_OK;
    resp->recompile_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
    return true;
}

static int32_t authoring_clamp_double_to_i32(double v)
{
    if (!isfinite(v)) {
        return 0;
    }
    if (v <= (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (v >= (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)v;
}

bool daemon_dispatch_ipc_req_brush_stroke(daemon_state_t *state,
                                          const waywal_ipc_req_brush_stroke_t *req,
                                          const we_brush_dab_t *dabs,
                                          waywal_ipc_resp_brush_stroke_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->action > (uint32_t)WE_BRUSH_IPC_REDO_STROKE ||
        req->dab_count > WE_BRUSH_MAX_STROKE_DABS ||
        !memchr(req->target_output, '\0', sizeof(req->target_output))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    if (req->dab_count > 0u && !dabs) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    resp->stroke_id = req->stroke_id;

    if (!state) {
        if (req->dab_count > 0u) {
            double min_x = (double)dabs[0].ax - (double)dabs[0].radius_a;
            double min_y = (double)dabs[0].ay - (double)dabs[0].radius_a;
            double max_x = (double)dabs[0].ax + (double)dabs[0].radius_a;
            double max_y = (double)dabs[0].ay + (double)dabs[0].radius_a;
            for (uint32_t i = 0; i < req->dab_count; ++i) {
                double x0 = fmin((double)dabs[i].ax - (double)dabs[i].radius_a,
                                 (double)dabs[i].bx - (double)dabs[i].radius_b);
                double y0 = fmin((double)dabs[i].ay - (double)dabs[i].radius_a,
                                 (double)dabs[i].by - (double)dabs[i].radius_b);
                double x1 = fmax((double)dabs[i].ax + (double)dabs[i].radius_a,
                                 (double)dabs[i].bx + (double)dabs[i].radius_b);
                double y1 = fmax((double)dabs[i].ay + (double)dabs[i].radius_a,
                                 (double)dabs[i].by + (double)dabs[i].radius_b);
                if (x0 < min_x)
                    min_x = x0;
                if (y0 < min_y)
                    min_y = y0;
                if (x1 > max_x)
                    max_x = x1;
                if (y1 > max_y)
                    max_y = y1;
            }
            resp->dirty_rect.x = authoring_clamp_double_to_i32(floor(min_x));
            resp->dirty_rect.y = authoring_clamp_double_to_i32(floor(min_y));
            double span_w = ceil(max_x - min_x);
            double span_h = ceil(max_y - min_y);
            resp->dirty_rect.width = authoring_clamp_double_to_i32(span_w > 0.0 ? span_w : 0.0);
            resp->dirty_rect.height = authoring_clamp_double_to_i32(span_h > 0.0 ? span_h : 0.0);
        }
        resp->status_code = WAYWAL_STATUS_OK;
        return true;
    }

#if WAYWAL_HAVE_GPU_BRUSH
    we_scene_t *target_sc = NULL;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (authoring_matches_target(out, req->target_output) && out->we_scene) {
            target_sc = out->we_scene;
            break;
        }
    }
    if (!target_sc || !target_sc->gl_initialized) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    authoring_ensure_scene_egl(target_sc);
    if (!g_authoring_brush_ctx) {
        g_authoring_brush_ctx = we_gpu_brush_create(WE_BRUSH_POOL_DEFAULT_CAPS);
        if (!g_authoring_brush_ctx) {
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    bool ok = false;
    switch (req->action) {
    case WE_BRUSH_IPC_BIND_TARGET:
        ok = we_gpu_brush_bind_target(g_authoring_brush_ctx, target_sc, req->object_id,
                                      req->pass_index, req->tex_slot);
        break;
    case WE_BRUSH_IPC_BEGIN_STROKE:
        ok = we_gpu_brush_begin_stroke(g_authoring_brush_ctx, req->stroke_id, &req->params);
        break;
    case WE_BRUSH_IPC_DAB_BATCH:
        ok = we_gpu_brush_dispatch_dabs(g_authoring_brush_ctx, dabs, req->dab_count,
                                        &resp->dirty_rect);
        break;
    case WE_BRUSH_IPC_END_STROKE:
        ok = we_gpu_brush_end_stroke(g_authoring_brush_ctx, &resp->captured_tile_count);
        break;
    case WE_BRUSH_IPC_UNDO_STROKE:
        ok = we_gpu_brush_undo_stroke(g_authoring_brush_ctx, req->stroke_id);
        break;
    case WE_BRUSH_IPC_REDO_STROKE:
        ok = we_gpu_brush_redo_stroke(g_authoring_brush_ctx, req->stroke_id);
        break;
    default:
        ok = false;
        break;
    }
    we_gpu_brush_get_stats(g_authoring_brush_ctx, &resp->stats);
    resp->status_code = ok ? WAYWAL_STATUS_OK : WAYWAL_STATUS_ERR_SYNTAX;
    return ok;
#else
    resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
    return false;
#endif
}

bool daemon_dispatch_ipc_req_particle_config(daemon_state_t *state,
                                             const waywal_ipc_req_particle_config_t *req,
                                             const void *payload,
                                             waywal_ipc_resp_particle_config_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->config_op > (uint32_t)WE_PARTICLE_CFG_PAUSE_TOGGLE ||
        !memchr(req->target_output, '\0', sizeof(req->target_output))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    resp->object_id = req->object_id;

    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        return true;
    }

    uint64_t t0 = now_ns();
    we_scene_t *visited[64];
    size_t visited_count = 0u;
    bool applied = false;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!authoring_matches_target(out, req->target_output) || !out->we_scene) {
            continue;
        }
        if (scene_already_visited(visited, visited_count, out->we_scene)) {
            continue;
        }
        if (visited_count < 64u) {
            visited[visited_count++] = out->we_scene;
        }
        we_scene_object_t *obj = authoring_find_object(out->we_scene, req->object_id, NULL);
        if (!obj) {
            continue;
        }
        if (req->config_op == WE_PARTICLE_CFG_SET_SYSTEM_JSON) {
            if (!payload || req->payload_len == 0u) {
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                return false;
            }
            we_particle_sys_t *next_ps =
                we_particle_create_from_json((const char *)payload, req->payload_len);
            if (!next_ps) {
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                return false;
            }
            if (obj->particle_sys) {
                we_particle_destroy(obj->particle_sys);
            }
            obj->particle_sys = next_ps;
        } else if (req->config_op == WE_PARTICLE_CFG_APPLY_PRESET) {
            we_particle_sys_t *next_ps = we_particle_create_preset(
                (we_particle_preset_id_t)req->preset_id, (req->flags & 0x2u) != 0u);
            if (!next_ps) {
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                return false;
            }
            next_ps->paused = (req->flags & 0x1u) != 0u;
            if (obj->particle_sys) {
                we_particle_destroy(obj->particle_sys);
            }
            obj->particle_sys = next_ps;
        } else {
            we_particle_sys_t *ps = obj->particle_sys;
            if (!ps) {
                continue;
            }
            if (req->config_op == WE_PARTICLE_CFG_PAUSE_TOGGLE) {
                ps->paused = !ps->paused;
            } else if (req->config_op == WE_PARTICLE_CFG_TRIGGER_BURST) {
                uint32_t burst_count = req->_pad[0];
                if (burst_count == 0u) {
                    burst_count = req->preset_id;
                }
                if (burst_count == 0u) {
                    burst_count = (ps->emitter_count > 0u && ps->emitters[0].instantaneous > 0u)
                                      ? ps->emitters[0].instantaneous
                                      : 16u;
                }
                if (burst_count > WE_PARTICLE_MAX_CAP) {
                    burst_count = WE_PARTICLE_MAX_CAP;
                }
                we_particle_trigger_burst(ps, burst_count);
            } else if (req->config_op == WE_PARTICLE_CFG_SET_RAMP_LUT) {
                if (!payload || req->payload_len != sizeof(we_particle_ramp_lut_t)) {
                    resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                    return false;
                }
                we_particle_ramp_lut_t lut;
                memcpy(&lut, payload, sizeof(lut));
                if (!we_particle_apply_ramp_lut(ps, &lut)) {
                    resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                    return false;
                }
            }
        }

        if (obj->particle_sys) {
            resp->alive_count = obj->particle_sys->alive_count;
            resp->max_count = obj->particle_sys->max_count;
        }
        applied = true;
    }

    resp->sim_time_us = (uint32_t)((now_ns() - t0) / 1000ULL);
    resp->status_code = applied ? WAYWAL_STATUS_OK : WAYWAL_STATUS_ERR_SYNTAX;
    return applied;
}

bool daemon_dispatch_ipc_req_set_audio_bindings(daemon_state_t *state,
                                                const waywal_ipc_req_set_audio_bindings_t *req,
                                                const we_audio_binding_t *bindings,
                                                waywal_ipc_resp_set_audio_bindings_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->bind_op > (uint32_t)WE_AUDIO_BIND_OP_CLEAR ||
        req->binding_count > WE_MAX_AUDIO_BINDINGS ||
        !memchr(req->target_output, '\0', sizeof(req->target_output))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    if (req->binding_count > 0u && !bindings) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        resp->active_binding_count = req->binding_count;
        return true;
    }

    uint64_t t0 = now_ns();
    we_scene_t *visited[64];
    size_t visited_count = 0u;
    bool found = false;
    uint32_t last_active = 0u;
    uint64_t last_eval_ns = 0u;

    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!authoring_matches_target(out, req->target_output) || !out->we_scene) {
            continue;
        }
        if (scene_already_visited(visited, visited_count, out->we_scene)) {
            continue;
        }
        if (visited_count < 64u) {
            visited[visited_count++] = out->we_scene;
        }
        we_scene_t *sc = out->we_scene;
        if (req->bind_op == WE_AUDIO_BIND_OP_UPSERT) {
            uint32_t new_slots = 0u;
            for (uint32_t k = 0; k < req->binding_count; ++k) {
                bool exists = false;
                for (uint32_t i = 0; i < sc->audio_binding_count; ++i) {
                    if (sc->audio_bindings[i].binding_id == bindings[k].binding_id) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    for (uint32_t prev = 0; prev < k; ++prev) {
                        if (bindings[prev].binding_id == bindings[k].binding_id) {
                            exists = true;
                            break;
                        }
                    }
                }
                if (!exists) {
                    new_slots++;
                }
            }
            if (sc->audio_binding_count + new_slots > WE_MAX_AUDIO_BINDINGS) {
                resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
                return false;
            }
        }
        if (!we_scene_apply_audio_bindings(sc, (we_audio_bind_op_t)req->bind_op, bindings,
                                           req->binding_count, req->remove_id)) {
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
        authoring_scene_slot_t *slot = authoring_get_scene_slot(sc, true);
        if (slot) {
            slot->revision++;
        }
        found = true;
        last_active = sc->audio_binding_count;
        last_eval_ns = (uint64_t)sc->last_audio_bindings_eval_ns;
    }

    uint64_t eval_ns = last_eval_ns ? last_eval_ns : (now_ns() - t0);
    resp->eval_time_ns = eval_ns > UINT32_MAX ? UINT32_MAX : (uint32_t)eval_ns;
    resp->active_binding_count = last_active;
    resp->status_code = found ? WAYWAL_STATUS_OK : WAYWAL_STATUS_ERR_SYNTAX;
    return found;
}

#if WAYWAL_HAVE_ASSET_WRITER
static void authoring_sum_export_dir(const char *dir_path, uint32_t *out_files, uint64_t *out_bytes,
                                     uint32_t depth)
{
    if (!dir_path || depth >= 16u) {
        return;
    }
    DIR *dp = opendir(dir_path);
    if (!dp) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char sub[4096];
        int n = snprintf(sub, sizeof(sub), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (lstat(sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            authoring_sum_export_dir(sub, out_files, out_bytes, depth + 1u);
        } else if (S_ISREG(st.st_mode)) {
            (*out_files)++;
            if (st.st_size > 0) {
                *out_bytes += (uint64_t)st.st_size;
            }
        }
    }
    closedir(dp);
}

static void authoring_rm_tree(const char *path, uint32_t depth)
{
    if (!path || depth >= 16u) {
        return;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        (void)unlink(path);
        return;
    }
    DIR *dp = opendir(path);
    if (dp) {
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char sub[PATH_MAX];
            int n = snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
            if (n > 0 && (size_t)n < sizeof(sub)) {
                authoring_rm_tree(sub, depth + 1u);
            }
        }
        closedir(dp);
    }
    (void)rmdir(path);
}

static bool authoring_mkdir_p(const char *dir)
{
    if (!dir || dir[0] != '/') {
        return false;
    }
    char tmp[PATH_MAX];
    size_t len = strnlen(dir, sizeof(tmp));
    if (len <= 1u || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, dir, len + 1u);
    while (len > 1u && tmp[len - 1u] == '/') {
        tmp[--len] = '\0';
    }
    for (size_t i = 1u; i <= len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            struct stat st;
            if (lstat(tmp, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    return false;
                }
            } else if (errno != ENOENT || mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST) {
                    return false;
                }
            }
            tmp[i] = saved;
        }
    }
    return true;
}

static bool authoring_move_dir_contents(const char *src_dir, const char *dst_dir, uint32_t depth)
{
    if (depth >= 16u || !authoring_mkdir_p(dst_dir)) {
        return false;
    }
    DIR *dp = opendir(src_dir);
    if (!dp) {
        return false;
    }
    bool ok = true;
    struct dirent *ent;
    while (ok && (ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char s_sub[PATH_MAX];
        char d_sub[PATH_MAX];
        int sn = snprintf(s_sub, sizeof(s_sub), "%s/%s", src_dir, ent->d_name);
        int dn = snprintf(d_sub, sizeof(d_sub), "%s/%s", dst_dir, ent->d_name);
        if (sn <= 0 || (size_t)sn >= sizeof(s_sub) || dn <= 0 || (size_t)dn >= sizeof(d_sub)) {
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(s_sub, &st) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = authoring_move_dir_contents(s_sub, d_sub, depth + 1u);
        } else if (S_ISREG(st.st_mode)) {
            if (rename(s_sub, d_sub) != 0) {
                ok = false;
            }
        }
    }
    closedir(dp);
    return ok;
}
#endif

bool daemon_dispatch_ipc_req_export_scene_pkg(daemon_state_t *state,
                                              const waywal_ipc_req_export_scene_pkg_t *req,
                                              waywal_ipc_resp_export_scene_pkg_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->dest_dir_or_pkg[0] == '\0' ||
        !memchr(req->target_output, '\0', sizeof(req->target_output)) ||
        !memchr(req->dest_dir_or_pkg, '\0', sizeof(req->dest_dir_or_pkg))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Invalid export request");
        return false;
    }
    if ((req->flags & WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE) != 0u &&
        (req->flags & WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG) == 0u) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Direct package export requires pack flag");
        return false;
    }
    char resolved_dest[PATH_MAX];
    if (!waywal_ipc_export_path_is_allowed(req->dest_dir_or_pkg, resolved_dest,
                                           sizeof(resolved_dest))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Export destination outside allowlisted directory");
        return false;
    }

    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        return true;
    }

    uint64_t t0 = now_ns();
    we_scene_t *sc = NULL;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (authoring_matches_target(out, req->target_output) && out->we_scene) {
            sc = out->we_scene;
            break;
        }
    }
    if (!sc) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "No active scene matching target output");
        return false;
    }

#if WAYWAL_HAVE_ASSET_WRITER
    char parent_dir[PATH_MAX];
    memcpy(parent_dir, resolved_dest, strlen(resolved_dest) + 1u);
    char *slash = strrchr(parent_dir, '/');
    if (!slash || slash == parent_dir) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Invalid export parent path");
        return false;
    }
    *slash = '\0';
    if (!authoring_mkdir_p(parent_dir)) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Failed to create export parent directory");
        return false;
    }

    char stage_dir[PATH_MAX];
    int sn = snprintf(stage_dir, sizeof(stage_dir), "%s/.wwal-export-stage-XXXXXX", parent_dir);
    if (sn <= 0 || (size_t)sn >= sizeof(stage_dir) || !mkdtemp(stage_dir)) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Failed to create staging directory");
        return false;
    }

    bool pack_pkg = (req->flags & WE_EXPORT_PKG_FLAG_PACK_SCENE_PKG) != 0u;
    bool direct_pkg = (req->flags & WE_EXPORT_PKG_FLAG_DIRECT_PKG_FILE) != 0u;
    we_asset_writer_err_t werr = WE_WRITER_OK;
    bool ok = we_scene_export_dir(sc, stage_dir, pack_pkg, &werr);
    resp->export_time_us = (now_ns() - t0) / 1000ULL;
    if (!ok) {
        authoring_rm_tree(stage_dir, 0u);
        resp->status_code = (uint32_t)werr;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                            "Scene package export failed");
        return false;
    }

    uint32_t file_count = 0u;
    uint64_t total_bytes = 0u;
    authoring_sum_export_dir(stage_dir, &file_count, &total_bytes, 0u);
    uint32_t entries = file_count;
    if (direct_pkg) {
        char stage_pkg[PATH_MAX];
        int pn = snprintf(stage_pkg, sizeof(stage_pkg), "%s/scene.pkg", stage_dir);
        struct stat pkg_st;
        if (pn <= 0 || (size_t)pn >= sizeof(stage_pkg) || lstat(stage_pkg, &pkg_st) != 0 ||
            !S_ISREG(pkg_st.st_mode)) {
            authoring_rm_tree(stage_dir, 0u);
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "Export did not produce scene.pkg");
            return false;
        }
        uint32_t pkg_entries = 1u;
        we_pkg_t *pkg = we_pkg_open_file(stage_pkg, stage_dir);
        if (pkg) {
            pkg_entries = pkg->entry_count > 0u ? pkg->entry_count : 1u;
            we_pkg_close(pkg);
        }
        if (rename(stage_pkg, resolved_dest) != 0) {
            authoring_rm_tree(stage_dir, 0u);
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "Failed to finalize exported scene.pkg");
            return false;
        }
        authoring_rm_tree(stage_dir, 0u);
        resp->status_code = WAYWAL_STATUS_OK;
        resp->entry_count = pkg_entries;
        resp->total_bytes = pkg_st.st_size > 0 ? (uint64_t)pkg_st.st_size : 0ULL;
        return true;
    }

    if (pack_pkg) {
        char pkg_path[PATH_MAX];
        int pn = snprintf(pkg_path, sizeof(pkg_path), "%s/scene.pkg", stage_dir);
        if (pn > 0 && (size_t)pn < sizeof(pkg_path)) {
            we_pkg_t *pkg = we_pkg_open_file(pkg_path, stage_dir);
            if (pkg) {
                entries = (file_count > 0u ? (file_count - 1u) : 0u) + pkg->entry_count;
                we_pkg_close(pkg);
            }
        }
    }
    if (rename(stage_dir, resolved_dest) != 0) {
        if (!authoring_move_dir_contents(stage_dir, resolved_dest, 0u)) {
            authoring_rm_tree(stage_dir, 0u);
            resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg),
                                "Failed to finalize export directory");
            return false;
        }
        authoring_rm_tree(stage_dir, 0u);
    }
    resp->status_code = WAYWAL_STATUS_OK;
    resp->entry_count = entries;
    resp->total_bytes = total_bytes;
    return true;
#else
    (void)t0;
    resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
    authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Asset exporter unavailable");
    return false;
#endif
}

bool daemon_dispatch_ipc_req_lint_scenescript(daemon_state_t *state,
                                              const waywal_ipc_req_lint_scenescript_t *req,
                                              const char *source_utf8,
                                              waywal_ipc_resp_lint_scenescript_t *resp)
{
    (void)state;
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->source_len > WAYWAL_IPC_MAX_CODE_BYTES) {
        resp->status_code = 1u;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Invalid lint request size");
        return false;
    }
    if (req->source_len > 0u && !source_utf8) {
        resp->status_code = 1u;
        authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), "Missing SceneScript body");
        return false;
    }

    uint64_t t0 = now_ns();
    we_script_diag_t diag;
    memset(&diag, 0, sizeof(diag));
    bool ok = we_script_lint(source_utf8 ? source_utf8 : "", req->source_len, req->flags, &diag);
    resp->status_code = ok ? WAYWAL_STATUS_OK : (diag.status_code ? diag.status_code : 1u);
    resp->error_line = diag.error_line;
    resp->error_col = diag.error_col;
    resp->lint_time_us =
        diag.lint_time_us ? diag.lint_time_us : (uint32_t)((now_ns() - t0) / 1000ULL);
    authoring_copy_cstr(resp->error_msg, sizeof(resp->error_msg), diag.error_msg);
    return ok;
}

static uint8_t *authoring_copy_rgba8_unswiz(const we_tex_mip_t *m0, size_t *out_len)
{
    if (!m0 || !m0->rgba8 || m0->width == 0u || m0->height == 0u) {
        return NULL;
    }
    size_t w = (size_t)m0->width;
    if ((size_t)m0->height > SIZE_MAX / w) {
        return NULL;
    }
    size_t npx = w * (size_t)m0->height;
    if (npx > WAYWAL_IPC_MAX_READBACK_BYTES / 4u) {
        return NULL;
    }
    size_t len = npx * 4u;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, m0->rgba8, len);
    if (m0->is_bgra_swiz) {
        for (size_t i = 0u; i + 3u < len; i += 4u) {
            uint8_t tmp = buf[i];
            buf[i] = buf[i + 2u];
            buf[i + 2u] = tmp;
        }
    }
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

static we_tex_t *authoring_readback_resolve_tex(daemon_state_t *state,
                                                const waywal_ipc_req_readback_texture_t *req,
                                                we_scene_t **out_sc)
{
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        if (!authoring_matches_target(out, req->target_output) || !out->we_scene) {
            continue;
        }
        we_scene_object_t *obj = authoring_find_object(out->we_scene, req->object_id, NULL);
        we_material_pass_t *pass = authoring_resolve_pass(obj, req->pass_index);
        if (pass && req->tex_slot < WE_MAX_PASS_TEXTURES && pass->textures[req->tex_slot]) {
            if (out_sc) {
                *out_sc = out->we_scene;
            }
            return pass->textures[req->tex_slot];
        }
    }
    return NULL;
}

static uint8_t *authoring_readback_try_vram(we_scene_t *target_sc, const we_tex_t *tex,
                                            size_t *out_len, we_tex_format_t *out_fmt,
                                            uint32_t *out_w, uint32_t *out_h)
{
#if WAYWAL_HAVE_GPU_BRUSH
    if (!g_authoring_brush_ctx || tex->gl_tex == 0u) {
        return NULL;
    }
    we_brush_stats_t bstats;
    memset(&bstats, 0, sizeof(bstats));
    we_gpu_brush_get_stats(g_authoring_brush_ctx, &bstats);
    if (bstats.target_gl_tex != tex->gl_tex) {
        return NULL;
    }
    authoring_ensure_scene_egl(target_sc);
    uint8_t *vram_px = NULL;
    size_t vram_len = 0u;
    if (!we_gpu_brush_readback_texels(g_authoring_brush_ctx, (uint32_t)tex->format, &vram_px,
                                      &vram_len) ||
        !vram_px || vram_len == 0u) {
        return NULL;
    }
    uint32_t cw = bstats.canvas_width ? bstats.canvas_width : *out_w;
    uint32_t ch = bstats.canvas_height ? bstats.canvas_height : *out_h;
    size_t npix = (size_t)cw * (size_t)ch;
    if (npix > 0u) {
        if (vram_len == npix) {
            *out_fmt = WE_TEX_FMT_R8;
        } else if (vram_len == npix * 2u) {
            *out_fmt = WE_TEX_FMT_RG88;
        } else if (vram_len == npix * 4u) {
            *out_fmt = WE_TEX_FMT_RGBA8888;
        }
    }
    *out_len = vram_len;
    *out_w = cw;
    *out_h = ch;
    return vram_px;
#else
    (void)target_sc;
    (void)tex;
    (void)out_len;
    (void)out_fmt;
    (void)out_w;
    (void)out_h;
    return NULL;
#endif
}

static uint8_t *authoring_readback_raw(we_tex_t *tex, const we_tex_mip_t *m0, uint8_t **raw_px,
                                       size_t raw_len, we_tex_format_t raw_fmt, uint32_t raw_w,
                                       uint32_t raw_h, waywal_ipc_resp_readback_texture_t *resp,
                                       size_t *out_len)
{
    if (*raw_px) {
        uint8_t *buf = *raw_px;
        *raw_px = NULL;
        *out_len = raw_len;
        resp->tex_format = (uint32_t)raw_fmt;
        resp->width = raw_w;
        resp->height = raw_h;
        return buf;
    }
    if (m0 && m0->rgba8 && m0->width > 0u && m0->height > 0u) {
        uint8_t *buf = authoring_copy_rgba8_unswiz(m0, out_len);
        if (buf) {
            resp->tex_format = (uint32_t)WE_TEX_FMT_RGBA8888;
            resp->width = m0->width;
            resp->height = m0->height;
        }
        return buf;
    }
    uint8_t *dec_buf = NULL;
    uint32_t dw = 0u;
    uint32_t dh = 0u;
    if (we_tex_decode_rgba8(tex, &dec_buf, &dw, &dh) && dec_buf && dw > 0u && dh > 0u) {
        size_t npx = (size_t)dw * (size_t)dh;
        if (npx <= WAYWAL_IPC_MAX_READBACK_BYTES / 4u) {
            *out_len = npx * 4u;
            resp->tex_format = (uint32_t)WE_TEX_FMT_RGBA8888;
            resp->width = dw;
            resp->height = dh;
            return dec_buf;
        }
        free(dec_buf);
    }
    return NULL;
}

static uint8_t *authoring_readback_encode(we_tex_t *tex, const we_tex_mip_t *m0, uint8_t **raw_px,
                                          size_t raw_len, we_tex_format_t raw_fmt, uint32_t raw_w,
                                          uint32_t raw_h, waywal_ipc_resp_readback_texture_t *resp,
                                          size_t *out_len)
{
#if WAYWAL_HAVE_ASSET_WRITER
    const uint8_t *enc_src = NULL;
    size_t enc_src_len = 0u;
    uint8_t *owned_src = NULL;
    we_tex_format_t enc_fmt = WE_TEX_FMT_RGBA8888;
    uint32_t enc_w = 0u;
    uint32_t enc_h = 0u;

    if (*raw_px && (raw_fmt == WE_TEX_FMT_RGBA8888 || raw_fmt == WE_TEX_FMT_RG88 ||
                    raw_fmt == WE_TEX_FMT_R8)) {
        enc_src = *raw_px;
        enc_src_len = raw_len;
        enc_fmt = raw_fmt;
        enc_w = raw_w;
        enc_h = raw_h;
    } else if (m0 &&
               (tex->format == WE_TEX_FMT_RGBA8888 || tex->format == WE_TEX_FMT_RG88 ||
                tex->format == WE_TEX_FMT_R8) &&
               m0->data && m0->data_size > 0u && m0->width > 0u && m0->height > 0u) {
        size_t bpp =
            (tex->format == WE_TEX_FMT_R8) ? 1u : (tex->format == WE_TEX_FMT_RG88 ? 2u : 4u);
        if (m0->data_size == (size_t)m0->width * (size_t)m0->height * bpp) {
            enc_src = m0->data;
            enc_src_len = m0->data_size;
            enc_fmt = tex->format;
            enc_w = m0->width;
            enc_h = m0->height;
        }
    }
    if (!enc_src && m0 && m0->rgba8 && m0->width > 0u && m0->height > 0u) {
        size_t len = 0u;
        owned_src = authoring_copy_rgba8_unswiz(m0, &len);
        if (owned_src) {
            enc_src = owned_src;
            enc_src_len = len;
            enc_fmt = WE_TEX_FMT_RGBA8888;
            enc_w = m0->width;
            enc_h = m0->height;
        }
    }
    if (!enc_src) {
        uint8_t *dec_buf = NULL;
        uint32_t dw = 0u;
        uint32_t dh = 0u;
        if (we_tex_decode_rgba8(tex, &dec_buf, &dw, &dh) && dec_buf && dw > 0u && dh > 0u) {
            owned_src = dec_buf;
            enc_src = owned_src;
            enc_src_len = (size_t)dw * (size_t)dh * 4u;
            enc_fmt = WE_TEX_FMT_RGBA8888;
            enc_w = dw;
            enc_h = dh;
        }
    }
    uint8_t *out_buf = NULL;
    if (enc_src && enc_src_len > 0u && enc_w > 0u && enc_h > 0u) {
        we_tex_encode_mip_t mip = {
            .width = enc_w,
            .height = enc_h,
            .pixels = enc_src,
            .byte_size = enc_src_len,
        };
        we_tex_encode_image_t img = {.mip_count = 1u, .mips = &mip};
        we_tex_encode_spec_t spec;
        memset(&spec, 0, sizeof(spec));
        spec.format = enc_fmt;
        spec.flags = tex->flags;
        if ((spec.flags & WE_TEX_FLAG_IS_GIF) != 0u && (!tex->frames || tex->frame_count == 0u)) {
            spec.flags &= ~(uint32_t)WE_TEX_FLAG_IS_GIF;
        }
        spec.texture_width = enc_w;
        spec.texture_height = enc_h;
        spec.image_width =
            (tex->image_width > 0u && tex->image_width <= enc_w) ? tex->image_width : enc_w;
        spec.image_height =
            (tex->image_height > 0u && tex->image_height <= enc_h) ? tex->image_height : enc_h;
        spec.enable_lz4 = true;
        spec.image_count = 1u;
        spec.images = &img;
        spec.frame_count = tex->frame_count;
        spec.gif_width = tex->gif_width;
        spec.gif_height = tex->gif_height;
        spec.frames = tex->frames;
        we_asset_writer_err_t werr = WE_WRITER_OK;
        if (we_tex_encode_v5(&spec, &out_buf, out_len, &werr)) {
            resp->tex_format = (uint32_t)enc_fmt;
            resp->width = spec.image_width;
            resp->height = spec.image_height;
        }
    }
    free(owned_src);
    return out_buf;
#else
    (void)tex;
    if (*raw_px) {
        uint8_t *buf = *raw_px;
        *raw_px = NULL;
        *out_len = raw_len;
        resp->tex_format = (uint32_t)raw_fmt;
        resp->width = raw_w;
        resp->height = raw_h;
        return buf;
    }
    if (m0 && m0->data && m0->data_size > 0u && m0->data_size <= WAYWAL_IPC_MAX_READBACK_BYTES) {
        uint8_t *buf = (uint8_t *)malloc(m0->data_size);
        if (buf) {
            memcpy(buf, m0->data, m0->data_size);
            *out_len = m0->data_size;
        }
        return buf;
    }
    return NULL;
#endif
}

bool daemon_dispatch_ipc_req_readback_texture(daemon_state_t *state,
                                              const waywal_ipc_req_readback_texture_t *req,
                                              waywal_ipc_resp_readback_texture_t *resp,
                                              uint8_t **out_data, uint32_t *out_data_len)
{
    if (out_data) {
        *out_data = NULL;
    }
    if (out_data_len) {
        *out_data_len = 0u;
    }
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || req->format_mode > 1u ||
        !memchr(req->target_output, '\0', sizeof(req->target_output))) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    if (!state) {
        resp->status_code = WAYWAL_STATUS_OK;
        return true;
    }

    we_scene_t *target_sc = NULL;
    we_tex_t *tex = authoring_readback_resolve_tex(state, req, &target_sc);
    if (!tex) {
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    resp->tex_format = (uint32_t)tex->format;
    resp->width = tex->image_width ? tex->image_width : tex->texture_width;
    resp->height = tex->image_height ? tex->image_height : tex->texture_height;
    resp->flags = tex->flags;

    size_t raw_len = 0u;
    we_tex_format_t raw_fmt = tex->format;
    uint32_t raw_w = resp->width;
    uint32_t raw_h = resp->height;
    uint8_t *raw_px =
        authoring_readback_try_vram(target_sc, tex, &raw_len, &raw_fmt, &raw_w, &raw_h);

    const we_tex_mip_t *m0 = (tex->images && tex->image_count > 0u && tex->images[0].mip_count > 0u)
                                 ? &tex->images[0].mips[0]
                                 : NULL;

    size_t out_len = 0u;
    uint8_t *out_buf = (req->format_mode == 0u)
                           ? authoring_readback_raw(tex, m0, &raw_px, raw_len, raw_fmt, raw_w,
                                                    raw_h, resp, &out_len)
                           : authoring_readback_encode(tex, m0, &raw_px, raw_len, raw_fmt, raw_w,
                                                       raw_h, resp, &out_len);

    free(raw_px);
    if (!out_buf || out_len == 0u || out_len > WAYWAL_IPC_MAX_READBACK_BYTES) {
        free(out_buf);
        resp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    resp->data_len = (uint32_t)out_len;
    resp->status_code = WAYWAL_STATUS_OK;
    if (out_data) {
        *out_data = out_buf;
    } else {
        free(out_buf);
    }
    if (out_data_len) {
        *out_data_len = (uint32_t)out_len;
    }
    return true;
}

static bool authoring_send_msg(int client_fd, const waywal_ipc_hdr_t *hdr, const void *payload,
                               size_t payload_len)
{
    if (!ipc_dispatch_write_full(client_fd, hdr, sizeof(*hdr), 500)) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }
    if (payload_len > 0u && !ipc_dispatch_write_full(client_fd, payload, payload_len, 500)) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }
    return true;
}

static bool authoring_read_var_packet(int client_fd, uint64_t total_payload_size, size_t fixed_size,
                                      size_t max_total_size, uint8_t **out_buf, size_t *out_len)
{
    if (total_payload_size < fixed_size || total_payload_size > max_total_size) {
        (void)authoring_drain_payload(client_fd, total_payload_size, 500);
        return false;
    }
    size_t len = (size_t)total_payload_size;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        (void)authoring_drain_payload(client_fd, total_payload_size, 500);
        return false;
    }
    if (!ipc_dispatch_read_full(client_fd, buf, len, 500)) {
        free(buf);
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }
    *out_buf = buf;
    *out_len = len;
    return true;
}

static bool authoring_ipc_handle_scene_mutate(daemon_state_t *state, int client_fd,
                                              const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_scene_mutate_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!authoring_read_var_packet(
            client_fd, hdr->payload_size, sizeof(waywal_ipc_req_scene_mutate_t),
            sizeof(waywal_ipc_req_scene_mutate_t) + WAYWAL_IPC_MAX_MUTATE_PAYLOAD_BYTES, &pkt,
            &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_scene_mutate(WAYWAL_STATUS_ERR_SYNTAX, 0, 0u, 0u,
                                               "Invalid scene mutate packet size", &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_scene_mutate_t *req = (const waywal_ipc_req_scene_mutate_t *)pkt;
    const char *payload_json = NULL;
    if (!waywal_ipc_validate_req_scene_mutate(req, pkt_len, &payload_json)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_scene_mutate(WAYWAL_STATUS_ERR_SYNTAX, 0, 0u, 0u,
                                               "Invalid scene mutate payload", &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    char *json_copy = NULL;
    if (payload_json && req->payload_len > 0u) {
        json_copy = (char *)malloc((size_t)req->payload_len + 1u);
        if (json_copy) {
            memcpy(json_copy, payload_json, req->payload_len);
            json_copy[req->payload_len] = '\0';
        }
    }
    daemon_dispatch_ipc_req_scene_mutate(state, req, json_copy, &resp);
    free(json_copy);
    free(pkt);
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_scene_mutate(resp.status_code, resp.object_id, resp.scene_revision,
                                           resp.object_count, resp.error_msg, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_hot_reload(daemon_state_t *state, int client_fd,
                                            const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_hot_reload_code_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!authoring_read_var_packet(
            client_fd, hdr->payload_size, sizeof(waywal_ipc_req_hot_reload_code_t),
            sizeof(waywal_ipc_req_hot_reload_code_t) + WAYWAL_IPC_MAX_CODE_BYTES, &pkt, &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_hot_reload_code(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u, 0u, 0u,
                                                  "Invalid hot-reload packet size", NULL, 0u, &rh,
                                                  &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_hot_reload_code_t *req = (const waywal_ipc_req_hot_reload_code_t *)pkt;
    const char *src_utf8 = NULL;
    if (!waywal_ipc_validate_req_hot_reload_code(req, pkt_len, &src_utf8)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_hot_reload_code(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u, 0u, 0u,
                                                  "Invalid hot-reload payload", NULL, 0u, &rh,
                                                  &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    char *meta_json = NULL;
    size_t meta_len = 0u;
    daemon_dispatch_ipc_req_hot_reload_code(state, req, src_utf8, &resp, &meta_json, &meta_len);
    free(pkt);
    size_t out_cap = sizeof(resp) + meta_len;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    waywal_ipc_hdr_t rh;
    size_t out_len = 0u;
    bool ser_ok =
        out_buf && waywal_ipc_serialize_resp_hot_reload_code(
                       resp.status_code, resp.error_line, resp.error_col, resp.uniform_count,
                       resp.combo_count, resp.compile_time_us, resp.error_msg, meta_json, meta_len,
                       &rh, &resp, out_buf, out_cap, &out_len);
    free(meta_json);
    if (!ser_ok) {
        free(out_buf);
        waywal_ipc_serialize_resp_hot_reload_code(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u, 0u, 0u,
                                                  "Failed to serialize hot-reload response", NULL,
                                                  0u, &rh, &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

static bool authoring_ipc_handle_query_tree(daemon_state_t *state, int client_fd,
                                            const waywal_ipc_hdr_t *hdr)
{
    waywal_ipc_resp_query_scene_tree_t resp;
    memset(&resp, 0, sizeof(resp));
    if (hdr->payload_size != sizeof(waywal_ipc_req_query_scene_tree_t)) {
        if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
            return false;
        }
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_query_scene_tree(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, NULL, 0u, &rh,
                                                   &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    waywal_ipc_req_query_scene_tree_t req;
    if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }
    char *tree_json = NULL;
    size_t tree_len = 0u;
    if (waywal_ipc_validate_req_query_scene_tree(&req, sizeof(req))) {
        daemon_dispatch_ipc_req_query_scene_tree(state, &req, &resp, &tree_json, &tree_len);
    } else {
        resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
    }
    size_t out_cap = sizeof(resp) + tree_len;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    waywal_ipc_hdr_t rh;
    size_t out_len = 0u;
    bool ser_ok = out_buf && waywal_ipc_serialize_resp_query_scene_tree(
                                 resp.status_code, resp.scene_revision, resp.object_count,
                                 tree_json, tree_len, &rh, &resp, out_buf, out_cap, &out_len);
    free(tree_json);
    if (!ser_ok) {
        free(out_buf);
        waywal_ipc_serialize_resp_query_scene_tree(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, NULL, 0u, &rh,
                                                   &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

static bool authoring_ipc_handle_shader_combo(daemon_state_t *state, int client_fd,
                                              const waywal_ipc_hdr_t *hdr)
{
    waywal_ipc_resp_set_shader_combo_t resp;
    memset(&resp, 0, sizeof(resp));
    if (hdr->payload_size != sizeof(waywal_ipc_req_set_shader_combo_t)) {
        if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
            return false;
        }
        resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp.error_msg, sizeof(resp.error_msg), "Invalid combo size");
    } else {
        waywal_ipc_req_set_shader_combo_t req;
        if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        if (!waywal_ipc_validate_req_set_shader_combo(&req, sizeof(req))) {
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp.error_msg, sizeof(resp.error_msg), "Invalid combo payload");
        } else {
            daemon_dispatch_ipc_req_set_shader_combo(state, &req, &resp);
        }
    }
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_set_shader_combo(resp.status_code, resp.recompile_time_us,
                                               resp.error_msg, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_brush_stroke(daemon_state_t *state, int client_fd,
                                              const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_brush_stroke_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t max_sz = sizeof(waywal_ipc_req_brush_stroke_t) +
                    (size_t)WE_BRUSH_MAX_STROKE_DABS * sizeof(we_brush_dab_t);
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_req_brush_stroke_t), max_sz, &pkt, &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_brush_stroke(WAYWAL_STATUS_ERR_SYNTAX, 0ULL, NULL, NULL, 0u, &rh,
                                               &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_brush_stroke_t *req = (const waywal_ipc_req_brush_stroke_t *)pkt;
    const we_brush_dab_t *dabs = NULL;
    uint32_t dab_count = 0u;
    if (!waywal_ipc_validate_req_brush_stroke(req, pkt_len, &dabs, &dab_count)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_brush_stroke(WAYWAL_STATUS_ERR_SYNTAX, 0ULL, NULL, NULL, 0u, &rh,
                                               &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    daemon_dispatch_ipc_req_brush_stroke(state, req, dabs, &resp);
    free(pkt);
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_brush_stroke(resp.status_code, resp.stroke_id, &resp.dirty_rect,
                                           &resp.stats, resp.captured_tile_count, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_particle_config(daemon_state_t *state, int client_fd,
                                                 const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_particle_config_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t max_sz = sizeof(waywal_ipc_req_particle_config_t) + WAYWAL_IPC_MAX_SCENE_JSON;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_req_particle_config_t), max_sz, &pkt,
                                   &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_particle_config(WAYWAL_STATUS_ERR_SYNTAX, 0, 0u, 0u, 0u, &rh,
                                                  &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_particle_config_t *req = (const waywal_ipc_req_particle_config_t *)pkt;
    const void *payload = NULL;
    uint32_t payload_len = 0u;
    if (!waywal_ipc_validate_req_particle_config(req, pkt_len, &payload, &payload_len)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_particle_config(WAYWAL_STATUS_ERR_SYNTAX, 0, 0u, 0u, 0u, &rh,
                                                  &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    daemon_dispatch_ipc_req_particle_config(state, req, payload, &resp);
    free(pkt);
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_particle_config(resp.status_code, resp.object_id, resp.alive_count,
                                              resp.max_count, resp.sim_time_us, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_audio_bindings(daemon_state_t *state, int client_fd,
                                                const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_set_audio_bindings_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t max_sz = sizeof(waywal_ipc_req_set_audio_bindings_t) +
                    (size_t)WE_MAX_AUDIO_BINDINGS * sizeof(we_audio_binding_t);
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_req_set_audio_bindings_t), max_sz, &pkt,
                                   &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_set_audio_bindings(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_set_audio_bindings_t *req =
        (const waywal_ipc_req_set_audio_bindings_t *)pkt;
    const we_audio_binding_t *bindings = NULL;
    uint32_t count = 0u;
    if (!waywal_ipc_validate_req_set_audio_bindings(req, pkt_len, &bindings, &count)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_set_audio_bindings(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    daemon_dispatch_ipc_req_set_audio_bindings(state, req, bindings, &resp);
    free(pkt);
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_set_audio_bindings(resp.status_code, resp.active_binding_count,
                                                 resp.eval_time_ns, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_export_pkg(daemon_state_t *state, int client_fd,
                                            const waywal_ipc_hdr_t *hdr)
{
    waywal_ipc_resp_export_scene_pkg_t resp;
    memset(&resp, 0, sizeof(resp));
    if (hdr->payload_size != sizeof(waywal_ipc_req_export_scene_pkg_t)) {
        if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
            return false;
        }
        resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        authoring_copy_cstr(resp.error_msg, sizeof(resp.error_msg), "Invalid export size");
    } else {
        waywal_ipc_req_export_scene_pkg_t req;
        if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        if (!waywal_ipc_validate_req_export_scene_pkg(&req, sizeof(req))) {
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
            authoring_copy_cstr(resp.error_msg, sizeof(resp.error_msg), "Invalid export payload");
        } else {
            daemon_dispatch_ipc_req_export_scene_pkg(state, &req, &resp);
        }
    }
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_export_scene_pkg(resp.status_code, resp.entry_count, resp.total_bytes,
                                               resp.export_time_us, resp.error_msg, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_lint_script(daemon_state_t *state, int client_fd,
                                             const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_resp_lint_scenescript_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!authoring_read_var_packet(
            client_fd, hdr->payload_size, sizeof(waywal_ipc_req_lint_scenescript_t),
            sizeof(waywal_ipc_req_lint_scenescript_t) + WAYWAL_IPC_MAX_CODE_BYTES, &pkt,
            &pkt_len)) {
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_lint_scenescript(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u,
                                                   "Invalid lint packet size", &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_req_lint_scenescript_t *req = (const waywal_ipc_req_lint_scenescript_t *)pkt;
    const char *src_utf8 = NULL;
    if (!waywal_ipc_validate_req_lint_scenescript(req, pkt_len, &src_utf8)) {
        free(pkt);
        waywal_ipc_hdr_t rh;
        waywal_ipc_serialize_resp_lint_scenescript(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u,
                                                   "Invalid lint payload", &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    daemon_dispatch_ipc_req_lint_scenescript(state, req, src_utf8, &resp);
    free(pkt);
    waywal_ipc_hdr_t rh;
    waywal_ipc_serialize_resp_lint_scenescript(resp.status_code, resp.error_line, resp.error_col,
                                               resp.lint_time_us, resp.error_msg, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool authoring_ipc_handle_readback(daemon_state_t *state, int client_fd,
                                          const waywal_ipc_hdr_t *hdr)
{
    waywal_ipc_resp_readback_texture_t resp;
    memset(&resp, 0, sizeof(resp));
    uint8_t *tex_bytes = NULL;
    uint32_t tex_len = 0u;
    if (hdr->payload_size != sizeof(waywal_ipc_req_readback_texture_t)) {
        if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
            return false;
        }
        resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
    } else {
        waywal_ipc_req_readback_texture_t req;
        if (!ipc_dispatch_read_full(client_fd, &req, sizeof(req), 500)) {
            (void)shutdown(client_fd, SHUT_RDWR);
            return false;
        }
        if (!waywal_ipc_validate_req_readback_texture(&req, sizeof(req))) {
            resp.status_code = WAYWAL_STATUS_ERR_SYNTAX;
        } else {
            daemon_dispatch_ipc_req_readback_texture(state, &req, &resp, &tex_bytes, &tex_len);
        }
    }
    size_t out_cap = sizeof(resp) + (size_t)tex_len;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    waywal_ipc_hdr_t rh;
    size_t out_len = 0u;
    bool ser_ok =
        out_buf && waywal_ipc_serialize_resp_readback_texture(
                       resp.status_code, resp.tex_format, resp.width, resp.height, resp.flags,
                       tex_bytes, tex_len, &rh, &resp, out_buf, out_cap, &out_len);
    free(tex_bytes);
    if (!ser_ok) {
        free(out_buf);
        waywal_ipc_serialize_resp_readback_texture(WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u, 0u, NULL,
                                                   0u, &rh, &resp, NULL, 0u, NULL);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

#define RIG_MAX_PUPPET_SLOTS 128u

typedef struct {
    bool in_use;
    const we_scene_t *scene;
    uint32_t object_id;
    uint64_t last_used_seq;
    waywal_ipc_puppet_heatmap_mode_t heatmap;
    we_rig_pose_ctx_t rig_ctx;
    bool preview_active;
    double preview_time_sec;
    bool playback_paused;
} rig_puppet_slot_t;

static rig_puppet_slot_t g_rig_slots[RIG_MAX_PUPPET_SLOTS];
static uint64_t g_rig_lru_seq = 0u;

static void rig_reset_slot_state(rig_puppet_slot_t *slot, uint32_t object_id)
{
    if (!slot) {
        return;
    }
    memset(&slot->heatmap, 0, sizeof(slot->heatmap));
    slot->heatmap.object_id = object_id;
    slot->heatmap.active_bone_idx = UINT32_MAX;
    slot->heatmap.opacity = 0.65f;
    memset(&slot->rig_ctx, 0, sizeof(slot->rig_ctx));
    slot->preview_active = false;
    slot->preview_time_sec = 0.0;
    slot->playback_paused = false;
}

static void rig_invalidate_scene_slots(const we_scene_t *sc)
{
    if (!sc) {
        return;
    }
    for (size_t i = 0u; i < RIG_MAX_PUPPET_SLOTS; ++i) {
        if (g_rig_slots[i].in_use && g_rig_slots[i].scene == sc) {
            memset(&g_rig_slots[i], 0, sizeof(g_rig_slots[i]));
        }
    }
}

static void rig_clear_all_slots(void)
{
    memset(g_rig_slots, 0, sizeof(g_rig_slots));
    g_rig_lru_seq = 0u;
}

static rig_puppet_slot_t *rig_get_puppet_slot(const we_scene_t *scene, uint32_t object_id,
                                              bool create_if_missing)
{
    rig_puppet_slot_t *free_slot = NULL;
    rig_puppet_slot_t *lru_slot = &g_rig_slots[0];
    for (size_t i = 0u; i < RIG_MAX_PUPPET_SLOTS; ++i) {
        if (g_rig_slots[i].in_use) {
            if (g_rig_slots[i].scene == scene && g_rig_slots[i].object_id == object_id) {
                g_rig_slots[i].last_used_seq = ++g_rig_lru_seq;
                return &g_rig_slots[i];
            }
            if (!lru_slot->in_use || g_rig_slots[i].last_used_seq < lru_slot->last_used_seq) {
                lru_slot = &g_rig_slots[i];
            }
        } else if (!free_slot) {
            free_slot = &g_rig_slots[i];
        }
    }
    if (!create_if_missing) {
        return NULL;
    }
    if (!free_slot) {
        free_slot = lru_slot;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = true;
    free_slot->scene = scene;
    free_slot->object_id = object_id;
    free_slot->last_used_seq = ++g_rig_lru_seq;
    rig_reset_slot_state(free_slot, object_id);
    return free_slot;
}

static void rig_sync_slot_to_puppet(we_puppet_t *puppet, const rig_puppet_slot_t *slot)
{
    if (!puppet || !slot) {
        return;
    }
    if (!puppet->rig_ctx) {
        puppet->rig_ctx = calloc(1u, sizeof(we_rig_pose_ctx_t));
    }
    if (puppet->rig_ctx) {
        memcpy(puppet->rig_ctx, &slot->rig_ctx, sizeof(we_rig_pose_ctx_t));
    }
    puppet->heatmap_mode = (uint8_t)slot->heatmap.enabled;
    puppet->heatmap_palette = (uint8_t)slot->heatmap.palette;
    puppet->heatmap_active_bone = (slot->heatmap.active_bone_idx <= 0xFFFFu)
                                      ? (uint16_t)slot->heatmap.active_bone_idx
                                      : UINT16_MAX;
    puppet->heatmap_opacity = slot->heatmap.opacity;
    puppet->playback_paused = slot->playback_paused;
    puppet->preview_time_sec = slot->preview_time_sec;
}

static void rig_mat4_identity(float out[16])
{
    memset(out, 0, sizeof(float) * 16u);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
}

static void rig_mat4_from_trs(const float pos[3], const float angle[3], const float scale[3],
                              float out[16])
{
    float cx = cosf(angle[0]), sx = sinf(angle[0]);
    float cy = cosf(angle[1]), sy = sinf(angle[1]);
    float cz = cosf(angle[2]), sz = sinf(angle[2]);

    float scx = scale[0], scy = scale[1], scz = scale[2];

    out[0] = (cz * cy) * scx;
    out[1] = (sz * cy) * scx;
    out[2] = (-sy) * scx;
    out[3] = 0.0f;

    out[4] = (cz * sy * sx - sz * cx) * scy;
    out[5] = (sz * sy * sx + cz * cx) * scy;
    out[6] = (cy * sx) * scy;
    out[7] = 0.0f;

    out[8] = (cz * sy * cx + sz * sx) * scz;
    out[9] = (sz * sy * cx - cz * sx) * scz;
    out[10] = (cy * cx) * scz;
    out[11] = 0.0f;

    out[12] = pos[0];
    out[13] = pos[1];
    out[14] = pos[2];
    out[15] = 1.0f;
}

static void rig_mat4_decompose_trs(const float m[16], float out_pos[3], float out_angle[3],
                                   float out_scale[3])
{
    out_pos[0] = m[12];
    out_pos[1] = m[13];
    out_pos[2] = m[14];

    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

    float det3 = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                 m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (det3 < 0.0f) {
        sx = -sx;
    }

    out_scale[0] = fabsf(sx) > 1e-6f ? sx : 1.0f;
    out_scale[1] = sy > 1e-6f ? sy : 1.0f;
    out_scale[2] = sz > 1e-6f ? sz : 1.0f;

    float r00 = m[0] / out_scale[0], r10 = m[1] / out_scale[0], r20 = m[2] / out_scale[0];
    float r11 = m[5] / out_scale[1], r21 = m[6] / out_scale[1];
    float r12 = m[9] / out_scale[2], r22 = m[10] / out_scale[2];

    float sy_rot = -r20;
    if (sy_rot < -1.0f) {
        sy_rot = -1.0f;
    } else if (sy_rot > 1.0f) {
        sy_rot = 1.0f;
    }
    out_angle[1] = asinf(sy_rot);
    if (fabsf(r20) < 0.9999f) {
        out_angle[0] = atan2f(r21, r22);
        out_angle[2] = atan2f(r10, r00);
    } else {
        out_angle[0] = atan2f(-r12, r11);
        out_angle[2] = 0.0f;
    }
}

static void rig_invalidate_puppet_gl(const we_scene_t *sc, we_puppet_t *puppet)
{
    if (!puppet || !puppet->gl_uploaded) {
        return;
    }
    if (sc && sc->gl_initialized) {
        authoring_ensure_scene_egl(sc);
        if (puppet->gl_ebo) {
            glDeleteBuffers(1, &puppet->gl_ebo);
            puppet->gl_ebo = 0u;
        }
        if (puppet->gl_vbo_skin) {
            glDeleteBuffers(1, &puppet->gl_vbo_skin);
            puppet->gl_vbo_skin = 0u;
        }
        if (puppet->gl_vbo_dyn) {
            glDeleteBuffers(1, &puppet->gl_vbo_dyn);
            puppet->gl_vbo_dyn = 0u;
        }
        if (puppet->gl_vao) {
            glDeleteVertexArrays(1, &puppet->gl_vao);
            puppet->gl_vao = 0u;
        }
    }
    puppet->gl_uploaded = false;
    puppet->vbo_has_bind_pose = false;
    puppet->cached_gl_prog = 0u;
}

static void rig_free_anim_clip(we_mdl_anim_t *anim)
{
    if (!anim) {
        return;
    }
    if (anim->tracks) {
        for (uint32_t t = 0u; t < anim->track_count; ++t) {
            free(anim->tracks[t].keyframes);
        }
        free(anim->tracks);
    }
    memset(anim, 0, sizeof(*anim));
}

static void rig_clear_puppet_anims(we_puppet_t *puppet)
{
    if (!puppet) {
        return;
    }
    if (puppet->anims) {
        for (uint32_t a = 0u; a < puppet->anim_count; ++a) {
            rig_free_anim_clip(&puppet->anims[a]);
        }
        free(puppet->anims);
        puppet->anims = NULL;
    }
    puppet->anim_count = 0u;
    puppet->layer_count = 0u;
    memset(puppet->layers, 0, sizeof(puppet->layers));
}

typedef struct {
    we_scene_t *scenes[64];
    we_scene_object_t *objects[64];
    size_t count;
} rig_target_list_t;

static void rig_collect_targets(daemon_state_t *state, uint32_t object_id, bool require_puppet,
                                rig_target_list_t *out_list)
{
    memset(out_list, 0, sizeof(*out_list));
    if (!state || object_id > (uint32_t)INT32_MAX) {
        return;
    }
    we_scene_t *visited[64];
    size_t visited_count = 0u;
    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
        we_scene_t *sc = out->we_scene;
        if (!sc || scene_already_visited(visited, visited_count, sc)) {
            continue;
        }
        if (visited_count < 64u) {
            visited[visited_count++] = sc;
        }
        we_scene_object_t *obj = authoring_find_object(sc, (int32_t)object_id, NULL);
        if (!obj || (require_puppet && !obj->puppet)) {
            continue;
        }
        if (out_list->count < 64u) {
            out_list->scenes[out_list->count] = sc;
            out_list->objects[out_list->count] = obj;
            out_list->count++;
        }
    }
}

static void rig_bump_scene_revision(we_scene_t *sc)
{
    authoring_scene_slot_t *slot = authoring_get_scene_slot(sc, true);
    if (slot) {
        slot->revision++;
    }
}

static void rig_recompute_world_bind(we_puppet_t *puppet)
{
    if (!puppet || !puppet->bones || puppet->bone_count == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < puppet->bone_count; ++i) {
        we_mdl_bone_t *b = &puppet->bones[i];
        rig_mat4_decompose_trs(b->local_trans, b->bind_pos, b->bind_angle, b->bind_scale);
        if (b->parent == UINT32_MAX || b->parent >= i) {
            memcpy(b->world_bind, b->local_trans, sizeof(b->world_bind));
        } else {
            we_mat4_mul(puppet->bones[b->parent].world_bind, b->local_trans, b->world_bind);
        }
        we_mat4_inverse(b->world_bind, b->offset_trans);
        if (puppet->skin_mats) {
            rig_mat4_identity(puppet->skin_mats[i]);
        }
    }
}

static we_mdl_anim_mode_t rig_parse_anim_mode(const char *mode_str)
{
    if (mode_str) {
        if (strcmp(mode_str, "mirror") == 0) {
            return WE_ANIM_MIRROR;
        }
        if (strcmp(mode_str, "single") == 0) {
            return WE_ANIM_SINGLE;
        }
    }
    return WE_ANIM_LOOP;
}

static bool rig_send_status_reply(int client_fd, bool ok, const char *err_msg)
{
    waywal_ipc_hdr_t rh;
    memset(&rh, 0, sizeof(rh));
    rh.magic = WAYWAL_IPC_MAGIC;
    rh.version = WAYWAL_IPC_VERSION;
    if (ok) {
        rh.opcode = WAYWAL_RESP_OK;
        rh.payload_size = 0u;
        return authoring_send_msg(client_fd, &rh, NULL, 0u);
    }
    rh.opcode = WAYWAL_RESP_ERR;
    size_t len = err_msg ? strlen(err_msg) : 0u;
    rh.payload_size = (uint32_t)len;
    return authoring_send_msg(client_fd, &rh, err_msg, len);
}

bool daemon_dispatch_ipc_req_puppet_set_mesh(daemon_state_t *state,
                                             const waywal_ipc_puppet_set_mesh_t *req,
                                             const waywal_ipc_mdl_vertex_t *verts,
                                             const uint16_t *indices,
                                             waywal_ipc_puppet_mesh_ack_t *resp)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (!req || (req->flags & ~0x3u) != 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    bool clear_mesh = (req->vertex_count == 0u && req->triangle_count == 0u);
    if (!clear_mesh &&
        (!verts || !indices || req->vertex_count < 3u ||
         req->vertex_count > WAYWAL_IPC_RIGGING_MAX_VERTICES || req->triangle_count < 1u ||
         req->triangle_count > WAYWAL_IPC_RIGGING_MAX_TRIANGLES)) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    resp->object_id = req->object_id;
    if (!state) {
        resp->status = (int32_t)WAYWAL_STATUS_OK;
        resp->vertex_count = req->vertex_count;
        resp->triangle_count = req->triangle_count;
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, false, &targets);
    if (targets.count == 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    typedef struct {
        we_puppet_t *new_puppet;
        we_mdl_vertex_t *new_verts;
        uint16_t *new_indices;
        we_skinned_vertex_t *new_skinned;
    } mesh_stage_t;

    size_t idx_count = (size_t)req->triangle_count * 3u;
    bool preserve_skeleton = (req->flags & 2u) != 0u;
    bool normalize_weights = (req->flags & 1u) != 0u;

    mesh_stage_t *stages = (mesh_stage_t *)calloc(targets.count, sizeof(mesh_stage_t));
    if (!stages) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *existing = targets.objects[t]->puppet;
        if (!existing) {
            stages[t].new_puppet = (we_puppet_t *)calloc(1u, sizeof(we_puppet_t));
            if (!stages[t].new_puppet) {
                goto fail;
            }
        }
        if (!clear_mesh) {
            stages[t].new_verts =
                (we_mdl_vertex_t *)calloc(req->vertex_count, sizeof(we_mdl_vertex_t));
            stages[t].new_indices = (uint16_t *)malloc(idx_count * sizeof(uint16_t));
            stages[t].new_skinned =
                (we_skinned_vertex_t *)calloc(req->vertex_count, sizeof(we_skinned_vertex_t));
            if (!stages[t].new_verts || !stages[t].new_indices || !stages[t].new_skinned) {
                goto fail;
            }
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_scene_object_t *obj = targets.objects[t];
        we_puppet_t *puppet = obj->puppet;
        if (!puppet) {
            puppet = stages[t].new_puppet;
            puppet->mdlv_version = 23u;
            puppet->mdat_version = 1u;
            puppet->mdls_version = 2u;
            puppet->mdla_version = 3u;
            puppet->mdl_flag = 1;
            obj->puppet = puppet;
            stages[t].new_puppet = NULL;
        }

        rig_puppet_slot_t *slot = rig_get_puppet_slot(sc, req->object_id, true);
        if (!preserve_skeleton) {
            if (puppet->bone_count > 0u) {
                rig_clear_puppet_anims(puppet);
                free(puppet->physics_ctx);
                puppet->physics_ctx = NULL;
                free(puppet->bones);
                puppet->bones = NULL;
                free(puppet->skin_mats);
                puppet->skin_mats = NULL;
                puppet->bone_count = 0u;
            }
            if (slot) {
                rig_reset_slot_state(slot, req->object_id);
            }
        }

        float min_p[3] = {0.0f, 0.0f, 0.0f};
        float max_p[3] = {0.0f, 0.0f, 0.0f};
        if (!clear_mesh) {
            min_p[0] = min_p[1] = min_p[2] = 1e30f;
            max_p[0] = max_p[1] = max_p[2] = -1e30f;
            for (uint32_t i = 0u; i < req->vertex_count; ++i) {
                waywal_ipc_mdl_vertex_t src_v;
                memcpy(&src_v, &verts[i], sizeof(src_v));
                we_mdl_vertex_t *dv = &stages[t].new_verts[i];
                memcpy(dv->pos, src_v.pos, sizeof(dv->pos));
                memcpy(dv->texcoord, src_v.texcoord, sizeof(dv->texcoord));
                for (int k = 0; k < 3; ++k) {
                    if (dv->pos[k] < min_p[k]) {
                        min_p[k] = dv->pos[k];
                    }
                    if (dv->pos[k] > max_p[k]) {
                        max_p[k] = dv->pos[k];
                    }
                }
                float wsum = 0.0f;
                for (int j = 0; j < 4; ++j) {
                    uint32_t bi = src_v.blend_indices[j];
                    float w = src_v.weight[j];
                    if (puppet->bone_count > 0u && bi >= puppet->bone_count) {
                        bi = 0u;
                        w = 0.0f;
                    } else if (bi == UINT32_MAX) {
                        bi = 0u;
                        w = 0.0f;
                    }
                    dv->blend_indices[j] = bi;
                    dv->weight[j] = w;
                    wsum += w;
                }
                if (normalize_weights) {
                    if (wsum > 1e-6f) {
                        for (int j = 0; j < 4; ++j) {
                            dv->weight[j] /= wsum;
                        }
                    } else {
                        dv->blend_indices[0] = 0u;
                        dv->weight[0] = 1.0f;
                        dv->weight[1] = dv->weight[2] = dv->weight[3] = 0.0f;
                    }
                }
            }
            memcpy(stages[t].new_indices, indices, idx_count * sizeof(uint16_t));
        }

        rig_invalidate_puppet_gl(sc, puppet);
        free(puppet->vertices);
        free(puppet->indices);
        free(puppet->skinned_verts);
        puppet->vertices = stages[t].new_verts;
        puppet->vertex_count = req->vertex_count;
        puppet->indices = stages[t].new_indices;
        puppet->index_count = (uint32_t)idx_count;
        puppet->triangle_count = req->triangle_count;
        puppet->skinned_verts = stages[t].new_skinned;
        memcpy(puppet->min_pos, min_p, sizeof(min_p));
        memcpy(puppet->max_pos, max_p, sizeof(max_p));
        if (slot) {
            rig_sync_slot_to_puppet(puppet, slot);
        }
        rig_bump_scene_revision(sc);
    }

    free(stages);
    resp->status = (int32_t)WAYWAL_STATUS_OK;
    resp->vertex_count = req->vertex_count;
    resp->triangle_count = req->triangle_count;
    return true;

fail:
    for (size_t t = 0u; t < targets.count; ++t) {
        free(stages[t].new_verts);
        free(stages[t].new_indices);
        free(stages[t].new_skinned);
        free(stages[t].new_puppet);
    }
    free(stages);
    resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
    return false;
}

bool daemon_dispatch_ipc_req_puppet_set_skeleton(daemon_state_t *state,
                                                 const waywal_ipc_puppet_set_skeleton_t *req,
                                                 const waywal_ipc_mdl_bone_t *bones)
{
    if (!req || req->bone_count > WAYWAL_IPC_RIGGING_MAX_BONES) {
        return false;
    }
    if (req->bone_count > 0u && !bones) {
        return false;
    }
    if (!state) {
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        return false;
    }

    typedef struct {
        we_mdl_bone_t *new_bones;
        float (*new_skin)[16];
        we_puppet_physics_ctx_t *new_phys;
        bool replace_phys;
    } skel_stage_t;

    skel_stage_t *stages = (skel_stage_t *)calloc(targets.count, sizeof(skel_stage_t));
    if (!stages) {
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (req->bone_count > 0u) {
            stages[t].new_bones = (we_mdl_bone_t *)calloc(req->bone_count, sizeof(we_mdl_bone_t));
            stages[t].new_skin = (float (*)[16])calloc(req->bone_count, sizeof(float[16]));
            if (!stages[t].new_bones || !stages[t].new_skin) {
                goto fail;
            }
            for (uint32_t i = 0u; i < req->bone_count; ++i) {
                waywal_ipc_mdl_bone_t sb;
                memcpy(&sb, &bones[i], sizeof(sb));
                memcpy(stages[t].new_bones[i].name, sb.name, sizeof(stages[t].new_bones[i].name));
                stages[t].new_bones[i].name[sizeof(stages[t].new_bones[i].name) - 1u] = '\0';
                stages[t].new_bones[i].unk = sb.unk;
                stages[t].new_bones[i].parent = sb.parent;
                memcpy(stages[t].new_bones[i].local_trans, sb.local_trans,
                       sizeof(stages[t].new_bones[i].local_trans));
                memcpy(stages[t].new_bones[i].simulation_json, sb.simulation_json,
                       sizeof(stages[t].new_bones[i].simulation_json));
                stages[t]
                    .new_bones[i]
                    .simulation_json[sizeof(stages[t].new_bones[i].simulation_json) - 1u] = '\0';
            }
        }
        stages[t].replace_phys = ((req->flags & 1u) != 0u || !puppet->physics_ctx);
        if (stages[t].replace_phys && req->bone_count > 0u) {
            stages[t].new_phys =
                (we_puppet_physics_ctx_t *)calloc(1u, sizeof(we_puppet_physics_ctx_t));
            if (!stages[t].new_phys) {
                goto fail;
            }
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;

        free(puppet->bones);
        free(puppet->skin_mats);
        puppet->bones = stages[t].new_bones;
        puppet->skin_mats = stages[t].new_skin;
        puppet->bone_count = req->bone_count;

        if (puppet->vertices && puppet->vertex_count > 0u) {
            for (uint32_t v = 0u; v < puppet->vertex_count; ++v) {
                for (int j = 0; j < 4; ++j) {
                    if (req->bone_count == 0u ||
                        puppet->vertices[v].blend_indices[j] >= req->bone_count) {
                        puppet->vertices[v].blend_indices[j] = 0u;
                        puppet->vertices[v].weight[j] = 0.0f;
                    }
                }
            }
            rig_invalidate_puppet_gl(sc, puppet);
        }

        rig_recompute_world_bind(puppet);

        if (stages[t].replace_phys) {
            free(puppet->physics_ctx);
            puppet->physics_ctx = stages[t].new_phys;
            if (puppet->physics_ctx) {
                we_physics_init_from_puppet(puppet->physics_ctx, puppet);
            }
        } else if (puppet->physics_ctx) {
            we_physics_init_from_puppet(puppet->physics_ctx, puppet);
        }

        rig_puppet_slot_t *slot = rig_get_puppet_slot(sc, req->object_id, true);
        if (slot) {
            memset(&slot->rig_ctx, 0, sizeof(slot->rig_ctx));
            for (uint32_t i = 0u; i < puppet->bone_count && i < WE_MDL_MAX_BONES; ++i) {
                slot->rig_ctx.bone_drive_modes[i] = (puppet->bones[i].simulation_json[0] != '\0')
                                                        ? WE_BONE_DRIVE_XPBD
                                                        : WE_BONE_DRIVE_FK;
            }
            slot->preview_active = false;
            if (slot->heatmap.active_bone_idx != UINT32_MAX &&
                slot->heatmap.active_bone_idx >= req->bone_count) {
                slot->heatmap.active_bone_idx = UINT32_MAX;
            }
            rig_sync_slot_to_puppet(puppet, slot);
        }
        rig_bump_scene_revision(sc);
    }

    free(stages);
    return true;

fail:
    for (size_t t = 0u; t < targets.count; ++t) {
        free(stages[t].new_bones);
        free(stages[t].new_skin);
        free(stages[t].new_phys);
    }
    free(stages);
    return false;
}

bool daemon_dispatch_ipc_req_puppet_solve_weights(daemon_state_t *state,
                                                  const waywal_ipc_puppet_solve_weights_t *req,
                                                  const uint64_t *locked_bs,
                                                  waywal_ipc_puppet_weights_resp_t *resp,
                                                  waywal_ipc_vertex_weight_t **out_entries,
                                                  uint32_t *out_entry_count)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (out_entries) {
        *out_entries = NULL;
    }
    if (out_entry_count) {
        *out_entry_count = 0u;
    }
    if (!req) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    resp->object_id = req->object_id;
    if (!state) {
        resp->status = (int32_t)WAYWAL_STATUS_OK;
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    bool apply_live = (req->flags & 1u) != 0u;
    bool respect_locked = (req->flags & 2u) != 0u;

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (!puppet->vertices || puppet->vertex_count < 3u || !puppet->indices ||
            puppet->triangle_count < 1u || !puppet->bones || puppet->bone_count == 0u) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
        uint32_t needed_words = (puppet->vertex_count + 63u) / 64u;
        if (respect_locked && (!locked_bs || req->locked_vert_word_count < needed_words)) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    we_mdl_vertex_t **scratch_verts =
        (we_mdl_vertex_t **)calloc(targets.count, sizeof(we_mdl_vertex_t *));
    waywal_ipc_vertex_weight_t *result_entries = NULL;
    uint32_t result_count = 0u;
    if (!scratch_verts) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    if (out_entries && out_entry_count) {
        result_count = targets.objects[0]->puppet->vertex_count;
        result_entries = (waywal_ipc_vertex_weight_t *)malloc((size_t)result_count *
                                                              sizeof(waywal_ipc_vertex_weight_t));
        if (!result_entries) {
            free(scratch_verts);
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        scratch_verts[t] =
            (we_mdl_vertex_t *)malloc((size_t)puppet->vertex_count * sizeof(we_mdl_vertex_t));
        if (!scratch_verts[t]) {
            goto fail;
        }
        memcpy(scratch_verts[t], puppet->vertices,
               (size_t)puppet->vertex_count * sizeof(we_mdl_vertex_t));

        we_heat_skin_cfg_t cfg;
        we_heat_skin_cfg_default(&cfg);
        cfg.falloff_power = req->falloff_power;
        cfg.smooth_factor = req->smoothness;
        cfg.max_influences = req->max_influences;
        cfg.use_visibility_raycast = (req->flags & 4u) != 0u;
        cfg.use_intrinsic_delaunay = (req->flags & 8u) != 0u;
        cfg.locked_vert_bs = respect_locked ? locked_bs : NULL;

        we_puppet_t scratch_puppet = *puppet;
        scratch_puppet.vertices = scratch_verts[t];

        we_heat_solve_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        if (!we_mesh_solve_heat_weights(&scratch_puppet, &cfg, &stats)) {
            goto fail;
        }
    }

    if (result_entries) {
        for (uint32_t v = 0u; v < result_count; ++v) {
            result_entries[v].vertex_index = v;
            memcpy(result_entries[v].blend_indices, scratch_verts[0][v].blend_indices,
                   sizeof(result_entries[v].blend_indices));
            memcpy(result_entries[v].weight, scratch_verts[0][v].weight,
                   sizeof(result_entries[v].weight));
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (apply_live) {
            free(puppet->vertices);
            puppet->vertices = scratch_verts[t];
            scratch_verts[t] = NULL;
            rig_invalidate_puppet_gl(sc, puppet);
            rig_bump_scene_revision(sc);
        } else {
            free(scratch_verts[t]);
            scratch_verts[t] = NULL;
        }
    }
    free(scratch_verts);

    resp->status = (int32_t)WAYWAL_STATUS_OK;
    resp->entry_count = result_count;
    if (out_entries) {
        *out_entries = result_entries;
    } else {
        free(result_entries);
    }
    if (out_entry_count) {
        *out_entry_count = result_count;
    }
    return true;

fail:
    for (size_t t = 0u; t < targets.count; ++t) {
        free(scratch_verts[t]);
    }
    free(scratch_verts);
    free(result_entries);
    resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
    return false;
}

bool daemon_dispatch_ipc_req_puppet_paint_weights(daemon_state_t *state,
                                                  const waywal_ipc_puppet_paint_weights_t *req,
                                                  const waywal_ipc_vertex_weight_t *entries)
{
    if (!req || !entries || req->entry_count < 1u ||
        req->entry_count > WAYWAL_IPC_RIGGING_MAX_VERTICES) {
        return false;
    }
    if (!state) {
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (!puppet->vertices || puppet->vertex_count == 0u || puppet->bone_count == 0u) {
            return false;
        }
        for (uint32_t i = 0u; i < req->entry_count; ++i) {
            waywal_ipc_vertex_weight_t e;
            memcpy(&e, &entries[i], sizeof(e));
            if (e.vertex_index >= puppet->vertex_count) {
                return false;
            }
            for (int j = 0; j < 4; ++j) {
                if (!isfinite(e.weight[j]) || e.weight[j] < 0.0f) {
                    return false;
                }
                if (e.blend_indices[j] != UINT32_MAX && e.blend_indices[j] >= puppet->bone_count) {
                    return false;
                }
            }
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;
        for (uint32_t i = 0u; i < req->entry_count; ++i) {
            waywal_ipc_vertex_weight_t e;
            memcpy(&e, &entries[i], sizeof(e));

            /* Deduplicate repeated bone indices per vertex and enforce partition-of-unity. */
            uint32_t u_bones[4] = {0u, 0u, 0u, 0u};
            float u_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int u_count = 0;
            uint32_t fallback_bone = 0u;
            bool have_fallback = false;
            for (int j = 0; j < 4; ++j) {
                if (e.blend_indices[j] == UINT32_MAX) {
                    continue;
                }
                uint32_t bi = e.blend_indices[j];
                float w = e.weight[j];
                if (!have_fallback) {
                    fallback_bone = bi;
                    have_fallback = true;
                }
                if (w <= 0.0f) {
                    continue;
                }
                int found = -1;
                for (int k = 0; k < u_count; ++k) {
                    if (u_bones[k] == bi) {
                        found = k;
                        break;
                    }
                }
                if (found >= 0) {
                    u_weights[found] += w;
                } else if (u_count < 4) {
                    u_bones[u_count] = bi;
                    u_weights[u_count] = w;
                    u_count++;
                }
            }

            we_mdl_vertex_t *dv = &puppet->vertices[e.vertex_index];
            float sum = 0.0f;
            for (int k = 0; k < u_count; ++k) {
                sum += u_weights[k];
            }
            if (sum > 1e-6f) {
                for (int j = 0; j < 4; ++j) {
                    if (j < u_count) {
                        dv->blend_indices[j] = u_bones[j];
                        dv->weight[j] = u_weights[j] / sum;
                    } else {
                        dv->blend_indices[j] = 0u;
                        dv->weight[j] = 0.0f;
                    }
                }
            } else {
                dv->blend_indices[0] = fallback_bone;
                dv->weight[0] = 1.0f;
                for (int j = 1; j < 4; ++j) {
                    dv->blend_indices[j] = 0u;
                    dv->weight[j] = 0.0f;
                }
            }
        }
        rig_invalidate_puppet_gl(sc, puppet);
        rig_bump_scene_revision(sc);
    }
    return true;
}

bool daemon_dispatch_ipc_req_puppet_set_heatmap_mode(daemon_state_t *state,
                                                     const waywal_ipc_puppet_heatmap_mode_t *req)
{
    if (!req || req->enabled > 1u || req->palette > 2u || !isfinite(req->opacity) ||
        req->opacity < 0.0f || req->opacity > 1.0f) {
        return false;
    }
    if (!state) {
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (req->active_bone_idx != UINT32_MAX && req->active_bone_idx >= puppet->bone_count) {
            return false;
        }
    }
    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        rig_puppet_slot_t *slot = rig_get_puppet_slot(targets.scenes[t], req->object_id, true);
        if (slot) {
            slot->heatmap = *req;
            rig_sync_slot_to_puppet(puppet, slot);
        }
        rig_bump_scene_revision(targets.scenes[t]);
    }
    return true;
}

bool daemon_dispatch_ipc_req_puppet_preview_ik(daemon_state_t *state,
                                               const waywal_ipc_puppet_preview_ik_t *req,
                                               const waywal_ipc_bone_trs_t *overrides,
                                               waywal_ipc_puppet_ik_pose_resp_t *resp,
                                               waywal_ipc_bone_trs_t **out_bones,
                                               uint32_t *out_bone_count)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (out_bones) {
        *out_bones = NULL;
    }
    if (out_bone_count) {
        *out_bone_count = 0u;
    }
    if (!req || (req->override_count > 0u && !overrides)) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    resp->object_id = req->object_id;
    if (!state) {
        resp->status = (int32_t)WAYWAL_STATUS_OK;
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if ((req->root_bone != 0xFFFFu && req->root_bone >= puppet->bone_count) ||
            (req->mid_bone != 0xFFFFu && req->mid_bone >= puppet->bone_count) ||
            (req->end_bone != 0xFFFFu && req->end_bone >= puppet->bone_count)) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
        for (uint32_t k = 0u; k < req->override_count; ++k) {
            waywal_ipc_bone_trs_t ov;
            memcpy(&ov, &overrides[k], sizeof(ov));
            if (ov.bone_index >= puppet->bone_count) {
                resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
                return false;
            }
        }
    }

    waywal_ipc_bone_trs_t *pose_out = NULL;
    uint32_t pose_count = 0u;
    if (out_bones && out_bone_count && targets.objects[0]->puppet->bone_count > 0u) {
        pose_count = targets.objects[0]->puppet->bone_count;
        pose_out =
            (waywal_ipc_bone_trs_t *)malloc((size_t)pose_count * sizeof(waywal_ipc_bone_trs_t));
        if (!pose_out) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;
        rig_puppet_slot_t *slot = rig_get_puppet_slot(sc, req->object_id, true);

        float anim_local[WE_MDL_MAX_BONES][16];
        for (uint32_t b = 0u; b < puppet->bone_count; ++b) {
            rig_mat4_from_trs(puppet->bones[b].bind_pos, puppet->bones[b].bind_angle,
                              puppet->bones[b].bind_scale, anim_local[b]);
        }

        if ((req->mode_flags & 2u) != 0u) {
            if (slot) {
                slot->rig_ctx.ik_chain_count = 0u;
                slot->rig_ctx.aim_count = 0u;
                for (uint32_t b = 0u; b < puppet->bone_count && b < WE_MDL_MAX_BONES; ++b) {
                    slot->rig_ctx.bone_drive_modes[b] =
                        (puppet->bones[b].simulation_json[0] != '\0') ? WE_BONE_DRIVE_XPBD
                                                                      : WE_BONE_DRIVE_FK;
                }
                slot->preview_active = false;
            }
        } else {
            for (uint32_t k = 0u; k < req->override_count; ++k) {
                waywal_ipc_bone_trs_t ov;
                memcpy(&ov, &overrides[k], sizeof(ov));
                rig_mat4_from_trs(ov.local_pos, ov.local_angle, ov.local_scale,
                                  anim_local[ov.bone_index]);
            }

            if ((req->mode_flags & 1u) != 0u) {
                if (req->root_bone != 0xFFFFu && req->mid_bone != 0xFFFFu &&
                    req->end_bone != 0xFFFFu) {
                    we_ik_chain_t chain;
                    memset(&chain, 0, sizeof(chain));
                    chain.root_bone = req->root_bone;
                    chain.mid_bone = req->mid_bone;
                    chain.end_bone = req->end_bone;
                    chain.bend_sign = req->pole_flip ? -1 : 1;
                    chain.enabled = 1u;
                    memcpy(chain.target_world, req->target_world, sizeof(chain.target_world));
                    memcpy(chain.pole_world, req->target_world, sizeof(chain.pole_world));
                    chain.soft_zone_ratio = 0.05f;
                    chain.max_stretch_ratio = 1.0f;
                    chain.root_angle_min_rad = -3.14159265f;
                    chain.root_angle_max_rad = 3.14159265f;
                    chain.mid_angle_min_rad = -3.14159265f;
                    chain.mid_angle_max_rad = 3.14159265f;
                    chain.blend_weight = 1.0f;
                    (void)we_ik_solve_two_bone(puppet, &chain, anim_local);
                    if (slot) {
                        slot->rig_ctx.ik_chains[0] = chain;
                        slot->rig_ctx.ik_chain_count = 1u;
                        slot->rig_ctx.aim_count = 0u;
                        slot->rig_ctx.bone_drive_modes[chain.root_bone] = WE_BONE_DRIVE_IK;
                        slot->rig_ctx.bone_drive_modes[chain.mid_bone] = WE_BONE_DRIVE_IK;
                        slot->preview_active = true;
                    }
                } else if (req->root_bone != 0xFFFFu && req->mid_bone == 0xFFFFu) {
                    we_aim_constraint_t aim;
                    memset(&aim, 0, sizeof(aim));
                    aim.bone_index = req->root_bone;
                    aim.enabled = 1u;
                    aim.aim_axis_local[0] = 1.0f;
                    memcpy(aim.target_world, req->target_world, sizeof(aim.target_world));
                    aim.max_cone_rad = 3.14159265f;
                    aim.blend_weight = 1.0f;
                    (void)we_ik_solve_aim(puppet, &aim, anim_local);
                    if (slot) {
                        slot->rig_ctx.aim_constraints[0] = aim;
                        slot->rig_ctx.aim_count = 1u;
                        slot->rig_ctx.ik_chain_count = 0u;
                        slot->rig_ctx.bone_drive_modes[aim.bone_index] = WE_BONE_DRIVE_AIM;
                        slot->preview_active = true;
                    }
                }
            }
            if (slot) {
                we_rig_apply_constraints(puppet, &slot->rig_ctx, anim_local);
            }
        }

        if ((req->mode_flags & 4u) != 0u && puppet->bones) {
            for (uint32_t b = 0u; b < puppet->bone_count; ++b) {
                memcpy(puppet->bones[b].local_trans, anim_local[b], sizeof(anim_local[b]));
                rig_mat4_decompose_trs(anim_local[b], puppet->bones[b].bind_pos,
                                       puppet->bones[b].bind_angle, puppet->bones[b].bind_scale);
            }
        }

        if (puppet->bone_count > 0u && puppet->skin_mats) {
            double eval_t = (slot && slot->playback_paused) ? slot->preview_time_sec : sc->sim_time;
            we_puppet_eval_skin_matrices_ex(puppet, puppet->layers, puppet->layer_count,
                                            slot ? &slot->rig_ctx : NULL, eval_t,
                                            puppet->skin_mats);
        }

        if (slot) {
            rig_sync_slot_to_puppet(puppet, slot);
        }

        if (t == 0u && pose_out && pose_count > 0u) {
            for (uint32_t b = 0u; b < pose_count; ++b) {
                pose_out[b].bone_index = b;
                rig_mat4_decompose_trs(anim_local[b], pose_out[b].local_pos,
                                       pose_out[b].local_angle, pose_out[b].local_scale);
            }
        }
        rig_bump_scene_revision(sc);
    }

    resp->status = (int32_t)WAYWAL_STATUS_OK;
    resp->bone_count = pose_count;
    if (out_bones) {
        *out_bones = pose_out;
    } else {
        free(pose_out);
    }
    if (out_bone_count) {
        *out_bone_count = pose_count;
    }
    return true;
}

static bool rig_decode_anim_clips(const waywal_ipc_puppet_set_anim_tracks_t *req,
                                  const uint8_t *clips_blob, size_t clips_blob_len,
                                  uint32_t bone_count, we_mdl_anim_t **out_anims,
                                  we_puppet_layer_t *out_layers,
                                  we_anim_blend_mode_t *out_blend_modes)
{
    *out_anims = NULL;
    if (req->clip_count == 0u) {
        return clips_blob_len == 0u;
    }
    if (!clips_blob) {
        return false;
    }
    we_mdl_anim_t *anims = (we_mdl_anim_t *)calloc(req->clip_count, sizeof(we_mdl_anim_t));
    if (!anims) {
        return false;
    }
    size_t cursor = 0u;
    for (uint32_t c = 0u; c < req->clip_count; ++c) {
        if (clips_blob_len - cursor < sizeof(waywal_ipc_anim_clip_desc_t)) {
            goto fail;
        }
        waywal_ipc_anim_clip_desc_t cd;
        memcpy(&cd, clips_blob + cursor, sizeof(cd));
        cursor += sizeof(cd);

        we_mdl_anim_t *dst = &anims[c];
        dst->id = cd.id;
        memcpy(dst->name, cd.name, sizeof(dst->name));
        dst->name[sizeof(dst->name) - 1u] = '\0';
        memcpy(dst->mode_str, cd.mode_str, sizeof(dst->mode_str));
        dst->mode_str[sizeof(dst->mode_str) - 1u] = '\0';
        dst->mode = rig_parse_anim_mode(dst->mode_str);
        dst->fps = cd.fps;
        dst->length = cd.length;
        dst->track_count = cd.track_count;
        memset(dst->bone_to_track, -1, sizeof(dst->bone_to_track));
        dst->bone_lut_valid = true;
        if (dst->track_count > 0u) {
            dst->tracks =
                (we_mdl_bone_track_t *)calloc(dst->track_count, sizeof(we_mdl_bone_track_t));
            if (!dst->tracks) {
                goto fail;
            }
        }
        if (out_layers && c < WE_MDL_MAX_LAYERS) {
            out_layers[c].anim_id = cd.id;
            memcpy(out_layers[c].anim_name, dst->name, sizeof(out_layers[c].anim_name));
            out_layers[c].blend = cd.layer_blend;
            out_layers[c].rate = cd.layer_rate;
            out_layers[c].visible = (cd.layer_visible != 0u);
        }
        if (out_blend_modes && c < WE_MDL_MAX_LAYERS) {
            out_blend_modes[c] = (we_anim_blend_mode_t)cd.layer_blend_mode;
        }

        for (uint32_t tr = 0u; tr < cd.track_count; ++tr) {
            if (clips_blob_len - cursor < sizeof(waywal_ipc_anim_track_desc_t)) {
                goto fail;
            }
            waywal_ipc_anim_track_desc_t td;
            memcpy(&td, clips_blob + cursor, sizeof(td));
            cursor += sizeof(td);
            if (td.bone_index < 0 || (uint32_t)td.bone_index >= bone_count) {
                goto fail;
            }
            size_t kf_bytes = (size_t)td.keyframe_count * sizeof(waywal_ipc_mdl_keyframe_t);
            if (clips_blob_len - cursor < kf_bytes) {
                goto fail;
            }
            dst->tracks[tr].bone_index = td.bone_index;
            dst->tracks[tr].kf_count = td.keyframe_count;
            dst->bone_to_track[td.bone_index] = (int16_t)tr;
            dst->tracks[tr].keyframes =
                (we_mdl_keyframe_t *)calloc(td.keyframe_count, sizeof(we_mdl_keyframe_t));
            if (!dst->tracks[tr].keyframes) {
                goto fail;
            }
            for (uint32_t k = 0u; k < td.keyframe_count; ++k) {
                waywal_ipc_mdl_keyframe_t skf;
                memcpy(&skf, clips_blob + cursor, sizeof(skf));
                cursor += sizeof(skf);
                memcpy(dst->tracks[tr].keyframes[k].pos, skf.pos, sizeof(skf.pos));
                memcpy(dst->tracks[tr].keyframes[k].angle, skf.angle, sizeof(skf.angle));
                memcpy(dst->tracks[tr].keyframes[k].scale, skf.scale, sizeof(skf.scale));
            }
        }
    }
    if (cursor != clips_blob_len) {
        goto fail;
    }
    *out_anims = anims;
    return true;

fail:
    for (uint32_t c = 0u; c < req->clip_count; ++c) {
        rig_free_anim_clip(&anims[c]);
    }
    free(anims);
    return false;
}

bool daemon_dispatch_ipc_req_puppet_set_anim_tracks(daemon_state_t *state,
                                                    const waywal_ipc_puppet_set_anim_tracks_t *req,
                                                    const uint8_t *clips_blob,
                                                    size_t clips_blob_len)
{
    if (!req || req->clip_count > WAYWAL_IPC_RIGGING_MAX_ANIMS) {
        return false;
    }
    if (!state) {
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        return false;
    }

    typedef struct {
        we_mdl_anim_t *decoded;
        we_mdl_anim_t *merged;
        we_puppet_layer_t layers[WE_MDL_MAX_LAYERS];
        we_anim_blend_mode_t modes[WE_MDL_MAX_LAYERS];
    } anim_stage_t;

    bool replace_all = (req->flags & 1u) != 0u;
    bool sync_layers = (req->flags & 2u) != 0u;
    anim_stage_t *stages = (anim_stage_t *)calloc(targets.count, sizeof(anim_stage_t));
    if (!stages) {
        return false;
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_puppet_t *puppet = targets.objects[t]->puppet;
        if (!rig_decode_anim_clips(req, clips_blob, clips_blob_len, puppet->bone_count,
                                   &stages[t].decoded, stages[t].layers, stages[t].modes)) {
            goto fail;
        }
        if (!replace_all) {
            uint32_t max_new = puppet->anim_count + req->clip_count;
            if (max_new > 0u) {
                stages[t].merged = (we_mdl_anim_t *)calloc(max_new, sizeof(we_mdl_anim_t));
                if (!stages[t].merged) {
                    goto fail;
                }
            }
        }
    }

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;
        we_mdl_anim_t *decoded = stages[t].decoded;
        stages[t].decoded = NULL;

        if (replace_all) {
            rig_clear_puppet_anims(puppet);
            puppet->anims = decoded;
            puppet->anim_count = req->clip_count;
            if (sync_layers) {
                uint32_t lc =
                    req->clip_count < WE_MDL_MAX_LAYERS ? req->clip_count : WE_MDL_MAX_LAYERS;
                puppet->layer_count = lc;
                if (lc > 0u) {
                    memcpy(puppet->layers, stages[t].layers, lc * sizeof(we_puppet_layer_t));
                }
            }
        } else {
            we_mdl_anim_t *merged = stages[t].merged;
            stages[t].merged = NULL;
            uint32_t out_n = puppet->anim_count;
            if (out_n > 0u && puppet->anims) {
                memcpy(merged, puppet->anims, (size_t)out_n * sizeof(we_mdl_anim_t));
            }
            for (uint32_t c = 0u; c < req->clip_count; ++c) {
                bool replaced = false;
                for (uint32_t e = 0u; e < out_n; ++e) {
                    if ((decoded[c].id != 0 && merged[e].id == decoded[c].id) ||
                        strcmp(merged[e].name, decoded[c].name) == 0) {
                        rig_free_anim_clip(&merged[e]);
                        merged[e] = decoded[c];
                        memset(&decoded[c], 0, sizeof(decoded[c]));
                        replaced = true;
                        break;
                    }
                }
                if (!replaced && out_n < WE_MDL_MAX_ANIMS) {
                    merged[out_n++] = decoded[c];
                    memset(&decoded[c], 0, sizeof(decoded[c]));
                } else if (!replaced) {
                    rig_free_anim_clip(&decoded[c]);
                }
            }
            free(decoded);
            free(puppet->anims);
            puppet->anims = merged;
            puppet->anim_count = out_n;
            if (sync_layers) {
                uint32_t lc = out_n < WE_MDL_MAX_LAYERS ? out_n : WE_MDL_MAX_LAYERS;
                puppet->layer_count = lc;
                for (uint32_t l = 0u; l < lc; ++l) {
                    puppet->layers[l].anim_id = puppet->anims[l].id;
                    memcpy(puppet->layers[l].anim_name, puppet->anims[l].name,
                           sizeof(puppet->layers[l].anim_name));
                    if (l < req->clip_count) {
                        puppet->layers[l].blend = stages[t].layers[l].blend;
                        puppet->layers[l].rate = stages[t].layers[l].rate;
                        puppet->layers[l].visible = stages[t].layers[l].visible;
                    } else if (puppet->layers[l].rate == 0.0f) {
                        puppet->layers[l].blend = 1.0f;
                        puppet->layers[l].rate = 1.0f;
                        puppet->layers[l].visible = true;
                    }
                }
            }
        }

        rig_puppet_slot_t *slot = rig_get_puppet_slot(sc, req->object_id, true);
        if (slot) {
            if (sync_layers) {
                uint32_t lc =
                    req->clip_count < WE_MDL_MAX_LAYERS ? req->clip_count : WE_MDL_MAX_LAYERS;
                if (lc > 0u) {
                    memcpy(slot->rig_ctx.layer_blend_modes, stages[t].modes,
                           lc * sizeof(we_anim_blend_mode_t));
                }
            }
            rig_sync_slot_to_puppet(puppet, slot);
        }
        rig_bump_scene_revision(sc);
    }

    free(stages);
    return true;

fail:
    for (size_t t = 0u; t < targets.count; ++t) {
        if (stages[t].decoded) {
            for (uint32_t c = 0u; c < req->clip_count; ++c) {
                rig_free_anim_clip(&stages[t].decoded[c]);
            }
            free(stages[t].decoded);
        }
        free(stages[t].merged);
    }
    free(stages);
    return false;
}

bool daemon_dispatch_ipc_req_puppet_scrub_time(daemon_state_t *state,
                                               const waywal_ipc_puppet_scrub_t *req)
{
    if (!req || !isfinite(req->time_sec) || req->time_sec < 0.0f || req->pause_playback > 1u) {
        return false;
    }
    if (!state) {
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        return false;
    }

    bool reset_phys = (req->flags & 1u) != 0u;
    bool step_phys = (req->flags & 2u) != 0u;

    for (size_t t = 0u; t < targets.count; ++t) {
        we_scene_t *sc = targets.scenes[t];
        we_puppet_t *puppet = targets.objects[t]->puppet;
        double target_t = (double)req->time_sec;
        double prev_t = (puppet->physics_ctx && puppet->physics_ctx->has_last_wall_time)
                            ? puppet->physics_ctx->last_wall_time_sec
                            : sc->sim_time;
        bool backward = (target_t + 1e-6 < prev_t);

        sc->sim_time = target_t;

        rig_puppet_slot_t *slot = rig_get_puppet_slot(sc, req->object_id, true);
        if (slot) {
            slot->preview_time_sec = target_t;
            slot->playback_paused = (req->pause_playback != 0u);
            rig_sync_slot_to_puppet(puppet, slot);
        }

        if (puppet->physics_ctx && (reset_phys || (step_phys && backward))) {
            we_physics_init_from_puppet(puppet->physics_ctx, puppet);
        }

        if (puppet->bone_count > 0u && puppet->skin_mats) {
            if (!step_phys && puppet->physics_ctx) {
                bool saved_has = puppet->physics_ctx->has_last_wall_time;
                double saved_last = puppet->physics_ctx->last_wall_time_sec;
                float saved_acc = puppet->physics_ctx->time_accumulator_sec;
                /* Suppress XPBD wall-clock stepping when flags omits step_physics_dt. */
                puppet->physics_ctx->has_last_wall_time = true;
                puppet->physics_ctx->last_wall_time_sec = target_t;
                puppet->physics_ctx->time_accumulator_sec = 0.0f;
                we_puppet_eval_skin_matrices_ex(puppet, puppet->layers, puppet->layer_count,
                                                slot ? &slot->rig_ctx : NULL, target_t,
                                                puppet->skin_mats);
                if (!reset_phys) {
                    puppet->physics_ctx->has_last_wall_time = saved_has;
                    puppet->physics_ctx->last_wall_time_sec = saved_last;
                    puppet->physics_ctx->time_accumulator_sec = saved_acc;
                }
            } else {
                if (puppet->physics_ctx && (reset_phys || backward)) {
                    double start_t = (target_t > 2.0) ? (target_t - 2.0) : 0.0;
                    puppet->physics_ctx->has_last_wall_time = true;
                    puppet->physics_ctx->last_wall_time_sec = start_t;
                    puppet->physics_ctx->time_accumulator_sec = 0.0f;
                    for (double t_step = start_t + (1.0 / 60.0); t_step + 1e-6 < target_t;
                         t_step += (1.0 / 60.0)) {
                        we_puppet_eval_skin_matrices_ex(puppet, puppet->layers, puppet->layer_count,
                                                        slot ? &slot->rig_ctx : NULL, t_step,
                                                        puppet->skin_mats);
                    }
                }
                we_puppet_eval_skin_matrices_ex(puppet, puppet->layers, puppet->layer_count,
                                                slot ? &slot->rig_ctx : NULL, target_t,
                                                puppet->skin_mats);
            }
        } else if (puppet->physics_ctx && step_phys) {
            (void)we_physics_advance_wall_clock(puppet->physics_ctx, target_t);
        }
        rig_bump_scene_revision(sc);
    }
    return true;
}

bool daemon_dispatch_ipc_req_export_puppet_mdl(daemon_state_t *state,
                                               const waywal_ipc_export_puppet_mdl_t *req,
                                               waywal_ipc_export_puppet_mdl_resp_t *resp,
                                               uint8_t **out_mdl_blob, uint32_t *out_mdl_size)
{
    if (!resp) {
        return false;
    }
    memset(resp, 0, sizeof(*resp));
    if (out_mdl_blob) {
        *out_mdl_blob = NULL;
    }
    if (out_mdl_size) {
        *out_mdl_size = 0u;
    }
    if (!req || (req->flags & 0x3u) == 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }
    resp->object_id = req->object_id;

    char resolved_path[PATH_MAX];
    resolved_path[0] = '\0';
    if ((req->flags & 0x2u) != 0u) {
        if (!memchr(req->out_path, '\0', sizeof(req->out_path)) || req->out_path[0] == '\0' ||
            !waywal_ipc_export_path_is_allowed(req->out_path, resolved_path,
                                               sizeof(resolved_path))) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
        struct stat st;
        if (lstat(resolved_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    if (!state) {
        resp->status = (int32_t)WAYWAL_STATUS_OK;
        return true;
    }

    rig_target_list_t targets;
    rig_collect_targets(state, req->object_id, true, &targets);
    if (targets.count == 0u) {
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    we_puppet_t *puppet = targets.objects[0]->puppet;
    we_mdl_write_opts_t opts;
    we_mdl_write_opts_default(&opts);

    bool wants_inline = (req->flags & 0x1u) != 0u;
    uint8_t *mdl_bytes = NULL;
    size_t mdl_size = 0u;
    if (!we_mdl_write_mem(puppet, &opts, &mdl_bytes, &mdl_size) || !mdl_bytes || mdl_size == 0u ||
        mdl_size > (size_t)UINT32_MAX ||
        (wants_inline && mdl_size > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES - sizeof(*resp))) {
        we_mdl_write_free(mdl_bytes);
        resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
        return false;
    }

    if ((req->flags & 0x2u) != 0u) {
        if (!we_mdl_write_file(puppet, &opts, resolved_path)) {
            we_mdl_write_free(mdl_bytes);
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
    }

    if ((req->flags & 0x1u) != 0u && out_mdl_blob && out_mdl_size) {
        uint8_t *copy = (uint8_t *)malloc(mdl_size);
        if (!copy) {
            we_mdl_write_free(mdl_bytes);
            resp->status = (int32_t)WAYWAL_STATUS_ERR_SYNTAX;
            return false;
        }
        memcpy(copy, mdl_bytes, mdl_size);
        *out_mdl_blob = copy;
        *out_mdl_size = (uint32_t)mdl_size;
    }

    we_mdl_write_free(mdl_bytes);
    resp->status = (int32_t)WAYWAL_STATUS_OK;
    resp->mdl_byte_size = (uint32_t)mdl_size;
    return true;
}

static bool rig_ipc_handle_set_mesh(daemon_state_t *state, int client_fd,
                                    const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_puppet_mesh_ack_t resp;
    waywal_ipc_hdr_t rh;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_set_mesh_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        waywal_ipc_serialize_resp_puppet_mesh_ack((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u,
                                                  &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    const waywal_ipc_puppet_set_mesh_t *req = NULL;
    const waywal_ipc_mdl_vertex_t *verts = NULL;
    const uint16_t *indices = NULL;
    if (!waywal_ipc_validate_req_puppet_set_mesh(pkt, pkt_len, &req, &verts, &indices)) {
        free(pkt);
        waywal_ipc_serialize_resp_puppet_mesh_ack((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, 0u, 0u,
                                                  &rh, &resp);
        return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
    }
    (void)daemon_dispatch_ipc_req_puppet_set_mesh(state, req, verts, indices, &resp);
    free(pkt);
    waywal_ipc_serialize_resp_puppet_mesh_ack(resp.status, resp.object_id, resp.vertex_count,
                                              resp.triangle_count, &rh, &resp);
    return authoring_send_msg(client_fd, &rh, &resp, sizeof(resp));
}

static bool rig_ipc_handle_set_skeleton(daemon_state_t *state, int client_fd,
                                        const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_set_skeleton_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        return rig_send_status_reply(client_fd, false, "Invalid set_skeleton packet size");
    }
    const waywal_ipc_puppet_set_skeleton_t *req = NULL;
    const waywal_ipc_mdl_bone_t *bones = NULL;
    if (!waywal_ipc_validate_req_puppet_set_skeleton(pkt, pkt_len, &req, &bones)) {
        free(pkt);
        return rig_send_status_reply(client_fd, false, "Invalid set_skeleton payload");
    }
    bool ok = daemon_dispatch_ipc_req_puppet_set_skeleton(state, req, bones);
    free(pkt);
    return rig_send_status_reply(client_fd, ok, ok ? NULL : "Failed to apply puppet skeleton");
}

static bool rig_ipc_handle_solve_weights(daemon_state_t *state, int client_fd,
                                         const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_hdr_t rh;
    waywal_ipc_puppet_weights_resp_t err_resp;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_solve_weights_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_weights((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                 &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    const waywal_ipc_puppet_solve_weights_t *req = NULL;
    const uint64_t *locked_bs = NULL;
    if (!waywal_ipc_validate_req_puppet_solve_weights(pkt, pkt_len, &req, &locked_bs)) {
        free(pkt);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_weights((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                 &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    waywal_ipc_puppet_weights_resp_t resp;
    waywal_ipc_vertex_weight_t *entries = NULL;
    uint32_t entry_count = 0u;
    (void)daemon_dispatch_ipc_req_puppet_solve_weights(state, req, locked_bs, &resp, &entries,
                                                       &entry_count);
    free(pkt);

    size_t out_cap = sizeof(waywal_ipc_puppet_weights_resp_t) +
                     (size_t)entry_count * sizeof(waywal_ipc_vertex_weight_t);
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) {
        free(entries);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_weights((int32_t)WAYWAL_STATUS_ERR_SYNTAX, resp.object_id,
                                                 NULL, 0u, &rh, &err_resp, sizeof(err_resp),
                                                 &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    size_t out_len = 0u;
    if (!waywal_ipc_serialize_resp_puppet_weights(resp.status, resp.object_id, entries, entry_count,
                                                  &rh, out_buf, out_cap, &out_len)) {
        free(out_buf);
        free(entries);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_weights((int32_t)WAYWAL_STATUS_ERR_SYNTAX, resp.object_id,
                                                 NULL, 0u, &rh, &err_resp, sizeof(err_resp),
                                                 &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    free(entries);
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

static bool rig_ipc_handle_paint_weights(daemon_state_t *state, int client_fd,
                                         const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_paint_weights_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        return rig_send_status_reply(client_fd, false, "Invalid paint_weights packet size");
    }
    const waywal_ipc_puppet_paint_weights_t *req = NULL;
    const waywal_ipc_vertex_weight_t *entries = NULL;
    if (!waywal_ipc_validate_req_puppet_paint_weights(pkt, pkt_len, &req, &entries)) {
        free(pkt);
        return rig_send_status_reply(client_fd, false, "Invalid paint_weights payload");
    }
    bool ok = daemon_dispatch_ipc_req_puppet_paint_weights(state, req, entries);
    free(pkt);
    return rig_send_status_reply(client_fd, ok, ok ? NULL : "Failed to paint puppet weights");
}

static bool rig_ipc_handle_heatmap_mode(daemon_state_t *state, int client_fd,
                                        const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_heatmap_mode_t),
                                   sizeof(waywal_ipc_puppet_heatmap_mode_t), &pkt, &pkt_len)) {
        return rig_send_status_reply(client_fd, false, "Invalid heatmap_mode packet size");
    }
    const waywal_ipc_puppet_heatmap_mode_t *req = NULL;
    if (!waywal_ipc_validate_req_puppet_heatmap_mode(pkt, pkt_len, &req)) {
        free(pkt);
        return rig_send_status_reply(client_fd, false, "Invalid heatmap_mode payload");
    }
    bool ok = daemon_dispatch_ipc_req_puppet_set_heatmap_mode(state, req);
    free(pkt);
    return rig_send_status_reply(client_fd, ok, ok ? NULL : "Failed to set heatmap mode");
}

static bool rig_ipc_handle_preview_ik(daemon_state_t *state, int client_fd,
                                      const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_hdr_t rh;
    waywal_ipc_puppet_ik_pose_resp_t err_resp;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_preview_ik_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_ik_pose((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                 &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    const waywal_ipc_puppet_preview_ik_t *req = NULL;
    const waywal_ipc_bone_trs_t *overrides = NULL;
    if (!waywal_ipc_validate_req_puppet_preview_ik(pkt, pkt_len, &req, &overrides)) {
        free(pkt);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_ik_pose((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                 &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    waywal_ipc_puppet_ik_pose_resp_t resp;
    waywal_ipc_bone_trs_t *bones = NULL;
    uint32_t bone_count = 0u;
    (void)daemon_dispatch_ipc_req_puppet_preview_ik(state, req, overrides, &resp, &bones,
                                                    &bone_count);
    free(pkt);

    size_t out_cap = sizeof(waywal_ipc_puppet_ik_pose_resp_t) +
                     (size_t)bone_count * sizeof(waywal_ipc_bone_trs_t);
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) {
        free(bones);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_ik_pose((int32_t)WAYWAL_STATUS_ERR_SYNTAX, resp.object_id,
                                                 NULL, 0u, &rh, &err_resp, sizeof(err_resp),
                                                 &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    size_t out_len = 0u;
    if (!waywal_ipc_serialize_resp_puppet_ik_pose(resp.status, resp.object_id, bones, bone_count,
                                                  &rh, out_buf, out_cap, &out_len)) {
        free(out_buf);
        free(bones);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_puppet_ik_pose((int32_t)WAYWAL_STATUS_ERR_SYNTAX, resp.object_id,
                                                 NULL, 0u, &rh, &err_resp, sizeof(err_resp),
                                                 &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    free(bones);
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

static bool rig_ipc_handle_set_anim_tracks(daemon_state_t *state, int client_fd,
                                           const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_puppet_set_anim_tracks_t),
                                   WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES, &pkt, &pkt_len)) {
        return rig_send_status_reply(client_fd, false, "Invalid set_anim_tracks packet size");
    }
    const waywal_ipc_puppet_set_anim_tracks_t *req = NULL;
    const uint8_t *clips_blob = NULL;
    size_t clips_blob_len = 0u;
    if (!waywal_ipc_validate_req_puppet_set_anim_tracks(pkt, pkt_len, &req, &clips_blob,
                                                        &clips_blob_len)) {
        free(pkt);
        return rig_send_status_reply(client_fd, false, "Invalid set_anim_tracks payload");
    }
    bool ok =
        daemon_dispatch_ipc_req_puppet_set_anim_tracks(state, req, clips_blob, clips_blob_len);
    free(pkt);
    return rig_send_status_reply(client_fd, ok, ok ? NULL : "Failed to update puppet animations");
}

static bool rig_ipc_handle_scrub_time(daemon_state_t *state, int client_fd,
                                      const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size, sizeof(waywal_ipc_puppet_scrub_t),
                                   sizeof(waywal_ipc_puppet_scrub_t), &pkt, &pkt_len)) {
        return rig_send_status_reply(client_fd, false, "Invalid scrub_time packet size");
    }
    const waywal_ipc_puppet_scrub_t *req = NULL;
    if (!waywal_ipc_validate_req_puppet_scrub_time(pkt, pkt_len, &req)) {
        free(pkt);
        return rig_send_status_reply(client_fd, false, "Invalid scrub_time payload");
    }
    bool ok = daemon_dispatch_ipc_req_puppet_scrub_time(state, req);
    free(pkt);
    return rig_send_status_reply(client_fd, ok, ok ? NULL : "Failed to scrub puppet time");
}

static bool rig_ipc_handle_export_mdl(daemon_state_t *state, int client_fd,
                                      const waywal_ipc_hdr_t *hdr)
{
    uint8_t *pkt = NULL;
    size_t pkt_len = 0u;
    waywal_ipc_hdr_t rh;
    waywal_ipc_export_puppet_mdl_resp_t err_resp;
    if (!authoring_read_var_packet(client_fd, hdr->payload_size,
                                   sizeof(waywal_ipc_export_puppet_mdl_t),
                                   sizeof(waywal_ipc_export_puppet_mdl_t), &pkt, &pkt_len)) {
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_export_puppet_mdl((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                    &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    const waywal_ipc_export_puppet_mdl_t *req = NULL;
    if (!waywal_ipc_validate_req_export_puppet_mdl(pkt, pkt_len, &req)) {
        free(pkt);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_export_puppet_mdl((int32_t)WAYWAL_STATUS_ERR_SYNTAX, 0u, NULL, 0u,
                                                    &rh, &err_resp, sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }

    waywal_ipc_export_puppet_mdl_resp_t resp;
    uint8_t *mdl_blob = NULL;
    uint32_t mdl_size = 0u;
    (void)daemon_dispatch_ipc_req_export_puppet_mdl(state, req, &resp, &mdl_blob, &mdl_size);
    free(pkt);

    size_t out_cap =
        sizeof(waywal_ipc_export_puppet_mdl_resp_t) + (mdl_blob ? (size_t)mdl_size : 0u);
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) {
        free(mdl_blob);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_export_puppet_mdl((int32_t)WAYWAL_STATUS_ERR_SYNTAX,
                                                    resp.object_id, NULL, 0u, &rh, &err_resp,
                                                    sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    size_t out_len = 0u;
    if (!waywal_ipc_serialize_resp_export_puppet_mdl(resp.status, resp.object_id, mdl_blob,
                                                     mdl_blob ? mdl_size : resp.mdl_byte_size, &rh,
                                                     out_buf, out_cap, &out_len)) {
        free(out_buf);
        free(mdl_blob);
        size_t err_len = 0u;
        waywal_ipc_serialize_resp_export_puppet_mdl((int32_t)WAYWAL_STATUS_ERR_SYNTAX,
                                                    resp.object_id, NULL, 0u, &rh, &err_resp,
                                                    sizeof(err_resp), &err_len);
        return authoring_send_msg(client_fd, &rh, &err_resp, err_len);
    }
    free(mdl_blob);
    bool sent = authoring_send_msg(client_fd, &rh, out_buf, out_len);
    free(out_buf);
    return sent;
}

bool daemon_dispatch_rigging_ipc(daemon_state_t *state, int client_fd, const waywal_ipc_hdr_t *hdr,
                                 int attached_fd)
{
    if (attached_fd >= 0) {
        close(attached_fd);
    }
    if (!hdr) {
        return false;
    }
    if (hdr->magic != WAYWAL_IPC_MAGIC || hdr->version != WAYWAL_IPC_VERSION) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }

    switch (hdr->opcode) {
    case WAYWAL_REQ_PUPPET_SET_MESH:
        return rig_ipc_handle_set_mesh(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SET_SKELETON:
        return rig_ipc_handle_set_skeleton(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS:
        return rig_ipc_handle_solve_weights(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS:
        return rig_ipc_handle_paint_weights(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE:
        return rig_ipc_handle_heatmap_mode(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_PREVIEW_IK:
        return rig_ipc_handle_preview_ik(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS:
        return rig_ipc_handle_set_anim_tracks(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
        return rig_ipc_handle_scrub_time(state, client_fd, hdr);
    case WAYWAL_REQ_EXPORT_PUPPET_MDL:
        return rig_ipc_handle_export_mdl(state, client_fd, hdr);
    default:
        break;
    }

    if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
        return false;
    }
    return false;
}

bool daemon_dispatch_authoring_ipc(daemon_state_t *state, int client_fd,
                                   const waywal_ipc_hdr_t *hdr, int attached_fd)
{
    if (attached_fd >= 0) {
        close(attached_fd);
    }
    if (!hdr || client_fd < 0) {
        return false;
    }
    if (hdr->magic != WAYWAL_IPC_MAGIC || hdr->version != WAYWAL_IPC_VERSION) {
        (void)shutdown(client_fd, SHUT_RDWR);
        return false;
    }

    switch (hdr->opcode) {
    case WAYWAL_REQ_SCENE_MUTATE:
        return authoring_ipc_handle_scene_mutate(state, client_fd, hdr);
    case WAYWAL_REQ_HOT_RELOAD_CODE:
        return authoring_ipc_handle_hot_reload(state, client_fd, hdr);
    case WAYWAL_REQ_QUERY_SCENE_TREE:
        return authoring_ipc_handle_query_tree(state, client_fd, hdr);
    case WAYWAL_REQ_SET_SHADER_COMBO:
        return authoring_ipc_handle_shader_combo(state, client_fd, hdr);
    case WAYWAL_REQ_BRUSH_STROKE:
        return authoring_ipc_handle_brush_stroke(state, client_fd, hdr);
    case WAYWAL_REQ_PARTICLE_CONFIG:
        return authoring_ipc_handle_particle_config(state, client_fd, hdr);
    case WAYWAL_REQ_SET_AUDIO_BINDINGS:
        return authoring_ipc_handle_audio_bindings(state, client_fd, hdr);
    case WAYWAL_REQ_EXPORT_SCENE_PKG:
        return authoring_ipc_handle_export_pkg(state, client_fd, hdr);
    case WAYWAL_REQ_LINT_SCENESCRIPT:
        return authoring_ipc_handle_lint_script(state, client_fd, hdr);
    case WAYWAL_REQ_READBACK_TEXTURE:
        return authoring_ipc_handle_readback(state, client_fd, hdr);
    case WAYWAL_REQ_PUPPET_SET_MESH:
    case WAYWAL_REQ_PUPPET_SET_SKELETON:
    case WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS:
    case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS:
    case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE:
    case WAYWAL_REQ_PUPPET_PREVIEW_IK:
    case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS:
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
    case WAYWAL_REQ_EXPORT_PUPPET_MDL:
        return daemon_dispatch_rigging_ipc(state, client_fd, hdr, -1);
    default:
        break;
    }

    if (!authoring_drain_payload(client_fd, hdr->payload_size, 500)) {
        return false;
    }
    return false;
}

int main(int argc, char *argv[])
{
    (void)setenv("CUDA_MODULE_LOADING", "LAZY", 0);
    (void)setenv("NVD_MAX_DETACHED_BACKING_IMAGES", "0", 0);
    (void)setenv("NVD_MAX_DETACHED_BACKING_IMAGE_BYTES", "0", 0);
#if defined(__GLIBC__)
    (void)mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    (void)mallopt(M_TRIM_THRESHOLD, 64 * 1024);
#endif
    daemon_state_t state;
    memset(&state, 0, sizeof(state));
    state.shader_timer_fd = -1;
    state.anim_timer_fd = -1;
    state.solar_timer_fd = -1;
    daemon_copy_cstr(state.namespace_str, sizeof(state.namespace_str), "default");
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
        daemon_copy_cstr(state.backdrop_path, sizeof(state.backdrop_path),
                         state.config.daemon.backdrop);
    }
    if (state.config.daemon.namespace_str[0] != '\0' &&
        strcmp(state.config.daemon.namespace_str, "waywal") != 0) {
        daemon_copy_cstr(state.namespace_str, sizeof(state.namespace_str),
                         state.config.daemon.namespace_str);
    }
    const char *env_ns = getenv("AWWW_NAMESPACE");
    if (!env_ns || env_ns[0] == '\0') {
        env_ns = getenv("SWWW_NAMESPACE");
    }
    if (env_ns && env_ns[0] != '\0') {
        daemon_copy_cstr(state.namespace_str, sizeof(state.namespace_str), env_ns);
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
            daemon_copy_cstr(state.namespace_str, sizeof(state.namespace_str), optarg);
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
                daemon_copy_cstr(state.backdrop_path, sizeof(state.backdrop_path), optarg);
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
            uring_loop_add_poll(&uring_loop, gov_ipc_fd, POLLIN, URING_EV_POWER_IPC, &state);
        }
        if (gov_inotify_fd >= 0) {
            uring_loop_add_poll(&uring_loop, gov_inotify_fd, POLLIN, URING_EV_POWER_INOTIFY,
                                &state);
        }
        if (mpris_fd >= 0) {
            uring_loop_add_poll(&uring_loop, mpris_fd, POLLIN, URING_EV_MPRIS, &state);
        }
        WAYWAL_LOG_INFO("Event loop running via Linux io_uring (zero-syscall polling)");
    } else {
        WAYWAL_LOG_INFO("Event loop running via epoll fallback");
    }

    /* Ensure package extraction and repair directories exist before Landlock enumerates
     * ~/.local/share */
    const char *home_dir = getenv("HOME");
    if (home_dir && home_dir[0] == '/' && strstr(home_dir, "..") == NULL) {
        const char *wwal_dirs[] = {
            ".local",
            ".local/share",
            ".local/share/wwal",
            ".local/share/wwal/packages",
            ".local/state",
            ".local/state/wwal",
            ".local/state/wwal/packages",
            ".cache",
            ".cache/wwal",
            ".cache/wwal/repairs",
        };
        char dir_buf[4096];
        for (size_t di = 0; di < sizeof(wwal_dirs) / sizeof(wwal_dirs[0]); ++di) {
            int dn = snprintf(dir_buf, sizeof(dir_buf), "%s/%s", home_dir, wwal_dirs[di]);
            if (dn > 0 && (size_t)dn < sizeof(dir_buf)) {
                (void)mkdir(dir_buf, 0700);
            }
        }
    }

    /* Apply security hardening (PR_SET_NO_NEW_PRIVS & Landlock LSM) */
    security_sandbox_register_config(&state.config);
    security_sandbox_apply(NULL);

    /* Spawn bounded IPC worker threads after sandbox confinement so workers inherit
     * Landlock/Seccomp */
    if (daemon_ipc_async_init(&state)) {
        int ipc_async_fd = daemon_ipc_async_get_fd();
        if (ipc_async_fd >= 0) {
            register_fd_in_loops(&state, ipc_async_fd, URING_EV_IPC_READ);
        }
    }

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
        ssize_t wr = write(ready_pipe[1], &ok_byte, 1);
        (void)wr;
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
                } else if (ev_fd == daemon_ipc_async_get_fd()) {
                    daemon_ipc_async_dispatch(&state);
                    wl_display_flush(state.display);
                } else if (daemon_ipc_dispatch_subscriber_fd(&state, ev_fd)) {
                    wl_display_flush(state.display);
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
                    if (!waywal_mpris_dispatch(state.mpris)) {
                        unregister_fd_from_loops(&state, ev_fd);
                    }
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

    {
        int ipc_async_fd = daemon_ipc_async_get_fd();
        if (ipc_async_fd >= 0) {
            unregister_fd_from_loops(&state, ipc_async_fd);
        }
        daemon_ipc_async_shutdown(&state);
    }

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
        if (state.dmabuf_ctx.available) {
            render_engine_make_current(&state.render_engine);
        }
        authoring_invalidate_scene(state.we_scene);
        we_scene_destroy(state.we_scene);
        state.we_scene = NULL;
    }
    if (state.dmabuf_ctx.available) {
        render_engine_make_current(&state.render_engine);
    }
    daemon_authoring_cleanup();
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
#ifdef HAVE_LIBSECCOMP
#if defined(__SANITIZE_ADDRESS__)
    fflush(NULL);
    _exit(0);
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
    fflush(NULL);
    _exit(0);
#endif
#endif
#endif
    return 0;
}
