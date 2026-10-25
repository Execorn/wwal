#ifndef WAYWAL_VAAPI_DEC_H
#define WAYWAL_VAAPI_DEC_H

#include "waywal/demuxer.h"
#include "waywal/dmabuf.h"

#include <stdbool.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_VA_SURFACE_POOL_SIZE 20
#define WAYWAL_VA_VPP_POOL_SIZE 3

typedef struct {
    VASurfaceID surface;
    int32_t frame_num;
    int32_t pic_order_cnt;
    bool is_reference;
    bool is_long_term;
} h264_dpb_entry_t;

typedef struct {
    h264_dpb_entry_t entries[16];
    size_t count;
} h264_dpb_t;

typedef struct {
    uint8_t profile_idc;
    uint8_t level_idc;
    uint8_t chroma_format_idc;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    bool residual_colour_transform_flag;
    uint8_t log2_max_frame_num_minus4;
    uint8_t pic_order_cnt_type;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;
    bool delta_pic_order_always_zero_flag;
    uint8_t num_ref_frames;
    bool gaps_in_frame_num_value_allowed_flag;
    bool frame_mbs_only_flag;
    bool mb_adaptive_frame_field_flag;
    bool direct_8x8_inference_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    bool seq_scaling_matrix_present_flag;
    uint8_t scaling_list_4x4[6][16];
    uint8_t scaling_list_8x8[2][64];
    uint8_t colour_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    bool video_full_range_flag;
    bool valid;
} parsed_sps_t;

typedef struct {
    uint8_t entropy_coding_mode_flag;
    bool bottom_field_pic_order_in_frame_present_flag;
    uint8_t num_slice_groups_minus1;
    uint8_t num_ref_idx_l0_default_active_minus1;
    uint8_t num_ref_idx_l1_default_active_minus1;
    bool weighted_pred_flag;
    uint8_t weighted_bipred_idc;
    int8_t pic_init_qp_minus26;
    int8_t pic_init_qs_minus26;
    int8_t chroma_qp_index_offset;
    int8_t second_chroma_qp_index_offset;
    bool deblocking_filter_control_present_flag;
    bool constrained_intra_pred_flag;
    bool redundant_pic_cnt_present_flag;
    bool transform_8x8_mode_flag;
    bool pic_scaling_matrix_present_flag;
    uint8_t scaling_list_4x4[6][16];
    uint8_t scaling_list_8x8[2][64];
    bool valid;
} parsed_pps_t;

typedef struct {
    uint8_t num_negative_pics;
    uint8_t num_positive_pics;
    int32_t delta_poc_s0[16];
    uint8_t used_by_curr_pic_s0[16];
    int32_t delta_poc_s1[16];
    uint8_t used_by_curr_pic_s1[16];
} hevc_st_rps_t;

typedef struct {
    uint8_t sps_max_sub_layers_minus1;
    uint8_t chroma_format_idc;
    bool separate_colour_plane_flag;
    uint16_t pic_width_in_luma_samples;
    uint16_t pic_height_in_luma_samples;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    uint8_t log2_max_pic_order_cnt_lsb_minus4;
    uint8_t sps_max_dec_pic_buffering_minus1;
    uint8_t sps_max_num_reorder_pics;
    uint8_t log2_min_luma_coding_block_size_minus3;
    uint8_t log2_diff_max_min_luma_coding_block_size;
    uint8_t log2_min_transform_block_size_minus2;
    uint8_t log2_diff_max_min_transform_block_size;
    uint8_t max_transform_hierarchy_depth_inter;
    uint8_t max_transform_hierarchy_depth_intra;
    bool scaling_list_enabled_flag;
    bool amp_enabled_flag;
    bool sample_adaptive_offset_enabled_flag;
    bool pcm_enabled_flag;
    uint8_t pcm_sample_bit_depth_luma_minus1;
    uint8_t pcm_sample_bit_depth_chroma_minus1;
    uint8_t log2_min_pcm_luma_coding_block_size_minus3;
    uint8_t log2_diff_max_min_pcm_luma_coding_block_size;
    bool pcm_loop_filter_disabled_flag;
    uint8_t num_short_term_ref_pic_sets;
    uint8_t st_rps_num_neg[65];
    uint8_t st_rps_num_pos[65];
    hevc_st_rps_t st_rps[65];
    bool long_term_ref_pics_present_flag;
    uint8_t num_long_term_ref_pics_sps;
    bool sps_temporal_mvp_enabled_flag;
    bool strong_intra_smoothing_enabled_flag;
    bool valid;
} parsed_hevc_sps_t;

