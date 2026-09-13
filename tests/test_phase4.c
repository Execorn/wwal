#include "waywal/demuxer.h"
#include "waywal/log.h"
#include "waywal/vaapi_dec.h"
#include "waywal/video_engine.h"

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
const char *__lsan_default_suppressions(void)
{
    return "leak:libnvidia\nleak:libGLX_nvidia\nleak:libEGL_nvidia\nleak:radeonsi\nleak:libva\n";
}
#endif
#endif

#define FOURCC(a, b, c, d)                                                                         \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |                             \
     ((uint32_t)(uint8_t)(c) << 8) | ((uint32_t)(uint8_t)(d)))

static inline void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/* Helper to build a minimal valid synthetic MP4 container in memory */
static size_t build_synthetic_mp4(uint8_t *buf, size_t max_size, uint32_t width, uint32_t height,
                                  uint32_t sample_count)
{
    uint8_t *p = buf;

    /* 1. ftyp box */
    uint8_t *ftyp_start = p;
    p += 4; /* size placeholder */
    write_u32_be(p, FOURCC('f', 't', 'y', 'p'));
    p += 4;
    write_u32_be(p, FOURCC('i', 's', 'o', 'm'));
    p += 4; /* major brand */
    write_u32_be(p, 512);
    p += 4; /* minor version */
    write_u32_be(p, FOURCC('i', 's', 'o', 'm'));
    p += 4; /* compatible brands */
    write_u32_be(ftyp_start, (uint32_t)(p - ftyp_start));

    /* 2. mdat box (raw video samples) */
    uint8_t *mdat_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'a', 't'));
    p += 4;
    size_t sample_payload_offset = (size_t)(p - buf);
    size_t sample_size = 64;

    for (uint32_t i = 0; i < sample_count; ++i) {
        memset(p, (int)(0xAA + (i & 0x0F)), sample_size);
        p += sample_size;
    }
    write_u32_be(mdat_start, (uint32_t)(p - mdat_start));

    /* 3. moov box */
    uint8_t *moov_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'o', 'o', 'v'));
    p += 4;

    /* mvhd */
    uint8_t *mvhd_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'v', 'h', 'd'));
    p += 4;
    write_u32_be(p, 0);
    p += 4; /* version + flags */
    write_u32_be(p, 0);
    p += 4; /* creation */
    write_u32_be(p, 0);
    p += 4; /* mod */
    write_u32_be(p, 1000);
    p += 4; /* timescale = 1000 */
    write_u32_be(p, sample_count * 33);
    p += 4; /* duration */
    write_u32_be(p, 0x00010000);
    p += 4; /* rate 1.0 */
    write_u16_be(p, 0x0100);
    p += 2; /* volume */
    memset(p, 0, 70);
    p += 70; /* reserved + matrix + pre_defined */
    write_u32_be(p, 2);
    p += 4; /* next_track_id */
    write_u32_be(mvhd_start, (uint32_t)(p - mvhd_start));

    /* trak */
    uint8_t *trak_start = p;
    p += 4;
    write_u32_be(p, FOURCC('t', 'r', 'a', 'k'));
    p += 4;

    /* mdia */
    uint8_t *mdia_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'i', 'a'));
    p += 4;

    /* mdhd */
    uint8_t *mdhd_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'h', 'd'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1000);
    p += 4; /* timescale */
    write_u32_be(p, sample_count * 33);
    p += 4;
    write_u16_be(p, 0);
    p += 2;
    write_u16_be(p, 0);
    p += 2;
    write_u32_be(mdhd_start, (uint32_t)(p - mdhd_start));

    /* hdlr (vide) */
    uint8_t *hdlr_start = p;
    p += 4;
    write_u32_be(p, FOURCC('h', 'd', 'l', 'r'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, FOURCC('v', 'i', 'd', 'e'));
    p += 4;
    memset(p, 0, 16);
    p += 16;
    write_u32_be(hdlr_start, (uint32_t)(p - hdlr_start));

    /* minf */
    uint8_t *minf_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'i', 'n', 'f'));
    p += 4;

    /* stbl */
    uint8_t *stbl_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 'b', 'l'));
    p += 4;

    /* stsd */
    uint8_t *stsd_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'd'));
    p += 4;
    write_u32_be(p, 0);
    p += 4; /* version + flags */
    write_u32_be(p, 1);
    p += 4; /* entry_count */

    /* avc1 sample entry */
    uint8_t *avc1_start = p;
    p += 4;
    write_u32_be(p, FOURCC('a', 'v', 'c', '1'));
    p += 4;
    memset(p, 0, 6);
    p += 6; /* reserved */
    write_u16_be(p, 1);
    p += 2; /* data_reference_index */
    memset(p, 0, 16);
    p += 16; /* pre_defined + reserved */
    write_u16_be(p, (uint16_t)width);
    p += 2;
    write_u16_be(p, (uint16_t)height);
    p += 2;
    memset(p, 0, 50);
    p += 50; /* resolution, frame_count, compressorname, depth */
    write_u32_be(avc1_start, (uint32_t)(p - avc1_start));
    write_u32_be(stsd_start, (uint32_t)(p - stsd_start));

    /* stsz */
    uint8_t *stsz_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'z'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, (uint32_t)sample_size);
    p += 4; /* uniform sample size */
    write_u32_be(p, sample_count);
    p += 4;
    write_u32_be(stsz_start, (uint32_t)(p - stsz_start));

    /* stco */
    uint8_t *stco_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 'c', 'o'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, sample_count);
    p += 4; /* 1 chunk per sample for simplicity */
    for (uint32_t i = 0; i < sample_count; ++i) {
        write_u32_be(p, (uint32_t)(sample_payload_offset + i * sample_size));
        p += 4;
    }
    write_u32_be(stco_start, (uint32_t)(p - stco_start));

    /* stsc */
    uint8_t *stsc_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'c'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1);
    p += 4; /* 1 entry */
    write_u32_be(p, 1);
    p += 4; /* first_chunk */
    write_u32_be(p, 1);
    p += 4; /* samples_per_chunk */
    write_u32_be(p, 1);
    p += 4; /* sample_description_index */
    write_u32_be(stsc_start, (uint32_t)(p - stsc_start));

    /* stts */
    uint8_t *stts_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 't', 's'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(p, sample_count);
    p += 4; /* sample count */
    write_u32_be(p, 33);
    p += 4; /* sample delta (33 ms ~ 30fps) */
    write_u32_be(stts_start, (uint32_t)(p - stts_start));

    /* stss (keyframes) */
    uint8_t *stss_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 's'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 2);
    p += 4;
    write_u32_be(p, 1);
    p += 4; /* sample 1 is keyframe */
    write_u32_be(p, sample_count / 2 + 1);
    p += 4; /* middle sample is keyframe */
    write_u32_be(stss_start, (uint32_t)(p - stss_start));

    write_u32_be(stbl_start, (uint32_t)(p - stbl_start));
    write_u32_be(minf_start, (uint32_t)(p - minf_start));
    write_u32_be(mdia_start, (uint32_t)(p - mdia_start));
    write_u32_be(trak_start, (uint32_t)(p - trak_start));
    write_u32_be(moov_start, (uint32_t)(p - moov_start));

    assert((size_t)(p - buf) <= max_size);
    return (size_t)(p - buf);
}

