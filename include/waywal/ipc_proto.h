// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_IPC_PROTO_H
#define WAYWAL_IPC_PROTO_H

#include "waywal/bezier.h"
#include "waywal/path.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define WAYWAL_IPC_MAGIC                                                                           \
    0x4C415757U /* "WWAL" in little-endian ASCII: 'W' | 'W'<<8 | 'A'<<16 | 'L'<<24 */
#define WAYWAL_IPC_VERSION 1

typedef enum : uint16_t {
    WAYWAL_REQ_PING = 0x0001,
    WAYWAL_REQ_QUERY = 0x0002,
    WAYWAL_REQ_CLEAR = 0x0003,
    WAYWAL_REQ_SET_IMAGE = 0x0004,
    WAYWAL_REQ_IMG = WAYWAL_REQ_SET_IMAGE,
    WAYWAL_REQ_SET_VIDEO = 0x0005,
    WAYWAL_REQ_VIDEO = WAYWAL_REQ_SET_VIDEO,
    WAYWAL_REQ_TOGGLE = 0x0006,
    WAYWAL_REQ_PAUSE = 0x0007,
    WAYWAL_REQ_UNPAUSE = 0x0008,
    WAYWAL_REQ_KILL = 0x0009,
    WAYWAL_REQ_SLIDESHOW = 0x000A,
    WAYWAL_REQ_SHADER = 0x000B,
    WAYWAL_IPC_OP_SHADER = WAYWAL_REQ_SHADER,
    WAYWAL_REQ_STATS = 0x000C,
    WAYWAL_IPC_OP_STATS = WAYWAL_REQ_STATS,
    WAYWAL_REQ_RESTORE = 0x000D,
    WAYWAL_IPC_OP_RESTORE = WAYWAL_REQ_RESTORE,
    WAYWAL_REQ_SLIDESHOW_CTRL = 0x000E,
    WAYWAL_REQ_WORKSHOP = 0x000F,
    WAYWAL_IPC_OP_WORKSHOP = WAYWAL_REQ_WORKSHOP,
    WAYWAL_REQ_QUERY_JSON = 0x0010,
    WAYWAL_IPC_OP_QUERY_JSON = WAYWAL_REQ_QUERY_JSON,
    WAYWAL_REQ_SET_UNIFORM = 0x0011,
    WAYWAL_IPC_OP_SET_UNIFORM = WAYWAL_REQ_SET_UNIFORM,
    WAYWAL_REQ_SUBSCRIBE = 0x0012,
    WAYWAL_IPC_OP_SUBSCRIBE = WAYWAL_REQ_SUBSCRIBE,
    WAYWAL_REQ_AUDIO_CTRL = 0x0013,
    WAYWAL_IPC_OP_AUDIO_CTRL = WAYWAL_REQ_AUDIO_CTRL,
    WAYWAL_REQ_SLIDESHOW_NEXT = 0x0014,
    WAYWAL_IPC_OP_SLIDESHOW_NEXT = WAYWAL_REQ_SLIDESHOW_NEXT,
    WAYWAL_REQ_SLIDESHOW_PREV = 0x0015,
    WAYWAL_IPC_OP_SLIDESHOW_PREV = WAYWAL_REQ_SLIDESHOW_PREV,
    WAYWAL_REQ_SET_SHADER_PARAM = 0x0016,
    WAYWAL_IPC_OP_SET_SHADER_PARAM = WAYWAL_REQ_SET_SHADER_PARAM,
    WAYWAL_REQ_GET_TELEMETRY = 0x0017,
    WAYWAL_IPC_OP_GET_TELEMETRY = WAYWAL_REQ_GET_TELEMETRY,
    WAYWAL_REQ_EXPORT_OUTPUT_DMABUF = 0x0018,
    WAYWAL_IPC_OP_EXPORT_OUTPUT_DMABUF = WAYWAL_REQ_EXPORT_OUTPUT_DMABUF,
    WAYWAL_REQ_WORKSHOP_SYNC = 0x0019u,
    WAYWAL_IPC_OP_WORKSHOP_SYNC = WAYWAL_REQ_WORKSHOP_SYNC,
    WAYWAL_REQ_WORKSHOP_DOWNLOAD = 0x001Au,
    WAYWAL_IPC_OP_WORKSHOP_DOWNLOAD = WAYWAL_REQ_WORKSHOP_DOWNLOAD,
    WAYWAL_REQ_WWPKG_INSTALL = 0x001Bu,
    WAYWAL_IPC_OP_WWPKG_INSTALL = WAYWAL_REQ_WWPKG_INSTALL,
    WAYWAL_REQ_SCAN_COMPAT = 0x001Cu,
    WAYWAL_IPC_OP_SCAN_COMPAT = WAYWAL_REQ_SCAN_COMPAT,
    WAYWAL_REQ_COMPILE_SHADER_GRAPH = 0x001Du,
    WAYWAL_IPC_OP_COMPILE_SHADER_GRAPH = WAYWAL_REQ_COMPILE_SHADER_GRAPH,
    WAYWAL_REQ_SET_CAMERA_3D = 0x001Eu,
    WAYWAL_IPC_OP_SET_CAMERA_3D = WAYWAL_REQ_SET_CAMERA_3D,
    WAYWAL_REQ_SET_PHYSICS_PARAMS = 0x001Fu,
    WAYWAL_IPC_OP_SET_PHYSICS_PARAMS = WAYWAL_REQ_SET_PHYSICS_PARAMS,
    WAYWAL_REQ_SET_SCENE_LIGHTS = 0x0020u,
    WAYWAL_IPC_OP_SET_SCENE_LIGHTS = WAYWAL_REQ_SET_SCENE_LIGHTS,

    WAYWAL_RESP_OK = 0x8001,
    WAYWAL_RESP_PONG = 0x8002,
    WAYWAL_RESP_INFO = 0x8003,
    WAYWAL_RESP_ERR = 0x8004,
    WAYWAL_RESP_EVENT = 0x8005,
    WAYWAL_RESP_TELEMETRY = 0x8006,
    WAYWAL_RESP_DMABUF = 0x8007,
    WAYWAL_RESP_WORKSHOP_SYNC = 0x8008u,
    WAYWAL_RESP_WORKSHOP_DOWNLOAD = 0x8009u,
    WAYWAL_RESP_WWPKG_INSTALL = 0x800Au,
    WAYWAL_RESP_SCAN_COMPAT = 0x800Bu,
    WAYWAL_RESP_COMPILE_SHADER_GRAPH = 0x800Cu,
    WAYWAL_RESP_SET_CAMERA_3D = 0x800Du,
    WAYWAL_RESP_SET_PHYSICS_PARAMS = 0x800Eu,
    WAYWAL_RESP_SET_SCENE_LIGHTS = 0x800Fu,
} waywal_opcode_t;
typedef waywal_opcode_t waywal_ipc_op_t;

