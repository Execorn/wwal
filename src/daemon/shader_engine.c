// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/shader_engine.h"

#include "waywal/dmabuf.h"
#include "waywal/image_loader.h"
#include "waywal/log.h"
#include "waywal/path.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#define SHADER_CACHE_MAGIC         0x57534842u /* "WSHB" */
#define SHADER_NUM_PASSES          5           /* 0..3 = BUFFER_A..D, 4 = IMAGE */
#define SHADER_NOISE_DIM           256u
#define SHADER_MAX_CUSTOM_UNIFORMS 64

typedef struct {
    bool active;
    char name[64];
    uint32_t num_components;
    float values[4];
    GLint locations[SHADER_NUM_PASSES];
    GLenum types[SHADER_NUM_PASSES];
} shader_custom_uniform_t;

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
    SHADER_CHAN_ALBUM_ART = 9,
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
    GLint u_span_rect;
    GLint u_local_size;
    GLint u_swap_rb;
    GLint u_flip_y;
    GLint u_album_art;
    GLint u_media_playing;
    GLint u_noise_tex;
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
    bool is_compute;
    GLuint prog;
    shader_uniforms_t uniforms;
    shader_channel_binding_t channels[4];
    GLuint ping_pong_tex[2];
    uint32_t tex_w;
    uint32_t tex_h;
    int read_idx;
    bool uniforms_initialized;
    bool audio_uniforms_dirty;
    uint32_t cached_res_w;
    uint32_t cached_res_h;
    uint32_t cached_local_w;
    uint32_t cached_local_h;
    int cached_swap_rb;
    int cached_flip_y;
    float cached_depth_strength;
    float cached_workspace_offset[2];
    float cached_span_rect[4];
    float cached_mouse[4];
    float cached_mouse_smooth[4];
    float cached_mouse_vel[2];
    float cached_palette_primary[3];
    float cached_palette_secondary[3];
    float cached_palette_tertiary[3];
    float cached_media_playing;
    float cached_chan_res[12];
    bool needs_mipmap;
    float res_scale;
    uint32_t target_fps;
    double last_render_sec;
} shader_pass_t;

struct waywal_shader_engine {
    EGLDisplay display;
    EGLContext context;
    bool owns_egl;

    shader_pass_t passes[SHADER_NUM_PASSES];
    GLuint audio_tex;
    bool audio_tex_dirty;
    GLuint noise_tex;
    GLuint wallpaper_tex;
    uint32_t wallpaper_w;
    uint32_t wallpaper_h;
    GLuint album_art_tex;
    uint32_t album_art_w;
    uint32_t album_art_h;
    float media_playing;
    GLuint depth_tex;
    uint32_t depth_w;
    uint32_t depth_h;
    float depth_strength;

    shader_custom_uniform_t custom_uniforms[SHADER_MAX_CUSTOM_UNIFORMS];
    uint32_t num_custom_uniforms;

    bool uses_lod;
    bool uses_audio_channel;
    bool uses_date;
    uint64_t frame_idx;

    GLuint scratch_tex;
    uint32_t scratch_w;
    uint32_t scratch_h;
    GLuint read_fbo;
    GLuint draw_fbo;
    GLuint prog_blit;
    GLint loc_blit_swap_rb;
    GLuint blit_vao;
    GLuint last_complete_bo_tex;
    const dmabuf_bo_t *last_rendered_bo;
    GLuint last_rendered_tex;
    uint32_t last_rendered_w;
    uint32_t last_rendered_h;

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
        const dmabuf_bo_t *bo;
        int fd0;
        uint32_t width;
        uint32_t height;
        uint32_t drm_format;
        GLuint gl_tex;
        EGLImageKHR egl_image;
        bool owns_egl;
    } imported_bos[32];
    size_t num_imported_bos;
};

static const char SHADERTOY_PROLOGUE[] =
    "precision highp float;\n"
    "precision highp int;\n"
    "out highp vec4 waywal_FragColor;\n"
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
    "uniform vec4 u_span_rect;\n"
    "uniform vec2 u_local_size;\n"
    "uniform int u_swap_rb;\n"
    "uniform int u_flip_y;\n"
    "uniform highp sampler2D u_album_art;\n"
    "uniform float u_media_playing;\n"
    "uniform highp sampler2D u_noise_tex;\n"
    "float waywal_noise2d(highp vec2 x) {\n"
    "    highp vec2 i = floor(x);\n"
    "    highp vec2 f = fract(x);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return textureLod(u_noise_tex, (i + f + 0.5) * (1.0 / 256.0), 0.0).r;\n"
    "}\n"
    "vec2 waywal_noise2d_dual(highp vec2 x) {\n"
    "    highp vec2 i = floor(x);\n"
    "    highp vec2 f = fract(x);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return textureLod(u_noise_tex, (i + f + 0.5) * (1.0 / 256.0), 0.0).rb;\n"
    "}\n"
    "float waywal_noise3d(highp vec3 p) {\n"
    "    highp vec3 i = floor(p);\n"
    "    highp vec3 f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    highp vec2 uv = (i.xy + vec2(37.0, 17.0) * i.z) + f.xy;\n"
    "    highp vec2 rg = textureLod(u_noise_tex, (uv + 0.5) * (1.0 / 256.0), 0.0).rg;\n"
    "    return mix(rg.x, rg.y, f.z);\n"
    "}\n"
    "\n";

static const char COMPUTE_PROLOGUE[] =
    "uniform highp sampler2D u_noise_tex;\n"
    "float waywal_noise2d(highp vec2 x) {\n"
    "    highp vec2 i = floor(x);\n"
    "    highp vec2 f = fract(x);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return textureLod(u_noise_tex, (i + f + 0.5) * (1.0 / 256.0), 0.0).r;\n"
    "}\n"
    "vec2 waywal_noise2d_dual(highp vec2 x) {\n"
    "    highp vec2 i = floor(x);\n"
    "    highp vec2 f = fract(x);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    return textureLod(u_noise_tex, (i + f + 0.5) * (1.0 / 256.0), 0.0).rb;\n"
    "}\n"
    "float waywal_noise3d(highp vec3 p) {\n"
    "    highp vec3 i = floor(p);\n"
    "    highp vec3 f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    highp vec2 uv = (i.xy + vec2(37.0, 17.0) * i.z) + f.xy;\n"
    "    highp vec2 rg = textureLod(u_noise_tex, (uv + 0.5) * (1.0 / 256.0), 0.0).rg;\n"
    "    return mix(rg.x, rg.y, f.z);\n"
    "}\n";

static const char SHADERTOY_EPILOGUE[] =
    "\n"
    "void main() {\n"
    "    float local_y = (u_flip_y != 0) ? (u_local_size.y - gl_FragCoord.y) : gl_FragCoord.y;\n"
    "    vec2 fragCoord = vec2(gl_FragCoord.x, local_y) + u_span_rect.xy;\n"
    "    vec4 fragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    mainImage(fragColor, fragCoord);\n"
    "#ifdef IMAGE_PASS\n"
    "    fragColor = clamp(fragColor, 0.0, 1.0);\n"
    "    if (u_swap_rb != 0) {\n"
    "        waywal_FragColor = vec4(fragColor.bgr, 1.0);\n"
    "    } else {\n"
    "        waywal_FragColor = vec4(fragColor.rgb, 1.0);\n"
    "    }\n"
    "#else\n"
    "    waywal_FragColor = fragColor;\n"
    "#endif\n"
    "}\n";

