#include "waywal/vaapi_dec.h"
#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <drm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
#ifdef VAProfileAV1Profile0
#include <va/va_dec_av1.h>
#endif

static bool profile_supported(const VAProfile *profiles, int count, VAProfile target) {
    for (int i = 0; i < count; ++i) {
        if (profiles[i] == target) return true;
    }
    return false;
}

static VAProfile select_va_profile(VADisplay dpy, waywal_codec_t codec) {
    int max_profiles = vaMaxNumProfiles(dpy);
    if (max_profiles <= 0) return VAProfileNone;

    VAProfile *profiles = (VAProfile *)calloc((size_t)max_profiles, sizeof(VAProfile));
    if (!profiles) return VAProfileNone;

    int num_profiles = 0;
    VAStatus st = vaQueryConfigProfiles(dpy, profiles, &num_profiles);
    if (st != VA_STATUS_SUCCESS) {
        free(profiles);
        return VAProfileNone;
    }

    VAProfile chosen = VAProfileNone;
    switch (codec) {
        case WAYWAL_CODEC_H264:
            if (profile_supported(profiles, num_profiles, VAProfileH264Main)) chosen = VAProfileH264Main;
            else if (profile_supported(profiles, num_profiles, VAProfileH264High)) chosen = VAProfileH264High;
            else if (profile_supported(profiles, num_profiles, VAProfileH264ConstrainedBaseline)) chosen = VAProfileH264ConstrainedBaseline;
            break;

        case WAYWAL_CODEC_HEVC:
            if (profile_supported(profiles, num_profiles, VAProfileHEVCMain)) chosen = VAProfileHEVCMain;
            else if (profile_supported(profiles, num_profiles, VAProfileHEVCMain10)) chosen = VAProfileHEVCMain10;
            break;

        case WAYWAL_CODEC_VP9:
            if (profile_supported(profiles, num_profiles, VAProfileVP9Profile0)) chosen = VAProfileVP9Profile0;
            else if (profile_supported(profiles, num_profiles, VAProfileVP9Profile2)) chosen = VAProfileVP9Profile2;
            break;

        case WAYWAL_CODEC_AV1:
#ifdef VAProfileAV1Profile0
            if (profile_supported(profiles, num_profiles, VAProfileAV1Profile0)) chosen = VAProfileAV1Profile0;
#endif
            break;

        default:
            break;
    }

    free(profiles);
    return chosen;
}

