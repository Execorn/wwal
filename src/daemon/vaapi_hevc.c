// SPDX-License-Identifier: GPL-3.0-or-later
#include "vaapi_internal.h"

static void skip_hevc_profile_tier_level(bs_t *br, bool profile_present,
                                         uint32_t max_sub_layers_minus1)
{
    if (profile_present) {
        read_bits(br, 8);
        read_bits(br, 16);
        read_bits(br, 16);
        read_bits(br, 16);
        read_bits(br, 16);
        read_bits(br, 16);
    }
    read_bits(br, 8);
    bool sub_profile[8] = {false};
    bool sub_level[8] = {false};
    for (uint32_t i = 0; i < max_sub_layers_minus1 && i < 7; ++i) {
        sub_profile[i] = (read_bit(br) != 0);
        sub_level[i] = (read_bit(br) != 0);
    }
    if (max_sub_layers_minus1 > 0) {
        for (uint32_t i = max_sub_layers_minus1; i < 8; ++i) {
            read_bits(br, 2);
        }
    }
    for (uint32_t i = 0; i < max_sub_layers_minus1 && i < 7; ++i) {
        if (sub_profile[i]) {
            read_bits(br, 8);
            read_bits(br, 16);
            read_bits(br, 16);
            read_bits(br, 16);
            read_bits(br, 16);
            read_bits(br, 16);
        }
        if (sub_level[i]) {
            read_bits(br, 8);
        }
    }
}

static void skip_hevc_scaling_list_data(bs_t *br)
{
    for (uint32_t size_id = 0; size_id < 4; ++size_id) {
        uint32_t num_matrices = (size_id == 3) ? 2 : 6;
        for (uint32_t matrix_id = 0; matrix_id < num_matrices; ++matrix_id) {
            if (!read_bit(br)) {
                read_ue(br);
            } else {
                uint32_t coef_num = 1U << (4 + (size_id << 1));
                if (coef_num > 64)
                    coef_num = 64;
                if (size_id > 1) {
                    read_se(br);
                }
                for (uint32_t k = 0; k < coef_num; ++k) {
                    read_se(br);
                }
            }
        }
    }
}