static const char DEPTH_PARALLAX_POM_SHADER[] =
    "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
    "    vec2 dst_res = max(iResolution.xy, vec2(1.0));\n"
    "    vec2 src_res = (iChannelResolution[0].x > 1.0 && iChannelResolution[0].y > 1.0)\n"
    "                       ? iChannelResolution[0].xy\n"
    "                       : dst_res;\n"
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
    "    vec2 mouse_pos = (u_mouse_smooth.x > 0.0 || u_mouse_smooth.y > 0.0)\n"
    "                         ? u_mouse_smooth.xy\n"
    "                         : (dst_res * 0.5);\n"
    "    vec2 mouse_norm = (mouse_pos / dst_res) - vec2(0.5);\n"
    "    mouse_norm.y = -mouse_norm.y;\n"
    "    float strength = (u_depth_strength > 0.0) ? u_depth_strength : 1.0;\n"
    "    vec2 parallax = (mouse_norm * 0.045 + u_workspace_offset * 0.025) * strength;\n"
    "    uv = (uv - 0.5) * 0.94 + 0.5;\n"
    "    vec2 ddx = dFdx(uv);\n"
    "    vec2 ddy = dFdy(uv);\n"
    "    int num_steps = int(mix(6.0, 20.0, clamp(length(parallax) / 0.04, 0.0, 1.0)));\n"
    "    float step_size = 1.0 / float(num_steps);\n"
    "    vec2 delta_uv = parallax * step_size;\n"
    "    vec2 cur_uv = uv;\n"
    "    float cur_layer = 0.0;\n"
    "    float cur_depth = 1.0 - textureGrad(iChannel1, clamp(cur_uv, 0.0, 1.0), ddx, ddy).r;\n"
    "    float prev_depth = cur_depth;\n"
    "    for (int i = 0; i < 20; ++i) {\n"
    "        if (i >= num_steps || cur_layer >= cur_depth) {\n"
    "            break;\n"
    "        }\n"
    "        prev_depth = cur_depth;\n"
    "        cur_uv -= delta_uv;\n"
    "        cur_depth = 1.0 - textureGrad(iChannel1, clamp(cur_uv, 0.0, 1.0), ddx, ddy).r;\n"
    "        cur_layer += step_size;\n"
    "    }\n"
    "    vec2 prev_uv = cur_uv + delta_uv;\n"
    "    float after_depth = cur_depth - cur_layer;\n"
    "    float before_depth = prev_depth - cur_layer + step_size;\n"
    "    float weight = clamp(after_depth / max(after_depth - before_depth, 1e-4), 0.0, 1.0);\n"
    "    vec2 final_uv = clamp(mix(cur_uv, prev_uv, weight), 0.0, 1.0);\n"
    "    float mid_depth = 1.0 - textureGrad(iChannel1, final_uv, ddx, ddy).r;\n"
    "    float mid_layer = mix(cur_layer, cur_layer - step_size, weight);\n"
    "    if (mid_depth < mid_layer) {\n"
    "        float w2 = clamp((mid_depth - mid_layer) /\n"
    "                         max((mid_depth - mid_layer) - before_depth, 1e-4), 0.0, 1.0);\n"
    "        final_uv = clamp(mix(final_uv, prev_uv, w2), 0.0, 1.0);\n"
    "    } else {\n"
    "        float w2 = clamp(after_depth /\n"
    "                         max(after_depth - (mid_depth - mid_layer), 1e-4), 0.0, 1.0);\n"
    "        final_uv = clamp(mix(cur_uv, final_uv, w2), 0.0, 1.0);\n"
    "    }\n"
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
    if (i + 2 <= len && line[i] == '/' && (line[i + 1] == '/' || line[i + 1] == '*')) {
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

static bool line_references_pass_macro(const char *line, size_t len, const char *macro)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] != '#') {
        return false;
    }
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    size_t code_end = len;
    for (size_t k = i; k + 1 < len; ++k) {
        if (line[k] == '/' && (line[k + 1] == '/' || line[k + 1] == '*')) {
            code_end = k;
            break;
        }
    }
    size_t mlen = strlen(macro);
    for (size_t k = i; k + mlen <= code_end; ++k) {
        if (strncmp(line + k, macro, mlen) == 0) {
            bool prev_ok = (k == i) || (!isalnum((unsigned char)line[k - 1]) && line[k - 1] != '_');
            bool next_ok = (k + mlen == code_end) ||
                           (!isalnum((unsigned char)line[k + mlen]) && line[k + mlen] != '_');
            if (prev_ok && next_ok) {
                return true;
            }
        }
    }
    return false;
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

    static const char *const macros[4] = {"BUFFER_A", "BUFFER_B", "BUFFER_C", "BUFFER_D"};
    bool in_block_comment = false;
    const char *p = user_src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (!in_block_comment) {
            for (int i = 0; i < 4; ++i) {
                if (!out_active[i] && line_references_pass_macro(p, len, macros[i])) {
                    out_active[i] = true;
                }
            }
        }
        for (size_t k = 0; k + 1 < len; ++k) {
            if (!in_block_comment && p[k] == '/' && p[k + 1] == '/') {
                break;
            }
            if (!in_block_comment && p[k] == '/' && p[k + 1] == '*') {
                in_block_comment = true;
                k++;
            } else if (in_block_comment && p[k] == '*' && p[k + 1] == '/') {
                in_block_comment = false;
                k++;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
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
        if (strstr(user_src, "waywal_noise2d") != NULL) {
            return strdup(user_src);
        }
        const char *ver = strstr(user_src, "#version");
        const char *ver_nl = ver ? strchr(ver, '\n') : NULL;
        if (!ver_nl) {
            return strdup(user_src);
        }
        size_t head_len = (size_t)(ver_nl - user_src + 1);
        size_t cpro_len = sizeof(COMPUTE_PROLOGUE) - 1u;
        size_t tail_len = strlen(ver_nl + 1);
        char *out = (char *)malloc(head_len + cpro_len + tail_len + 1u);
        if (!out) {
            return strdup(user_src);
        }
        memcpy(out, user_src, head_len);
        memcpy(out + head_len, COMPUTE_PROLOGUE, cpro_len);
        memcpy(out + head_len + cpro_len, ver_nl + 1, tail_len + 1u);
        return out;
    }

    static const char *const pass_defines[SHADER_NUM_PASSES] = {
        "#define BUFFER_A 1\n", "#define BUFFER_B 1\n",   "#define BUFFER_C 1\n",
        "#define BUFFER_D 1\n", "#define IMAGE_PASS 1\n",
    };

    bool sec_present[SHADER_NUM_PASSES];
    bool use_sections = has_any_section_markers(user_src, sec_present);

    static const char ver_hdr[] = "#version 310 es\n";
    size_t ver_len = sizeof(ver_hdr) - 1u;
    size_t pro_len = sizeof(SHADERTOY_PROLOGUE) - 1u;
    size_t def_len = strlen(pass_defines[pass_idx]);
    size_t src_len = strlen(user_src);
    size_t epi_len = sizeof(SHADERTOY_EPILOGUE) - 1u;

    size_t num_lines = 1u;
    for (size_t i = 0; i < src_len; ++i) {
        if (user_src[i] == '\n') {
            num_lines++;
        }
    }

    char *wrapped =
        (char *)malloc(ver_len + pro_len + def_len + src_len + num_lines * 2u + epi_len + 32u);
    if (!wrapped) {
        return NULL;
    }
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
    } else if ((val_len == 9 && strncasecmp(val_str, "mpris_art", 9) == 0) ||
               (val_len == 9 && strncasecmp(val_str, "album_art", 9) == 0)) {
        binding->target = SHADER_CHAN_ALBUM_ART;
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

static void apply_parsed_buffer_scale(shader_pass_t passes[SHADER_NUM_PASSES], int target_pass,
                                      const char *val_str, size_t val_len)
{
    while (val_len > 0 &&
           (val_str[0] == ' ' || val_str[0] == '\t' || val_str[0] == '=' || val_str[0] == ':')) {
        val_str++;
        val_len--;
    }
    if (val_len == 0) {
        return;
    }
    char buf[64];
    size_t n = val_len < sizeof(buf) - 1u ? val_len : sizeof(buf) - 1u;
    memcpy(buf, val_str, n);
    buf[n] = '\0';
    char *endp = NULL;
    float sc = strtof(buf, &endp);
    if (endp == buf || !isfinite(sc) || sc <= 0.0f) {
        return;
    }
    if (sc < 0.125f) {
        sc = 0.125f;
    } else if (sc > 1.0f) {
        sc = 1.0f;
    }
    if (target_pass >= 0 && target_pass < 4) {
        passes[target_pass].res_scale = sc;
    } else {
        for (int p = 0; p < 4; ++p) {
            passes[p].res_scale = sc;
        }
    }
}

static void apply_parsed_buffer_fps(shader_pass_t passes[SHADER_NUM_PASSES], int target_pass,
                                    const char *val_str, size_t val_len)
{
    while (val_len > 0 &&
           (val_str[0] == ' ' || val_str[0] == '\t' || val_str[0] == '=' || val_str[0] == ':')) {
        val_str++;
        val_len--;
    }
    if (val_len == 0) {
        return;
    }
    char buf[64];
    size_t n = val_len < sizeof(buf) - 1u ? val_len : sizeof(buf) - 1u;
    memcpy(buf, val_str, n);
    buf[n] = '\0';
    char *endp = NULL;
    long fps = strtol(buf, &endp, 10);
    if (endp == buf || fps < 0) {
        return;
    }
    if (fps > 1000) {
        fps = 1000;
    }
    if (target_pass >= 0 && target_pass < 4) {
        passes[target_pass].target_fps = (uint32_t)fps;
    } else {
        for (int p = 0; p < 4; ++p) {
            passes[p].target_fps = (uint32_t)fps;
        }
    }
}

static void parse_shader_channel_directives(const char *user_src,
                                            shader_pass_t passes[SHADER_NUM_PASSES])
{
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        passes[p].res_scale = 1.0f;
        passes[p].target_fps = 0;
        passes[p].last_render_sec = -1e9;
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
                if (slen > 8 &&
                    (strncasecmp(s, "bufferA.", 8) == 0 || strncasecmp(s, "bufferA_", 8) == 0)) {
                    target_pass = 0;
                    s += 8;
                    slen -= 8;
                } else if (slen > 9 && (strncasecmp(s, "buffer_a.", 9) == 0 ||
                                        strncasecmp(s, "buffer_a_", 9) == 0)) {
                    target_pass = 0;
                    s += 9;
                    slen -= 9;
                } else if (slen > 8 && (strncasecmp(s, "bufferB.", 8) == 0 ||
                                        strncasecmp(s, "bufferB_", 8) == 0)) {
                    target_pass = 1;
                    s += 8;
                    slen -= 8;
                } else if (slen > 9 && (strncasecmp(s, "buffer_b.", 9) == 0 ||
                                        strncasecmp(s, "buffer_b_", 9) == 0)) {
                    target_pass = 1;
                    s += 9;
                    slen -= 9;
                } else if (slen > 8 && (strncasecmp(s, "bufferC.", 8) == 0 ||
                                        strncasecmp(s, "bufferC_", 8) == 0)) {
                    target_pass = 2;
                    s += 8;
                    slen -= 8;
                } else if (slen > 9 && (strncasecmp(s, "buffer_c.", 9) == 0 ||
                                        strncasecmp(s, "buffer_c_", 9) == 0)) {
                    target_pass = 2;
                    s += 9;
                    slen -= 9;
                } else if (slen > 8 && (strncasecmp(s, "bufferD.", 8) == 0 ||
                                        strncasecmp(s, "bufferD_", 8) == 0)) {
                    target_pass = 3;
                    s += 8;
                    slen -= 8;
                } else if (slen > 9 && (strncasecmp(s, "buffer_d.", 9) == 0 ||
                                        strncasecmp(s, "buffer_d_", 9) == 0)) {
                    target_pass = 3;
                    s += 9;
                    slen -= 9;
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
                } else if (slen >= 12 && strncasecmp(s, "buffer_scale", 12) == 0) {
                    apply_parsed_buffer_scale(passes, target_pass, s + 12, slen - 12);
                } else if (slen >= 5 && strncasecmp(s, "scale", 5) == 0) {
                    apply_parsed_buffer_scale(passes, target_pass, s + 5, slen - 5);
                } else if (slen >= 10 && strncasecmp(s, "buffer_fps", 10) == 0) {
                    apply_parsed_buffer_fps(passes, target_pass, s + 10, slen - 10);
                } else if (slen >= 3 && strncasecmp(s, "fps", 3) == 0) {
                    apply_parsed_buffer_fps(passes, target_pass, s + 3, slen - 3);
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
                } else if (slen > 6 && strncasecmp(s, "@wwal_", 6) == 0) {
                    const char *directive = s + 6;
                    size_t dlen = slen - 6;
                    int target_pass = cur_sec;
                    if (dlen > 7 && strncasecmp(directive, "buffer_", 7) == 0 &&
                        (directive[7] == 'a' || directive[7] == 'A' || directive[7] == 'b' ||
                         directive[7] == 'B' || directive[7] == 'c' || directive[7] == 'C' ||
                         directive[7] == 'd' || directive[7] == 'D') &&
                        (directive[8] == '_' || directive[8] == '.')) {
                        target_pass = tolower((unsigned char)directive[7]) - 'a';
                        directive += 9;
                        dlen -= 9;
                    }
                    if (dlen >= 12 && strncasecmp(directive, "buffer_scale", 12) == 0) {
                        apply_parsed_buffer_scale(passes, target_pass, directive + 12, dlen - 12);
                    } else if (dlen >= 5 && strncasecmp(directive, "scale", 5) == 0) {
                        apply_parsed_buffer_scale(passes, target_pass, directive + 5, dlen - 5);
                    } else if (dlen >= 10 && strncasecmp(directive, "buffer_fps", 10) == 0) {
                        apply_parsed_buffer_fps(passes, target_pass, directive + 10, dlen - 10);
                    } else if (dlen >= 3 && strncasecmp(directive, "fps", 3) == 0) {
                        apply_parsed_buffer_fps(passes, target_pass, directive + 3, dlen - 3);
                    }
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
    u->u_span_rect = glGetUniformLocation(prog, "u_span_rect");
    u->u_local_size = glGetUniformLocation(prog, "u_local_size");
    u->u_swap_rb = glGetUniformLocation(prog, "u_swap_rb");
    u->u_flip_y = glGetUniformLocation(prog, "u_flip_y");
    u->u_album_art = glGetUniformLocation(prog, "u_album_art");
    u->u_media_playing = glGetUniformLocation(prog, "u_media_playing");
    u->u_noise_tex = glGetUniformLocation(prog, "u_noise_tex");
}

static EGLDisplay s_shared_dpy = EGL_NO_DISPLAY;
static EGLContext s_shared_ctx = EGL_NO_CONTEXT;
static uint32_t s_shared_egl_refcount = 0;
static bool s_shared_dpy_borrowed = false;

static void make_context_current(waywal_shader_engine_t *eng)
{
    if (eng && eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != eng->context) {
            eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, eng->context);
        }
    }
}

