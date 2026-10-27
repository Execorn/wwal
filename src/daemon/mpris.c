// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/mpris.h"

#include "waywal/image_loader.h"
#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;

typedef struct {
    const char *name;
    const char *message;
    int _need_free;
} sd_bus_error;

typedef int (*sd_bus_message_handler_t)(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

typedef struct {
    void *dl_handle;
    int (*open_user)(sd_bus **ret);
    sd_bus *(*unref)(sd_bus *bus);
    sd_bus *(*flush_close_unref)(sd_bus *bus);
    sd_bus_slot *(*slot_unref)(sd_bus_slot *slot);
    int (*get_fd)(sd_bus *bus);
    int (*get_events)(sd_bus *bus);
    int (*get_timeout)(sd_bus *bus, uint64_t *timeout_usec);
    int (*process)(sd_bus *bus, sd_bus_message **ret);
    int (*flush)(sd_bus *bus);
    int (*add_match)(sd_bus *bus, sd_bus_slot **slot, const char *match,
                     sd_bus_message_handler_t callback, void *userdata);
    int (*call_method_async)(sd_bus *bus, sd_bus_slot **slot, const char *destination,
                             const char *path, const char *interface, const char *member,
                             sd_bus_message_handler_t callback, void *userdata, const char *types,
                             ...);
    int (*message_read)(sd_bus_message *m, const char *types, ...);
    int (*message_read_basic)(sd_bus_message *m, char type, void *p);
    int (*message_enter_container)(sd_bus_message *m, char type, const char *contents);
    int (*message_exit_container)(sd_bus_message *m);
    int (*message_peek_type)(sd_bus_message *m, char *type, const char **contents);
    int (*message_skip)(sd_bus_message *m, const char *types);
    const char *(*message_get_sender)(sd_bus_message *m);
} sdbus_api_t;

#define MPRIS_MAX_PLAYERS 16

typedef struct {
    struct waywal_mpris *parent;
    bool active;
    bool just_started_playing;
    char bus_name[128];
    char owner[128];
    sd_bus_slot *pending_slot;
    waywal_mpris_status_t status;
    char playback_status[32];
    char title[256];
    char artist[256];
    char album[256];
    char art_url[1024];
    int64_t length_us;
} mpris_player_t;

struct waywal_mpris {
    sdbus_api_t api;
    sd_bus *bus;
    sd_bus_slot *props_match_slot;
    sd_bus_slot *owner_match_slot;
    sd_bus_slot *list_names_slot;
    mpris_player_t players[MPRIS_MAX_PLAYERS];
    waywal_mpris_change_cb_t on_change;
    void *userdata;
    char active_sender[128];
    char active_bus_name[128];
    waywal_mpris_metadata_t meta;
    bool art_changed;
    bool metadata_changed;
    bool test_mode;
};

static void notify_mpris_change(waywal_mpris_t *mpris)
{
    if (mpris && mpris->on_change) {
        mpris->on_change(&mpris->meta, mpris->userdata);
    }
}

static bool resolve_sdbus_sym(void *handle, const char *name, void *out_fn)
{
    void *sym = dlsym(handle, name);
    if (!sym)
        return false;
    memcpy(out_fn, &sym, sizeof(void *));
    return true;
}

static bool init_sdbus_api(sdbus_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->dl_handle = dlopen("libsystemd.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!api->dl_handle) {
        api->dl_handle = dlopen("libelogind.so.0", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!api->dl_handle)
        return false;

    (void)resolve_sdbus_sym(api->dl_handle, "sd_bus_flush_close_unref", &api->flush_close_unref);
    (void)resolve_sdbus_sym(api->dl_handle, "sd_bus_slot_unref", &api->slot_unref);

    if (!resolve_sdbus_sym(api->dl_handle, "sd_bus_open_user", &api->open_user) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_unref", &api->unref) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_get_fd", &api->get_fd) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_get_events", &api->get_events) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_get_timeout", &api->get_timeout) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_process", &api->process) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_flush", &api->flush) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_add_match", &api->add_match) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_call_method_async", &api->call_method_async) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_read", &api->message_read) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_read_basic", &api->message_read_basic) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_enter_container",
                           &api->message_enter_container) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_exit_container",
                           &api->message_exit_container) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_peek_type", &api->message_peek_type) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_skip", &api->message_skip) ||
        !resolve_sdbus_sym(api->dl_handle, "sd_bus_message_get_sender", &api->message_get_sender)) {
        dlclose(api->dl_handle);
        memset(api, 0, sizeof(*api));
        return false;
    }
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool path_has_dotdot_segment(const char *path)
{
    if (!path || path[0] != '/')
        return true;
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *seg_start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.')
            return true;
    }
    return false;
}

