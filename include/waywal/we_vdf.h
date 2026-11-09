// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_VDF_H
#define WAYWAL_WE_VDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VDF_NODE_OBJECT = 0,
    VDF_NODE_STRING = 1,
} waywal_vdf_type_t;

typedef struct waywal_vdf_node_t waywal_vdf_node_t;

typedef struct {
    waywal_vdf_node_t **children;
    size_t child_count;
    size_t child_capacity;
} waywal_vdf_object_t;

struct waywal_vdf_node_t {
    char *key;
    waywal_vdf_type_t type;
    union {
        char *string_value;
        waywal_vdf_object_t object;
    };
};

typedef struct waywal_vdf_doc_t {
    waywal_vdf_node_t *root;
    void *arena_ctx;
} waywal_vdf_doc_t;

/* Parses a raw VDF text buffer using a high-speed bump arena */
waywal_vdf_doc_t *waywal_vdf_parse(const char *text, size_t length);
void waywal_vdf_free(waywal_vdf_doc_t *doc);

/* AST Traversal Helpers */
const waywal_vdf_node_t *waywal_vdf_find_child(const waywal_vdf_node_t *parent, const char *key);
const char *waywal_vdf_get_string(const waywal_vdf_node_t *parent, const char *key,
                                  const char *fallback);

/* Discovered Multi-Drive Steam Library */
typedef struct {
    char path[4096];
    char label[128];
    uint64_t total_size_bytes;
    uint64_t app_size_bytes;
    bool has_we_app;
} waywal_steam_library_t;

/* Scans host environment and returns array of all active Steam libraries */
size_t waywal_steam_discover_libraries(waywal_steam_library_t *out_libs, size_t max_libs);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_VDF_H */
