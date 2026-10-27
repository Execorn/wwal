// SPDX-License-Identifier: GPL-3.0-or-later
#include "vaapi_internal.h"

uint64_t av1_read_leb128(const uint8_t *data, size_t size, size_t *out_bytes)
{
    uint64_t val = 0;
    size_t i = 0;
    for (; i < 8 && i < size; ++i) {
        uint8_t b = data[i];
        val |= ((uint64_t)(b & 0x7F)) << (i * 7);
        if ((b & 0x80) == 0) {
            i++;
            break;
        }
    }
    if (out_bytes)
        *out_bytes = i;
    return val;
}

static uint32_t av1_read_uvlc(bs_t *br)
{
    uint32_t lz = 0;
    while (read_bit(br) == 0 && br->bit_pos < br->size * 8) {
        lz++;
        if (lz >= 32)
            return 0xFFFFFFFFU;
    }
    if (lz == 0)
        return 0;
    uint32_t v = read_bits(br, lz);
    return v + (1U << lz) - 1U;
}

static int32_t av1_read_su(bs_t *br, uint32_t n)
{
    uint32_t v = read_bits(br, n);
    uint32_t sign_mask = 1U << (n - 1);
    if (v & sign_mask) {
        return (int32_t)(v - (sign_mask << 1));
    }
    return (int32_t)v;
}

static uint32_t av1_read_ns(bs_t *br, uint32_t n)
{
    if (n <= 1)
        return 0;
    uint32_t w = ceil_log2_u32(n);
    uint32_t m = (1U << w) - n;
    uint32_t v = read_bits(br, w - 1);
    if (v < m)
        return v;
    return (v << 1) - m + read_bit(br);
}

