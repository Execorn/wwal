// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_compute.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on

#ifdef _OPENMP
#include <omp.h>
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
#include <immintrin.h>
#endif

#include "shaders_embedded.h"
#include "waywal/log.h"

typedef struct {
    GLint loc_prog;
    GLint loc_res;
    GLint loc_angle;
    GLint loc_wave_freq;
    GLint loc_wave_amp;
    GLint loc_center;
    GLint loc_smoothness;
    GLint loc_step;
    float last_prog;
    GLint last_w;
    GLint last_h;
    float last_angle;
    float last_wave_freq;
    float last_wave_amp;
    float last_center_x;
    float last_center_y;
    float last_smoothness;
    float last_step;
} gpu_program_uniforms_t;

struct gpu_compute_ctx {
    EGLDisplay display;
    EGLContext context;

    GLuint prog_fade;
    GLuint prog_wipe;
    GLuint prog_grow;
    GLuint prog_outer;
    GLuint prog_wave;
    GLuint prog_noise;
    GLuint prog_crosszoom;
    GLuint prog_slide;
    GLuint prog_glitch;
    GLuint prog_burn;
    GLuint prog_ripple;
    GLuint prog_pixelate;
    GLuint prog_doom;
    GLuint prog_swirl;
    GLuint prog_cube;
    GLuint prog_luma;
    GLuint prog_light_leak;
    GLuint prog_page_curl;
    GLuint prog_custom;
    GLuint prog_scale;
    GLuint prog_kawase;

    GLint loc_scale_src_res;
    GLint loc_scale_dst_res;
    GLint loc_scale_mode;
    GLint loc_scale_clear;
    GLint loc_scale_has_src;
    GLint loc_scale_span_bounds;
    GLint loc_scale_gravity;
    int32_t span_bounds[4];
    float crop_gravity[2];
    GLint loc_kawase_src_res;
    GLint loc_kawase_dst_res;
    GLint loc_kawase_radius;
    GLuint upload_src_tex;
    uint32_t upload_src_w;
    uint32_t upload_src_h;
    const void *last_upload_ptr;
    uint32_t last_upload_w;
    uint32_t last_upload_h;
    uint64_t upload_gen;
    uint64_t last_upload_gen;
    GLuint kawase_half_tex;
    uint32_t kawase_half_w;
    uint32_t kawase_half_h;

    GLuint scratch_10bit_tex;
    uint32_t scratch_10bit_w;
    uint32_t scratch_10bit_h;
    GLuint scratch_pool_tex[4];
    uint32_t scratch_pool_w[4];
    uint32_t scratch_pool_h[4];
    uint32_t scratch_pool_next;
    GLuint blit_10bit_fbo;
    GLuint bound_fbo_tex;
    bool scratch_fbo_validated;
    GLuint prog_blit_10bit;
    GLuint blit_10bit_vao;

    gpu_program_uniforms_t unif_fade;
    gpu_program_uniforms_t unif_wipe;
    gpu_program_uniforms_t unif_grow;
    gpu_program_uniforms_t unif_outer;
    gpu_program_uniforms_t unif_wave;
    gpu_program_uniforms_t unif_noise;
    gpu_program_uniforms_t unif_crosszoom;
    gpu_program_uniforms_t unif_slide;
    gpu_program_uniforms_t unif_glitch;
    gpu_program_uniforms_t unif_burn;
    gpu_program_uniforms_t unif_ripple;
    gpu_program_uniforms_t unif_pixelate;
    gpu_program_uniforms_t unif_doom;
    gpu_program_uniforms_t unif_swirl;
    gpu_program_uniforms_t unif_cube;
    gpu_program_uniforms_t unif_luma;
    gpu_program_uniforms_t unif_light_leak;
    gpu_program_uniforms_t unif_page_curl;
    gpu_program_uniforms_t unif_custom;

    GLuint current_prog;
    GLuint bound_target_tex;
    GLuint bound_old_tex;
    GLuint bound_new_tex;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;
};

static void query_program_uniforms(GLuint prog, gpu_program_uniforms_t *u)
{
    if (!prog || !u)
        return;
    u->loc_prog = glGetUniformLocation(prog, "u_progress");
    u->loc_res = glGetUniformLocation(prog, "u_resolution");
    u->loc_angle = glGetUniformLocation(prog, "u_angle");
    u->loc_wave_freq = glGetUniformLocation(prog, "u_wave_freq");
    u->loc_wave_amp = glGetUniformLocation(prog, "u_wave_amp");
    u->loc_center = glGetUniformLocation(prog, "u_center");
    u->loc_smoothness = glGetUniformLocation(prog, "u_smoothness");
    u->loc_step = glGetUniformLocation(prog, "u_step");
    u->last_prog = -1.0f;
    u->last_w = -1;
    u->last_h = -1;
    u->last_angle = -1e9f;
    u->last_wave_freq = -1.0f;
    u->last_wave_amp = -1.0f;
    u->last_center_x = -1e9f;
    u->last_center_y = -1e9f;
    u->last_smoothness = -1.0f;
    u->last_step = -1.0f;
}

static GLuint compile_shader_program(const char *src, const char *name)
{
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!shader) {
        WAYWAL_LOG_ERR("glCreateShader failed for %s", name);
        return 0;
    }

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log_buf[1024];
        glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Compute shader compilation failed for %s: %s", name, log_buf);
        glDeleteShader(shader);
        return 0;
    }

    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(shader);
        return 0;
    }

    glAttachShader(prog, shader);
    glLinkProgram(prog);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024];
        glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("Compute program linking failed for %s: %s", name, log_buf);
        glDeleteProgram(prog);
        glDeleteShader(shader);
        return 0;
    }

    glDeleteShader(shader);
    return prog;
}

static const char *SHADER_SRC_SCALE =
    "#version 310 es\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_target;\n"
    "layout(binding = 1) uniform highp sampler2D u_src;\n"
    "uniform ivec2 u_src_res;\n"
    "uniform ivec2 u_dst_res;\n"
    "uniform int u_scaling_mode;\n"
    "uniform highp vec4 u_clear_color;\n"
    "uniform int u_has_src;\n"
    "uniform ivec4 u_span_bounds;\n"
    "uniform highp vec2 u_crop_gravity;\n"
    "void main() {\n"
    "    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (coord.x >= u_dst_res.x || coord.y >= u_dst_res.y) return;\n"
    "    if (u_has_src == 0 || u_src_res.x <= 0 || u_src_res.y <= 0) {\n"
    "        imageStore(u_target, coord, u_clear_color.bgra);\n"
    "        return;\n"
    "    }\n"
    "    if (u_dst_res.x == u_src_res.x && u_dst_res.y == u_src_res.y && u_scaling_mode != 4 && "
    "u_scaling_mode != 5) {\n"
    "        vec4 c = texelFetch(u_src, coord, 0);\n"
    "        imageStore(u_target, coord, c.bgra);\n"
    "        return;\n"
    "    }\n"
    "    vec2 src_f = vec2(u_src_res), dst_f = vec2(u_dst_res);\n"
    "    vec4 out_col = u_clear_color;\n"
    "    if (u_scaling_mode == 0 || u_scaling_mode == 5) {\n"
    "        vec2 tot = (u_scaling_mode == 5 && u_span_bounds.z > 0 && u_span_bounds.w > 0) ? "
    "vec2(u_span_bounds.zw) : dst_f;\n"
    "        vec2 soff = (u_scaling_mode == 5) ? vec2(u_span_bounds.xy) : vec2(0.0);\n"
    "        if (int(tot.x) == u_src_res.x && int(tot.y) == u_src_res.y) {\n"
    "            ivec2 sc = clamp(ivec2(soff) + coord, ivec2(0), u_src_res - ivec2(1));\n"
    "            out_col = texelFetch(u_src, sc, 0);\n"
    "        } else {\n"
    "            float r_src = src_f.x / src_f.y, r_dst = tot.x / tot.y;\n"
    "            vec2 crop = (r_src > r_dst) ? vec2(src_f.y * r_dst, src_f.y) : vec2(src_f.x, "
    "src_f.x / r_dst);\n"
    "            vec2 off = (src_f - crop) * u_crop_gravity;\n"
    "            vec2 s_px = off + (soff + vec2(coord) + 0.5) * crop / tot;\n"
    "            out_col = texture(u_src, clamp(s_px, vec2(0.5), src_f - 0.5) / src_f);\n"
    "        }\n"
    "    } else if (u_scaling_mode == 1) {\n"
    "        float r_src = src_f.x / src_f.y, r_dst = dst_f.x / dst_f.y;\n"
    "        if (r_src > r_dst) {\n"
    "            int sh = min(int(dst_f.x / r_src + 0.5), u_dst_res.y), py = (u_dst_res.y - sh) / "
    "2;\n"
    "            if (coord.y >= py && coord.y < py + sh && sh > 0) {\n"
    "                vec2 s_px = vec2((float(coord.x) + 0.5) * src_f.x / dst_f.x, (float(coord.y - "
    "py) + 0.5) * src_f.y / float(sh));\n"
    "                out_col = texture(u_src, clamp(s_px, vec2(0.5), src_f - 0.5) / src_f);\n"
    "            }\n"
    "        } else {\n"
    "            int sw = min(int(dst_f.y * r_src + 0.5), u_dst_res.x), px = (u_dst_res.x - sw) / "
    "2;\n"
    "            if (coord.x >= px && coord.x < px + sw && sw > 0) {\n"
    "                vec2 s_px = vec2((float(coord.x - px) + 0.5) * src_f.x / float(sw), "
    "(float(coord.y) + 0.5) * src_f.y / dst_f.y);\n"
    "                out_col = texture(u_src, clamp(s_px, vec2(0.5), src_f - 0.5) / src_f);\n"
    "            }\n"
    "        }\n"
    "    } else if (u_scaling_mode == 2) {\n"
    "        vec2 s_px = (vec2(coord) + 0.5) * src_f / dst_f;\n"
    "        out_col = texture(u_src, clamp(s_px, vec2(0.5), src_f - 0.5) / src_f);\n"
    "    } else if (u_scaling_mode == 3) {\n"
    "        int sx = coord.x - (u_dst_res.x - u_src_res.x) / 2, sy = coord.y - (u_dst_res.y - "
    "u_src_res.y) / 2;\n"
    "        if (sx >= 0 && sx < u_src_res.x && sy >= 0 && sy < u_src_res.y)\n"
    "            out_col = texelFetch(u_src, ivec2(sx, sy), 0);\n"
    "    } else if (u_scaling_mode == 4) {\n"
    "        out_col = texelFetch(u_src, ivec2(coord.x % u_src_res.x, coord.y % u_src_res.y), 0);\n"
    "    }\n"
    "    imageStore(u_target, coord, out_col.bgra);\n"
    "}\n";

