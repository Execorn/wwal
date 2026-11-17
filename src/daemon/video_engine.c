// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/video_engine.h"

#include "gpu_compute.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "wayland_core.h"
#include "waywal/log.h"
#include "waywal/power_governor.h"
#include "waywal/render_engine.h"
#include "waywal/shader_engine.h"
#include "waywal/we_scene.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 0x20203852U
#endif
#ifndef DRM_FORMAT_RG88
#define DRM_FORMAT_RG88 0x38384752U
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88 0x38385247U
#endif
#ifndef DRM_FORMAT_R16
#define DRM_FORMAT_R16 0x20363152U
#endif
#ifndef DRM_FORMAT_RG1616
#define DRM_FORMAT_RG1616 0x32334752U
#endif
#ifndef DRM_FORMAT_GR1616
#define DRM_FORMAT_GR1616 0x32335247U
#endif

static void video_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    int *in_use = (int *)data;
    if (in_use && *in_use > 0) {
        (*in_use)--;
    }
}

static const struct wl_buffer_listener g_video_buffer_listener = {
    .release = video_buffer_release,
};

static const char *const SHADER_YUV_HDR_RGBA8 =
    "#version 310 es\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_target;\n";

static const char *const SHADER_YUV_HDR_RGBA16F =
    "#version 310 es\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(rgba16f, binding = 0) writeonly uniform highp image2D u_target;\n";

static const char *const SHADER_YUV_EVAL_PART =
    "layout(binding = 1) uniform highp sampler2D u_y_tex;\n"
    "layout(binding = 2) uniform highp sampler2D u_uv_tex;\n"
    "uniform ivec2 u_src_res;\n"
    "uniform ivec2 u_dst_res;\n"
    "uniform int u_is_10bit;\n"
    "uniform int u_color_std;\n"
    "uniform int u_full_range;\n"
    "uniform int u_swap_uv;\n"
    "uniform int u_scaling_mode;\n"
    "uniform highp vec4 u_clear_color;\n"
    "uniform ivec4 u_span_bounds;\n"
    "uniform highp vec2 u_crop_gravity;\n"
    "highp vec3 eval_yuv(highp vec2 s_px, bool block_chroma) {\n"
    "    highp vec2 src_f = vec2(u_src_res);\n"
    "    highp vec2 uv_res = vec2(max((u_src_res.x + 1) / 2, 1), max((u_src_res.y + 1) / 2, "
    "1));\n"
    "    highp vec2 y_tc = clamp(s_px, vec2(0.5), src_f - 0.5) / src_f;\n"
    "    highp vec2 uv_px = block_chroma ? (floor(s_px * 0.5) + 0.5) : (s_px * 0.5);\n"
    "    highp vec2 uv_tc = clamp(uv_px, vec2(0.5), uv_res - 0.5) / uv_res;\n"
    "    highp float y_n = texture(u_y_tex, y_tc).r;\n"
    "    highp vec2 uv_n = texture(u_uv_tex, uv_tc).rg;\n"
    "    if (u_swap_uv != 0) uv_n = uv_n.yx;\n"
    "    highp float y, cb, cr;\n"
    "    if (u_is_10bit != 0) {\n"
    "        highp float y10 = y_n * (65535.0 / 64.0);\n"
    "        highp vec2 uv10 = uv_n * (65535.0 / 64.0);\n"
    "        if (u_full_range != 0) {\n"
    "            y = clamp(y10 / 1023.0, 0.0, 1.0);\n"
    "            cb = (uv10.x - 512.0) / 1023.0;\n"
    "            cr = (uv10.y - 512.0) / 1023.0;\n"
    "        } else {\n"
    "            y = clamp((y10 - 64.0) / 876.0, 0.0, 1.0);\n"
    "            cb = (uv10.x - 512.0) / 896.0;\n"
    "            cr = (uv10.y - 512.0) / 896.0;\n"
    "        }\n"
    "    } else {\n"
    "        highp float y8 = y_n * 255.0;\n"
    "        highp vec2 uv8 = uv_n * 255.0;\n"
    "        if (u_full_range != 0) {\n"
    "            y = clamp(y8 / 255.0, 0.0, 1.0);\n"
    "            cb = (uv8.x - 128.0) / 255.0;\n"
    "            cr = (uv8.y - 128.0) / 255.0;\n"
    "        } else {\n"
    "            y = clamp((y8 - 16.0) / 219.0, 0.0, 1.0);\n"
    "            cb = (uv8.x - 128.0) / 224.0;\n"
    "            cr = (uv8.y - 128.0) / 224.0;\n"
    "        }\n"
    "    }\n"
    "    int std_id = u_color_std;\n"
    "    if (std_id == 0) std_id = (u_src_res.x >= 1280 || u_src_res.y >= 720) ? 2 : 1;\n"
    "    highp vec3 rgb;\n"
    "    if (std_id == 1) {\n"
    "        rgb = vec3(y + 1.402 * cr, y - 0.344136286 * cb - 0.714136286 * cr, y + 1.772 * "
    "cb);\n"
    "    } else if (std_id == 3) {\n"
    "        rgb = vec3(y + 1.4746 * cr, y - 0.164553127 * cb - 0.571353127 * cr, y + 1.8814 * "
    "cb);\n"
    "    } else {\n"
    "        rgb = vec3(y + 1.5748 * cr, y - 0.187324273 * cb - 0.468124273 * cr, y + 1.8556 * "
    "cb);\n"
    "    }\n"
    "    return clamp(rgb, vec3(0.0), vec3(1.0));\n"
    "}\n";

static const char *const SHADER_YUV_MAIN_PART =
    "void main() {\n"
    "    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (coord.x >= u_dst_res.x || coord.y >= u_dst_res.y) return;\n"
    "    if (u_src_res.x <= 0 || u_src_res.y <= 0) {\n"
    "        imageStore(u_target, coord, vec4(u_clear_color.rgb, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    if (u_dst_res.x == u_src_res.x && u_dst_res.y == u_src_res.y && u_scaling_mode != 4 && "
    "u_scaling_mode != 5) {\n"
    "        imageStore(u_target, coord, vec4(eval_yuv(vec2(coord) + 0.5, true), 1.0));\n"
    "        return;\n"
    "    }\n"
    "    highp vec2 src_f = vec2(u_src_res), dst_f = vec2(u_dst_res);\n"
    "    highp vec3 out_rgb = u_clear_color.rgb;\n"
    "    if (u_scaling_mode == 0 || u_scaling_mode == 5) {\n"
    "        highp vec2 tot = (u_scaling_mode == 5 && u_span_bounds.z > 0 && u_span_bounds.w > "
    "0) ? vec2(u_span_bounds.zw) : dst_f;\n"
    "        highp vec2 soff = (u_scaling_mode == 5) ? vec2(u_span_bounds.xy) : vec2(0.0);\n"
    "        if (int(tot.x) == u_src_res.x && int(tot.y) == u_src_res.y) {\n"
    "            highp vec2 sc = clamp(soff + vec2(coord), vec2(0.0), src_f - 1.0);\n"
    "            out_rgb = eval_yuv(sc + 0.5, true);\n"
    "        } else {\n"
    "            highp float r_src = src_f.x / src_f.y, r_dst = tot.x / tot.y;\n"
    "            highp vec2 crop = (r_src > r_dst) ? vec2(src_f.y * r_dst, src_f.y) : "
    "vec2(src_f.x, src_f.x / r_dst);\n"
    "            highp vec2 off = (src_f - crop) * u_crop_gravity;\n"
    "            highp vec2 s_px = off + (soff + vec2(coord) + 0.5) * crop / tot;\n"
    "            out_rgb = eval_yuv(s_px, false);\n"
    "        }\n"
    "    } else if (u_scaling_mode == 1) {\n"
    "        highp float r_src = src_f.x / src_f.y, r_dst = dst_f.x / dst_f.y;\n"
    "        if (r_src > r_dst) {\n"
    "            int sh = min(int(dst_f.x / r_src + 0.5), u_dst_res.y), py = (u_dst_res.y - sh) "
    "/ 2;\n"
    "            if (coord.y >= py && coord.y < py + sh && sh > 0) {\n"
    "                highp vec2 s_px = vec2((float(coord.x) + 0.5) * src_f.x / dst_f.x, "
    "(float(coord.y - py) + 0.5) * src_f.y / float(sh));\n"
    "                out_rgb = eval_yuv(s_px, false);\n"
    "            }\n"
    "        } else {\n"
    "            int sw = min(int(dst_f.y * r_src + 0.5), u_dst_res.x), px = (u_dst_res.x - sw) "
    "/ 2;\n"
    "            if (coord.x >= px && coord.x < px + sw && sw > 0) {\n"
    "                highp vec2 s_px = vec2((float(coord.x - px) + 0.5) * src_f.x / float(sw), "
    "(float(coord.y) + 0.5) * src_f.y / dst_f.y);\n"
    "                out_rgb = eval_yuv(s_px, false);\n"
    "            }\n"
    "        }\n"
    "    } else if (u_scaling_mode == 2) {\n"
    "        highp vec2 s_px = (vec2(coord) + 0.5) * src_f / dst_f;\n"
    "        out_rgb = eval_yuv(s_px, false);\n"
    "    } else if (u_scaling_mode == 3) {\n"
    "        int sx = coord.x - (u_dst_res.x - u_src_res.x) / 2, sy = coord.y - (u_dst_res.y - "
    "u_src_res.y) / 2;\n"
    "        if (sx >= 0 && sx < u_src_res.x && sy >= 0 && sy < u_src_res.y)\n"
    "            out_rgb = eval_yuv(vec2(float(sx), float(sy)) + 0.5, true);\n"
    "    } else if (u_scaling_mode == 4) {\n"
    "        out_rgb = eval_yuv(vec2(float(coord.x % u_src_res.x), float(coord.y % u_src_res.y)) "
    "+ 0.5, true);\n"
    "    }\n"
    "    imageStore(u_target, coord, vec4(out_rgb, 1.0));\n"
    "}\n";

typedef struct {
    GLuint prog;
    GLint loc_src_res;
    GLint loc_dst_res;
    GLint loc_is_10bit;
    GLint loc_color_std;
    GLint loc_full_range;
    GLint loc_swap_uv;
    GLint loc_scaling_mode;
    GLint loc_clear_color;
    GLint loc_span_bounds;
    GLint loc_crop_gravity;
} video_yuv_prog_t;

typedef struct {
    EGLDisplay egl_dpy;
    PFNEGLCREATEIMAGEKHRPROC pfn_eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC pfn_eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_glEGLImageTargetTexture2DOES;
    wwal_egl_sync_procs_t sync_procs;
    wwal_scratch_present_state_t present_state;
    video_yuv_prog_t prog8;
    video_yuv_prog_t prog16f;
    GLuint scratch8_tex;
    uint32_t scratch8_w;
    uint32_t scratch8_h;
    GLuint scratch16f_tex;
    uint32_t scratch16f_w;
    uint32_t scratch16f_h;
    bool egl_procs_ready;
} video_yuv_gl_ctx_t;

static void video_yuv_query_uniforms(video_yuv_prog_t *vp)
{
    vp->loc_src_res = glGetUniformLocation(vp->prog, "u_src_res");
    vp->loc_dst_res = glGetUniformLocation(vp->prog, "u_dst_res");
    vp->loc_is_10bit = glGetUniformLocation(vp->prog, "u_is_10bit");
    vp->loc_color_std = glGetUniformLocation(vp->prog, "u_color_std");
    vp->loc_full_range = glGetUniformLocation(vp->prog, "u_full_range");
    vp->loc_swap_uv = glGetUniformLocation(vp->prog, "u_swap_uv");
    vp->loc_scaling_mode = glGetUniformLocation(vp->prog, "u_scaling_mode");
    vp->loc_clear_color = glGetUniformLocation(vp->prog, "u_clear_color");
    vp->loc_span_bounds = glGetUniformLocation(vp->prog, "u_span_bounds");
    vp->loc_crop_gravity = glGetUniformLocation(vp->prog, "u_crop_gravity");
}

static video_yuv_gl_ctx_t *video_yuv_ensure_gl_ctx(video_engine_t *ve)
{
    if (!ve || !ve->daemon_state || !ve->daemon_state->render_engine.gpu_compute_active) {
        return NULL;
    }
    render_engine_make_current(&ve->daemon_state->render_engine);
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return NULL;
    }

    video_yuv_gl_ctx_t *yg = (video_yuv_gl_ctx_t *)ve->yuv_gl_ctx;
    if (!yg) {
        yg = (video_yuv_gl_ctx_t *)calloc(1, sizeof(video_yuv_gl_ctx_t));
        if (!yg)
            return NULL;
        ve->yuv_gl_ctx = yg;
    }

    if (!yg->egl_procs_ready || yg->egl_dpy != dpy) {
        yg->egl_dpy = dpy;
        yg->pfn_eglCreateImageKHR =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        yg->pfn_eglDestroyImageKHR =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        yg->pfn_glEGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!yg->pfn_eglCreateImageKHR || !yg->pfn_eglDestroyImageKHR ||
            !yg->pfn_glEGLImageTargetTexture2DOES) {
            return NULL;
        }
        wwal_egl_init_sync_procs(dpy, &yg->sync_procs);
        yg->egl_procs_ready = true;
    }
    return yg;
}

static void video_yuv_gl_destroy(video_engine_t *ve)
{
    if (!ve || !ve->yuv_gl_ctx)
        return;
    video_yuv_gl_ctx_t *yg = (video_yuv_gl_ctx_t *)ve->yuv_gl_ctx;
    if (ve->daemon_state && ve->daemon_state->render_engine.gpu_compute_active) {
        render_engine_make_current(&ve->daemon_state->render_engine);
        if (yg->scratch8_tex != 0u) {
            glDeleteTextures(1, &yg->scratch8_tex);
        }
        if (yg->scratch16f_tex != 0u) {
            glDeleteTextures(1, &yg->scratch16f_tex);
        }
        if (yg->prog8.prog != 0u) {
            glDeleteProgram(yg->prog8.prog);
        }
        if (yg->prog16f.prog != 0u) {
            glDeleteProgram(yg->prog16f.prog);
        }
        wwal_scratch_present_destroy(&yg->present_state);
    }
    free(yg);
    ve->yuv_gl_ctx = NULL;
}

static bool video_yuv_import_plane_tex(video_yuv_gl_ctx_t *yg, uint32_t width, uint32_t height,
                                       uint32_t drm_format, int fd, uint32_t offset,
                                       uint32_t stride, uint64_t modifier, GLenum filter,
                                       EGLImageKHR *out_img, GLuint *out_tex)
{
    if (!yg || fd < 0 || width == 0 || height == 0 || stride == 0 || !out_img || !out_tex)
        return false;
    EGLint attribs[24];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)drm_format;
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attribs[n++] = fd;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attribs[n++] = (EGLint)offset;
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attribs[n++] = (EGLint)stride;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[n++] = (EGLint)(modifier & 0xFFFFFFFFULL);
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[n++] = (EGLint)((modifier >> 32) & 0xFFFFFFFFULL);
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img = yg->pfn_eglCreateImageKHR(yg->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                                NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR)
        return false;

    GLuint tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) {
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, img);
        return false;
    }
    while (glGetError() != GL_NO_ERROR)
        ;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    yg->pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, img);
        return false;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    *out_img = img;
    *out_tex = tex;
    return true;
}

