// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_DMABUF_H
#define WAYWAL_DMABUF_H
#endif
#include "fuzz_harness.h"
#include "waywal/demuxer.h"
#include "waywal/vaapi_dec.h"

#include "../../src/daemon/vaapi_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_avcc_synthetic_wrap(const uint8_t *data, size_t size)
{
    if (size < 2u) {
        return;
    }

    /* Cap synthetic SPS/PPS wrap at 16-bit NAL length without dropping large inputs globally. */
    size_t wrap_len = size <= 32768u ? size : 32768u;
    size_t half = wrap_len / 2u;
    size_t rest = wrap_len - half;
    size_t cap = 16u + wrap_len;

    uint8_t stack_buf[1040];
    uint8_t *avcc = (cap <= sizeof(stack_buf)) ? stack_buf : (uint8_t *)malloc(cap);
    if (!avcc) {
        return;
    }

    avcc[0] = 1u;
    avcc[1] = data[0];
    avcc[2] = 0u;
    avcc[3] = data[1];
    avcc[4] = 0xFFu;
    avcc[5] = 0xE1u;
    avcc[6] = (uint8_t)((half + 1u) >> 8u);
    avcc[7] = (uint8_t)((half + 1u) & 0xFFu);
    /* Force NAL unit type 7 (SPS) so the Exp-Golomb SPS parser runs on arbitrary payloads. */
    avcc[8] = 0x67u;
    memcpy(avcc + 9u, data, half);

    size_t off = 9u + half;
    avcc[off++] = 1u;
    avcc[off++] = (uint8_t)((rest + 1u) >> 8u);
    avcc[off++] = (uint8_t)((rest + 1u) & 0xFFu);
    /* Force NAL unit type 8 (PPS) on the second half of the payload. */
    avcc[off++] = 0x68u;
    memcpy(avcc + off, data + half, rest);
    off += rest;

    vaapi_decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    dec.va_profile = VAProfileH264High;
    vaapi_decoder_set_extradata(&dec, avcc, off);
    vaapi_decoder_reset(&dec);

    if (avcc != stack_buf) {
        free(avcc);
    }
}

static void fuzz_seed_dpb(h264_dpb_t *dpb, const uint8_t *data, size_t size)
{
    memset(dpb, 0, sizeof(*dpb));
    size_t n = (size > 0u) ? (size_t)(data[0] % 5u) : 0u;
    for (size_t i = 0u; i < n; ++i) {
        dpb->entries[i].surface = (VASurfaceID)(i + 1u);
        dpb->entries[i].frame_num = (int32_t)i;
        dpb->entries[i].pic_order_cnt = (int32_t)(i * 2u);
        dpb->entries[i].is_reference = true;
        dpb->entries[i].is_long_term = (size > i + 1u) && ((data[i + 1u] & 1u) != 0u);
    }
    dpb->count = n;
}

static void fuzz_h264_parsers(const uint8_t *data, size_t size)
{
    parsed_sps_t sps;
    parsed_pps_t pps;
    memset(&sps, 0, sizeof(sps));
    memset(&pps, 0, sizeof(pps));

    if (!parse_h264_sps(data, size, &sps)) {
        memset(&sps, 0, sizeof(sps));
        sps.profile_idc = 100u;
        sps.chroma_format_idc = 1u;
        sps.log2_max_frame_num_minus4 = 0u;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 2u;
        sps.num_ref_frames = 2u;
        sps.frame_mbs_only_flag = true;
        sps.pic_width_in_mbs_minus1 = 15u;
        sps.pic_height_in_map_units_minus1 = 8u;
        sps.valid = true;
    }

    if (!parse_h264_pps(data, size, &sps, &pps)) {
        memset(&pps, 0, sizeof(pps));
        pps.valid = true;
    }

    h264_dpb_t dpb;
    fuzz_seed_dpb(&dpb, data, size);
    int32_t max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4u);

    VASliceParameterBufferH264 sp;
    slice_header_meta_t meta;
    memset(&sp, 0, sizeof(sp));
    memset(&meta, 0, sizeof(meta));
    if (parse_h264_slice_header(data, size, &sps, &pps, &sp, &meta)) {
        build_h264_ref_lists(&dpb, sp.slice_type, (int32_t)meta.frame_num, max_fn, meta.poc_lsb,
                             &sp);
        apply_h264_ref_pic_list_mod(&dpb, (int32_t)meta.frame_num, max_fn, &meta, &sp);
    }

    for (int avcc_mode = 0; avcc_mode < 2; ++avcc_mode) {
        bool is_avcc = (avcc_mode == 1);
        uint8_t nal_len_sz = (size > 0u && (data[0] & 1u)) ? 2u : 4u;
        size_t off = 0u;
        for (uint32_t nals = 0u; nals < 32u; ++nals) {
            const uint8_t *nal = NULL;
            uint32_t nal_len = 0u;
            if (!next_h264_nal(data, size, is_avcc, nal_len_sz, &off, &nal, &nal_len)) {
                break;
            }
            if (!nal || nal_len == 0u) {
                continue;
            }
            uint8_t nal_type = nal[0] & 0x1Fu;
            if (nal_type == 7u) {
                parsed_sps_t parsed;
                if (parse_h264_sps(nal, nal_len, &parsed)) {
                    sps = parsed;
                    max_fn = 1 << (sps.log2_max_frame_num_minus4 + 4u);
                }
            } else if (nal_type == 8u) {
                parsed_pps_t parsed;
                if (parse_h264_pps(nal, nal_len, &sps, &parsed)) {
                    pps = parsed;
                }
            }
            memset(&sp, 0, sizeof(sp));
            memset(&meta, 0, sizeof(meta));
            if (parse_h264_slice_header(nal, nal_len, &sps, &pps, &sp, &meta)) {
                build_h264_ref_lists(&dpb, sp.slice_type, (int32_t)meta.frame_num, max_fn,
                                     meta.poc_lsb, &sp);
                apply_h264_ref_pic_list_mod(&dpb, (int32_t)meta.frame_num, max_fn, &meta, &sp);
            }
        }
    }
}