static const char *SHADER_SRC_KAWASE =
    "#version 310 es\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_target;\n"
    "layout(binding = 1) uniform highp sampler2D u_src;\n"
    "uniform ivec2 u_src_res;\n"
    "uniform ivec2 u_dst_res;\n"
    "uniform highp float u_radius;\n"
    "void main() {\n"
    "    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (coord.x >= u_dst_res.x || coord.y >= u_dst_res.y) return;\n"
    "    vec2 src_f = max(vec2(u_src_res), vec2(1.0));\n"
    "    vec2 dst_f = max(vec2(u_dst_res), vec2(1.0));\n"
    "    vec2 uv = (vec2(coord) + 0.5) / dst_f;\n"
    "    vec2 step_uv = max(u_radius, 1.0) / src_f;\n"
    "    vec4 sum = texture(u_src, uv) * 4.0;\n"
    "    sum += texture(u_src, clamp(uv + vec2(-step_uv.x, -step_uv.y), vec2(0.0), vec2(1.0))) * "
    "2.0;\n"
    "    sum += texture(u_src, clamp(uv + vec2( step_uv.x, -step_uv.y), vec2(0.0), vec2(1.0))) * "
    "2.0;\n"
    "    sum += texture(u_src, clamp(uv + vec2(-step_uv.x,  step_uv.y), vec2(0.0), vec2(1.0))) * "
    "2.0;\n"
    "    sum += texture(u_src, clamp(uv + vec2( step_uv.x,  step_uv.y), vec2(0.0), vec2(1.0))) * "
    "2.0;\n"
    "    sum += texture(u_src, clamp(uv + vec2(-2.0 * step_uv.x, 0.0), vec2(0.0), vec2(1.0)));\n"
    "    sum += texture(u_src, clamp(uv + vec2( 2.0 * step_uv.x, 0.0), vec2(0.0), vec2(1.0)));\n"
    "    sum += texture(u_src, clamp(uv + vec2(0.0, -2.0 * step_uv.y), vec2(0.0), vec2(1.0)));\n"
    "    sum += texture(u_src, clamp(uv + vec2(0.0,  2.0 * step_uv.y), vec2(0.0), vec2(1.0)));\n"
    "    vec4 out_col = sum * (1.0 / 16.0);\n"
    "    imageStore(u_target, coord, out_col.bgra);\n"
    "}\n";

static GLuint compile_blit_10bit_program(void)
{
    static const char *vs_src = "#version 310 es\n"
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
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return 0;
    }
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    GLint ok_vs = 0, ok_fs = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok_vs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok_fs);
    if (!ok_vs || !ok_fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

gpu_compute_ctx_t *gpu_compute_create(dmabuf_context_t *dmabuf_ctx)
{
    if (!dmabuf_ctx || dmabuf_ctx->drm_fd < 0 || !dmabuf_ctx->gbm) {
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
        WAYWAL_LOG_WARN("Failed to acquire EGL display from GBM device");
        return NULL;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        WAYWAL_LOG_WARN("eglInitialize failed on GBM EGL display (0x%x)", eglGetError());
        return NULL;
    }

    const char *egl_exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!egl_exts || !strstr(egl_exts, "EGL_EXT_image_dma_buf_import")) {
        WAYWAL_LOG_WARN("EGL display does not support EGL_EXT_image_dma_buf_import");
        eglTerminate(dpy);
        return NULL;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        WAYWAL_LOG_WARN("eglBindAPI(EGL_OPENGL_ES_API) failed");
        eglTerminate(dpy);
        return NULL;
    }

    static const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1,
                                         EGL_NONE};

    EGLContext egl_ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_ctx == EGL_NO_CONTEXT) {
        WAYWAL_LOG_WARN("eglCreateContext for GLES 3.1 failed (0x%x)", eglGetError());
        eglTerminate(dpy);
        return NULL;
    }

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        WAYWAL_LOG_WARN("eglMakeCurrent failed (0x%x)", eglGetError());
        eglDestroyContext(dpy, egl_ctx);
        eglTerminate(dpy);
        return NULL;
    }

    gpu_compute_ctx_t *ctx = (gpu_compute_ctx_t *)calloc(1, sizeof(gpu_compute_ctx_t));
    if (!ctx) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, egl_ctx);
        eglTerminate(dpy);
        return NULL;
    }

    ctx->display = dpy;
    ctx->context = egl_ctx;
    ctx->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    ctx->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    ctx->glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    ctx->glEGLImageTargetTexStorageEXT =
        (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");

    if (!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR || !ctx->glEGLImageTargetTexture2DOES) {
        WAYWAL_LOG_WARN("Missing required EGLImage extension entry points");
        free(ctx);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, egl_ctx);
        eglTerminate(dpy);
        return NULL;
    }

    /* Compile embedded compute shaders */
    ctx->prog_fade = compile_shader_program(SHADER_SRC_FADE, "fade");
    ctx->prog_wipe = compile_shader_program(SHADER_SRC_WIPE, "wipe");
    ctx->prog_grow = compile_shader_program(SHADER_SRC_GROW, "grow");
    ctx->prog_outer = compile_shader_program(SHADER_SRC_OUTER, "outer");
    ctx->prog_wave = compile_shader_program(SHADER_SRC_WAVE, "wave");
    ctx->prog_noise = compile_shader_program(SHADER_SRC_NOISE, "noise");
    ctx->prog_crosszoom = compile_shader_program(SHADER_SRC_CROSSZOOM, "crosszoom");
    ctx->prog_slide = compile_shader_program(SHADER_SRC_SLIDE, "slide");
    ctx->prog_glitch = compile_shader_program(SHADER_SRC_GLITCH, "glitch");
    ctx->prog_burn = compile_shader_program(SHADER_SRC_BURN, "burn");
    ctx->prog_ripple = compile_shader_program(SHADER_SRC_RIPPLE, "ripple");
    ctx->prog_pixelate = compile_shader_program(SHADER_SRC_PIXELATE, "pixelate");
    ctx->prog_doom = compile_shader_program(SHADER_SRC_DOOM, "doom");
    ctx->prog_swirl = compile_shader_program(SHADER_SRC_SWIRL, "swirl");
    ctx->prog_cube = compile_shader_program(SHADER_SRC_CUBE, "cube");
    ctx->prog_luma = compile_shader_program(SHADER_SRC_LUMA, "luma");
    ctx->prog_light_leak = compile_shader_program(SHADER_SRC_LIGHT_LEAK, "light_leak");
    ctx->prog_page_curl = compile_shader_program(SHADER_SRC_PAGE_CURL, "page_curl");
    ctx->prog_scale = compile_shader_program(SHADER_SRC_SCALE, "scale");
    ctx->prog_kawase = compile_shader_program(SHADER_SRC_KAWASE, "kawase");

    if (!ctx->prog_fade || !ctx->prog_wipe || !ctx->prog_grow || !ctx->prog_outer ||
        !ctx->prog_wave || !ctx->prog_noise || !ctx->prog_crosszoom || !ctx->prog_slide ||
        !ctx->prog_glitch || !ctx->prog_burn || !ctx->prog_ripple || !ctx->prog_pixelate ||
        !ctx->prog_doom || !ctx->prog_swirl || !ctx->prog_cube || !ctx->prog_luma ||
        !ctx->prog_light_leak || !ctx->prog_page_curl || !ctx->prog_scale || !ctx->prog_kawase) {
        WAYWAL_LOG_WARN("One or more compute shader programs failed to compile");
        gpu_compute_destroy(ctx);
        return NULL;
    }

    ctx->loc_scale_src_res = glGetUniformLocation(ctx->prog_scale, "u_src_res");
    ctx->loc_scale_dst_res = glGetUniformLocation(ctx->prog_scale, "u_dst_res");
    ctx->loc_scale_mode = glGetUniformLocation(ctx->prog_scale, "u_scaling_mode");
    ctx->loc_scale_clear = glGetUniformLocation(ctx->prog_scale, "u_clear_color");
    ctx->loc_scale_has_src = glGetUniformLocation(ctx->prog_scale, "u_has_src");
    ctx->loc_scale_span_bounds = glGetUniformLocation(ctx->prog_scale, "u_span_bounds");
    ctx->loc_scale_gravity = glGetUniformLocation(ctx->prog_scale, "u_crop_gravity");
    ctx->crop_gravity[0] = 0.5f;
    ctx->crop_gravity[1] = 0.5f;
    ctx->loc_kawase_src_res = glGetUniformLocation(ctx->prog_kawase, "u_src_res");
    ctx->loc_kawase_dst_res = glGetUniformLocation(ctx->prog_kawase, "u_dst_res");
    ctx->loc_kawase_radius = glGetUniformLocation(ctx->prog_kawase, "u_radius");

    /* Cache uniform locations at compile/link time to avoid per-frame driver lookups */
    query_program_uniforms(ctx->prog_fade, &ctx->unif_fade);
    query_program_uniforms(ctx->prog_wipe, &ctx->unif_wipe);
    query_program_uniforms(ctx->prog_grow, &ctx->unif_grow);
    query_program_uniforms(ctx->prog_outer, &ctx->unif_outer);
    query_program_uniforms(ctx->prog_wave, &ctx->unif_wave);
    query_program_uniforms(ctx->prog_noise, &ctx->unif_noise);
    query_program_uniforms(ctx->prog_crosszoom, &ctx->unif_crosszoom);
    query_program_uniforms(ctx->prog_slide, &ctx->unif_slide);
    query_program_uniforms(ctx->prog_glitch, &ctx->unif_glitch);
    query_program_uniforms(ctx->prog_burn, &ctx->unif_burn);
    query_program_uniforms(ctx->prog_ripple, &ctx->unif_ripple);
    query_program_uniforms(ctx->prog_pixelate, &ctx->unif_pixelate);
    query_program_uniforms(ctx->prog_doom, &ctx->unif_doom);
    query_program_uniforms(ctx->prog_swirl, &ctx->unif_swirl);
    query_program_uniforms(ctx->prog_cube, &ctx->unif_cube);
    query_program_uniforms(ctx->prog_luma, &ctx->unif_luma);
    query_program_uniforms(ctx->prog_light_leak, &ctx->unif_light_leak);
    query_program_uniforms(ctx->prog_page_curl, &ctx->unif_page_curl);

    /* Query fence sync extension entry points for GPU-to-Wayland handoff synchronization */
    ctx->eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    ctx->eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    ctx->eglClientWaitSyncKHR =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

    WAYWAL_LOG_INFO("GPU compute transition engine initialized (GLES 3.1 compute on GBM)");
    return ctx;
}

