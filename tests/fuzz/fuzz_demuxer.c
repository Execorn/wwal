// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuzz_harness.h"
#include "waywal/demuxer.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WWAL_FUZZ_VERBOSE
static void silence_stderr_once(void)
{
    static bool silenced = false;
    if (silenced)
        return;
    silenced = true;
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data)
        return 0;

#ifndef WWAL_FUZZ_VERBOSE
    silence_stderr_once();
#endif

    demuxer_t *d = demuxer_open_mem(data, size);
    if (!d)
        return 0;

    demux_packet_t pkt;
    uint32_t max_packets = 256;
    uint32_t count = 0;

    while (count < max_packets && d->read_packet(d, &pkt)) {
        if (pkt.data && pkt.size > 0 && pkt.data >= data && pkt.data + pkt.size <= data + size) {
            volatile uint8_t first = pkt.data[0];
            volatile uint8_t last = pkt.data[pkt.size - 1];
            (void)first;
            (void)last;
        }
        count++;
    }

    if (d->seek_us) {
        d->seek_us(d, 0);
        if (d->read_packet(d, &pkt) && pkt.data && pkt.size > 0 && pkt.data >= data &&
            pkt.data + pkt.size <= data + size) {
            volatile uint8_t b = pkt.data[0];
            (void)b;
        }
        d->seek_us(d, d->duration_us / 2);
        d->seek_us(d, -1000);
        d->seek_us(d, INT64_MAX);
    }

    if (d->extradata && d->extradata_size > 0 && d->extradata >= data &&
        d->extradata + d->extradata_size <= data + size) {
        volatile uint8_t ex0 = d->extradata[0];
        volatile uint8_t ex1 = d->extradata[d->extradata_size - 1];
        (void)ex0;
        (void)ex1;
    }

    d->destroy(d);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE

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

static size_t build_seed_mp4(uint8_t *buf, size_t max_size)
{
    if (max_size < 1024)
        return 0;
    uint8_t *p = buf;

    uint8_t *ftyp_start = p;
    p += 4;
    write_u32_be(p, FOURCC('f', 't', 'y', 'p'));
    p += 4;
    write_u32_be(p, FOURCC('i', 's', 'o', 'm'));
    p += 4;
    write_u32_be(p, 512);
    p += 4;
    write_u32_be(p, FOURCC('i', 's', 'o', 'm'));
    p += 4;
    write_u32_be(ftyp_start, (uint32_t)(p - ftyp_start));

    uint8_t *mdat_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'a', 't'));
    p += 4;
    size_t sample_offset = (size_t)(p - buf);
    memset(p, 0xAB, 64);
    p += 64;
    write_u32_be(mdat_start, (uint32_t)(p - mdat_start));

    uint8_t *moov_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'o', 'o', 'v'));
    p += 4;

    uint8_t *trak_start = p;
    p += 4;
    write_u32_be(p, FOURCC('t', 'r', 'a', 'k'));
    p += 4;

    uint8_t *mdia_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'i', 'a'));
    p += 4;

    uint8_t *mdhd_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'd', 'h', 'd'));
    p += 4;
    memset(p, 0, 12);
    p += 12;
    write_u32_be(p, 1000);
    p += 4;
    write_u32_be(p, 66);
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(mdhd_start, (uint32_t)(p - mdhd_start));

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

    uint8_t *minf_start = p;
    p += 4;
    write_u32_be(p, FOURCC('m', 'i', 'n', 'f'));
    p += 4;

    uint8_t *stbl_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 'b', 'l'));
    p += 4;

    uint8_t *stsd_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'd'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1);
    p += 4;

    uint8_t *avc1_start = p;
    p += 4;
    write_u32_be(p, FOURCC('a', 'v', 'c', '1'));
    p += 4;
    memset(p, 0, 24);
    p += 24;
    write_u16_be(p, 1920);
    p += 2;
    write_u16_be(p, 1080);
    p += 2;
    memset(p, 0, 50);
    p += 50;
    write_u32_be(avc1_start, (uint32_t)(p - avc1_start));
    write_u32_be(stsd_start, (uint32_t)(p - stsd_start));

    uint8_t *stsz_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'z'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 32);
    p += 4;
    write_u32_be(p, 2);
    p += 4;
    write_u32_be(stsz_start, (uint32_t)(p - stsz_start));

    uint8_t *stco_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 'c', 'o'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 2);
    p += 4;
    write_u32_be(p, (uint32_t)sample_offset);
    p += 4;
    write_u32_be(p, (uint32_t)(sample_offset + 32));
    p += 4;
    write_u32_be(stco_start, (uint32_t)(p - stco_start));

    uint8_t *stsc_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 's', 'c'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(stsc_start, (uint32_t)(p - stsc_start));

    uint8_t *stts_start = p;
    p += 4;
    write_u32_be(p, FOURCC('s', 't', 't', 's'));
    p += 4;
    write_u32_be(p, 0);
    p += 4;
    write_u32_be(p, 1);
    p += 4;
    write_u32_be(p, 2);
    p += 4;
    write_u32_be(p, 33);
    p += 4;
    write_u32_be(stts_start, (uint32_t)(p - stts_start));

    write_u32_be(stbl_start, (uint32_t)(p - stbl_start));
    write_u32_be(minf_start, (uint32_t)(p - minf_start));
    write_u32_be(mdia_start, (uint32_t)(p - mdia_start));
    write_u32_be(trak_start, (uint32_t)(p - trak_start));
    write_u32_be(moov_start, (uint32_t)(p - moov_start));

    return (size_t)(p - buf);
}