static bool video_yuv_import_dst_bo(video_yuv_gl_ctx_t *yg, dmabuf_bo_t *bo)
{
    if (!yg || !bo || bo->width == 0 || bo->height == 0)
        return false;
    if (bo->egl_image && bo->gl_tex != 0u)
        return true;

    static const EGLint plane_fd_keys[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                            EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint plane_off_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint plane_pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint plane_mod_lo[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint plane_mod_hi[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    uint32_t np = bo->num_planes > 0 ? bo->num_planes : 1;
    if (np > 4)
        np = 4;
    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)bo->width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)bo->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)bo->drm_format;
    for (uint32_t p = 0; p < np; ++p) {
        if (bo->fd[p] < 0)
            continue;
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_off_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];
        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFFULL);
            attribs[n++] = plane_mod_hi[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32) & 0xFFFFFFFFULL);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img = yg->pfn_eglCreateImageKHR(yg->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                                NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR)
        return false;

    GLuint tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) {
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, img);
        return false;
    }
    while (glGetError() != GL_NO_ERROR)
        ;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    yg->pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, img);
        return false;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    bo->egl_image = img;
    bo->gl_tex = tex;
    return true;
}

typedef struct {
    EGLImageKHR img_y;
    EGLImageKHR img_uv;
    GLuint tex_y;
    GLuint tex_uv;
    bool is_10bit;
    bool swap_uv;
    int color_std;
    bool full_range;
    uint32_t width;
    uint32_t height;
} video_yuv_imported_frame_t;

static bool video_yuv_import_prime_frame(video_yuv_gl_ctx_t *yg, const vaapi_prime_frame_t *pf,
                                         video_yuv_imported_frame_t *out)
{
    if (!yg || !pf || !out || pf->num_planes < 2 || pf->width == 0 || pf->height == 0)
        return false;
    memset(out, 0, sizeof(*out));
    out->img_y = EGL_NO_IMAGE_KHR;
    out->img_uv = EGL_NO_IMAGE_KHR;
    out->is_10bit = pf->is_10bit || (pf->drm_format == DRM_FORMAT_P010);

    uint32_t fmt_y = pf->layer_formats[0];
    uint32_t fmt_uv = pf->layer_formats[1];
    if (fmt_y == 0)
        fmt_y = out->is_10bit ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
    if (fmt_uv == 0)
        fmt_uv = out->is_10bit ? DRM_FORMAT_RG1616 : DRM_FORMAT_RG88;
    out->swap_uv = (fmt_uv == DRM_FORMAT_GR88 || fmt_uv == DRM_FORMAT_GR1616);
    out->color_std = pf->color_space;
    out->full_range = pf->full_range;
    out->width = pf->width;
    out->height = pf->height;

    uint32_t uv_w = (pf->width + 1U) / 2U;
    uint32_t uv_h = (pf->height + 1U) / 2U;
    if (!video_yuv_import_plane_tex(yg, pf->width, pf->height, fmt_y, pf->fds[0], pf->offsets[0],
                                    pf->strides[0], pf->modifiers[0], GL_LINEAR, &out->img_y,
                                    &out->tex_y)) {
        return false;
    }
    if (!video_yuv_import_plane_tex(yg, uv_w, uv_h, fmt_uv, pf->fds[1], pf->offsets[1],
                                    pf->strides[1], pf->modifiers[1], GL_LINEAR, &out->img_uv,
                                    &out->tex_uv)) {
        glDeleteTextures(1, &out->tex_y);
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, out->img_y);
        out->tex_y = 0u;
        out->img_y = EGL_NO_IMAGE_KHR;
        return false;
    }
    return true;
}

static void video_yuv_release_imported_frame(video_yuv_gl_ctx_t *yg,
                                             video_yuv_imported_frame_t *imp)
{
    if (!yg || !imp)
        return;
    if (imp->tex_y != 0u) {
        glDeleteTextures(1, &imp->tex_y);
        imp->tex_y = 0u;
    }
    if (imp->tex_uv != 0u) {
        glDeleteTextures(1, &imp->tex_uv);
        imp->tex_uv = 0u;
    }
    if (imp->img_y != EGL_NO_IMAGE_KHR) {
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, imp->img_y);
        imp->img_y = EGL_NO_IMAGE_KHR;
    }
    if (imp->img_uv != EGL_NO_IMAGE_KHR) {
        yg->pfn_eglDestroyImageKHR(yg->egl_dpy, imp->img_uv);
        imp->img_uv = EGL_NO_IMAGE_KHR;
    }
}

static bool video_yuv_render_imported_to_bo(video_engine_t *ve, video_yuv_gl_ctx_t *yg,
                                            const video_yuv_imported_frame_t *imp,
                                            dmabuf_bo_t *dst_bo, uint32_t scaling_mode,
                                            uint32_t clear_color, int32_t span_ox, int32_t span_oy,
                                            int32_t span_tw, int32_t span_th, float grav_x,
                                            float grav_y)
{
    if (!ve || !yg || !imp || !dst_bo || !ve->daemon_state)
        return false;
    if (!video_yuv_import_dst_bo(yg, dst_bo))
        return false;

    bool use_16f = imp->is_10bit || dst_bo->is_10bit ||
                   dst_bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_ARGB2101010 ||
                   dst_bo->drm_format == (uint32_t)WWAL_DRM_FORMAT_XRGB2101010;
    video_yuv_prog_t *vp = use_16f ? &yg->prog16f : &yg->prog8;
    if (vp->prog == 0u) {
        char cs_buf[8192];
        int n = snprintf(cs_buf, sizeof(cs_buf), "%s%s%s",
                         use_16f ? SHADER_YUV_HDR_RGBA16F : SHADER_YUV_HDR_RGBA8,
                         SHADER_YUV_EVAL_PART, SHADER_YUV_MAIN_PART);
        if (n <= 0 || (size_t)n >= sizeof(cs_buf))
            return false;
        vp->prog = wwal_gl_compile_compute_program_cached(
            cs_buf, use_16f ? "wwal_prog_yuv_to_rgba16f" : "wwal_prog_yuv_to_rgba8", NULL);
        if (vp->prog == 0u)
            return false;
        video_yuv_query_uniforms(vp);
    }

    GLuint *scratch_slot = use_16f ? &yg->scratch16f_tex : &yg->scratch8_tex;
    uint32_t *sw = use_16f ? &yg->scratch16f_w : &yg->scratch8_w;
    uint32_t *sh = use_16f ? &yg->scratch16f_h : &yg->scratch8_h;
    if (*scratch_slot == 0u || *sw != dst_bo->width || *sh != dst_bo->height) {
        if (*scratch_slot != 0u) {
            glDeleteTextures(1, scratch_slot);
            *scratch_slot = 0u;
        }
        *scratch_slot = wwal_gl_alloc_linear_scratch_tex(
            dst_bo->width, dst_bo->height, use_16f ? GL_RGBA16F : GL_RGBA8, GL_NEAREST, false);
        if (*scratch_slot == 0u)
            return false;
        *sw = dst_bo->width;
        *sh = dst_bo->height;
    }

    glUseProgram(vp->prog);
    glBindImageTexture(0, *scratch_slot, 0, GL_FALSE, 0, GL_WRITE_ONLY,
                       use_16f ? GL_RGBA16F : GL_RGBA8);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, imp->tex_y);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, imp->tex_uv);

    if (vp->loc_src_res >= 0)
        glUniform2i(vp->loc_src_res, (GLint)imp->width, (GLint)imp->height);
    if (vp->loc_dst_res >= 0)
        glUniform2i(vp->loc_dst_res, (GLint)dst_bo->width, (GLint)dst_bo->height);
    if (vp->loc_is_10bit >= 0)
        glUniform1i(vp->loc_is_10bit, imp->is_10bit ? 1 : 0);
    if (vp->loc_color_std >= 0)
        glUniform1i(vp->loc_color_std, imp->color_std);
    if (vp->loc_full_range >= 0)
        glUniform1i(vp->loc_full_range, imp->full_range ? 1 : 0);
    if (vp->loc_swap_uv >= 0)
        glUniform1i(vp->loc_swap_uv, imp->swap_uv ? 1 : 0);
    if (vp->loc_scaling_mode >= 0)
        glUniform1i(vp->loc_scaling_mode, (GLint)scaling_mode);
    if (vp->loc_clear_color >= 0) {
        float ca = (float)((clear_color >> 24) & 0xFFu) / 255.0f;
        float cr = (float)((clear_color >> 16) & 0xFFu) / 255.0f;
        float cg = (float)((clear_color >> 8) & 0xFFu) / 255.0f;
        float cb = (float)(clear_color & 0xFFu) / 255.0f;
        glUniform4f(vp->loc_clear_color, cr, cg, cb, ca);
    }
    if (vp->loc_span_bounds >= 0)
        glUniform4i(vp->loc_span_bounds, span_ox, span_oy, span_tw, span_th);
    if (vp->loc_crop_gravity >= 0)
        glUniform2f(vp->loc_crop_gravity, grav_x, grav_y);

    glDispatchCompute((dst_bo->width + 15U) / 16U, (dst_bo->height + 15U) / 16U, 1U);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    wwal_target_bo_t tbo = wwal_target_bo_from_dmabuf(dst_bo);
    bool ok = wwal_gl_present_via_scratch(&yg->present_state, *scratch_slot, &tbo, false, true);
    if (ok) {
        wwal_egl_sync_before_commit(yg->egl_dpy, &yg->sync_procs, NULL);
    }
    if (ve->daemon_state->render_engine.gpu_ctx) {
        gpu_compute_invalidate_state_cache(
            (gpu_compute_ctx_t *)ve->daemon_state->render_engine.gpu_ctx);
    }
    return ok;
}

#ifdef HAVE_FFMPEG_SWDEC
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>

#define SWDEC_GBM_RING_SIZE 4

typedef struct {
    void *lib_avcodec;
    void *lib_avutil;
    void *lib_swscale;

    const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID id);
    AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *codec);
    void (*p_avcodec_free_context)(AVCodecContext **avctx);
    int (*p_avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
    int (*p_avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt);
    int (*p_avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame);
    void (*p_avcodec_flush_buffers)(AVCodecContext *avctx);

    AVPacket *(*p_av_packet_alloc)(void);
    int (*p_av_new_packet)(AVPacket *pkt, int size);
    void (*p_av_packet_free)(AVPacket **pkt);
    void (*p_av_packet_unref)(AVPacket *pkt);
    AVFrame *(*p_av_frame_alloc)(void);
    void (*p_av_frame_free)(AVFrame **frame);
    void (*p_av_frame_unref)(AVFrame *frame);
    void *(*p_av_mallocz)(size_t size);

    struct SwsContext *(*p_sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                           int dstW, int dstH, enum AVPixelFormat dstFormat,
                                           int flags, SwsFilter *srcFilter, SwsFilter *dstFilter,
                                           const double *param);
    int (*p_sws_scale)(struct SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[],
                       int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
    void (*p_sws_freeContext)(struct SwsContext *swsContext);

    AVCodecContext *codec_ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws_ctx;
    uint8_t *rgba_buf;
    size_t rgba_buf_cap;
    struct gbm_bo *gbm_bos[SWDEC_GBM_RING_SIZE];
    struct wl_buffer *wl_bufs[SWDEC_GBM_RING_SIZE];
    int buf_busy[SWDEC_GBM_RING_SIZE];
    uint32_t ring_idx;
    int last_ready_ring_idx;
    bool has_gbm_ring;
    bool eof_flushing;
    uint32_t width;
    uint32_t height;
    int last_src_fmt;
    uint32_t err_streak;
} ffmpeg_swdec_t;

static pthread_once_t g_swdec_once = PTHREAD_ONCE_INIT;
static ffmpeg_swdec_t g_swdec_proto;
static bool g_swdec_proto_ok = false;

static void *dlopen_any(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        /* Pin FFmpeg DSOs with RTLD_NODELETE so transitive codec worker threads or TLS
         * destructors never execute unmapped pages after decoder teardown. */
        void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
        if (h) {
            return h;
        }
    }
    return NULL;
}

static void swdec_probe_once(void)
{
    static const char *const avcodec_names[] = {"libavcodec.so.63", "libavcodec.so.62",
                                                "libavcodec.so.61", "libavcodec.so.60",
                                                "libavcodec.so.59", "libavcodec.so"};
    static const char *const avutil_names[] = {"libavutil.so.61", "libavutil.so.60",
                                               "libavutil.so.59", "libavutil.so.58",
                                               "libavutil.so.57", "libavutil.so"};
    static const char *const swscale_names[] = {"libswscale.so.10", "libswscale.so.9",
                                                "libswscale.so.8",  "libswscale.so.7",
                                                "libswscale.so.6",  "libswscale.so"};

    memset(&g_swdec_proto, 0, sizeof(g_swdec_proto));
    ffmpeg_swdec_t *sw = &g_swdec_proto;
    sw->lib_avutil = dlopen_any(avutil_names, sizeof(avutil_names) / sizeof(avutil_names[0]));
    sw->lib_avcodec = dlopen_any(avcodec_names, sizeof(avcodec_names) / sizeof(avcodec_names[0]));
    sw->lib_swscale = dlopen_any(swscale_names, sizeof(swscale_names) / sizeof(swscale_names[0]));
    if (!sw->lib_avutil || !sw->lib_avcodec || !sw->lib_swscale) {
        memset(&g_swdec_proto, 0, sizeof(g_swdec_proto));
        return;
    }

#define RESOLVE_SYM(handle, field, sym)                                                            \
    do {                                                                                           \
        *(void **)(&sw->field) = dlsym(handle, sym);                                               \
        if (!sw->field) {                                                                          \
            memset(&g_swdec_proto, 0, sizeof(g_swdec_proto));                                      \
            return;                                                                                \
        }                                                                                          \
    } while (0)

    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_find_decoder, "avcodec_find_decoder");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_alloc_context3, "avcodec_alloc_context3");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_free_context, "avcodec_free_context");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_open2, "avcodec_open2");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_send_packet, "avcodec_send_packet");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_receive_frame, "avcodec_receive_frame");
    RESOLVE_SYM(sw->lib_avcodec, p_avcodec_flush_buffers, "avcodec_flush_buffers");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_alloc, "av_packet_alloc");
    RESOLVE_SYM(sw->lib_avcodec, p_av_new_packet, "av_new_packet");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_free, "av_packet_free");
    RESOLVE_SYM(sw->lib_avcodec, p_av_packet_unref, "av_packet_unref");

    RESOLVE_SYM(sw->lib_avutil, p_av_frame_alloc, "av_frame_alloc");
    RESOLVE_SYM(sw->lib_avutil, p_av_frame_free, "av_frame_free");
    RESOLVE_SYM(sw->lib_avutil, p_av_frame_unref, "av_frame_unref");
    RESOLVE_SYM(sw->lib_avutil, p_av_mallocz, "av_mallocz");

    RESOLVE_SYM(sw->lib_swscale, p_sws_getContext, "sws_getContext");
    RESOLVE_SYM(sw->lib_swscale, p_sws_scale, "sws_scale");
    RESOLVE_SYM(sw->lib_swscale, p_sws_freeContext, "sws_freeContext");

#undef RESOLVE_SYM
    g_swdec_proto_ok = true;
}

