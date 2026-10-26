// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/shader_engine.h"

#include "waywal/log.h"

#if __has_include("linux-dmabuf-v1-client-protocol.h")
#include "waywal/dmabuf.h"
#else
#include <drm_fourcc.h>
#include <gbm.h>
#include <sys/types.h>
#define WAYWAL_MAX_BUFFER_PLANES 4
struct dmabuf_bo {
    int fd[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t stride[WAYWAL_MAX_BUFFER_PLANES];
    uint32_t offset[WAYWAL_MAX_BUFFER_PLANES];
    uint64_t modifier;
    uint32_t num_planes;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    struct gbm_bo *gbm_bo;
    struct wl_buffer *wl_buffer;
    void *egl_image;
    uint32_t gl_tex;
    bool in_use;
};
struct dmabuf_context {
    int drm_fd;
    struct gbm_device *gbm;
    void *dmabuf_proto;
    void *default_feedback;
    uint64_t preferred_modifier;
    uint32_t preferred_format;
    bool has_10bit;
    uint32_t format_10bit;
    uint64_t modifier_10bit;
    bool has_nv12;
    bool has_p010;
    dev_t compositor_dev;
    bool has_compositor_dev;
    bool available;
    bool map_supported;
};
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on

#define SHADER_CACHE_MAGIC 0x57534842u /* "WSHB" */

typedef struct {
    uint32_t magic;
    uint32_t binary_format;
    uint64_t source_hash;
    uint32_t binary_len;
} shader_cache_header_t;

typedef struct {
    GLint iResolution;
    GLint iTime;
    GLint iTimeDelta;
    GLint iFrame;
    GLint iMouse;
    GLint iDate;
    GLint iChannel0;
    GLint u_audio_bands;
    GLint u_audio_envelope;
    GLint u_audio_beat;
    GLint u_palette_primary;
    GLint u_palette_secondary;
    GLint u_palette_tertiary;
} shader_uniforms_t;

struct waywal_shader_engine {
    EGLDisplay display;
    EGLContext context;
    bool owns_egl;

    GLuint active_prog;
    shader_uniforms_t uniforms;
    GLuint audio_tex;
    bool last_from_cache;

    char gpu_id[256];
    char watched_path[4096];
    char watched_dir[4096];
    char watched_base[256];
    int inotify_fd;
    int watch_file_wd;
    int watch_dir_wd;

    void *shaderc_lib;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
};

static const char SHADERTOY_PROLOGUE[] =
    "#version 310 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_dst;\n"
    "uniform vec3 iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iMouse;\n"
    "uniform vec4 iDate;\n"
    "uniform highp sampler2D iChannel0;\n"
    "uniform float u_audio_bands[64];\n"
    "uniform float u_audio_envelope;\n"
    "uniform float u_audio_beat;\n"
    "uniform vec3 u_palette_primary;\n"
    "uniform vec3 u_palette_secondary;\n"
    "uniform vec3 u_palette_tertiary;\n"
    "\n";

static const char SHADERTOY_EPILOGUE[] =
    "\n"
    "void main() {\n"
    "    ivec2 px = ivec2(gl_GlobalInvocationID.xy);\n"
    "    int w = int(iResolution.x);\n"
    "    int h = int(iResolution.y);\n"
    "    if (px.x >= w || px.y >= h) {\n"
    "        return;\n"
    "    }\n"
    "    vec2 fragCoord = vec2(float(px.x) + 0.5, float(h - 1 - px.y) + 0.5);\n"
    "    vec4 fragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    mainImage(fragColor, fragCoord);\n"
    "    fragColor = clamp(fragColor, 0.0, 1.0);\n"
    "    imageStore(u_dst, px, vec4(fragColor.bgr, 1.0));\n"
    "}\n";

char *waywal_shader_wrap_source(const char *user_src)
{
    if (!user_src) {
        return NULL;
    }

    if (strstr(user_src, "local_size_x") != NULL && strstr(user_src, "#version") != NULL) {
        return strdup(user_src);
    }

    const char *body = user_src;
    while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') {
        body++;
    }
    if (strncmp(body, "#version", 8) == 0) {
        const char *nl = strchr(body, '\n');
        body = nl ? (nl + 1) : (body + strlen(body));
    }

