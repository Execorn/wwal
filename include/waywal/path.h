#ifndef WAYWAL_PATH_H
#define WAYWAL_PATH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_PATH_MAX 4096

typedef struct {
    char data[WAYWAL_PATH_MAX];
    size_t len; /* Length excluding final null terminator */
} path_buf_t;

void path_buf_init(path_buf_t *pb);
void path_buf_from_str(path_buf_t *pb, const char *str);

/* Appends component with '/' separator */
bool path_buf_push(path_buf_t *pb, const char *component);

/* Appends characters to the current component without adding '/' */
bool path_buf_append(path_buf_t *pb, const char *suffix);

/* Extracts directory parent */
bool path_buf_parent(path_buf_t *pb);

const char *path_buf_cstr(const path_buf_t *pb);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_PATH_H */