static bool swdec_load_symbols(ffmpeg_swdec_t *sw)
{
    pthread_once(&g_swdec_once, swdec_probe_once);
    if (!g_swdec_proto_ok) {
        return false;
    }
    *sw = g_swdec_proto;
    return true;
}

static void swdec_destroy(video_engine_t *ve)
{
    if (!ve || !ve->swdec_ctx)
        return;
    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
    for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
        if (sw->wl_bufs[i]) {
            wl_buffer_destroy(sw->wl_bufs[i]);
            sw->wl_bufs[i] = NULL;
        }
        if (sw->gbm_bos[i]) {
            gbm_bo_destroy(sw->gbm_bos[i]);
            sw->gbm_bos[i] = NULL;
        }
    }
    if (sw->sws_ctx && sw->p_sws_freeContext) {
        sw->p_sws_freeContext(sw->sws_ctx);
    }
    if (sw->frame && sw->p_av_frame_free) {
        sw->p_av_frame_free(&sw->frame);
    }
    if (sw->pkt && sw->p_av_packet_free) {
        sw->p_av_packet_free(&sw->pkt);
    }
    /* avcodec_free_context frees codec_ctx->extradata via av_freep internally. */
    if (sw->codec_ctx && sw->p_avcodec_free_context) {
        sw->p_avcodec_free_context(&sw->codec_ctx);
    }
    free(sw->rgba_buf);
    free(sw);
    ve->swdec_ctx = NULL;
    ve->using_swdec = false;
}

static void video_engine_reconcile_outputs(video_engine_t *ve);

static bool swdec_rebuild_surfaces(video_engine_t *ve, ffmpeg_swdec_t *sw, uint32_t w, uint32_t h)
{
    if (!ve || !sw || w == 0 || h == 0 || w > (uint32_t)(INT32_MAX / 4))
        return false;
    uint64_t need64 = (uint64_t)w * (uint64_t)h * 4ULL;
    if (need64 > (uint64_t)SIZE_MAX)
        return false;

    for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
        if (sw->wl_bufs[i]) {
            wl_buffer_destroy(sw->wl_bufs[i]);
            sw->wl_bufs[i] = NULL;
        }
        if (sw->gbm_bos[i]) {
            gbm_bo_destroy(sw->gbm_bos[i]);
            sw->gbm_bos[i] = NULL;
        }
        sw->buf_busy[i] = 0;
    }
    for (size_t i = 0; i < sizeof(ve->tracked_outs) / sizeof(ve->tracked_outs[0]); ++i) {
        ve->tracked_outs[i].sw_buf_idx = -1;
    }
    sw->has_gbm_ring = false;
    sw->last_ready_ring_idx = -1;
    sw->width = w;
    sw->height = h;

    if (ve->daemon_state && ve->daemon_state->dmabuf_ctx.gbm &&
        ve->daemon_state->dmabuf_ctx.dmabuf_proto && ve->daemon_state->dmabuf_ctx.map_supported) {
        bool ring_ok = true;
        for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
            sw->gbm_bos[i] =
                gbm_bo_create(ve->daemon_state->dmabuf_ctx.gbm, sw->width, sw->height,
                              DRM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
            if (!sw->gbm_bos[i]) {
                ring_ok = false;
                break;
            }
            int bo_fd = gbm_bo_get_fd(sw->gbm_bos[i]);
            if (bo_fd < 0) {
                ring_ok = false;
                break;
            }
            uint32_t stride = gbm_bo_get_stride(sw->gbm_bos[i]);
            uint64_t mod = DRM_FORMAT_MOD_LINEAR;
            struct zwp_linux_buffer_params_v1 *params =
                zwp_linux_dmabuf_v1_create_params(ve->daemon_state->dmabuf_ctx.dmabuf_proto);
            if (!params) {
                close(bo_fd);
                ring_ok = false;
                break;
            }
            zwp_linux_buffer_params_v1_add(params, bo_fd, 0, 0, stride, (uint32_t)(mod >> 32),
                                           (uint32_t)(mod & 0xFFFFFFFFULL));
            sw->wl_bufs[i] = zwp_linux_buffer_params_v1_create_immed(
                params, (int32_t)sw->width, (int32_t)sw->height, DRM_FORMAT_XRGB8888, 0);
            zwp_linux_buffer_params_v1_destroy(params);
            close(bo_fd);
            if (!sw->wl_bufs[i]) {
                ring_ok = false;
                break;
            }
            wl_buffer_add_listener(sw->wl_bufs[i], &g_video_buffer_listener, &sw->buf_busy[i]);
        }
        if (!ring_ok) {
            for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
                if (sw->wl_bufs[i]) {
                    wl_buffer_destroy(sw->wl_bufs[i]);
                    sw->wl_bufs[i] = NULL;
                }
                if (sw->gbm_bos[i]) {
                    gbm_bo_destroy(sw->gbm_bos[i]);
                    sw->gbm_bos[i] = NULL;
                }
                sw->buf_busy[i] = 0;
            }
        }
        sw->has_gbm_ring = ring_ok;
    }

    size_t need_bytes = (size_t)w * (size_t)h * 4ULL;
    if (!sw->has_gbm_ring || sw->rgba_buf) {
        if (!sw->rgba_buf || sw->rgba_buf_cap < need_bytes) {
            uint8_t *nb = (uint8_t *)realloc(sw->rgba_buf, need_bytes);
            if (!nb) {
                return sw->has_gbm_ring;
            }
            sw->rgba_buf = nb;
            sw->rgba_buf_cap = need_bytes;
        }
    }
    return true;
}

static bool swdec_init(video_engine_t *ve, const demuxer_t *demux)
{
    if (!ve || !demux || demux->width == 0 || demux->height == 0 ||
        demux->width > (uint32_t)INT_MAX || demux->height > (uint32_t)INT_MAX)
        return false;

    enum AVCodecID cid = AV_CODEC_ID_NONE;
    switch (demux->codec) {
    case WAYWAL_CODEC_H264:
        cid = AV_CODEC_ID_H264;
        break;
    case WAYWAL_CODEC_HEVC:
        cid = AV_CODEC_ID_HEVC;
        break;
    case WAYWAL_CODEC_VP9:
        cid = AV_CODEC_ID_VP9;
        break;
    case WAYWAL_CODEC_AV1:
        cid = AV_CODEC_ID_AV1;
        break;
    default:
        return false;
    }

    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)calloc(1, sizeof(ffmpeg_swdec_t));
    if (!sw)
        return false;

    if (!swdec_load_symbols(sw)) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    const AVCodec *codec = sw->p_avcodec_find_decoder(cid);
    if (!codec) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    sw->codec_ctx = sw->p_avcodec_alloc_context3(codec);
    sw->pkt = sw->p_av_packet_alloc();
    sw->frame = sw->p_av_frame_alloc();
    sw->width = demux->width;
    sw->height = demux->height;
    sw->last_src_fmt = -1;
    sw->last_ready_ring_idx = -1;

    if (!sw->codec_ctx || !sw->pkt || !sw->frame) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    sw->codec_ctx->width = (int)demux->width;
    sw->codec_ctx->height = (int)demux->height;
    sw->codec_ctx->thread_count = 2;

    if (demux->extradata && demux->extradata_size > 0) {
        if (demux->extradata_size > (size_t)INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
            ve->swdec_ctx = sw;
            swdec_destroy(ve);
            return false;
        }
        sw->codec_ctx->extradata =
            (uint8_t *)sw->p_av_mallocz(demux->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!sw->codec_ctx->extradata) {
            ve->swdec_ctx = sw;
            swdec_destroy(ve);
            return false;
        }
        memcpy(sw->codec_ctx->extradata, demux->extradata, demux->extradata_size);
        memset(sw->codec_ctx->extradata + demux->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        sw->codec_ctx->extradata_size = (int)demux->extradata_size;
    }

    if (sw->p_avcodec_open2(sw->codec_ctx, codec, NULL) < 0) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    if (!swdec_rebuild_surfaces(ve, sw, demux->width, demux->height) ||
        (!sw->has_gbm_ring && !sw->rgba_buf)) {
        ve->swdec_ctx = sw;
        swdec_destroy(ve);
        return false;
    }

    ve->swdec_ctx = sw;
    ve->using_swdec = true;
    WAYWAL_LOG_INFO(
        "Initialized FFmpeg software video decoder fallback (%ux%u, codec=%d, gbm_ring=%d)",
        sw->width, sw->height, (int)demux->codec, (int)sw->has_gbm_ring);
    return true;
}

static bool swdec_decode_and_present(video_engine_t *ve, int64_t *out_pts_us)
{
    if (!ve || !ve->swdec_ctx || !ve->demuxer)
        return false;
    ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;

    for (;;) {
        int recv_rc = sw->p_avcodec_receive_frame(sw->codec_ctx, sw->frame);
        if (recv_rc == 0) {
            if (sw->frame->width <= 0 || sw->frame->height <= 0) {
                sw->p_av_frame_unref(sw->frame);
                continue;
            }
            uint32_t fw = (uint32_t)sw->frame->width;
            uint32_t fh = (uint32_t)sw->frame->height;
            bool dims_changed = (fw != sw->width || fh != sw->height);
            if (dims_changed) {
                if (!swdec_rebuild_surfaces(ve, sw, fw, fh)) {
                    sw->p_av_frame_unref(sw->frame);
                    video_engine_stop(ve);
                    return false;
                }
            }
            if (dims_changed || sw->frame->format != sw->last_src_fmt || !sw->sws_ctx) {
                if (sw->sws_ctx) {
                    sw->p_sws_freeContext(sw->sws_ctx);
                }
                sw->sws_ctx = sw->p_sws_getContext((int)sw->width, (int)sw->height,
                                                   (enum AVPixelFormat)sw->frame->format,
                                                   (int)sw->width, (int)sw->height, AV_PIX_FMT_BGRA,
                                                   SWS_BILINEAR, NULL, NULL, NULL);
                sw->last_src_fmt = sw->frame->format;
            }
            if (sw->sws_ctx) {
                sw->err_streak = 0;
                sw->last_ready_ring_idx = -1;
                bool mapped_gbm = false;
                video_engine_reconcile_outputs(ve);
                if (sw->has_gbm_ring) {
                    int chosen = -1;
                    for (int i = 0; i < SWDEC_GBM_RING_SIZE; ++i) {
                        int cand = (int)((sw->ring_idx + (uint32_t)i) % SWDEC_GBM_RING_SIZE);
                        if (sw->buf_busy[cand] == 0) {
                            chosen = cand;
                            break;
                        }
                    }
                    if (chosen >= 0) {
                        sw->ring_idx = (uint32_t)(chosen + 1) % SWDEC_GBM_RING_SIZE;
                        uint32_t map_stride = 0;
                        void *map_data = NULL;
                        void *ptr = gbm_bo_map(sw->gbm_bos[chosen], 0, 0, sw->width, sw->height,
                                               GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
                        if (ptr && map_data && map_stride >= sw->width * 4U &&
                            map_stride <= (uint32_t)INT_MAX) {
                            uint8_t *dst_planes[4] = {(uint8_t *)ptr, NULL, NULL, NULL};
                            int dst_strides[4] = {(int)map_stride, 0, 0, 0};
                            int sc_rc = sw->p_sws_scale(
                                sw->sws_ctx, (const uint8_t *const *)sw->frame->data,
                                sw->frame->linesize, 0, (int)sw->height, dst_planes, dst_strides);
                            gbm_bo_unmap(sw->gbm_bos[chosen], map_data);
                            if (sc_rc > 0) {
                                sw->last_ready_ring_idx = chosen;
                                mapped_gbm = true;
                            }
                        } else if (ptr && map_data) {
                            gbm_bo_unmap(sw->gbm_bos[chosen], map_data);
                        }
                    }
                }
                if (!mapped_gbm) {
                    uint64_t need64 = (uint64_t)sw->width * (uint64_t)sw->height * 4ULL;
                    if (sw->width > (uint32_t)(INT32_MAX / 4) || need64 > (uint64_t)SIZE_MAX) {
                        sw->p_av_frame_unref(sw->frame);
                        return false;
                    }
                    size_t need = (size_t)need64;
                    if (!sw->rgba_buf || sw->rgba_buf_cap < need) {
                        uint8_t *nb = (uint8_t *)realloc(sw->rgba_buf, need);
                        if (!nb) {
                            sw->p_av_frame_unref(sw->frame);
                            return false;
                        }
                        sw->rgba_buf = nb;
                        sw->rgba_buf_cap = need;
                    }
                    uint8_t *dst_planes[4] = {sw->rgba_buf, NULL, NULL, NULL};
                    int dst_strides[4] = {(int)(sw->width * 4U), 0, 0, 0};
                    if (sw->p_sws_scale(sw->sws_ctx, (const uint8_t *const *)sw->frame->data,
                                        sw->frame->linesize, 0, (int)sw->height, dst_planes,
                                        dst_strides) <= 0) {
                        sw->p_av_frame_unref(sw->frame);
                        return false;
                    }
                }
                if (out_pts_us) {
                    *out_pts_us = sw->frame->pts;
                }
                sw->p_av_frame_unref(sw->frame);
                return true;
            }
            sw->p_av_frame_unref(sw->frame);
        } else if (recv_rc != AVERROR(EAGAIN) && recv_rc != AVERROR_EOF) {
            sw->err_streak++;
            if (sw->err_streak >= 16U) {
                WAYWAL_LOG_ERR("FFmpeg swdec receive_frame failed persistently (%d), stopping",
                               recv_rc);
                video_engine_stop(ve);
                return false;
            }
        } else if (recv_rc == AVERROR(EAGAIN) && sw->eof_flushing) {
            sw->eof_flushing = false;
            if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
                ve->current_loop++;
                video_engine_stop(ve);
                return false;
            }
            ve->current_loop++;
            ve->demuxer->seek_us(ve->demuxer, 0);
            sw->p_avcodec_flush_buffers(sw->codec_ctx);
        }

        demux_packet_t dpkt;
        if (!ve->demuxer->read_packet(ve->demuxer, &dpkt)) {
            if (!sw->eof_flushing) {
                sw->eof_flushing = true;
                (void)sw->p_avcodec_send_packet(sw->codec_ctx, NULL);
                continue;
            }
            sw->eof_flushing = false;
            if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
                ve->current_loop++;
                video_engine_stop(ve);
                return false;
            }
            ve->current_loop++;
            ve->demuxer->seek_us(ve->demuxer, 0);
            sw->p_avcodec_flush_buffers(sw->codec_ctx);
            if (!ve->demuxer->read_packet(ve->demuxer, &dpkt)) {
                video_engine_stop(ve);
                return false;
            }
        }

        if (dpkt.size > (size_t)INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
            WAYWAL_LOG_ERR("FFmpeg swdec packet size overflow (%zu)", dpkt.size);
            video_engine_stop(ve);
            return false;
        }
        sw->p_av_packet_unref(sw->pkt);
        if (sw->p_av_new_packet(sw->pkt, (int)dpkt.size) != 0) {
            return false;
        }
        if (dpkt.size > 0 && dpkt.data) {
            memcpy(sw->pkt->data, dpkt.data, dpkt.size);
        }
        memset(sw->pkt->data + dpkt.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        sw->pkt->pts = dpkt.pts_us;
        sw->pkt->dts = dpkt.dts_us;
        sw->pkt->flags = dpkt.is_keyframe ? AV_PKT_FLAG_KEY : 0;

        int send_rc = sw->p_avcodec_send_packet(sw->codec_ctx, sw->pkt);
        sw->p_av_packet_unref(sw->pkt);
        if (send_rc < 0 && send_rc != AVERROR(EAGAIN) && send_rc != AVERROR_EOF) {
            sw->err_streak++;
            if (sw->err_streak >= 16U) {
                WAYWAL_LOG_ERR("FFmpeg swdec avcodec_send_packet failed persistently (%d)",
                               send_rc);
                video_engine_stop(ve);
                return false;
            }
        }
    }
}
#endif

