// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/shader_engine.h"

#include "image_loader.h"
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
    void *map_data;
    uint32_t gl_tex;
    bool is_10bit;
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on

#define SHADER_CACHE_MAGIC 0x57534842u /* "WSHB" */
#define SHADER_NUM_PASSES  5           /* 0..3 = BUFFER_A..D, 4 = IMAGE */
#define SHADER_NOISE_DIM   256u

typedef enum {
    SHADER_CHAN_AUDIO = 0,
    SHADER_CHAN_BUFFER_A = 1,
    SHADER_CHAN_BUFFER_B = 2,
    SHADER_CHAN_BUFFER_C = 3,
    SHADER_CHAN_BUFFER_D = 4,
    SHADER_CHAN_NOISE = 5,
    SHADER_CHAN_WALLPAPER = 6,
    SHADER_CHAN_DEPTH = 7,
    SHADER_CHAN_IMAGE = 8,
} shader_channel_target_t;

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
    GLint iSampleRate;
    GLint iChannel[4];
    GLint iChannelResolution;
    GLint iChannelTime;
    GLint u_audio_bands;
    GLint u_audio_envelope;
    GLint u_audio_beat;
    GLint u_palette_primary;
    GLint u_palette_secondary;
    GLint u_palette_tertiary;
    GLint u_mouse_smooth;
    GLint u_mouse_vel;
    GLint u_workspace_offset;
    GLint u_depth_strength;
    GLint u_swap_rb;
    GLint u_flip_y;
} shader_uniforms_t;

typedef struct {
    shader_channel_target_t target;
    char image_path[1024];
    GLuint custom_tex;
    uint32_t custom_w;
    uint32_t custom_h;
} shader_channel_binding_t;

typedef struct {
    bool active;
    GLuint prog;
    shader_uniforms_t uniforms;
    shader_channel_binding_t channels[4];
    GLuint ping_pong_tex[2];
    uint32_t tex_w;
    uint32_t tex_h;
    int read_idx;
    bool uniforms_initialized;
    uint32_t cached_res_w;
    uint32_t cached_res_h;
    int cached_swap_rb;
    int cached_flip_y;
    float cached_depth_strength;
    float cached_workspace_offset[2];
} shader_pass_t;

struct waywal_shader_engine {
    EGLDisplay display;
    EGLContext context;
    bool owns_egl;

    shader_pass_t passes[SHADER_NUM_PASSES];
    GLuint audio_tex;
    GLuint noise_tex;
    GLuint wallpaper_tex;
    uint32_t wallpaper_w;
    uint32_t wallpaper_h;
    GLuint depth_tex;
    uint32_t depth_w;
    uint32_t depth_h;
    float depth_strength;

    GLuint scratch_tex;
    uint32_t scratch_w;
    uint32_t scratch_h;
    GLuint read_fbo;
    GLuint draw_fbo;
    GLuint prog_blit;
    GLuint blit_vao;

    bool last_from_cache;

    char gpu_id[256];
    char watched_path[4096];
    char watched_dir[4096];
    char watched_base[256];
    int inotify_fd;
    int watch_file_wd;
    int watch_dir_wd;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;

    struct {
        dmabuf_bo_t *bo;
        GLuint gl_tex;
        EGLImageKHR egl_image;
    } imported_bos[32];
    size_t num_imported_bos;
};

static const char SHADERTOY_PROLOGUE[] =
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "#ifdef IMAGE_PASS\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_dst;\n"
    "#else\n"
    "layout(rgba16f, binding = 0) writeonly uniform highp image2D u_dst;\n"
    "#endif\n"
    "uniform vec3 iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iMouse;\n"
    "uniform vec4 iDate;\n"
    "uniform float iSampleRate;\n"
    "uniform highp sampler2D iChannel0;\n"
    "uniform highp sampler2D iChannel1;\n"
    "uniform highp sampler2D iChannel2;\n"
    "uniform highp sampler2D iChannel3;\n"
    "uniform vec3 iChannelResolution[4];\n"
    "uniform float iChannelTime[4];\n"
    "uniform float u_audio_bands[64];\n"
    "uniform float u_audio_envelope;\n"
    "uniform float u_audio_beat;\n"
    "uniform vec3 u_palette_primary;\n"
    "uniform vec3 u_palette_secondary;\n"
    "uniform vec3 u_palette_tertiary;\n"
    "uniform vec4 u_mouse_smooth;\n"
    "uniform vec2 u_mouse_vel;\n"
    "uniform vec2 u_workspace_offset;\n"
    "uniform float u_depth_strength;\n"
    "uniform int u_swap_rb;\n"
    "uniform int u_flip_y;\n"
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
    "    float fy = (u_flip_y != 0) ? float(h - 1 - px.y) : float(px.y);\n"
    "    vec2 fragCoord = vec2(float(px.x) + 0.5, fy + 0.5);\n"
    "    vec4 fragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    mainImage(fragColor, fragCoord);\n"
    "#ifdef IMAGE_PASS\n"
    "    fragColor = clamp(fragColor, 0.0, 1.0);\n"
    "    if (u_swap_rb != 0) {\n"
    "        imageStore(u_dst, px, vec4(fragColor.bgr, 1.0));\n"
    "    } else {\n"
    "        imageStore(u_dst, px, vec4(fragColor.rgb, 1.0));\n"
    "    }\n"
    "#else\n"
    "    imageStore(u_dst, px, fragColor);\n"
    "#endif\n"
    "}\n";