    size_t pro_len = sizeof(SHADERTOY_PROLOGUE) - 1u;
    size_t body_len = strlen(body);
    size_t epi_len = sizeof(SHADERTOY_EPILOGUE) - 1u;

    char *wrapped = (char *)malloc(pro_len + body_len + epi_len + 1u);
    if (!wrapped) {
        return NULL;
    }

    memcpy(wrapped, SHADERTOY_PROLOGUE, pro_len);
    memcpy(wrapped + pro_len, body, body_len);
    memcpy(wrapped + pro_len + body_len, SHADERTOY_EPILOGUE, epi_len);
    wrapped[pro_len + body_len + epi_len] = '\0';
    return wrapped;
}

uint64_t waywal_shader_compute_cache_key(const char *wrapped_src, const char *gpu_id)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;

    if (wrapped_src) {
        for (const unsigned char *p = (const unsigned char *)wrapped_src; *p; ++p) {
            hash ^= (uint64_t)(*p);
            hash *= prime;
        }
    }
    hash ^= 0xFFu;
    hash *= prime;
    if (gpu_id) {
        for (const unsigned char *p = (const unsigned char *)gpu_id; *p; ++p) {
            hash ^= (uint64_t)(*p);
            hash *= prime;
        }
    }
    return hash;
}

static bool build_cache_filepath(uint64_t key, char *out_path, size_t out_sz)
{
    char dir[3800];
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0] != '\0') {
        if (snprintf(dir, sizeof(dir), "%s/wwal", xdg) >= (int)sizeof(dir)) {
            return false;
        }
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') {
            return false;
        }
        if (snprintf(dir, sizeof(dir), "%s/.cache", home) >= (int)sizeof(dir)) {
            return false;
        }
        mkdir(dir, 0755);
        if (snprintf(dir, sizeof(dir), "%s/.cache/wwal", home) >= (int)sizeof(dir)) {
            return false;
        }
    }
    mkdir(dir, 0755);

    char shader_dir[3900];
    if (snprintf(shader_dir, sizeof(shader_dir), "%s/shaders", dir) >= (int)sizeof(shader_dir)) {
        return false;
    }
    mkdir(shader_dir, 0755);

    int n = snprintf(out_path, out_sz, "%s/%016llx.bin", shader_dir, (unsigned long long)key);
    return n > 0 && (size_t)n < out_sz;
}

static GLuint try_load_cached_program(uint64_t key)
{
    char path[4096];
    if (!build_cache_filepath(key, path, sizeof(path))) {
        return 0;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    shader_cache_header_t hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) || hdr.magic != SHADER_CACHE_MAGIC ||
        hdr.source_hash != key || hdr.binary_len == 0 || hdr.binary_len > (16u * 1024u * 1024u)) {
        close(fd);
        return 0;
    }

    uint8_t *bin = (uint8_t *)malloc(hdr.binary_len);
    if (!bin) {
        close(fd);
        return 0;
    }

    ssize_t rd = read(fd, bin, hdr.binary_len);
    close(fd);
    if (rd != (ssize_t)hdr.binary_len) {
        free(bin);
        return 0;
    }

    GLuint prog = glCreateProgram();
    if (!prog) {
        free(bin);
        return 0;
    }

    glProgramBinary(prog, (GLenum)hdr.binary_format, bin, (GLsizei)hdr.binary_len);
    free(bin);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void save_program_to_cache(GLuint prog, uint64_t key)
{
    GLint bin_len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &bin_len);
    if (bin_len <= 0) {
        return;
    }

    uint8_t *bin = (uint8_t *)malloc((size_t)bin_len);
    if (!bin) {
        return;
    }

    GLsizei actual_len = 0;
    GLenum bin_format = 0;
    glGetProgramBinary(prog, bin_len, &actual_len, &bin_format, bin);
    if (actual_len <= 0) {
        free(bin);
        return;
    }

    char final_path[4000];
    char tmp_path[4096];
    if (!build_cache_filepath(key, final_path, sizeof(final_path))) {
        free(bin);
        return;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", final_path, (int)getpid()) >=
        (int)sizeof(tmp_path)) {
        free(bin);
        return;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(bin);
        return;
    }

    shader_cache_header_t hdr = {
        .magic = SHADER_CACHE_MAGIC,
        .binary_format = (uint32_t)bin_format,
        .source_hash = key,
        .binary_len = (uint32_t)actual_len,
    };

    bool ok = (write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) &&
              (write(fd, bin, (size_t)actual_len) == (ssize_t)actual_len);
    close(fd);
    free(bin);

    if (ok) {
        rename(tmp_path, final_path);
    } else {
        unlink(tmp_path);
    }
}