typedef struct {
    bool dependent_slice_segments_enabled_flag;
    bool output_flag_present_flag;
    uint8_t num_extra_slice_header_bits;
    bool sign_data_hiding_enabled_flag;
    bool cabac_init_present_flag;
    uint8_t num_ref_idx_l0_default_active_minus1;
    uint8_t num_ref_idx_l1_default_active_minus1;
    int8_t init_qp_minus26;
    bool constrained_intra_pred_flag;
    bool transform_skip_enabled_flag;
    bool cu_qp_delta_enabled_flag;
    uint8_t diff_cu_qp_delta_depth;
    int8_t pps_cb_qp_offset;
    int8_t pps_cr_qp_offset;
    bool pps_slice_chroma_qp_offsets_present_flag;
    bool weighted_pred_flag;
    bool weighted_bipred_flag;
    bool transquant_bypass_enabled_flag;
    bool tiles_enabled_flag;
    bool entropy_coding_sync_enabled_flag;
    uint8_t num_tile_columns_minus1;
    uint8_t num_tile_rows_minus1;
    bool uniform_spacing_flag;
    uint16_t column_width_minus1[19];
    uint16_t row_height_minus1[21];
    bool loop_filter_across_tiles_enabled_flag;
    bool pps_loop_filter_across_slices_enabled_flag;
    bool deblocking_filter_control_present_flag;
    bool deblocking_filter_override_enabled_flag;
    bool pps_disable_deblocking_filter_flag;
    int8_t pps_beta_offset_div2;
    int8_t pps_tc_offset_div2;
    bool lists_modification_present_flag;
    uint8_t log2_parallel_merge_level_minus2;
    bool slice_segment_header_extension_present_flag;
    bool valid;
} parsed_hevc_pps_t;

typedef struct {
    uint8_t seq_profile;
    bool still_picture;
    bool reduced_still_picture_header;
    uint8_t frame_width_bits_minus_1;
    uint8_t frame_height_bits_minus_1;
    uint16_t max_frame_width_minus_1;
    uint16_t max_frame_height_minus_1;
    bool frame_id_numbers_present_flag;
    uint8_t delta_frame_id_length_minus_2;
    uint8_t additional_frame_id_length_minus_1;
    bool use_128x128_superblock;
    bool enable_filter_intra;
    bool enable_intra_edge_filter;
    bool enable_interintra_compound;
    bool enable_masked_compound;
    bool enable_warped_motion;
    bool enable_dual_filter;
    bool enable_order_hint;
    bool enable_jnt_comp;
    bool enable_ref_frame_mvs;
    uint8_t seq_force_screen_content_tools;
    uint8_t seq_force_integer_mv;
    uint8_t order_hint_bits_minus_1;
    bool enable_superres;
    bool enable_cdef;
    bool enable_restoration;
    bool high_bitdepth;
    bool twelve_bit;
    bool mono_chrome;
    uint8_t color_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    bool color_range;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    bool separate_uv_delta_q;
    bool film_grain_params_present;
    bool valid;
} parsed_av1_seq_t;