static const char DEPTH_PARALLAX_POM_SHADER[] =
    "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
    "    vec2 dst_res = max(iResolution.xy, vec2(1.0));\n"
    "    vec2 src_res = max(iChannelResolution[0].xy, dst_res);\n"
    "    vec2 uv = fragCoord / dst_res;\n"
    "    uv.y = 1.0 - uv.y;\n"
    "    float r_src = src_res.x / src_res.y;\n"
    "    float r_dst = dst_res.x / dst_res.y;\n"
    "    if (r_src > r_dst) {\n"
    "        float scale = r_dst / r_src;\n"
    "        uv.x = (uv.x - 0.5) * scale + 0.5;\n"
    "    } else {\n"
    "        float scale = r_src / r_dst;\n"
    "        uv.y = (uv.y - 0.5) * scale + 0.5;\n"
    "    }\n"
    "    vec2 mouse_norm = (u_mouse_smooth.xy / dst_res) - vec2(0.5);\n"
    "    mouse_norm.y = -mouse_norm.y;\n"
    "    float strength = (u_depth_strength > 0.0) ? u_depth_strength : 1.0;\n"
    "    vec2 parallax = (mouse_norm * 0.045 + u_workspace_offset * 0.025) * strength;\n"
    "    uv = (uv - 0.5) * 0.94 + 0.5;\n"
    "    const int NUM_STEPS = 16;\n"
    "    float step_size = 1.0 / float(NUM_STEPS);\n"
    "    vec2 delta_uv = parallax / float(NUM_STEPS);\n"
    "    vec2 cur_uv = uv;\n"
    "    float cur_layer = 0.0;\n"
    "    float cur_depth = 1.0 - texture(iChannel1, clamp(cur_uv, 0.0, 1.0)).r;\n"
    "    for (int i = 0; i < NUM_STEPS; ++i) {\n"
    "        if (cur_layer >= cur_depth) {\n"
    "            break;\n"
    "        }\n"
    "        cur_uv -= delta_uv;\n"
    "        cur_depth = 1.0 - texture(iChannel1, clamp(cur_uv, 0.0, 1.0)).r;\n"
    "        cur_layer += step_size;\n"
    "    }\n"
    "    vec2 prev_uv = cur_uv + delta_uv;\n"
    "    float after_depth = cur_depth - cur_layer;\n"
    "    float before_depth = (1.0 - texture(iChannel1, clamp(prev_uv, 0.0, 1.0)).r) -\n"
    "                         cur_layer + step_size;\n"
    "    float weight = clamp(after_depth / max(after_depth - before_depth, 1e-4), 0.0, 1.0);\n"
    "    vec2 final_uv = clamp(mix(cur_uv, prev_uv, weight), 0.0, 1.0);\n"
    "    fragColor = vec4(texture(iChannel0, final_uv).rgb, 1.0);\n"
    "}\n";

static int match_pass_tag(const char *tag, size_t tag_len)
{
    while (tag_len > 0 && (*tag == ' ' || *tag == '\t')) {
        tag++;
        tag_len--;
    }
    while (tag_len > 0 &&
           (tag[tag_len - 1] == ' ' || tag[tag_len - 1] == '\t' || tag[tag_len - 1] == '\r')) {
        tag_len--;
    }
    if (tag_len == 6 && strncasecmp(tag, "COMMON", 6) == 0) {
        return -1;
    }
    if (tag_len == 8 &&
        (strncasecmp(tag, "BUFFER A", 8) == 0 || strncasecmp(tag, "BUFFER_A", 8) == 0)) {
        return 0;
    }
    if (tag_len == 7 && strncasecmp(tag, "BUFFERA", 7) == 0) {
        return 0;
    }
    if (tag_len == 8 &&
        (strncasecmp(tag, "BUFFER B", 8) == 0 || strncasecmp(tag, "BUFFER_B", 8) == 0)) {
        return 1;
    }
    if (tag_len == 7 && strncasecmp(tag, "BUFFERB", 7) == 0) {
        return 1;
    }
    if (tag_len == 8 &&
        (strncasecmp(tag, "BUFFER C", 8) == 0 || strncasecmp(tag, "BUFFER_C", 8) == 0)) {
        return 2;
    }
    if (tag_len == 7 && strncasecmp(tag, "BUFFERC", 7) == 0) {
        return 2;
    }
    if (tag_len == 8 &&
        (strncasecmp(tag, "BUFFER D", 8) == 0 || strncasecmp(tag, "BUFFER_D", 8) == 0)) {
        return 3;
    }
    if (tag_len == 7 && strncasecmp(tag, "BUFFERD", 7) == 0) {
        return 3;
    }
    if (tag_len == 5 && strncasecmp(tag, "IMAGE", 5) == 0) {
        return 4;
    }
    return -2;
}

static int match_section_marker(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i + 2 <= len && line[i] == '/' && line[i + 1] == '/') {
        i += 2;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) {
            i++;
        }
    }
    if (i < len && line[i] == '[') {
        i++;
        const char *tag = line + i;
        size_t tag_len = 0;
        while (i + tag_len < len && line[i + tag_len] != ']') {
            tag_len++;
        }
        if (i + tag_len >= len || line[i + tag_len] != ']') {
            return -2;
        }
        return match_pass_tag(tag, tag_len);
    }
    if (i + 19 <= len && strncasecmp(line + i, "#pragma waywal pass", 19) == 0) {
        i += 19;
        return match_pass_tag(line + i, len - i);
    }
    return -2;
}