#define WAYWAL_REQ_WORKSHOP_SYNC         WAYWAL_REQ_WORKSHOP_SYNC
#define WAYWAL_RESP_WORKSHOP_SYNC        WAYWAL_RESP_WORKSHOP_SYNC
#define WAYWAL_REQ_WORKSHOP_DOWNLOAD     WAYWAL_REQ_WORKSHOP_DOWNLOAD
#define WAYWAL_RESP_WORKSHOP_DOWNLOAD    WAYWAL_RESP_WORKSHOP_DOWNLOAD
#define WAYWAL_REQ_WWPKG_INSTALL         WAYWAL_REQ_WWPKG_INSTALL
#define WAYWAL_RESP_WWPKG_INSTALL        WAYWAL_RESP_WWPKG_INSTALL
#define WAYWAL_REQ_SCAN_COMPAT           WAYWAL_REQ_SCAN_COMPAT
#define WAYWAL_RESP_SCAN_COMPAT          WAYWAL_RESP_SCAN_COMPAT
#define WAYWAL_REQ_COMPILE_SHADER_GRAPH  WAYWAL_REQ_COMPILE_SHADER_GRAPH
#define WAYWAL_RESP_COMPILE_SHADER_GRAPH WAYWAL_RESP_COMPILE_SHADER_GRAPH
#define WAYWAL_REQ_SET_CAMERA_3D         WAYWAL_REQ_SET_CAMERA_3D
#define WAYWAL_RESP_SET_CAMERA_3D        WAYWAL_RESP_SET_CAMERA_3D
#define WAYWAL_REQ_SET_PHYSICS_PARAMS    WAYWAL_REQ_SET_PHYSICS_PARAMS
#define WAYWAL_RESP_SET_PHYSICS_PARAMS   WAYWAL_RESP_SET_PHYSICS_PARAMS
#define WAYWAL_REQ_SET_SCENE_LIGHTS      WAYWAL_REQ_SET_SCENE_LIGHTS
#define WAYWAL_RESP_SET_SCENE_LIGHTS     WAYWAL_RESP_SET_SCENE_LIGHTS

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
    uint32_t filter_mode;       /* waywal_filter_mode_t (default: 0 = LANCZOS3) */
    bezier_curve_t bezier;
    bool has_custom_bezier;
    bool ten_bit;
    bool extract_theme;
    bool no_theme;
    bool theme_light_mode;
    char depth_map_path[256];
    char backdrop[256];
    char layer_name[32];
    char battery_policy[32];
    char filepath[4096];
    float depth_strength;
    float crop_gravity_x;
    float crop_gravity_y;
    uint8_t has_crop_gravity;
    uint8_t invert_y;
    uint8_t no_cache;
    union {
        uint8_t _pad_flags;
        struct {
            uint8_t is_solid_color : 1;
            uint8_t pos_y_inverted : 1;
            uint8_t _pad_flags_bits : 6;
        };
    };
    uint8_t source_color_index;
    uint8_t has_source_color_index;
    uint8_t has_seed_color;
    uint8_t _pad_seed;
    uint32_t seed_color;
    uint8_t has_theme_mode;
    uint8_t client_exported_theme;
    uint8_t has_auto_day_night;
    uint8_t auto_day_night;
    uint8_t has_solar_coords;
    uint8_t _pad_solar;
    double solar_latitude;
    double solar_longitude;
    /* Followed by num_target_outputs null-terminated strings, then custom shader GLSL string */
} waywal_img_metadata_t;
typedef waywal_img_metadata_t waywal_ipc_img_meta_t;
static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
              "Legacy 72-byte prefix of waywal_img_metadata_t must remain frozen at offset 72");

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
    uint32_t scaling_mode;
    uint32_t no_cache;
    float volume;
    bool mute_on_audio;
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
    char target_output[64];
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
    char target_outputs[16][64];
    uint32_t scaling_mode; /* waywal_scaling_mode_t (default: 0 = FILL) */
    uint32_t filter_mode;  /* waywal_filter_mode_t (default: 0 = LANCZOS3) */
    bezier_curve_t bezier;
    bool has_custom_bezier;
    uint8_t color[4];
    bool ten_bit;
    char path[4096];
    float crop_gravity_x;
    float crop_gravity_y;
    uint8_t has_crop_gravity;
    uint8_t invert_y;
    uint8_t pos_y_inverted;
    uint8_t _pad_ss[1];
    uint32_t no_cache;
    uint8_t extract_theme;
    uint8_t no_theme;
    uint8_t has_theme_mode;
    uint8_t theme_light_mode;
    uint8_t has_source_color_index;
    uint8_t source_color_index;
    uint8_t has_seed_color;
    uint8_t auto_day_night;
    uint32_t seed_color;
    uint8_t sort_mode;     /* waywal_sort_mode_t */
    uint8_t has_sort_mode; /* 1 if sort_mode explicitly set */
    uint8_t no_recursive;  /* 1 to disable subdirectory recursion (0 = recursive by default) */
    uint8_t _pad_ss3;
    uint8_t has_solar_coords;
    uint8_t _pad_ss2[3];
    double solar_latitude;
    double solar_longitude;
} waywal_slideshow_payload_t;