static void fuzz_hevc_parsers(const uint8_t *data, size_t size)
{
    parsed_hevc_sps_t sps;
    parsed_hevc_pps_t pps;
    memset(&sps, 0, sizeof(sps));
    memset(&pps, 0, sizeof(pps));

    if (!parse_hevc_sps(data, size, &sps)) {
        memset(&sps, 0, sizeof(sps));
        sps.chroma_format_idc = 1u;
        sps.pic_width_in_luma_samples = 1920u;
        sps.pic_height_in_luma_samples = 1080u;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 4u;
        sps.sps_max_dec_pic_buffering_minus1 = 3u;
        sps.log2_min_luma_coding_block_size_minus3 = 0u;
        sps.log2_diff_max_min_luma_coding_block_size = 3u;
        sps.valid = true;
    }

    if (!parse_hevc_pps(data, size, &pps)) {
        memset(&pps, 0, sizeof(pps));
        pps.valid = true;
    }

    h264_dpb_t dpb;
    fuzz_seed_dpb(&dpb, data, size);

    VASliceParameterBufferHEVC sp;
    hevc_st_rps_t st_rps;
    int32_t poc_lsb = 0;
    uint32_t st_rps_bits = 0u;
    memset(&sp, 0, sizeof(sp));
    memset(&st_rps, 0, sizeof(st_rps));
    if (parse_hevc_slice_header_ex(data, size, &sps, &pps, &sp, &poc_lsb, &st_rps_bits, &st_rps)) {
        build_hevc_ref_lists(&dpb, sp.LongSliceFlags.fields.slice_type, poc_lsb, &sp);
    }

    /* Also split Annex-B/length-prefixed chunks to parse multi-NAL HEVC bitstreams. */
    size_t off = 0u;
    for (uint32_t nals = 0u; nals < 32u; ++nals) {
        const uint8_t *nal = NULL;
        uint32_t nal_len = 0u;
        if (!next_h264_nal(data, size, false, 4u, &off, &nal, &nal_len)) {
            break;
        }
        if (!nal || nal_len < 2u) {
            continue;
        }
        uint8_t nal_type = (uint8_t)((nal[0] >> 1u) & 0x3Fu);
        if (nal_type == 33u) {
            parsed_hevc_sps_t parsed;
            if (parse_hevc_sps(nal, nal_len, &parsed)) {
                sps = parsed;
            }
        } else if (nal_type == 34u) {
            parsed_hevc_pps_t parsed;
            if (parse_hevc_pps(nal, nal_len, &parsed)) {
                pps = parsed;
            }
        }
        memset(&sp, 0, sizeof(sp));
        memset(&st_rps, 0, sizeof(st_rps));
        poc_lsb = 0;
        st_rps_bits = 0u;
        if (parse_hevc_slice_header_ex(nal, nal_len, &sps, &pps, &sp, &poc_lsb, &st_rps_bits,
                                       &st_rps)) {
            build_hevc_ref_lists(&dpb, sp.LongSliceFlags.fields.slice_type, poc_lsb, &sp);
        }
    }
}

