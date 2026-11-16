// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_GPU_COMPUTE_H
#define WAYWAL_GPU_COMPUTE_H

#include "waywal/dmabuf.h"
#include "waywal/log.h"
#include "waywal/render_engine.h"
#include "waywal/types.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define WWAL_MAYBE_UNUSED        [[maybe_unused]]
#define WWAL_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#if defined(__GNUC__) || defined(__clang__)
#define WWAL_MAYBE_UNUSED __attribute__((unused))
#else
#define WWAL_MAYBE_UNUSED
#endif
#define WWAL_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
enum : uint64_t {
#else
enum {
#endif
    WWAL_DRM_FORMAT_ARGB8888 = 0x34325241ULL,
    WWAL_DRM_FORMAT_XRGB8888 = 0x34325258ULL,
    WWAL_DRM_FORMAT_ABGR8888 = 0x34324241ULL,
    WWAL_DRM_FORMAT_XBGR8888 = 0x34324258ULL,
    WWAL_DRM_FORMAT_ARGB2101010 = 0x30335241ULL,
    WWAL_DRM_FORMAT_XRGB2101010 = 0x30335258ULL,
    WWAL_DRM_FORMAT_ABGR2101010 = 0x30334241ULL,
    WWAL_DRM_FORMAT_XBGR2101010 = 0x30334258ULL,
    WWAL_DRM_FORMAT_MOD_LINEAR = 0ULL,
    WWAL_DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL
};

enum {
    WWAL_WWBC_MAGIC = 0x43425757u,
    WWAL_WWBC_VERSION = 2u,
    WWAL_WWBC_MAX_BIN_SIZE = 16u * 1024u * 1024u,
    WWAL_FBO_LRU_CAPACITY = 16u
};

typedef struct wwal_target_bo {
    uint32_t drm_format;
    uint64_t modifier;
    uint32_t width;
    uint32_t height;
    uint32_t gl_tex;
} wwal_target_bo_t;

typedef struct wwal_fbo_cache_key {
    uint32_t drm_format;
    uint64_t modifier;
    bool validated;
} wwal_fbo_cache_key_t;

typedef struct wwal_fbo_lru {
    wwal_fbo_cache_key_t keys[WWAL_FBO_LRU_CAPACITY];
    uint64_t ticks[WWAL_FBO_LRU_CAPACITY];
    uint64_t clock;
} wwal_fbo_lru_t;

typedef struct wwal_scratch_present_state {
    GLuint blit_prog;
    GLint loc_tex;
    GLint loc_swap_rb;
    GLint loc_force_opaque;
    int last_swap_rb;
    int last_force_opaque;
    GLuint blit_vao;
    GLuint draw_fbo;
    GLuint bound_fbo_tex;
    wwal_fbo_lru_t fbo_lru;
} wwal_scratch_present_state_t;

typedef struct wwal_prog_cache_hdr {
    uint32_t magic;
    uint32_t version;
    uint64_t vs_hash;
    uint64_t main_hash;
    uint64_t driver_hash;
    uint32_t bin_format;
    uint32_t bin_size;
    uint32_t bin_crc32c;
    uint32_t reserved;
} wwal_prog_cache_hdr_t;

WWAL_STATIC_ASSERT(sizeof(wwal_prog_cache_hdr_t) == 48u,
                   "WWBC v2 program cache header must remain fixed-width 48 bytes");

typedef struct wwal_egl_sync_procs {
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;
    bool has_fence_sync;
    bool has_native_fence_sync;
} wwal_egl_sync_procs_t;

WWAL_MAYBE_UNUSED static inline wwal_target_bo_t wwal_target_bo_from_dmabuf(const dmabuf_bo_t *bo)
{
    wwal_target_bo_t out = {0u, 0ULL, 0u, 0u, 0u};
    if (!bo) {
        return out;
    }
    out.drm_format = bo->drm_format;
    out.modifier = bo->modifier;
    out.width = bo->width;
    out.height = bo->height;
    out.gl_tex = bo->gl_tex;
    return out;
}

WWAL_MAYBE_UNUSED static inline bool wwal_bo_requires_scratch_blit(const wwal_target_bo_t *bo)
{
    if (!bo) {
        return true;
    }
    bool non_linear = (bo->modifier != WWAL_DRM_FORMAT_MOD_LINEAR) &&
                      (bo->modifier != WWAL_DRM_FORMAT_MOD_INVALID);
    bool ten_bit = (bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_ARGB2101010) ||
                   (bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_XRGB2101010) ||
                   (bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_ABGR2101010) ||
                   (bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_XBGR2101010);
    return non_linear || ten_bit;
}

WWAL_MAYBE_UNUSED static inline bool wwal_fbo_cache_matches(const wwal_fbo_cache_key_t *cached,
                                                            const wwal_target_bo_t *bo)
{
    if (!cached || !bo || !cached->validated) {
        return false;
    }
    return cached->drm_format == bo->drm_format && cached->modifier == bo->modifier;
}

WWAL_MAYBE_UNUSED static inline void wwal_fbo_lru_reset(wwal_fbo_lru_t *lru)
{
    if (!lru) {
        return;
    }
    memset(lru, 0, sizeof(*lru));
}

WWAL_MAYBE_UNUSED static inline bool wwal_fbo_lru_contains(wwal_fbo_lru_t *lru,
                                                           const wwal_target_bo_t *bo)
{
    if (!lru || !bo) {
        return false;
    }
    for (uint32_t i = 0u; i < WWAL_FBO_LRU_CAPACITY; ++i) {
        if (wwal_fbo_cache_matches(&lru->keys[i], bo)) {
            lru->ticks[i] = ++lru->clock;
            return true;
        }
    }
    return false;
}

WWAL_MAYBE_UNUSED static inline void wwal_fbo_lru_record_valid(wwal_fbo_lru_t *lru,
                                                               const wwal_target_bo_t *bo)
{
    if (!lru || !bo) {
        return;
    }
    uint32_t victim = 0u;
    uint64_t min_tick = UINT64_MAX;
    for (uint32_t i = 0u; i < WWAL_FBO_LRU_CAPACITY; ++i) {
        if (wwal_fbo_cache_matches(&lru->keys[i], bo)) {
            lru->ticks[i] = ++lru->clock;
            return;
        }
        if (!lru->keys[i].validated) {
            victim = i;
            break;
        }
        if (lru->ticks[i] < min_tick) {
            min_tick = lru->ticks[i];
            victim = i;
        }
    }
    lru->keys[victim].drm_format = bo->drm_format;
    lru->keys[victim].modifier = bo->modifier;
    lru->keys[victim].validated = true;
    lru->ticks[victim] = ++lru->clock;
}

/* Validates the currently bound GL_FRAMEBUFFER against a 16-entry (drm_format, modifier) LRU. */
WWAL_MAYBE_UNUSED static inline bool wwal_fbo_validate_attached(wwal_fbo_lru_t *lru,
                                                                const wwal_target_bo_t *bo)
{
    if (!bo || bo->gl_tex == 0u) {
        return false;
    }
    if (lru && wwal_fbo_lru_contains(lru, bo)) {
        return true;
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    if (lru) {
        wwal_fbo_lru_record_valid(lru, bo);
    }
    return true;
}

WWAL_MAYBE_UNUSED static inline uint64_t wwal_fnv1a_64_update(uint64_t hash, const void *data,
                                                              size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

WWAL_MAYBE_UNUSED static inline uint64_t wwal_fnv1a_64_str(uint64_t hash, const char *s)
{
    if (!s) {
        const uint8_t nul = 0xFEu;
        return wwal_fnv1a_64_update(hash, &nul, 1u);
    }
    hash = wwal_fnv1a_64_update(hash, s, strlen(s));
    const uint8_t sep = 0xFFu;
    return wwal_fnv1a_64_update(hash, &sep, 1u);
}

WWAL_MAYBE_UNUSED static inline uint32_t wwal_crc32c(const void *data, size_t len)
{
    static const uint32_t nibble_tbl[16] = {
        0x00000000u, 0x105EC76Fu, 0x20BD8EDEu, 0x30E349B1u, 0x417B1DBCu, 0x5125DAD3u,
        0x61C69362u, 0x7198540Du, 0x82F63B78u, 0x92A8FC17u, 0xA24BB5A6u, 0xB21572C9u,
        0xC38D26C4u, 0xD3D3E1ABu, 0xE330A81Au, 0xF36E6F75u,
    };
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ nibble_tbl[crc & 0x0Fu];
        crc = (crc >> 4) ^ nibble_tbl[crc & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

WWAL_MAYBE_UNUSED static inline uint64_t wwal_gl_driver_hash(void)
{
    uint64_t h = 14695981039346656037ULL;
    h = wwal_fnv1a_64_str(h, (const char *)glGetString(GL_VENDOR));
    h = wwal_fnv1a_64_str(h, (const char *)glGetString(GL_RENDERER));
    h = wwal_fnv1a_64_str(h, (const char *)glGetString(GL_VERSION));
    h = wwal_fnv1a_64_str(h, (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    return h;
}

WWAL_MAYBE_UNUSED static inline bool wwal_io_read_all(int fd, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0u;
    while (got < len) {
        ssize_t n = read(fd, dst + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

WWAL_MAYBE_UNUSED static inline bool wwal_io_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t sent = 0u;
    while (sent < len) {
        ssize_t n = write(fd, src + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

WWAL_MAYBE_UNUSED static inline bool wwal_prog_cache_path(uint64_t vs_hash, uint64_t main_hash,
                                                          uint64_t driver_hash, char *out_path,
                                                          size_t out_cap)
{
    if (!out_path || out_cap == 0u) {
        return false;
    }
    char base_dir[PATH_MAX];
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        int bn = snprintf(base_dir, sizeof(base_dir), "%s", xdg);
        if (bn < 0 || (size_t)bn >= sizeof(base_dir)) {
            return false;
        }
        (void)mkdir(base_dir, 0700);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') {
            return false;
        }
        int bn = snprintf(base_dir, sizeof(base_dir), "%s/.cache", home);
        if (bn < 0 || (size_t)bn >= sizeof(base_dir)) {
            return false;
        }
        (void)mkdir(base_dir, 0700);
    }

    char wwal_dir[PATH_MAX];
    int wn = snprintf(wwal_dir, sizeof(wwal_dir), "%s/wwal", base_dir);
    if (wn < 0 || (size_t)wn >= sizeof(wwal_dir)) {
        return false;
    }
    (void)mkdir(wwal_dir, 0700);

    char shaders_dir[PATH_MAX];
    int sn = snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", wwal_dir);
    if (sn < 0 || (size_t)sn >= sizeof(shaders_dir)) {
        return false;
    }
    (void)mkdir(shaders_dir, 0700);

    uint64_t key_hash = 14695981039346656037ULL;
    key_hash = wwal_fnv1a_64_update(key_hash, &vs_hash, sizeof(vs_hash));
    key_hash = wwal_fnv1a_64_update(key_hash, &main_hash, sizeof(main_hash));
    key_hash = wwal_fnv1a_64_update(key_hash, &driver_hash, sizeof(driver_hash));

    int n = snprintf(out_path, out_cap, "%s/wwbc_%016llx.bin", shaders_dir,
                     (unsigned long long)key_hash);
    return (n > 0 && (size_t)n < out_cap);
}

/* Loads a WWBC v2 program binary, unlinking any truncated, mismatched, or corrupt cache file. */
WWAL_MAYBE_UNUSED static inline GLuint wwal_prog_cache_load(const char *cache_path,
                                                            uint64_t vs_hash, uint64_t main_hash,
                                                            uint64_t driver_hash)
{
    if (!cache_path || cache_path[0] == '\0') {
        return 0u;
    }
    int fd = open(cache_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP) {
            (void)unlink(cache_path);
        }
        return 0u;
    }

    bool should_unlink = false;
    GLuint loaded_prog = 0u;
    struct stat st;
    wwal_prog_cache_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return 0u;
    }

    if (st.st_size <= (off_t)sizeof(wwal_prog_cache_hdr_t) ||
        !wwal_io_read_all(fd, &hdr, sizeof(hdr)) || hdr.magic != (uint32_t)WWAL_WWBC_MAGIC ||
        hdr.version != (uint32_t)WWAL_WWBC_VERSION || hdr.vs_hash != vs_hash ||
        hdr.main_hash != main_hash || hdr.driver_hash != driver_hash || hdr.bin_size == 0u ||
        hdr.bin_size > (uint32_t)WWAL_WWBC_MAX_BIN_SIZE || hdr.reserved != 0u) {
        should_unlink = true;
    } else {
        uint64_t expected_size = (uint64_t)sizeof(wwal_prog_cache_hdr_t) + (uint64_t)hdr.bin_size;
        if ((uint64_t)st.st_size != expected_size) {
            should_unlink = true;
        } else {
            uint8_t *bin = (uint8_t *)malloc((size_t)hdr.bin_size);
            if (bin) {
                if (!wwal_io_read_all(fd, bin, (size_t)hdr.bin_size) ||
                    wwal_crc32c(bin, (size_t)hdr.bin_size) != hdr.bin_crc32c) {
                    should_unlink = true;
                } else {
                    GLuint prog = glCreateProgram();
                    if (prog) {
                        while (glGetError() != GL_NO_ERROR) {
                        }
                        glProgramBinary(prog, (GLenum)hdr.bin_format, bin, (GLsizei)hdr.bin_size);
                        GLenum bin_err = glGetError();
                        GLint linked = 0;
                        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
                        if (bin_err == GL_NO_ERROR && linked) {
                            loaded_prog = prog;
                        } else {
                            glDeleteProgram(prog);
                            should_unlink = true;
                        }
                    }
                }
                free(bin);
            }
        }
    }

    close(fd);
    if (should_unlink) {
        (void)unlink(cache_path);
    }
    return loaded_prog;
}

WWAL_MAYBE_UNUSED static inline void wwal_prog_cache_save(const char *cache_path, GLuint prog,
                                                          uint64_t vs_hash, uint64_t main_hash,
                                                          uint64_t driver_hash)
{
    if (!cache_path || cache_path[0] == '\0' || prog == 0u) {
        return;
    }
    GLint bin_len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &bin_len);
    if (bin_len <= 0 || (uint32_t)bin_len > (uint32_t)WWAL_WWBC_MAX_BIN_SIZE) {
        return;
    }
    uint8_t *bin = (uint8_t *)malloc((size_t)bin_len);
    if (!bin) {
        return;
    }

    GLsizei actual_len = 0;
    GLenum bin_fmt = 0;
    glGetProgramBinary(prog, bin_len, &actual_len, &bin_fmt, bin);
    if (actual_len > 0 && actual_len <= bin_len) {
        static uint32_t tmp_seq = 0u;
        uint32_t seq = __atomic_fetch_add(&tmp_seq, 1u, __ATOMIC_RELAXED);
        char tmp_path[PATH_MAX + 48];
        int tn =
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%u", cache_path, (int)getpid(), seq);
        if (tn > 0 && (size_t)tn < sizeof(tmp_path)) {
            int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (fd >= 0) {
                wwal_prog_cache_hdr_t hdr;
                hdr.magic = (uint32_t)WWAL_WWBC_MAGIC;
                hdr.version = (uint32_t)WWAL_WWBC_VERSION;
                hdr.vs_hash = vs_hash;
                hdr.main_hash = main_hash;
                hdr.driver_hash = driver_hash;
                hdr.bin_format = (uint32_t)bin_fmt;
                hdr.bin_size = (uint32_t)actual_len;
                hdr.bin_crc32c = wwal_crc32c(bin, (size_t)actual_len);
                hdr.reserved = 0u;

                bool ok = wwal_io_write_all(fd, &hdr, sizeof(hdr)) &&
                          wwal_io_write_all(fd, bin, (size_t)actual_len);
                if (close(fd) != 0) {
                    ok = false;
                }
                if (!ok || rename(tmp_path, cache_path) != 0) {
                    (void)unlink(tmp_path);
                }
            }
        }
    }
    free(bin);
}

WWAL_MAYBE_UNUSED static inline GLuint wwal_gl_compile_shader_stage(GLenum stage, const char *src,
                                                                    const char *debug_name)
{
    if (!src) {
        return 0u;
    }
    GLuint shader = glCreateShader(stage);
    if (!shader) {
        WAYWAL_LOG_ERR("glCreateShader(0x%x) failed for %s", (unsigned)stage,
                       debug_name ? debug_name : "shader");
        return 0u;
    }
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log_buf[1024];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Shader compilation failed for %s: %s", debug_name ? debug_name : "shader",
                       log_buf);
        glDeleteShader(shader);
        return 0u;
    }
    return shader;
}

WWAL_MAYBE_UNUSED static inline GLuint
wwal_gl_compile_compute_program_cached(const char *cs_src, const char *debug_name, bool *out_cached)
{
    if (out_cached) {
        *out_cached = false;
    }
    if (!cs_src) {
        return 0u;
    }
    uint64_t vs_hash = wwal_fnv1a_64_str(14695981039346656037ULL, "__compute_stage__");
    uint64_t main_hash = wwal_fnv1a_64_str(14695981039346656037ULL, cs_src);
    uint64_t driver_hash = wwal_gl_driver_hash();

    char cache_path[PATH_MAX];
    bool has_cache =
        wwal_prog_cache_path(vs_hash, main_hash, driver_hash, cache_path, sizeof(cache_path));
    if (has_cache) {
        GLuint cached = wwal_prog_cache_load(cache_path, vs_hash, main_hash, driver_hash);
        if (cached != 0u) {
            if (out_cached) {
                *out_cached = true;
            }
            return cached;
        }
    }

    GLuint cs = wwal_gl_compile_shader_stage(GL_COMPUTE_SHADER, cs_src, debug_name);
    if (!cs) {
        return 0u;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(cs);
        return 0u;
    }
    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glDeleteShader(cs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024];
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Compute program linking failed for %s: %s",
                       debug_name ? debug_name : "compute", log_buf);
        glDeleteProgram(prog);
        return 0u;
    }

    if (has_cache) {
        wwal_prog_cache_save(cache_path, prog, vs_hash, main_hash, driver_hash);
    }
    return prog;
}

WWAL_MAYBE_UNUSED static inline GLuint
wwal_gl_compile_graphics_program_cached(const char *vs_src, const char *fs_src,
                                        const char *debug_name, bool *out_cached)
{
    if (out_cached) {
        *out_cached = false;
    }
    if (!vs_src || !fs_src) {
        return 0u;
    }
    uint64_t vs_hash = wwal_fnv1a_64_str(14695981039346656037ULL, vs_src);
    uint64_t main_hash = wwal_fnv1a_64_str(14695981039346656037ULL, fs_src);
    uint64_t driver_hash = wwal_gl_driver_hash();

    char cache_path[PATH_MAX];
    bool has_cache =
        wwal_prog_cache_path(vs_hash, main_hash, driver_hash, cache_path, sizeof(cache_path));
    if (has_cache) {
        GLuint cached = wwal_prog_cache_load(cache_path, vs_hash, main_hash, driver_hash);
        if (cached != 0u) {
            if (out_cached) {
                *out_cached = true;
            }
            return cached;
        }
    }

    GLuint vs = wwal_gl_compile_shader_stage(GL_VERTEX_SHADER, vs_src, debug_name);
    GLuint fs = wwal_gl_compile_shader_stage(GL_FRAGMENT_SHADER, fs_src, debug_name);
    if (!vs || !fs) {
        if (vs) {
            glDeleteShader(vs);
        }
        if (fs) {
            glDeleteShader(fs);
        }
        return 0u;
    }

    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0u;
    }
    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024];
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Graphics program linking failed for %s: %s",
                       debug_name ? debug_name : "graphics", log_buf);
        glDeleteProgram(prog);
        return 0u;
    }

    if (has_cache) {
        wwal_prog_cache_save(cache_path, prog, vs_hash, main_hash, driver_hash);
    }
    return prog;
}

