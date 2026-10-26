// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_DMABUF_H
#define WAYWAL_DMABUF_H
#endif
#include "waywal/demuxer.h"
#include "waywal/vaapi_dec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_avcc_synthetic_wrap(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 1024)
        return;

    uint8_t avcc[1040];
    size_t half = size / 2;
    size_t rest = size - half;

    avcc[0] = 1;
    avcc[1] = data[0];
    avcc[2] = 0;
    avcc[3] = data[1];
    avcc[4] = 0xFF;
    avcc[5] = 0xE1;
    avcc[6] = (uint8_t)((half + 1) >> 8);
    avcc[7] = (uint8_t)((half + 1) & 0xFF);
    /* Force NAL unit type 7 (SPS) so the inner Exp-Golomb SPS parser runs on every fuzz input. */
    avcc[8] = 0x67;
    memcpy(avcc + 9, data, half);

    size_t off = 9 + half;
    avcc[off++] = 1;
    avcc[off++] = (uint8_t)((rest + 1) >> 8);
    avcc[off++] = (uint8_t)((rest + 1) & 0xFF);
    /* Force NAL unit type 8 (PPS) on the second half of the fuzz buffer. */
    avcc[off++] = 0x68;
    memcpy(avcc + off, data + half, rest);
    off += rest;

    vaapi_decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    dec.va_profile = VAProfileH264High;
    vaapi_decoder_set_extradata(&dec, avcc, off);
    vaapi_decoder_reset(&dec);
}

static void fuzz_annexb_and_obu_framing(const uint8_t *data, size_t size)
{
    size_t i = 0;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
            size_t hdr_start = (data[i + 2] == 1) ? (i + 3) : (i + 4);
            if (hdr_start < size) {
                uint8_t h264_type = data[hdr_start] & 0x1F;
                uint8_t hevc_type = (data[hdr_start] >> 1) & 0x3F;
                volatile uint8_t sink = (uint8_t)(h264_type ^ hevc_type);
                (void)sink;
            }
            i = hdr_start;
        } else {
            i++;
        }
    }

    size_t obu_off = 0;
    while (obu_off < size) {
        uint8_t obu_hdr = data[obu_off++];
        bool has_ext = (obu_hdr & 0x04) != 0;
        bool has_size = (obu_hdr & 0x02) != 0;
        if (has_ext) {
            if (obu_off >= size)
                break;
            obu_off++;
        }
        if (!has_size)
            break;
        uint64_t obu_len = 0;
        for (int b = 0; b < 8 && obu_off < size; ++b) {
            uint8_t byte = data[obu_off++];
            obu_len |= (uint64_t)(byte & 0x7F) << (b * 7);
            if ((byte & 0x80) == 0)
                break;
        }
        if (obu_len > size - obu_off)
            break;
        obu_off += (size_t)obu_len;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data)
        return 0;

    static const VAProfile profiles[] = {
        VAProfileH264Main,    VAProfileH264High,   VAProfileH264ConstrainedBaseline,
        VAProfileHEVCMain,    VAProfileHEVCMain10, VAProfileVP9Profile0,
        VAProfileVP9Profile2,
#ifdef VAProfileAV1Profile0
        VAProfileAV1Profile0,
#endif
    };

    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        vaapi_decoder_t dec;
        memset(&dec, 0, sizeof(dec));
        dec.va_profile = profiles[i];
        dec.width = 1920;
        dec.height = 1080;
        vaapi_decoder_set_extradata(&dec, data, size);
        vaapi_decoder_reset(&dec);
    }

    fuzz_avcc_synthetic_wrap(data, size);
    fuzz_annexb_and_obu_framing(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE
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
            if (len >= 0 && len <= 4 * 1024 * 1024) {
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

    /* Seed 1: Valid H.264 High Profile 1080p avcC extradata with SPS + PPS */
    static const uint8_t seed_avcc[] = {
        0x01, 0x64, 0x00, 0x28, 0xFF, 0xE1, 0x00, 0x19, 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40,
        0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00,
        0xF0, 0x3C, 0x60, 0xC6, 0x58, 0x01, 0x00, 0x05, 0x68, 0xEB, 0xE3, 0xCB, 0x22,
    };

    /* Seed 2: Valid HEVC Main10 hvcC extradata with VPS/SPS/PPS */
    static const uint8_t seed_hvcc[] = {
        0x01, 0x02, 0x20, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0xF0, 0x00,
        0xFC, 0xFD, 0xFA, 0xFA, 0x00, 0x03, 0x20, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0C, 0x01,
        0xFF, 0xFF, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
        0x00, 0x78, 0x95, 0x98, 0x21, 0x00, 0x01, 0x00, 0x21, 0x42, 0x01, 0x01, 0x02, 0x20, 0x00,
        0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xA0, 0x03, 0xC0,
        0x80, 0x10, 0xE5, 0x96, 0x56, 0x69, 0x24, 0xCA, 0xE0, 0x10, 0x00, 0x00, 0x22, 0x00, 0x01,
        0x00, 0x07, 0x44, 0x01, 0xC1, 0x72, 0xB4, 0x62, 0x40,
    };

    /* Seed 3: Valid AV1 av1C + OBU_SEQUENCE_HEADER + OBU_FRAME_HEADER */
    static const uint8_t seed_av1[] = {
        0x81, 0x08, 0x0C, 0x00, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x42, 0xA7, 0xBF, 0xE6, 0x2E,
        0xDF, 0xF8, 0x42, 0x32, 0x08, 0x10, 0x00, 0x90, 0x40, 0x00, 0x00, 0x80, 0x00,
    };

    /* Seed 4: VP9 uncompressed keyframe header */
    static const uint8_t seed_vp9[] = {
        0x82, 0x49, 0x83, 0x42, 0x00, 0x07, 0x7F, 0x04, 0x37, 0x08, 0x20, 0x00, 0x00,
    };

    LLVMFuzzerTestOneInput(seed_avcc, 0);
    LLVMFuzzerTestOneInput(seed_avcc, sizeof(seed_avcc));
    LLVMFuzzerTestOneInput(seed_hvcc, sizeof(seed_hvcc));
    LLVMFuzzerTestOneInput(seed_av1, sizeof(seed_av1));
    LLVMFuzzerTestOneInput(seed_vp9, sizeof(seed_vp9));

    uint32_t rng = 0x9E3779B9U;
    uint8_t mutated[256];
    const uint8_t *seeds[] = {seed_avcc, seed_hvcc, seed_av1, seed_vp9};
    const size_t seed_lens[] = {sizeof(seed_avcc), sizeof(seed_hvcc), sizeof(seed_av1),
                                sizeof(seed_vp9)};

    for (int iter = 0; iter < 512; ++iter) {
        size_t sidx = (size_t)iter & 3U;
        size_t len = seed_lens[sidx];
        memcpy(mutated, seeds[sidx], len);
        uint32_t flips = (xorshift32(&rng) % 5U) + 1U;
        for (uint32_t f = 0; f < flips; ++f) {
            size_t pos = xorshift32(&rng) % len;
            mutated[pos] ^= (uint8_t)(xorshift32(&rng) & 0xFFU);
        }
        size_t slice_len = (xorshift32(&rng) % len) + 1U;
        LLVMFuzzerTestOneInput(mutated, slice_len);
    }

    printf("[FUZZ] fuzz_bitstream standalone corpus smoke test PASSED.\n");
    return 0;
}
#endif
