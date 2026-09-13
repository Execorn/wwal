#ifndef WAYWAL_LOG_H
#define WAYWAL_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_LOG_LEVEL_DEBUG = 0,
    WAYWAL_LOG_LEVEL_INFO  = 1,
    WAYWAL_LOG_LEVEL_WARN  = 2,
    WAYWAL_LOG_LEVEL_ERR   = 3,
    WAYWAL_LOG_LEVEL_NONE  = 4,
} waywal_log_level_t;

void waywal_log_set_level(waywal_log_level_t level);
waywal_log_level_t waywal_log_get_level(void);

void waywal_log(waywal_log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define WAYWAL_LOG_DEBUG(fmt, ...) waywal_log(WAYWAL_LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define WAYWAL_LOG_INFO(fmt, ...)  waywal_log(WAYWAL_LOG_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define WAYWAL_LOG_WARN(fmt, ...)  waywal_log(WAYWAL_LOG_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define WAYWAL_LOG_ERR(fmt, ...)   waywal_log(WAYWAL_LOG_LEVEL_ERR,   __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_LOG_H */
