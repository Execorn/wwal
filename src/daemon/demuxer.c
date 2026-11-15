// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/demuxer.h"

#include "waywal/log.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#if defined(__has_include) && __has_include(<stdckdint.h>)
#include <stdckdint.h>
#else
#define ckd_add(r, a, b) __builtin_add_overflow((a), (b), (r))
#define ckd_sub(r, a, b) __builtin_sub_overflow((a), (b), (r))
#define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FOURCC(a, b, c, d)                                                                         \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |                             \
     ((uint32_t)(uint8_t)(c) << 8) | ((uint32_t)(uint8_t)(d)))

/* Bound timestamps so downstream ns scaling (pts_us * 1000LL) cannot overflow int64_t. */
#define WAYWAL_DEMUXER_MAX_PTS_US (INT64_MAX / 1000LL)
#define WAYWAL_DEMUXER_MIN_PTS_US (INT64_MIN / 1000LL)

typedef struct {
    size_t offset;
    size_t size;
    int64_t pts_us;
    int64_t dts_us;
    bool is_keyframe;
} demuxer_sample_entry_t;

typedef struct {
    /* Must stay first in this exact order because video_engine_load_mem casts internal_state. */
    int file_fd;
    bool owns_mmap;

    demuxer_sample_entry_t *samples;
    size_t num_samples;
    size_t current_sample_idx;
    size_t last_evicted_offset;
    size_t *keyframe_indices;
    size_t num_keyframes;
} demuxer_state_t;

_Static_assert(offsetof(demuxer_state_t, file_fd) == 0u,
               "file_fd must stay at offset 0 for video_engine_load_mem");
_Static_assert(offsetof(demuxer_state_t, owns_mmap) == sizeof(int),
               "owns_mmap must stay immediately after file_fd for video_engine_load_mem");

static inline uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static inline uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static inline uint64_t read_u64_be(const uint8_t *p)
{
    return ((uint64_t)read_u32_be(p) << 32) | (uint64_t)read_u32_be(p + 4);
}

static void evict_mmap_range_preserving_extradata(const demuxer_t *d, uintptr_t p_start,
                                                  uintptr_t p_end)
{
    if (p_end <= p_start)
        return;
    if (d->extradata && d->extradata_size > 0) {
        uintptr_t ex_start = ((uintptr_t)d->extradata) & ~(uintptr_t)4095U;
        uintptr_t ex_end =
            ((uintptr_t)d->extradata + d->extradata_size + 4095U) & ~(uintptr_t)4095U;
        if (ex_start < p_end && ex_end > p_start) {
            if (ex_start > p_start) {
                madvise((void *)p_start, (size_t)(ex_start - p_start), MADV_DONTNEED);
            }
            p_start = ex_end;
        }
    }
    if (p_end > p_start) {
        madvise((void *)p_start, (size_t)(p_end - p_start), MADV_DONTNEED);
    }
}

static void evict_mmap_chunk(const demuxer_t *d, size_t *last_evicted_offset, size_t consumed_end)
{
    if (!d || !d->mapped_buf || !last_evicted_offset)
        return;
    if (consumed_end < *last_evicted_offset) {
        *last_evicted_offset = consumed_end & ~(size_t)4095U;
        return;
    }
    if (consumed_end - *last_evicted_offset < (256U * 1024U))
        return;

    uintptr_t base = (uintptr_t)d->mapped_buf;
    uintptr_t p_start = (base + *last_evicted_offset) & ~(uintptr_t)4095U;
    uintptr_t p_end = (base + consumed_end) & ~(uintptr_t)4095U;
    evict_mmap_range_preserving_extradata(d, p_start, p_end);
    *last_evicted_offset = consumed_end & ~(size_t)4095U;
}

static void evict_mmap_for_seek(const demuxer_t *d, size_t keep_offset)
{
    if (!d || !d->mapped_buf || d->mapped_size < 4096)
        return;
    uintptr_t raw_base = (uintptr_t)d->mapped_buf;
    uintptr_t map_base = (raw_base + 4095U) & ~(uintptr_t)4095U;
    uintptr_t map_end = (raw_base + d->mapped_size) & ~(uintptr_t)4095U;
    if (map_end <= map_base)
        return;

    size_t clamped_off = keep_offset <= d->mapped_size ? keep_offset : d->mapped_size;
    size_t window_len = (2U << 20);
    size_t keep_end_off =
        (d->mapped_size > window_len && clamped_off <= d->mapped_size - window_len)
            ? (clamped_off + window_len)
            : d->mapped_size;

    uintptr_t keep_start = (raw_base + clamped_off) & ~(uintptr_t)4095U;
    uintptr_t keep_end = (raw_base + keep_end_off + 4095U) & ~(uintptr_t)4095U;
    if (keep_start < map_base)
        keep_start = map_base;
    if (keep_end > map_end)
        keep_end = map_end;

    evict_mmap_range_preserving_extradata(d, map_base, keep_start);
    evict_mmap_range_preserving_extradata(d, keep_end, map_end);
}

static bool demuxer_read_packet(demuxer_t *d, demux_packet_t *pkt)
{
    if (!d || !d->internal_state || !pkt)
        return false;
    demuxer_state_t *st = (demuxer_state_t *)d->internal_state;

    if (st->current_sample_idx >= st->num_samples) {
        return false; /* End of stream */
    }

    if (st->owns_mmap && st->current_sample_idx > 0) {
        const demuxer_sample_entry_t *prev = &st->samples[st->current_sample_idx - 1];
        if (prev->offset <= d->mapped_size && prev->size <= d->mapped_size - prev->offset) {
            evict_mmap_chunk(d, &st->last_evicted_offset, prev->offset + prev->size);
        }
    }

    const demuxer_sample_entry_t *s = &st->samples[st->current_sample_idx++];
    if (s->offset > d->mapped_size || s->size > d->mapped_size - s->offset) {
        return false;
    }

    pkt->data = d->mapped_buf + s->offset;
    pkt->size = s->size;
    pkt->pts_us = s->pts_us;
    pkt->dts_us = s->dts_us;
    pkt->is_keyframe = s->is_keyframe;
    return true;
}

static bool demuxer_seek_us(demuxer_t *d, int64_t target_pts_us)
{
    if (!d || !d->internal_state)
        return false;
    demuxer_state_t *st = (demuxer_state_t *)d->internal_state;
    if (st->num_samples == 0)
        return false;

    size_t best_idx = 0;
    if (target_pts_us > 0) {
        if (st->num_keyframes > 0 && st->keyframe_indices) {
            size_t left = 0;
            size_t right = st->num_keyframes;
            while (left < right) {
                size_t mid = left + (right - left) / 2;
                size_t s_idx = st->keyframe_indices[mid];
                if (st->samples[s_idx].pts_us <= target_pts_us) {
                    best_idx = s_idx;
                    left = mid + 1;
                } else {
                    right = mid;
                }
            }
        } else {
            /* Fallback linear search */
            for (size_t i = 0; i < st->num_samples; ++i) {
                if (st->samples[i].is_keyframe) {
                    if (st->samples[i].pts_us > target_pts_us) {
                        break;
                    }
                    best_idx = i;
                }
            }
        }
    }

    if (st->owns_mmap) {
        evict_mmap_for_seek(d, st->samples[best_idx].offset);
        if (d->mapped_buf && d->mapped_size > 0 && st->samples[best_idx].offset < d->mapped_size) {
            size_t off = st->samples[best_idx].offset;
            uintptr_t base = (uintptr_t)d->mapped_buf;
            uintptr_t page_start = (base + off) & ~(uintptr_t)4095U;
            size_t avail = d->mapped_size - off;
            size_t prefetch_len = avail < (2U << 20) ? avail : (2U << 20);
            if (prefetch_len > 0) {
                madvise((void *)page_start, prefetch_len, MADV_WILLNEED);
            }
        }
    }

    st->current_sample_idx = best_idx;
    st->last_evicted_offset =
        (target_pts_us <= 0) ? 0 : (st->samples[best_idx].offset & ~(size_t)4095U);
    return true;
}

