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
    VADisplay va_display;
    int drm_fd;
    bool owns_drm_fd;
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