void gpu_compute_invalidate_state_cache(gpu_compute_ctx_t *ctx)
{
    if (!ctx)
        return;
    ctx->current_prog = 0;
    ctx->bound_target_tex = 0;
    ctx->bound_old_tex = 0;
    ctx->bound_new_tex = 0;
    ctx->bound_fbo_tex = 0;
}

static bool gpu_compute_import_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *bo)
{
    if (!ctx || !bo)
        return false;
    if (bo->egl_image && bo->gl_tex != 0) {
        return true; /* Already imported and cached */
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    static const EGLint plane_fd_keys[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                            EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint plane_offset_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint plane_pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint plane_mod_lo_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint plane_mod_hi_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    uint32_t num_planes = bo->num_planes > 0 ? bo->num_planes : 1;
    if (num_planes > 4)
        num_planes = 4;

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)bo->width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)bo->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)bo->drm_format;

    for (uint32_t p = 0; p < num_planes; ++p) {
        if (bo->fd[p] < 0)
            continue;
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_offset_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];

        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo_keys[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFF);
            attribs[n++] = plane_mod_hi_keys[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32) & 0xFFFFFFFF);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img =
        ctx->eglCreateImageKHR(ctx->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        WAYWAL_LOG_ERR("eglCreateImageKHR failed for BO %ux%u (0x%x)", bo->width, bo->height,
                       eglGetError());
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (ctx->glEGLImageTargetTexStorageEXT) {
        ctx->glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    bo->egl_image = img;
    bo->gl_tex = tex;
    ctx->bound_target_tex = 0;
    ctx->bound_old_tex = 0;
    ctx->bound_new_tex = 0;
    ctx->bound_fbo_tex = 0;
    return true;
}

static bool is_10bit_drm_format(uint32_t fmt)
{
    return fmt == DRM_FORMAT_ARGB2101010 || fmt == DRM_FORMAT_XRGB2101010 ||
           fmt == DRM_FORMAT_ABGR2101010 || fmt == DRM_FORMAT_XBGR2101010;
}

static bool needs_scratch_fbo_blit(const gpu_compute_ctx_t *ctx, const dmabuf_bo_t *bo)
{
    if (!ctx || !bo)
        return true;
    if (bo->is_10bit || is_10bit_drm_format(bo->drm_format))
        return true;
    if (ctx->glEGLImageTargetTexStorageEXT == NULL)
        return true;
    /* Any vendor-tiled or compressed modifier (DCC, CCS, AFBC, NVIDIA block-linear)
     * must be written through the FBO render backend so auxiliary compression planes
     * and tiling swizzles are updated by hardware ROPs. */
    if (bo->modifier != DRM_FORMAT_MOD_LINEAR && bo->modifier != DRM_FORMAT_MOD_INVALID)
        return true;
    if (bo->num_planes > 1)
        return true;
    return false;
}

static GLuint ensure_10bit_scratch_target(gpu_compute_ctx_t *ctx, uint32_t width, uint32_t height)
{
    if (ctx->scratch_10bit_tex != 0 && ctx->scratch_10bit_w == width &&
        ctx->scratch_10bit_h == height) {
        return ctx->scratch_10bit_tex;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (ctx->scratch_pool_tex[i] != 0 && ctx->scratch_pool_w[i] == width &&
            ctx->scratch_pool_h[i] == height) {
            ctx->scratch_10bit_tex = ctx->scratch_pool_tex[i];
            ctx->scratch_10bit_w = width;
            ctx->scratch_10bit_h = height;
            return ctx->scratch_10bit_tex;
        }
    }
    uint32_t slot = ctx->scratch_pool_next & 3u;
    ctx->scratch_pool_next++;
    if (ctx->scratch_pool_tex[slot] != 0) {
        if (ctx->bound_target_tex == ctx->scratch_pool_tex[slot]) {
            ctx->bound_target_tex = 0;
        }
        glDeleteTextures(1, &ctx->scratch_pool_tex[slot]);
        ctx->scratch_pool_tex[slot] = 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height);
    ctx->scratch_pool_tex[slot] = tex;
    ctx->scratch_pool_w[slot] = width;
    ctx->scratch_pool_h[slot] = height;
    ctx->scratch_10bit_tex = tex;
    ctx->scratch_10bit_w = width;
    ctx->scratch_10bit_h = height;
    ctx->bound_target_tex = 0;
    return ctx->scratch_10bit_tex;
}

static bool blit_scratch_to_10bit_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo)
{
    if (!ctx->prog_blit_10bit) {
        ctx->prog_blit_10bit = compile_blit_10bit_program();
        if (!ctx->prog_blit_10bit)
            return false;
    }
    if (!ctx->blit_10bit_fbo) {
        glGenFramebuffers(1, &ctx->blit_10bit_fbo);
        ctx->bound_fbo_tex = 0;
        ctx->scratch_fbo_validated = false;
    }
    if (!ctx->blit_10bit_vao) {
        glGenVertexArrays(1, &ctx->blit_10bit_vao);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->blit_10bit_fbo);
    if (ctx->bound_fbo_tex != target_bo->gl_tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               target_bo->gl_tex, 0);
        ctx->bound_fbo_tex = target_bo->gl_tex;
        if (!ctx->scratch_fbo_validated) {
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                ctx->bound_fbo_tex = 0;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return false;
            }
            ctx->scratch_fbo_validated = true;
        }
    }

    glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
    glUseProgram(ctx->prog_blit_10bit);
    ctx->current_prog = ctx->prog_blit_10bit;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->scratch_10bit_tex);
    glBindVertexArray(ctx->blit_10bit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ctx->bound_target_tex = 0;
    return true;
}