/* Zero-initializes a GL_RGBA16F texture via glTexSubImage2D instead of FBO clears (Invariant #2).
 */
WWAL_MAYBE_UNUSED static inline bool wwal_gl_zero_init_rgba16f(GLuint tex, uint32_t width,
                                                               uint32_t height)
{
    if (tex == 0u || width == 0u || height == 0u) {
        return false;
    }
    uint32_t chunk_h = height < 64u ? height : 64u;
    if ((size_t)width > SIZE_MAX / 8u / (size_t)chunk_h) {
        return false;
    }
    size_t chunk_bytes = (size_t)width * (size_t)chunk_h * 8u;
    void *zero_buf = calloc(1u, chunk_bytes);
    if (!zero_buf) {
        return false;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (uint32_t y = 0u; y < height; y += chunk_h) {
        uint32_t rows = (height - y < chunk_h) ? (height - y) : chunk_h;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, (GLsizei)width, (GLsizei)rows, GL_RGBA,
                        GL_HALF_FLOAT, zero_buf);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    free(zero_buf);
    return true;
}

WWAL_MAYBE_UNUSED static inline GLuint
wwal_gl_alloc_linear_scratch_tex(uint32_t width, uint32_t height, GLenum internal_format,
                                 GLenum filter, bool zero_init_if_16f)
{
    if (width == 0u || height == 0u) {
        return 0u;
    }
    GLenum fmt = (internal_format == GL_RGBA16F) ? GL_RGBA16F : GL_RGBA8;
    GLenum filt = (filter == GL_LINEAR) ? GL_LINEAR : GL_NEAREST;

    GLuint tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) {
        return 0u;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, (GLsizei)width, (GLsizei)height);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (fmt == GL_RGBA16F && zero_init_if_16f) {
        if (!wwal_gl_zero_init_rgba16f(tex, width, height)) {
            glDeleteTextures(1, &tex);
            return 0u;
        }
    }
    return tex;
}