typedef struct {
    VADisplay va_display;
    int drm_fd;
    bool owns_drm_fd;
    bool is_nvidia_vaapi;
    VAConfigID va_config;
    VAContextID va_context;
    VAProfile va_profile;
    VASurfaceID surfaces[WAYWAL_VA_SURFACE_POOL_SIZE];
    uint32_t num_surfaces;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc; /* VA_FOURCC_NV12 or VA_FOURCC_P010 */
    size_t current_surface_idx;
    uint32_t frame_num;
    int32_t prev_poc_msb;
    int32_t prev_poc_lsb;
    int32_t last_decoded_poc;
    bool last_decoded_is_idr;
    bool is_avcc;
    uint8_t nal_length_size;
    h264_dpb_t dpb;
    parsed_sps_t cached_sps;
    parsed_pps_t cached_pps;
    parsed_hevc_sps_t cached_hevc_sps;
    parsed_hevc_pps_t cached_hevc_pps;
    parsed_av1_seq_t cached_av1_seq;
    VASurfaceID vp9_ref_frames[8];
    VASurfaceID av1_ref_frames[8];
    uint8_t av1_ref_order_hints[8];
    uint8_t vp9_subsampling_x;
    uint8_t vp9_subsampling_y;
    uint8_t vp9_bit_depth;
    uint8_t vp9_color_space;
    uint8_t vp9_color_range;
    int8_t vp9_ref_deltas[4];
    int8_t vp9_mode_deltas[2];
    uint8_t vp9_mb_segment_tree_probs[7];
    uint8_t vp9_segment_pred_probs[3];
    bool vp9_seg_abs_delta;
    uint8_t vp9_seg_feature_enabled[8][4];
    int16_t vp9_seg_feature_data[8][4];
    bool allow_direct_p010;
    VAIQMatrixBufferH264 cached_iq;
    bool iq_valid;
    VAConfigID vpp_config;
    VAContextID vpp_context;
    VASurfaceID vpp_surfaces[WAYWAL_VA_SURFACE_POOL_SIZE];
    uint32_t num_vpp_surfaces;
    bool has_vpp;
    bool vpp_warmed_up;
    bool initialized;
    const bool *surface_busy_mask; /* External compositor in-use flags (WAYWAL_VA_SURFACE_POOL_SIZE) */
} vaapi_decoder_t;

/* Exported PRIME 2 DMA-BUF representation */
typedef struct {
    int fds[4];
    uint32_t strides[4];
    uint32_t offsets[4];
    uint64_t modifiers[4];
    uint32_t num_planes;
    uint32_t drm_format;
    uint32_t width;
    uint32_t height;
    VASurfaceID surface_id;
} vaapi_prime_frame_t;

/* Initialize hardware VA-API decoder on DRM file descriptor */
bool vaapi_decoder_init(vaapi_decoder_t *dec, int drm_render_fd, waywal_codec_t codec,
                        uint32_t width, uint32_t height);

/* Initialize hardware VA-API decoder with extradata for DPB pool sizing and direct NV12 hint */
bool vaapi_decoder_init_with_params(vaapi_decoder_t *dec, int drm_render_fd, waywal_codec_t codec,
                                    uint32_t width, uint32_t height, const uint8_t *extradata,
                                    size_t extradata_size, bool allow_direct_nv12);

/* Sets codec extradata (e.g. SPS/PPS from MP4 avcC or WebM) */
bool vaapi_decoder_set_extradata(vaapi_decoder_t *dec, const uint8_t *extradata, size_t size);

/* Destroy VA-API decoder and release all surfaces and context */
void vaapi_decoder_destroy(vaapi_decoder_t *dec);

/* Reset decoder state (DPB, frame_num) */
void vaapi_decoder_reset(vaapi_decoder_t *dec);

/* Submits elementary stream packet to hardware ASIC */
bool vaapi_decoder_decode_packet(vaapi_decoder_t *dec, const demux_packet_t *pkt,
                                 VASurfaceID *out_surface);

/* Converts a decoded YUV surface to an RGB32 VPP surface on-demand */
bool vaapi_decoder_vpp_blit(vaapi_decoder_t *dec, VASurfaceID src_surface,
                            VASurfaceID dst_vpp_surface);

/* Exports decoded hardware surface directly to PRIME 2 DMA-BUF */
bool vaapi_decoder_export_prime(vaapi_decoder_t *dec, VASurfaceID surface,
                                vaapi_prime_frame_t *out_frame);

/* Releases exported PRIME file descriptors */
void vaapi_prime_frame_close(vaapi_prime_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_VAAPI_DEC_H */