bool waywal_mpris_decode_file_uri(const char *uri, char *out_path, size_t out_cap)
{
    if (!uri || !out_path || out_cap < 2)
        return false;
    out_path[0] = '\0';

    const char *raw_path = NULL;
    if (strncmp(uri, "file://localhost/", 17) == 0) {
        raw_path = uri + 16;
    } else if (strncmp(uri, "file:///", 8) == 0) {
        raw_path = uri + 7;
    } else {
        return false;
    }

    size_t out_idx = 0;
    for (size_t i = 0; raw_path[i] != '\0'; ++i) {
        if (out_idx + 1 >= out_cap) {
            out_path[0] = '\0';
            return false;
        }
        unsigned char ch = (unsigned char)raw_path[i];
        if (ch == '%') {
            int hi = hex_nibble(raw_path[i + 1]);
            int lo = (hi >= 0) ? hex_nibble(raw_path[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                out_path[0] = '\0';
                return false;
            }
            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            if (decoded == 0) {
                out_path[0] = '\0';
                return false;
            }
            out_path[out_idx++] = (char)decoded;
            i += 2;
        } else {
            out_path[out_idx++] = (char)ch;
        }
    }
    out_path[out_idx] = '\0';

    if (out_idx == 0 || out_path[0] != '/' || path_has_dotdot_segment(out_path)) {
        out_path[0] = '\0';
        return false;
    }
    return true;
}

static void free_album_art_buffers(waywal_mpris_t *mpris)
{
    if (!mpris)
        return;
    free(mpris->meta.art_rgba);
    free(mpris->meta.art_bgra);
    mpris->meta.art_rgba = NULL;
    mpris->meta.art_bgra = NULL;
    mpris->meta.art_w = 0;
    mpris->meta.art_h = 0;
    mpris->meta.has_art_pixels = false;
}

static void update_album_art_from_url(waywal_mpris_t *mpris, const char *new_art_url)
{
    if (!mpris)
        return;

    const char *safe_url = new_art_url ? new_art_url : "";
    if (strcmp(mpris->meta.art_url, safe_url) == 0 &&
        (safe_url[0] == '\0' || (mpris->meta.has_art_pixels && mpris->meta.art_bgra != NULL))) {
        return;
    }

    strncpy(mpris->meta.art_url, safe_url, sizeof(mpris->meta.art_url) - 1);
    mpris->meta.art_url[sizeof(mpris->meta.art_url) - 1] = '\0';
    mpris->meta.art_local_path[0] = '\0';
    free_album_art_buffers(mpris);
    mpris->art_changed = true;

    if (safe_url[0] == '\0')
        return;

    char local_path[sizeof(mpris->meta.art_local_path)];
    if (!waywal_mpris_decode_file_uri(safe_url, local_path, sizeof(local_path))) {
        return;
    }

    int fd = open(local_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4 ||
        st.st_size > 64L * 1024L * 1024L) {
        close(fd);
        return;
    }

    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) {
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }

    raw_image_t raw;
    memset(&raw, 0, sizeof(raw));
    bool ok = image_load_fd(fd, &raw);
    close(fd);

    if (!ok || !raw.data || raw.width == 0 || raw.height == 0)
        return;

    strncpy(mpris->meta.art_local_path, local_path, sizeof(mpris->meta.art_local_path) - 1);
    mpris->meta.art_local_path[sizeof(mpris->meta.art_local_path) - 1] = '\0';
    mpris->meta.art_bgra = raw.data;
    mpris->meta.art_rgba = NULL;
    mpris->meta.art_w = raw.width;
    mpris->meta.art_h = raw.height;
    mpris->meta.has_art_pixels = true;
}