static void demuxer_destroy(demuxer_t *d)
{
    if (!d)
        return;
    if (d->internal_state) {
        demuxer_state_t *st = (demuxer_state_t *)d->internal_state;
        free(st->samples);
        free(st->keyframe_indices);
        if (st->owns_mmap && d->mapped_buf && d->mapped_size > 0) {
            munmap((void *)d->mapped_buf, d->mapped_size);
        }
        if (st->file_fd >= 0) {
            close(st->file_fd);
        }
        free(st);
        d->internal_state = NULL;
    }
    free(d);
}

static void demuxer_build_keyframe_index(demuxer_state_t *st)
{
    size_t kf_count = 0;
    for (size_t i = 0; i < st->num_samples; ++i) {
        if (st->samples[i].is_keyframe)
            kf_count++;
    }
    if (kf_count == 0)
        return;

    size_t *kf_indices = (size_t *)malloc(kf_count * sizeof(size_t));
    if (!kf_indices)
        return;

    size_t ki = 0;
    for (size_t i = 0; i < st->num_samples; ++i) {
        if (st->samples[i].is_keyframe)
            kf_indices[ki++] = i;
    }
    st->keyframe_indices = kf_indices;
    st->num_keyframes = kf_count;
}

static uint32_t demuxer_compute_fps_num(size_t frame_count, int64_t duration_us)
{
    if (frame_count == 0 || duration_us <= 0) {
        return 30u;
    }
    uint64_t fc = (uint64_t)frame_count;
    uint64_t dur = (uint64_t)duration_us;
    uint64_t scaled = 0;
    uint64_t rounded = 0;
    uint64_t fps = 0;
    if (!ckd_mul(&scaled, fc, 1000000ULL) && !ckd_add(&rounded, scaled, dur / 2u)) {
        fps = rounded / dur;
    } else {
        uint64_t q = fc / dur;
        if (q > (uint64_t)UINT32_MAX / 1000000ULL + 1ULL) {
            return UINT32_MAX;
        }
        uint64_t r = fc % dur;
        uint64_t rem_scaled = 0;
        uint64_t rem_rounded = 0;
        if (!ckd_mul(&rem_scaled, r, 1000000ULL) && !ckd_add(&rem_rounded, rem_scaled, dur / 2u)) {
            fps = q * 1000000ULL + rem_rounded / dur;
        } else {
            uint64_t denom = dur / 1000000ULL;
            fps = q * 1000000ULL + (denom > 0u ? r / denom : (uint64_t)UINT32_MAX);
        }
    }
    if (fps == 0u) {
        return 30u;
    }
    if (fps > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)fps;
}

static demuxer_t *demuxer_alloc_instance(const uint8_t *data, size_t size,
                                         demuxer_sample_entry_t *samples, size_t num_samples,
                                         waywal_codec_t codec, uint32_t width, uint32_t height,
                                         uint32_t fps_num, int64_t duration_us,
                                         const uint8_t *extradata, size_t extradata_size)
{
    demuxer_t *d = (demuxer_t *)calloc(1, sizeof(demuxer_t));
    if (!d)
        return NULL;

    demuxer_state_t *st = (demuxer_state_t *)calloc(1, sizeof(demuxer_state_t));
    if (!st) {
        free(d);
        return NULL;
    }

    st->file_fd = -1;
    st->samples = samples;
    st->num_samples = num_samples;
    st->current_sample_idx = 0;
    demuxer_build_keyframe_index(st);

    d->mapped_buf = data;
    d->mapped_size = size;
    d->codec = codec;
    d->width = width;
    d->height = height;
    d->fps_num = (fps_num > 0u) ? fps_num : 30u;
    d->fps_den = 1u;
    d->duration_us = duration_us;
    d->extradata = extradata;
    d->extradata_size = extradata_size;
    d->internal_state = st;

    d->read_packet = demuxer_read_packet;
    d->seek_us = demuxer_seek_us;
    d->destroy = demuxer_destroy;
    return d;
}

/* =========================================================================
 * ISO BMFF / MP4 Demuxer Implementation
 * ========================================================================= */

typedef struct {
    const uint8_t *stsz_data;
    size_t stsz_size;
    const uint8_t *stco_data;
    size_t stco_size;
    const uint8_t *co64_data;
    size_t co64_size;
    const uint8_t *stsc_data;
    size_t stsc_size;
    const uint8_t *stts_data;
    size_t stts_size;
    const uint8_t *ctts_data;
    size_t ctts_size;
    const uint8_t *stss_data;
    size_t stss_size;
    uint32_t timescale;
    uint32_t width;
    uint32_t height;
    waywal_codec_t codec;
    const uint8_t *extradata;
    size_t extradata_size;
} mp4_parser_temp_t;

typedef struct {
    uint32_t type;
    const uint8_t *payload;
    size_t payload_len;
    size_t next_offset;
} mp4_box_t;

static bool mp4_next_box(const uint8_t *buf, size_t size, size_t offset, mp4_box_t *out)
{
    if (!buf || !out || offset > size || size - offset < 8u) {
        return false;
    }

    uint64_t box_size = read_u32_be(buf + offset);
    uint32_t box_type = read_u32_be(buf + offset + 4u);
    size_t hdr_len = 8u;

    if (box_size == 1u) {
        if (size - offset < 16u) {
            return false;
        }
        box_size = read_u64_be(buf + offset + 8u);
        hdr_len = 16u;
    } else if (box_size == 0u) {
        box_size = (uint64_t)(size - offset);
    }

    if (!wwal_box_bounds_valid(offset, hdr_len, box_size, size)) {
        return false;
    }

    out->type = box_type;
    out->payload = buf + offset + hdr_len;
    out->payload_len = (size_t)(box_size - hdr_len);
    out->next_offset = offset + (size_t)box_size;
    return true;
}

