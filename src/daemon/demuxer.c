#include "waywal/demuxer.h"
#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <endian.h>

#define FOURCC(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8)  | ((uint32_t)(uint8_t)(d)))

typedef struct {
    size_t   offset;
    size_t   size;
    int64_t  pts_us;
    int64_t  dts_us;
    bool     is_keyframe;
} mp4_sample_entry_t;

typedef struct {
    int file_fd;
    bool owns_mmap;

    mp4_sample_entry_t *samples;
    size_t              num_samples;
    size_t              current_sample_idx;
} mp4_state_t;

static inline uint16_t read_u16_be(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static inline uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static inline uint64_t read_u64_be(const uint8_t *p) {
    return ((uint64_t)read_u32_be(p) << 32) | (uint64_t)read_u32_be(p + 4);
}

static bool mp4_read_packet(demuxer_t *d, demux_packet_t *pkt) {
    if (!d || !d->internal_state || !pkt) return false;
    mp4_state_t *st = (mp4_state_t *)d->internal_state;

    if (st->current_sample_idx >= st->num_samples) {
        return false; /* End of stream */
    }

    const mp4_sample_entry_t *s = &st->samples[st->current_sample_idx++];
    if (s->offset + s->size > d->mapped_size) {
        return false;
    }

    pkt->data = d->mapped_buf + s->offset;
    pkt->size = s->size;
    pkt->pts_us = s->pts_us;
    pkt->dts_us = s->dts_us;
    pkt->is_keyframe = s->is_keyframe;
    return true;
}

static bool mp4_seek_us(demuxer_t *d, int64_t target_pts_us) {
    if (!d || !d->internal_state) return false;
    mp4_state_t *st = (mp4_state_t *)d->internal_state;
    if (st->num_samples == 0) return false;

    if (target_pts_us <= 0) {
        st->current_sample_idx = 0;
        return true;
    }

    /* Find nearest keyframe at or before target_pts_us */
    size_t best_idx = 0;
    for (size_t i = 0; i < st->num_samples; ++i) {
        if (st->samples[i].pts_us > target_pts_us) {
            break;
        }
        if (st->samples[i].is_keyframe) {
            best_idx = i;
        }
    }

    st->current_sample_idx = best_idx;
    return true;
}

static void mp4_destroy(demuxer_t *d) {
    if (!d) return;
    if (d->internal_state) {
        mp4_state_t *st = (mp4_state_t *)d->internal_state;
        if (st->samples) {
            free(st->samples);
        }
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

typedef struct {
    const uint8_t *stsz_data;
    size_t         stsz_size;
    const uint8_t *stco_data;
    size_t         stco_size;
    const uint8_t *co64_data;
    size_t         co64_size;
    const uint8_t *stsc_data;
    size_t         stsc_size;
    const uint8_t *stts_data;
    size_t         stts_size;
    const uint8_t *ctts_data;
    size_t         ctts_size;
    const uint8_t *stss_data;
    size_t         stss_size;
    uint32_t       timescale;
    uint32_t       width;
    uint32_t       height;
    waywal_codec_t   codec;
    const uint8_t *extradata;
    size_t         extradata_size;
} mp4_parser_temp_t;

static void parse_stbl(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint64_t box_size = read_u32_be(buf + offset);
        uint32_t box_type = read_u32_be(buf + offset + 4);
        size_t hdr_len = 8;

        if (box_size == 1) {
            if (offset + 16 > size) break;
            box_size = read_u64_be(buf + offset + 8);
            hdr_len = 16;
        } else if (box_size == 0) {
            box_size = size - offset;
        }

        if (box_size < hdr_len || offset + box_size > size) break;

        const uint8_t *box_data = buf + offset + hdr_len;
        size_t payload_len = box_size - hdr_len;

        switch (box_type) {
            case FOURCC('s','t','s','d'): {
                if (payload_len >= 8) {
                    uint32_t entry_count = read_u32_be(box_data + 4);
                    size_t entry_off = 8;
                    for (uint32_t i = 0; i < entry_count && entry_off + 8 <= payload_len; ++i) {
                        uint32_t e_size = read_u32_be(box_data + entry_off);
                        uint32_t e_type = read_u32_be(box_data + entry_off + 4);
                        if (e_type == FOURCC('a','v','c','1') || e_type == FOURCC('a','v','c','3')) {
                            pt->codec = WAYWAL_CODEC_H264;
                        } else if (e_type == FOURCC('h','v','c','1') || e_type == FOURCC('h','e','v','1')) {
                            pt->codec = WAYWAL_CODEC_HEVC;
                        } else if (e_type == FOURCC('v','p','0','9')) {
                            pt->codec = WAYWAL_CODEC_VP9;
                        } else if (e_type == FOURCC('a','v','0','1')) {
                            pt->codec = WAYWAL_CODEC_AV1;
                        }

                        if (e_size >= 36 && entry_off + e_size <= payload_len) {
                            pt->width = read_u16_be(box_data + entry_off + 32);
                            pt->height = read_u16_be(box_data + entry_off + 34);

                            /* Search for avcC / hvcC sub-boxes */
                            size_t sub_off = entry_off + 36;
                            while (sub_off + 8 <= entry_off + e_size) {
                                uint32_t sub_size = read_u32_be(box_data + sub_off);
                                uint32_t sub_type = read_u32_be(box_data + sub_off + 4);
                                if (sub_size < 8 || sub_off + sub_size > entry_off + e_size) break;

                                if (sub_type == FOURCC('a','v','c','C') ||
                                    sub_type == FOURCC('h','v','c','C')) {
                                    pt->extradata = box_data + sub_off + 8;
                                    pt->extradata_size = sub_size - 8;
                                    break;
                                }
                                sub_off += sub_size;
                            }
                        }
                        if (e_size == 0) break;
                        entry_off += e_size;
                    }
                }
                break;
            }
            case FOURCC('s','t','s','z'):
                pt->stsz_data = box_data;
                pt->stsz_size = payload_len;
                break;
            case FOURCC('s','t','c','o'):
                pt->stco_data = box_data;
                pt->stco_size = payload_len;
                break;
            case FOURCC('c','o','6','4'):
                pt->co64_data = box_data;
                pt->co64_size = payload_len;
                break;
            case FOURCC('s','t','s','c'):
                pt->stsc_data = box_data;
                pt->stsc_size = payload_len;
                break;
            case FOURCC('s','t','t','s'):
                pt->stts_data = box_data;
                pt->stts_size = payload_len;
                break;
            case FOURCC('c','t','t','s'):
                pt->ctts_data = box_data;
                pt->ctts_size = payload_len;
                break;
            case FOURCC('s','t','s','s'):
                pt->stss_data = box_data;
                pt->stss_size = payload_len;
                break;
            default:
                break;
        }

        offset += box_size;
    }
}

static void parse_minf(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint32_t box_size = read_u32_be(buf + offset);
        uint32_t box_type = read_u32_be(buf + offset + 4);
        if (box_size < 8 || offset + box_size > size) break;

        if (box_type == FOURCC('s','t','b','l')) {
            parse_stbl(buf + offset + 8, box_size - 8, pt);
            break;
        }
        offset += box_size;
    }
}

static void parse_mdia(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt) {
    size_t offset = 0;
    bool is_video = false;

    while (offset + 8 <= size) {
        uint32_t box_size = read_u32_be(buf + offset);
        uint32_t box_type = read_u32_be(buf + offset + 4);
        if (box_size < 8 || offset + box_size > size) break;

        const uint8_t *box_data = buf + offset + 8;
        size_t payload_len = box_size - 8;

        if (box_type == FOURCC('h','d','l','r')) {
            if (payload_len >= 12) {
                uint32_t hdlr_type = read_u32_be(box_data + 8);
                if (hdlr_type == FOURCC('v','i','d','e')) {
                    is_video = true;
                }
            }
        } else if (box_type == FOURCC('m','d','h','d')) {
            if (payload_len >= 16) {
                uint8_t ver = box_data[0];
                if (ver == 0 && payload_len >= 20) {
                    pt->timescale = read_u32_be(box_data + 12);
                } else if (ver == 1 && payload_len >= 28) {
                    pt->timescale = read_u32_be(box_data + 20);
                }
            }
        } else if (box_type == FOURCC('m','i','n','f')) {
            if (is_video) {
                parse_minf(box_data, payload_len, pt);
            }
        }

        offset += box_size;
    }
}

static void parse_trak(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint32_t box_size = read_u32_be(buf + offset);
        uint32_t box_type = read_u32_be(buf + offset + 4);
        if (box_size < 8 || offset + box_size > size) break;

        if (box_type == FOURCC('m','d','i','a')) {
            parse_mdia(buf + offset + 8, box_size - 8, pt);
            if (pt->codec != WAYWAL_CODEC_UNKNOWN) {
                break; /* Successfully found video track */
            }
        }
        offset += box_size;
    }
}

