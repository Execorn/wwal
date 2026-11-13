// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "waywal/we_mdl_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waywal/we_asset_writer.h"
#include "waywal/we_puppet.h"

#define WE_MDL_VERTEX_MAGIC_STR "MDLV0023"
#define WE_MDL_ATTACH_MAGIC_STR "MDAT0001"
#define WE_MDL_SKEL_MAGIC_STR   "MDLS0002"
#define WE_MDL_ANIM_MAGIC_STR   "MDLA0003"
#define WE_MDL_MAGIC_SIZE       9u /* 8 ASCII bytes + NUL terminator */
#define WE_MDL_VERTEX_STRIDE    52u
#define WE_MDL_VERTEX_CURR_FMT  0x01800009u
#define WE_MDL_BONE_MAT_BYTES   64u
#define WE_MDL_KF_STRIDE        36u
#define WE_MDL_MAX_KEYFRAMES    65536u

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
} mdl_cursor_t;

static bool size_add_checked(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool size_mul_checked(size_t a, size_t b, size_t *out)
{
    if (b != 0u && a > SIZE_MAX / b) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool float_array_finite(const float *arr, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        if (!isfinite(arr[i])) {
            return false;
        }
    }
    return true;
}

static void write_u16_le(mdl_cursor_t *c, uint16_t v)
{
    c->buf[c->pos + 0u] = (uint8_t)(v & 0xFFu);
    c->buf[c->pos + 1u] = (uint8_t)((v >> 8u) & 0xFFu);
    c->pos += 2u;
}

static void write_u32_le(mdl_cursor_t *c, uint32_t v)
{
    c->buf[c->pos + 0u] = (uint8_t)(v & 0xFFu);
    c->buf[c->pos + 1u] = (uint8_t)((v >> 8u) & 0xFFu);
    c->buf[c->pos + 2u] = (uint8_t)((v >> 16u) & 0xFFu);
    c->buf[c->pos + 3u] = (uint8_t)((v >> 24u) & 0xFFu);
    c->pos += 4u;
}

static void write_i32_le(mdl_cursor_t *c, int32_t v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    write_u32_le(c, u);
}

static void write_f32_le(mdl_cursor_t *c, float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    write_u32_le(c, u);
}

static void write_magic9(mdl_cursor_t *c, const char magic8[8])
{
    memcpy(c->buf + c->pos, magic8, 8u);
    c->buf[c->pos + 8u] = '\0';
    c->pos += WE_MDL_MAGIC_SIZE;
}

static bool is_valid_mdl_char(unsigned char ch)
{
    return (ch >= 0x20u && ch <= 0x7Eu) || ch == '\t' || ch == '\n' || ch == '\r';
}

static bool prepare_mdl_string(const char *src, size_t max_chars, bool sanitize, char *out,
                               size_t out_cap, uint32_t *out_len_with_nul)
{
    if (!src || !out || out_cap <= max_chars) {
        return false;
    }

    size_t n = strnlen(src, max_chars + 1u);
    if (n > max_chars) {
        if (!sanitize) {
            return false;
        }
        n = max_chars;
    }

    for (size_t i = 0u; i < n; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (!is_valid_mdl_char(ch)) {
            if (!sanitize) {
                return false;
            }
            out[i] = '_';
        } else {
            out[i] = (char)ch;
        }
    }
    out[n] = '\0';
    *out_len_with_nul = (uint32_t)(n + 1u);
    return true;
}

static void write_mdl_string(mdl_cursor_t *c, const char *clean, uint32_t len_with_nul)
{
    /* Prefixing len_with_nul (< 1024) selects Branch A in read_mdl_string_ex unambiguously. */
    write_u32_le(c, len_with_nul);
    memcpy(c->buf + c->pos, clean, len_with_nul);
    c->pos += len_with_nul;
}

static bool resolve_anim_mode(const we_mdl_anim_t *anim, char out_mode[32],
                              uint32_t *out_len_with_nul)
{
    const char *canonical = NULL;
    if (anim->mode_str[0] != '\0') {
        if (strcasecmp(anim->mode_str, "loop") == 0) {
            canonical = "loop";
        } else if (strcasecmp(anim->mode_str, "mirror") == 0) {
            canonical = "mirror";
        } else if (strcasecmp(anim->mode_str, "single") == 0) {
            canonical = "single";
        } else {
            return false;
        }
    } else {
        switch (anim->mode) {
        case WE_ANIM_LOOP:
            canonical = "loop";
            break;
        case WE_ANIM_MIRROR:
            canonical = "mirror";
            break;
        case WE_ANIM_SINGLE:
            canonical = "single";
            break;
        default:
            return false;
        }
    }

    size_t len = strlen(canonical);
    memcpy(out_mode, canonical, len + 1u);
    *out_len_with_nul = (uint32_t)(len + 1u);
    return true;
}

static bool validate_skeleton_topology_and_matrices(const we_puppet_t *puppet)
{
    float world_binds[WE_MDL_MAX_BONES][16];

    for (uint32_t i = 0u; i < puppet->bone_count; ++i) {
        const we_mdl_bone_t *b = &puppet->bones[i];
        if (i == 0u) {
            if (b->parent != 0xFFFFFFFFu) {
                return false;
            }
        } else if (b->parent != 0xFFFFFFFFu && b->parent >= i) {
            /* Topological order is required because Pass 1 skinning evaluates parent < child. */
            return false;
        }

        if (!float_array_finite(b->local_trans, 16u) || fabsf(b->local_trans[15]) < 1e-6f) {
            return false;
        }

        if (b->parent == 0xFFFFFFFFu) {
            memcpy(world_binds[i], b->local_trans, sizeof(world_binds[i]));
        } else {
            we_mat4_mul(world_binds[b->parent], b->local_trans, world_binds[i]);
        }

        float inv_tmp[16];
        if (!we_mat4_inverse(world_binds[i], inv_tmp) || !float_array_finite(inv_tmp, 16u)) {
            return false;
        }
    }
    return true;
}

static bool path_has_symlink_component(const char *path)
{
    if (!path || path[0] == '\0') {
        return true;
    }
    size_t len = strnlen(path, PATH_MAX);
    if (len == 0u || len >= PATH_MAX || path[len - 1u] == '/') {
        return true;
    }

    struct stat st;
    if (lstat(path, &st) == 0 && (S_ISLNK(st.st_mode) || S_ISDIR(st.st_mode))) {
        return true;
    }

    char walk[PATH_MAX];
    memcpy(walk, path, len + 1u);
    for (size_t i = 1u; i < len; ++i) {
        if (walk[i] != '/') {
            continue;
        }
        walk[i] = '\0';
        if (lstat(walk, &st) == 0) {
            if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
                return true;
            }
        } else if (errno != ENOENT) {
            return true;
        }
        walk[i] = '/';
    }
    return false;
}