typedef struct {
    char output[64];
    char shader_path[4096];
    uint32_t fps;
    bool audio_reactive;
    uint32_t scaling_mode;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    bezier_curve_t bezier;
    bool has_custom_bezier;
    uint8_t invert_y;
    uint8_t pos_y_inverted;
    uint32_t no_cache;
    float volume;
    bool mute_on_audio;
    float speed;
    char backdrop[256];
} waywal_shader_payload_t;

typedef struct {
    char output[64];
    char workshop_dir[4096];
    char prop_overrides[2048];
    uint32_t fps;
    bool audio_reactive;
    uint32_t scaling_mode;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    bezier_curve_t bezier;
    bool has_custom_bezier;
    uint8_t invert_y;
    uint8_t pos_y_inverted;
    uint32_t no_cache;
    uint8_t extract_theme;
    uint8_t no_theme;
    uint8_t has_theme_mode;
    uint8_t theme_light_mode;
    uint8_t has_source_color_index;
    uint8_t source_color_index;
    uint8_t has_seed_color;
    uint8_t client_exported_theme;
    uint32_t seed_color;
    float volume;
    bool mute_on_audio;
    char backdrop[256];
    char layer_name[32];
} waywal_workshop_payload_t;

#define WAYWAL_UNIFORM_FLAG_PERSIST 0x0001U

