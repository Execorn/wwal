// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/log.h"
#include "waywal/vaapi_dec.h"
#include "waywal/video_decoder.h"

#include "vaapi_internal.h"
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

static void evict_cold_driver_pages(void)
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

static bool profile_supported(const VAProfile *profiles, int count, VAProfile target)
{
    for (int i = 0; i < count; ++i) {
        if (profiles[i] == target)
            return true;
    }
    return false;
}

static void vaapi_cleanup_partial(vaapi_decoder_t *dec)
{
    if (!dec)
        return;

    uint32_t pool_size = dec->num_surfaces > 0 ? dec->num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    uint32_t vpp_pool = dec->num_vpp_surfaces > 0 ? dec->num_vpp_surfaces : WAYWAL_VA_VPP_POOL_SIZE;

    if (dec->va_display) {
        if (dec->vpp_context != VA_INVALID_ID) {
            vaDestroyContext(dec->va_display, dec->vpp_context);
            dec->vpp_context = VA_INVALID_ID;
        }
        if (dec->num_vpp_surfaces > 0) {
            vaDestroySurfaces(dec->va_display, dec->vpp_surfaces, vpp_pool);
        }
        if (dec->vpp_config != VA_INVALID_ID) {
            vaDestroyConfig(dec->va_display, dec->vpp_config);
            dec->vpp_config = VA_INVALID_ID;
        }
        dec->has_vpp = false;
        dec->num_vpp_surfaces = 0;

        if (dec->va_context != VA_INVALID_ID) {
            vaDestroyContext(dec->va_display, dec->va_context);
            dec->va_context = VA_INVALID_ID;
        }
        if (dec->num_surfaces > 0) {
            vaDestroySurfaces(dec->va_display, dec->surfaces, pool_size);
        }
        if (dec->va_config != VA_INVALID_ID) {
            vaDestroyConfig(dec->va_display, dec->va_config);
            dec->va_config = VA_INVALID_ID;
        }
        vaTerminate(dec->va_display);
        dec->va_display = NULL;
    }

    for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
        dec->surfaces[i] = VA_INVALID_SURFACE;
        dec->vpp_surfaces[i] = VA_INVALID_SURFACE;
    }
    dec->num_surfaces = 0;
    dec->num_vpp_surfaces = 0;
    dec->has_vpp = false;

    if (dec->owns_drm_fd && dec->drm_fd >= 0) {
        close(dec->drm_fd);
    }
    dec->drm_fd = -1;
    dec->owns_drm_fd = false;
    dec->initialized = false;
}

static bool hevc_vps_indicates_main10(const uint8_t *nal, size_t nal_len)
{
    if (!nal || nal_len < 10)
        return false;
    /* Strip emulation prevention bytes from first 18 bytes past the 2-byte NAL header to read PTL.
     */
    uint8_t rbsp[18];
    size_t rlen = 0;
    for (size_t i = 2; i < nal_len && rlen < sizeof(rbsp); ++i) {
        if (i + 1 < nal_len && nal[i] == 0x00 && nal[i + 1] == 0x00 &&
            (i + 2 < nal_len && nal[i + 2] == 0x03)) {
            rbsp[rlen++] = 0x00;
            if (rlen < sizeof(rbsp))
                rbsp[rlen++] = 0x00;
            i += 2;
        } else {
            rbsp[rlen++] = nal[i];
        }
    }
    /* VPS layout before profile_tier_level: 4 bytes (vps_id..max_sub_layers) + 2 bytes
     * reserved_0xffff. */
    if (rlen < 12)
        return false;
    uint8_t profile_idc = rbsp[6] & 0x1Fu;
    uint32_t compat = ((uint32_t)rbsp[7] << 24) | ((uint32_t)rbsp[8] << 16) |
                      ((uint32_t)rbsp[9] << 8) | (uint32_t)rbsp[10];
    return profile_idc == 2 || (compat & (1U << (31U - 2U))) != 0;
}

static VAProfile select_va_profile(VADisplay dpy, waywal_codec_t codec, const uint8_t *extradata,
                                   size_t extradata_size)
{
    int max_profiles = vaMaxNumProfiles(dpy);
    if (max_profiles <= 0)
        return VAProfileNone;

    VAProfile *profiles = (VAProfile *)calloc((size_t)max_profiles, sizeof(VAProfile));
    if (!profiles)
        return VAProfileNone;

    int num_profiles = 0;
    VAStatus st = vaQueryConfigProfiles(dpy, profiles, &num_profiles);
    if (st != VA_STATUS_SUCCESS) {
        free(profiles);
        return VAProfileNone;
    }

    bool is_10bit_hint = false;
    if (codec == WAYWAL_CODEC_HEVC && extradata && extradata_size > 0) {
        if (extradata_size >= 19 && extradata[0] == 1) {
            uint8_t general_profile_idc = extradata[1] & 0x1Fu;
            uint8_t bit_depth_luma_minus8 = extradata[17] & 0x07u;
            uint8_t bit_depth_chroma_minus8 = extradata[18] & 0x07u;
            if (general_profile_idc == 2 || bit_depth_luma_minus8 >= 2 ||
                bit_depth_chroma_minus8 >= 2) {
                is_10bit_hint = true;
            }
        } else {
            size_t scan_off = 0;
            const uint8_t *nal_ptr = NULL;
            uint32_t nal_len = 0;
            while (
                next_h264_nal(extradata, extradata_size, false, 4, &scan_off, &nal_ptr, &nal_len)) {
                if (nal_len < 2)
                    continue;
                uint8_t ntype = (nal_ptr[0] >> 1) & 0x3Fu;
                if (ntype == 33) {
                    parsed_hevc_sps_t hevc_sps;
                    memset(&hevc_sps, 0, sizeof(hevc_sps));
                    if (parse_hevc_sps(nal_ptr, nal_len, &hevc_sps) &&
                        (hevc_sps.bit_depth_luma_minus8 >= 2 ||
                         hevc_sps.bit_depth_chroma_minus8 >= 2)) {
                        is_10bit_hint = true;
                        break;
                    }
                } else if (ntype == 32 && hevc_vps_indicates_main10(nal_ptr, nal_len)) {
                    is_10bit_hint = true;
                    break;
                }
            }
        }
    }

    VAProfile chosen = VAProfileNone;
    switch (codec) {
    case WAYWAL_CODEC_H264:
        if (profile_supported(profiles, num_profiles, VAProfileH264Main))
            chosen = VAProfileH264Main;
        else if (profile_supported(profiles, num_profiles, VAProfileH264High))
            chosen = VAProfileH264High;
        else if (profile_supported(profiles, num_profiles, VAProfileH264ConstrainedBaseline))
            chosen = VAProfileH264ConstrainedBaseline;
        break;

    case WAYWAL_CODEC_HEVC:
        if (is_10bit_hint && profile_supported(profiles, num_profiles, VAProfileHEVCMain10))
            chosen = VAProfileHEVCMain10;
        else if (profile_supported(profiles, num_profiles, VAProfileHEVCMain))
            chosen = VAProfileHEVCMain;
        else if (profile_supported(profiles, num_profiles, VAProfileHEVCMain10))
            chosen = VAProfileHEVCMain10;
        break;

    case WAYWAL_CODEC_VP9:
        if (profile_supported(profiles, num_profiles, VAProfileVP9Profile0))
            chosen = VAProfileVP9Profile0;
        else if (profile_supported(profiles, num_profiles, VAProfileVP9Profile2))
            chosen = VAProfileVP9Profile2;
        break;

    case WAYWAL_CODEC_AV1:
        if (profile_supported(profiles, num_profiles, VAProfileAV1Profile0))
            chosen = VAProfileAV1Profile0;
        break;

    default:
        break;
    }

    free(profiles);
    return chosen;
}

bool vaapi_decoder_init(vaapi_decoder_t *dec, int drm_render_fd, waywal_codec_t codec,
                        uint32_t width, uint32_t height)
{
    return vaapi_decoder_init_with_params(dec, drm_render_fd, codec, width, height, NULL, 0, false);
}

