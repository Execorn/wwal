// SPDX-License-Identifier: GPL-3.0-or-later
#include "vaapi_internal.h"

/* ITU-T H.264 Table 7-3 and Table 7-4 default scaling lists */
static const uint8_t g_default_4x4_intra[16] = {6,  13, 13, 20, 20, 20, 28, 28,
                                                28, 28, 32, 32, 32, 37, 37, 42};
static const uint8_t g_default_4x4_inter[16] = {10, 14, 14, 20, 20, 20, 24, 24,
                                                24, 24, 27, 27, 27, 30, 30, 34};
static const uint8_t g_default_8x8_intra[64] = {
    6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
    25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31,
    31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
static const uint8_t g_default_8x8_inter[64] = {
    9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22,
    22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27,
    27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

static void parse_h264_scaling_list(bit_reader_t *br, uint8_t *dst, uint32_t size,
                                    const uint8_t *default_list)
{
    int32_t last_scale = 8, next_scale = 8;
    bool use_default = false;
    for (uint32_t j = 0; j < size; ++j) {
        if (next_scale != 0) {
            int32_t delta_scale = read_se(br);
            next_scale = (last_scale + delta_scale + 256) % 256;
            if (j == 0 && next_scale == 0) {
                use_default = true;
            }
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
        if (dst) {
            dst[j] = (uint8_t)last_scale;
        }
    }
    if (use_default && dst && default_list) {
        memcpy(dst, default_list, size);
    }
}

void refresh_cached_h264_iq(vaapi_decoder_t *dec)
{
    if (!dec)
        return;
    if (dec->cached_pps.valid && dec->cached_pps.pic_scaling_matrix_present_flag) {
        memcpy(dec->cached_iq.ScalingList4x4, dec->cached_pps.scaling_list_4x4,
               sizeof(dec->cached_iq.ScalingList4x4));
        memcpy(dec->cached_iq.ScalingList8x8, dec->cached_pps.scaling_list_8x8,
               sizeof(dec->cached_iq.ScalingList8x8));
    } else if (dec->cached_sps.valid && dec->cached_sps.seq_scaling_matrix_present_flag) {
        memcpy(dec->cached_iq.ScalingList4x4, dec->cached_sps.scaling_list_4x4,
               sizeof(dec->cached_iq.ScalingList4x4));
        memcpy(dec->cached_iq.ScalingList8x8, dec->cached_sps.scaling_list_8x8,
               sizeof(dec->cached_iq.ScalingList8x8));
    } else {
        memset(&dec->cached_iq, 16, sizeof(dec->cached_iq));
    }
    dec->iq_valid = true;
}

bool parse_h264_sps(const uint8_t *nal, size_t nal_len, parsed_sps_t *sps)
{
    if (nal_len < 4)
        return false;
    uint8_t rbsp[512];
    size_t rbsp_len = unescape_rbsp(nal, nal_len, rbsp, sizeof(rbsp));
    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    uint32_t forbidden = read_bit(&br);
    uint32_t ref_idc = read_bits(&br, 2);
    uint32_t nal_type = read_bits(&br, 5);
    (void)forbidden;
    (void)ref_idc;
    if (nal_type != 7)
        return false;

    memset(sps, 0, sizeof(*sps));
    memset(sps->scaling_list_4x4, 16, sizeof(sps->scaling_list_4x4));
    memset(sps->scaling_list_8x8, 16, sizeof(sps->scaling_list_8x8));
    sps->profile_idc = (uint8_t)read_bits(&br, 8);
    read_bits(&br, 8); /* constraint flags */
    sps->level_idc = (uint8_t)read_bits(&br, 8);
    read_ue(&br); /* seq_parameter_set_id */

    if (sps->profile_idc == 100 || sps->profile_idc == 110 || sps->profile_idc == 122 ||
        sps->profile_idc == 244 || sps->profile_idc == 44 || sps->profile_idc == 83 ||
        sps->profile_idc == 86 || sps->profile_idc == 118 || sps->profile_idc == 128 ||
        sps->profile_idc == 138) {
        sps->chroma_format_idc = (uint8_t)read_ue(&br);
        if (sps->chroma_format_idc == 3) {
            sps->residual_colour_transform_flag = read_bit(&br) != 0;
        }
        sps->bit_depth_luma_minus8 = (uint8_t)read_ue(&br);
        sps->bit_depth_chroma_minus8 = (uint8_t)read_ue(&br);
        read_bit(&br); /* qpprime_y_zero_transform_bypass_flag */
        uint32_t seq_scaling_matrix_present = read_bit(&br);
        sps->seq_scaling_matrix_present_flag = (seq_scaling_matrix_present != 0);
        if (seq_scaling_matrix_present) {
            uint32_t num_lists = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (uint32_t i = 0; i < num_lists; ++i) {
                uint8_t *dst = (i < 6) ? sps->scaling_list_4x4[i]
                                       : ((i < 8) ? sps->scaling_list_8x8[i - 6] : NULL);
                const uint8_t *def =
                    (i < 6) ? ((i < 3) ? g_default_4x4_intra : g_default_4x4_inter)
                            : ((i % 2 == 0) ? g_default_8x8_intra : g_default_8x8_inter);
                if (read_bit(&br)) {
                    parse_h264_scaling_list(&br, dst, (i < 6) ? 16 : 64, def);
                } else if (i < 6) {
                    if (i == 0 || i == 3) {
                        memcpy(sps->scaling_list_4x4[i], def, 16);
                    } else {
                        memcpy(sps->scaling_list_4x4[i], sps->scaling_list_4x4[i - 1], 16);
                    }
                } else if (i < 8) {
                    memcpy(sps->scaling_list_8x8[i - 6], def, 64);
                }
            }
        }
    } else {
        sps->chroma_format_idc = 1;
    }

    uint32_t log2_max_fn_m4 = read_ue(&br);
    if (log2_max_fn_m4 > 12)
        log2_max_fn_m4 = 12;
    sps->log2_max_frame_num_minus4 = (uint8_t)log2_max_fn_m4;
    sps->pic_order_cnt_type = (uint8_t)read_ue(&br);
    if (sps->pic_order_cnt_type == 0) {
        uint32_t log2_max_poc_m4 = read_ue(&br);
        if (log2_max_poc_m4 > 12)
            log2_max_poc_m4 = 12;
        sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)log2_max_poc_m4;
    } else if (sps->pic_order_cnt_type == 1) {
        sps->delta_pic_order_always_zero_flag = read_bit(&br) != 0;
        read_se(&br);
        read_se(&br);
        uint32_t num_ref = read_ue(&br);
        for (uint32_t i = 0; i < num_ref && i < 256 && br.bit_pos < br.size * 8; ++i) {
            read_se(&br);
        }
    }

    sps->num_ref_frames = (uint8_t)read_ue(&br);
    sps->gaps_in_frame_num_value_allowed_flag = read_bit(&br) != 0;
    sps->pic_width_in_mbs_minus1 = read_ue(&br);
    sps->pic_height_in_map_units_minus1 = read_ue(&br);
    sps->frame_mbs_only_flag = read_bit(&br) != 0;
    if (!sps->frame_mbs_only_flag) {
        sps->mb_adaptive_frame_field_flag = read_bit(&br) != 0;
    }
    sps->direct_8x8_inference_flag = read_bit(&br) != 0;
    if (br.bit_pos > br.size * 8 ||
        (sps->pic_width_in_mbs_minus1 == 0 && sps->pic_height_in_map_units_minus1 == 0)) {
        return false;
    }

    if (read_bit(&br)) { /* frame_cropping_flag */
        read_ue(&br);    /* frame_crop_left_offset */
        read_ue(&br);    /* frame_crop_right_offset */
        read_ue(&br);    /* frame_crop_top_offset */
        read_ue(&br);    /* frame_crop_bottom_offset */
    }
    if (read_bit(&br) && more_rbsp_data(&br)) { /* vui_parameters_present_flag */
        if (read_bit(&br)) {                    /* aspect_ratio_info_present_flag */
            uint32_t aspect_ratio_idc = read_bits(&br, 8);
            if (aspect_ratio_idc == 255) {
                read_bits(&br, 16);
                read_bits(&br, 16);
            }
        }
        if (read_bit(&br)) { /* overscan_info_present_flag */
            read_bit(&br);
        }
        if (read_bit(&br)) { /* video_signal_type_present_flag */
            read_bits(&br, 3);
            sps->video_full_range_flag = (read_bit(&br) != 0);
            if (read_bit(&br)) { /* colour_description_present_flag */
                sps->colour_primaries = (uint8_t)read_bits(&br, 8);
                sps->transfer_characteristics = (uint8_t)read_bits(&br, 8);
                sps->matrix_coefficients = (uint8_t)read_bits(&br, 8);
            }
        }
    }

    sps->valid = true;
    return true;
}

bool parse_h264_pps(const uint8_t *nal, size_t nal_len, const parsed_sps_t *sps, parsed_pps_t *pps)
{
    if (nal_len < 2)
        return false;
    uint8_t rbsp[512];
    size_t rbsp_len = unescape_rbsp(nal, nal_len, rbsp, sizeof(rbsp));
    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    uint32_t forbidden = read_bit(&br);
    uint32_t ref_idc = read_bits(&br, 2);
    uint32_t nal_type = read_bits(&br, 5);
    (void)forbidden;
    (void)ref_idc;
    if (nal_type != 8)
        return false;

    memset(pps, 0, sizeof(*pps));
    if (sps && sps->valid && sps->seq_scaling_matrix_present_flag) {
        memcpy(pps->scaling_list_4x4, sps->scaling_list_4x4, sizeof(pps->scaling_list_4x4));
        memcpy(pps->scaling_list_8x8, sps->scaling_list_8x8, sizeof(pps->scaling_list_8x8));
    } else {
        memset(pps->scaling_list_4x4, 16, sizeof(pps->scaling_list_4x4));
        memset(pps->scaling_list_8x8, 16, sizeof(pps->scaling_list_8x8));
    }
    read_ue(&br);
    read_ue(&br);
    pps->entropy_coding_mode_flag = (uint8_t)read_bit(&br);
    pps->bottom_field_pic_order_in_frame_present_flag = read_bit(&br) != 0;
    pps->num_slice_groups_minus1 = (uint8_t)read_ue(&br);
    pps->num_ref_idx_l0_default_active_minus1 = (uint8_t)read_ue(&br);
    pps->num_ref_idx_l1_default_active_minus1 = (uint8_t)read_ue(&br);
    pps->weighted_pred_flag = read_bit(&br) != 0;
    pps->weighted_bipred_idc = (uint8_t)read_bits(&br, 2);
    pps->pic_init_qp_minus26 = (int8_t)read_se(&br);
    pps->pic_init_qs_minus26 = (int8_t)read_se(&br);
    pps->chroma_qp_index_offset = (int8_t)read_se(&br);
    pps->deblocking_filter_control_present_flag = read_bit(&br) != 0;
    pps->constrained_intra_pred_flag = read_bit(&br) != 0;
    pps->redundant_pic_cnt_present_flag = read_bit(&br) != 0;
    if (more_rbsp_data(&br)) {
        pps->transform_8x8_mode_flag = read_bit(&br) != 0;
        uint32_t pic_scaling_matrix_present = read_bit(&br);
        pps->pic_scaling_matrix_present_flag = (pic_scaling_matrix_present != 0);
        if (pic_scaling_matrix_present) {
            uint32_t count = 6 + 2 * pps->transform_8x8_mode_flag;
            bool has_sps_matrix = (sps && sps->valid && sps->seq_scaling_matrix_present_flag);
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t *dst = (i < 6) ? pps->scaling_list_4x4[i]
                                       : ((i < 8) ? pps->scaling_list_8x8[i - 6] : NULL);
                const uint8_t *def =
                    (i < 6) ? ((i < 3) ? g_default_4x4_intra : g_default_4x4_inter)
                            : ((i % 2 == 0) ? g_default_8x8_intra : g_default_8x8_inter);
                if (read_bit(&br)) {
                    parse_h264_scaling_list(&br, dst, (i < 6) ? 16 : 64, def);
                } else if (i < 6) {
                    if (i == 0 || i == 3) {
                        if (!has_sps_matrix) {
                            memcpy(pps->scaling_list_4x4[i], def, 16);
                        }
                    } else {
                        memcpy(pps->scaling_list_4x4[i], pps->scaling_list_4x4[i - 1], 16);
                    }
                } else if (i < 8 && !has_sps_matrix) {
                    memcpy(pps->scaling_list_8x8[i - 6], def, 64);
                }
            }
        }
        pps->second_chroma_qp_index_offset = (int8_t)read_se(&br);
    } else {
        pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    }
    pps->valid = true;
    return true;
}

bool parse_h264_slice_header(const uint8_t *nal, size_t nal_len, const parsed_sps_t *sps,
                             const parsed_pps_t *pps, VASliceParameterBufferH264 *sp,
                             slice_header_meta_t *meta)
{
    if (!nal || nal_len < 2 || !sps || !pps || !sp)
        return false;

    uint8_t rbsp[4096];
    size_t cap = nal_len - 1 < sizeof(rbsp) ? nal_len - 1 : sizeof(rbsp);
    size_t rbsp_len = unescape_rbsp(nal + 1, cap, rbsp, sizeof(rbsp));
    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    uint8_t nal_type = nal[0] & 0x1F;
    uint8_t nal_ref_idc = (nal[0] >> 5) & 3;

    sp->first_mb_in_slice = (uint16_t)read_ue(&br);
    uint32_t stype = read_ue(&br);
    sp->slice_type = (uint8_t)(stype % 5);
    read_ue(&br); /* pic_parameter_set_id */

    uint32_t log2_max_fn_m4 = sps->valid ? sps->log2_max_frame_num_minus4 : 4;
    if (log2_max_fn_m4 > 12)
        log2_max_fn_m4 = 12;
    uint32_t fn_bits = log2_max_fn_m4 + 4;
    uint32_t slice_fn = read_bits(&br, fn_bits);
    if (meta) {
        meta->frame_num = slice_fn;
    }

    uint32_t field_pic = 0;
    if (sps->valid && !sps->frame_mbs_only_flag) {
        field_pic = read_bit(&br);
        if (field_pic)
            read_bit(&br);
    }

    if (nal_type == 5) {
        read_ue(&br); /* idr_pic_id */
    }

    uint8_t poc_type = sps->valid ? sps->pic_order_cnt_type : 0;
    if (poc_type == 0) {
        uint32_t log2_max_poc_m4 = sps->valid ? sps->log2_max_pic_order_cnt_lsb_minus4 : 4;
        if (log2_max_poc_m4 > 12)
            log2_max_poc_m4 = 12;
        uint32_t poc_bits = log2_max_poc_m4 + 4;
        int32_t poc_lsb = (int32_t)read_bits(&br, poc_bits);
        if (meta) {
            meta->poc_lsb = poc_lsb;
        }
        if (pps->bottom_field_pic_order_in_frame_present_flag && !field_pic) {
            read_se(&br);
        }
    } else if (poc_type == 1 && !sps->delta_pic_order_always_zero_flag) {
        read_se(&br);
        if (pps->bottom_field_pic_order_in_frame_present_flag && !field_pic) {
            read_se(&br);
        }
    }

    if (pps->redundant_pic_cnt_present_flag) {
        read_ue(&br);
    }

    if (sp->slice_type == 1) { /* B-slice */
        sp->direct_spatial_mv_pred_flag = (uint8_t)read_bit(&br);
    }

    if (sp->slice_type == 0 || sp->slice_type == 1 || sp->slice_type == 3) { /* P, B, SP */
        if (read_bit(&br)) { /* num_ref_idx_active_override_flag */
            sp->num_ref_idx_l0_active_minus1 = (uint8_t)read_ue(&br);
            if (sp->slice_type == 1)
                sp->num_ref_idx_l1_active_minus1 = (uint8_t)read_ue(&br);
        } else {
            sp->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
            sp->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
        }
        if (sp->num_ref_idx_l0_active_minus1 > 31)
            sp->num_ref_idx_l0_active_minus1 = 31;
        if (sp->num_ref_idx_l1_active_minus1 > 31)
            sp->num_ref_idx_l1_active_minus1 = 31;
    }

    /* ref_pic_list_modification() */
    if (meta) {
        meta->ref_pic_list_mod_flag_l0 = false;
        meta->num_ref_mod_l0 = 0;
        meta->ref_pic_list_mod_flag_l1 = false;
        meta->num_ref_mod_l1 = 0;
    }
    if (sp->slice_type != 2 && sp->slice_type != 4) {
        if (read_bit(&br)) { /* ref_pic_list_modification_flag_l0 */
            if (meta)
                meta->ref_pic_list_mod_flag_l0 = true;
            uint32_t mod_idc;
            do {
                mod_idc = read_ue(&br);
                if (mod_idc == 0 || mod_idc == 1 || mod_idc == 2) {
                    uint32_t val = read_ue(&br);
                    if (meta && meta->num_ref_mod_l0 < 32) {
                        meta->mod_of_pic_nums_idc_l0[meta->num_ref_mod_l0] = (uint8_t)mod_idc;
                        meta->abs_diff_pic_num_l0[meta->num_ref_mod_l0] = val;
                        meta->num_ref_mod_l0++;
                    }
                }
            } while (mod_idc != 3 && br.bit_pos < br.size * 8);
        }
        if (sp->slice_type == 1) {
            if (read_bit(&br)) { /* ref_pic_list_modification_flag_l1 */
                if (meta)
                    meta->ref_pic_list_mod_flag_l1 = true;
                uint32_t mod_idc;
                do {
                    mod_idc = read_ue(&br);
                    if (mod_idc == 0 || mod_idc == 1 || mod_idc == 2) {
                        uint32_t val = read_ue(&br);
                        if (meta && meta->num_ref_mod_l1 < 32) {
                            meta->mod_of_pic_nums_idc_l1[meta->num_ref_mod_l1] = (uint8_t)mod_idc;
                            meta->abs_diff_pic_num_l1[meta->num_ref_mod_l1] = val;
                            meta->num_ref_mod_l1++;
                        }
                    }
                } while (mod_idc != 3 && br.bit_pos < br.size * 8);
            }
        }
    }

    /* pred_weight_table() */
    if ((pps->weighted_pred_flag && (sp->slice_type == 0 || sp->slice_type == 3)) ||
        (pps->weighted_bipred_idc == 1 && sp->slice_type == 1)) {
        uint32_t luma_denom = read_ue(&br);
        if (luma_denom > 7)
            luma_denom = 7;
        sp->luma_log2_weight_denom = (uint8_t)luma_denom;
        if (sps->chroma_format_idc != 0) {
            uint32_t chroma_denom = read_ue(&br);
            if (chroma_denom > 7)
                chroma_denom = 7;
            sp->chroma_log2_weight_denom = (uint8_t)chroma_denom;
        }
        sp->luma_weight_l0_flag = 0;
        sp->chroma_weight_l0_flag = 0;
        for (uint32_t i = 0; i <= sp->num_ref_idx_l0_active_minus1; ++i) {
            uint8_t luma_w_flag = (uint8_t)read_bit(&br);
            sp->luma_weight_l0_flag |= luma_w_flag;
            if (luma_w_flag) {
                sp->luma_weight_l0[i] = (int16_t)read_se(&br);
                sp->luma_offset_l0[i] = (int16_t)read_se(&br);
            } else {
                sp->luma_weight_l0[i] = (int16_t)(1U << sp->luma_log2_weight_denom);
                sp->luma_offset_l0[i] = 0;
            }
            if (sps->chroma_format_idc != 0) {
                uint8_t chroma_w_flag = (uint8_t)read_bit(&br);
                sp->chroma_weight_l0_flag |= chroma_w_flag;
                if (chroma_w_flag) {
                    for (int j = 0; j < 2; ++j) {
                        sp->chroma_weight_l0[i][j] = (int16_t)read_se(&br);
                        sp->chroma_offset_l0[i][j] = (int16_t)read_se(&br);
                    }
                } else {
                    for (int j = 0; j < 2; ++j) {
                        sp->chroma_weight_l0[i][j] = (int16_t)(1U << sp->chroma_log2_weight_denom);
                        sp->chroma_offset_l0[i][j] = 0;
                    }
                }
            }
        }
        if (sp->slice_type == 1) {
            sp->luma_weight_l1_flag = 0;
            sp->chroma_weight_l1_flag = 0;
            for (uint32_t i = 0; i <= sp->num_ref_idx_l1_active_minus1; ++i) {
                uint8_t luma_w_flag = (uint8_t)read_bit(&br);
                sp->luma_weight_l1_flag |= luma_w_flag;
                if (luma_w_flag) {
                    sp->luma_weight_l1[i] = (int16_t)read_se(&br);
                    sp->luma_offset_l1[i] = (int16_t)read_se(&br);
                } else {
                    sp->luma_weight_l1[i] = (int16_t)(1U << sp->luma_log2_weight_denom);
                    sp->luma_offset_l1[i] = 0;
                }
                if (sps->chroma_format_idc != 0) {
                    uint8_t chroma_w_flag = (uint8_t)read_bit(&br);
                    sp->chroma_weight_l1_flag |= chroma_w_flag;
                    if (chroma_w_flag) {
                        for (int j = 0; j < 2; ++j) {
                            sp->chroma_weight_l1[i][j] = (int16_t)read_se(&br);
                            sp->chroma_offset_l1[i][j] = (int16_t)read_se(&br);
                        }
                    } else {
                        for (int j = 0; j < 2; ++j) {
                            sp->chroma_weight_l1[i][j] =
                                (int16_t)(1U << sp->chroma_log2_weight_denom);
                            sp->chroma_offset_l1[i][j] = 0;
                        }
                    }
                }
            }
        }
    }

    /* dec_ref_pic_marking() */
    if (nal_ref_idc != 0) {
        if (nal_type == 5) {
            read_bit(&br); /* no_output_of_prior_pics_flag */
            read_bit(&br); /* long_term_reference_flag */
        } else {
            if (read_bit(&br)) { /* adaptive_ref_pic_marking_mode_flag */
                uint32_t mmco;
                int32_t max_fn = (int32_t)(1U << fn_bits);
                do {
                    mmco = read_ue(&br);
                    if (mmco == 1 || mmco == 3) {
                        uint32_t diff_minus1 = read_ue(&br);
                        if (mmco == 1 && meta && meta->num_unmark < 16) {
                            int32_t pn =
                                ((int32_t)slice_fn - (int32_t)(diff_minus1 + 1)) & (max_fn - 1);
                            meta->unmark_pic_nums[meta->num_unmark++] = pn;
                        }
                    }
                    if (mmco == 2)
                        read_ue(&br);
                    if (mmco == 3)
                        read_ue(&br);
                    if (mmco == 4)
                        read_ue(&br);
                    if (mmco == 5 && meta)
                        meta->mmco_reset = true;
                    if (mmco == 6)
                        read_ue(&br);
                } while (mmco != 0 && br.bit_pos < br.size * 8);
            }
        }
    }

    if (pps->entropy_coding_mode_flag && sp->slice_type != 2 && sp->slice_type != 4) {
        sp->cabac_init_idc = (uint8_t)read_ue(&br);
    }

    sp->slice_qp_delta = (int8_t)read_se(&br);

    if (pps->deblocking_filter_control_present_flag) {
        sp->disable_deblocking_filter_idc = (uint8_t)read_ue(&br);
        if (sp->disable_deblocking_filter_idc != 1) {
            sp->slice_alpha_c0_offset_div2 = (int8_t)read_se(&br);
            sp->slice_beta_offset_div2 = (int8_t)read_se(&br);
        }
    }

    if (pps->entropy_coding_mode_flag && br.bit_pos < br.end_bit_pos) {
        while ((br.bit_pos & 7u) != 0 && br.bit_pos < br.end_bit_pos) {
            read_bit(&br); /* cabac_alignment_one_bit */
        }
    }

    /* Calculate raw EBSP bitstream bit offset including emulation_prevention_three_byte */
    size_t ebsp_payload_bits = rbsp_bit_offset_to_ebsp_bits(nal + 1, nal_len - 1, br.bit_pos);
    sp->slice_data_bit_offset = (uint32_t)(8 + ebsp_payload_bits);
    return true;
}

static inline size_t find_annexb_start_code(const uint8_t *data, size_t start, size_t size)
{
    size_t pos = start;
#if defined(__AVX2__) && !defined(WWAL_DISABLE_SIMD)
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_one = _mm256_set1_epi8(1);
    while (pos + 32 <= size) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(data + pos));
        uint32_t zmask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_zero));
        uint32_t omask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_one));
        uint32_t sc_mask = zmask & (zmask >> 1) & (omask >> 2);
        if (sc_mask != 0) {
            size_t q = pos + (uint32_t)__builtin_ctz(sc_mask);
            return (q > start && data[q - 1] == 0) ? (q - 1) : q;
        }
        pos += 30;
    }