static void select_transition_program(gpu_compute_ctx_t *ctx, waywal_transition_type_t type,
                                      GLuint *out_prog, gpu_program_uniforms_t **out_u);

bool gpu_compute_dispatch_transition(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                     dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                     const waywal_transition_params_t *params)
{
    if (!ctx || !target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    if (!gpu_compute_import_bo(ctx, target_bo) || !gpu_compute_import_bo(ctx, old_bo) ||
        !gpu_compute_import_bo(ctx, new_bo)) {
        return false;
    }

    bool use_scratch = needs_scratch_fbo_blit(ctx, target_bo);
    GLuint dst_image_tex = target_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_10bit_scratch_target(ctx, target_bo->width, target_bo->height);
        if (!dst_image_tex)
            return false;
    }

    GLuint prog = 0;
    gpu_program_uniforms_t *u = NULL;
    if (params->progress <= 0.0f || params->progress >= 1.0f) {
        prog = ctx->prog_fade;
        u = &ctx->unif_fade;
    } else {
        select_transition_program(ctx, params->type, &prog, &u);
    }

    if (ctx->current_prog != prog) {
        glUseProgram(prog);
        ctx->current_prog = prog;
    }

    /* Bind target storage image to binding point 0 */
    if (ctx->bound_target_tex != dst_image_tex) {
        glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = dst_image_tex;
    }

    /* Bind source textures */
    if (ctx->bound_old_tex != old_bo->gl_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, old_bo->gl_tex);
        ctx->bound_old_tex = old_bo->gl_tex;
    }

    if (ctx->bound_new_tex != new_bo->gl_tex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, new_bo->gl_tex);
        ctx->bound_new_tex = new_bo->gl_tex;
    }

    /* Update uniforms only when values change to minimize driver validation overhead */
    if (u->loc_prog >= 0 && u->last_prog != params->progress) {
        glUniform1f(u->loc_prog, params->progress);
        u->last_prog = params->progress;
    }
    if (u->loc_res >= 0 &&
        (u->last_w != (GLint)target_bo->width || u->last_h != (GLint)target_bo->height)) {
        glUniform2i(u->loc_res, (GLint)target_bo->width, (GLint)target_bo->height);
        u->last_w = (GLint)target_bo->width;
        u->last_h = (GLint)target_bo->height;
    }
    if (u->loc_angle >= 0 && u->last_angle != params->angle_rad) {
        glUniform1f(u->loc_angle, params->angle_rad);
        u->last_angle = params->angle_rad;
    }
    if (u->loc_wave_freq >= 0) {
        float wf = params->wave_freq;
        if (u->last_wave_freq != wf) {
            glUniform1f(u->loc_wave_freq, wf);
            u->last_wave_freq = wf;
        }
    }
    if (u->loc_wave_amp >= 0) {
        float wa = params->wave_amp;
        if (u->last_wave_amp != wa) {
            glUniform1f(u->loc_wave_amp, wa);
            u->last_wave_amp = wa;
        }
    }
    if (u->loc_center >= 0 &&
        (u->last_center_x != params->center_x || u->last_center_y != params->center_y)) {
        glUniform2f(u->loc_center, params->center_x, params->center_y);
        u->last_center_x = params->center_x;
        u->last_center_y = params->center_y;
    }
    if (u->loc_smoothness >= 0) {
        float sm = params->wave_amp > 0.0f ? params->wave_amp : 0.2f;
        if (u->last_smoothness != sm) {
            glUniform1f(u->loc_smoothness, sm);
            u->last_smoothness = sm;
        }
    }
    if (u->loc_step >= 0) {
        float st = params->wave_freq > 0.0f ? params->wave_freq : 1.0f;
        if (u->last_step != st) {
            glUniform1f(u->loc_step, st);
            u->last_step = st;
        }
    }

    /* Dispatch compute shader (16x16 local workgroups) */
    GLuint groups_x = (target_bo->width + 15) / 16;
    GLuint groups_y = (target_bo->height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    /* Targeted memory barrier ensuring GPU storage writes are visible */
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!blit_scratch_to_10bit_bo(ctx, target_bo))
            return false;
    }

    if (ctx->eglCreateSyncKHR && ctx->eglClientWaitSyncKHR && ctx->eglDestroySyncKHR) {
        EGLSyncKHR sync = ctx->eglCreateSyncKHR(ctx->display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            ctx->eglClientWaitSyncKHR(ctx->display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                      EGL_FOREVER_KHR);
            ctx->eglDestroySyncKHR(ctx->display, sync);
        } else {
            glFlush();
            glFinish();
        }
    } else {
        glFinish();
    }

    return true;
}

static void select_transition_program(gpu_compute_ctx_t *ctx, waywal_transition_type_t type,
                                      GLuint *out_prog, gpu_program_uniforms_t **out_u)
{
    if (type == WAYWAL_TRANSITION_RANDOM) {
        type = (waywal_transition_type_t)(1 + (rand() % 19));
    }
    GLuint prog = ctx->prog_fade;
    gpu_program_uniforms_t *u = &ctx->unif_fade;
    switch (type) {
    case WAYWAL_TRANSITION_WIPE:
        prog = ctx->prog_wipe;
        u = &ctx->unif_wipe;
        break;
    case WAYWAL_TRANSITION_GROW:
        prog = ctx->prog_grow;
        u = &ctx->unif_grow;
        break;
    case WAYWAL_TRANSITION_OUTER:
        prog = ctx->prog_outer;
        u = &ctx->unif_outer;
        break;
    case WAYWAL_TRANSITION_WAVE:
        prog = ctx->prog_wave;
        u = &ctx->unif_wave;
        break;
    case WAYWAL_TRANSITION_NOISE:
        prog = ctx->prog_noise;
        u = &ctx->unif_noise;
        break;
    case WAYWAL_TRANSITION_CROSSZOOM:
        prog = ctx->prog_crosszoom;
        u = &ctx->unif_crosszoom;
        break;
    case WAYWAL_TRANSITION_SLIDE:
        prog = ctx->prog_slide;
        u = &ctx->unif_slide;
        break;
    case WAYWAL_TRANSITION_GLITCH:
        prog = ctx->prog_glitch;
        u = &ctx->unif_glitch;
        break;
    case WAYWAL_TRANSITION_BURN:
        prog = ctx->prog_burn;
        u = &ctx->unif_burn;
        break;
    case WAYWAL_TRANSITION_RIPPLE:
        prog = ctx->prog_ripple;
        u = &ctx->unif_ripple;
        break;
    case WAYWAL_TRANSITION_PIXELATE:
        prog = ctx->prog_pixelate;
        u = &ctx->unif_pixelate;
        break;
    case WAYWAL_TRANSITION_DOOM:
        prog = ctx->prog_doom;
        u = &ctx->unif_doom;
        break;
    case WAYWAL_TRANSITION_SWIRL:
        prog = ctx->prog_swirl;
        u = &ctx->unif_swirl;
        break;
    case WAYWAL_TRANSITION_CUBE:
        prog = ctx->prog_cube;
        u = &ctx->unif_cube;
        break;
    case WAYWAL_TRANSITION_LUMA:
        prog = ctx->prog_luma;
        u = &ctx->unif_luma;
        break;
    case WAYWAL_TRANSITION_LIGHT_LEAK:
        prog = ctx->prog_light_leak;
        u = &ctx->unif_light_leak;
        break;
    case WAYWAL_TRANSITION_PAGE_CURL:
        prog = ctx->prog_page_curl;
        u = &ctx->unif_page_curl;
        break;
    case WAYWAL_TRANSITION_CUSTOM:
        if (ctx->prog_custom) {
            prog = ctx->prog_custom;
            u = &ctx->unif_custom;
        } else {
            prog = ctx->prog_fade;
            u = &ctx->unif_fade;
        }
        break;
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE:
    default:
        prog = ctx->prog_fade;
        u = &ctx->unif_fade;
        break;
    }
    *out_prog = prog;
    *out_u = u;
}