static void parse_moov(const uint8_t *buf, size_t size, mp4_parser_temp_t *pt) {
    size_t offset = 0;
    while (offset + 8 <= size) {
        uint32_t box_size = read_u32_be(buf + offset);
        uint32_t box_type = read_u32_be(buf + offset + 4);
        if (box_size < 8 || offset + box_size > size) break;

        if (box_type == FOURCC('t','r','a','k')) {
            parse_trak(buf + offset + 8, box_size - 8, pt);
            if (pt->codec != WAYWAL_CODEC_UNKNOWN) {
                break;
            }
        }
        offset += box_size;
    }
}

/* =========================================================================
 * EBML / Matroska / WebM Demuxer Implementation (PERF-07)
 * ========================================================================= */

#define EBML_ID_HEADER          0x1A45DFA3
#define EBML_ID_SEGMENT         0x18538067
#define EBML_ID_SEEK_HEAD       0x114D9B74
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

typedef struct {
    size_t   offset;
    size_t   size;
    int64_t  pts_us;
    int64_t  dts_us;
    bool     is_keyframe;
} webm_sample_entry_t;

typedef struct {
    int file_fd;
    bool owns_mmap;

    webm_sample_entry_t *samples;
    size_t               num_samples;
    size_t               current_sample_idx;
} webm_state_t;

