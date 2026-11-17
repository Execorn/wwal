// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/gif_transcode.h"

#include "waywal/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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

#define WAYWAL_GIF_TRANSCODE_VERSION 0x56365F415544494FULL /* "V6_AUDIO" */
#define TRANSCODE_HASH_CHUNK_SIZE    65536ULL
#define TRANSCODE_HASH_FULL_LIMIT    (4ULL * TRANSCODE_HASH_CHUNK_SIZE)
#define WWAL_FOURCC(a, b, c, d)                                                                    \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |                             \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

static inline uint64_t hash_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static uint64_t hash_chunk_64(uint64_t seed, const uint8_t *buf, size_t len)
{
    uint64_t h0 = seed ^ 0x9E3779B97F4A7C15ULL;
    uint64_t h1 = seed ^ 0xBF58476D1CE4E5B9ULL;
    uint64_t h2 = seed ^ 0x94D049BB133111EBULL;
    uint64_t h3 = seed ^ 0xD6E8FEB86659FD93ULL;

    size_t i = 0;
    while (len - i >= 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, buf + i, 8);
        memcpy(&w1, buf + i + 8, 8);
        memcpy(&w2, buf + i + 16, 8);
        memcpy(&w3, buf + i + 24, 8);
        h0 = (h0 ^ w0) * 1099511628211ULL;
        h1 = (h1 ^ w1) * 0x9E3779B97F4A7C15ULL;
        h2 = (h2 ^ w2) * 0xBF58476D1CE4E5B9ULL;
        h3 = (h3 ^ w3) * 0x94D049BB133111EBULL;
        i += 32;
    }
    while (len - i >= 8) {
        uint64_t w;
        memcpy(&w, buf + i, 8);
        h0 = (h0 ^ w) * 1099511628211ULL;
        i += 8;
    }
    if (i < len) {
        uint64_t tail = 0;
        memcpy(&tail, buf + i, len - i);
        h1 = (h1 ^ tail) * 0x9E3779B97F4A7C15ULL;
    }

    uint64_t combined = h0 ^ ((h1 << 13) | (h1 >> 51)) ^ ((h2 << 29) | (h2 >> 35)) ^
                        ((h3 << 43) | (h3 >> 21)) ^ (uint64_t)len;
    return hash_mix64(combined);
}

static bool hash_fd_chunk(int fd, uint64_t offset, size_t len, uint8_t *buf, uint64_t *hash)
{
    size_t total = 0;
    while (total < len) {
        ssize_t r = pread(fd, buf + total, len - total, (off_t)(offset + total));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            break;
        total += (size_t)r;
    }
    *hash = hash_mix64(*hash ^ offset);
    *hash = hash_chunk_64(*hash, buf, total);
    return true;
}

