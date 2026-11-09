// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/we_appworkshop.h"
#include "waywal/we_vdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static waywal_workshop_item_manifest_t *find_or_add_item(waywal_workshop_item_manifest_t **items,
                                                         size_t *count, size_t *cap,
                                                         uint64_t published_file_id)
{
    if (!items || !count || !cap) {
        return NULL;
    }

    for (size_t i = 0; i < *count; i++) {
        if ((*items)[i].published_file_id == published_file_id) {
            return &(*items)[i];
        }
    }

    if (*count >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        waywal_workshop_item_manifest_t *new_items =
            realloc(*items, new_cap * sizeof(waywal_workshop_item_manifest_t));
        if (!new_items) {
            return NULL;
        }
        *items = new_items;
        *cap = new_cap;
    }

    waywal_workshop_item_manifest_t *it = &(*items)[*count];
    memset(it, 0, sizeof(*it));
    it->published_file_id = published_file_id;
    (*count)++;
    return it;
}

bool waywal_appworkshop_parse_file(const char *manifest_path,
                                   waywal_appworkshop_manifest_t *out_manifest)
{
    if (!manifest_path || manifest_path[0] == '\0' || !out_manifest) {
        return false;
    }
    memset(out_manifest, 0, sizeof(*out_manifest));

    FILE *fp = fopen(manifest_path, "rb");
    if (!fp) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[read_bytes] = '\0';

    waywal_vdf_doc_t *doc = waywal_vdf_parse(buf, read_bytes);
    free(buf);
    if (!doc || !doc->root) {
        if (doc) {
            waywal_vdf_free(doc);
        }
        return false;
    }

    const waywal_vdf_node_t *app_ws = waywal_vdf_find_child(doc->root, "AppWorkshop");
    if (!app_ws) {
        app_ws = doc->root;
    }

    out_manifest->app_id =
        (uint32_t)strtoul(waywal_vdf_get_string(app_ws, "appid", "431960"), NULL, 10);
    out_manifest->size_on_disk_bytes =
        strtoull(waywal_vdf_get_string(app_ws, "SizeOnDisk", "0"), NULL, 10);
    const char *nu_str = waywal_vdf_get_string(app_ws, "NeedsUpdate", NULL);
    if (!nu_str) {
        nu_str = waywal_vdf_get_string(app_ws, "NeedsUpdateCount", "0");
    }
    out_manifest->needs_update_count = (uint32_t)strtoul(nu_str, NULL, 10);
    out_manifest->time_last_updated =
        strtoull(waywal_vdf_get_string(app_ws, "TimeLastUpdated", "0"), NULL, 10);
    out_manifest->time_last_app_check =
        strtoull(waywal_vdf_get_string(app_ws, "TimeLastAppCheck", "0"), NULL, 10);

    waywal_workshop_item_manifest_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    /* 1. Parse WorkshopItemsInstalled */
    const waywal_vdf_node_t *installed_block =
        waywal_vdf_find_child(app_ws, "WorkshopItemsInstalled");
    if (installed_block && installed_block->type == VDF_NODE_OBJECT &&
        installed_block->object.children) {
        for (size_t i = 0; i < installed_block->object.child_count; i++) {
            const waywal_vdf_node_t *entry = installed_block->object.children[i];
            if (!entry || !entry->key) {
                continue;
            }
            uint64_t id = strtoull(entry->key, NULL, 10);
            if (id == 0) {
                continue;
            }

            waywal_workshop_item_manifest_t *it = find_or_add_item(&items, &count, &cap, id);
            if (!it) {
                continue;
            }

            it->is_installed = true;
            it->state_flags |= WAYWAL_UGC_STATE_INSTALLED;

            if (entry->type == VDF_NODE_OBJECT) {
                it->size_bytes = strtoull(waywal_vdf_get_string(entry, "size", "0"), NULL, 10);
                it->time_updated =
                    strtoull(waywal_vdf_get_string(entry, "timeupdated", "0"), NULL, 10);
                const char *man_str = waywal_vdf_get_string(entry, "manifest", NULL);
                if (!man_str) {
                    man_str = waywal_vdf_get_string(entry, "manifestid", "0");
                }
                it->manifest_id = strtoull(man_str, NULL, 10);
            } else if (entry->type == VDF_NODE_STRING && entry->string_value) {
                it->manifest_id = strtoull(entry->string_value, NULL, 10);
            }
        }
    }

    /* 2. Parse WorkshopItemDetails */
    const waywal_vdf_node_t *details_block = waywal_vdf_find_child(app_ws, "WorkshopItemDetails");
    if (details_block && details_block->type == VDF_NODE_OBJECT && details_block->object.children) {
        for (size_t i = 0; i < details_block->object.child_count; i++) {
            const waywal_vdf_node_t *entry = details_block->object.children[i];
            if (!entry || !entry->key) {
                continue;
            }
            uint64_t id = strtoull(entry->key, NULL, 10);
            if (id == 0) {
                continue;
            }

            waywal_workshop_item_manifest_t *it = find_or_add_item(&items, &count, &cap, id);
            if (!it) {
                continue;
            }

            if (entry->type == VDF_NODE_OBJECT) {
                const char *man_str = waywal_vdf_get_string(entry, "manifest", NULL);
                if (!man_str) {
                    man_str = waywal_vdf_get_string(entry, "manifestid", "0");
                }
                uint64_t man = strtoull(man_str, NULL, 10);
                if (man != 0) {
                    it->manifest_id = man;
                }

                uint64_t tu = strtoull(waywal_vdf_get_string(entry, "timeupdated", "0"), NULL, 10);
                if (tu != 0) {
                    it->time_updated = tu;
                }

                const char *sub_str = waywal_vdf_get_string(entry, "subscribedbyuser", NULL);
                if (sub_str && atoi(sub_str) != 0) {
                    it->subscribed_by_user = true;
                    it->state_flags |= WAYWAL_UGC_STATE_SUBSCRIBED;
                }

                const char *inst_str = waywal_vdf_get_string(entry, "installed", NULL);
                if (inst_str && atoi(inst_str) != 0) {
                    it->is_installed = true;
                    it->state_flags |= WAYWAL_UGC_STATE_INSTALLED;
                }

                const char *flags_str = waywal_vdf_get_string(entry, "state", NULL);
                if (!flags_str) {
                    flags_str = waywal_vdf_get_string(entry, "itemstate", NULL);
                }
                if (!flags_str) {
                    flags_str = waywal_vdf_get_string(entry, "flags", NULL);
                }
                if (flags_str) {
                    uint32_t raw_flags = (uint32_t)strtoul(flags_str, NULL, 10);
                    it->state_flags |= raw_flags;
                    if (it->state_flags & WAYWAL_UGC_STATE_SUBSCRIBED) {
                        it->subscribed_by_user = true;
                    }
                    if (it->state_flags & WAYWAL_UGC_STATE_INSTALLED) {
                        it->is_installed = true;
                    }
                }
            }
        }
    }

    uint32_t calc_needs_update = 0;
    uint32_t calc_needs_download = 0;
    uint64_t total_computed_size = 0;

    for (size_t i = 0; i < count; i++) {
        total_computed_size += items[i].size_bytes;
        if (items[i].state_flags & WAYWAL_UGC_STATE_NEEDS_UPDATE) {
            calc_needs_update++;
        }
        if ((items[i].state_flags &
             (WAYWAL_UGC_STATE_DOWNLOADING | WAYWAL_UGC_STATE_DOWNLOAD_PENDING)) ||
            (items[i].subscribed_by_user && !items[i].is_installed)) {
            calc_needs_download++;
        }
    }

    if (out_manifest->size_on_disk_bytes == 0) {
        out_manifest->size_on_disk_bytes = total_computed_size;
    }
    if (out_manifest->needs_update_count == 0) {
        out_manifest->needs_update_count = calc_needs_update;
    }
    out_manifest->needs_download_count = calc_needs_download;
    out_manifest->item_count = count;
    out_manifest->items = items;

    waywal_vdf_free(doc);
    return true;
}

void waywal_appworkshop_free(waywal_appworkshop_manifest_t *manifest)
{
    if (!manifest) {
        return;
    }
    if (manifest->items) {
        free(manifest->items);
        manifest->items = NULL;
    }
    manifest->item_count = 0;
}