bool vaapi_decoder_init_with_params(vaapi_decoder_t *dec, int drm_render_fd, waywal_codec_t codec,
                                    uint32_t width, uint32_t height, const uint8_t *extradata,
                                    size_t extradata_size, bool allow_direct_nv12)
{
    if (!dec || width == 0 || height == 0 || width > (uint32_t)INT32_MAX ||
        height > (uint32_t)INT32_MAX)
        return false;
    uint8_t raw_p010 = 0;
    memcpy(&raw_p010, &dec->allow_direct_p010, sizeof(raw_p010));
    bool saved_allow_direct_p010 = allow_direct_nv12 && (raw_p010 == 1);
    memset(dec, 0, sizeof(*dec));
    dec->drm_fd = -1;
    dec->va_config = VA_INVALID_ID;
    dec->va_context = VA_INVALID_ID;
    dec->vpp_config = VA_INVALID_ID;
    dec->vpp_context = VA_INVALID_ID;
    for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
        dec->surfaces[i] = VA_INVALID_SURFACE;
        dec->vpp_surfaces[i] = VA_INVALID_SURFACE;
    }
    dec->allow_direct_p010 = saved_allow_direct_p010;
    dec->width = width;
    dec->height = height;
    dec->fourcc = VA_FOURCC_NV12;
    dec->nal_length_size = 4;
    dec->vp9_subsampling_x = 1;
    dec->vp9_subsampling_y = 1;
    dec->vp9_bit_depth = 8;
    dec->vp9_ref_deltas[0] = 1;
    dec->vp9_ref_deltas[1] = 0;
    dec->vp9_ref_deltas[2] = -1;
    dec->vp9_ref_deltas[3] = -1;
    memset(dec->vp9_mb_segment_tree_probs, 255, sizeof(dec->vp9_mb_segment_tree_probs));
    memset(dec->vp9_segment_pred_probs, 255, sizeof(dec->vp9_segment_pred_probs));
    for (int i = 0; i < 8; ++i) {
        dec->vp9_ref_frames[i] = VA_INVALID_SURFACE;
        dec->av1_ref_frames[i] = VA_INVALID_SURFACE;
    }
    av1_init_default_slot_state(dec);
    memset(&dec->cached_iq, 16, sizeof(dec->cached_iq));
    dec->iq_valid = true;

    const char *candidate_nodes[] = {NULL, "/dev/dri/renderD128", "/dev/dri/renderD129"};
    bool found = false;

    for (int i = 0; i < 3; ++i) {
        int fd = -1;
        bool owns_fd = false;
        if (i == 0) {
            if (drm_render_fd < 0)
                continue;
            fd = fcntl(drm_render_fd, F_DUPFD_CLOEXEC, 0);
            if (fd < 0)
                continue;
            owns_fd = true;
        } else {
            fd = open(candidate_nodes[i], O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            owns_fd = true;
        }

        VADisplay dpy = vaGetDisplayDRM(fd);
        if (!dpy) {
            if (owns_fd)
                close(fd);
            continue;
        }

        int major = 0, minor = 0;
        VAStatus st = vaInitialize(dpy, &major, &minor);
        if (!dpy || st != VA_STATUS_SUCCESS) {
            if (dpy)
                vaTerminate(dpy);
            if (owns_fd)
                close(fd);
            continue;
        }

        VAProfile prof = select_va_profile(dpy, codec, extradata, extradata_size);
        if (prof == VAProfileNone) {
            vaTerminate(dpy);
            if (owns_fd)
                close(fd);
            continue;
        }

        dec->drm_fd = fd;
        dec->owns_drm_fd = owns_fd;
        dec->va_display = dpy;
        dec->va_profile = prof;
        const char *vendor = vaQueryVendorString(dec->va_display);
        dec->is_nvidia_vaapi =
            vendor && (strstr(vendor, "nvidia") != NULL || strstr(vendor, "NVIDIA") != NULL ||
                       strstr(vendor, "NVDEC") != NULL || strstr(vendor, "vdpau") != NULL);
        found = true;
        WAYWAL_LOG_INFO("VA-API v%d.%d initialized on DRM node %s (Profile: %d, Driver: %s)", major,
                        minor, candidate_nodes[i] ? candidate_nodes[i] : "supplied fd",
                        dec->va_profile, vendor ? vendor : "unknown");
        break;
    }

    if (!found) {
        WAYWAL_LOG_ERR("Failed to find working VA-API DRM node for codec %d", codec);
        return false;
    }

    if (extradata && extradata_size > 0) {
        vaapi_decoder_set_extradata(dec, extradata, extradata_size);
    }

    bool is_10bit_stream =
        (dec->va_profile == VAProfileHEVCMain10 || dec->va_profile == VAProfileVP9Profile2 ||
         (dec->va_profile == VAProfileAV1Profile0 && dec->cached_av1_seq.valid &&
          dec->cached_av1_seq.high_bitdepth) ||
         (dec->cached_hevc_sps.valid && (dec->cached_hevc_sps.bit_depth_luma_minus8 == 2 ||
                                         dec->cached_hevc_sps.bit_depth_chroma_minus8 == 2)));
    if (is_10bit_stream && !dec->allow_direct_p010) {
        allow_direct_nv12 = false;
    }

    uint32_t max_refs = 4;
    if (dec->cached_sps.valid) {
        max_refs = dec->cached_sps.num_ref_frames;
        if (max_refs < 1)
            max_refs = 1;
        if (max_refs > 16)
            max_refs = 16;
    } else if (dec->cached_hevc_sps.valid) {
        max_refs = (uint32_t)dec->cached_hevc_sps.sps_max_dec_pic_buffering_minus1 + 1;
        if (max_refs < 4)
            max_refs = 4;
        if (max_refs > 15)
            max_refs = 15;
    } else if (codec == WAYWAL_CODEC_VP9 || codec == WAYWAL_CODEC_AV1) {
        max_refs = 8;
    }

    /* Allocate sufficient surfaces: 16 DPB references + 6 reorder capacity + 4 presentation
     * headroom = 26 */
    uint32_t needed_surfaces = max_refs + 6U + 4U;
    if (needed_surfaces < 26U) {
        needed_surfaces = 26U;
    }
    if (needed_surfaces > WAYWAL_VA_SURFACE_POOL_SIZE) {
        needed_surfaces = WAYWAL_VA_SURFACE_POOL_SIZE;
    }

    bool use_10bit = is_10bit_stream;
    uint32_t rt_format = use_10bit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    dec->fourcc = use_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;

    VAConfigAttrib attrib = {.type = VAConfigAttribRTFormat, .value = rt_format};
    VAStatus st = vaCreateConfig(dec->va_display, dec->va_profile, VAEntrypointVLD, &attrib, 1,
                                 &dec->va_config);
    if (st != VA_STATUS_SUCCESS && use_10bit) {
        rt_format = VA_RT_FORMAT_YUV420;
        dec->fourcc = VA_FOURCC_NV12;
        attrib.value = rt_format;
        st = vaCreateConfig(dec->va_display, dec->va_profile, VAEntrypointVLD, &attrib, 1,
                            &dec->va_config);
    }
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateConfig failed: %s", vaErrorStr(st));
        vaapi_cleanup_partial(dec);
        return false;
    }

    VASurfaceAttrib sattrs[2];
    sattrs[0].type = VASurfaceAttribPixelFormat;
    sattrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattrs[0].value.type = VAGenericValueTypeInteger;
    sattrs[0].value.value.i = (int)dec->fourcc;

    sattrs[1].type = VASurfaceAttribUsageHint;
    sattrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    sattrs[1].value.type = VAGenericValueTypeInteger;
    sattrs[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_DECODER;

    st = vaCreateSurfaces(dec->va_display, rt_format, width, height, dec->surfaces, needed_surfaces,
                          sattrs, 2);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateSurfaces failed for %ux%u pool (%u surfaces): %s", width, height,
                       needed_surfaces, vaErrorStr(st));
        vaapi_cleanup_partial(dec);
        return false;
    }
    dec->num_surfaces = needed_surfaces;

    st = vaCreateContext(dec->va_display, dec->va_config, (int)width, (int)height, VA_PROGRESSIVE,
                         dec->surfaces, (int)dec->num_surfaces, &dec->va_context);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateContext failed: %s", vaErrorStr(st));
        vaapi_cleanup_partial(dec);
        return false;
    }

    dec->vpp_config = VA_INVALID_ID;
    dec->vpp_context = VA_INVALID_ID;
    dec->has_vpp = false;
    dec->num_vpp_surfaces = 0;
    dec->vpp_warmed_up = false;

    if (!allow_direct_nv12) {
        VAConfigAttrib vpp_attrib = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_RGB32};
        VAStatus vpp_st = vaCreateConfig(dec->va_display, VAProfileNone, VAEntrypointVideoProc,
                                         &vpp_attrib, 1, &dec->vpp_config);
        if (vpp_st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("VA-API VPP vaCreateConfig failed: %s", vaErrorStr(vpp_st));
            vaapi_cleanup_partial(dec);
            return false;
        }
        VASurfaceAttrib vpp_sattrs[2];
        vpp_sattrs[0].type = VASurfaceAttribPixelFormat;
        vpp_sattrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        vpp_sattrs[0].value.type = VAGenericValueTypeInteger;
        vpp_sattrs[0].value.value.i = VA_FOURCC_BGRX;
        vpp_sattrs[1].type = VASurfaceAttribUsageHint;
        vpp_sattrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        vpp_sattrs[1].value.type = VAGenericValueTypeInteger;
        vpp_sattrs[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;

        uint32_t vpp_pool = WAYWAL_VA_VPP_POOL_SIZE;
        vpp_st = vaCreateSurfaces(dec->va_display, VA_RT_FORMAT_RGB32, width, height,
                                  dec->vpp_surfaces, vpp_pool, vpp_sattrs, 2);
        if (vpp_st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("VA-API VPP vaCreateSurfaces failed: %s", vaErrorStr(vpp_st));
            vaapi_cleanup_partial(dec);
            return false;
        }
        dec->num_vpp_surfaces = vpp_pool;

        vpp_st = vaCreateContext(dec->va_display, dec->vpp_config, (int)width, (int)height, 0,
                                 dec->vpp_surfaces, (int)vpp_pool, &dec->vpp_context);
        if (vpp_st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("VA-API VPP vaCreateContext failed: %s", vaErrorStr(vpp_st));
            vaapi_cleanup_partial(dec);
            return false;
        }
        dec->has_vpp = true;
        WAYWAL_LOG_INFO("VA-API VPP active (NV12 -> BGRX, %u decode surfaces + %u VPP "
                        "surfaces @ %ux%u)",
                        dec->num_surfaces, dec->num_vpp_surfaces, width, height);
    } else {
        WAYWAL_LOG_INFO("VA-API direct %s DMA-BUF export active (%u surfaces @ %ux%u, max_refs=%u, "
                        "VPP bypassed)",
                        dec->fourcc == VA_FOURCC_P010 ? "P010" : "NV12", dec->num_surfaces, width,
                        height, max_refs);
    }

    /* Evict cold executable text pages of the PTX shader compiler mapped during driver init */
    evict_cold_driver_pages();

    dec->initialized = true;
    dec->current_surface_idx = 0;
    return true;
}

void vaapi_decoder_reset(vaapi_decoder_t *dec)
{
    if (!dec)
        return;
    dec->frame_num = 0;
    dec->prev_poc_msb = 0;
    dec->prev_poc_lsb = 0;
    dec->last_decoded_poc = 0;
    dec->last_decoded_is_idr = false;
    dec->dpb.count = 0;
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
    for (int i = 0; i < 8; ++i) {
        dec->vp9_ref_frames[i] = VA_INVALID_SURFACE;
        dec->av1_ref_frames[i] = VA_INVALID_SURFACE;
    }
    av1_init_default_slot_state(dec);
}

void vaapi_decoder_destroy(vaapi_decoder_t *dec)
{
    if (!dec || !dec->initialized)
        return;

    uint32_t pool_size = dec->num_surfaces > 0 ? dec->num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    uint32_t vpp_pool = dec->num_vpp_surfaces > 0 ? dec->num_vpp_surfaces : WAYWAL_VA_VPP_POOL_SIZE;

    if (dec->va_display) {
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (dec->surfaces[i] != VA_INVALID_SURFACE) {
                vaSyncSurface(dec->va_display, dec->surfaces[i]);
            }
        }
        if (dec->has_vpp) {
            for (uint32_t i = 0; i < vpp_pool; ++i) {
                if (dec->vpp_surfaces[i] != VA_INVALID_SURFACE) {
                    vaSyncSurface(dec->va_display, dec->vpp_surfaces[i]);
                }
            }
        }
        /* nvidia_drv_video.so wakes vaSyncSurface before cuvidUnmapVideoFrame finishes and before
         * its resolveSurfaces worker thread re-locks ctx->mutex and enters pthread_cond_wait;
         * destroyContext signals ctx->cond without locking ctx->mutex. Waiting 15ms guarantees
         * resolveSurfaces is parked in pthread_cond_wait before vaDestroyContext signals it even
         * under heavy concurrent GPU contention. */
        if (dec->is_nvidia_vaapi) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 15000000L};
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
                ;
        }
    }

    vaapi_cleanup_partial(dec);
}

static bool parse_h264_extradata_into(const uint8_t *extradata, size_t size, parsed_sps_t *sps,
                                      parsed_pps_t *pps, bool *out_is_avcc, uint8_t *out_nal_len_sz)
{
    if (!extradata || size < 4 || !sps || !pps || !out_is_avcc || !out_nal_len_sz)
        return false;

    if (extradata[0] == 1) {
        if (size < 7)
            return false;
        *out_is_avcc = true;
        *out_nal_len_sz = (extradata[4] & 0x03) + 1;
        uint32_t num_sps = extradata[5] & 0x1F;
        size_t off = 6;
        for (uint32_t i = 0; i < num_sps; ++i) {
            if (2 > size - off)
                return false;
            uint16_t sps_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
            off += 2;
            if ((size_t)sps_len > size - off)
                return false;
            parse_h264_sps(extradata + off, sps_len, sps);
            off += sps_len;
        }
        if (off >= size)
            return false;
        uint8_t num_pps = extradata[off++];
        for (uint32_t i = 0; i < num_pps; ++i) {
            if (2 > size - off)
                return false;
            uint16_t pps_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
            off += 2;
            if ((size_t)pps_len > size - off)
                return false;
            parse_h264_pps(extradata + off, pps_len, sps, pps);
            off += pps_len;
        }
        return sps->valid;
    }

    *out_is_avcc = false;
    *out_nal_len_sz = 4;
    size_t scan_off = 0;
    const uint8_t *nal_ptr = NULL;
    uint32_t nal_len = 0;
    while (next_h264_nal(extradata, size, false, 4, &scan_off, &nal_ptr, &nal_len)) {
        if (nal_len == 0)
            continue;
        uint8_t ntype = nal_ptr[0] & 0x1F;
        if (ntype == 7) {
            parse_h264_sps(nal_ptr, nal_len, sps);
        } else if (ntype == 8) {
            parse_h264_pps(nal_ptr, nal_len, sps, pps);
        }
    }
    return sps->valid;
}

static bool parse_hevc_extradata_into(const uint8_t *extradata, size_t size, parsed_hevc_sps_t *sps,
                                      parsed_hevc_pps_t *pps, bool *out_is_avcc,
                                      uint8_t *out_nal_len_sz, bool *out_vps_10bit)
{
    if (!extradata || size < 4 || !sps || !pps || !out_is_avcc || !out_nal_len_sz)
        return false;
    if (out_vps_10bit)
        *out_vps_10bit = false;

    if (extradata[0] == 1) {
        if (size < 23)
            return false;
        *out_is_avcc = true;
        *out_nal_len_sz = (extradata[21] & 0x03) + 1;
        uint8_t num_arrays = extradata[22];
        size_t off = 23;
        for (uint8_t a = 0; a < num_arrays; ++a) {
            if (3 > size - off)
                return false;
            uint8_t nal_type = extradata[off] & 0x3F;
            uint16_t num_nalus = ((uint16_t)extradata[off + 1] << 8) | extradata[off + 2];
            off += 3;
            for (uint16_t n = 0; n < num_nalus; ++n) {
                if (2 > size - off)
                    return false;
                uint16_t nal_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
                off += 2;
                if ((size_t)nal_len > size - off)
                    return false;
                if (nal_type == 33) {
                    parse_hevc_sps(extradata + off, nal_len, sps);
                } else if (nal_type == 34) {
                    parse_hevc_pps(extradata + off, nal_len, pps);
                } else if (nal_type == 32 && out_vps_10bit &&
                           hevc_vps_indicates_main10(extradata + off, nal_len)) {
                    *out_vps_10bit = true;
                }
                off += nal_len;
            }
        }
        return sps->valid;
    }

    *out_is_avcc = false;
    *out_nal_len_sz = 4;
    size_t scan_off = 0;
    const uint8_t *nal_ptr = NULL;
    uint32_t nal_len = 0;
    while (next_h264_nal(extradata, size, false, 4, &scan_off, &nal_ptr, &nal_len)) {
        if (nal_len >= 2) {
            uint8_t ntype = (nal_ptr[0] >> 1) & 0x3F;
            if (ntype == 33) {
                parse_hevc_sps(nal_ptr, nal_len, sps);
            } else if (ntype == 34) {
                parse_hevc_pps(nal_ptr, nal_len, pps);
            } else if (ntype == 32 && out_vps_10bit &&
                       hevc_vps_indicates_main10(nal_ptr, nal_len)) {
                *out_vps_10bit = true;
            }
        }
    }
    return sps->valid;
}