static void test_demuxer_zero_allocation(void)
{
    printf("[TEST] Running test_demuxer_zero_allocation...\n");

    uint8_t mp4_buf[65536];
    size_t mp4_size = build_synthetic_mp4(mp4_buf, sizeof(mp4_buf), 1920, 1080, 20);
    assert(mp4_size > 0);

    demuxer_t *d = demuxer_open_mem(mp4_buf, mp4_size);
    assert(d != NULL);
    assert(d->codec == WAYWAL_CODEC_H264);
    assert(d->width == 1920);
    assert(d->height == 1080);
    assert(d->fps_num >= 29 && d->fps_num <= 31);

    demux_packet_t pkt;
    uint32_t packet_count = 0;
    while (d->read_packet(d, &pkt)) {
        assert(pkt.data >= mp4_buf && pkt.data + pkt.size <= mp4_buf + mp4_size);
        assert(pkt.size == 64);
        if (packet_count == 0 || packet_count == 10) {
            assert(pkt.is_keyframe);
        }
        packet_count++;
    }
    assert(packet_count == 20);

    /* Test seek to beginning */
    bool seek_ok = d->seek_us(d, 0);
    assert(seek_ok);
    bool read_after_seek = d->read_packet(d, &pkt);
    assert(read_after_seek);
    assert(pkt.pts_us == 0);
    assert(pkt.is_keyframe);

    d->destroy(d);
    printf("  Successfully demuxed 20 packets with zero-copy buffer pointers\n");
    printf("[TEST] test_demuxer_zero_allocation PASSED.\n");
}

