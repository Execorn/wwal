#include "waywal/path.h"

#include <string.h>

void path_buf_init(path_buf_t *pb) {
    if (!pb) return;
    pb->len = 0;
    pb->data[0] = '\0';
}

void path_buf_from_str(path_buf_t *pb, const char *str) {
    if (!pb) return;
    pb->len = 0;
    pb->data[0] = '\0';

    if (!str) return;

    size_t slen = strlen(str);
    if (slen >= WAYWAL_PATH_MAX) {
        slen = WAYWAL_PATH_MAX - 1;
    }

    memcpy(pb->data, str, slen);
    pb->len = slen;
    pb->data[pb->len] = '\0';
}

bool path_buf_push(path_buf_t *pb, const char *component) {
    if (!pb) return false;
    if (!component || component[0] == '\0') return true;

    size_t comp_len = strlen(component);
    if (comp_len == 0) return true;

    bool need_sep = (pb->len > 0 && pb->data[pb->len - 1] != '/' && component[0] != '/');
    size_t needed = pb->len + comp_len + (need_sep ? 1 : 0) + 1;
    if (needed > WAYWAL_PATH_MAX) {
        return false;
    }

    if (need_sep) {
        pb->data[pb->len++] = '/';
    }

    memcpy(&pb->data[pb->len], component, comp_len);
    pb->len += comp_len;
    pb->data[pb->len] = '\0';
    return true;
}

bool path_buf_append(path_buf_t *pb, const char *suffix) {
    if (!pb) return false;
    if (!suffix || suffix[0] == '\0') return true;

    size_t suf_len = strlen(suffix);
    if (pb->len + suf_len + 1 > WAYWAL_PATH_MAX) {
        return false;
    }

    memcpy(&pb->data[pb->len], suffix, suf_len);
    pb->len += suf_len;
    pb->data[pb->len] = '\0';
    return true;
}

bool path_buf_parent(path_buf_t *pb) {
    if (!pb || pb->len == 0) return false;

    /* Trim trailing slashes if len > 1 */
    while (pb->len > 1 && pb->data[pb->len - 1] == '/') {
        pb->data[--pb->len] = '\0';
    }

    if (pb->len == 1 && pb->data[0] == '/') {
        return false; /* Root "/" has no parent */
    }

    /* Search backwards for previous slash */
    for (size_t i = pb->len; i > 0; --i) {
        if (pb->data[i - 1] == '/') {
            if (i - 1 == 0) {
                /* Parent is root "/" */
                pb->len = 1;
                pb->data[1] = '\0';
            } else {
                pb->len = i - 1;
                pb->data[pb->len] = '\0';
            }
            return true;
        }
    }

    /* No slash found: relative single-component path */
    pb->len = 0;
    pb->data[0] = '\0';
    return true;
}

const char *path_buf_cstr(const path_buf_t *pb) {
    if (!pb) return "";
    return pb->data;
}