static bool parse_mpris_metadata_dict(waywal_mpris_t *mpris, sd_bus_message *m, char new_title[256],
                                      char new_artist[256], char new_album[256],
                                      char new_art_url[1024], int64_t *out_length)
{
    sdbus_api_t *api = &mpris->api;
    char vtype = 0;
    const char *vcontents = NULL;
    if (api->message_peek_type(m, &vtype, &vcontents) < 0 || vtype != 'v' || !vcontents ||
        strcmp(vcontents, "a{sv}") != 0) {
        api->message_skip(m, "v");
        return false;
    }

    if (api->message_enter_container(m, 'v', "a{sv}") < 0) {
        api->message_skip(m, "v");
        return false;
    }
    if (api->message_enter_container(m, 'a', "{sv}") < 0) {
        api->message_skip(m, "a{sv}");
        api->message_exit_container(m);
        return false;
    }

    new_title[0] = '\0';
    new_artist[0] = '\0';
    new_album[0] = '\0';
    new_art_url[0] = '\0';
    *out_length = 0;

    while (api->message_enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL;
        if (api->message_read_basic(m, 's', &key) < 0 || !key) {
            api->message_exit_container(m);
            break;
        }

        char mt = 0;
        const char *mc = NULL;
        if (api->message_peek_type(m, &mt, &mc) < 0 || mt != 'v' || !mc) {
            api->message_skip(m, "v");
            api->message_exit_container(m);
            continue;
        }

        if (strcmp(key, "xesam:title") == 0 && strcmp(mc, "s") == 0) {
            if (api->message_enter_container(m, 'v', "s") >= 0) {
                const char *val = NULL;
                if (api->message_read_basic(m, 's', &val) >= 0 && val) {
                    strncpy(new_title, val, 255);
                    new_title[255] = '\0';
                }
                api->message_exit_container(m);
            } else {
                api->message_skip(m, "v");
            }
        } else if (strcmp(key, "xesam:album") == 0 && strcmp(mc, "s") == 0) {
            if (api->message_enter_container(m, 'v', "s") >= 0) {
                const char *val = NULL;
                if (api->message_read_basic(m, 's', &val) >= 0 && val) {
                    strncpy(new_album, val, 255);
                    new_album[255] = '\0';
                }
                api->message_exit_container(m);
            } else {
                api->message_skip(m, "v");
            }
        } else if (strcmp(key, "xesam:artist") == 0) {
            if (strcmp(mc, "as") == 0) {
                if (api->message_enter_container(m, 'v', "as") >= 0) {
                    if (api->message_enter_container(m, 'a', "s") >= 0) {
                        size_t off = 0;
                        const char *astr = NULL;
                        while (api->message_read_basic(m, 's', &astr) > 0 && astr) {
                            if (astr[0] == '\0')
                                continue;
                            if (off > 0 && off + 2 < 256) {
                                new_artist[off++] = ',';
                                new_artist[off++] = ' ';
                            }
                            size_t alen = strlen(astr);
                            if (off + alen >= 256)
                                alen = 255 - off;
                            if (alen > 0) {
                                memcpy(new_artist + off, astr, alen);
                                off += alen;
                                new_artist[off] = '\0';
                            }
                        }
                        api->message_exit_container(m);
                    } else {
                        api->message_skip(m, "as");
                    }
                    api->message_exit_container(m);
                } else {
                    api->message_skip(m, "v");
                }
            } else if (strcmp(mc, "s") == 0) {
                if (api->message_enter_container(m, 'v', "s") >= 0) {
                    const char *val = NULL;
                    if (api->message_read_basic(m, 's', &val) >= 0 && val) {
                        strncpy(new_artist, val, 255);
                        new_artist[255] = '\0';
                    }
                    api->message_exit_container(m);
                } else {
                    api->message_skip(m, "v");
                }
            } else {
                api->message_skip(m, "v");
            }
        } else if (strcmp(key, "mpris:artUrl") == 0 && strcmp(mc, "s") == 0) {
            if (api->message_enter_container(m, 'v', "s") >= 0) {
                const char *val = NULL;
                if (api->message_read_basic(m, 's', &val) >= 0 && val) {
                    strncpy(new_art_url, val, 1023);
                    new_art_url[1023] = '\0';
                }
                api->message_exit_container(m);
            } else {
                api->message_skip(m, "v");
            }
        } else if (strcmp(key, "mpris:length") == 0 &&
                   (strcmp(mc, "x") == 0 || strcmp(mc, "t") == 0 || strcmp(mc, "i") == 0 ||
                    strcmp(mc, "u") == 0 || strcmp(mc, "d") == 0)) {
            if (api->message_enter_container(m, 'v', mc) >= 0) {
                if (mc[0] == 'x') {
                    int64_t lval = 0;
                    if (api->message_read_basic(m, 'x', &lval) >= 0)
                        *out_length = lval;
                } else if (mc[0] == 't') {
                    uint64_t uval = 0;
                    if (api->message_read_basic(m, 't', &uval) >= 0)
                        *out_length = (int64_t)uval;
                } else if (mc[0] == 'i') {
                    int32_t ival = 0;
                    if (api->message_read_basic(m, 'i', &ival) >= 0)
                        *out_length = (int64_t)ival;
                } else if (mc[0] == 'u') {
                    uint32_t u32val = 0;
                    if (api->message_read_basic(m, 'u', &u32val) >= 0)
                        *out_length = (int64_t)u32val;
                } else if (mc[0] == 'd') {
                    double dval = 0.0;
                    if (api->message_read_basic(m, 'd', &dval) >= 0)
                        *out_length = (int64_t)dval;
                }
                api->message_exit_container(m);
            } else {
                api->message_skip(m, "v");
            }
        } else {
            api->message_skip(m, "v");
        }

        api->message_exit_container(m);
    }

    api->message_exit_container(m);
    api->message_exit_container(m);
    return true;
}

static mpris_player_t *find_player_by_name(waywal_mpris_t *mpris, const char *bus_name)
{
    if (!mpris || !bus_name || bus_name[0] == '\0')
        return NULL;
    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        if (mpris->players[i].active && strcmp(mpris->players[i].bus_name, bus_name) == 0) {
            return &mpris->players[i];
        }
    }
    return NULL;
}

static mpris_player_t *find_player_by_owner(waywal_mpris_t *mpris, const char *owner)
{
    if (!mpris || !owner || owner[0] == '\0')
        return NULL;
    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        if (mpris->players[i].active && strcmp(mpris->players[i].owner, owner) == 0) {
            return &mpris->players[i];
        }
    }
    return NULL;
}

static void clear_player_slot(waywal_mpris_t *mpris, mpris_player_t *slot)
{
    if (!mpris || !slot)
        return;
    if (slot->pending_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(slot->pending_slot);
    }
    memset(slot, 0, sizeof(*slot));
    slot->parent = mpris;
    slot->status = WAYWAL_MPRIS_STOPPED;
    strncpy(slot->playback_status, "Stopped", sizeof(slot->playback_status) - 1);
}

static void merge_duplicate_player_slots(waywal_mpris_t *mpris, mpris_player_t *primary)
{
    if (!mpris || !primary || !primary->active)
        return;
    if (primary->bus_name[0] == '\0' || primary->owner[0] == '\0')
        return;

    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        mpris_player_t *other = &mpris->players[i];
        if (other == primary || !other->active)
            continue;
        bool same_bus =
            (other->bus_name[0] != '\0' && strcmp(other->bus_name, primary->bus_name) == 0);
        bool same_unnamed_owner = (other->bus_name[0] == '\0' && other->owner[0] != '\0' &&
                                   strcmp(other->owner, primary->owner) == 0);
        if (same_bus || same_unnamed_owner) {
            if (primary->status == WAYWAL_MPRIS_STOPPED && other->status != WAYWAL_MPRIS_STOPPED) {
                primary->status = other->status;
                primary->just_started_playing =
                    primary->just_started_playing || other->just_started_playing;
                strncpy(primary->playback_status, other->playback_status,
                        sizeof(primary->playback_status) - 1);
                primary->playback_status[sizeof(primary->playback_status) - 1] = '\0';
            }
            if (primary->title[0] == '\0' && other->title[0] != '\0') {
                strncpy(primary->title, other->title, sizeof(primary->title) - 1);
                primary->title[sizeof(primary->title) - 1] = '\0';
                strncpy(primary->artist, other->artist, sizeof(primary->artist) - 1);
                primary->artist[sizeof(primary->artist) - 1] = '\0';
                strncpy(primary->album, other->album, sizeof(primary->album) - 1);
                primary->album[sizeof(primary->album) - 1] = '\0';
                strncpy(primary->art_url, other->art_url, sizeof(primary->art_url) - 1);
                primary->art_url[sizeof(primary->art_url) - 1] = '\0';
                primary->length_us = other->length_us;
            }
            clear_player_slot(mpris, other);
        }
    }
}