static size_t build_seed_webm(uint8_t *buf, size_t max_size)
{
    if (max_size < 256)
        return 0;
    static const uint8_t tpl[] = {
        0x1A, 0x45, 0xDF, 0xA3, 0x80, 0x18, 0x53, 0x80, 0x67, 0x10, 0x00, 0x00, 0x40, 0x15,
        0x49, 0xA9, 0x66, 0x87, 0x2A, 0xD7, 0xB1, 0x84, 0x00, 0x0F, 0x42, 0x40, 0x16, 0x54,
        0xAE, 0x6B, 0x40, 0x19, 0xAE, 0x97, 0xD7, 0x81, 0x01, 0x83, 0x81, 0x01, 0x86, 0x85,
        'V',  '_',  'V',  'P',  '9',  0xE0, 0x88, 0xB0, 0x82, 0x07, 0x80, 0xBA, 0x82, 0x04,
        0x38, 0x1F, 0x43, 0xB6, 0x75, 0x40, 0x14, 0xE7, 0x82, 0x00, 0x00, 0xA3, 0x8C, 0x81,
        0x00, 0x00, 0x80, 0x82, 0x49, 0x83, 0x42, 0x00, 0x77, 0xF0, 0x38,
    };
    memcpy(buf, tpl, sizeof(tpl));
    return sizeof(tpl);
}

static size_t build_seed_mp4_wraparound_68b(uint8_t *buf, size_t cap)
{
    if (cap < 68u) {
        return 0u;
    }
    memset(buf, 0, 68u);
    /* ftyp box + nested moov/trak with 64-bit box_size=1 wraparound trigger. */
    write_u32_be(buf + 0, 20u);
    write_u32_be(buf + 4, FOURCC('f', 't', 'y', 'p'));
    write_u32_be(buf + 8, FOURCC('i', 's', 'o', 'm'));
    write_u32_be(buf + 12, 0x200u);
    write_u32_be(buf + 16, FOURCC('m', 'p', '4', '1'));

    write_u32_be(buf + 20, 48u);
    write_u32_be(buf + 24, FOURCC('m', 'o', 'o', 'v'));
    write_u32_be(buf + 28, 1u);
    write_u32_be(buf + 32, FOURCC('t', 'r', 'a', 'k'));
    /* 64-bit largesize near UINT64_MAX to stress offset+size overflow checks. */
    memset(buf + 36, 0xFF, 7);
    buf[43] = 0xFCu;
    return 68u;
}

static void run_builtin_demuxer_corpus(void)
{
    uint8_t empty = 0u;
    (void)LLVMFuzzerTestOneInput(&empty, 0u);
    (void)LLVMFuzzerTestOneInput(&empty, 1u);

    uint8_t seed_mp4[2048];
    size_t mp4_len = build_seed_mp4(seed_mp4, sizeof(seed_mp4));
    for (size_t cut = 1u; cut < mp4_len; cut += 7u) {
        (void)LLVMFuzzerTestOneInput(seed_mp4, cut);
    }

    uint8_t seed_webm[512];
    size_t webm_len = build_seed_webm(seed_webm, sizeof(seed_webm));
    for (size_t cut = 1u; cut < webm_len; cut += 3u) {
        (void)LLVMFuzzerTestOneInput(seed_webm, cut);
    }

    /* Include truncated EBML sizes and oversized MP4 box regression seed. */
    static const uint8_t bad_ebml[] = {
        0x1Au, 0x45u, 0xDFu, 0xA3u, 0xFFu, 0x18u, 0x53u, 0x80u, 0x67u, 0x01u, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x1Fu, 0x43u, 0xB6u, 0x75u, 0x84u, 0xE7u, 0x81u,
    };
    uint8_t wrap_mp4[68];
    size_t wrap_len = build_seed_mp4_wraparound_68b(wrap_mp4, sizeof(wrap_mp4));

    wwal_fuzz_sample_t seeds[4];
    size_t seed_count = 0u;
    if (mp4_len > 0u) {
        seeds[seed_count++] = (wwal_fuzz_sample_t){seed_mp4, mp4_len};
    }
    if (webm_len > 0u) {
        seeds[seed_count++] = (wwal_fuzz_sample_t){seed_webm, webm_len};
    }
    if (wrap_len > 0u) {
        seeds[seed_count++] = (wwal_fuzz_sample_t){wrap_mp4, wrap_len};
    }
    seeds[seed_count++] = (wwal_fuzz_sample_t){bad_ebml, sizeof(bad_ebml)};

    wwal_fuzz_run_mutations(seeds, seed_count, 0xC0FFEE11u, 512u, 2048u);
}

WWAL_FUZZ_HARNESS_MAIN("fuzz_demuxer", NULL, run_builtin_demuxer_corpus, NULL)
#endif
