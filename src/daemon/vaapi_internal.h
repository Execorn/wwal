// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_VAAPI_INTERNAL_H
#define WAYWAL_VAAPI_INTERNAL_H

#include "waywal/log.h"
#include "waywal/vaapi_dec.h"
#include "waywal/video_decoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>

#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
#include <immintrin.h>
#endif

typedef struct {
    uint32_t frame_num;
    int32_t poc_lsb;
    bool mmco_reset;
    uint32_t num_unmark;
    int32_t unmark_pic_nums[16];
    bool ref_pic_list_mod_flag_l0;
    uint8_t num_ref_mod_l0;
    uint8_t mod_of_pic_nums_idc_l0[32];
    uint32_t abs_diff_pic_num_l0[32];
    bool ref_pic_list_mod_flag_l1;
    uint8_t num_ref_mod_l1;
    uint8_t mod_of_pic_nums_idc_l1[32];
    uint32_t abs_diff_pic_num_l1[32];
} slice_header_meta_t;

static inline void av1_init_default_slot_state(vaapi_decoder_t *dec)
{
    if (!dec)
        return;
    static const int8_t k_default_ref_deltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};
    memset(dec->av1_ref_order_hints, 0, sizeof(dec->av1_ref_order_hints));
    memset(dec->loop_filter_mode_deltas, 0, sizeof(dec->loop_filter_mode_deltas));
    memset(dec->seg_feature_data, 0, sizeof(dec->seg_feature_data));
    memset(dec->seg_feature_mask, 0, sizeof(dec->seg_feature_mask));
    memset(dec->film_grain, 0, sizeof(dec->film_grain));
    for (int s = 0; s < 8; ++s) {
        memcpy(dec->loop_filter_ref_deltas[s], k_default_ref_deltas, sizeof(k_default_ref_deltas));
        for (int r = 0; r < 7; ++r) {
            memset(&dec->wm[s][r], 0, sizeof(dec->wm[s][r]));
            dec->wm[s][r].wmtype = VAAV1TransformationIdentity;
            dec->wm[s][r].wmmat[2] = 1 << 16;
            dec->wm[s][r].wmmat[5] = 1 << 16;
        }
    }
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit_pos;
    size_t end_bit_pos;
} bit_reader_t;

typedef bit_reader_t bs_t;

static inline void bit_reader_init_raw(bit_reader_t *br, const uint8_t *data, size_t size)
{
    br->data = data;
    br->size = size;
    br->bit_pos = 0;
    br->end_bit_pos = size * 8;
}

static inline void bit_reader_init_rbsp(bit_reader_t *br, const uint8_t *data, size_t size)
{
    br->data = data;
    br->size = size;
    br->bit_pos = 0;
    br->end_bit_pos = size * 8;
    for (size_t k = size; k > 0; --k) {
        if (data[k - 1] != 0) {
            uint8_t v = data[k - 1];
            uint32_t tz = 0;
            while (((v >> tz) & 1U) == 0) {
                tz++;
            }
            br->end_bit_pos = (k - 1) * 8 + (7 - tz);
            break;
        }
    }
}

static inline void bit_reader_init(bit_reader_t *br, const uint8_t *data, size_t size)
{
    bit_reader_init_rbsp(br, data, size);
}

static inline bool more_rbsp_data(const bit_reader_t *br) { return br->bit_pos < br->end_bit_pos; }

static inline uint32_t read_bit(bit_reader_t *br)
{
    if (br->bit_pos >= br->size * 8)
        return 0;
    size_t byte_idx = br->bit_pos / 8;
    uint32_t bit_idx = 7 - (br->bit_pos % 8);
    br->bit_pos++;
    return (br->data[byte_idx] >> bit_idx) & 1;
}

static inline uint32_t read_bits(bit_reader_t *br, uint32_t n)
{
    uint32_t val = 0;
    for (uint32_t i = 0; i < n; ++i) {
        val = (val << 1) | read_bit(br);
    }
    return val;
}