static bool compute_file_hash(const char *filepath, uint32_t max_w, uint32_t max_h, char *out_hex,
                              size_t out_size)
{
    if (!filepath || !out_hex || out_size < 17)
        return false;

    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0) {
        close(fd);
        return false;
    }

    uint64_t sz = (uint64_t)st.st_size;
    uint64_t mt_sec = (uint64_t)st.st_mtim.tv_sec;
    uint64_t mt_nsec = (uint64_t)st.st_mtim.tv_nsec;
    uint64_t dims = ((uint64_t)max_w << 32) | (uint64_t)max_h;

    uint64_t hash = 14695981039346656037ULL ^ WAYWAL_GIF_TRANSCODE_VERSION;
    hash = hash_mix64(hash ^ sz);
    hash = hash_mix64(hash ^ mt_sec);
    hash = hash_mix64(hash ^ mt_nsec);
    hash = hash_mix64(hash ^ dims);

    uint8_t buf[TRANSCODE_HASH_CHUNK_SIZE];
    if (S_ISREG(st.st_mode)) {
        if (sz > 0 && sz <= TRANSCODE_HASH_FULL_LIMIT) {
            for (uint64_t off = 0; off < sz; off += TRANSCODE_HASH_CHUNK_SIZE) {
                uint64_t rem = sz - off;
                size_t chunk_len = (rem > TRANSCODE_HASH_CHUNK_SIZE)
                                       ? (size_t)TRANSCODE_HASH_CHUNK_SIZE
                                       : (size_t)rem;
                if (!hash_fd_chunk(fd, off, chunk_len, buf, &hash)) {
                    close(fd);
                    return false;
                }
            }
        } else if (sz > TRANSCODE_HASH_FULL_LIMIT) {
            /* Bound large-file hashing to four 64 KiB samples for O(1) cache lookup latency */
            const uint64_t offsets[4] = {
                0ULL,
                (sz / 3ULL) & ~4095ULL,
                ((sz / 3ULL) * 2ULL) & ~4095ULL,
                sz - TRANSCODE_HASH_CHUNK_SIZE,
            };
            for (size_t k = 0; k < 4; ++k) {
                uint64_t off = offsets[k];
                uint64_t rem = sz - off;
                size_t chunk_len = (rem > TRANSCODE_HASH_CHUNK_SIZE)
                                       ? (size_t)TRANSCODE_HASH_CHUNK_SIZE
                                       : (size_t)rem;
                if (!hash_fd_chunk(fd, off, chunk_len, buf, &hash)) {
                    close(fd);
                    return false;
                }
            }
        }
    } else {
        uint64_t seen = 0;
        while (seen < TRANSCODE_HASH_FULL_LIMIT) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                close(fd);
                return false;
            }
            if (r == 0)
                break;
            hash = hash_chunk_64(hash, buf, (size_t)r);
            seen += (uint64_t)r;
        }
    }

    close(fd);
    snprintf(out_hex, out_size, "%016" PRIx64, hash);
    return true;
}

static bool get_cache_dir(char *out_dir, size_t out_size)
{
    int n = -1;
    const char *env_cache = getenv("WWAL_CACHE_DIR");
    if (env_cache && env_cache[0]) {
        n = snprintf(out_dir, out_size, "%s/transcoded", env_cache);
    } else {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && xdg[0]) {
            n = snprintf(out_dir, out_size, "%s/wwal/transcoded", xdg);
        } else {
            const char *home = getenv("HOME");
            if (!home || !home[0])
                return false;
            n = snprintf(out_dir, out_size, "%s/.cache/wwal/transcoded", home);
        }
    }
    if (n < 0 || (size_t)n >= out_size)
        return false;
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

#ifdef __SIZEOF_INT128__
__extension__ typedef unsigned __int128 wwal_u128_t;
#endif

static uint64_t scale_duration_us(uint64_t duration, uint32_t timescale)
{
    if (timescale == 0 || duration == 0)
        return 0;
#ifdef __SIZEOF_INT128__
    wwal_u128_t scaled = ((wwal_u128_t)duration * 1000000ULL) / timescale;
    return (scaled > UINT64_MAX) ? UINT64_MAX : (uint64_t)scaled;
#else
    uint64_t q = duration / timescale;
    uint64_t r = duration % timescale;
    uint64_t hi = 0;
    if (__builtin_mul_overflow(q, 1000000ULL, &hi))
        return UINT64_MAX;
    uint64_t lo = (r * 1000000ULL) / timescale;
    uint64_t total = 0;
    if (__builtin_add_overflow(hi, lo, &total))
        return UINT64_MAX;
    return total;
#endif
}

static uint64_t compute_bitrate_bps(uint64_t file_bytes, uint64_t duration_us)
{
    if (duration_us == 0 || file_bytes == 0)
        return 0;
#ifdef __SIZEOF_INT128__
    wwal_u128_t bps = ((wwal_u128_t)file_bytes * 8000000ULL) / duration_us;
    return (bps > UINT64_MAX) ? UINT64_MAX : (uint64_t)bps;
#else
    uint64_t q = file_bytes / duration_us;
    uint64_t r = file_bytes % duration_us;
    uint64_t hi = 0;
    if (__builtin_mul_overflow(q, 8000000ULL, &hi))
        return UINT64_MAX;
    uint64_t r_scaled = 0;
    uint64_t lo = 0;
    if (!__builtin_mul_overflow(r, 8000000ULL, &r_scaled)) {
        lo = r_scaled / duration_us;
    } else {
        uint64_t denom = duration_us / 8000000ULL;
        lo = denom > 0 ? r / denom : UINT64_MAX;
    }
    uint64_t total = 0;
    if (__builtin_add_overflow(hi, lo, &total))
        return UINT64_MAX;
    return total;
#endif
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
    int zeros;
    bool overrun;
} sps_bitreader_t;