static bool is_output_active_for_video(const video_engine_t *ve, const output_node_t *out)
{
    if (!ve || !out)
        return false;
    if (out->video_eng)
        return (out->video_eng == ve);
    if (ve->daemon_state && ve != &ve->daemon_state->video_engine)
        return (ve->target_output != NULL && out == ve->target_output);
    if (out->shader_active || out->anim_active)
        return false;
    if (ve->target_output)
        return (out == ve->target_output);
    if (ve->has_target_filter)
        return out->video_targeted;
    return true;
}

static void video_engine_reconcile_outputs(video_engine_t *ve)
{
    if (!ve || !ve->daemon_state)
        return;
    for (size_t i = 0; i < sizeof(ve->tracked_outs) / sizeof(ve->tracked_outs[0]); ++i) {
        if (!ve->tracked_outs[i].active)
            continue;
        bool still_valid = false;
        for (output_node_t *cur = ve->daemon_state->outputs; cur != NULL; cur = cur->next) {
            if (cur == ve->tracked_outs[i].out && cur->surface != NULL &&
                cur->surface == ve->tracked_outs[i].surface &&
                is_output_active_for_video(ve, cur)) {
                still_valid = true;
                break;
            }
        }
        if (!still_valid) {
            int32_t b_idx = ve->tracked_outs[i].buf_idx;
            if (b_idx >= 0 && b_idx < WAYWAL_VA_SURFACE_POOL_SIZE &&
                ve->video_buffer_in_use[b_idx] > 0) {
                ve->video_buffer_in_use[b_idx]--;
            }
#ifdef HAVE_FFMPEG_SWDEC
            int32_t sw_idx = ve->tracked_outs[i].sw_buf_idx;
            if (sw_idx >= 0 && sw_idx < SWDEC_GBM_RING_SIZE && ve->using_swdec && ve->swdec_ctx) {
                ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
                if (sw->buf_busy[sw_idx] > 0) {
                    sw->buf_busy[sw_idx]--;
                }
            }
#endif
            memset(&ve->tracked_outs[i], 0, sizeof(ve->tracked_outs[i]));
            ve->tracked_outs[i].buf_idx = -1;
            ve->tracked_outs[i].sw_buf_idx = -1;
        }
    }
}

static void video_engine_track_attach(video_engine_t *ve, output_node_t *out, int32_t va_buf_idx,
                                      int32_t sw_buf_idx)
{
    if (!ve || !out)
        return;
    ssize_t empty_slot = -1;
    for (size_t i = 0; i < sizeof(ve->tracked_outs) / sizeof(ve->tracked_outs[0]); ++i) {
        if (ve->tracked_outs[i].active && ve->tracked_outs[i].out == out) {
            ve->tracked_outs[i].surface = out->surface;
            ve->tracked_outs[i].buf_idx = va_buf_idx;
            ve->tracked_outs[i].sw_buf_idx = sw_buf_idx;
            return;
        }
        if (!ve->tracked_outs[i].active && empty_slot < 0) {
            empty_slot = (ssize_t)i;
        }
    }
    if (empty_slot >= 0) {
        ve->tracked_outs[empty_slot].out = out;
        ve->tracked_outs[empty_slot].surface = out->surface;
        ve->tracked_outs[empty_slot].buf_idx = va_buf_idx;
        ve->tracked_outs[empty_slot].sw_buf_idx = sw_buf_idx;
        ve->tracked_outs[empty_slot].active = true;
    }
}

static void video_engine_clear_tracked_outputs(video_engine_t *ve)
{
    if (!ve)
        return;
    for (size_t i = 0; i < sizeof(ve->tracked_outs) / sizeof(ve->tracked_outs[0]); ++i) {
        memset(&ve->tracked_outs[i], 0, sizeof(ve->tracked_outs[i]));
        ve->tracked_outs[i].buf_idx = -1;
        ve->tracked_outs[i].sw_buf_idx = -1;
    }
}

static bool video_engine_use_gpu_yuv(const video_engine_t *ve)
{
    if (!ve || !ve->decoder.initialized || ve->decoder.has_vpp || !ve->daemon_state)
        return false;
    if (!ve->daemon_state->render_engine.gpu_compute_active ||
        !ve->daemon_state->dmabuf_ctx.available || !ve->daemon_state->dmabuf_ctx.dmabuf_proto) {
        return false;
    }
    if (ve->decoder.is_nvidia_vaapi)
        return true;
    bool is_10bit = (ve->decoder.fourcc == VA_FOURCC_P010);
    bool comp_yuv =
        is_10bit ? ve->daemon_state->dmabuf_ctx.has_p010 : ve->daemon_state->dmabuf_ctx.has_nv12;
    return !comp_yuv;
}

static output_node_t *select_pll_reference_output(const video_engine_t *ve)
{
    if (!ve || !ve->daemon_state)
        return NULL;
    output_node_t *best_unpaused = NULL;
    output_node_t *best_any = NULL;
    for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
        if (!is_output_active_for_video(ve, out) || !out->surface)
            continue;
        if (!best_any || out->refresh_mhz > best_any->refresh_mhz) {
            best_any = out;
        }
        bool paused = ve->daemon_state->power_gov && waywal_power_governor_should_pause_output(
                                                         ve->daemon_state->power_gov, out->name);
        if (!paused && (!best_unpaused || out->refresh_mhz > best_unpaused->refresh_mhz)) {
            best_unpaused = out;
        }
    }
    return best_unpaused ? best_unpaused : best_any;
}

static int64_t get_output_refresh_period_ns(const output_node_t *out)
{
    if (!out)
        return 0;
    if (out->pres_sync.pll.frame_seq > 0 && out->pres_sync.pll.period_est_ns > 0)
        return out->pres_sync.pll.period_est_ns;
    if (out->refresh_mhz > 0)
        return 1000000000000LL / (int64_t)out->refresh_mhz;
    return out->pres_sync.pll.period_est_ns;
}

static int64_t prepare_output_fifo_and_feedback(output_node_t *out,
                                                const output_node_t *pll_ref_out,
                                                int64_t frame_interval_ns)
{
    if (!out)
        return 0;

    bool prev_still_pending = out->pres_sync.pending_feedback;
    if (out->pres_sync.wp_pres && (out == pll_ref_out || out->fifo != NULL)) {
        presentation_sync_request(&out->pres_sync);
    }

    int64_t out_period_ns = get_output_refresh_period_ns(out);
    if (out->fifo && out->pres_sync.wp_pres && out_period_ns > 0 &&
        frame_interval_ns + (frame_interval_ns / 16) >= out_period_ns) {
        /* Omit wait_barrier when the previous commit is still awaiting presentation so
         * in-flight FIFO depth never exceeds one frame and VBlank slips recover cleanly. */
        if (out->fifo_barrier_pending && !prev_still_pending) {
            waywal_fifo_wait_barrier(out->fifo);
        }
        waywal_fifo_set_barrier(out->fifo);
        out->fifo_barrier_pending = true;
    } else {
        out->fifo_barrier_pending = false;
    }

    return out_period_ns;
}

static struct wl_buffer *ensure_shared_video_buffer(video_engine_t *ve, VASurfaceID export_surf,
                                                    uint32_t buf_idx)
{
    if (!ve || buf_idx >= WAYWAL_VA_SURFACE_POOL_SIZE || export_surf == VA_INVALID_SURFACE)
        return NULL;

    if (ve->video_wl_buffers[buf_idx]) {
        return ve->video_wl_buffers[buf_idx];
    }

    struct zwp_linux_dmabuf_v1 *dmabuf_proto = ve->daemon_state->dmabuf_ctx.dmabuf_proto;
    if (!dmabuf_proto)
        return NULL;

    vaapi_prime_frame_t prime_frame;
    if (!vaapi_decoder_export_prime(&ve->decoder, export_surf, &prime_frame)) {
        WAYWAL_LOG_ERR("vaapi_decoder_export_prime failed for surface %u", export_surf);
        return NULL;
    }

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf_proto);
    if (!params) {
        vaapi_prime_frame_close(&prime_frame);
        return NULL;
    }

    for (uint32_t p = 0; p < prime_frame.num_planes; ++p) {
        zwp_linux_buffer_params_v1_add(params, prime_frame.fds[p], p, prime_frame.offsets[p],
                                       prime_frame.strides[p],
                                       (uint32_t)(prime_frame.modifiers[p] >> 32),
                                       (uint32_t)(prime_frame.modifiers[p] & 0xFFFFFFFF));
    }

    struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(
        params, prime_frame.width, prime_frame.height, prime_frame.drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    vaapi_prime_frame_close(&prime_frame);

    if (wl_buf) {
        WAYWAL_LOG_INFO(
            "Created shared video wl_buffer %p for surface %u (idx %u, %ux%u, fmt 0x%08x)",
            (void *)wl_buf, export_surf, buf_idx, prime_frame.width, prime_frame.height,
            prime_frame.drm_format);
        wl_buffer_add_listener(wl_buf, &g_video_buffer_listener, &ve->video_buffer_in_use[buf_idx]);
        ve->video_wl_buffers[buf_idx] = wl_buf;
        ve->video_buffer_in_use[buf_idx] = 0;
    } else {
        WAYWAL_LOG_ERR("create_immed returned NULL for shared video surface %u", export_surf);
    }
    return wl_buf;
}

static void configure_output_video_viewport(output_node_t *out, uint32_t vid_w, uint32_t vid_h);

static bool video_engine_present_gpu_yuv_output(video_engine_t *ve, video_yuv_gl_ctx_t *yg,
                                                const video_yuv_imported_frame_t *imp,
                                                output_node_t *out, int64_t frame_interval_ns,
                                                const output_node_t *pll_ref_out, bool reset_epoch,
                                                int64_t this_frame_pres_ns, int64_t now_ns,
                                                bool *out_used_commit_timer)
{
    if (!ve || !yg || !imp || !out || !out->surface || !ve->daemon_state || out->width <= 0 ||
        out->height <= 0)
        return false;

    if (!layer_shell_ensure_dmabuf_ring(ve->daemon_state, out, out->width, out->height)) {
        return false;
    }
    dmabuf_bo_t *dst_bo = dmabuf_ring_acquire(&out->dmabuf_ring);
    if (!dst_bo || !dst_bo->wl_buffer) {
        if (dst_bo)
            dst_bo->in_use = false;
        return false;
    }

    layer_shell_compute_span_bounds(ve->daemon_state, out);
    float gx = out->has_crop_gravity ? out->crop_gravity_x : 0.5f;
    float gy = out->has_crop_gravity ? out->crop_gravity_y : 0.5f;
    uint32_t clear_col = ((uint32_t)out->fill_color.a << 24) | ((uint32_t)out->fill_color.r << 16) |
                         ((uint32_t)out->fill_color.g << 8) | (uint32_t)out->fill_color.b;

    if (!video_yuv_render_imported_to_bo(ve, yg, imp, dst_bo, out->scaling_mode, clear_col,
                                         out->span_off_x, out->span_off_y, out->span_total_w,
                                         out->span_total_h, gx, gy)) {
        dst_bo->in_use = false;
        return false;
    }

    int32_t dst_w = out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
    int32_t dst_h = out->logical_height > 0 ? (int32_t)out->logical_height : out->height;
    if (out->viewport) {
        wp_viewport_set_destination(out->viewport, dst_w, dst_h);
        wp_viewport_set_source(out->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        out->vp_dst_w = dst_w;
        out->vp_dst_h = dst_h;
        out->vp_src_w = dst_bo->width;
        out->vp_src_h = dst_bo->height;
        out->vp_scaling_mode = out->scaling_mode;
        out->vp_configured = true;
    }

    int64_t out_period_ns = prepare_output_fifo_and_feedback(out, pll_ref_out, frame_interval_ns);
    if (out->commit_timer && ve->daemon_state->presentation &&
        ve->daemon_state->has_presentation_clock_id) {
        if (out_used_commit_timer)
            *out_used_commit_timer = true;
        if (!reset_epoch) {
            int64_t slop_ns = (out_period_ns > 0) ? (out_period_ns / 4) : 1500000LL;
            if (slop_ns < 500000LL)
                slop_ns = 500000LL;
            if (slop_ns > 4000000LL)
                slop_ns = 4000000LL;
            if (slop_ns > frame_interval_ns / 4)
                slop_ns = frame_interval_ns / 4;

            int64_t not_before_mono_ns = this_frame_pres_ns - slop_ns;
            if (not_before_mono_ns > now_ns) {
                int64_t hw_target_ns = not_before_mono_ns;
                if (ve->daemon_state->presentation_clock_id != CLOCK_MONOTONIC) {
                    struct timespec pres_ts;
                    if (clock_gettime(ve->daemon_state->presentation_clock_id, &pres_ts) == 0) {
                        int64_t pres_now_ns =
                            (int64_t)pres_ts.tv_sec * 1000000000LL + pres_ts.tv_nsec;
                        hw_target_ns = pres_now_ns + (not_before_mono_ns - now_ns);
                    }
                }
                if (hw_target_ns > 0) {
                    waywal_commit_timer_set_timestamp_ns(out->commit_timer, hw_target_ns);
                }
            }
        }
    }

    wl_surface_attach(out->surface, dst_bo->wl_buffer, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, (int32_t)dst_bo->width, (int32_t)dst_bo->height);
    wl_surface_commit(out->surface);
    out->has_image = true;
    out->last_presented_bo = dst_bo;
    out->last_committed_ring_idx = (int)(dst_bo - &out->dmabuf_ring.buffers[0]);
    return true;
}

static bool video_engine_redraw_output(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !out || !out->surface || ve->state == WAYWAL_VIDEO_STATE_STOPPED ||
        !is_output_active_for_video(ve, out)) {
        return false;
    }

    video_engine_reconcile_outputs(ve);

#ifdef HAVE_FFMPEG_SWDEC
    if (ve->using_swdec && ve->swdec_ctx) {
        ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
        if (sw->has_gbm_ring && sw->last_ready_ring_idx >= 0 &&
            sw->wl_bufs[sw->last_ready_ring_idx]) {
            int r_idx = sw->last_ready_ring_idx;
            configure_output_video_viewport(out, sw->width, sw->height);
            bool prev_still_pending = out->pres_sync.pending_feedback;
            if (out->pres_sync.wp_pres) {
                presentation_sync_request(&out->pres_sync);
            }
            if (out->fifo && out->pres_sync.wp_pres) {
                if (out->fifo_barrier_pending && !prev_still_pending) {
                    waywal_fifo_wait_barrier(out->fifo);
                }
                waywal_fifo_set_barrier(out->fifo);
                out->fifo_barrier_pending = true;
            } else {
                out->fifo_barrier_pending = false;
            }
            wl_surface_attach(out->surface, sw->wl_bufs[r_idx], 0, 0);
            wl_surface_damage_buffer(out->surface, 0, 0, (int32_t)sw->width, (int32_t)sw->height);
            wl_surface_commit(out->surface);
            out->has_image = true;
            sw->buf_busy[r_idx]++;
            video_engine_track_attach(ve, out, -1, r_idx);
            if (ve->daemon_state && ve->daemon_state->display) {
                wl_display_flush(ve->daemon_state->display);
            }
            return true;
        }
        if (sw->rgba_buf && sw->width > 0 && sw->height > 0 && ve->daemon_state) {
            output_node_render_image(ve->daemon_state, out, sw->rgba_buf, sw->width, sw->height);
            return true;
        }
        return false;
    }
#endif

    if (video_engine_use_gpu_yuv(ve) && ve->last_export_surf != VA_INVALID_SURFACE) {
        vaapi_prime_frame_t pf;
        if (vaapi_decoder_export_prime(&ve->decoder, ve->last_export_surf, &pf)) {
            video_yuv_gl_ctx_t *yg = video_yuv_ensure_gl_ctx(ve);
            video_yuv_imported_frame_t imp;
            bool ok = false;
            if (yg && video_yuv_import_prime_frame(yg, &pf, &imp)) {
                ok = video_engine_present_gpu_yuv_output(ve, yg, &imp, out, 33333000LL, out, true,
                                                         0, 0, NULL);
                video_yuv_release_imported_frame(yg, &imp);
            }
            vaapi_prime_frame_close(&pf);
            if (ok) {
                if (ve->daemon_state && ve->daemon_state->display) {
                    wl_display_flush(ve->daemon_state->display);
                }
                return true;
            }
        }
    }

    struct wl_buffer *wl_buf = NULL;
    uint32_t buf_idx = 0;
    if (ve->last_presented_buf_idx >= 0 &&
        (uint32_t)ve->last_presented_buf_idx < WAYWAL_VA_SURFACE_POOL_SIZE &&
        ve->video_wl_buffers[ve->last_presented_buf_idx]) {
        buf_idx = (uint32_t)ve->last_presented_buf_idx;
        wl_buf = ve->video_wl_buffers[buf_idx];
    }
    if (wl_buf && ve->decoder.width > 0 && ve->decoder.height > 0) {
        configure_output_video_viewport(out, ve->decoder.width, ve->decoder.height);
        bool prev_still_pending = out->pres_sync.pending_feedback;
        if (out->pres_sync.wp_pres) {
            presentation_sync_request(&out->pres_sync);
        }
        if (out->fifo && out->pres_sync.wp_pres) {
            if (out->fifo_barrier_pending && !prev_still_pending) {
                waywal_fifo_wait_barrier(out->fifo);
            }
            waywal_fifo_set_barrier(out->fifo);
            out->fifo_barrier_pending = true;
        } else {
            out->fifo_barrier_pending = false;
        }
        wl_surface_attach(out->surface, wl_buf, 0, 0);
        wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(out->surface);
        out->has_image = true;
        ve->video_buffer_in_use[buf_idx]++;
        video_engine_track_attach(ve, out, (int32_t)buf_idx, -1);
        if (ve->daemon_state && ve->daemon_state->display) {
            wl_display_flush(ve->daemon_state->display);
        }
        return true;
    }

    if (ve->state == WAYWAL_VIDEO_STATE_PLAYING && ve->timer_fd >= 0) {
        struct itimerspec its = {.it_interval = {0, 0},
                                 .it_value = {.tv_sec = 0, .tv_nsec = 1000000L}};
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
    }
    return false;
}