bool vaapi_decoder_init(vaapi_decoder_t *dec, int drm_render_fd, waywal_codec_t codec, uint32_t width, uint32_t height) {
    if (!dec || width == 0 || height == 0) return false;
    memset(dec, 0, sizeof(*dec));
    dec->width = width;
    dec->height = height;
    dec->fourcc = VA_FOURCC_NV12;

    const char *candidate_nodes[] = { NULL, "/dev/dri/renderD128", "/dev/dri/renderD129" };
    bool found = false;

    for (int i = 0; i < 3; ++i) {
        int fd = -1;
        bool owns_fd = false;
        if (i == 0) {
            if (drm_render_fd < 0) continue;
            fd = drm_render_fd;
            owns_fd = false;
        } else {
            fd = open(candidate_nodes[i], O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            owns_fd = true;
        }

        VADisplay dpy = vaGetDisplayDRM(fd);
        if (!dpy) {
            if (owns_fd) close(fd);
            continue;
        }

        int major = 0, minor = 0;
        VAStatus st = vaInitialize(dpy, &major, &minor);
        if (st != VA_STATUS_SUCCESS) {
            vaTerminate(dpy);
            if (owns_fd) close(fd);
            continue;
        }

        VAProfile prof = select_va_profile(dpy, codec);
        if (prof == VAProfileNone) {
            vaTerminate(dpy);
            if (owns_fd) close(fd);
            continue;
        }

        dec->drm_fd = fd;
        dec->owns_drm_fd = owns_fd;
        dec->va_display = dpy;
        dec->va_profile = prof;
        found = true;
        WAYWAL_LOG_INFO("VA-API v%d.%d initialized on DRM node %s (Profile: %d, Driver: %s)",
                      major, minor, candidate_nodes[i] ? candidate_nodes[i] : "supplied fd",
                      dec->va_profile, vaQueryVendorString(dec->va_display));
        break;
    }

    if (!found) {
        WAYWAL_LOG_ERR("Failed to find working VA-API DRM node for codec %d", codec);
        return false;
    }

    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAStatus st = vaCreateConfig(dec->va_display, dec->va_profile, VAEntrypointVLD, &attrib, 1, &dec->va_config);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateConfig failed: %s", vaErrorStr(st));
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0) close(dec->drm_fd);
        return false;
    }

    VASurfaceAttrib sattrs[2];
    sattrs[0].type = VASurfaceAttribPixelFormat;
    sattrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattrs[0].value.type = VAGenericValueTypeInteger;
    sattrs[0].value.value.i = VA_FOURCC_NV12;

    sattrs[1].type = VASurfaceAttribUsageHint;
    sattrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattrs[1].value.type = VAGenericValueTypeInteger;
    sattrs[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER;

    st = vaCreateSurfaces(dec->va_display, VA_RT_FORMAT_YUV420, width, height,
                          dec->surfaces, WAYWAL_VA_SURFACE_POOL_SIZE, sattrs, 2);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateSurfaces failed for %ux%u pool: %s", width, height, vaErrorStr(st));
        vaDestroyConfig(dec->va_display, dec->va_config);
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0) close(dec->drm_fd);
        return false;
    }

    st = vaCreateContext(dec->va_display, dec->va_config, (int)width, (int)height,
                         VA_PROGRESSIVE, dec->surfaces, WAYWAL_VA_SURFACE_POOL_SIZE,
                         &dec->va_context);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateContext failed: %s", vaErrorStr(st));
        vaDestroySurfaces(dec->va_display, dec->surfaces, WAYWAL_VA_SURFACE_POOL_SIZE);
        vaDestroyConfig(dec->va_display, dec->va_config);
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0) close(dec->drm_fd);
        return false;
    }

    dec->initialized = true;
    dec->current_surface_idx = 0;
    return true;
}

void vaapi_decoder_destroy(vaapi_decoder_t *dec) {
    if (!dec || !dec->initialized) return;

    if (dec->va_context != VA_INVALID_ID) {
        vaDestroyContext(dec->va_display, dec->va_context);
        dec->va_context = VA_INVALID_ID;
    }

    vaDestroySurfaces(dec->va_display, dec->surfaces, WAYWAL_VA_SURFACE_POOL_SIZE);

    if (dec->va_config != VA_INVALID_ID) {
        vaDestroyConfig(dec->va_display, dec->va_config);
        dec->va_config = VA_INVALID_ID;
    }

    if (dec->va_display) {
        vaTerminate(dec->va_display);
        dec->va_display = NULL;
    }

    if (dec->owns_drm_fd && dec->drm_fd >= 0) {
        close(dec->drm_fd);
        dec->drm_fd = -1;
    }

    dec->initialized = false;
}

/* Bitstream reader and Exp-Golomb parser for H.264 SPS/PPS (BUG-06) */
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit_pos;
} bit_reader_t;

static inline void bit_reader_init(bit_reader_t *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->bit_pos = 0;
}

static inline uint32_t read_bit(bit_reader_t *br) {
    if (br->bit_pos >= br->size * 8) return 0;
    size_t byte_idx = br->bit_pos / 8;
    uint32_t bit_idx = 7 - (br->bit_pos % 8);
    br->bit_pos++;
    return (br->data[byte_idx] >> bit_idx) & 1;
}

static inline uint32_t read_bits(bit_reader_t *br, uint32_t n) {
    uint32_t val = 0;
    for (uint32_t i = 0; i < n; ++i) {
        val = (val << 1) | read_bit(br);
    }
    return val;
}

static inline uint32_t read_ue(bit_reader_t *br) {
    uint32_t zeros = 0;
    while (read_bit(br) == 0 && br->bit_pos < br->size * 8) {
        zeros++;
        if (zeros > 31) return 0;
    }
    if (zeros == 0) return 0;
    uint32_t val = read_bits(br, zeros);
    return (1U << zeros) - 1 + val;
}

static inline int32_t read_se(bit_reader_t *br) {
    uint32_t ue = read_ue(br);
    if (ue % 2 == 1) {
        return (int32_t)((ue + 1) / 2);
    } else {
        return -(int32_t)(ue / 2);
    }
}