static void test_real_mp4_demuxing(void)
{
    printf("[TEST] Running test_real_mp4_demuxing...\n");
    demuxer_t *d = demuxer_open_file("tests/test_video.mp4");
    if (!d) {
        d = demuxer_open_file("../tests/test_video.mp4");
    }
    if (!d) {
        printf("  [SKIPPED] tests/test_video.mp4 not found\n");
        return;
    }

    assert(d->codec == WAYWAL_CODEC_H264);
    assert(d->width == 640);
    assert(d->height == 480);
    assert(d->fps_num >= 59 && d->fps_num <= 61);

    demux_packet_t pkt;
    uint32_t count = 0;
    while (d->read_packet(d, &pkt)) {
        assert(pkt.size > 0);
        assert(pkt.data != NULL);
        count++;
    }
    assert(count == 60);

    /* Test seek and loop boundary */
    bool seek_ok = d->seek_us(d, 0);
    assert(seek_ok);
    bool read_ok = d->read_packet(d, &pkt);
    assert(read_ok);
    assert(pkt.pts_us == 0);

    d->destroy(d);
    printf("  Successfully verified 60 real H.264 video frames with zero-copy packet slicing\n");
    printf("[TEST] test_real_mp4_demuxing PASSED.\n");
}

/* Helper to build a minimal valid synthetic WebM/EBML container in memory */
static size_t build_synthetic_webm(uint8_t *buf, size_t max_size, uint32_t width, uint32_t height,
                                   uint32_t sample_count)
{
    uint8_t *p = buf;

    /* EBML Header: ID 0x1A45DFA3, size 0 */
    *p++ = 0x1A;
    *p++ = 0x45;
    *p++ = 0xDF;
    *p++ = 0xA3;
    *p++ = 0x80; /* size 0 */

    /* Segment: ID 0x18538067, 4-byte vint size */
    *p++ = 0x18;
    *p++ = 0x53;
    *p++ = 0x80;
    *p++ = 0x67;
    uint8_t *seg_sz = p;
    p += 4;
    uint8_t *seg_content_start = p;

    /* Info: ID 0x1549A966 */
    *p++ = 0x15;
    *p++ = 0x49;
    *p++ = 0xA9;
    *p++ = 0x66;
    uint8_t *info_sz = p;
    p += 1;
    uint8_t *info_start = p;
    /* TimecodeScale (ID 0x2AD7B1, 4 bytes = 1,000,000 ns / 1 ms) */
    *p++ = 0x2A;
    *p++ = 0xD7;
    *p++ = 0xB1;
    *p++ = 0x84; /* size 4 */
    write_u32_be(p, 1000000);
    p += 4;
    *info_sz = (uint8_t)(0x80 | (p - info_start));

    /* Tracks: ID 0x1654AE6B */
    *p++ = 0x16;
    *p++ = 0x54;
    *p++ = 0xAE;
    *p++ = 0x6B;
    uint8_t *tracks_sz = p;
    p += 2;
    uint8_t *tracks_start = p;

    /* TrackEntry: ID 0xAE */
    *p++ = 0xAE;
    uint8_t *entry_sz = p;
    p += 1;
    uint8_t *entry_start = p;

    /* TrackNumber: ID 0xD7, size 1, value 1 */
    *p++ = 0xD7;
    *p++ = 0x81;
    *p++ = 1;

    /* TrackType: ID 0x83, size 1, value 1 (video) */
    *p++ = 0x83;
    *p++ = 0x81;
    *p++ = 1;

    /* CodecID: ID 0x86, size 5, string "V_VP9" */
    *p++ = 0x86;
    *p++ = 0x85;
    memcpy(p, "V_VP9", 5);
    p += 5;

    /* Video: ID 0xE0 */
    *p++ = 0xE0;
    uint8_t *vid_sz = p;
    p += 1;
    uint8_t *vid_start = p;

    /* PixelWidth: ID 0xB0, size 2 */
    *p++ = 0xB0;
    *p++ = 0x82;
    write_u16_be(p, (uint16_t)width);
    p += 2;

    /* PixelHeight: ID 0xBA, size 2 */
    *p++ = 0xBA;
    *p++ = 0x82;
    write_u16_be(p, (uint16_t)height);
    p += 2;

    *vid_sz = (uint8_t)(0x80 | (p - vid_start));
    *entry_sz = (uint8_t)(0x80 | (p - entry_start));

    size_t trk_len = (size_t)(p - tracks_start);
    tracks_sz[0] = (uint8_t)(0x40 | (trk_len >> 8));
    tracks_sz[1] = (uint8_t)(trk_len & 0xFF);

    /* Clusters */
    for (uint32_t i = 0; i < sample_count; ++i) {
        *p++ = 0x1F;
        *p++ = 0x43;
        *p++ = 0xB6;
        *p++ = 0x75;
        uint8_t *clus_sz = p;
        p += 2;
        uint8_t *clus_start = p;

        /* ClusterTimecode: ID 0xE7, size 2 */
        *p++ = 0xE7;
        *p++ = 0x82;
        write_u16_be(p, (uint16_t)(i * 33));
        p += 2;

        /* SimpleBlock: ID 0xA3 */
        *p++ = 0xA3;
        size_t block_payload_size = 4 + 64; /* track(1) + rel_time(2) + flags(1) + data(64) */
        *p++ = (uint8_t)(0x80 | block_payload_size);

        *p++ = 0x81; /* track 1 */
        write_u16_be(p, 0);
        p += 2;                                   /* rel time = 0 */
        *p++ = (i == 0 || i == 10) ? 0x80 : 0x00; /* keyframe flag (0x80 = keyframe) */
        memset(p, 0xCC, 64);
        p += 64;

        size_t clus_len = (size_t)(p - clus_start);
        clus_sz[0] = (uint8_t)(0x40 | (clus_len >> 8));
        clus_sz[1] = (uint8_t)(clus_len & 0xFF);
    }

    size_t seg_len = (size_t)(p - seg_content_start);
    seg_sz[0] = (uint8_t)(0x10 | (seg_len >> 24));
    seg_sz[1] = (uint8_t)((seg_len >> 16) & 0xFF);
    seg_sz[2] = (uint8_t)((seg_len >> 8) & 0xFF);
    seg_sz[3] = (uint8_t)(seg_len & 0xFF);

    assert((size_t)(p - buf) <= max_size);
    return (size_t)(p - buf);
}