static bool ebml_read_vint(const uint8_t *data, size_t size, size_t *offset,
                           uint64_t *out_val, uint32_t *out_len, bool mask) {
    if (!data || *offset >= size) return false;
    uint8_t first = data[*offset];
    if (first == 0) return false;

    uint32_t len = 1;
    uint8_t mask_val = 0x80;
    while (!(first & mask_val)) {
        len++;
        mask_val >>= 1;
        if (len > 8) return false;
    }

    if (*offset + len > size) return false;

    uint64_t val = mask ? (first & (mask_val - 1)) : first;
    for (uint32_t i = 1; i < len; ++i) {
        val = (val << 8) | data[*offset + i];
    }

    *offset += len;
    if (out_val) *out_val = val;
    if (out_len) *out_len = len;
    return true;
}

static uint64_t ebml_read_uint(const uint8_t *data, size_t size) {
    uint64_t val = 0;
    for (size_t i = 0; i < size; ++i) {
        val = (val << 8) | data[i];
    }
    return val;
}

static bool webm_read_packet(demuxer_t *d, demux_packet_t *pkt) {
    if (!d || !d->internal_state || !pkt) return false;
    webm_state_t *st = (webm_state_t *)d->internal_state;

    if (st->current_sample_idx >= st->num_samples) {
        return false; /* End of stream */
    }

    const webm_sample_entry_t *s = &st->samples[st->current_sample_idx++];
    if (s->offset + s->size > d->mapped_size) {
        return false;
    }

    pkt->data = d->mapped_buf + s->offset;
    pkt->size = s->size;
    pkt->pts_us = s->pts_us;
    pkt->dts_us = s->dts_us;
    pkt->is_keyframe = s->is_keyframe;
    return true;
}