static size_t unescape_rbsp(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_len) {
        if (si + 2 < src_len && src[si] == 0 && src[si + 1] == 0 && src[si + 2] == 3) {
            dst[di++] = 0;
            dst[di++] = 0;
            si += 3;
        } else {
            dst[di++] = src[si++];
        }
    }
    return di;
}

typedef struct {
    uint8_t  profile_idc;
    uint8_t  level_idc;
    uint8_t  chroma_format_idc;
    uint8_t  log2_max_frame_num_minus4;
    uint8_t  pic_order_cnt_type;
    uint8_t  log2_max_pic_order_cnt_lsb_minus4;
    uint8_t  num_ref_frames;
    bool     frame_mbs_only_flag;
    bool     direct_8x8_inference_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    bool     valid;
} parsed_sps_t;

typedef struct {
    uint8_t  entropy_coding_mode_flag;
    bool     bottom_field_pic_order_in_frame_present_flag;
    uint8_t  num_slice_groups_minus1;
    uint8_t  num_ref_idx_l0_default_active_minus1;
    uint8_t  num_ref_idx_l1_default_active_minus1;
    uint8_t  weighted_bipred_idc;
    int8_t   pic_init_qp_minus26;
    bool     deblocking_filter_control_present_flag;
    bool     constrained_intra_pred_flag;
    bool     redundant_pic_cnt_present_flag;
    bool     transform_8x8_mode_flag;
    bool     valid;
} parsed_pps_t;