static void parse_stbl(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt)
{
    size_t offset = 0;
    mp4_box_t box;
    while (mp4_next_box(buf, size, offset, &box)) {
        switch (box.type) {
        case FOURCC('s', 't', 's', 'd'): {
            if (box.payload_len >= 8u) {
                uint32_t entry_count = read_u32_be(box.payload + 4u);
                size_t entry_off = 8u;
                for (uint32_t i = 0; i < entry_count && entry_off <= box.payload_len &&
                                     box.payload_len - entry_off >= 8u;
                     ++i) {
                    uint32_t e_size = read_u32_be(box.payload + entry_off);
                    uint32_t e_type = read_u32_be(box.payload + entry_off + 4u);
                    if (!wwal_box_bounds_valid(entry_off, 8u, e_size, box.payload_len)) {
                        break;
                    }

                    if (e_type == FOURCC('a', 'v', 'c', '1') ||
                        e_type == FOURCC('a', 'v', 'c', '3')) {
                        pt->codec = WAYWAL_CODEC_H264;
                    } else if (e_type == FOURCC('h', 'v', 'c', '1') ||
                               e_type == FOURCC('h', 'e', 'v', '1')) {
                        pt->codec = WAYWAL_CODEC_HEVC;
                    } else if (e_type == FOURCC('v', 'p', '0', '9')) {
                        pt->codec = WAYWAL_CODEC_VP9;
                    } else if (e_type == FOURCC('a', 'v', '0', '1')) {
                        pt->codec = WAYWAL_CODEC_AV1;
                    }

                    if (e_size >= 36u) {
                        pt->width = read_u16_be(box.payload + entry_off + 32u);
                        pt->height = read_u16_be(box.payload + entry_off + 34u);

                        /* Search for avcC / hvcC sub-boxes (VisualSampleEntry fixed header is 86
                         * bytes) */
                        size_t entry_end = entry_off + e_size;
                        size_t sub_off = entry_off + (e_size >= 86u ? 86u : 36u);
                        mp4_box_t sub_box;
                        while (mp4_next_box(box.payload, entry_end, sub_off, &sub_box)) {
                            if (sub_box.type == FOURCC('a', 'v', 'c', 'C') ||
                                sub_box.type == FOURCC('h', 'v', 'c', 'C') ||
                                sub_box.type == FOURCC('v', 'p', 'c', 'C') ||
                                sub_box.type == FOURCC('a', 'v', '1', 'C')) {
                                pt->extradata = sub_box.payload;
                                pt->extradata_size = sub_box.payload_len;
                                break;
                            }
                            sub_off = sub_box.next_offset;
                        }
                    }
                    entry_off += e_size;
                }
            }
            break;
        }
        case FOURCC('s', 't', 's', 'z'):
            pt->stsz_data = box.payload;
            pt->stsz_size = box.payload_len;
            break;
        case FOURCC('s', 't', 'c', 'o'):
            pt->stco_data = box.payload;
            pt->stco_size = box.payload_len;
            break;
        case FOURCC('c', 'o', '6', '4'):
            pt->co64_data = box.payload;
            pt->co64_size = box.payload_len;
            break;
        case FOURCC('s', 't', 's', 'c'):
            pt->stsc_data = box.payload;
            pt->stsc_size = box.payload_len;
            break;
        case FOURCC('s', 't', 't', 's'):
            pt->stts_data = box.payload;
            pt->stts_size = box.payload_len;
            break;
        case FOURCC('c', 't', 't', 's'):
            pt->ctts_data = box.payload;
            pt->ctts_size = box.payload_len;
            break;
        case FOURCC('s', 't', 's', 's'):
            pt->stss_data = box.payload;
            pt->stss_size = box.payload_len;
            break;
        default:
            break;
        }

        offset = box.next_offset;
    }
}

static void parse_minf(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt)
{
    size_t offset = 0;
    mp4_box_t box;
    while (mp4_next_box(buf, size, offset, &box)) {
        if (box.type == FOURCC('s', 't', 'b', 'l')) {
            parse_stbl(box.payload, box.payload_len, pt);
            break;
        }
        offset = box.next_offset;
    }
}

static void parse_mdia(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt)
{
    size_t offset = 0;
    bool is_video = false;
    uint32_t mdia_timescale = 0;
    const uint8_t *minf_data = NULL;
    size_t minf_len = 0;
    mp4_box_t box;

    while (mp4_next_box(buf, size, offset, &box)) {
        if (box.type == FOURCC('h', 'd', 'l', 'r')) {
            if (box.payload_len >= 12u) {
                uint32_t hdlr_type = read_u32_be(box.payload + 8u);
                if (hdlr_type == FOURCC('v', 'i', 'd', 'e')) {
                    is_video = true;
                }
            }
        } else if (box.type == FOURCC('m', 'd', 'h', 'd')) {
            if (box.payload_len >= 16u) {
                uint8_t ver = box.payload[0];
                if (ver == 0 && box.payload_len >= 20u) {
                    mdia_timescale = read_u32_be(box.payload + 12u);
                } else if (ver == 1 && box.payload_len >= 28u) {
                    mdia_timescale = read_u32_be(box.payload + 20u);
                }
            }
        } else if (box.type == FOURCC('m', 'i', 'n', 'f')) {
            minf_data = box.payload;
            minf_len = box.payload_len;
        }

        offset = box.next_offset;
    }

    if (is_video) {
        if (mdia_timescale > 0) {
            pt->timescale = mdia_timescale;
        }
        if (minf_data) {
            parse_minf(minf_data, minf_len, pt);
        }
    }
}

static void parse_trak(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt)
{
    size_t offset = 0;
    mp4_box_t box;
    while (mp4_next_box(buf, size, offset, &box)) {
        if (box.type == FOURCC('m', 'd', 'i', 'a')) {
            parse_mdia(box.payload, box.payload_len, pt);
            if (pt->codec != WAYWAL_CODEC_UNKNOWN) {
                break; /* Successfully found video track */
            }
        }
        offset = box.next_offset;
    }
}

static void parse_moov(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt)
{
    size_t offset = 0;
    mp4_box_t box;
    while (mp4_next_box(buf, size, offset, &box)) {
        if (box.type == FOURCC('t', 'r', 'a', 'k')) {
            memset(pt, 0, sizeof(*pt));
            pt->timescale = 1000u;
            parse_trak(box.payload, box.payload_len, pt);
            if (pt->codec != WAYWAL_CODEC_UNKNOWN) {
                break;
            }
        }
        offset = box.next_offset;
    }
}

static bool mp4_validate_stsc(const uint8_t *stsc_entries, uint32_t stsc_entry_count)
{
    if (stsc_entry_count == 0u) {
        return false;
    }
    uint32_t prev_first_chunk = 0u;
    for (uint32_t i = 0; i < stsc_entry_count; ++i) {
        const uint8_t *ent = stsc_entries + (size_t)i * 12u;
        uint32_t first_chunk = read_u32_be(ent);
        uint32_t samples_per_chunk = read_u32_be(ent + 4u);
        if (i == 0u && first_chunk != 1u) {
            return false;
        }
        if (i > 0u && first_chunk <= prev_first_chunk) {
            return false;
        }
        if (samples_per_chunk == 0u) {
            return false;
        }
        prev_first_chunk = first_chunk;
    }
    return true;
}