WWAL_MAYBE_UNUSED static inline void
wwal_scratch_present_destroy(wwal_scratch_present_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->blit_prog != 0u) {
        glDeleteProgram(state->blit_prog);
    }
    if (state->draw_fbo != 0u) {
        glDeleteFramebuffers(1, &state->draw_fbo);
    }
    if (state->blit_vao != 0u) {
        glDeleteVertexArrays(1, &state->blit_vao);
    }
    memset(state, 0, sizeof(*state));
    state->loc_tex = -1;
    state->loc_swap_rb = -1;
    state->loc_force_opaque = -1;
    state->last_swap_rb = -1;
    state->last_force_opaque = -1;
}

WWAL_MAYBE_UNUSED static inline bool
wwal_gl_present_via_scratch(wwal_scratch_present_state_t *state, GLuint src_tex,
                            const wwal_target_bo_t *target_bo, bool swap_rb,
                            bool force_opaque_alpha)
{
    if (!state || src_tex == 0u || !target_bo || target_bo->gl_tex == 0u ||
        target_bo->width == 0u || target_bo->height == 0u) {
        return false;
    }

    if (state->blit_prog == 0u) {
        static const char *vs_src =
            "#version 310 es\n"
            "out highp vec2 v_uv;\n"
            "void main() {\n"
            "    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    v_uv = pos;\n"
            "    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        static const char *fs_src = "#version 310 es\n"
                                    "precision highp float;\n"
                                    "in highp vec2 v_uv;\n"
                                    "layout(binding = 0) uniform highp sampler2D u_tex;\n"
                                    "uniform int u_swap_rb;\n"
                                    "uniform int u_force_opaque;\n"
                                    "out highp vec4 frag_color;\n"
                                    "void main() {\n"
                                    "    vec4 c = texture(u_tex, v_uv);\n"
                                    "    vec3 rgb = (u_swap_rb != 0) ? c.bgr : c.rgb;\n"
                                    "    float a = (u_force_opaque != 0) ? 1.0 : c.a;\n"
                                    "    frag_color = vec4(rgb, a);\n"
                                    "}\n";
        state->blit_prog =
            wwal_gl_compile_graphics_program_cached(vs_src, fs_src, "wwal_prog_blit", NULL);
        if (state->blit_prog == 0u) {
            return false;
        }
        state->loc_tex = glGetUniformLocation(state->blit_prog, "u_tex");
        state->loc_swap_rb = glGetUniformLocation(state->blit_prog, "u_swap_rb");
        state->loc_force_opaque = glGetUniformLocation(state->blit_prog, "u_force_opaque");
        state->last_swap_rb = -1;
        state->last_force_opaque = -1;
    }

    if (state->draw_fbo == 0u) {
        glGenFramebuffers(1, &state->draw_fbo);
        if (state->draw_fbo == 0u) {
            return false;
        }
        state->bound_fbo_tex = 0u;
        wwal_fbo_lru_reset(&state->fbo_lru);
    }
    if (state->blit_vao == 0u) {
        glGenVertexArrays(1, &state->blit_vao);
        if (state->blit_vao == 0u) {
            return false;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, state->draw_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint)target_bo->gl_tex, 0);
    state->bound_fbo_tex = (GLuint)target_bo->gl_tex;

    if (!wwal_fbo_validate_attached(&state->fbo_lru, target_bo)) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        state->bound_fbo_tex = 0u;
        return false;
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);

    glUseProgram(state->blit_prog);
    if (state->loc_tex >= 0) {
        glUniform1i(state->loc_tex, 0);
    }
    int swap_val = swap_rb ? 1 : 0;
    if (state->loc_swap_rb >= 0 && state->last_swap_rb != swap_val) {
        glUniform1i(state->loc_swap_rb, swap_val);
        state->last_swap_rb = swap_val;
    }
    int opaque_val = force_opaque_alpha ? 1 : 0;
    if (state->loc_force_opaque >= 0 && state->last_force_opaque != opaque_val) {
        glUniform1i(state->loc_force_opaque, opaque_val);
        state->last_force_opaque = opaque_val;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glBindVertexArray(state->blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Leave GL unit 0, VAO, program, and FBO unbound so callers never observe stale state. */
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    state->bound_fbo_tex = 0u;
    return true;
}

WWAL_MAYBE_UNUSED static inline bool wwal_egl_has_extension(const char *extensions,
                                                            const char *ext_name)
{
    if (!extensions || !ext_name || ext_name[0] == '\0') {
        return false;
    }
    size_t ext_len = strlen(ext_name);
    const char *p = extensions;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char *end = p;
        while (*end && *end != ' ') {
            ++end;
        }
        if ((size_t)(end - p) == ext_len && strncmp(p, ext_name, ext_len) == 0) {
            return true;
        }
        p = end;
    }
    return false;
}