typedef struct {
    union {
        /* Standard 64/64 layout (CLI, daemon, legacy) */
        struct {
            char output[64];         /* Empty string "" = apply to all outputs */
            char name[64];           /* GLSL uniform name or WE property key */
            uint32_t num_components; /* 1..4 */
            float values[4];
        };
        /* Structured 32/64 layout with explicit persistence flags */
        struct {
            char target_output[32];
            char uniform_name[64];
            uint32_t uniform_components;
            float uniform_values[4];
            uint32_t flags; /* Bit 0: WAYWAL_UNIFORM_FLAG_PERSIST */
            uint8_t _reserved[28];
        };
        uint8_t raw[148];
    };
} waywal_set_uniform_payload_t;
static_assert(sizeof(waywal_set_uniform_payload_t) == 148,
              "waywal_set_uniform_payload_t must be 148 bytes");
typedef waywal_set_uniform_payload_t waywal_shader_param_payload_t;

typedef struct {
    float volume;            /* < 0.0f = unchanged, 0.0..1.0 = set master volume */
    int8_t mute_on_audio;    /* -1 = unchanged, 0 = disable, 1 = enable */
    int8_t theme_from_media; /* -1 = unchanged, 0 = disable, 1 = enable */
    uint8_t reserved[2];
} waywal_audio_ctrl_payload_t;
static_assert(sizeof(waywal_audio_ctrl_payload_t) == 8,
              "waywal_audio_ctrl_payload_t must be 8 bytes");

#define WAYWAL_MAX_TELEMETRY_OUTPUTS 8

typedef enum : uint8_t {
    WAYWAL_BO_STATE_FREE = 0,
    WAYWAL_BO_STATE_RENDERING = 1,
    WAYWAL_BO_STATE_COMMITTED = 2,
    WAYWAL_BO_STATE_SCANOUT_LOCKED = 3,
} waywal_bo_slot_state_t;

typedef struct {
    char name[64];
    float scanout_fps;
    uint32_t last_render_us;
    uint32_t vram_kib;
    uint8_t bo_states[4]; /* waywal_bo_slot_state_t */
    uint8_t engine_kind;  /* 0=static, 1=shader, 2=workshop, 3=video, 4=slideshow, 5=animation */
    uint8_t is_paused;
    uint16_t pause_mask;
    uint32_t width;
    uint32_t height;
    uint32_t logical_width;
    uint32_t logical_height;
    int32_t scale_120;
    uint8_t _pad[8];
} waywal_output_telemetry_t;
static_assert(sizeof(waywal_output_telemetry_t) == 112,
              "waywal_output_telemetry_t must be 112 bytes");

#define WAYWAL_TELEMETRY_FORMAT_BINARY 0x0000U
#define WAYWAL_TELEMETRY_FORMAT_JSON   0x0001U

