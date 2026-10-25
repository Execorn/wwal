#include "waywal/log.h"
#include "waywal/vaapi_dec.h"
#include "waywal/video_decoder.h"

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
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return;
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        if (!strstr(line, "libnvidia-gpucomp.so") &&
            !strstr(line, "libnvidia-ptxjitcompiler.so"))
            continue;
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
            if (strcmp(perms, "r-xp") == 0 && end > start) {
                madvise((void *)start, (size_t)(end - start), MADV_DONTNEED);
            }
        }
    }
    fclose(maps);
}

static bool profile_supported(const VAProfile *profiles, int count, VAProfile target)
{
    for (int i = 0; i < count; ++i) {
        if (profiles[i] == target)
            return true;
    }
    return false;
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
    if (codec == WAYWAL_CODEC_HEVC && extradata && extradata_size >= 19 && extradata[0] == 1) {
        uint8_t general_profile_idc = extradata[1] & 0x1Fu;
        uint8_t bit_depth_luma_minus8 = extradata[17] & 0x07u;
        uint8_t bit_depth_chroma_minus8 = extradata[18] & 0x07u;
        if (general_profile_idc == 2 || bit_depth_luma_minus8 == 2 ||
            bit_depth_chroma_minus8 == 2) {
            is_10bit_hint = true;
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
    if (!dec || width == 0 || height == 0)
        return false;
    bool saved_allow_direct_p010 = dec->allow_direct_p010;
    memset(dec, 0, sizeof(*dec));
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
    memset(&dec->cached_iq, 16, sizeof(dec->cached_iq));
    dec->iq_valid = true;

    /* Avoid eagerly faulting libnvidia-ptxjitcompiler.so (~21.5 MiB PSS) into host RAM */
    setenv("CUDA_MODULE_LOADING", "LAZY", 0);

    const char *candidate_nodes[] = {NULL, "/dev/dri/renderD128", "/dev/dri/renderD129"};
    bool found = false;

    for (int i = 0; i < 3; ++i) {
        int fd = -1;
        bool owns_fd = false;
        if (i == 0) {
            if (drm_render_fd < 0)
                continue;
            fd = drm_render_fd;
            owns_fd = false;
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
        if (st != VA_STATUS_SUCCESS && getenv("LIBVA_DRIVER_NAME") != NULL) {
            vaTerminate(dpy);
            unsetenv("LIBVA_DRIVER_NAME");
            dpy = vaGetDisplayDRM(fd);
            if (dpy) {
                st = vaInitialize(dpy, &major, &minor);
            }
        }
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
        dec->is_nvidia_vaapi = vendor && (strstr(vendor, "nvidia") != NULL ||
                                          strstr(vendor, "NVIDIA") != NULL ||
                                          strstr(vendor, "NVDEC") != NULL ||
                                          strstr(vendor, "vdpau") != NULL);
        if (dec->is_nvidia_vaapi && drm_render_fd >= 0) {
            allow_direct_nv12 = false;
        }
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
    dec->num_surfaces = max_refs + 3;
    if (dec->num_surfaces < 4)
        dec->num_surfaces = 4;
    if (dec->num_surfaces > WAYWAL_VA_SURFACE_POOL_SIZE)
        dec->num_surfaces = WAYWAL_VA_SURFACE_POOL_SIZE;

    bool is_10bit_stream =
        (dec->va_profile == VAProfileHEVCMain10 || dec->va_profile == VAProfileVP9Profile2 ||
         (dec->va_profile == VAProfileAV1Profile0 && dec->cached_av1_seq.valid &&
          dec->cached_av1_seq.high_bitdepth) ||
         (dec->cached_hevc_sps.valid && (dec->cached_hevc_sps.bit_depth_luma_minus8 == 2 ||
                                         dec->cached_hevc_sps.bit_depth_chroma_minus8 == 2)));
    if (is_10bit_stream && !dec->allow_direct_p010) {
        allow_direct_nv12 = false;
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
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0)
            close(dec->drm_fd);
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

    st = vaCreateSurfaces(dec->va_display, rt_format, width, height, dec->surfaces,
                          dec->num_surfaces, sattrs, 2);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateSurfaces failed for %ux%u pool (%u surfaces): %s", width, height,
                       dec->num_surfaces, vaErrorStr(st));
        vaDestroyConfig(dec->va_display, dec->va_config);
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0)
            close(dec->drm_fd);
        return false;
    }

    st = vaCreateContext(dec->va_display, dec->va_config, (int)width, (int)height, VA_PROGRESSIVE,
                         dec->surfaces, (int)dec->num_surfaces, &dec->va_context);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaCreateContext failed: %s", vaErrorStr(st));
        vaDestroySurfaces(dec->va_display, dec->surfaces, dec->num_surfaces);
        vaDestroyConfig(dec->va_display, dec->va_config);
        vaTerminate(dec->va_display);
        if (dec->owns_drm_fd && dec->drm_fd >= 0)
            close(dec->drm_fd);
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
        if (vpp_st == VA_STATUS_SUCCESS) {
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
            if (vpp_st == VA_STATUS_SUCCESS) {
                vpp_st = vaCreateContext(dec->va_display, dec->vpp_config, (int)width, (int)height,
                                         0, dec->vpp_surfaces, (int)vpp_pool, &dec->vpp_context);
                if (vpp_st == VA_STATUS_SUCCESS) {
                    dec->has_vpp = true;
                    dec->num_vpp_surfaces = vpp_pool;
                    WAYWAL_LOG_INFO(
                        "VA-API VPP active (NV12 -> BGRX, %u decode surfaces + %u VPP surfaces @ %ux%u)",
                        dec->num_surfaces, dec->num_vpp_surfaces, width, height);
                } else {
                    vaDestroySurfaces(dec->va_display, dec->vpp_surfaces, vpp_pool);
                    vaDestroyConfig(dec->va_display, dec->vpp_config);
                    dec->vpp_config = VA_INVALID_ID;
                }
            } else {
                vaDestroyConfig(dec->va_display, dec->vpp_config);
                dec->vpp_config = VA_INVALID_ID;
            }
        }
    } else {
        WAYWAL_LOG_INFO(
            "VA-API direct %s DMA-BUF export active (%u surfaces @ %ux%u, max_refs=%u, VPP bypassed)",
            dec->fourcc == VA_FOURCC_P010 ? "P010" : "NV12", dec->num_surfaces, width, height,
            max_refs);
    }

    /* Evict cold executable text pages of the PTX shader compiler mapped during driver init */
    evict_cold_driver_pages();

    dec->initialized = true;
    dec->current_surface_idx = 0;
    return true;
}

void vaapi_decoder_reset(vaapi_decoder_t *dec)
{
    if (!dec) return;
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
}

void vaapi_decoder_destroy(vaapi_decoder_t *dec)
{
    if (!dec || !dec->initialized)
        return;

    uint32_t pool_size = dec->num_surfaces > 0 ? dec->num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    uint32_t vpp_pool =
        dec->num_vpp_surfaces > 0 ? dec->num_vpp_surfaces : WAYWAL_VA_VPP_POOL_SIZE;

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
         * destroyContext signals ctx->cond without locking ctx->mutex. Waiting 1ms guarantees
         * resolveSurfaces is parked in pthread_cond_wait before vaDestroyContext signals it. */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }

    if (dec->has_vpp) {
        if (dec->vpp_context != VA_INVALID_ID) {
            vaDestroyContext(dec->va_display, dec->vpp_context);
            dec->vpp_context = VA_INVALID_ID;
        }
        vaDestroySurfaces(dec->va_display, dec->vpp_surfaces, vpp_pool);
        if (dec->vpp_config != VA_INVALID_ID) {
            vaDestroyConfig(dec->va_display, dec->vpp_config);
            dec->vpp_config = VA_INVALID_ID;
        }
        dec->has_vpp = false;
        dec->num_vpp_surfaces = 0;
    }

    if (dec->va_context != VA_INVALID_ID) {
        vaDestroyContext(dec->va_display, dec->va_context);
        dec->va_context = VA_INVALID_ID;
    }

    vaDestroySurfaces(dec->va_display, dec->surfaces, pool_size);

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

