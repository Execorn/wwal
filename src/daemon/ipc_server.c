// SPDX-License-Identifier: GPL-3.0-or-later
#include "daemon_internal.h"

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

size_t daemon_build_status_json(daemon_state_t *state, const char *event_name, char *buf,
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
        } else if (!out->displaying_color && out->active_media_path[0] != '\0') {
            mpath = out->active_media_path;
        } else {
            const waywal_output_saved_state_t *sv =
                waywal_state_store_find_output(&g_saved_state, out->name);
            if (!sv) {
                sv = waywal_state_store_find_output(&g_saved_state, "*");
            }
            if (sv && sv->kind != WAYWAL_CONTENT_SOLID_COLOR) {
                mpath = sv->media_path;
            }
        }
        char esc_oname[128];
        char esc_odesc[256];
        char esc_mpath[4096];
        waywal_json_escape_string(out->name[0] ? out->name : "unknown", esc_oname,
                                  sizeof(esc_oname));
        waywal_json_escape_string(out->description[0] ? out->description : "unknown", esc_odesc,
                                  sizeof(esc_odesc));
        waywal_json_escape_string(mpath, esc_mpath, sizeof(esc_mpath));

        double scale_val = json_safe_float(fscale > 0 ? ((float)fscale / 120.0f) : 1.0f);
        char out_entry[6144];
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

void daemon_reap_dead_subscribers(daemon_state_t *state)
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

void daemon_broadcast_event(daemon_state_t *state, const char *event_name)
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

static bool output_matches_extra(const output_node_t *out, const char *extra_buf, size_t extra_len,
                                 uint32_t count)
{
    if (!out || !extra_buf || extra_len == 0 || count == 0)
        return false;
    const char *ptr = extra_buf;
    size_t rem = extra_len;
    uint32_t i = 0;
    while (i < count && rem > 0) {
        const char *end = (const char *)memchr(ptr, '\0', rem);
        if (!end)
            break;
        size_t step = (size_t)(end - ptr) + 1;
        if (end > ptr) {
            if (strcmp(ptr, "*") == 0 || output_matches_target_spec(out, ptr))
                return true;
            i++;
        }
        ptr += step;
        rem -= step;
    }
    return false;
}