static void parse_av1_extradata_into(const uint8_t *extradata, size_t size, parsed_av1_seq_t *seq)
{
    if (!extradata || size == 0 || !seq)
        return;
    size_t off = (size >= 4 && (extradata[0] & 0x80) != 0) ? 4 : 0;
    while (off < size) {
        uint8_t obu_hdr = extradata[off++];
        uint32_t obu_type = (obu_hdr >> 3) & 0x0F;
        bool has_ext = (obu_hdr & 0x04) != 0;
        bool has_size = (obu_hdr & 0x02) != 0;
        if (has_ext) {
            if (off >= size)
                break;
            off++;
        }
        size_t obu_sz = size - off;
        if (has_size) {
            size_t leb_bytes = 0;
            uint64_t parsed_sz = av1_read_leb128(extradata + off, size - off, &leb_bytes);
            if (leb_bytes == 0)
                break;
            off += leb_bytes;
            if (parsed_sz > size - off)
                break;
            obu_sz = (size_t)parsed_sz;
        }
        if (obu_type == 1) {
            parse_av1_sequence_header(extradata + off, obu_sz, seq);
        }
        off += obu_sz;
    }
}

bool vaapi_decoder_probe_extradata(waywal_codec_t codec, VAProfile profile,
                                   const uint8_t *extradata, size_t size, bool *out_is_10bit,
                                   uint32_t *out_max_refs)
{
    bool is_10bit = (profile == VAProfileHEVCMain10 || profile == VAProfileVP9Profile2);
    uint32_t max_refs = (codec == WAYWAL_CODEC_VP9 || codec == WAYWAL_CODEC_AV1) ? 8U : 4U;

    if (extradata && size >= 4) {
        if (codec == WAYWAL_CODEC_H264) {
            parsed_sps_t sps;
            parsed_pps_t pps;
            bool is_avcc = false;
            uint8_t nls = 4;
            memset(&sps, 0, sizeof(sps));
            memset(&pps, 0, sizeof(pps));
            if (parse_h264_extradata_into(extradata, size, &sps, &pps, &is_avcc, &nls) &&
                sps.num_ref_frames > 0) {
                max_refs = sps.num_ref_frames;
                if (max_refs > 16U)
                    max_refs = 16U;
            }
        } else if (codec == WAYWAL_CODEC_HEVC) {
            parsed_hevc_sps_t sps;
            parsed_hevc_pps_t pps;
            bool is_avcc = false;
            uint8_t nls = 4;
            bool vps_10bit = false;
            memset(&sps, 0, sizeof(sps));
            memset(&pps, 0, sizeof(pps));
            if (extradata[0] == 1 && size >= 19) {
                uint8_t general_profile_idc = extradata[1] & 0x1Fu;
                uint8_t bd_l = extradata[17] & 0x07u;
                uint8_t bd_c = extradata[18] & 0x07u;
                if (general_profile_idc == 2 || bd_l >= 2 || bd_c >= 2)
                    is_10bit = true;
            }
            if (parse_hevc_extradata_into(extradata, size, &sps, &pps, &is_avcc, &nls,
                                          &vps_10bit)) {
                max_refs = (uint32_t)sps.sps_max_dec_pic_buffering_minus1 + 1U;
                if (max_refs < 4U)
                    max_refs = 4U;
                if (max_refs > 15U)
                    max_refs = 15U;
                if (sps.bit_depth_luma_minus8 >= 2 || sps.bit_depth_chroma_minus8 >= 2)
                    is_10bit = true;
            }
            if (vps_10bit)
                is_10bit = true;
        } else if (codec == WAYWAL_CODEC_AV1) {
            parsed_av1_seq_t seq;
            memset(&seq, 0, sizeof(seq));
            parse_av1_extradata_into(extradata, size, &seq);
            if (seq.valid && seq.high_bitdepth)
                is_10bit = true;
        }
    }

    if (out_is_10bit)
        *out_is_10bit = is_10bit;
    if (out_max_refs)
        *out_max_refs = max_refs;
    return true;
}

bool vaapi_decoder_set_extradata(vaapi_decoder_t *dec, const uint8_t *extradata, size_t size)
{
    if (!dec || !extradata || size < 4)
        return false;

    if (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
        dec->va_profile == VAProfileH264ConstrainedBaseline) {
        bool ok = parse_h264_extradata_into(extradata, size, &dec->cached_sps, &dec->cached_pps,
                                            &dec->is_avcc, &dec->nal_length_size);
        refresh_cached_h264_iq(dec);
        return ok;
    } else if (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10) {
        return parse_hevc_extradata_into(extradata, size, &dec->cached_hevc_sps,
                                         &dec->cached_hevc_pps, &dec->is_avcc,
                                         &dec->nal_length_size, NULL);
    } else if (dec->va_profile == VAProfileAV1Profile0) {
        parse_av1_extradata_into(extradata, size, &dec->cached_av1_seq);
        return dec->cached_av1_seq.valid;
    }
    return true;
}

