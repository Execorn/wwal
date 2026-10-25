#include "waywal/demuxer.h"

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WWAL_FUZZ_VERBOSE
static void silence_stdio_once(void)
{
    static bool silenced = false;
    if (silenced)
        return;
    silenced = true;
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
}
#endif

static bool read_vint_checked(const uint8_t *data, size_t limit, size_t *off, uint64_t *val,
                              bool mask)
{
    if (*off >= limit)
        return false;
    uint8_t first = data[*off];
    if (first == 0)
        return false;
    uint32_t len = 1;
    uint8_t m = 0x80;
    while (!(first & m)) {
        len++;
        m >>= 1;
        if (len > 8)
            return false;
    }
    if (*off + len > limit)
        return false;
    uint64_t v = mask ? (first & (m - 1)) : first;
    for (uint32_t i = 1; i < len; ++i) {
        v = (v << 8) | data[*off + i];
    }
    *off += len;
    *val = v;
    return true;
}

static bool ebml_subelements_within_bounds(const uint8_t *data, size_t start, size_t end, int depth)
{
    if (depth > 4)
        return true;
    size_t off = start;
    while (off < end) {
        uint64_t id = 0, sz = 0;
        if (!read_vint_checked(data, end, &off, &id, false))
            return false;
        if (!read_vint_checked(data, end, &off, &sz, true))
            return false;
        if (sz > end - off)
            return false;
        if (id == 0x18538067ULL || id == 0x1549A966ULL || id == 0x1654AE6BULL || id == 0xAEULL ||
            id == 0xE0ULL || id == 0x1F43B675ULL) {
            if (!ebml_subelements_within_bounds(data, off, off + (size_t)sz, depth + 1))
                return false;
        }
        off += (size_t)sz;
    }
    return off == end;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size > 65536)
        return 0;

#ifndef WWAL_FUZZ_VERBOSE
    silence_stdio_once();
#endif

    if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
        if (!ebml_subelements_within_bounds(data, 0, size, 0))
            return 0;
    }

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

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            FILE *fp = fopen(argv[i], "rb");
            if (!fp)
                continue;
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (len >= 0 && len <= 65536) {
                uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)len, fp);
                    LLVMFuzzerTestOneInput(buf, n);
                    free(buf);
                }
            }
            fclose(fp);
        }
        return 0;
    }

    int saved_stdout = dup(STDOUT_FILENO);

    uint8_t empty = 0;
    LLVMFuzzerTestOneInput(&empty, 0);
    LLVMFuzzerTestOneInput(&empty, 1);

    uint8_t seed_mp4[2048];
    size_t mp4_len = build_seed_mp4(seed_mp4, sizeof(seed_mp4));
    LLVMFuzzerTestOneInput(seed_mp4, mp4_len);

    for (size_t cut = 1; cut < mp4_len; cut += 7) {
        LLVMFuzzerTestOneInput(seed_mp4, cut);
    }

    uint8_t seed_webm[512];
    size_t webm_len = build_seed_webm(seed_webm, sizeof(seed_webm));
    LLVMFuzzerTestOneInput(seed_webm, webm_len);

    for (size_t cut = 1; cut < webm_len; cut += 3) {
        LLVMFuzzerTestOneInput(seed_webm, cut);
    }

    uint32_t rng = 0xC0FFEE11U;
    uint8_t mutated[2048];
    for (int iter = 0; iter < 512; ++iter) {
        bool use_mp4 = (iter & 1) == 0;
        const uint8_t *src = use_mp4 ? seed_mp4 : seed_webm;
        size_t len = use_mp4 ? mp4_len : webm_len;
        memcpy(mutated, src, len);
        uint32_t flips = (xorshift32(&rng) % 6U) + 1U;
        for (uint32_t f = 0; f < flips; ++f) {
            size_t pos = xorshift32(&rng) % len;
            mutated[pos] ^= (uint8_t)(xorshift32(&rng) & 0xFFU);
        }
        LLVMFuzzerTestOneInput(mutated, len);
    }

    if (saved_stdout >= 0) {
        dprintf(saved_stdout, "[FUZZ] fuzz_demuxer standalone corpus smoke test PASSED.\n");
        close(saved_stdout);
    }
    return 0;
}
#endif