static void parse_hevc_st_ref_pic_set(bs_t *br, uint32_t st_rps_idx,
                                      uint32_t num_short_term_ref_pic_sets,
                                      hevc_st_rps_t *rps_table, uint8_t *num_neg, uint8_t *num_pos)
{
    if (!rps_table || st_rps_idx > 64)
        return;
    hevc_st_rps_t *curr = &rps_table[st_rps_idx];
    memset(curr, 0, sizeof(*curr));

    bool inter_pred = (st_rps_idx != 0) ? (read_bit(br) != 0) : false;
    if (inter_pred) {
        uint32_t delta_idx_minus1 = 0;
        if (st_rps_idx == num_short_term_ref_pic_sets) {
            delta_idx_minus1 = read_ue(br);
        }
        uint32_t delta_rps_sign = read_bit(br);
        uint32_t abs_delta_rps_minus1 = read_ue(br);
        int32_t delta_rps = (1 - 2 * (int32_t)delta_rps_sign) * ((int32_t)abs_delta_rps_minus1 + 1);

        uint32_t ref_idx =
            (st_rps_idx > delta_idx_minus1) ? (st_rps_idx - (delta_idx_minus1 + 1)) : 0;
        if (ref_idx > 64)
            ref_idx = 0;
        const hevc_st_rps_t *ref = &rps_table[ref_idx];
        uint32_t ref_neg = ref->num_negative_pics;
        uint32_t ref_pos = ref->num_positive_pics;
        uint32_t num_delta_pocs = ref_neg + ref_pos;
        if (num_delta_pocs > 32)
            num_delta_pocs = 32;

        uint8_t used_by_curr[33] = {0};
        uint8_t use_delta_flag[33] = {0};
        for (uint32_t j = 0; j <= num_delta_pocs; ++j) {
            used_by_curr[j] = (uint8_t)read_bit(br);
            if (!used_by_curr[j]) {
                use_delta_flag[j] = (uint8_t)read_bit(br);
            } else {
                use_delta_flag[j] = 1;
            }
        }

        uint32_t i = 0;
        for (int32_t j = (int32_t)ref_pos - 1; j >= 0; --j) {
            int32_t d_poc = ref->delta_poc_s1[j] + delta_rps;
            if (d_poc < 0 && use_delta_flag[ref_neg + (uint32_t)j] && i < 16) {
                curr->delta_poc_s0[i] = d_poc;
                curr->used_by_curr_pic_s0[i] = used_by_curr[ref_neg + (uint32_t)j];
                i++;
            }
        }
        if (delta_rps < 0 && use_delta_flag[num_delta_pocs] && i < 16) {
            curr->delta_poc_s0[i] = delta_rps;
            curr->used_by_curr_pic_s0[i] = used_by_curr[num_delta_pocs];
            i++;
        }
        for (uint32_t j = 0; j < ref_neg; ++j) {
            int32_t d_poc = ref->delta_poc_s0[j] + delta_rps;
            if (d_poc < 0 && use_delta_flag[j] && i < 16) {
                curr->delta_poc_s0[i] = d_poc;
                curr->used_by_curr_pic_s0[i] = used_by_curr[j];
                i++;
            }
        }
        curr->num_negative_pics = (uint8_t)i;

        i = 0;
        for (int32_t j = (int32_t)ref_neg - 1; j >= 0; --j) {
            int32_t d_poc = ref->delta_poc_s0[j] + delta_rps;
            if (d_poc > 0 && use_delta_flag[j] && i < 16) {
                curr->delta_poc_s1[i] = d_poc;
                curr->used_by_curr_pic_s1[i] = used_by_curr[j];
                i++;
            }
        }
        if (delta_rps > 0 && use_delta_flag[num_delta_pocs] && i < 16) {
            curr->delta_poc_s1[i] = delta_rps;
            curr->used_by_curr_pic_s1[i] = used_by_curr[num_delta_pocs];
            i++;
        }
        for (uint32_t j = 0; j < ref_pos; ++j) {
            int32_t d_poc = ref->delta_poc_s1[j] + delta_rps;
            if (d_poc > 0 && use_delta_flag[ref_neg + j] && i < 16) {
                curr->delta_poc_s1[i] = d_poc;
                curr->used_by_curr_pic_s1[i] = used_by_curr[ref_neg + j];
                i++;
            }
        }
        curr->num_positive_pics = (uint8_t)i;
    } else {
        uint32_t neg = read_ue(br);
        uint32_t pos = read_ue(br);
        if (neg > 16)
            neg = 16;
        if (pos > 16)
            pos = 16;
        curr->num_negative_pics = (uint8_t)neg;
        curr->num_positive_pics = (uint8_t)pos;
        for (uint32_t j = 0; j < neg; ++j) {
            uint32_t delta_poc_s0_minus1 = read_ue(br);
            curr->used_by_curr_pic_s0[j] = (uint8_t)read_bit(br);
            curr->delta_poc_s0[j] =
                (j == 0) ? -((int32_t)delta_poc_s0_minus1 + 1)
                         : (curr->delta_poc_s0[j - 1] - ((int32_t)delta_poc_s0_minus1 + 1));
        }
        for (uint32_t j = 0; j < pos; ++j) {
            uint32_t delta_poc_s1_minus1 = read_ue(br);
            curr->used_by_curr_pic_s1[j] = (uint8_t)read_bit(br);
            curr->delta_poc_s1[j] =
                (j == 0) ? ((int32_t)delta_poc_s1_minus1 + 1)
                         : (curr->delta_poc_s1[j - 1] + ((int32_t)delta_poc_s1_minus1 + 1));
        }
    }
    if (num_neg)
        num_neg[st_rps_idx] = curr->num_negative_pics;
    if (num_pos)
        num_pos[st_rps_idx] = curr->num_positive_pics;
}