bool video_engine_init(video_engine_t *ve, daemon_state_t *state)
{
    if (!ve || !state)
        return false;
    memset(ve, 0, sizeof(*ve));
    video_engine_clear_tracked_outputs(ve);
    ve->daemon_state = state;
    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->has_decoded_any = false;
    ve->playback_speed = 1.0f;
    ve->loop_count = 0; /* infinite loop */
    ve->seamless_looping = true;
    ve->last_presented_buf_idx = -1;
    ve->last_export_surf = VA_INVALID_SURFACE;
    ve->redraw_output = video_engine_redraw_output;

    ve->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ve->timer_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create video presentation timerfd: %s", strerror(errno));
        return false;
    }

    /* Register timerfd in daemon epoll event loop */
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = ve->timer_fd};
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, ve->timer_fd, &ev) < 0) {
        WAYWAL_LOG_ERR("epoll_ctl ADD failed for video timerfd: %s", strerror(errno));
        close(ve->timer_fd);
        ve->timer_fd = -1;
        return false;
    }

    WAYWAL_LOG_INFO("Hardware video engine initialized (timer_fd: %d)", ve->timer_fd);
    return true;
}

static void video_engine_release_wl_buffers(video_engine_t *ve);

void video_engine_destroy(video_engine_t *ve)
{
    if (!ve)
        return;
    video_engine_stop(ve);

    if (ve->decoder.initialized) {
        video_engine_release_wl_buffers(ve);
        vaapi_decoder_destroy(&ve->decoder);
    }

    video_yuv_gl_destroy(ve);

    if (ve->timer_fd >= 0) {
        if (ve->daemon_state && ve->daemon_state->epoll_fd >= 0) {
            epoll_ctl(ve->daemon_state->epoll_fd, EPOLL_CTL_DEL, ve->timer_fd, NULL);
        }
        close(ve->timer_fd);
        ve->timer_fd = -1;
    }
}

static void reclaim_output_shm_for_video(output_node_t *out)
{
    if (!out)
        return;
    out->transition_active = false;
    if (out->shm_old_data) {
        free(out->shm_old_data);
        out->shm_old_data = NULL;
    }
    if (out->shm_new_data) {
        free(out->shm_new_data);
        out->shm_new_data = NULL;
    }
    out->shm_buffer_cap = 0;

    if (out->shm_data && out->shm_size > 0) {
        madvise(out->shm_data, out->shm_size, MADV_REMOVE);
        madvise(out->shm_data, out->shm_size, MADV_DONTNEED);
    }

    daemon_state_t *state = out->state;
    if (out->has_source_old_bo) {
        if (out->shader_eng) {
            waywal_shader_engine_forget_bo(out->shader_eng, &out->source_old_bo);
        }
        if (state && state->shader_eng && state->shader_eng != out->shader_eng) {
            waywal_shader_engine_forget_bo(state->shader_eng, &out->source_old_bo);
        }
        if (out->we_scene) {
            waywal_we_scene_forget_bo(out->we_scene, &out->source_old_bo);
        }
        if (state && state->we_scene && state->we_scene != out->we_scene) {
            waywal_we_scene_forget_bo(state->we_scene, &out->source_old_bo);
        }
        if (state) {
            render_engine_release_bo(&state->render_engine, &out->source_old_bo);
        }
        dmabuf_bo_free(&out->source_old_bo);
        out->has_source_old_bo = false;
        if (out->last_presented_bo == &out->source_old_bo) {
            out->last_presented_bo = NULL;
        }
    }
    if (out->has_source_new_bo) {
        if (out->shader_eng) {
            waywal_shader_engine_forget_bo(out->shader_eng, &out->source_new_bo);
        }
        if (state && state->shader_eng && state->shader_eng != out->shader_eng) {
            waywal_shader_engine_forget_bo(state->shader_eng, &out->source_new_bo);
        }
        if (out->we_scene) {
            waywal_we_scene_forget_bo(out->we_scene, &out->source_new_bo);
        }
        if (state && state->we_scene && state->we_scene != out->we_scene) {
            waywal_we_scene_forget_bo(state->we_scene, &out->source_new_bo);
        }
        if (state) {
            render_engine_release_bo(&state->render_engine, &out->source_new_bo);
        }
        dmabuf_bo_free(&out->source_new_bo);
        out->has_source_new_bo = false;
        if (out->last_presented_bo == &out->source_new_bo) {
            out->last_presented_bo = NULL;
        }
    }
    if (out->use_dmabuf && out->dmabuf_ring.initialized) {
        for (size_t i = 0; i < WAYWAL_DMABUF_RING_SIZE; ++i) {
            dmabuf_bo_t *bo = &out->dmabuf_ring.buffers[i];
            if ((bo->gbm_bo != NULL || bo->wl_buffer != NULL) && !bo->in_use) {
                layer_shell_release_output_bo_refs(state, out, bo);
                dmabuf_bo_free(bo);
            }
        }
    }
    out->last_presented_bo = NULL;
    out->last_committed_ring_idx = -1;
    if (state) {
        render_engine_release_upload_cache(&state->render_engine);
    }
}

static void configure_output_video_viewport(output_node_t *out, uint32_t vid_w, uint32_t vid_h)
{
    if (!out || !out->viewport || out->width <= 0 || out->height <= 0 || vid_w == 0 || vid_h == 0)
        return;

    int32_t dst_w = out->logical_width > 0 ? (int32_t)out->logical_width : out->width;
    int32_t dst_h = out->logical_height > 0 ? (int32_t)out->logical_height : out->height;

    if (out->scaling_mode == WAYWAL_SCALING_SPAN && out->state) {
        layer_shell_compute_span_bounds(out->state, out);
    }

    if (out->vp_configured && out->vp_dst_w == dst_w && out->vp_dst_h == dst_h &&
        out->vp_src_w == vid_w && out->vp_src_h == vid_h &&
        out->vp_scaling_mode == out->scaling_mode && out->scaling_mode != WAYWAL_SCALING_SPAN) {
        return;
    }

    wp_viewport_set_destination(out->viewport, dst_w, dst_h);

    if (out->scaling_mode == WAYWAL_SCALING_SPAN) {
        double tot_w = (out->span_total_w > 0) ? (double)out->span_total_w : (double)out->width;
        double tot_h = (out->span_total_h > 0) ? (double)out->span_total_h : (double)out->height;
        double r_src = (double)vid_w / (double)vid_h;
        double r_dst = tot_w / tot_h;
        double crop_w, crop_h, off_x, off_y;

        if (r_src > r_dst) {
            crop_h = (double)vid_h;
            crop_w = crop_h * r_dst;
            off_x = ((double)vid_w - crop_w) * 0.5;
            off_y = 0.0;
        } else {
            crop_w = (double)vid_w;
            crop_h = crop_w / r_dst;
            off_x = 0.0;
            off_y = ((double)vid_h - crop_h) * 0.5;
        }

        double sub_off_x = off_x + ((double)out->span_off_x / tot_w) * crop_w;
        double sub_off_y = off_y + ((double)out->span_off_y / tot_h) * crop_h;
        double sub_crop_w = ((double)out->width / tot_w) * crop_w;
        double sub_crop_h = ((double)out->height / tot_h) * crop_h;
        if (sub_crop_w < 1.0)
            sub_crop_w = 1.0;
        if (sub_crop_h < 1.0)
            sub_crop_h = 1.0;
        if (sub_off_x < 0.0)
            sub_off_x = 0.0;
        if (sub_off_y < 0.0)
            sub_off_y = 0.0;
        if (sub_off_x + sub_crop_w > (double)vid_w)
            sub_crop_w = (double)vid_w - sub_off_x;
        if (sub_off_y + sub_crop_h > (double)vid_h)
            sub_crop_h = (double)vid_h - sub_off_y;

        wp_viewport_set_source(out->viewport, wl_fixed_from_double(sub_off_x),
                               wl_fixed_from_double(sub_off_y), wl_fixed_from_double(sub_crop_w),
                               wl_fixed_from_double(sub_crop_h));
    } else if (out->scaling_mode == WAYWAL_SCALING_FILL) {
        double r_src = (double)vid_w / (double)vid_h;
        double r_dst = (double)out->width / (double)out->height;
        double crop_w = (double)vid_w;
        double crop_h = (double)vid_h;
        double off_x = 0.0;
        double off_y = 0.0;

        if (r_src > r_dst + 1e-4) {
            crop_h = (double)vid_h;
            crop_w = crop_h * r_dst;
            off_x = ((double)vid_w - crop_w) * 0.5;
        } else if (r_dst > r_src + 1e-4) {
            crop_w = (double)vid_w;
            crop_h = crop_w / r_dst;
            off_y = ((double)vid_h - crop_h) * 0.5;
        }

        if (fabs(crop_w - (double)vid_w) < 0.5 && fabs(crop_h - (double)vid_h) < 0.5) {
            wp_viewport_set_source(out->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                                   wl_fixed_from_int((int)vid_w), wl_fixed_from_int((int)vid_h));
        } else {
            wp_viewport_set_source(out->viewport, wl_fixed_from_double(off_x),
                                   wl_fixed_from_double(off_y), wl_fixed_from_double(crop_w),
                                   wl_fixed_from_double(crop_h));
        }
    } else {
        wp_viewport_set_source(out->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
                               wl_fixed_from_int((int)vid_w), wl_fixed_from_int((int)vid_h));
    }

    out->vp_dst_w = dst_w;
    out->vp_dst_h = dst_h;
    out->vp_src_w = vid_w;
    out->vp_src_h = vid_h;
    out->vp_scaling_mode = out->scaling_mode;
    out->vp_configured = true;
}

static void video_engine_release_wl_buffers(video_engine_t *ve)
{
    if (!ve)
        return;
    bool destroyed_any = false;
    for (uint32_t i = 0; i < WAYWAL_VA_SURFACE_POOL_SIZE; ++i) {
        if (ve->video_wl_buffers[i]) {
            wl_buffer_destroy(ve->video_wl_buffers[i]);
            ve->video_wl_buffers[i] = NULL;
            destroyed_any = true;
        }
        ve->video_buffer_in_use[i] = 0;
    }
    video_engine_clear_tracked_outputs(ve);
    ve->last_presented_buf_idx = -1;
    if (destroyed_any && ve->daemon_state && ve->daemon_state->display) {
        wl_display_flush(ve->daemon_state->display);
    }
}

static void video_engine_prepare_reload(video_engine_t *ve)
{
    if (!ve)
        return;
    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
    }
    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->has_decoded_any = false;
    ve->hw_decode_err_count = 0;
    ve->playback_epoch_ns = 0;
    ve->last_presented_buf_idx = -1;
    ve->last_export_surf = VA_INVALID_SURFACE;
    if (ve->daemon_state) {
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            if (is_output_active_for_video(ve, out)) {
                out->fifo_barrier_pending = false;
            }
        }
    }
    video_engine_release_wl_buffers(ve);
#ifdef HAVE_FFMPEG_SWDEC
    swdec_destroy(ve);
#endif
    if (ve->demuxer) {
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
    }
}