WWAL_MAYBE_UNUSED static inline void wwal_egl_init_sync_procs(EGLDisplay dpy,
                                                              wwal_egl_sync_procs_t *procs)
{
    if (!procs) {
        return;
    }
    memset(procs, 0, sizeof(*procs));
    if (dpy == EGL_NO_DISPLAY) {
        return;
    }
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    bool has_fence = wwal_egl_has_extension(exts, "EGL_KHR_fence_sync");
    bool has_native = wwal_egl_has_extension(exts, "EGL_ANDROID_native_fence_sync");

    if (has_fence || has_native) {
        procs->eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        procs->eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        procs->eglClientWaitSyncKHR =
            (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    }
    if (has_native) {
        procs->eglDupNativeFenceFDANDROID =
            (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    }
    procs->has_fence_sync = has_fence && procs->eglCreateSyncKHR && procs->eglDestroySyncKHR &&
                            procs->eglClientWaitSyncKHR;
    procs->has_native_fence_sync = has_native && procs->eglCreateSyncKHR &&
                                   procs->eglDestroySyncKHR && procs->eglDupNativeFenceFDANDROID;
}

WWAL_MAYBE_UNUSED static inline int
wwal_egl_create_native_fence_fd(EGLDisplay dpy, const wwal_egl_sync_procs_t *procs)
{
    if (dpy == EGL_NO_DISPLAY) {
        return -1;
    }
    wwal_egl_sync_procs_t local_procs;
    if (!procs) {
        wwal_egl_init_sync_procs(dpy, &local_procs);
        procs = &local_procs;
    }
    if (!procs->has_native_fence_sync || !procs->eglCreateSyncKHR || !procs->eglDestroySyncKHR ||
        !procs->eglDupNativeFenceFDANDROID) {
        return -1;
    }

    EGLSyncKHR sync = procs->eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        return -1;
    }
    /* EGL_ANDROID_native_fence_sync requires a GL flush so the driver materializes a kernel FD. */
    glFlush();
    EGLint fd = procs->eglDupNativeFenceFDANDROID(dpy, sync);
    procs->eglDestroySyncKHR(dpy, sync);
    if (fd < 0) {
        return -1;
    }
    int flags = fcntl((int)fd, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC) == 0) {
        (void)fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
    }
    return (int)fd;
}