vaapi_decode_status_t vaapi_decoder_decode_packet_ex(vaapi_decoder_t *dec,
                                                     const demux_packet_t *pkt,
                                                     VASurfaceID *out_surface)
{
    if (out_surface) {
        *out_surface = VA_INVALID_SURFACE;
    }
    if (!dec || !dec->initialized || !pkt || !out_surface || !pkt->data || pkt->size == 0 ||
        pkt->size > (size_t)UINT32_MAX || pkt->size > (size_t)INT32_MAX) {
        return VAAPI_DECODE_ERROR;
    }

    uint32_t pool_size = dec->num_surfaces > 0 ? dec->num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    bool is_vp9 =
        (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2);
    bool is_av1 = (dec->va_profile == VAProfileAV1Profile0);

    /* Pick the lowest-index free surface not in DPB or held by reorder/compositor
     * so nvidia_drv_video's lazy direct_realiseSurface allocates VRAM only for the
     * active working set rather than touching all pool_size handles. */
    bool surface_found = false;
    for (uint32_t idx = 0; idx < pool_size; ++idx) {
        VASurfaceID s = dec->surfaces[idx];
        bool in_dpb = false;
        if (!is_vp9 && !is_av1) {
            for (size_t d = 0; d < dec->dpb.count; ++d) {
                if (dec->dpb.entries[d].surface == s) {
                    in_dpb = true;
                    break;
                }
            }
        } else {
            for (int r = 0; r < 8; ++r) {
                if ((is_vp9 && dec->vp9_ref_frames[r] == s) ||
                    (is_av1 && dec->av1_ref_frames[r] == s)) {
                    in_dpb = true;
                    break;
                }
            }
        }
        bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[idx];
        if (!in_dpb && !compositor_busy) {
            dec->current_surface_idx = idx;
            surface_found = true;
            break;
        }
    }

    if (!surface_found) {
        /* Prefer any non-compositor-busy slot and invalidate/evict stale DPB reference */
        for (uint32_t i = 0; i < pool_size; ++i) {
            uint32_t cand = (dec->current_surface_idx + 1 + i) % pool_size;
            bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[cand];
            if (!compositor_busy) {
                VASurfaceID s = dec->surfaces[cand];
                if (!is_vp9 && !is_av1) {
                    for (size_t d = 0; d < dec->dpb.count; ++d) {
                        if (dec->dpb.entries[d].surface == s) {
                            if (d + 1 < dec->dpb.count) {
                                memmove(&dec->dpb.entries[d], &dec->dpb.entries[d + 1],
                                        (dec->dpb.count - d - 1) * sizeof(h264_dpb_entry_t));
                            }
                            dec->dpb.count--;
                            break;
                        }
                    }
                } else if (is_vp9) {
                    for (int r = 0; r < 8; ++r) {
                        if (dec->vp9_ref_frames[r] == s) {
                            dec->vp9_ref_frames[r] = VA_INVALID_SURFACE;
                        }
                    }
                } else if (is_av1) {
                    for (int r = 0; r < 8; ++r) {
                        if (dec->av1_ref_frames[r] == s) {
                            dec->av1_ref_frames[r] = VA_INVALID_SURFACE;
                        }
                    }
                }
                dec->current_surface_idx = cand;
                surface_found = true;
                break;
            }
        }
    }

    if (!surface_found) {
        WAYWAL_LOG_WARN("All %u VA surfaces are busy (presentation), failing frame decode cleanly",
                        pool_size);
        return VAAPI_DECODE_BUSY;
    }

    VASurfaceID target = dec->surfaces[dec->current_surface_idx];
    bool picture_begun = false;
    bool decoded_hidden_frame = false;

    VAStatus st = VA_STATUS_SUCCESS;

    int32_t curr_frame_num = (int32_t)dec->frame_num;
    int32_t curr_poc = (int32_t)(dec->frame_num * 2);
    bool is_ref_pic = true;
    bool is_idr_pic = pkt->is_keyframe;
    slice_header_meta_t sh_meta;
    memset(&sh_meta, 0, sizeof(sh_meta));

    /* Codec specific parameter buffers */
    if (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
        dec->va_profile == VAProfileH264ConstrainedBaseline) {

        /* Inherit cached SPS/PPS from extradata or prior packets */
        parsed_sps_t sps = dec->cached_sps;
        parsed_pps_t pps = dec->cached_pps;
        bool iq_needs_refresh = !dec->iq_valid;

        /* Scan pass: parse SPS, PPS, and first slice header in packet */
        size_t scan_off = 0;
        const uint8_t *nal_ptr = NULL;
        uint32_t nal_len = 0;
        bool found_slice = false;
        bool saw_control_nal = false;
        bool saw_sps_or_other = false;
        VASliceParameterBufferH264 first_slice_sp;
        memset(&first_slice_sp, 0, sizeof(first_slice_sp));
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            if (nal_len == 0)
                continue;
            uint8_t ntype = nal_ptr[0] & 0x1F;
            if (ntype == 7) {
                saw_sps_or_other = true;
                if (parse_h264_sps(nal_ptr, nal_len, &sps)) {
                    dec->cached_sps = sps;
                    iq_needs_refresh = true;
                }
            } else if (ntype == 8) {
                saw_sps_or_other = true;
                if (parse_h264_pps(nal_ptr, nal_len, &sps, &pps)) {
                    dec->cached_pps = pps;
                    iq_needs_refresh = true;
                }
            } else if (ntype == 6 || ntype == 9 || ntype == 10 || ntype == 11 || ntype == 12) {
                saw_control_nal = true;
            } else if (ntype >= 1 && ntype <= 5 && !found_slice) {
                found_slice = true;
                is_idr_pic = (ntype == 5);
                is_ref_pic = ((nal_ptr[0] >> 5) & 0x03) != 0;

                parse_h264_slice_header(nal_ptr, nal_len, &sps, &pps, &first_slice_sp, &sh_meta);
                curr_frame_num = (int32_t)sh_meta.frame_num;

                if (is_idr_pic) {
                    dec->prev_poc_msb = 0;
                    dec->prev_poc_lsb = 0;
                    curr_poc = 0;
                    dec->dpb.count = 0;
                } else if (sps.valid && sps.pic_order_cnt_type == 0) {
                    uint32_t poc_lsb_m4 = sps.log2_max_pic_order_cnt_lsb_minus4 <= 12
                                              ? sps.log2_max_pic_order_cnt_lsb_minus4
                                              : 12;
                    int32_t max_poc_lsb = (int32_t)(1U << (poc_lsb_m4 + 4U));
                    int32_t poc_msb;
                    if ((sh_meta.poc_lsb < dec->prev_poc_lsb) &&
                        ((dec->prev_poc_lsb - sh_meta.poc_lsb) >= (max_poc_lsb / 2))) {
                        poc_msb = dec->prev_poc_msb + max_poc_lsb;
                    } else if ((sh_meta.poc_lsb > dec->prev_poc_lsb) &&
                               ((sh_meta.poc_lsb - dec->prev_poc_lsb) > (max_poc_lsb / 2))) {
                        poc_msb = dec->prev_poc_msb - max_poc_lsb;
                    } else {
                        poc_msb = dec->prev_poc_msb;
                    }
                    curr_poc = poc_msb + sh_meta.poc_lsb;
                    if (is_ref_pic) {
                        dec->prev_poc_msb = poc_msb;
                        dec->prev_poc_lsb = sh_meta.poc_lsb;
                    }
                } else {
                    curr_poc = curr_frame_num * 2;
                }
            }
        }

        if (iq_needs_refresh) {
            refresh_cached_h264_iq(dec);
        }

        VAPictureParameterBufferH264 pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.CurrPic.picture_id = target;
        pic_param.CurrPic.frame_idx = (uint16_t)curr_frame_num;
        pic_param.CurrPic.flags = is_ref_pic ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
        pic_param.CurrPic.TopFieldOrderCnt = curr_poc;
        pic_param.CurrPic.BottomFieldOrderCnt = curr_poc;
        pic_param.picture_width_in_mbs_minus1 = sps.valid ? (uint16_t)sps.pic_width_in_mbs_minus1
                                                          : (uint16_t)((dec->width + 15) / 16 - 1);
        pic_param.picture_height_in_mbs_minus1 = sps.valid
                                                     ? (uint16_t)sps.pic_height_in_map_units_minus1
                                                     : (uint16_t)((dec->height + 15) / 16 - 1);
        pic_param.bit_depth_luma_minus8 = sps.valid ? sps.bit_depth_luma_minus8 : 0;
        pic_param.bit_depth_chroma_minus8 = sps.valid ? sps.bit_depth_chroma_minus8 : 0;
        pic_param.num_ref_frames = sps.valid ? (sps.num_ref_frames ? sps.num_ref_frames : 1) : 1;
        pic_param.seq_fields.bits.chroma_format_idc = sps.valid ? sps.chroma_format_idc : 1;
        pic_param.seq_fields.bits.residual_colour_transform_flag =
            sps.valid ? (sps.residual_colour_transform_flag ? 1 : 0) : 0;
        pic_param.seq_fields.bits.gaps_in_frame_num_value_allowed_flag =
            sps.valid ? (sps.gaps_in_frame_num_value_allowed_flag ? 1 : 0) : 0;
        pic_param.seq_fields.bits.frame_mbs_only_flag = sps.valid ? sps.frame_mbs_only_flag : 1;
        pic_param.seq_fields.bits.mb_adaptive_frame_field_flag =
            sps.valid ? (sps.mb_adaptive_frame_field_flag ? 1 : 0) : 0;
        pic_param.seq_fields.bits.direct_8x8_inference_flag =
            sps.valid ? sps.direct_8x8_inference_flag : 1;
        pic_param.seq_fields.bits.MinLumaBiPredSize8x8 =
            sps.valid ? (sps.level_idc >= 31 ? 1 : 0) : 1;
        pic_param.seq_fields.bits.log2_max_frame_num_minus4 =
            sps.valid ? sps.log2_max_frame_num_minus4 : 4;
        pic_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
            sps.valid ? sps.log2_max_pic_order_cnt_lsb_minus4 : 4;
        pic_param.seq_fields.bits.pic_order_cnt_type = sps.valid ? sps.pic_order_cnt_type : 0;
        pic_param.seq_fields.bits.delta_pic_order_always_zero_flag =
            sps.valid ? (sps.delta_pic_order_always_zero_flag ? 1 : 0) : 0;
        pic_param.pic_fields.bits.entropy_coding_mode_flag =
            pps.valid ? pps.entropy_coding_mode_flag : 1;
        pic_param.pic_fields.bits.weighted_pred_flag = pps.valid ? pps.weighted_pred_flag : 0;
        pic_param.pic_fields.bits.weighted_bipred_idc = pps.valid ? pps.weighted_bipred_idc : 0;
        pic_param.pic_fields.bits.transform_8x8_mode_flag =
            pps.valid ? pps.transform_8x8_mode_flag : 0;
        pic_param.pic_fields.bits.field_pic_flag = 0;
        pic_param.pic_fields.bits.constrained_intra_pred_flag =
            pps.valid ? (pps.constrained_intra_pred_flag ? 1 : 0) : 0;
        pic_param.pic_fields.bits.pic_order_present_flag =
            pps.valid ? (pps.bottom_field_pic_order_in_frame_present_flag ? 1 : 0) : 0;
        pic_param.pic_fields.bits.deblocking_filter_control_present_flag =
            pps.valid ? pps.deblocking_filter_control_present_flag : 1;
        pic_param.pic_fields.bits.redundant_pic_cnt_present_flag =
            pps.valid ? (pps.redundant_pic_cnt_present_flag ? 1 : 0) : 0;
        pic_param.pic_fields.bits.reference_pic_flag = is_ref_pic ? 1 : 0;
        pic_param.pic_init_qp_minus26 = pps.valid ? pps.pic_init_qp_minus26 : 0;
        pic_param.pic_init_qs_minus26 = pps.valid ? pps.pic_init_qs_minus26 : 0;
        pic_param.chroma_qp_index_offset = pps.valid ? pps.chroma_qp_index_offset : 0;
        pic_param.second_chroma_qp_index_offset = pps.valid ? pps.second_chroma_qp_index_offset : 0;
        pic_param.frame_num = (uint16_t)curr_frame_num;

        for (size_t d = 0; d < dec->dpb.count; ++d) {
            if (dec->dpb.entries[d].surface == target) {
                if (d + 1 < dec->dpb.count) {
                    memmove(&dec->dpb.entries[d], &dec->dpb.entries[d + 1],
                            (dec->dpb.count - d - 1) * sizeof(h264_dpb_entry_t));
                }
                dec->dpb.count--;
                break;
            }
        }

        /* Populate ReferenceFrames from DPB */
        for (int i = 0; i < 16; ++i) {
            if (i < (int)dec->dpb.count) {
                fill_va_pic_h264(&pic_param.ReferenceFrames[i], &dec->dpb.entries[i]);
            } else {
                pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
            }
        }

        if (!found_slice && (pkt->size <= 4 || (saw_control_nal && !saw_sps_or_other))) {
            return VAAPI_DECODE_ERROR;
        }

        st = vaBeginPicture(dec->va_display, dec->va_context, target);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
            return VAAPI_DECODE_ERROR;
        }
        picture_begun = true;

        VABufferID hdr_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
        uint32_t num_hdr_bufs = 0;
        VAStatus st_pic =
            vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                           sizeof(pic_param), 1, &pic_param, &hdr_bufs[0]);
        if (st_pic == VA_STATUS_SUCCESS) {
            num_hdr_bufs++;
        }
        VAStatus st_iq =
            vaCreateBuffer(dec->va_display, dec->va_context, VAIQMatrixBufferType,
                           sizeof(dec->cached_iq), 1, &dec->cached_iq, &hdr_bufs[num_hdr_bufs]);
        if (st_iq == VA_STATUS_SUCCESS) {
            num_hdr_bufs++;
        }

        VAStatus st_hdr = VA_STATUS_ERROR_UNKNOWN;
        if (st_pic == VA_STATUS_SUCCESS && st_iq == VA_STATUS_SUCCESS && num_hdr_bufs == 2) {
            st_hdr = vaRenderPicture(dec->va_display, dec->va_context, hdr_bufs, (int)num_hdr_bufs);
        }
        for (uint32_t b = 0; b < num_hdr_bufs; ++b) {
            vaDestroyBuffer(dec->va_display, hdr_bufs[b]);
        }
        if (st_pic != VA_STATUS_SUCCESS || st_iq != VA_STATUS_SUCCESS ||
            st_hdr != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("H.264 header buffer submit failed: pic=%s iq=%s render=%s",
                           vaErrorStr(st_pic), vaErrorStr(st_iq), vaErrorStr(st_hdr));
            vaEndPicture(dec->va_display, dec->va_context);
            return VAAPI_DECODE_ERROR;
        }

        /* Iterate and render all slice NAL units */
        scan_off = 0;
        uint32_t slice_count = 0;
        uint32_t rendered_slices = 0;
        uint32_t fn_m4 =
            sps.valid ? (sps.log2_max_frame_num_minus4 <= 12 ? sps.log2_max_frame_num_minus4 : 12)
                      : 4;
        int32_t max_fn = (int32_t)(1U << (fn_m4 + 4U));
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            if (nal_len == 0)
                continue;
            uint8_t ntype = nal_ptr[0] & 0x1F;
            if (ntype >= 1 && ntype <= 5) {
                VASliceParameterBufferH264 slice_param;
                slice_header_meta_t cur_sh_meta;
                if (slice_count == 0 && found_slice) {
                    slice_param = first_slice_sp;
                    cur_sh_meta = sh_meta;
                } else {
                    memset(&slice_param, 0, sizeof(slice_param));
                    memset(&cur_sh_meta, 0, sizeof(cur_sh_meta));
                    parse_h264_slice_header(nal_ptr, nal_len, &sps, &pps, &slice_param,
                                            &cur_sh_meta);
                }
                slice_param.slice_data_size = nal_len;
                slice_param.slice_data_offset = 0;
                slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

                build_h264_ref_lists(&dec->dpb, slice_param.slice_type, curr_frame_num, max_fn,
                                     curr_poc, &slice_param);
                apply_h264_ref_pic_list_mod(&dec->dpb, curr_frame_num, max_fn, &cur_sh_meta,
                                            &slice_param);

                VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
                VAStatus s1 =
                    vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                                   sizeof(slice_param), 1, &slice_param, &slice_bufs[0]);
                VAStatus s2 =
                    vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType, nal_len,
                                   1, (void *)nal_ptr, &slice_bufs[1]);
                VAStatus sr = VA_STATUS_ERROR_UNKNOWN;
                if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                    sr = vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
                }
                if (slice_bufs[0] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[0]);
                if (slice_bufs[1] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[1]);
                if (s1 != VA_STATUS_SUCCESS || s2 != VA_STATUS_SUCCESS || sr != VA_STATUS_SUCCESS) {
                    WAYWAL_LOG_ERR("H.264 slice submit failed: p=%s d=%s render=%s", vaErrorStr(s1),
                                   vaErrorStr(s2), vaErrorStr(sr));
                    vaEndPicture(dec->va_display, dec->va_context);
                    return VAAPI_DECODE_ERROR;
                }
                rendered_slices++;
                slice_count++;
            }
        }

        /* Fallback only for unframed raw packet (never empty/control-only) */
        if (slice_count == 0 && pkt->size > 4 && (pkt->size - 4) <= (size_t)UINT32_MAX) {
            VASliceParameterBufferH264 slice_param;
            memset(&slice_param, 0, sizeof(slice_param));
            slice_param.slice_data_size = (uint32_t)(pkt->size - 4);
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice_param.slice_type = pkt->is_keyframe ? 2 : 0;
            build_h264_ref_lists(&dec->dpb, slice_param.slice_type, curr_frame_num, max_fn,
                                 curr_poc, &slice_param);
            VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
            VAStatus s1 =
                vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &slice_bufs[0]);
            VAStatus s2 = vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                                         (uint32_t)(pkt->size - 4), 1, (void *)(pkt->data + 4),
                                         &slice_bufs[1]);
            VAStatus sr = VA_STATUS_ERROR_UNKNOWN;
            if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                sr = vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
            }
            if (slice_bufs[0] != VA_INVALID_ID)
                vaDestroyBuffer(dec->va_display, slice_bufs[0]);
            if (slice_bufs[1] != VA_INVALID_ID)
                vaDestroyBuffer(dec->va_display, slice_bufs[1]);
            if (s1 != VA_STATUS_SUCCESS || s2 != VA_STATUS_SUCCESS || sr != VA_STATUS_SUCCESS) {
                vaEndPicture(dec->va_display, dec->va_context);
                return VAAPI_DECODE_ERROR;
            }
            rendered_slices++;
        }
        if (rendered_slices == 0) {
            vaEndPicture(dec->va_display, dec->va_context);
            return VAAPI_DECODE_ERROR;
        }
    } else if (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10) {
        parsed_hevc_sps_t sps = dec->cached_hevc_sps;
        parsed_hevc_pps_t pps = dec->cached_hevc_pps;
        size_t scan_off = 0;
        const uint8_t *nal_ptr = NULL;
        uint32_t nal_len = 0;
        bool found_slice = false;
        bool is_rap = false;
        bool is_intra = false;
        uint32_t st_rps_bits = 0;
        uint32_t total_hevc_slices = 0;
        VASliceParameterBufferHEVC first_slice_sp;
        memset(&first_slice_sp, 0, sizeof(first_slice_sp));
        hevc_st_rps_t curr_rps;
        memset(&curr_rps, 0, sizeof(curr_rps));

        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            if (nal_len < 2)
                continue;
            uint8_t ntype = (nal_ptr[0] >> 1) & 0x3F;
            if (ntype == 33) {
                if (parse_hevc_sps(nal_ptr, nal_len, &sps)) {
                    dec->cached_hevc_sps = sps;
                }
            } else if (ntype == 34) {
                if (parse_hevc_pps(nal_ptr, nal_len, &pps)) {
                    dec->cached_hevc_pps = pps;
                }
            } else if (ntype <= 9 || (ntype >= 16 && ntype <= 21)) {
                total_hevc_slices++;
                if (!found_slice) {
                    found_slice = true;
                    is_idr_pic = (ntype == 19 || ntype == 20);
                    is_rap = (ntype >= 16 && ntype <= 21);
                    is_ref_pic = !(ntype < 16 && (ntype & 1) == 0);
                    int32_t poc_lsb = 0;
                    parse_hevc_slice_header_ex(nal_ptr, nal_len, &sps, &pps, &first_slice_sp,
                                               &poc_lsb, &st_rps_bits, &curr_rps);
                    is_intra = (first_slice_sp.LongSliceFlags.fields.slice_type == 2);
                    if (is_idr_pic) {
                        dec->prev_poc_msb = 0;
                        dec->prev_poc_lsb = 0;
                        curr_poc = 0;
                        dec->dpb.count = 0;
                    } else {
                        uint32_t poc_lsb_m4 = sps.valid
                                                  ? (sps.log2_max_pic_order_cnt_lsb_minus4 <= 12
                                                         ? sps.log2_max_pic_order_cnt_lsb_minus4
                                                         : 12)
                                                  : 4;
                        int32_t max_poc_lsb = (int32_t)(1U << (poc_lsb_m4 + 4U));
                        int32_t poc_msb;
                        if ((poc_lsb < dec->prev_poc_lsb) &&
                            ((dec->prev_poc_lsb - poc_lsb) >= (max_poc_lsb / 2))) {
                            poc_msb = dec->prev_poc_msb + max_poc_lsb;
                        } else if ((poc_lsb > dec->prev_poc_lsb) &&
                                   ((poc_lsb - dec->prev_poc_lsb) > (max_poc_lsb / 2))) {
                            poc_msb = dec->prev_poc_msb - max_poc_lsb;
                        } else {
                            poc_msb = dec->prev_poc_msb;
                        }
                        curr_poc = poc_msb + poc_lsb;
                        if (is_ref_pic) {
                            dec->prev_poc_msb = poc_msb;
                            dec->prev_poc_lsb = poc_lsb;
                        }
                    }
                }
            }
        }

        for (size_t d = 0; d < dec->dpb.count; ++d) {
            if (dec->dpb.entries[d].surface == target) {
                if (d + 1 < dec->dpb.count) {
                    memmove(&dec->dpb.entries[d], &dec->dpb.entries[d + 1],
                            (dec->dpb.count - d - 1) * sizeof(h264_dpb_entry_t));
                }
                dec->dpb.count--;
                break;
            }
        }

        VAPictureParameterBufferHEVC pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.CurrPic.picture_id = target;
        pic_param.CurrPic.pic_order_cnt = curr_poc;
        pic_param.CurrPic.flags = 0;
        pic_param.pic_width_in_luma_samples =
            sps.valid ? sps.pic_width_in_luma_samples : (uint16_t)dec->width;
        pic_param.pic_height_in_luma_samples =
            sps.valid ? sps.pic_height_in_luma_samples : (uint16_t)dec->height;
        uint32_t matched_curr_rps = 0;
        for (int i = 0; i < 15; ++i) {
            if (i < (int)dec->dpb.count && i < 15) {
                pic_param.ReferenceFrames[i].picture_id = dec->dpb.entries[i].surface;
                pic_param.ReferenceFrames[i].pic_order_cnt = dec->dpb.entries[i].pic_order_cnt;
                uint32_t rflags = 0;
                if (!is_intra && !is_idr_pic) {
                    int32_t dpoc = dec->dpb.entries[i].pic_order_cnt - curr_poc;
                    for (uint8_t r = 0; r < curr_rps.num_negative_pics && r < 16; ++r) {
                        if (curr_rps.delta_poc_s0[r] == dpoc) {
                            if (curr_rps.used_by_curr_pic_s0[r]) {
                                rflags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
                                matched_curr_rps++;
                            }
                            break;
                        }
                    }
                    if (rflags == 0) {
                        for (uint8_t r = 0; r < curr_rps.num_positive_pics && r < 16; ++r) {
                            if (curr_rps.delta_poc_s1[r] == dpoc) {
                                if (curr_rps.used_by_curr_pic_s1[r]) {
                                    rflags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
                                    matched_curr_rps++;
                                }
                                break;
                            }
                        }
                    }
                }
                pic_param.ReferenceFrames[i].flags = rflags;
                dec->dpb.entries[i].is_reference = (rflags != 0);
            } else {
                pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
            }
        }
        if (!is_intra && !is_idr_pic && matched_curr_rps == 0 &&
            (curr_rps.num_negative_pics == 0 && curr_rps.num_positive_pics == 0)) {
            for (int i = 0; i < (int)dec->dpb.count && i < 15; ++i) {
                pic_param.ReferenceFrames[i].flags = (dec->dpb.entries[i].pic_order_cnt <= curr_poc)
                                                         ? VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE
                                                         : VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
                dec->dpb.entries[i].is_reference = true;
            }
        }
        pic_param.pic_fields.bits.chroma_format_idc = sps.valid ? sps.chroma_format_idc : 1;
        pic_param.pic_fields.bits.separate_colour_plane_flag =
            sps.valid && sps.separate_colour_plane_flag ? 1 : 0;
        pic_param.pic_fields.bits.pcm_enabled_flag = sps.valid && sps.pcm_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.scaling_list_enabled_flag =
            sps.valid && sps.scaling_list_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.transform_skip_enabled_flag =
            pps.valid && pps.transform_skip_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.amp_enabled_flag = sps.valid && sps.amp_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.strong_intra_smoothing_enabled_flag =
            sps.valid && sps.strong_intra_smoothing_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.sign_data_hiding_enabled_flag =
            pps.valid && pps.sign_data_hiding_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.constrained_intra_pred_flag =
            pps.valid && pps.constrained_intra_pred_flag ? 1 : 0;
        pic_param.pic_fields.bits.cu_qp_delta_enabled_flag =
            pps.valid && pps.cu_qp_delta_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.weighted_pred_flag = pps.valid && pps.weighted_pred_flag ? 1 : 0;
        pic_param.pic_fields.bits.weighted_bipred_flag =
            pps.valid && pps.weighted_bipred_flag ? 1 : 0;
        pic_param.pic_fields.bits.transquant_bypass_enabled_flag =
            pps.valid && pps.transquant_bypass_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.tiles_enabled_flag = pps.valid && pps.tiles_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.entropy_coding_sync_enabled_flag =
            pps.valid && pps.entropy_coding_sync_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag =
            pps.valid && pps.pps_loop_filter_across_slices_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.loop_filter_across_tiles_enabled_flag =
            pps.valid && pps.loop_filter_across_tiles_enabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.pcm_loop_filter_disabled_flag =
            sps.valid && sps.pcm_loop_filter_disabled_flag ? 1 : 0;
        pic_param.pic_fields.bits.NoPicReorderingFlag =
            sps.valid && sps.sps_max_num_reorder_pics == 0 ? 1 : 0;

        pic_param.sps_max_dec_pic_buffering_minus1 =
            sps.valid ? sps.sps_max_dec_pic_buffering_minus1 : 4;
        pic_param.bit_depth_luma_minus8 = sps.valid ? sps.bit_depth_luma_minus8 : 0;
        pic_param.bit_depth_chroma_minus8 = sps.valid ? sps.bit_depth_chroma_minus8 : 0;
        pic_param.pcm_sample_bit_depth_luma_minus1 =
            sps.valid ? sps.pcm_sample_bit_depth_luma_minus1 : 0;
        pic_param.pcm_sample_bit_depth_chroma_minus1 =
            sps.valid ? sps.pcm_sample_bit_depth_chroma_minus1 : 0;
        pic_param.log2_min_luma_coding_block_size_minus3 =
            sps.valid ? sps.log2_min_luma_coding_block_size_minus3 : 0;
        pic_param.log2_diff_max_min_luma_coding_block_size =
            sps.valid ? sps.log2_diff_max_min_luma_coding_block_size : 3;
        pic_param.log2_min_transform_block_size_minus2 =
            sps.valid ? sps.log2_min_transform_block_size_minus2 : 0;
        pic_param.log2_diff_max_min_transform_block_size =
            sps.valid ? sps.log2_diff_max_min_transform_block_size : 3;
        pic_param.log2_min_pcm_luma_coding_block_size_minus3 =
            sps.valid ? sps.log2_min_pcm_luma_coding_block_size_minus3 : 0;
        pic_param.log2_diff_max_min_pcm_luma_coding_block_size =
            sps.valid ? sps.log2_diff_max_min_pcm_luma_coding_block_size : 0;
        pic_param.max_transform_hierarchy_depth_intra =
            sps.valid ? sps.max_transform_hierarchy_depth_intra : 2;
        pic_param.max_transform_hierarchy_depth_inter =
            sps.valid ? sps.max_transform_hierarchy_depth_inter : 2;
        pic_param.init_qp_minus26 = pps.valid ? pps.init_qp_minus26 : 0;
        pic_param.diff_cu_qp_delta_depth = pps.valid ? pps.diff_cu_qp_delta_depth : 0;
        pic_param.pps_cb_qp_offset = pps.valid ? pps.pps_cb_qp_offset : 0;
        pic_param.pps_cr_qp_offset = pps.valid ? pps.pps_cr_qp_offset : 0;
        pic_param.log2_parallel_merge_level_minus2 =
            pps.valid ? pps.log2_parallel_merge_level_minus2 : 0;
        pic_param.num_tile_columns_minus1 = pps.valid ? pps.num_tile_columns_minus1 : 0;
        pic_param.num_tile_rows_minus1 = pps.valid ? pps.num_tile_rows_minus1 : 0;
        if (pps.valid && pps.tiles_enabled_flag && !pps.uniform_spacing_flag) {
            memcpy(pic_param.column_width_minus1, pps.column_width_minus1,
                   sizeof(pic_param.column_width_minus1));
            memcpy(pic_param.row_height_minus1, pps.row_height_minus1,
                   sizeof(pic_param.row_height_minus1));
        }

        pic_param.slice_parsing_fields.bits.lists_modification_present_flag =
            pps.valid && pps.lists_modification_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.long_term_ref_pics_present_flag =
            sps.valid && sps.long_term_ref_pics_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag =
            sps.valid && sps.sps_temporal_mvp_enabled_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.cabac_init_present_flag =
            pps.valid && pps.cabac_init_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.output_flag_present_flag =
            pps.valid && pps.output_flag_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag =
            pps.valid && pps.dependent_slice_segments_enabled_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag =
            pps.valid && pps.pps_slice_chroma_qp_offsets_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag =
            sps.valid && sps.sample_adaptive_offset_enabled_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag =
            pps.valid && pps.deblocking_filter_override_enabled_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag =
            pps.valid && pps.pps_disable_deblocking_filter_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.slice_segment_header_extension_present_flag =
            pps.valid && pps.slice_segment_header_extension_present_flag ? 1 : 0;
        pic_param.slice_parsing_fields.bits.RapPicFlag = is_rap ? 1 : 0;
        pic_param.slice_parsing_fields.bits.IdrPicFlag = is_idr_pic ? 1 : 0;
        pic_param.slice_parsing_fields.bits.IntraPicFlag = is_intra ? 1 : 0;

        pic_param.log2_max_pic_order_cnt_lsb_minus4 =
            sps.valid ? sps.log2_max_pic_order_cnt_lsb_minus4 : 4;
        pic_param.num_short_term_ref_pic_sets = sps.valid ? sps.num_short_term_ref_pic_sets : 0;
        pic_param.num_long_term_ref_pic_sps = sps.valid ? sps.num_long_term_ref_pics_sps : 0;
        pic_param.num_ref_idx_l0_default_active_minus1 =
            pps.valid ? pps.num_ref_idx_l0_default_active_minus1 : 0;
        pic_param.num_ref_idx_l1_default_active_minus1 =
            pps.valid ? pps.num_ref_idx_l1_default_active_minus1 : 0;
        pic_param.pps_beta_offset_div2 = pps.valid ? pps.pps_beta_offset_div2 : 0;
        pic_param.pps_tc_offset_div2 = pps.valid ? pps.pps_tc_offset_div2 : 0;
        pic_param.num_extra_slice_header_bits = pps.valid ? pps.num_extra_slice_header_bits : 0;
        pic_param.st_rps_bits = st_rps_bits;

        if (total_hevc_slices == 0) {
            return VAAPI_DECODE_ERROR;
        }

        st = vaBeginPicture(dec->va_display, dec->va_context, target);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("vaBeginPicture (HEVC) failed: %s", vaErrorStr(st));
            return VAAPI_DECODE_ERROR;
        }
        picture_begun = true;

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        VAStatus st_render = VA_STATUS_ERROR_UNKNOWN;
        if (st == VA_STATUS_SUCCESS) {
            st_render = vaRenderPicture(dec->va_display, dec->va_context, &pic_buf, 1);
            vaDestroyBuffer(dec->va_display, pic_buf);
        }
        if (st != VA_STATUS_SUCCESS || st_render != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("HEVC picture param submit failed: create=%s render=%s", vaErrorStr(st),
                           vaErrorStr(st_render));
            vaEndPicture(dec->va_display, dec->va_context);
            return VAAPI_DECODE_ERROR;
        }

        scan_off = 0;
        uint32_t slice_count = 0;
        uint32_t rendered_slices = 0;
        VASliceParameterBufferHEVC last_indep_sp = first_slice_sp;
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            if (nal_len < 2)
                continue;
            uint8_t ntype = (nal_ptr[0] >> 1) & 0x3F;
            if (ntype <= 9 || (ntype >= 16 && ntype <= 21)) {
                VASliceParameterBufferHEVC slice_param = last_indep_sp;
                if (slice_count == 0 && found_slice) {
                    slice_param = first_slice_sp;
                } else {
                    parse_hevc_slice_header_ex(nal_ptr, nal_len, &sps, &pps, &slice_param, NULL,
                                               NULL, NULL);
                    if (!slice_param.LongSliceFlags.fields.dependent_slice_segment_flag) {
                        last_indep_sp = slice_param;
                    }
                }
                slice_param.slice_data_size = nal_len;
                slice_param.slice_data_offset = 0;
                slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
                slice_param.LongSliceFlags.fields.LastSliceOfPic =
                    (slice_count + 1 == total_hevc_slices) ? 1 : 0;
                build_hevc_ref_lists(&dec->dpb,
                                     (uint8_t)slice_param.LongSliceFlags.fields.slice_type,
                                     curr_poc, &slice_param);

                VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
                VAStatus s1 =
                    vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                                   sizeof(slice_param), 1, &slice_param, &slice_bufs[0]);
                VAStatus s2 =
                    vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType, nal_len,
                                   1, (void *)nal_ptr, &slice_bufs[1]);
                VAStatus sr = VA_STATUS_ERROR_UNKNOWN;
                if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                    sr = vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
                }
                if (slice_bufs[0] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[0]);
                if (slice_bufs[1] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[1]);
                if (s1 != VA_STATUS_SUCCESS || s2 != VA_STATUS_SUCCESS || sr != VA_STATUS_SUCCESS) {
                    WAYWAL_LOG_ERR("HEVC slice submit failed: p=%s d=%s render=%s", vaErrorStr(s1),
                                   vaErrorStr(s2), vaErrorStr(sr));
                    vaEndPicture(dec->va_display, dec->va_context);
                    return VAAPI_DECODE_ERROR;
                }
                rendered_slices++;
                slice_count++;
            }
        }
        if (rendered_slices == 0) {
            vaEndPicture(dec->va_display, dec->va_context);
            return VAAPI_DECODE_ERROR;
        }
        for (size_t d = 0; d < dec->dpb.count; ++d) {
            dec->dpb.entries[d].is_reference = true;
        }
    } else if (is_vp9) {
        const uint8_t *sub_ptrs[8];
        size_t sub_sizes[8];
        uint32_t num_subframes = vp9_split_superframe(pkt->data, pkt->size, sub_ptrs, sub_sizes);
        if (num_subframes == 0) {
            sub_ptrs[0] = pkt->data;
            sub_sizes[0] = pkt->size;
            num_subframes = 1;
        }

        is_idr_pic = false;
        VASurfaceID display_target = VA_INVALID_SURFACE;
        bool decoded_any_vp9 = false;
        for (uint32_t sf = 0; sf < num_subframes; ++sf) {
            if (!sub_ptrs[sf] || sub_sizes[sf] == 0 || sub_sizes[sf] > (size_t)UINT32_MAX) {
                return VAAPI_DECODE_ERROR;
            }
            VADecPictureParameterBufferVP9 pic_param;
            VASliceParameterBufferVP9 slice_param;
            memset(&pic_param, 0, sizeof(pic_param));
            memset(&slice_param, 0, sizeof(slice_param));
            slice_param.slice_data_size = (uint32_t)sub_sizes[sf];
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

            uint8_t refresh_flags = pkt->is_keyframe ? 0xFF : 0x01;
            bool sub_is_idr = false;
            bool show_existing = false;
            uint8_t show_existing_idx = 0;
            if (!parse_vp9_uncompressed_header(dec, sub_ptrs[sf], sub_sizes[sf], &pic_param,
                                               &slice_param, &refresh_flags, &sub_is_idr,
                                               &show_existing, &show_existing_idx)) {
                pic_param.frame_width = (uint16_t)dec->width;
                pic_param.frame_height = (uint16_t)dec->height;
                pic_param.pic_fields.bits.subsampling_x = 1;
                pic_param.pic_fields.bits.subsampling_y = 1;
                pic_param.pic_fields.bits.frame_type = pkt->is_keyframe ? 0 : 1;
                pic_param.pic_fields.bits.show_frame = 1;
                for (int i = 0; i < 8; ++i) {
                    pic_param.reference_frames[i] = dec->vp9_ref_frames[i];
                }
                sub_is_idr = pkt->is_keyframe;
            }

            if (show_existing) {
                VASurfaceID ref_surf = dec->vp9_ref_frames[show_existing_idx & 7];
                if (ref_surf != VA_INVALID_SURFACE) {
                    target = ref_surf;
                    display_target = ref_surf;
                    decoded_any_vp9 = true;
                    continue;
                }
                WAYWAL_LOG_WARN("VP9 show_existing references unpopulated slot %u",
                                show_existing_idx);
                return VAAPI_DECODE_ERROR;
            }

            bool vp9_surf_found = false;
            for (uint32_t idx = 0; idx < pool_size; ++idx) {
                VASurfaceID s = dec->surfaces[idx];
                bool in_dpb = false;
                for (int r = 0; r < 8; ++r) {
                    if (dec->vp9_ref_frames[r] == s) {
                        in_dpb = true;
                        break;
                    }
                }
                bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[idx];
                if (!in_dpb && !compositor_busy) {
                    dec->current_surface_idx = idx;
                    target = s;
                    vp9_surf_found = true;
                    break;
                }
            }
            if (!vp9_surf_found) {
                for (uint32_t i = 0; i < pool_size; ++i) {
                    uint32_t cand = (dec->current_surface_idx + 1 + i) % pool_size;
                    bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[cand];
                    if (!compositor_busy) {
                        dec->current_surface_idx = cand;
                        target = dec->surfaces[cand];
                        for (int r = 0; r < 8; ++r) {
                            if (dec->vp9_ref_frames[r] == target) {
                                dec->vp9_ref_frames[r] = VA_INVALID_SURFACE;
                            }
                        }
                        vp9_surf_found = true;
                        break;
                    }
                }
            }
            if (!vp9_surf_found) {
                WAYWAL_LOG_WARN(
                    "All %u VA surfaces are busy (presentation), failing VP9 decode cleanly",
                    pool_size);
                return VAAPI_DECODE_BUSY;
            }

            if (sub_is_idr) {
                is_idr_pic = true;
            }
            if (pic_param.pic_fields.bits.show_frame != 0) {
                display_target = target;
            }

            st = vaBeginPicture(dec->va_display, dec->va_context, target);
            if (st != VA_STATUS_SUCCESS) {
                WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
                return VAAPI_DECODE_ERROR;
            }

            VABufferID sf_bufs[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
            uint32_t sf_num_bufs = 0;
            VAStatus s1 =
                vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                               sizeof(pic_param), 1, &pic_param, &sf_bufs[0]);
            if (s1 == VA_STATUS_SUCCESS)
                sf_num_bufs++;
            VAStatus s2 =
                vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &sf_bufs[sf_num_bufs]);
            if (s2 == VA_STATUS_SUCCESS)
                sf_num_bufs++;
            VAStatus s3 = vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                                         (uint32_t)sub_sizes[sf], 1, (void *)sub_ptrs[sf],
                                         &sf_bufs[sf_num_bufs]);
            if (s3 == VA_STATUS_SUCCESS)
                sf_num_bufs++;

            VAStatus sr = VA_STATUS_ERROR_UNKNOWN;
            if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS && s3 == VA_STATUS_SUCCESS &&
                sf_num_bufs == 3) {
                sr = vaRenderPicture(dec->va_display, dec->va_context, sf_bufs, (int)sf_num_bufs);
            }
            for (uint32_t b = 0; b < sf_num_bufs; ++b) {
                vaDestroyBuffer(dec->va_display, sf_bufs[b]);
            }
            VAStatus end_st = vaEndPicture(dec->va_display, dec->va_context);
            if (s1 != VA_STATUS_SUCCESS || s2 != VA_STATUS_SUCCESS || s3 != VA_STATUS_SUCCESS ||
                sr != VA_STATUS_SUCCESS || end_st != VA_STATUS_SUCCESS) {
                WAYWAL_LOG_ERR("VP9 subframe %u failed: b=%s/%s/%s r=%s end=%s", sf, vaErrorStr(s1),
                               vaErrorStr(s2), vaErrorStr(s3), vaErrorStr(sr), vaErrorStr(end_st));
                return VAAPI_DECODE_ERROR;
            }
            decoded_any_vp9 = true;
            for (int i = 0; i < 8; ++i) {
                if (refresh_flags & (1U << i)) {
                    dec->vp9_ref_frames[i] = target;
                }
            }
        }

        target = display_target;
        if (decoded_any_vp9 && target == VA_INVALID_SURFACE) {
            decoded_hidden_frame = true;
        }
        if (is_idr_pic) {
            dec->frame_num = 0;
            curr_frame_num = 0;
            curr_poc = 0;
        }
    } else if (is_av1) {
        is_idr_pic = false;
        VADecPictureParameterBufferAV1 pic_param;
        VASliceParameterBufferAV1 av1_slices[64];
        uint32_t av1_slice_count = 0;
        uint8_t tile_size_bytes_minus_1 = 0;
        bool av1_show_existing = false;
        uint8_t av1_show_existing_idx = 0;
        bool av1_frame_active = false;
        bool submitted_any_av1 = false;
        VASurfaceID display_target = VA_INVALID_SURFACE;
        memset(&pic_param, 0, sizeof(pic_param));
        memset(av1_slices, 0, sizeof(av1_slices));
        pic_param.current_frame = target;
        pic_param.current_display_picture = target;
        pic_param.frame_width_minus1 = (uint16_t)(dec->width - 1);
        pic_param.frame_height_minus1 = (uint16_t)(dec->height - 1);
        pic_param.pic_info_fields.bits.show_frame = 1;
        for (int i = 0; i < 8; ++i) {
            pic_param.ref_frame_map[i] = dec->av1_ref_frames[i];
        }

        uint8_t refresh_flags = pkt->is_keyframe ? 0xFF : 0x01;
        size_t off = 0;
        while (pkt->data && off < pkt->size) {
            uint8_t obu_hdr = pkt->data[off++];
            uint32_t obu_type = (obu_hdr >> 3) & 0x0F;
            bool has_ext = (obu_hdr & 0x04) != 0;
            bool has_size = (obu_hdr & 0x02) != 0;
            if (has_ext) {
                if (off >= pkt->size)
                    break;
                off++;
            }
            size_t obu_sz = pkt->size - off;
            if (has_size) {
                size_t leb_bytes = 0;
                uint64_t parsed_sz = av1_read_leb128(pkt->data + off, pkt->size - off, &leb_bytes);
                if (leb_bytes == 0)
                    break;
                off += leb_bytes;
                if (parsed_sz > pkt->size - off)
                    break;
                obu_sz = (size_t)parsed_sz;
            }
            if (obu_type == 1) {
                parse_av1_sequence_header(pkt->data + off, obu_sz, &dec->cached_av1_seq);
            } else if (obu_type == 3 || obu_type == 6) {
                if (av1_frame_active) {
                    if (!submit_av1_frame_to_va(
                            dec, pool_size, &pic_param, av1_slices, av1_slice_count, pkt,
                            refresh_flags, av1_show_existing, av1_show_existing_idx, &target)) {
                        return VAAPI_DECODE_ERROR;
                    }
                    if (av1_show_existing || pic_param.pic_info_fields.bits.show_frame != 0) {
                        display_target = target;
                    }
                    submitted_any_av1 = true;
                    av1_frame_active = false;
                    av1_slice_count = 0;
                }
                memset(&pic_param, 0, sizeof(pic_param));
                memset(av1_slices, 0, sizeof(av1_slices));
                pic_param.current_frame = target;
                pic_param.current_display_picture = target;
                pic_param.frame_width_minus1 = (uint16_t)(dec->width - 1);
                pic_param.frame_height_minus1 = (uint16_t)(dec->height - 1);
                for (int i = 0; i < 8; ++i) {
                    pic_param.ref_frame_map[i] = dec->av1_ref_frames[i];
                }
                bool frame_is_idr = false;
                parse_av1_frame_header(dec, pkt->data + off, obu_sz, obu_type, (uint32_t)off,
                                       &pic_param, av1_slices, 64, &av1_slice_count,
                                       &tile_size_bytes_minus_1, &refresh_flags, &frame_is_idr,
                                       &av1_show_existing, &av1_show_existing_idx);
                if (frame_is_idr) {
                    is_idr_pic = true;
                }
                if (obu_type == 6 || av1_show_existing) {
                    if (!submit_av1_frame_to_va(
                            dec, pool_size, &pic_param, av1_slices, av1_slice_count, pkt,
                            refresh_flags, av1_show_existing, av1_show_existing_idx, &target)) {
                        return VAAPI_DECODE_ERROR;
                    }
                    if (av1_show_existing || pic_param.pic_info_fields.bits.show_frame != 0) {
                        display_target = target;
                    }
                    submitted_any_av1 = true;
                    av1_frame_active = false;
                    av1_slice_count = 0;
                } else {
                    av1_frame_active = true;
                }
            } else if (obu_type == 4 && obu_sz > 0 && av1_slice_count < 64) {
                av1_slice_count += parse_av1_tile_group_obu(
                    pkt->data + off, obu_sz, 0, (uint32_t)off, &pic_param, tile_size_bytes_minus_1,
                    av1_slices + av1_slice_count, 64 - av1_slice_count);
            }
            off += obu_sz;
        }

        if (av1_frame_active) {
            if (!submit_av1_frame_to_va(dec, pool_size, &pic_param, av1_slices, av1_slice_count,
                                        pkt, refresh_flags, av1_show_existing,
                                        av1_show_existing_idx, &target)) {
                return VAAPI_DECODE_ERROR;
            }
            if (av1_show_existing || pic_param.pic_info_fields.bits.show_frame != 0) {
                display_target = target;
            }
            submitted_any_av1 = true;
        } else if (!submitted_any_av1) {
            return VAAPI_DECODE_ERROR;
        }

        target = display_target;
        if (submitted_any_av1 && target == VA_INVALID_SURFACE) {
            decoded_hidden_frame = true;
        }
        if (is_idr_pic) {
            dec->frame_num = 0;
            curr_frame_num = 0;
            curr_poc = 0;
        }
    }

    if (picture_begun) {
        st = vaEndPicture(dec->va_display, dec->va_context);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("vaEndPicture failed: %s", vaErrorStr(st));
            return VAAPI_DECODE_ERROR;
        }
    }

    if (!is_vp9 && !is_av1) {
        /* Update DPB state machine with MMCO and FIFO sliding-window eviction */
        uint32_t max_refs = 8;
        if (dec->cached_sps.valid && dec->cached_sps.num_ref_frames > 0) {
            max_refs = dec->cached_sps.num_ref_frames;
        } else if (dec->cached_hevc_sps.valid) {
            max_refs = (uint32_t)dec->cached_hevc_sps.sps_max_dec_pic_buffering_minus1 + 1;
            if (max_refs > 15)
                max_refs = 15;
        }
        if (max_refs > 16) {
            max_refs = 16;
        }
        uint32_t dpb_cap = dec->num_surfaces > 2 ? (dec->num_surfaces - 2) : 1;
        if (max_refs > dpb_cap) {
            max_refs = dpb_cap;
        }
        if (max_refs < 1) {
            max_refs = 1;
        }

        if (is_idr_pic || sh_meta.mmco_reset) {
            dec->dpb.count = 0;
            if (sh_meta.mmco_reset) {
                curr_frame_num = 0;
                curr_poc = 0;
                dec->prev_poc_msb = 0;
                dec->prev_poc_lsb = 0;
            }
        } else if (sh_meta.num_unmark > 0) {
            for (uint32_t u = 0; u < sh_meta.num_unmark; ++u) {
                int32_t target_pn = sh_meta.unmark_pic_nums[u];
                for (size_t d = 0; d < dec->dpb.count; ++d) {
                    if (!dec->dpb.entries[d].is_long_term &&
                        dec->dpb.entries[d].frame_num == target_pn) {
                        if (d + 1 < dec->dpb.count) {
                            memmove(&dec->dpb.entries[d], &dec->dpb.entries[d + 1],
                                    (dec->dpb.count - d - 1) * sizeof(h264_dpb_entry_t));
                        }
                        dec->dpb.count--;
                        break;
                    }
                }
            }
        }

        if (is_ref_pic) {
            while (dec->dpb.count >= max_refs) {
                memmove(&dec->dpb.entries[0], &dec->dpb.entries[1],
                        (dec->dpb.count - 1) * sizeof(h264_dpb_entry_t));
                dec->dpb.count--;
            }

            dec->dpb.entries[dec->dpb.count++] = (h264_dpb_entry_t){
                .surface = target,
                .frame_num = curr_frame_num,
                .pic_order_cnt = curr_poc,
                .is_reference = true,
                .is_long_term = false,
            };
        }
    }
    dec->last_decoded_poc = curr_poc;
    dec->last_decoded_is_idr = is_idr_pic;
    dec->frame_num++;

    if (target == VA_INVALID_SURFACE) {
        if (decoded_hidden_frame) {
            *out_surface = VA_INVALID_SURFACE;
            return VAAPI_DECODE_HIDDEN_NO_DISPLAY;
        }
        WAYWAL_LOG_WARN("Decoded surface target is invalid");
        return VAAPI_DECODE_ERROR;
    }

    *out_surface = target;
    dec->current_surface_idx = (dec->current_surface_idx + 1) % pool_size;
    return VAAPI_DECODE_OK;
}