static inline uint32_t read_ue(bit_reader_t *br)
{
    uint32_t zeros = 0;
    while (read_bit(br) == 0 && br->bit_pos < br->size * 8) {
        zeros++;
        if (zeros > 31)
            return 0;
    }
    if (zeros == 0)
        return 0;
    uint32_t val = read_bits(br, zeros);
    if (zeros == 31)
        return 0x7FFFFFFFU + val;
    return (1U << zeros) - 1 + val;
}

static inline int32_t read_se(bit_reader_t *br)
{
    uint32_t ue = read_ue(br);
    if (ue == 0xFFFFFFFFU)
        return 0;
    if (ue % 2 == 1) {
        return (int32_t)((ue + 1) / 2);
    } else {
        return -(int32_t)(ue / 2);
    }
}

static inline size_t unescape_rbsp(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    size_t si = 0, di = 0;
#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_three = _mm256_set1_epi8(3);
    while (si + 32 <= src_len && di + 32 <= dst_len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(src + si));
        uint32_t zmask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_zero));
        uint32_t tmask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_three));
        uint32_t epb_mask = zmask & (zmask >> 1) & (tmask >> 2);
        _mm256_storeu_si256((__m256i *)(void *)(dst + di), chunk);
        if (epb_mask == 0) {
            if ((zmask & 0xC0000000U) == 0) {
                si += 32;
                di += 32;
            } else {
                si += 30;
                di += 30;
            }
        } else {
            uint32_t first_k = (uint32_t)__builtin_ctz(epb_mask);
            di += first_k + 2U;
            si += first_k + 3U;
        }
    }
#endif
    while (si < src_len && di < dst_len) {
        if (si + 2 < src_len && src[si] == 0 && src[si + 1] == 0 && src[si + 2] == 3) {
            if (di + 2 > dst_len)
                break;
            dst[di++] = 0;
            dst[di++] = 0;
            si += 3;
        } else {
            dst[di++] = src[si++];
        }
    }
    return di;
}

static inline size_t rbsp_bit_offset_to_ebsp_bits(const uint8_t *ebsp, size_t ebsp_len,
                                                  size_t rbsp_bits)
{
    size_t target_rbsp_bytes = rbsp_bits / 8;
    size_t rem_bits = rbsp_bits % 8;
    size_t si = 0, di = 0, epb_count = 0;
    while (si < ebsp_len && di < target_rbsp_bytes) {
        if (si + 2 < ebsp_len && ebsp[si] == 0 && ebsp[si + 1] == 0 && ebsp[si + 2] == 3) {
            if (di + 2 >= target_rbsp_bytes) {
                di = target_rbsp_bytes;
                break;
            }
            di += 2;
            si += 3;
            epb_count++;
        } else {
            di++;
            si++;
        }
    }
    return (target_rbsp_bytes + epb_count) * 8 + rem_bits;
}

static inline uint32_t ceil_log2_u32(uint32_t val)
{
    if (val <= 1)
        return 0;
    uint32_t bits = 0;
    uint32_t v = val - 1;
    while (v > 0) {
        v >>= 1;
        bits++;
    }
    return bits;
}

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* H.264 bitstream parser & DPB helpers (vaapi_h264.c) */
void refresh_cached_h264_iq(vaapi_decoder_t *dec);
bool parse_h264_sps(const uint8_t *nal, size_t nal_len, parsed_sps_t *sps);
bool parse_h264_pps(const uint8_t *nal, size_t nal_len, const parsed_sps_t *sps, parsed_pps_t *pps);
bool parse_h264_slice_header(const uint8_t *nal, size_t nal_len, const parsed_sps_t *sps,
                             const parsed_pps_t *pps, VASliceParameterBufferH264 *sp,
                             slice_header_meta_t *meta);
bool next_h264_nal(const uint8_t *data, size_t size, bool is_avcc, uint8_t nal_len_sz,
                   size_t *scan_off, const uint8_t **out_nal, uint32_t *out_len);