static void test_webm_demuxer_zero_allocation(void)
{
    printf("[TEST] Running test_webm_demuxer_zero_allocation...\n");

    uint8_t webm_buf[65536];
    size_t webm_size = build_synthetic_webm(webm_buf, sizeof(webm_buf), 1920, 1080, 20);
    assert(webm_size > 0);

    demuxer_t *d = demuxer_open_mem(webm_buf, webm_size);
    assert(d != NULL);
    assert(d->codec == WAYWAL_CODEC_VP9);
    assert(d->width == 1920);
    assert(d->height == 1080);
    assert(d->fps_num >= 29 && d->fps_num <= 32);

    demux_packet_t pkt;
    uint32_t packet_count = 0;
    while (d->read_packet(d, &pkt)) {
        assert(pkt.data >= webm_buf && pkt.data + pkt.size <= webm_buf + webm_size);
        assert(pkt.size == 64);
        if (packet_count == 0 || packet_count == 10) {
            assert(pkt.is_keyframe);
        } else {
            assert(!pkt.is_keyframe);
        }
        packet_count++;
    }
    assert(packet_count == 20);

    /* Test seek to beginning */
    bool seek_ok = d->seek_us(d, 0);
    assert(seek_ok);
    bool read_after_seek = d->read_packet(d, &pkt);
    assert(read_after_seek);
    assert(pkt.pts_us == 0);
    assert(pkt.is_keyframe);

    d->destroy(d);
    printf("  Successfully demuxed 20 WebM VP9 packets with zero-copy buffer pointers\n");
    printf("[TEST] test_webm_demuxer_zero_allocation PASSED.\n");
}