static mpris_player_t *ensure_player_slot(waywal_mpris_t *mpris, const char *bus_name,
                                          const char *owner)
{
    if (!mpris)
        return NULL;
    mpris_player_t *slot = NULL;
    if (bus_name && bus_name[0] != '\0')
        slot = find_player_by_name(mpris, bus_name);
    if (!slot && owner && owner[0] != '\0')
        slot = find_player_by_owner(mpris, owner);
    if (!slot) {
        for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
            if (!mpris->players[i].active) {
                slot = &mpris->players[i];
                break;
            }
        }
    }
    if (!slot) {
        for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
            if (mpris->players[i].status != WAYWAL_MPRIS_PLAYING) {
                slot = &mpris->players[i];
                break;
            }
        }
        if (!slot)
            slot = &mpris->players[0];
        clear_player_slot(mpris, slot);
    }
    if (!slot->active) {
        clear_player_slot(mpris, slot);
        slot->active = true;
    }
    if (bus_name && bus_name[0] != '\0') {
        strncpy(slot->bus_name, bus_name, sizeof(slot->bus_name) - 1);
        slot->bus_name[sizeof(slot->bus_name) - 1] = '\0';
    }
    if (owner && owner[0] != '\0') {
        strncpy(slot->owner, owner, sizeof(slot->owner) - 1);
        slot->owner[sizeof(slot->owner) - 1] = '\0';
    }
    merge_duplicate_player_slots(mpris, slot);
    return slot;
}

static void reconcile_active_player(waywal_mpris_t *mpris, mpris_player_t *preferred)
{
    if (!mpris)
        return;

    mpris_player_t *current = NULL;
    if (mpris->active_bus_name[0] != '\0')
        current = find_player_by_name(mpris, mpris->active_bus_name);
    if (!current && mpris->active_sender[0] != '\0')
        current = find_player_by_owner(mpris, mpris->active_sender);

    mpris_player_t *win = NULL;
    if (preferred && preferred->active && preferred->status == WAYWAL_MPRIS_PLAYING &&
        (preferred->just_started_playing || !current || !current->active ||
         current->status != WAYWAL_MPRIS_PLAYING)) {
        win = preferred;
    } else if (current && current->active && current->status == WAYWAL_MPRIS_PLAYING) {
        win = current;
    } else {
        for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
            if (mpris->players[i].active && mpris->players[i].status == WAYWAL_MPRIS_PLAYING) {
                win = &mpris->players[i];
                break;
            }
        }
    }

    if (!win) {
        if (current && current->active && current->status == WAYWAL_MPRIS_PAUSED) {
            win = current;
        } else if (preferred && preferred->active && preferred->status == WAYWAL_MPRIS_PAUSED) {
            win = preferred;
        } else {
            for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
                if (mpris->players[i].active && mpris->players[i].status == WAYWAL_MPRIS_PAUSED) {
                    win = &mpris->players[i];
                    break;
                }
            }
        }
    }

    if (!win) {
        if (current && current->active) {
            win = current;
        } else if (preferred && preferred->active) {
            win = preferred;
        } else {
            for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
                if (mpris->players[i].active) {
                    win = &mpris->players[i];
                    break;
                }
            }
        }
    }

    if (preferred) {
        preferred->just_started_playing = false;
    }

    if (!win || win->status == WAYWAL_MPRIS_STOPPED) {
        const char *p_name = win ? ((win->bus_name[0] != '\0') ? win->bus_name : win->owner) : "";
        if (win) {
            strncpy(mpris->active_bus_name, win->bus_name, sizeof(mpris->active_bus_name) - 1);
            mpris->active_bus_name[sizeof(mpris->active_bus_name) - 1] = '\0';
            strncpy(mpris->active_sender, win->owner, sizeof(mpris->active_sender) - 1);
            mpris->active_sender[sizeof(mpris->active_sender) - 1] = '\0';
        } else {
            mpris->active_sender[0] = '\0';
            mpris->active_bus_name[0] = '\0';
        }

        bool meta_diff = (strcmp(mpris->meta.player_name, p_name) != 0 || mpris->meta.playing ||
                          mpris->meta.status != WAYWAL_MPRIS_STOPPED ||
                          strcmp(mpris->meta.playback_status, "Stopped") != 0 ||
                          mpris->meta.title[0] != '\0' || mpris->meta.artist[0] != '\0' ||
                          mpris->meta.album[0] != '\0' || mpris->meta.length_us != 0);

        strncpy(mpris->meta.player_name, p_name, sizeof(mpris->meta.player_name) - 1);
        mpris->meta.player_name[sizeof(mpris->meta.player_name) - 1] = '\0';
        mpris->meta.playing = false;
        mpris->meta.status = WAYWAL_MPRIS_STOPPED;
        strncpy(mpris->meta.playback_status, "Stopped", sizeof(mpris->meta.playback_status) - 1);
        mpris->meta.playback_status[sizeof(mpris->meta.playback_status) - 1] = '\0';
        mpris->meta.title[0] = '\0';
        mpris->meta.artist[0] = '\0';
        mpris->meta.album[0] = '\0';
        mpris->meta.length_us = 0;
        if (meta_diff) {
            mpris->metadata_changed = true;
        }
        update_album_art_from_url(mpris, "");
        if (mpris->art_changed || mpris->metadata_changed) {
            notify_mpris_change(mpris);
        }
        return;
    }

    const char *p_name = (win->bus_name[0] != '\0') ? win->bus_name : win->owner;
    strncpy(mpris->active_bus_name, win->bus_name, sizeof(mpris->active_bus_name) - 1);
    mpris->active_bus_name[sizeof(mpris->active_bus_name) - 1] = '\0';
    strncpy(mpris->active_sender, win->owner, sizeof(mpris->active_sender) - 1);
    mpris->active_sender[sizeof(mpris->active_sender) - 1] = '\0';

    bool now_playing = (win->status == WAYWAL_MPRIS_PLAYING);
    if (strcmp(mpris->meta.player_name, p_name) != 0 || mpris->meta.playing != now_playing ||
        mpris->meta.status != win->status ||
        strcmp(mpris->meta.playback_status, win->playback_status) != 0 ||
        strcmp(mpris->meta.title, win->title) != 0 ||
        strcmp(mpris->meta.artist, win->artist) != 0 ||
        strcmp(mpris->meta.album, win->album) != 0 || mpris->meta.length_us != win->length_us) {
        strncpy(mpris->meta.player_name, p_name, sizeof(mpris->meta.player_name) - 1);
        mpris->meta.player_name[sizeof(mpris->meta.player_name) - 1] = '\0';
        mpris->meta.playing = now_playing;
        mpris->meta.status = win->status;
        strncpy(mpris->meta.playback_status, win->playback_status,
                sizeof(mpris->meta.playback_status) - 1);
        mpris->meta.playback_status[sizeof(mpris->meta.playback_status) - 1] = '\0';
        strncpy(mpris->meta.title, win->title, sizeof(mpris->meta.title) - 1);
        mpris->meta.title[sizeof(mpris->meta.title) - 1] = '\0';
        strncpy(mpris->meta.artist, win->artist, sizeof(mpris->meta.artist) - 1);
        mpris->meta.artist[sizeof(mpris->meta.artist) - 1] = '\0';
        strncpy(mpris->meta.album, win->album, sizeof(mpris->meta.album) - 1);
        mpris->meta.album[sizeof(mpris->meta.album) - 1] = '\0';
        mpris->meta.length_us = win->length_us;
        mpris->metadata_changed = true;
    }

    update_album_art_from_url(mpris, win->art_url);

    if (mpris->art_changed || mpris->metadata_changed) {
        notify_mpris_change(mpris);
    }
}

