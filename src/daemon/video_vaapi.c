#include "waywal/log.h"
#include "waywal/vaapi_dec.h"

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
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#ifdef VAProfileAV1Profile0
#include <va/va_dec_av1.h>
#endif

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
    if (codec == WAYWAL_CODEC_HEVC && extradata && extradata_size >= 17) {
        uint8_t general_profile_idc = extradata[1] & 0x1F;
        uint8_t bit_depth_luma_minus8 = extradata[16] & 0x03;
        if (general_profile_idc == 2 || bit_depth_luma_minus8 == 2) {
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
#ifdef VAProfileAV1Profile0
        if (profile_supported(profiles, num_profiles, VAProfileAV1Profile0))
            chosen = VAProfileAV1Profile0;
#endif
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
    memset(dec, 0, sizeof(*dec));
    dec->width = width;
    dec->height = height;
    dec->fourcc = VA_FOURCC_NV12;
    dec->nal_length_size = 4;
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
        found = true;
        WAYWAL_LOG_INFO("VA-API v%d.%d initialized on DRM node %s (Profile: %d, Driver: %s)", major,
                        minor, candidate_nodes[i] ? candidate_nodes[i] : "supplied fd",
                        dec->va_profile, vaQueryVendorString(dec->va_display));
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
    }
    dec->num_surfaces = max_refs + 3;
    if (dec->num_surfaces < 4)
        dec->num_surfaces = 4;
    if (dec->num_surfaces > WAYWAL_VA_SURFACE_POOL_SIZE)
        dec->num_surfaces = WAYWAL_VA_SURFACE_POOL_SIZE;

    bool use_10bit = (dec->va_profile == VAProfileHEVCMain10 ||
                      dec->va_profile == VAProfileVP9Profile2) &&
                     allow_direct_nv12;
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

/* Bitstream reader and Exp-Golomb parser for H.264 SPS/PPS (BUG-06) */
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit_pos;
    size_t end_bit_pos;
} bit_reader_t;

static inline void bit_reader_init(bit_reader_t *br, const uint8_t *data, size_t size)
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

    /* Calculate raw bitstream bit offset: */
    sp->slice_data_bit_offset = (uint32_t)(8 + br.bit_pos);
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

bool vaapi_decoder_set_extradata(vaapi_decoder_t *dec, const uint8_t *extradata, size_t size)
{
    if (!dec || !extradata || size < 7)
        return false;

    if (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
        dec->va_profile == VAProfileH264ConstrainedBaseline) {
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
        bool compositor_busy = dec->surface_busy_mask && dec->surface_busy_mask[idx];
        if (!in_dpb && !compositor_busy) {
            dec->current_surface_idx = idx;
            break;
        }
    }

    VASurfaceID target = dec->surfaces[dec->current_surface_idx];

    VAStatus st = vaBeginPicture(dec->va_display, dec->va_context, target);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaBeginPicture failed: %s", vaErrorStr(st));
        return false;
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
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = pic_buf;

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
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = slice_buf;
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
            pic_param.reference_frames[i] =
                (i < (int)dec->dpb.count) ? dec->dpb.entries[i].surface : VA_INVALID_SURFACE;
        }

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = pic_buf;

        VASliceParameterBufferVP9 slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = 0;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = slice_buf;
#ifdef VAProfileAV1Profile0
    } else if (dec->va_profile == VAProfileAV1Profile0) {
        /* AV1 Parameter Buffer Builder (BUG-06) */
        VADecPictureParameterBufferAV1 pic_param;
        memset(&pic_param, 0, sizeof(pic_param));
        pic_param.current_frame = target;
        pic_param.frame_width_minus_1 = (uint16_t)(dec->width - 1);
        pic_param.frame_height_minus_1 = (uint16_t)(dec->height - 1);
        for (int i = 0; i < 8; ++i) {
            pic_param.ref_frame_map[i] =
                (i < (int)dec->dpb.count) ? dec->dpb.entries[i].surface : VA_INVALID_SURFACE;
        }

        VABufferID pic_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VAPictureParameterBufferType,
                            sizeof(pic_param), 1, &pic_param, &pic_buf);
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = pic_buf;

        VASliceParameterBufferAV1 slice_param;
        memset(&slice_param, 0, sizeof(slice_param));
        slice_param.slice_data_size = (uint32_t)pkt->size;
        slice_param.slice_data_offset = 0;
        slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

        VABufferID slice_buf = VA_INVALID_ID;
        st = vaCreateBuffer(dec->va_display, dec->va_context, VASliceParameterBufferType,
                            sizeof(slice_param), 1, &slice_param, &slice_buf);
        if (st == VA_STATUS_SUCCESS)
            bufs[num_bufs++] = slice_buf;
#endif
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

    st = vaEndPicture(dec->va_display, dec->va_context);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("vaEndPicture returned: %s", vaErrorStr(st));
    }

    st = vaSyncSurface(dec->va_display, target);
    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_WARN("vaSyncSurface returned: %s", vaErrorStr(st));
    }

    /* Update DPB state machine with MMCO and FIFO sliding-window eviction */
    uint32_t max_refs = (dec->cached_sps.valid && dec->cached_sps.num_ref_frames > 0)
                            ? dec->cached_sps.num_ref_frames
                            : 16;
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

    /* nvidia_drv_video.so only supports VA_EXPORT_SURFACE_SEPARATE_LAYERS for NV12/P010 */
    VAStatus st = vaExportSurfaceHandle(
        dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime_desc);

    if (st != VA_STATUS_SUCCESS) {
        st = vaExportSurfaceHandle(dec->va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &prime_desc);
    }

    if (st != VA_STATUS_SUCCESS) {
        WAYWAL_LOG_ERR("vaExportSurfaceHandle failed for surface %u: %s", surface, vaErrorStr(st));
        return false;
    }

    out_frame->surface_id = surface;
    out_frame->width = prime_desc.width;
    out_frame->height = prime_desc.height;
    if (prime_desc.fourcc == VA_FOURCC_NV12) {
        out_frame->drm_format = DRM_FORMAT_NV12;
    } else if (prime_desc.fourcc == VA_FOURCC_P010) {
        out_frame->drm_format = DRM_FORMAT_P010;
    } else if (prime_desc.num_layers > 0 && prime_desc.layers[0].drm_format != 0) {
        out_frame->drm_format = prime_desc.layers[0].drm_format;
    } else {
        out_frame->drm_format = DRM_FORMAT_NV12;
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