WWAL_MAYBE_UNUSED static inline void wwal_egl_wait_sync_fallback(EGLDisplay dpy,
                                                                 const wwal_egl_sync_procs_t *procs)
{
    wwal_egl_sync_procs_t local_procs;
    if (!procs && dpy != EGL_NO_DISPLAY) {
        wwal_egl_init_sync_procs(dpy, &local_procs);
        procs = &local_procs;
    }
    if (dpy != EGL_NO_DISPLAY && procs && procs->eglCreateSyncKHR && procs->eglClientWaitSyncKHR &&
        procs->eglDestroySyncKHR) {
        EGLSyncKHR sync = procs->eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            procs->eglClientWaitSyncKHR(dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                        EGL_FOREVER_KHR);
            procs->eglDestroySyncKHR(dpy, sync);
            return;
        }
    }
    glFlush();
    glFinish();
}

WWAL_MAYBE_UNUSED static inline void
wwal_egl_sync_before_commit(EGLDisplay dpy, const wwal_egl_sync_procs_t *procs, int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = wwal_egl_create_native_fence_fd(dpy, procs);
        if (*out_fence_fd >= 0) {
            return;
        }
    }
    wwal_egl_wait_sync_fallback(dpy, procs);
}

typedef struct gpu_compute_ctx gpu_compute_ctx_t;

