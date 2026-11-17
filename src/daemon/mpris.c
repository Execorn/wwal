// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/mpris.h"

#include "waywal/image_loader.h"
#include "waywal/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void mpris_copy_cstr(char *dst, size_t cap, const char *src)
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
    bool loaded;
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

static sdbus_api_t g_sdbus_api;
static pthread_once_t g_sdbus_once = PTHREAD_ONCE_INIT;

#define MPRIS_INITIAL_PLAYERS 16u
#define MPRIS_MAX_PLAYERS     128u

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
    int wait_fd;
    int bus_fd;
    uint32_t bus_epoll_events;
    int art_event_fd;
    int reconnect_timer_fd;
    uint32_t reconnect_backoff_sec;
    sd_bus_slot *props_match_slot;
    sd_bus_slot *owner_match_slot;
    sd_bus_slot *list_names_slot;
    mpris_player_t **players;
    size_t player_cap;
    waywal_mpris_change_cb_t on_change;
    void *userdata;
    char active_sender[128];
    char active_bus_name[128];
    waywal_mpris_metadata_t meta;
    bool art_changed;
    bool metadata_changed;
    bool test_mode;

    pthread_mutex_t art_mu;
    pthread_cond_t art_cv;
    pthread_t art_thread;
    bool art_sync_init;
    bool art_thread_started;
    bool art_stop;
    bool art_loading_pending;
    bool art_has_req;
    uint64_t art_req_gen;
    char art_req_url[1024];
    char art_req_path[1024];
    uint64_t art_next_gen;
    bool art_has_completed;
    bool art_completed_ok;
    uint64_t art_completed_gen;
    char art_completed_url[1024];
    char art_completed_path[1024];
    raw_image_t art_completed_raw;
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

static void init_sdbus_api_once(void)
{
    memset(&g_sdbus_api, 0, sizeof(g_sdbus_api));
    void *dl_handle = dlopen("libsystemd.so.0", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
    if (!dl_handle) {
        dl_handle = dlopen("libelogind.so.0", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
    }
    if (!dl_handle) {
        return;
    }

    (void)resolve_sdbus_sym(dl_handle, "sd_bus_flush_close_unref", &g_sdbus_api.flush_close_unref);
    (void)resolve_sdbus_sym(dl_handle, "sd_bus_slot_unref", &g_sdbus_api.slot_unref);

    if (!resolve_sdbus_sym(dl_handle, "sd_bus_open_user", &g_sdbus_api.open_user) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_unref", &g_sdbus_api.unref) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_get_fd", &g_sdbus_api.get_fd) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_get_events", &g_sdbus_api.get_events) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_get_timeout", &g_sdbus_api.get_timeout) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_process", &g_sdbus_api.process) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_flush", &g_sdbus_api.flush) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_add_match", &g_sdbus_api.add_match) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_call_method_async", &g_sdbus_api.call_method_async) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_read", &g_sdbus_api.message_read) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_read_basic",
                           &g_sdbus_api.message_read_basic) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_enter_container",
                           &g_sdbus_api.message_enter_container) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_exit_container",
                           &g_sdbus_api.message_exit_container) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_peek_type", &g_sdbus_api.message_peek_type) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_skip", &g_sdbus_api.message_skip) ||
        !resolve_sdbus_sym(dl_handle, "sd_bus_message_get_sender",
                           &g_sdbus_api.message_get_sender)) {
        memset(&g_sdbus_api, 0, sizeof(g_sdbus_api));
        return;
    }
    g_sdbus_api.loaded = true;
}