static GLuint create_texture_ex(const uint8_t *pixels, uint32_t w, uint32_t h, GLint wrap_mode,
                                bool is_bgra, bool want_mipmaps)
{
    if (!pixels || w == 0 || h == 0) {
        return 0;
    }
    GLuint tex = 0;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    want_mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode);
    if (!want_mipmaps) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    if (is_bgra) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    if (want_mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    return tex;
}

static GLuint create_rgba_texture(const uint8_t *rgba, uint32_t w, uint32_t h, GLint wrap_mode)
{
    return create_texture_ex(rgba, w, h, wrap_mode, false, true);
}

static GLuint create_r8_depth_texture(const uint8_t *depth_px, uint32_t w, uint32_t h)
{
    if (!depth_px || w == 0 || h == 0) {
        return 0;
    }
    bool want_mipmaps = (w <= 64u && h <= 64u);
    size_t npx = (size_t)w * (size_t)h;
    uint8_t *r8 = (uint8_t *)malloc(npx);
    if (!r8) {
        return create_texture_ex(depth_px, w, h, GL_CLAMP_TO_EDGE, false, want_mipmaps);
    }
    /* Channel 0 (R in RGBA) or Channel 1 (G in both RGBA and BGRA grayscale depth maps) */
    for (size_t i = 0; i < npx; ++i) {
        r8[i] = depth_px[i * 4u];
    }
    GLuint tex = 0;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    want_mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)w, (GLsizei)h, 0, GL_RED, GL_UNSIGNED_BYTE, r8);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (want_mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    free(r8);
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
    eng->noise_tex =
        create_texture_ex(noise, SHADER_NOISE_DIM, SHADER_NOISE_DIM, GL_REPEAT, false, false);
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
    eng->loc_blit_swap_rb = -1;
    eng->depth_strength = 1.0f;

    if (s_shared_egl_refcount > 0 && s_shared_dpy != EGL_NO_DISPLAY &&
        s_shared_ctx != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() == s_shared_ctx ||
            eglMakeCurrent(s_shared_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_shared_ctx)) {
            eng->display = s_shared_dpy;
            eng->context = s_shared_ctx;
            eng->owns_egl = true;
            s_shared_egl_refcount++;
        }
    } else if (!dmabuf_ctx) {
        eng->display = eglGetCurrentDisplay();
        eng->context = eglGetCurrentContext();
    }

    if (eng->display == EGL_NO_DISPLAY || eng->context == EGL_NO_CONTEXT) {
        if (!dmabuf_ctx) {
            return eng;
        }
        if (!dmabuf_ctx->gbm || !dmabuf_ctx->available) {
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
            free(eng);
            return NULL;
        }

        eng->display = dpy;
        eng->context = ctx;
        eng->owns_egl = true;
        s_shared_dpy = dpy;
        s_shared_ctx = ctx;
        s_shared_egl_refcount = 1;
        s_shared_dpy_borrowed = true;
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
        glUseProgram(0);
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
    if (eng->last_rendered_bo == bo) {
        eng->last_rendered_bo = NULL;
        eng->last_rendered_tex = 0;
        eng->last_rendered_w = 0;
        eng->last_rendered_h = 0;
    }
    size_t dst = 0;
    for (size_t i = 0; i < eng->num_imported_bos; ++i) {
        if (eng->imported_bos[i].bo == bo) {
            if (eng->last_complete_bo_tex == eng->imported_bos[i].gl_tex) {
                eng->last_complete_bo_tex = 0;
            }
            if (!bo->egl_image && bo->gl_tex == eng->imported_bos[i].gl_tex) {
                ((dmabuf_bo_t *)bo)->gl_tex = 0;
            }
            if (eng->imported_bos[i].owns_egl && eng->display != EGL_NO_DISPLAY &&
                eng->context != EGL_NO_CONTEXT) {
                make_context_current(eng);
                if (eng->draw_fbo) {
                    glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0,
                                           0);
                }
                if (eng->read_fbo) {
                    glBindFramebuffer(GL_FRAMEBUFFER, eng->read_fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0,
                                           0);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (eng->imported_bos[i].gl_tex) {
                    glDeleteTextures(1, &eng->imported_bos[i].gl_tex);
                }
                if (eng->imported_bos[i].egl_image && eng->eglDestroyImageKHR) {
                    eng->eglDestroyImageKHR(eng->display, eng->imported_bos[i].egl_image);
                }
            }
        } else {
            if (dst != i) {
                eng->imported_bos[dst] = eng->imported_bos[i];
            }
            dst++;
        }
    }
    eng->num_imported_bos = dst;
}