static void sps_br_init(sps_bitreader_t *br, const uint8_t *data, size_t size)
{
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->zeros = (size > 0 && data && data[0] == 0) ? 1 : 0;
    br->overrun = (!data || size == 0);
}

static uint32_t sps_br_get_bit(sps_bitreader_t *br)
{
    if (br->overrun || br->byte_pos >= br->size) {
        br->overrun = true;
        return 0;
    }
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
    for (int i = 0; i < n && !br->overrun; ++i)
        v = (v << 1) | sps_br_get_bit(br);
    return v;
}

static uint32_t sps_br_get_ue(sps_bitreader_t *br)
{
    int lz = 0;
    while (!br->overrun && sps_br_get_bit(br) == 0) {
        lz++;
        if (lz >= 31) {
            br->overrun = true;
            return 0;
        }
    }
    if (br->overrun || lz == 0)
        return 0;
    uint32_t suffix = sps_br_get_bits(br, lz);
    if (br->overrun)
        return 0;
    return ((1U << lz) - 1U) + suffix;
}

static int32_t sps_br_get_se(sps_bitreader_t *br)
{
    uint32_t ue = sps_br_get_ue(br);
    if (br->overrun)
        return 0;
    return (ue & 1U) ? (int32_t)((ue + 1U) >> 1) : -(int32_t)(ue >> 1);
}

