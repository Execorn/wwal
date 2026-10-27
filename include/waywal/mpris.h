// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_MPRIS_H
#define WAYWAL_MPRIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_MPRIS_STOPPED = 0,
    WAYWAL_MPRIS_PLAYING = 1,
    WAYWAL_MPRIS_PAUSED = 2,
} waywal_mpris_status_t;

typedef struct {
    bool playing;
    waywal_mpris_status_t status;
    char playback_status[32];
    char player_name[128];
    char title[256];
    char artist[256];
    char album[256];
    char art_url[1024];
    char art_local_path[1024];
    int64_t length_us;
    uint8_t *art_rgba;
    uint8_t *art_bgra;
    union {
        uint32_t art_w;
        uint32_t art_width;
    };
    union {
        uint32_t art_h;
        uint32_t art_height;
    };
    bool has_art_pixels;
} waywal_mpris_metadata_t;

typedef waywal_mpris_metadata_t waywal_mpris_state_t;

typedef void (*waywal_mpris_change_cb_t)(const waywal_mpris_state_t *state, void *userdata);

typedef struct waywal_mpris waywal_mpris_t;

waywal_mpris_t *waywal_mpris_create(waywal_mpris_change_cb_t on_change, void *userdata);
void waywal_mpris_destroy(waywal_mpris_t *mpris);

int waywal_mpris_get_fd(const waywal_mpris_t *mpris);
bool waywal_mpris_dispatch(waywal_mpris_t *mpris);

const waywal_mpris_metadata_t *waywal_mpris_get_metadata(const waywal_mpris_t *mpris);
const waywal_mpris_state_t *waywal_mpris_get_state(const waywal_mpris_t *mpris);
bool waywal_mpris_is_playing(const waywal_mpris_t *mpris);
bool waywal_mpris_consume_art_changed(waywal_mpris_t *mpris);
bool waywal_mpris_consume_metadata_changed(waywal_mpris_t *mpris);

bool waywal_mpris_decode_file_uri(const char *uri, char *out_path, size_t out_cap);
void waywal_mpris_inject_state_for_test(waywal_mpris_t *mpris, const waywal_mpris_state_t *state);
void waywal_mpris_update_player_for_test(waywal_mpris_t *mpris, const char *bus_name,
                                         const char *owner, const waywal_mpris_state_t *state);
void waywal_mpris_remove_player_for_test(waywal_mpris_t *mpris, const char *bus_name,
                                         const char *owner);

size_t waywal_json_escape_string(const char *src, char *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_MPRIS_H */