/* Walks every parent directory component with O_DIRECTORY | O_NOFOLLOW to prevent TOCTOU. */
static int open_parent_dir_nofollow(const char *path, char *out_leaf, size_t leaf_cap)
{
    if (!path || path[0] == '\0' || !out_leaf || leaf_cap == 0u) {
        return -1;
    }
    size_t len = strnlen(path, PATH_MAX);
    if (len == 0u || len >= PATH_MAX || path[len - 1u] == '/') {
        return -1;
    }

    const char *last_slash = strrchr(path, '/');
    const char *leaf = last_slash ? (last_slash + 1u) : path;
    size_t leaf_len = strlen(leaf);
    if (leaf_len == 0u || leaf_len >= leaf_cap || strcmp(leaf, ".") == 0 ||
        strcmp(leaf, "..") == 0) {
        return -1;
    }
    memcpy(out_leaf, leaf, leaf_len + 1u);

    int cur_fd = -1;
    size_t pos = 0u;
    size_t prefix_end = 0u;
    if (last_slash) {
        prefix_end = (size_t)(last_slash - path);
        if (path[0] == '/') {
            cur_fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            pos = 1u;
        } else {
            cur_fd = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
    } else {
        cur_fd = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (cur_fd < 0) {
        return -1;
    }

    while (pos < prefix_end) {
        while (pos < prefix_end && path[pos] == '/') {
            ++pos;
        }
        if (pos >= prefix_end) {
            break;
        }
        size_t start = pos;
        while (pos < prefix_end && path[pos] != '/') {
            ++pos;
        }
        size_t comp_len = pos - start;
        if (comp_len == 0u || comp_len > NAME_MAX) {
            close(cur_fd);
            return -1;
        }
        char comp[NAME_MAX + 1u];
        memcpy(comp, path + start, comp_len);
        comp[comp_len] = '\0';

        int next_fd = openat(cur_fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(cur_fd);
        if (next_fd < 0) {
            return -1;
        }
        cur_fd = next_fd;
    }

    struct stat leaf_st;
    if (fstatat(cur_fd, out_leaf, &leaf_st, AT_SYMLINK_NOFOLLOW) == 0 &&
        (S_ISLNK(leaf_st.st_mode) || S_ISDIR(leaf_st.st_mode))) {
        close(cur_fd);
        return -1;
    }
    return cur_fd;
}

static bool write_all_fd(int fd, const uint8_t *data, size_t size)
{
    size_t written = 0u;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

void we_mdl_write_opts_default(we_mdl_write_opts_t *out_opts)
{
    if (!out_opts) {
        return;
    }
    *out_opts = (we_mdl_write_opts_t){
        .emit_attachments = true,
        .emit_skeleton = true,
        .emit_animations = true,
        .sanitize_ascii_strings = true,
        .override_mdl_flag = 0,
    };
}

void we_mdl_write_free(uint8_t *bytes) { free(bytes); }

bool we_physics_format_simulation_json(const we_bone_sim_config_t *cfg, char out_json[256])
{
    if (!cfg || !out_json) {
        return false;
    }
    if (!float_array_finite(&cfg->compliance_stretch, 12u)) {
        return false;
    }
    if ((cfg->flags & WE_BONE_PHYS_ENABLED) == 0u) {
        out_json[0] = '\0';
        return true;
    }

    float ratio = fminf(fmaxf(cfg->compliance_bend / 0.05f, 0.0f), 1.0f);
    float stiffness = fminf(fmaxf(1.0f - cbrtf(ratio), 0.0f), 1.0f);
    float damping = fmaxf(cfg->damping_ratio, 0.0f);
    /* Parser divides gravity values > 2.0 by 9.8, assuming pixel-space acceleration units. */
    float gravity =
        (fabsf(cfg->gravity_scale) > 2.0f) ? (cfg->gravity_scale * 9.8f) : cfg->gravity_scale;
    float mass = fmaxf(cfg->mass_kg, 0.01f);
    float length = fmaxf(cfg->rest_length_px, 1.0f);
    float wind = fmaxf(cfg->wind_drag_coeff, 0.0f);
    float audio = fmaxf(cfg->audio_drag_coeff, 0.0f);

    locale_t cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!cloc) {
        return false;
    }
    locale_t prev = uselocale(cloc);

    char extra[96];
    extra[0] = '\0';
    size_t extra_len = 0u;
    bool ok = true;

    if ((cfg->flags & WE_BONE_PHYS_ANGULAR_LIMIT) != 0u) {
        float maxangle = fmaxf(fabsf(cfg->angle_limit_max_rad), fabsf(cfg->angle_limit_min_rad));
        maxangle = fminf(fmaxf(maxangle, 0.0f), 3.14159f);
        int en = snprintf(extra + extra_len, sizeof(extra) - extra_len, ",\"maxangle\":%.6g",
                          (double)maxangle);
        if (en < 0 || (size_t)en >= sizeof(extra) - extra_len) {
            ok = false;
        } else {
            extra_len += (size_t)en;
        }
    }
    if (ok && (cfg->flags & WE_BONE_PHYS_CURSOR_COLLIDE) != 0u) {
        float radius = fmaxf(cfg->collision_radius_px, 1.0f);
        int en = snprintf(extra + extra_len, sizeof(extra) - extra_len, ",\"radius\":%.6g",
                          (double)radius);
        if (en < 0 || (size_t)en >= sizeof(extra) - extra_len) {
            ok = false;
        }
    }

    int n = -1;
    if (ok) {
        n = snprintf(out_json, 256u,
                     "{\"stiffness\":%.6g,\"damping\":%.6g,\"gravity\":%.6g,\"mass\":%.6g,"
                     "\"length\":%.6g,\"wind\":%.6g,\"audio\":%.6g%s}",
                     (double)stiffness, (double)damping, (double)gravity, (double)mass,
                     (double)length, (double)wind, (double)audio, extra);
    }

    uselocale(prev);
    freelocale(cloc);

    if (!ok || n <= 0 || (size_t)n >= 256u) {
        out_json[0] = '\0';
        return false;
    }
    return true;
}

bool we_mdl_write_mem(const we_puppet_t *puppet, const we_mdl_write_opts_t *opts,
                      uint8_t **out_bytes, size_t *out_size)
{
    if (!out_bytes || !out_size) {
        return false;
    }
    *out_bytes = NULL;
    *out_size = 0u;
    if (!puppet) {
        return false;
    }

    we_mdl_write_opts_t eff;
    we_mdl_write_opts_default(&eff);
    if (opts) {
        eff = *opts;
    }

    int32_t mdl_flag = (eff.override_mdl_flag > 0) ? eff.override_mdl_flag : puppet->mdl_flag;
    /* mdl_flag == 9 marks stub meshes rejected by parser; reject unless explicitly overridden. */
    if (mdl_flag == 9 && eff.override_mdl_flag <= 0) {
        return false;
    }
    if (mdl_flag < 0 || mdl_flag > 256) {
        return false;
    }

    if (puppet->vertex_count == 0u || puppet->vertex_count > WE_MDL_MAX_VERTICES ||
        !puppet->vertices) {
        return false;
    }
    if (puppet->triangle_count == 0u || puppet->triangle_count > WE_MDL_MAX_TRIANGLES ||
        puppet->index_count != puppet->triangle_count * 3u || !puppet->indices) {
        return false;
    }
    if (puppet->bone_count > WE_MDL_MAX_BONES || (puppet->bone_count > 0u && !puppet->bones)) {
        return false;
    }
    if (puppet->anim_count > WE_MDL_MAX_ANIMS || (puppet->anim_count > 0u && !puppet->anims)) {
        return false;
    }

    const bool write_skel = eff.emit_skeleton && puppet->bone_count > 0u;
    const bool write_anim = write_skel && eff.emit_animations && puppet->anim_count > 0u;

    char clean_mat[256];
    uint32_t clean_mat_len = 0u;
    if (!prepare_mdl_string(puppet->mat_json_file, sizeof(clean_mat) - 1u,
                            eff.sanitize_ascii_strings, clean_mat, sizeof(clean_mat),
                            &clean_mat_len)) {
        return false;
    }

    for (uint32_t i = 0u; i < puppet->vertex_count; ++i) {
        const we_mdl_vertex_t *v = &puppet->vertices[i];
        if (!float_array_finite(v->pos, 3u) || !float_array_finite(v->weight, 4u) ||
            !float_array_finite(v->texcoord, 2u)) {
            return false;
        }
        for (uint32_t k = 0u; k < 4u; ++k) {
            if (v->weight[k] < 0.0f) {
                return false;
            }
            if (write_skel && (v->weight[k] > 0.0f || v->blend_indices[k] != 0u) &&
                v->blend_indices[k] >= puppet->bone_count) {
                return false;
            }
        }
    }

    for (uint32_t i = 0u; i < puppet->index_count; ++i) {
        if ((uint32_t)puppet->indices[i] >= puppet->vertex_count) {
            return false;
        }
    }

    size_t v_bytes = 0u;
    size_t idx_bytes = 0u;
    size_t total_size = 0u;
    if (!size_mul_checked(puppet->vertex_count, WE_MDL_VERTEX_STRIDE, &v_bytes) ||
        !size_mul_checked(puppet->triangle_count, 6u, &idx_bytes)) {
        return false;
    }

    /* MDLV header: magic(9) + 3*i32(12) + mat_str(4+len) + zero+curr(8) + v_sz(4) + v_bytes +
     * idx_sz(4) + idx_bytes */
    size_t mdlv_fixed = WE_MDL_MAGIC_SIZE + 12u + 4u + (size_t)clean_mat_len + 8u + 4u + 4u;
    if (!size_add_checked(mdlv_fixed, v_bytes, &total_size) ||
        !size_add_checked(total_size, idx_bytes, &total_size)) {
        return false;
    }

    if (eff.emit_attachments) {
        if (!size_add_checked(total_size, WE_MDL_MAGIC_SIZE + 4u, &total_size)) {
            return false;
        }
    }

    typedef struct {
        char bone_names[WE_MDL_MAX_BONES][128];
        uint32_t bone_name_lens[WE_MDL_MAX_BONES];
        char sim_jsons[WE_MDL_MAX_BONES][256];
        uint32_t sim_json_lens[WE_MDL_MAX_BONES];
        char anim_names[WE_MDL_MAX_ANIMS][128];
        uint32_t anim_name_lens[WE_MDL_MAX_ANIMS];
        char mode_strs[WE_MDL_MAX_ANIMS][32];
        uint32_t mode_str_lens[WE_MDL_MAX_ANIMS];
    } mdl_write_scratch_t;

    mdl_write_scratch_t *scratch = NULL;
    uint8_t *buf = NULL;
    if (write_skel || write_anim) {
        scratch = (mdl_write_scratch_t *)calloc(1u, sizeof(*scratch));
        if (!scratch) {
            return false;
        }
    }

    if (write_skel) {
        if (!validate_skeleton_topology_and_matrices(puppet)) {
            goto fail;
        }
        if (!size_add_checked(total_size, WE_MDL_MAGIC_SIZE + 4u, &total_size)) {
            goto fail;
        }
        for (uint32_t i = 0u; i < puppet->bone_count; ++i) {
            const we_mdl_bone_t *b = &puppet->bones[i];
            if (!prepare_mdl_string(b->name, 127u, eff.sanitize_ascii_strings,
                                    scratch->bone_names[i], sizeof(scratch->bone_names[i]),
                                    &scratch->bone_name_lens[i]) ||
                !prepare_mdl_string(b->simulation_json, 255u, eff.sanitize_ascii_strings,
                                    scratch->sim_jsons[i], sizeof(scratch->sim_jsons[i]),
                                    &scratch->sim_json_lens[i])) {
                goto fail;
            }
            size_t bone_sz = 4u + (size_t)scratch->bone_name_lens[i] + 12u + WE_MDL_BONE_MAT_BYTES +
                             4u + (size_t)scratch->sim_json_lens[i];
            if (!size_add_checked(total_size, bone_sz, &total_size)) {
                goto fail;
            }
        }
    }

    if (write_anim) {
        if (!size_add_checked(total_size, WE_MDL_MAGIC_SIZE + 4u, &total_size)) {
            goto fail;
        }
        for (uint32_t a = 0u; a < puppet->anim_count; ++a) {
            const we_mdl_anim_t *anim = &puppet->anims[a];
            if (anim->id < -1 || anim->id > 100000 || !isfinite(anim->fps) || anim->fps <= 0.0f ||
                anim->fps > 1000.0f || anim->length < 1 || anim->length > 1000000) {
                goto fail;
            }
            if (anim->track_count > WE_MDL_MAX_BONES || (anim->track_count > 0u && !anim->tracks)) {
                goto fail;
            }
            if (!prepare_mdl_string(anim->name, 127u, eff.sanitize_ascii_strings,
                                    scratch->anim_names[a], sizeof(scratch->anim_names[a]),
                                    &scratch->anim_name_lens[a]) ||
                !resolve_anim_mode(anim, scratch->mode_strs[a], &scratch->mode_str_lens[a])) {
                goto fail;
            }

            size_t anim_hdr = 8u + 4u + (size_t)scratch->anim_name_lens[a] + 4u +
                              (size_t)scratch->mode_str_lens[a] + 16u;
            if (!size_add_checked(total_size, anim_hdr, &total_size)) {
                goto fail;
            }

            bool bone_seen[WE_MDL_MAX_BONES] = {0};
            for (uint32_t t = 0u; t < anim->track_count; ++t) {
                const we_mdl_bone_track_t *tr = &anim->tracks[t];
                if (tr->bone_index < 0 || (uint32_t)tr->bone_index >= puppet->bone_count) {
                    goto fail;
                }
                /* Enforce distinct track bone indices to avoid parser all_same_idx quirk. */
                if (bone_seen[tr->bone_index]) {
                    goto fail;
                }
                bone_seen[tr->bone_index] = true;

                if (tr->kf_count == 0u || tr->kf_count > WE_MDL_MAX_KEYFRAMES || !tr->keyframes) {
                    goto fail;
                }
                for (uint32_t k = 0u; k < tr->kf_count; ++k) {
                    const we_mdl_keyframe_t *kf = &tr->keyframes[k];
                    if (!float_array_finite(kf->pos, 3u) || !float_array_finite(kf->angle, 3u) ||
                        !float_array_finite(kf->scale, 3u)) {
                        goto fail;
                    }
                }

                size_t kf_bytes = 0u;
                if (!size_mul_checked(tr->kf_count, WE_MDL_KF_STRIDE, &kf_bytes) ||
                    !size_add_checked(total_size, 12u + kf_bytes, &total_size)) {
                    goto fail;
                }
            }
        }
    }

    if (total_size > WE_MDL_WRITER_MAX_BYTES) {
        goto fail;
    }

    buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        goto fail;
    }

    mdl_cursor_t c = {.buf = buf, .cap = total_size, .pos = 0u};

    write_magic9(&c, WE_MDL_VERTEX_MAGIC_STR);
    write_i32_le(&c, mdl_flag);
    write_i32_le(&c, puppet->unk_1);
    write_i32_le(&c, puppet->unk_2);
    write_mdl_string(&c, clean_mat, clean_mat_len);
    write_i32_le(&c, 0);
    write_u32_le(&c, WE_MDL_VERTEX_CURR_FMT);

    write_u32_le(&c, (uint32_t)v_bytes);
    for (uint32_t i = 0u; i < puppet->vertex_count; ++i) {
        const we_mdl_vertex_t *v = &puppet->vertices[i];
        write_f32_le(&c, v->pos[0]);
        write_f32_le(&c, v->pos[1]);
        write_f32_le(&c, v->pos[2]);
        for (uint32_t k = 0u; k < 4u; ++k) {
            write_u32_le(&c, v->blend_indices[k]);
        }
        for (uint32_t k = 0u; k < 4u; ++k) {
            write_f32_le(&c, v->weight[k]);
        }
        write_f32_le(&c, v->texcoord[0]);
        write_f32_le(&c, v->texcoord[1]);
    }

    write_u32_le(&c, (uint32_t)idx_bytes);
    for (uint32_t i = 0u; i < puppet->index_count; ++i) {
        write_u16_le(&c, puppet->indices[i]);
    }

    if (eff.emit_attachments) {
        write_magic9(&c, WE_MDL_ATTACH_MAGIC_STR);
        write_u32_le(&c, 0u);
    }

    if (write_skel) {
        write_magic9(&c, WE_MDL_SKEL_MAGIC_STR);
        write_u32_le(&c, puppet->bone_count);
        for (uint32_t i = 0u; i < puppet->bone_count; ++i) {
            const we_mdl_bone_t *b = &puppet->bones[i];
            write_mdl_string(&c, scratch->bone_names[i], scratch->bone_name_lens[i]);
            write_i32_le(&c, b->unk);
            write_u32_le(&c, b->parent);
            write_u32_le(&c, WE_MDL_BONE_MAT_BYTES);
            for (uint32_t m = 0u; m < 16u; ++m) {
                write_f32_le(&c, b->local_trans[m]);
            }
            write_mdl_string(&c, scratch->sim_jsons[i], scratch->sim_json_lens[i]);
        }
    }

    if (write_anim) {
        write_magic9(&c, WE_MDL_ANIM_MAGIC_STR);
        write_u32_le(&c, puppet->anim_count);
        for (uint32_t a = 0u; a < puppet->anim_count; ++a) {
            const we_mdl_anim_t *anim = &puppet->anims[a];
            write_i32_le(&c, anim->id);
            write_i32_le(&c, anim->unk);
            write_mdl_string(&c, scratch->anim_names[a], scratch->anim_name_lens[a]);
            write_mdl_string(&c, scratch->mode_strs[a], scratch->mode_str_lens[a]);
            write_f32_le(&c, anim->fps);
            write_i32_le(&c, anim->length);
            write_i32_le(&c, anim->unk2);
            write_u32_le(&c, anim->track_count);

            for (uint32_t t = 0u; t < anim->track_count; ++t) {
                const we_mdl_bone_track_t *tr = &anim->tracks[t];
                write_i32_le(&c, tr->bone_index);
                write_i32_le(&c, tr->unk);
                write_u32_le(&c, tr->kf_count * WE_MDL_KF_STRIDE);
                for (uint32_t k = 0u; k < tr->kf_count; ++k) {
                    const we_mdl_keyframe_t *kf = &tr->keyframes[k];
                    write_f32_le(&c, kf->pos[0]);
                    write_f32_le(&c, kf->pos[1]);
                    write_f32_le(&c, kf->pos[2]);
                    write_f32_le(&c, kf->angle[0]);
                    write_f32_le(&c, kf->angle[1]);
                    write_f32_le(&c, kf->angle[2]);
                    write_f32_le(&c, kf->scale[0]);
                    write_f32_le(&c, kf->scale[1]);
                    write_f32_le(&c, kf->scale[2]);
                }
            }
        }
    }

    if (c.pos != total_size) {
        goto fail;
    }

    free(scratch);
    *out_bytes = buf;
    *out_size = total_size;
    return true;

fail:
    free(buf);
    free(scratch);
    return false;
}

bool we_mdl_write_file(const we_puppet_t *puppet, const we_mdl_write_opts_t *opts, const char *path)
{
    if (!puppet || !path || path[0] == '\0') {
        return false;
    }
    if (path_has_symlink_component(path)) {
        return false;
    }

    char leaf[NAME_MAX + 1u];
    int dir_fd = open_parent_dir_nofollow(path, leaf, sizeof(leaf));
    if (dir_fd < 0) {
        return false;
    }

    uint8_t *bytes = NULL;
    size_t size = 0u;
    if (!we_mdl_write_mem(puppet, opts, &bytes, &size)) {
        close(dir_fd);
        return false;
    }

    static atomic_uint_fast64_t s_mdl_tmp_seq = 0;
    uint64_t seq = atomic_fetch_add_explicit(&s_mdl_tmp_seq, 1u, memory_order_relaxed);
    char tmp_name[NAME_MAX + 1u];
    int n = snprintf(tmp_name, sizeof(tmp_name), ".%s.tmp.%ld.%llu", leaf, (long)getpid(),
                     (unsigned long long)seq);
    if (n < 0 || (size_t)n >= sizeof(tmp_name)) {
        n = snprintf(tmp_name, sizeof(tmp_name), ".waywal_mdl.tmp.%ld.%llu", (long)getpid(),
                     (unsigned long long)seq);
        if (n < 0 || (size_t)n >= sizeof(tmp_name)) {
            we_mdl_write_free(bytes);
            close(dir_fd);
            return false;
        }
    }

    int fd = openat(dir_fd, tmp_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        we_mdl_write_free(bytes);
        close(dir_fd);
        return false;
    }

    bool ok = (size == 0u) || write_all_fd(fd, bytes, size);
    we_mdl_write_free(bytes);

    if (ok && fsync(fd) < 0 && errno != EINVAL) {
        ok = false;
    }
    if (close(fd) < 0) {
        ok = false;
    }
    if (ok && renameat(dir_fd, tmp_name, dir_fd, leaf) < 0) {
        ok = false;
    }
    if (!ok) {
        unlinkat(dir_fd, tmp_name, 0);
    }
    close(dir_fd);
    return ok;
}