static void sps_skip_scaling_list(sps_bitreader_t *br, int size)
{
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size && !br->overrun; ++j) {
        if (next_scale != 0) {
            int32_t delta = sps_br_get_se(br);
            if (br->overrun)
                return;
            int64_t sum = ((int64_t)last_scale + (int64_t)delta) % 256;
            if (sum < 0)
                sum += 256;
            next_scale = (int)sum;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

static uint32_t parse_avcc_num_ref_frames(const uint8_t *avcc, size_t size)
{
    if (!avcc || size < 8)
        return 16;
    uint8_t num_sps = avcc[5] & 0x1F;
    size_t pos = 6;
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (size - pos < 2)
            break;
        uint16_t sps_len = (uint16_t)(((uint16_t)avcc[pos] << 8) | (uint16_t)avcc[pos + 1]);
        pos += 2;
        if (sps_len < 4 || (size_t)sps_len > size - pos)
            break;
        sps_bitreader_t br;
        sps_br_init(&br, avcc + pos + 1, (size_t)sps_len - 1);
        pos += (size_t)sps_len;

        uint32_t profile_idc = sps_br_get_bits(&br, 8);
        sps_br_get_bits(&br, 8);
        sps_br_get_bits(&br, 8);
        sps_br_get_ue(&br);
        if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
            profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
            profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
            uint32_t chroma = sps_br_get_ue(&br);
            if (br.overrun || chroma > 3)
                continue;
            if (chroma == 3)
                sps_br_get_bit(&br);
            sps_br_get_ue(&br);
            sps_br_get_ue(&br);
            sps_br_get_bit(&br);
            if (!br.overrun && sps_br_get_bit(&br)) {
                int count = (chroma != 3) ? 8 : 12;
                for (int j = 0; j < count && !br.overrun; ++j) {
                    if (sps_br_get_bit(&br))
                        sps_skip_scaling_list(&br, (j < 6) ? 16 : 64);
                }
            }
        }
        sps_br_get_ue(&br);
        uint32_t poc_type = sps_br_get_ue(&br);
        if (br.overrun)
            continue;
        if (poc_type == 0) {
            sps_br_get_ue(&br);
        } else if (poc_type == 1) {
            sps_br_get_bit(&br);
            sps_br_get_se(&br);
            sps_br_get_se(&br);
            uint32_t n = sps_br_get_ue(&br);
            if (br.overrun || n > 255)
                continue;
            for (uint32_t j = 0; j < n && !br.overrun; ++j)
                sps_br_get_se(&br);
        }
        uint32_t num_ref_frames = sps_br_get_ue(&br);
        if (br.overrun)
            continue;
        return num_ref_frames;
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
    if (depth > 8 || !data || !probe || start > end)
        return;
    size_t pos = start;
    while (end - pos >= 8) {
        uint64_t box_size = read_be32(data + pos);
        uint32_t box_type = read_be32(data + pos + 4);
        size_t hdr_size = 8;
        if (box_size == 1) {
            if (end - pos < 16)
                break;
            box_size = read_be64(data + pos + 8);
            hdr_size = 16;
        } else if (box_size == 0) {
            box_size = (uint64_t)(end - pos);
        }
        if (box_size < (uint64_t)hdr_size || box_size > (uint64_t)(end - pos))
            break;

        size_t payload = pos + hdr_size;
        size_t box_end = pos + (size_t)box_size;
        size_t payload_len = box_end - payload;

        if (box_type == WWAL_FOURCC('m', 'o', 'o', 'v') ||
            box_type == WWAL_FOURCC('t', 'r', 'a', 'k') ||
            box_type == WWAL_FOURCC('m', 'd', 'i', 'a') ||
            box_type == WWAL_FOURCC('m', 'i', 'n', 'f') ||
            box_type == WWAL_FOURCC('s', 't', 'b', 'l')) {
            parse_mp4_boxes(data, payload, box_end, probe, depth + 1);
        } else if (box_type == WWAL_FOURCC('m', 'v', 'h', 'd') && payload_len >= 20) {
            uint8_t version = data[payload];
            if (version == 1 && payload_len >= 32) {
                uint32_t timescale = read_be32(data + payload + 20);
                uint64_t duration = read_be64(data + payload + 24);
                if (timescale > 0)
                    probe->duration_us = scale_duration_us(duration, timescale);
            } else if (version == 0) {
                uint32_t timescale = read_be32(data + payload + 12);
                uint32_t duration = read_be32(data + payload + 16);
                if (timescale > 0)
                    probe->duration_us = scale_duration_us((uint64_t)duration, timescale);
            }
        } else if (box_type == WWAL_FOURCC('c', 't', 't', 's') && payload_len >= 8) {
            /* Non-empty ctts implies B-frame reordering */
            uint32_t entry_count = read_be32(data + payload + 4);
            if (entry_count > 0)
                probe->has_bframes = true;
        } else if (box_type == WWAL_FOURCC('s', 't', 's', 'd') && payload_len >= 8) {
            uint32_t entry_count = read_be32(data + payload + 4);
            size_t entry_pos = payload + 8;
            for (uint32_t ei = 0; ei < entry_count && box_end - entry_pos >= 8; ++ei) {
                uint32_t entry_size = read_be32(data + entry_pos);
                uint32_t entry_type = read_be32(data + entry_pos + 4);
                if (entry_size < 8 || (uint64_t)entry_size > (uint64_t)(box_end - entry_pos))
                    break;
                if (entry_size >= 86 && (entry_type == WWAL_FOURCC('a', 'v', 'c', '1') ||
                                         entry_type == WWAL_FOURCC('a', 'v', 'c', '3') ||
                                         entry_type == WWAL_FOURCC('h', 'e', 'v', '1') ||
                                         entry_type == WWAL_FOURCC('h', 'v', 'c', '1') ||
                                         entry_type == WWAL_FOURCC('v', 'p', '0', '9') ||
                                         entry_type == WWAL_FOURCC('a', 'v', '0', '1'))) {
                    const uint8_t *vse = data + entry_pos + 8;
                    probe->width = (uint32_t)(((uint32_t)vse[24] << 8) | (uint32_t)vse[25]);
                    probe->height = (uint32_t)(((uint32_t)vse[26] << 8) | (uint32_t)vse[27]);
                    probe->valid = (probe->width > 0 && probe->height > 0);
                    probe->is_supported_codec = true;
                    if (entry_type == WWAL_FOURCC('a', 'v', 'c', '1') ||
                        entry_type == WWAL_FOURCC('a', 'v', 'c', '3')) {
                        probe->is_h264 = true;
                        parse_mp4_boxes(data, entry_pos + 86, entry_pos + (size_t)entry_size, probe,
                                        depth + 1);
                    }
                    break;
                }
                entry_pos += (size_t)entry_size;
            }
        } else if (box_type == WWAL_FOURCC('a', 'v', 'c', 'C') && payload_len > 0) {
            probe->num_ref_frames = parse_avcc_num_ref_frames(data + payload, payload_len);
        }

        pos = box_end;
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
    if (len > 4 || (size_t)len > limit - *pos)
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
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 32 ||
        (uint64_t)st.st_size > SIZE_MAX) {
        close(fd);
        return false;
    }

    size_t map_size = (size_t)st.st_size;
    void *mapped = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED)
        return false;

    const uint8_t *data = (const uint8_t *)mapped;
    if (read_be32(data + 4) == WWAL_FOURCC('f', 't', 'y', 'p')) {
        parse_mp4_boxes(data, 0, map_size, out_probe, 0);
        if (out_probe->duration_us > 0) {
            out_probe->bitrate_bps =
                compute_bitrate_bps((uint64_t)st.st_size, out_probe->duration_us);
        }
    } else if (read_be32(data) == 0x1A45DFA3) {
        size_t scan_lim = map_size < 65536 ? map_size : 65536;
        bool has_codec = false;
        for (size_t i = 0; scan_lim - i > 8; ++i) {
            if (!has_codec &&
                ((memcmp(data + i, "V_VP9", 5) == 0) || (memcmp(data + i, "V_AV1", 5) == 0) ||
                 (scan_lim - i >= 15 && memcmp(data + i, "V_MPEG4/ISO/AVC", 15) == 0) ||
                 (scan_lim - i >= 16 && memcmp(data + i, "V_MPEGH/ISO/HEVC", 16) == 0))) {
                has_codec = true;
            }
            if (data[i] == 0xE0 && out_probe->width == 0) {
                size_t vpos = i + 1;
                uint64_t vsize = 0;
                if (ebml_probe_vint(data, scan_lim, &vpos, &vsize, true) && vsize >= 6 &&
                    vsize <= 4096 && vsize <= (uint64_t)(scan_lim - vpos)) {
                    size_t vid_end = vpos + (size_t)vsize;
                    uint32_t w = 0, h = 0;
                    while (vpos < vid_end) {
                        uint64_t cid = 0, csz = 0;
                        if (!ebml_probe_vint(data, vid_end, &vpos, &cid, false) ||
                            !ebml_probe_vint(data, vid_end, &vpos, &csz, true) ||
                            csz > (uint64_t)(vid_end - vpos)) {
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
            for (size_t i = 0; scan_lim - i > 8; ++i) {
                if (data[i] == 0xB0 && (i == 0 || (data[i - 1] != 0x54 && data[i - 1] != 0x55)) &&
                    (data[i + 1] == 0x82 || data[i + 1] == 0x81) && out_probe->width == 0) {
                    uint32_t w = (data[i + 1] == 0x82)
                                     ? (uint32_t)((data[i + 2] << 8) | data[i + 3])
                                     : (uint32_t)data[i + 2];
                    size_t next = i + (data[i + 1] == 0x82 ? 4 : 3);
                    size_t win_end = (scan_lim - next > 32) ? next + 32 : scan_lim;
                    for (size_t j = next; win_end - j > 3; ++j) {
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

#define MAX_DRM_RENDER_NODES 8

typedef struct {
    char path[64];
    unsigned int minor_num;
} drm_render_node_t;

static bool parse_render_node_minor(const char *name, unsigned int *out_minor)
{
    if (strncmp(name, "renderD", 7) != 0 || name[7] == '\0')
        return false;
    const char *p = name + 7;
    unsigned int val = 0;
    while (*p) {
        if (*p < '0' || *p > '9' || val > 100000U)
            return false;
        val = val * 10U + (unsigned int)(*p - '0');
        p++;
    }
    *out_minor = val;
    return true;
}

static bool add_render_node_candidate(drm_render_node_t *nodes, size_t *count, size_t max_nodes,
                                      const char *dev_name)
{
    unsigned int minor_num = 0;
    if (!parse_render_node_minor(dev_name, &minor_num))
        return false;

    char path[64];
    int n = snprintf(path, sizeof(path), "/dev/dri/%s", dev_name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    for (size_t i = 0; i < *count; ++i) {
        if (strcmp(nodes[i].path, path) == 0)
            return false;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode) || access(path, R_OK | W_OK) != 0)
        return false;

    if (*count >= max_nodes)
        return false;
    memcpy(nodes[*count].path, path, (size_t)n + 1);
    nodes[*count].minor_num = minor_num;
    (*count)++;
    return true;
}

static int cmp_render_node_minor(const void *a, const void *b)
{
    const drm_render_node_t *na = (const drm_render_node_t *)a;
    const drm_render_node_t *nb = (const drm_render_node_t *)b;
    if (na->minor_num < nb->minor_num)
        return -1;
    if (na->minor_num > nb->minor_num)
        return 1;
    return 0;
}

static size_t discover_drm_render_nodes(drm_render_node_t *nodes, size_t max_nodes)
{
    size_t count = 0;

    /* Prefer render nodes backing connected displays before headless or secondary GPUs */
    DIR *sys_dir = opendir("/sys/class/drm");
    if (sys_dir) {
        struct dirent *entry;
        while ((entry = readdir(sys_dir)) != NULL && count < max_nodes) {
            if (entry->d_name[0] == '.')
                continue;
            const char *dash = strchr(entry->d_name, '-');
            if (!dash)
                continue;

            char status_path[512];
            int sn = snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status",
                              entry->d_name);
            if (sn < 0 || (size_t)sn >= sizeof(status_path))
                continue;

            FILE *f = fopen(status_path, "r");
            if (!f)
                continue;
            char status[64] = {0};
            bool connected = false;
            if (fgets(status, sizeof(status), f)) {
                size_t len = strlen(status);
                while (len > 0 && (status[len - 1] == '\n' || status[len - 1] == '\r' ||
                                   status[len - 1] == ' ')) {
                    status[--len] = '\0';
                }
                connected = (strcmp(status, "connected") == 0);
            }
            fclose(f);
            if (!connected)
                continue;

            size_t card_len = (size_t)(dash - entry->d_name);
            if (card_len == 0 || card_len >= 64)
                continue;
            char card_name[64];
            memcpy(card_name, entry->d_name, card_len);
            card_name[card_len] = '\0';

            char drm_path[512];
            int dn =
                snprintf(drm_path, sizeof(drm_path), "/sys/class/drm/%s/device/drm", card_name);
            if (dn < 0 || (size_t)dn >= sizeof(drm_path))
                continue;

            DIR *card_drm = opendir(drm_path);
            if (card_drm) {
                struct dirent *de;
                while ((de = readdir(card_drm)) != NULL && count < max_nodes) {
                    add_render_node_candidate(nodes, &count, max_nodes, de->d_name);
                }
                closedir(card_drm);
            }
        }
        closedir(sys_dir);
    }

    size_t connected_count = count;
    DIR *dri_dir = opendir("/dev/dri");
    if (dri_dir) {
        struct dirent *de;
        while ((de = readdir(dri_dir)) != NULL && count < max_nodes) {
            add_render_node_candidate(nodes, &count, max_nodes, de->d_name);
        }
        closedir(dri_dir);
    }

    if (count > connected_count + 1) {
        qsort(nodes + connected_count, count - connected_count, sizeof(drm_render_node_t),
              cmp_render_node_minor);
    }
    return count;
}

typedef enum {
    TRANSCODE_ENC_NVENC = 0,
    TRANSCODE_ENC_VAAPI,
    TRANSCODE_ENC_QSV,
    TRANSCODE_ENC_LIBX264,
} transcode_encoder_t;

static bool run_ffmpeg_transcode(const char *filepath, const char *tmp_mp4, const char *vf_arg,
                                 transcode_encoder_t enc, const char *vaapi_device, bool keep_audio)
{
    if (enc == TRANSCODE_ENC_VAAPI &&
        (!vaapi_device || !vaapi_device[0] || access(vaapi_device, R_OK | W_OK) != 0)) {
        return false;
    }

    char vaapi_vf[600];
    const char *effective_vf = vf_arg;
    if (enc == TRANSCODE_ENC_VAAPI) {
        int vn = snprintf(vaapi_vf, sizeof(vaapi_vf), "%s,format=nv12,hwupload", vf_arg);
        if (vn < 0 || (size_t)vn >= sizeof(vaapi_vf))
            return false;
        effective_vf = vaapi_vf;
    }

    const char *args[64];
    size_t argc = 0;

    args[argc++] = "ffmpeg";
    args[argc++] = "-y";
    args[argc++] = "-v";
    args[argc++] = "error";
    if (enc == TRANSCODE_ENC_VAAPI) {
        args[argc++] = "-vaapi_device";
        args[argc++] = vaapi_device;
    }
    args[argc++] = "-i";
    args[argc++] = filepath;
    args[argc++] = "-vf";
    args[argc++] = effective_vf;

    if (enc == TRANSCODE_ENC_NVENC) {
        static const char *const nvenc_opts[] = {
            "-c:v",      "h264_nvenc", "-preset",  "p4",  "-rc", "vbr", "-cq",         "24",
            "-maxrate",  "10M",        "-bufsize", "20M", "-bf", "0",   "-b_ref_mode", "0",
            "-dpb_size", "1",          "-refs",    "1",   "-g",  "60",  "-pix_fmt",    "yuv420p",
        };
        for (size_t i = 0; i < sizeof(nvenc_opts) / sizeof(nvenc_opts[0]); ++i)
            args[argc++] = nvenc_opts[i];
    } else if (enc == TRANSCODE_ENC_VAAPI) {
        static const char *const vaapi_opts[] = {
            "-c:v",     "h264_vaapi", "-qp", "24", "-maxrate", "10M",
            "-bufsize", "20M",        "-bf", "0",  "-g",       "60",
        };
        for (size_t i = 0; i < sizeof(vaapi_opts) / sizeof(vaapi_opts[0]); ++i)
            args[argc++] = vaapi_opts[i];
    } else if (enc == TRANSCODE_ENC_QSV) {
        static const char *const qsv_opts[] = {
            "-c:v",     "h264_qsv", "-preset", "medium", "-global_quality", "24", "-maxrate", "10M",
            "-bufsize", "20M",      "-bf",     "0",      "-refs",           "1",  "-g",       "60",
            "-pix_fmt", "nv12",
        };
        for (size_t i = 0; i < sizeof(qsv_opts) / sizeof(qsv_opts[0]); ++i)
            args[argc++] = qsv_opts[i];
    } else {
        static const char *const x264_opts[] = {
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
        };
        for (size_t i = 0; i < sizeof(x264_opts) / sizeof(x264_opts[0]); ++i)
            args[argc++] = x264_opts[i];
    }

    static const char *const color_faststart_opts[] = {
        "-colorspace",  "bt709", "-color_primaries", "bt709",      "-color_trc", "bt709",
        "-color_range", "tv",    "-movflags",        "+faststart",
    };
    for (size_t i = 0; i < sizeof(color_faststart_opts) / sizeof(color_faststart_opts[0]); ++i)
        args[argc++] = color_faststart_opts[i];

    if (keep_audio) {
        args[argc++] = "-c:a";
        args[argc++] = "aac";
        args[argc++] = "-b:a";
        args[argc++] = "128k";
    } else {
        args[argc++] = "-an";
    }
    args[argc++] = tmp_mp4;
    args[argc++] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        WAYWAL_LOG_ERR("fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        if (close_range(3, ~0U, 0) != 0) {
            int max_fd = (int)sysconf(_SC_OPEN_MAX);
            if (max_fd < 256)
                max_fd = 256;
            if (max_fd > 4096)
                max_fd = 4096;
            for (int fd = 3; fd < max_fd; ++fd) {
                close(fd);
            }
        }

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (enc != TRANSCODE_ENC_LIBX264) {
                /* Silence hardware encoder probe failures when falling back through vendors */
                dup2(devnull, STDERR_FILENO);
            }
            close(devnull);
        }

        execvp("ffmpeg", (char *const *)args);
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

static bool copy_out_path(char *out_path, size_t out_path_size, const char *src)
{
    size_t len = strlen(src);
    if (len >= out_path_size)
        return false;
    memcpy(out_path, src, len + 1);
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
            return copy_out_path(out_path, out_path_size, filepath);
        }

        if (probe_mp4_stream(filepath, &probe)) {
            bool fits_bounds = (probe.width <= max_width && probe.height <= max_height);
            bool is_supported_hw_stream =
                (probe.is_supported_codec && (!probe.is_h264 || probe.num_ref_frames <= 4));

            if (fits_bounds && is_supported_hw_stream) {
                return copy_out_path(out_path, out_path_size, filepath);
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
    int tn = snprintf(target_mp4, sizeof(target_mp4), "%s/%s.mp4", cache_dir, hash_str);
    if (tn < 0 || (size_t)tn >= sizeof(target_mp4)) {
        WAYWAL_LOG_ERR("Cached video path exceeds internal buffer");
        return false;
    }

    if ((size_t)tn >= out_path_size) {
        WAYWAL_LOG_ERR("Output buffer too small for cached video path");
        return false;
    }

    struct stat st;
    if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
        WAYWAL_LOG_INFO("Using cached hardware-optimized video: %s", target_mp4);
        return copy_out_path(out_path, out_path_size, target_mp4);
    }

    char lock_path[4200];
    int ln = snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", cache_dir, hash_str);
    int lock_fd = (ln > 0 && (size_t)ln < sizeof(lock_path))
                      ? open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644)
                      : -1;
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_EX);
        if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
            WAYWAL_LOG_INFO("Using cached hardware-optimized video: %s", target_mp4);
            bool copied = copy_out_path(out_path, out_path_size, target_mp4);
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return copied;
        }
    }

    bool success = false;
    WAYWAL_LOG_INFO("Optimizing media via ffmpeg (target max %ux%u, IPPP refs=1)...", max_width,
                    max_height);

    char tmp_mp4[4200];
    int mn =
        snprintf(tmp_mp4, sizeof(tmp_mp4), "%s/%s.tmp.%d.mp4", cache_dir, hash_str, (int)getpid());
    if (mn < 0 || (size_t)mn >= sizeof(tmp_mp4)) {
        WAYWAL_LOG_ERR("Temporary cache path exceeds internal buffer");
        goto cleanup;
    }

    char vf_arg[512];
    snprintf(vf_arg, sizeof(vf_arg),
             "scale='min(%u,iw)':'min(%u,ih)':force_original_aspect_ratio=decrease:"
             "out_color_matrix=bt709:out_range=tv,pad=ceil(iw/2)*2:ceil(ih/2)*2",
             max_width, max_height);

    bool keep_audio = !is_gif;
    bool transcoded = false;
    if (!is_gif && (!probe.valid || (probe.width >= 256 && probe.height >= 256))) {
        transcoded =
            run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_NVENC, NULL, keep_audio);
        if (!transcoded) {
            drm_render_node_t vaapi_nodes[MAX_DRM_RENDER_NODES];
            size_t vaapi_count = discover_drm_render_nodes(vaapi_nodes, MAX_DRM_RENDER_NODES);
            for (size_t ni = 0; ni < vaapi_count && !transcoded; ++ni) {
                transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_VAAPI,
                                                  vaapi_nodes[ni].path, keep_audio);
            }
        }
        if (!transcoded) {
            transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_QSV, NULL,
                                              keep_audio);
        }
    }
    if (!transcoded) {
        transcoded = run_ffmpeg_transcode(filepath, tmp_mp4, vf_arg, TRANSCODE_ENC_LIBX264, NULL,
                                          keep_audio);
    }

    if (!transcoded) {
        if (!is_gif) {
            WAYWAL_LOG_WARN("ffmpeg optimization failed; falling back to original file: %s",
                            filepath);
            success = copy_out_path(out_path, out_path_size, filepath);
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
    success = copy_out_path(out_path, out_path_size, target_mp4);

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