static bool has_any_section_markers(const char *src, bool *out_has_pass_sec)
{
    bool any = false;
    for (int i = 0; i < SHADER_NUM_PASSES; ++i) {
        out_has_pass_sec[i] = false;
    }
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int sec = match_section_marker(p, len);
        if (sec >= -1) {
            any = true;
            if (sec >= 0 && sec < SHADER_NUM_PASSES) {
                out_has_pass_sec[sec] = true;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return any;
}

static bool detect_active_passes(const char *user_src, bool out_active[SHADER_NUM_PASSES])
{
    for (int i = 0; i < SHADER_NUM_PASSES; ++i) {
        out_active[i] = false;
    }
    out_active[4] = true; /* IMAGE pass always active */
    if (!user_src) {
        return false;
    }

    bool sec_present[SHADER_NUM_PASSES];
    if (has_any_section_markers(user_src, sec_present)) {
        for (int i = 0; i < 4; ++i) {
            out_active[i] = sec_present[i];
        }
    }
    if (strstr(user_src, "BUFFER_A") != NULL) {
        out_active[0] = true;
    }
    if (strstr(user_src, "BUFFER_B") != NULL) {
        out_active[1] = true;
    }
    if (strstr(user_src, "BUFFER_C") != NULL) {
        out_active[2] = true;
    }
    if (strstr(user_src, "BUFFER_D") != NULL) {
        out_active[3] = true;
    }
    return true;
}

char *waywal_shader_wrap_pass_source(const char *user_src, int pass_idx)
{
    if (!user_src || pass_idx < 0 || pass_idx >= SHADER_NUM_PASSES) {
        return NULL;
    }

    bool active_passes[SHADER_NUM_PASSES];
    detect_active_passes(user_src, active_passes);
    if (!active_passes[pass_idx]) {
        return NULL;
    }

    if (pass_idx == 4 && strstr(user_src, "local_size_x") != NULL &&
        strstr(user_src, "#version") != NULL) {
        return strdup(user_src);
    }

    static const char *const pass_defines[SHADER_NUM_PASSES] = {
        "#define BUFFER_A 1\n", "#define BUFFER_B 1\n",   "#define BUFFER_C 1\n",
        "#define BUFFER_D 1\n", "#define IMAGE_PASS 1\n",
    };

    bool sec_present[SHADER_NUM_PASSES];
    bool use_sections = has_any_section_markers(user_src, sec_present);

    size_t pro_len = sizeof(SHADERTOY_PROLOGUE) - 1u;
    size_t def_len = strlen(pass_defines[pass_idx]);
    size_t src_len = strlen(user_src);
    size_t epi_len = sizeof(SHADERTOY_EPILOGUE) - 1u;

    char *wrapped = (char *)malloc(pro_len + def_len + src_len + epi_len + 32u);
    if (!wrapped) {
        return NULL;
    }

    static const char ver_hdr[] = "#version 310 es\n";
    size_t ver_len = sizeof(ver_hdr) - 1u;
    size_t out_off = 0;
    memcpy(wrapped + out_off, ver_hdr, ver_len);
    out_off += ver_len;
    memcpy(wrapped + out_off, pass_defines[pass_idx], def_len);
    out_off += def_len;
    memcpy(wrapped + out_off, SHADERTOY_PROLOGUE, pro_len);
    out_off += pro_len;

    int cur_sec = -1; /* -1 = COMMON */
    const char *p = user_src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p + 1) : strlen(p);
        size_t content_len = nl ? (size_t)(nl - p) : line_len;

        int marker = match_section_marker(p, content_len);
        if (marker >= -1) {
            cur_sec = marker;
        } else {
            bool include_line = true;
            if (use_sections) {
                if (cur_sec == -1 || cur_sec == pass_idx) {
                    include_line = true;
                } else if (pass_idx == 4 && !sec_present[4] && cur_sec == -1) {
                    include_line = true;
                } else {
                    include_line = false;
                }
            }

            if (include_line) {
                const char *trimmed = p;
                while (trimmed < p + content_len && (*trimmed == ' ' || *trimmed == '\t')) {
                    trimmed++;
                }
                size_t rem = (size_t)(p + content_len - trimmed);
                if ((rem >= 8 && strncmp(trimmed, "#version", 8) == 0) ||
                    (rem >= 14 && strncmp(trimmed, "#pragma waywal", 14) == 0)) {
                    wrapped[out_off++] = '/';
                    wrapped[out_off++] = '/';
                    memcpy(wrapped + out_off, trimmed + 1, line_len - (size_t)(trimmed + 1 - p));
                    out_off += line_len - (size_t)(trimmed + 1 - p);
                } else {
                    memcpy(wrapped + out_off, p, line_len);
                    out_off += line_len;
                }
            }
        }

        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    memcpy(wrapped + out_off, SHADERTOY_EPILOGUE, epi_len);
    out_off += epi_len;
    wrapped[out_off] = '\0';
    return wrapped;
}

char *waywal_shader_wrap_source(const char *user_src)
{
    return waywal_shader_wrap_pass_source(user_src, 4);
}

static void parse_channel_target_value(const char *val_str, size_t val_len,
                                       shader_channel_binding_t *binding)
{
    while (val_len > 0 &&
           (val_str[0] == ' ' || val_str[0] == '\t' || val_str[0] == '=' || val_str[0] == ':')) {
        val_str++;
        val_len--;
    }
    while (val_len > 0 && (val_str[val_len - 1] == ' ' || val_str[val_len - 1] == '\t' ||
                           val_str[val_len - 1] == '\r' || val_str[val_len - 1] == ';')) {
        val_len--;
    }
    if (val_len == 0) {
        return;
    }

    if (val_len == 5 && strncasecmp(val_str, "audio", 5) == 0) {
        binding->target = SHADER_CHAN_AUDIO;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferA", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_a", 8) == 0)) {
        binding->target = SHADER_CHAN_BUFFER_A;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferB", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_b", 8) == 0)) {
        binding->target = SHADER_CHAN_BUFFER_B;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferC", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_c", 8) == 0)) {
        binding->target = SHADER_CHAN_BUFFER_C;
    } else if ((val_len == 7 && strncasecmp(val_str, "bufferD", 7) == 0) ||
               (val_len == 8 && strncasecmp(val_str, "buffer_d", 8) == 0)) {
        binding->target = SHADER_CHAN_BUFFER_D;
    } else if (val_len == 5 && strncasecmp(val_str, "noise", 5) == 0) {
        binding->target = SHADER_CHAN_NOISE;
    } else if (val_len == 9 && strncasecmp(val_str, "wallpaper", 9) == 0) {
        binding->target = SHADER_CHAN_WALLPAPER;
    } else if (val_len == 5 && strncasecmp(val_str, "depth", 5) == 0) {
        binding->target = SHADER_CHAN_DEPTH;
    } else if (val_len > 6 && strncasecmp(val_str, "image(", 6) == 0) {
        const char *q1 = memchr(val_str + 6, '"', val_len - 6);
        if (q1) {
            const char *q2 = memchr(q1 + 1, '"', (size_t)(val_str + val_len - (q1 + 1)));
            if (q2 && (size_t)(q2 - (q1 + 1)) < sizeof(binding->image_path)) {
                size_t plen = (size_t)(q2 - (q1 + 1));
                memcpy(binding->image_path, q1 + 1, plen);
                binding->image_path[plen] = '\0';
                binding->target = SHADER_CHAN_IMAGE;
            }
        }
    } else if (val_len > 2 && val_str[0] == '"' && val_str[val_len - 1] == '"') {
        size_t plen = val_len - 2;
        if (plen < sizeof(binding->image_path)) {
            memcpy(binding->image_path, val_str + 1, plen);
            binding->image_path[plen] = '\0';
            binding->target = SHADER_CHAN_IMAGE;
        }
    }
}

static void apply_parsed_channel_binding(shader_pass_t passes[SHADER_NUM_PASSES], int target_pass,
                                         int ch, const shader_channel_binding_t *parsed)
{
    if (target_pass >= 0 && target_pass < SHADER_NUM_PASSES) {
        passes[target_pass].channels[ch].target = parsed->target;
        snprintf(passes[target_pass].channels[ch].image_path,
                 sizeof(passes[target_pass].channels[ch].image_path), "%s", parsed->image_path);
    } else {
        for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
            passes[p].channels[ch].target = parsed->target;
            snprintf(passes[p].channels[ch].image_path, sizeof(passes[p].channels[ch].image_path),
                     "%s", parsed->image_path);
        }
    }
}