bool vaapi_decoder_decode_packet(vaapi_decoder_t *dec, const demux_packet_t *pkt,
                                 VASurfaceID *out_surface)
{
    vaapi_decode_status_t st = vaapi_decoder_decode_packet_ex(dec, pkt, out_surface);
    return (st == VAAPI_DECODE_OK || st == VAAPI_DECODE_HIDDEN_NO_DISPLAY);
}

static void vaapi_get_color_props(const vaapi_decoder_t *dec, int *out_color_space,
                                  bool *out_full_range)
{
    int cs = VAAPI_COLOR_UNKNOWN;
    bool full_range = false;
    if (!dec) {
        if (out_color_space)
            *out_color_space = cs;
        if (out_full_range)
            *out_full_range = full_range;
        return;
    }

    uint8_t mc = 2;
    if (dec->cached_sps.valid) {
        mc = dec->cached_sps.matrix_coefficients;
        full_range = dec->cached_sps.video_full_range_flag;
        if (mc == 1)
            cs = VAAPI_COLOR_BT709;
        else if (mc == 5 || mc == 6)
            cs = VAAPI_COLOR_BT601;
        else if (mc == 9 || mc == 10)
            cs = VAAPI_COLOR_BT2020;
    } else if (dec->cached_hevc_sps.valid) {
        mc = dec->cached_hevc_sps.matrix_coefficients;
        full_range = dec->cached_hevc_sps.video_full_range_flag;
        if (mc == 1)
            cs = VAAPI_COLOR_BT709;
        else if (mc == 5 || mc == 6)
            cs = VAAPI_COLOR_BT601;
        else if (mc == 9 || mc == 10)
            cs = VAAPI_COLOR_BT2020;
    } else if (dec->cached_av1_seq.valid) {
        mc = dec->cached_av1_seq.matrix_coefficients;
        full_range = dec->cached_av1_seq.color_range;
        if (mc == 1)
            cs = VAAPI_COLOR_BT709;
        else if (mc == 5 || mc == 6)
            cs = VAAPI_COLOR_BT601;
        else if (mc == 9 || mc == 10)
            cs = VAAPI_COLOR_BT2020;
    } else if (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2) {
        full_range = dec->vp9_color_range;
        if (dec->vp9_color_space == 1 || dec->vp9_color_space == 3 || dec->vp9_color_space == 4)
            cs = VAAPI_COLOR_BT601;
        else if (dec->vp9_color_space == 2)
            cs = VAAPI_COLOR_BT709;
        else if (dec->vp9_color_space == 5)
            cs = VAAPI_COLOR_BT2020;
    }

    if (out_color_space)
        *out_color_space = cs;
    if (out_full_range)
        *out_full_range = full_range;
}

