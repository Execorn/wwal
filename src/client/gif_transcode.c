// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/gif_transcode.h"

#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool is_gif_file(const char *filepath)
{
    if (!filepath)
        return false;
    FILE *fp = fopen(filepath, "rb");
    if (!fp)
        return false;
    uint8_t magic[6];
    size_t n = fread(magic, 1, 6, fp);
    fclose(fp);
    if (n < 6)
        return false;
    return (memcmp(magic, "GIF87a", 6) == 0 || memcmp(magic, "GIF89a", 6) == 0);
}

static bool mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return false;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST)
                    return false;
                struct stat st;
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
                    return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST)
            return false;
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
            return false;
    }
    return true;
}

#define WAYWAL_GIF_TRANSCODE_VERSION 0x56355F4254373039ULL /* "V5_BT709" */

static bool compute_file_hash(const char *filepath, uint32_t max_w, uint32_t max_h, char *out_hex,
                              size_t out_size)
{
    if (out_size < 17)
        return false;

    struct stat st;
    if (stat(filepath, &st) < 0)
        return false;

    uint64_t hash = 14695981039346656037ULL ^ WAYWAL_GIF_TRANSCODE_VERSION;
    uint64_t sz = (uint64_t)st.st_size;
    uint64_t mt = (uint64_t)st.st_mtime;
    uint64_t dims = ((uint64_t)max_w << 32) | (uint64_t)max_h;

    for (int i = 0; i < 8; ++i) {
        hash ^= (sz >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
        hash ^= (mt >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
        hash ^= (dims >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp)
        return false;

    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    fclose(fp);

    snprintf(out_hex, out_size, "%016" PRIx64, hash);
    return true;
}

static bool get_cache_dir(char *out_dir, size_t out_size)
{
    const char *env_cache = getenv("WWAL_CACHE_DIR");
    if (env_cache && env_cache[0]) {
        snprintf(out_dir, out_size, "%s/transcoded", env_cache);
        return mkdir_p(out_dir);
    }
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out_dir, out_size, "%s/wwal/transcoded", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return false;
        snprintf(out_dir, out_size, "%s/.cache/wwal/transcoded", home);
    }
    return mkdir_p(out_dir);
}

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t read_be64(const uint8_t *p)
{
    return ((uint64_t)read_be32(p) << 32) | (uint64_t)read_be32(p + 4);
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
    int zeros;
} sps_bitreader_t;

static void sps_br_init(sps_bitreader_t *br, const uint8_t *data, size_t size)
{
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->zeros = 0;
    if (size > 0 && data[0] == 0)
        br->zeros = 1;
}

static uint32_t sps_br_get_bit(sps_bitreader_t *br)
{
    if (br->byte_pos >= br->size)
        return 0;
    uint32_t bit = (br->data[br->byte_pos] >> (7 - br->bit_pos)) & 1U;
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
        if (br->byte_pos < br->size) {
            if (br->zeros >= 2 && br->data[br->byte_pos] == 0x03) {
                br->byte_pos++;
                br->zeros = 0;
            }
            if (br->byte_pos < br->size) {
                if (br->data[br->byte_pos] == 0x00)
                    br->zeros++;
                else
                    br->zeros = 0;
            }
        }
    }
    return bit;
}

static uint32_t sps_br_get_bits(sps_bitreader_t *br, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; ++i)
        v = (v << 1) | sps_br_get_bit(br);
    return v;
}

static uint32_t sps_br_get_ue(sps_bitreader_t *br)
{
    int lz = 0;
    while (sps_br_get_bit(br) == 0 && lz < 31 && br->byte_pos < br->size)
        lz++;
    if (lz == 0)
        return 0;
    return ((1U << lz) - 1U) + sps_br_get_bits(br, lz);
}

static int32_t sps_br_get_se(sps_bitreader_t *br)
{
    uint32_t ue = sps_br_get_ue(br);
    return (ue & 1U) ? (int32_t)((ue + 1U) >> 1) : -(int32_t)(ue >> 1);
}