#endif
    while (pos + 3 <= size) {
        if (data[pos] == 0 && data[pos + 1] == 0 &&
            (data[pos + 2] == 1 || (pos + 3 < size && data[pos + 2] == 0 && data[pos + 3] == 1))) {
            return pos;
        }
        pos++;
    }
    return size;
}

bool next_h264_nal(const uint8_t *data, size_t size, bool is_avcc, uint8_t nal_len_sz,
                   size_t *scan_off, const uint8_t **out_nal, uint32_t *out_len)
{
    if (*scan_off >= size)
        return false;

    if (is_avcc) {
        if (nal_len_sz == 0)
            nal_len_sz = 4;
        if (*scan_off + nal_len_sz > size)
            return false;
        uint32_t len = 0;
        for (uint8_t i = 0; i < nal_len_sz; ++i) {
            len = (len << 8) | data[*scan_off + i];
        }
        *scan_off += nal_len_sz;
        if (len == 0 || *scan_off + len > size)
            return false;
        *out_nal = data + *scan_off;
        *out_len = len;
        *scan_off += len;
        return true;
    }

    /* Annex-B start code or fallback 4-byte length */
    size_t off = *scan_off;
    if (off + 4 <= size && !(data[off] == 0 && data[off + 1] == 0 &&
                             (data[off + 2] == 1 || (data[off + 2] == 0 && data[off + 3] == 1)))) {
        uint32_t len = ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
                       ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
        if (len > 0 && len <= size - (off + 4)) {
            *out_nal = data + off + 4;
            *out_len = len;
            *scan_off = off + 4 + len;
            return true;
        }
    }

    while (off + 3 <= size) {
        off = find_annexb_start_code(data, off, size);
        if (off + 3 > size)
            break;
        size_t hdr_len = (data[off + 2] == 1) ? 3 : 4;
        size_t nal_start = off + hdr_len;
        size_t next_sc = find_annexb_start_code(data, nal_start, size);
        if (next_sc > nal_start) {
            *out_nal = data + nal_start;
            *out_len = (uint32_t)(next_sc - nal_start);
            *scan_off = next_sc;
            return true;
        }
        off = nal_start;
    }
    *scan_off = size;
    return false;
}