static bool parse_h264_sps(const uint8_t *nal, size_t nal_len, parsed_sps_t *sps) {
    if (nal_len < 4) return false;
    uint8_t rbsp[512];
    size_t rbsp_len = unescape_rbsp(nal, nal_len, rbsp, sizeof(rbsp));
    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    uint32_t forbidden = read_bit(&br);
    uint32_t ref_idc = read_bits(&br, 2);
    uint32_t nal_type = read_bits(&br, 5);
    (void)forbidden; (void)ref_idc;
    if (nal_type != 7) return false;

    sps->profile_idc = (uint8_t)read_bits(&br, 8);
    read_bits(&br, 8); /* constraint flags */
    sps->level_idc = (uint8_t)read_bits(&br, 8);
    read_ue(&br); /* seq_parameter_set_id */

    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44  || sps->profile_idc == 83  ||
        sps->profile_idc == 86  || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138) {
        sps->chroma_format_idc = (uint8_t)read_ue(&br);
        if (sps->chroma_format_idc == 3) {
            read_bit(&br);
        }
        read_ue(&br);
        read_ue(&br);
        read_bit(&br);
        uint32_t seq_scaling_matrix_present = read_bit(&br);
        if (seq_scaling_matrix_present) {
            uint32_t num_lists = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (uint32_t i = 0; i < num_lists; ++i) {
                if (read_bit(&br)) {
                    uint32_t size = (i < 6) ? 16 : 64;
                    uint32_t last_scale = 8, next_scale = 8;
                    for (uint32_t j = 0; j < size; ++j) {
                        if (next_scale != 0) {
                            int32_t delta_scale = read_se(&br);
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    } else {
        sps->chroma_format_idc = 1;
    }

    sps->log2_max_frame_num_minus4 = (uint8_t)read_ue(&br);
    sps->pic_order_cnt_type = (uint8_t)read_ue(&br);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)read_ue(&br);
    } else if (sps->pic_order_cnt_type == 1) {
        read_bit(&br);
        read_se(&br);
        read_se(&br);
        uint32_t num_ref = read_ue(&br);
        for (uint32_t i = 0; i < num_ref; ++i) {
            read_se(&br);
        }
    }

    sps->num_ref_frames = (uint8_t)read_ue(&br);
    read_bit(&br);
    sps->pic_width_in_mbs_minus1 = read_ue(&br);
    sps->pic_height_in_map_units_minus1 = read_ue(&br);
    sps->frame_mbs_only_flag = read_bit(&br) != 0;
    if (!sps->frame_mbs_only_flag) {
        read_bit(&br);
    }
    sps->direct_8x8_inference_flag = read_bit(&br) != 0;
    sps->valid = true;
    return true;
}

static bool parse_h264_pps(const uint8_t *nal, size_t nal_len, parsed_pps_t *pps) {
    if (nal_len < 2) return false;
    uint8_t rbsp[512];
    size_t rbsp_len = unescape_rbsp(nal, nal_len, rbsp, sizeof(rbsp));
    bit_reader_t br;
    bit_reader_init(&br, rbsp, rbsp_len);

    uint32_t forbidden = read_bit(&br);
    uint32_t ref_idc = read_bits(&br, 2);
    uint32_t nal_type = read_bits(&br, 5);
    (void)forbidden; (void)ref_idc;
    if (nal_type != 8) return false;

    read_ue(&br);
    read_ue(&br);
    pps->entropy_coding_mode_flag = (uint8_t)read_bit(&br);
    pps->bottom_field_pic_order_in_frame_present_flag = read_bit(&br) != 0;
    pps->num_slice_groups_minus1 = (uint8_t)read_ue(&br);
    pps->num_ref_idx_l0_default_active_minus1 = (uint8_t)read_ue(&br);
    pps->num_ref_idx_l1_default_active_minus1 = (uint8_t)read_ue(&br);
    read_bit(&br);
    pps->weighted_bipred_idc = (uint8_t)read_bits(&br, 2);
    pps->pic_init_qp_minus26 = (int8_t)read_se(&br);
    read_se(&br);
    read_se(&br);
    pps->deblocking_filter_control_present_flag = read_bit(&br) != 0;
    pps->constrained_intra_pred_flag = read_bit(&br) != 0;
    pps->redundant_pic_cnt_present_flag = read_bit(&br) != 0;
    if (br.bit_pos < br.size * 8) {
        pps->transform_8x8_mode_flag = read_bit(&br) != 0;
    }
    pps->valid = true;
    return true;
}

bool vaapi_decoder_decode_packet(vaapi_decoder_t *dec, const demux_packet_t *pkt, VASurfaceID *out_surface) {
    if (!dec || !dec->initialized || !pkt || !out_surface) return false;

    VASurfaceID target = dec->surfaces[dec->current_surface_idx];

    VAStatus st = vaBeginPicture(dec->va_display, dec->va_context, target);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
        return false;
    }

    VABufferID bufs[4];
    uint32_t num_bufs = 0;

    /* Codec specific parameter buffers */
    if (dec->va_profile == VAProfileH264Main ||
        dec->va_profile == VAProfileH264High ||
        dec->va_profile == VAProfileH264ConstrainedBaseline) {

        /* Scan packet for SPS/PPS NAL units to update parameters dynamically (BUG-06) */
        parsed_sps_t sps = { .log2_max_frame_num_minus4 = 4, .log2_max_pic_order_cnt_lsb_minus4 = 4,
                             .frame_mbs_only_flag = true, .direct_8x8_inference_flag = true, .valid = false };
        parsed_pps_t pps = { .entropy_coding_mode_flag = 1, .deblocking_filter_control_present_flag = true, .valid = false };

        uint32_t slice_data_offset = 0;
        uint32_t slice_type = pkt->is_keyframe ? 2 : 0;

        if (pkt->size >= 4) {
            if (pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1) {
                slice_data_offset = 3;
            } else if (pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 0 && pkt->data[3] == 1) {
                slice_data_offset = 4;
            } else {
                /* MP4 4-byte big-endian NAL length prefix (BUG-06) */
                slice_data_offset = 4;
            }

            /* Search for SPS / PPS in packet */
            size_t scan_off = 0;
            while (scan_off + 4 < pkt->size) {
                uint32_t nal_len = 0;
                size_t nal_hdr_len = 0;
                if (pkt->data[scan_off] == 0 && pkt->data[scan_off + 1] == 0 && pkt->data[scan_off + 2] == 1) {
                    nal_hdr_len = 3;
                } else if (pkt->data[scan_off] == 0 && pkt->data[scan_off + 1] == 0 &&
                           pkt->data[scan_off + 2] == 0 && pkt->data[scan_off + 3] == 1) {
                    nal_hdr_len = 4;
                } else {
                    nal_len = ((uint32_t)pkt->data[scan_off] << 24) |
                              ((uint32_t)pkt->data[scan_off + 1] << 16) |
                              ((uint32_t)pkt->data[scan_off + 2] << 8) |
                              ((uint32_t)pkt->data[scan_off + 3]);
                    nal_hdr_len = 4;
                }

                if (nal_hdr_len == 4 && nal_len > 0 && scan_off + 4 + nal_len <= pkt->size) {
                    uint8_t ntype = pkt->data[scan_off + 4] & 0x1F;
                    if (ntype == 7) parse_h264_sps(pkt->data + scan_off + 4, nal_len, &sps);
                    else if (ntype == 8) parse_h264_pps(pkt->data + scan_off + 4, nal_len, &pps);
                    scan_off += 4 + nal_len;
                } else {
                    break;
                }
            }
        }

        VAPictureParameterBufferH264 pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.CurrPic.picture_id = target;
        pic_param.CurrPic.frame_idx = (uint16_t)dec->frame_num;
        pic_param.CurrPic.flags = pkt->is_keyframe ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
        pic_param.CurrPic.TopFieldOrderCnt = (int32_t)(dec->frame_num * 2);
        pic_param.CurrPic.BottomFieldOrderCnt = (int32_t)(dec->frame_num * 2);
        pic_param.picture_width_in_mbs_minus1 = (uint16_t)((dec->width + 15) / 16 - 1);
        pic_param.picture_height_in_mbs_minus1 = (uint16_t)((dec->height + 15) / 16 - 1);
        pic_param.seq_fields.bits.frame_mbs_only_flag = sps.valid ? sps.frame_mbs_only_flag : 1;
        pic_param.seq_fields.bits.direct_8x8_inference_flag = sps.valid ? sps.direct_8x8_inference_flag : 1;
        pic_param.seq_fields.bits.log2_max_frame_num_minus4 = sps.valid ? sps.log2_max_frame_num_minus4 : 4;
        pic_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = sps.valid ? sps.log2_max_pic_order_cnt_lsb_minus4 : 4;
        pic_param.seq_fields.bits.pic_order_cnt_type = sps.valid ? sps.pic_order_cnt_type : 0;
        pic_param.pic_fields.bits.entropy_coding_mode_flag = pps.valid ? pps.entropy_coding_mode_flag : 1;
        pic_param.pic_fields.bits.deblocking_filter_control_present_flag = pps.valid ? pps.deblocking_filter_control_present_flag : 1;
        pic_param.pic_fields.bits.weighted_bipred_idc = pps.valid ? pps.weighted_bipred_idc : 0;
        pic_param.pic_fields.bits.transform_8x8_mode_flag = pps.valid ? pps.transform_8x8_mode_flag : 0;

        /* Populate ReferenceFrames from DPB (BUG-06) */
        for (int i = 0; i < 16; ++i) {
            if (i < (int)dec->dpb.count) {
                pic_param.ReferenceFrames[i].picture_id = dec->dpb.entries[i].surface;
                pic_param.ReferenceFrames[i].frame_idx = (uint16_t)dec->dpb.entries[i].frame_num;
                pic_param.ReferenceFrames[i].flags = dec->dpb.entries[i].is_long_term
                    ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                    : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
                pic_param.ReferenceFrames[i].TopFieldOrderCnt = dec->dpb.entries[i].pic_order_cnt;
                pic_param.ReferenceFrames[i].BottomFieldOrderCnt = dec->dpb.entries[i].pic_order_cnt;
            } else {
                pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
            }
        }

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS) {
            bufs[num_bufs++] = pic_buf;
        }

        VASliceParameterBufferH264 slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = slice_data_offset;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slice_param.slice_type = slice_type;

        /* Populate RefPicList0 and RefPicList1 for P/B-frames (BUG-06) */
        for (int i = 0; i < 32; ++i) {
            if (i < (int)dec->dpb.count) {
                size_t idx = dec->dpb.count - 1 - (size_t)i;
                slice_param.RefPicList0[i].picture_id = dec->dpb.entries[idx].surface;
                slice_param.RefPicList0[i].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
                slice_param.RefPicList0[i].frame_idx = (uint16_t)dec->dpb.entries[idx].frame_num;
                slice_param.RefPicList0[i].TopFieldOrderCnt = dec->dpb.entries[idx].pic_order_cnt;
                slice_param.RefPicList0[i].BottomFieldOrderCnt = dec->dpb.entries[idx].pic_order_cnt;
            } else {
                slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
                slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
            }

            if (slice_type == 1 && i < (int)dec->dpb.count) {
                slice_param.RefPicList1[i] = slice_param.RefPicList0[i];
            } else {
                slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
                slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
            }
        }

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS) {
            bufs[num_bufs++] = slice_buf;
        }
    } else if (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10) {
        /* HEVC Parameter Buffer Builder (BUG-06) */
        VAPictureParameterBufferHEVC pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.CurrPic.picture_id = target;
        pic_param.CurrPic.pic_order_cnt = (int32_t)(dec->frame_num * 2);
        pic_param.CurrPic.flags = pkt->is_keyframe ? 0 : VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        pic_param.pic_width_in_luma_samples = (uint16_t)dec->width;
        pic_param.pic_height_in_luma_samples = (uint16_t)dec->height;
        for (int i = 0; i < 15; ++i) {
            if (i < (int)dec->dpb.count) {
                pic_param.ReferenceFrames[i].picture_id = dec->dpb.entries[i].surface;
                pic_param.ReferenceFrames[i].pic_order_cnt = dec->dpb.entries[i].pic_order_cnt;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
            } else {
                pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
            }
        }
        pic_param.pic_fields.bits.chroma_format_idc = 1;
        pic_param.log2_diff_max_min_luma_coding_block_size = 3;
        pic_param.log2_diff_max_min_transform_block_size = 3;

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = pic_buf;

        VASliceParameterBufferHEVC slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = 0;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slice_param.LongSliceFlags.fields.slice_type = pkt->is_keyframe ? 2 : 0;
        slice_param.LongSliceFlags.fields.LastSliceOfPic = 1;
        for (int i = 0; i < 15; ++i) {
            slice_param.RefPicList[0][i] = (i < (int)dec->dpb.count) ? (uint8_t)i : 0xFF;
            slice_param.RefPicList[1][i] = 0xFF;
        }

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = slice_buf;
    } else if (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2) {
        /* VP9 Parameter Buffer Builder (BUG-06) */
        VADecPictureParameterBufferVP9 pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.frame_width = (uint16_t)dec->width;
        pic_param.frame_height = (uint16_t)dec->height;
        pic_param.pic_fields.bits.subsampling_x = 1;
        pic_param.pic_fields.bits.subsampling_y = 1;
        pic_param.pic_fields.bits.frame_type = pkt->is_keyframe ? 0 : 1;
        pic_param.pic_fields.bits.show_frame = 1;
        for (int i = 0; i < 8; ++i) {
            pic_param.reference_frames[i] = (i < (int)dec->dpb.count) ? dec->dpb.entries[i].surface : VA_INVALID_SURFACE;
        }

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = pic_buf;

        VASliceParameterBufferVP9 slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = 0;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = slice_buf;
#ifdef VAProfileAV1Profile0
    } else if (dec->va_profile == VAProfileAV1Profile0) {
        /* AV1 Parameter Buffer Builder (BUG-06) */
        VADecPictureParameterBufferAV1 pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.current_frame = target;
        pic_param.frame_width_minus_1 = (uint16_t)(dec->width - 1);
        pic_param.frame_height_minus_1 = (uint16_t)(dec->height - 1);
        for (int i = 0; i < 8; ++i) {
            pic_param.ref_frame_map[i] = (i < (int)dec->dpb.count) ? dec->dpb.entries[i].surface : VA_INVALID_SURFACE;
        }

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = pic_buf;

        VASliceParameterBufferAV1 slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = 0;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS) bufs[num_bufs++] = slice_buf;