/* Initializes EGL on GBM and compiles GLES 3.1 compute shaders */
gpu_compute_ctx_t *gpu_compute_create(dmabuf_context_t *dmabuf_ctx);

/* Destroys GPU compute context and deletes all GL/EGL resources */
void gpu_compute_destroy(gpu_compute_ctx_t *ctx);

/* Returns true if EGL_ANDROID_native_fence_sync FD export is supported */
bool gpu_compute_has_native_fence(const gpu_compute_ctx_t *ctx);

/* Flushes current GL stream and exports a CLOEXEC native fence FD, or -1 if unsupported */
int gpu_compute_export_fence_fd(gpu_compute_ctx_t *ctx);

/* Blocks until prior GPU compute/blit commands finish using EGL fence sync or glFinish */
bool gpu_compute_wait_idle(gpu_compute_ctx_t *ctx);

/* Dispatches compute shader writing directly into target_bo */
bool gpu_compute_dispatch_transition(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                     dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                     const waywal_transition_params_t *params);

/* Dispatches transition and exports an async native fence FD when out_fence_fd != NULL */
bool gpu_compute_dispatch_transition_async(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                           dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                           const waywal_transition_params_t *params,
                                           int *out_fence_fd);

/* Returns the locked transition algorithm selected for the active WAYWAL_TRANSITION_RANDOM run */
waywal_transition_type_t gpu_compute_get_active_random_transition(const gpu_compute_ctx_t *ctx);