static void parse_shader_channel_directives(const char *user_src,
                                            shader_pass_t passes[SHADER_NUM_PASSES])
{
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        for (int c = 0; c < 4; ++c) {
            passes[p].channels[c].target = SHADER_CHAN_AUDIO;
            passes[p].channels[c].image_path[0] = '\0';
        }
    }
    if (!user_src) {
        return;
    }

    int cur_sec = -1;
    const char *line = user_src;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        int sec = match_section_marker(line, len);
        if (sec >= -1) {
            cur_sec = sec;
        } else {
            const char *s = line;
            size_t slen = len;
            while (slen > 0 && (*s == ' ' || *s == '\t')) {
                s++;
                slen--;
            }

            bool was_comment = false;
            if (slen >= 2 && s[0] == '/' && s[1] == '/') {
                was_comment = true;
                s += 2;
                slen -= 2;
                while (slen > 0 && (*s == ' ' || *s == '\t')) {
                    s++;
                    slen--;
                }
            }

            if (slen > 14 && strncmp(s, "#pragma waywal", 14) == 0) {
                s += 14;
                slen -= 14;
                while (slen > 0 && (*s == ' ' || *s == '\t')) {
                    s++;
                    slen--;
                }
                int target_pass = cur_sec;
                if (slen > 8 && strncasecmp(s, "bufferA.", 8) == 0) {
                    target_pass = 0;
                    s += 8;
                    slen -= 8;
                } else if (slen > 8 && strncasecmp(s, "bufferB.", 8) == 0) {
                    target_pass = 1;
                    s += 8;
                    slen -= 8;
                } else if (slen > 8 && strncasecmp(s, "bufferC.", 8) == 0) {
                    target_pass = 2;
                    s += 8;
                    slen -= 8;
                } else if (slen > 8 && strncasecmp(s, "bufferD.", 8) == 0) {
                    target_pass = 3;
                    s += 8;
                    slen -= 8;
                } else if (slen > 6 && strncasecmp(s, "image.", 6) == 0) {
                    target_pass = 4;
                    s += 6;
                    slen -= 6;
                }

                int ch = -1;
                if (slen >= 9 && strncasecmp(s, "iChannel", 8) == 0 && s[8] >= '0' && s[8] <= '3') {
                    ch = s[8] - '0';
                    s += 9;
                    slen -= 9;
                } else if (slen >= 8 && strncasecmp(s, "channel", 7) == 0 && s[7] >= '0' &&
                           s[7] <= '3') {
                    ch = s[7] - '0';
                    s += 8;
                    slen -= 8;
                }
                if (ch >= 0) {
                    shader_channel_binding_t parsed = {.target = SHADER_CHAN_AUDIO};
                    parse_channel_target_value(s, slen, &parsed);
                    apply_parsed_channel_binding(passes, target_pass, ch, &parsed);
                }
            } else if (was_comment) {
                int ch = -1;
                if (slen > 10 && strncasecmp(s, "[CHANNEL", 8) == 0 && s[8] >= '0' && s[8] <= '3' &&
                    s[9] == ']') {
                    ch = s[8] - '0';
                    s += 10;
                    slen -= 10;
                } else if (slen > 9 && strncasecmp(s, "iChannel", 8) == 0 && s[8] >= '0' &&
                           s[8] <= '3' &&
                           (s[9] == ':' || s[9] == '=' || s[9] == ' ' || s[9] == '\t')) {
                    ch = s[8] - '0';
                    s += 9;
                    slen -= 9;
                } else if (slen > 8 && strncasecmp(s, "channel", 7) == 0 && s[7] >= '0' &&
                           s[7] <= '3' &&
                           (s[8] == ':' || s[8] == '=' || s[8] == ' ' || s[8] == '\t')) {
                    ch = s[7] - '0';
                    s += 8;
                    slen -= 8;
                }
                if (ch >= 0) {
                    shader_channel_binding_t parsed = {.target = SHADER_CHAN_AUDIO};
                    parse_channel_target_value(s, slen, &parsed);
                    apply_parsed_channel_binding(passes, cur_sec, ch, &parsed);
                }
            }
        }

        if (!nl) {
            break;
        }
        line = nl + 1;
    }
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

    while (glGetError() != GL_NO_ERROR) {
    }
    glProgramBinary(prog, (GLenum)hdr.binary_format, bin, (GLsizei)hdr.binary_len);
    free(bin);

    GLenum err = glGetError();
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (err != GL_NO_ERROR || !linked) {
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
    u->iSampleRate = glGetUniformLocation(prog, "iSampleRate");
    u->iChannel[0] = glGetUniformLocation(prog, "iChannel0");
    u->iChannel[1] = glGetUniformLocation(prog, "iChannel1");
    u->iChannel[2] = glGetUniformLocation(prog, "iChannel2");
    u->iChannel[3] = glGetUniformLocation(prog, "iChannel3");
    u->iChannelResolution = glGetUniformLocation(prog, "iChannelResolution");
    if (u->iChannelResolution < 0) {
        u->iChannelResolution = glGetUniformLocation(prog, "iChannelResolution[0]");
    }
    u->iChannelTime = glGetUniformLocation(prog, "iChannelTime");
    if (u->iChannelTime < 0) {
        u->iChannelTime = glGetUniformLocation(prog, "iChannelTime[0]");
    }
    u->u_audio_bands = glGetUniformLocation(prog, "u_audio_bands");
    if (u->u_audio_bands < 0) {
        u->u_audio_bands = glGetUniformLocation(prog, "u_audio_bands[0]");
    }
    u->u_audio_envelope = glGetUniformLocation(prog, "u_audio_envelope");
    u->u_audio_beat = glGetUniformLocation(prog, "u_audio_beat");
    u->u_palette_primary = glGetUniformLocation(prog, "u_palette_primary");
    u->u_palette_secondary = glGetUniformLocation(prog, "u_palette_secondary");
    u->u_palette_tertiary = glGetUniformLocation(prog, "u_palette_tertiary");
    u->u_mouse_smooth = glGetUniformLocation(prog, "u_mouse_smooth");
    u->u_mouse_vel = glGetUniformLocation(prog, "u_mouse_vel");
    u->u_workspace_offset = glGetUniformLocation(prog, "u_workspace_offset");
    u->u_depth_strength = glGetUniformLocation(prog, "u_depth_strength");
    u->u_swap_rb = glGetUniformLocation(prog, "u_swap_rb");
    u->u_flip_y = glGetUniformLocation(prog, "u_flip_y");
}

static void make_context_current(waywal_shader_engine_t *eng)
{
    if (eng && eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != eng->context) {
            eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, eng->context);
        }
    }
}