static void cache_uniform_locations(GLuint prog, shader_uniforms_t *u)
{
    u->iResolution = glGetUniformLocation(prog, "iResolution");
    u->iTime = glGetUniformLocation(prog, "iTime");
    u->iTimeDelta = glGetUniformLocation(prog, "iTimeDelta");
    u->iFrame = glGetUniformLocation(prog, "iFrame");
    u->iMouse = glGetUniformLocation(prog, "iMouse");
    u->iDate = glGetUniformLocation(prog, "iDate");
    u->iChannel0 = glGetUniformLocation(prog, "iChannel0");
    u->u_audio_bands = glGetUniformLocation(prog, "u_audio_bands");
    u->u_audio_envelope = glGetUniformLocation(prog, "u_audio_envelope");
    u->u_audio_beat = glGetUniformLocation(prog, "u_audio_beat");
    u->u_palette_primary = glGetUniformLocation(prog, "u_palette_primary");
    u->u_palette_secondary = glGetUniformLocation(prog, "u_palette_secondary");
    u->u_palette_tertiary = glGetUniformLocation(prog, "u_palette_tertiary");
}

static void make_context_current(waywal_shader_engine_t *eng)
{
    if (eng && eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != eng->context) {
            eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, eng->context);
        }
    }
}

waywal_shader_engine_t *waywal_shader_engine_create(dmabuf_context_t *dmabuf_ctx)
{
    waywal_shader_engine_t *eng =
        (waywal_shader_engine_t *)calloc(1, sizeof(waywal_shader_engine_t));
    if (!eng) {
        return NULL;
    }

    eng->inotify_fd = -1;
    eng->watch_file_wd = -1;
    eng->watch_dir_wd = -1;
    eng->display = eglGetCurrentDisplay();
    eng->context = eglGetCurrentContext();

    if (eng->display == EGL_NO_DISPLAY || eng->context == EGL_NO_CONTEXT) {
        if (!dmabuf_ctx || !dmabuf_ctx->gbm || !dmabuf_ctx->available) {
            free(eng);
            return NULL;
        }

        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLDisplay dpy = EGL_NO_DISPLAY;
        if (get_platform_display) {
            dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, dmabuf_ctx->gbm, NULL);
        }
        if (dpy == EGL_NO_DISPLAY) {
            dpy = eglGetDisplay((EGLNativeDisplayType)dmabuf_ctx->gbm);
        }
        if (dpy == EGL_NO_DISPLAY) {
            free(eng);
            return NULL;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API)) {
            eglTerminate(dpy);
            free(eng);
            return NULL;
        }

        static const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE,
        };
        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            if (ctx != EGL_NO_CONTEXT) {
                eglDestroyContext(dpy, ctx);
            }
            eglTerminate(dpy);
            free(eng);
            return NULL;
        }

        eng->display = dpy;
        eng->context = ctx;
        eng->owns_egl = true;
    }

    eng->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eng->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    eng->glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    eng->glEGLImageTargetTexStorageEXT =
        (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    snprintf(eng->gpu_id, sizeof(eng->gpu_id), "%s|%s", renderer ? renderer : "gpu",
             version ? version : "gles31");

    glGenTextures(1, &eng->audio_tex);
    glBindTexture(GL_TEXTURE_2D, eng->audio_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Swizzle R->RGB so Shadertoy shaders reading texture(iChannel0, uv).x or .rgb both work. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);

    uint8_t zero_tex[WAYWAL_AUDIO_TEX_WIDTH * WAYWAL_AUDIO_TEX_HEIGHT];
    memset(zero_tex, 0, WAYWAL_AUDIO_TEX_WIDTH);
    memset(zero_tex + WAYWAL_AUDIO_TEX_WIDTH, 128, WAYWAL_AUDIO_TEX_WIDTH);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, WAYWAL_AUDIO_TEX_WIDTH, WAYWAL_AUDIO_TEX_HEIGHT, 0,
                 GL_RED, GL_UNSIGNED_BYTE, zero_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    eng->shaderc_lib = dlopen("libshaderc_shared.so.1", RTLD_LAZY);
    eng->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    return eng;
}