static inline bool more_rbsp_data(const bit_reader_t *br)
{
    return br->bit_pos < br->end_bit_pos;
}

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
    if (ue == 0xFFFFFFFFU) return 0;
    if (ue % 2 == 1) {
        return (int32_t)((ue + 1) / 2);
    } else {
        return -(int32_t)(ue / 2);
    }
}

static size_t unescape_rbsp(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_len) {
        if (si + 2 < src_len && src[si] == 0 && src[si + 1] == 0 && src[si + 2] == 3) {
            if (di + 2 > dst_len) break;
            dst[di++] = 0;
            dst[di++] = 0;
            si += 3;
        } else {
            dst[di++] = src[si++];
        }
    }
    return di;
}

static size_t rbsp_bit_offset_to_ebsp_bits(const uint8_t *ebsp, size_t ebsp_len, size_t rbsp_bits)
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

/* ITU-T H.264 Table 7-3 and Table 7-4 default scaling lists */
static const uint8_t g_default_4x4_intra[16] = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};
static const uint8_t g_default_4x4_inter[16] = {
    10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};
static const uint8_t g_default_8x8_intra[64] = {
     6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};
static const uint8_t g_default_8x8_inter[64] = {
     9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

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

static void refresh_cached_h264_iq(vaapi_decoder_t *dec)
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

static bool parse_h264_sps(const uint8_t *nal, size_t nal_len, parsed_sps_t *sps)
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
                const uint8_t *def = (i < 6) ? ((i < 3) ? g_default_4x4_intra : g_default_4x4_inter)
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

    sps->log2_max_frame_num_minus4 = (uint8_t)read_ue(&br);
    sps->pic_order_cnt_type = (uint8_t)read_ue(&br);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)read_ue(&br);
    } else if (sps->pic_order_cnt_type == 1) {
        sps->delta_pic_order_always_zero_flag = read_bit(&br) != 0;
        read_se(&br);
        read_se(&br);
        uint32_t num_ref = read_ue(&br);
        for (uint32_t i = 0; i < num_ref; ++i) {
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

static bool parse_h264_pps(const uint8_t *nal, size_t nal_len, const parsed_sps_t *sps,
                           parsed_pps_t *pps)
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
                const uint8_t *def = (i < 6) ? ((i < 3) ? g_default_4x4_intra : g_default_4x4_inter)
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

typedef struct {
    uint32_t frame_num;
    int32_t poc_lsb;
    bool mmco_reset;
    uint32_t num_unmark;
    int32_t unmark_pic_nums[16];
} slice_header_meta_t;

static bool parse_h264_slice_header(const uint8_t *nal, size_t nal_len,
                                    const parsed_sps_t *sps, const parsed_pps_t *pps,
                                    VASliceParameterBufferH264 *sp,
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

    uint32_t fn_bits = (sps->valid ? sps->log2_max_frame_num_minus4 : 4) + 4;
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
        uint32_t poc_bits = (sps->valid ? sps->log2_max_pic_order_cnt_lsb_minus4 : 4) + 4;
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
        if (sp->num_ref_idx_l0_active_minus1 > 31) sp->num_ref_idx_l0_active_minus1 = 31;
        if (sp->num_ref_idx_l1_active_minus1 > 31) sp->num_ref_idx_l1_active_minus1 = 31;
    }

    /* ref_pic_list_modification() */
    if (sp->slice_type != 2 && sp->slice_type != 4) {
        if (read_bit(&br)) { /* ref_pic_list_modification_flag_l0 */
            uint32_t mod_idc;
            do {
                mod_idc = read_ue(&br);
                if (mod_idc == 0 || mod_idc == 1 || mod_idc == 2)
                    read_ue(&br);
            } while (mod_idc != 3 && br.bit_pos < br.size * 8);
        }
        if (sp->slice_type == 1) {
            if (read_bit(&br)) { /* ref_pic_list_modification_flag_l1 */
                uint32_t mod_idc;
                do {
                    mod_idc = read_ue(&br);
                    if (mod_idc == 0 || mod_idc == 1 || mod_idc == 2)
                        read_ue(&br);
                } while (mod_idc != 3 && br.bit_pos < br.size * 8);
            }
        }
    }

    /* pred_weight_table() */
    if ((pps->weighted_pred_flag && (sp->slice_type == 0 || sp->slice_type == 3)) ||
        (pps->weighted_bipred_idc == 1 && sp->slice_type == 1)) {
        sp->luma_log2_weight_denom = (uint8_t)read_ue(&br);
        if (sps->chroma_format_idc != 0) {
            sp->chroma_log2_weight_denom = (uint8_t)read_ue(&br);
        }
        for (uint32_t i = 0; i <= sp->num_ref_idx_l0_active_minus1; ++i) {
            sp->luma_weight_l0_flag = (uint8_t)read_bit(&br);
            if (sp->luma_weight_l0_flag) {
                sp->luma_weight_l0[i] = (int16_t)read_se(&br);
                sp->luma_offset_l0[i] = (int16_t)read_se(&br);
            } else {
                sp->luma_weight_l0[i] = (int16_t)(1 << sp->luma_log2_weight_denom);
                sp->luma_offset_l0[i] = 0;
            }
            if (sps->chroma_format_idc != 0) {
                sp->chroma_weight_l0_flag = (uint8_t)read_bit(&br);
                if (sp->chroma_weight_l0_flag) {
                    for (int j = 0; j < 2; ++j) {
                        sp->chroma_weight_l0[i][j] = (int16_t)read_se(&br);
                        sp->chroma_offset_l0[i][j] = (int16_t)read_se(&br);
                    }
                } else {
                    for (int j = 0; j < 2; ++j) {
                        sp->chroma_weight_l0[i][j] = (int16_t)(1 << sp->chroma_log2_weight_denom);
                        sp->chroma_offset_l0[i][j] = 0;
                    }
                }
            }
        }
        if (sp->slice_type == 1) {
            for (uint32_t i = 0; i <= sp->num_ref_idx_l1_active_minus1; ++i) {
                sp->luma_weight_l1_flag = (uint8_t)read_bit(&br);
                if (sp->luma_weight_l1_flag) {
                    sp->luma_weight_l1[i] = (int16_t)read_se(&br);
                    sp->luma_offset_l1[i] = (int16_t)read_se(&br);
                } else {
                    sp->luma_weight_l1[i] = (int16_t)(1 << sp->luma_log2_weight_denom);
                    sp->luma_offset_l1[i] = 0;
                }
                if (sps->chroma_format_idc != 0) {
                    sp->chroma_weight_l1_flag = (uint8_t)read_bit(&br);
                    if (sp->chroma_weight_l1_flag) {
                        for (int j = 0; j < 2; ++j) {
                            sp->chroma_weight_l1[i][j] = (int16_t)read_se(&br);
                            sp->chroma_offset_l1[i][j] = (int16_t)read_se(&br);
                        }
                    } else {
                        for (int j = 0; j < 2; ++j) {
                            sp->chroma_weight_l1[i][j] = (int16_t)(1 << sp->chroma_log2_weight_denom);
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
                int32_t max_fn = 1 << fn_bits;
                do {
                    mmco = read_ue(&br);
                    if (mmco == 1 || mmco == 3) {
                        uint32_t diff_minus1 = read_ue(&br);
                        if (mmco == 1 && meta && meta->num_unmark < 16) {
                            int32_t pn = ((int32_t)slice_fn - (int32_t)(diff_minus1 + 1)) & (max_fn - 1);
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

static bool next_h264_nal(const uint8_t *data, size_t size, bool is_avcc, uint8_t nal_len_sz,
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
    if (off + 4 <= size &&
        !(data[off] == 0 && data[off + 1] == 0 &&
          (data[off + 2] == 1 || (data[off + 2] == 0 && data[off + 3] == 1)))) {
        uint32_t len = ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
                       ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
        if (len > 0 && off + 4 + len <= size) {
            *out_nal = data + off + 4;
            *out_len = len;
            *scan_off = off + 4 + len;
            return true;
        }
    }

    while (off + 3 <= size) {
        if (data[off] == 0 && data[off + 1] == 0) {
            size_t hdr_len = 0;
            if (data[off + 2] == 1) {
                hdr_len = 3;
            } else if (off + 4 <= size && data[off + 2] == 0 && data[off + 3] == 1) {
                hdr_len = 4;
            }
            if (hdr_len > 0) {
                size_t nal_start = off + hdr_len;
                size_t next_sc = nal_start;
                while (next_sc + 3 <= size) {
                    if (data[next_sc] == 0 && data[next_sc + 1] == 0 &&
                        (data[next_sc + 2] == 1 ||
                         (next_sc + 3 < size && data[next_sc + 2] == 0 && data[next_sc + 3] == 1))) {
                        break;
                    }
                    next_sc++;
                }
                if (next_sc + 3 > size)
                    next_sc = size;
                if (next_sc > nal_start) {
                    *out_nal = data + nal_start;
                    *out_len = (uint32_t)(next_sc - nal_start);
                    *scan_off = next_sc;
                    return true;
                }
            }
        }
        off++;
    }
    *scan_off = size;
    return false;
}

static void fill_va_pic_h264(VAPictureH264 *vp, const h264_dpb_entry_t *e)
{
    vp->picture_id = e->surface;
    vp->frame_idx = (uint16_t)e->frame_num;
    vp->flags = e->is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    vp->TopFieldOrderCnt = e->pic_order_cnt;
    vp->BottomFieldOrderCnt = e->pic_order_cnt;
}

static void build_h264_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_fn,
                                 int32_t max_fn, int32_t curr_poc,
                                 VASliceParameterBufferH264 *sp)
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
        for (size_t i = 0; i < n_before; ++i) fill_va_pic_h264(&sp->RefPicList0[idx0++], &before[i]);
        for (size_t i = 0; i < n_after; ++i) fill_va_pic_h264(&sp->RefPicList0[idx0++], &after[i]);

        size_t idx1 = 0;
        for (size_t i = 0; i < n_after; ++i) fill_va_pic_h264(&sp->RefPicList1[idx1++], &after[i]);
        for (size_t i = 0; i < n_before; ++i) fill_va_pic_h264(&sp->RefPicList1[idx1++], &before[i]);

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

static uint32_t ceil_log2_u32(uint32_t val)
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
                                      hevc_st_rps_t *rps_table, uint8_t *num_neg,
                                      uint8_t *num_pos)
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
        int32_t delta_rps =
            (1 - 2 * (int32_t)delta_rps_sign) * ((int32_t)abs_delta_rps_minus1 + 1);

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

static bool parse_hevc_sps(const uint8_t *nal, size_t nal_len, parsed_hevc_sps_t *sps)
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
    sps->valid = (sps->pic_width_in_luma_samples > 0 && sps->pic_height_in_luma_samples > 0);
    return sps->valid;
}

static bool parse_hevc_pps(const uint8_t *nal, size_t nal_len, parsed_hevc_pps_t *pps)
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

static void skip_hevc_pred_weight_table(bs_t *br, const parsed_hevc_sps_t *sps,
                                        uint32_t slice_type, uint32_t num_ref_l0,
                                        uint32_t num_ref_l1)
{
    uint32_t chroma_array_type =
        (sps && !sps->separate_colour_plane_flag) ? sps->chroma_format_idc : 0;
    read_ue(br); /* luma_log2_weight_denom */
    if (chroma_array_type != 0) {
        read_se(br); /* delta_chroma_log2_weight_denom */
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
            read_se(br);
            read_se(br);
        }
        if (chroma_w_l0[i]) {
            for (int j = 0; j < 2; ++j) {
                read_se(br);
                read_se(br);
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
                read_se(br);
                read_se(br);
            }
            if (chroma_w_l1[i]) {
                for (int j = 0; j < 2; ++j) {
                    read_se(br);
                    read_se(br);
                }
            }
        }
    }
}

static bool parse_hevc_slice_header(const uint8_t *nal, size_t nal_len,
                                    const parsed_hevc_sps_t *sps, const parsed_hevc_pps_t *pps,
                                    VASliceParameterBufferHEVC *sp, int32_t *out_poc_lsb,
                                    uint32_t *out_st_rps_bits)
{
    if (!nal || nal_len < 3 || !sps || !pps || !sp)
        return false;

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
    if (!first_slice) {
        if (pps->dependent_slice_segments_enabled_flag) {
            dependent_slice = (read_bit(&br) != 0);
        }
        uint32_t ctb_log2 =
            (sps->valid ? (sps->log2_min_luma_coding_block_size_minus3 + 3 +
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
        sp->slice_segment_address = read_bits(&br, ceil_log2_u32(pic_ctbs));
    }
    sp->LongSliceFlags.fields.dependent_slice_segment_flag = dependent_slice ? 1 : 0;
    sp->LongSliceFlags.fields.collocated_from_l0_flag = 1;
    sp->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag =
        pps->pps_disable_deblocking_filter_flag ? 1 : 0;
    sp->slice_beta_offset_div2 = pps->pps_beta_offset_div2;
    sp->slice_tc_offset_div2 = pps->pps_tc_offset_div2;
    sp->LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag =
        pps->pps_loop_filter_across_slices_enabled_flag ? 1 : 0;

    if (!dependent_slice) {
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
                            read_bits(&br, list_entry_bits);
                        }
                    }
                    if (sp->LongSliceFlags.fields.slice_type == 0 && read_bit(&br)) {
                        for (uint32_t i = 0; i <= sp->num_ref_idx_l1_active_minus1; ++i) {
                            read_bits(&br, list_entry_bits);
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
                    skip_hevc_pred_weight_table(&br, sps, sp->LongSliceFlags.fields.slice_type,
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
                    for (uint32_t i = 0;
                         i < num_entry_point_offsets && br.bit_pos < br.end_bit_pos; ++i) {
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

static void build_hevc_ref_lists(const h264_dpb_t *dpb, uint8_t slice_type, int32_t curr_poc,
                                 VASliceParameterBufferHEVC *sp)
{
    for (int i = 0; i < 15; ++i) {
        sp->RefPicList[0][i] = 0xFF;
        sp->RefPicList[1][i] = 0xFF;
    }
    if (!dpb || dpb->count == 0 || slice_type == 2)
        return;

    size_t n = dpb->count > 15 ? 15 : dpb->count;
    uint8_t before_idx[15];
    uint8_t after_idx[15];
    size_t n_before = 0;
    size_t n_after = 0;

    for (size_t i = 0; i < n; ++i) {
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

    size_t idx0 = 0;
    for (size_t i = 0; i < n_before && idx0 < 15; ++i)
        sp->RefPicList[0][idx0++] = before_idx[i];
    for (size_t i = 0; i < n_after && idx0 < 15; ++i)
        sp->RefPicList[0][idx0++] = after_idx[i];

    if (slice_type == 0) {
        size_t idx1 = 0;
        for (size_t i = 0; i < n_after && idx1 < 15; ++i)
            sp->RefPicList[1][idx1++] = after_idx[i];
        for (size_t i = 0; i < n_before && idx1 < 15; ++i)
            sp->RefPicList[1][idx1++] = before_idx[i];
    }
}

static const int16_t g_vp9_dc_qlookup[256] = {
    4,    8,    8,    9,    10,   11,   12,   12,   13,   14,   15,   16,   17,   18,   19,   19,
    20,   21,   22,   23,   24,   25,   26,   26,   27,   28,   29,   30,   31,   32,   32,   33,
    34,   35,   36,   37,   38,   38,   39,   40,   41,   42,   43,   43,   44,   45,   46,   47,
    48,   48,   49,   50,   51,   52,   53,   53,   54,   55,   56,   57,   57,   58,   59,   60,
    61,   62,   62,   63,   64,   65,   66,   66,   67,   68,   69,   70,   70,   71,   72,   73,
    74,   74,   75,   76,   77,   78,   78,   79,   80,   81,   81,   82,   83,   84,   85,   85,
    87,   88,   90,   92,   93,   95,   96,   98,   99,   101,  102,  104,  105,  107,  108,  110,
    111,  113,  114,  116,  117,  118,  120,  121,  123,  125,  127,  129,  131,  134,  136,  138,
    140,  142,  144,  146,  148,  150,  152,  154,  156,  158,  161,  164,  166,  169,  172,  174,
    177,  180,  182,  185,  187,  190,  192,  195,  199,  202,  205,  208,  211,  214,  217,  220,
    223,  226,  230,  233,  237,  240,  243,  247,  250,  253,  257,  261,  265,  269,  272,  276,
    280,  284,  288,  292,  296,  300,  304,  309,  313,  317,  322,  326,  330,  335,  340,  344,
    349,  354,  359,  364,  369,  374,  379,  384,  389,  395,  400,  406,  411,  417,  423,  429,
    435,  441,  447,  454,  461,  467,  475,  482,  489,  497,  505,  513,  522,  530,  539,  549,
    559,  569,  579,  590,  602,  614,  626,  640,  654,  668,  684,  700,  717,  736,  755,  775,
    796,  819,  843,  869,  896,  925,  955,  988,  1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
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

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uint32_t vp9_split_superframe(const uint8_t *data, size_t size, const uint8_t *sub_ptrs[8],
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
                if (offset + frame_sz > size - index_size) {
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

static bool parse_vp9_uncompressed_header(vaapi_decoder_t *dec, const uint8_t *data, size_t size,
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

static uint64_t av1_read_leb128(const uint8_t *data, size_t size, size_t *out_bytes)
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
    wm->wmmat[idx] =
        (av1_decode_signed_subexp_with_ref(br, -mx, mx + 1, r) << prec_diff) + round;
}

static bool parse_av1_sequence_header(const uint8_t *payload, size_t len, parsed_av1_seq_t *seq)
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
static uint32_t parse_av1_tile_group_obu(const uint8_t *obu_data, size_t obu_size,
                                         size_t tg_offset, uint32_t base_obu_offset,
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

static bool parse_av1_frame_header(vaapi_decoder_t *dec, const uint8_t *payload, size_t len,
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
    uint32_t allow_screen_content_tools =
        (seq->seq_force_screen_content_tools == 2) ? read_bit(&br)
                                                   : seq->seq_force_screen_content_tools;
    uint32_t force_integer_mv = (frame_type == 0 || frame_type == 2) ? 1 : 0;
    if (allow_screen_content_tools) {
        force_integer_mv =
            (seq->seq_force_integer_mv == 2) ? read_bit(&br) : seq->seq_force_integer_mv;
    }

    if (seq->frame_id_numbers_present_flag) {
        uint32_t id_len =
            (uint32_t)seq->delta_frame_id_length_minus_2 + 2 +
            (uint32_t)seq->additional_frame_id_length_minus_1 + 1;
        read_bits(&br, id_len);
    }

    uint32_t frame_size_override =
        (frame_type == 3) ? 1 : (seq->reduced_still_picture_header ? 0 : read_bit(&br));
    uint8_t order_hint =
        seq->enable_order_hint ? (uint8_t)read_bits(&br, (uint32_t)seq->order_hint_bits_minus_1 + 1)
                               : 0;
    uint8_t primary_ref_frame =
        (frame_type == 0 || frame_type == 2 || error_resilient) ? 7 : (uint8_t)read_bits(&br, 3);

    uint8_t refresh_frame_flags = 0xFF;
    if (frame_type != 0) {
        refresh_frame_flags = (frame_type == 3) ? 0xFF : (uint8_t)read_bits(&br, 8);
    } else if (!show_frame) {
        refresh_frame_flags = (uint8_t)read_bits(&br, 8);
    }

    uint16_t width_m1 =
        seq->valid ? seq->max_frame_width_minus_1 : (uint16_t)(dec->width - 1);
    uint16_t height_m1 =
        seq->valid ? seq->max_frame_height_minus_1 : (uint16_t)(dec->height - 1);
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
        if (frame_type == 2 && refresh_frame_flags != 0xFF && seq->enable_order_hint) {
            for (int i = 0; i < 8; ++i) {
                read_bits(&br, (uint32_t)seq->order_hint_bits_minus_1 + 1);
            }
        }
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
            uint32_t max_w = (sb_cols - start_sb) < max_tile_width_sb ? (sb_cols - start_sb)
                                                                      : max_tile_width_sb;
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
            for (int i = 0; i < 8; ++i) {
                uint8_t mask = 0;
                for (int j = 0; j < 8; ++j) {
                    if (read_bit(&br)) {
                        mask |= (uint8_t)(1U << j);
                        int16_t val = 0;
                        if (seg_feature_bits[j] > 0) {
                            if (seg_feature_signed[j]) {
                                val = (int16_t)av1_read_su(&br, (uint32_t)seg_feature_bits[j] + 1U);
                                val = (int16_t)clamp_i32(val, -seg_feature_max[j], seg_feature_max[j]);
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
        pic_param->loop_filter_info_fields.bits.mode_ref_delta_update =
            lf_ref_delta_update ? 1 : 0;
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
    if (!frame_is_intra) {
        for (int ref = 0; ref < 7; ++ref) {
            VAAV1TransformationType type = VAAV1TransformationIdentity;
            if (read_bit(&br)) {
                if (read_bit(&br)) {
                    type = VAAV1TransformationRotzoom;
                } else {
                    type = read_bit(&br) ? VAAV1TransformationTranslation
                                         : VAAV1TransformationAffine;
                }
            }
            pic_param->wm[ref].wmtype = type;
            if (type >= VAAV1TransformationRotzoom) {
                av1_read_global_param(&br, type, 2, allow_high_precision_mv, &pic_param->wm[ref]);
                av1_read_global_param(&br, type, 3, allow_high_precision_mv, &pic_param->wm[ref]);
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
                av1_read_global_param(&br, type, 0, allow_high_precision_mv, &pic_param->wm[ref]);
                av1_read_global_param(&br, type, 1, allow_high_precision_mv, &pic_param->wm[ref]);
            }
        }
    }

    /* 10. film_grain_params() (AV1 Spec 5.9.30) */
    if (seq->film_grain_params_present && (show_frame || showable_frame)) {
        bool apply_grain = (read_bit(&br) != 0);
        pic_param->film_grain_info.film_grain_info_fields.bits.apply_grain = apply_grain ? 1 : 0;
        if (apply_grain) {
            pic_param->film_grain_info.grain_seed = (uint16_t)read_bits(&br, 16);
            bool update_grain = (frame_type == 1) ? (read_bit(&br) != 0) : true;
            if (!update_grain) {
                read_bits(&br, 3);
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
                        pic_param->film_grain_info.point_cb_scaling[i] =
                            (uint8_t)read_bits(&br, 8);
                    }
                    num_cr_points = (uint8_t)read_bits(&br, 4);
                    if (num_cr_points > 10)
                        num_cr_points = 10;
                    for (uint8_t i = 0; i < num_cr_points; ++i) {
                        pic_param->film_grain_info.point_cr_value[i] = (uint8_t)read_bits(&br, 8);
                        pic_param->film_grain_info.point_cr_scaling[i] =
                            (uint8_t)read_bits(&br, 8);
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
    pic_param->seq_info_fields.fields.enable_masked_compound =
        seq->enable_masked_compound ? 1 : 0;
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

    *out_refresh_flags = refresh_frame_flags;
    *out_is_keyframe = (frame_type == 0 && show_frame != 0);
    return true;
}

bool vaapi_decoder_set_extradata(vaapi_decoder_t *dec, const uint8_t *extradata, size_t size)
{
    if (!dec || !extradata || size < 4)
        return false;

    if (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
        dec->va_profile == VAProfileH264ConstrainedBaseline) {
        if (size < 7)
            return false;
        if (extradata[0] == 1) {
            dec->is_avcc = true;
            dec->nal_length_size = (extradata[4] & 0x03) + 1;
        }
        uint32_t num_sps = extradata[5] & 0x1F;
        size_t off = 6;
        for (uint32_t i = 0; i < num_sps && off + 2 <= size; ++i) {
            uint16_t sps_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
            off += 2;
            if (off + sps_len <= size) {
                parse_h264_sps(extradata + off, sps_len, &dec->cached_sps);
                off += sps_len;
            }
        }
        if (off < size) {
            uint8_t num_pps = extradata[off++];
            for (uint32_t i = 0; i < num_pps && off + 2 <= size; ++i) {
                uint16_t pps_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
                off += 2;
                if (off + pps_len <= size) {
                    parse_h264_pps(extradata + off, pps_len, &dec->cached_sps, &dec->cached_pps);
                    off += pps_len;
                }
            }
        }
        refresh_cached_h264_iq(dec);
        return dec->cached_sps.valid;
    } else if (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10) {
        if (size >= 23 && (extradata[0] == 1 || extradata[0] == 0)) {
            dec->is_avcc = true;
            dec->nal_length_size = (extradata[21] & 0x03) + 1;
            uint8_t num_arrays = extradata[22];
            size_t off = 23;
            for (uint8_t a = 0; a < num_arrays && off + 3 <= size; ++a) {
                uint8_t nal_type = extradata[off] & 0x3F;
                uint16_t num_nalus = ((uint16_t)extradata[off + 1] << 8) | extradata[off + 2];
                off += 3;
                for (uint16_t n = 0; n < num_nalus && off + 2 <= size; ++n) {
                    uint16_t nal_len = ((uint16_t)extradata[off] << 8) | extradata[off + 1];
                    off += 2;
                    if (off + nal_len > size)
                        break;
                    if (nal_type == 33) {
                        parse_hevc_sps(extradata + off, nal_len, &dec->cached_hevc_sps);
                    } else if (nal_type == 34) {
                        parse_hevc_pps(extradata + off, nal_len, &dec->cached_hevc_pps);
                    }
                    off += nal_len;
                }
            }
            return dec->cached_hevc_sps.valid;
        }
        size_t scan_off = 0;
        const uint8_t *nal_ptr = NULL;
        uint32_t nal_len = 0;
        while (next_h264_nal(extradata, size, false, 4, &scan_off, &nal_ptr, &nal_len)) {
            if (nal_len >= 2) {
                uint8_t ntype = (nal_ptr[0] >> 1) & 0x3F;
                if (ntype == 33) {
                    parse_hevc_sps(nal_ptr, nal_len, &dec->cached_hevc_sps);
                } else if (ntype == 34) {
                    parse_hevc_pps(nal_ptr, nal_len, &dec->cached_hevc_pps);
                }
            }
        }
        return dec->cached_hevc_sps.valid;
    } else if (dec->va_profile == VAProfileAV1Profile0) {
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
                off += leb_bytes;
                if (parsed_sz > size - off)
                    break;
                obu_sz = (size_t)parsed_sz;
            }
            if (obu_type == 1) {
                parse_av1_sequence_header(extradata + off, obu_sz, &dec->cached_av1_seq);
            }
            off += obu_sz;
        }
        return dec->cached_av1_seq.valid;
    }
    return true;
}

bool vaapi_decoder_decode_packet(vaapi_decoder_t *dec, const demux_packet_t *pkt,
                                 VASurfaceID *out_surface)
{
    if (!dec || !dec->initialized || !pkt || !out_surface)
        return false;

    uint32_t pool_size = dec->num_surfaces > 0 ? dec->num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;

    /* Pick the lowest-index free surface not in DPB or held by reorder/compositor
     * so nvidia_drv_video's lazy direct_realiseSurface allocates VRAM only for the
     * active working set rather than touching all pool_size handles. */
    for (uint32_t idx = 0; idx < pool_size; ++idx) {
        VASurfaceID s = dec->surfaces[idx];
        bool in_dpb = false;
        for (size_t d = 0; d < dec->dpb.count; ++d) {
            if (dec->dpb.entries[d].surface == s) {
                in_dpb = true;
                break;
            }
        }
        for (int r = 0; r < 8 && !in_dpb; ++r) {
            if (dec->vp9_ref_frames[r] == s || dec->av1_ref_frames[r] == s) {
                in_dpb = true;
                break;
            }
        }
        bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[idx];
        if (!in_dpb && !compositor_busy) {
            dec->current_surface_idx = idx;
            break;
        }
    }

    VASurfaceID target = dec->surfaces[dec->current_surface_idx];
    bool picture_begun = false;
    bool is_vp9 =
        (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2);

    VAStatus st = VA_STATUS_SUCCESS;
    if (!is_vp9) {
        st = vaBeginPicture(dec->va_display, dec->va_context, target);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
            return false;
        }
        picture_begun = true;
    }

    VABufferID bufs[8];
    uint32_t num_bufs = 0;

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
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            uint8_t ntype = nal_ptr[0] & 0x1F;
            if (ntype == 7) {
                if (parse_h264_sps(nal_ptr, nal_len, &sps)) {
                    dec->cached_sps = sps;
                    iq_needs_refresh = true;
                }
            } else if (ntype == 8) {
                if (parse_h264_pps(nal_ptr, nal_len, &sps, &pps)) {
                    dec->cached_pps = pps;
                    iq_needs_refresh = true;
                }
            } else if (ntype >= 1 && ntype <= 5 && !found_slice) {
                found_slice = true;
                is_idr_pic = (ntype == 5);
                is_ref_pic = ((nal_ptr[0] >> 5) & 0x03) != 0;

                VASliceParameterBufferH264 dummy_sp;
                memset(&dummy_sp, 0, sizeof(dummy_sp));
                parse_h264_slice_header(nal_ptr, nal_len, &sps, &pps, &dummy_sp, &sh_meta);
                curr_frame_num = (int32_t)sh_meta.frame_num;

                if (is_idr_pic) {
                    dec->prev_poc_msb = 0;
                    dec->prev_poc_lsb = 0;
                    curr_poc = 0;
                    dec->dpb.count = 0;
                } else if (sps.valid && sps.pic_order_cnt_type == 0) {
                    int32_t max_poc_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
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
        pic_param.picture_width_in_mbs_minus1 =
            sps.valid ? (uint16_t)sps.pic_width_in_mbs_minus1 : (uint16_t)((dec->width + 15) / 16 - 1);
        pic_param.picture_height_in_mbs_minus1 =
            sps.valid ? (uint16_t)sps.pic_height_in_map_units_minus1 : (uint16_t)((dec->height + 15) / 16 - 1);
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

        VABufferID hdr_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
        uint32_t num_hdr_bufs = 0;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &hdr_bufs[0]);
        if (st == VA_STATUS_SUCCESS) {
            num_hdr_bufs++;
        }

        st = vaCreateBuffer(dec->va_display, dec->va_context, VAIQMatrixBufferType,
                            sizeof(dec->cached_iq), 1, &dec->cached_iq, &hdr_bufs[num_hdr_bufs]);
        if (st == VA_STATUS_SUCCESS) {
            num_hdr_bufs++;
        }

        if (num_hdr_bufs > 0) {
            vaRenderPicture(dec->va_display, dec->va_context, hdr_bufs, (int)num_hdr_bufs);
            for (uint32_t b = 0; b < num_hdr_bufs; ++b) {
                vaDestroyBuffer(dec->va_display, hdr_bufs[b]);
            }
        }

        /* Iterate and render all slice NAL units */
        scan_off = 0;
        uint32_t slice_count = 0;
        int32_t max_fn = 1 << ((sps.valid ? sps.log2_max_frame_num_minus4 : 4) + 4);
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            uint8_t ntype = nal_ptr[0] & 0x1F;
            if (ntype >= 1 && ntype <= 5) {
                VASliceParameterBufferH264 slice_param;
                memset(&slice_param, 0, sizeof(slice_param));
                slice_param.slice_data_size = nal_len;
                slice_param.slice_data_offset = 0;
                slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

                parse_h264_slice_header(nal_ptr, nal_len, &sps, &pps, &slice_param, NULL);
                build_h264_ref_lists(&dec->dpb, slice_param.slice_type, curr_frame_num, max_fn,
                                     curr_poc, &slice_param);

                VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
                VAStatus s1 = vaCreateBuffer(dec->va_display, dec->va_context,
                                             VASliceParameterBufferType, sizeof(slice_param), 1,
                                             &slice_param, &slice_bufs[0]);
                VAStatus s2 = vaCreateBuffer(dec->va_display, dec->va_context,
                                             VASliceDataBufferType, nal_len, 1, (void *)nal_ptr,
                                             &slice_bufs[1]);
                if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                    vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
                }
                if (slice_bufs[0] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[0]);
                if (slice_bufs[1] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[1]);
                slice_count++;
            }
        }

        /* Fallback if no slice NAL was identified */
        if (slice_count == 0 && pkt->size > 4) {
            VASliceParameterBufferH264 slice_param;
            memset(&slice_param, 0, sizeof(slice_param));
            slice_param.slice_data_size = (uint32_t)pkt->size - 4;
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice_param.slice_type = pkt->is_keyframe ? 2 : 0;
            build_h264_ref_lists(&dec->dpb, slice_param.slice_type, curr_frame_num, max_fn,
                                 curr_poc, &slice_param);
            VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
            VAStatus s1 = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                                         sizeof(slice_param), 1, &slice_param, &slice_bufs[0]);
            VAStatus s2 = vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                                         pkt->size - 4, 1, (void *)(pkt->data + 4), &slice_bufs[1]);
            if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
            }
            if (slice_bufs[0] != VA_INVALID_ID)
                vaDestroyBuffer(dec->va_display, slice_bufs[0]);
            if (slice_bufs[1] != VA_INVALID_ID)
                vaDestroyBuffer(dec->va_display, slice_bufs[1]);
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
            } else if ((ntype <= 9 || (ntype >= 16 && ntype <= 21)) && !found_slice) {
                found_slice = true;
                is_idr_pic = (ntype == 19 || ntype == 20);
                is_rap = (ntype >= 16 && ntype <= 21);
                VASliceParameterBufferHEVC dummy_sp;
                memset(&dummy_sp, 0, sizeof(dummy_sp));
                int32_t poc_lsb = 0;
                parse_hevc_slice_header(nal_ptr, nal_len, &sps, &pps, &dummy_sp, &poc_lsb,
                                        &st_rps_bits);
                is_intra = (dummy_sp.LongSliceFlags.fields.slice_type == 2);
                if (is_idr_pic) {
                    dec->prev_poc_msb = 0;
                    dec->prev_poc_lsb = 0;
                    curr_poc = 0;
                    dec->dpb.count = 0;
                } else {
                    int32_t max_poc_lsb =
                        1 << ((sps.valid ? sps.log2_max_pic_order_cnt_lsb_minus4 : 4) + 4);
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
                    if (ntype != 0 && ntype != 2 && ntype != 4) {
                        dec->prev_poc_msb = poc_msb;
                        dec->prev_poc_lsb = poc_lsb;
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
        for (int i = 0; i < 15; ++i) {
            if (i < (int)dec->dpb.count && i < 8) {
                pic_param.ReferenceFrames[i].picture_id = dec->dpb.entries[i].surface;
                pic_param.ReferenceFrames[i].pic_order_cnt = dec->dpb.entries[i].pic_order_cnt;
                pic_param.ReferenceFrames[i].flags =
                    (dec->dpb.entries[i].pic_order_cnt <= curr_poc)
                        ? VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE
                        : VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
            } else {
                pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
                pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
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

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS) {
            vaRenderPicture(dec->va_display, dec->va_context, &pic_buf, 1);
            vaDestroyBuffer(dec->va_display, pic_buf);
        }

        scan_off = 0;
        uint32_t slice_count = 0;
        while (next_h264_nal(pkt->data, pkt->size, dec->is_avcc, dec->nal_length_size, &scan_off,
                             &nal_ptr, &nal_len)) {
            if (nal_len < 2)
                continue;
            uint8_t ntype = (nal_ptr[0] >> 1) & 0x3F;
            if (ntype <= 9 || (ntype >= 16 && ntype <= 21)) {
                VASliceParameterBufferHEVC slice_param;
                memset(&slice_param, 0, sizeof(slice_param));
                slice_param.slice_data_size = nal_len;
                slice_param.slice_data_offset = 0;
                slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
                slice_param.LongSliceFlags.fields.LastSliceOfPic = 1;
                parse_hevc_slice_header(nal_ptr, nal_len, &sps, &pps, &slice_param, NULL, NULL);
                build_hevc_ref_lists(&dec->dpb,
                                     (uint8_t)slice_param.LongSliceFlags.fields.slice_type,
                                     curr_poc, &slice_param);

                VABufferID slice_bufs[2] = {VA_INVALID_ID, VA_INVALID_ID};
                VAStatus s1 = vaCreateBuffer(dec->va_display, dec->va_context,
                                             VASliceParameterBufferType, sizeof(slice_param), 1,
                                             &slice_param, &slice_bufs[0]);
                VAStatus s2 = vaCreateBuffer(dec->va_display, dec->va_context,
                                             VASliceDataBufferType, nal_len, 1, (void *)nal_ptr,
                                             &slice_bufs[1]);
                if (s1 == VA_STATUS_SUCCESS && s2 == VA_STATUS_SUCCESS) {
                    vaRenderPicture(dec->va_display, dec->va_context, slice_bufs, 2);
                }
                if (slice_bufs[0] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[0]);
                if (slice_bufs[1] != VA_INVALID_ID)
                    vaDestroyBuffer(dec->va_display, slice_bufs[1]);
                slice_count++;
            }
        }
        if (slice_count == 0 && pkt->size > 0) {
            VASliceParameterBufferHEVC slice_param;
            memset(&slice_param, 0, sizeof(slice_param));
            slice_param.slice_data_size = (uint32_t)pkt->size;
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice_param.LongSliceFlags.fields.slice_type = pkt->is_keyframe ? 2 : 1;
            slice_param.LongSliceFlags.fields.LastSliceOfPic = 1;
            build_hevc_ref_lists(&dec->dpb, (uint8_t)slice_param.LongSliceFlags.fields.slice_type,
                                 curr_poc, &slice_param);
            VABufferID slice_buf = VA_INVALID_ID;
            if (vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param,
                               &slice_buf) == VA_STATUS_SUCCESS) {
                bufs[num_bufs++] = slice_buf;
            }
        }
    } else if (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2) {
        const uint8_t *sub_ptrs[8];
        size_t sub_sizes[8];
        uint32_t num_subframes = vp9_split_superframe(pkt->data, pkt->size, sub_ptrs, sub_sizes);
        if (num_subframes == 0) {
            sub_ptrs[0] = pkt->data;
            sub_sizes[0] = pkt->size;
            num_subframes = 1;
        }

        for (uint32_t sf = 0; sf < num_subframes; ++sf) {
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
                }
                continue;
            }

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
                    break;
                }
            }

            if (sub_is_idr) {
                is_idr_pic = true;
            }
            for (int i = 0; i < 8; ++i) {
                if (refresh_flags & (1U << i)) {
                    dec->vp9_ref_frames[i] = target;
                }
            }

            st = vaBeginPicture(dec->va_display, dec->va_context, target);
            if (st != VA_STATUS_SUCCESS) {
                WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
                return false;
            }

            VABufferID sf_bufs[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
            uint32_t sf_num_bufs = 0;
            if (vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                               sizeof(pic_param), 1, &pic_param, &sf_bufs[sf_num_bufs]) ==
                VA_STATUS_SUCCESS) {
                sf_num_bufs++;
            }
            if (vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &sf_bufs[sf_num_bufs]) ==
                VA_STATUS_SUCCESS) {
                sf_num_bufs++;
            }
            if (sub_sizes[sf] > 0 && sub_ptrs[sf] &&
                vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                               (uint32_t)sub_sizes[sf], 1, (void *)sub_ptrs[sf],
                               &sf_bufs[sf_num_bufs]) == VA_STATUS_SUCCESS) {
                sf_num_bufs++;
            }

            if (sf_num_bufs > 0) {
                vaRenderPicture(dec->va_display, dec->va_context, sf_bufs, (int)sf_num_bufs);
                for (uint32_t b = 0; b < sf_num_bufs; ++b) {
                    vaDestroyBuffer(dec->va_display, sf_bufs[b]);
                }
            }
            vaEndPicture(dec->va_display, dec->va_context);
            vaSyncSurface(dec->va_display, target);
        }

        if (is_idr_pic) {
            dec->frame_num = 0;
            curr_frame_num = 0;
            curr_poc = 0;
        }
    } else if (dec->va_profile == VAProfileAV1Profile0) {
        VADecPictureParameterBufferAV1 pic_param;
        VASliceParameterBufferAV1 av1_slices[64];
        uint32_t av1_slice_count = 0;
        uint8_t tile_size_bytes_minus_1 = 0;
        bool av1_show_existing = false;
        uint8_t av1_show_existing_idx = 0;
        memset(&pic_param, 0, sizeof(pic_param));
        memset(av1_slices, 0, sizeof(av1_slices));
        pic_param.current_frame = target;
        pic_param.current_display_picture = target;
        pic_param.frame_width_minus1 = (uint16_t)(dec->width - 1);
        pic_param.frame_height_minus1 = (uint16_t)(dec->height - 1);
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
                uint64_t parsed_sz =
                    av1_read_leb128(pkt->data + off, pkt->size - off, &leb_bytes);
                off += leb_bytes;
                if (parsed_sz > pkt->size - off)
                    break;
                obu_sz = (size_t)parsed_sz;
            }
            if (obu_type == 1) {
                parse_av1_sequence_header(pkt->data + off, obu_sz, &dec->cached_av1_seq);
            } else if (obu_type == 3 || obu_type == 6) {
                parse_av1_frame_header(dec, pkt->data + off, obu_sz, obu_type, (uint32_t)off,
                                       &pic_param, av1_slices, 64, &av1_slice_count,
                                       &tile_size_bytes_minus_1, &refresh_flags, &is_idr_pic,
                                       &av1_show_existing, &av1_show_existing_idx);
            } else if (obu_type == 4 && obu_sz > 0 && av1_slice_count < 64) {
                av1_slice_count += parse_av1_tile_group_obu(
                    pkt->data + off, obu_sz, 0, (uint32_t)off, &pic_param, tile_size_bytes_minus_1,
                    av1_slices + av1_slice_count, 64 - av1_slice_count);
            }
            off += obu_sz;
        }

        if (av1_show_existing) {
            VASurfaceID ref_surf = dec->av1_ref_frames[av1_show_existing_idx & 7];
            if (ref_surf != VA_INVALID_SURFACE) {
                target = ref_surf;
            }
        } else {
            for (int i = 0; i < 8; ++i) {
                if (refresh_flags & (1U << i)) {
                    dec->av1_ref_frames[i] = target;
                    dec->av1_ref_order_hints[i] = pic_param.order_hint;
                }
            }
            if (av1_slice_count == 0) {
                av1_slices[0].slice_data_size = (uint32_t)pkt->size;
                av1_slices[0].slice_data_offset = 0;
                av1_slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
                av1_slice_count = 1;
            }

            VABufferID pic_buf = VA_INVALID_ID;
            st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                                sizeof(pic_param), 1, &pic_param, &pic_buf);
            if (st == VA_STATUS_SUCCESS)
                bufs[num_bufs++] = pic_buf;

            VABufferID slice_buf = VA_INVALID_ID;
            st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                                sizeof(VASliceParameterBufferAV1), av1_slice_count, av1_slices,
                                &slice_buf);
            if (st == VA_STATUS_SUCCESS)
                bufs[num_bufs++] = slice_buf;
        }

        if (is_idr_pic) {
            dec->frame_num = 0;
            curr_frame_num = 0;
            curr_poc = 0;
        }
    }

    /* Slice bitstream data buffer for non-H.264 codecs */
    if (num_bufs > 0) {
        if (pkt->size > 0 && pkt->data) {
            VABufferID data_buf = VA_INVALID_ID;
            st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceDataBufferType,
                                (uint32_t)pkt->size, 1, (void *)pkt->data, &data_buf);
            if (st == VA_STATUS_SUCCESS) {
                bufs[num_bufs++] = data_buf;
            }
        }

        st = vaRenderPicture(dec->va_display, dec->va_context, bufs, (int)num_bufs);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_WARN("vaRenderPicture returned: %s", vaErrorStr(st));
        }

        for (uint32_t i = 0; i < num_bufs; ++i) {
            vaDestroyBuffer(dec->va_display, bufs[i]);
        }
    }

    if (picture_begun) {
        st = vaEndPicture(dec->va_display, dec->va_context);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_WARN("vaEndPicture returned: %s", vaErrorStr(st));
        }

        st = vaSyncSurface(dec->va_display, target);
        if (st != VA_STATUS_SUCCESS) {
            WAYWAL_LOG_WARN("vaSyncSurface returned: %s", vaErrorStr(st));
        }
    }

    /* Update DPB state machine with MMCO and FIFO sliding-window eviction */
    uint32_t max_refs = 8;
    if (dec->cached_sps.valid && dec->cached_sps.num_ref_frames > 0) {
        max_refs = dec->cached_sps.num_ref_frames;
    } else if (dec->cached_hevc_sps.valid) {
        max_refs = (uint32_t)dec->cached_hevc_sps.sps_max_dec_pic_buffering_minus1 + 1;
        if (max_refs > 8)
            max_refs = 8;
    }
    if (max_refs > 16) {
        max_refs = 16;
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
                if (!dec->dpb.entries[d].is_long_term && dec->dpb.entries[d].frame_num == target_pn) {
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
    dec->last_decoded_poc = curr_poc;
    dec->last_decoded_is_idr = is_idr_pic;
    dec->frame_num++;

    *out_surface = target;
    dec->current_surface_idx = (dec->current_surface_idx + 1) % pool_size;
    return true;
}

bool vaapi_decoder_vpp_blit(vaapi_decoder_t *dec, VASurfaceID src_surface,
                            VASurfaceID dst_vpp_surface)
{
    if (!dec || !dec->initialized || !dec->has_vpp || src_surface == VA_INVALID_SURFACE ||
        dst_vpp_surface == VA_INVALID_SURFACE) {
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
    VAProcColorStandardType color_std = VAProcColorStandardNone;
    if (dec->cached_sps.valid) {
        switch (dec->cached_sps.matrix_coefficients) {
        case 1:
            color_std = VAProcColorStandardBT709;
            break;
        case 5:
        case 6:
            color_std = VAProcColorStandardBT601;
            break;
        case 9:
        case 10:
            color_std = VAProcColorStandardBT2020;
            break;
        default:
            color_std = VAProcColorStandardNone;
            break;
        }
        if (dec->cached_sps.video_full_range_flag) {
            vpp_param.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        }
    } else if (dec->cached_av1_seq.valid) {
        switch (dec->cached_av1_seq.matrix_coefficients) {
        case 1:
            color_std = VAProcColorStandardBT709;
            break;
        case 5:
        case 6:
            color_std = VAProcColorStandardBT601;
            break;
        case 9:
        case 10:
            color_std = VAProcColorStandardBT2020;
            break;
        default:
            color_std = VAProcColorStandardNone;
            break;
        }
        if (dec->cached_av1_seq.color_range) {
            vpp_param.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        }
    } else if (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2) {
        switch (dec->vp9_color_space) {
        case 1:
        case 3:
        case 4:
            color_std = VAProcColorStandardBT601;
            break;
        case 2:
            color_std = VAProcColorStandardBT709;
            break;
        case 5:
            color_std = VAProcColorStandardBT2020;
            break;
        default:
            color_std = VAProcColorStandardNone;
            break;
        }
        if (dec->vp9_color_range) {
            vpp_param.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        }
    }
    vpp_param.surface_color_standard = color_std;
    vpp_param.output_color_standard = VAProcColorStandardNone;

    VABufferID vpp_buf = VA_INVALID_ID;
    VAStatus vpp_st = vaCreateBuffer(dec->va_display, dec->vpp_context,
                                     VAProcPipelineParameterBufferType, sizeof(vpp_param), 1,
                                     &vpp_param, &vpp_buf);
    if (vpp_st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("VPP vaCreateBuffer failed: %s", vaErrorStr(vpp_st));
        return false;
    }

    bool ok = false;
    vpp_st = vaBeginPicture(dec->va_display, dec->vpp_context, dst_vpp_surface);
    if (vpp_st == VA_STATUS_SUCCESS) {
        vaRenderPicture(dec->va_display, dec->vpp_context, &vpp_buf, 1);
        vaEndPicture(dec->va_display, dec->vpp_context);
        vaSyncSurface(dec->va_display, dst_vpp_surface);
        ok = true;
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
        malloc_trim(0);
    }
    return ok;
}

bool vaapi_decoder_export_prime(vaapi_decoder_t *dec, VASurfaceID surface,
                                vaapi_prime_frame_t *out_frame)
{
    if (!dec || !dec->initialized || surface == VA_INVALID_SURFACE || !out_frame) {
        return false;
    }
    memset(out_frame, 0, sizeof(*out_frame));
    for (int i = 0; i < 4; ++i) out_frame->fds[i] = -1;

    VADRMPRIMESurfaceDescriptor prime_desc;
    memset(&prime_desc, 0, sizeof(prime_desc));

    VAStatus st;
    if (!dec->has_vpp && !dec->is_nvidia_vaapi) {
        /* Intel iHD/i965 and AMD radeonsi export a single multi-planar NV12/P010 descriptor
         * via COMPOSED_LAYERS that Wayland compositors scan out directly on KMS YUV planes. */
        st = vaExportSurfaceHandle(
            dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime_desc);
        if (st != VA_STATUS_SUCCESS) {
            st = vaExportSurfaceHandle(
                dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime_desc);
        }
    } else {
        /* nvidia_drv_video.so only supports VA_EXPORT_SURFACE_SEPARATE_LAYERS for NV12/P010 */
        st = vaExportSurfaceHandle(
            dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime_desc);
        if (st != VA_STATUS_SUCCESS) {
            st = vaExportSurfaceHandle(
                dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime_desc);
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
            dec->has_vpp ? DRM_FORMAT_XRGB8888
                         : (dec->fourcc == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12);
    }

    uint32_t plane_idx = 0;
    bool obj_used[256] = {false};
    for (uint32_t l = 0; l < prime_desc.num_layers && plane_idx < 4; ++l) {
        for (uint32_t p = 0; p < prime_desc.layers[l].num_planes && plane_idx < 4; ++p) {
            uint32_t obj_idx = prime_desc.layers[l].object_index[p];
            if (obj_idx < prime_desc.num_objects) {
                out_frame->fds[plane_idx] = prime_desc.objects[obj_idx].fd;
                out_frame->modifiers[plane_idx] = prime_desc.objects[obj_idx].drm_format_modifier;
                out_frame->offsets[plane_idx] = prime_desc.layers[l].offset[p];
                out_frame->strides[plane_idx] = prime_desc.layers[l].pitch[p];
                if (obj_idx < 256) obj_used[obj_idx] = true;
                plane_idx++;
            }
        }
    }

    for (uint32_t i = 0; i < prime_desc.num_objects; ++i) {
        if ((i >= 256 || !obj_used[i]) && prime_desc.objects[i].fd >= 0) {
            close(prime_desc.objects[i].fd);
        }
    }

    out_frame->num_planes = plane_idx > 0 ? plane_idx : prime_desc.num_objects;
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

static bool vaapi_ops_export_prime(void *ctx, uint32_t surface, vaapi_prime_frame_t *out_frame)
{
    return vaapi_decoder_export_prime((vaapi_decoder_t *)ctx, (VASurfaceID)surface, out_frame);
}

static void vaapi_ops_reset(void *ctx)
{
    vaapi_decoder_reset((vaapi_decoder_t *)ctx);
}

static void vaapi_ops_destroy(void *ctx)
{
    vaapi_decoder_destroy((vaapi_decoder_t *)ctx);
}

const waywal_video_decoder_ops_t g_waywal_vaapi_decoder_ops = {
    .backend_name = "VA-API",
    .backend_type = WAYWAL_VIDEO_BACKEND_VAAPI,
    .init = vaapi_ops_init,
    .decode_packet = vaapi_ops_decode_packet,
    .export_prime = vaapi_ops_export_prime,
    .release_frame = vaapi_prime_frame_close,
    .reset = vaapi_ops_reset,
    .destroy = vaapi_ops_destroy,
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
