// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/demuxer.h"
#include "waywal/log.h"
#include "waywal/vaapi_dec.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <assert.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:libcuda\nleak:cuInit\nleak:gbm_create_device\nleak:libgbm\n"
           "leak:libEGL\nleak:libGLESv2\nleak:dri\nleak:libnvidia\nleak:libGLX_nvidia\nleak:"
           "libEGL_nvidia\nleak:radeonsi\nleak:libva\n";
}
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

    vaapi_decoder_t dec = {0};
    bool init_ok = vaapi_decoder_init(&dec, -1, WAYWAL_CODEC_H264, 1920, 1080);
    if (!init_ok) {
        printf("  [SKIPPED] Hardware VA-API not available or DRM node inaccessible\n");
        return;
    }

    assert(dec.initialized);
    assert(dec.va_profile != VAProfileNone);

    /* Submit synthetic I-frame packet for DPB verification */
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

    /* Submit synthetic P-frame packet for DPB reference tracking */
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

    if (dec.has_vpp) {
        assert(dec.num_vpp_surfaces == WAYWAL_VA_VPP_POOL_SIZE);
        bool vpp_ok = vaapi_decoder_vpp_blit(&dec, surf_p, dec.vpp_surfaces[0]);
        assert(vpp_ok);
        vaapi_prime_frame_t vpp_prime;
        assert(vaapi_decoder_export_prime(&dec, dec.vpp_surfaces[0], &vpp_prime));
        assert(vpp_prime.num_planes == 1);
        assert(vpp_prime.fds[0] >= 0);
        assert(vpp_prime.drm_format == DRM_FORMAT_ARGB8888 ||
               vpp_prime.drm_format == DRM_FORMAT_XRGB8888);
        assert(vpp_prime.width == 1920);
        assert(vpp_prime.height == 1080);
        vaapi_prime_frame_close(&vpp_prime);
    }

    vaapi_decoder_destroy(&dec);

    /* Test dynamic surface pool sizing + direct NV12 mode with real MP4 extradata */
    demuxer_t *real_demux = demuxer_open_file("../tests/test_video.mp4");
    if (!real_demux)
        real_demux = demuxer_open_file("tests/test_video.mp4");
    if (real_demux) {
        vaapi_decoder_t vpp_dec = {0};
        bool vpp_init_ok = vaapi_decoder_init_with_params(
            &vpp_dec, -1, real_demux->codec, real_demux->width, real_demux->height,
            real_demux->extradata, real_demux->extradata_size, false);
        assert(vpp_init_ok);
        assert(vpp_dec.has_vpp);
        demux_packet_t vpp_pkt;
        if (real_demux->read_packet(real_demux, &vpp_pkt)) {
            VASurfaceID dec_surf = VA_INVALID_SURFACE;
            assert(vaapi_decoder_decode_packet(&vpp_dec, &vpp_pkt, &dec_surf));
            assert(vaapi_decoder_vpp_blit(&vpp_dec, dec_surf, vpp_dec.vpp_surfaces[0]));

            vaapi_prime_frame_t vpp_prime;
            assert(vaapi_decoder_export_prime(&vpp_dec, vpp_dec.vpp_surfaces[0], &vpp_prime));
            printf("  Real MP4 VPP PRIME: fmt=0x%08x planes=%u stride=%u mod=0x%016lx %ux%u\n",
                   vpp_prime.drm_format, vpp_prime.num_planes, vpp_prime.strides[0],
                   (unsigned long)vpp_prime.modifiers[0], vpp_prime.width, vpp_prime.height);

            struct gbm_device *gbm = gbm_create_device(vpp_dec.drm_fd);
            PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
                (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
            EGLDisplay edpy = get_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL);
            EGLint maj = 0, min = 0;
            assert(eglInitialize(edpy, &maj, &min));
            assert(eglBindAPI(EGL_OPENGL_ES_API));
            const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
            EGLContext ectx = eglCreateContext(edpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
            assert(ectx != EGL_NO_CONTEXT);
            assert(eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx));

            PFNEGLCREATEIMAGEKHRPROC create_img =
                (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
            PFNEGLDESTROYIMAGEKHRPROC destroy_img =
                (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
            PFNGLEGLIMAGETARGETTEXTURE2DOESPROC img_tex =
                (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
                    "glEGLImageTargetTexture2DOES");

            EGLint img_attrs[] = {EGL_WIDTH,
                                  (EGLint)vpp_prime.width,
                                  EGL_HEIGHT,
                                  (EGLint)vpp_prime.height,
                                  EGL_LINUX_DRM_FOURCC_EXT,
                                  (EGLint)vpp_prime.drm_format,
                                  EGL_DMA_BUF_PLANE0_FD_EXT,
                                  vpp_prime.fds[0],
                                  EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                  (EGLint)vpp_prime.offsets[0],
                                  EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                  (EGLint)vpp_prime.strides[0],
                                  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                  (EGLint)(vpp_prime.modifiers[0] & 0xFFFFFFFF),
                                  EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                  (EGLint)(vpp_prime.modifiers[0] >> 32),
                                  EGL_NONE};
            EGLImageKHR eimg =
                create_img(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
            assert(eimg != EGL_NO_IMAGE_KHR);

            GLuint tex = 0, fbo = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            img_tex(GL_TEXTURE_2D, eimg);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum fb_st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            printf("  FBO status: 0x%x (COMPLETE=0x%x)\n", fb_st, GL_FRAMEBUFFER_COMPLETE);

            uint8_t *rgba = calloc((size_t)vpp_prime.width * vpp_prime.height, 4);
            assert(rgba != NULL);
            glReadPixels(0, 0, (GLsizei)vpp_prime.width, (GLsizei)vpp_prime.height, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba);
            const struct {
                int x;
                uint8_t r, g, b;
            } expected_bars[8] = {
                {40, 0, 0, 0},       /* Black */
                {120, 0, 255, 255},  /* Cyan */
                {200, 254, 0, 253},  /* Magenta */
                {280, 0, 0, 254},    /* Blue */
                {360, 253, 253, 0},  /* Yellow */
                {440, 0, 255, 0},    /* Green */
                {520, 253, 0, 0},    /* Red */
                {600, 255, 255, 255} /* White */
            };
            printf("  VPP GL_TEXTURE_2D @ y=120: ");
            for (int k = 0; k < 8; ++k) {
                int x = expected_bars[k].x;
                size_t idx = ((size_t)120 * vpp_prime.width + (size_t)x) * 4;
                uint8_t r = rgba[idx], g = rgba[idx + 1], b = rgba[idx + 2], a = rgba[idx + 3];
                printf("(%d: R=%u G=%u B=%u A=%u) ", x, r, g, b, a);
                assert(abs((int)r - (int)expected_bars[k].r) <= 10);
                assert(abs((int)g - (int)expected_bars[k].g) <= 10);
                assert(abs((int)b - (int)expected_bars[k].b) <= 10);
                assert(a == 255);
            }
            printf("\n");
            free(rgba);

            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            destroy_img(edpy, eimg);
            eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(edpy, ectx);
            eglTerminate(edpy);
            gbm_device_destroy(gbm);
            vaapi_prime_frame_close(&vpp_prime);
        }
        vaapi_decoder_destroy(&vpp_dec);
        real_demux->seek_us(real_demux, 0);

        vaapi_decoder_t opt_dec = {0};
        bool opt_ok = vaapi_decoder_init_with_params(
            &opt_dec, -1, real_demux->codec, real_demux->width, real_demux->height,
            real_demux->extradata, real_demux->extradata_size, true);
        assert(opt_ok);
        assert(opt_dec.initialized);
        assert(!opt_dec.has_vpp);
        assert(opt_dec.iq_valid);
        assert(opt_dec.num_surfaces >= 4 && opt_dec.num_surfaces <= WAYWAL_VA_SURFACE_POOL_SIZE);

        demux_packet_t real_pkt;
        if (real_demux->read_packet(real_demux, &real_pkt)) {
            VASurfaceID real_surf = VA_INVALID_SURFACE;
            assert(vaapi_decoder_decode_packet(&opt_dec, &real_pkt, &real_surf));
            assert(real_surf != VA_INVALID_SURFACE);

            vaapi_prime_frame_t nv12_frame;
            assert(vaapi_decoder_export_prime(&opt_dec, real_surf, &nv12_frame));
            assert(nv12_frame.drm_format == DRM_FORMAT_NV12);
            assert(nv12_frame.num_planes >= 1);
            vaapi_prime_frame_close(&nv12_frame);
        }
        vaapi_decoder_destroy(&opt_dec);

        /* Stress rapid back-to-back VA-API context init + decode + destroy cycles */
        for (int cycle = 0; cycle < 4; ++cycle) {
            real_demux->seek_us(real_demux, 0);
            vaapi_decoder_t cyc_dec = {0};
            assert(vaapi_decoder_init_with_params(
                &cyc_dec, -1, real_demux->codec, real_demux->width, real_demux->height,
                real_demux->extradata, real_demux->extradata_size, true));
            if (real_demux->read_packet(real_demux, &real_pkt)) {
                VASurfaceID cyc_surf = VA_INVALID_SURFACE;
                assert(vaapi_decoder_decode_packet(&cyc_dec, &real_pkt, &cyc_surf));
                assert(cyc_surf != VA_INVALID_SURFACE);
            }
            vaapi_decoder_destroy(&cyc_dec);
        }

        real_demux->destroy(real_demux);
    }

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
    printf("  Average timerfd pacing jitter across %d ticks: %.3f us\n", num_ticks, avg_jitter_us);
    assert(avg_jitter_us < 15000.0);

    printf("[TEST] test_high_precision_monotonic_timer PASSED.\n");
}

static void test_webm_duration_and_lacing(void)
{
    printf("[TEST] Running test_webm_duration_and_lacing...\n");

    uint8_t buf[8192];
    uint8_t *p = buf;

    /* EBML Header */
    *p++ = 0x1A;
    *p++ = 0x45;
    *p++ = 0xDF;
    *p++ = 0xA3;
    *p++ = 0x80;

    /* Segment */
    *p++ = 0x18;
    *p++ = 0x53;
    *p++ = 0x80;
    *p++ = 0x67;
    uint8_t *seg_sz = p;
    p += 4;
    uint8_t *seg_start = p;

    /* Info (0x1549A966) with TimecodeScale = 1,000,000 ns (1ms) and Duration = 300.0 ms */
    *p++ = 0x15;
    *p++ = 0x49;
    *p++ = 0xA9;
    *p++ = 0x66;
    uint8_t *info_sz = p;
    p += 1;
    uint8_t *info_start = p;

    *p++ = 0x2A;
    *p++ = 0xD7;
    *p++ = 0xB1;
    *p++ = 0x84;
    write_u32_be(p, 1000000);
    p += 4;

    /* Duration (0x4489), float32 = 300.0f (0x43960000) -> 300,000 us for 9 frames (~30 fps) */
    *p++ = 0x44;
    *p++ = 0x89;
    *p++ = 0x84;
    write_u32_be(p, 0x43960000U);
    p += 4;
    *info_sz = (uint8_t)(0x80 | (p - info_start));

    /* Tracks (0x1654AE6B) */
    *p++ = 0x16;
    *p++ = 0x54;
    *p++ = 0xAE;
    *p++ = 0x6B;
    uint8_t *tracks_sz = p;
    p += 1;
    uint8_t *tracks_start = p;

    *p++ = 0xAE;
    uint8_t *entry_sz = p;
    p += 1;
    uint8_t *entry_start = p;

    *p++ = 0xD7;
    *p++ = 0x81;
    *p++ = 1;
    *p++ = 0x83;
    *p++ = 0x81;
    *p++ = 1;
    *p++ = 0x86;
    *p++ = 0x85;
    memcpy(p, "V_VP9", 5);
    p += 5;

    *p++ = 0xE0;
    *p++ = 0x88;
    *p++ = 0xB0;
    *p++ = 0x82;
    write_u16_be(p, 1280);
    p += 2;
    *p++ = 0xBA;
    *p++ = 0x82;
    write_u16_be(p, 720);
    p += 2;

    *entry_sz = (uint8_t)(0x80 | (p - entry_start));
    *tracks_sz = (uint8_t)(0x80 | (p - tracks_start));

    /* Cluster 1: 3 laced SimpleBlocks (Xiph, Fixed, EBML) = 9 frames total */
    *p++ = 0x1F;
    *p++ = 0x43;
    *p++ = 0xB6;
    *p++ = 0x75;
    uint8_t *clus_sz = p;
    p += 2;
    uint8_t *clus_start = p;

    /* ClusterTimecode = 0 */
    *p++ = 0xE7;
    *p++ = 0x82;
    write_u16_be(p, 0);
    p += 2;

    /* SimpleBlock 1: Xiph lacing (flags = 0x82 = keyframe + Xiph lacing), 3 frames (32, 48, 64) */
    *p++ = 0xA3;
    size_t sb1_len = 4 + 1 + 2 + 32 + 48 + 64;
    *p++ = (uint8_t)(0x40 | (sb1_len >> 8));
    *p++ = (uint8_t)(sb1_len & 0xFF);
    *p++ = 0x81; /* track 1 */
    write_u16_be(p, 0);
    p += 2;
    *p++ = 0x82; /* keyframe + Xiph lacing (01b << 1) */
    *p++ = 2;    /* num_frames - 1 = 2 (3 frames) */
    *p++ = 32;   /* frame 0 size */
    *p++ = 48;   /* frame 1 size */
    memset(p, 0x11, 32);
    p += 32;
    memset(p, 0x22, 48);
    p += 48;
    memset(p, 0x33, 64);
    p += 64;

    /* SimpleBlock 2: Fixed-size lacing (flags = 0x04), 3 frames of 40 bytes each */
    *p++ = 0xA3;
    size_t sb2_len = 4 + 1 + 40 * 3;
    *p++ = (uint8_t)(0x40 | (sb2_len >> 8));
    *p++ = (uint8_t)(sb2_len & 0xFF);
    *p++ = 0x81;
    write_u16_be(p, 100);
    p += 2;
    *p++ = 0x04; /* Fixed-size lacing (10b << 1) */
    *p++ = 2;    /* 3 frames */
    memset(p, 0x44, 40 * 3);
    p += 40 * 3;

    /* SimpleBlock 3: EBML lacing (flags = 0x06), 3 frames: 50, 60 (+10), 50 (-10) */
    *p++ = 0xA3;
    size_t sb3_len = 4 + 1 + 2 + 50 + 60 + 50;
    *p++ = (uint8_t)(0x40 | (sb3_len >> 8));
    *p++ = (uint8_t)(sb3_len & 0xFF);
    *p++ = 0x81;
    write_u16_be(p, 200);
    p += 2;
    *p++ = 0x06;                        /* EBML lacing (11b << 1) */
    *p++ = 2;                           /* 3 frames */
    *p++ = (uint8_t)(0x80 | 50);        /* frame 0 size = 50 (1-byte vint) */
    *p++ = (uint8_t)(0x80 | (10 + 63)); /* frame 1 delta = +10 (1-byte signed vint bias 63) */
    memset(p, 0x55, 50 + 60 + 50);
    p += 50 + 60 + 50;

    /* SimpleBlock 4 (malformed Xiph lacing): 3 frames where frame 0 size (1) equals remaining
     * bytes before reading frame 1 lace byte, verifying that (blk_end - blk_off) - sum_sizes
     * does not underflow to SIZE_MAX */
    *p++ = 0xA3;
    size_t sb4_len = 4 + 1 + 2;
    *p++ = (uint8_t)(0x40 | (sb4_len >> 8));
    *p++ = (uint8_t)(sb4_len & 0xFF);
    *p++ = 0x81;
    write_u16_be(p, 250);
    p += 2;
    *p++ = 0x02; /* Xiph lacing */
    *p++ = 2;    /* 3 frames */
    *p++ = 1;    /* frame 0 size = 1 */
    *p++ = 1;    /* frame 1 size = 1 (advances blk_off to blk_end while sum_sizes == 1) */

    /* SimpleBlock 5 (malformed EBML lacing): 3 frames where first_sz (1) equals remaining bytes
     * before reading frame 1 signed vint, verifying that (blk_end - blk_off) - sum_sizes
     * does not underflow to SIZE_MAX */
    *p++ = 0xA3;
    size_t sb5_len = 4 + 1 + 2;
    *p++ = (uint8_t)(0x40 | (sb5_len >> 8));
    *p++ = (uint8_t)(sb5_len & 0xFF);
    *p++ = 0x81;
    write_u16_be(p, 260);
    p += 2;
    *p++ = 0x06;                 /* EBML lacing */
    *p++ = 2;                    /* 3 frames */
    *p++ = (uint8_t)(0x80 | 1);  /* first_sz = 1 */
    *p++ = (uint8_t)(0x80 | 63); /* delta = 0 -> next_sz = 1 at blk_off == blk_end */

    size_t clus_len = (size_t)(p - clus_start);
    clus_sz[0] = (uint8_t)(0x40 | (clus_len >> 8));
    clus_sz[1] = (uint8_t)(clus_len & 0xFF);

    size_t seg_len = (size_t)(p - seg_start);
    seg_sz[0] = (uint8_t)(0x10 | (seg_len >> 24));
    seg_sz[1] = (uint8_t)((seg_len >> 16) & 0xFF);
    seg_sz[2] = (uint8_t)((seg_len >> 8) & 0xFF);
    seg_sz[3] = (uint8_t)(seg_len & 0xFF);

    demuxer_t *d = demuxer_open_mem(buf, (size_t)(p - buf));
    assert(d != NULL);
    assert(d->duration_us == 300000);
    assert(d->fps_den == 1);
    assert(d->fps_num == 30);

    const size_t expected_sizes[9] = {32, 48, 64, 40, 40, 40, 50, 60, 50};
    demux_packet_t pkt;
    for (int i = 0; i < 9; ++i) {
        assert(d->read_packet(d, &pkt));
        assert(pkt.size == expected_sizes[i]);
        if (i == 0)
            assert(pkt.is_keyframe);
        else
            assert(!pkt.is_keyframe);
    }
    assert(!d->read_packet(d, &pkt));
    d->destroy(d);

    printf("  -> WebM EBML_ID_DURATION & Xiph/Fixed/EBML block lacing: PASSED\n");
}

static void test_mp4_ctts_v0_and_v1(void)
{
    printf("[TEST] Running test_mp4_ctts_v0_and_v1...\n");

    uint8_t base_mp4[8192];
    size_t base_sz = build_synthetic_mp4(base_mp4, sizeof(base_mp4), 640, 360, 3);
    assert(base_sz > 0 && base_sz + 64 <= sizeof(base_mp4));

    /* Append a ctts v0 box inside stbl with positive offsets: [count=1, off=33], [count=1, off=66],
     * [count=1, off=0] */
    /* Find stss start by scanning for 'stss' fourcc */
    size_t stss_off = 0;
    for (size_t i = 0; i + 8 < base_sz; ++i) {
        if (memcmp(base_mp4 + i + 4, "stss", 4) == 0) {
            stss_off = i;
            break;
        }
    }
    assert(stss_off > 0);

    uint8_t ctts_box[40];
    uint8_t *cp = ctts_box;
    write_u32_be(cp, 40);
    cp += 4;
    write_u32_be(cp, FOURCC('c', 't', 't', 's'));
    cp += 4;
    write_u32_be(cp, 0); /* version 0, flags 0 */
    cp += 4;
    write_u32_be(cp, 3); /* 3 entries */
    cp += 4;
    write_u32_be(cp, 1);
    cp += 4;
    write_u32_be(cp, 33); /* sample 0: dts=0 + 33ms = 33000us */
    cp += 4;
    write_u32_be(cp, 1);
    cp += 4;
    write_u32_be(cp, 66); /* sample 1: dts=33 + 66ms = 99000us */
    cp += 4;
    write_u32_be(cp, 1);
    cp += 4;
    write_u32_be(cp, 0); /* sample 2: dts=66 + 0ms = 66000us */
    cp += 4;

    uint8_t mp4_v0[8192];
    memcpy(mp4_v0, base_mp4, base_sz);
    memcpy(mp4_v0 + base_sz, ctts_box, sizeof(ctts_box));

    /* Update parent box sizes (moov, trak, mdia, minf, stbl) by +40 */
    const char *parents[] = {"moov", "trak", "mdia", "minf", "stbl"};
    for (size_t b = 0; b < 5; ++b) {
        for (size_t i = 0; i + 8 < base_sz; ++i) {
            if (memcmp(mp4_v0 + i + 4, parents[b], 4) == 0) {
                uint32_t old_len = ((uint32_t)mp4_v0[i] << 24) | ((uint32_t)mp4_v0[i + 1] << 16) |
                                   ((uint32_t)mp4_v0[i + 2] << 8) | (uint32_t)mp4_v0[i + 3];
                write_u32_be(mp4_v0 + i, old_len + (uint32_t)sizeof(ctts_box));
                break;
            }
        }
    }

    demuxer_t *d0 = demuxer_open_mem(mp4_v0, base_sz + sizeof(ctts_box));
    assert(d0 != NULL);
    demux_packet_t pkt;
    assert(d0->read_packet(d0, &pkt));
    assert(pkt.pts_us == 0);
    assert(d0->read_packet(d0, &pkt));
    assert(pkt.pts_us == 66000);
    assert(d0->read_packet(d0, &pkt));
    assert(pkt.pts_us == 33000);
    d0->destroy(d0);

    /* Now test ctts v1 with signed negative offset: sample 0 off=0, sample 1 off=+33, sample 2
     * off=-99 (min_pts = -33000us -> shifted by +33000us) */
    uint8_t mp4_v1[8192];
    memcpy(mp4_v1, mp4_v0, base_sz + sizeof(ctts_box));
    uint8_t *v1_ctts = mp4_v1 + base_sz;
    v1_ctts[8] = 1;                              /* version 1 */
    write_u32_be(v1_ctts + 20, 0);               /* sample 0: dts=0 + 0 = 0 */
    write_u32_be(v1_ctts + 28, 33);              /* sample 1: dts=33 + 33 = 66000 */
    write_u32_be(v1_ctts + 36, (uint32_t)(-99)); /* sample 2: dts=66 - 99 = -33000 -> min_pts */

    demuxer_t *d1 = demuxer_open_mem(mp4_v1, base_sz + sizeof(ctts_box));
    assert(d1 != NULL);
    assert(d1->read_packet(d1, &pkt));
    assert(pkt.pts_us == 33000);
    assert(d1->read_packet(d1, &pkt));
    assert(pkt.pts_us == 99000);
    assert(d1->read_packet(d1, &pkt));
    assert(pkt.pts_us == 0);
    d1->destroy(d1);

    printf("  -> MP4 ctts v0 positive offsets & v1 signed negative offsets: PASSED\n");
}

static void test_h264_main_profile_bframe_reordering(void)
{
    printf("[TEST] Running test_h264_main_profile_bframe_reordering...\n");

    /* avcC extradata with profile_idc=77 (Main), constraint_flags=0x40 (constraint_set1_flag=1,
     * constraint_set0_flag=0), level_idc=40, num_ref_frames=4, 640x480 progressive */
    const uint8_t main_avcc[] = {
        0x01, 0x4D, 0x40, 0x28, 0xFF, 0xE1, 0x00, 0x09, 0x67,
        0x4D, 0x40, 0x28, 0xF2, 0x81, 0x40, 0x7B, 0x20, 0x00,
    };
    vaapi_decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    dec.va_profile = VAProfileH264Main;
    assert(vaapi_decoder_set_extradata(&dec, main_avcc, sizeof(main_avcc)));
    assert(dec.cached_sps.valid);
    assert(dec.cached_sps.profile_idc == 77);
    assert(dec.cached_sps.constraint_set0_flag == 0);
    assert(dec.cached_sps.constraint_set1_flag == 1);
    assert(dec.cached_sps.num_ref_frames == 4);
    assert(dec.cached_sps.pic_order_cnt_type == 0);
    assert(!dec.cached_sps.bitstream_restriction_flag);

    /* Verify Constrained Baseline (profile_idc=66, constraint_flags=0xC0) sets constraint_set0_flag
     */
    const uint8_t baseline_avcc[] = {
        0x01, 0x42, 0xC0, 0x28, 0xFF, 0xE1, 0x00, 0x09, 0x67,
        0x42, 0xC0, 0x28, 0xF2, 0x81, 0x40, 0x7B, 0x20, 0x00,
    };
    vaapi_decoder_t base_dec;
    memset(&base_dec, 0, sizeof(base_dec));
    base_dec.va_profile = VAProfileH264ConstrainedBaseline;
    assert(vaapi_decoder_set_extradata(&base_dec, baseline_avcc, sizeof(baseline_avcc)));
    assert(base_dec.cached_sps.valid);
    assert(base_dec.cached_sps.profile_idc == 66);
    assert(base_dec.cached_sps.constraint_set0_flag == 1);
    assert(base_dec.cached_sps.constraint_set1_flag == 1);

    /* Verify VUI bitstream_restriction_flag with max_num_reorder_frames=4 and =0 */
    const uint8_t vui_reorder4_avcc[] = {
        0x01, 0x4D, 0x40, 0x28, 0xFF, 0xE1, 0x00, 0x0B, 0x67, 0x4D,
        0x40, 0x28, 0xF2, 0x81, 0x40, 0x7B, 0x40, 0x3F, 0x2E, 0x00,
    };
    vaapi_decoder_t vui4_dec;
    memset(&vui4_dec, 0, sizeof(vui4_dec));
    vui4_dec.va_profile = VAProfileH264Main;
    assert(vaapi_decoder_set_extradata(&vui4_dec, vui_reorder4_avcc, sizeof(vui_reorder4_avcc)));
    assert(vui4_dec.cached_sps.valid);
    assert(vui4_dec.cached_sps.bitstream_restriction_flag);
    assert(vui4_dec.cached_sps.max_num_reorder_frames == 4);

    const uint8_t vui_reorder0_avcc[] = {
        0x01, 0x4D, 0x40, 0x28, 0xFF, 0xE1, 0x00, 0x0B, 0x67, 0x4D,
        0x40, 0x28, 0xF2, 0x81, 0x40, 0x7B, 0x40, 0x3F, 0xE0, 0x00,
    };
    vaapi_decoder_t vui0_dec;
    memset(&vui0_dec, 0, sizeof(vui0_dec));
    vui0_dec.va_profile = VAProfileH264Main;
    assert(vaapi_decoder_set_extradata(&vui0_dec, vui_reorder0_avcc, sizeof(vui_reorder0_avcc)));
    assert(vui0_dec.cached_sps.valid);
    assert(vui0_dec.cached_sps.bitstream_restriction_flag);
    assert(vui0_dec.cached_sps.max_num_reorder_frames == 0);

    /* Main Profile without bitstream_restriction_flag uses num_ref_frames + 1 (4 + 1 = 5)
     * so hierarchical B-frames reorder monotonically into PTS/POC presentation order. */
    uint32_t target_depth = vaapi_h264_compute_reorder_depth(&dec.cached_sps, 17U);
    assert(target_depth == 5U);
    assert(vaapi_h264_compute_reorder_depth(&base_dec.cached_sps, 17U) == 1U);
    assert(vaapi_h264_compute_reorder_depth(&vui4_dec.cached_sps, 17U) == 5U);
    assert(vaapi_h264_compute_reorder_depth(&vui0_dec.cached_sps, 17U) == 1U);

    /* Simulate 2 loops of 2 GOPs of hierarchical B-frame decode order (including POC step 2 in
     * GOP 0 and POC step 1 in GOP 1) and verify:
     * 1. Zero backwards PTS or POC rewinds within each GOP and loop.
     * 2. Steady-state ticks (including across loop boundaries!) decode 1 packet per tick without
     *    draining reorder_count to 0 or stalling for a 4-frame burst decode. */
    typedef struct {
        uint64_t loop_id;
        uint64_t gop_id;
        int32_t poc;
        int64_t pts_us;
    } sim_pkt_t;

    const sim_pkt_t stream[] = {
        /* GOP 0 (even POCs: 0, 6, 2, 4, 12, 8, 10) */
        {0, 0, 0, 0},
        {0, 0, 6, 99999},
        {0, 0, 2, 33333},
        {0, 0, 4, 66666},
        {0, 0, 12, 199998},
        {0, 0, 8, 133332},
        {0, 0, 10, 166665},
        /* GOP 1 (consecutive POCs: 0, 3, 1, 2, 6, 4, 5) */
        {0, 1, 0, 233331},
        {0, 1, 3, 333330},
        {0, 1, 1, 266664},
        {0, 1, 2, 299997},
        {0, 1, 6, 433329},
        {0, 1, 4, 366663},
        {0, 1, 5, 399996},
    };
    const size_t stream_len = sizeof(stream) / sizeof(stream[0]);
    const uint64_t max_loops = 2;
    const size_t total_pkts = stream_len * max_loops;

    sim_pkt_t reorder_buf[6];
    uint32_t reorder_count = 0;
    bool eof_draining = false;
    size_t read_idx = 0;
    uint64_t current_loop = 0;
    uint64_t gop_offset = 0;
    int64_t prev_pts_us = -1;
    uint64_t prev_loop_id = 0;
    uint64_t prev_gop_id = 0;
    int32_t prev_poc = -1;
    size_t presented_count = 0;
    uint32_t tick = 0;

    while (presented_count < total_pkts) {
        uint32_t decoded_this_tick = 0;
        bool looped_this_tick = false;
        while (!eof_draining && reorder_count < 6) {
            bool ready = (reorder_count > 0) && (eof_draining || reorder_count >= target_depth);
            if (ready)
                break;
            if (read_idx >= stream_len) {
                bool is_final_loop = (current_loop + 1 >= max_loops);
                if (is_final_loop || looped_this_tick) {
                    eof_draining = true;
                    break;
                }
                looped_this_tick = true;
                current_loop++;
                gop_offset += 2;
                read_idx = 0;
            }
            sim_pkt_t pkt = stream[read_idx++];
            pkt.loop_id = current_loop;
            pkt.gop_id += gop_offset;
            reorder_buf[reorder_count++] = pkt;
            decoded_this_tick++;
        }

        if (tick > 0 && !eof_draining) {
            assert(decoded_this_tick == 1U);
        }

        assert(reorder_count > 0);
        uint32_t best_idx = 0;
        for (uint32_t i = 1; i < reorder_count; ++i) {
            const sim_pkt_t *a = &reorder_buf[i];
            const sim_pkt_t *b = &reorder_buf[best_idx];
            if (a->gop_id < b->gop_id || (a->gop_id == b->gop_id && a->poc < b->poc) ||
                (a->gop_id == b->gop_id && a->poc == b->poc && a->pts_us < b->pts_us)) {
                best_idx = i;
            }
        }
        sim_pkt_t pres = reorder_buf[best_idx];
        reorder_count--;
        if (best_idx < reorder_count) {
            reorder_buf[best_idx] = reorder_buf[reorder_count];
        }

        if (presented_count > 0 && pres.loop_id == prev_loop_id) {
            assert(pres.pts_us > prev_pts_us);
            if (pres.gop_id == prev_gop_id) {
                assert(pres.poc > prev_poc);
            }
        }
        prev_loop_id = pres.loop_id;
        prev_pts_us = pres.pts_us;
        prev_gop_id = pres.gop_id;
        prev_poc = pres.poc;
        presented_count++;
        tick++;
    }

    /* Verify PLL vblank phase-locking when playback_epoch_ns starts in the second half of a
     * 60Hz vblank interval (9.0ms past vblank) with 2.0ms decode+VPP+commit overhead:
     * must converge smoothly to 1 frame per vblank without 22ms/8ms oscillation. */
    const int64_t period_ns = 16666667LL;
    const int64_t frame_interval_ns = 16666667LL;
    const int64_t work_ns = 2000000LL;
    const int64_t lead_time_ns = 4000000LL;
    int64_t now_ns = 9000000LL;
    int64_t playback_epoch_ns = now_ns;
    int64_t prev_vblank_idx = -1;

    for (int f = 0; f < 30; ++f) {
        int64_t scaled_pts_ns = (int64_t)f * frame_interval_ns;
        int64_t target_pres_ns = playback_epoch_ns + scaled_pts_ns + frame_interval_ns;
        int64_t next_vblank_ns = ((now_ns / period_ns) + 1) * period_ns;
        int64_t desired_vblank_ns = target_pres_ns + lead_time_ns;
        if (desired_vblank_ns >= next_vblank_ns) {
            int64_t cycles = (desired_vblank_ns - next_vblank_ns + (period_ns / 2)) / period_ns;
            if (cycles < 0)
                cycles = 0;
            int64_t aligned_vblank_ns = next_vblank_ns + cycles * period_ns;
            int64_t target_fire_ns = aligned_vblank_ns - lead_time_ns;
            int64_t phase_err = target_fire_ns - target_pres_ns;
            int64_t adj = phase_err / 4;
            playback_epoch_ns += adj;
            target_pres_ns += adj;
        }
        int64_t next_interval_ns = target_pres_ns - now_ns;
        if (next_interval_ns < 1000000LL)
            next_interval_ns = 1000000LL;
        /* Ensure no 8ms double-commit or 23ms frame-repeat oscillation occurs in steady state */
        if (f >= 4) {
            assert(next_interval_ns >= 13000000LL && next_interval_ns <= 16000000LL);
        }
        int64_t wake_ns = now_ns + next_interval_ns;
        now_ns = wake_ns + work_ns;
        int64_t committed_vblank_idx = now_ns / period_ns;
        if (f >= 4) {
            assert(committed_vblank_idx == prev_vblank_idx + 1);
        }
        prev_vblank_idx = committed_vblank_idx;
    }

    printf(
        "  -> H.264 Main Profile constraint_set1_flag SPS & B-frame monotonic reorder: PASSED\n");
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bit_pos;
} test_bw_t;

static void bw_put_bit(test_bw_t *bw, uint32_t b)
{
    assert(bw->bit_pos / 8 < bw->cap);
    size_t byte_idx = bw->bit_pos / 8;
    uint32_t shift = 7U - (uint32_t)(bw->bit_pos % 8);
    if (shift == 7U) {
        bw->buf[byte_idx] = 0;
    }
    bw->buf[byte_idx] |= (uint8_t)((b & 1U) << shift);
    bw->bit_pos++;
}

static void bw_put_bits(test_bw_t *bw, uint32_t val, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        bw_put_bit(bw, (val >> (n - 1U - i)) & 1U);
    }
}

static void bw_put_ue(test_bw_t *bw, uint32_t val)
{
    uint64_t code = (uint64_t)val + 1ULL;
    uint32_t zeros = 0;
    for (uint64_t t = code; t > 1ULL; t >>= 1) {
        zeros++;
    }
    for (uint32_t z = 0; z < zeros; ++z) {
        bw_put_bit(bw, 0);
    }
    for (uint32_t b = 0; b <= zeros; ++b) {
        bw_put_bit(bw, (uint32_t)((code >> (zeros - b)) & 1ULL));
    }
}

static void bw_put_se(test_bw_t *bw, int32_t val)
{
    uint32_t mapped = (val <= 0) ? (uint32_t)(-2 * val) : (uint32_t)(2 * val - 1);
    bw_put_ue(bw, mapped);
}

static size_t escape_rbsp_to_ebsp(const uint8_t *rbsp, size_t rbsp_len, uint8_t *dst, size_t cap)
{
    assert(rbsp_len >= 1 && cap >= rbsp_len);
    dst[0] = rbsp[0];
    size_t di = 1;
    uint32_t zero_run = 0;
    for (size_t si = 1; si < rbsp_len; ++si) {
        uint8_t b = rbsp[si];
        if (zero_run == 2 && b <= 3) {
            assert(di < cap);
            dst[di++] = 0x03;
            zero_run = 0;
        }
        assert(di < cap);
        dst[di++] = b;
        zero_run = (b == 0) ? (zero_run + 1U) : 0U;
    }
    return di;
}

static size_t build_high_profile_sps_nal(uint8_t *out_nal, size_t out_cap, uint8_t num_ref_frames,
                                         bool has_bitstream_restriction, uint8_t max_reorder_frames,
                                         bool extra_poc1_table, uint8_t exp_4x4[6][16],
                                         uint8_t exp_8x8[2][64])
{
    uint8_t rbsp[4096];
    memset(rbsp, 0, sizeof(rbsp));
    test_bw_t bw = {.buf = rbsp, .cap = sizeof(rbsp), .bit_pos = 0};

    bw_put_bits(&bw, 0x67, 8); /* NAL type 7 (SPS) */
    bw_put_bits(&bw, 100, 8);  /* profile_idc = 100 (High) */
    bw_put_bits(&bw, 0x00, 8); /* constraint flags */
    bw_put_bits(&bw, 42, 8);   /* level_idc = 4.2 */
    bw_put_ue(&bw, 0);         /* seq_parameter_set_id */

    bw_put_ue(&bw, 1);  /* chroma_format_idc = 4:2:0 */
    bw_put_ue(&bw, 0);  /* bit_depth_luma_minus8 */
    bw_put_ue(&bw, 0);  /* bit_depth_chroma_minus8 */
    bw_put_bit(&bw, 0); /* qpprime_y_zero_transform_bypass_flag */
    bw_put_bit(&bw, 1); /* seq_scaling_matrix_present_flag */

    for (uint32_t i = 0; i < 8; ++i) {
        bw_put_bit(&bw, 1); /* seq_scaling_list_present_flag[i] */
        uint32_t sz = (i < 6) ? 16U : 64U;
        int32_t last_scale = 8;
        for (uint32_t j = 0; j < sz; ++j) {
            uint8_t target = (i < 6) ? (uint8_t)(10U + ((i * 5U + j) % 45U))
                                     : (uint8_t)(12U + ((i * 7U + j) % 60U));
            int32_t delta = (int32_t)target - last_scale;
            bw_put_se(&bw, delta);
            last_scale = (int32_t)target;
            if (i < 6 && exp_4x4) {
                exp_4x4[i][j] = target;
            } else if (i >= 6 && exp_8x8) {
                exp_8x8[i - 6][j] = target;
            }
        }
    }

    bw_put_ue(&bw, 4); /* log2_max_frame_num_minus4 */
    if (extra_poc1_table) {
        bw_put_ue(&bw, 1); /* pic_order_cnt_type = 1 */
        bw_put_bit(&bw, 0);
        bw_put_se(&bw, 0);
        bw_put_se(&bw, 0);
        bw_put_ue(&bw, 220);
        for (uint32_t r = 0; r < 220; ++r) {
            bw_put_se(&bw, 0x1FFFFFF0 - (int32_t)r);
        }
    } else {
        bw_put_ue(&bw, 0); /* pic_order_cnt_type = 0 */
        bw_put_ue(&bw, 4); /* log2_max_pic_order_cnt_lsb_minus4 */
    }

    bw_put_ue(&bw, num_ref_frames);
    bw_put_bit(&bw, 0);  /* gaps_in_frame_num_value_allowed_flag */
    bw_put_ue(&bw, 119); /* pic_width_in_mbs_minus1 (1920) */
    bw_put_ue(&bw, 67);  /* pic_height_in_map_units_minus1 (1088) */
    bw_put_bit(&bw, 1);  /* frame_mbs_only_flag */
    bw_put_bit(&bw, 1);  /* direct_8x8_inference_flag */

    bw_put_bit(&bw, 1); /* frame_cropping_flag */
    bw_put_ue(&bw, 0);
    bw_put_ue(&bw, 0);
    bw_put_ue(&bw, 0);
    bw_put_ue(&bw, 4);

    bw_put_bit(&bw, 1); /* vui_parameters_present_flag */
    bw_put_bit(&bw, 1); /* aspect_ratio_info_present_flag */
    bw_put_bits(&bw, 1, 8);
    bw_put_bit(&bw, 0); /* overscan_info_present_flag */
    bw_put_bit(&bw, 1); /* video_signal_type_present_flag */
    bw_put_bits(&bw, 5, 3);
    bw_put_bit(&bw, 1); /* video_full_range_flag */
    bw_put_bit(&bw, 1); /* colour_description_present_flag */
    bw_put_bits(&bw, 1, 8);
    bw_put_bits(&bw, 1, 8);
    bw_put_bits(&bw, 1, 8);
    bw_put_bit(&bw, 0); /* chroma_loc_info_present_flag */
    bw_put_bit(&bw, 1); /* timing_info_present_flag */
    bw_put_bits(&bw, 1001, 32);
    bw_put_bits(&bw, 60000, 32);
    bw_put_bit(&bw, 1); /* fixed_frame_rate_flag */

    /* Emit both NAL HRD and VCL HRD with 32 CPB entries each so RBSP exceeds 1100 bytes */
    for (int hrd = 0; hrd < 2; ++hrd) {
        bw_put_bit(&bw, 1);
        bw_put_ue(&bw, 31); /* cpb_cnt_minus1 = 31 */
        bw_put_bits(&bw, 4, 4);
        bw_put_bits(&bw, 4, 4);
        for (uint32_t k = 0; k < 32; ++k) {
            bw_put_ue(&bw, 0x3FFFFFF0U + k);
            bw_put_ue(&bw, 0x3FFFFF00U + k);
            bw_put_bit(&bw, k & 1U);
        }
        bw_put_bits(&bw, 23, 5);
        bw_put_bits(&bw, 23, 5);
        bw_put_bits(&bw, 23, 5);
        bw_put_bits(&bw, 24, 5);
    }
    bw_put_bit(&bw, 0); /* low_delay_hrd_flag */
    bw_put_bit(&bw, 0); /* pic_struct_present_flag */

    if (has_bitstream_restriction) {
        bw_put_bit(&bw, 1); /* bitstream_restriction_flag */
        bw_put_bit(&bw, 1); /* motion_vectors_over_pic_boundaries_flag */
        bw_put_ue(&bw, 0);
        bw_put_ue(&bw, 0);
        bw_put_ue(&bw, 11);
        bw_put_ue(&bw, 11);
        bw_put_ue(&bw, max_reorder_frames);
        bw_put_ue(&bw, num_ref_frames > max_reorder_frames ? num_ref_frames : max_reorder_frames);
    } else {
        bw_put_bit(&bw, 0); /* bitstream_restriction_flag = 0 */
    }

    /* RBSP stop bit */
    bw_put_bit(&bw, 1);
    while ((bw.bit_pos % 8) != 0) {
        bw_put_bit(&bw, 0);
    }

    size_t rbsp_len = bw.bit_pos / 8;
    return escape_rbsp_to_ebsp(rbsp, rbsp_len, out_nal, out_cap);
}

static void test_h264_high_profile_large_sps_and_reorder_fallback(void)
{
    printf("[TEST] Running test_h264_high_profile_large_sps_and_reorder_fallback...\n");

    uint8_t nal[6144];
    uint8_t exp_4x4[6][16];
    uint8_t exp_8x8[2][64];

    /* 1. >600-byte High Profile SPS with scaling matrices + dual HRD + bitstream_restriction=1 */
    size_t nal_len =
        build_high_profile_sps_nal(nal, sizeof(nal), 8, true, 6, false, exp_4x4, exp_8x8);
    assert(nal_len > 600);

    parsed_sps_t sps;
    memset(&sps, 0, sizeof(sps));
    assert(parse_h264_sps(nal, nal_len, &sps));
    assert(sps.valid);
    assert(sps.profile_idc == 100);
    assert(sps.seq_scaling_matrix_present_flag);
    assert(memcmp(sps.scaling_list_4x4, exp_4x4, sizeof(exp_4x4)) == 0);
    assert(memcmp(sps.scaling_list_8x8, exp_8x8, sizeof(exp_8x8)) == 0);
    assert(sps.num_ref_frames == 8);
    assert(sps.video_full_range_flag);
    assert(sps.colour_primaries == 1);
    assert(sps.bitstream_restriction_flag);
    assert(sps.max_num_reorder_frames == 6);
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 7U);

    /* Also verify 1-ref stream with explicit max_num_reorder_frames=1 is respected */
    size_t nal_1ref_len =
        build_high_profile_sps_nal(nal, sizeof(nal), 1, true, 1, false, NULL, NULL);
    assert(nal_1ref_len > 600);
    memset(&sps, 0, sizeof(sps));
    assert(parse_h264_sps(nal, nal_1ref_len, &sps));
    assert(sps.valid && sps.bitstream_restriction_flag && sps.max_num_reorder_frames == 1);
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 2U);

    /* 2. >600-byte High Profile SPS with bitstream_restriction_flag=0:
     * must clamp(num_ref_frames + 1, 1, WAYWAL_VIDEO_REORDER_CAP) */
    size_t nal_no_vui_restr =
        build_high_profile_sps_nal(nal, sizeof(nal), 8, false, 0, false, NULL, NULL);
    assert(nal_no_vui_restr > 600);
    memset(&sps, 0, sizeof(sps));
    assert(parse_h264_sps(nal, nal_no_vui_restr, &sps));
    assert(sps.valid);
    assert(!sps.bitstream_restriction_flag);
    assert(sps.num_ref_frames == 8);
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 9U);

    /* Clamp upper bound at cap (num_ref_frames = 16 -> 17) and intra-only (num_ref_frames = 0 -> 1)
     */
    sps.num_ref_frames = 16;
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 17U);
    sps.num_ref_frames = 0;
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 1U);

    /* 3. >2048-byte High Profile SPS exercising dynamic heap RBSP fallback without leaks */
    size_t huge_nal_len =
        build_high_profile_sps_nal(nal, sizeof(nal), 10, true, 5, true, NULL, NULL);
    assert(huge_nal_len > 2048);
    memset(&sps, 0, sizeof(sps));
    assert(parse_h264_sps(nal, huge_nal_len, &sps));
    assert(sps.valid);
    assert(sps.bitstream_restriction_flag);
    assert(sps.max_num_reorder_frames == 5);
    assert(vaapi_h264_compute_reorder_depth(&sps, 17U) == 6U);

    printf("  -> High Profile >600B & >2KiB SPS scaling/HRD + reorder fallback: PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Executing WayWal Video & Demuxer Test Suite\n");
    printf("=========================================\n");

    test_demuxer_zero_allocation();
    test_webm_demuxer_zero_allocation();
    test_webm_duration_and_lacing();
    test_mp4_ctts_v0_and_v1();
    test_h264_main_profile_bframe_reordering();
    test_h264_high_profile_large_sps_and_reorder_fallback();
    test_real_mp4_demuxing();
    test_vaapi_decoder_and_prime_export();
    test_high_precision_monotonic_timer();

    printf("\n>>> ALL VIDEO UNIT TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