static bool extra_has_wildcard(const char *extra_buf, size_t extra_len, uint32_t count)
{
    if (!extra_buf || extra_len == 0 || count == 0)
        return false;
    const char *ptr = extra_buf;
    size_t rem = extra_len;
    uint32_t i = 0;
    while (i < count && rem > 0) {
        const char *end = (const char *)memchr(ptr, '\0', rem);
        if (!end)
            break;
        size_t step = (size_t)(end - ptr) + 1;
        if (end > ptr) {
            if (strcmp(ptr, "*") == 0)
                return true;
            i++;
        }
        ptr += step;
        rem -= step;
    }
    return false;
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
        char filter_buf[4097];
        size_t filter_len = 0;
        uint32_t num_targets = 0;
        if (req_hdr.payload_size > 0) {
            if (req_hdr.payload_size <= 4096) {
                if (!read_exact(client_fd, filter_buf, req_hdr.payload_size)) {
                    resp_hdr.opcode = WAYWAL_RESP_ERR;
                    waywal_ipc_send(client_fd, &resp_hdr, -1);
                    break;
                }
                filter_len = req_hdr.payload_size + 1;
                filter_buf[req_hdr.payload_size] = '\0';
                const char *p = filter_buf;
                size_t rem = filter_len;
                while (rem > 0) {
                    const char *end = (const char *)memchr(p, '\0', rem);
                    if (!end)
                        break;
                    if (end > p)
                        num_targets++;
                    size_t step = (size_t)(end - p) + 1;
                    p += step;
                    rem -= step;
                }
            } else {
                uint8_t drain[1024];
                size_t rem_drain = req_hdr.payload_size;
                while (rem_drain > 0) {
                    size_t chunk = rem_drain > sizeof(drain) ? sizeof(drain) : rem_drain;
                    if (!read_exact(client_fd, drain, chunk))
                        break;
                    rem_drain -= chunk;
                }
                resp_hdr.opcode = WAYWAL_RESP_ERR;
                waywal_ipc_send(client_fd, &resp_hdr, -1);
                break;
            }
        }

        char query_buf[8192];
        size_t off = 0;
        const char *ns = state->namespace_str[0] != '\0' ? state->namespace_str : "default";

        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
            const char *oname = out->name[0] ? out->name : "unknown";
            if (num_targets > 0 &&
                !output_matches_extra(out, filter_buf, filter_len, num_targets)) {
                continue;
            }
            int32_t sc = out->scale_factor > 0 ? out->scale_factor : 1;
            const char *media_path = out->active_media_path;

            if (!out->displaying_color && media_path[0] == '\0') {
                if (out->workshop_active && out->workshop_dir[0] != '\0') {
                    media_path = out->workshop_dir;
                } else if (out->shader_active && out->shader_path[0] != '\0') {
                    media_path = out->shader_path;
                } else if (out->video_eng && out->video_eng->filepath[0] != '\0') {
                    media_path = out->video_eng->filepath;
                } else if (out->anim_active && out->anim_meta.filepath[0] != '\0') {
                    media_path = out->anim_meta.filepath;
                } else if (out->slideshow_eng && out->slideshow_eng->num_files > 0 &&
                           out->slideshow_eng->current_file_idx < out->slideshow_eng->num_files &&
                           out->slideshow_eng->file_list &&
                           out->slideshow_eng->file_list[out->slideshow_eng->current_file_idx]) {
                    media_path =
                        out->slideshow_eng->file_list[out->slideshow_eng->current_file_idx];
                } else {
                    const waywal_output_saved_state_t *s_ent =
                        waywal_state_store_find_output(&g_saved_state, oname);
                    if (s_ent && s_ent->kind != WAYWAL_CONTENT_SOLID_COLOR &&
                        s_ent->media_path[0] != '\0') {
                        media_path = s_ent->media_path;
                    }
                }
            }

            if (!out->displaying_color && media_path && media_path[0] != '\0') {
                buf_appendf(query_buf, sizeof(query_buf), &off,
                            "%s: %s: %dx%d, scale: %d, currently displaying: image: %s\n", ns,
                            oname, out->width, out->height, sc, media_path);
            } else {
                color_rgba_t c =
                    out->displaying_color
                        ? out->active_color
                        : (out->has_fill_color ? out->fill_color : state->current_color);
                buf_appendf(
                    query_buf, sizeof(query_buf), &off,
                    "%s: %s: %dx%d, scale: %d, currently displaying: color: #%02x%02x%02x\n", ns,
                    oname, out->width, out->height, sc, c.r, c.g, c.b);
            }
        }

        resp_hdr.opcode = WAYWAL_RESP_INFO;
        resp_hdr.payload_size = (uint32_t)off;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (off > 0) {
            (void)write_exact(client_fd, query_buf, off);
        }
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
                char extra_buf[4097];
                size_t raw_extra_len = req_hdr.payload_size - sizeof(clear_payload);
                size_t extra_len = 0;
                bool has_targets = false;
                if (clear_payload.num_target_outputs > 0 &&
                    clear_payload.num_target_outputs <= 64 && raw_extra_len > 0 &&
                    raw_extra_len <= 4096) {
                    if (read_exact(client_fd, extra_buf, raw_extra_len)) {
                        extra_buf[raw_extra_len] = '\0';
                        extra_len = raw_extra_len + 1;
                        if (!extra_has_wildcard(extra_buf, extra_len,
                                                clear_payload.num_target_outputs)) {
                            has_targets = true;
                        }
                    }
                }

                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                s_entry.kind = WAYWAL_CONTENT_SOLID_COLOR;
                s_entry.solid_color = ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
                                      ((uint32_t)color.b << 8) | (uint32_t)color.a;

                uint8_t solid_px[64 * 64 * 4];
                for (size_t i = 0; i < 64 * 64; ++i) {
                    solid_px[i * 4 + 0] = color.b;
                    solid_px[i * 4 + 1] = color.g;
                    solid_px[i * 4 + 2] = color.r;
                    solid_px[i * 4 + 3] = color.a;
                }
                char matched_outputs[512] = {0};
                size_t mo_len = 0;

                if (has_targets && state->outputs) {
                    const char *ptr = extra_buf;
                    size_t rem = extra_len;
                    for (uint32_t i = 0; i < clear_payload.num_target_outputs && rem > 0; ++i) {
                        const char *end = (const char *)memchr(ptr, '\0', rem);
                        if (!end)
                            break;
                        bool any_matched = false;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (output_matches_target_spec(out, ptr)) {
                                stop_engines_on_output(state, out, false, false, false, false);
                                output_node_render_color(state, out, color);
                                if (out->name[0] != '\0') {
                                    memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                                    strncpy(s_entry.output_name, out->name,
                                            sizeof(s_entry.output_name) - 1);
                                    (void)waywal_state_store_update_output(&g_saved_state,
                                                                           &s_entry);
                                    int n = snprintf(matched_outputs + mo_len,
                                                     sizeof(matched_outputs) - mo_len, "%s%s",
                                                     mo_len ? "," : "", out->name);
                                    if (n > 0 && mo_len + (size_t)n < sizeof(matched_outputs)) {
                                        mo_len += (size_t)n;
                                    }
                                }
                                any_matched = true;
                            }
                        }
                        if (!any_matched && ptr[0] != '\0') {
                            memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                            strncpy(s_entry.output_name, ptr, sizeof(s_entry.output_name) - 1);
                            (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                            int n =
                                snprintf(matched_outputs + mo_len, sizeof(matched_outputs) - mo_len,
                                         "%s%s", mo_len ? "," : "", ptr);
                            if (n > 0 && mo_len + (size_t)n < sizeof(matched_outputs)) {
                                mo_len += (size_t)n;
                            }
                        }
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
                            int n =
                                snprintf(matched_outputs + mo_len, sizeof(matched_outputs) - mo_len,
                                         "%s%s", mo_len ? "," : "", out->name);
                            if (n > 0 && mo_len + (size_t)n < sizeof(matched_outputs)) {
                                mo_len += (size_t)n;
                            }
                        }
                    }
                }
                state->solar_owns_wallpaper = false;
                state->active_dynwall_path[0] = '\0';
                state->last_dynwall_frame_path[0] = '\0';
                (void)waywal_ecosystem_cache_sync(mo_len > 0 ? matched_outputs : "*", "",
                                                  "Lanczos3", solid_px, 64, 64,
                                                  state->config.general.caelestia_sync, false);
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
                    bool valid_strings = true;
                    if (hdr_size < sizeof(waywal_img_metadata_t)) {
                        compat_buf = (uint8_t *)calloc(1, sizeof(waywal_img_metadata_t) +
                                                              raw_meta->extra_data_len + 1u);
                        if (!compat_buf) {
                            munmap(mapped, req_hdr.payload_size);
                            if (attached_fd >= 0) {
                                close(attached_fd);
                                attached_fd = -1;
                            }
                            resp_hdr.opcode = WAYWAL_RESP_ERR;
                            waywal_ipc_send(client_fd, &resp_hdr, -1);
                            break;
                        }
                        memcpy(compat_buf, mapped, hdr_size);
                        if (raw_meta->extra_data_len > 0) {
                            memcpy(compat_buf + sizeof(waywal_img_metadata_t),
                                   (const uint8_t *)mapped + hdr_size, raw_meta->extra_data_len);
                        }
                        meta = (waywal_img_metadata_t *)compat_buf;
                    }

                    if (valid_strings && meta->num_target_outputs > 64) {
                        valid_strings = false;
                    } else if (valid_strings) {
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
                        bool duplicate_inflight = false;
                        if (state->outputs && !meta->is_solid_color && meta->filepath[0] != '\0' &&
                            meta->custom_shader_len == 0 && !meta->extract_theme &&
                            !meta->has_seed_color && meta->depth_map_path[0] == '\0' &&
                            meta->transition_type != WAYWAL_TRANSITION_NONE &&
                            meta->transition_duration_ms > 0) {
                            uint32_t matched_cnt = 0;
                            uint32_t inflight_same_cnt = 0;
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (!out->surface || !out->configured || out->width <= 0 ||
                                    out->height <= 0) {
                                    continue;
                                }
                                if (output_matches_img_meta(out, meta)) {
                                    matched_cnt++;
                                    if (out->transition_active && !out->displaying_color &&
                                        strcmp(out->active_media_path, meta->filepath) == 0 &&
                                        out->scaling_mode == meta->scaling_mode &&
                                        out->filter_mode == meta->filter_mode &&
                                        out->ten_bit == (meta->ten_bit != 0)) {
                                        inflight_same_cnt++;
                                    }
                                }
                            }
                            if (matched_cnt > 0 && matched_cnt == inflight_same_cnt) {
                                duplicate_inflight = true;
                            }
                        }

                        if (duplicate_inflight) {
                            WAYWAL_LOG_INFO("Skipping duplicate in-flight transition for %s",
                                            meta->filepath);
                            ok = true;
                        } else {
                            const uint8_t *pixels =
                                (const uint8_t *)mapped + hdr_size + meta->extra_data_len;
                            WAYWAL_LOG_INFO("Rendering received image: %ux%u", meta->width,
                                            meta->height);

                            const char *first_target = "*";
                            if (meta->num_target_outputs > 0) {
                                first_target = (const char *)meta + sizeof(waywal_img_metadata_t);
                            }

                            bool has_transition =
                                (meta->transition_type != WAYWAL_TRANSITION_NONE &&
                                 meta->transition_duration_ms > 0);
                            bool can_transition = !has_transition;
                            char matched_outputs[512] = {0};
                            size_t mo_len = 0;
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (output_matches_img_meta(out, meta)) {
                                    if (out->surface && out->configured && out->width > 0 &&
                                        out->height > 0) {
                                        can_transition = true;
                                    }
                                    if (out->name[0] != '\0') {
                                        int n = snprintf(matched_outputs + mo_len,
                                                         sizeof(matched_outputs) - mo_len, "%s%s",
                                                         mo_len ? "," : "", out->name);
                                        if (n > 0 && mo_len + (size_t)n < sizeof(matched_outputs)) {
                                            mo_len += (size_t)n;
                                        }
                                    }
                                }
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
                                state->use_bottom_layer =
                                    (strcasecmp(meta->layer_name, "bottom") == 0);
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

                            if (meta->has_auto_day_night) {
                                bool adn = (meta->auto_day_night != 0);
                                state->config.solar.auto_day_night = adn;
                                state->config.theme.auto_day_night = adn;
                                state->config.daemon.auto_day_night = adn;
                                state->config.auto_day_night = adn;
                            }
                            if (meta->has_solar_coords && isfinite(meta->solar_latitude) &&
                                isfinite(meta->solar_longitude)) {
                                state->config.solar.latitude = meta->solar_latitude;
                                state->config.solar.longitude = meta->solar_longitude;
                                state->config.solar.has_coordinates = true;
                            }

                            state->sched_theme_override_active = false;
                            if (!meta->is_solid_color || meta->extract_theme ||
                                meta->has_seed_color || meta->client_exported_theme ||
                                (state->config.theme.enabled && !meta->no_theme)) {
                                daemon_update_palette_from_bgra(state, pixels, meta->width,
                                                                meta->height, meta, false);
                            }

                            bool cache_synced = false;
                            if (can_transition && !meta->no_cache) {
                                const char *filt_str = waywal_filter_mode_to_swww_str(
                                    (waywal_filter_mode_t)meta->filter_mode);
                                (void)waywal_ecosystem_cache_sync(
                                    mo_len > 0 ? matched_outputs : first_target, meta->filepath,
                                    filt_str, pixels, meta->width, meta->height,
                                    state->config.general.caelestia_sync, false);
                                cache_synced = true;
                            }

                            if (state->outputs) {
                                for (output_node_t *out = state->outputs; out != NULL;
                                     out = out->next) {
                                    if (output_matches_img_meta(out, meta)) {
                                        if (has_transition) {
                                            layer_shell_snapshot_live_output(state, out);
                                        }
                                        stop_engines_on_output(state, out, false, false, false,
                                                               false);
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

                            bool is_anim = (!meta->is_solid_color) &&
                                           try_start_native_animation(state, meta->filepath, meta,
                                                                      first_target);
                            bool direct_depth_parallax = false;
                            if (!meta->is_solid_color && !is_anim &&
                                meta->depth_map_path[0] != '\0' && !has_transition) {
                                for (output_node_t *out = state->outputs; out != NULL;
                                     out = out->next) {
                                    if (!output_matches_img_meta(out, meta))
                                        continue;
                                    out->scaling_mode = meta->scaling_mode;
                                    out->filter_mode = meta->filter_mode;
                                    out->ten_bit = meta->ten_bit;
                                    out->crop_gravity_x =
                                        meta->has_crop_gravity ? meta->crop_gravity_x : 0.5f;
                                    out->crop_gravity_y =
                                        meta->has_crop_gravity ? meta->crop_gravity_y : 0.5f;
                                    out->has_crop_gravity = (meta->has_crop_gravity != 0);
                                }
                                direct_depth_parallax = daemon_activate_depth_parallax(
                                    state, pixels, meta->width, meta->height, meta, first_target);
                            }

                            bool will_succeed = direct_depth_parallax ||
                                                (is_anim && !has_transition) || can_transition;
                            if (will_succeed) {
                                if (!meta->is_solid_color &&
                                    waywal_dynwall_is_manifest_path(meta->filepath)) {
                                    state->solar_owns_wallpaper = true;
                                    snprintf(state->active_dynwall_path,
                                             sizeof(state->active_dynwall_path), "%s",
                                             meta->filepath);
                                    snprintf(state->active_dynwall_output,
                                             sizeof(state->active_dynwall_output), "%s",
                                             first_target);
                                    waywal_dynwall_manifest_t man;
                                    waywal_dynwall_resolved_t res;
                                    time_t now_utc = time(NULL);
                                    struct tm lt;
                                    localtime_r(&now_utc, &lt);
                                    waywal_solar_state_t sstate;
                                    if (state->config.solar.has_coordinates) {
                                        waywal_solar_compute(state->config.solar.latitude,
                                                             state->config.solar.longitude, now_utc,
                                                             &lt, &sstate);
                                    } else {
                                        waywal_solar_synthesize_local(
                                            state->config.solar.sunrise_fallback_min,
                                            state->config.solar.sunset_fallback_min, now_utc, &lt,
                                            &sstate);
                                    }
                                    if (waywal_dynwall_load_file(meta->filepath, &man) &&
                                        waywal_dynwall_resolve(&man, now_utc, &lt, &sstate, &res) &&
                                        waywal_dynwall_effective_path(&res)[0] != '\0') {
                                        snprintf(state->last_dynwall_frame_path,
                                                 sizeof(state->last_dynwall_frame_path), "%s",
                                                 waywal_dynwall_effective_path(&res));
                                        if (res.has_theme_mode) {
                                            state->sched_theme_override_active = true;
                                        }
                                    }
                                } else {
                                    state->solar_owns_wallpaper = false;
                                    state->active_dynwall_path[0] = '\0';
                                    state->last_dynwall_frame_path[0] = '\0';
                                }

                                if (state->active_dynwall_path[0] != '\0' ||
                                    state->config.solar.auto_day_night ||
                                    state->config.theme.auto_day_night ||
                                    state->config.daemon.auto_day_night ||
                                    state->config.auto_day_night) {
                                    daemon_tick_solar_and_schedule(state, true);
                                }

                                if (!cache_synced && !meta->no_cache) {
                                    const char *filt_str = waywal_filter_mode_to_swww_str(
                                        (waywal_filter_mode_t)meta->filter_mode);
                                    (void)waywal_ecosystem_cache_sync(
                                        mo_len > 0 ? matched_outputs : first_target, meta->filepath,
                                        filt_str, pixels, meta->width, meta->height,
                                        state->config.general.caelestia_sync, false);
                                }

                                if (!meta->no_cache &&
                                    (meta->filepath[0] != '\0' || meta->is_solid_color)) {
                                    waywal_output_saved_state_t s_entry;
                                    memset(&s_entry, 0, sizeof(s_entry));
                                    if (meta->is_solid_color) {
                                        s_entry.kind = WAYWAL_CONTENT_SOLID_COLOR;
                                        s_entry.media_path[0] = '\0';
                                    } else {
                                        s_entry.kind = is_anim ? WAYWAL_CONTENT_ANIMATION
                                                               : WAYWAL_CONTENT_STATIC_IMAGE;
                                        snprintf(s_entry.media_path, sizeof(s_entry.media_path),
                                                 "%.*s", (int)(sizeof(meta->filepath) - 1),
                                                 meta->filepath);
                                    }
                                    s_entry.scaling = (waywal_scaling_mode_t)meta->scaling_mode;
                                    s_entry.filter = (waywal_filter_mode_t)meta->filter_mode;
                                    s_entry.ten_bit = meta->ten_bit;
                                    uint8_t col_a = meta->color[3] ? meta->color[3] : 255;
                                    s_entry.solid_color = ((uint32_t)meta->color[0] << 24) |
                                                          ((uint32_t)meta->color[1] << 16) |
                                                          ((uint32_t)meta->color[2] << 8) |
                                                          (uint32_t)col_a;
                                    s_entry.crop_gravity_x =
                                        meta->has_crop_gravity ? meta->crop_gravity_x : 0.5f;
                                    s_entry.crop_gravity_y =
                                        meta->has_crop_gravity ? meta->crop_gravity_y : 0.5f;
                                    s_entry.has_crop_gravity = meta->has_crop_gravity ? 1 : 0;

                                    if (meta->num_target_outputs > 0 &&
                                        !extra_has_wildcard(
                                            (const char *)meta + sizeof(waywal_img_metadata_t),
                                            meta->extra_data_len, meta->num_target_outputs)) {
                                        const char *ptr =
                                            (const char *)meta + sizeof(waywal_img_metadata_t);
                                        for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
                                            bool matched_out = false;
                                            for (output_node_t *out = state->outputs; out != NULL;
                                                 out = out->next) {
                                                if (output_matches_target_spec(out, ptr) &&
                                                    out->name[0] != '\0') {
                                                    memset(s_entry.output_name, 0,
                                                           sizeof(s_entry.output_name));
                                                    strncpy(s_entry.output_name, out->name,
                                                            sizeof(s_entry.output_name) - 1);
                                                    (void)waywal_state_store_update_output(
                                                        &g_saved_state, &s_entry);
                                                    matched_out = true;
                                                }
                                            }
                                            if (!matched_out) {
                                                memset(s_entry.output_name, 0,
                                                       sizeof(s_entry.output_name));
                                                strncpy(s_entry.output_name, ptr,
                                                        sizeof(s_entry.output_name) - 1);
                                                (void)waywal_state_store_update_output(
                                                    &g_saved_state, &s_entry);
                                            }
                                            ptr += strlen(ptr) + 1;
                                        }
                                    } else {
                                        strncpy(s_entry.output_name, "*",
                                                sizeof(s_entry.output_name) - 1);
                                        (void)waywal_state_store_update_output(&g_saved_state,
                                                                               &s_entry);
                                        for (output_node_t *out = state->outputs; out != NULL;
                                             out = out->next) {
                                            if (out->name[0] != '\0') {
                                                memset(s_entry.output_name, 0,
                                                       sizeof(s_entry.output_name));
                                                strncpy(s_entry.output_name, out->name,
                                                        sizeof(s_entry.output_name) - 1);
                                                (void)waywal_state_store_update_output(
                                                    &g_saved_state, &s_entry);
                                            }
                                        }
                                    }
                                    (void)waywal_state_store_save(&g_saved_state, NULL);
                                }
                            }

                            if (direct_depth_parallax || (is_anim && !has_transition)) {
                                ok = true;
                            } else {
                                ok = daemon_start_image_transition(state, pixels, meta->width,
                                                                   meta->height, meta);
                                if (ok && !meta->is_solid_color && !is_anim &&
                                    meta->depth_map_path[0] != '\0') {
                                    (void)daemon_activate_depth_parallax(state, pixels, meta->width,
                                                                         meta->height, meta,
                                                                         first_target);
                                }
                            }
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
        char extra_buf[4097];
        size_t extra_len = 0;
        bool has_extra_targets = false;
        uint32_t matched_count = 0;

        if (req_hdr.payload_size >= sizeof(waywal_video_payload_t)) {
            if (read_exact(client_fd, &vpay, sizeof(vpay))) {
                vpay.filepath[sizeof(vpay.filepath) - 1] = '\0';
                has_vpay = true;

                size_t raw_extra_len = req_hdr.payload_size - sizeof(waywal_video_payload_t);
                if (vpay.num_target_outputs > 0 && vpay.num_target_outputs <= 64 &&
                    raw_extra_len > 0 && raw_extra_len <= 4096) {
                    if (read_exact(client_fd, extra_buf, raw_extra_len)) {
                        extra_buf[raw_extra_len] = '\0';
                        extra_len = raw_extra_len + 1;
                        if (!extra_has_wildcard(extra_buf, extra_len, vpay.num_target_outputs)) {
                            has_extra_targets = true;
                            const char *ptr = extra_buf;
                            size_t remaining = extra_len;
                            output_node_t *last_matched = NULL;
                            for (uint32_t i = 0; i < vpay.num_target_outputs && remaining > 0;
                                 ++i) {
                                const char *end = (const char *)memchr(ptr, '\0', remaining);
                                if (!end)
                                    break;
                                for (output_node_t *out = state->outputs; out != NULL;
                                     out = out->next) {
                                    if (output_matches_target_spec(out, ptr)) {
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
        }

        if (state->outputs != NULL && has_extra_targets && matched_count == 0) {
            if (attached_fd >= 0) {
                close(attached_fd);
                attached_fd = -1;
            }
            ok = false;
        } else {
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
                        if (output_matches_target_spec(out, ptr)) {
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
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (output_matches_target_spec(out, ptr)) {
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
                            for (output_node_t *out = state->outputs; out != NULL;
                                 out = out->next) {
                                if (output_matches_target_spec(out, ptr)) {
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
                            if (output_matches_target_spec(out, ptr)) {
                                out->video_eng = ve;
                                out->video_targeted = true;
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
                        out->video_targeted = true;
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
                }
            }
        }

        if (ok) {
            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (out->video_eng != NULL &&
                    (!has_extra_targets ||
                     output_matches_extra(out, extra_buf, extra_len, vpay.num_target_outputs))) {
                    out->displaying_color = false;
                    if (has_vpay && vpay.filepath[0] != '\0') {
                        snprintf(out->active_media_path, sizeof(out->active_media_path), "%s",
                                 vpay.filepath);
                    } else {
                        snprintf(out->active_media_path, sizeof(out->active_media_path), "<stdin>");
                    }
                }
            }
            daemon_reconcile_power_state(state);
            if (has_vpay && !vpay.no_cache) {
                char matched_outputs[256] = {0};
                size_t mlen = 0;
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (out->video_eng != NULL &&
                        (!has_extra_targets || output_matches_extra(out, extra_buf, extra_len,
                                                                    vpay.num_target_outputs)) &&
                        out->name[0] != '\0') {
                        int n = snprintf(matched_outputs + mlen, sizeof(matched_outputs) - mlen,
                                         "%s%s", mlen > 0 ? "," : "", out->name);
                        if (n > 0 && mlen + (size_t)n < sizeof(matched_outputs)) {
                            mlen += (size_t)n;
                        }
                    }
                }
                const char *fallback_target = has_extra_targets ? extra_buf : "*";
                (void)waywal_ecosystem_cache_sync(
                    matched_outputs[0] ? matched_outputs : fallback_target, vpay.filepath,
                    "Lanczos3", NULL, 0, 0, state->config.general.caelestia_sync,
                    vpay.no_cache != 0);

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
                        bool matched_out = false;
                        for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                            if (output_matches_target_spec(out, ptr) && out->name[0] != '\0') {
                                memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                                strncpy(s_entry.output_name, out->name,
                                        sizeof(s_entry.output_name) - 1);
                                (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                                matched_out = true;
                            }
                        }
                        if (!matched_out) {
                            memset(s_entry.output_name, 0, sizeof(s_entry.output_name));
                            strncpy(s_entry.output_name, ptr, sizeof(s_entry.output_name) - 1);
                            (void)waywal_state_store_update_output(&g_saved_state, &s_entry);
                        }
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
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (ok) {
            state->solar_owns_wallpaper = false;
            state->active_dynwall_path[0] = '\0';
            state->last_dynwall_frame_path[0] = '\0';
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
        memset(&spay, 0, sizeof(spay));
        bool ok = false;
        const size_t min_spay = offsetof(waywal_slideshow_payload_t, extract_theme);
        size_t read_spay =
            req_hdr.payload_size < sizeof(spay) ? req_hdr.payload_size : sizeof(spay);
        if (req_hdr.payload_size >= min_spay && req_hdr.payload_size <= sizeof(spay)) {
            if (read_exact(client_fd, &spay, read_spay)) {
                spay.path[sizeof(spay.path) - 1] = '\0';
                if (spay.auto_day_night != 0) {
                    bool adn = (spay.auto_day_night == 1u);
                    state->config.solar.auto_day_night = adn;
                    state->config.theme.auto_day_night = adn;
                    state->config.daemon.auto_day_night = adn;
                    state->config.auto_day_night = adn;
                }
                if (spay.has_solar_coords && isfinite(spay.solar_latitude) &&
                    isfinite(spay.solar_longitude)) {
                    state->config.solar.latitude = spay.solar_latitude;
                    state->config.solar.longitude = spay.solar_longitude;
                    state->config.solar.has_coordinates = true;
                }
                state->solar_owns_wallpaper = false;
                state->active_dynwall_path[0] = '\0';
                ok = daemon_start_slideshow_isolated(state, &spay);
                if (ok) {
                    daemon_reconcile_power_state(state);
                }
                if (ok && !spay.no_cache) {
                    waywal_output_saved_state_t s_entry;
                    memset(&s_entry, 0, sizeof(s_entry));
                    s_entry.kind = WAYWAL_CONTENT_SLIDESHOW;
                    strncpy(s_entry.media_path, spay.path, sizeof(s_entry.media_path) - 1);
                    s_entry.slideshow_interval_sec = spay.interval_s;
                    s_entry.scaling = (waywal_scaling_mode_t)spay.scaling_mode;
                    s_entry.filter = (waywal_filter_mode_t)spay.filter_mode;
                    s_entry.ten_bit = spay.ten_bit;
                    s_entry.crop_gravity_x = spay.has_crop_gravity ? spay.crop_gravity_x : 0.5f;
                    s_entry.crop_gravity_y = spay.has_crop_gravity ? spay.crop_gravity_y : 0.5f;
                    s_entry.has_crop_gravity = spay.has_crop_gravity ? 1 : 0;
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

    case WAYWAL_REQ_SLIDESHOW_CTRL:
    case WAYWAL_REQ_SLIDESHOW_NEXT:
    case WAYWAL_REQ_SLIDESHOW_PREV: {
        waywal_slideshow_ctrl_t sctrl;
        memset(&sctrl, 0, sizeof(sctrl));
        bool ok = false;
        bool valid_payload = false;

        if (req_hdr.opcode == WAYWAL_REQ_SLIDESHOW_NEXT) {
            sctrl.action = WAYWAL_SLIDESHOW_NEXT;
            valid_payload = true;
        } else if (req_hdr.opcode == WAYWAL_REQ_SLIDESHOW_PREV) {
            sctrl.action = WAYWAL_SLIDESHOW_PREV;
            valid_payload = true;
        }

        if (req_hdr.payload_size > 0) {
            if (req_hdr.opcode == WAYWAL_REQ_SLIDESHOW_CTRL &&
                req_hdr.payload_size < sizeof(uint32_t)) {
                valid_payload = false;
            } else {
                size_t to_read =
                    req_hdr.payload_size < sizeof(sctrl) ? req_hdr.payload_size : sizeof(sctrl);
                if (read_exact(client_fd, &sctrl, to_read)) {
                    sctrl.target_output[sizeof(sctrl.target_output) - 1] = '\0';
                    if (req_hdr.opcode == WAYWAL_REQ_SLIDESHOW_NEXT) {
                        sctrl.action = WAYWAL_SLIDESHOW_NEXT;
                    } else if (req_hdr.opcode == WAYWAL_REQ_SLIDESHOW_PREV) {
                        sctrl.action = WAYWAL_SLIDESHOW_PREV;
                    }
                    valid_payload = true;
                } else {
                    valid_payload = false;
                }
            }
        }

        if (valid_payload) {
            const char *target_spec = (sctrl.target_output[0] != '\0') ? sctrl.target_output : "*";
            bool is_wildcard = (strcmp(target_spec, "*") == 0);
            bool global_matches = is_wildcard;
            if (!global_matches && !state->outputs) {
                if (state->slideshow.num_target_outputs == 0) {
                    global_matches = true;
                } else {
                    for (uint32_t i = 0; i < state->slideshow.num_target_outputs && i < 16; ++i) {
                        if (strcmp(state->slideshow.target_outputs[i], "*") == 0 ||
                            strcmp(state->slideshow.target_outputs[i], target_spec) == 0) {
                            global_matches = true;
                            break;
                        }
                    }
                }
            }

            if (global_matches &&
                (state->slideshow.active || sctrl.action == WAYWAL_SLIDESHOW_STOP)) {
                ok = slideshow_control(&state->slideshow, sctrl.action);
                if (sctrl.action == WAYWAL_SLIDESHOW_STOP) {
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (out->slideshow_eng == &state->slideshow) {
                            out->slideshow_eng = NULL;
                        }
                    }
                }
            }

            slideshow_engine_t *seen[32];
            size_t num_seen = 0;
            if (sctrl.action == WAYWAL_SLIDESHOW_STOP) {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (!output_matches_target_spec(out, target_spec)) {
                        continue;
                    }
                    ok = true;
                    slideshow_engine_t *ss = out->slideshow_eng;
                    out->slideshow_eng = NULL;
                    if (!ss) {
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
                    }
                }
                for (size_t k = 0; k < num_seen; ++k) {
                    bool still_used = false;
                    for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                        if (out->slideshow_eng == seen[k]) {
                            still_used = true;
                            break;
                        }
                    }
                    if (still_used) {
                        uint32_t rem_count = 0;
                        memset(seen[k]->target_outputs, 0, sizeof(seen[k]->target_outputs));
                        for (output_node_t *o = state->outputs; o != NULL && rem_count < 16;
                             o = o->next) {
                            if (o->slideshow_eng == seen[k] && o->name[0] != '\0') {
                                strncpy(seen[k]->target_outputs[rem_count], o->name,
                                        sizeof(seen[k]->target_outputs[rem_count]) - 1);
                                rem_count++;
                            }
                        }
                        if (rem_count > 0) {
                            seen[k]->num_target_outputs = rem_count;
                        }
                    } else {
                        slideshow_stop(seen[k]);
                        if (seen[k] != &state->slideshow) {
                            if (seen[k]->timer_fd >= 0) {
                                unregister_fd_from_loops(state, seen[k]->timer_fd);
                            }
                            slideshow_destroy(seen[k]);
                            free(seen[k]);
                        }
                    }
                }
            } else {
                for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                    if (!output_matches_target_spec(out, target_spec)) {
                        continue;
                    }
                    slideshow_engine_t *ss = out->slideshow_eng;
                    if (!ss || (ss == &state->slideshow && is_wildcard)) {
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
            }
        }
        resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
        waywal_ipc_send(client_fd, &resp_hdr, -1);
        if (ok &&
            (sctrl.action == WAYWAL_SLIDESHOW_NEXT || sctrl.action == WAYWAL_SLIDESHOW_PREV)) {
            daemon_broadcast_event(state, "wallpaper_changed");
        }
        break;
    }

    case WAYWAL_REQ_SHADER: {
        waywal_shader_payload_t spay;
        bool ok = false;
        if (req_hdr.payload_size >= sizeof(spay) && read_exact(client_fd, &spay, sizeof(spay))) {
            spay.output[sizeof(spay.output) - 1] = '\0';
            spay.shader_path[sizeof(spay.shader_path) - 1] = '\0';

            ok = daemon_activate_shader(state, &spay);
            if (ok && !spay.no_cache) {
                waywal_output_saved_state_t s_entry;
                memset(&s_entry, 0, sizeof(s_entry));
                s_entry.kind = WAYWAL_CONTENT_SHADER;
                const char *saved_sh_path =
                    state->shader_path[0] != '\0' ? state->shader_path : spay.shader_path;
                strncpy(s_entry.media_path, saved_sh_path, sizeof(s_entry.media_path) - 1);
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
            state->solar_owns_wallpaper = false;
            state->active_dynwall_path[0] = '\0';
            state->last_dynwall_frame_path[0] = '\0';
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
        memset(&wpay, 0, sizeof(wpay));
        bool ok = false;
        const size_t min_wpay = offsetof(waywal_workshop_payload_t, extract_theme);
        size_t read_wpay =
            req_hdr.payload_size < sizeof(wpay) ? req_hdr.payload_size : sizeof(wpay);
        if (req_hdr.payload_size >= min_wpay && req_hdr.payload_size <= sizeof(wpay) &&
            read_exact(client_fd, &wpay, read_wpay)) {
            wpay.output[sizeof(wpay.output) - 1] = '\0';
            wpay.workshop_dir[sizeof(wpay.workshop_dir) - 1] = '\0';
            wpay.prop_overrides[sizeof(wpay.prop_overrides) - 1] = '\0';

            ok = daemon_activate_workshop(state, &wpay);
            if (ok && !wpay.no_cache) {
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
            state->solar_owns_wallpaper = false;
            state->active_dynwall_path[0] = '\0';
            state->last_dynwall_frame_path[0] = '\0';
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