/* Releases cached OpenGL texture and EGLImageKHR associated with a BO */
void gpu_compute_release_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *bo);

/* Invalidates cached GL program and texture bindings after external context use */
void gpu_compute_invalidate_state_cache(gpu_compute_ctx_t *ctx);

/* Prepares GPU textures and FBO for hardware-accelerated transition on an SHM output */
bool gpu_compute_prepare_shm(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                             uint32_t *tex_target, uint32_t *fbo, uint32_t *cur_w, uint32_t *cur_h,
                             uint32_t width, uint32_t height, const uint32_t *old_pixels,
                             const uint32_t *new_pixels);

/* Dispatches compute shader to target texture and reads back result into dst_pixels */
bool gpu_compute_dispatch_transition_shm(gpu_compute_ctx_t *ctx, uint32_t tex_old, uint32_t tex_new,
                                         uint32_t tex_target, uint32_t fbo, uint32_t width,
                                         uint32_t height, const waywal_transition_params_t *params,
                                         uint32_t *dst_pixels);

/* Releases GL textures and FBO allocated for SHM output */
void gpu_compute_release_shm_res(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                                 uint32_t *tex_target, uint32_t *fbo);

/* Hot-compiles and loads an arbitrary custom GLSL compute shader */
bool gpu_compute_load_custom_shader(gpu_compute_ctx_t *ctx, const char *src);