static bool parse_player_properties_dict_into_slot(waywal_mpris_t *mpris, mpris_player_t *slot,
                                                   sd_bus_message *m)
{
    sdbus_api_t *api = &mpris->api;
    if (api->message_enter_container(m, 'a', "{sv}") < 0)
        return false;

    bool relevant = false;
    slot->just_started_playing = false;

    while (api->message_enter_container(m, 'e', "sv") > 0) {
        const char *prop = NULL;
        if (api->message_read_basic(m, 's', &prop) < 0 || !prop) {
            api->message_exit_container(m);
            break;
        }

        if (strcmp(prop, "PlaybackStatus") == 0) {
            char vt = 0;
            const char *vc = NULL;
            if (api->message_peek_type(m, &vt, &vc) >= 0 && vt == 'v' && vc &&
                strcmp(vc, "s") == 0) {
                if (api->message_enter_container(m, 'v', "s") >= 0) {
                    const char *status = NULL;
                    if (api->message_read_basic(m, 's', &status) >= 0 && status) {
                        relevant = true;
                        waywal_mpris_status_t prev_status = slot->status;
                        slot->status = WAYWAL_MPRIS_STOPPED;
                        if (strcmp(status, "Playing") == 0) {
                            slot->status = WAYWAL_MPRIS_PLAYING;
                        } else if (strcmp(status, "Paused") == 0) {
                            slot->status = WAYWAL_MPRIS_PAUSED;
                        }
                        slot->just_started_playing = (slot->status == WAYWAL_MPRIS_PLAYING &&
                                                      prev_status != WAYWAL_MPRIS_PLAYING);
                        strncpy(slot->playback_status, status, sizeof(slot->playback_status) - 1);
                        slot->playback_status[sizeof(slot->playback_status) - 1] = '\0';
                        if (slot->status == WAYWAL_MPRIS_STOPPED &&
                            prev_status != WAYWAL_MPRIS_STOPPED) {
                            slot->title[0] = '\0';
                            slot->artist[0] = '\0';
                            slot->album[0] = '\0';
                            slot->art_url[0] = '\0';
                            slot->length_us = 0;
                        }
                    }
                    api->message_exit_container(m);
                } else {
                    api->message_skip(m, "v");
                }
            } else {
                api->message_skip(m, "v");
            }
        } else if (strcmp(prop, "Metadata") == 0) {
            char new_title[256] = {0};
            char new_artist[256] = {0};
            char new_album[256] = {0};
            char new_art_url[1024] = {0};
            int64_t new_length = 0;
            if (parse_mpris_metadata_dict(mpris, m, new_title, new_artist, new_album, new_art_url,
                                          &new_length)) {
                relevant = true;
                strncpy(slot->title, new_title, sizeof(slot->title) - 1);
                slot->title[sizeof(slot->title) - 1] = '\0';
                strncpy(slot->artist, new_artist, sizeof(slot->artist) - 1);
                slot->artist[sizeof(slot->artist) - 1] = '\0';
                strncpy(slot->album, new_album, sizeof(slot->album) - 1);
                slot->album[sizeof(slot->album) - 1] = '\0';
                strncpy(slot->art_url, new_art_url, sizeof(slot->art_url) - 1);
                slot->art_url[sizeof(slot->art_url) - 1] = '\0';
                slot->length_us = new_length;
            }
        } else {
            api->message_skip(m, "v");
        }

        api->message_exit_container(m);
    }

    api->message_exit_container(m);
    return relevant;
}