#endif
    }

    /* Slice bitstream data buffer */
    if (pkt->size > 0 && pkt->data) {
        VABufferID data_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                            pkt->size, 1, (void *)pkt->data, &data_buf);
        if (st == VA_STATUS_SUCCESS) {
            bufs[num_bufs++] = data_buf;
        }
    }

    if (num_bufs > 0) {
        st = vaRenderPicture(dec->va_display, dec->va_context, bufs, (int)num_bufs);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_WARN("vaRenderPicture returned: %s", vaErrorStr(st));
        }
    }

    st = vaEndPicture(dec->va_display, dec->va_context);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("vaEndPicture returned: %s", vaErrorStr(st));
    }

    for (uint32_t i = 0; i < num_bufs; ++i) {
        vaDestroyBuffer(dec->va_display, bufs[i]);
    }

    st = vaSyncSurface(dec->va_display, target);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("vaSyncSurface returned: %s", vaErrorStr(st));
    }

    /* Update DPB state machine (BUG-06) */
    if (pkt->is_keyframe) {
        dec->dpb.count = 0;
        dec->dpb.entries[0] = (h264_dpb_entry_t){
            .surface = target,
            .frame_num = (int32_t)dec->frame_num,
            .pic_order_cnt = (int32_t)(dec->frame_num * 2),
            .is_reference = true,
            .is_long_term = false,
        };
        dec->dpb.count = 1;
    } else {
        if (dec->dpb.count < 16) {
            dec->dpb.entries[dec->dpb.count++] = (h264_dpb_entry_t){
                .surface = target,
                .frame_num = (int32_t)dec->frame_num,
                .pic_order_cnt = (int32_t)(dec->frame_num * 2),
                .is_reference = true,
                .is_long_term = false,
            };
        } else {
            memmove(&dec->dpb.entries[0], &dec->dpb.entries[1], 15 * sizeof(h264_dpb_entry_t));
            dec->dpb.entries[15] = (h264_dpb_entry_t){
                .surface = target,
                .frame_num = (int32_t)dec->frame_num,
                .pic_order_cnt = (int32_t)(dec->frame_num * 2),
                .is_reference = true,
                .is_long_term = false,
            };
        }
    }
    dec->frame_num++;

    *out_surface = target;
    dec->current_surface_idx = (dec->current_surface_idx + 1) % WAYWAL_VA_SURFACE_POOL_SIZE;
    return true;
}