static void fuzz_vp9_parsers(const uint8_t *data, size_t size)
{
    vaapi_decoder_t dec;
    memset(&dec, 0, sizeof(dec));
    dec.va_profile = VAProfileVP9Profile0;
    dec.width = 1920u;
    dec.height = 1080u;

    VADecPictureParameterBufferVP9 pic_param;
    VASliceParameterBufferVP9 slice_param;
    uint8_t refresh_flags = 0u;
    bool is_kf = false;
    bool show_ex = false;
    uint8_t show_idx = 0u;

    memset(&pic_param, 0, sizeof(pic_param));
    memset(&slice_param, 0, sizeof(slice_param));
    (void)parse_vp9_uncompressed_header(&dec, data, size, &pic_param, &slice_param, &refresh_flags,
                                        &is_kf, &show_ex, &show_idx);

    const uint8_t *sub_ptrs[8] = {0};
    size_t sub_sizes[8] = {0};
    uint32_t count = vp9_split_superframe(data, size, sub_ptrs, sub_sizes);
    for (uint32_t i = 0u; i < count && i < 8u; ++i) {
        if (sub_ptrs[i] && sub_sizes[i] > 0u) {
            memset(&pic_param, 0, sizeof(pic_param));
            memset(&slice_param, 0, sizeof(slice_param));
            (void)parse_vp9_uncompressed_header(&dec, sub_ptrs[i], sub_sizes[i], &pic_param,
                                                &slice_param, &refresh_flags, &is_kf, &show_ex,
                                                &show_idx);
        }
    }
}