static GLuint create_rgba_texture(const uint8_t *rgba, uint32_t w, uint32_t h, GLint wrap_mode)
{
    if (!rgba || w == 0 || h == 0) {
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
    return tex;
}

static void init_builtin_noise_texture(waywal_shader_engine_t *eng)
{
    uint8_t *noise = (uint8_t *)malloc(SHADER_NOISE_DIM * SHADER_NOISE_DIM * 4u);
    if (!noise) {
        return;
    }
    uint32_t state = 0xA341316Cu;
    for (size_t i = 0; i < SHADER_NOISE_DIM * SHADER_NOISE_DIM * 4u; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        noise[i] = (uint8_t)(state & 0xFFu);
    }
    /* Shadertoy standard RG offset trick: green channel is offset (37, 17) of red channel */
    for (uint32_t y = 0; y < SHADER_NOISE_DIM; ++y) {
        for (uint32_t x = 0; x < SHADER_NOISE_DIM; ++x) {
            uint32_t ox = (x + 37u) & (SHADER_NOISE_DIM - 1u);
            uint32_t oy = (y + 17u) & (SHADER_NOISE_DIM - 1u);
            noise[(y * SHADER_NOISE_DIM + x) * 4u + 1u] =
                noise[(oy * SHADER_NOISE_DIM + ox) * 4u + 0u];
            noise[(y * SHADER_NOISE_DIM + x) * 4u + 3u] =
                noise[(oy * SHADER_NOISE_DIM + ox) * 4u + 2u];
        }
    }
    eng->noise_tex = create_rgba_texture(noise, SHADER_NOISE_DIM, SHADER_NOISE_DIM, GL_REPEAT);
    free(noise);
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
    eng->depth_strength = 1.0f;
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
    eng->eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    eng->eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    eng->eglClientWaitSyncKHR =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

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

    init_builtin_noise_texture(eng);
    glGenFramebuffers(1, &eng->read_fbo);
    glGenFramebuffers(1, &eng->draw_fbo);

    eng->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    return eng;
}

static void destroy_pass_resources(shader_pass_t *pass)
{
    if (pass->prog) {
        glDeleteProgram(pass->prog);
        pass->prog = 0;
    }
    for (int i = 0; i < 2; ++i) {
        if (pass->ping_pong_tex[i]) {
            glDeleteTextures(1, &pass->ping_pong_tex[i]);
            pass->ping_pong_tex[i] = 0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        if (pass->channels[c].custom_tex) {
            glDeleteTextures(1, &pass->channels[c].custom_tex);
            pass->channels[c].custom_tex = 0;
        }
    }
    pass->tex_w = 0;
    pass->tex_h = 0;
    pass->read_idx = 0;
    pass->uniforms_initialized = false;
    pass->active = false;
}

void waywal_shader_engine_forget_bo(waywal_shader_engine_t *eng, const dmabuf_bo_t *bo)
{
    if (!eng || !bo) {
        return;
    }
    size_t dst = 0;
    for (size_t i = 0; i < eng->num_imported_bos; ++i) {
        if (eng->imported_bos[i].bo != bo) {
            if (dst != i) {
                eng->imported_bos[dst] = eng->imported_bos[i];
            }
            dst++;
        }
    }
    eng->num_imported_bos = dst;
}

void waywal_shader_engine_destroy(waywal_shader_engine_t *eng)
{
    if (!eng) {
        return;
    }
    make_context_current(eng);
    for (int i = 0; i < SHADER_NUM_PASSES; ++i) {
        destroy_pass_resources(&eng->passes[i]);
    }
    if (eng->audio_tex) {
        glDeleteTextures(1, &eng->audio_tex);
    }
    if (eng->noise_tex) {
        glDeleteTextures(1, &eng->noise_tex);
    }
    if (eng->wallpaper_tex) {
        glDeleteTextures(1, &eng->wallpaper_tex);
    }
    if (eng->depth_tex) {
        glDeleteTextures(1, &eng->depth_tex);
    }
    if (eng->scratch_tex) {
        glDeleteTextures(1, &eng->scratch_tex);
    }
    if (eng->owns_egl) {
        for (size_t i = 0; i < eng->num_imported_bos; ++i) {
            if (eng->imported_bos[i].gl_tex) {
                glDeleteTextures(1, &eng->imported_bos[i].gl_tex);
            }
            if (eng->imported_bos[i].egl_image && eng->eglDestroyImageKHR &&
                eng->display != EGL_NO_DISPLAY) {
                eng->eglDestroyImageKHR(eng->display, eng->imported_bos[i].egl_image);
            }
            if (eng->imported_bos[i].bo) {
                if (eng->imported_bos[i].bo->gl_tex == eng->imported_bos[i].gl_tex) {
                    eng->imported_bos[i].bo->gl_tex = 0;
                }
                if (eng->imported_bos[i].bo->egl_image == eng->imported_bos[i].egl_image) {
                    eng->imported_bos[i].bo->egl_image = NULL;
                }
            }
        }
    }
    eng->num_imported_bos = 0;
    if (eng->read_fbo) {
        glDeleteFramebuffers(1, &eng->read_fbo);
    }
    if (eng->draw_fbo) {
        glDeleteFramebuffers(1, &eng->draw_fbo);
    }
    if (eng->prog_blit) {
        glDeleteProgram(eng->prog_blit);
    }
    if (eng->blit_vao) {
        glDeleteVertexArrays(1, &eng->blit_vao);
    }
    if (eng->owns_egl) {
        eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(eng->display, eng->context);
        eglTerminate(eng->display);
    }
    if (eng->inotify_fd >= 0) {
        close(eng->inotify_fd);
    }
    free(eng);
}

static GLuint compile_or_load_pass(waywal_shader_engine_t *eng, const char *wrapped,
                                   const char *debug_name, bool *out_from_cache)
{
    uint64_t key = waywal_shader_compute_cache_key(wrapped, eng->gpu_id);
    GLuint prog = try_load_cached_program(key);
    if (prog != 0) {
        if (out_from_cache) {
            *out_from_cache = true;
        }
        return prog;
    }
    if (out_from_cache) {
        *out_from_cache = false;
    }

    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!shader) {
        return 0;
    }

    const char *src_ptr = wrapped;
    glShaderSource(shader, 1, &src_ptr, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log_buf[1024];
        glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Shader compilation failed (%s): %s", debug_name ? debug_name : "live",
                       log_buf);
        glDeleteShader(shader);
        return 0;
    }

    prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(shader);
        return 0;
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
        return 0;
    }

    save_program_to_cache(prog, key);
    return prog;
}

static void load_custom_channel_images(waywal_shader_engine_t *eng,
                                       shader_pass_t passes[SHADER_NUM_PASSES],
                                       const char *base_dir)
{
    (void)eng;
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        if (!passes[p].active) {
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            shader_channel_binding_t *b = &passes[p].channels[c];
            if (b->target != SHADER_CHAN_IMAGE || b->image_path[0] == '\0') {
                continue;
            }
            char resolved[4096];
            if (b->image_path[0] != '/' && base_dir && base_dir[0] != '\0') {
                snprintf(resolved, sizeof(resolved), "%s/%s", base_dir, b->image_path);
            } else {
                snprintf(resolved, sizeof(resolved), "%s", b->image_path);
            }
            uint8_t *px = NULL;
            uint32_t w = 0, h = 0;
            if (image_load(resolved, &px, &w, &h) && px) {
                size_t npx = (size_t)w * (size_t)h;
                for (size_t i = 0; i < npx; ++i) {
                    uint8_t tmp = px[i * 4 + 0];
                    px[i * 4 + 0] = px[i * 4 + 2];
                    px[i * 4 + 2] = tmp;
                }
                b->custom_tex = create_rgba_texture(px, w, h, GL_REPEAT);
                b->custom_w = w;
                b->custom_h = h;
                image_free(px);
            } else {
                WAYWAL_LOG_WARN("Shader channel %d failed to load image '%s'", c, resolved);
            }
        }
    }
}