bool vaapi_decoder_export_prime(vaapi_decoder_t *dec, VASurfaceID surface, vaapi_prime_frame_t *out_frame) {
    if (!dec || !dec->initialized || surface == VA_INVALID_SURFACE || !out_frame) {
        return false;
    }
    memset(out_frame, 0, sizeof(*out_frame));

    VADRMPRIMESurfaceDescriptor prime_desc;
    memset(&prime_desc, 0, sizeof(prime_desc));

    VAStatus st = vaExportSurfaceHandle(
        dec->va_display,
        surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &prime_desc
    );

    if (st != VA_STATUS_SUCCESS) {
        /* Retry with separate layers if composed fails */
        st = vaExportSurfaceHandle(
            dec->va_display,
            surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &prime_desc
        );
    }

    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaExportSurfaceHandle failed for surface %u: %s", surface, vaErrorStr(st));
        return false;
    }

    out_frame->surface_id = surface;
    out_frame->width = prime_desc.width;
    out_frame->height = prime_desc.height;
    out_frame->drm_format = (prime_desc.num_layers > 0 && prime_desc.layers[0].drm_format != 0)
                                ? prime_desc.layers[0].drm_format
                                : DRM_FORMAT_NV12;

    uint32_t plane_idx = 0;
    for (uint32_t l = 0; l < prime_desc.num_layers && plane_idx < 4; ++l) {
        for (uint32_t p = 0; p < prime_desc.layers[l].num_planes && plane_idx < 4; ++p) {
            uint32_t obj_idx = prime_desc.layers[l].object_index[p];
            if (obj_idx < prime_desc.num_objects) {
                out_frame->fds[plane_idx] = prime_desc.objects[obj_idx].fd;
                out_frame->modifiers[plane_idx] = prime_desc.objects[obj_idx].drm_format_modifier;
                out_frame->offsets[plane_idx] = prime_desc.layers[l].offset[p];
                out_frame->strides[plane_idx] = prime_desc.layers[l].pitch[p];
                plane_idx++;
            }
        }
    }

    out_frame->num_planes = plane_idx > 0 ? plane_idx : prime_desc.num_objects;
    return true;
}

void vaapi_prime_frame_close(vaapi_prime_frame_t *frame) {
    if (!frame) return;

    for (uint32_t i = 0; i < frame->num_planes; ++i) {
        int fd = frame->fds[i];
        if (fd >= 0) {
            close(fd);
            for (uint32_t j = i + 1; j < frame->num_planes; ++j) {
                if (frame->fds[j] == fd) {
                    frame->fds[j] = -1;
                }
            }
            frame->fds[i] = -1;
        }
    }
}