void waywal_shader_engine_release_scratch(waywal_shader_engine_t *eng)
{
    if (!eng || eng->display == EGL_NO_DISPLAY || eng->context == EGL_NO_CONTEXT) {
        return;
    }
    make_context_current(eng);
    if (eng->draw_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
    if (eng->read_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, eng->read_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eng->last_complete_bo_tex = 0;
    for (int i = 0; i < 4; ++i) {
        shader_pass_t *pass = &eng->passes[i];
        for (int j = 0; j < 2; ++j) {
            if (pass->ping_pong_tex[j]) {
                glDeleteTextures(1, &pass->ping_pong_tex[j]);
                pass->ping_pong_tex[j] = 0;
            }
        }
        pass->tex_w = 0;
        pass->tex_h = 0;
        pass->read_idx = 0;
    }
    if (eng->scratch_tex) {
        glDeleteTextures(1, &eng->scratch_tex);
        eng->scratch_tex = 0;
        eng->scratch_w = 0;
        eng->scratch_h = 0;
    }
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
    if (eng->album_art_tex) {
        glDeleteTextures(1, &eng->album_art_tex);
    }
    if (eng->depth_tex) {
        glDeleteTextures(1, &eng->depth_tex);
    }
    if (eng->scratch_tex) {
        glDeleteTextures(1, &eng->scratch_tex);
    }
    if (eng->read_fbo) {
        glDeleteFramebuffers(1, &eng->read_fbo);
        eng->read_fbo = 0;
    }
    if (eng->draw_fbo) {
        glDeleteFramebuffers(1, &eng->draw_fbo);
        eng->draw_fbo = 0;
    }
    if (eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        for (size_t i = 0; i < eng->num_imported_bos; ++i) {
            const dmabuf_bo_t *bo = eng->imported_bos[i].bo;
            if (bo && !bo->egl_image && bo->gl_tex == eng->imported_bos[i].gl_tex) {
                ((dmabuf_bo_t *)bo)->gl_tex = 0;
            }
            if (!eng->imported_bos[i].owns_egl) {
                continue;
            }
            if (eng->imported_bos[i].gl_tex) {
                glDeleteTextures(1, &eng->imported_bos[i].gl_tex);
            }
            if (eng->imported_bos[i].egl_image && eng->eglDestroyImageKHR) {
                eng->eglDestroyImageKHR(eng->display, eng->imported_bos[i].egl_image);
            }
        }
    }
    eng->num_imported_bos = 0;
    if (eng->prog_blit) {
        glDeleteProgram(eng->prog_blit);
    }
    if (eng->blit_vao) {
        glDeleteVertexArrays(1, &eng->blit_vao);
    }
    if (eng->owns_egl) {
        if (s_shared_egl_refcount > 0) {
            s_shared_egl_refcount--;
        }
        if (s_shared_egl_refcount == 0) {
            eglMakeCurrent(eng->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(eng->display, eng->context);
            if (!s_shared_dpy_borrowed) {
                eglTerminate(eng->display);
            }
            s_shared_dpy = EGL_NO_DISPLAY;
            s_shared_ctx = EGL_NO_CONTEXT;
            s_shared_dpy_borrowed = false;
        }
    }
    if (eng->inotify_fd >= 0) {
        close(eng->inotify_fd);
    }
    free(eng);
}

static GLuint compile_or_load_pass(waywal_shader_engine_t *eng, const char *wrapped,
                                   const char *debug_name, bool *out_from_cache,
                                   bool *out_is_compute)
{
    bool is_compute =
        (strstr(wrapped, "local_size_x") != NULL && strstr(wrapped, "waywal_FragColor") == NULL);
    if (out_is_compute) {
        *out_is_compute = is_compute;
    }

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

    if (is_compute) {
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
    } else {
        static const char *vs_src =
            "#version 310 es\n"
            "void main() {\n"
            "    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";

        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        if (!vs || !fs) {
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            return 0;
        }

        glShaderSource(vs, 1, &vs_src, NULL);
        glCompileShader(vs);

        const char *fs_ptr = wrapped;
        glShaderSource(fs, 1, &fs_ptr, NULL);
        glCompileShader(fs);

        GLint compiled = 0;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log_buf[1024];
            glGetShaderInfoLog(fs, sizeof(log_buf), NULL, log_buf);
            WAYWAL_LOG_ERR("Shader compilation failed (%s): %s", debug_name ? debug_name : "live",
                           log_buf);
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }

        prog = glCreateProgram();
        if (!prog) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }

        glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }

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

static void recompute_engine_feature_flags(waywal_shader_engine_t *eng)
{
    bool any_audio_chan = false;
    bool any_date = false;
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        if (!eng->passes[p].active || !eng->passes[p].prog) {
            continue;
        }
        if (eng->passes[p].uniforms.iDate >= 0) {
            any_date = true;
        }
        for (int c = 0; c < 4; ++c) {
            if (eng->passes[p].uniforms.iChannel[c] >= 0 &&
                eng->passes[p].channels[c].target == SHADER_CHAN_AUDIO) {
                any_audio_chan = true;
            }
        }
    }
    eng->uses_audio_channel = any_audio_chan;
    eng->uses_date = any_date;
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
        bool is_compute = false;
        GLuint prog = compile_or_load_pass(eng, wrapped, debug_name, &from_cache, &is_compute);
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
        new_passes[p].is_compute = is_compute;
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

    bool has_tex_lod = (strstr(user_src, "textureLod(") != NULL);
    bool any_buf_lod = false;
    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        bool buf_needs_mip = false;
        if (has_tex_lod && p < 4 && new_passes[p].active) {
            shader_channel_target_t self_target =
                (shader_channel_target_t)((int)SHADER_CHAN_BUFFER_A + p);
            for (int rp = 0; rp < SHADER_NUM_PASSES; ++rp) {
                if (!new_passes[rp].active) {
                    continue;
                }
                for (int c = 0; c < 4; ++c) {
                    if (new_passes[rp].channels[c].target == self_target) {
                        buf_needs_mip = true;
                        any_buf_lod = true;
                        break;
                    }
                }
                if (buf_needs_mip) {
                    break;
                }
            }
        }
        new_passes[p].needs_mipmap = buf_needs_mip;
        destroy_pass_resources(&eng->passes[p]);
        eng->passes[p] = new_passes[p];
    }
    eng->uses_lod = any_buf_lod;
    eng->frame_idx = 0;
    recompute_engine_feature_flags(eng);

    for (uint32_t i = 0; i < eng->num_custom_uniforms; ++i) {
        for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
            eng->custom_uniforms[i].locations[p] = -2;
            eng->custom_uniforms[i].types[p] = 0;
        }
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
    eng->wallpaper_tex =
        create_texture_ex(rgba, w, h, GL_CLAMP_TO_EDGE, false, (w <= 64u && h <= 64u));
    eng->wallpaper_w = w;
    eng->wallpaper_h = h;
}

void waywal_shader_engine_set_wallpaper_bgra(waywal_shader_engine_t *eng, const uint8_t *bgra,
                                             uint32_t w, uint32_t h)
{
    if (!eng || !bgra || w == 0 || h == 0) {
        return;
    }
    make_context_current(eng);
    if (eng->wallpaper_tex) {
        glDeleteTextures(1, &eng->wallpaper_tex);
        eng->wallpaper_tex = 0;
    }
    eng->wallpaper_tex =
        create_texture_ex(bgra, w, h, GL_CLAMP_TO_EDGE, true, (w <= 64u && h <= 64u));
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

void waywal_shader_engine_set_album_art(waywal_shader_engine_t *eng, const uint8_t *rgba,
                                        uint32_t w, uint32_t h)
{
    if (!eng) {
        return;
    }
    if (eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        make_context_current(eng);
        if (eng->album_art_tex) {
            glDeleteTextures(1, &eng->album_art_tex);
            eng->album_art_tex = 0;
        }
        if (rgba && w > 0 && h > 0) {
            eng->album_art_tex =
                create_texture_ex(rgba, w, h, GL_CLAMP_TO_EDGE, false, (w <= 64u && h <= 64u));
        }
    }
    if (rgba && w > 0 && h > 0) {
        eng->album_art_w = w;
        eng->album_art_h = h;
    } else {
        eng->album_art_w = 0;
        eng->album_art_h = 0;
    }
}

void waywal_shader_engine_set_album_art_bgra(waywal_shader_engine_t *eng, const uint8_t *bgra,
                                             uint32_t w, uint32_t h)
{
    if (!eng) {
        return;
    }
    if (eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        make_context_current(eng);
        if (eng->album_art_tex) {
            glDeleteTextures(1, &eng->album_art_tex);
            eng->album_art_tex = 0;
        }
        if (bgra && w > 0 && h > 0) {
            eng->album_art_tex =
                create_texture_ex(bgra, w, h, GL_CLAMP_TO_EDGE, true, (w <= 64u && h <= 64u));
        }
    }
    if (bgra && w > 0 && h > 0) {
        eng->album_art_w = w;
        eng->album_art_h = h;
    } else {
        eng->album_art_w = 0;
        eng->album_art_h = 0;
    }
}

void waywal_shader_engine_set_media_playing(waywal_shader_engine_t *eng, float playing)
{
    if (!eng) {
        return;
    }
    eng->media_playing = playing;
}

static void query_custom_uniform_loc_and_type(GLuint prog, const char *name, GLint *out_loc,
                                              GLenum *out_type)
{
    *out_loc = -1;
    *out_type = GL_FLOAT;
    if (!prog || !name || name[0] == '\0') {
        return;
    }
    *out_loc = glGetUniformLocation(prog, name);
    if (*out_loc < 0) {
        return;
    }
    const GLchar *u_names[1] = {name};
    GLuint u_idx = GL_INVALID_INDEX;
    glGetUniformIndices(prog, 1, u_names, &u_idx);
    if (u_idx != GL_INVALID_INDEX) {
        GLint u_size = 0;
        GLenum u_type = GL_FLOAT;
        glGetActiveUniform(prog, u_idx, 0, NULL, &u_size, &u_type, NULL);
        if (u_type != 0) {
            *out_type = u_type;
        }
    }
}

bool waywal_shader_engine_set_custom_uniform(waywal_shader_engine_t *eng, const char *name,
                                             const float *values, uint32_t num_components)
{
    if (!eng || !name || name[0] == '\0' || !values || num_components < 1 || num_components > 4) {
        return false;
    }
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') {
        return false;
    }
    for (size_t k = 1; name[k] != '\0'; ++k) {
        if (!isalnum((unsigned char)name[k]) && name[k] != '_') {
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (i < (int)num_components && !isfinite(values[i])) {
            return false;
        }
    }
    if (strcmp(name, "u_depth_strength") == 0 && num_components == 1) {
        eng->depth_strength = values[0];
    } else if (strcmp(name, "u_media_playing") == 0 && num_components == 1) {
        eng->media_playing = values[0];
    }

    int slot = -1;
    for (uint32_t i = 0; i < eng->num_custom_uniforms; ++i) {
        if (eng->custom_uniforms[i].active && strcmp(eng->custom_uniforms[i].name, name) == 0) {
            slot = (int)i;
            break;
        }
        if (slot < 0 && !eng->custom_uniforms[i].active) {
            slot = (int)i;
        }
    }
    if (slot < 0) {
        if (eng->num_custom_uniforms < SHADER_MAX_CUSTOM_UNIFORMS) {
            slot = (int)eng->num_custom_uniforms++;
        } else {
            return false;
        }
    }
    shader_custom_uniform_t *cu = &eng->custom_uniforms[slot];
    if (!cu->active || strcmp(cu->name, name) != 0) {
        snprintf(cu->name, sizeof(cu->name), "%s", name);
        for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
            cu->locations[p] = -2;
            cu->types[p] = 0;
        }
    }
    cu->num_components = num_components;
    memset(cu->values, 0, sizeof(cu->values));
    memcpy(cu->values, values, num_components * sizeof(float));
    cu->active = true;

    if (eng->display != EGL_NO_DISPLAY && eng->context != EGL_NO_CONTEXT) {
        make_context_current(eng);
        for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
            if (eng->passes[p].active && eng->passes[p].prog) {
                query_custom_uniform_loc_and_type(eng->passes[p].prog, cu->name, &cu->locations[p],
                                                  &cu->types[p]);
            } else {
                cu->locations[p] = -1;
                cu->types[p] = 0;
            }
        }
    }
    return true;
}