typedef struct {
    uint32_t version;      /* 1 */
    uint32_t flags;        /* Bit 0: format (0=binary, 1=json) */
    uint64_t timestamp_ns; /* CLOCK_MONOTONIC timestamp */

    /* Process resources */
    uint32_t rss_kib;
    uint32_t pss_kib;
    uint32_t total_vram_kib;
    uint32_t gpu_active; /* 1 = GPU compute active, 0 = CPU fallback */

    /* Power governor */
    uint8_t on_battery;
    uint8_t battery_policy; /* 0=ignore, 1=reduce-fps, 2=pause-video */
    uint16_t global_pause_mask;

    /* Audio FFT */
    float bark_bands[64];    /* 64 Bark-scale frequency bands */
    float rms_level;         /* RMS envelope */
    float beat_pulse;        /* Normalized beat onset */
    uint8_t beat_onset;      /* 1 if beat onset active */
    uint8_t audio_active;    /* 1 if audio engine active */
    uint8_t pipewire_active; /* 1 if PipeWire stream active */
    uint8_t mute_on_audio;
    float audio_volume;

    /* Outputs */
    uint32_t num_outputs;
    waywal_output_telemetry_t outputs[WAYWAL_MAX_TELEMETRY_OUTPUTS];
} waywal_telemetry_snapshot_t;
static_assert(sizeof(waywal_telemetry_snapshot_t) == 1208,
              "waywal_telemetry_snapshot_t must be 1208 bytes");

typedef struct {
    char output[64]; /* Target output name, or empty string "" for primary active output */
} waywal_export_dmabuf_req_t;
static_assert(sizeof(waywal_export_dmabuf_req_t) == 64,
              "waywal_export_dmabuf_req_t must be 64 bytes");

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint32_t num_planes;
    uint64_t modifier;
    uint32_t strides[4];
    uint32_t offsets[4];
    char output[64];
    uint32_t flags; /* Bit 0: 10-bit color, Bit 1: linear */
    uint8_t _pad[4];
} waywal_export_dmabuf_resp_t;
static_assert(sizeof(waywal_export_dmabuf_resp_t) == 128,
              "waywal_export_dmabuf_resp_t must be 128 bytes");
typedef waywal_export_dmabuf_resp_t waywal_dmabuf_export_resp_t;
typedef waywal_export_dmabuf_resp_t waywal_dmabuf_frame_desc_t;

/* WAYWAL_REQ_WORKSHOP_SYNC (0x0019) */
#define WAYWAL_WORKSHOP_SYNC_FLAG_FORCE_RESCAN 0x0001u
#define WAYWAL_WORKSHOP_SYNC_FLAG_PURGE_STALE  0x0002u

typedef struct {
    uint32_t flags; /* 0x1 = Force re-scan, 0x2 = Purge stale cache */
    char custom_root[1024];
} waywal_ipc_req_workshop_sync_t;
static_assert(sizeof(waywal_ipc_req_workshop_sync_t) == 1028, "Must be 1028 bytes");

/* WAYWAL_RESP_WORKSHOP_SYNC (0x8008) */
typedef struct {
    uint32_t status_code; /* 0 = OK */
    uint32_t total_libraries_found;
    uint32_t total_items_indexed;
    uint32_t
        _reserved; /* Explicit padding to naturally 8-byte align total_size_bytes at offset 16 */
    uint64_t total_size_bytes;
} waywal_ipc_resp_workshop_sync_t;
static_assert(sizeof(waywal_ipc_resp_workshop_sync_t) == 24,
              "Must be 24 bytes with 8-byte aligned total_size_bytes");
static_assert(offsetof(waywal_ipc_resp_workshop_sync_t, total_size_bytes) == 16,
              "total_size_bytes must be at offset 16");

/* WAYWAL_REQ_WORKSHOP_DOWNLOAD (0x001A) */
#define WAYWAL_DOWNLOAD_BACKEND_AUTO     0u
#define WAYWAL_DOWNLOAD_BACKEND_STEAMAPI 1u
#define WAYWAL_DOWNLOAD_BACKEND_WEBAPI   2u
#define WAYWAL_DOWNLOAD_BACKEND_STEAMCMD 3u