static bool can_reuse_vaapi_decoder(const vaapi_decoder_t *dec, const demuxer_t *demux,
                                    bool allow_direct_nv12, bool allow_direct_p010)
{
    if (!dec || !dec->initialized || !demux)
        return false;
    if (dec->width != demux->width || dec->height != demux->height)
        return false;

    bool profile_matches = false;
    switch (demux->codec) {
    case WAYWAL_CODEC_H264:
        profile_matches =
            (dec->va_profile == VAProfileH264Main || dec->va_profile == VAProfileH264High ||
             dec->va_profile == VAProfileH264ConstrainedBaseline);
        break;
    case WAYWAL_CODEC_HEVC:
        profile_matches =
            (dec->va_profile == VAProfileHEVCMain || dec->va_profile == VAProfileHEVCMain10);
        break;
    case WAYWAL_CODEC_VP9:
        profile_matches =
            (dec->va_profile == VAProfileVP9Profile0 || dec->va_profile == VAProfileVP9Profile2);
        break;
    case WAYWAL_CODEC_AV1:
        profile_matches = (dec->va_profile == VAProfileAV1Profile0);
        break;
    default:
        break;
    }
    if (!profile_matches)
        return false;

    bool is_10bit = false;
    uint32_t max_refs = 4;
    vaapi_decoder_probe_extradata(demux->codec, dec->va_profile, demux->extradata,
                                  demux->extradata_size, &is_10bit, &max_refs);
    if (demux->codec == WAYWAL_CODEC_HEVC) {
        if (is_10bit && dec->va_profile != VAProfileHEVCMain10)
            return false;
        if (!is_10bit && dec->va_profile != VAProfileHEVCMain)
            return false;
    }
    uint32_t expected_fourcc = is_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    if (dec->fourcc != expected_fourcc)
        return false;

    bool want_direct = is_10bit ? (allow_direct_nv12 && allow_direct_p010) : allow_direct_nv12;
    if (dec->has_vpp != (!want_direct))
        return false;

    uint32_t needed_surfaces = max_refs + 6U + 4U;
    if (needed_surfaces < 26U)
        needed_surfaces = 26U;
    if (needed_surfaces > WAYWAL_VA_SURFACE_POOL_SIZE)
        needed_surfaces = WAYWAL_VA_SURFACE_POOL_SIZE;

    return (dec->num_surfaces >= needed_surfaces);
}

static bool video_engine_start_playback(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !ve->demuxer)
        return false;

    int drm_fd = ve->daemon_state ? ve->daemon_state->dmabuf_ctx.drm_fd : -1;
    bool has_nv12 = ve->daemon_state && ve->daemon_state->dmabuf_ctx.has_nv12;
    bool has_p010 = ve->daemon_state && ve->daemon_state->dmabuf_ctx.has_p010;
    bool gpu_yuv_ok = ve->daemon_state && ve->daemon_state->render_engine.gpu_compute_active &&
                      ve->daemon_state->dmabuf_ctx.available &&
                      ve->daemon_state->dmabuf_ctx.dmabuf_proto != NULL;
    bool allow_direct_nv12 =
        gpu_yuv_ok || (has_nv12 && (!ve->decoder.initialized || !ve->decoder.is_nvidia_vaapi));
    bool allow_direct_p010 =
        gpu_yuv_ok || (has_p010 && (!ve->decoder.initialized || !ve->decoder.is_nvidia_vaapi));

    if (can_reuse_vaapi_decoder(&ve->decoder, ve->demuxer, allow_direct_nv12, allow_direct_p010)) {
        if (ve->decoder.va_display) {
            for (uint32_t i = 0; i < ve->decoder.num_surfaces; ++i) {
                if (ve->decoder.surfaces[i] != VA_INVALID_SURFACE) {
                    vaSyncSurface(ve->decoder.va_display, ve->decoder.surfaces[i]);
                }
            }
            if (ve->decoder.has_vpp) {
                for (uint32_t i = 0; i < ve->decoder.num_vpp_surfaces; ++i) {
                    if (ve->decoder.vpp_surfaces[i] != VA_INVALID_SURFACE) {
                        vaSyncSurface(ve->decoder.va_display, ve->decoder.vpp_surfaces[i]);
                    }
                }
            }
        }
        memset(&ve->decoder.cached_sps, 0, sizeof(ve->decoder.cached_sps));
        memset(&ve->decoder.cached_pps, 0, sizeof(ve->decoder.cached_pps));
        memset(&ve->decoder.cached_hevc_sps, 0, sizeof(ve->decoder.cached_hevc_sps));
        memset(&ve->decoder.cached_hevc_pps, 0, sizeof(ve->decoder.cached_hevc_pps));
        memset(&ve->decoder.cached_av1_seq, 0, sizeof(ve->decoder.cached_av1_seq));
        ve->decoder.is_avcc = false;
        ve->decoder.nal_length_size = 4;
        memset(&ve->decoder.cached_iq, 16, sizeof(ve->decoder.cached_iq));
        ve->decoder.iq_valid = true;
        ve->decoder.allow_direct_p010 = allow_direct_p010;
        vaapi_decoder_reset(&ve->decoder);
        if (ve->demuxer->extradata && ve->demuxer->extradata_size > 0) {
            vaapi_decoder_set_extradata(&ve->decoder, ve->demuxer->extradata,
                                        ve->demuxer->extradata_size);
        }
        ve->decoder.current_surface_idx = 0;
        WAYWAL_LOG_INFO("Reusing active VA-API decoder context (%ux%u, %u surfaces)",
                        ve->decoder.width, ve->decoder.height, ve->decoder.num_surfaces);
    } else {
        if (ve->decoder.initialized) {
            if (ve->daemon_state) {
                for (output_node_t *node = ve->daemon_state->outputs; node != NULL;
                     node = node->next) {
                    if (is_output_active_for_video(ve, node)) {
                        output_node_render_color(ve->daemon_state, node,
                                                 ve->daemon_state->current_color);
                    }
                }
            }
            video_engine_release_wl_buffers(ve);
            vaapi_decoder_destroy(&ve->decoder);
        }
        ve->decoder.allow_direct_p010 = allow_direct_p010;
        if (!vaapi_decoder_init_with_params(
                &ve->decoder, drm_fd, ve->demuxer->codec, ve->demuxer->width, ve->demuxer->height,
                ve->demuxer->extradata, ve->demuxer->extradata_size, allow_direct_nv12)) {
#ifdef HAVE_FFMPEG_SWDEC
            if (!swdec_init(ve, ve->demuxer)) {
                WAYWAL_LOG_ERR(
                    "Failed to initialize both VA-API hardware and FFmpeg software video decoder");
                ve->demuxer->destroy(ve->demuxer);
                ve->demuxer = NULL;
                ve->state = WAYWAL_VIDEO_STATE_STOPPED;
                ve->reorder_count = 0;
                ve->has_decoded_any = false;
                return false;
            }
#else
            WAYWAL_LOG_ERR("Failed to initialize hardware VA-API decoder for video");
            ve->demuxer->destroy(ve->demuxer);
            ve->demuxer = NULL;
            ve->state = WAYWAL_VIDEO_STATE_STOPPED;
            ve->reorder_count = 0;
            ve->has_decoded_any = false;
            return false;
#endif
        }
    }

    ve->target_output = out;
    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->current_loop = 0;
    ve->presented_loop_id = 0;
    ve->last_pts_us = 0;
    ve->playback_epoch_ns = 0;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->current_gop_id = 0;
    ve->next_present_gop = 0;
    ve->next_present_poc = 0;
    ve->has_decoded_any = false;
    ve->hw_decode_err_count = 0;
    ve->last_vpp_idx = 0;
    ve->last_presented_buf_idx = -1;
    ve->last_export_surf = VA_INVALID_SURFACE;
    memset(ve->combined_busy_mask, 0, sizeof(ve->combined_busy_mask));

    if (ve->daemon_state && !ve->using_swdec) {
        for (output_node_t *node = ve->daemon_state->outputs; node != NULL; node = node->next) {
            if (is_output_active_for_video(ve, node)) {
                node->vp_configured = false;
                reclaim_output_shm_for_video(node);
            }
        }
        malloc_trim(0);
    }

    WAYWAL_LOG_INFO("Hardware video playback started: %ux%u @ %u fps (duration: %.2fs)",
                    ve->demuxer->width, ve->demuxer->height, ve->demuxer->fps_num,
                    (double)ve->demuxer->duration_us / 1e6);

    /* Arm timer to trigger first frame decode immediately (1 ms) */
    struct itimerspec its = {
        .it_interval = {0, 0}, .it_value = {.tv_sec = 0, .tv_nsec = 1000000L} /* 1ms */
    };
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
    return true;
}

bool video_engine_load_file(video_engine_t *ve, const char *filepath, output_node_t *out)
{
    if (!ve || !filepath)
        return false;

    bool saved_filter = ve->has_target_filter;
    video_engine_prepare_reload(ve);
    ve->has_target_filter = saved_filter;

    ve->demuxer = demuxer_open_file(filepath);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video file '%s'", filepath);
        ve->filepath[0] = '\0';
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    if (!video_engine_start_playback(ve, out)) {
        ve->filepath[0] = '\0';
        return false;
    }
    snprintf(ve->filepath, sizeof(ve->filepath), "%s", filepath);
    return true;
}

bool video_engine_load_mem(video_engine_t *ve, int memfd, size_t size, output_node_t *out)
{
    if (!ve || memfd < 0 || size == 0)
        return false;

    bool saved_filter = ve->has_target_filter;
    video_engine_prepare_reload(ve);
    ve->has_target_filter = saved_filter;
    ve->filepath[0] = '\0';

    void *mapped = mmap(NULL, size, PROT_READ, PROT_READ ? MAP_SHARED : MAP_SHARED, memfd, 0);
    if (mapped == MAP_FAILED) {
        WAYWAL_LOG_ERR("mmap failed for video memfd: %s", strerror(errno));
        close(memfd);
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }
    (void)madvise(mapped, size, MADV_RANDOM);

    ve->demuxer = demuxer_open_mem((const uint8_t *)mapped, size);
    if (!ve->demuxer) {
        WAYWAL_LOG_ERR("Failed to demux video from memory buffer");
        munmap(mapped, size);
        close(memfd);
        video_engine_release_wl_buffers(ve);
        if (ve->decoder.initialized) {
            vaapi_decoder_destroy(&ve->decoder);
        }
        return false;
    }

    typedef struct {
        int file_fd;
        bool owns_mmap;
    } demuxer_common_state_t;
    demuxer_common_state_t *state = (demuxer_common_state_t *)ve->demuxer->internal_state;
    if (state) {
        state->owns_mmap = true;
        state->file_fd = memfd;
    }
    if (ve->demuxer->seek_us) {
        ve->demuxer->seek_us(ve->demuxer, 0);
    }

    return video_engine_start_playback(ve, out);
}

static void update_combined_busy_mask(video_engine_t *ve)
{
    video_engine_reconcile_outputs(ve);
    uint32_t pool_size =
        ve->decoder.num_surfaces > 0 ? ve->decoder.num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;
    memset(ve->combined_busy_mask, 0, sizeof(ve->combined_busy_mask));
    if (!ve->decoder.has_vpp) {
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (ve->video_buffer_in_use[i] > 0 ||
                (ve->last_export_surf != VA_INVALID_SURFACE &&
                 ve->decoder.surfaces[i] == ve->last_export_surf)) {
                ve->combined_busy_mask[i] = true;
            }
        }
    }
    for (uint32_t r = 0; r < ve->reorder_count; ++r) {
        uint32_t idx = ve->reorder_buf[r].surf_idx;
        if (idx < pool_size) {
            ve->combined_busy_mask[idx] = true;
        }
    }
    ve->decoder.surface_busy_mask = ve->combined_busy_mask;
}

static bool reorder_has_ready_frame(video_engine_t *ve)
{
    if (ve->reorder_count == 0)
        return false;
    if (ve->eof_draining)
        return true;

    uint32_t target_depth = 1;
    if (ve->demuxer) {
        if (ve->demuxer->codec == WAYWAL_CODEC_HEVC) {
            if (ve->decoder.cached_hevc_sps.valid) {
                target_depth = (uint32_t)ve->decoder.cached_hevc_sps.sps_max_num_reorder_pics + 1U;
            } else {
                target_depth = 3U;
            }
        } else if (ve->demuxer->codec == WAYWAL_CODEC_H264) {
            target_depth =
                vaapi_h264_compute_reorder_depth(&ve->decoder.cached_sps, WAYWAL_VIDEO_REORDER_CAP);
        }
    }
    if (target_depth > WAYWAL_VIDEO_REORDER_CAP) {
        target_depth = WAYWAL_VIDEO_REORDER_CAP;
    }
    return (ve->reorder_count >= target_depth);
}

void video_engine_dispatch_frame(video_engine_t *ve)
{
    if (!ve || ve->timer_fd < 0) {
        return;
    }

    uint64_t expirations = 0;
    ssize_t s = read(ve->timer_fd, &expirations, sizeof(expirations));
    if (s <= 0 || expirations == 0)
        return;

    if (ve->state != WAYWAL_VIDEO_STATE_PLAYING || !ve->demuxer) {
        return;
    }

    output_node_t *pll_ref_out = select_pll_reference_output(ve);

    uint32_t base_fps = (ve->demuxer->fps_num > 0 && ve->demuxer->fps_den > 0)
                            ? (ve->demuxer->fps_num / ve->demuxer->fps_den)
                            : 30U;
    if (base_fps == 0)
        base_fps = 30U;

    uint32_t eff_fps = base_fps;
    if (ve->daemon_state && ve->daemon_state->power_gov && ve->has_decoded_any) {
        uint32_t max_out_fps = 0;
        bool found_active_out = false;
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            if (!is_output_active_for_video(ve, out) || !out->surface)
                continue;
            found_active_out = true;
            uint32_t out_fps = waywal_power_governor_effective_fps(
                ve->daemon_state->power_gov, out->name[0] ? out->name : NULL, base_fps);
            if (out_fps > max_out_fps)
                max_out_fps = out_fps;
        }
        eff_fps = found_active_out
                      ? max_out_fps
                      : waywal_power_governor_effective_fps(ve->daemon_state->power_gov,
                                                            pll_ref_out ? pll_ref_out->name : NULL,
                                                            base_fps);
        if (eff_fps == 0) {
            /* All target outputs occluded or paused by power governor: check again in 250ms */
            struct itimerspec its_idle = {.it_interval = {0, 0},
                                          .it_value = {.tv_sec = 0, .tv_nsec = 250000000L}};
            timerfd_settime(ve->timer_fd, 0, &its_idle, NULL);
            return;
        }
    }

#ifdef HAVE_FFMPEG_SWDEC
    if (ve->using_swdec && ve->swdec_ctx) {
        int64_t sw_interval_us = 1000000LL / (int64_t)eff_fps;
        double sw_speed = (ve->playback_speed > 0.01f) ? (double)ve->playback_speed : 1.0;
        sw_interval_us = (int64_t)((double)sw_interval_us / sw_speed);
        if (sw_interval_us < 1000LL)
            sw_interval_us = 1000LL;
        int64_t sw_interval_ns = sw_interval_us * 1000LL;

        int64_t sw_pts_us = 0;
        bool first_frame = !ve->has_decoded_any;
        if (swdec_decode_and_present(ve, &sw_pts_us)) {
            ve->has_decoded_any = true;
            ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
            if (ve->daemon_state) {
                render_engine_bump_upload_gen(&ve->daemon_state->render_engine);
                int sw_attached_count = 0;
                for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
                    if (!is_output_active_for_video(ve, out) || !out->surface)
                        continue;
                    if (!first_frame && ve->daemon_state->power_gov &&
                        waywal_power_governor_should_pause_output(ve->daemon_state->power_gov,
                                                                  out->name)) {
                        continue;
                    }
                    if (sw->has_gbm_ring && sw->last_ready_ring_idx >= 0 &&
                        sw->wl_bufs[sw->last_ready_ring_idx]) {
                        int r_idx = sw->last_ready_ring_idx;
                        configure_output_video_viewport(out, sw->width, sw->height);
                        (void)prepare_output_fifo_and_feedback(out, pll_ref_out, sw_interval_ns);
                        wl_surface_attach(out->surface, sw->wl_bufs[r_idx], 0, 0);
                        wl_surface_damage_buffer(out->surface, 0, 0, (int32_t)sw->width,
                                                 (int32_t)sw->height);
                        wl_surface_commit(out->surface);
                        out->has_image = true;
                        sw_attached_count++;
                        video_engine_track_attach(ve, out, -1, r_idx);
                    } else if (sw->rgba_buf) {
                        out->fifo_barrier_pending = false;
                        output_node_render_image(ve->daemon_state, out, sw->rgba_buf, sw->width,
                                                 sw->height);
                    }
                }
                if (sw_attached_count > 0 && sw->has_gbm_ring && sw->last_ready_ring_idx >= 0) {
                    sw->buf_busy[sw->last_ready_ring_idx] += sw_attached_count;
                }
                wl_display_flush(ve->daemon_state->display);
            }
        }
        if (ve->state != WAYWAL_VIDEO_STATE_PLAYING)
            return;

        struct itimerspec sw_its = {.it_interval = {0, 0},
                                    .it_value = {
                                        .tv_sec = sw_interval_ns / 1000000000LL,
                                        .tv_nsec = sw_interval_ns % 1000000000LL,
                                    }};
        timerfd_settime(ve->timer_fd, 0, &sw_its, NULL);
        return;
    }
