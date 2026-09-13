#include "waywal/log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

static waywal_log_level_t g_current_level = WAYWAL_LOG_LEVEL_INFO;

void waywal_log_set_level(waywal_log_level_t level) {
    g_current_level = level;
}

waywal_log_level_t waywal_log_get_level(void) {
    return g_current_level;
}

void waywal_log(waywal_log_level_t level, const char *file, int line, const char *fmt, ...) {
    (void)file;
    (void)line;

    if (level < g_current_level || level > WAYWAL_LOG_LEVEL_ERR) {
        return;
    }

    static int is_tty = -1;
    if (is_tty == -1) {
        is_tty = isatty(STDERR_FILENO);
    }

    const char *prefix;
    size_t prefix_len;

    if (is_tty) {
        switch (level) {
            case WAYWAL_LOG_LEVEL_DEBUG:
                prefix = "\033[90m[DEBUG]\033[0m ";
                prefix_len = 16;
                break;
            case WAYWAL_LOG_LEVEL_INFO:
                prefix = "\033[32m[INFO]\033[0m ";
                prefix_len = 15;
                break;
            case WAYWAL_LOG_LEVEL_WARN:
                prefix = "\033[33m[WARN]\033[0m ";
                prefix_len = 15;
                break;
            case WAYWAL_LOG_LEVEL_ERR:
            default:
                prefix = "\033[31m[ERROR]\033[0m ";
                prefix_len = 16;
                break;
        }
    } else {
        switch (level) {
            case WAYWAL_LOG_LEVEL_DEBUG:
                prefix = "[DEBUG] ";
                prefix_len = 8;
                break;
            case WAYWAL_LOG_LEVEL_INFO:
                prefix = "[INFO] ";
                prefix_len = 7;
                break;
            case WAYWAL_LOG_LEVEL_WARN:
                prefix = "[WARN] ";
                prefix_len = 7;
                break;
            case WAYWAL_LOG_LEVEL_ERR:
            default:
                prefix = "[ERROR] ";
                prefix_len = 8;
                break;
        }
    }

    char msg_buf[2048];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    size_t msg_len = (size_t)written;
    if (msg_len >= sizeof(msg_buf)) {
        msg_len = sizeof(msg_buf) - 1;
    }

    static const char newline[] = "\n";

    struct iovec iov[3] = {
        { .iov_base = (void *)prefix, .iov_len = prefix_len },
        { .iov_base = msg_buf,         .iov_len = msg_len },
        { .iov_base = (void *)newline,.iov_len = 1 }
    };

    (void)writev(STDERR_FILENO, iov, 3);
}