bool parse_hevc_sps(const uint8_t *nal, size_t nal_len, parsed_hevc_sps_t *sps)
{
    if (!nal || nal_len < 4 || !sps)
        return false;
    uint8_t nal_type = (nal[0] >> 1) & 0x3F;
    if (nal_type != 33)
        return false;

    uint8_t rbsp[1024];
    size_t rbsp_len = unescape_rbsp(nal + 2, nal_len - 2, rbsp, sizeof(rbsp));
    bs_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    memset(sps, 0, sizeof(*sps));
    read_bits(&br, 4);
    sps->sps_max_sub_layers_minus1 = (uint8_t)read_bits(&br, 3);
    if (sps->sps_max_sub_layers_minus1 > 6)
        sps->sps_max_sub_layers_minus1 = 6;
    read_bit(&br);
    skip_hevc_profile_tier_level(&br, true, sps->sps_max_sub_layers_minus1);

    read_ue(&br);
    sps->chroma_format_idc = (uint8_t)read_ue(&br);
    if (sps->chroma_format_idc == 3) {
        sps->separate_colour_plane_flag = (read_bit(&br) != 0);
    }
    sps->pic_width_in_luma_samples = (uint16_t)read_ue(&br);
    sps->pic_height_in_luma_samples = (uint16_t)read_ue(&br);
    if (read_bit(&br)) {
        read_ue(&br);
        read_ue(&br);
        read_ue(&br);
        read_ue(&br);
    }
    sps->bit_depth_luma_minus8 = (uint8_t)read_ue(&br);
    sps->bit_depth_chroma_minus8 = (uint8_t)read_ue(&br);
    sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)read_ue(&br);
    if (sps->log2_max_pic_order_cnt_lsb_minus4 > 12)
        sps->log2_max_pic_order_cnt_lsb_minus4 = 12;

    bool sub_layer_ordering = (read_bit(&br) != 0);
    uint32_t start_layer = sub_layer_ordering ? 0 : sps->sps_max_sub_layers_minus1;
    for (uint32_t i = start_layer; i <= sps->sps_max_sub_layers_minus1; ++i) {
        sps->sps_max_dec_pic_buffering_minus1 = (uint8_t)read_ue(&br);
        sps->sps_max_num_reorder_pics = (uint8_t)read_ue(&br);
        read_ue(&br);
    }

    sps->log2_min_luma_coding_block_size_minus3 = (uint8_t)read_ue(&br);
    sps->log2_diff_max_min_luma_coding_block_size = (uint8_t)read_ue(&br);
    sps->log2_min_transform_block_size_minus2 = (uint8_t)read_ue(&br);
    sps->log2_diff_max_min_transform_block_size = (uint8_t)read_ue(&br);
    sps->max_transform_hierarchy_depth_inter = (uint8_t)read_ue(&br);
    sps->max_transform_hierarchy_depth_intra = (uint8_t)read_ue(&br);

    sps->scaling_list_enabled_flag = (read_bit(&br) != 0);
    if (sps->scaling_list_enabled_flag) {
        if (read_bit(&br)) {
            skip_hevc_scaling_list_data(&br);
        }
    }

    sps->amp_enabled_flag = (read_bit(&br) != 0);
    sps->sample_adaptive_offset_enabled_flag = (read_bit(&br) != 0);
    sps->pcm_enabled_flag = (read_bit(&br) != 0);
    if (sps->pcm_enabled_flag) {
        sps->pcm_sample_bit_depth_luma_minus1 = (uint8_t)read_bits(&br, 4);
        sps->pcm_sample_bit_depth_chroma_minus1 = (uint8_t)read_bits(&br, 4);
        sps->log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)read_ue(&br);
        sps->log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t)read_ue(&br);
        sps->pcm_loop_filter_disabled_flag = (read_bit(&br) != 0);
    }

    uint32_t num_st_rps = read_ue(&br);
    if (num_st_rps > 64)
        num_st_rps = 64;
    sps->num_short_term_ref_pic_sets = (uint8_t)num_st_rps;
    for (uint32_t i = 0; i < num_st_rps; ++i) {
        parse_hevc_st_ref_pic_set(&br, i, num_st_rps, sps->st_rps, sps->st_rps_num_neg,
                                  sps->st_rps_num_pos);
    }

    sps->long_term_ref_pics_present_flag = (read_bit(&br) != 0);
    if (sps->long_term_ref_pics_present_flag) {
        uint32_t num_lt = read_ue(&br);
        if (num_lt > 32)
            num_lt = 32;
        sps->num_long_term_ref_pics_sps = (uint8_t)num_lt;
        uint32_t poc_bits = (uint32_t)sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
        for (uint32_t i = 0; i < num_lt; ++i) {
            read_bits(&br, poc_bits);
            read_bit(&br);
        }
    }

    sps->sps_temporal_mvp_enabled_flag = (read_bit(&br) != 0);
    sps->strong_intra_smoothing_enabled_flag = (read_bit(&br) != 0);
    if (read_bit(&br) && more_rbsp_data(&br)) { /* vui_parameters_present_flag */
        if (read_bit(&br)) {                    /* aspect_ratio_info_present_flag */
            uint32_t ar_idc = read_bits(&br, 8);
            if (ar_idc == 255) {
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
    sps->valid = (sps->pic_width_in_luma_samples > 0 && sps->pic_height_in_luma_samples > 0);
    return sps->valid;
}

bool parse_hevc_pps(const uint8_t *nal, size_t nal_len, parsed_hevc_pps_t *pps)
{
    if (!nal || nal_len < 3 || !pps)
        return false;
    uint8_t nal_type = (nal[0] >> 1) & 0x3F;
    if (nal_type != 34)
        return false;

    uint8_t rbsp[512];
    size_t rbsp_len = unescape_rbsp(nal + 2, nal_len - 2, rbsp, sizeof(rbsp));
    bs_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    memset(pps, 0, sizeof(*pps));
    read_ue(&br);
    read_ue(&br);
    pps->dependent_slice_segments_enabled_flag = (read_bit(&br) != 0);
    pps->output_flag_present_flag = (read_bit(&br) != 0);
    pps->num_extra_slice_header_bits = (uint8_t)read_bits(&br, 3);
    pps->sign_data_hiding_enabled_flag = (read_bit(&br) != 0);
    pps->cabac_init_present_flag = (read_bit(&br) != 0);
    pps->num_ref_idx_l0_default_active_minus1 = (uint8_t)read_ue(&br);
    pps->num_ref_idx_l1_default_active_minus1 = (uint8_t)read_ue(&br);
    pps->init_qp_minus26 = (int8_t)read_se(&br);
    pps->constrained_intra_pred_flag = (read_bit(&br) != 0);
    pps->transform_skip_enabled_flag = (read_bit(&br) != 0);
    pps->cu_qp_delta_enabled_flag = (read_bit(&br) != 0);
    if (pps->cu_qp_delta_enabled_flag) {
        pps->diff_cu_qp_delta_depth = (uint8_t)read_ue(&br);
    }
    pps->pps_cb_qp_offset = (int8_t)read_se(&br);
    pps->pps_cr_qp_offset = (int8_t)read_se(&br);
    pps->pps_slice_chroma_qp_offsets_present_flag = (read_bit(&br) != 0);
    pps->weighted_pred_flag = (read_bit(&br) != 0);
    pps->weighted_bipred_flag = (read_bit(&br) != 0);
    pps->transquant_bypass_enabled_flag = (read_bit(&br) != 0);
    pps->tiles_enabled_flag = (read_bit(&br) != 0);
    pps->entropy_coding_sync_enabled_flag = (read_bit(&br) != 0);
    if (pps->tiles_enabled_flag) {
        uint32_t cols = read_ue(&br);
        uint32_t rows = read_ue(&br);
        if (cols > 19)
            cols = 19;
        if (rows > 21)
            rows = 21;
        pps->num_tile_columns_minus1 = (uint8_t)cols;
        pps->num_tile_rows_minus1 = (uint8_t)rows;
        pps->uniform_spacing_flag = (read_bit(&br) != 0);
        if (!pps->uniform_spacing_flag) {
            for (uint32_t i = 0; i < cols; ++i) {
                pps->column_width_minus1[i] = (uint16_t)read_ue(&br);
            }
            for (uint32_t i = 0; i < rows; ++i) {
                pps->row_height_minus1[i] = (uint16_t)read_ue(&br);
            }
        }
        pps->loop_filter_across_tiles_enabled_flag = (read_bit(&br) != 0);
    } else {
        pps->loop_filter_across_tiles_enabled_flag = true;
    }
    pps->pps_loop_filter_across_slices_enabled_flag = (read_bit(&br) != 0);
    pps->deblocking_filter_control_present_flag = (read_bit(&br) != 0);
    if (pps->deblocking_filter_control_present_flag) {
        pps->deblocking_filter_override_enabled_flag = (read_bit(&br) != 0);
        pps->pps_disable_deblocking_filter_flag = (read_bit(&br) != 0);
        if (!pps->pps_disable_deblocking_filter_flag) {
            pps->pps_beta_offset_div2 = (int8_t)read_se(&br);
            pps->pps_tc_offset_div2 = (int8_t)read_se(&br);
        }
    }
    if (read_bit(&br)) {
        skip_hevc_scaling_list_data(&br);
    }
    pps->lists_modification_present_flag = (read_bit(&br) != 0);
    pps->log2_parallel_merge_level_minus2 = (uint8_t)read_ue(&br);
    pps->slice_segment_header_extension_present_flag = (read_bit(&br) != 0);
    pps->valid = true;
    return true;
}

static void parse_hevc_pred_weight_table(bs_t *br, const parsed_hevc_sps_t *sps,
                                         VASliceParameterBufferHEVC *sp, uint32_t slice_type,
                                         uint32_t num_ref_l0, uint32_t num_ref_l1)
{
    uint32_t chroma_array_type =
        (sps && !sps->separate_colour_plane_flag) ? sps->chroma_format_idc : 0;
    uint32_t luma_denom = read_ue(br);
    if (luma_denom > 7)
        luma_denom = 7;
    sp->luma_log2_weight_denom = (uint8_t)luma_denom;
    int32_t chroma_denom = (int32_t)luma_denom;
    if (chroma_array_type != 0) {
        int32_t delta_c_denom = read_se(br);
        sp->delta_chroma_log2_weight_denom = (int8_t)clamp_i32(delta_c_denom, -7, 7);
        chroma_denom = clamp_i32((int32_t)luma_denom + delta_c_denom, 0, 7);
    }
    bool luma_w_l0[16] = {false};
    bool chroma_w_l0[16] = {false};
    for (uint32_t i = 0; i < num_ref_l0 && i < 16; ++i) {
        luma_w_l0[i] = (read_bit(br) != 0);
    }
    if (chroma_array_type != 0) {
        for (uint32_t i = 0; i < num_ref_l0 && i < 16; ++i) {
            chroma_w_l0[i] = (read_bit(br) != 0);
        }
    }
    for (uint32_t i = 0; i < num_ref_l0 && i < 16; ++i) {
        if (luma_w_l0[i]) {
            int32_t dlw = read_se(br);
            int32_t loff = read_se(br);
            if (i < 15) {
                sp->delta_luma_weight_l0[i] = (int8_t)clamp_i32(dlw, -128, 127);
                sp->luma_offset_l0[i] = (int8_t)clamp_i32(loff, -128, 127);
            }
        }
        if (chroma_w_l0[i]) {
            for (int j = 0; j < 2; ++j) {
                int32_t dcw = read_se(br);
                int32_t dco = read_se(br);
                if (i < 15) {
                    sp->delta_chroma_weight_l0[i][j] = (int8_t)clamp_i32(dcw, -128, 127);
                    int32_t cw = (1 << chroma_denom) + dcw;
                    int32_t coff = 128 + dco - ((128 * cw) >> chroma_denom);
                    sp->ChromaOffsetL0[i][j] = (int8_t)clamp_i32(coff, -128, 127);
                }
            }
        }
    }
    if (slice_type == 0) { /* B-slice */
        bool luma_w_l1[16] = {false};
        bool chroma_w_l1[16] = {false};
        for (uint32_t i = 0; i < num_ref_l1 && i < 16; ++i) {
            luma_w_l1[i] = (read_bit(br) != 0);
        }
        if (chroma_array_type != 0) {
            for (uint32_t i = 0; i < num_ref_l1 && i < 16; ++i) {
                chroma_w_l1[i] = (read_bit(br) != 0);
            }
        }
        for (uint32_t i = 0; i < num_ref_l1 && i < 16; ++i) {
            if (luma_w_l1[i]) {
                int32_t dlw = read_se(br);
                int32_t loff = read_se(br);
                if (i < 15) {
                    sp->delta_luma_weight_l1[i] = (int8_t)clamp_i32(dlw, -128, 127);
                    sp->luma_offset_l1[i] = (int8_t)clamp_i32(loff, -128, 127);
                }
            }
            if (chroma_w_l1[i]) {
                for (int j = 0; j < 2; ++j) {
                    int32_t dcw = read_se(br);
                    int32_t dco = read_se(br);
                    if (i < 15) {
                        sp->delta_chroma_weight_l1[i][j] = (int8_t)clamp_i32(dcw, -128, 127);
                        int32_t cw = (1 << chroma_denom) + dcw;
                        int32_t coff = 128 + dco - ((128 * cw) >> chroma_denom);
                        sp->ChromaOffsetL1[i][j] = (int8_t)clamp_i32(coff, -128, 127);
                    }
                }
            }
        }
    }
}

bool parse_hevc_slice_header_ex(const uint8_t *nal, size_t nal_len, const parsed_hevc_sps_t *sps,
                                const parsed_hevc_pps_t *pps, VASliceParameterBufferHEVC *sp,
                                int32_t *out_poc_lsb, uint32_t *out_st_rps_bits,
                                hevc_st_rps_t *out_rps)
{
    if (!nal || nal_len < 3 || !sps || !pps || !sp)
        return false;
    if (out_rps)
        memset(out_rps, 0, sizeof(*out_rps));

    uint8_t nal_type = (nal[0] >> 1) & 0x3F;
    uint8_t rbsp[2048];
    size_t cap = (nal_len - 2) < sizeof(rbsp) ? (nal_len - 2) : sizeof(rbsp);
    size_t rbsp_len = unescape_rbsp(nal + 2, cap, rbsp, sizeof(rbsp));
    bs_t br;
    bit_reader_init_rbsp(&br, rbsp, rbsp_len);

    uint32_t first_slice = read_bit(&br);
    if (nal_type >= 16 && nal_type <= 23) {
        read_bit(&br);
    }
    read_ue(&br);

    bool dependent_slice = false;
    uint32_t seg_addr = 0;
    if (!first_slice) {
        if (pps->dependent_slice_segments_enabled_flag) {
            dependent_slice = (read_bit(&br) != 0);
        }
        uint32_t ctb_log2 = (sps->valid ? (sps->log2_min_luma_coding_block_size_minus3 + 3 +
                                           sps->log2_diff_max_min_luma_coding_block_size)
                                        : 6);
        if (ctb_log2 < 4)
            ctb_log2 = 4;
        if (ctb_log2 > 6)
            ctb_log2 = 6;
        uint32_t ctb_size = 1U << ctb_log2;
        uint32_t w = sps->valid ? sps->pic_width_in_luma_samples : 1920;
        uint32_t h = sps->valid ? sps->pic_height_in_luma_samples : 1080;
        uint32_t pic_ctbs = ((w + ctb_size - 1) / ctb_size) * ((h + ctb_size - 1) / ctb_size);
        seg_addr = read_bits(&br, ceil_log2_u32(pic_ctbs));
    }
    sp->slice_segment_address = seg_addr;
    sp->LongSliceFlags.fields.dependent_slice_segment_flag = dependent_slice ? 1 : 0;

    if (!dependent_slice) {
        sp->LongSliceFlags.fields.collocated_from_l0_flag = 1;
        sp->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag =
            pps->pps_disable_deblocking_filter_flag ? 1 : 0;
        sp->slice_beta_offset_div2 = pps->pps_beta_offset_div2;
        sp->slice_tc_offset_div2 = pps->pps_tc_offset_div2;
        sp->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag =
            pps->pps_loop_filter_across_slices_enabled_flag ? 1 : 0;
        memset(sp->RefPicList, 0xFF, sizeof(sp->RefPicList));
        if (pps->num_extra_slice_header_bits > 0) {
            read_bits(&br, pps->num_extra_slice_header_bits);
        }
        uint32_t slice_type = read_ue(&br);
        if (slice_type > 2)
            slice_type = 2;
        sp->LongSliceFlags.fields.slice_type = slice_type;
        if (pps->output_flag_present_flag) {
            read_bit(&br);
        }
        if (sps->separate_colour_plane_flag) {
            sp->LongSliceFlags.fields.color_plane_id = read_bits(&br, 2);
        }
        uint32_t num_pic_total_curr = 0;
        if (nal_type != 19 && nal_type != 20) {
            uint32_t poc_bits = (sps->valid ? sps->log2_max_pic_order_cnt_lsb_minus4 : 4) + 4;
            int32_t poc_lsb = (int32_t)read_bits(&br, poc_bits);
            if (out_poc_lsb)
                *out_poc_lsb = poc_lsb;
            uint32_t st_sps_flag = read_bit(&br);
            hevc_st_rps_t local_rps[65];
            memset(local_rps, 0, sizeof(local_rps));
            if (sps->valid) {
                memcpy(local_rps, sps->st_rps, sizeof(local_rps));
            }
            uint32_t curr_rps_idx = sps->num_short_term_ref_pic_sets;
            if (!st_sps_flag) {
                size_t rps_start = br.bit_pos;
                parse_hevc_st_ref_pic_set(&br, sps->num_short_term_ref_pic_sets,
                                          sps->num_short_term_ref_pic_sets, local_rps, NULL, NULL);
                if (out_st_rps_bits)
                    *out_st_rps_bits = (uint32_t)(br.bit_pos - rps_start);
                curr_rps_idx = sps->num_short_term_ref_pic_sets;
            } else if (sps->num_short_term_ref_pic_sets > 1) {
                curr_rps_idx = read_bits(&br, ceil_log2_u32(sps->num_short_term_ref_pic_sets));
            } else {
                curr_rps_idx = 0;
            }
            if (curr_rps_idx <= 64) {
                const hevc_st_rps_t *cr = &local_rps[curr_rps_idx];
                if (out_rps)
                    *out_rps = *cr;
                for (uint32_t i = 0; i < cr->num_negative_pics && i < 16; ++i) {
                    if (cr->used_by_curr_pic_s0[i])
                        num_pic_total_curr++;
                }
                for (uint32_t i = 0; i < cr->num_positive_pics && i < 16; ++i) {
                    if (cr->used_by_curr_pic_s1[i])
                        num_pic_total_curr++;
                }
            }
            if (sps->long_term_ref_pics_present_flag) {
                uint32_t num_lt_sps = (sps->num_long_term_ref_pics_sps > 0) ? read_ue(&br) : 0;
                uint32_t num_lt_pics = read_ue(&br);
                uint32_t lt_tot = num_lt_sps + num_lt_pics;
                if (lt_tot > 16)
                    lt_tot = 16;
                for (uint32_t i = 0; i < lt_tot; ++i) {
                    if (i < num_lt_sps) {
                        if (sps->num_long_term_ref_pics_sps > 1) {
                            read_bits(&br, ceil_log2_u32(sps->num_long_term_ref_pics_sps));
                        }
                    } else {
                        read_bits(&br, poc_bits);
                        if (read_bit(&br))
                            num_pic_total_curr++;
                    }
                    if (read_bit(&br)) {
                        read_ue(&br);
                    }
                }
            }
            if (sps->sps_temporal_mvp_enabled_flag) {
                sp->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = read_bit(&br);
            }
        }
        if (sps->sample_adaptive_offset_enabled_flag) {
            sp->LongSliceFlags.fields.slice_sao_luma_flag = read_bit(&br);
            if (!sps->separate_colour_plane_flag && sps->chroma_format_idc != 0) {
                sp->LongSliceFlags.fields.slice_sao_chroma_flag = read_bit(&br);
            }
        }
        sp->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
        sp->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
        if (sp->LongSliceFlags.fields.slice_type == 0 ||
            sp->LongSliceFlags.fields.slice_type == 1) {
            if (read_bit(&br)) {
                sp->num_ref_idx_l0_active_minus1 = (uint8_t)read_ue(&br);
                if (sp->LongSliceFlags.fields.slice_type == 0) {
                    sp->num_ref_idx_l1_active_minus1 = (uint8_t)read_ue(&br);
                }
            }
            if (sp->num_ref_idx_l0_active_minus1 > 14)
                sp->num_ref_idx_l0_active_minus1 = 14;
            if (sp->num_ref_idx_l1_active_minus1 > 14)
                sp->num_ref_idx_l1_active_minus1 = 14;
        }

        if (br.bit_pos < br.end_bit_pos) {
            if (sp->LongSliceFlags.fields.slice_type == 0 ||
                sp->LongSliceFlags.fields.slice_type == 1) {
                if (pps->lists_modification_present_flag && num_pic_total_curr > 1) {
                    uint32_t list_entry_bits = ceil_log2_u32(num_pic_total_curr);
                    if (read_bit(&br)) {
                        for (uint32_t i = 0; i <= sp->num_ref_idx_l0_active_minus1; ++i) {
                            uint32_t entry = read_bits(&br, list_entry_bits);
                            if (i < 15) {
                                sp->RefPicList[0][i] = (uint8_t)(0x80U | (entry & 0x0FU));
                            }
                        }
                    }
                    if (sp->LongSliceFlags.fields.slice_type == 0 && read_bit(&br)) {
                        for (uint32_t i = 0; i <= sp->num_ref_idx_l1_active_minus1; ++i) {
                            uint32_t entry = read_bits(&br, list_entry_bits);
                            if (i < 15) {
                                sp->RefPicList[1][i] = (uint8_t)(0x80U | (entry & 0x0FU));
                            }
                        }
                    }
                }
                if (sp->LongSliceFlags.fields.slice_type == 0) {
                    sp->LongSliceFlags.fields.mvd_l1_zero_flag = read_bit(&br);
                }
                if (pps->cabac_init_present_flag) {
                    sp->LongSliceFlags.fields.cabac_init_flag = read_bit(&br);
                }
                if (sp->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
                    if (sp->LongSliceFlags.fields.slice_type == 0) {
                        sp->LongSliceFlags.fields.collocated_from_l0_flag = read_bit(&br);
                    }
                    uint32_t coll_max = sp->LongSliceFlags.fields.collocated_from_l0_flag
                                            ? sp->num_ref_idx_l0_active_minus1
                                            : sp->num_ref_idx_l1_active_minus1;
                    if (coll_max > 0) {
                        uint32_t c_idx = read_ue(&br);
                        sp->collocated_ref_idx = (uint8_t)(c_idx > 14 ? 14 : c_idx);
                    } else {
                        sp->collocated_ref_idx = 0;
                    }
                }
                if ((pps->weighted_pred_flag && sp->LongSliceFlags.fields.slice_type == 1) ||
                    (pps->weighted_bipred_flag && sp->LongSliceFlags.fields.slice_type == 0)) {
                    parse_hevc_pred_weight_table(&br, sps, sp, sp->LongSliceFlags.fields.slice_type,
                                                 (uint32_t)sp->num_ref_idx_l0_active_minus1 + 1u,
                                                 (uint32_t)sp->num_ref_idx_l1_active_minus1 + 1u);
                }
                uint32_t five_minus = read_ue(&br);
                if (five_minus > 4)
                    five_minus = 4;
                sp->five_minus_max_num_merge_cand = (uint8_t)five_minus;
            }

            if (br.bit_pos < br.end_bit_pos) {
                sp->slice_qp_delta = (int8_t)read_se(&br);
                if (pps->pps_slice_chroma_qp_offsets_present_flag) {
                    sp->slice_cb_qp_offset = (int8_t)read_se(&br);
                    sp->slice_cr_qp_offset = (int8_t)read_se(&br);
                }
                bool deblocking_override = false;
                if (pps->deblocking_filter_override_enabled_flag) {
                    deblocking_override = (read_bit(&br) != 0);
                }
                if (deblocking_override) {
                    bool disable_deblock = (read_bit(&br) != 0);
                    sp->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag =
                        disable_deblock ? 1 : 0;
                    if (!disable_deblock) {
                        sp->slice_beta_offset_div2 = (int8_t)read_se(&br);
                        sp->slice_tc_offset_div2 = (int8_t)read_se(&br);
                    }
                }
                if (pps->pps_loop_filter_across_slices_enabled_flag &&
                    (sp->LongSliceFlags.fields.slice_sao_luma_flag ||
                     sp->LongSliceFlags.fields.slice_sao_chroma_flag ||
                     !sp->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag)) {
                    sp->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag =
                        read_bit(&br);
                }
            }
        }
    }

    if (br.bit_pos < br.end_bit_pos) {
        if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag) {
            uint32_t num_entry_point_offsets = read_ue(&br);
            if (num_entry_point_offsets > 0) {
                uint32_t offset_len_minus1 = read_ue(&br);
                if (offset_len_minus1 < 32) {
                    for (uint32_t i = 0; i < num_entry_point_offsets && br.bit_pos < br.end_bit_pos;
                         ++i) {
                        read_bits(&br, offset_len_minus1 + 1);
                    }
                }
            }
        }
        if (pps->slice_segment_header_extension_present_flag) {
            uint32_t ext_len = read_ue(&br);
            for (uint32_t i = 0; i < ext_len && br.bit_pos < br.end_bit_pos; ++i) {
                read_bits(&br, 8);
            }
        }
        if (br.bit_pos < br.end_bit_pos) {
            read_bit(&br); /* alignment_bit_equal_to_one */
            while ((br.bit_pos & 7u) != 0) {
                read_bit(&br); /* alignment_bit_equal_to_zero */
            }
        }
    }

    if (!sp->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
        sp->collocated_ref_idx = 0xFF;
    }
    size_t ebsp_payload_bits = rbsp_bit_offset_to_ebsp_bits(nal + 2, nal_len - 2, br.bit_pos);
    uint32_t byte_off = (uint32_t)(2 + (ebsp_payload_bits + 7) / 8);
    if (byte_off > (uint32_t)nal_len) {
        byte_off = (uint32_t)nal_len;
    }
    sp->slice_data_byte_offset = byte_off;
    return true;
}

bool parse_hevc_slice_header(const uint8_t *nal, size_t nal_len, const parsed_hevc_sps_t *sps,
                             const parsed_hevc_pps_t *pps, VASliceParameterBufferHEVC *sp,
                             int32_t *out_poc_lsb, uint32_t *out_st_rps_bits)
{
    return parse_hevc_slice_header_ex(nal, nal_len, sps, pps, sp, out_poc_lsb, out_st_rps_bits,
                                      NULL);
}

void build_hevc_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_poc,
                          VASliceParameterBufferHEVC *sp)
{
    uint8_t mod_l0[15];
    uint8_t mod_l1[15];
    memcpy(mod_l0, sp->RefPicList[0], sizeof(mod_l0));
    memcpy(mod_l1, sp->RefPicList[1], sizeof(mod_l1));

    for (int i = 0; i < 15; ++i) {
        sp->RefPicList[0][i] = 0xFF;
        sp->RefPicList[1][i] = 0xFF;
    }
    if (!dpb || dpb->count == 0 || slice_type == 2)
        return;

    size_t n = dpb->count > 15 ? 15 : dpb->count;
    bool has_ref_filter = false;
    for (size_t i = 0; i < n; ++i) {
        if (dpb->entries[i].is_reference) {
            has_ref_filter = true;
            break;
        }
    }

    uint8_t before_idx[15];
    uint8_t after_idx[15];
    size_t n_before = 0;
    size_t n_after = 0;

    for (size_t i = 0; i < n; ++i) {
        if (has_ref_filter && !dpb->entries[i].is_reference)
            continue;
        if (dpb->entries[i].pic_order_cnt <= curr_poc) {
            before_idx[n_before++] = (uint8_t)i;
        } else {
            after_idx[n_after++] = (uint8_t)i;
        }
    }
    for (size_t i = 0; i < n_before; ++i) {
        for (size_t j = i + 1; j < n_before; ++j) {
            if (dpb->entries[before_idx[j]].pic_order_cnt >
                dpb->entries[before_idx[i]].pic_order_cnt) {
                uint8_t tmp = before_idx[i];
                before_idx[i] = before_idx[j];
                before_idx[j] = tmp;
            }
        }
    }
    for (size_t i = 0; i < n_after; ++i) {
        for (size_t j = i + 1; j < n_after; ++j) {
            if (dpb->entries[after_idx[j]].pic_order_cnt <
                dpb->entries[after_idx[i]].pic_order_cnt) {
                uint8_t tmp = after_idx[i];
                after_idx[i] = after_idx[j];
                after_idx[j] = tmp;
            }
        }
    }

    uint8_t temp0[15];
    size_t idx0 = 0;
    for (size_t i = 0; i < n_before && idx0 < 15; ++i)
        temp0[idx0++] = before_idx[i];
    for (size_t i = 0; i < n_after && idx0 < 15; ++i)
        temp0[idx0++] = after_idx[i];

    if (idx0 > 0) {
        size_t count0 = (size_t)sp->num_ref_idx_l0_active_minus1 + 1U;
        if (count0 < idx0)
            count0 = idx0;
        if (count0 > 15U)
            count0 = 15U;
        for (size_t i = 0; i < count0; ++i)
            sp->RefPicList[0][i] = temp0[i % idx0];
        if ((mod_l0[0] & 0xF0U) == 0x80U) {
            for (uint32_t i = 0; i <= sp->num_ref_idx_l0_active_minus1 && i < 15U; ++i) {
                if ((mod_l0[i] & 0xF0U) == 0x80U) {
                    sp->RefPicList[0][i] = temp0[(mod_l0[i] & 0x0FU) % idx0];
                }
            }
        }
    }

    if (slice_type == 0) {
        uint8_t temp1[15];
        size_t idx1 = 0;
        for (size_t i = 0; i < n_after && idx1 < 15; ++i)
            temp1[idx1++] = after_idx[i];
        for (size_t i = 0; i < n_before && idx1 < 15; ++i)
            temp1[idx1++] = before_idx[i];

        if (idx1 > 0) {
            size_t count1 = (size_t)sp->num_ref_idx_l1_active_minus1 + 1U;
            if (count1 < idx1)
                count1 = idx1;
            if (count1 > 15U)
                count1 = 15U;
            for (size_t i = 0; i < count1; ++i)
                sp->RefPicList[1][i] = temp1[i % idx1];
            if ((mod_l1[0] & 0xF0U) == 0x80U) {
                for (uint32_t i = 0; i <= sp->num_ref_idx_l1_active_minus1 && i < 15U; ++i) {
                    if ((mod_l1[i] & 0xF0U) == 0x80U) {
                        sp->RefPicList[1][i] = temp1[(mod_l1[i] & 0x0FU) % idx1];
                    }
                }
            }
        }
    }
}
