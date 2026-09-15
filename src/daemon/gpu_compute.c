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

#include "shaders/shaders_embedded.h"
#include "waywal/log.h"

typedef struct {
    GLint loc_prog;
    GLint loc_res;
    GLint loc_angle;
    GLint loc_wave_freq;
    GLint loc_wave_amp;
    GLint loc_center;
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

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
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

gpu_compute_ctx_t *gpu_compute_create(dmabuf_context_t *dmabuf_ctx)
{
    if (!dmabuf_ctx || !dmabuf_ctx->gbm || !dmabuf_ctx->available) {
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

    if (!ctx->prog_fade || !ctx->prog_wipe || !ctx->prog_grow || !ctx->prog_outer ||
        !ctx->prog_wave || !ctx->prog_noise || !ctx->prog_crosszoom || !ctx->prog_slide ||
        !ctx->prog_glitch || !ctx->prog_burn || !ctx->prog_ripple || !ctx->prog_pixelate ||
        !ctx->prog_doom || !ctx->prog_swirl || !ctx->prog_cube || !ctx->prog_luma ||
        !ctx->prog_light_leak || !ctx->prog_page_curl) {
        WAYWAL_LOG_WARN("One or more compute shader programs failed to compile");
        gpu_compute_destroy(ctx);
        return NULL;
    }

    /* Cache uniform locations at compile/link time (PERF-01) */
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

    /* Query fence sync extension entry points (ARCH-03) */
    ctx->eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    ctx->eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");

    WAYWAL_LOG_INFO("GPU compute transition engine initialized (GLES 3.1 compute on GBM)");
    return ctx;
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
    if (img == EGL_NO_IMAGE_KHR && bo->modifier != DRM_FORMAT_MOD_INVALID) {
        n = 0;
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
        }
        attribs[n] = EGL_NONE;
        img = ctx->eglCreateImageKHR(ctx->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                                     attribs);
    }

    if (img == EGL_NO_IMAGE_KHR) {
        WAYWAL_LOG_ERR("eglCreateImageKHR failed for BO %ux%u (0x%x)", bo->width, bo->height,
                       eglGetError());
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    bo->egl_image = img;
    bo->gl_tex = tex;
    return true;
}

static void select_transition_program(gpu_compute_ctx_t *ctx, waywal_transition_type_t type,
                                      GLuint *out_prog, const gpu_program_uniforms_t **out_u);

bool gpu_compute_dispatch_transition(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                     dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                     const waywal_transition_params_t *params)
{
    if (!ctx || !target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (!gpu_compute_import_bo(ctx, target_bo) || !gpu_compute_import_bo(ctx, old_bo) ||
        !gpu_compute_import_bo(ctx, new_bo)) {
        return false;
    }

    GLuint prog = 0;
    const gpu_program_uniforms_t *u = NULL;
    select_transition_program(ctx, params->type, &prog, &u);

    glUseProgram(prog);

    /* Bind target storage image to binding point 0 */
    glBindImageTexture(0, target_bo->gl_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    /* Bind source textures */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, old_bo->gl_tex);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, new_bo->gl_tex);

    /* Set uniform parameters from cached locations (PERF-01) */
    if (u->loc_prog >= 0)
        glUniform1f(u->loc_prog, params->progress);
    if (u->loc_res >= 0)
        glUniform2i(u->loc_res, (GLint)target_bo->width, (GLint)target_bo->height);
    if (u->loc_angle >= 0)
        glUniform1f(u->loc_angle, params->angle_rad);
    if (u->loc_wave_freq >= 0) {
        glUniform1f(u->loc_wave_freq, params->wave_freq > 0.0f ? params->wave_freq : 20.0f);
    }
    if (u->loc_wave_amp >= 0) {
        glUniform1f(u->loc_wave_amp, params->wave_amp > 0.0f ? params->wave_amp : 0.05f);
    }
    if (u->loc_center >= 0) {
        glUniform2f(u->loc_center, params->center_x, params->center_y);
    }

    /* Dispatch compute shader (16x16 local workgroups) */
    GLuint groups_x = (target_bo->width + 15) / 16;
    GLuint groups_y = (target_bo->height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    /* Targeted memory barrier ensuring GPU storage writes are visible */
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glFlush();

    return true;
}

static void select_transition_program(gpu_compute_ctx_t *ctx, waywal_transition_type_t type,
                                      GLuint *out_prog, const gpu_program_uniforms_t **out_u)
{
    GLuint prog = ctx->prog_fade;
    const gpu_program_uniforms_t *u = &ctx->unif_fade;
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
    if (!ctx || !tex_old || !tex_new || !tex_target || !fbo || !cur_w || !cur_h ||
        width == 0 || height == 0 || !old_pixels || !new_pixels) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        }
    }

    if (*cur_w != width || *cur_h != height || *tex_old == 0 || *tex_new == 0 ||
        *tex_target == 0 || *fbo == 0) {
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

    return true;
}

bool gpu_compute_dispatch_transition_shm(gpu_compute_ctx_t *ctx, uint32_t tex_old,
                                         uint32_t tex_new, uint32_t tex_target,
                                         uint32_t fbo, uint32_t width, uint32_t height,
                                         const waywal_transition_params_t *params,
                                         uint32_t *dst_pixels)
{
    if (!ctx || tex_old == 0 || tex_new == 0 || tex_target == 0 || fbo == 0 ||
        width == 0 || height == 0 || !params || !dst_pixels) {
        return false;
    }

    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        }
    }

    GLuint prog = 0;
    const gpu_program_uniforms_t *u = NULL;
    select_transition_program(ctx, params->type, &prog, &u);

    glUseProgram(prog);

    /* Bind target storage image */
    glBindImageTexture(0, tex_target, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    /* Bind sources */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_old);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_new);

    if (u->loc_prog >= 0)
        glUniform1f(u->loc_prog, params->progress);
    if (u->loc_res >= 0)
        glUniform2i(u->loc_res, (GLint)width, (GLint)height);
    if (u->loc_angle >= 0)
        glUniform1f(u->loc_angle, params->angle_rad);
    if (u->loc_wave_freq >= 0) {
        glUniform1f(u->loc_wave_freq, params->wave_freq > 0.0f ? params->wave_freq : 20.0f);
    }
    if (u->loc_wave_amp >= 0) {
        glUniform1f(u->loc_wave_amp, params->wave_amp > 0.0f ? params->wave_amp : 0.05f);
    }
    if (u->loc_center >= 0) {
        glUniform2f(u->loc_center, params->center_x, params->center_y);
    }

    struct timespec t_a, t_b, t_c;
    clock_gettime(CLOCK_MONOTONIC, &t_a);

    GLuint groups_x = (width + 15) / 16;
    GLuint groups_y = (height + 15) / 16;
    glDispatchCompute(groups_x, groups_y, 1);

    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);

    clock_gettime(CLOCK_MONOTONIC, &t_b);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, dst_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    clock_gettime(CLOCK_MONOTONIC, &t_c);

    double comp_ms = (t_b.tv_sec - t_a.tv_sec) * 1000.0 + (t_b.tv_nsec - t_a.tv_nsec) / 1000000.0;
    double read_ms = (t_c.tv_sec - t_b.tv_sec) * 1000.0 + (t_c.tv_nsec - t_b.tv_nsec) / 1000000.0;

    static int s_log_count = 0;
    if (++s_log_count <= 6 || s_log_count % 30 == 0) {
        WAYWAL_LOG_INFO("GPU SHM dispatch (%ux%u): compute=%.2fms readpixels=%.2fms",
                        width, height, comp_ms, read_ms);
    }

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
    if (bo->gl_tex != 0) {
        glDeleteTextures(1, &bo->gl_tex);
        bo->gl_tex = 0;
    }
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

    ctx->prog_custom = compile_shader_program(src, "custom_user_shader");
    if (!ctx->prog_custom) {
        WAYWAL_LOG_ERR("Failed to compile and link custom compute shader");
        return false;
    }

    query_program_uniforms(ctx->prog_custom, &ctx->unif_custom);
    WAYWAL_LOG_INFO("Loaded custom user compute shader successfully (prog=%u)", ctx->prog_custom);
    return true;
}