bool waywal_shader_engine_get_custom_uniform(const waywal_shader_engine_t *eng, const char *name,
                                             float *out_values, uint32_t *out_num_components)
{
    if (!eng || !name) {
        return false;
    }
    for (uint32_t i = 0; i < eng->num_custom_uniforms; ++i) {
        if (eng->custom_uniforms[i].active && strcmp(eng->custom_uniforms[i].name, name) == 0) {
            if (out_values) {
                memcpy(out_values, eng->custom_uniforms[i].values, 4 * sizeof(float));
            }
            if (out_num_components) {
                *out_num_components = eng->custom_uniforms[i].num_components;
            }
            return true;
        }
    }
    return false;
}

void waywal_shader_engine_copy_custom_uniforms(waywal_shader_engine_t *dst,
                                               const waywal_shader_engine_t *src)
{
    if (!dst || !src || dst == src) {
        return;
    }
    for (uint32_t i = 0; i < src->num_custom_uniforms; ++i) {
        if (src->custom_uniforms[i].active) {
            (void)waywal_shader_engine_set_custom_uniform(dst, src->custom_uniforms[i].name,
                                                          src->custom_uniforms[i].values,
                                                          src->custom_uniforms[i].num_components);
        }
    }
}

int waywal_shader_engine_get_channel_target(const waywal_shader_engine_t *eng, int pass_idx,
                                            int chan_idx)
{
    if (!eng || pass_idx < 0 || pass_idx >= SHADER_NUM_PASSES || chan_idx < 0 || chan_idx >= 4) {
        return -1;
    }
    return (int)eng->passes[pass_idx].channels[chan_idx].target;
}