void waywal_shader_engine_destroy(waywal_shader_engine_t *eng)
{
    if (!eng) {
        return;
    }
    make_context_current(eng);
    if (eng->active_prog) {
        glDeleteProgram(eng->active_prog);
    }
    if (eng->audio_tex) {
        glDeleteTextures(1, &eng->audio_tex);
    }
    if (eng->owns_egl) {
        eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(eng->display, eng->context);
    }
    if (eng->inotify_fd >= 0) {
        close(eng->inotify_fd);
    }
    if (eng->shaderc_lib) {
        dlclose(eng->shaderc_lib);
    }
    free(eng);
}

bool waywal_shader_engine_load_source(waywal_shader_engine_t *eng, const char *user_src,
                                      const char *debug_name)
{
    if (!eng || !user_src) {
        return false;
    }

    make_context_current(eng);

    char *wrapped = waywal_shader_wrap_source(user_src);
    if (!wrapped) {
        return false;
    }

    uint64_t key = waywal_shader_compute_cache_key(wrapped, eng->gpu_id);
    GLuint prog = try_load_cached_program(key);
    if (prog != 0) {
        free(wrapped);
        if (eng->active_prog) {
            glDeleteProgram(eng->active_prog);
        }
        eng->active_prog = prog;
        eng->last_from_cache = true;
        cache_uniform_locations(prog, &eng->uniforms);
        return true;
    }

    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!shader) {
        free(wrapped);
        return false;
    }

    const char *src_ptr = wrapped;
    glShaderSource(shader, 1, &src_ptr, NULL);
    glCompileShader(shader);
    free(wrapped);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log_buf[1024];
        glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Shader compilation failed (%s): %s", debug_name ? debug_name : "live",
                       log_buf);
        glDeleteShader(shader);
        return false;
    }

    prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(shader);
        return false;
    }

    glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    glDeleteShader(shader);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024];
        glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Shader link failed (%s): %s", debug_name ? debug_name : "live", log_buf);
        glDeleteProgram(prog);
        return false;
    }

    save_program_to_cache(prog, key);

    if (eng->active_prog) {
        glDeleteProgram(eng->active_prog);
    }
    eng->active_prog = prog;
    eng->last_from_cache = false;
    cache_uniform_locations(prog, &eng->uniforms);
    return true;
}