static demuxer_t *mp4_demuxer_open(const uint8_t *data, size_t size)
{
    /* Scan top-level boxes to locate 'moov' */
    size_t offset = 0;
    mp4_parser_temp_t pt;
    memset(&pt, 0, sizeof(pt));
    pt.timescale = 1000u;

    mp4_box_t box;
    while (mp4_next_box(data, size, offset, &box)) {
        if (box.type == FOURCC('m', 'o', 'o', 'v')) {
            parse_moov(box.payload, box.payload_len, &pt);
            break;
        }
        offset = box.next_offset;
    }

    if (pt.codec == WAYWAL_CODEC_UNKNOWN || !pt.stsz_data || (!pt.stco_data && !pt.co64_data) ||
        !pt.stsc_data) {
        WAYWAL_LOG_WARN(
            "Failed to demux MP4 container (unsupported codec or missing index tables)");
        return NULL;
    }

    /* Parse sample count and strict size-table length from stsz */
    if (pt.stsz_size < 12u)
        return NULL;
    uint32_t uniform_sample_size = read_u32_be(pt.stsz_data + 4u);
    uint32_t sample_count = read_u32_be(pt.stsz_data + 8u);
    if (sample_count == 0u || sample_count > WAYWAL_DEMUXER_MAX_SAMPLES)
        return NULL;
    if (uniform_sample_size == 0u &&
        (uint64_t)sample_count * 4ULL > (uint64_t)(pt.stsz_size - 12u)) {
        WAYWAL_LOG_ERR("stsz bounds check failed");
        return NULL;
    }

    /* Parse chunk count from co64 or stco */
    uint32_t chunk_count = 0;
    const uint8_t *chunk_offsets_ptr = NULL;
    bool is_64bit_co = false;

    if (pt.co64_data) {
        if (pt.co64_size < 8u) {
            WAYWAL_LOG_ERR("co64 bounds check failed");
            return NULL;
        }
        chunk_count = read_u32_be(pt.co64_data + 4u);
        if ((uint64_t)chunk_count * 8ULL > (uint64_t)(pt.co64_size - 8u)) {
            WAYWAL_LOG_ERR("co64 bounds check failed");
            return NULL;
        }
        chunk_offsets_ptr = pt.co64_data + 8u;
        is_64bit_co = true;
    } else if (pt.stco_data) {
        if (pt.stco_size < 8u) {
            WAYWAL_LOG_ERR("stco bounds check failed");
            return NULL;
        }
        chunk_count = read_u32_be(pt.stco_data + 4u);
        if ((uint64_t)chunk_count * 4ULL > (uint64_t)(pt.stco_size - 8u)) {
            WAYWAL_LOG_ERR("stco bounds check failed");
            return NULL;
        }
        chunk_offsets_ptr = pt.stco_data + 8u;
    }
    if (chunk_count == 0u)
        return NULL;

    /* Parse stsc (sample to chunk) */
    if (pt.stsc_size < 8u)
        return NULL;
    uint32_t stsc_entry_count = read_u32_be(pt.stsc_data + 4u);
    if ((uint64_t)stsc_entry_count * 12ULL > (uint64_t)(pt.stsc_size - 8u)) {
        WAYWAL_LOG_ERR("stsc bounds check failed");
        return NULL;
    }
    const uint8_t *stsc_entries = pt.stsc_data + 8u;
    if (!mp4_validate_stsc(stsc_entries, stsc_entry_count)) {
        WAYWAL_LOG_ERR("stsc table validation failed");
        return NULL;
    }

    /* Build sample table */
    demuxer_sample_entry_t *samples =
        (demuxer_sample_entry_t *)calloc(sample_count, sizeof(demuxer_sample_entry_t));
    if (!samples)
        return NULL;

    /* Fill sample sizes */
    if (uniform_sample_size > 0u) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            samples[i].size = uniform_sample_size;
        }
    } else {
        const uint8_t *sizes_table = pt.stsz_data + 12u;
        for (uint32_t i = 0; i < sample_count; ++i) {
            samples[i].size = read_u32_be(sizes_table + (size_t)i * 4u);
        }
    }

    /* Fill sample offsets via chunk mapping */
    uint32_t cur_sample = 0;
    uint32_t stsc_idx = 0;
    uint32_t cur_samples_per_chunk = 0;

    for (uint32_t chunk_idx = 1; chunk_idx <= chunk_count && cur_sample < sample_count;
         ++chunk_idx) {
        if (stsc_idx < stsc_entry_count) {
            const uint8_t *ent = stsc_entries + (size_t)stsc_idx * 12u;
            uint32_t first_chunk = read_u32_be(ent);
            if (chunk_idx >= first_chunk) {
                cur_samples_per_chunk = read_u32_be(ent + 4u);
                if (stsc_idx + 1u < stsc_entry_count) {
                    uint32_t next_first = read_u32_be(ent + 12u);
                    if (chunk_idx + 1u >= next_first) {
                        stsc_idx++;
                    }
                }
            }
        }

        uint64_t chunk_offset =
            is_64bit_co ? read_u64_be(chunk_offsets_ptr + (size_t)(chunk_idx - 1u) * 8u)
                        : read_u32_be(chunk_offsets_ptr + (size_t)(chunk_idx - 1u) * 4u);

        for (uint32_t s = 0; s < cur_samples_per_chunk && cur_sample < sample_count; ++s) {
            uint64_t s_sz = (uint64_t)samples[cur_sample].size;
            if (chunk_offset > (uint64_t)size || s_sz > (uint64_t)size - chunk_offset) {
                WAYWAL_LOG_ERR("MP4 sample offset/size out of bounds");
                free(samples);
                return NULL;
            }
            samples[cur_sample].offset = (size_t)chunk_offset;
            chunk_offset += s_sz;
            cur_sample++;
        }
    }

    if (cur_sample != sample_count) {
        WAYWAL_LOG_ERR("MP4 chunk mapping incomplete (%u/%u samples)", cur_sample, sample_count);
        free(samples);
        return NULL;
    }

    /* Fill timestamps via stts */
    int64_t cur_dts = 0;
    uint32_t stts_sample_idx = 0;
    if (pt.stts_data) {
        if (pt.stts_size < 8u) {
            WAYWAL_LOG_ERR("stts bounds check failed");
            free(samples);
            return NULL;
        }
        uint32_t stts_count = read_u32_be(pt.stts_data + 4u);
        if ((uint64_t)stts_count * 8ULL > (uint64_t)(pt.stts_size - 8u)) {
            WAYWAL_LOG_ERR("stts bounds check failed");
            free(samples);
            return NULL;
        }
        if (pt.timescale == 0u) {
            pt.timescale = 1000u;
        }
        const uint8_t *stts_entries = pt.stts_data + 8u;
        for (uint32_t i = 0; i < stts_count && stts_sample_idx < sample_count; ++i) {
            uint32_t count = read_u32_be(stts_entries + (size_t)i * 8u);
            uint32_t delta = read_u32_be(stts_entries + (size_t)i * 8u + 4u);
            uint64_t delta_u64 = ((uint64_t)delta * 1000000ULL) / (uint64_t)pt.timescale;
            if (delta_u64 > (uint64_t)WAYWAL_DEMUXER_MAX_PTS_US) {
                WAYWAL_LOG_ERR("stts sample duration overflow");
                free(samples);
                return NULL;
            }
            int64_t delta_us = (int64_t)delta_u64;

            for (uint32_t j = 0; j < count && stts_sample_idx < sample_count; ++j) {
                samples[stts_sample_idx].dts_us = cur_dts;
                samples[stts_sample_idx].pts_us = cur_dts; /* Default pts = dts */
                int64_t next_dts = 0;
                if (ckd_add(&next_dts, cur_dts, delta_us) || next_dts > WAYWAL_DEMUXER_MAX_PTS_US) {
                    WAYWAL_LOG_ERR("stts timestamp overflow");
                    free(samples);
                    return NULL;
                }
                cur_dts = next_dts;
                stts_sample_idx++;
            }
        }
    }

    /* Apply composition time offsets via ctts (B-frame PTS reordering, ISO 14496-12 v0 & v1) */
    if (pt.ctts_data) {
        if (pt.ctts_size < 8u) {
            WAYWAL_LOG_ERR("ctts bounds check failed");
            free(samples);
            return NULL;
        }
        uint8_t ctts_version = pt.ctts_data[0];
        uint32_t ctts_count = read_u32_be(pt.ctts_data + 4u);
        if ((uint64_t)ctts_count * 8ULL > (uint64_t)(pt.ctts_size - 8u)) {
            WAYWAL_LOG_ERR("ctts bounds check failed");
            free(samples);
            return NULL;
        }
        if (pt.timescale == 0u) {
            pt.timescale = 1000u;
        }
        const uint8_t *ctts_entries = pt.ctts_data + 8u;
        uint32_t ctts_sample_idx = 0;
        int64_t min_pts = INT64_MAX;
        for (uint32_t i = 0; i < ctts_count && ctts_sample_idx < sample_count; ++i) {
            uint32_t count = read_u32_be(ctts_entries + (size_t)i * 8u);
            uint32_t raw_off = read_u32_be(ctts_entries + (size_t)i * 8u + 4u);
            int64_t sample_offset = (ctts_version == 0 && (raw_off & 0x80000000U) == 0)
                                        ? (int64_t)raw_off
                                        : (int64_t)(int32_t)raw_off;
            int64_t offset_us = (sample_offset * 1000000LL) / (int64_t)pt.timescale;
            for (uint32_t j = 0; j < count && ctts_sample_idx < sample_count; ++j) {
                int64_t pts = 0;
                if (ckd_add(&pts, samples[ctts_sample_idx].dts_us, offset_us)) {
                    WAYWAL_LOG_ERR("ctts timestamp overflow");
                    free(samples);
                    return NULL;
                }
                samples[ctts_sample_idx].pts_us = pts;
                if (pts < min_pts)
                    min_pts = pts;
                ctts_sample_idx++;
            }
        }
        if (ctts_sample_idx > 0 && min_pts != INT64_MAX && min_pts != 0) {
            for (uint32_t i = 0; i < sample_count; ++i) {
                int64_t shifted = 0;
                if (ckd_sub(&shifted, samples[i].pts_us, min_pts) ||
                    shifted > WAYWAL_DEMUXER_MAX_PTS_US || shifted < WAYWAL_DEMUXER_MIN_PTS_US) {
                    WAYWAL_LOG_ERR("ctts shifted timestamp overflow");
                    free(samples);
                    return NULL;
                }
                samples[i].pts_us = shifted;
            }
        } else {
            for (uint32_t i = 0; i < sample_count; ++i) {
                if (samples[i].pts_us > WAYWAL_DEMUXER_MAX_PTS_US ||
                    samples[i].pts_us < WAYWAL_DEMUXER_MIN_PTS_US) {
                    WAYWAL_LOG_ERR("ctts timestamp out of range");
                    free(samples);
                    return NULL;
                }
            }
        }
    }

    /* Mark keyframes via stss */
    if (pt.stss_data) {
        if (pt.stss_size < 8u) {
            WAYWAL_LOG_ERR("stss bounds check failed");
            free(samples);
            return NULL;
        }
        uint32_t stss_count = read_u32_be(pt.stss_data + 4u);
        if ((uint64_t)stss_count * 4ULL > (uint64_t)(pt.stss_size - 8u)) {
            WAYWAL_LOG_ERR("stss bounds check failed");
            free(samples);
            return NULL;
        }
        const uint8_t *stss_entries = pt.stss_data + 8u;
        for (uint32_t i = 0; i < stss_count; ++i) {
            uint32_t s_idx = read_u32_be(stss_entries + (size_t)i * 4u);
            if (s_idx > 0 && s_idx <= sample_count) {
                samples[s_idx - 1u].is_keyframe = true;
            }
        }
    } else {
        /* If no sync sample table, all samples are keyframes (e.g. MJPEG or intra-only) */
        for (uint32_t i = 0; i < sample_count; ++i) {
            samples[i].is_keyframe = true;
        }
    }

    uint32_t fps_num =
        (sample_count > 1u && cur_dts > 0) ? demuxer_compute_fps_num(sample_count, cur_dts) : 30u;
    demuxer_t *d =
        demuxer_alloc_instance(data, size, samples, sample_count, pt.codec, pt.width, pt.height,
                               fps_num, cur_dts, pt.extradata, pt.extradata_size);
    if (!d) {
        free(samples);
        return NULL;
    }

    WAYWAL_LOG_INFO("Demuxer initialized: codec=%d, %ux%u @ %u fps (samples: %u, duration: %.2fs)",
                    d->codec, d->width, d->height, d->fps_num, sample_count,
                    (double)d->duration_us / 1e6);
    return d;
}

