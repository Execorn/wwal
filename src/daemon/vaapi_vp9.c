// SPDX-License-Identifier: GPL-3.0-or-later
#include "vaapi_internal.h"

static const int16_t g_vp9_dc_qlookup[256] = {
    4,   8,   8,   9,   10,  11,  12,  12,  13,   14,   15,   16,   17,   18,   19,   19,
    20,  21,  22,  23,  24,  25,  26,  26,  27,   28,   29,   30,   31,   32,   32,   33,
    34,  35,  36,  37,  38,  38,  39,  40,  41,   42,   43,   43,   44,   45,   46,   47,
    48,  48,  49,  50,  51,  52,  53,  53,  54,   55,   56,   57,   57,   58,   59,   60,
    61,  62,  62,  63,  64,  65,  66,  66,  67,   68,   69,   70,   70,   71,   72,   73,
    74,  74,  75,  76,  77,  78,  78,  79,  80,   81,   81,   82,   83,   84,   85,   85,
    87,  88,  90,  92,  93,  95,  96,  98,  99,   101,  102,  104,  105,  107,  108,  110,
    111, 113, 114, 116, 117, 118, 120, 121, 123,  125,  127,  129,  131,  134,  136,  138,
    140, 142, 144, 146, 148, 150, 152, 154, 156,  158,  161,  164,  166,  169,  172,  174,
    177, 180, 182, 185, 187, 190, 192, 195, 199,  202,  205,  208,  211,  214,  217,  220,
    223, 226, 230, 233, 237, 240, 243, 247, 250,  253,  257,  261,  265,  269,  272,  276,
    280, 284, 288, 292, 296, 300, 304, 309, 313,  317,  322,  326,  330,  335,  340,  344,
    349, 354, 359, 364, 369, 374, 379, 384, 389,  395,  400,  406,  411,  417,  423,  429,
    435, 441, 447, 454, 461, 467, 475, 482, 489,  497,  505,  513,  522,  530,  539,  549,
    559, 569, 579, 590, 602, 614, 626, 640, 654,  668,  684,  700,  717,  736,  755,  775,
    796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
};

static const int16_t g_vp9_ac_qlookup[256] = {
    4,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,   20,   21,   22,
    23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
    39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   70,
    71,   72,   73,   74,   75,   76,   77,   78,   79,   80,   81,   82,   83,   84,   85,   86,
    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,   97,   98,   99,   100,  101,  102,
    104,  106,  108,  110,  112,  114,  116,  118,  120,  122,  124,  126,  128,  130,  132,  134,
    136,  138,  140,  142,  144,  146,  148,  150,  152,  155,  158,  161,  164,  167,  170,  173,
    176,  179,  182,  185,  188,  191,  194,  197,  200,  203,  207,  211,  215,  219,  223,  227,
    231,  235,  239,  243,  247,  251,  255,  260,  265,  270,  275,  280,  285,  290,  295,  300,
    305,  311,  317,  323,  329,  335,  341,  347,  353,  359,  366,  373,  380,  387,  394,  401,
    408,  416,  424,  432,  440,  448,  456,  465,  474,  483,  492,  501,  510,  520,  530,  540,
    550,  560,  571,  582,  593,  604,  615,  627,  639,  651,  663,  676,  689,  702,  715,  729,
    743,  757,  771,  786,  801,  816,  832,  848,  864,  881,  898,  915,  933,  951,  969,  988,
    1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343,
    1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
};

static inline int32_t vp9_read_signed(bs_t *br, uint32_t n)
{
    int32_t val = (int32_t)read_bits(br, n);
    return read_bit(br) ? -val : val;
}

uint32_t vp9_split_superframe(const uint8_t *data, size_t size, const uint8_t *sub_ptrs[8],
                              size_t sub_sizes[8])
{
    if (!data || size == 0)
        return 0;
    uint8_t marker = data[size - 1];
    if ((marker & 0xE0U) == 0xC0U) {
        uint32_t frames_in_superframe = (marker & 0x07U) + 1U;
        uint32_t mag = ((marker >> 3) & 0x03U) + 1U;
        size_t index_size = 2U + (size_t)frames_in_superframe * mag;
        if (size >= index_size && data[size - index_size] == marker) {
            const uint8_t *idx_ptr = data + size - index_size + 1;
            size_t offset = 0;
            uint32_t valid_count = 0;
            for (uint32_t i = 0; i < frames_in_superframe; ++i) {
                size_t frame_sz = 0;
                for (uint32_t b = 0; b < mag; ++b) {
                    frame_sz |= (size_t)idx_ptr[i * mag + b] << (8U * b);
                }
                if (frame_sz > (size - index_size) - offset) {
                    valid_count = 0;
                    break;
                }
                sub_ptrs[valid_count] = data + offset;
                sub_sizes[valid_count] = frame_sz;
                valid_count++;
                offset += frame_sz;
            }
            if (valid_count == frames_in_superframe) {
                return valid_count;
            }
        }
    }
    sub_ptrs[0] = data;
    sub_sizes[0] = size;
    return 1;
}