static bool init_sdbus_api(sdbus_api_t *api)
{
    (void)pthread_once(&g_sdbus_once, init_sdbus_api_once);
    *api = g_sdbus_api;
    return api->loaded;
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

static bool decode_album_art_file(const char *local_path, raw_image_t *out_raw)
{
    if (!local_path || local_path[0] == '\0' || !out_raw) {
        return false;
    }
    memset(out_raw, 0, sizeof(*out_raw));

    int fd = open(local_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4 ||
        st.st_size > 64L * 1024L * 1024L) {
        close(fd);
        return false;
    }

    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) {
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }

    bool ok = image_load_fd(fd, out_raw);
    close(fd);

    if (!ok || !out_raw->data || out_raw->width == 0 || out_raw->height == 0) {
        image_free(out_raw);
        return false;
    }
    return true;
}

static void *mpris_art_worker_main(void *arg)
{
    waywal_mpris_t *mpris = (waywal_mpris_t *)arg;
    for (;;) {
        char req_url[1024];
        char req_path[1024];
        uint64_t req_gen = 0;

        pthread_mutex_lock(&mpris->art_mu);
        while (!mpris->art_stop && !mpris->art_has_req) {
            pthread_cond_wait(&mpris->art_cv, &mpris->art_mu);
        }
        if (mpris->art_stop) {
            pthread_mutex_unlock(&mpris->art_mu);
            break;
        }
        mpris_copy_cstr(req_url, sizeof(req_url), mpris->art_req_url);
        mpris_copy_cstr(req_path, sizeof(req_path), mpris->art_req_path);
        req_gen = mpris->art_next_gen;
        mpris->art_has_req = false;
        pthread_mutex_unlock(&mpris->art_mu);

        raw_image_t raw;
        bool ok = decode_album_art_file(req_path, &raw);

        bool should_notify = false;
        pthread_mutex_lock(&mpris->art_mu);
        if (!mpris->art_stop && !mpris->art_has_req && req_gen == mpris->art_next_gen) {
            if (mpris->art_has_completed) {
                image_free(&mpris->art_completed_raw);
                mpris->art_has_completed = false;
            }
            mpris->art_completed_raw = raw;
            mpris->art_completed_ok = ok;
            mpris->art_completed_gen = req_gen;
            mpris_copy_cstr(mpris->art_completed_url, sizeof(mpris->art_completed_url), req_url);
            mpris_copy_cstr(mpris->art_completed_path, sizeof(mpris->art_completed_path), req_path);
            mpris->art_has_completed = true;
            should_notify = (mpris->art_event_fd >= 0);
        } else {
            image_free(&raw);
        }
        pthread_mutex_unlock(&mpris->art_mu);

        if (should_notify) {
            uint64_t one = 1u;
            while (write(mpris->art_event_fd, &one, sizeof(one)) < 0 && errno == EINTR) {
            }
        }
    }
    return NULL;
}

static bool init_mpris_art_sync(waywal_mpris_t *mpris)
{
    if (mpris->art_sync_init) {
        return true;
    }
    if (pthread_mutex_init(&mpris->art_mu, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&mpris->art_cv, NULL) != 0) {
        pthread_mutex_destroy(&mpris->art_mu);
        return false;
    }
    mpris->art_sync_init = true;
    return true;
}

static void enqueue_album_art_async(waywal_mpris_t *mpris, const char *safe_url,
                                    const char *local_path, uint64_t req_gen)
{
    if (!init_mpris_art_sync(mpris)) {
        return;
    }
    pthread_mutex_lock(&mpris->art_mu);
    if (!mpris->art_thread_started) {
        if (pthread_create(&mpris->art_thread, NULL, mpris_art_worker_main, mpris) == 0) {
            mpris->art_thread_started = true;
        }
    }
    if (mpris->art_thread_started) {
        if (mpris->art_has_completed) {
            image_free(&mpris->art_completed_raw);
            mpris->art_has_completed = false;
        }
        mpris_copy_cstr(mpris->art_req_url, sizeof(mpris->art_req_url), safe_url);
        mpris_copy_cstr(mpris->art_req_path, sizeof(mpris->art_req_path), local_path);
        mpris->art_next_gen = req_gen;
        mpris->art_has_req = true;
        mpris->art_loading_pending = true;
        pthread_cond_signal(&mpris->art_cv);
    }
    pthread_mutex_unlock(&mpris->art_mu);
}