/* =========================================================================
 * EBML / Matroska / WebM Demuxer Implementation
 * ========================================================================= */

#define EBML_ID_HEADER          0x1A45DFA3
#define EBML_ID_SEGMENT         0x18538067
#define EBML_ID_INFO            0x1549A966
#define EBML_ID_TIMECODE_SCALE  0x2AD7B1
#define EBML_ID_DURATION        0x4489
#define EBML_ID_TRACKS          0x1654AE6B
#define EBML_ID_TRACK_ENTRY     0xAE
#define EBML_ID_TRACK_NUMBER    0xD7
#define EBML_ID_TRACK_TYPE      0x83
#define EBML_ID_CODEC_ID        0x86
#define EBML_ID_CODEC_PRIVATE   0x63A2
#define EBML_ID_VIDEO           0xE0
#define EBML_ID_PIXEL_WIDTH     0xB0
#define EBML_ID_PIXEL_HEIGHT    0xBA
#define EBML_ID_CLUSTER         0x1F43B675
#define EBML_ID_CLUSTER_TIME    0xE7
#define EBML_ID_SIMPLE_BLOCK    0xA3
#define EBML_ID_BLOCK_GROUP     0xA0
#define EBML_ID_BLOCK           0xA1
#define EBML_ID_REFERENCE_BLOCK 0xFB

static bool ebml_read_vint(const uint8_t *data, size_t size, size_t *offset, uint64_t *out_val,
                           uint32_t *out_len, bool mask)
{
    if (!data || !offset || *offset >= size)
        return false;
    uint8_t first = data[*offset];
    if (first == 0)
        return false;

    uint32_t len = 1;
    uint8_t mask_val = 0x80;
    while (!(first & mask_val)) {
        len++;
        mask_val >>= 1;
        if (len > 8)
            return false;
    }

    if ((size_t)len > size - *offset)
        return false;

    uint64_t val = mask ? (first & (mask_val - 1)) : first;
    for (uint32_t i = 1; i < len; ++i) {
        val = (val << 8) | data[*offset + i];
    }

    *offset += len;
    if (out_val)
        *out_val = val;
    if (out_len)
        *out_len = len;
    return true;
}

static uint64_t ebml_read_uint(const uint8_t *data, size_t size)
{
    uint64_t val = 0;
    for (size_t i = 0; i < size; ++i) {
        val = (val << 8) | data[i];
    }
    return val;
}

static size_t ebml_clamp_elem_end(uint64_t elem_size, size_t content_start, size_t parent_end)
{
    if (elem_size == 0xFFFFFFFFFFFFFFULL || elem_size > (uint64_t)(parent_end - content_start)) {
        return parent_end;
    }
    return content_start + (size_t)elem_size;
}

static bool webm_reserve_samples(demuxer_sample_entry_t **samples_ptr, size_t *cap_ptr,
                                 size_t current_count, size_t add_count)
{
    if (!samples_ptr || !cap_ptr || current_count > (size_t)WAYWAL_DEMUXER_MAX_SAMPLES ||
        add_count > (size_t)WAYWAL_DEMUXER_MAX_SAMPLES - current_count) {
        return false;
    }

    size_t needed = current_count + add_count;
    if (needed <= *cap_ptr) {
        return true;
    }

    size_t new_cap = *cap_ptr;
    while (new_cap < needed) {
        if (new_cap >= (size_t)WAYWAL_DEMUXER_MAX_SAMPLES) {
            return false;
        }
        new_cap = (new_cap <= (size_t)WAYWAL_DEMUXER_MAX_SAMPLES / 2u)
                      ? (new_cap * 2u)
                      : (size_t)WAYWAL_DEMUXER_MAX_SAMPLES;
    }

    size_t alloc_bytes = 0;
    if (ckd_mul(&alloc_bytes, new_cap, sizeof(demuxer_sample_entry_t))) {
        return false;
    }
    demuxer_sample_entry_t *new_s = (demuxer_sample_entry_t *)realloc(*samples_ptr, alloc_bytes);
    if (!new_s) {
        return false;
    }
    *samples_ptr = new_s;
    *cap_ptr = new_cap;
    return true;
}