bool gpu_compute_prepare_shm(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                             uint32_t *tex_target, uint32_t *fbo, uint32_t *cur_w, uint32_t *cur_h,
                             uint32_t width, uint32_t height, const uint32_t *old_pixels,
                             const uint32_t *new_pixels)
{
    if (!ctx || !tex_old || !tex_new || !tex_target || !fbo || !cur_w || !cur_h || width == 0 ||
        height == 0 || !old_pixels || !new_pixels) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    if (*cur_w != width || *cur_h != height || *tex_old == 0 || *tex_new == 0 || *tex_target == 0 ||
        *fbo == 0) {
        gpu_compute_release_shm_res(ctx, tex_old, tex_new, tex_target, fbo);

        GLuint texs[3] = {0, 0, 0};
        glGenTextures(3, texs);
        *tex_old = texs[0];
        *tex_new = texs[1];
        *tex_target = texs[2];

        for (int i = 0; i < 3; ++i) {
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height);
        }

        /* WL_SHM ARGB8888 in little-endian memory is [B, G, R, A]; swizzle sampler channels to
         * present R, G, B, A to compute shaders. */
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }

        GLuint my_fbo = 0;
        glGenFramebuffers(1, &my_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, my_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex_target, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        *fbo = my_fbo;

        *cur_w = width;
        *cur_h = height;
    }

    /* Upload source textures (format GL_RGBA with GL_UNSIGNED_BYTE on little-endian stores ARGB) */
    glBindTexture(GL_TEXTURE_2D, *tex_old);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA,
                    GL_UNSIGNED_BYTE, old_pixels);

    glBindTexture(GL_TEXTURE_2D, *tex_new);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA,
                    GL_UNSIGNED_BYTE, new_pixels);

    ctx->bound_target_tex = 0;
    ctx->bound_old_tex = 0;
    ctx->bound_new_tex = 0;

    return true;
}

bool gpu_compute_dispatch_transition_shm(gpu_compute_ctx_t *ctx, uint32_t tex_old, uint32_t tex_new,
                                         uint32_t tex_target, uint32_t fbo, uint32_t width,
                                         uint32_t height, const waywal_transition_params_t *params,
                                         uint32_t *dst_pixels)
{
    if (!ctx || tex_old == 0 || tex_new == 0 || tex_target == 0 || fbo == 0 || width == 0 ||
        height == 0 || !params || !dst_pixels) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    GLuint prog = 0;
    gpu_program_uniforms_t *u = NULL;
    if (params->progress <= 0.0f || params->progress >= 1.0f) {
        prog = ctx->prog_fade;
        u = &ctx->unif_fade;
    } else {
        select_transition_program(ctx, params->type, &prog, &u);
    }

    if (ctx->current_prog != prog) {
        glUseProgram(prog);
        ctx->current_prog = prog;
    }

    /* Bind target storage image */
    if (ctx->bound_target_tex != tex_target) {
        glBindImageTexture(0, tex_target, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = tex_target;
    }

    /* Bind sources */
    if (ctx->bound_old_tex != tex_old) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_old);
        ctx->bound_old_tex = tex_old;
    }

    if (ctx->bound_new_tex != tex_new) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex_new);
        ctx->bound_new_tex = tex_new;
    }

    if (u->loc_prog >= 0 && u->last_prog != params->progress) {
        glUniform1f(u->loc_prog, params->progress);
        u->last_prog = params->progress;
    }
    if (u->loc_res >= 0 && (u->last_w != (GLint)width || u->last_h != (GLint)height)) {
        glUniform2i(u->loc_res, (GLint)width, (GLint)height);
        u->last_w = (GLint)width;
        u->last_h = (GLint)height;
    }
    if (u->loc_angle >= 0 && u->last_angle != params->angle_rad) {
        glUniform1f(u->loc_angle, params->angle_rad);
        u->last_angle = params->angle_rad;
    }
    if (u->loc_wave_freq >= 0 && u->last_wave_freq != params->wave_freq) {
        glUniform1f(u->loc_wave_freq, params->wave_freq);
        u->last_wave_freq = params->wave_freq;
    }
    if (u->loc_wave_amp >= 0 && u->last_wave_amp != params->wave_amp) {
        glUniform1f(u->loc_wave_amp, params->wave_amp);
        u->last_wave_amp = params->wave_amp;
    }
    if (u->loc_center >= 0 &&
        (u->last_center_x != params->center_x || u->last_center_y != params->center_y)) {
        glUniform2f(u->loc_center, params->center_x, params->center_y);
        u->last_center_x = params->center_x;
        u->last_center_y = params->center_y;
    }
    if (u->loc_smoothness >= 0) {
        float sm = params->wave_amp > 0.0f ? params->wave_amp : 0.2f;
        if (u->last_smoothness != sm) {
            glUniform1f(u->loc_smoothness, sm);
            u->last_smoothness = sm;
        }
    }
    if (u->loc_step >= 0) {
        float st = params->wave_freq > 0.0f ? params->wave_freq : 1.0f;
        if (u->last_step != st) {
            glUniform1f(u->loc_step, st);
            u->last_step = st;
        }
    }

    GLuint groups_x = (width + 15) / 16;
    GLuint groups_y = (height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, dst_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
}

void gpu_compute_release_shm_res(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                                 uint32_t *tex_target, uint32_t *fbo)
{
    if (ctx && ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        }
    }
    if (tex_old && *tex_old != 0) {
        glDeleteTextures(1, tex_old);
        *tex_old = 0;
    }
    if (tex_new && *tex_new != 0) {
        glDeleteTextures(1, tex_new);
        *tex_new = 0;
    }
    if (tex_target && *tex_target != 0) {
        glDeleteTextures(1, tex_target);
        *tex_target = 0;
    }
    if (fbo && *fbo != 0) {
        glDeleteFramebuffers(1, fbo);
        *fbo = 0;
    }
    if (ctx) {
        ctx->bound_target_tex = 0;
        ctx->bound_old_tex = 0;
        ctx->bound_new_tex = 0;
    }
}

void gpu_compute_destroy(gpu_compute_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);

        if (ctx->prog_fade)
            glDeleteProgram(ctx->prog_fade);
        if (ctx->prog_wipe)
            glDeleteProgram(ctx->prog_wipe);
        if (ctx->prog_grow)
            glDeleteProgram(ctx->prog_grow);
        if (ctx->prog_outer)
            glDeleteProgram(ctx->prog_outer);
        if (ctx->prog_wave)
            glDeleteProgram(ctx->prog_wave);
        if (ctx->prog_noise)
            glDeleteProgram(ctx->prog_noise);
        if (ctx->prog_crosszoom)
            glDeleteProgram(ctx->prog_crosszoom);
        if (ctx->prog_slide)
            glDeleteProgram(ctx->prog_slide);
        if (ctx->prog_glitch)
            glDeleteProgram(ctx->prog_glitch);
        if (ctx->prog_burn)
            glDeleteProgram(ctx->prog_burn);
        if (ctx->prog_ripple)
            glDeleteProgram(ctx->prog_ripple);
        if (ctx->prog_pixelate)
            glDeleteProgram(ctx->prog_pixelate);
        if (ctx->prog_doom)
            glDeleteProgram(ctx->prog_doom);
        if (ctx->prog_swirl)
            glDeleteProgram(ctx->prog_swirl);
        if (ctx->prog_cube)
            glDeleteProgram(ctx->prog_cube);
        if (ctx->prog_luma)
            glDeleteProgram(ctx->prog_luma);
        if (ctx->prog_light_leak)
            glDeleteProgram(ctx->prog_light_leak);
        if (ctx->prog_page_curl)
            glDeleteProgram(ctx->prog_page_curl);
        if (ctx->prog_custom)
            glDeleteProgram(ctx->prog_custom);
        if (ctx->prog_scale)
            glDeleteProgram(ctx->prog_scale);
        if (ctx->prog_kawase)
            glDeleteProgram(ctx->prog_kawase);
        if (ctx->prog_blit_10bit)
            glDeleteProgram(ctx->prog_blit_10bit);
        if (ctx->upload_src_tex)
            glDeleteTextures(1, &ctx->upload_src_tex);
        if (ctx->kawase_half_tex)
            glDeleteTextures(1, &ctx->kawase_half_tex);
        for (uint32_t i = 0; i < 4; ++i) {
            if (ctx->scratch_pool_tex[i])
                glDeleteTextures(1, &ctx->scratch_pool_tex[i]);
        }
        if (ctx->blit_10bit_fbo)
            glDeleteFramebuffers(1, &ctx->blit_10bit_fbo);
        if (ctx->blit_10bit_vao)
            glDeleteVertexArrays(1, &ctx->blit_10bit_vao);

        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->context != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->display, ctx->context);
        }
        eglTerminate(ctx->display);
    }

    free(ctx);
}