typedef struct {
    uint64_t published_file_id;
    uint32_t priority;          /* TaskPriority */
    uint32_t preferred_backend; /* 0=Auto, 1=SteamAPI, 2=WebAPI, 3=steamcmd */
} waywal_ipc_req_workshop_download_t;
static_assert(sizeof(waywal_ipc_req_workshop_download_t) == 16, "Must be 16 bytes");

/* WAYWAL_RESP_WORKSHOP_DOWNLOAD (0x8009) */
typedef struct {
    uint64_t published_file_id;
    uint32_t task_state; /* TaskState */
    float progress_pct;
    float speed_bytes_per_sec;
    uint32_t eta_seconds;
    char error_msg[256];
} waywal_ipc_resp_workshop_download_t;
static_assert(sizeof(waywal_ipc_resp_workshop_download_t) == 280, "Must be 280 bytes");

/* WAYWAL_REQ_WWPKG_INSTALL (0x001B) */
#define WAYWAL_WWPKG_INSTALL_FLAG_VERIFY_ONLY     0x0001u
#define WAYWAL_WWPKG_INSTALL_FLAG_FORCE_OVERWRITE 0x0002u
#define WAYWAL_WWPKG_INSTALL_FLAG_AUTO_APPLY      0x0004u

typedef struct {
    uint8_t package_uuid[16];
    uint32_t flags; /* 0x1 = Verify only, 0x2 = Force overwrite, 0x4 = Auto-apply */
    uint32_t target_output_len;
    /* target_output_len bytes of UTF-8 output name follow immediately */
} waywal_ipc_req_wwpkg_install_t;
static_assert(sizeof(waywal_ipc_req_wwpkg_install_t) == 24, "Must be 24 bytes header");

/* WAYWAL_RESP_WWPKG_INSTALL (0x800A) */
typedef struct {
    uint8_t package_uuid[16];
    uint32_t status_code; /* 0 = OK, non-zero = error */
    uint32_t error_msg_len;
    /* error_msg_len bytes of UTF-8 error message follow if status_code != 0 */
} waywal_ipc_resp_wwpkg_install_t;
static_assert(sizeof(waywal_ipc_resp_wwpkg_install_t) == 24, "Must be 24 bytes header");

/* WAYWAL_REQ_SCAN_COMPAT (0x001C) */
#define WAYWAL_SCAN_COMPAT_FLAG_APPLY_REPAIRS 0x0001u

typedef struct {
    char target_path[1024];
    uint32_t auto_repair_flags; /* 0x1 = Apply repairs to cache */
} waywal_ipc_req_scan_compat_t;
static_assert(sizeof(waywal_ipc_req_scan_compat_t) == 1028, "Must be 1028 bytes");

/* WAYWAL_RESP_SCAN_COMPAT (0x800B) */
typedef struct {
    uint32_t status_code;
    uint32_t compat_tier;  /* we_compat_tier_t */
    uint32_t compat_score; /* 0 .. 100 */
    uint32_t total_issues;
    uint32_t repairs_applied;
    float estimated_vram_mb;
    uint32_t estimated_fps;
    char report_json_path[1024];
} waywal_ipc_resp_scan_compat_t;
static_assert(sizeof(waywal_ipc_resp_scan_compat_t) == 1052, "Must be 1052 bytes");

#pragma pack(pop)

/* ========================================================================= */
/* Workshop, Open Hub & Compatibility IPC Serialization Helpers             */
/* ========================================================================= */

bool waywal_ipc_validate_req_workshop_sync(const waywal_ipc_req_workshop_sync_t *req,
                                           size_t payload_size);
bool waywal_ipc_serialize_req_workshop_sync(uint32_t flags, const char *custom_root,
                                            waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_req_workshop_sync_t *out_req);

bool waywal_ipc_validate_resp_workshop_sync(const waywal_ipc_resp_workshop_sync_t *resp,
                                            size_t payload_size);