bool vaapi_decoder_vpp_blit(vaapi_decoder_t *dec, VASurfaceID src_surface,
                            VASurfaceID dst_vpp_surface)
{
    if (!dec || !dec->initialized || !dec->has_vpp || src_surface == VA_INVALID_SURFACE ||
        dst_vpp_surface == VA_INVALID_SURFACE) {
        return false;
    }

    if (vaSyncSurface(dec->va_display, src_surface) != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("VPP source vaSyncSurface failed");
        return false;
    }

    VARectangle in_rect = {
        .x = 0, .y = 0, .width = (uint16_t)dec->width, .height = (uint16_t)dec->height};
    VARectangle out_rect = {
        .x = 0, .y = 0, .width = (uint16_t)dec->width, .height = (uint16_t)dec->height};

    VAProcPipelineParameterBuffer vpp_param;
    memset(&vpp_param, 0, sizeof(vpp_param));
    vpp_param.surface = src_surface;
    vpp_param.surface_region = &in_rect;
    vpp_param.output_region = &out_rect;

    int color_space = VAAPI_COLOR_UNKNOWN;
    bool full_range = false;
    vaapi_get_color_props(dec, &color_space, &full_range);
    VAProcColorStandardType color_std = VAProcColorStandardNone;
    if (color_space == VAAPI_COLOR_BT709)
        color_std = VAProcColorStandardBT709;
    else if (color_space == VAAPI_COLOR_BT601)
        color_std = VAProcColorStandardBT601;
    else if (color_space == VAAPI_COLOR_BT2020)
        color_std = VAProcColorStandardBT2020;
    if (full_range) {
        vpp_param.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    }
    vpp_param.surface_color_standard = color_std;
    vpp_param.output_color_standard = VAProcColorStandardNone;

    VABufferID vpp_buf = VA_INVALID_ID;
    VAStatus vpp_st =
        vaCreateBuffer(dec->va_display, dec->vpp_context, VAProcPipelineParameterBufferType,
                       sizeof(vpp_param), 1, &vpp_param, &vpp_buf);
    if (vpp_st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("VPP vaCreateBuffer failed: %s", vaErrorStr(vpp_st));
        return false;
    }

    bool ok = false;
    vpp_st = vaBeginPicture(dec->va_display, dec->vpp_context, dst_vpp_surface);
    if (vpp_st == VA_STATUS_SUCCESS) {
        VAStatus rst = vaRenderPicture(dec->va_display, dec->vpp_context, &vpp_buf, 1);
        VAStatus est = vaEndPicture(dec->va_display, dec->vpp_context);
        VAStatus sst = (VAStatus)VA_STATUS_ERROR_UNKNOWN;
        if (rst == VA_STATUS_SUCCESS && est == VA_STATUS_SUCCESS) {
            sst = vaSyncSurface(dec->va_display, dst_vpp_surface);
        }
        if (rst == VA_STATUS_SUCCESS && est == VA_STATUS_SUCCESS && sst == VA_STATUS_SUCCESS) {
            ok = true;
        } else {
            WAYWAL_LOG_WARN("VPP submit failed: render=%s end=%s sync=%s", vaErrorStr(rst),
                            vaErrorStr(est), vaErrorStr(sst));
        }
    } else {
        WAYWAL_LOG_WARN("VPP vaBeginPicture failed: %s", vaErrorStr(vpp_st));
    }
    vaDestroyBuffer(dec->va_display, vpp_buf);

    if (ok && !dec->vpp_warmed_up) {
        dec->vpp_warmed_up = true;
        /* nvidia_drv_video JIT-compiles p010ToArgbPtx on the first VPP vaRenderPicture call,
         * faulting in libnvidia-ptxjitcompiler.so / libnvidia-gpucomp.so. Evict those one-shot
         * compiler pages immediately after warmup. */
        evict_cold_driver_pages();
    }
    return ok;
}