void fill_va_pic_h264(VAPictureH264 *vp, const h264_dpb_entry_t *e)
{
    vp->picture_id = e->surface;
    vp->frame_idx = (uint16_t)e->frame_num;
    vp->flags = e->is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    vp->TopFieldOrderCnt = e->pic_order_cnt;
    vp->BottomFieldOrderCnt = e->pic_order_cnt;
}

static void apply_h264_ref_list_mod_single(const h264_dpb_t *dpb, int32_t curr_fn, int32_t max_fn,
                                           uint8_t num_active_minus1, uint8_t num_mods,
                                           const uint8_t *mod_idc, const uint32_t *abs_diff_val,
                                           VAPictureH264 *ref_list)
{
    if (!dpb || !mod_idc || !abs_diff_val || !ref_list || max_fn <= 0)
        return;

    uint32_t num_active = (uint32_t)num_active_minus1 + 1U;
    if (num_active > 32U)
        num_active = 32U;

    VAPictureH264 list[33];
    for (uint32_t i = 0; i < 32U; ++i)
        list[i] = ref_list[i];
    list[32].picture_id = VA_INVALID_SURFACE;
    list[32].flags = VA_PICTURE_H264_INVALID;

    int32_t pic_num_pred = curr_fn;
    uint32_t ref_idx = 0;

    for (uint8_t m = 0; m < num_mods && ref_idx < num_active; ++m) {
        uint8_t idc = mod_idc[m];
        uint32_t val = abs_diff_val[m];
        const h264_dpb_entry_t *found = NULL;

        if (idc == 0 || idc == 1) {
            int32_t diff = (int32_t)((val & 0x3FFFFFFFU) + 1U);
            int32_t pic_num_no_wrap;
            if (idc == 0) {
                pic_num_no_wrap = pic_num_pred - diff;
                if (pic_num_no_wrap < 0)
                    pic_num_no_wrap += max_fn;
            } else {
                pic_num_no_wrap = pic_num_pred + diff;
                if (pic_num_no_wrap >= max_fn)
                    pic_num_no_wrap -= max_fn;
            }
            pic_num_no_wrap &= (max_fn - 1);
            pic_num_pred = pic_num_no_wrap;

            for (size_t d = 0; d < dpb->count; ++d) {
                if (!dpb->entries[d].is_long_term && dpb->entries[d].frame_num == pic_num_no_wrap) {
                    found = &dpb->entries[d];
                    break;
                }
            }
        } else if (idc == 2) {
            for (size_t d = 0; d < dpb->count; ++d) {
                if (dpb->entries[d].is_long_term && dpb->entries[d].frame_num == (int32_t)val) {
                    found = &dpb->entries[d];
                    break;
                }
            }
        } else {
            break;
        }

        if (found) {
            for (uint32_t c_idx = num_active; c_idx > ref_idx; --c_idx) {
                list[c_idx] = list[c_idx - 1U];
            }
            fill_va_pic_h264(&list[ref_idx++], found);
            uint32_t n_idx = ref_idx;
            for (uint32_t c_idx = ref_idx; c_idx <= num_active; ++c_idx) {
                if (list[c_idx].picture_id != found->surface) {
                    list[n_idx++] = list[c_idx];
                }
            }
            while (n_idx <= num_active) {
                list[n_idx].picture_id = VA_INVALID_SURFACE;
                list[n_idx].flags = VA_PICTURE_H264_INVALID;
                n_idx++;
            }
        }
    }

    for (uint32_t i = 0; i < 32U; ++i)
        ref_list[i] = list[i];
}