static bool webm_parse_lacing_sizes(const uint8_t *data, size_t blk_off, size_t blk_end,
                                    uint8_t lacing, uint32_t lace_count, size_t lace_sizes[256],
                                    size_t *out_payload_off)
{
    /* Require at least 1 byte per frame so lacing cannot amplify past actual block bytes. */
    if (lace_count == 0u || lace_count > 256u || blk_off > blk_end ||
        (blk_end - blk_off) < (size_t)lace_count) {
        return false;
    }

    size_t sum_sizes = 0;
    if (lacing == 1u) {
        for (uint32_t li = 0; li + 1u < lace_count; ++li) {
            size_t lsz = 0;
            while (true) {
                if (blk_off >= blk_end) {
                    return false;
                }
                uint8_t b = data[blk_off++];
                size_t rem = blk_end - blk_off;
                if (sum_sizes > rem || (size_t)b > rem - sum_sizes ||
                    lsz > (rem - sum_sizes) - (size_t)b) {
                    return false;
                }
                lsz += (size_t)b;
                if (b < 255u) {
                    break;
                }
            }
            if (lsz == 0u) {
                return false;
            }
            lace_sizes[li] = lsz;
            sum_sizes += lsz;
        }
        if (sum_sizes >= blk_end - blk_off) {
            return false;
        }
        lace_sizes[lace_count - 1u] = (blk_end - blk_off) - sum_sizes;
    } else if (lacing == 2u) {
        size_t rem = blk_end - blk_off;
        size_t fsz = rem / lace_count;
        if (fsz == 0u) {
            return false;
        }
        for (uint32_t li = 0; li + 1u < lace_count; ++li) {
            lace_sizes[li] = fsz;
            sum_sizes += fsz;
        }
        lace_sizes[lace_count - 1u] = rem - sum_sizes;
    } else if (lacing == 3u) {
        if (lace_count == 1u) {
            lace_sizes[0] = blk_end - blk_off;
        } else {
            uint64_t first_sz = 0;
            if (!ebml_read_vint(data, blk_end, &blk_off, &first_sz, NULL, true) ||
                first_sz == 0ULL || first_sz > (uint64_t)(blk_end - blk_off)) {
                return false;
            }
            lace_sizes[0] = (size_t)first_sz;
            sum_sizes = lace_sizes[0];
            for (uint32_t li = 1; li + 1u < lace_count; ++li) {
                uint64_t raw_vint = 0;
                uint32_t vlen = 0;
                if (!ebml_read_vint(data, blk_end, &blk_off, &raw_vint, &vlen, true) ||
                    vlen == 0u || vlen > 8u) {
                    return false;
                }
                int64_t bias = (1LL << (7u * vlen - 1u)) - 1LL;
                int64_t delta = (int64_t)raw_vint - bias;
                if (lace_sizes[li - 1u] > (size_t)INT64_MAX) {
                    return false;
                }
                int64_t next_sz = 0;
                if (ckd_add(&next_sz, (int64_t)lace_sizes[li - 1u], delta) || next_sz <= 0) {
                    return false;
                }
                size_t rem = blk_end - blk_off;
                if (sum_sizes > rem || (size_t)next_sz > rem - sum_sizes) {
                    return false;
                }
                lace_sizes[li] = (size_t)next_sz;
                sum_sizes += lace_sizes[li];
            }
            if (sum_sizes >= blk_end - blk_off) {
                return false;
            }
            lace_sizes[lace_count - 1u] = (blk_end - blk_off) - sum_sizes;
        }
    } else {
        return false;
    }

    *out_payload_off = blk_off;
    return true;
}

static bool webm_compute_block_pts(uint64_t cluster_time, int16_t rel_time, uint64_t timecode_scale,
                                   int64_t *out_pts_us)
{
    uint64_t abs_time = 0;
    if (rel_time >= 0) {
        if (ckd_add(&abs_time, cluster_time, (uint64_t)rel_time)) {
            return false;
        }
    } else {
        uint64_t neg = (uint64_t)(-(int64_t)rel_time);
        abs_time = (cluster_time > neg) ? (cluster_time - neg) : 0ULL;
    }

    uint64_t scaled_ns = 0;
    if (ckd_mul(&scaled_ns, abs_time, timecode_scale)) {
        return false;
    }
    uint64_t pts_us = scaled_ns / 1000ULL;
    if (pts_us > (uint64_t)WAYWAL_DEMUXER_MAX_PTS_US) {
        return false;
    }
    *out_pts_us = (int64_t)pts_us;
    return true;
}

static bool webm_append_block_samples(const uint8_t *data, size_t blk_off, size_t blk_end,
                                      uint64_t cluster_time, int16_t rel_time, uint8_t flags,
                                      bool is_kf, uint64_t timecode_scale,
                                      uint64_t default_duration_ns,
                                      demuxer_sample_entry_t **samples_ptr, size_t *samples_cap,
                                      size_t *samples_count)
{
    if (blk_off >= blk_end) {
        return true;
    }

    int64_t pts_us = 0;
    if (!webm_compute_block_pts(cluster_time, rel_time, timecode_scale, &pts_us)) {
        return false;
    }

    /* Check lacing bits (bits 2-1): 0 = no lacing, 1 = Xiph, 2 = fixed-size, 3 = EBML */
    uint8_t lacing = (flags >> 1) & 0x03u;
    if (lacing == 0u) {
        if (!webm_reserve_samples(samples_ptr, samples_cap, *samples_count, 1u)) {
            return false;
        }
        (*samples_ptr)[(*samples_count)++] = (demuxer_sample_entry_t){
            .offset = blk_off,
            .size = blk_end - blk_off,
            .pts_us = pts_us,
            .dts_us = pts_us,
            .is_keyframe = is_kf,
        };
        return true;
    }

    uint32_t lace_count = (uint32_t)data[blk_off++] + 1U;
    size_t lace_sizes[256];
    size_t frame_off = blk_off;
    if (!webm_parse_lacing_sizes(data, blk_off, blk_end, lacing, lace_count, lace_sizes,
                                 &frame_off)) {
        return true; /* Ignore malformed laced block without tearing down valid stream blocks */
    }

    uint64_t lace_step_u64 =
        (default_duration_ns > 0ULL) ? (default_duration_ns / 1000ULL) : 33333ULL;
    if (lace_step_u64 > (uint64_t)WAYWAL_DEMUXER_MAX_PTS_US) {
        return false;
    }
    int64_t max_step_off = 0;
    int64_t max_lace_pts = 0;
    if (ckd_mul(&max_step_off, (int64_t)(lace_count - 1u), (int64_t)lace_step_u64) ||
        ckd_add(&max_lace_pts, pts_us, max_step_off) || max_lace_pts > WAYWAL_DEMUXER_MAX_PTS_US) {
        return false;
    }

    if (!webm_reserve_samples(samples_ptr, samples_cap, *samples_count, (size_t)lace_count)) {
        return false;
    }

    for (uint32_t li = 0; li < lace_count; ++li) {
        size_t fsz = lace_sizes[li];
        if (fsz == 0u || fsz > blk_end - frame_off) {
            break;
        }
        int64_t f_pts = pts_us + (int64_t)li * (int64_t)lace_step_u64;
        (*samples_ptr)[(*samples_count)++] = (demuxer_sample_entry_t){
            .offset = frame_off,
            .size = fsz,
            .pts_us = f_pts,
            .dts_us = f_pts,
            .is_keyframe = (li == 0u) ? is_kf : false,
        };
        frame_off += fsz;
    }
    return true;
}

