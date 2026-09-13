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

#define WAYWAL_VA_SURFACE_POOL_SIZE 4

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
    VADisplay va_display;
    int drm_fd;
    bool owns_drm_fd;
    VAConfigID va_config;
    VAContextID va_context;
    VAProfile va_profile;
    VASurfaceID surfaces[WAYWAL_VA_SURFACE_POOL_SIZE];
    uint32_t width;
    uint32_t height;
    uint32_t fourcc; /* VA_FOURCC_NV12 or VA_FOURCC_P010 */
    size_t current_surface_idx;
    uint32_t frame_num;
    h264_dpb_t dpb;
    bool initialized;
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

/* Destroy VA-API decoder and release all surfaces and context */
void vaapi_decoder_destroy(vaapi_decoder_t *dec);

/* Submits elementary stream packet to hardware ASIC */
bool vaapi_decoder_decode_packet(vaapi_decoder_t *dec, const demux_packet_t *pkt,
                                 VASurfaceID *out_surface);

/* Exports decoded hardware surface directly to PRIME 2 DMA-BUF */
bool vaapi_decoder_export_prime(vaapi_decoder_t *dec, VASurfaceID surface,
                                vaapi_prime_frame_t *out_frame);

/* Releases exported PRIME file descriptors */
void vaapi_prime_frame_close(vaapi_prime_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_VAAPI_DEC_H */