void gpu_compute_release_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *bo)
{
    if (!ctx || !bo)
        return;
    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        }
    }
    if (bo->gl_tex != 0) {
        glDeleteTextures(1, &bo->gl_tex);
        bo->gl_tex = 0;
    }
    gpu_compute_invalidate_state_cache(ctx);
    if (bo->egl_image != NULL && ctx->eglDestroyImageKHR && ctx->display != EGL_NO_DISPLAY) {
        ctx->eglDestroyImageKHR(ctx->display, bo->egl_image);
        bo->egl_image = NULL;
    }
}

bool gpu_compute_load_custom_shader(gpu_compute_ctx_t *ctx, const char *src)
{
    if (!ctx || !src || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT)
        return false;

    eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);

    if (ctx->prog_custom) {
        glDeleteProgram(ctx->prog_custom);
        ctx->prog_custom = 0;
    }
    ctx->current_prog = 0;

    ctx->prog_custom = compile_shader_program(src, "custom_user_shader");
    if (!ctx->prog_custom) {
        WAYWAL_LOG_ERR("Failed to compile and link custom compute shader");
        return false;
    }

    query_program_uniforms(ctx->prog_custom, &ctx->unif_custom);
    WAYWAL_LOG_INFO("Loaded custom user compute shader successfully (prog=%u)", ctx->prog_custom);
    return true;
}

void gpu_compute_bump_upload_gen(gpu_compute_ctx_t *ctx)
{
    if (!ctx)
        return;
    ctx->upload_gen++;
    if (ctx->upload_gen == 0)
        ctx->upload_gen = 1;
}

void gpu_compute_release_upload_cache(gpu_compute_ctx_t *ctx)
{
    if (!ctx || ctx->upload_src_tex == 0)
        return;
    gpu_compute_make_current(ctx);
    if (ctx->bound_old_tex == ctx->upload_src_tex)
        ctx->bound_old_tex = 0;
    if (ctx->bound_new_tex == ctx->upload_src_tex)
        ctx->bound_new_tex = 0;
    glDeleteTextures(1, &ctx->upload_src_tex);
    ctx->upload_src_tex = 0;
    ctx->upload_src_w = 0;
    ctx->upload_src_h = 0;
    ctx->last_upload_ptr = NULL;
    ctx->last_upload_w = 0;
    ctx->last_upload_h = 0;
    ctx->last_upload_gen = 0;
}

static void upload_source_rgba_dedup(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                     uint32_t src_w, uint32_t src_h)
{
    if (ctx->upload_src_tex == 0 || ctx->upload_src_w != src_w || ctx->upload_src_h != src_h) {
        if (ctx->upload_src_tex != 0) {
            glDeleteTextures(1, &ctx->upload_src_tex);
            ctx->upload_src_tex = 0;
        }
        glGenTextures(1, &ctx->upload_src_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* Input pixels are little-endian ARGB8888 ([B, G, R, A]); swizzle to RGBA for shader
         * sampling. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)src_w, (GLsizei)src_h);
        ctx->upload_src_w = src_w;
        ctx->upload_src_h = src_h;
        ctx->last_upload_ptr = NULL;
        ctx->last_upload_gen = 0;
    }

    if (ctx->upload_gen != 0 && ctx->last_upload_gen == ctx->upload_gen &&
        ctx->last_upload_ptr == (const void *)src_rgba && ctx->last_upload_w == src_w &&
        ctx->last_upload_h == src_h) {
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)src_w, (GLsizei)src_h, GL_RGBA,
                    GL_UNSIGNED_BYTE, src_rgba);
    ctx->last_upload_ptr = (const void *)src_rgba;
    ctx->last_upload_w = src_w;
    ctx->last_upload_h = src_h;
    ctx->last_upload_gen = ctx->upload_gen;
}

static GLuint ensure_kawase_half_tex(gpu_compute_ctx_t *ctx, uint32_t width, uint32_t height)
{
    if (ctx->kawase_half_tex != 0 && ctx->kawase_half_w == width && ctx->kawase_half_h == height) {
        return ctx->kawase_half_tex;
    }
    if (ctx->kawase_half_tex != 0) {
        glDeleteTextures(1, &ctx->kawase_half_tex);
        ctx->kawase_half_tex = 0;
    }
    glGenTextures(1, &ctx->kawase_half_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->kawase_half_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height);
    ctx->kawase_half_w = width;
    ctx->kawase_half_h = height;
    return ctx->kawase_half_tex;
}

void gpu_compute_set_span_bounds(gpu_compute_ctx_t *ctx, int32_t off_x, int32_t off_y,
                                 int32_t total_w, int32_t total_h)
{
    if (!ctx)
        return;
    ctx->span_bounds[0] = off_x;
    ctx->span_bounds[1] = off_y;
    ctx->span_bounds[2] = total_w;
    ctx->span_bounds[3] = total_h;
}

void gpu_compute_set_crop_gravity(gpu_compute_ctx_t *ctx, float gx, float gy)
{
    if (!ctx)
        return;
    if (gx < 0.0f)
        gx = 0.0f;
    if (gx > 1.0f)
        gx = 1.0f;
    if (gy < 0.0f)
        gy = 0.0f;
    if (gy > 1.0f)
        gy = 1.0f;
    ctx->crop_gravity[0] = gx;
    ctx->crop_gravity[1] = gy;
}

bool gpu_compute_upload_static(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                               uint32_t src_h, dmabuf_bo_t *dst_bo, waywal_scaling_mode_t scaling,
                               uint32_t clear_color)
{
    if (!ctx || !dst_bo || dst_bo->width == 0 || dst_bo->height == 0 || !ctx->prog_scale) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    if (!gpu_compute_import_bo(ctx, dst_bo)) {
        return false;
    }

    bool has_src = (src_rgba != NULL && src_w > 0 && src_h > 0);
    if (has_src) {
        upload_source_rgba_dedup(ctx, src_rgba, src_w, src_h);
    }

    bool use_scratch = needs_scratch_fbo_blit(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_10bit_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex)
            return false;
    }

    glUseProgram(ctx->prog_scale);
    ctx->current_prog = ctx->prog_scale;

    glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ctx->bound_target_tex = dst_image_tex;

    if (has_src) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
        ctx->bound_old_tex = ctx->upload_src_tex;
    }

    float ca = (float)((clear_color >> 24) & 0xFF) / 255.0f;
    float cr = (float)((clear_color >> 16) & 0xFF) / 255.0f;
    float cg = (float)((clear_color >> 8) & 0xFF) / 255.0f;
    float cb = (float)(clear_color & 0xFF) / 255.0f;

    if (ctx->loc_scale_src_res >= 0)
        glUniform2i(ctx->loc_scale_src_res, (GLint)src_w, (GLint)src_h);
    if (ctx->loc_scale_dst_res >= 0)
        glUniform2i(ctx->loc_scale_dst_res, (GLint)dst_bo->width, (GLint)dst_bo->height);
    if (ctx->loc_scale_mode >= 0)
        glUniform1i(ctx->loc_scale_mode, (GLint)scaling);
    if (ctx->loc_scale_clear >= 0)
        glUniform4f(ctx->loc_scale_clear, cr, cg, cb, ca);
    if (ctx->loc_scale_has_src >= 0)
        glUniform1i(ctx->loc_scale_has_src, has_src ? 1 : 0);
    if (ctx->loc_scale_span_bounds >= 0)
        glUniform4i(ctx->loc_scale_span_bounds, ctx->span_bounds[0], ctx->span_bounds[1],
                    ctx->span_bounds[2], ctx->span_bounds[3]);
    if (ctx->loc_scale_gravity >= 0)
        glUniform2f(ctx->loc_scale_gravity, ctx->crop_gravity[0], ctx->crop_gravity[1]);

    GLuint groups_x = (dst_bo->width + 15) / 16;
    GLuint groups_y = (dst_bo->height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!blit_scratch_to_10bit_bo(ctx, dst_bo))
            return false;
    }

    if (ctx->eglCreateSyncKHR && ctx->eglClientWaitSyncKHR && ctx->eglDestroySyncKHR) {
        EGLSyncKHR sync = ctx->eglCreateSyncKHR(ctx->display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            ctx->eglClientWaitSyncKHR(ctx->display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                      EGL_FOREVER_KHR);
            ctx->eglDestroySyncKHR(ctx->display, sync);
        } else {
            glFlush();
            glFinish();
        }
    } else {
        glFinish();
    }

    return true;
}