static void fuzz_av1_parsers(const uint8_t *data, size_t size)
{
    vaapi_decoder_t dec;
    memset(&dec, 0, sizeof(dec));
#ifdef VAProfileAV1Profile0
    dec.va_profile = VAProfileAV1Profile0;
#endif
    dec.width = 1920u;
    dec.height = 1080u;
    av1_init_default_slot_state(&dec);

    dec.cached_av1_seq.frame_width_bits_minus_1 = 10u;
    dec.cached_av1_seq.frame_height_bits_minus_1 = 10u;
    dec.cached_av1_seq.max_frame_width_minus_1 = 1919u;
    dec.cached_av1_seq.max_frame_height_minus_1 = 1079u;
    dec.cached_av1_seq.enable_order_hint = true;
    dec.cached_av1_seq.order_hint_bits_minus_1 = 7u;
    dec.cached_av1_seq.valid = true;

    parsed_av1_seq_t parsed_seq;
    if (parse_av1_sequence_header(data, size, &parsed_seq)) {
        dec.cached_av1_seq = parsed_seq;
    }

    VADecPictureParameterBufferAV1 pic_param;
    VASliceParameterBufferAV1 slices[8];
    uint32_t slice_count = 0u;
    uint8_t tile_sz_minus1 = 0u;
    uint8_t refresh_flags = 0u;
    bool is_kf = false;
    bool show_ex = false;
    uint8_t show_idx = 0u;

    memset(&pic_param, 0, sizeof(pic_param));
    memset(slices, 0, sizeof(slices));
    if (parse_av1_frame_header(&dec, data, size, 6u, 0u, &pic_param, slices, 8u, &slice_count,
                               &tile_sz_minus1, &refresh_flags, &is_kf, &show_ex, &show_idx)) {
        (void)parse_av1_tile_group_obu(data, size, 0u, 0u, &pic_param, tile_sz_minus1, slices, 8u);
    }

    memset(&pic_param, 0, sizeof(pic_param));
    memset(slices, 0, sizeof(slices));
    slice_count = 0u;
    (void)parse_av1_frame_header(&dec, data, size, 3u, 0u, &pic_param, slices, 8u, &slice_count,
                                 &tile_sz_minus1, &refresh_flags, &is_kf, &show_ex, &show_idx);
    (void)parse_av1_tile_group_obu(data, size, 0u, 0u, &pic_param,
                                   (size > 0u) ? (uint8_t)(data[0] & 3u) : 0u, slices, 8u);

    size_t obu_off = 0u;
    for (uint32_t i = 0u; i < 32u && obu_off < size; ++i) {
        uint8_t obu_hdr = data[obu_off++];
        uint32_t obu_type = (uint32_t)((obu_hdr >> 3u) & 0x0Fu);
        bool has_ext = (obu_hdr & 0x04u) != 0u;
        bool has_size = (obu_hdr & 0x02u) != 0u;
        if (has_ext) {
            if (obu_off >= size) {
                break;
            }
            obu_off++;
        }
        size_t payload_len = size - obu_off;
        if (has_size) {
            size_t leb_bytes = 0u;
            uint64_t val = av1_read_leb128(data + obu_off, size - obu_off, &leb_bytes);
            if (leb_bytes == 0u || val > (uint64_t)(size - obu_off - leb_bytes)) {
                break;
            }
            obu_off += leb_bytes;
            payload_len = (size_t)val;
        }
        const uint8_t *payload = data + obu_off;
        if (obu_type == 1u) {
            if (parse_av1_sequence_header(payload, payload_len, &parsed_seq)) {
                dec.cached_av1_seq = parsed_seq;
            }
        } else if (obu_type == 3u || obu_type == 6u) {
            memset(&pic_param, 0, sizeof(pic_param));
            memset(slices, 0, sizeof(slices));
            slice_count = 0u;
            (void)parse_av1_frame_header(&dec, payload, payload_len, obu_type, (uint32_t)obu_off,
                                         &pic_param, slices, 8u, &slice_count, &tile_sz_minus1,
                                         &refresh_flags, &is_kf, &show_ex, &show_idx);
        } else if (obu_type == 4u) {
            (void)parse_av1_tile_group_obu(payload, payload_len, 0u, (uint32_t)obu_off, &pic_param,
                                           tile_sz_minus1, slices, 8u);
        }
        obu_off += payload_len;
        if (!has_size) {
            break;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data) {
        return 0;
    }

    static const VAProfile profiles[] = {
        VAProfileH264Main,    VAProfileH264High,   VAProfileH264ConstrainedBaseline,
        VAProfileHEVCMain,    VAProfileHEVCMain10, VAProfileVP9Profile0,
        VAProfileVP9Profile2,
#ifdef VAProfileAV1Profile0
        VAProfileAV1Profile0,
#endif
    };

    for (size_t i = 0u; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        vaapi_decoder_t dec;
        memset(&dec, 0, sizeof(dec));
        dec.va_profile = profiles[i];
        dec.width = 1920u;
        dec.height = 1080u;
        vaapi_decoder_set_extradata(&dec, data, size);
        vaapi_decoder_reset(&dec);
    }

    fuzz_avcc_synthetic_wrap(data, size);
    fuzz_h264_parsers(data, size);
    fuzz_hevc_parsers(data, size);
    fuzz_vp9_parsers(data, size);
    fuzz_av1_parsers(data, size);
    return 0;
}

#ifdef WWAL_FUZZ_STANDALONE
static void run_builtin_bitstream_corpus(void)
{
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

    /* Seed 3: Valid AV1 av1C + OBU_SEQUENCE_HEADER + OBU_FRAME + OBU_TILE_GROUP */
    static const uint8_t seed_av1[] = {
        0x81, 0x08, 0x0C, 0x00, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x42, 0xA7, 0xBF, 0xE6, 0x2E,
        0xDF, 0xF8, 0x42, 0x32, 0x08, 0x10, 0x00, 0x90, 0x40, 0x00, 0x00, 0x80, 0x00, 0x32,
        0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x04, 0x00, 0x01, 0x02, 0x03,
    };

    /* Seed 4: VP9 uncompressed keyframe header + superframe index regression seed */
    static const uint8_t seed_vp9[] = {
        0x82, 0x49, 0x83, 0x42, 0x00, 0x07, 0x7F, 0x04,
        0x37, 0x08, 0x20, 0x00, 0x00, 0xD1, 0x0D, 0xD1,
    };

    /* Seed 5: Annex-B H.264 High SPS with extreme scaling_list delta_scale values */
    static const uint8_t seed_h264_annexb[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD1, 0x00, 0x78, 0x02,
        0x27, 0xE2, 0x70, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00,
        0x3C, 0x8F, 0x14, 0x29, 0x96, 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0xA0, 0x00, 0x00, 0xFF, 0x80,
    };

    /* Seed 6: Annex-B HEVC SPS + Slice with extreme st_ref_pic_set delta POC values */
    static const uint8_t seed_hevc_annexb[] = {
        0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
        0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5D, 0xA0, 0x02, 0x80, 0x80,
        0x2D, 0x16, 0x59, 0x59, 0xA4, 0x93, 0x2B, 0xC0, 0x00, 0x00, 0x00, 0x01, 0x26,
        0x01, 0xAF, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFE, 0x00,
    };

    const wwal_fuzz_sample_t seeds[] = {
        {seed_avcc, 0u},
        {seed_avcc, sizeof(seed_avcc)},
        {seed_hvcc, sizeof(seed_hvcc)},
        {seed_av1, sizeof(seed_av1)},
        {seed_vp9, sizeof(seed_vp9)},
        {seed_h264_annexb, sizeof(seed_h264_annexb)},
        {seed_hevc_annexb, sizeof(seed_hevc_annexb)},
    };

    wwal_fuzz_run_mutations(seeds, sizeof(seeds) / sizeof(seeds[0]), 0x9E3779B9u, 512u, 256u);
}

WWAL_FUZZ_HARNESS_MAIN("fuzz_bitstream", NULL, run_builtin_bitstream_corpus, NULL)
#endif