static char *read_entire_file(const char *filepath)
{
    int fd = open(filepath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (4 * 1024 * 1024)) {
        close(fd);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) {
        close(fd);
        return NULL;
    }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n != st.st_size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static void setup_file_watches(waywal_shader_engine_t *eng, const char *filepath)
{
    if (eng->inotify_fd < 0 || !filepath) {
        return;
    }
    if (eng->watch_file_wd >= 0) {
        inotify_rm_watch(eng->inotify_fd, eng->watch_file_wd);
        eng->watch_file_wd = -1;
    }
    if (eng->watch_dir_wd >= 0) {
        inotify_rm_watch(eng->inotify_fd, eng->watch_dir_wd);
        eng->watch_dir_wd = -1;
    }

    snprintf(eng->watched_path, sizeof(eng->watched_path), "%s", filepath);
    const char *slash = strrchr(filepath, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - filepath);
        if (dir_len == 0) {
            snprintf(eng->watched_dir, sizeof(eng->watched_dir), "/");
        } else if (dir_len < sizeof(eng->watched_dir)) {
            memcpy(eng->watched_dir, filepath, dir_len);
            eng->watched_dir[dir_len] = '\0';
        }
        snprintf(eng->watched_base, sizeof(eng->watched_base), "%s", slash + 1);
    } else {
        snprintf(eng->watched_dir, sizeof(eng->watched_dir), ".");
        snprintf(eng->watched_base, sizeof(eng->watched_base), "%s", filepath);
    }

    eng->watch_file_wd =
        inotify_add_watch(eng->inotify_fd, eng->watched_path, IN_CLOSE_WRITE | IN_MODIFY);
    eng->watch_dir_wd =
        inotify_add_watch(eng->inotify_fd, eng->watched_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
}

bool waywal_shader_engine_load_file(waywal_shader_engine_t *eng, const char *filepath)
{
    if (!eng || !filepath) {
        return false;
    }
    char *src = read_entire_file(filepath);
    if (!src) {
        return false;
    }
    bool ok = waywal_shader_engine_load_source(eng, src, filepath);
    free(src);
    if (ok) {
        setup_file_watches(eng, filepath);
    }
    return ok;
}

int waywal_shader_engine_get_inotify_fd(const waywal_shader_engine_t *eng)
{
    return eng ? eng->inotify_fd : -1;
}

bool waywal_shader_engine_poll_hot_reload(waywal_shader_engine_t *eng)
{
    if (!eng || eng->inotify_fd < 0 || eng->watched_path[0] == '\0') {
        return false;
    }

    alignas(struct inotify_event) uint8_t buf[4096];
    bool triggered = false;

    for (;;) {
        ssize_t n = read(eng->inotify_fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        size_t off = 0;
        while (off < (size_t)n) {
            const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
            if (ev->wd == eng->watch_file_wd) {
                triggered = true;
            } else if (ev->wd == eng->watch_dir_wd && ev->len > 0) {
                if (strcmp(ev->name, eng->watched_base) == 0) {
                    triggered = true;
                }
            }
            off += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (!triggered) {
        return false;
    }

    char *src = read_entire_file(eng->watched_path);
    if (!src) {
        return false;
    }
    bool ok = waywal_shader_engine_load_source(eng, src, eng->watched_path);
    free(src);

    /* Re-arm file watch if an atomic editor rename replaced the inode. */
    if (eng->watch_file_wd >= 0) {
        inotify_rm_watch(eng->inotify_fd, eng->watch_file_wd);
    }
    eng->watch_file_wd =
        inotify_add_watch(eng->inotify_fd, eng->watched_path, IN_CLOSE_WRITE | IN_MODIFY);
    return ok;
}

static bool ensure_bo_imported(waywal_shader_engine_t *eng, dmabuf_bo_t *bo)
{
    if (!eng || !bo) {
        return false;
    }
    if (bo->egl_image && bo->gl_tex != 0) {
        return true;
    }
    if (!eng->eglCreateImageKHR || !eng->glEGLImageTargetTexture2DOES) {
        return false;
    }

    static const EGLint plane_fd_keys[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLint plane_offset_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLint plane_pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLint plane_mod_lo_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLint plane_mod_hi_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    uint32_t num_planes = bo->num_planes > 0 ? bo->num_planes : 1u;
    if (num_planes > 4u) {
        num_planes = 4u;
    }

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)bo->width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)bo->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)bo->drm_format;

    for (uint32_t p = 0; p < num_planes; ++p) {
        if (bo->fd[p] < 0) {
            continue;
        }
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_offset_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];
        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo_keys[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFFu);
            attribs[n++] = plane_mod_hi_keys[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32u) & 0xFFFFFFFFu);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img =
        eng->eglCreateImageKHR(eng->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (eng->glEGLImageTargetTexStorageEXT) {
        eng->glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        eng->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    bo->egl_image = img;
    bo->gl_tex = tex;
    return true;
}

bool waywal_shader_engine_render_frame(waywal_shader_engine_t *eng, dmabuf_bo_t *target_bo,
                                       const waywal_shader_frame_params_t *params)
{
    if (!eng || !target_bo || !params || !eng->active_prog) {
        return false;
    }

    make_context_current(eng);
    if (!ensure_bo_imported(eng, target_bo)) {
        return false;
    }

    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glUseProgram(eng->active_prog);

    const shader_uniforms_t *u = &eng->uniforms;
    if (u->iResolution >= 0) {
        glUniform3f(u->iResolution, (float)target_bo->width, (float)target_bo->height, 1.0f);
    }
    if (u->iTime >= 0) {
        glUniform1f(u->iTime, params->time_sec);
    }
    if (u->iTimeDelta >= 0) {
        glUniform1f(u->iTimeDelta, params->time_delta_sec);
    }
    if (u->iFrame >= 0) {
        glUniform1i(u->iFrame, params->frame_index);
    }
    if (u->iMouse >= 0) {
        glUniform4fv(u->iMouse, 1, params->mouse);
    }
    if (u->iDate >= 0) {
        glUniform4fv(u->iDate, 1, params->date);
    }
    if (u->u_palette_primary >= 0) {
        glUniform3fv(u->u_palette_primary, 1, params->palette_primary);
    }
    if (u->u_palette_secondary >= 0) {
        glUniform3fv(u->u_palette_secondary, 1, params->palette_secondary);
    }
    if (u->u_palette_tertiary >= 0) {
        glUniform3fv(u->u_palette_tertiary, 1, params->palette_tertiary);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eng->audio_tex);
    if (params->audio) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WAYWAL_AUDIO_TEX_WIDTH, WAYWAL_AUDIO_TEX_HEIGHT,
                        GL_RED, GL_UNSIGNED_BYTE, params->audio->shadertoy_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (u->u_audio_bands >= 0) {
            glUniform1fv(u->u_audio_bands, WAYWAL_AUDIO_BARK_BANDS, params->audio->bands);
        }
        if (u->u_audio_envelope >= 0) {
            glUniform1f(u->u_audio_envelope, params->audio->envelope);
        }
        if (u->u_audio_beat >= 0) {
            glUniform1f(u->u_audio_beat, params->audio->beat_pulse);
        }
    }
    if (u->iChannel0 >= 0) {
        glUniform1i(u->iChannel0, 0);
    }

    glBindImageTexture(0, target_bo->gl_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    GLuint groups_x = (target_bo->width + 15u) / 16u;
    GLuint groups_y = (target_bo->height + 15u) / 16u;
    glDispatchCompute(groups_x, groups_y, 1u);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT |
                    GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glUseProgram((GLuint)prev_prog);
    glFlush();
    return true;
}

bool waywal_shader_engine_has_active_program(const waywal_shader_engine_t *eng)
{
    return eng && eng->active_prog != 0;
}

bool waywal_shader_engine_last_load_from_cache(const waywal_shader_engine_t *eng)
{
    return eng && eng->last_from_cache;
}