/* Uploads and scales static RGBA8/BGRA8 pixels or solid color into dst_bo on the GPU */
bool gpu_compute_upload_static(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                               uint32_t src_h, dmabuf_bo_t *dst_bo, waywal_scaling_mode_t scaling,
                               uint32_t clear_color);

bool gpu_compute_upload_static_async(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                     uint32_t src_w, uint32_t src_h, dmabuf_bo_t *dst_bo,
                                     waywal_scaling_mode_t scaling, uint32_t clear_color,
                                     int *out_fence_fd);

bool gpu_compute_scale_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *src_bo, dmabuf_bo_t *dst_bo,
                          waywal_scaling_mode_t scaling, uint32_t clear_color, bool release_src_gl);

bool gpu_compute_scale_bo_async(gpu_compute_ctx_t *ctx, dmabuf_bo_t *src_bo, dmabuf_bo_t *dst_bo,
                                waywal_scaling_mode_t scaling, uint32_t clear_color,
                                bool release_src_gl, int *out_fence_fd);

void gpu_compute_make_current(gpu_compute_ctx_t *ctx);

void gpu_compute_set_span_bounds(gpu_compute_ctx_t *ctx, int32_t off_x, int32_t off_y,
                                 int32_t total_w, int32_t total_h);
void gpu_compute_set_crop_gravity(gpu_compute_ctx_t *ctx, float gx, float gy);

bool gpu_compute_render_kawase_blur(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                                    uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t radius);

bool gpu_compute_render_kawase_blur_async(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                          uint32_t src_w, uint32_t src_h, dmabuf_bo_t *dst_bo,
                                          uint32_t radius, int *out_fence_fd);

void gpu_compute_bump_upload_gen(gpu_compute_ctx_t *ctx);
void gpu_compute_release_upload_cache(gpu_compute_ctx_t *ctx);

void layer_shell_blur_rgba_cpu(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_GPU_COMPUTE_H */