void apply_h264_ref_pic_list_mod(const h264_dpb_t *dpb, int32_t curr_fn, int32_t max_fn,
                                 const slice_header_meta_t *meta, VASliceParameterBufferH264 *sp)
{
    if (!dpb || !meta || !sp || sp->slice_type == 2 || sp->slice_type == 4)
        return;

    if (meta->ref_pic_list_mod_flag_l0 && meta->num_ref_mod_l0 > 0) {
        apply_h264_ref_list_mod_single(dpb, curr_fn, max_fn, sp->num_ref_idx_l0_active_minus1,
                                       meta->num_ref_mod_l0, meta->mod_of_pic_nums_idc_l0,
                                       meta->abs_diff_pic_num_l0, sp->RefPicList0);
    }
    if (sp->slice_type == 1 && meta->ref_pic_list_mod_flag_l1 && meta->num_ref_mod_l1 > 0) {
        apply_h264_ref_list_mod_single(dpb, curr_fn, max_fn, sp->num_ref_idx_l1_active_minus1,
                                       meta->num_ref_mod_l1, meta->mod_of_pic_nums_idc_l1,
                                       meta->abs_diff_pic_num_l1, sp->RefPicList1);
    }
}

void build_h264_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_fn,
                          int32_t max_fn, int32_t curr_poc, VASliceParameterBufferH264 *sp)
{
    for (int i = 0; i < 32; ++i) {
        sp->RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        sp->RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        sp->RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        sp->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

    if (dpb->count == 0 || slice_type == 2 || slice_type == 4)
        return;

    if (slice_type == 0 || slice_type == 3) {
        /* P-slice: sort short-term references by descending PicNum */
        h264_dpb_entry_t sorted[16];
        size_t n = dpb->count > 16 ? 16 : dpb->count;
        memcpy(sorted, dpb->entries, n * sizeof(h264_dpb_entry_t));
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                int32_t pn_i = sorted[i].frame_num > curr_fn ? sorted[i].frame_num - max_fn
                                                             : sorted[i].frame_num;
                int32_t pn_j = sorted[j].frame_num > curr_fn ? sorted[j].frame_num - max_fn
                                                             : sorted[j].frame_num;
                if (pn_j > pn_i) {
                    h264_dpb_entry_t tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            fill_va_pic_h264(&sp->RefPicList0[i], &sorted[i]);
        }
    } else if (slice_type == 1) {
        /* B-slice: L0 = {POC <= curr descending, POC > curr ascending}
         *          L1 = {POC > curr ascending, POC <= curr descending} */
        h264_dpb_entry_t before[16], after[16];
        size_t n_before = 0, n_after = 0;
        size_t n = dpb->count > 16 ? 16 : dpb->count;
        for (size_t i = 0; i < n; ++i) {
            if (dpb->entries[i].pic_order_cnt <= curr_poc) {
                before[n_before++] = dpb->entries[i];
            } else {
                after[n_after++] = dpb->entries[i];
            }
        }
        for (size_t i = 0; i < n_before; ++i) {
            for (size_t j = i + 1; j < n_before; ++j) {
                if (before[j].pic_order_cnt > before[i].pic_order_cnt) {
                    h264_dpb_entry_t tmp = before[i];
                    before[i] = before[j];
                    before[j] = tmp;
                }
            }
        }
        for (size_t i = 0; i < n_after; ++i) {
            for (size_t j = i + 1; j < n_after; ++j) {
                if (after[j].pic_order_cnt < after[i].pic_order_cnt) {
                    h264_dpb_entry_t tmp = after[i];
                    after[i] = after[j];
                    after[j] = tmp;
                }
            }
        }

        size_t idx0 = 0;
        for (size_t i = 0; i < n_before; ++i)
            fill_va_pic_h264(&sp->RefPicList0[idx0++], &before[i]);
        for (size_t i = 0; i < n_after; ++i)
            fill_va_pic_h264(&sp->RefPicList0[idx0++], &after[i]);

        size_t idx1 = 0;
        for (size_t i = 0; i < n_after; ++i)
            fill_va_pic_h264(&sp->RefPicList1[idx1++], &after[i]);
        for (size_t i = 0; i < n_before; ++i)
            fill_va_pic_h264(&sp->RefPicList1[idx1++], &before[i]);

        if (idx1 > 1 && idx0 == idx1) {
            bool identical = true;
            for (size_t i = 0; i < idx1; ++i) {
                if (sp->RefPicList0[i].picture_id != sp->RefPicList1[i].picture_id) {
                    identical = false;
                    break;
                }
            }
            if (identical) {
                VAPictureH264 tmp = sp->RefPicList1[0];
                sp->RefPicList1[0] = sp->RefPicList1[1];
                sp->RefPicList1[1] = tmp;
            }
        }
    }
}