void gpu_compute_make_current(gpu_compute_ctx_t *ctx)
{
    if (!ctx || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT)
        return;
    if (eglGetCurrentContext() != ctx->context) {
        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        gpu_compute_invalidate_state_cache(ctx);
    }
}

bool gpu_compute_scale_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *src_bo, dmabuf_bo_t *dst_bo,
                          waywal_scaling_mode_t scaling, uint32_t clear_color, bool release_src_gl)
{
    if (!ctx || !src_bo || !dst_bo || src_bo->width == 0 || src_bo->height == 0 ||
        dst_bo->width == 0 || dst_bo->height == 0 || !ctx->prog_scale) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (!gpu_compute_import_bo(ctx, src_bo) || !gpu_compute_import_bo(ctx, dst_bo)) {
        if (release_src_gl) {
            gpu_compute_release_bo(ctx, src_bo);
        }
        return false;
    }

    bool use_scratch = needs_scratch_fbo_blit(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_10bit_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex) {
            if (release_src_gl) {
                gpu_compute_release_bo(ctx, src_bo);
            }
            return false;
        }
    }

    glUseProgram(ctx->prog_scale);
    ctx->current_prog = ctx->prog_scale;

    glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ctx->bound_target_tex = dst_image_tex;

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, src_bo->gl_tex);
    ctx->bound_old_tex = src_bo->gl_tex;

    float ca = (float)((clear_color >> 24) & 0xFF) / 255.0f;
    float cr = (float)((clear_color >> 16) & 0xFF) / 255.0f;
    float cg = (float)((clear_color >> 8) & 0xFF) / 255.0f;
    float cb = (float)(clear_color & 0xFF) / 255.0f;

    if (ctx->loc_scale_src_res >= 0)
        glUniform2i(ctx->loc_scale_src_res, (GLint)src_bo->width, (GLint)src_bo->height);
    if (ctx->loc_scale_dst_res >= 0)
        glUniform2i(ctx->loc_scale_dst_res, (GLint)dst_bo->width, (GLint)dst_bo->height);
    if (ctx->loc_scale_mode >= 0)
        glUniform1i(ctx->loc_scale_mode, (GLint)scaling);
    if (ctx->loc_scale_clear >= 0)
        glUniform4f(ctx->loc_scale_clear, cr, cg, cb, ca);
    if (ctx->loc_scale_has_src >= 0)
        glUniform1i(ctx->loc_scale_has_src, 1);
    if (ctx->loc_scale_span_bounds >= 0)
        glUniform4i(ctx->loc_scale_span_bounds, ctx->span_bounds[0], ctx->span_bounds[1],
                    ctx->span_bounds[2], ctx->span_bounds[3]);
    if (ctx->loc_scale_gravity >= 0)
        glUniform2f(ctx->loc_scale_gravity, ctx->crop_gravity[0], ctx->crop_gravity[1]);

    GLuint groups_x = (dst_bo->width + 15) / 16;
    GLuint groups_y = (dst_bo->height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!blit_scratch_to_10bit_bo(ctx, dst_bo)) {
            if (release_src_gl) {
                gpu_compute_release_bo(ctx, src_bo);
            }
            return false;
        }
    }

    if (ctx->eglCreateSyncKHR && ctx->eglClientWaitSyncKHR && ctx->eglDestroySyncKHR) {
        EGLSyncKHR sync = ctx->eglCreateSyncKHR(ctx->display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            ctx->eglClientWaitSyncKHR(ctx->display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                      EGL_FOREVER_KHR);
            ctx->eglDestroySyncKHR(ctx->display, sync);
        } else {
            glFlush();
            glFinish();
        }
    } else {
        glFinish();
    }

    if (release_src_gl) {
        gpu_compute_release_bo(ctx, src_bo);
    }
    return true;
}

bool gpu_compute_render_kawase_blur(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                                    uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t radius)
{
    if (!ctx || !src_rgba || src_w == 0 || src_h == 0 || !dst_bo || dst_bo->width == 0 ||
        dst_bo->height == 0 || !ctx->prog_kawase) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
            gpu_compute_invalidate_state_cache(ctx);
        }
    }

    if (!gpu_compute_import_bo(ctx, dst_bo)) {
        return false;
    }

    upload_source_rgba_dedup(ctx, src_rgba, src_w, src_h);

    uint32_t half_w = dst_bo->width > 1u ? (dst_bo->width / 2u) : 1u;
    uint32_t half_h = dst_bo->height > 1u ? (dst_bo->height / 2u) : 1u;
    GLuint half_tex = ensure_kawase_half_tex(ctx, half_w, half_h);
    if (!half_tex) {
        return false;
    }

    glUseProgram(ctx->prog_kawase);
    ctx->current_prog = ctx->prog_kawase;

    /* Pass 1: Downsample + Kawase pre-blur into half_tex */
    glBindImageTexture(0, half_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ctx->bound_target_tex = half_tex;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
    ctx->bound_old_tex = ctx->upload_src_tex;
    if (ctx->loc_kawase_src_res >= 0)
        glUniform2i(ctx->loc_kawase_src_res, (GLint)src_w, (GLint)src_h);
    if (ctx->loc_kawase_dst_res >= 0)
        glUniform2i(ctx->loc_kawase_dst_res, (GLint)half_w, (GLint)half_h);
    if (ctx->loc_kawase_radius >= 0)
        glUniform1f(ctx->loc_kawase_radius, (float)(radius > 1u ? (radius / 2u) : 2u));

    glDispatchCompute((half_w + 15u) / 16u, (half_h + 15u) / 16u, 1u);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    /* Pass 2: Upsample + Kawase final blur from half_tex into dst_bo / scratch */
    bool use_scratch = needs_scratch_fbo_blit(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_10bit_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex)
            return false;
    }

    glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ctx->bound_target_tex = dst_image_tex;

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, half_tex);
    ctx->bound_old_tex = half_tex;

    if (ctx->loc_kawase_src_res >= 0)
        glUniform2i(ctx->loc_kawase_src_res, (GLint)half_w, (GLint)half_h);
    if (ctx->loc_kawase_dst_res >= 0)
        glUniform2i(ctx->loc_kawase_dst_res, (GLint)dst_bo->width, (GLint)dst_bo->height);
    if (ctx->loc_kawase_radius >= 0)
        glUniform1f(ctx->loc_kawase_radius, (float)(radius > 1u ? (radius / 2u) : 2u));

    GLuint groups_x = (dst_bo->width + 15u) / 16u;
    GLuint groups_y = (dst_bo->height + 15u) / 16u;
    glDispatchCompute(groups_x, groups_y, 1u);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!blit_scratch_to_10bit_bo(ctx, dst_bo))
            return false;
    }

    if (ctx->eglCreateSyncKHR && ctx->eglClientWaitSyncKHR && ctx->eglDestroySyncKHR) {
        EGLSyncKHR sync = ctx->eglCreateSyncKHR(ctx->display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            ctx->eglClientWaitSyncKHR(ctx->display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                      EGL_FOREVER_KHR);
            ctx->eglDestroySyncKHR(ctx->display, sync);
        } else {
            glFlush();
            glFinish();
        }
    } else {
        glFinish();
    }

    return true;
}

