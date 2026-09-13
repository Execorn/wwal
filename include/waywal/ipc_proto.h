#ifndef WAYWAL_IPC_PROTO_H
#define WAYWAL_IPC_PROTO_H

#include "waywal/path.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_IPC_MAGIC                                                                           \
    0x4C415757U /* "WWAL" in little-endian ASCII: 'W' | 'W'<<8 | 'A'<<16 | 'L'<<24 */
#define WAYWAL_IPC_VERSION 1

typedef enum : uint16_t {
    WAYWAL_REQ_PING = 0x0001,
    WAYWAL_REQ_QUERY = 0x0002,
    WAYWAL_REQ_CLEAR = 0x0003,
    WAYWAL_REQ_SET_IMAGE = 0x0004,
    WAYWAL_REQ_SET_VIDEO = 0x0005,
    WAYWAL_REQ_TOGGLE = 0x0006,
    WAYWAL_REQ_PAUSE = 0x0007,
    WAYWAL_REQ_UNPAUSE = 0x0008,
    WAYWAL_REQ_KILL = 0x0009,
    WAYWAL_REQ_SLIDESHOW = 0x000A,
    WAYWAL_REQ_SLIDESHOW_CTRL = 0x000B,

    WAYWAL_RESP_OK = 0x8001,
    WAYWAL_RESP_PONG = 0x8002,
    WAYWAL_RESP_INFO = 0x8003,
    WAYWAL_RESP_ERR = 0x8004,
} waywal_opcode_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        /* WAYWAL_IPC_MAGIC */
    uint16_t version;      /* WAYWAL_IPC_VERSION */
    uint16_t opcode;       /* waywal_opcode_t */
    uint64_t payload_size; /* Attached payload size in bytes */
} waywal_ipc_hdr_t;
static_assert(sizeof(waywal_ipc_hdr_t) == 16, "Header must be exactly 16 bytes");

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    uint8_t color[4];
    uint32_t num_target_outputs;
    uint32_t sync_mode;         /* 0 = simultaneous, 1 = staggered */
    uint32_t stagger_delay_ms;  /* delay between cascading monitors */
    uint32_t custom_shader_len; /* bytes of custom shader code following outputs */
    uint32_t extra_data_len;    /* total length of (outputs strings + custom shader string) */
    uint32_t scaling_mode;      /* waywal_scaling_mode_t (default: 0 = FILL) */
    /* Followed by num_target_outputs null-terminated strings, then custom shader GLSL string */
} waywal_img_metadata_t;

typedef struct {
    uint8_t color[4];
    uint32_t num_target_outputs;
    /* Followed by num_target_outputs null-terminated strings */
} waywal_clear_payload_t;

typedef struct {
    uint64_t loop_count;  /* 0 = infinite loop */
    float playback_speed; /* default 1.0f */
    uint32_t num_target_outputs;
    char filepath[4096];
} waywal_video_payload_t;

typedef enum : uint32_t {
    WAYWAL_SLIDESHOW_STOP = 0,
    WAYWAL_SLIDESHOW_PAUSE = 1,
    WAYWAL_SLIDESHOW_RESUME = 2,
    WAYWAL_SLIDESHOW_TOGGLE = 3,
    WAYWAL_SLIDESHOW_NEXT = 4,
    WAYWAL_SLIDESHOW_PREV = 5,
} waywal_slideshow_action_t;

typedef struct {
    uint32_t action; /* waywal_slideshow_action_t */
} waywal_slideshow_ctrl_t;

typedef struct {
    uint32_t interval_s;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    bool random_order;
    uint32_t num_target_outputs;
    uint32_t scaling_mode; /* waywal_scaling_mode_t (default: 0 = FILL) */
    char path[4096];
} waywal_slideshow_payload_t;
#pragma pack(pop)

/* Resolves runtime socket path: $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY-wwald.<namespace>.sock */
bool waywal_ipc_resolve_socket_path(path_buf_t *out_path, const char *namespace_str);

/* Server: Create listening non-blocking Unix domain socket */
int waywal_ipc_server_create(const char *namespace_str);

/* Client: Connect to running daemon socket */
int waywal_ipc_client_connect(const char *namespace_str, int timeout_ms);

/* SCM_RIGHTS file descriptor sending and receiving */
bool waywal_ipc_send(int socket_fd, const waywal_ipc_hdr_t *hdr, int attached_fd);
bool waywal_ipc_recv(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_attached_fd);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_IPC_PROTO_H */