#endif

    uint32_t pool_size =
        ve->decoder.num_surfaces > 0 ? ve->decoder.num_surfaces : WAYWAL_VA_SURFACE_POOL_SIZE;

    if (ve->eof_draining && ve->reorder_count == 0) {
        ve->eof_draining = false;
        if (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count) {
            ve->current_loop++;
            WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback", ve->loop_count);
            video_engine_stop(ve);
            return;
        }
        ve->current_loop++;
        ve->demuxer->seek_us(ve->demuxer, 0);
        vaapi_decoder_reset(&ve->decoder);
        ve->current_gop_id++;
    }

    bool use_gpu_yuv = video_engine_use_gpu_yuv(ve);
    bool looped_this_tick = false;
    while (!ve->eof_draining && ve->reorder_count < WAYWAL_VIDEO_REORDER_CAP) {
        if (reorder_has_ready_frame(ve)) {
            break;
        }

        update_combined_busy_mask(ve);
        bool any_surface_free = false;
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (!ve->combined_busy_mask[i]) {
                any_surface_free = true;
                break;
            }
        }
        if (!any_surface_free && ve->has_decoded_any) {
            break;
        }

        demux_packet_t pkt;
        if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
            bool is_final_loop = (ve->loop_count > 0 && ve->current_loop + 1 >= ve->loop_count);
            if (is_final_loop || looped_this_tick) {
                if (ve->reorder_count > 0) {
                    /* Drain remaining B-frame / reorder_buf frames in POC order before stopping */
                    ve->eof_draining = true;
                    break;
                }
                ve->current_loop++;
                WAYWAL_LOG_INFO("Video loop count (%lu) reached, stopping playback",
                                ve->loop_count);
                video_engine_stop(ve);
                return;
            }
            looped_this_tick = true;
            ve->current_loop++;

            ve->demuxer->seek_us(ve->demuxer, 0);
            vaapi_decoder_reset(&ve->decoder);
            ve->current_gop_id++;
            if (!ve->demuxer->read_packet(ve->demuxer, &pkt)) {
                if (ve->reorder_count > 0) {
                    ve->eof_draining = true;
                    break;
                }
                WAYWAL_LOG_WARN("Failed to seek to start of stream on video loop");
                video_engine_stop(ve);
                return;
            }
        }

        VASurfaceID decoded_surface = VA_INVALID_SURFACE;
        vaapi_decode_status_t dst =
            vaapi_decoder_decode_packet_ex(&ve->decoder, &pkt, &decoded_surface);
        if (dst == VAAPI_DECODE_HIDDEN_NO_DISPLAY) {
            ve->hw_decode_err_count = 0;
            continue;
        }
        if (dst == VAAPI_DECODE_BUSY) {
            break;
        }
        if (dst != VAAPI_DECODE_OK) {
            ve->hw_decode_err_count++;
            if (ve->hw_decode_err_count >= 8U ||
                (ve->hw_decode_err_count >= 2U && !ve->has_decoded_any)) {
                WAYWAL_LOG_ERR("Unrecoverable HW VA-API decode failure (streak=%u), falling back",
                               ve->hw_decode_err_count);
                video_engine_release_wl_buffers(ve);
                vaapi_decoder_destroy(&ve->decoder);
                ve->reorder_count = 0;
#ifdef HAVE_FFMPEG_SWDEC
                if (ve->demuxer->seek_us) {
                    ve->demuxer->seek_us(ve->demuxer, 0);
                }
                if (swdec_init(ve, ve->demuxer)) {
                    ve->hw_decode_err_count = 0;
                    struct itimerspec fb_its = {.it_interval = {0, 0},
                                                .it_value = {.tv_sec = 0, .tv_nsec = 1000000L}};
                    timerfd_settime(ve->timer_fd, 0, &fb_its, NULL);
                    return;
                }
#endif
                video_engine_stop(ve);
                return;
            }
            break;
        }

        ve->hw_decode_err_count = 0;
        if (decoded_surface == VA_INVALID_SURFACE) {
            continue;
        }
        if (ve->decoder.last_decoded_is_idr && ve->has_decoded_any) {
            ve->current_gop_id++;
        }
        ve->has_decoded_any = true;

        uint32_t surf_idx = 0;
        for (uint32_t i = 0; i < pool_size; ++i) {
            if (ve->decoder.surfaces[i] == decoded_surface) {
                surf_idx = i;
                break;
            }
        }

        ve->reorder_buf[ve->reorder_count++] = (video_reorder_entry_t){
            .surface = decoded_surface,
            .surf_idx = surf_idx,
            .pts_us = pkt.pts_us,
            .poc = ve->decoder.last_decoded_poc,
            .gop_id = ve->current_gop_id,
            .loop_id = ve->current_loop,
        };
    }

    int64_t frame_interval_us = 33333LL;
    if (ve->demuxer->fps_num > 0) {
        frame_interval_us =
            (1000000LL * (int64_t)ve->demuxer->fps_den) / (int64_t)ve->demuxer->fps_num;
    }
    if (eff_fps < base_fps && eff_fps > 0) {
        int64_t throttled_us = 1000000LL / (int64_t)eff_fps;
        if (throttled_us > frame_interval_us) {
            frame_interval_us = throttled_us;
        }
    }
    double speed = (ve->playback_speed > 0.01f) ? (double)ve->playback_speed : 1.0;
    frame_interval_us = (int64_t)((double)frame_interval_us / speed);
    if (frame_interval_us < 1000LL)
        frame_interval_us = 1000LL;
    int64_t frame_interval_ns = frame_interval_us * 1000LL;

    struct timespec now_ts;
    int64_t now_ns = 0;
    int64_t target_pres_ns = 0;
    bool used_hw_commit_timer = false;

    if (ve->reorder_count > 0) {
        uint32_t best_idx = 0;
        for (uint32_t i = 1; i < ve->reorder_count; ++i) {
            const video_reorder_entry_t *a = &ve->reorder_buf[i];
            const video_reorder_entry_t *b = &ve->reorder_buf[best_idx];
            if (a->gop_id < b->gop_id || (a->gop_id == b->gop_id && a->poc < b->poc) ||
                (a->gop_id == b->gop_id && a->poc == b->poc && a->pts_us < b->pts_us)) {
                best_idx = i;
            }
        }

        video_reorder_entry_t pres = ve->reorder_buf[best_idx];
        bool vpp_busy_stall = false;
        VASurfaceID export_surf = pres.surface;
        uint32_t buf_idx = pres.surf_idx;
        bool was_vpp_warmed_up = ve->decoder.vpp_warmed_up;

        video_engine_reconcile_outputs(ve);

        if (ve->decoder.has_vpp) {
            uint32_t vpp_pool = ve->decoder.num_vpp_surfaces > 0 ? ve->decoder.num_vpp_surfaces
                                                                 : WAYWAL_VA_VPP_POOL_SIZE;
            uint32_t vpp_idx = (ve->last_vpp_idx + 1) % vpp_pool;
            bool found_free = false;
            for (uint32_t k = 0; k < vpp_pool; ++k) {
                uint32_t cand = (ve->last_vpp_idx + 1 + k) % vpp_pool;
                if (cand == ve->last_vpp_idx && vpp_pool > 1)
                    continue;
                if (ve->video_buffer_in_use[cand] <= 0) {
                    vpp_idx = cand;
                    found_free = true;
                    break;
                }
            }
            if (!found_free) {
                /* Do not overwrite an in-flight VPP buffer owned by the compositor. */
                vpp_busy_stall = true;
            } else {
                export_surf = ve->decoder.vpp_surfaces[vpp_idx];
                buf_idx = vpp_idx;
                if (!vaapi_decoder_vpp_blit(&ve->decoder, pres.surface, export_surf)) {
                    ve->hw_decode_err_count++;
                    ve->reorder_count--;
                    if (best_idx < ve->reorder_count) {
                        ve->reorder_buf[best_idx] = ve->reorder_buf[ve->reorder_count];
                    }
                    if (ve->hw_decode_err_count >= 8U) {
                        video_engine_stop(ve);
                        return;
                    }
                    struct itimerspec retry_its = {.it_interval = {0, 0},
                                                   .it_value = {.tv_sec = 0, .tv_nsec = 2000000L}};
                    timerfd_settime(ve->timer_fd, 0, &retry_its, NULL);
                    return;
                }
                ve->last_vpp_idx = vpp_idx;
                if (!was_vpp_warmed_up && ve->decoder.vpp_warmed_up) {
                    ve->playback_epoch_ns = 0;
                }
            }
        } else if (!use_gpu_yuv && ve->decoder.va_display && ve->video_wl_buffers[buf_idx]) {
            vaSyncSurface(ve->decoder.va_display, export_surf);
        }

        if (vpp_busy_stall) {
            if (ve->daemon_state && ve->daemon_state->display) {
                wl_display_flush(ve->daemon_state->display);
            }
            struct itimerspec retry_its = {.it_interval = {0, 0},
                                           .it_value = {.tv_sec = 0, .tv_nsec = 2000000L}};
            timerfd_settime(ve->timer_fd, 0, &retry_its, NULL);
            return;
        }

        ve->reorder_count--;
        if (best_idx < ve->reorder_count) {
            ve->reorder_buf[best_idx] = ve->reorder_buf[ve->reorder_count];
        }
        ve->last_export_surf = export_surf;

        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        now_ns = (int64_t)now_ts.tv_sec * 1000000000LL + now_ts.tv_nsec;

        int64_t scaled_pts_ns = (int64_t)((double)(pres.pts_us * 1000LL) / speed);
        bool reset_epoch = (ve->playback_epoch_ns == 0 || pres.loop_id != ve->presented_loop_id ||
                            pres.pts_us < ve->last_pts_us);
        if (reset_epoch) {
            ve->playback_epoch_ns = now_ns - scaled_pts_ns;
            ve->presented_loop_id = pres.loop_id;
        }
        int64_t this_frame_pres_ns = ve->playback_epoch_ns + scaled_pts_ns;
        target_pres_ns = this_frame_pres_ns + frame_interval_ns;
        if (target_pres_ns <= now_ns - 2 * frame_interval_ns) {
            ve->playback_epoch_ns = now_ns - scaled_pts_ns;
            this_frame_pres_ns = now_ns;
            target_pres_ns = now_ns + frame_interval_ns;
            reset_epoch = true;
        }

        if (use_gpu_yuv && ve->daemon_state) {
            vaapi_prime_frame_t pf;
            if (vaapi_decoder_export_prime(&ve->decoder, export_surf, &pf)) {
                video_yuv_gl_ctx_t *yg = video_yuv_ensure_gl_ctx(ve);
                video_yuv_imported_frame_t imp;
                if (yg && video_yuv_import_prime_frame(yg, &pf, &imp)) {
                    render_engine_bump_upload_gen(&ve->daemon_state->render_engine);
                    int committed = 0;
                    for (output_node_t *out = ve->daemon_state->outputs; out != NULL;
                         out = out->next) {
                        if (!is_output_active_for_video(ve, out) || !out->surface)
                            continue;
                        if (ve->daemon_state->power_gov && out->has_image &&
                            waywal_power_governor_should_pause_output(ve->daemon_state->power_gov,
                                                                      out->name)) {
                            continue;
                        }
                        if (video_engine_present_gpu_yuv_output(
                                ve, yg, &imp, out, frame_interval_ns, pll_ref_out, reset_epoch,
                                this_frame_pres_ns, now_ns, &used_hw_commit_timer)) {
                            committed++;
                        }
                    }
                    video_yuv_release_imported_frame(yg, &imp);
                    if (committed > 0) {
                        render_engine_release_upload_cache(&ve->daemon_state->render_engine);
                        wl_display_flush(ve->daemon_state->display);
                    }
                }
                vaapi_prime_frame_close(&pf);
            }
        } else {
            struct wl_buffer *wl_buf = ensure_shared_video_buffer(ve, export_surf, buf_idx);
            if (wl_buf && ve->daemon_state) {
                ve->last_presented_buf_idx = (int32_t)buf_idx;
                int attached_count = 0;
                for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
                    if (!is_output_active_for_video(ve, out) || !out->surface)
                        continue;
                    if (ve->daemon_state->power_gov && out->has_image &&
                        waywal_power_governor_should_pause_output(ve->daemon_state->power_gov,
                                                                  out->name)) {
                        continue;
                    }
                    configure_output_video_viewport(out, ve->decoder.width, ve->decoder.height);
                    int64_t out_period_ns =
                        prepare_output_fifo_and_feedback(out, pll_ref_out, frame_interval_ns);
                    if (out->commit_timer && ve->daemon_state->presentation &&
                        ve->daemon_state->has_presentation_clock_id) {
                        used_hw_commit_timer = true;
                        if (!reset_epoch) {
                            /* Subtract a quarter-refresh margin because commit-timing-v1 specifies
                             * a not-before deadline rather than a nearest-vblank target. */
                            int64_t slop_ns = (out_period_ns > 0) ? (out_period_ns / 4) : 1500000LL;
                            if (slop_ns < 500000LL)
                                slop_ns = 500000LL;
                            if (slop_ns > 4000000LL)
                                slop_ns = 4000000LL;
                            if (slop_ns > frame_interval_ns / 4)
                                slop_ns = frame_interval_ns / 4;

                            int64_t not_before_mono_ns = this_frame_pres_ns - slop_ns;
                            if (not_before_mono_ns > now_ns) {
                                int64_t hw_target_ns = not_before_mono_ns;
                                if (ve->daemon_state->presentation_clock_id != CLOCK_MONOTONIC) {
                                    struct timespec pres_ts;
                                    if (clock_gettime(ve->daemon_state->presentation_clock_id,
                                                      &pres_ts) == 0) {
                                        int64_t pres_now_ns =
                                            (int64_t)pres_ts.tv_sec * 1000000000LL +
                                            pres_ts.tv_nsec;
                                        hw_target_ns = pres_now_ns + (not_before_mono_ns - now_ns);
                                    }
                                }
                                if (hw_target_ns > 0) {
                                    waywal_commit_timer_set_timestamp_ns(out->commit_timer,
                                                                         hw_target_ns);
                                }
                            }
                        }
                    }
                    wl_surface_attach(out->surface, wl_buf, 0, 0);
                    wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
                    wl_surface_commit(out->surface);
                    out->has_image = true;
                    attached_count++;
                    video_engine_track_attach(ve, out, (int32_t)buf_idx, -1);
                    if (out->dmabuf_ring.initialized) {
                        for (size_t k = 0; k < WAYWAL_DMABUF_RING_SIZE; ++k) {
                            dmabuf_bo_t *rbo = &out->dmabuf_ring.buffers[k];
                            if ((rbo->gbm_bo != NULL || rbo->wl_buffer != NULL) && !rbo->in_use) {
                                if (out->shader_eng) {
                                    waywal_shader_engine_forget_bo(out->shader_eng, rbo);
                                }
                                if (ve->daemon_state->shader_eng &&
                                    ve->daemon_state->shader_eng != out->shader_eng) {
                                    waywal_shader_engine_forget_bo(ve->daemon_state->shader_eng,
                                                                   rbo);
                                }
                                if (out->we_scene) {
                                    waywal_we_scene_forget_bo(out->we_scene, rbo);
                                }
                                if (ve->daemon_state->we_scene &&
                                    ve->daemon_state->we_scene != out->we_scene) {
                                    waywal_we_scene_forget_bo(ve->daemon_state->we_scene, rbo);
                                }
                                if (out->last_presented_bo == rbo) {
                                    out->last_presented_bo = NULL;
                                }
                                if (out->last_committed_ring_idx == (int)k) {
                                    out->last_committed_ring_idx = -1;
                                }
                                render_engine_release_bo(&ve->daemon_state->render_engine, rbo);
                                dmabuf_bo_free(rbo);
                            }
                        }
                    }
                }
                if (attached_count > 0) {
                    ve->video_buffer_in_use[buf_idx] += attached_count;
                    render_engine_release_upload_cache(&ve->daemon_state->render_engine);
                    wl_display_flush(ve->daemon_state->display);
                }
            }
        }

        ve->last_pts_us = pres.pts_us;
        ve->next_present_gop = pres.gop_id;
        ve->next_present_poc = pres.poc + 1;
    } else {
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        now_ns = (int64_t)now_ts.tv_sec * 1000000000LL + now_ts.tv_nsec;
        target_pres_ns = now_ns + frame_interval_ns;
    }

    int64_t wake_target_ns = target_pres_ns;
    if (used_hw_commit_timer) {
        /* Wake ahead of the next frame's presentation deadline so the decoded buffer and
         * commit_timer timestamp are queued inside the compositor before latch. */
        int64_t commit_lead_ns = 4000000LL;
        if (commit_lead_ns > frame_interval_ns / 2)
            commit_lead_ns = frame_interval_ns / 2;
        wake_target_ns = target_pres_ns - commit_lead_ns;
    } else if (pll_ref_out && pll_ref_out->pres_sync.pll.phase_est_ns > 0 &&
               pll_ref_out->pres_sync.pll.period_est_ns > 0 &&
               (!pll_ref_out->pres_sync.pll.has_clock_id ||
                pll_ref_out->pres_sync.pll.clock_id == CLOCK_MONOTONIC)) {
        int64_t next_vblank_ns = presentation_pll_predict_next_vblank(&pll_ref_out->pres_sync.pll);
        int64_t period_ns = pll_ref_out->pres_sync.pll.period_est_ns;

        if (period_ns > 0 && frame_interval_ns >= period_ns - (period_ns / 12)) {
            int64_t rem = frame_interval_ns % period_ns;
            bool harmonic = (rem <= period_ns / 12 || (period_ns - rem) <= period_ns / 12);
            if (harmonic) {
                int64_t lead_time_ns = period_ns / 4;
                if (lead_time_ns < 1500000LL)
                    lead_time_ns = 1500000LL;
                if (lead_time_ns > 4000000LL)
                    lead_time_ns = 4000000LL;
                if (lead_time_ns > period_ns / 2)
                    lead_time_ns = period_ns / 2;

                int64_t desired_vblank_ns = target_pres_ns + lead_time_ns;
                if (desired_vblank_ns >= next_vblank_ns) {
                    int64_t cycles =
                        (desired_vblank_ns - next_vblank_ns + (period_ns / 2)) / period_ns;
                    if (cycles < 0)
                        cycles = 0;
                    int64_t aligned_vblank_ns = next_vblank_ns + cycles * period_ns;
                    int64_t target_fire_ns = aligned_vblank_ns - lead_time_ns;
                    int64_t phase_err = target_fire_ns - target_pres_ns;
                    int64_t adj = phase_err / 4;
                    if (ve->playback_epoch_ns > 0) {
                        ve->playback_epoch_ns += adj;
                    }
                    target_pres_ns += adj;
                    wake_target_ns = target_pres_ns;
                }
            }
        }
    }

    int64_t next_interval_ns = wake_target_ns - now_ns;
    if (next_interval_ns < 1000000LL)
        next_interval_ns = 1000000LL;

    struct itimerspec its = {.it_interval = {0, 0},
                             .it_value = {
                                 .tv_sec = next_interval_ns / 1000000000LL,
                                 .tv_nsec = next_interval_ns % 1000000000LL,
                             }};
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_pause(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PLAYING)
        return;

    struct itimerspec its = {0};
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
    uint64_t dummy;
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;
    ve->state = WAYWAL_VIDEO_STATE_PAUSED;
    if (ve->daemon_state) {
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            if (is_output_active_for_video(ve, out)) {
                out->fifo_barrier_pending = false;
            }
        }
    }
    WAYWAL_LOG_INFO("Hardware video playback paused (0%% VPU usage)");
}