static bool load_depth_parallax_internal(waywal_shader_engine_t *eng, const uint8_t *base_px,
                                         uint32_t base_w, uint32_t base_h, const uint8_t *depth_px,
                                         uint32_t depth_w, uint32_t depth_h, bool is_bgra)
{
    if (!eng || !base_px || !depth_px || base_w == 0 || base_h == 0 || depth_w == 0 ||
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

    if (is_bgra) {
        waywal_shader_engine_set_wallpaper_bgra(eng, base_px, base_w, base_h);
    } else {
        waywal_shader_engine_set_wallpaper(eng, base_px, base_w, base_h);
    }

    if (eng->depth_tex) {
        glDeleteTextures(1, &eng->depth_tex);
        eng->depth_tex = 0;
    }
    eng->depth_tex = create_r8_depth_texture(depth_px, depth_w, depth_h);
    eng->depth_w = depth_w;
    eng->depth_h = depth_h;

    if (!waywal_shader_engine_load_source(eng, DEPTH_PARALLAX_POM_SHADER, "builtin_depth_pom")) {
        return false;
    }
    eng->passes[4].channels[0].target = SHADER_CHAN_WALLPAPER;
    eng->passes[4].channels[1].target = SHADER_CHAN_DEPTH;
    recompute_engine_feature_flags(eng);
    return true;
}

bool waywal_shader_engine_load_depth_parallax(waywal_shader_engine_t *eng, const uint8_t *base_rgba,
                                              uint32_t base_w, uint32_t base_h,
                                              const uint8_t *depth_rgba, uint32_t depth_w,
                                              uint32_t depth_h)
{
    return load_depth_parallax_internal(eng, base_rgba, base_w, base_h, depth_rgba, depth_w,
                                        depth_h, false);
}

bool waywal_shader_engine_load_depth_parallax_bgra(waywal_shader_engine_t *eng,
                                                   const uint8_t *base_bgra, uint32_t base_w,
                                                   uint32_t base_h, const uint8_t *depth_bgra,
                                                   uint32_t depth_w, uint32_t depth_h)
{
    return load_depth_parallax_internal(eng, base_bgra, base_w, base_h, depth_bgra, depth_w,
                                        depth_h, true);
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
    char resolved[4096];
    const char *target =
        waywal_shader_resolve_path(filepath, resolved, sizeof(resolved)) ? resolved : filepath;
    char *src = read_entire_file(target);
    if (!src) {
        return false;
    }
    setup_file_watches(eng, target);
    bool ok = waywal_shader_engine_load_source(eng, src, target);
    free(src);
    return ok;
}

bool waywal_shader_engine_compile(waywal_shader_engine_t *eng, const char *src_or_path)
{
    if (!eng || !src_or_path || src_or_path[0] == '\0') {
        return false;
    }
    if (strstr(src_or_path, "mainImage") != NULL || strstr(src_or_path, "void main") != NULL) {
        return waywal_shader_engine_load_source(eng, src_or_path, "inline");
    }
    return waywal_shader_engine_load_file(eng, src_or_path);
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

static void record_imported_bo(waywal_shader_engine_t *eng, const dmabuf_bo_t *bo, GLuint tex,
                               EGLImageKHR img, bool owns_egl)
{
    if (!eng || !bo) {
        return;
    }
    for (size_t i = 0; i < eng->num_imported_bos; ++i) {
        if (eng->imported_bos[i].bo == bo) {
            if (eng->imported_bos[i].gl_tex == tex && eng->imported_bos[i].egl_image == img) {
                if (owns_egl) {
                    eng->imported_bos[i].owns_egl = true;
                }
                eng->imported_bos[i].fd0 = bo->fd[0];
                eng->imported_bos[i].width = bo->width;
                eng->imported_bos[i].height = bo->height;
                eng->imported_bos[i].drm_format = bo->drm_format;
                return;
            }
            if (eng->last_complete_bo_tex == eng->imported_bos[i].gl_tex &&
                eng->imported_bos[i].gl_tex != tex) {
                eng->last_complete_bo_tex = 0;
            }
            if (eng->imported_bos[i].owns_egl) {
                if (eng->imported_bos[i].gl_tex && eng->imported_bos[i].gl_tex != tex) {
                    glDeleteTextures(1, &eng->imported_bos[i].gl_tex);
                }
                if (eng->imported_bos[i].egl_image && eng->imported_bos[i].egl_image != img &&
                    eng->eglDestroyImageKHR) {
                    eng->eglDestroyImageKHR(eng->display, eng->imported_bos[i].egl_image);
                }
            }
            eng->imported_bos[i].fd0 = bo->fd[0];
            eng->imported_bos[i].width = bo->width;
            eng->imported_bos[i].height = bo->height;
            eng->imported_bos[i].drm_format = bo->drm_format;
            eng->imported_bos[i].gl_tex = tex;
            eng->imported_bos[i].egl_image = img;
            eng->imported_bos[i].owns_egl = owns_egl;
            return;
        }
    }
    if (eng->num_imported_bos >= 32u) {
        if (eng->last_complete_bo_tex == eng->imported_bos[0].gl_tex) {
            eng->last_complete_bo_tex = 0;
        }
        if (eng->imported_bos[0].owns_egl) {
            if (eng->draw_fbo) {
                glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            if (eng->read_fbo) {
                glBindFramebuffer(GL_FRAMEBUFFER, eng->read_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (eng->imported_bos[0].gl_tex) {
                glDeleteTextures(1, &eng->imported_bos[0].gl_tex);
            }
            if (eng->imported_bos[0].egl_image && eng->eglDestroyImageKHR) {
                eng->eglDestroyImageKHR(eng->display, eng->imported_bos[0].egl_image);
            }
        }
        memmove(&eng->imported_bos[0], &eng->imported_bos[1], 31u * sizeof(eng->imported_bos[0]));
        eng->num_imported_bos = 31u;
    }
    size_t slot = eng->num_imported_bos++;
    eng->imported_bos[slot].bo = bo;
    eng->imported_bos[slot].fd0 = bo->fd[0];
    eng->imported_bos[slot].width = bo->width;
    eng->imported_bos[slot].height = bo->height;
    eng->imported_bos[slot].drm_format = bo->drm_format;
    eng->imported_bos[slot].gl_tex = tex;
    eng->imported_bos[slot].egl_image = img;
    eng->imported_bos[slot].owns_egl = owns_egl;
}

static bool ensure_bo_imported(waywal_shader_engine_t *eng, dmabuf_bo_t *bo, GLuint *out_tex)
{
    if (!eng || !bo || !out_tex) {
        return false;
    }
    for (size_t i = 0; i < eng->num_imported_bos; ++i) {
        if (eng->imported_bos[i].bo == bo && eng->imported_bos[i].fd0 == bo->fd[0] &&
            eng->imported_bos[i].width == bo->width && eng->imported_bos[i].height == bo->height &&
            eng->imported_bos[i].drm_format == bo->drm_format && eng->imported_bos[i].gl_tex != 0) {
            if (bo->gl_tex == 0) {
                bo->gl_tex = eng->imported_bos[i].gl_tex;
            }
            *out_tex = eng->imported_bos[i].gl_tex;
            return true;
        }
    }
    if (bo->fd[0] < 0) {
        if (bo->gl_tex != 0) {
            *out_tex = bo->gl_tex;
            return true;
        }
        return false;
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
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (eng->glEGLImageTargetTexStorageEXT) {
        eng->glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        eng->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (eng->display != EGL_NO_DISPLAY) {
        record_imported_bo(eng, bo, tex, img, true);
    }
    if (bo->gl_tex == 0) {
        bo->gl_tex = tex;
    }
    *out_tex = tex;
    return true;
}

static bool ensure_pass_ping_pong_textures(waywal_shader_engine_t *eng, shader_pass_t *pass,
                                           uint32_t w, uint32_t h, uint16_t **shared_zero_half,
                                           size_t *shared_zero_cap)
{
    (void)eng;
    if (pass->ping_pong_tex[0] && pass->ping_pong_tex[1] && pass->tex_w == w && pass->tex_h == h) {
        return false;
    }
    size_t needed = (size_t)w * (size_t)h * 4u;
    if (shared_zero_half && shared_zero_cap && (!*shared_zero_half || *shared_zero_cap < needed)) {
        free(*shared_zero_half);
        *shared_zero_half = (uint16_t *)calloc(needed, sizeof(uint16_t));
        *shared_zero_cap = *shared_zero_half ? needed : 0;
    }
    const uint16_t *zero_half = shared_zero_half ? *shared_zero_half : NULL;
    GLsizei levels = 1;
    if (pass->needs_mipmap) {
        uint32_t max_dim = w > h ? w : h;
        while (max_dim > 1u) {
            max_dim >>= 1u;
            levels++;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 2; ++i) {
        if (pass->ping_pong_tex[i]) {
            glDeleteTextures(1, &pass->ping_pong_tex[i]);
        }
        glGenTextures(1, &pass->ping_pong_tex[i]);
        glBindTexture(GL_TEXTURE_2D, pass->ping_pong_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA16F, (GLsizei)w, (GLsizei)h);
        if (zero_half) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_HALF_FLOAT,
                            zero_half);
            if (levels > 1) {
                glGenerateMipmap(GL_TEXTURE_2D);
            }
        }
    }
    pass->tex_w = w;
    pass->tex_h = h;
    pass->read_idx = 0;
    return true;
}

static void ensure_scratch_texture(waywal_shader_engine_t *eng, uint32_t w, uint32_t h)
{
    if (eng->scratch_tex && eng->scratch_w == w && eng->scratch_h == h) {
        return;
    }
    glActiveTexture(GL_TEXTURE0);
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
    case SHADER_CHAN_ALBUM_ART:
        if (eng->album_art_tex) {
            *out_tex = eng->album_art_tex;
            out_res[0] = (float)eng->album_art_w;
            out_res[1] = (float)eng->album_art_h;
            out_res[2] = 1.0f;
            return;
        }
        if (eng->wallpaper_tex) {
            *out_tex = eng->wallpaper_tex;
            out_res[0] = (float)eng->wallpaper_w;
            out_res[1] = (float)eng->wallpaper_h;
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

static void ensure_blit_resources(waywal_shader_engine_t *eng)
{
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
                                    "uniform int u_swap_rb;\n"
                                    "out highp vec4 frag_color;\n"
                                    "void main() {\n"
                                    "    vec4 c = texture(u_tex, v_uv);\n"
                                    "    frag_color = (u_swap_rb != 0) ? c.bgra : c;\n"
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
                    eng->loc_blit_swap_rb = glGetUniformLocation(p, "u_swap_rb");
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
}

bool waywal_shader_engine_render_frame(waywal_shader_engine_t *eng, dmabuf_bo_t *target_bo,
                                       const waywal_shader_frame_params_t *params)
{
    if (!eng || !target_bo || !params || !eng->passes[4].active || !eng->passes[4].prog) {
        return false;
    }

    make_context_current(eng);
    GLuint target_tex = 0;
    if (!ensure_bo_imported(eng, target_bo, &target_tex)) {
        return false;
    }

    if (eng->uses_audio_channel) {
        if (params->audio) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, eng->audio_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WAYWAL_AUDIO_TEX_WIDTH, WAYWAL_AUDIO_TEX_HEIGHT,
                            GL_RED, GL_UNSIGNED_BYTE, params->audio->shadertoy_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            eng->audio_tex_dirty = true;
        } else if (eng->audio_tex_dirty && eng->audio_tex) {
            uint8_t zero_tex[WAYWAL_AUDIO_TEX_WIDTH * WAYWAL_AUDIO_TEX_HEIGHT];
            memset(zero_tex, 0, WAYWAL_AUDIO_TEX_WIDTH);
            memset(zero_tex + WAYWAL_AUDIO_TEX_WIDTH, 128, WAYWAL_AUDIO_TEX_WIDTH);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, eng->audio_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WAYWAL_AUDIO_TEX_WIDTH, WAYWAL_AUDIO_TEX_HEIGHT,
                            GL_RED, GL_UNSIGNED_BYTE, zero_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            eng->audio_tex_dirty = false;
        }
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
        if (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f) {
            mouse_smooth_vals[0] = params->mouse[0];
            mouse_smooth_vals[1] = params->mouse[1];
            mouse_smooth_vals[2] = params->mouse[2];
            mouse_smooth_vals[3] = params->mouse[3];
        } else {
            mouse_smooth_vals[0] = (float)target_bo->width * 0.5f;
            mouse_smooth_vals[1] = (float)target_bo->height * 0.5f;
        }
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

    if (!eng->blit_vao) {
        glGenVertexArrays(1, &eng->blit_vao);
    }

    bool has_multipass = false;
    bool tex_recreated[4] = {false, false, false, false};
    uint32_t buf_w[4] = {target_bo->width, target_bo->width, target_bo->width, target_bo->width};
    uint32_t buf_h[4] = {target_bo->height, target_bo->height, target_bo->height,
                         target_bo->height};
    uint16_t *shared_zero_half = NULL;
    size_t shared_zero_cap = 0;
    for (int p = 0; p < 4; ++p) {
        if (eng->passes[p].active) {
            has_multipass = true;
            float sc = eng->passes[p].res_scale;
            if (sc <= 0.0f || sc > 1.0f) {
                sc = 1.0f;
            } else if (sc < 0.125f) {
                sc = 0.125f;
            }
            uint32_t bw = (uint32_t)lroundf((float)target_bo->width * sc);
            uint32_t bh = (uint32_t)lroundf((float)target_bo->height * sc);
            if (bw < 1u) {
                bw = 1u;
            }
            if (bh < 1u) {
                bh = 1u;
            }
            buf_w[p] = bw;
            buf_h[p] = bh;
            tex_recreated[p] = ensure_pass_ping_pong_textures(eng, &eng->passes[p], bw, bh,
                                                              &shared_zero_half, &shared_zero_cap);
        }
    }
    free(shared_zero_half);

    GLuint groups_x = (target_bo->width + 15u) / 16u;
    GLuint groups_y = (target_bo->height + 15u) / 16u;

    bool has_span = (params->span_rect[2] > 0.0f && params->span_rect[3] > 0.0f);
    uint32_t span_res_w = has_span ? (uint32_t)(params->span_rect[2] + 0.5f) : target_bo->width;
    uint32_t span_res_h = has_span ? (uint32_t)(params->span_rect[3] + 0.5f) : target_bo->height;
    float span_vec[4] = {
        has_span ? params->span_rect[0] : 0.0f,
        has_span ? params->span_rect[1] : 0.0f,
        has_span ? params->span_rect[2] : 0.0f,
        has_span ? params->span_rect[3] : 0.0f,
    };

    for (int p = 0; p < SHADER_NUM_PASSES; ++p) {
        shader_pass_t *pass = &eng->passes[p];
        if (!pass->active || !pass->prog) {
            continue;
        }

        if (p < 4 && !tex_recreated[p] && pass->target_fps > 0 && eng->frame_idx > 0 &&
            params->frame_index > 0 && (double)params->time_sec >= pass->last_render_sec &&
            ((double)params->time_sec - pass->last_render_sec) <
                (0.95 / (double)pass->target_fps)) {
            continue;
        }

        glUseProgram(pass->prog);
        const shader_uniforms_t *u = &pass->uniforms;
        bool init_u = !pass->uniforms_initialized;

        bool use_span_on_pass = (p == 4 && !has_multipass);
        uint32_t pass_res_w =
            (p < 4) ? buf_w[p] : (use_span_on_pass ? span_res_w : target_bo->width);
        uint32_t pass_res_h =
            (p < 4) ? buf_h[p] : (use_span_on_pass ? span_res_h : target_bo->height);
        uint32_t local_w = (p < 4) ? buf_w[p] : target_bo->width;
        uint32_t local_h = (p < 4) ? buf_h[p] : target_bo->height;
        const float *pass_span =
            use_span_on_pass ? span_vec : (const float[4]){0.0f, 0.0f, 0.0f, 0.0f};

        if (u->iResolution >= 0 &&
            (init_u || pass->cached_res_w != pass_res_w || pass->cached_res_h != pass_res_h)) {
            glUniform3f(u->iResolution, (float)pass_res_w, (float)pass_res_h, 1.0f);
            pass->cached_res_w = pass_res_w;
            pass->cached_res_h = pass_res_h;
        }
        if (u->u_local_size >= 0 &&
            (init_u || pass->cached_local_w != local_w || pass->cached_local_h != local_h)) {
            glUniform2f(u->u_local_size, (float)local_w, (float)local_h);
            pass->cached_local_w = local_w;
            pass->cached_local_h = local_h;
        }
        if (u->u_span_rect >= 0 && (init_u || pass->cached_span_rect[0] != pass_span[0] ||
                                    pass->cached_span_rect[1] != pass_span[1] ||
                                    pass->cached_span_rect[2] != pass_span[2] ||
                                    pass->cached_span_rect[3] != pass_span[3])) {
            glUniform4fv(u->u_span_rect, 1, pass_span);
            memcpy(pass->cached_span_rect, pass_span, sizeof(pass->cached_span_rect));
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
        float pass_mouse[4] = {
            params->mouse[0],
            params->mouse[1],
            params->mouse[2],
            params->mouse[3],
        };
        float pass_mouse_smooth[4] = {
            mouse_smooth_vals[0],
            mouse_smooth_vals[1],
            mouse_smooth_vals[2],
            mouse_smooth_vals[3],
        };
        if (has_span && !use_span_on_pass) {
            pass_mouse[0] -= span_vec[0];
            pass_mouse[1] -= span_vec[1];
            pass_mouse_smooth[0] -= span_vec[0];
            pass_mouse_smooth[1] -= span_vec[1];
            if (pass_mouse[2] > 0.0f)
                pass_mouse[2] -= span_vec[0];
            else if (pass_mouse[2] < 0.0f)
                pass_mouse[2] += span_vec[0];
            if (pass_mouse[3] > 0.0f)
                pass_mouse[3] -= span_vec[1];
            else if (pass_mouse[3] < 0.0f)
                pass_mouse[3] += span_vec[1];
            if (pass_mouse_smooth[2] > 0.0f)
                pass_mouse_smooth[2] -= span_vec[0];
            else if (pass_mouse_smooth[2] < 0.0f)
                pass_mouse_smooth[2] += span_vec[0];
            if (pass_mouse_smooth[3] > 0.0f)
                pass_mouse_smooth[3] -= span_vec[1];
            else if (pass_mouse_smooth[3] < 0.0f)
                pass_mouse_smooth[3] += span_vec[1];
        }
        if (p < 4 && pass->res_scale > 0.0f && pass->res_scale < 1.0f) {
            for (int mi = 0; mi < 4; ++mi) {
                pass_mouse[mi] *= pass->res_scale;
                pass_mouse_smooth[mi] *= pass->res_scale;
            }
        }
        if (u->iMouse >= 0 &&
            (init_u || memcmp(pass->cached_mouse, pass_mouse, sizeof(pass_mouse)) != 0)) {
            glUniform4fv(u->iMouse, 1, pass_mouse);
            memcpy(pass->cached_mouse, pass_mouse, sizeof(pass_mouse));
        }
        if (u->iDate >= 0) {
            glUniform4fv(u->iDate, 1, date_vals);
        }
        if (u->iSampleRate >= 0 && init_u) {
            glUniform1f(u->iSampleRate, 48000.0f);
        }
        if (u->u_palette_primary >= 0 &&
            (init_u || memcmp(pass->cached_palette_primary, params->palette_primary,
                              sizeof(pass->cached_palette_primary)) != 0)) {
            glUniform3fv(u->u_palette_primary, 1, params->palette_primary);
            memcpy(pass->cached_palette_primary, params->palette_primary,
                   sizeof(pass->cached_palette_primary));
        }
        if (u->u_palette_secondary >= 0 &&
            (init_u || memcmp(pass->cached_palette_secondary, params->palette_secondary,
                              sizeof(pass->cached_palette_secondary)) != 0)) {
            glUniform3fv(u->u_palette_secondary, 1, params->palette_secondary);
            memcpy(pass->cached_palette_secondary, params->palette_secondary,
                   sizeof(pass->cached_palette_secondary));
        }
        if (u->u_palette_tertiary >= 0 &&
            (init_u || memcmp(pass->cached_palette_tertiary, params->palette_tertiary,
                              sizeof(pass->cached_palette_tertiary)) != 0)) {
            glUniform3fv(u->u_palette_tertiary, 1, params->palette_tertiary);
            memcpy(pass->cached_palette_tertiary, params->palette_tertiary,
                   sizeof(pass->cached_palette_tertiary));
        }
        if (u->u_mouse_smooth >= 0 &&
            (init_u || memcmp(pass->cached_mouse_smooth, pass_mouse_smooth,
                              sizeof(pass_mouse_smooth)) != 0)) {
            glUniform4fv(u->u_mouse_smooth, 1, pass_mouse_smooth);
            memcpy(pass->cached_mouse_smooth, pass_mouse_smooth, sizeof(pass_mouse_smooth));
        }
        if (u->u_mouse_vel >= 0 && (init_u || memcmp(pass->cached_mouse_vel, params->mouse_vel,
                                                     sizeof(pass->cached_mouse_vel)) != 0)) {
            glUniform2fv(u->u_mouse_vel, 1, params->mouse_vel);
            memcpy(pass->cached_mouse_vel, params->mouse_vel, sizeof(pass->cached_mouse_vel));
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
        if (u->u_media_playing >= 0) {
            float mplay =
                (params->media_playing > 0.0f) ? params->media_playing : eng->media_playing;
            if (init_u || pass->cached_media_playing != mplay) {
                glUniform1f(u->u_media_playing, mplay);
                pass->cached_media_playing = mplay;
            }
        }
        if (u->u_album_art >= 0) {
            GLuint atex = eng->album_art_tex
                              ? eng->album_art_tex
                              : (eng->wallpaper_tex ? eng->wallpaper_tex : eng->audio_tex);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, atex);
            if (init_u) {
                glUniform1i(u->u_album_art, 4);
            }
        }
        if (u->u_noise_tex >= 0 && eng->noise_tex != 0) {
            glActiveTexture(GL_TEXTURE5);
            glBindTexture(GL_TEXTURE_2D, eng->noise_tex);
            if (init_u) {
                glUniform1i(u->u_noise_tex, 5);
            }
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
            pass->audio_uniforms_dirty = true;
        } else if (init_u || pass->audio_uniforms_dirty) {
            static const float zero_bands[WAYWAL_AUDIO_BARK_BANDS] = {0};
            if (u->u_audio_bands >= 0) {
                glUniform1fv(u->u_audio_bands, WAYWAL_AUDIO_BARK_BANDS, zero_bands);
            }
            if (u->u_audio_envelope >= 0) {
                glUniform1f(u->u_audio_envelope, 0.0f);
            }
            if (u->u_audio_beat >= 0) {
                glUniform1f(u->u_audio_beat, 0.0f);
            }
            pass->audio_uniforms_dirty = false;
        }

        for (uint32_t cu_idx = 0; cu_idx < eng->num_custom_uniforms; ++cu_idx) {
            shader_custom_uniform_t *cu = &eng->custom_uniforms[cu_idx];
            if (!cu->active) {
                continue;
            }
            if (cu->locations[p] == -2) {
                query_custom_uniform_loc_and_type(pass->prog, cu->name, &cu->locations[p],
                                                  &cu->types[p]);
            }
            GLint loc = cu->locations[p];
            if (loc >= 0) {
                GLenum ut = cu->types[p];
                float fvals[4] = {
                    cu->values[0],
                    (cu->num_components >= 2) ? cu->values[1] : cu->values[0],
                    (cu->num_components >= 3) ? cu->values[2] : cu->values[0],
                    (cu->num_components >= 4) ? cu->values[3] : cu->values[0],
                };
                GLint ivals[4] = {
                    (GLint)lroundf(fvals[0]),
                    (GLint)lroundf(fvals[1]),
                    (GLint)lroundf(fvals[2]),
                    (GLint)lroundf(fvals[3]),
                };
                switch (ut) {
                case GL_INT:
                case GL_BOOL:
                    glUniform1i(loc, ivals[0]);
                    break;
                case GL_INT_VEC2:
                case GL_BOOL_VEC2:
                    glUniform2iv(loc, 1, ivals);
                    break;
                case GL_INT_VEC3:
                case GL_BOOL_VEC3:
                    glUniform3iv(loc, 1, ivals);
                    break;
                case GL_INT_VEC4:
                case GL_BOOL_VEC4:
                    glUniform4iv(loc, 1, ivals);
                    break;
                case GL_FLOAT:
                    glUniform1f(loc, fvals[0]);
                    break;
                case GL_FLOAT_VEC2:
                    glUniform2fv(loc, 1, fvals);
                    break;
                case GL_FLOAT_VEC3:
                    glUniform3fv(loc, 1, fvals);
                    break;
                case GL_FLOAT_VEC4:
                    glUniform4fv(loc, 1, fvals);
                    break;
                default:
                    if (cu->num_components == 1) {
                        glUniform1f(loc, cu->values[0]);
                    } else if (cu->num_components == 2) {
                        glUniform2fv(loc, 1, cu->values);
                    } else if (cu->num_components == 3) {
                        glUniform3fv(loc, 1, cu->values);
                    } else if (cu->num_components == 4) {
                        glUniform4fv(loc, 1, cu->values);
                    }
                    break;
                }
            }
        }

        float chan_res[12] = {0};
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
        if (u->iChannelResolution >= 0 &&
            (init_u || memcmp(pass->cached_chan_res, chan_res, sizeof(chan_res)) != 0)) {
            glUniform3fv(u->iChannelResolution, 4, chan_res);
            memcpy(pass->cached_chan_res, chan_res, sizeof(chan_res));
        }
        if (u->iChannelTime >= 0) {
            glUniform1fv(u->iChannelTime, 4, chan_time);
        }

        if (p < 4) {
            int write_idx = 1 - pass->read_idx;
            GLuint pass_groups_x = (buf_w[p] + 15u) / 16u;
            GLuint pass_groups_y = (buf_h[p] + 15u) / 16u;
            if (u->u_swap_rb >= 0 && (init_u || pass->cached_swap_rb != 0)) {
                glUniform1i(u->u_swap_rb, 0);
                pass->cached_swap_rb = 0;
            }
            if (u->u_flip_y >= 0 && (init_u || pass->cached_flip_y != 0)) {
                glUniform1i(u->u_flip_y, 0);
                pass->cached_flip_y = 0;
            }
            pass->uniforms_initialized = true;
            if (pass->is_compute) {
                glBindImageTexture(0, pass->ping_pong_tex[write_idx], 0, GL_FALSE, 0, GL_WRITE_ONLY,
                                   GL_RGBA16F);
                glDispatchCompute(pass_groups_x, pass_groups_y, 1u);
                glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
                if (pass->needs_mipmap) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, pass->ping_pong_tex[write_idx]);
                    glGenerateMipmap(GL_TEXTURE_2D);
                }
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       pass->ping_pong_tex[write_idx], 0);
                glViewport(0, 0, (GLsizei)buf_w[p], (GLsizei)buf_h[p]);
                glBindVertexArray(eng->blit_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (pass->needs_mipmap) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, pass->ping_pong_tex[write_idx]);
                    glGenerateMipmap(GL_TEXTURE_2D);
                }
            }
            pass->read_idx = write_idx;
            pass->last_render_sec = (double)params->time_sec;
        } else if (pass->is_compute) {
            GLuint dst_tex = use_scratch_blit ? eng->scratch_tex : target_tex;
            int swap_rb = 1;
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
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
            if (!use_scratch_blit) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       target_tex, 0);
                if (eng->last_complete_bo_tex != target_tex) {
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        use_scratch_blit = true;
                        ensure_scratch_texture(eng, target_bo->width, target_bo->height);
                    } else {
                        eng->last_complete_bo_tex = target_tex;
                    }
                }
            }
            if (use_scratch_blit) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       eng->scratch_tex, 0);
            }
            if (u->u_swap_rb >= 0 && (init_u || pass->cached_swap_rb != 0)) {
                glUniform1i(u->u_swap_rb, 0);
                pass->cached_swap_rb = 0;
            }
            if (u->u_flip_y >= 0 && (init_u || pass->cached_flip_y != 1)) {
                glUniform1i(u->u_flip_y, 1);
                pass->cached_flip_y = 1;
            }
            pass->uniforms_initialized = true;
            glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
            glBindVertexArray(eng->blit_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    if (use_scratch_blit) {
        ensure_blit_resources(eng);

        glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
        bool fbo_ok = (eng->last_complete_bo_tex == target_tex);
        if (!fbo_ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            eng->last_complete_bo_tex = target_tex;
            fbo_ok = true;
        }
        if (eng->prog_blit && fbo_ok) {
            glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
            glUseProgram(eng->prog_blit);
            if (eng->loc_blit_swap_rb >= 0) {
                glUniform1i(eng->loc_blit_swap_rb, eng->passes[4].is_compute ? 1 : 0);
            }
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

        /* Discard transient scratch attachment after blit to avoid writeback traffic */
        glBindFramebuffer(GL_FRAMEBUFFER, eng->read_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               eng->scratch_tex, 0);
        const GLenum discard_att = GL_COLOR_ATTACHMENT0;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard_att);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glUseProgram(0);
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
    eng->last_rendered_bo = target_bo;
    eng->last_rendered_tex = target_tex;
    eng->last_rendered_w = target_bo->width;
    eng->last_rendered_h = target_bo->height;
    eng->frame_idx++;
    return true;
}

bool waywal_shader_engine_blit_from_tex(waywal_shader_engine_t *eng, uint32_t src_tex,
                                        uint32_t src_w, uint32_t src_h, dmabuf_bo_t *target_bo)
{
    if (!eng || !target_bo || src_w == 0 || src_h == 0) {
        return false;
    }

    make_context_current(eng);
    GLuint real_src_tex = (GLuint)src_tex;
    if (eng->last_rendered_bo && eng->last_rendered_tex != 0 &&
        (src_tex == 0 || eng->last_rendered_bo->gl_tex == src_tex ||
         eng->last_rendered_tex == src_tex) &&
        eng->last_rendered_w == src_w && eng->last_rendered_h == src_h) {
        real_src_tex = eng->last_rendered_tex;
    } else if (src_tex != 0) {
        for (size_t i = 0; i < eng->num_imported_bos; ++i) {
            const dmabuf_bo_t *b = eng->imported_bos[i].bo;
            if (b && (b->gl_tex == src_tex || eng->imported_bos[i].gl_tex == src_tex)) {
                real_src_tex = eng->imported_bos[i].gl_tex;
                break;
            }
        }
    }
    if (real_src_tex == 0) {
        return false;
    }

    GLuint target_tex = 0;
    if (!ensure_bo_imported(eng, target_bo, &target_tex)) {
        return false;
    }
    if (target_tex == real_src_tex) {
        return true;
    }

    ensure_blit_resources(eng);

    glBindFramebuffer(GL_FRAMEBUFFER, eng->draw_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
    bool fbo_ok = (eng->last_complete_bo_tex == target_tex);
    if (!fbo_ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        eng->last_complete_bo_tex = target_tex;
        fbo_ok = true;
    }
    if (eng->prog_blit && fbo_ok) {
        glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
        glUseProgram(eng->prog_blit);
        if (eng->loc_blit_swap_rb >= 0) {
            glUniform1i(eng->loc_blit_swap_rb, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, real_src_tex);
        glBindVertexArray(eng->blit_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, eng->read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               real_src_tex, 0);
        GLenum filter =
            (src_w == target_bo->width && src_h == target_bo->height) ? GL_NEAREST : GL_LINEAR;
        glBlitFramebuffer(0, 0, (GLint)src_w, (GLint)src_h, 0, 0, (GLint)target_bo->width,
                          (GLint)target_bo->height, GL_COLOR_BUFFER_BIT, filter);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }

    glUseProgram(0);
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

bool waywal_shader_engine_blit_bo(waywal_shader_engine_t *eng, dmabuf_bo_t *src_bo,
                                  dmabuf_bo_t *target_bo)
{
    if (!eng || !src_bo || !target_bo) {
        return false;
    }
    make_context_current(eng);
    GLuint src_tex = 0;
    if (!ensure_bo_imported(eng, src_bo, &src_tex)) {
        return false;
    }
    return waywal_shader_engine_blit_from_tex(eng, src_tex, src_bo->width, src_bo->height,
                                              target_bo);
}

bool waywal_shader_engine_has_active_program(const waywal_shader_engine_t *eng)
{
    return eng && eng->passes[4].active && eng->passes[4].prog != 0;
}

bool waywal_shader_engine_uses_mouse(const waywal_shader_engine_t *eng)
{
    if (!eng) {
        return false;
    }
    for (int i = 0; i < SHADER_NUM_PASSES; ++i) {
        if (!eng->passes[i].active || !eng->passes[i].prog) {
            continue;
        }
        const shader_uniforms_t *u = &eng->passes[i].uniforms;
        if (u->iMouse >= 0 || u->u_mouse_smooth >= 0 || u->u_mouse_vel >= 0) {
            return true;
        }
    }
    return false;
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

bool waywal_shader_engine_shares_egl_context(const waywal_shader_engine_t *a,
                                             const waywal_shader_engine_t *b)
{
    if (!a || !b || a->display == EGL_NO_DISPLAY || a->context == EGL_NO_CONTEXT) {
        return false;
    }
    return a->display == b->display && a->context == b->context;
}