static void query_player_properties(waywal_mpris_t *mpris, mpris_player_t *slot);

static int on_get_all_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    mpris_player_t *slot = (mpris_player_t *)userdata;
    if (!slot || !slot->parent || !slot->active || !m)
        return 0;
    waywal_mpris_t *mpris = slot->parent;
    if (slot->pending_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(slot->pending_slot);
        slot->pending_slot = NULL;
    }
    const char *sender = mpris->api.message_get_sender(m);
    if (sender && sender[0] != '\0') {
        strncpy(slot->owner, sender, sizeof(slot->owner) - 1);
        slot->owner[sizeof(slot->owner) - 1] = '\0';
    }
    (void)parse_player_properties_dict_into_slot(mpris, slot, m);
    merge_duplicate_player_slots(mpris, slot);
    reconcile_active_player(mpris, slot);
    return 0;
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    waywal_mpris_t *mpris = (waywal_mpris_t *)userdata;
    if (!mpris || !m)
        return 0;

    const char *iface = NULL;
    if (mpris->api.message_read_basic(m, 's', &iface) < 0 || !iface)
        return 0;
    if (strcmp(iface, "org.mpris.MediaPlayer2.Player") != 0)
        return 0;

    const char *sender = mpris->api.message_get_sender(m);
    mpris_player_t *slot = ensure_player_slot(mpris, NULL, sender);
    if (!slot)
        return 0;

    bool relevant_dict = parse_player_properties_dict_into_slot(mpris, slot, m);

    bool need_requery = false;
    if (mpris->api.message_enter_container(m, 'a', "s") >= 0) {
        const char *inv_prop = NULL;
        while (mpris->api.message_read_basic(m, 's', &inv_prop) > 0 && inv_prop) {
            if (strcmp(inv_prop, "Metadata") == 0 || strcmp(inv_prop, "PlaybackStatus") == 0) {
                need_requery = true;
            }
        }
        mpris->api.message_exit_container(m);
    }

    if (!relevant_dict && !need_requery) {
        return 0;
    }

    if (need_requery) {
        query_player_properties(mpris, slot);
    }

    reconcile_active_player(mpris, slot);
    return 0;
}

static void query_player_properties(waywal_mpris_t *mpris, mpris_player_t *slot)
{
    if (!mpris || !mpris->bus || !slot || !slot->active)
        return;
    const char *dest = (slot->bus_name[0] != '\0') ? slot->bus_name : slot->owner;
    if (!dest || dest[0] == '\0')
        return;
    if (slot->pending_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(slot->pending_slot);
        slot->pending_slot = NULL;
    }
    (void)mpris->api.call_method_async(mpris->bus, &slot->pending_slot, dest,
                                       "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                       "GetAll", on_get_all_reply, slot, "s",
                                       "org.mpris.MediaPlayer2.Player");
}

static int on_list_names_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    waywal_mpris_t *mpris = (waywal_mpris_t *)userdata;
    if (!mpris || !m)
        return 0;

    if (mpris->list_names_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(mpris->list_names_slot);
        mpris->list_names_slot = NULL;
    }

    if (mpris->api.message_enter_container(m, 'a', "s") < 0)
        return 0;

    const char *name = NULL;
    while (mpris->api.message_read_basic(m, 's', &name) > 0 && name) {
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
            mpris_player_t *slot = ensure_player_slot(mpris, name, NULL);
            if (slot) {
                query_player_properties(mpris, slot);
            }
        }
    }
    mpris->api.message_exit_container(m);
    return 0;
}

static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    waywal_mpris_t *mpris = (waywal_mpris_t *)userdata;
    if (!mpris || !m)
        return 0;

    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    if (mpris->api.message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || !name)
        return 0;

    if (strncmp(name, "org.mpris.MediaPlayer2.", 23) != 0)
        return 0;

    if (new_owner && new_owner[0] != '\0') {
        mpris_player_t *slot = ensure_player_slot(mpris, name, new_owner);
        if (slot) {
            query_player_properties(mpris, slot);
        }
    } else if (old_owner && old_owner[0] != '\0') {
        bool any_cleared = false;
        for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
            mpris_player_t *p = &mpris->players[i];
            if (!p->active)
                continue;
            bool match_name = (p->bus_name[0] != '\0' && strcmp(p->bus_name, name) == 0);
            bool match_unnamed_owner =
                (p->bus_name[0] == '\0' && p->owner[0] != '\0' && strcmp(p->owner, old_owner) == 0);
            if (match_name || match_unnamed_owner) {
                clear_player_slot(mpris, p);
                any_cleared = true;
            }
        }
        if (any_cleared) {
            reconcile_active_player(mpris, NULL);
        }
    }
    return 0;
}