static demuxer_t *webm_demuxer_open(const uint8_t *data, size_t size)
{
    if (!data || size < 16u)
        return NULL;

    size_t offset = 0;
    uint64_t timecode_scale = 1000000ULL; /* default 1 ms in ns */
    double raw_duration = 0.0;
    uint64_t default_duration_ns = 0;
    uint64_t video_track_num = 1;
    waywal_codec_t codec = WAYWAL_CODEC_UNKNOWN;
    uint32_t width = 0, height = 0;
    const uint8_t *extradata = NULL;
    size_t extradata_size = 0;

    size_t samples_cap = 256u;
    size_t samples_count = 0u;
    demuxer_sample_entry_t *samples =
        (demuxer_sample_entry_t *)malloc(samples_cap * sizeof(demuxer_sample_entry_t));
    if (!samples)
        return NULL;

    /* Parse root elements */
    while (offset < size) {
        uint64_t elem_id = 0;
        uint64_t elem_size = 0;

        if (!ebml_read_vint(data, size, &offset, &elem_id, NULL, false))
            break;
        if (!ebml_read_vint(data, size, &offset, &elem_size, NULL, true))
            break;

        size_t content_start = offset;
        size_t content_end = ebml_clamp_elem_end(elem_size, content_start, size);

        if (elem_id == EBML_ID_HEADER) {
            offset = content_end;
            continue;
        }

        if (elem_id == EBML_ID_SEGMENT) {
            size_t seg_offset = content_start;
            while (seg_offset < content_end) {
                uint64_t sub_id = 0;
                uint64_t sub_size = 0;
                if (!ebml_read_vint(data, content_end, &seg_offset, &sub_id, NULL, false))
                    break;
                if (!ebml_read_vint(data, content_end, &seg_offset, &sub_size, NULL, true))
                    break;

                size_t sub_start = seg_offset;
                size_t sub_end = ebml_clamp_elem_end(sub_size, sub_start, content_end);

                if (sub_id == EBML_ID_INFO) {
                    size_t info_off = sub_start;
                    while (info_off < sub_end) {
                        uint64_t inf_id = 0, inf_size = 0;
                        if (!ebml_read_vint(data, sub_end, &info_off, &inf_id, NULL, false))
                            break;
                        if (!ebml_read_vint(data, sub_end, &info_off, &inf_size, NULL, true))
                            break;
                        if (inf_size > (uint64_t)(sub_end - info_off))
                            break;
                        if (inf_id == EBML_ID_TIMECODE_SCALE && inf_size <= 8u) {
                            timecode_scale = ebml_read_uint(data + info_off, (size_t)inf_size);
                            if (timecode_scale == 0 || timecode_scale > 1000000000ULL) {
                                timecode_scale = 1000000ULL; /* Clamp to default 1ms */
                            }
                        } else if (inf_id == EBML_ID_DURATION) {
                            if (inf_size == 4u) {
                                uint32_t bits = read_u32_be(data + info_off);
                                float f_dur = 0.0f;
                                memcpy(&f_dur, &bits, sizeof(f_dur));
                                if (isfinite(f_dur) && f_dur > 0.0f) {
                                    raw_duration = (double)f_dur;
                                }
                            } else if (inf_size == 8u) {
                                uint64_t bits = read_u64_be(data + info_off);
                                double d_dur = 0.0;
                                memcpy(&d_dur, &bits, sizeof(d_dur));
                                if (isfinite(d_dur) && d_dur > 0.0) {
                                    raw_duration = d_dur;
                                }
                            }
                        }
                        info_off += (size_t)inf_size;
                    }
                } else if (sub_id == EBML_ID_TRACKS) {
                    size_t trk_off = sub_start;
                    while (trk_off < sub_end) {
                        uint64_t trk_id = 0, trk_size = 0;
                        if (!ebml_read_vint(data, sub_end, &trk_off, &trk_id, NULL, false))
                            break;
                        if (!ebml_read_vint(data, sub_end, &trk_off, &trk_size, NULL, true))
                            break;

                        size_t trk_end = ebml_clamp_elem_end(trk_size, trk_off, sub_end);

                        if (trk_id == EBML_ID_TRACK_ENTRY) {
                            size_t entry_off = trk_off;
                            size_t entry_end = trk_end;
                            uint64_t cur_track_num = 0;
                            uint64_t track_type = 0;
                            uint64_t cur_def_dur = 0;
                            waywal_codec_t cur_codec = WAYWAL_CODEC_UNKNOWN;
                            uint32_t cur_w = 0, cur_h = 0;
                            const uint8_t *cur_extra = NULL;
                            size_t cur_extra_sz = 0;

                            while (entry_off < entry_end) {
                                uint64_t f_id = 0, f_size = 0;
                                if (!ebml_read_vint(data, entry_end, &entry_off, &f_id, NULL,
                                                    false))
                                    break;
                                if (!ebml_read_vint(data, entry_end, &entry_off, &f_size, NULL,
                                                    true))
                                    break;
                                if (f_size > (uint64_t)(entry_end - entry_off))
                                    break;

                                if (f_id == EBML_ID_TRACK_NUMBER && f_size <= 8u) {
                                    cur_track_num =
                                        ebml_read_uint(data + entry_off, (size_t)f_size);
                                } else if (f_id == EBML_ID_TRACK_TYPE && f_size <= 8u) {
                                    track_type = ebml_read_uint(data + entry_off, (size_t)f_size);
                                } else if (f_id == 0x23E383 /* DefaultDuration */ && f_size <= 8u) {
                                    cur_def_dur = ebml_read_uint(data + entry_off, (size_t)f_size);
                                } else if (f_id == EBML_ID_CODEC_ID) {
                                    if (f_size >= 5u && memcmp(data + entry_off, "V_VP9", 5) == 0) {
                                        cur_codec = WAYWAL_CODEC_VP9;
                                    } else if (f_size >= 5u &&
                                               memcmp(data + entry_off, "V_AV1", 5) == 0) {
                                        cur_codec = WAYWAL_CODEC_AV1;
                                    } else if (f_size >= 15u &&
                                               memcmp(data + entry_off, "V_MPEG4/ISO/AVC", 15) ==
                                                   0) {
                                        cur_codec = WAYWAL_CODEC_H264;
                                    } else if (f_size >= 16u &&
                                               memcmp(data + entry_off, "V_MPEGH/ISO/HEVC", 16) ==
                                                   0) {
                                        cur_codec = WAYWAL_CODEC_HEVC;
                                    }
                                } else if (f_id == EBML_ID_CODEC_PRIVATE) {
                                    cur_extra = data + entry_off;
                                    cur_extra_sz = (size_t)f_size;
                                } else if (f_id == EBML_ID_VIDEO) {
                                    size_t vid_off = entry_off;
                                    size_t vid_end = vid_off + (size_t)f_size;
                                    while (vid_off < vid_end) {
                                        uint64_t v_id = 0, v_size = 0;
                                        if (!ebml_read_vint(data, vid_end, &vid_off, &v_id, NULL,
                                                            false))
                                            break;
                                        if (!ebml_read_vint(data, vid_end, &vid_off, &v_size, NULL,
                                                            true))
                                            break;
                                        if (v_size > (uint64_t)(vid_end - vid_off))
                                            break;
                                        if (v_id == EBML_ID_PIXEL_WIDTH && v_size <= 8u) {
                                            uint64_t w =
                                                ebml_read_uint(data + vid_off, (size_t)v_size);
                                            cur_w = (w <= UINT32_MAX) ? (uint32_t)w : 0u;
                                        } else if (v_id == EBML_ID_PIXEL_HEIGHT && v_size <= 8u) {
                                            uint64_t h =
                                                ebml_read_uint(data + vid_off, (size_t)v_size);
                                            cur_h = (h <= UINT32_MAX) ? (uint32_t)h : 0u;
                                        }
                                        vid_off += (size_t)v_size;
                                    }
                                }
                                entry_off += (size_t)f_size;
                            }

                            if (track_type == 1 /* Video */ && cur_codec != WAYWAL_CODEC_UNKNOWN) {
                                video_track_num = cur_track_num;
                                codec = cur_codec;
                                width = cur_w;
                                height = cur_h;
                                extradata = cur_extra;
                                extradata_size = cur_extra_sz;
                                default_duration_ns = cur_def_dur;
                            }
                        }
                        trk_off = trk_end;
                    }
                } else if (sub_id == EBML_ID_CLUSTER) {
                    size_t clus_off = sub_start;
                    uint64_t cluster_time = 0;

                    while (clus_off < sub_end) {
                        uint64_t c_id = 0, c_size = 0;
                        if (!ebml_read_vint(data, sub_end, &clus_off, &c_id, NULL, false))
                            break;
                        if (!ebml_read_vint(data, sub_end, &clus_off, &c_size, NULL, true))
                            break;

                        size_t c_end = ebml_clamp_elem_end(c_size, clus_off, sub_end);

                        if (c_id == EBML_ID_CLUSTER_TIME) {
                            if (c_size > (uint64_t)(c_end - clus_off))
                                break;
                            if (c_size <= 8u) {
                                cluster_time = ebml_read_uint(data + clus_off, (size_t)c_size);
                            }
                        } else if (c_id == EBML_ID_SIMPLE_BLOCK || c_id == EBML_ID_BLOCK_GROUP) {
                            size_t blk_off = clus_off;
                            size_t blk_end = c_end;
                            bool is_block_group = (c_id == EBML_ID_BLOCK_GROUP);
                            bool has_block = !is_block_group;
                            bool has_reference_block = false;

                            if (is_block_group) {
                                size_t bg_off = clus_off;
                                size_t bg_end = blk_end;
                                while (bg_off < bg_end) {
                                    uint64_t bg_id = 0, bg_size = 0;
                                    if (!ebml_read_vint(data, bg_end, &bg_off, &bg_id, NULL, false))
                                        break;
                                    if (!ebml_read_vint(data, bg_end, &bg_off, &bg_size, NULL,
                                                        true))
                                        break;
                                    size_t el_end = ebml_clamp_elem_end(bg_size, bg_off, bg_end);
                                    if (bg_id == EBML_ID_BLOCK) {
                                        blk_off = bg_off;
                                        blk_end = el_end;
                                        has_block = true;
                                    } else if (bg_id == EBML_ID_REFERENCE_BLOCK) {
                                        has_reference_block = true;
                                    }
                                    bg_off = el_end;
                                }
                            }

                            if (has_block) {
                                uint64_t blk_track = 0;
                                if (ebml_read_vint(data, blk_end, &blk_off, &blk_track, NULL,
                                                   true)) {
                                    if (blk_track == video_track_num && blk_off <= blk_end &&
                                        (blk_end - blk_off) >= 3u) {
                                        int16_t rel_time =
                                            (int16_t)(((uint16_t)data[blk_off] << 8) |
                                                      data[blk_off + 1u]);
                                        uint8_t flags = data[blk_off + 2u];
                                        blk_off += 3u;

                                        bool is_kf = is_block_group ? !has_reference_block
                                                                    : ((flags & 0x80u) != 0u);
                                        if (!webm_append_block_samples(
                                                data, blk_off, blk_end, cluster_time, rel_time,
                                                flags, is_kf, timecode_scale, default_duration_ns,
                                                &samples, &samples_cap, &samples_count)) {
                                            free(samples);
                                            return NULL;
                                        }
                                    }
                                }
                            }
                        }
                        clus_off = c_end;
                    }
                }

                seg_offset = sub_end;
            }
            break;
        }

        offset = content_end;
    }

    if (codec == WAYWAL_CODEC_UNKNOWN || samples_count == 0u || width == 0u || height == 0u) {
        free(samples);
        return NULL;
    }

    int64_t last_pts = samples[samples_count - 1u].pts_us;
    int64_t info_duration_us = 0;
    if (raw_duration > 0.0 && isfinite(raw_duration)) {
        double scaled_dur = raw_duration * ((double)timecode_scale / 1000.0);
        if (isfinite(scaled_dur) && scaled_dur > 0.0 &&
            scaled_dur < (double)WAYWAL_DEMUXER_MAX_PTS_US - 1.0) {
            info_duration_us = (int64_t)(scaled_dur + 0.5);
        }
    }
    int64_t total_duration_us = (info_duration_us > last_pts) ? info_duration_us : last_pts;

    uint32_t fps_num = 30u;
    if (info_duration_us > last_pts && total_duration_us > 0) {
        fps_num = demuxer_compute_fps_num(samples_count, total_duration_us);
    } else if (samples_count > 1u && last_pts > 0) {
        fps_num = demuxer_compute_fps_num(samples_count - 1u, last_pts);
    }

    demuxer_t *d = demuxer_alloc_instance(data, size, samples, samples_count, codec, width, height,
                                          fps_num, total_duration_us, extradata, extradata_size);
    if (!d) {
        free(samples);
        return NULL;
    }

    WAYWAL_LOG_INFO(
        "WebM/EBML demuxer initialized: codec=%d, %ux%u @ %u fps (samples: %zu, duration: %.2fs)",
        d->codec, d->width, d->height, d->fps_num, samples_count, (double)d->duration_us / 1e6);
    return d;
}