bool parse_vp9_uncompressed_header(vaapi_decoder_t *dec, const uint8_t *data, size_t size,
                                   VADecPictureParameterBufferVP9 *pic_param,
                                   VASliceParameterBufferVP9 *slice_param,
                                   uint8_t *out_refresh_flags, bool *out_is_keyframe,
                                   bool *out_show_existing, uint8_t *out_show_existing_idx)
{
    if (!dec || !data || size < 1 || !pic_param || !slice_param)
        return false;

    if (out_show_existing)
        *out_show_existing = false;
    if (out_show_existing_idx)
        *out_show_existing_idx = 0;

    bs_t br;
    bit_reader_init_raw(&br, data, size);

    uint32_t frame_marker = read_bits(&br, 2);
    if (frame_marker != 2)
        return false;

    uint32_t profile_low = read_bit(&br);
    uint32_t profile_high = read_bit(&br);
    uint8_t profile = (uint8_t)((profile_high << 1) | profile_low);
    if (profile == 3) {
        read_bit(&br);
    }

    uint32_t show_existing = read_bit(&br);
    if (show_existing) {
        uint8_t idx = (uint8_t)read_bits(&br, 3);
        if (out_show_existing)
            *out_show_existing = true;
        if (out_show_existing_idx)
            *out_show_existing_idx = idx;
        *out_refresh_flags = 0;
        *out_is_keyframe = false;
        return true;
    }

    uint32_t frame_type = read_bit(&br);
    uint32_t show_frame = read_bit(&br);
    uint32_t error_resilient = read_bit(&br);
    uint32_t intra_only = 0;
    uint32_t reset_frame_context = 0;
    uint8_t refresh_frame_flags = 0xFF;
    uint8_t ref_frame_idx[3] = {0, 1, 2};
    uint8_t ref_frame_sign_bias[3] = {0, 0, 0};
    uint32_t allow_high_precision_mv = 0;
    uint32_t interp_filter = 0;
    uint32_t width = dec->width;
    uint32_t height = dec->height;

    if (frame_type == 0) {
        uint32_t sync = read_bits(&br, 24);
        if (sync != 0x498342U)
            return false;
        if (profile >= 2) {
            dec->vp9_bit_depth = read_bit(&br) ? 12 : 10;
        } else {
            dec->vp9_bit_depth = 8;
        }
        dec->vp9_color_space = (uint8_t)read_bits(&br, 3);
        if (dec->vp9_color_space != 7) {
            dec->vp9_color_range = (uint8_t)read_bit(&br);
            if (profile == 1 || profile == 3) {
                dec->vp9_subsampling_x = (uint8_t)read_bit(&br);
                dec->vp9_subsampling_y = (uint8_t)read_bit(&br);
                read_bit(&br);
            } else {
                dec->vp9_subsampling_x = 1;
                dec->vp9_subsampling_y = 1;
            }
        } else {
            dec->vp9_color_range = 1;
            if (profile == 1 || profile == 3) {
                dec->vp9_subsampling_x = 0;
                dec->vp9_subsampling_y = 0;
                read_bit(&br);
            }
        }
        width = read_bits(&br, 16) + 1;
        height = read_bits(&br, 16) + 1;
        if (read_bit(&br)) {
            read_bits(&br, 32);
        }
        refresh_frame_flags = 0xFF;
    } else {
        intra_only = show_frame ? 0 : read_bit(&br);
        reset_frame_context = error_resilient ? 0 : read_bits(&br, 2);
        if (intra_only) {
            uint32_t sync = read_bits(&br, 24);
            if (sync != 0x498342U)
                return false;
            if (profile > 0) {
                if (profile >= 2) {
                    dec->vp9_bit_depth = read_bit(&br) ? 12 : 10;
                } else {
                    dec->vp9_bit_depth = 8;
                }
                dec->vp9_color_space = (uint8_t)read_bits(&br, 3);
                if (dec->vp9_color_space != 7) {
                    dec->vp9_color_range = (uint8_t)read_bit(&br);
                    if (profile == 1 || profile == 3) {
                        dec->vp9_subsampling_x = (uint8_t)read_bit(&br);
                        dec->vp9_subsampling_y = (uint8_t)read_bit(&br);
                        read_bit(&br);
                    } else {
                        dec->vp9_subsampling_x = 1;
                        dec->vp9_subsampling_y = 1;
                    }
                } else {
                    dec->vp9_color_range = 1;
                    if (profile == 1 || profile == 3) {
                        dec->vp9_subsampling_x = 0;
                        dec->vp9_subsampling_y = 0;
                        read_bit(&br);
                    }
                }
            } else {
                dec->vp9_bit_depth = 8;
                dec->vp9_subsampling_x = 1;
                dec->vp9_subsampling_y = 1;
            }
            refresh_frame_flags = (uint8_t)read_bits(&br, 8);
            width = read_bits(&br, 16) + 1;
            height = read_bits(&br, 16) + 1;
            if (read_bit(&br)) {
                read_bits(&br, 32);
            }
        } else {
            refresh_frame_flags = (uint8_t)read_bits(&br, 8);
            for (int i = 0; i < 3; ++i) {
                ref_frame_idx[i] = (uint8_t)read_bits(&br, 3);
                ref_frame_sign_bias[i] = (uint8_t)read_bit(&br);
            }
            bool found_ref = false;
            for (int i = 0; i < 3; ++i) {
                if (read_bit(&br)) {
                    found_ref = true;
                    break;
                }
            }
            if (!found_ref) {
                width = read_bits(&br, 16) + 1;
                height = read_bits(&br, 16) + 1;
            }
            if (read_bit(&br)) {
                read_bits(&br, 32);
            }
            allow_high_precision_mv = read_bit(&br);
            if (read_bit(&br)) {
                interp_filter = 4;
            } else {
                static const uint8_t k_filter_map[4] = {1, 0, 2, 3};
                interp_filter = k_filter_map[read_bits(&br, 2)];
            }
        }
    }

    if (frame_type == 0 || intra_only || error_resilient) {
        dec->vp9_ref_deltas[0] = 1;
        dec->vp9_ref_deltas[1] = 0;
        dec->vp9_ref_deltas[2] = -1;
        dec->vp9_ref_deltas[3] = -1;
        dec->vp9_mode_deltas[0] = 0;
        dec->vp9_mode_deltas[1] = 0;
        memset(dec->vp9_mb_segment_tree_probs, 255, sizeof(dec->vp9_mb_segment_tree_probs));
        memset(dec->vp9_segment_pred_probs, 255, sizeof(dec->vp9_segment_pred_probs));
        dec->vp9_seg_abs_delta = false;
        memset(dec->vp9_seg_feature_enabled, 0, sizeof(dec->vp9_seg_feature_enabled));
        memset(dec->vp9_seg_feature_data, 0, sizeof(dec->vp9_seg_feature_data));
    }

    uint32_t refresh_frame_context = error_resilient ? 0 : read_bit(&br);
    uint32_t frame_parallel_decoding_mode = error_resilient ? 1 : read_bit(&br);
    uint32_t frame_context_idx = read_bits(&br, 2);

    uint8_t loop_filter_level = (uint8_t)read_bits(&br, 6);
    uint8_t loop_filter_sharpness = (uint8_t)read_bits(&br, 3);
    bool mode_ref_delta_enabled = (read_bit(&br) != 0);
    if (mode_ref_delta_enabled) {
        if (read_bit(&br)) {
            for (int i = 0; i < 4; ++i) {
                if (read_bit(&br)) {
                    dec->vp9_ref_deltas[i] = (int8_t)vp9_read_signed(&br, 6);
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (read_bit(&br)) {
                    dec->vp9_mode_deltas[i] = (int8_t)vp9_read_signed(&br, 6);
                }
            }
        }
    }

    int32_t base_qindex = (int32_t)read_bits(&br, 8);
    int32_t y_dc_delta_q = read_bit(&br) ? vp9_read_signed(&br, 4) : 0;
    int32_t uv_dc_delta_q = read_bit(&br) ? vp9_read_signed(&br, 4) : 0;
    int32_t uv_ac_delta_q = read_bit(&br) ? vp9_read_signed(&br, 4) : 0;
    bool lossless =
        (base_qindex == 0 && y_dc_delta_q == 0 && uv_dc_delta_q == 0 && uv_ac_delta_q == 0);

    uint32_t seg_enabled = read_bit(&br);
    uint32_t seg_update_map = 0;
    uint32_t seg_temporal_update = 0;

    if (seg_enabled) {
        seg_update_map = read_bit(&br);
        if (seg_update_map) {
            for (int i = 0; i < 7; ++i) {
                dec->vp9_mb_segment_tree_probs[i] =
                    read_bit(&br) ? (uint8_t)read_bits(&br, 8) : 255;
            }
            seg_temporal_update = read_bit(&br);
            if (seg_temporal_update) {
                for (int i = 0; i < 3; ++i) {
                    dec->vp9_segment_pred_probs[i] =
                        read_bit(&br) ? (uint8_t)read_bits(&br, 8) : 255;
                }
            } else {
                for (int i = 0; i < 3; ++i) {
                    dec->vp9_segment_pred_probs[i] = 255;
                }
            }
        }
        if (read_bit(&br)) {
            dec->vp9_seg_abs_delta = (read_bit(&br) != 0);
            memset(dec->vp9_seg_feature_enabled, 0, sizeof(dec->vp9_seg_feature_enabled));
            memset(dec->vp9_seg_feature_data, 0, sizeof(dec->vp9_seg_feature_data));
            for (int seg = 0; seg < 8; ++seg) {
                if (read_bit(&br)) {
                    dec->vp9_seg_feature_enabled[seg][0] = 1;
                    dec->vp9_seg_feature_data[seg][0] = (int16_t)vp9_read_signed(&br, 8);
                }
                if (read_bit(&br)) {
                    dec->vp9_seg_feature_enabled[seg][1] = 1;
                    dec->vp9_seg_feature_data[seg][1] = (int16_t)vp9_read_signed(&br, 6);
                }
                if (read_bit(&br)) {
                    dec->vp9_seg_feature_enabled[seg][2] = 1;
                    dec->vp9_seg_feature_data[seg][2] = (int16_t)read_bits(&br, 2);
                }
                if (read_bit(&br)) {
                    dec->vp9_seg_feature_enabled[seg][3] = 1;
                    dec->vp9_seg_feature_data[seg][3] = 1;
                }
            }
        }
    }

    for (int i = 0; i < 7; ++i)
        pic_param->mb_segment_tree_probs[i] = dec->vp9_mb_segment_tree_probs[i];
    for (int i = 0; i < 3; ++i)
        pic_param->segment_pred_probs[i] = dec->vp9_segment_pred_probs[i];

    uint32_t sb64_cols = (width + 63) >> 6;
    uint32_t min_log2_cols = 0;
    while ((64U << min_log2_cols) < sb64_cols) {
        min_log2_cols++;
    }
    uint32_t max_log2_cols = 0;
    while ((sb64_cols >> (max_log2_cols + 1)) >= 4) {
        max_log2_cols++;
    }
    if (max_log2_cols > 6)
        max_log2_cols = 6;
    uint32_t log2_tile_cols = min_log2_cols;
    while (log2_tile_cols < max_log2_cols && read_bit(&br)) {
        log2_tile_cols++;
    }
    uint32_t log2_tile_rows = read_bit(&br);
    if (log2_tile_rows) {
        log2_tile_rows += read_bit(&br);
    }

    uint16_t first_partition_size = (uint16_t)read_bits(&br, 16);
    uint8_t uncompressed_hdr_bytes = (uint8_t)((br.bit_pos + 7) / 8);

    pic_param->frame_width = (uint16_t)width;
    pic_param->frame_height = (uint16_t)height;
    for (int i = 0; i < 8; ++i) {
        pic_param->reference_frames[i] = dec->vp9_ref_frames[i];
    }
    pic_param->pic_fields.bits.subsampling_x = dec->vp9_subsampling_x;
    pic_param->pic_fields.bits.subsampling_y = dec->vp9_subsampling_y;
    pic_param->pic_fields.bits.frame_type = frame_type;
    pic_param->pic_fields.bits.show_frame = show_frame;
    pic_param->pic_fields.bits.error_resilient_mode = error_resilient;
    pic_param->pic_fields.bits.intra_only = intra_only;
    pic_param->pic_fields.bits.allow_high_precision_mv = allow_high_precision_mv;
    pic_param->pic_fields.bits.mcomp_filter_type = interp_filter;
    pic_param->pic_fields.bits.frame_parallel_decoding_mode = frame_parallel_decoding_mode;
    pic_param->pic_fields.bits.reset_frame_context = reset_frame_context;
    pic_param->pic_fields.bits.refresh_frame_context = refresh_frame_context;
    pic_param->pic_fields.bits.frame_context_idx = frame_context_idx;
    pic_param->pic_fields.bits.segmentation_enabled = seg_enabled;
    pic_param->pic_fields.bits.segmentation_temporal_update = seg_temporal_update;
    pic_param->pic_fields.bits.segmentation_update_map = seg_update_map;
    pic_param->pic_fields.bits.last_ref_frame = ref_frame_idx[0];
    pic_param->pic_fields.bits.last_ref_frame_sign_bias = ref_frame_sign_bias[0];
    pic_param->pic_fields.bits.golden_ref_frame = ref_frame_idx[1];
    pic_param->pic_fields.bits.golden_ref_frame_sign_bias = ref_frame_sign_bias[1];
    pic_param->pic_fields.bits.alt_ref_frame = ref_frame_idx[2];
    pic_param->pic_fields.bits.alt_ref_frame_sign_bias = ref_frame_sign_bias[2];
    pic_param->pic_fields.bits.lossless_flag = lossless ? 1 : 0;
    pic_param->filter_level = loop_filter_level;
    pic_param->sharpness_level = loop_filter_sharpness;
    pic_param->log2_tile_rows = (uint8_t)log2_tile_rows;
    pic_param->log2_tile_columns = (uint8_t)log2_tile_cols;
    pic_param->frame_header_length_in_bytes = uncompressed_hdr_bytes;
    pic_param->first_partition_size = first_partition_size;
    pic_param->profile = profile;
    pic_param->bit_depth = dec->vp9_bit_depth;

    int num_segs = seg_enabled ? 8 : 1;
    for (int seg = 0; seg < num_segs; ++seg) {
        if (seg_enabled) {
            slice_param->seg_param[seg].segment_flags.fields.segment_reference_enabled =
                dec->vp9_seg_feature_enabled[seg][2] ? 1 : 0;
            slice_param->seg_param[seg].segment_flags.fields.segment_reference =
                (uint16_t)(dec->vp9_seg_feature_data[seg][2] & 0x3);
            slice_param->seg_param[seg].segment_flags.fields.segment_reference_skipped =
                dec->vp9_seg_feature_enabled[seg][3] ? 1 : 0;
        }
        int32_t seg_qindex = base_qindex;
        if (seg_enabled && dec->vp9_seg_feature_enabled[seg][0]) {
            if (dec->vp9_seg_abs_delta)
                seg_qindex = dec->vp9_seg_feature_data[seg][0];
            else
                seg_qindex += dec->vp9_seg_feature_data[seg][0];
        }
        seg_qindex = clamp_i32(seg_qindex, 0, 255);
        bool seg_lossless =
            (seg_qindex == 0 && y_dc_delta_q == 0 && uv_dc_delta_q == 0 && uv_ac_delta_q == 0);
        int32_t q_y_dc = clamp_i32(seg_qindex + y_dc_delta_q, 0, 255);
        int32_t q_y_ac = seg_qindex;
        int32_t q_uv_dc = clamp_i32(seg_qindex + uv_dc_delta_q, 0, 255);
        int32_t q_uv_ac = clamp_i32(seg_qindex + uv_ac_delta_q, 0, 255);
        slice_param->seg_param[seg].luma_dc_quant_scale =
            seg_lossless ? 4 : g_vp9_dc_qlookup[q_y_dc];
        slice_param->seg_param[seg].luma_ac_quant_scale =
            seg_lossless ? 4 : g_vp9_ac_qlookup[q_y_ac];
        slice_param->seg_param[seg].chroma_dc_quant_scale =
            seg_lossless ? 4 : g_vp9_dc_qlookup[q_uv_dc];
        slice_param->seg_param[seg].chroma_ac_quant_scale =
            seg_lossless ? 4 : g_vp9_ac_qlookup[q_uv_ac];

        int32_t seg_lf = (int32_t)loop_filter_level;
        if (seg_enabled && dec->vp9_seg_feature_enabled[seg][1]) {
            if (dec->vp9_seg_abs_delta)
                seg_lf = dec->vp9_seg_feature_data[seg][1];
            else
                seg_lf += dec->vp9_seg_feature_data[seg][1];
        }
        seg_lf = clamp_i32(seg_lf, 0, 63);
        int32_t scale = 1 << (seg_lf >> 5);
        for (int r = 0; r < 4; ++r) {
            for (int m = 0; m < 2; ++m) {
                int32_t lvl = seg_lf;
                if (mode_ref_delta_enabled) {
                    lvl += dec->vp9_ref_deltas[r] * scale;
                    if (r > 0)
                        lvl += dec->vp9_mode_deltas[m] * scale;
                }
                slice_param->seg_param[seg].filter_level[r][m] = (uint8_t)clamp_i32(lvl, 0, 63);
            }
        }
    }

    *out_refresh_flags = (frame_type == 0) ? 0xFF : refresh_frame_flags;
    *out_is_keyframe = (frame_type == 0);
    return true;
}