static void harvest_completed_art(waywal_mpris_t *mpris)
{
    if (!mpris || !mpris->art_sync_init) {
        return;
    }
    raw_image_t raw;
    memset(&raw, 0, sizeof(raw));
    bool has = false;
    bool ok = false;
    uint64_t gen = 0;
    char url[1024];
    char path[1024];
    url[0] = '\0';
    path[0] = '\0';

    pthread_mutex_lock(&mpris->art_mu);
    if (mpris->art_has_completed) {
        has = true;
        raw = mpris->art_completed_raw;
        ok = mpris->art_completed_ok;
        gen = mpris->art_completed_gen;
        mpris_copy_cstr(url, sizeof(url), mpris->art_completed_url);
        mpris_copy_cstr(path, sizeof(path), mpris->art_completed_path);
        memset(&mpris->art_completed_raw, 0, sizeof(mpris->art_completed_raw));
        mpris->art_has_completed = false;
    }
    pthread_mutex_unlock(&mpris->art_mu);

    if (!has) {
        return;
    }
    mpris->art_loading_pending = false;
    if (!ok || gen != mpris->art_req_gen || strcmp(mpris->meta.art_url, url) != 0) {
        image_free(&raw);
        return;
    }

    free_album_art_buffers(mpris);
    mpris_copy_cstr(mpris->meta.art_local_path, sizeof(mpris->meta.art_local_path), path);
    mpris->meta.art_bgra = raw.data;
    mpris->meta.art_rgba = NULL;
    mpris->meta.art_w = raw.width;
    mpris->meta.art_h = raw.height;
    mpris->meta.has_art_pixels = true;
    mpris->art_changed = true;
    notify_mpris_change(mpris);
}

static void update_album_art_from_url(waywal_mpris_t *mpris, const char *new_art_url)
{
    if (!mpris)
        return;

    const char *safe_url = new_art_url ? new_art_url : "";
    if (strcmp(mpris->meta.art_url, safe_url) == 0 &&
        (safe_url[0] == '\0' || (mpris->meta.has_art_pixels && mpris->meta.art_bgra != NULL) ||
         (!mpris->test_mode && mpris->art_loading_pending))) {
        return;
    }

    mpris->art_req_gen++;
    mpris->art_loading_pending = false;
    mpris_copy_cstr(mpris->meta.art_url, sizeof(mpris->meta.art_url), safe_url);
    mpris->meta.art_local_path[0] = '\0';
    free_album_art_buffers(mpris);
    mpris->art_changed = true;

    if (safe_url[0] == '\0')
        return;

    char local_path[sizeof(mpris->meta.art_local_path)];
    if (!waywal_mpris_decode_file_uri(safe_url, local_path, sizeof(local_path))) {
        return;
    }

    if (!mpris->test_mode && mpris->art_event_fd >= 0) {
        enqueue_album_art_async(mpris, safe_url, local_path, mpris->art_req_gen);
        return;
    }

    raw_image_t raw;
    if (!decode_album_art_file(local_path, &raw)) {
        return;
    }

    mpris_copy_cstr(mpris->meta.art_local_path, sizeof(mpris->meta.art_local_path), local_path);
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
                    if (api->message_read_basic(m, 't', &uval) >= 0) {
                        *out_length = (uval <= (uint64_t)INT64_MAX) ? (int64_t)uval : INT64_MAX;
                    }
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
                    if (api->message_read_basic(m, 'd', &dval) >= 0 && isfinite(dval)) {
                        if (dval >= 0x1.0p63) {
                            *out_length = INT64_MAX;
                        } else if (dval <= -0x1.0p63) {
                            *out_length = INT64_MIN;
                        } else {
                            *out_length = (int64_t)dval;
                        }
                    }
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
    for (size_t i = 0; i < mpris->player_cap; ++i) {
        mpris_player_t *p = mpris->players[i];
        if (p && p->active && strcmp(p->bus_name, bus_name) == 0) {
            return p;
        }
    }
    return NULL;
}