static void sps_skip_scaling_list(sps_bitreader_t *br, int size)
{
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size; ++j) {
        if (next_scale != 0) {
            int32_t delta = sps_br_get_se(br);
            next_scale = (last_scale + delta + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

static uint32_t parse_avcc_num_ref_frames(const uint8_t *avcc, size_t size)
{
    if (size < 8)
        return 16;
    uint8_t num_sps = avcc[5] & 0x1F;
    size_t pos = 6;
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (pos + 2 > size)
            break;
        uint16_t sps_len = (uint16_t)((avcc[pos] << 8) | avcc[pos + 1]);
        pos += 2;
        if (pos + sps_len > size || sps_len < 4)
            break;
        sps_bitreader_t br;
        sps_br_init(&br, avcc + pos + 1, sps_len - 1);
        uint32_t profile_idc = sps_br_get_bits(&br, 8);
        sps_br_get_bits(&br, 8);
        sps_br_get_bits(&br, 8);
        sps_br_get_ue(&br);
        if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
            profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
            profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
            uint32_t chroma = sps_br_get_ue(&br);
            if (chroma == 3)
                sps_br_get_bit(&br);
            sps_br_get_ue(&br);
            sps_br_get_ue(&br);
            sps_br_get_bit(&br);
            if (sps_br_get_bit(&br)) {
                int count = (chroma != 3) ? 8 : 12;
                for (int j = 0; j < count; ++j) {
                    if (sps_br_get_bit(&br))
                        sps_skip_scaling_list(&br, (j < 6) ? 16 : 64);
                }
            }
        }
        sps_br_get_ue(&br);
        uint32_t poc_type = sps_br_get_ue(&br);
        if (poc_type == 0) {
            sps_br_get_ue(&br);
        } else if (poc_type == 1) {
            sps_br_get_bit(&br);
            sps_br_get_se(&br);
            sps_br_get_se(&br);
            uint32_t n = sps_br_get_ue(&br);
            for (uint32_t j = 0; j < n && j < 256; ++j)
                sps_br_get_se(&br);
        }
        return sps_br_get_ue(&br);
    }
    return 16;
}

typedef struct {
    bool valid;
    bool is_h264;
    bool is_supported_codec;
    bool has_bframes;
    uint32_t width;
    uint32_t height;
    uint32_t num_ref_frames;
    uint64_t duration_us;
    uint64_t bitrate_bps;
} mp4_probe_t;

static void parse_mp4_boxes(const uint8_t *data, size_t start, size_t end, mp4_probe_t *probe,
                            int depth)
{
    if (depth > 8)
        return;
    size_t pos = start;
    while (pos + 8 <= end) {
        uint64_t box_size = read_be32(data + pos);
        uint32_t box_type = read_be32(data + pos + 4);
        size_t hdr_size = 8;
        if (box_size == 1) {
            if (pos + 16 > end)
                break;
            box_size = read_be64(data + pos + 8);
            hdr_size = 16;
        } else if (box_size == 0) {
            box_size = end - pos;
        }
        if (box_size < hdr_size || pos + box_size > end)
            break;

        size_t payload = pos + hdr_size;
        size_t box_end = pos + box_size;

        if (box_type == 0x6D6F6F76 || box_type == 0x7472616B || box_type == 0x6D646961 ||
            box_type == 0x6D696E66 || box_type == 0x7374626C) {
            parse_mp4_boxes(data, payload, box_end, probe, depth + 1);
        } else if (box_type == 0x6D766864 && payload + 20 <= box_end) {
            /* mvhd */
            uint8_t version = data[payload];
            if (version == 1 && payload + 32 <= box_end) {
                uint32_t timescale = read_be32(data + payload + 20);
                uint64_t duration = read_be64(data + payload + 24);
                if (timescale > 0)
                    probe->duration_us = (duration * 1000000ULL) / timescale;
            } else if (version == 0) {
                uint32_t timescale = read_be32(data + payload + 12);
                uint32_t duration = read_be32(data + payload + 16);
                if (timescale > 0)
                    probe->duration_us = ((uint64_t)duration * 1000000ULL) / timescale;
            }
        } else if (box_type == 0x63747473 && payload + 8 <= box_end) {
            /* ctts (composition time offset indicates B-frame reordering) */
            uint32_t entry_count = read_be32(data + payload + 4);
            if (entry_count > 0)
                probe->has_bframes = true;
        } else if (box_type == 0x73747364 && payload + 8 <= box_end) {
            /* stsd */
            size_t entry_pos = payload + 8;
            if (entry_pos + 86 <= box_end) {
                uint32_t entry_size = read_be32(data + entry_pos);
                uint32_t entry_type = read_be32(data + entry_pos + 4);
                if (entry_size >= 86 && entry_pos + entry_size <= box_end) {
                    if (entry_type == 0x61766331 || entry_type == 0x61766333 ||
                        entry_type == 0x68657631 || entry_type == 0x68766331 ||
                        entry_type == 0x76703039 || entry_type == 0x61763031) {
                        const uint8_t *vse = data + entry_pos + 8;
                        probe->width = (uint32_t)((vse[24] << 8) | vse[25]);
                        probe->height = (uint32_t)((vse[26] << 8) | vse[27]);
                        probe->valid = (probe->width > 0 && probe->height > 0);
                        probe->is_supported_codec = true;
                        if (entry_type == 0x61766331 || entry_type == 0x61766333) {
                            probe->is_h264 = true;
                            parse_mp4_boxes(data, entry_pos + 86, entry_pos + entry_size, probe,
                                            depth + 1);
                        }
                    }
                }
            }
        } else if (box_type == 0x61766343 && payload < box_end) {
            /* avcC */
            probe->num_ref_frames = parse_avcc_num_ref_frames(data + payload, box_end - payload);
        }

        pos += box_size;
    }
}

static bool ebml_probe_vint(const uint8_t *data, size_t limit, size_t *pos, uint64_t *out_val,
                            bool strip_marker)
{
    if (*pos >= limit)
        return false;
    uint8_t first = data[*pos];
    if (first == 0)
        return false;
    uint32_t len = 1;
    uint8_t mask = 0x80;
    while (len <= 4 && (first & mask) == 0) {
        len++;
        mask >>= 1;
    }
    if (len > 4 || *pos + len > limit)
        return false;
    uint64_t val = strip_marker ? (uint64_t)(first & (mask - 1U)) : (uint64_t)first;
    for (uint32_t k = 1; k < len; ++k) {
        val = (val << 8) | data[*pos + k];
    }
    *pos += len;
    *out_val = val;
    return true;
}

static bool probe_mp4_stream(const char *filepath, mp4_probe_t *out_probe)
{
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->num_ref_frames = 16;

    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 32) {
        close(fd);
        return false;
    }

    size_t map_size = (size_t)st.st_size;
    void *mapped = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED)
        return false;

    const uint8_t *data = (const uint8_t *)mapped;
    if (read_be32(data + 4) == 0x66747970) {
        parse_mp4_boxes(data, 0, map_size, out_probe, 0);
        if (out_probe->duration_us > 0) {
            out_probe->bitrate_bps =
                ((uint64_t)st.st_size * 8ULL * 1000000ULL) / out_probe->duration_us;
        }
    } else if (read_be32(data) == 0x1A45DFA3) {
        size_t scan_lim = map_size < 65536 ? map_size : 65536;
        bool has_codec = false;
        for (size_t i = 0; i + 8 < scan_lim; ++i) {
            if (!has_codec &&
                ((memcmp(data + i, "V_VP9", 5) == 0) || (memcmp(data + i, "V_AV1", 5) == 0) ||
                 (i + 15 <= scan_lim && memcmp(data + i, "V_MPEG4/ISO/AVC", 15) == 0) ||
                 (i + 16 <= scan_lim && memcmp(data + i, "V_MPEGH/ISO/HEVC", 16) == 0))) {
                has_codec = true;
            }
            if (data[i] == 0xE0 && out_probe->width == 0) {
                size_t vpos = i + 1;
                uint64_t vsize = 0;
                if (ebml_probe_vint(data, scan_lim, &vpos, &vsize, true) && vsize >= 6 &&
                    vsize <= 4096 && vpos + vsize <= scan_lim) {
                    size_t vid_end = vpos + (size_t)vsize;
                    uint32_t w = 0, h = 0;
                    while (vpos < vid_end) {
                        uint64_t cid = 0, csz = 0;
                        if (!ebml_probe_vint(data, vid_end, &vpos, &cid, false) ||
                            !ebml_probe_vint(data, vid_end, &vpos, &csz, true) ||
                            csz > vid_end - vpos) {
                            w = 0;
                            h = 0;
                            break;
                        }
                        if ((cid == 0xB0 || cid == 0xBA) && csz >= 1 && csz <= 4) {
                            uint32_t val = 0;
                            for (uint64_t b = 0; b < csz; ++b)
                                val = (val << 8) | data[vpos + b];
                            if (cid == 0xB0)
                                w = val;
                            else
                                h = val;
                        }
                        vpos += (size_t)csz;
                    }
                    if (vpos == vid_end && w > 0 && w <= 16384 && h > 0 && h <= 16384) {
                        out_probe->width = w;
                        out_probe->height = h;
                    }
                }
            }
        }
        /* Fallback for synthetic/truncated EBML streams lacking a framing TrackVideo (0xE0) box */
        if (out_probe->width == 0 || out_probe->height == 0) {
            for (size_t i = 0; i + 8 < scan_lim; ++i) {
                if (data[i] == 0xB0 && (i == 0 || (data[i - 1] != 0x54 && data[i - 1] != 0x55)) &&
                    (data[i + 1] == 0x82 || data[i + 1] == 0x81) && out_probe->width == 0) {
                    uint32_t w = (data[i + 1] == 0x82)
                                     ? (uint32_t)((data[i + 2] << 8) | data[i + 3])
                                     : (uint32_t)data[i + 2];
                    size_t next = i + (data[i + 1] == 0x82 ? 4 : 3);
                    size_t win_end = next + 32 < scan_lim ? next + 32 : scan_lim;
                    for (size_t j = next; j + 3 < win_end; ++j) {
                        if (data[j] == 0xBA && data[j - 1] != 0x54 &&
                            (data[j + 1] == 0x82 || data[j + 1] == 0x81)) {
                            uint32_t h = (data[j + 1] == 0x82)
                                             ? (uint32_t)((data[j + 2] << 8) | data[j + 3])
                                             : (uint32_t)data[j + 2];
                            if (w > 0 && w <= 16384 && h > 0 && h <= 16384) {
                                out_probe->width = w;
                                out_probe->height = h;
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (has_codec && out_probe->width > 0 && out_probe->height > 0) {
            out_probe->valid = true;
            out_probe->is_supported_codec = true;
        }
    }

    munmap(mapped, map_size);
    return out_probe->valid;
}

typedef enum {
    TRANSCODE_ENC_NVENC = 0,
    TRANSCODE_ENC_VAAPI,
    TRANSCODE_ENC_QSV,
    TRANSCODE_ENC_LIBX264,
} transcode_encoder_t;

static bool run_ffmpeg_transcode(const char *filepath, const char *tmp_mp4, const char *vf_arg,
                                 transcode_encoder_t enc)
{
    if (enc == TRANSCODE_ENC_VAAPI && access("/dev/dri/renderD128", R_OK | W_OK) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        WAYWAL_LOG_ERR("fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (enc != TRANSCODE_ENC_LIBX264) {
                /* Silence hardware encoder probe failures when falling back through vendors */
                dup2(devnull, STDERR_FILENO);
            }
            close(devnull);
        }

        if (enc == TRANSCODE_ENC_NVENC) {
            char *const args[] = {"ffmpeg",
                                  "-y",
                                  "-v",
                                  "error",
                                  "-i",
                                  (char *)filepath,
                                  "-vf",
                                  (char *)vf_arg,
                                  "-c:v",
                                  "h264_nvenc",
                                  "-preset",
                                  "p4",
                                  "-rc",
                                  "vbr",
                                  "-cq",
                                  "24",
                                  "-maxrate",
                                  "10M",
                                  "-bufsize",
                                  "20M",
                                  "-bf",
                                  "0",
                                  "-b_ref_mode",
                                  "0",
                                  "-dpb_size",
                                  "1",
                                  "-refs",
                                  "1",
                                  "-g",
                                  "60",
                                  "-pix_fmt",
                                  "yuv420p",
                                  "-colorspace",
                                  "bt709",
                                  "-color_primaries",
                                  "bt709",
                                  "-color_trc",
                                  "bt709",
                                  "-color_range",
                                  "tv",
                                  "-movflags",
                                  "+faststart",
                                  "-an",
                                  (char *)tmp_mp4,
                                  NULL};
            execvp("ffmpeg", args);
        } else if (enc == TRANSCODE_ENC_VAAPI) {
            char vaapi_vf[600];
            snprintf(vaapi_vf, sizeof(vaapi_vf), "%s,format=nv12,hwupload", vf_arg);
            char *const args[] = {"ffmpeg",
                                  "-y",
                                  "-v",
                                  "error",
                                  "-vaapi_device",
                                  "/dev/dri/renderD128",
                                  "-i",
                                  (char *)filepath,
                                  "-vf",
                                  vaapi_vf,
                                  "-c:v",
                                  "h264_vaapi",
                                  "-qp",
                                  "24",
                                  "-maxrate",
                                  "10M",
                                  "-bufsize",
                                  "20M",
                                  "-bf",
                                  "0",
                                  "-g",
                                  "60",
                                  "-colorspace",
                                  "bt709",
                                  "-color_primaries",
                                  "bt709",
                                  "-color_trc",
                                  "bt709",
                                  "-color_range",
                                  "tv",
                                  "-movflags",
                                  "+faststart",
                                  "-an",
                                  (char *)tmp_mp4,
                                  NULL};
            execvp("ffmpeg", args);
        } else if (enc == TRANSCODE_ENC_QSV) {
            char *const args[] = {"ffmpeg",
                                  "-y",
                                  "-v",
                                  "error",
                                  "-i",
                                  (char *)filepath,
                                  "-vf",
                                  (char *)vf_arg,
                                  "-c:v",
                                  "h264_qsv",
                                  "-preset",
                                  "medium",
                                  "-global_quality",
                                  "24",
                                  "-maxrate",
                                  "10M",
                                  "-bufsize",
                                  "20M",
                                  "-bf",
                                  "0",
                                  "-refs",
                                  "1",
                                  "-g",
                                  "60",
                                  "-pix_fmt",
                                  "nv12",
                                  "-colorspace",
                                  "bt709",
                                  "-color_primaries",
                                  "bt709",
                                  "-color_trc",
                                  "bt709",
                                  "-color_range",
                                  "tv",
                                  "-movflags",
                                  "+faststart",
                                  "-an",
                                  (char *)tmp_mp4,
                                  NULL};
            execvp("ffmpeg", args);
        } else {
            char *const args[] = {
                "ffmpeg",
                "-y",
                "-v",
                "error",
                "-i",
                (char *)filepath,
                "-vf",
                (char *)vf_arg,
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-preset",
                "veryfast",
                "-crf",
                "21",
                "-maxrate",
                "10M",
                "-bufsize",
                "20M",
                "-bf",
                "0",
                "-refs",
                "1",
                "-g",
                "60",
                "-slices",
                "1",
                "-x264-params",
                "repeat-headers=1:bframes=0:ref=1:colorprim=bt709:transfer=bt709:colormatrix=bt709",
                "-colorspace",
                "bt709",
                "-color_primaries",
                "bt709",
                "-color_trc",
                "bt709",
                "-color_range",
                "tv",
                "-movflags",
                "+faststart",
                "-an",
                (char *)tmp_mp4,
                NULL};
            execvp("ffmpeg", args);
        }
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            unlink(tmp_mp4);
            return false;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp_mp4);
        return false;
    }

    struct stat st;
    if (stat(tmp_mp4, &st) != 0 || st.st_size <= 512) {
        unlink(tmp_mp4);
        return false;
    }
    return true;
}

bool video_transcode_optimize(const char *filepath, uint32_t max_width, uint32_t max_height,
                              char *out_path, size_t out_path_size)
{
    if (!filepath || !out_path || out_path_size == 0)
        return false;

    if (max_width == 0)
        max_width = 3840;
    if (max_height == 0)
        max_height = 2160;

    bool is_gif = is_gif_file(filepath);
    mp4_probe_t probe;
    memset(&probe, 0, sizeof(probe));

    if (!is_gif) {
        struct stat src_st;
        if (stat(filepath, &src_st) != 0 || src_st.st_size <= 0 ||
            strstr(filepath, "/wwal/transcoded/") != NULL) {
            strncpy(out_path, filepath, out_path_size - 1);
            out_path[out_path_size - 1] = '\0';
            return true;
        }

        if (probe_mp4_stream(filepath, &probe)) {
            bool fits_bounds = (probe.width <= max_width && probe.height <= max_height);
            bool is_supported_hw_stream =
                (probe.is_supported_codec && (!probe.is_h264 || probe.num_ref_frames <= 16));

            if (fits_bounds && is_supported_hw_stream) {
                strncpy(out_path, filepath, out_path_size - 1);
                out_path[out_path_size - 1] = '\0';
                return true;
            }
        }
    }

    char cache_dir[4096];
    if (!get_cache_dir(cache_dir, sizeof(cache_dir))) {
        WAYWAL_LOG_ERR("Failed to create JIT transcode cache directory");
        return false;
    }

    char hash_str[32];
    if (!compute_file_hash(filepath, max_width, max_height, hash_str, sizeof(hash_str))) {
        WAYWAL_LOG_ERR("Failed to compute hash for media file: %s", filepath);
        return false;
    }

    char target_mp4[4200];
    snprintf(target_mp4, sizeof(target_mp4), "%s/%s.mp4", cache_dir, hash_str);

    if (strlen(target_mp4) >= out_path_size) {
        WAYWAL_LOG_ERR("Output buffer too small for cached video path");
        return false;
    }

    struct stat st;
    if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
        WAYWAL_LOG_INFO("Using cached hardware-optimized video: %s", target_mp4);
        strncpy(out_path, target_mp4, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
        return true;
    }

    char lock_path[4200];
    snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", cache_dir, hash_str);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_EX);
        if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
            WAYWAL_LOG_INFO("Using cached hardware-optimized video: %s", target_mp4);
            strncpy(out_path, target_mp4, out_path_size - 1);
            out_path[out_path_size - 1] = '\0';
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return true;
        }
    }

    bool success = false;
    WAYWAL_LOG_INFO("Optimizing media via ffmpeg (target max %ux%u, IPPP refs=1)...", max_width,
                    max_height);

    char tmp_mp4[4200];
    snprintf(tmp_mp4, sizeof(tmp_mp4), "%s/%s.tmp.%d.mp4", cache_dir, hash_str, (int)getpid());

    char vf_arg[512];
    snprintf(vf_arg, sizeof(vf_arg),
             "scale='min(%u,iw)':'min(%u,ih)':force_original_aspect_ratio=decrease:"
             "out_color_matrix=bt709:out_range=tv,pad=ceil(iw/2)*2:ceil(ih/2)*2",
             max_width, max_height);

    bool transcoded = false;
    if (!is_gif && (!probe.valid || (probe.width >= 256 && probe.height >= 256))) {
        transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_NVENC);
        if (!transcoded) {
            transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_VAAPI);
        }
        if (!transcoded) {
            transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_QSV);
        }
    }
    if (!transcoded) {
        transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_LIBX264);
    }

    if (!transcoded) {
        if (!is_gif) {
            WAYWAL_LOG_WARN("ffmpeg optimization failed; falling back to original file: %s",
                            filepath);
            strncpy(out_path, filepath, out_path_size - 1);
            out_path[out_path_size - 1] = '\0';
            success = true;
        } else {
            WAYWAL_LOG_ERR("ffmpeg GIF transcoding failed for %s", filepath);
        }
        goto cleanup;
    }

    if (rename(tmp_mp4, target_mp4) != 0) {
        WAYWAL_LOG_ERR("Failed to rename temporary transcoded file to cache: %s", strerror(errno));
        unlink(tmp_mp4);
        goto cleanup;
    }

    WAYWAL_LOG_INFO("JIT optimization complete: cached hardware-accelerated video to %s",
                    target_mp4);
    strncpy(out_path, target_mp4, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    success = true;

cleanup:
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return success;
}

bool gif_transcode_resolve(const char *filepath, char *out_path, size_t out_path_size)
{
    return video_transcode_optimize(filepath, 2560, 1440, out_path, out_path_size);
}