waywal_mpris_t *waywal_mpris_create(waywal_mpris_change_cb_t on_change, void *userdata)
{
    waywal_mpris_t *mpris = (waywal_mpris_t *)calloc(1, sizeof(waywal_mpris_t));
    if (!mpris)
        return NULL;

    mpris->on_change = on_change;
    mpris->userdata = userdata;
    mpris->meta.status = WAYWAL_MPRIS_STOPPED;
    strncpy(mpris->meta.playback_status, "Stopped", sizeof(mpris->meta.playback_status) - 1);
    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        clear_player_slot(mpris, &mpris->players[i]);
    }

    if (!init_sdbus_api(&mpris->api)) {
        WAYWAL_LOG_DEBUG("MPRIS2: libsystemd.so.0 not available; running in offline/test mode");
        return mpris;
    }

    if (mpris->api.open_user(&mpris->bus) < 0 || !mpris->bus) {
        WAYWAL_LOG_DEBUG("MPRIS2: session D-Bus unavailable; running in offline/test mode");
        mpris->bus = NULL;
        return mpris;
    }

    (void)mpris->api.add_match(mpris->bus, &mpris->props_match_slot,
                               "type='signal',interface='org.freedesktop.DBus.Properties',"
                               "member='PropertiesChanged',arg0='org.mpris.MediaPlayer2.Player'",
                               on_properties_changed, mpris);

    (void)mpris->api.add_match(mpris->bus, &mpris->owner_match_slot,
                               "type='signal',sender='org.freedesktop.DBus',"
                               "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                               "arg0namespace='org.mpris.MediaPlayer2'",
                               on_name_owner_changed, mpris);

    (void)mpris->api.call_method_async(mpris->bus, &mpris->list_names_slot, "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
                                       on_list_names_reply, mpris, "");

    /* Non-blocking D-Bus handshake flush invariant: complete SASL auth & send match/ListNames
     * before returning the socket FD to epoll/io_uring POLLIN wait. */
    mpris->api.flush(mpris->bus);
    while (mpris->api.process(mpris->bus, NULL) > 0) {
    }
    mpris->api.flush(mpris->bus);

    return mpris;
}

void waywal_mpris_destroy(waywal_mpris_t *mpris)
{
    if (!mpris)
        return;
    free_album_art_buffers(mpris);
    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        clear_player_slot(mpris, &mpris->players[i]);
    }
    if (mpris->list_names_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(mpris->list_names_slot);
        mpris->list_names_slot = NULL;
    }
    if (mpris->props_match_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(mpris->props_match_slot);
        mpris->props_match_slot = NULL;
    }
    if (mpris->owner_match_slot && mpris->api.slot_unref) {
        mpris->api.slot_unref(mpris->owner_match_slot);
        mpris->owner_match_slot = NULL;
    }
    if (mpris->bus) {
        if (mpris->api.flush_close_unref) {
            mpris->api.flush_close_unref(mpris->bus);
        } else if (mpris->api.unref) {
            mpris->api.unref(mpris->bus);
        }
        mpris->bus = NULL;
    }
    if (mpris->api.dl_handle) {
        dlclose(mpris->api.dl_handle);
        mpris->api.dl_handle = NULL;
    }
    free(mpris);
}

int waywal_mpris_get_fd(const waywal_mpris_t *mpris)
{
    if (!mpris || !mpris->bus || !mpris->api.get_fd)
        return -1;
    return mpris->api.get_fd(mpris->bus);
}

bool waywal_mpris_dispatch(waywal_mpris_t *mpris)
{
    if (!mpris)
        return false;
    if (mpris->bus && mpris->api.process && mpris->api.flush) {
        while (mpris->api.process(mpris->bus, NULL) > 0) {
        }
        mpris->api.flush(mpris->bus);
    }
    return mpris->art_changed || mpris->metadata_changed;
}

const waywal_mpris_metadata_t *waywal_mpris_get_metadata(const waywal_mpris_t *mpris)
{
    if (!mpris)
        return NULL;
    return &mpris->meta;
}

const waywal_mpris_state_t *waywal_mpris_get_state(const waywal_mpris_t *mpris)
{
    if (!mpris)
        return NULL;
    return &mpris->meta;
}

bool waywal_mpris_is_playing(const waywal_mpris_t *mpris)
{
    return mpris ? mpris->meta.playing : false;
}

bool waywal_mpris_consume_art_changed(waywal_mpris_t *mpris)
{
    if (!mpris)
        return false;
    bool changed = mpris->art_changed;
    mpris->art_changed = false;
    return changed;
}

bool waywal_mpris_consume_metadata_changed(waywal_mpris_t *mpris)
{
    if (!mpris)
        return false;
    bool changed = mpris->metadata_changed;
    mpris->metadata_changed = false;
    return changed;
}

void waywal_mpris_inject_state_for_test(waywal_mpris_t *mpris, const waywal_mpris_state_t *state)
{
    if (!mpris || !state)
        return;

    mpris->test_mode = true;
    mpris->meta.playing = state->playing || (state->status == WAYWAL_MPRIS_PLAYING) ||
                          (strcmp(state->playback_status, "Playing") == 0);
    if (state->status != WAYWAL_MPRIS_STOPPED) {
        mpris->meta.status = state->status;
    } else if (mpris->meta.playing) {
        mpris->meta.status = WAYWAL_MPRIS_PLAYING;
    } else if (strcmp(state->playback_status, "Paused") == 0) {
        mpris->meta.status = WAYWAL_MPRIS_PAUSED;
    } else {
        mpris->meta.status = WAYWAL_MPRIS_STOPPED;
    }

    if (state->playback_status[0] != '\0') {
        strncpy(mpris->meta.playback_status, state->playback_status,
                sizeof(mpris->meta.playback_status) - 1);
    } else {
        strncpy(mpris->meta.playback_status, mpris->meta.playing ? "Playing" : "Stopped",
                sizeof(mpris->meta.playback_status) - 1);
    }
    mpris->meta.playback_status[sizeof(mpris->meta.playback_status) - 1] = '\0';

    strncpy(mpris->meta.player_name, state->player_name, sizeof(mpris->meta.player_name) - 1);
    mpris->meta.player_name[sizeof(mpris->meta.player_name) - 1] = '\0';

    strncpy(mpris->meta.title, state->title, sizeof(mpris->meta.title) - 1);
    mpris->meta.title[sizeof(mpris->meta.title) - 1] = '\0';

    strncpy(mpris->meta.artist, state->artist, sizeof(mpris->meta.artist) - 1);
    mpris->meta.artist[sizeof(mpris->meta.artist) - 1] = '\0';

    strncpy(mpris->meta.album, state->album, sizeof(mpris->meta.album) - 1);
    mpris->meta.album[sizeof(mpris->meta.album) - 1] = '\0';

    mpris->meta.length_us = state->length_us;
    mpris->metadata_changed = true;
    mpris->meta.art_url[0] = '\0';
    mpris->meta.art_local_path[0] = '\0';
    free_album_art_buffers(mpris);
    update_album_art_from_url(mpris, state->art_url);
    notify_mpris_change(mpris);
}