bool waywal_ipc_serialize_resp_workshop_sync(uint32_t status_code, uint32_t total_libraries_found,
                                             uint32_t total_items_indexed,
                                             uint64_t total_size_bytes, waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_workshop_sync_t *out_resp);

bool waywal_ipc_validate_req_workshop_download(const waywal_ipc_req_workshop_download_t *req,
                                               size_t payload_size);
bool waywal_ipc_serialize_req_workshop_download(uint64_t published_file_id, uint32_t priority,
                                                uint32_t preferred_backend,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_req_workshop_download_t *out_req);

bool waywal_ipc_validate_resp_workshop_download(const waywal_ipc_resp_workshop_download_t *resp,
                                                size_t payload_size);
bool waywal_ipc_serialize_resp_workshop_download(uint64_t published_file_id, uint32_t task_state,
                                                 float progress_pct, float speed_bytes_per_sec,
                                                 uint32_t eta_seconds, const char *error_msg,
                                                 waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_resp_workshop_download_t *out_resp);

bool waywal_ipc_validate_req_wwpkg_install(const waywal_ipc_req_wwpkg_install_t *req,
                                           size_t payload_size, const char **out_target_output);
bool waywal_ipc_serialize_req_wwpkg_install(const uint8_t package_uuid[16], uint32_t flags,
                                            const char *target_output, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_req_wwpkg_install_t *out_req, void *out_buf,
                                            size_t out_buf_cap, size_t *out_total_len);

bool waywal_ipc_validate_resp_wwpkg_install(const waywal_ipc_resp_wwpkg_install_t *resp,
                                            size_t payload_size, const char **out_error_msg);
bool waywal_ipc_serialize_resp_wwpkg_install(const uint8_t package_uuid[16], uint32_t status_code,
                                             const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_wwpkg_install_t *out_resp,
                                             void *out_buf, size_t out_buf_cap,
                                             size_t *out_total_len);

bool waywal_ipc_validate_req_scan_compat(const waywal_ipc_req_scan_compat_t *req,
                                         size_t payload_size);
bool waywal_ipc_serialize_req_scan_compat(const char *target_path, uint32_t auto_repair_flags,
                                          waywal_ipc_hdr_t *out_hdr,
                                          waywal_ipc_req_scan_compat_t *out_req);

bool waywal_ipc_validate_resp_scan_compat(const waywal_ipc_resp_scan_compat_t *resp,
                                          size_t payload_size);
bool waywal_ipc_serialize_resp_scan_compat(uint32_t status_code, uint32_t compat_tier,
                                           uint32_t compat_score, uint32_t total_issues,
                                           uint32_t repairs_applied, float estimated_vram_mb,
                                           uint32_t estimated_fps, const char *report_json_path,
                                           waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_resp_scan_compat_t *out_resp);

/* Resolves runtime socket path: $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY-wwald.<namespace>.sock */
bool waywal_ipc_resolve_socket_path(path_buf_t *out_path, const char *namespace_str);

/* Server: Create listening non-blocking Unix domain socket */
int waywal_ipc_server_create(const char *namespace_str);
bool waywal_ipc_is_socket_activated(void);
bool waywal_ipc_namespace_is_alive(const char *namespace_str);

/* Client: Connect to running daemon socket */
int waywal_ipc_client_connect(const char *namespace_str, int timeout_ms);

/* SCM_RIGHTS file descriptor sending and receiving */
bool waywal_ipc_send(int socket_fd, const waywal_ipc_hdr_t *hdr, int attached_fd);
bool waywal_ipc_recv(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_attached_fd);
bool waywal_ipc_send_fds(int socket_fd, const waywal_ipc_hdr_t *hdr, const int *fds,
                         size_t num_fds);
bool waywal_ipc_recv_fds(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_fds, size_t max_fds,
                         size_t *out_num_fds);

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef __cplusplus
}
#endif

#include "waywal/ipc_proto_phase3.h"

#endif /* WAYWAL_IPC_PROTO_H */