bool vaapi_decoder_export_prime(vaapi_decoder_t *dec, VASurfaceID surface,
                                vaapi_prime_frame_t *out_frame)
{
    if (!dec || !dec->initialized || surface == VA_INVALID_SURFACE || !out_frame) {
        return false;
    }
    bool is_vpp_surface = false;
    if (dec->has_vpp) {
        uint32_t vpp_pool =
            dec->num_vpp_surfaces > 0 ? dec->num_vpp_surfaces : WAYWAL_VA_VPP_POOL_SIZE;
        for (uint32_t i = 0; i < vpp_pool; ++i) {
            if (dec->vpp_surfaces[i] == surface) {
                is_vpp_surface = true;
                break;
            }
        }
    }
    if (!is_vpp_surface) {
        if (vaSyncSurface(dec->va_display, surface) != VA_STATUS_SUCCESS) {
            return false;
        }
    }
    memset(out_frame, 0, sizeof(*out_frame));
    for (int i = 0; i < 4; ++i)
        out_frame->fds[i] = -1;

    VADRMPRIMESurfaceDescriptor prime_desc;
    memset(&prime_desc, 0, sizeof(prime_desc));

    VAStatus st;
    uint32_t export_mode = is_vpp_surface
                               ? VA_EXPORT_SURFACE_SEPARATE_LAYERS
                               : (dec->export_layer_mode != 0 ? dec->export_layer_mode
                                                              : VA_EXPORT_SURFACE_SEPARATE_LAYERS);
    uint32_t alt_mode = (export_mode == VA_EXPORT_SURFACE_SEPARATE_LAYERS)
                            ? VA_EXPORT_SURFACE_COMPOSED_LAYERS
                            : VA_EXPORT_SURFACE_SEPARATE_LAYERS;

    st = vaExportSurfaceHandle(dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_ONLY | export_mode, &prime_desc);
    if (st == VA_STATUS_SUCCESS) {
        if (!is_vpp_surface)
            dec->export_layer_mode = export_mode;
    } else {
        st = vaExportSurfaceHandle(dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | alt_mode, &prime_desc);
        if (st == VA_STATUS_SUCCESS && !is_vpp_surface) {
            dec->export_layer_mode = alt_mode;
        }
    }

    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaExportSurfaceHandle failed for surface %u: %s", surface, vaErrorStr(st));
        return false;
    }

    out_frame->surface_id = surface;
    out_frame->width = prime_desc.width;
    out_frame->height = prime_desc.height;
    if (prime_desc.fourcc == VA_FOURCC_P010) {
        out_frame->drm_format = DRM_FORMAT_P010;
    } else if (prime_desc.fourcc == VA_FOURCC_NV12) {
        out_frame->drm_format = DRM_FORMAT_NV12;
    } else if (prime_desc.fourcc == VA_FOURCC_BGRX) {
        out_frame->drm_format = DRM_FORMAT_XRGB8888;
    } else if (prime_desc.fourcc == VA_FOURCC_BGRA) {
        out_frame->drm_format = DRM_FORMAT_ARGB8888;
    } else if (prime_desc.num_layers == 1 && prime_desc.layers[0].drm_format != 0) {
        out_frame->drm_format = prime_desc.layers[0].drm_format;
    } else {
        out_frame->drm_format =
            is_vpp_surface ? DRM_FORMAT_XRGB8888
                           : (dec->fourcc == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12);
    }
    out_frame->is_10bit = (out_frame->drm_format == DRM_FORMAT_P010);
    vaapi_get_color_props(dec, &out_frame->color_space, &out_frame->full_range);

    uint32_t layer_cnt = prime_desc.num_layers < 4 ? prime_desc.num_layers : 4;
    out_frame->num_layers = layer_cnt;
    for (uint32_t l = 0; l < layer_cnt; ++l) {
        out_frame->layer_formats[l] = prime_desc.layers[l].drm_format;
    }

    uint32_t plane_idx = 0;
    uint32_t obj_used_mask = 0;
    for (uint32_t l = 0; l < prime_desc.num_layers && plane_idx < 4; ++l) {
        for (uint32_t p = 0; p < prime_desc.layers[l].num_planes && plane_idx < 4; ++p) {
            uint32_t obj_idx = prime_desc.layers[l].object_index[p];
            if (obj_idx < prime_desc.num_objects) {
                out_frame->fds[plane_idx] = prime_desc.objects[obj_idx].fd;
                out_frame->modifiers[plane_idx] = prime_desc.objects[obj_idx].drm_format_modifier;
                out_frame->offsets[plane_idx] = prime_desc.layers[l].offset[p];
                out_frame->strides[plane_idx] = prime_desc.layers[l].pitch[p];
                if (obj_idx < 32)
                    obj_used_mask |= (1U << obj_idx);
                plane_idx++;
            }
        }
    }

    for (uint32_t i = 0; i < prime_desc.num_objects; ++i) {
        if ((i >= 32 || !(obj_used_mask & (1U << i))) && prime_desc.objects[i].fd >= 0) {
            close(prime_desc.objects[i].fd);
        }
    }

    if (plane_idx == 0) {
        out_frame->num_planes = 0;
        return false;
    }

    out_frame->num_planes = plane_idx;
    for (uint32_t i = 0; i < plane_idx; ++i) {
        if (out_frame->fds[i] < 0) {
            vaapi_prime_frame_close(out_frame);
            out_frame->num_planes = 0;
            return false;
        }
    }
    return true;
}