static mpris_player_t *find_player_by_owner(waywal_mpris_t *mpris, const char *owner)
{
    if (!mpris || !owner || owner[0] == '\0')
        return NULL;
    for (size_t i = 0; i < mpris->player_cap; ++i) {
        mpris_player_t *p = mpris->players[i];
        if (p && p->active && strcmp(p->owner, owner) == 0) {
            return p;
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

    for (size_t i = 0; i < mpris->player_cap; ++i) {
        mpris_player_t *other = mpris->players[i];
        if (!other || other == primary || !other->active)
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
                mpris_copy_cstr(primary->playback_status, sizeof(primary->playback_status),
                                other->playback_status);
            }
            if (primary->title[0] == '\0' && other->title[0] != '\0') {
                mpris_copy_cstr(primary->title, sizeof(primary->title), other->title);
                mpris_copy_cstr(primary->artist, sizeof(primary->artist), other->artist);
                mpris_copy_cstr(primary->album, sizeof(primary->album), other->album);
                mpris_copy_cstr(primary->art_url, sizeof(primary->art_url), other->art_url);
                primary->length_us = other->length_us;
            }
            clear_player_slot(mpris, other);
        }
    }
}

static bool is_current_active_slot(const waywal_mpris_t *mpris, const mpris_player_t *p)
{
    if (!mpris || !p || !p->active) {
        return false;
    }
    if (mpris->active_bus_name[0] != '\0' && p->bus_name[0] != '\0' &&
        strcmp(mpris->active_bus_name, p->bus_name) == 0) {
        return true;
    }
    if (mpris->active_sender[0] != '\0' && p->owner[0] != '\0' &&
        strcmp(mpris->active_sender, p->owner) == 0) {
        return true;
    }
    return false;
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
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            mpris_player_t *cand = mpris->players[i];
            if (!cand) {
                cand = (mpris_player_t *)calloc(1, sizeof(mpris_player_t));
                if (!cand) {
                    return NULL;
                }
                mpris->players[i] = cand;
                clear_player_slot(mpris, cand);
                slot = cand;
                break;
            }
            if (!cand->active) {
                slot = cand;
                break;
            }
        }
    }
    if (!slot && mpris->player_cap < MPRIS_MAX_PLAYERS) {
        size_t old_cap = mpris->player_cap;
        size_t new_cap = (old_cap == 0u) ? MPRIS_INITIAL_PLAYERS : (old_cap * 2u);
        if (new_cap > MPRIS_MAX_PLAYERS) {
            new_cap = MPRIS_MAX_PLAYERS;
        }
        mpris_player_t **grown =
            (mpris_player_t **)realloc(mpris->players, new_cap * sizeof(*grown));
        if (grown) {
            memset(grown + old_cap, 0, (new_cap - old_cap) * sizeof(*grown));
            mpris->players = grown;
            mpris->player_cap = new_cap;
            slot = (mpris_player_t *)calloc(1, sizeof(mpris_player_t));
            if (!slot) {
                return NULL;
            }
            mpris->players[old_cap] = slot;
            clear_player_slot(mpris, slot);
        }
    }
    if (!slot) {
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            mpris_player_t *cand = mpris->players[i];
            if (cand && cand->status == WAYWAL_MPRIS_STOPPED &&
                !is_current_active_slot(mpris, cand)) {
                slot = cand;
                break;
            }
        }
        if (!slot) {
            for (size_t i = 0; i < mpris->player_cap; ++i) {
                mpris_player_t *cand = mpris->players[i];
                if (cand && cand->status == WAYWAL_MPRIS_PAUSED &&
                    !is_current_active_slot(mpris, cand)) {
                    slot = cand;
                    break;
                }
            }
        }
        if (!slot) {
            return NULL;
        }
        clear_player_slot(mpris, slot);
    }
    if (!slot->active) {
        clear_player_slot(mpris, slot);
        slot->active = true;
    }
    if (bus_name && bus_name[0] != '\0') {
        mpris_copy_cstr(slot->bus_name, sizeof(slot->bus_name), bus_name);
    }
    if (owner && owner[0] != '\0') {
        mpris_copy_cstr(slot->owner, sizeof(slot->owner), owner);
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
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            mpris_player_t *p = mpris->players[i];
            if (p && p->active && p->status == WAYWAL_MPRIS_PLAYING) {
                win = p;
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
            for (size_t i = 0; i < mpris->player_cap; ++i) {
                mpris_player_t *p = mpris->players[i];
                if (p && p->active && p->status == WAYWAL_MPRIS_PAUSED) {
                    win = p;
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
            for (size_t i = 0; i < mpris->player_cap; ++i) {
                mpris_player_t *p = mpris->players[i];
                if (p && p->active) {
                    win = p;
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
            snprintf(mpris->active_bus_name, sizeof(mpris->active_bus_name), "%s", win->bus_name);
            snprintf(mpris->active_sender, sizeof(mpris->active_sender), "%s", win->owner);
        } else {
            mpris->active_sender[0] = '\0';
            mpris->active_bus_name[0] = '\0';
        }

        bool meta_diff = (strcmp(mpris->meta.player_name, p_name) != 0 || mpris->meta.playing ||
                          mpris->meta.status != WAYWAL_MPRIS_STOPPED ||
                          strcmp(mpris->meta.playback_status, "Stopped") != 0 ||
                          mpris->meta.title[0] != '\0' || mpris->meta.artist[0] != '\0' ||
                          mpris->meta.album[0] != '\0' || mpris->meta.length_us != 0);

        snprintf(mpris->meta.player_name, sizeof(mpris->meta.player_name), "%s", p_name);
        mpris->meta.playing = false;
        mpris->meta.status = WAYWAL_MPRIS_STOPPED;
        snprintf(mpris->meta.playback_status, sizeof(mpris->meta.playback_status), "%s", "Stopped");
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
    mpris_copy_cstr(mpris->active_bus_name, sizeof(mpris->active_bus_name), win->bus_name);
    mpris_copy_cstr(mpris->active_sender, sizeof(mpris->active_sender), win->owner);

    bool now_playing = (win->status == WAYWAL_MPRIS_PLAYING);
    if (strcmp(mpris->meta.player_name, p_name) != 0 || mpris->meta.playing != now_playing ||
        mpris->meta.status != win->status ||
        strcmp(mpris->meta.playback_status, win->playback_status) != 0 ||
        strcmp(mpris->meta.title, win->title) != 0 ||
        strcmp(mpris->meta.artist, win->artist) != 0 ||
        strcmp(mpris->meta.album, win->album) != 0 || mpris->meta.length_us != win->length_us) {
        mpris_copy_cstr(mpris->meta.player_name, sizeof(mpris->meta.player_name), p_name);
        mpris->meta.playing = now_playing;
        mpris->meta.status = win->status;
        mpris_copy_cstr(mpris->meta.playback_status, sizeof(mpris->meta.playback_status),
                        win->playback_status);
        mpris_copy_cstr(mpris->meta.title, sizeof(mpris->meta.title), win->title);
        mpris_copy_cstr(mpris->meta.artist, sizeof(mpris->meta.artist), win->artist);
        mpris_copy_cstr(mpris->meta.album, sizeof(mpris->meta.album), win->album);
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
                mpris_copy_cstr(slot->title, sizeof(slot->title), new_title);
                mpris_copy_cstr(slot->artist, sizeof(slot->artist), new_artist);
                mpris_copy_cstr(slot->album, sizeof(slot->album), new_album);
                mpris_copy_cstr(slot->art_url, sizeof(slot->art_url), new_art_url);
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
    if (!parse_player_properties_dict_into_slot(mpris, slot, m)) {
        return 0;
    }
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
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            mpris_player_t *p = mpris->players[i];
            if (!p || !p->active)
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

static void update_bus_epoll_mask(waywal_mpris_t *mpris)
{
    if (!mpris || mpris->wait_fd < 0 || mpris->bus_fd < 0 || !mpris->bus ||
        !mpris->api.get_events) {
        return;
    }
    int ev = mpris->api.get_events(mpris->bus);
    uint32_t mask = EPOLLIN;
    if (ev > 0 && (ev & POLLOUT)) {
        mask |= EPOLLOUT;
    }
    if (mask != mpris->bus_epoll_events) {
        struct epoll_event ee = {.events = mask, .data.fd = mpris->bus_fd};
        if (epoll_ctl(mpris->wait_fd, EPOLL_CTL_MOD, mpris->bus_fd, &ee) == 0) {
            mpris->bus_epoll_events = mask;
        }
    }
}

static void arm_reconnect_timer(waywal_mpris_t *mpris, uint32_t delay_sec)
{
    if (!mpris || mpris->reconnect_timer_fd < 0) {
        return;
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)delay_sec;
    (void)timerfd_settime(mpris->reconnect_timer_fd, 0, &its, NULL);
}

static void close_dbus_session(waywal_mpris_t *mpris)
{
    if (!mpris) {
        return;
    }
    if (mpris->wait_fd >= 0 && mpris->bus_fd >= 0) {
        (void)epoll_ctl(mpris->wait_fd, EPOLL_CTL_DEL, mpris->bus_fd, NULL);
    }
    mpris->bus_fd = -1;
    mpris->bus_epoll_events = 0;

    for (size_t i = 0; i < mpris->player_cap; ++i) {
        if (mpris->players[i]) {
            clear_player_slot(mpris, mpris->players[i]);
        }
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
}

static bool connect_dbus_session(waywal_mpris_t *mpris)
{
    if (!mpris || mpris->test_mode || !mpris->api.loaded || mpris->bus) {
        return false;
    }
    if (mpris->api.open_user(&mpris->bus) < 0 || !mpris->bus) {
        mpris->bus = NULL;
        return false;
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

    /* Flush SASL handshake and initial requests before entering event wait. */
    mpris->api.flush(mpris->bus);
    while (mpris->api.process(mpris->bus, NULL) > 0) {
    }
    mpris->api.flush(mpris->bus);

    if (mpris->wait_fd >= 0 && mpris->api.get_fd) {
        int bfd = mpris->api.get_fd(mpris->bus);
        if (bfd >= 0) {
            struct epoll_event ee = {.events = EPOLLIN, .data.fd = bfd};
            if (epoll_ctl(mpris->wait_fd, EPOLL_CTL_ADD, bfd, &ee) == 0) {
                mpris->bus_fd = bfd;
                mpris->bus_epoll_events = EPOLLIN;
                update_bus_epoll_mask(mpris);
            }
        }
    }
    mpris->reconnect_backoff_sec = 1u;
    arm_reconnect_timer(mpris, 0u);
    return true;
}

waywal_mpris_t *waywal_mpris_create(waywal_mpris_change_cb_t on_change, void *userdata)
{
    waywal_mpris_t *mpris = (waywal_mpris_t *)calloc(1, sizeof(waywal_mpris_t));
    if (!mpris)
        return NULL;

    mpris->wait_fd = -1;
    mpris->bus_fd = -1;
    mpris->art_event_fd = -1;
    mpris->reconnect_timer_fd = -1;
    mpris->reconnect_backoff_sec = 1u;
    mpris->on_change = on_change;
    mpris->userdata = userdata;
    mpris->meta.status = WAYWAL_MPRIS_STOPPED;
    strncpy(mpris->meta.playback_status, "Stopped", sizeof(mpris->meta.playback_status) - 1);

    mpris->players = (mpris_player_t **)calloc(MPRIS_INITIAL_PLAYERS, sizeof(mpris_player_t *));
    if (!mpris->players) {
        free(mpris);
        return NULL;
    }
    mpris->player_cap = MPRIS_INITIAL_PLAYERS;

    if (!init_sdbus_api(&mpris->api)) {
        WAYWAL_LOG_DEBUG("MPRIS2: libsystemd.so.0 not available; running in offline/test mode");
        return mpris;
    }

    if (!connect_dbus_session(mpris)) {
        WAYWAL_LOG_DEBUG("MPRIS2: session D-Bus unavailable; running in offline/test mode");
        return mpris;
    }

    int wfd = epoll_create1(EPOLL_CLOEXEC);
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (wfd >= 0) {
        mpris->wait_fd = wfd;
        if (efd >= 0) {
            struct epoll_event ee = {.events = EPOLLIN, .data.fd = efd};
            if (epoll_ctl(wfd, EPOLL_CTL_ADD, efd, &ee) == 0) {
                mpris->art_event_fd = efd;
            } else {
                close(efd);
            }
        }
        if (tfd >= 0) {
            struct epoll_event ee = {.events = EPOLLIN, .data.fd = tfd};
            if (epoll_ctl(wfd, EPOLL_CTL_ADD, tfd, &ee) == 0) {
                mpris->reconnect_timer_fd = tfd;
            } else {
                close(tfd);
            }
        }
        if (mpris->api.get_fd) {
            int bfd = mpris->api.get_fd(mpris->bus);
            if (bfd >= 0) {
                struct epoll_event ee = {.events = EPOLLIN, .data.fd = bfd};
                if (epoll_ctl(wfd, EPOLL_CTL_ADD, bfd, &ee) == 0) {
                    mpris->bus_fd = bfd;
                    mpris->bus_epoll_events = EPOLLIN;
                    update_bus_epoll_mask(mpris);
                }
            }
        }
    } else {
        if (efd >= 0) {
            close(efd);
        }
        if (tfd >= 0) {
            close(tfd);
        }
    }

    return mpris;
}

void waywal_mpris_destroy(waywal_mpris_t *mpris)
{
    if (!mpris)
        return;

    if (mpris->art_sync_init) {
        if (mpris->art_thread_started) {
            pthread_mutex_lock(&mpris->art_mu);
            mpris->art_stop = true;
            pthread_cond_signal(&mpris->art_cv);
            pthread_mutex_unlock(&mpris->art_mu);
            (void)pthread_join(mpris->art_thread, NULL);
            mpris->art_thread_started = false;
        }
        if (mpris->art_has_completed) {
            image_free(&mpris->art_completed_raw);
            mpris->art_has_completed = false;
        }
        pthread_cond_destroy(&mpris->art_cv);
        pthread_mutex_destroy(&mpris->art_mu);
        mpris->art_sync_init = false;
    }

    close_dbus_session(mpris);

    if (mpris->art_event_fd >= 0) {
        close(mpris->art_event_fd);
        mpris->art_event_fd = -1;
    }
    if (mpris->reconnect_timer_fd >= 0) {
        close(mpris->reconnect_timer_fd);
        mpris->reconnect_timer_fd = -1;
    }
    if (mpris->wait_fd >= 0) {
        close(mpris->wait_fd);
        mpris->wait_fd = -1;
    }

    free_album_art_buffers(mpris);
    if (mpris->players) {
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            free(mpris->players[i]);
        }
        free(mpris->players);
        mpris->players = NULL;
    }
    free(mpris);
}

int waywal_mpris_get_fd(const waywal_mpris_t *mpris)
{
    if (!mpris)
        return -1;
    if (mpris->wait_fd >= 0)
        return mpris->wait_fd;
    if (!mpris->bus || !mpris->api.get_fd)
        return -1;
    return mpris->api.get_fd(mpris->bus);
}

bool waywal_mpris_dispatch(waywal_mpris_t *mpris)
{
    if (!mpris)
        return false;
    if (mpris->test_mode && !mpris->bus)
        return true;

    if (mpris->art_event_fd >= 0) {
        uint64_t val = 0;
        ssize_t n = read(mpris->art_event_fd, &val, sizeof(val));
        if (n > 0) {
            harvest_completed_art(mpris);
        }
    }

    if (!mpris->bus) {
        if (mpris->reconnect_timer_fd < 0 || mpris->wait_fd < 0) {
            return false;
        }
        uint64_t expirations = 0;
        ssize_t tr = read(mpris->reconnect_timer_fd, &expirations, sizeof(expirations));
        if (tr > 0 && expirations > 0) {
            if (!connect_dbus_session(mpris)) {
                uint32_t next_sec = mpris->reconnect_backoff_sec * 2u;
                if (next_sec > 10u) {
                    next_sec = 10u;
                }
                mpris->reconnect_backoff_sec = next_sec;
                arm_reconnect_timer(mpris, next_sec);
            }
        }
        return true;
    }

    if (mpris->api.process && mpris->api.flush) {
        int r = 0;
        while ((r = mpris->api.process(mpris->bus, NULL)) > 0) {
        }
        if (r < 0) {
            WAYWAL_LOG_WARN("MPRIS: D-Bus connection error (%d), scheduling reconnect", r);
            close_dbus_session(mpris);
            reconcile_active_player(mpris, NULL);
            if (mpris->wait_fd >= 0 && mpris->reconnect_timer_fd >= 0) {
                mpris->reconnect_backoff_sec = 1u;
                arm_reconnect_timer(mpris, 1u);
                return true;
            }
            return false;
        }
        mpris->api.flush(mpris->bus);
        update_bus_epoll_mask(mpris);
    }
    return true;
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

    mpris_copy_cstr(mpris->meta.playback_status, sizeof(mpris->meta.playback_status),
                    state->playback_status[0] != '\0'
                        ? state->playback_status
                        : (mpris->meta.playing ? "Playing" : "Stopped"));

    mpris_copy_cstr(mpris->meta.player_name, sizeof(mpris->meta.player_name), state->player_name);
    mpris_copy_cstr(mpris->meta.title, sizeof(mpris->meta.title), state->title);
    mpris_copy_cstr(mpris->meta.artist, sizeof(mpris->meta.artist), state->artist);
    mpris_copy_cstr(mpris->meta.album, sizeof(mpris->meta.album), state->album);

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
        for (size_t i = 0; i < mpris->player_cap; ++i) {
            if (mpris->players[i]) {
                clear_player_slot(mpris, mpris->players[i]);
            }
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
        snprintf(slot->playback_status, sizeof(slot->playback_status), "%s",
                 state->playback_status);
    } else {
        const char *status_str = "Stopped";
        if (new_status == WAYWAL_MPRIS_PLAYING)
            status_str = "Playing";
        else if (new_status == WAYWAL_MPRIS_PAUSED)
            status_str = "Paused";
        snprintf(slot->playback_status, sizeof(slot->playback_status), "%s", status_str);
    }

    if (new_status == WAYWAL_MPRIS_STOPPED && prev_status != WAYWAL_MPRIS_STOPPED &&
        state->title[0] == '\0') {
        slot->title[0] = '\0';
        slot->artist[0] = '\0';
        slot->album[0] = '\0';
        slot->art_url[0] = '\0';
        slot->length_us = 0;
    } else if (state->title[0] != '\0' || state->artist[0] != '\0' || state->album[0] != '\0' ||
               state->art_url[0] != '\0' || state->length_us != 0) {
        mpris_copy_cstr(slot->title, sizeof(slot->title), state->title);
        mpris_copy_cstr(slot->artist, sizeof(slot->artist), state->artist);
        mpris_copy_cstr(slot->album, sizeof(slot->album), state->album);
        mpris_copy_cstr(slot->art_url, sizeof(slot->art_url), state->art_url);
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
    for (size_t i = 0; i < mpris->player_cap; ++i) {
        mpris_player_t *p = mpris->players[i];
        if (!p || !p->active)
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