static inline uint32_t avg_u32_epu8_scalar(uint32_t a, uint32_t b)
{
    uint32_t rb = (((a & 0x00FF00FFu) + (b & 0x00FF00FFu) + 0x00010001u) >> 1) & 0x00FF00FFu;
    uint32_t ag =
        ((((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + 0x00010001u) >> 1) & 0x00FF00FFu;
    return rb | (ag << 8);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static void
kawase_pass_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict row_up,
                     const uint32_t *restrict row_cc, const uint32_t *restrict row_dn, uint32_t hw)
{
    if (hw == 0)
        return;

    /* x = 0 boundary */
    {
        uint32_t x1 = (hw > 1u) ? 1u : 0u;
        uint32_t d0 = avg_u32_epu8_scalar(row_up[0], row_up[x1]);
        uint32_t d1 = avg_u32_epu8_scalar(row_dn[0], row_dn[x1]);
        dst_row[0] = avg_u32_epu8_scalar(row_cc[0], avg_u32_epu8_scalar(d0, d1));
    }

    uint32_t x = 1u;
    for (; x + 8u < hw; x += 8u) {
        __m256i v_ul = _mm256_loadu_si256((const __m256i *)(row_up + x - 1u));
        __m256i v_ur = _mm256_loadu_si256((const __m256i *)(row_up + x + 1u));
        __m256i v_dl = _mm256_loadu_si256((const __m256i *)(row_dn + x - 1u));
        __m256i v_dr = _mm256_loadu_si256((const __m256i *)(row_dn + x + 1u));
        __m256i v_cc = _mm256_loadu_si256((const __m256i *)(row_cc + x));
        __m256i v_diag = _mm256_avg_epu8(_mm256_avg_epu8(v_ul, v_ur), _mm256_avg_epu8(v_dl, v_dr));
        __m256i v_out = _mm256_avg_epu8(v_cc, v_diag);
        _mm256_storeu_si256((__m256i *)(dst_row + x), v_out);
    }

    for (; x < hw; ++x) {
        uint32_t xm = (x > 0u) ? (x - 1u) : 0u;
        uint32_t xp = (x + 1u < hw) ? (x + 1u) : (hw - 1u);
        uint32_t d0 = avg_u32_epu8_scalar(row_up[xm], row_up[xp]);
        uint32_t d1 = avg_u32_epu8_scalar(row_dn[xm], row_dn[xp]);
        dst_row[x] = avg_u32_epu8_scalar(row_cc[x], avg_u32_epu8_scalar(d0, d1));
    }
}
#endif

static void kawase_pass_row_scalar(uint32_t *restrict dst_row, const uint32_t *restrict row_up,
                                   const uint32_t *restrict row_cc, const uint32_t *restrict row_dn,
                                   uint32_t hw)
{
    for (uint32_t x = 0; x < hw; ++x) {
        uint32_t xm = (x > 0u) ? (x - 1u) : 0u;
        uint32_t xp = (x + 1u < hw) ? (x + 1u) : (hw - 1u);
        uint32_t d0 = avg_u32_epu8_scalar(row_up[xm], row_up[xp]);
        uint32_t d1 = avg_u32_epu8_scalar(row_dn[xm], row_dn[xp]);
        dst_row[x] = avg_u32_epu8_scalar(row_cc[x], avg_u32_epu8_scalar(d0, d1));
    }
}

void layer_shell_blur_rgba_cpu(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    if (!src || !dst || w == 0 || h == 0)
        return;

    size_t total_bytes = (size_t)w * (size_t)h * 4u;
    uint32_t ds = (w >= 256u && h >= 256u) ? 4u : ((w >= 16u && h >= 16u) ? 2u : 1u);
    uint32_t hw = (w + ds - 1u) / ds;
    uint32_t hh = (h + ds - 1u) / ds;

    size_t pyr_bytes = (size_t)hw * (size_t)hh * sizeof(uint32_t);
    uint32_t *buf_a = (uint32_t *)malloc(pyr_bytes * 2u);
    if (!buf_a) {
        if (src != dst)
            memcpy(dst, src, total_bytes);
        return;
    }
    uint32_t *buf_b = buf_a + (size_t)hw * (size_t)hh;
    const uint32_t *src_u32 = (const uint32_t *)src;
    uint32_t *dst_u32 = (uint32_t *)dst;

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
    bool has_avx2 = __builtin_cpu_supports("avx2");
#endif

    /* Stage 1: Box Downsample (src -> buf_a) */
#pragma omp parallel for schedule(static)
    for (uint32_t hy = 0; hy < hh; ++hy) {
        uint32_t *out_row = buf_a + (size_t)hy * hw;
        uint32_t y0 = hy * ds;
        uint32_t y1 = (y0 + 1u < h) ? (y0 + 1u) : (h - 1u);
        const uint32_t *r0 = src_u32 + (size_t)y0 * w;
        const uint32_t *r1 = src_u32 + (size_t)y1 * w;

        if (ds == 4u) {
            uint32_t y2 = (y0 + 2u < h) ? (y0 + 2u) : (h - 1u);
            uint32_t y3 = (y0 + 3u < h) ? (y0 + 3u) : (h - 1u);
            const uint32_t *r2 = src_u32 + (size_t)y2 * w;
            const uint32_t *r3 = src_u32 + (size_t)y3 * w;
            for (uint32_t hx = 0; hx < hw; ++hx) {
                uint32_t x0 = hx * 4u;
                uint32_t x1 = (x0 + 1u < w) ? (x0 + 1u) : (w - 1u);
                uint32_t x2 = (x0 + 2u < w) ? (x0 + 2u) : (w - 1u);
                uint32_t x3 = (x0 + 3u < w) ? (x0 + 3u) : (w - 1u);
                uint32_t c0 = avg_u32_epu8_scalar(avg_u32_epu8_scalar(r0[x0], r0[x1]),
                                                  avg_u32_epu8_scalar(r0[x2], r0[x3]));
                uint32_t c1 = avg_u32_epu8_scalar(avg_u32_epu8_scalar(r1[x0], r1[x1]),
                                                  avg_u32_epu8_scalar(r1[x2], r1[x3]));
                uint32_t c2 = avg_u32_epu8_scalar(avg_u32_epu8_scalar(r2[x0], r2[x1]),
                                                  avg_u32_epu8_scalar(r2[x2], r2[x3]));
                uint32_t c3 = avg_u32_epu8_scalar(avg_u32_epu8_scalar(r3[x0], r3[x1]),
                                                  avg_u32_epu8_scalar(r3[x2], r3[x3]));
                out_row[hx] =
                    avg_u32_epu8_scalar(avg_u32_epu8_scalar(c0, c1), avg_u32_epu8_scalar(c2, c3));
            }
        } else if (ds == 2u) {
            for (uint32_t hx = 0; hx < hw; ++hx) {
                uint32_t x0 = hx * 2u;
                uint32_t x1 = (x0 + 1u < w) ? (x0 + 1u) : (w - 1u);
                uint32_t c0 = avg_u32_epu8_scalar(r0[x0], r0[x1]);
                uint32_t c1 = avg_u32_epu8_scalar(r1[x0], r1[x1]);
                out_row[hx] = avg_u32_epu8_scalar(c0, c1);
            }
        } else {
            memcpy(out_row, r0, (size_t)hw * sizeof(uint32_t));
        }
    }

    /* Stage 2: 2-Pass Dual-Filter Kawase Pyramid on hw x hh (buf_a -> buf_b -> buf_a) */
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t *in_pyr = (pass == 0) ? buf_a : buf_b;
        uint32_t *out_pyr = (pass == 0) ? buf_b : buf_a;
#pragma omp parallel for schedule(static)
        for (uint32_t hy = 0; hy < hh; ++hy) {
            uint32_t y_up = (hy > 0u) ? (hy - 1u) : 0u;
            uint32_t y_dn = (hy + 1u < hh) ? (hy + 1u) : (hh - 1u);
            const uint32_t *row_up = in_pyr + (size_t)y_up * hw;
            const uint32_t *row_cc = in_pyr + (size_t)hy * hw;
            const uint32_t *row_dn = in_pyr + (size_t)y_dn * hw;
            uint32_t *dst_row = out_pyr + (size_t)hy * hw;
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
            if (has_avx2) {
                kawase_pass_row_avx2(dst_row, row_up, row_cc, row_dn, hw);
                continue;
            }
#endif
            kawase_pass_row_scalar(dst_row, row_up, row_cc, row_dn, hw);
        }
    }

    /* Stage 3: Bilinear / Kawase Upsample from buf_a (hw x hh) to dst (w x h) */
#pragma omp parallel for schedule(static)
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t *dst_row = dst_u32 + (size_t)y * w;
        if (ds == 1u) {
            memcpy(dst_row, buf_a + (size_t)y * hw, (size_t)w * sizeof(uint32_t));
            continue;
        }
        uint32_t hy0 = y / ds;
        if (hy0 >= hh)
            hy0 = hh - 1u;
        uint32_t hy1 = (hy0 + 1u < hh) ? (hy0 + 1u) : (hh - 1u);
        uint32_t ry = y % ds;
        const uint32_t *r0 = buf_a + (size_t)hy0 * hw;
        const uint32_t *r1 = buf_a + (size_t)hy1 * hw;

        for (uint32_t x = 0; x < w; ++x) {
            uint32_t hx0 = x / ds;
            if (hx0 >= hw)
                hx0 = hw - 1u;
            uint32_t hx1 = (hx0 + 1u < hw) ? (hx0 + 1u) : (hw - 1u);
            uint32_t rx = x % ds;

            uint32_t top = (rx == 0u) ? r0[hx0] : avg_u32_epu8_scalar(r0[hx0], r0[hx1]);
            uint32_t bot = (rx == 0u) ? r1[hx0] : avg_u32_epu8_scalar(r1[hx0], r1[hx1]);
            dst_row[x] = (ry == 0u) ? top : avg_u32_epu8_scalar(top, bot);
        }
    }

    free(buf_a);
}