bool waywal_shader_engine_load_source(waywal_shader_engine_t *eng, const char *user_src,
                                      const char *debug_name)
{
    if (!eng || !user_src) {
        return false;
    }

    make_context_current(eng);

    bool active_passes[SHADER_NUM_PASSES];
    detect_active_passes(user_src, active_passes);

    shader_pass_t new_passes[SHADER_NUM_PASSES];
    memset(new_passes, 0, sizeof(new_passes));
    parse_shader_channel_directives(user_src, new_passes);

    bool all_cached = true;
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        if (!active_passes[p]) {
            continue;
        }
        char *wrapped = waywal_shader_wrap_pass_source(user_src, p);
        if (!wrapped) {
            for (int j = 0; j < p; ++j) {
                destroy_pass_resources(&new_passes[j]);
            }
            return false;
        }
        bool from_cache = false;
        GLuint prog = compile_or_load_pass(eng, wrapped, debug_name, &from_cache);
        free(wrapped);
        if (!prog) {
            for (int j = 0; j < p; ++j) {
                destroy_pass_resources(&new_passes[j]);
            }
            return false;
        }
        if (!from_cache) {
            all_cached = false;
        }
        new_passes[p].active = true;
        new_passes[p].prog = prog;
        cache_uniform_locations(prog, &new_passes[p].uniforms);
    }

    char dir_fallback[4096] = {0};
    const char *base_dir = eng->watched_dir;
    if ((!base_dir || base_dir[0] == '\0') && debug_name) {
        const char *slash = strrchr(debug_name, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - debug_name);
            if (dlen == 0) {
                snprintf(dir_fallback, sizeof(dir_fallback), "/");
            } else if (dlen < sizeof(dir_fallback)) {
                memcpy(dir_fallback, debug_name, dlen);
                dir_fallback[dlen] = '\0';
            }
            base_dir = dir_fallback;
        }
    }
    load_custom_channel_images(eng, new_passes, base_dir);

    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        destroy_pass_resources(&eng->passes[p]);
        eng->passes[p] = new_passes[p];
    }
    eng->last_from_cache = all_cached;
    return true;
}

void waywal_shader_engine_set_wallpaper(waywal_shader_engine_t *eng, const uint8_t *rgba,
                                        uint32_t w, uint32_t h)
{
    if (!eng || !rgba || w == 0 || h == 0) {
        return;
    }
    make_context_current(eng);
    if (eng->wallpaper_tex) {
        glDeleteTextures(1, &eng->wallpaper_tex);
        eng->wallpaper_tex = 0;
    }
    eng->wallpaper_tex = create_rgba_texture(rgba, w, h, GL_CLAMP_TO_EDGE);
    eng->wallpaper_w = w;
    eng->wallpaper_h = h;
}

void waywal_shader_engine_set_depth_strength(waywal_shader_engine_t *eng, float strength)
{
    if (!eng) {
        return;
    }
    eng->depth_strength = (strength > 0.0f) ? strength : 1.0f;
}

int waywal_shader_engine_get_channel_target(const waywal_shader_engine_t *eng, int pass_idx,
                                            int chan_idx)
{
    if (!eng || pass_idx < 0 || pass_idx >= SHADER_NUM_PASSES || chan_idx < 0 || chan_idx >= 4) {
        return -1;
    }
    return (int)eng->passes[pass_idx].channels[chan_idx].target;
}

bool waywal_shader_engine_load_depth_parallax(waywal_shader_engine_t *eng, const uint8_t *base_rgba,
                                              uint32_t base_w, uint32_t base_h,
                                              const uint8_t *depth_rgba, uint32_t depth_w,
                                              uint32_t depth_h)
{
    if (!eng || !base_rgba || !depth_rgba || base_w == 0 || base_h == 0 || depth_w == 0 ||
        depth_h == 0) {
        return false;
    }
    make_context_current(eng);
    if (eng->inotify_fd >= 0) {
        if (eng->watch_file_wd >= 0) {
            inotify_rm_watch(eng->inotify_fd, eng->watch_file_wd);
            eng->watch_file_wd = -1;
        }
        if (eng->watch_dir_wd >= 0) {
            inotify_rm_watch(eng->inotify_fd, eng->watch_dir_wd);
            eng->watch_dir_wd = -1;
        }
    }
    eng->watched_path[0] = '\0';
    eng->watched_dir[0] = '\0';
    eng->watched_base[0] = '\0';

    waywal_shader_engine_set_wallpaper(eng, base_rgba, base_w, base_h);

    if (eng->depth_tex) {
        glDeleteTextures(1, &eng->depth_tex);
        eng->depth_tex = 0;
    }
    eng->depth_tex = create_rgba_texture(depth_rgba, depth_w, depth_h, GL_CLAMP_TO_EDGE);
    eng->depth_w = depth_w;
    eng->depth_h = depth_h;

    if (!waywal_shader_engine_load_source(eng, DEPTH_PARALLAX_POM_SHADER, "builtin_depth_pom")) {
        return false;
    }
    eng->passes[4].channels[0].target = SHADER_CHAN_WALLPAPER;
    eng->passes[4].channels[1].target = SHADER_CHAN_DEPTH;
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
    if (!filepath) {
        return;
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

    if (eng->inotify_fd < 0) {
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
    setup_file_watches(eng, filepath);
    bool ok = waywal_shader_engine_load_source(eng, src, filepath);
    free(src);
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
    if (eng->owns_egl && eng->num_imported_bos < 32u) {
        eng->imported_bos[eng->num_imported_bos].bo = bo;
        eng->imported_bos[eng->num_imported_bos].gl_tex = tex;
        eng->imported_bos[eng->num_imported_bos].egl_image = img;
        eng->num_imported_bos++;
    }
    return true;
}

static void ensure_pass_ping_pong_textures(waywal_shader_engine_t *eng, shader_pass_t *pass,
                                           uint32_t w, uint32_t h, uint16_t **shared_zero_half)
{
    (void)eng;
    if (pass->ping_pong_tex[0] && pass->ping_pong_tex[1] && pass->tex_w == w && pass->tex_h == h) {
        return;
    }
    if (shared_zero_half && !*shared_zero_half) {
        *shared_zero_half = (uint16_t *)calloc((size_t)w * (size_t)h * 4u, sizeof(uint16_t));
    }
    const uint16_t *zero_half = shared_zero_half ? *shared_zero_half : NULL;
    for (int i = 0; i < 2; ++i) {
        if (pass->ping_pong_tex[i]) {
            glDeleteTextures(1, &pass->ping_pong_tex[i]);
        }
        glGenTextures(1, &pass->ping_pong_tex[i]);
        glBindTexture(GL_TEXTURE_2D, pass->ping_pong_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, (GLsizei)w, (GLsizei)h);
        if (zero_half) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_HALF_FLOAT,
                            zero_half);
        }
    }
    pass->tex_w = w;
    pass->tex_h = h;
    pass->read_idx = 0;
}