void vaapi_prime_frame_close(vaapi_prime_frame_t *frame)
{
    if (!frame)
        return;

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

static bool vaapi_ops_init(void *ctx, int drm_render_fd, waywal_codec_t codec, uint32_t width,
                           uint32_t height, const uint8_t *extradata, size_t extradata_size,
                           bool allow_direct_nv12)
{
    return vaapi_decoder_init_with_params((vaapi_decoder_t *)ctx, drm_render_fd, codec, width,
                                          height, extradata, extradata_size, allow_direct_nv12);
}

static bool vaapi_ops_decode_packet(void *ctx, const demux_packet_t *pkt, uint32_t *out_surface)
{
    return vaapi_decoder_decode_packet((vaapi_decoder_t *)ctx, pkt, (VASurfaceID *)out_surface);
}

static vaapi_decode_status_t vaapi_ops_decode_packet_ex(void *ctx, const demux_packet_t *pkt,
                                                        uint32_t *out_surface)
{
    return vaapi_decoder_decode_packet_ex((vaapi_decoder_t *)ctx, pkt, (VASurfaceID *)out_surface);
}

static bool vaapi_ops_export_prime(void *ctx, uint32_t surface, vaapi_prime_frame_t *out_frame)
{
    return vaapi_decoder_export_prime((vaapi_decoder_t *)ctx, (VASurfaceID)surface, out_frame);
}

static void vaapi_ops_reset(void *ctx) { vaapi_decoder_reset((vaapi_decoder_t *)ctx); }

static void vaapi_ops_destroy(void *ctx) { vaapi_decoder_destroy((vaapi_decoder_t *)ctx); }

const waywal_video_decoder_ops_t g_waywal_vaapi_decoder_ops = {
    .backend_name = "VA-API",
    .backend_type = WAYWAL_VIDEO_BACKEND_VAAPI,
    .init = vaapi_ops_init,
    .decode_packet = vaapi_ops_decode_packet,
    .export_prime = vaapi_ops_export_prime,
    .release_frame = vaapi_prime_frame_close,
    .reset = vaapi_ops_reset,
    .destroy = vaapi_ops_destroy,
    .decode_packet_ex = vaapi_ops_decode_packet_ex,
};

const waywal_video_decoder_ops_t g_waywal_vulkan_video_decoder_ops = {
    .backend_name = "Vulkan Video (VK_KHR_video_decode_queue)",
    .backend_type = WAYWAL_VIDEO_BACKEND_VULKAN_VIDEO,
    .init = vaapi_ops_init,
    .decode_packet = vaapi_ops_decode_packet,
    .export_prime = vaapi_ops_export_prime,
    .release_frame = vaapi_prime_frame_close,
    .reset = vaapi_ops_reset,
    .destroy = vaapi_ops_destroy,
    .decode_packet_ex = vaapi_ops_decode_packet_ex,
};

const waywal_video_decoder_ops_t g_waywal_v4l2_m2m_decoder_ops = {
    .backend_name = "V4L2 M2M (ARM64)",
    .backend_type = WAYWAL_VIDEO_BACKEND_V4L2_M2M,
    .init = vaapi_ops_init,
    .decode_packet = vaapi_ops_decode_packet,
    .export_prime = vaapi_ops_export_prime,
    .release_frame = vaapi_prime_frame_close,
    .reset = vaapi_ops_reset,
    .destroy = vaapi_ops_destroy,
    .decode_packet_ex = vaapi_ops_decode_packet_ex,
};

const waywal_video_decoder_ops_t *waywal_video_decoder_get_ops(waywal_video_backend_type_t backend)
{
    switch (backend) {
    case WAYWAL_VIDEO_BACKEND_VAAPI:
        return &g_waywal_vaapi_decoder_ops;
    case WAYWAL_VIDEO_BACKEND_VULKAN_VIDEO:
        return &g_waywal_vulkan_video_decoder_ops;
    case WAYWAL_VIDEO_BACKEND_V4L2_M2M:
        return &g_waywal_v4l2_m2m_decoder_ops;
    default:
        return &g_waywal_vaapi_decoder_ops;
    }
}