void waywal_mpris_update_player_for_test(waywal_mpris_t *mpris, const char *bus_name,
                                         const char *owner, const waywal_mpris_state_t *state)
{
    if (!mpris || !state)
        return;

    if (!mpris->test_mode) {
        mpris->test_mode = true;
        for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
            clear_player_slot(mpris, &mpris->players[i]);
        }
        mpris->active_bus_name[0] = '\0';
        mpris->active_sender[0] = '\0';
    }

    mpris_player_t *slot = ensure_player_slot(mpris, bus_name, owner);
    if (!slot)
        return;

    waywal_mpris_status_t prev_status = slot->status;
    waywal_mpris_status_t new_status = WAYWAL_MPRIS_STOPPED;
    if (state->status != WAYWAL_MPRIS_STOPPED) {
        new_status = state->status;
    } else if (state->playing || strcmp(state->playback_status, "Playing") == 0) {
        new_status = WAYWAL_MPRIS_PLAYING;
    } else if (strcmp(state->playback_status, "Paused") == 0) {
        new_status = WAYWAL_MPRIS_PAUSED;
    }

    slot->status = new_status;
    slot->just_started_playing =
        (new_status == WAYWAL_MPRIS_PLAYING && prev_status != WAYWAL_MPRIS_PLAYING);

    if (state->playback_status[0] != '\0') {
        strncpy(slot->playback_status, state->playback_status, sizeof(slot->playback_status) - 1);
    } else {
        const char *status_str = "Stopped";
        if (new_status == WAYWAL_MPRIS_PLAYING)
            status_str = "Playing";
        else if (new_status == WAYWAL_MPRIS_PAUSED)
            status_str = "Paused";
        strncpy(slot->playback_status, status_str, sizeof(slot->playback_status) - 1);
    }
    slot->playback_status[sizeof(slot->playback_status) - 1] = '\0';

    if (new_status == WAYWAL_MPRIS_STOPPED && prev_status != WAYWAL_MPRIS_STOPPED &&
        state->title[0] == '\0') {
        slot->title[0] = '\0';
        slot->artist[0] = '\0';
        slot->album[0] = '\0';
        slot->art_url[0] = '\0';
        slot->length_us = 0;
    } else if (state->title[0] != '\0' || state->artist[0] != '\0' || state->album[0] != '\0' ||
               state->art_url[0] != '\0' || state->length_us != 0) {
        strncpy(slot->title, state->title, sizeof(slot->title) - 1);
        slot->title[sizeof(slot->title) - 1] = '\0';
        strncpy(slot->artist, state->artist, sizeof(slot->artist) - 1);
        slot->artist[sizeof(slot->artist) - 1] = '\0';
        strncpy(slot->album, state->album, sizeof(slot->album) - 1);
        slot->album[sizeof(slot->album) - 1] = '\0';
        strncpy(slot->art_url, state->art_url, sizeof(slot->art_url) - 1);
        slot->art_url[sizeof(slot->art_url) - 1] = '\0';
        slot->length_us = state->length_us;
    }

    reconcile_active_player(mpris, slot);
}

void waywal_mpris_remove_player_for_test(waywal_mpris_t *mpris, const char *bus_name,
                                         const char *owner)
{
    if (!mpris)
        return;

    bool any_cleared = false;
    for (size_t i = 0; i < MPRIS_MAX_PLAYERS; ++i) {
        mpris_player_t *p = &mpris->players[i];
        if (!p->active)
            continue;
        bool match_name = (bus_name && bus_name[0] != '\0' && strcmp(p->bus_name, bus_name) == 0);
        bool match_owner = (owner && owner[0] != '\0' && strcmp(p->owner, owner) == 0 &&
                            (p->bus_name[0] == '\0' || !bus_name || bus_name[0] == '\0'));
        if (match_name || match_owner) {
            clear_player_slot(mpris, p);
            any_cleared = true;
        }
    }
    if (any_cleared) {
        reconcile_active_player(mpris, NULL);
    }
}

size_t waywal_json_escape_string(const char *src, char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0)
        return 0;
    dst[0] = '\0';
    if (!src)
        return 0;

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        char hex_esc[7];
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        default:
            if (c < 0x20) {
                snprintf(hex_esc, sizeof(hex_esc), "\\u%04x", (unsigned int)c);
                esc = hex_esc;
            }
            break;
        }
        if (esc) {
            size_t elen = strlen(esc);
            if (out + elen >= dst_cap)
                break;
            memcpy(dst + out, esc, elen);
            out += elen;
        } else {
            if (out + 1 >= dst_cap)
                break;
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
    return out;
}