void video_engine_resume(video_engine_t *ve)
{
    if (!ve || ve->state != WAYWAL_VIDEO_STATE_PAUSED)
        return;

    ve->state = WAYWAL_VIDEO_STATE_PLAYING;
    ve->playback_epoch_ns = 0;
    WAYWAL_LOG_INFO("Hardware video playback resumed");

    /* Arm timer to trigger next frame immediately */
    struct itimerspec its = {
        .it_interval = {0, 0}, .it_value = {.tv_sec = 0, .tv_nsec = 1000000L} /* 1ms */
    };
    uint64_t dummy;
    while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
        ;
    timerfd_settime(ve->timer_fd, 0, &its, NULL);
}

void video_engine_stop(video_engine_t *ve)
{
    if (!ve || (ve->state == WAYWAL_VIDEO_STATE_STOPPED && !ve->demuxer && !ve->using_swdec &&
                !ve->decoder.initialized))
        return;

    struct itimerspec its = {0};
    if (ve->timer_fd >= 0) {
        timerfd_settime(ve->timer_fd, 0, &its, NULL);
        uint64_t dummy;
        while (read(ve->timer_fd, &dummy, sizeof(dummy)) > 0)
            ;
    }

    ve->state = WAYWAL_VIDEO_STATE_STOPPED;
    ve->reorder_count = 0;
    ve->eof_draining = false;
    ve->has_decoded_any = false;
    ve->hw_decode_err_count = 0;
    ve->playback_epoch_ns = 0;
    ve->last_presented_buf_idx = -1;
    ve->last_export_surf = VA_INVALID_SURFACE;
    ve->filepath[0] = '\0';

    video_engine_release_wl_buffers(ve);

#ifdef HAVE_FFMPEG_SWDEC
    swdec_destroy(ve);
#endif

    if (ve->daemon_state) {
        for (output_node_t *out = ve->daemon_state->outputs; out != NULL; out = out->next) {
            out->vp_configured = false;
            out->fifo_barrier_pending = false;
            if (out->video_eng == ve) {
                out->video_eng = NULL;
            }
        }
    }

    if (ve->decoder.initialized) {
        vaapi_decoder_destroy(&ve->decoder);
    }

    if (ve->demuxer) {
        ve->demuxer->destroy(ve->demuxer);
        ve->demuxer = NULL;
    }

    ve->target_output = NULL;
    ve->has_target_filter = false;
    malloc_trim(0);
    WAYWAL_LOG_INFO("Hardware video playback stopped");
}

bool video_engine_snapshot_output(video_engine_t *ve, output_node_t *out)
{
    if (!ve || !out || !ve->daemon_state || out->width <= 0 || out->height <= 0)
        return false;

    daemon_state_t *state = ve->daemon_state;
    uint32_t w = (uint32_t)out->width;
    uint32_t h = (uint32_t)out->height;
    uint64_t src_mod = state->dmabuf_ctx.preferred_modifier;

    if (!out->has_source_old_bo || out->source_old_bo.width != w ||
        out->source_old_bo.height != h) {
        if (out->has_source_old_bo) {
            if (out->shader_eng) {
                waywal_shader_engine_forget_bo(out->shader_eng, &out->source_old_bo);
            }
            if (state->shader_eng && state->shader_eng != out->shader_eng) {
                waywal_shader_engine_forget_bo(state->shader_eng, &out->source_old_bo);
            }
            if (out->we_scene) {
                waywal_we_scene_forget_bo(out->we_scene, &out->source_old_bo);
            }
            if (state->we_scene && state->we_scene != out->we_scene) {
                waywal_we_scene_forget_bo(state->we_scene, &out->source_old_bo);
            }
            if (out->last_presented_bo == &out->source_old_bo) {
                out->last_presented_bo = NULL;
            }
            render_engine_release_bo(&state->render_engine, &out->source_old_bo);
            dmabuf_bo_free(&out->source_old_bo);
            out->has_source_old_bo = false;
        }
        if (dmabuf_bo_allocate(&state->dmabuf_ctx, &out->source_old_bo, w, h, DRM_FORMAT_ARGB8888,
                               src_mod)) {
            out->has_source_old_bo = true;
        }
    }
    if (!out->has_source_old_bo)
        return false;

    layer_shell_compute_span_bounds(state, out);
    render_engine_set_span_bounds(&state->render_engine, out->span_off_x, out->span_off_y,
                                  out->span_total_w, out->span_total_h);
    render_engine_set_crop_gravity(&state->render_engine,
                                   out->has_crop_gravity ? out->crop_gravity_x : 0.5f,
                                   out->has_crop_gravity ? out->crop_gravity_y : 0.5f);
    uint32_t clear_col = ((uint32_t)out->fill_color.a << 24) | ((uint32_t)out->fill_color.r << 16) |
                         ((uint32_t)out->fill_color.g << 8) | (uint32_t)out->fill_color.b;

    if (ve->decoder.initialized && ve->last_export_surf != VA_INVALID_SURFACE) {
        vaapi_prime_frame_t prime_frame;
        if (vaapi_decoder_export_prime(&ve->decoder, ve->last_export_surf, &prime_frame)) {
            bool ok = false;
            if (!ve->decoder.has_vpp &&
                (prime_frame.drm_format == DRM_FORMAT_NV12 ||
                 prime_frame.drm_format == DRM_FORMAT_P010) &&
                prime_frame.num_planes >= 2 && state->render_engine.gpu_compute_active) {
                video_yuv_gl_ctx_t *yg = video_yuv_ensure_gl_ctx(ve);
                video_yuv_imported_frame_t imp;
                if (yg && video_yuv_import_prime_frame(yg, &prime_frame, &imp)) {
                    float gx = out->has_crop_gravity ? out->crop_gravity_x : 0.5f;
                    float gy = out->has_crop_gravity ? out->crop_gravity_y : 0.5f;
                    ok = video_yuv_render_imported_to_bo(
                        ve, yg, &imp, &out->source_old_bo, out->scaling_mode, clear_col,
                        out->span_off_x, out->span_off_y, out->span_total_w, out->span_total_h, gx,
                        gy);
                    video_yuv_release_imported_frame(yg, &imp);
                }
            } else if (prime_frame.num_planes >= 1 &&
                       prime_frame.num_planes <= WAYWAL_MAX_BUFFER_PLANES &&
                       (prime_frame.drm_format == DRM_FORMAT_XRGB8888 ||
                        prime_frame.drm_format == DRM_FORMAT_ARGB8888 ||
                        prime_frame.drm_format == DRM_FORMAT_NV12 ||
                        prime_frame.drm_format == DRM_FORMAT_P010)) {
                dmabuf_bo_t tmp_bo;
                memset(&tmp_bo, 0, sizeof(tmp_bo));
                for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p)
                    tmp_bo.fd[p] = -1;
                tmp_bo.num_planes = prime_frame.num_planes;
                tmp_bo.width = prime_frame.width;
                tmp_bo.height = prime_frame.height;
                for (uint32_t p = 0; p < prime_frame.num_planes; ++p) {
                    tmp_bo.fd[p] = prime_frame.fds[p];
                    tmp_bo.stride[p] = prime_frame.strides[p];
                    tmp_bo.offset[p] = prime_frame.offsets[p];
                }
                tmp_bo.drm_format = prime_frame.drm_format;
                tmp_bo.modifier = prime_frame.modifiers[0];
                ok = render_engine_copy_bo(&state->render_engine, &out->source_old_bo, &tmp_bo,
                                           out->scaling_mode, clear_col, true);
                render_engine_release_bo(&state->render_engine, &tmp_bo);
            }
            vaapi_prime_frame_close(&prime_frame);
            if (ok) {
                out->has_image = true;
                out->last_presented_bo = &out->source_old_bo;
                out->last_committed_ring_idx = -1;
                return true;
            }
        }
    }

#ifdef HAVE_FFMPEG_SWDEC
    if (ve->using_swdec && ve->swdec_ctx) {
        ffmpeg_swdec_t *sw = (ffmpeg_swdec_t *)ve->swdec_ctx;
        if (sw->has_gbm_ring && sw->last_ready_ring_idx >= 0 &&
            sw->gbm_bos[sw->last_ready_ring_idx]) {
            struct gbm_bo *gbo = sw->gbm_bos[sw->last_ready_ring_idx];
            int fd = gbm_bo_get_fd(gbo);
            if (fd >= 0) {
                dmabuf_bo_t tmp_bo;
                memset(&tmp_bo, 0, sizeof(tmp_bo));
                for (int p = 0; p < WAYWAL_MAX_BUFFER_PLANES; ++p)
                    tmp_bo.fd[p] = -1;
                tmp_bo.fd[0] = fd;
                tmp_bo.num_planes = 1;
                tmp_bo.width = sw->width;
                tmp_bo.height = sw->height;
                tmp_bo.stride[0] = gbm_bo_get_stride(gbo);
                tmp_bo.drm_format = DRM_FORMAT_XRGB8888;
                tmp_bo.modifier = DRM_FORMAT_MOD_LINEAR;
                bool ok = render_engine_copy_bo(&state->render_engine, &out->source_old_bo, &tmp_bo,
                                                out->scaling_mode, clear_col, true);
                render_engine_release_bo(&state->render_engine, &tmp_bo);
                close(fd);
                if (ok) {
                    out->has_image = true;
                    out->last_presented_bo = &out->source_old_bo;
                    out->last_committed_ring_idx = -1;
                    return true;
                }
            }
        }
        if (sw->rgba_buf && sw->width > 0 && sw->height > 0) {
            if (render_engine_upload_static(&state->render_engine, sw->rgba_buf, sw->width,
                                            sw->height, &out->source_old_bo, out->scaling_mode,
                                            clear_col)) {
                out->has_image = true;
                out->last_presented_bo = &out->source_old_bo;
                out->last_committed_ring_idx = -1;
                return true;
            }
        }
    }
#endif

    return false;
}