demuxer_t *demuxer_open_mem(const uint8_t *data, size_t size)
{
    if (!data || size < 16u)
        return NULL;

    /* Check for EBML/WebM magic: 0x1A 0x45 0xDF 0xA3 */
    if (data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
        return webm_demuxer_open(data, size);
    }

    return mp4_demuxer_open(data, size);
}

demuxer_t *demuxer_open_file(const char *filepath)
{
    if (!filepath)
        return NULL;

    int fd = open(filepath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        WAYWAL_LOG_ERR("Failed to open video file '%s': %s", filepath, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        return NULL;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    size_t file_size = (size_t)st.st_size;
    /* Use MAP_PRIVATE on read-only regular files so MADV_DONTNEED PTE eviction never targets
     * MAP_SHARED mappings. */
    void *mapped = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for video file '%s': %s", filepath, strerror(errno));
        close(fd);
        return NULL;
    }
    madvise(mapped, file_size, MADV_RANDOM);

    demuxer_t *d = demuxer_open_mem((const uint8_t *)mapped, file_size);
    if (!d) {
        munmap(mapped, file_size);
        close(fd);
        return NULL;
    }

    demuxer_state_t *state = (demuxer_state_t *)d->internal_state;
    state->file_fd = fd;
    state->owns_mmap = true;
    evict_mmap_for_seek(d, 0);
    madvise(mapped, file_size, MADV_SEQUENTIAL);
    size_t prefetch_len = file_size < (2U << 20) ? file_size : (2U << 20);
    madvise(mapped, prefetch_len, MADV_WILLNEED);
    return d;
}