static int32_t av1_get_relative_dist(int32_t a, int32_t b, const parsed_av1_seq_t *seq)
{
    if (!seq->enable_order_hint)
        return 0;
    int32_t diff = a - b;
    int32_t m = 1 << seq->order_hint_bits_minus_1;
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

static uint32_t av1_decode_subexp(bs_t *br, uint32_t num_syms)
{
    uint32_t i = 0;
    uint32_t mk = 0;
    uint32_t k = 3;
    while (true) {
        uint32_t b2 = i ? (k + i - 1U) : k;
        uint32_t a = 1U << b2;
        if (num_syms <= mk + 3U * a) {
            return av1_read_ns(br, num_syms - mk) + mk;
        }
        if (!read_bit(br)) {
            return read_bits(br, b2) + mk;
        }
        i++;
        mk += a;
    }
}

static uint32_t av1_decode_unsigned_subexp_with_ref(bs_t *br, uint32_t mx, uint32_t r)
{
    uint32_t v = av1_decode_subexp(br, mx);
    if ((r << 1) <= mx) {
        if ((v & 1U) != 0)
            return r - ((v + 1U) >> 1);
        return r + (v >> 1);
    } else {
        uint32_t inv_r = mx - 1U - r;
        uint32_t res = ((v & 1U) != 0) ? (inv_r - ((v + 1U) >> 1)) : (inv_r + (v >> 1));
        return mx - 1U - res;
    }
}

static int32_t av1_decode_signed_subexp_with_ref(bs_t *br, int32_t low, int32_t high, int32_t r)
{
    return (int32_t)av1_decode_unsigned_subexp_with_ref(br, (uint32_t)(high - low),
                                                        (uint32_t)(r - low)) +
           low;
}

static void av1_read_global_param(bs_t *br, VAAV1TransformationType type, int idx,
                                  uint32_t allow_high_precision_mv, VAWarpedMotionParamsAV1 *wm)
{
    uint32_t abs_bits = 12;
    uint32_t prec_bits = 15;
    if (idx < 2) {
        if (type == VAAV1TransformationTranslation) {
            abs_bits = 9U - (allow_high_precision_mv ? 0U : 1U);
            prec_bits = 3U - (allow_high_precision_mv ? 0U : 1U);
        } else {
            abs_bits = 12;
            prec_bits = 6;
        }
    }
    int32_t prec_diff = 16 - (int32_t)prec_bits;
    int32_t round = (idx % 3) == 2 ? (1 << 16) : 0;
    int32_t sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
    int32_t mx = 1 << abs_bits;
    int32_t r = (wm->wmmat[idx] >> prec_diff) - sub;
    wm->wmmat[idx] = (av1_decode_signed_subexp_with_ref(br, -mx, mx + 1, r) << prec_diff) + round;
}

bool parse_av1_sequence_header(const uint8_t *payload, size_t len, parsed_av1_seq_t *seq)
{
    if (!payload || len < 3 || !seq)
        return false;

    bs_t br;
    bit_reader_init_raw(&br, payload, len);
    memset(seq, 0, sizeof(*seq));

    seq->seq_profile = (uint8_t)read_bits(&br, 3);
    seq->still_picture = (read_bit(&br) != 0);
    seq->reduced_still_picture_header = (read_bit(&br) != 0);

    if (seq->reduced_still_picture_header) {
        read_bits(&br, 5);
    } else {
        bool decoder_model_info_present = false;
        uint32_t buffer_delay_length_minus_1 = 0;
        if (read_bit(&br)) {
            read_bits(&br, 16);
            read_bits(&br, 16);
            read_bits(&br, 16);
            read_bits(&br, 16);
            if (read_bit(&br)) {
                av1_read_uvlc(&br);
            }
            decoder_model_info_present = (read_bit(&br) != 0);
            if (decoder_model_info_present) {
                buffer_delay_length_minus_1 = read_bits(&br, 5);
                read_bits(&br, 16);
                read_bits(&br, 16);
                read_bits(&br, 5);
                read_bits(&br, 5);
            }
        }
        bool initial_display_delay_present = (read_bit(&br) != 0);
        uint32_t op_cnt_minus_1 = read_bits(&br, 5);
        for (uint32_t i = 0; i <= op_cnt_minus_1; ++i) {
            read_bits(&br, 12);
            uint32_t seq_level_idx = read_bits(&br, 5);
            if (seq_level_idx > 7) {
                read_bit(&br);
            }
            if (decoder_model_info_present) {
                if (read_bit(&br)) {
                    uint32_t n = buffer_delay_length_minus_1 + 1;
                    read_bits(&br, n);
                    read_bits(&br, n);
                    read_bit(&br);
                }
            }
            if (initial_display_delay_present) {
                if (read_bit(&br)) {
                    read_bits(&br, 4);
                }
            }
        }
    }

    seq->frame_width_bits_minus_1 = (uint8_t)read_bits(&br, 4);
    seq->frame_height_bits_minus_1 = (uint8_t)read_bits(&br, 4);
    seq->max_frame_width_minus_1 =
        (uint16_t)read_bits(&br, (uint32_t)seq->frame_width_bits_minus_1 + 1);
    seq->max_frame_height_minus_1 =
        (uint16_t)read_bits(&br, (uint32_t)seq->frame_height_bits_minus_1 + 1);

    if (!seq->reduced_still_picture_header) {
        seq->frame_id_numbers_present_flag = (read_bit(&br) != 0);
        if (seq->frame_id_numbers_present_flag) {
            seq->delta_frame_id_length_minus_2 = (uint8_t)read_bits(&br, 4);
            seq->additional_frame_id_length_minus_1 = (uint8_t)read_bits(&br, 3);
        }
    }

    seq->use_128x128_superblock = (read_bit(&br) != 0);
    seq->enable_filter_intra = (read_bit(&br) != 0);
    seq->enable_intra_edge_filter = (read_bit(&br) != 0);

    if (seq->reduced_still_picture_header) {
        seq->seq_force_screen_content_tools = 2;
        seq->seq_force_integer_mv = 2;
    } else {
        seq->enable_interintra_compound = (read_bit(&br) != 0);
        seq->enable_masked_compound = (read_bit(&br) != 0);
        seq->enable_warped_motion = (read_bit(&br) != 0);
        seq->enable_dual_filter = (read_bit(&br) != 0);
        seq->enable_order_hint = (read_bit(&br) != 0);
        if (seq->enable_order_hint) {
            seq->enable_jnt_comp = (read_bit(&br) != 0);
            seq->enable_ref_frame_mvs = (read_bit(&br) != 0);
        }
        seq->seq_force_screen_content_tools = read_bit(&br) ? 2 : (uint8_t)read_bit(&br);
        if (seq->seq_force_screen_content_tools > 0) {
            seq->seq_force_integer_mv = read_bit(&br) ? 2 : (uint8_t)read_bit(&br);
        } else {
            seq->seq_force_integer_mv = 2;
        }
        if (seq->enable_order_hint) {
            seq->order_hint_bits_minus_1 = (uint8_t)read_bits(&br, 3);
        }
    }

    seq->enable_superres = (read_bit(&br) != 0);
    seq->enable_cdef = (read_bit(&br) != 0);
    seq->enable_restoration = (read_bit(&br) != 0);

    seq->high_bitdepth = (read_bit(&br) != 0);
    if (seq->seq_profile == 2 && seq->high_bitdepth) {
        seq->twelve_bit = (read_bit(&br) != 0);
    }
    if (seq->seq_profile != 1) {
        seq->mono_chrome = (read_bit(&br) != 0);
    }
    if (read_bit(&br)) {
        seq->color_primaries = (uint8_t)read_bits(&br, 8);
        seq->transfer_characteristics = (uint8_t)read_bits(&br, 8);
        seq->matrix_coefficients = (uint8_t)read_bits(&br, 8);
    } else {
        seq->color_primaries = 2;
        seq->transfer_characteristics = 2;
        seq->matrix_coefficients = 2;
    }

    if (seq->mono_chrome) {
        seq->color_range = (read_bit(&br) != 0);
        seq->subsampling_x = 1;
        seq->subsampling_y = 1;
        seq->chroma_sample_position = 0;
        seq->separate_uv_delta_q = false;
    } else {
        if (seq->color_primaries == 1 && seq->transfer_characteristics == 13 &&
            seq->matrix_coefficients == 0) {
            seq->color_range = true;
            seq->subsampling_x = 0;
            seq->subsampling_y = 0;
        } else {
            seq->color_range = (read_bit(&br) != 0);
            if (seq->seq_profile == 0) {
                seq->subsampling_x = 1;
                seq->subsampling_y = 1;
            } else if (seq->seq_profile == 1) {
                seq->subsampling_x = 0;
                seq->subsampling_y = 0;
            } else {
                if (seq->twelve_bit) {
                    seq->subsampling_x = (uint8_t)read_bit(&br);
                    seq->subsampling_y = seq->subsampling_x ? (uint8_t)read_bit(&br) : 0;
                } else {
                    seq->subsampling_x = 1;
                    seq->subsampling_y = 0;
                }
            }
            if (seq->subsampling_x && seq->subsampling_y) {
                seq->chroma_sample_position = (uint8_t)read_bits(&br, 2);
            }
        }
        seq->separate_uv_delta_q = (read_bit(&br) != 0);
    }

    seq->film_grain_params_present = (read_bit(&br) != 0);
    seq->valid = true;
    return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
uint32_t parse_av1_tile_group_obu(const uint8_t *obu_data, size_t obu_size, size_t tg_offset,
                                  uint32_t base_obu_offset,
                                  const VADecPictureParameterBufferAV1 *pic_param,
                                  uint8_t tile_size_bytes_minus_1,
                                  VASliceParameterBufferAV1 *out_slices, uint32_t max_slices)
{
    if (!obu_data || tg_offset >= obu_size || !pic_param || !out_slices || max_slices == 0)
        return 0;
    uint32_t tile_cols = pic_param->tile_cols ? pic_param->tile_cols : 1U;
    uint32_t tile_rows = pic_param->tile_rows ? pic_param->tile_rows : 1U;
    uint32_t num_tiles = tile_cols * tile_rows;

    bs_t br;
    bit_reader_init_raw(&br, obu_data + tg_offset, obu_size - tg_offset);

    uint32_t tg_start = 0;
    uint32_t tg_end = num_tiles - 1U;
    if (num_tiles > 1U) {
        uint32_t tile_bits = ceil_log2_u32(tile_cols) + ceil_log2_u32(tile_rows);
        if (read_bit(&br)) {
            tg_start = read_bits(&br, tile_bits);
            tg_end = read_bits(&br, tile_bits);
        }
    }
    while ((br.bit_pos & 7U) != 0) {
        (void)read_bit(&br);
    }

    size_t pos = tg_offset + (br.bit_pos / 8U);
    uint32_t tile_size_bytes = (uint32_t)tile_size_bytes_minus_1 + 1U;
    uint32_t slice_count = 0;

    for (uint32_t tile_num = tg_start;
         tile_num <= tg_end && tile_num < num_tiles && slice_count < max_slices; ++tile_num) {
        size_t tile_sz = 0;
        if (tile_num < tg_end) {
            if (pos + tile_size_bytes > obu_size)
                break;
            for (uint32_t b = 0; b < tile_size_bytes; ++b) {
                tile_sz |= (size_t)obu_data[pos + b] << (8U * b);
            }
            tile_sz += 1U;
            pos += tile_size_bytes;
            if (pos + tile_sz > obu_size)
                break;
        } else {
            if (pos > obu_size)
                break;
            tile_sz = obu_size - pos;
        }

        VASliceParameterBufferAV1 *sp = &out_slices[slice_count++];
        memset(sp, 0, sizeof(*sp));
        sp->slice_data_size = (uint32_t)tile_sz;
        sp->slice_data_offset = base_obu_offset + (uint32_t)pos;
        sp->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        sp->tile_row = (uint16_t)(tile_num / tile_cols);
        sp->tile_column = (uint16_t)(tile_num % tile_cols);
        sp->tg_start = (uint16_t)tg_start;
        sp->tg_end = (uint16_t)tg_end;

        pos += tile_sz;
    }
    return slice_count;
}
#pragma GCC diagnostic pop

bool parse_av1_frame_header(vaapi_decoder_t *dec, const uint8_t *payload, size_t len,
                            uint32_t obu_type, uint32_t obu_payload_offset,
                            VADecPictureParameterBufferAV1 *pic_param,
                            VASliceParameterBufferAV1 *out_slices, uint32_t max_slices,
                            uint32_t *io_slice_count, uint8_t *out_tile_size_bytes_minus_1,
                            uint8_t *out_refresh_flags, bool *out_is_keyframe,
                            bool *out_show_existing, uint8_t *out_show_existing_idx)
{
    if (!dec || !payload || len < 1 || !pic_param || !out_slices)
        return false;

    if (out_show_existing)
        *out_show_existing = false;
    if (out_show_existing_idx)
        *out_show_existing_idx = 0;

    const parsed_av1_seq_t *seq = &dec->cached_av1_seq;
    bs_t br;
    bit_reader_init_raw(&br, payload, len);

    uint32_t frame_type = 0;
    uint32_t show_frame = 1;
    uint32_t showable_frame = 0;
    uint32_t error_resilient = 1;

    if (!seq->reduced_still_picture_header) {
        if (read_bit(&br)) {
            uint8_t idx = (uint8_t)read_bits(&br, 3);
            if (out_show_existing)
                *out_show_existing = true;
            if (out_show_existing_idx)
                *out_show_existing_idx = idx;
            *out_refresh_flags = 0;
            *out_is_keyframe = false;
            return true;
        }
        frame_type = read_bits(&br, 2);
        show_frame = read_bit(&br);
        if (!show_frame) {
            showable_frame = read_bit(&br);
        }
        if (frame_type == 3 || (frame_type == 0 && show_frame)) {
            error_resilient = 1;
        } else {
            error_resilient = read_bit(&br);
        }
    }

    uint32_t disable_cdf_update = read_bit(&br);
    uint32_t allow_screen_content_tools = (seq->seq_force_screen_content_tools == 2)
                                              ? read_bit(&br)
                                              : seq->seq_force_screen_content_tools;
    uint32_t force_integer_mv = (frame_type == 0 || frame_type == 2) ? 1 : 0;
    if (allow_screen_content_tools) {
        force_integer_mv =
            (seq->seq_force_integer_mv == 2) ? read_bit(&br) : seq->seq_force_integer_mv;
    }

    if (seq->frame_id_numbers_present_flag) {
        uint32_t id_len = (uint32_t)seq->delta_frame_id_length_minus_2 + 2 +
                          (uint32_t)seq->additional_frame_id_length_minus_1 + 1;
        read_bits(&br, id_len);
    }

    uint32_t frame_size_override =
        (frame_type == 3) ? 1 : (seq->reduced_still_picture_header ? 0 : read_bit(&br));
    uint8_t order_hint = seq->enable_order_hint
                             ? (uint8_t)read_bits(&br, (uint32_t)seq->order_hint_bits_minus_1 + 1)
                             : 0;
    uint8_t primary_ref_frame =
        (frame_type == 0 || frame_type == 2 || error_resilient) ? 7 : (uint8_t)read_bits(&br, 3);

    uint8_t refresh_frame_flags = 0xFF;
    if (frame_type != 0) {
        refresh_frame_flags = (frame_type == 3) ? 0xFF : (uint8_t)read_bits(&br, 8);
    } else if (!show_frame) {
        refresh_frame_flags = (uint8_t)read_bits(&br, 8);
    }
    if ((!(frame_type == 0 || frame_type == 2) || refresh_frame_flags != 0xFF) && error_resilient &&
        seq->enable_order_hint) {
        for (int i = 0; i < 8; ++i) {
            read_bits(&br, (uint32_t)seq->order_hint_bits_minus_1 + 1);
        }
    }

    uint16_t width_m1 = seq->valid ? seq->max_frame_width_minus_1 : (uint16_t)(dec->width - 1);
    uint16_t height_m1 = seq->valid ? seq->max_frame_height_minus_1 : (uint16_t)(dec->height - 1);
    uint32_t allow_intrabc = 0;
    uint32_t use_superres = 0;
    uint8_t superres_denom = 8;
    uint32_t allow_high_precision_mv = 0;
    uint8_t interp_filter = 0;
    uint32_t is_motion_mode_switchable = 0;
    uint32_t use_ref_frame_mvs = 0;
    uint8_t ref_frame_idx[7] = {0, 1, 2, 3, 4, 5, 6};

    if (frame_type == 0 || frame_type == 2) {
        if (frame_size_override && seq->valid) {
            width_m1 = (uint16_t)read_bits(&br, (uint32_t)seq->frame_width_bits_minus_1 + 1);
            height_m1 = (uint16_t)read_bits(&br, (uint32_t)seq->frame_height_bits_minus_1 + 1);
        }
        if (seq->enable_superres && read_bit(&br)) {
            use_superres = 1;
            superres_denom = (uint8_t)(read_bits(&br, 3) + 9);
        }
        if (read_bit(&br)) {
            read_bits(&br, 32);
        }
        if (allow_screen_content_tools && !use_superres) {
            allow_intrabc = read_bit(&br);
        }
    } else {
        bool short_signaling = false;
        if (seq->enable_order_hint) {
            short_signaling = (read_bit(&br) != 0);
            if (short_signaling) {
                ref_frame_idx[0] = (uint8_t)read_bits(&br, 3);
                ref_frame_idx[3] = (uint8_t)read_bits(&br, 3);
            }
        }
        for (int i = 0; i < 7; ++i) {
            if (!short_signaling) {
                ref_frame_idx[i] = (uint8_t)read_bits(&br, 3);
            }
            if (seq->frame_id_numbers_present_flag) {
                read_bits(&br, (uint32_t)seq->delta_frame_id_length_minus_2 + 2);
            }
        }
        bool found_ref = false;
        if (frame_size_override && !error_resilient) {
            for (int i = 0; i < 7; ++i) {
                if (read_bit(&br)) {
                    found_ref = true;
                    break;
                }
            }
        }
        if (!found_ref) {
            if (frame_size_override && seq->valid) {
                width_m1 = (uint16_t)read_bits(&br, (uint32_t)seq->frame_width_bits_minus_1 + 1);
                height_m1 = (uint16_t)read_bits(&br, (uint32_t)seq->frame_height_bits_minus_1 + 1);
            }
            if (seq->enable_superres && read_bit(&br)) {
                use_superres = 1;
                superres_denom = (uint8_t)(read_bits(&br, 3) + 9);
            }
            if (read_bit(&br)) {
                read_bits(&br, 32);
            }
        } else if (seq->enable_superres && read_bit(&br)) {
            use_superres = 1;
            superres_denom = (uint8_t)(read_bits(&br, 3) + 9);
        }
        allow_high_precision_mv = force_integer_mv ? 0 : read_bit(&br);
        interp_filter = read_bit(&br) ? 4 : (uint8_t)read_bits(&br, 2);
        is_motion_mode_switchable = read_bit(&br);
        use_ref_frame_mvs = (error_resilient || !seq->enable_ref_frame_mvs) ? 0 : read_bit(&br);
    }

    uint32_t disable_frame_end_update_cdf =
        (seq->reduced_still_picture_header || disable_cdf_update) ? 1 : read_bit(&br);

    uint32_t upscaled_width = (uint32_t)width_m1 + 1U;
    uint32_t frame_width = upscaled_width;
    if (use_superres) {
        frame_width = (upscaled_width * 8U + (superres_denom / 2U)) / superres_denom;
    }
    uint32_t mi_cols = 2U * ((frame_width + 7U) >> 3);
    uint32_t mi_rows = 2U * (((uint32_t)height_m1 + 8U) >> 3);
    uint32_t sb_shift = seq->use_128x128_superblock ? 5 : 4;
    uint32_t sb_cols = (mi_cols + (1U << sb_shift) - 1U) >> sb_shift;
    uint32_t sb_rows = (mi_rows + (1U << sb_shift) - 1U) >> sb_shift;
    uint32_t max_tile_width_sb = 4096U >> (sb_shift + 2);
    uint32_t max_tile_area_sb = (4096U * 2304U) >> (2 * (sb_shift + 2));
    uint32_t min_log2_tile_cols =
        ceil_log2_u32((sb_cols + max_tile_width_sb - 1) / max_tile_width_sb);
    uint32_t max_log2_tile_cols = ceil_log2_u32(sb_cols < 64 ? sb_cols : 64);
    uint32_t min_log2_tiles =
        ceil_log2_u32((sb_cols * sb_rows + max_tile_area_sb - 1) / max_tile_area_sb);
    if (min_log2_tiles < min_log2_tile_cols)
        min_log2_tiles = min_log2_tile_cols;

    uint32_t uniform_tile_spacing = read_bit(&br);
    uint32_t tile_cols = 1;
    uint32_t tile_rows = 1;
    uint32_t tile_cols_log2 = 0;
    uint32_t tile_rows_log2 = 0;
    if (uniform_tile_spacing) {
        tile_cols_log2 = min_log2_tile_cols;
        while (tile_cols_log2 < max_log2_tile_cols && read_bit(&br)) {
            tile_cols_log2++;
        }
        uint32_t tile_w_sb = (sb_cols + (1U << tile_cols_log2) - 1U) >> tile_cols_log2;
        if (tile_w_sb == 0)
            tile_w_sb = 1;
        uint32_t i = 0;
        for (uint32_t start_sb = 0; start_sb < sb_cols && i < 64U; start_sb += tile_w_sb, ++i) {
            uint32_t w_sb = (start_sb + tile_w_sb <= sb_cols) ? tile_w_sb : (sb_cols - start_sb);
            if (i < 63U)
                pic_param->width_in_sbs_minus_1[i] = (uint16_t)(w_sb - 1U);
        }
        tile_cols = i > 0 ? i : 1;

        uint32_t min_log2_rows =
            (min_log2_tiles > tile_cols_log2) ? (min_log2_tiles - tile_cols_log2) : 0;
        uint32_t max_log2_rows = ceil_log2_u32(sb_rows < 64 ? sb_rows : 64);
        tile_rows_log2 = min_log2_rows;
        while (tile_rows_log2 < max_log2_rows && read_bit(&br)) {
            tile_rows_log2++;
        }
        uint32_t tile_h_sb = (sb_rows + (1U << tile_rows_log2) - 1U) >> tile_rows_log2;
        if (tile_h_sb == 0)
            tile_h_sb = 1;
        i = 0;
        for (uint32_t start_sb = 0; start_sb < sb_rows && i < 64U; start_sb += tile_h_sb, ++i) {
            uint32_t h_sb = (start_sb + tile_h_sb <= sb_rows) ? tile_h_sb : (sb_rows - start_sb);
            if (i < 63U)
                pic_param->height_in_sbs_minus_1[i] = (uint16_t)(h_sb - 1U);
        }
        tile_rows = i > 0 ? i : 1;
    } else {
        uint32_t widest_tile_sb = 1;
        uint32_t start_sb = 0;
        uint32_t i = 0;
        while (start_sb < sb_cols && i < 64) {
            uint32_t max_w =
                (sb_cols - start_sb) < max_tile_width_sb ? (sb_cols - start_sb) : max_tile_width_sb;
            uint32_t size_sb = av1_read_ns(&br, max_w) + 1;
            if (size_sb > widest_tile_sb)
                widest_tile_sb = size_sb;
            if (i < 63)
                pic_param->width_in_sbs_minus_1[i] = (uint16_t)(size_sb - 1);
            start_sb += size_sb;
            i++;
        }
        tile_cols = i > 0 ? i : 1;
        tile_cols_log2 = ceil_log2_u32(tile_cols);

        uint32_t max_tile_height_sb = sb_rows;
        if (min_log2_tiles > 0) {
            max_tile_height_sb = (sb_cols * sb_rows) >> (min_log2_tiles + 1);
        } else {
            max_tile_height_sb = max_tile_area_sb / widest_tile_sb;
        }
        if (max_tile_height_sb == 0)
            max_tile_height_sb = 1;

        start_sb = 0;
        i = 0;
        while (start_sb < sb_rows && i < 64) {
            uint32_t max_h = (sb_rows - start_sb) < max_tile_height_sb ? (sb_rows - start_sb)
                                                                       : max_tile_height_sb;
            uint32_t size_sb = av1_read_ns(&br, max_h) + 1;
            if (i < 63)
                pic_param->height_in_sbs_minus_1[i] = (uint16_t)(size_sb - 1);
            start_sb += size_sb;
            i++;
        }
        tile_rows = i > 0 ? i : 1;
        tile_rows_log2 = ceil_log2_u32(tile_rows);
    }

    uint16_t context_update_tile_id = 0;
    uint8_t tile_size_bytes_minus_1 = 0;
    if (tile_cols_log2 > 0 || tile_rows_log2 > 0) {
        uint32_t tile_bits = tile_cols_log2 + tile_rows_log2;
        context_update_tile_id = (uint16_t)read_bits(&br, tile_bits);
        tile_size_bytes_minus_1 = (uint8_t)read_bits(&br, 2);
    }
    if (out_tile_size_bytes_minus_1)
        *out_tile_size_bytes_minus_1 = tile_size_bytes_minus_1;

    uint8_t base_q_idx = (uint8_t)read_bits(&br, 8);
    int8_t y_dc_delta_q = read_bit(&br) ? (int8_t)av1_read_su(&br, 7) : 0;
    int8_t u_dc_delta_q = 0, u_ac_delta_q = 0, v_dc_delta_q = 0, v_ac_delta_q = 0;
    if (!seq->mono_chrome) {
        bool diff_uv = seq->separate_uv_delta_q ? (read_bit(&br) != 0) : false;
        u_dc_delta_q = read_bit(&br) ? (int8_t)av1_read_su(&br, 7) : 0;
        u_ac_delta_q = read_bit(&br) ? (int8_t)av1_read_su(&br, 7) : 0;
        if (diff_uv) {
            v_dc_delta_q = read_bit(&br) ? (int8_t)av1_read_su(&br, 7) : 0;
            v_ac_delta_q = read_bit(&br) ? (int8_t)av1_read_su(&br, 7) : 0;
        } else {
            v_dc_delta_q = u_dc_delta_q;
            v_ac_delta_q = u_ac_delta_q;
        }
    }
    if (read_bit(&br)) {
        pic_param->qmatrix_fields.bits.using_qmatrix = 1;
        pic_param->qmatrix_fields.bits.qm_y = read_bits(&br, 4);
        pic_param->qmatrix_fields.bits.qm_u = read_bits(&br, 4);
        pic_param->qmatrix_fields.bits.qm_v =
            seq->separate_uv_delta_q ? read_bits(&br, 4) : pic_param->qmatrix_fields.bits.qm_u;
    }

    /* 1. segmentation_params() (AV1 Spec 5.9.14) */
    bool segmentation_enabled = (read_bit(&br) != 0);
    pic_param->seg_info.segment_info_fields.bits.enabled = segmentation_enabled ? 1 : 0;
    if (segmentation_enabled) {
        bool seg_update_map = false;
        bool seg_temporal_update = false;
        bool seg_update_data = true;
        if (primary_ref_frame == 7) {
            seg_update_map = true;
            seg_temporal_update = false;
            seg_update_data = true;
        } else {
            seg_update_map = (read_bit(&br) != 0);
            if (seg_update_map) {
                seg_temporal_update = (read_bit(&br) != 0);
            }
            seg_update_data = (read_bit(&br) != 0);
        }
        pic_param->seg_info.segment_info_fields.bits.update_map = seg_update_map ? 1 : 0;
        pic_param->seg_info.segment_info_fields.bits.temporal_update = seg_temporal_update ? 1 : 0;
        pic_param->seg_info.segment_info_fields.bits.update_data = seg_update_data ? 1 : 0;
        if (seg_update_data) {
            static const uint8_t seg_feature_bits[8] = {8, 6, 6, 6, 6, 3, 0, 0};
            static const uint8_t seg_feature_signed[8] = {1, 1, 1, 1, 1, 0, 0, 0};
            static const int16_t seg_feature_max[8] = {255, 63, 63, 63, 63, 7, 0, 0};
            memset(pic_param->seg_info.feature_data, 0, sizeof(pic_param->seg_info.feature_data));
            for (int i = 0; i < 8; ++i) {
                uint8_t mask = 0;
                for (int j = 0; j < 8; ++j) {
                    if (read_bit(&br)) {
                        mask |= (uint8_t)(1U << j);
                        int16_t val = 0;
                        if (seg_feature_bits[j] > 0) {
                            if (seg_feature_signed[j]) {
                                val = (int16_t)av1_read_su(&br, (uint32_t)seg_feature_bits[j] + 1U);
                                val = (int16_t)clamp_i32(val, -seg_feature_max[j],
                                                         seg_feature_max[j]);
                            } else {
                                val = (int16_t)read_bits(&br, seg_feature_bits[j]);
                                val = (int16_t)clamp_i32(val, 0, seg_feature_max[j]);
                            }
                        }
                        pic_param->seg_info.feature_data[i][j] = val;
                    }
                }
                pic_param->seg_info.feature_mask[i] = mask;
            }
        } else if (primary_ref_frame < 7) {
            uint8_t prev_slot = ref_frame_idx[primary_ref_frame] & 7U;
            memcpy(pic_param->seg_info.feature_data, dec->seg_feature_data[prev_slot],
                   sizeof(pic_param->seg_info.feature_data));
            memcpy(pic_param->seg_info.feature_mask, dec->seg_feature_mask[prev_slot],
                   sizeof(pic_param->seg_info.feature_mask));
        }
    }

    /* 2. delta_q_params() & delta_lf_params() (AV1 Spec 5.9.15 & 5.9.16) */
    bool delta_q_present = false;
    uint8_t delta_q_res = 0;
    if (base_q_idx > 0) {
        delta_q_present = (read_bit(&br) != 0);
    }
    if (delta_q_present) {
        delta_q_res = (uint8_t)read_bits(&br, 2);
    }
    bool delta_lf_present = false;
    uint8_t delta_lf_res = 0;
    bool delta_lf_multi = false;
    if (delta_q_present) {
        if (!allow_intrabc) {
            delta_lf_present = (read_bit(&br) != 0);
        }
        if (delta_lf_present) {
            delta_lf_res = (uint8_t)read_bits(&br, 2);
            delta_lf_multi = (read_bit(&br) != 0);
        }
    }
    pic_param->mode_control_fields.bits.delta_q_present_flag = delta_q_present ? 1 : 0;
    pic_param->mode_control_fields.bits.log2_delta_q_res = delta_q_res;
    pic_param->mode_control_fields.bits.delta_lf_present_flag = delta_lf_present ? 1 : 0;
    pic_param->mode_control_fields.bits.log2_delta_lf_res = delta_lf_res;
    pic_param->mode_control_fields.bits.delta_lf_multi = delta_lf_multi ? 1 : 0;

    bool coded_lossless = true;
    for (int i = 0; i < 8; ++i) {
        int32_t qindex = base_q_idx;
        if (segmentation_enabled && (pic_param->seg_info.feature_mask[i] & 1U)) {
            qindex = clamp_i32(qindex + pic_param->seg_info.feature_data[i][0], 0, 255);
        }
        bool seg_lossless = (qindex == 0 && y_dc_delta_q == 0 && u_dc_delta_q == 0 &&
                             u_ac_delta_q == 0 && v_dc_delta_q == 0 && v_ac_delta_q == 0);
        if (!seg_lossless) {
            coded_lossless = false;
            break;
        }
    }
    bool all_lossless = coded_lossless && (frame_width == upscaled_width);

    /* 3. loop_filter_params() (AV1 Spec 5.9.11) */
    if (primary_ref_frame < 7) {
        uint8_t prev_slot = ref_frame_idx[primary_ref_frame] & 7U;
        memcpy(pic_param->ref_deltas, dec->loop_filter_ref_deltas[prev_slot],
               sizeof(pic_param->ref_deltas));
        memcpy(pic_param->mode_deltas, dec->loop_filter_mode_deltas[prev_slot],
               sizeof(pic_param->mode_deltas));
    } else {
        pic_param->ref_deltas[0] = 1;
        pic_param->ref_deltas[1] = 0;
        pic_param->ref_deltas[2] = 0;
        pic_param->ref_deltas[3] = 0;
        pic_param->ref_deltas[4] = -1;
        pic_param->ref_deltas[5] = 0;
        pic_param->ref_deltas[6] = -1;
        pic_param->ref_deltas[7] = -1;
        pic_param->mode_deltas[0] = 0;
        pic_param->mode_deltas[1] = 0;
    }
    if (!coded_lossless && !allow_intrabc) {
        pic_param->filter_level[0] = (uint8_t)read_bits(&br, 6);
        pic_param->filter_level[1] = (uint8_t)read_bits(&br, 6);
        if (!seq->mono_chrome && (pic_param->filter_level[0] || pic_param->filter_level[1])) {
            pic_param->filter_level_u = (uint8_t)read_bits(&br, 6);
            pic_param->filter_level_v = (uint8_t)read_bits(&br, 6);
        }
        pic_param->loop_filter_info_fields.bits.sharpness_level = (uint8_t)read_bits(&br, 3);
        bool lf_ref_delta_enabled = (read_bit(&br) != 0);
        bool lf_ref_delta_update = false;
        if (lf_ref_delta_enabled) {
            lf_ref_delta_update = (read_bit(&br) != 0);
            if (lf_ref_delta_update) {
                for (int i = 0; i < 8; ++i) {
                    if (read_bit(&br)) {
                        pic_param->ref_deltas[i] = (int8_t)av1_read_su(&br, 7);
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (read_bit(&br)) {
                        pic_param->mode_deltas[i] = (int8_t)av1_read_su(&br, 7);
                    }
                }
            }
        }
        pic_param->loop_filter_info_fields.bits.mode_ref_delta_enabled =
            lf_ref_delta_enabled ? 1 : 0;
        pic_param->loop_filter_info_fields.bits.mode_ref_delta_update = lf_ref_delta_update ? 1 : 0;
    }

    /* 4. cdef_params() (AV1 Spec 5.9.19) */
    if (!coded_lossless && !allow_intrabc && seq->enable_cdef) {
        pic_param->cdef_damping_minus_3 = (uint8_t)read_bits(&br, 2);
        pic_param->cdef_bits = (uint8_t)read_bits(&br, 2);
        uint32_t num_cdef = 1U << pic_param->cdef_bits;
        for (uint32_t i = 0; i < num_cdef; ++i) {
            uint8_t y_pri = (uint8_t)read_bits(&br, 4);
            uint8_t y_sec = (uint8_t)read_bits(&br, 2);
            pic_param->cdef_y_strengths[i] = (uint8_t)((y_pri << 2) | (y_sec & 0x03U));
            if (!seq->mono_chrome) {
                uint8_t uv_pri = (uint8_t)read_bits(&br, 4);
                uint8_t uv_sec = (uint8_t)read_bits(&br, 2);
                pic_param->cdef_uv_strengths[i] = (uint8_t)((uv_pri << 2) | (uv_sec & 0x03U));
            }
        }
    }

    /* 5. lr_params() (AV1 Spec 5.9.20) */
    if (!all_lossless && !allow_intrabc && seq->enable_restoration) {
        static const uint8_t remap_lr_type[4] = {0, 3, 1, 2};
        bool uses_lr = false;
        bool uses_chroma_lr = false;
        uint8_t lr_type[3] = {0, 0, 0};
        int num_planes = seq->mono_chrome ? 1 : 3;
        for (int i = 0; i < num_planes; ++i) {
            lr_type[i] = remap_lr_type[read_bits(&br, 2)];
            if (lr_type[i] != 0) {
                uses_lr = true;
                if (i > 0)
                    uses_chroma_lr = true;
            }
        }
        pic_param->loop_restoration_fields.bits.yframe_restoration_type = lr_type[0];
        pic_param->loop_restoration_fields.bits.cbframe_restoration_type = lr_type[1];
        pic_param->loop_restoration_fields.bits.crframe_restoration_type = lr_type[2];
        if (uses_lr) {
            uint8_t lr_unit_shift = 0;
            if (seq->use_128x128_superblock) {
                lr_unit_shift = (uint8_t)(read_bit(&br) + 1U);
            } else {
                lr_unit_shift = (uint8_t)read_bit(&br);
                if (lr_unit_shift) {
                    lr_unit_shift = (uint8_t)(lr_unit_shift + read_bit(&br));
                }
            }
            pic_param->loop_restoration_fields.bits.lr_unit_shift = lr_unit_shift;
            if (seq->subsampling_x && seq->subsampling_y && uses_chroma_lr) {
                pic_param->loop_restoration_fields.bits.lr_uv_shift = (uint16_t)read_bit(&br);
            }
        }
    }

    /* 6. read_tx_mode() (AV1 Spec 5.9.21) */
    if (coded_lossless) {
        pic_param->mode_control_fields.bits.tx_mode = 0;
    } else {
        pic_param->mode_control_fields.bits.tx_mode = read_bit(&br) ? 2 : 1;
    }

    /* 7. frame_reference_mode() & skip_mode_params() (AV1 Spec 5.9.22 & 5.9.23) */
    bool frame_is_intra = (frame_type == 0 || frame_type == 2);
    bool reference_select = false;
    if (!frame_is_intra) {
        reference_select = (read_bit(&br) != 0);
    }
    pic_param->mode_control_fields.bits.reference_select = reference_select ? 1 : 0;

    bool skip_mode_allowed = false;
    if (!frame_is_intra && reference_select && seq->enable_order_hint) {
        int forward_idx = -1;
        int backward_idx = -1;
        int32_t forward_hint = 0;
        int32_t backward_hint = 0;
        for (int i = 0; i < 7; ++i) {
            int32_t ref_hint = dec->av1_ref_order_hints[ref_frame_idx[i] & 7];
            if (av1_get_relative_dist(ref_hint, order_hint, seq) < 0) {
                if (forward_idx < 0 || av1_get_relative_dist(ref_hint, forward_hint, seq) > 0) {
                    forward_idx = i;
                    forward_hint = ref_hint;
                }
            } else if (av1_get_relative_dist(ref_hint, order_hint, seq) > 0) {
                if (backward_idx < 0 || av1_get_relative_dist(ref_hint, backward_hint, seq) < 0) {
                    backward_idx = i;
                    backward_hint = ref_hint;
                }
            }
        }
        if (forward_idx >= 0 && backward_idx >= 0) {
            skip_mode_allowed = true;
        } else if (forward_idx >= 0) {
            int second_forward_idx = -1;
            int32_t second_forward_hint = 0;
            for (int i = 0; i < 7; ++i) {
                int32_t ref_hint = dec->av1_ref_order_hints[ref_frame_idx[i] & 7];
                if (av1_get_relative_dist(ref_hint, forward_hint, seq) < 0) {
                    if (second_forward_idx < 0 ||
                        av1_get_relative_dist(ref_hint, second_forward_hint, seq) > 0) {
                        second_forward_idx = i;
                        second_forward_hint = ref_hint;
                    }
                }
            }
            if (second_forward_idx >= 0) {
                skip_mode_allowed = true;
            }
        }
    }
    bool skip_mode_present = skip_mode_allowed ? (read_bit(&br) != 0) : false;
    pic_param->mode_control_fields.bits.skip_mode_present = skip_mode_present ? 1 : 0;

    /* 8. allow_warped_motion & reduced_tx_set (AV1 Spec 5.9.2) */
    bool allow_warped_motion = false;
    if (!frame_is_intra && !error_resilient && seq->enable_warped_motion) {
        allow_warped_motion = (read_bit(&br) != 0);
    }
    bool reduced_tx_set = (read_bit(&br) != 0);
    pic_param->mode_control_fields.bits.reduced_tx_set_used = reduced_tx_set ? 1 : 0;

    /* 9. global_motion_params() (AV1 Spec 5.9.24) */
    if (primary_ref_frame < 7) {
        uint8_t prev_slot = ref_frame_idx[primary_ref_frame] & 7U;
        memcpy(pic_param->wm, dec->wm[prev_slot], sizeof(pic_param->wm));
    } else {
        for (int ref = 0; ref < 7; ++ref) {
            pic_param->wm[ref].wmtype = VAAV1TransformationIdentity;
            pic_param->wm[ref].wmmat[0] = 0;
            pic_param->wm[ref].wmmat[1] = 0;
            pic_param->wm[ref].wmmat[2] = 1 << 16;
            pic_param->wm[ref].wmmat[3] = 0;
            pic_param->wm[ref].wmmat[4] = 0;
            pic_param->wm[ref].wmmat[5] = 1 << 16;
            pic_param->wm[ref].wmmat[6] = 0;
            pic_param->wm[ref].wmmat[7] = 0;
        }
    }
    if (!frame_is_intra) {
        for (int ref = 0; ref < 7; ++ref) {
            VAAV1TransformationType type = VAAV1TransformationIdentity;
            if (read_bit(&br)) {
                if (read_bit(&br)) {
                    type = VAAV1TransformationRotzoom;
                } else {
                    type =
                        read_bit(&br) ? VAAV1TransformationTranslation : VAAV1TransformationAffine;
                }
            }
            pic_param->wm[ref].wmtype = type;
            if (type == VAAV1TransformationIdentity) {
                pic_param->wm[ref].wmmat[0] = 0;
                pic_param->wm[ref].wmmat[1] = 0;
                pic_param->wm[ref].wmmat[2] = 1 << 16;
                pic_param->wm[ref].wmmat[3] = 0;
                pic_param->wm[ref].wmmat[4] = 0;
                pic_param->wm[ref].wmmat[5] = 1 << 16;
                pic_param->wm[ref].wmmat[6] = 0;
                pic_param->wm[ref].wmmat[7] = 0;
            } else {
                if (type >= VAAV1TransformationRotzoom) {
                    av1_read_global_param(&br, type, 2, allow_high_precision_mv,
                                          &pic_param->wm[ref]);
                    av1_read_global_param(&br, type, 3, allow_high_precision_mv,
                                          &pic_param->wm[ref]);
                    if (type == VAAV1TransformationAffine) {
                        av1_read_global_param(&br, type, 4, allow_high_precision_mv,
                                              &pic_param->wm[ref]);
                        av1_read_global_param(&br, type, 5, allow_high_precision_mv,
                                              &pic_param->wm[ref]);
                    } else {
                        pic_param->wm[ref].wmmat[4] = -pic_param->wm[ref].wmmat[3];
                        pic_param->wm[ref].wmmat[5] = pic_param->wm[ref].wmmat[2];
                    }
                }
                if (type >= VAAV1TransformationTranslation) {
                    av1_read_global_param(&br, type, 0, allow_high_precision_mv,
                                          &pic_param->wm[ref]);
                    av1_read_global_param(&br, type, 1, allow_high_precision_mv,
                                          &pic_param->wm[ref]);
                    if (type == VAAV1TransformationTranslation) {
                        pic_param->wm[ref].wmmat[2] = 1 << 16;
                        pic_param->wm[ref].wmmat[3] = 0;
                        pic_param->wm[ref].wmmat[4] = 0;
                        pic_param->wm[ref].wmmat[5] = 1 << 16;
                    }
                    pic_param->wm[ref].wmmat[6] = 0;
                    pic_param->wm[ref].wmmat[7] = 0;
                }
            }
        }
    } else {
        for (int ref = 0; ref < 7; ++ref) {
            pic_param->wm[ref].wmtype = VAAV1TransformationIdentity;
            pic_param->wm[ref].wmmat[0] = 0;
            pic_param->wm[ref].wmmat[1] = 0;
            pic_param->wm[ref].wmmat[2] = 1 << 16;
            pic_param->wm[ref].wmmat[3] = 0;
            pic_param->wm[ref].wmmat[4] = 0;
            pic_param->wm[ref].wmmat[5] = 1 << 16;
            pic_param->wm[ref].wmmat[6] = 0;
            pic_param->wm[ref].wmmat[7] = 0;
        }
    }

    /* 10. film_grain_params() (AV1 Spec 5.9.30) */
    if (seq->film_grain_params_present && (show_frame || showable_frame)) {
        bool apply_grain = (read_bit(&br) != 0);
        pic_param->film_grain_info.film_grain_info_fields.bits.apply_grain = apply_grain ? 1 : 0;
        if (apply_grain) {
            uint16_t grain_seed = (uint16_t)read_bits(&br, 16);
            pic_param->film_grain_info.grain_seed = grain_seed;
            bool update_grain = (frame_type == 1) ? (read_bit(&br) != 0) : true;
            if (!update_grain) {
                uint8_t fg_ref = (uint8_t)read_bits(&br, 3);
                pic_param->film_grain_info = dec->film_grain[fg_ref & 7U];
                pic_param->film_grain_info.film_grain_info_fields.bits.apply_grain = 1;
                pic_param->film_grain_info.grain_seed = grain_seed;
            } else {
                uint8_t num_y_points = (uint8_t)read_bits(&br, 4);
                if (num_y_points > 14)
                    num_y_points = 14;
                pic_param->film_grain_info.num_y_points = num_y_points;
                for (uint8_t i = 0; i < num_y_points; ++i) {
                    pic_param->film_grain_info.point_y_value[i] = (uint8_t)read_bits(&br, 8);
                    pic_param->film_grain_info.point_y_scaling[i] = (uint8_t)read_bits(&br, 8);
                }
                bool chroma_scaling_from_luma = seq->mono_chrome ? false : (read_bit(&br) != 0);
                pic_param->film_grain_info.film_grain_info_fields.bits.chroma_scaling_from_luma =
                    chroma_scaling_from_luma ? 1 : 0;
                uint8_t num_cb_points = 0;
                uint8_t num_cr_points = 0;
                if (!seq->mono_chrome && !chroma_scaling_from_luma &&
                    !(seq->subsampling_x == 1 && seq->subsampling_y == 1 && num_y_points == 0)) {
                    num_cb_points = (uint8_t)read_bits(&br, 4);
                    if (num_cb_points > 10)
                        num_cb_points = 10;
                    for (uint8_t i = 0; i < num_cb_points; ++i) {
                        pic_param->film_grain_info.point_cb_value[i] = (uint8_t)read_bits(&br, 8);
                        pic_param->film_grain_info.point_cb_scaling[i] = (uint8_t)read_bits(&br, 8);
                    }
                    num_cr_points = (uint8_t)read_bits(&br, 4);
                    if (num_cr_points > 10)
                        num_cr_points = 10;
                    for (uint8_t i = 0; i < num_cr_points; ++i) {
                        pic_param->film_grain_info.point_cr_value[i] = (uint8_t)read_bits(&br, 8);
                        pic_param->film_grain_info.point_cr_scaling[i] = (uint8_t)read_bits(&br, 8);
                    }
                }
                pic_param->film_grain_info.num_cb_points = num_cb_points;
                pic_param->film_grain_info.num_cr_points = num_cr_points;
                pic_param->film_grain_info.film_grain_info_fields.bits.grain_scaling_minus_8 =
                    (uint8_t)read_bits(&br, 2);
                uint8_t ar_coeff_lag = (uint8_t)read_bits(&br, 2);
                pic_param->film_grain_info.film_grain_info_fields.bits.ar_coeff_lag = ar_coeff_lag;
                uint32_t num_pos_luma = 2U * ar_coeff_lag * (ar_coeff_lag + 1U);
                uint32_t num_pos_chroma = num_pos_luma;
                if (num_y_points > 0) {
                    num_pos_chroma += 1U;
                    for (uint32_t i = 0; i < num_pos_luma && i < 24; ++i) {
                        pic_param->film_grain_info.ar_coeffs_y[i] =
                            (int8_t)((int32_t)read_bits(&br, 8) - 128);
                    }
                }
                if (chroma_scaling_from_luma || num_cb_points > 0) {
                    for (uint32_t i = 0; i < num_pos_chroma && i < 25; ++i) {
                        pic_param->film_grain_info.ar_coeffs_cb[i] =
                            (int8_t)((int32_t)read_bits(&br, 8) - 128);
                    }
                }
                if (chroma_scaling_from_luma || num_cr_points > 0) {
                    for (uint32_t i = 0; i < num_pos_chroma && i < 25; ++i) {
                        pic_param->film_grain_info.ar_coeffs_cr[i] =
                            (int8_t)((int32_t)read_bits(&br, 8) - 128);
                    }
                }
                pic_param->film_grain_info.film_grain_info_fields.bits.ar_coeff_shift_minus_6 =
                    (uint8_t)read_bits(&br, 2);
                pic_param->film_grain_info.film_grain_info_fields.bits.grain_scale_shift =
                    (uint8_t)read_bits(&br, 2);
                if (num_cb_points > 0) {
                    pic_param->film_grain_info.cb_mult = (uint8_t)read_bits(&br, 8);
                    pic_param->film_grain_info.cb_luma_mult = (uint8_t)read_bits(&br, 8);
                    pic_param->film_grain_info.cb_offset = (uint16_t)read_bits(&br, 9);
                }
                if (num_cr_points > 0) {
                    pic_param->film_grain_info.cr_mult = (uint8_t)read_bits(&br, 8);
                    pic_param->film_grain_info.cr_luma_mult = (uint8_t)read_bits(&br, 8);
                    pic_param->film_grain_info.cr_offset = (uint16_t)read_bits(&br, 9);
                }
                pic_param->film_grain_info.film_grain_info_fields.bits.overlap_flag =
                    (uint8_t)read_bit(&br);
                pic_param->film_grain_info.film_grain_info_fields.bits.clip_to_restricted_range =
                    (uint8_t)read_bit(&br);
            }
        }
    }

    pic_param->profile = seq->seq_profile;
    pic_param->order_hint_bits_minus_1 = seq->order_hint_bits_minus_1;
    pic_param->bit_depth_idx = seq->twelve_bit ? 2 : (seq->high_bitdepth ? 1 : 0);
    pic_param->matrix_coefficients = seq->matrix_coefficients;
    pic_param->seq_info_fields.fields.still_picture = seq->still_picture ? 1 : 0;
    pic_param->seq_info_fields.fields.use_128x128_superblock = seq->use_128x128_superblock ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_filter_intra = seq->enable_filter_intra ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_intra_edge_filter =
        seq->enable_intra_edge_filter ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_interintra_compound =
        seq->enable_interintra_compound ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_masked_compound = seq->enable_masked_compound ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_dual_filter = seq->enable_dual_filter ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_order_hint = seq->enable_order_hint ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_jnt_comp = seq->enable_jnt_comp ? 1 : 0;
    pic_param->seq_info_fields.fields.enable_cdef = seq->enable_cdef ? 1 : 0;
    pic_param->seq_info_fields.fields.mono_chrome = seq->mono_chrome ? 1 : 0;
    pic_param->seq_info_fields.fields.color_range = seq->color_range ? 1 : 0;
    pic_param->seq_info_fields.fields.subsampling_x = seq->subsampling_x;
    pic_param->seq_info_fields.fields.subsampling_y = seq->subsampling_y;
    pic_param->seq_info_fields.fields.film_grain_params_present =
        seq->film_grain_params_present ? 1 : 0;

    pic_param->frame_width_minus1 = width_m1;
    pic_param->frame_height_minus1 = height_m1;
    for (int i = 0; i < 8; ++i) {
        pic_param->ref_frame_map[i] = dec->av1_ref_frames[i];
    }
    for (int i = 0; i < 7; ++i) {
        pic_param->ref_frame_idx[i] = ref_frame_idx[i];
    }
    pic_param->primary_ref_frame = primary_ref_frame;
    pic_param->order_hint = order_hint;
    pic_param->tile_cols = (uint8_t)tile_cols;
    pic_param->tile_rows = (uint8_t)tile_rows;
    pic_param->context_update_tile_id = context_update_tile_id;
    pic_param->pic_info_fields.bits.frame_type = frame_type;
    pic_param->pic_info_fields.bits.show_frame = show_frame;
    pic_param->pic_info_fields.bits.showable_frame = showable_frame;
    pic_param->pic_info_fields.bits.error_resilient_mode = error_resilient;
    pic_param->pic_info_fields.bits.disable_cdf_update = disable_cdf_update;
    pic_param->pic_info_fields.bits.allow_screen_content_tools = allow_screen_content_tools;
    pic_param->pic_info_fields.bits.force_integer_mv = force_integer_mv;
    pic_param->pic_info_fields.bits.allow_intrabc = allow_intrabc;
    pic_param->pic_info_fields.bits.use_superres = use_superres;
    pic_param->pic_info_fields.bits.allow_high_precision_mv = allow_high_precision_mv;
    pic_param->pic_info_fields.bits.is_motion_mode_switchable = is_motion_mode_switchable;
    pic_param->pic_info_fields.bits.use_ref_frame_mvs = use_ref_frame_mvs;
    pic_param->pic_info_fields.bits.disable_frame_end_update_cdf = disable_frame_end_update_cdf;
    pic_param->pic_info_fields.bits.uniform_tile_spacing_flag = uniform_tile_spacing;
    pic_param->pic_info_fields.bits.allow_warped_motion = allow_warped_motion ? 1 : 0;
    pic_param->superres_scale_denominator = superres_denom;
    pic_param->interp_filter = interp_filter;
    pic_param->base_qindex = base_q_idx;
    pic_param->y_dc_delta_q = y_dc_delta_q;
    pic_param->u_dc_delta_q = u_dc_delta_q;
    pic_param->u_ac_delta_q = u_ac_delta_q;
    pic_param->v_dc_delta_q = v_dc_delta_q;
    pic_param->v_ac_delta_q = v_ac_delta_q;

    if (obu_type == 6 && io_slice_count && *io_slice_count < max_slices) {
        size_t hdr_bytes = (br.bit_pos + 7U) / 8U;
        if (hdr_bytes < len) {
            *io_slice_count += parse_av1_tile_group_obu(
                payload, len, hdr_bytes, obu_payload_offset, pic_param, tile_size_bytes_minus_1,
                out_slices + *io_slice_count, max_slices - *io_slice_count);
        }
    }

    for (int i = 0; i < 8; ++i) {
        if (refresh_frame_flags & (1U << i)) {
            dec->av1_ref_order_hints[i] = order_hint;
            memcpy(dec->wm[i], pic_param->wm, sizeof(pic_param->wm));
            memcpy(dec->loop_filter_ref_deltas[i], pic_param->ref_deltas,
                   sizeof(pic_param->ref_deltas));
            memcpy(dec->loop_filter_mode_deltas[i], pic_param->mode_deltas,
                   sizeof(pic_param->mode_deltas));
            memcpy(dec->seg_feature_data[i], pic_param->seg_info.feature_data,
                   sizeof(pic_param->seg_info.feature_data));
            memcpy(dec->seg_feature_mask[i], pic_param->seg_info.feature_mask,
                   sizeof(pic_param->seg_info.feature_mask));
            dec->film_grain[i] = pic_param->film_grain_info;
        }
    }

    *out_refresh_flags = refresh_frame_flags;
    *out_is_keyframe = (frame_type == 0 && show_frame != 0);
    return true;
}

bool submit_av1_frame_to_va(vaapi_decoder_t *dec, uint32_t pool_size,
                            VADecPictureParameterBufferAV1 *pic_param,
                            VASliceParameterBufferAV1 *av1_slices, uint32_t av1_slice_count,
                            const demux_packet_t *pkt, uint8_t refresh_flags,
                            bool av1_show_existing, uint8_t av1_show_existing_idx,
                            VASurfaceID *io_target)
{
    if (av1_show_existing) {
        VASurfaceID ref_surf = dec->av1_ref_frames[av1_show_existing_idx & 7];
        if (ref_surf != VA_INVALID_SURFACE) {
            *io_target = ref_surf;
        }
        return true;
    }

    VASurfaceID target = *io_target;
    for (uint32_t idx = 0; idx < pool_size; ++idx) {
        VASurfaceID s = dec->surfaces[idx];
        bool in_dpb = false;
        for (int r = 0; r < 8; ++r) {
            if (dec->av1_ref_frames[r] == s) {
                in_dpb = true;
                break;
            }
        }
        bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[idx];
        if (!in_dpb && !compositor_busy) {
            dec->current_surface_idx = idx;
            target = s;
            break;
        }
    }
    *io_target = target;
    pic_param->current_frame = target;
    pic_param->current_display_picture = target;
    for (int i = 0; i < 8; ++i) {
        pic_param->ref_frame_map[i] = dec->av1_ref_frames[i];
    }
    for (int i = 0; i < 8; ++i) {
        if (refresh_flags & (1U << i)) {
            dec->av1_ref_frames[i] = target;
            dec->av1_ref_order_hints[i] = pic_param->order_hint;
            memcpy(dec->wm[i], pic_param->wm, sizeof(pic_param->wm));
            memcpy(dec->loop_filter_ref_deltas[i], pic_param->ref_deltas,
                   sizeof(pic_param->ref_deltas));
            memcpy(dec->loop_filter_mode_deltas[i], pic_param->mode_deltas,
                   sizeof(pic_param->mode_deltas));
            memcpy(dec->seg_feature_data[i], pic_param->seg_info.feature_data,
                   sizeof(pic_param->seg_info.feature_data));
            memcpy(dec->seg_feature_mask[i], pic_param->seg_info.feature_mask,
                   sizeof(pic_param->seg_info.feature_mask));
            dec->film_grain[i] = pic_param->film_grain_info;
        }
    }
    if (av1_slice_count == 0) {
        av1_slices[0].slice_data_size = (uint32_t)pkt->size;
        av1_slices[0].slice_data_offset = 0;
        av1_slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        av1_slice_count = 1;
    }

    VAStatus st = vaBeginPicture(dec->va_display, dec->va_context, target);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
        return false;
    }

    VABufferID av1_bufs[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    uint32_t av1_num_bufs = 0;
    if (vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                       sizeof(*pic_param), 1, pic_param,
                       &av1_bufs[av1_num_bufs]) == VA_STATUS_SUCCESS) {
        av1_num_bufs++;
    }
    if (vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                       sizeof(VASliceParameterBufferAV1), av1_slice_count, av1_slices,
                       &av1_bufs[av1_num_bufs]) == VA_STATUS_SUCCESS) {
        av1_num_bufs++;
    }
    if (pkt->size > 0 && pkt->data &&
        vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType, (uint32_t)pkt->size,
                       1, (void *)pkt->data, &av1_bufs[av1_num_bufs]) == VA_STATUS_SUCCESS) {
        av1_num_bufs++;
    }
    if (av1_num_bufs > 0) {
        vaRenderPicture(dec->va_display, dec->va_context, av1_bufs, (int)av1_num_bufs);
        for (uint32_t b = 0; b < av1_num_bufs; ++b) {
            vaDestroyBuffer(dec->va_display, av1_bufs[b]);
        }
    }
    vaEndPicture(dec->va_display, dec->va_context);
    return true;
}