void fill_va_pic_h264(VAPictureH264 *vp, const h264_dpb_entry_t *e);
void build_h264_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_fn,
                          int32_t max_fn, int32_t curr_poc, VASliceParameterBufferH264 *sp);
void apply_h264_ref_pic_list_mod(const h264_dpb_t *dpb, int32_t curr_fn, int32_t max_fn,
                                 const slice_header_meta_t *meta, VASliceParameterBufferH264 *sp);

/* HEVC bitstream parser & DPB helpers (vaapi_hevc.c) */
bool parse_hevc_sps(const uint8_t *nal, size_t nal_len, parsed_hevc_sps_t *sps);
bool parse_hevc_pps(const uint8_t *nal, size_t nal_len, parsed_hevc_pps_t *pps);
bool parse_hevc_slice_header(const uint8_t *nal, size_t nal_len, const parsed_hevc_sps_t *sps,
                             const parsed_hevc_pps_t *pps, VASliceParameterBufferHEVC *sp,
                             int32_t *out_poc_lsb, uint32_t *out_st_rps_bits);
bool parse_hevc_slice_header_ex(const uint8_t *nal, size_t nal_len, const parsed_hevc_sps_t *sps,
                                const parsed_hevc_pps_t *pps, VASliceParameterBufferHEVC *sp,
                                int32_t *out_poc_lsb, uint32_t *out_st_rps_bits,
                                hevc_st_rps_t *out_rps);
void build_hevc_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_poc,
                          VASliceParameterBufferHEVC *sp);

/* VP9 bitstream parser & superframe splitter (vaapi_vp9.c) */
uint32_t vp9_split_superframe(const uint8_t *data, size_t size, const uint8_t *sub_ptrs[8],
                              size_t sub_sizes[8]);
bool parse_vp9_uncompressed_header(vaapi_decoder_t *dec, const uint8_t *data, size_t size,
                                   VADecPictureParameterBufferVP9 *pic_param,
                                   VASliceParameterBufferVP9 *slice_param,
                                   uint8_t *out_refresh_flags, bool *out_is_keyframe,
                                   bool *out_show_existing, uint8_t *out_show_existing_idx);

/* AV1 OBU parser & frame submission (vaapi_av1.c) */
uint64_t av1_read_leb128(const uint8_t *data, size_t size, size_t *out_bytes);
bool parse_av1_sequence_header(const uint8_t *payload, size_t len, parsed_av1_seq_t *seq);
uint32_t parse_av1_tile_group_obu(const uint8_t *obu_data, size_t obu_size, size_t tg_offset,
                                  uint32_t base_obu_offset,
                                  const VADecPictureParameterBufferAV1 *pic_param,
                                  uint8_t tile_size_bytes_minus_1,
                                  VASliceParameterBufferAV1 *out_slices, uint32_t max_slices);
bool parse_av1_frame_header(vaapi_decoder_t *dec, const uint8_t *payload, size_t len,
                            uint32_t obu_type, uint32_t obu_payload_offset,
                            VADecPictureParameterBufferAV1 *pic_param,
                            VASliceParameterBufferAV1 *out_slices, uint32_t max_slices,
                            uint32_t *io_slice_count, uint8_t *out_tile_size_bytes_minus_1,
                            uint8_t *out_refresh_flags, bool *out_is_keyframe,
                            bool *out_show_existing, uint8_t *out_show_existing_idx);
bool submit_av1_frame_to_va(vaapi_decoder_t *dec, uint32_t pool_size,
                            VADecPictureParameterBufferAV1 *pic_param,
                            VASliceParameterBufferAV1 *av1_slices, uint32_t av1_slice_count,
                            const demux_packet_t *pkt, uint8_t refresh_flags,
                            bool av1_show_existing, uint8_t av1_show_existing_idx,
                            VASurfaceID *io_target);

#endif /* WAYWAL_VAAPI_INTERNAL_H */