static void ensure_scratch_texture(waywal_shader_engine_t *eng, uint32_t w, uint32_t h)
{
    if (eng->scratch_tex && eng->scratch_w == w && eng->scratch_h == h) {
        return;
    }
    if (eng->scratch_tex) {
        glDeleteTextures(1, &eng->scratch_tex);
    }
    glGenTextures(1, &eng->scratch_tex);
    glBindTexture(GL_TEXTURE_2D, eng->scratch_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    eng->scratch_w = w;
    eng->scratch_h = h;
}

static void resolve_channel_texture(const waywal_shader_engine_t *eng,
                                    const shader_channel_binding_t *b, uint32_t target_w,
                                    uint32_t target_h, GLuint *out_tex, float *out_res)
{
    switch (b->target) {
    case SHADER_CHAN_BUFFER_A:
    case SHADER_CHAN_BUFFER_B:
    case SHADER_CHAN_BUFFER_C:
    case SHADER_CHAN_BUFFER_D: {
        int idx = (int)b->target - (int)SHADER_CHAN_BUFFER_A;
        const shader_pass_t *bp = &eng->passes[idx];
        if (bp->active && bp->ping_pong_tex[bp->read_idx]) {
            *out_tex = bp->ping_pong_tex[bp->read_idx];
            out_res[0] = (float)bp->tex_w;
            out_res[1] = (float)bp->tex_h;
            out_res[2] = 1.0f;
            return;
        }
        break;
    }
    case SHADER_CHAN_NOISE:
        if (eng->noise_tex) {
            *out_tex = eng->noise_tex;
            out_res[0] = (float)SHADER_NOISE_DIM;
            out_res[1] = (float)SHADER_NOISE_DIM;
            out_res[2] = 1.0f;
            return;
        }
        break;
    case SHADER_CHAN_WALLPAPER:
        if (eng->wallpaper_tex) {
            *out_tex = eng->wallpaper_tex;
            out_res[0] = (float)eng->wallpaper_w;
            out_res[1] = (float)eng->wallpaper_h;
            out_res[2] = 1.0f;
            return;
        }
        break;
    case SHADER_CHAN_DEPTH:
        if (eng->depth_tex) {
            *out_tex = eng->depth_tex;
            out_res[0] = (float)eng->depth_w;
            out_res[1] = (float)eng->depth_h;
            out_res[2] = 1.0f;
            return;
        }
        break;
    case SHADER_CHAN_IMAGE:
        if (b->custom_tex) {
            *out_tex = b->custom_tex;
            out_res[0] = (float)b->custom_w;
            out_res[1] = (float)b->custom_h;
            out_res[2] = 1.0f;
            return;
        }
        break;
    case SHADER_CHAN_AUDIO:
    default:
        break;
    }

    (void)target_w;
    (void)target_h;
    *out_tex = eng->audio_tex;
    out_res[0] = (float)WAYWAL_AUDIO_TEX_WIDTH;
    out_res[1] = (float)WAYWAL_AUDIO_TEX_HEIGHT;
    out_res[2] = 1.0f;
}

bool waywal_shader_engine_render_frame(waywal_shader_engine_t *eng, dmabuf_bo_t *target_bo,
                                       const waywal_shader_frame_params_t *params)
{
    if (!eng || !target_bo || !params || !eng->passes[4].active || !eng->passes[4].prog) {
        return false;
    }

    make_context_current(eng);
    if (!ensure_bo_imported(eng, target_bo)) {
        return false;
    }

    if (params->audio) {
        glBindTexture(GL_TEXTURE_2D, eng->audio_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WAYWAL_AUDIO_TEX_WIDTH, WAYWAL_AUDIO_TEX_HEIGHT,
                        GL_RED, GL_UNSIGNED_BYTE, params->audio->shadertoy_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    float date_vals[4] = {params->date[0], params->date[1], params->date[2], params->date[3]};
    if (date_vals[0] == 0.0f) {
        struct timespec ts_real;
        clock_gettime(CLOCK_REALTIME, &ts_real);
        struct tm tm_local;
        localtime_r(&ts_real.tv_sec, &tm_local);
        date_vals[0] = (float)(tm_local.tm_year + 1900);
        date_vals[1] = (float)tm_local.tm_mon;
        date_vals[2] = (float)tm_local.tm_mday;
        date_vals[3] = (float)(tm_local.tm_hour * 3600 + tm_local.tm_min * 60 + tm_local.tm_sec) +
                       (float)ts_real.tv_nsec * 1e-9f;
    }

    float mouse_smooth_vals[4] = {
        params->mouse_smooth[0],
        params->mouse_smooth[1],
        params->mouse_smooth[2],
        params->mouse_smooth[3],
    };
    if (mouse_smooth_vals[0] == 0.0f && mouse_smooth_vals[1] == 0.0f) {
        mouse_smooth_vals[0] = params->mouse[0];
        mouse_smooth_vals[1] = params->mouse[1];
        mouse_smooth_vals[2] = params->mouse[2];
        mouse_smooth_vals[3] = params->mouse[3];
    }

    bool is_10bit_fmt = target_bo->is_10bit || target_bo->drm_format == DRM_FORMAT_XRGB2101010 ||
                        target_bo->drm_format == DRM_FORMAT_ARGB2101010 ||
                        target_bo->drm_format == DRM_FORMAT_XBGR2101010 ||
                        target_bo->drm_format == DRM_FORMAT_ABGR2101010;
    bool is_non_linear = (target_bo->modifier != DRM_FORMAT_MOD_LINEAR &&
                          target_bo->modifier != DRM_FORMAT_MOD_INVALID);
    bool use_scratch_blit = is_10bit_fmt || is_non_linear;

    if (use_scratch_blit) {
        ensure_scratch_texture(eng, target_bo->width, target_bo->height);
    }

    uint16_t *shared_zero_half = NULL;
    for (int p = 0; p < 4; ++p) {
        if (eng->passes[p].active) {
            ensure_pass_ping_pong_textures(eng, &eng->passes[p], target_bo->width,
                                           target_bo->height, &shared_zero_half);
        }
    }
    free(shared_zero_half);

    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);

    GLuint groups_x = (target_bo->width + 15u) / 16u;
    GLuint groups_y = (target_bo->height + 15u) / 16u;

    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        shader_pass_t *pass = &eng->passes[p];
        if (!pass->active || !pass->prog) {
            continue;
        }

        glUseProgram(pass->prog);
        const shader_uniforms_t *u = &pass->uniforms;
        bool init_u = !pass->uniforms_initialized;

        if (u->iResolution >= 0 && (init_u || pass->cached_res_w != target_bo->width ||
                                    pass->cached_res_h != target_bo->height)) {
            glUniform3f(u->iResolution, (float)target_bo->width, (float)target_bo->height, 1.0f);
            pass->cached_res_w = target_bo->width;
            pass->cached_res_h = target_bo->height;
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
            glUniform4fv(u->iDate, 1, date_vals);
        }
        if (u->iSampleRate >= 0 && init_u) {
            glUniform1f(u->iSampleRate, 48000.0f);
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
        if (u->u_mouse_smooth >= 0) {
            glUniform4fv(u->u_mouse_smooth, 1, mouse_smooth_vals);
        }
        if (u->u_mouse_vel >= 0) {
            glUniform2fv(u->u_mouse_vel, 1, params->mouse_vel);
        }
        if (u->u_workspace_offset >= 0 &&
            (init_u || pass->cached_workspace_offset[0] != params->workspace_offset[0] ||
             pass->cached_workspace_offset[1] != params->workspace_offset[1])) {
            glUniform2fv(u->u_workspace_offset, 1, params->workspace_offset);
            pass->cached_workspace_offset[0] = params->workspace_offset[0];
            pass->cached_workspace_offset[1] = params->workspace_offset[1];
        }
        float d_strength = (eng->depth_strength > 0.0f) ? eng->depth_strength : 1.0f;
        if (u->u_depth_strength >= 0 && (init_u || pass->cached_depth_strength != d_strength)) {
            glUniform1f(u->u_depth_strength, d_strength);
            pass->cached_depth_strength = d_strength;
        }
        if (params->audio) {
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

        float chan_res[12];
        float chan_time[4] = {
            params->time_sec,
            params->time_sec,
            params->time_sec,
            params->time_sec,
        };
        for (int c = 0; c < 4; ++c) {
            GLuint ctex = 0;
            resolve_channel_texture(eng, &pass->channels[c], target_bo->width, target_bo->height,
                                    &ctex, &chan_res[c * 3]);
            glActiveTexture((GLenum)(GL_TEXTURE0 + c));
            glBindTexture(GL_TEXTURE_2D, ctex);
            if (u->iChannel[c] >= 0 && init_u) {
                glUniform1i(u->iChannel[c], c);
            }
        }
        if (u->iChannelResolution >= 0) {
            glUniform3fv(u->iChannelResolution, 4, chan_res);
        }
        if (u->iChannelTime >= 0) {
            glUniform1fv(u->iChannelTime, 4, chan_time);
        }

        if (p < 4) {
            int write_idx = 1 - pass->read_idx;
            if (u->u_swap_rb >= 0 && (init_u || pass->cached_swap_rb != 0)) {
                glUniform1i(u->u_swap_rb, 0);
                pass->cached_swap_rb = 0;
            }
            if (u->u_flip_y >= 0 && (init_u || pass->cached_flip_y != 0)) {
                glUniform1i(u->u_flip_y, 0);
                pass->cached_flip_y = 0;
            }
            pass->uniforms_initialized = true;
            glBindImageTexture(0, pass->ping_pong_tex[write_idx], 0, GL_FALSE, 0, GL_WRITE_ONLY,
                               GL_RGBA16F);
            glDispatchCompute(groups_x, groups_y, 1u);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            pass->read_idx = write_idx;
        } else {
            GLuint dst_tex = use_scratch_blit ? eng->scratch_tex : target_bo->gl_tex;
            int swap_rb = use_scratch_blit ? 0 : 1;
            if (u->u_swap_rb >= 0 && (init_u || pass->cached_swap_rb != swap_rb)) {
                glUniform1i(u->u_swap_rb, swap_rb);
                pass->cached_swap_rb = swap_rb;
            }
            if (u->u_flip_y >= 0 && (init_u || pass->cached_flip_y != 1)) {
                glUniform1i(u->u_flip_y, 1);
                pass->cached_flip_y = 1;
            }
            pass->uniforms_initialized = true;
            glBindImageTexture(0, dst_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            glDispatchCompute(groups_x, groups_y, 1u);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT |
                            GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        }
    }

    if (use_scratch_blit) {
        if (!eng->prog_blit) {
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
                                        "out highp vec4 frag_color;\n"
                                        "void main() {\n"
                                        "    frag_color = texture(u_tex, v_uv);\n"
                                        "}\n";
            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            if (vs && fs) {
                glShaderSource(vs, 1, &vs_src, NULL);
                glCompileShader(vs);
                glShaderSource(fs, 1, &fs_src, NULL);
                glCompileShader(fs);
                GLuint p = glCreateProgram();
                if (p) {
                    glAttachShader(p, vs);
                    glAttachShader(p, fs);
                    glLinkProgram(p);
                    GLint ok = 0;
                    glGetProgramiv(p, GL_LINK_STATUS, &ok);
                    if (ok) {
                        eng->prog_blit = p;
                    } else {
                        glDeleteProgram(p);
                    }
                }
            }
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
        }
        if (!eng->blit_vao) {
            glGenVertexArrays(1, &eng->blit_vao);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               target_bo->gl_tex, 0);
        if (eng->prog_blit && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
            glUseProgram(eng->prog_blit);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, eng->scratch_tex);
            glBindVertexArray(eng->blit_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, eng->read_fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   eng->scratch_tex, 0);
            glBlitFramebuffer(0, 0, (GLint)target_bo->width, (GLint)target_bo->height, 0, 0,
                              (GLint)target_bo->width, (GLint)target_bo->height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        }
    }

    glUseProgram((GLuint)prev_prog);
    if (eng->eglCreateSyncKHR && eng->eglClientWaitSyncKHR && eng->eglDestroySyncKHR) {
        EGLSyncKHR sync = eng->eglCreateSyncKHR(eng->display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            eng->eglClientWaitSyncKHR(eng->display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                      EGL_FOREVER_KHR);
            eng->eglDestroySyncKHR(eng->display, sync);
        } else {
            glFlush();
            glFinish();
        }
    } else {
        glFlush();
        glFinish();
    }
    return true;
}

bool waywal_shader_engine_has_active_program(const waywal_shader_engine_t *eng)
{
    return eng && eng->passes[4].active && eng->passes[4].prog != 0;
}

bool waywal_shader_engine_last_load_from_cache(const waywal_shader_engine_t *eng)
{
    return eng && eng->last_from_cache;
}

uint32_t waywal_shader_engine_active_pass_mask(const waywal_shader_engine_t *eng)
{
    if (!eng) {
        return 0;
    }
    uint32_t mask = 0;
    for (int i = 0; i < SHADER_NUM_PASSES; ++i) {
        if (eng->passes[i].active && eng->passes[i].prog != 0) {
            mask |= (1u << i);
        }
    }
    return mask;
}
