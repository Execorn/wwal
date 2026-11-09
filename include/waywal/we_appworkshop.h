// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_APPWORKSHOP_H
#define WAYWAL_WE_APPWORKSHOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Steamworks SDK EItemState Bitmask */
#ifdef __cplusplus
typedef enum : uint32_t {
#else
typedef enum {
#endif
    WAYWAL_UGC_STATE_NONE = 0u,
    WAYWAL_UGC_STATE_SUBSCRIBED = 1u << 0,
    WAYWAL_UGC_STATE_LEGACY_ITEM = 1u << 1,
    WAYWAL_UGC_STATE_INSTALLED = 1u << 2,
    WAYWAL_UGC_STATE_NEEDS_UPDATE = 1u << 3,
    WAYWAL_UGC_STATE_DOWNLOADING = 1u << 4,
    WAYWAL_UGC_STATE_DOWNLOAD_PENDING = 1u << 5,
} waywal_ugc_state_t;

typedef struct {
    uint64_t published_file_id;
    uint64_t size_bytes;
    uint64_t time_updated;
    uint64_t manifest_id;
    uint32_t state_flags; // Bitmask of waywal_ugc_state_t
    bool subscribed_by_user;
    bool is_installed;
} waywal_workshop_item_manifest_t;

typedef struct {
    uint32_t app_id; // 431960
    uint64_t size_on_disk_bytes;
    uint32_t needs_update_count;
    uint32_t needs_download_count;
    uint64_t time_last_updated;
    uint64_t time_last_app_check;
    size_t item_count;
    waywal_workshop_item_manifest_t *items;
} waywal_appworkshop_manifest_t;

/* Parses appworkshop_431960.vdf into structured manifest */
bool waywal_appworkshop_parse_file(const char *manifest_path,
                                   waywal_appworkshop_manifest_t *out_manifest);
void waywal_appworkshop_free(waywal_appworkshop_manifest_t *manifest);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_APPWORKSHOP_H */
