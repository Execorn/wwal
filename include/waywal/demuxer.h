#ifndef WAYWAL_DEMUXER_H
#define WAYWAL_DEMUXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAYWAL_CODEC_UNKNOWN = 0,
    WAYWAL_CODEC_H264,
    WAYWAL_CODEC_HEVC,
    WAYWAL_CODEC_VP9,
    WAYWAL_CODEC_AV1,
} waywal_codec_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t pts_us; /* Presentation timestamp in microseconds */
    int64_t dts_us; /* Decode timestamp in microseconds */
    bool is_keyframe;
} demux_packet_t;

typedef struct demuxer demuxer_t;

struct demuxer {
    const uint8_t *mapped_buf;
    size_t mapped_size;
    waywal_codec_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    int64_t duration_us;

    /* Codec-specific extradata (e.g. SPS/PPS for H.264/HEVC) */
    const uint8_t *extradata;
    size_t extradata_size;

    /* Internal parser state */
    void *internal_state;

    /* Demuxer vtable */
    bool (*read_packet)(demuxer_t *d, demux_packet_t *pkt);
    bool (*seek_us)(demuxer_t *d, int64_t target_pts_us);
    void (*destroy)(demuxer_t *d);
};

/* Probes and initializes demuxer from mapped memory (MP4 or WebM) */
demuxer_t *demuxer_open_mem(const uint8_t *data, size_t size);

/* Convenience helper to open and map a file path */
demuxer_t *demuxer_open_file(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_DEMUXER_H */