static bool webm_seek_us(demuxer_t *d, int64_t target_pts_us) {
    if (!d || !d->internal_state) return false;
    webm_state_t *st = (webm_state_t *)d->internal_state;
    if (st->num_samples == 0) return false;

    if (target_pts_us <= 0) {
        st->current_sample_idx = 0;
        return true;
    }

    size_t best_idx = 0;
    for (size_t i = 0; i < st->num_samples; ++i) {
        if (st->samples[i].pts_us > target_pts_us) {
            break;
        }
        if (st->samples[i].is_keyframe) {
            best_idx = i;
        }
    }

    st->current_sample_idx = best_idx;
    return true;
}

static void webm_destroy(demuxer_t *d) {
    if (!d) return;
    if (d->internal_state) {
        webm_state_t *st = (webm_state_t *)d->internal_state;
        if (st->samples) {
            free(st->samples);
        }
        if (st->owns_mmap && d->mapped_buf) {
            munmap((void *)d->mapped_buf, d->mapped_size);
        }
        if (st->file_fd >= 0) {
            close(st->file_fd);
        }
        free(st);
    }
    free(d);
}

static demuxer_t *webm_demuxer_open(const uint8_t *data, size_t size) {
    if (!data || size < 16) return NULL;

    size_t offset = 0;
    uint64_t timecode_scale = 1000000; /* default 1 ms in ns */
    uint64_t video_track_num = 1;
    waywal_codec_t codec = WAYWAL_CODEC_UNKNOWN;
    uint32_t width = 0, height = 0;
    const uint8_t *extradata = NULL;
    size_t extradata_size = 0;

    size_t samples_cap = 256;
    size_t samples_count = 0;
    webm_sample_entry_t *samples = (webm_sample_entry_t *)malloc(samples_cap * sizeof(webm_sample_entry_t));
    if (!samples) return NULL;

    /* Parse root elements */
    while (offset < size) {
        uint64_t elem_id = 0;
        uint64_t elem_size = 0;

        if (!ebml_read_vint(data, size, &offset, &elem_id, NULL, false)) break;
        if (!ebml_read_vint(data, size, &offset, &elem_size, NULL, true)) break;

        size_t content_start = offset;
        size_t content_end = content_start + elem_size;
        if (elem_size == 0xFFFFFFFFFFFFFFULL || content_end > size) {
            content_end = size;
        }

        if (elem_id == EBML_ID_HEADER) {
            offset = content_end;
            continue;
        }

        if (elem_id == EBML_ID_SEGMENT) {
            /* Enter Segment */
            size_t seg_offset = content_start;
            while (seg_offset < content_end) {
                uint64_t sub_id = 0;
                uint64_t sub_size = 0;
                if (!ebml_read_vint(data, content_end, &seg_offset, &sub_id, NULL, false)) break;
                if (!ebml_read_vint(data, content_end, &seg_offset, &sub_size, NULL, true)) break;

                size_t sub_start = seg_offset;
                size_t sub_end = sub_start + sub_size;
                if (sub_size == 0xFFFFFFFFFFFFFFULL || sub_end > content_end) {
                    sub_end = content_end;
                }

                if (sub_id == EBML_ID_INFO) {
                    size_t info_off = sub_start;
                    while (info_off < sub_end) {
                        uint64_t inf_id = 0, inf_size = 0;
                        if (!ebml_read_vint(data, sub_end, &info_off, &inf_id, NULL, false)) break;
                        if (!ebml_read_vint(data, sub_end, &info_off, &inf_size, NULL, true)) break;
                        if (inf_id == EBML_ID_TIMECODE_SCALE) {
                            timecode_scale = ebml_read_uint(data + info_off, inf_size);
                        }
                        info_off += inf_size;
                    }
                } else if (sub_id == EBML_ID_TRACKS) {
                    size_t trk_off = sub_start;
                    while (trk_off < sub_end) {
                        uint64_t trk_id = 0, trk_size = 0;
                        if (!ebml_read_vint(data, sub_end, &trk_off, &trk_id, NULL, false)) break;
                        if (!ebml_read_vint(data, sub_end, &trk_off, &trk_size, NULL, true)) break;

                        if (trk_id == EBML_ID_TRACK_ENTRY) {
                            size_t entry_off = trk_off;
                            size_t entry_end = entry_off + trk_size;
                            uint64_t cur_track_num = 0;
                            uint64_t track_type = 0;
                            waywal_codec_t cur_codec = WAYWAL_CODEC_UNKNOWN;
                            uint32_t cur_w = 0, cur_h = 0;
                            const uint8_t *cur_extra = NULL;
                            size_t cur_extra_sz = 0;

                            while (entry_off < entry_end) {
                                uint64_t f_id = 0, f_size = 0;
                                if (!ebml_read_vint(data, entry_end, &entry_off, &f_id, NULL, false)) break;
                                if (!ebml_read_vint(data, entry_end, &entry_off, &f_size, NULL, true)) break;

                                if (f_id == EBML_ID_TRACK_NUMBER) {
                                    cur_track_num = ebml_read_uint(data + entry_off, f_size);
                                } else if (f_id == EBML_ID_TRACK_TYPE) {
                                    track_type = ebml_read_uint(data + entry_off, f_size);
                                } else if (f_id == EBML_ID_CODEC_ID) {
                                    if (f_size >= 5 && memcmp(data + entry_off, "V_VP9", 5) == 0) {
                                        cur_codec = WAYWAL_CODEC_VP9;
                                    } else if (f_size >= 5 && memcmp(data + entry_off, "V_AV1", 5) == 0) {
                                        cur_codec = WAYWAL_CODEC_AV1;
                                    } else if (f_size >= 15 && memcmp(data + entry_off, "V_MPEG4/ISO/AVC", 15) == 0) {
                                        cur_codec = WAYWAL_CODEC_H264;
                                    } else if (f_size >= 16 && memcmp(data + entry_off, "V_MPEGH/ISO/HEVC", 16) == 0) {
                                        cur_codec = WAYWAL_CODEC_HEVC;
                                    }
                                } else if (f_id == EBML_ID_CODEC_PRIVATE) {
                                    cur_extra = data + entry_off;
                                    cur_extra_sz = f_size;
                                } else if (f_id == EBML_ID_VIDEO) {
                                    size_t vid_off = entry_off;
                                    size_t vid_end = vid_off + f_size;
                                    while (vid_off < vid_end) {
                                        uint64_t v_id = 0, v_size = 0;
                                        if (!ebml_read_vint(data, vid_end, &vid_off, &v_id, NULL, false)) break;
                                        if (!ebml_read_vint(data, vid_end, &vid_off, &v_size, NULL, true)) break;
                                        if (v_id == EBML_ID_PIXEL_WIDTH) {
                                            cur_w = (uint32_t)ebml_read_uint(data + vid_off, v_size);
                                        } else if (v_id == EBML_ID_PIXEL_HEIGHT) {
                                            cur_h = (uint32_t)ebml_read_uint(data + vid_off, v_size);
                                        }
                                        vid_off += v_size;
                                    }
                                }
                                entry_off += f_size;
                            }

                            if (track_type == 1 /* Video */ && cur_codec != WAYWAL_CODEC_UNKNOWN) {
                                video_track_num = cur_track_num;
                                codec = cur_codec;
                                width = cur_w;
                                height = cur_h;
                                extradata = cur_extra;
                                extradata_size = cur_extra_sz;
                            }
                        }
                        trk_off += trk_size;
                    }
                } else if (sub_id == EBML_ID_CLUSTER) {
                    size_t clus_off = sub_start;
                    uint64_t cluster_time = 0;

                    while (clus_off < sub_end) {
                        uint64_t c_id = 0, c_size = 0;
                        if (!ebml_read_vint(data, sub_end, &clus_off, &c_id, NULL, false)) break;
                        if (!ebml_read_vint(data, sub_end, &clus_off, &c_size, NULL, true)) break;

                        if (c_id == EBML_ID_CLUSTER_TIME) {
                            cluster_time = ebml_read_uint(data + clus_off, c_size);
                        } else if (c_id == EBML_ID_SIMPLE_BLOCK) {
                            size_t blk_off = clus_off;
                            size_t blk_end = blk_off + c_size;
                            uint64_t blk_track = 0;
                            if (ebml_read_vint(data, blk_end, &blk_off, &blk_track, NULL, true)) {
                                if (blk_track == video_track_num && blk_off + 3 <= blk_end) {
                                    int16_t rel_time = (int16_t)(((uint16_t)data[blk_off] << 8) | data[blk_off + 1]);
                                    uint8_t flags = data[blk_off + 2];
                                    blk_off += 3;

                                    /* Check lacing bits (bits 2-1): 0 = no lacing */
                                    uint8_t lacing = (flags >> 1) & 0x03;
                                    if (lacing == 0 && blk_off < blk_end) {
                                        int64_t pts_us = (int64_t)(cluster_time + rel_time) * (int64_t)timecode_scale / 1000LL;
                                        bool is_kf = (flags & 0x80) != 0;

                                        if (samples_count >= samples_cap) {
                                            samples_cap *= 2;
                                            webm_sample_entry_t *new_s = (webm_sample_entry_t *)realloc(
                                                samples, samples_cap * sizeof(webm_sample_entry_t));
                                            if (!new_s) {
                                                free(samples);
                                                return NULL;
                                            }
                                            samples = new_s;
                                        }

                                        samples[samples_count++] = (webm_sample_entry_t){
                                            .offset = blk_off,
                                            .size = blk_end - blk_off,
                                            .pts_us = pts_us,
                                            .dts_us = pts_us,
                                            .is_keyframe = is_kf,
                                        };
                                    }
                                }
                            }
                        }
                        clus_off += c_size;
                    }
                }

                seg_offset = sub_end;
            }
            break;
        }

        offset = content_end;
    }

    if (codec == WAYWAL_CODEC_UNKNOWN || samples_count == 0 || width == 0 || height == 0) {
        free(samples);
        return NULL;
    }

    demuxer_t *d = (demuxer_t *)calloc(1, sizeof(demuxer_t));
    if (!d) {
        free(samples);
        return NULL;
    }

    webm_state_t *st = (webm_state_t *)calloc(1, sizeof(webm_state_t));
    if (!st) {
        free(samples);
        free(d);
        return NULL;
    }

    st->file_fd = -1;
    st->samples = samples;
    st->num_samples = samples_count;
    st->current_sample_idx = 0;

    int64_t last_pts = samples[samples_count - 1].pts_us;

    d->mapped_buf = data;
    d->mapped_size = size;
    d->codec = codec;
    d->width = width;
    d->height = height;
    d->fps_num = (samples_count > 1 && last_pts > 0) ? (uint32_t)((int64_t)samples_count * 1000000LL / last_pts) : 30;
    if (d->fps_num == 0) d->fps_num = 30;
    d->fps_den = 1;
    d->duration_us = last_pts;
    d->extradata = extradata;
    d->extradata_size = extradata_size;
    d->internal_state = st;

    d->read_packet = webm_read_packet;
    d->seek_us = webm_seek_us;
    d->destroy = webm_destroy;

    WAYWAL_LOG_INFO("WebM/EBML demuxer initialized: codec=%d, %ux%u @ %u fps (samples: %zu, duration: %.2fs)",
                  d->codec, d->width, d->height, d->fps_num, samples_count, (double)d->duration_us / 1e6);
    return d;
}