static void test_vaapi_decoder_and_prime_export(void)
{
    printf("[TEST] Running test_vaapi_decoder_and_prime_export...\n");

    vaapi_decoder_t dec;
    bool init_ok = vaapi_decoder_init(&dec, -1, WAYWAL_CODEC_H264, 1920, 1080);
    if (!init_ok) {
        printf("  [SKIPPED] Hardware VA-API not available or DRM node inaccessible\n");
        return;
    }

    assert(dec.initialized);
    assert(dec.va_profile != VAProfileNone);

    /* Submit synthetic I-frame packet (BUG-06 DPB verification) */
    uint8_t dummy_nalu[32] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};
    demux_packet_t pkt_i = {.data = dummy_nalu,
                            .size = sizeof(dummy_nalu),
                            .pts_us = 0,
                            .dts_us = 0,
                            .is_keyframe = true};

    VASurfaceID surf_i = VA_INVALID_SURFACE;
    bool dec_ok1 = vaapi_decoder_decode_packet(&dec, &pkt_i, &surf_i);
    assert(dec_ok1);
    assert(surf_i != VA_INVALID_SURFACE);
    assert(dec.dpb.count == 1);
    assert(dec.dpb.entries[0].surface == surf_i);
    assert(dec.dpb.entries[0].is_reference);

    /* Submit synthetic P-frame packet (BUG-06 DPB reference tracking) */
    demux_packet_t pkt_p = {.data = dummy_nalu,
                            .size = sizeof(dummy_nalu),
                            .pts_us = 33333,
                            .dts_us = 33333,
                            .is_keyframe = false};

    VASurfaceID surf_p = VA_INVALID_SURFACE;
    bool dec_ok2 = vaapi_decoder_decode_packet(&dec, &pkt_p, &surf_p);
    assert(dec_ok2);
    assert(surf_p != VA_INVALID_SURFACE);
    assert(dec.dpb.count == 2);
    assert(dec.dpb.entries[0].surface == surf_i);
    assert(dec.dpb.entries[1].surface == surf_p);

    /* Export decoded surface to DRM PRIME 2 */
    vaapi_prime_frame_t prime_frame;
    bool export_ok = vaapi_decoder_export_prime(&dec, surf_p, &prime_frame);
    assert(export_ok);
    assert(prime_frame.num_planes >= 1);
    assert(prime_frame.fds[0] >= 0);
    assert(prime_frame.drm_format == DRM_FORMAT_NV12);
    assert(prime_frame.width == 1920);
    assert(prime_frame.height == 1080);

    printf("  Exported PRIME 2 DMA-BUF: planes=%u, fd0=%d, mod=0x%016lx, format=0x%08x (DPB count: "
           "%zu)\n",
           prime_frame.num_planes, prime_frame.fds[0], (unsigned long)prime_frame.modifiers[0],
           prime_frame.drm_format, dec.dpb.count);

    vaapi_prime_frame_close(&prime_frame);
    vaapi_decoder_destroy(&dec);
    printf("[TEST] test_vaapi_decoder_and_prime_export PASSED.\n");
}

static void test_high_precision_monotonic_timer(void)
{
    printf("[TEST] Running test_high_precision_monotonic_timer...\n");

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    assert(tfd >= 0);

    /* Target: 5 ms per tick */
    const int64_t target_interval_ns = 5000000LL;
    struct itimerspec its = {.it_interval = {0, 0},
                             .it_value = {.tv_sec = 0, .tv_nsec = target_interval_ns}};

    int64_t total_jitter_ns = 0;
    const int num_ticks = 5;

    for (int i = 0; i < num_ticks; ++i) {
        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        timerfd_settime(tfd, 0, &its, NULL);

        /* Wait for timerfd to trigger */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tfd, &rfds);

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int sel = select(tfd + 1, &rfds, NULL, NULL, &tv);
        assert(sel == 1);

        uint64_t exp = 0;
        ssize_t s = read(tfd, &exp, sizeof(exp));
        assert(s == sizeof(exp));

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        int64_t elapsed_ns =
            (t_end.tv_sec - t_start.tv_sec) * 1000000000LL + (t_end.tv_nsec - t_start.tv_nsec);
        int64_t jitter_ns = llabs(elapsed_ns - target_interval_ns);
        total_jitter_ns += jitter_ns;
    }

    close(tfd);

    double avg_jitter_us = (double)(total_jitter_ns / num_ticks) / 1000.0;
    printf("  Average timerfd pacing jitter across %d ticks: %.3f us (Target < 1000 us)\n",
           num_ticks, avg_jitter_us);
    assert(avg_jitter_us < 1000.0); /* Well within sub-millisecond requirement */

    printf("[TEST] test_high_precision_monotonic_timer PASSED.\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Executing WayWal Phase 4 Test Suite\n");
    printf("=========================================\n");

    test_demuxer_zero_allocation();
    test_webm_demuxer_zero_allocation();
    test_real_mp4_demuxing();
    test_vaapi_decoder_and_prime_export();
    test_high_precision_monotonic_timer();

    printf("\n>>> ALL PHASE 4 UNIT TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
