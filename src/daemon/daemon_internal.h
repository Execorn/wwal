// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_DAEMON_INTERNAL_H
#define WAYWAL_DAEMON_INTERNAL_H

#include "gpu_compute.h"
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
#include "waywal/uring_loop.h"
#include "waywal/we_scene.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

extern waywal_state_store_t g_saved_state;

__attribute__((format(printf, 4, 5))) static inline void
buf_appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...)
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

void disarm_timer_fd(int timer_fd);
void register_fd_in_loops(daemon_state_t *state, int fd, uring_event_type_t ev_type);
void unregister_fd_from_loops(daemon_state_t *state, int fd);
bool output_matches_target_spec(const output_node_t *out, const char *target_spec);
bool output_matches_img_meta(const output_node_t *out, const waywal_img_metadata_t *meta);
void stop_engines_on_output(daemon_state_t *state, output_node_t *out, bool keep_shader,
                            bool keep_anim, bool keep_video, bool keep_slideshow);
void shader_engine_stop_playback(daemon_state_t *state);
void anim_engine_stop_playback(daemon_state_t *state);
bool try_start_native_animation(daemon_state_t *state, const char *filepath,
                                const waywal_img_metadata_t *meta, const char *target_out);
void daemon_shader_render_once(daemon_state_t *state);
void daemon_apply_mpris_album_art_and_theme(daemon_state_t *state,
                                            const waywal_mpris_state_t *mstate,
                                            bool force_art_upload);
void daemon_update_palette_from_bgra(daemon_state_t *state, const uint8_t *bgra_pixels, uint32_t w,
                                     uint32_t h, const waywal_img_metadata_t *meta,
                                     bool export_and_hooks);
bool daemon_activate_depth_parallax(daemon_state_t *state, const uint8_t *bgra_pixels, uint32_t w,
                                    uint32_t h, const waywal_img_metadata_t *meta,
                                    const char *target_output);
void upsert_workshop_prop_kv(char *props_buf, size_t cap, const char *key, const char *kv_pair);
bool daemon_activate_shader(daemon_state_t *state, const waywal_shader_payload_t *spay);
bool daemon_start_slideshow_isolated(daemon_state_t *state, const waywal_slideshow_payload_t *spay);
bool daemon_activate_workshop(daemon_state_t *state, const waywal_workshop_payload_t *wpay);
bool daemon_restore_output_entry(daemon_state_t *state, const waywal_output_saved_state_t *saved,
                                 const char *override_output);

void daemon_broadcast_event(daemon_state_t *state, const char *event_name);
void daemon_reap_dead_subscribers(daemon_state_t *state);
size_t daemon_build_status_json(daemon_state_t *state, const char *event_name, char *buf,
                                size_t cap);

#endif /* WAYWAL_DAEMON_INTERNAL_H */