demuxer_t *demuxer_open_mem(const uint8_t *data, size_t size) {
    if (!data || size < 16) return NULL;

    /* Check for EBML/WebM magic: 0x1A 0x45 0xDF 0xA3 */
    if (data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
        return webm_demuxer_open(data, size);
    }

    /* Scan top-level boxes to locate 'moov' */
    size_t offset = 0;
    mp4_parser_temp_t pt;
    memset(&pt, 0, sizeof(pt));
    pt.timescale = 1000;

    while (offset + 8 <= size) {
        uint64_t box_size = read_u32_be(data + offset);
        uint32_t box_type = read_u32_be(data + offset + 4);
        size_t hdr_len = 8;

        if (box_size == 1) {
            if (offset + 16 > size) break;
            box_size = read_u64_be(data + offset + 8);
            hdr_len = 16;
        } else if (box_size == 0) {
            box_size = size - offset;
        }

        if (box_size < hdr_len || offset + box_size > size) break;

        if (box_type == FOURCC('m','o','o','v')) {
            parse_moov(data + offset + hdr_len, box_size - hdr_len, &pt);
            break;
        }

        offset += box_size;
    }

    if (pt.codec == WAYWAL_CODEC_UNKNOWN || !pt.stsz_data || (!pt.stco_data && !pt.co64_data) || !pt.stsc_data) {
        WAYWAL_LOG_WARN("Failed to demux MP4 container (unsupported codec or missing index tables)");
        return NULL;
    }

    /* Parse sample count from stsz */
    if (pt.stsz_size < 12) return NULL;
    uint32_t uniform_sample_size = read_u32_be(pt.stsz_data + 4);
    uint32_t sample_count = read_u32_be(pt.stsz_data + 8);
    if (sample_count == 0 || sample_count > 1000000) return NULL;

    /* Parse chunk count from stco or co64 */
    uint32_t chunk_count = 0;
    const uint8_t *chunk_offsets_ptr = NULL;
    bool is_64bit_co = false;

    if (pt.co64_data && pt.co64_size >= 8) {
        chunk_count = read_u32_be(pt.co64_data + 4);
        chunk_offsets_ptr = pt.co64_data + 8;
        is_64bit_co = true;
    } else if (pt.stco_data && pt.stco_size >= 8) {
        chunk_count = read_u32_be(pt.stco_data + 4);
        chunk_offsets_ptr = pt.stco_data + 8;
    }
    if (chunk_count == 0) return NULL;

    /* Parse stsc (sample to chunk) */
    if (pt.stsc_size < 8) return NULL;
    uint32_t stsc_entry_count = read_u32_be(pt.stsc_data + 4);
    const uint8_t *stsc_entries = pt.stsc_data + 8;

    /* Build sample table */
    mp4_sample_entry_t *samples = (mp4_sample_entry_t *)calloc(sample_count, sizeof(mp4_sample_entry_t));
    if (!samples) return NULL;

    /* Fill sample sizes */
    if (uniform_sample_size > 0) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            samples[i].size = uniform_sample_size;
        }
    } else {
        const uint8_t *sizes_table = pt.stsz_data + 12;
        for (uint32_t i = 0; i < sample_count && (i + 1) * 4 <= pt.stsz_size - 12; ++i) {
            samples[i].size = read_u32_be(sizes_table + i * 4);
        }
    }

    /* Fill sample offsets via chunk mapping */
    uint32_t cur_sample = 0;
    uint32_t stsc_idx = 0;
    uint32_t cur_samples_per_chunk = 0;

    for (uint32_t chunk_idx = 1; chunk_idx <= chunk_count && cur_sample < sample_count; ++chunk_idx) {
        if (stsc_idx < stsc_entry_count) {
            const uint8_t *ent = stsc_entries + stsc_idx * 12;
            uint32_t first_chunk = read_u32_be(ent);
            if (chunk_idx >= first_chunk) {
                cur_samples_per_chunk = read_u32_be(ent + 4);
                if (stsc_idx + 1 < stsc_entry_count) {
                    uint32_t next_first = read_u32_be(ent + 12);
                    if (chunk_idx + 1 >= next_first) {
                        stsc_idx++;
                    }
                }
            }
        }

        uint64_t chunk_offset = is_64bit_co ? read_u64_be(chunk_offsets_ptr + (chunk_idx - 1) * 8)
                                            : read_u32_be(chunk_offsets_ptr + (chunk_idx - 1) * 4);

        for (uint32_t s = 0; s < cur_samples_per_chunk && cur_sample < sample_count; ++s) {
            samples[cur_sample].offset = chunk_offset;
            chunk_offset += samples[cur_sample].size;
            cur_sample++;
        }
    }

    /* Fill timestamps via stts */
    int64_t cur_dts = 0;
    uint32_t stts_sample_idx = 0;
    if (pt.stts_data && pt.stts_size >= 8) {
        uint32_t stts_count = read_u32_be(pt.stts_data + 4);
        const uint8_t *stts_entries = pt.stts_data + 8;
        for (uint32_t i = 0; i < stts_count && stts_sample_idx < sample_count; ++i) {
            uint32_t count = read_u32_be(stts_entries + i * 8);
            uint32_t delta = read_u32_be(stts_entries + i * 8 + 4);
            int64_t delta_us = (int64_t)delta * 1000000LL / (int64_t)pt.timescale;

            for (uint32_t j = 0; j < count && stts_sample_idx < sample_count; ++j) {
                samples[stts_sample_idx].dts_us = cur_dts;
                samples[stts_sample_idx].pts_us = cur_dts; /* Default pts = dts */
                cur_dts += delta_us;
                stts_sample_idx++;
            }
        }
    }

    /* Mark keyframes via stss */
    if (pt.stss_data && pt.stss_size >= 8) {
        uint32_t stss_count = read_u32_be(pt.stss_data + 4);
        const uint8_t *stss_entries = pt.stss_data + 8;
        for (uint32_t i = 0; i < stss_count; ++i) {
            uint32_t s_idx = read_u32_be(stss_entries + i * 4);
            if (s_idx > 0 && s_idx <= sample_count) {
                samples[s_idx - 1].is_keyframe = true;
            }
        }
    } else {
        /* If no sync sample table, all samples are keyframes (e.g. MJPEG or intra-only) */
        for (uint32_t i = 0; i < sample_count; ++i) {
            samples[i].is_keyframe = true;
        }
    }

    /* Allocate demuxer object */
    demuxer_t *d = (demuxer_t *)calloc(1, sizeof(demuxer_t));
    if (!d) {
        free(samples);
        return NULL;
    }

    mp4_state_t *st = (mp4_state_t *)calloc(1, sizeof(mp4_state_t));
    if (!st) {
        free(samples);
        free(d);
        return NULL;
    }

    st->file_fd = -1;
    st->samples = samples;
    st->num_samples = sample_count;
    st->current_sample_idx = 0;

    d->mapped_buf = data;
    d->mapped_size = size;
    d->codec = pt.codec;
    d->width = pt.width;
    d->height = pt.height;
    d->fps_num = (sample_count > 1 && cur_dts > 0) ? (uint32_t)((int64_t)sample_count * 1000000LL / cur_dts) : 30;
    d->fps_den = 1;
    d->duration_us = cur_dts;
    d->extradata = pt.extradata;
    d->extradata_size = pt.extradata_size;
    d->internal_state = st;

    d->read_packet = mp4_read_packet;
    d->seek_us = mp4_seek_us;
    d->destroy = mp4_destroy;

    WAYWAL_LOG_INFO("Demuxer initialized: codec=%d, %ux%u @ %u fps (samples: %u, duration: %.2fs)",
                  d->codec, d->width, d->height, d->fps_num, sample_count, (double)d->duration_us / 1e6);
    return d;
}

demuxer_t *demuxer_open_file(const char *filepath) {
    if (!filepath) return NULL;

    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        WAYWAL_LOG_ERR("Failed to open video file '%s': %s", filepath, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    void *mapped = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for video file '%s': %s", filepath, strerror(errno));
        close(fd);
        return NULL;
    }

    demuxer_t *d = demuxer_open_mem((const uint8_t *)mapped, file_size);
    if (!d) {
        munmap(mapped, file_size);
        close(fd);
        return NULL;
    }

    typedef struct {
        int file_fd;
        bool owns_mmap;
    } demuxer_common_state_t;
    demuxer_common_state_t *state = (demuxer_common_state_t *)d->internal_state;
    state->file_fd = fd;
    state->owns_mmap = true;
    return d;
}
