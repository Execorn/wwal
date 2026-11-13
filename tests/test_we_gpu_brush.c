// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_gpu_brush.h"
#include "waywal/we_scene.h"
#include "waywal/we_tex.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
__attribute__((visibility("default"))) const char *__lsan_default_suppressions(void);
const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:gbm_create_device\nleak:libgbm\nleak:libEGL\n"
           "leak:libGLESv2\nleak:dri\nleak:libnvidia\n";
}
#endif

#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#endif
#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#endif
#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#endif

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
} headless_gles_t;

static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static bool init_headless_gles31(headless_gles_t *out)
{
    memset(out, 0, sizeof(*out));
    out->drm_fd = -1;
    out->dpy = EGL_NO_DISPLAY;
    out->ctx = EGL_NO_CONTEXT;

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    static const EGLint ctx_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1,
                                         EGL_NONE};

    for (int idx = 128; idx < 136; ++idx) {
        char node_path[64];
        snprintf(node_path, sizeof(node_path), "/dev/dri/renderD%d", idx);
        int fd = open(node_path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct gbm_device *gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }

        EGLDisplay dpy = EGL_NO_DISPLAY;
        if (get_platform_dpy) {
            dpy = get_platform_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        }
        if (dpy == EGL_NO_DISPLAY) {
            dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
        }
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            eglTerminate(dpy);
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT) {
            static const EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                                 EGL_SURFACE_TYPE, 0, EGL_NONE};
            EGLConfig cfg = NULL;
            EGLint num_cfg = 0;
            if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg) && num_cfg > 0) {
                ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
            }
        }
        if (ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            out->drm_fd = fd;
            out->gbm = gbm;
            out->dpy = dpy;
            out->ctx = ctx;
            return true;
        }
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(fd);
    }

    if (get_platform_dpy) {
        EGLDisplay dpy = get_platform_dpy(0x31DD, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL) &&
            eglBindAPI(EGL_OPENGL_ES_API)) {
            EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
            if (ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
                out->dpy = dpy;
                out->ctx = ctx;
                return true;
            }
            if (ctx != EGL_NO_CONTEXT)
                eglDestroyContext(dpy, ctx);
            eglTerminate(dpy);
        }
    }
    return false;
}

static void destroy_headless_gles31(headless_gles_t *gl)
{
    if (gl->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gl->ctx != EGL_NO_CONTEXT)
            eglDestroyContext(gl->dpy, gl->ctx);
        eglTerminate(gl->dpy);
    }
    if (gl->gbm)
        gbm_device_destroy(gl->gbm);
    if (gl->drm_fd >= 0)
        close(gl->drm_fd);
}

static void assert_no_gl_error(const char *where)
{
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "[GL ERROR] %s: 0x%x\n", where, (unsigned)err);
        assert(err == GL_NO_ERROR);
    }
}

typedef struct {
    we_scene_t scene;
    we_scene_object_t obj;
    we_tex_t tex;
} test_canvas_t;

static void init_test_canvas(test_canvas_t *c, we_tex_format_t fmt, uint32_t w, uint32_t h,
                             GLenum internal_fmt, GLenum src_fmt, const void *initial_data,
                             bool apply_we_swizzle)
{
    memset(c, 0, sizeof(*c));
    c->scene.objects = &c->obj;
    c->scene.object_count = 1;
    memset(c->scene.current_tex2d, 0xFF, sizeof(c->scene.current_tex2d));
    c->scene.current_active_unit = 0xFFFFFFFFu;

    c->obj.id = 42;
    c->obj.parent_id = -1;
    c->obj.has_base_pass = true;
    c->obj.base_pass.textures[0] = &c->tex;

    c->tex.format = fmt;
    c->tex.texture_width = w;
    c->tex.texture_height = h;
    c->tex.image_width = w;
    c->tex.image_height = h;
    c->tex.image_count = 1;

    GLuint gl_tex = 0;
    glGenTextures(1, &gl_tex);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (apply_we_swizzle && fmt == WE_TEX_FMT_RG88) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
    } else if (apply_we_swizzle && fmt == WE_TEX_FMT_R8) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_fmt, (GLsizei)w, (GLsizei)h, 0, src_fmt,
                 GL_UNSIGNED_BYTE, initial_data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    c->tex.gl_tex = (uint32_t)gl_tex;
    assert_no_gl_error("init_test_canvas");
}

static void destroy_test_canvas(test_canvas_t *c)
{
    if (c->tex.gl_tex) {
        GLuint t = (GLuint)c->tex.gl_tex;
        glDeleteTextures(1, &t);
        c->tex.gl_tex = 0;
    }
}

static void test_abi_and_flow_helpers(void)
{
    printf("[+] Verifying struct layouts & RG88 (128, 128) rest encoding helpers...\n");
    uint8_t r = 0, g = 0;
    we_brush_encode_flow_u8(0.0f, 0.0f, &r, &g);
    assert(r == WE_BRUSH_FLOW_NEUTRAL_U8 && g == WE_BRUSH_FLOW_NEUTRAL_U8);

    we_brush_encode_flow_u8(1.0f, -1.0f, &r, &g);
    assert(r == 255u && g == 1u);

    we_brush_encode_flow_u8(-1.0f, 1.0f, &r, &g);
    assert(r == 1u && g == 255u);
}

static void test_r8_mask_capsule_and_ops(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing R8 capsule SDF mask brush (ADD_BUILDUP, SUBTRACT, ERASE)...\n");
    const uint32_t w = 256, h = 256;
    uint8_t *init_r8 = calloc((size_t)w * h, 1u);
    assert(init_r8 != NULL);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_R8, w, h, GL_R8, GL_RED, init_r8, true);
    free(init_r8);

    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 20.0f,
        .hardness = 0.7f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .target_value = 1.0f,
    };
    assert(we_gpu_brush_begin_stroke(ctx, 1001u, &p));

    we_brush_dab_t dab = {
        .ax = 64.0f,
        .ay = 128.0f,
        .bx = 192.0f,
        .by = 128.0f,
        .radius_a = 20.0f,
        .radius_b = 10.0f,
        .opacity_eff = 1.0f,
    };
    we_brush_dirty_rect_t dirty = {0};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, &dirty));
    assert(dirty.width > 128 && dirty.height > 20);

    uint32_t tiles = 0;
    assert(we_gpu_brush_end_stroke(ctx, &tiles));
    assert(tiles > 0);
    assert_no_gl_error("r8_mask_paint");

    uint8_t *pixels = NULL;
    size_t byte_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &pixels, &byte_len));
    assert(byte_len == (size_t)w * h);

    /* Center of capsule is near 255; outside region remains 0; tapered end shrinks cleanly. */
    assert(pixels[128u * w + 64u] >= 250u);
    assert(pixels[128u * w + 128u] >= 250u);
    assert(pixels[128u * w + 192u] >= 250u);
    assert(pixels[(128u + 16u) * w + 64u] > 10u);
    assert(pixels[(128u + 16u) * w + 192u] == 0u);
    assert(pixels[10u * w + 10u] == 0u);
    free(pixels);

    /* Test ERASE over left endpoint. */
    p.op_mode = WE_BRUSH_OP_ERASE;
    assert(we_gpu_brush_begin_stroke(ctx, 1002u, &p));
    dab.bx = 64.0f;
    dab.radius_b = 20.0f;
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, &dirty));
    assert(we_gpu_brush_end_stroke(ctx, &tiles));

    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &pixels, &byte_len));
    assert(pixels[128u * w + 64u] <= 5u);
    assert(pixels[128u * w + 192u] >= 250u);
    free(pixels);

    destroy_test_canvas(&canvas);
    assert_no_gl_error("r8_mask_cleanup");
}

static void test_rg88_swizzle_and_rest_preservation(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing RG88 (R,R,R,G) copy-in normalization & exact (128,128) rest...\n");
    const uint32_t w = 256, h = 256;
    uint8_t *init_rg = malloc((size_t)w * h * 2u);
    assert(init_rg != NULL);
    memset(init_rg, WE_BRUSH_FLOW_NEUTRAL_U8, (size_t)w * h * 2u);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RG88, w, h, GL_RG8, GL_RG, init_rg, true);
    free(init_rg);

    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_FLOW_RG88,
        .op_mode = WE_BRUSH_OP_FLOW_PAINT,
        .radius_px = 24.0f,
        .hardness = 0.8f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .flow_strength = 1.0f,
    };
    assert(we_gpu_brush_begin_stroke(ctx, 2001u, &p));

    we_brush_dab_t dab = {
        .ax = 128.0f,
        .ay = 128.0f,
        .bx = 128.0f,
        .by = 128.0f,
        .radius_a = 24.0f,
        .radius_b = 24.0f,
        .opacity_eff = 1.0f,
        .flow_dir_x = 1.0f,
        .flow_dir_y = 0.0f,
    };
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *pixels = NULL;
    size_t byte_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RG88, &pixels, &byte_len));
    assert(byte_len == (size_t)w * h * 2u);

    /* Untouched pixels must remain bit-exact (128, 128). */
    for (uint32_t y = 0; y < 32u; y++) {
        for (uint32_t x = 0; x < 32u; x++) {
            size_t idx = ((size_t)y * w + x) * 2u;
            assert(pixels[idx] == 128u && pixels[idx + 1u] == 128u);
        }
    }

    /* Center pixel must be (+X, 0) -> (255, 128). */
    size_t cidx = ((size_t)128u * w + 128u) * 2u;
    assert(pixels[cidx] >= 253u);
    assert(pixels[cidx + 1u] == 128u);
    free(pixels);

    /* Verify invert_flow_y flag with FLOW_PAINT (+Y negated -> -Y = (128, 1)). */
    assert(we_gpu_brush_begin_stroke(ctx, 2002u, &p));
    dab.flow_dir_x = 0.0f;
    dab.flow_dir_y = 1.0f;
    dab.flags = 1u;
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RG88, &pixels, &byte_len));
    assert(pixels[cidx] == 128u);
    assert(pixels[cidx + 1u] <= 2u);
    free(pixels);

    /* Verify FLOW_ERASE restores center to exact (128, 128). */
    p.op_mode = WE_BRUSH_OP_FLOW_ERASE;
    assert(we_gpu_brush_begin_stroke(ctx, 2003u, &p));
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RG88, &pixels, &byte_len));
    assert(pixels[cidx] == 128u && pixels[cidx + 1u] == 128u);
    free(pixels);

    destroy_test_canvas(&canvas);
    assert_no_gl_error("rg88_flow");
}

static void test_rgba8_fbo_promotion_fallback(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing GL_RGBA8 promotion fallback on non-renderable GL_RGB8 target...\n");
    const uint32_t w = 128, h = 128;
    uint8_t *init_rgb = malloc((size_t)w * h * 3u);
    assert(init_rgb != NULL);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        init_rgb[i * 3u + 0u] = 10u;
        init_rgb[i * 3u + 1u] = 20u;
        init_rgb[i * 3u + 2u] = 30u;
    }

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGB8, GL_RGB, init_rgb, false);
    free(init_rgb);

    uint32_t old_tex = canvas.tex.gl_tex;
    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));
    assert(canvas.tex.gl_tex != 0 && canvas.tex.gl_tex != old_tex);

    GLint promoted_fmt = 0;
    glBindTexture(GL_TEXTURE_2D, canvas.tex.gl_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &promoted_fmt);
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(promoted_fmt == GL_RGBA8);

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_COLOR_RGBA8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 16.0f,
        .hardness = 0.8f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .color_rgba = {0.9f, 0.2f, 0.4f, 1.0f},
    };
    assert(we_gpu_brush_begin_stroke(ctx, 3001u, &p));
    we_brush_dab_t dab = {.ax = 64.0f,
                          .ay = 64.0f,
                          .bx = 64.0f,
                          .by = 64.0f,
                          .radius_a = 16.0f,
                          .radius_b = 16.0f,
                          .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *pixels = NULL;
    size_t byte_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &pixels, &byte_len));
    /* Untouched border keeps original RGB (10, 20, 30); center has painted color. */
    assert(pixels[0] == 10u && pixels[1] == 20u && pixels[2] == 30u);
    size_t cidx = ((size_t)64u * w + 64u) * 4u;
    assert(pixels[cidx + 0u] > 210u && pixels[cidx + 1u] < 70u && pixels[cidx + 2u] > 80u);
    free(pixels);

    destroy_test_canvas(&canvas);
    assert_no_gl_error("fbo_fallback");
}

static void test_blur_and_smudge(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing Gaussian (<=4px), Dual-Kawase (>4px) blur & smudge advection...\n");
    const uint32_t w = 256, h = 256;
    uint8_t *init_rgba = malloc((size_t)w * h * 4u);
    assert(init_rgba != NULL);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = (x < 128u) ? 255u : 0u;
            size_t idx = ((size_t)y * w + x) * 4u;
            init_rgba[idx + 0u] = v;
            init_rgba[idx + 1u] = v;
            init_rgba[idx + 2u] = v;
            init_rgba[idx + 3u] = 255u;
        }
    }

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGBA8, GL_RGBA, init_rgba, false);
    free(init_rgba);

    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    /* 1. Gaussian blur (radius 3.0 px) at (128, 64). */
    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_BLUR,
        .op_mode = WE_BRUSH_OP_BLUR_MASKED,
        .radius_px = 24.0f,
        .hardness = 0.8f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .blur_radius_px = 3.0f,
    };
    assert(we_gpu_brush_begin_stroke(ctx, 4001u, &p));
    we_brush_dab_t dab = {.ax = 128.0f,
                          .ay = 64.0f,
                          .bx = 128.0f,
                          .by = 64.0f,
                          .radius_a = 24.0f,
                          .radius_b = 24.0f,
                          .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    /* 2. Dual-Kawase blur (radius 16.0 px) at (128, 192). */
    p.blur_radius_px = 16.0f;
    assert(we_gpu_brush_begin_stroke(ctx, 4002u, &p));
    dab.ay = 192.0f;
    dab.by = 192.0f;
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *pixels = NULL;
    size_t byte_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &pixels, &byte_len));
    /* Blurred boundary pixels right of x=128 should gain brightness (> 20) while x=240 stays 0. */
    assert(pixels[((size_t)64u * w + 129u) * 4u] > 20u);
    assert(pixels[((size_t)192u * w + 134u) * 4u] > 20u);
    assert(pixels[((size_t)64u * w + 240u) * 4u] == 0u);
    free(pixels);

    /* 3. Smudge advection rightward across x=128 edge at y=128. */
    p.channel = WE_BRUSH_CHAN_SMUDGE;
    p.op_mode = WE_BRUSH_OP_SMUDGE_ADV;
    p.blur_radius_px = 2.0f;
    assert(we_gpu_brush_begin_stroke(ctx, 4003u, &p));
    dab = (we_brush_dab_t){.ax = 126.0f,
                           .ay = 128.0f,
                           .bx = 136.0f,
                           .by = 128.0f,
                           .radius_a = 16.0f,
                           .radius_b = 16.0f,
                           .opacity_eff = 1.0f,
                           .drag_dx_px = 16.0f,
                           .drag_dy_px = 0.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &pixels, &byte_len));
    assert(pixels[((size_t)128u * w + 134u) * 4u] > 100u);
    free(pixels);

    destroy_test_canvas(&canvas);
    assert_no_gl_error("blur_and_smudge");
}

static void test_cow_undo_redo_and_chunking(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing 64x64 CoW undo/redo bit-exactness & >1024px dab chunking...\n");
    const uint32_t w = 256, h = 256;
    uint8_t *init_r8 = malloc((size_t)w * h);
    assert(init_r8 != NULL);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        init_r8[i] = (uint8_t)(i & 0x3Fu);
    }

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_R8, w, h, GL_R8, GL_RED, init_r8, true);
    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 20.0f,
        .hardness = 0.7f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .target_value = 1.0f,
    };

    /* Stroke 1 */
    assert(we_gpu_brush_begin_stroke(ctx, 5001u, &p));
    we_brush_dab_t dab1 = {.ax = 40.0f,
                           .ay = 40.0f,
                           .bx = 80.0f,
                           .by = 40.0f,
                           .radius_a = 20.0f,
                           .radius_b = 20.0f,
                           .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab1, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *after_s1 = NULL;
    size_t len1 = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &after_s1, &len1));

    /* Stroke 2 */
    assert(we_gpu_brush_begin_stroke(ctx, 5002u, &p));
    we_brush_dab_t dab2 = {.ax = 160.0f,
                           .ay = 160.0f,
                           .bx = 200.0f,
                           .by = 160.0f,
                           .radius_a = 20.0f,
                           .radius_b = 20.0f,
                           .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab2, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *after_s2 = NULL;
    size_t len2 = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &after_s2, &len2));
    assert(memcmp(after_s1, after_s2, len1) != 0);

    /* Undo stroke 2 -> must match after_s1 bit-for-bit. */
    assert(we_gpu_brush_undo_stroke(ctx, 5002u));
    uint8_t *cur = NULL;
    size_t cur_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &cur, &cur_len));
    assert(memcmp(cur, after_s1, len1) == 0);
    free(cur);

    /* Undo stroke 1 -> must match init_r8 bit-for-bit. */
    assert(we_gpu_brush_undo_stroke(ctx, 5001u));
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &cur, &cur_len));
    assert(memcmp(cur, init_r8, len1) == 0);
    free(cur);

    /* Redo stroke 1 and stroke 2 -> must match after_s1 then after_s2 bit-for-bit. */
    assert(we_gpu_brush_redo_stroke(ctx, 5001u));
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &cur, &cur_len));
    assert(memcmp(cur, after_s1, len1) == 0);
    free(cur);

    assert(we_gpu_brush_redo_stroke(ctx, 5002u));
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &cur, &cur_len));
    assert(memcmp(cur, after_s2, len2) == 0);
    free(cur);

    free(after_s1);
    free(after_s2);
    free(init_r8);
    destroy_test_canvas(&canvas);

    /* Large radius chunking (> 1024px padded bbox) */
    const uint32_t lw = 1536, lh = 1536;
    uint8_t *big_buf = calloc((size_t)lw * lh, 1u);
    assert(big_buf != NULL);
    init_test_canvas(&canvas, WE_TEX_FMT_R8, lw, lh, GL_R8, GL_RED, big_buf, true);
    free(big_buf);
    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));
    p.radius_px = 512.0f;
    assert(we_gpu_brush_begin_stroke(ctx, 5003u, &p));
    we_brush_dab_t big_dab = {.ax = 768.0f,
                              .ay = 768.0f,
                              .bx = 768.0f,
                              .by = 768.0f,
                              .radius_a = 512.0f,
                              .radius_b = 512.0f,
                              .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &big_dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &cur, &cur_len));
    assert(cur[768u * lw + 768u] >= 250u);
    assert(cur[0] == 0u);
    free(cur);

    destroy_test_canvas(&canvas);

    /* Verify saved min/mag filters survive dabs + end_stroke and non-finite dabs are skipped. */
    init_test_canvas(&canvas, WE_TEX_FMT_RGBA8888, 128u, 128u, GL_RGBA8, GL_RGBA, NULL, false);
    glBindTexture(GL_TEXTURE_2D, canvas.tex.gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t cp = {
        .channel = WE_BRUSH_CHAN_COLOR_RGBA8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 16.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .color_rgba = {1.0f, 0.5f, 0.25f, 1.0f},
    };
    assert(we_gpu_brush_begin_stroke(ctx, 5004u, &cp));
    we_brush_dab_t mixed_dabs[3] = {
        {.ax = NAN,
         .ay = 64.0f,
         .bx = 64.0f,
         .by = 64.0f,
         .radius_a = 10.0f,
         .radius_b = 10.0f,
         .opacity_eff = 1.0f},
        {.ax = 1e20f,
         .ay = 64.0f,
         .bx = 1e20f,
         .by = 64.0f,
         .radius_a = 10.0f,
         .radius_b = 10.0f,
         .opacity_eff = 1.0f},
        {.ax = 64.0f,
         .ay = 64.0f,
         .bx = 64.0f,
         .by = 64.0f,
         .radius_a = 16.0f,
         .radius_b = 16.0f,
         .opacity_eff = 1.0f},
    };
    assert(we_gpu_brush_dispatch_dabs(ctx, mixed_dabs, 3u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    GLint min_f = 0, mag_f = 0;
    glBindTexture(GL_TEXTURE_2D, canvas.tex.gl_tex);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_f);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_f);
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(min_f == GL_LINEAR);
    assert(mag_f == GL_LINEAR);

    destroy_test_canvas(&canvas);
    assert_no_gl_error("cow_undo_redo_and_chunking");
}

static void test_cow_pool_eviction_on_overflow(void)
{
    printf("[+] Testing CoW tile pool FIFO eviction under tight slot budget...\n");
    we_gpu_brush_ctx_t *small = we_gpu_brush_create(8u);
    assert(small != NULL);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_R8, 256u, 256u, GL_R8, GL_RED, NULL, false);
    assert(we_gpu_brush_bind_target(small, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 4.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
    };

    /* Empty leading strokes (0 dirty tiles) must be skipped cleanly when evicting for a slot. */
    for (int e = 0; e < 2; e++) {
        uint32_t cap_cnt = 99u;
        assert(we_gpu_brush_begin_stroke(small, (uint64_t)(7990 + e), &p));
        we_brush_dab_t oob = {.ax = -200.0f,
                              .ay = -200.0f,
                              .bx = -200.0f,
                              .by = -200.0f,
                              .radius_a = 4.0f,
                              .radius_b = 4.0f,
                              .opacity_eff = 1.0f};
        assert(we_gpu_brush_dispatch_dabs(small, &oob, 1u, NULL));
        assert(we_gpu_brush_end_stroke(small, &cap_cnt));
        assert(cap_cnt == 0u);
    }

    /* 10 disjoint single-tile strokes on an 8-slot pool must evict past the empty strokes. */
    for (int i = 0; i < 10; i++) {
        assert(we_gpu_brush_begin_stroke(small, (uint64_t)(8000 + i), &p));
        float cx = (float)(32 + (i % 4) * 64);
        float cy = (float)(32 + (i / 4) * 64);
        we_brush_dab_t dab = {.ax = cx,
                              .ay = cy,
                              .bx = cx,
                              .by = cy,
                              .radius_a = 4.0f,
                              .radius_b = 4.0f,
                              .opacity_eff = 1.0f};
        assert(we_gpu_brush_dispatch_dabs(small, &dab, 1u, NULL));
        assert(we_gpu_brush_end_stroke(small, NULL));
    }

    assert(!we_gpu_brush_undo_stroke(small, 8000u));
    assert(we_gpu_brush_undo_stroke(small, 8009u));

    we_gpu_brush_destroy(small);
    destroy_test_canvas(&canvas);
    assert_no_gl_error("cow_pool_eviction");
}

static void benchmark_4k_brush_and_undo(void)
{
    printf("[+] Benchmarking 4K (3840x2160) r=128px brush dabs & 50-stroke CoW undo/redo...\n");
    const uint32_t w = 3840, h = 2160;
    we_gpu_brush_ctx_t *bench_ctx = we_gpu_brush_create(WE_BRUSH_POOL_DEFAULT_CAPS);
    assert(bench_ctx != NULL);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGBA8, GL_RGBA, NULL, false);
    assert(we_gpu_brush_bind_target(bench_ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t mask_p = {
        .channel = WE_BRUSH_CHAN_COLOR_RGBA8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 128.0f,
        .hardness = 0.7f,
        .falloff_power = 1.0f,
        .opacity = 0.8f,
        .color_rgba = {0.22f, 0.74f, 0.97f, 1.0f},
    };

    /* Warmup */
    assert(we_gpu_brush_begin_stroke(bench_ctx, 9000u, &mask_p));
    we_brush_dab_t dab = {.ax = 1920.0f,
                          .ay = 1080.0f,
                          .bx = 1960.0f,
                          .by = 1090.0f,
                          .radius_a = 128.0f,
                          .radius_b = 128.0f,
                          .opacity_eff = 0.8f};
    assert(we_gpu_brush_dispatch_dabs(bench_ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(bench_ctx, NULL));
    glFinish();

    /* Benchmark 50 color dabs at r=128 px */
    assert(we_gpu_brush_begin_stroke(bench_ctx, 9050u, &mask_p));
    glFinish();
    uint64_t t0 = get_time_us();
    for (int i = 0; i < 50; i++) {
        dab.ax = 600.0f + (float)(i % 10) * 200.0f;
        dab.bx = dab.ax + 24.0f;
        dab.ay = 500.0f + (float)(i / 10) * 200.0f;
        dab.by = dab.ay + 12.0f;
        assert(we_gpu_brush_dispatch_dabs(bench_ctx, &dab, 1u, NULL));
    }
    glFinish();
    uint64_t t1 = get_time_us();
    assert(we_gpu_brush_end_stroke(bench_ctx, NULL));
    double avg_color_ms = (double)(t1 - t0) / 50000.0;

    /* Benchmark Dual-Kawase blur r=128 px, blur=16 px */
    we_brush_params_t blur_p = {
        .channel = WE_BRUSH_CHAN_BLUR,
        .op_mode = WE_BRUSH_OP_BLUR_MASKED,
        .radius_px = 128.0f,
        .hardness = 0.5f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .blur_radius_px = 16.0f,
    };
    assert(we_gpu_brush_begin_stroke(bench_ctx, 9060u, &blur_p));
    dab.ax = 1920.0f;
    dab.ay = 1080.0f;
    dab.bx = 1940.0f;
    dab.by = 1080.0f;
    glFinish();
    uint64_t b0 = get_time_us();
    for (int i = 0; i < 16; i++) {
        assert(we_gpu_brush_dispatch_dabs(bench_ctx, &dab, 1u, NULL));
    }
    glFinish();
    uint64_t b1 = get_time_us();
    assert(we_gpu_brush_end_stroke(bench_ctx, NULL));
    double avg_kawase_ms = (double)(b1 - b0) / 16000.0;

    we_gpu_brush_destroy(bench_ctx);
    bench_ctx = we_gpu_brush_create(WE_BRUSH_POOL_DEFAULT_CAPS);
    assert(bench_ctx != NULL);
    assert(we_gpu_brush_bind_target(bench_ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    /* Benchmark 50-stroke CoW undo/redo with r=48 px (~9 tiles/stroke, <=900 slots). */
    we_brush_params_t undo_p = mask_p;
    undo_p.radius_px = 48.0f;
    dab.radius_a = 48.0f;
    dab.radius_b = 48.0f;
    for (int i = 0; i < 50; i++) {
        assert(we_gpu_brush_begin_stroke(bench_ctx, (uint64_t)(9100 + i), &undo_p));
        dab.ax = 600.0f + (float)(i % 10) * 200.0f;
        dab.bx = dab.ax + 12.0f;
        dab.ay = 500.0f + (float)(i / 10) * 200.0f;
        dab.by = dab.ay + 6.0f;
        assert(we_gpu_brush_dispatch_dabs(bench_ctx, &dab, 1u, NULL));
        assert(we_gpu_brush_end_stroke(bench_ctx, NULL));
    }
    glFinish();

    uint64_t u0 = get_time_us();
    for (int i = 49; i >= 0; i--) {
        assert(we_gpu_brush_undo_stroke(bench_ctx, (uint64_t)(9100 + i)));
    }
    glFinish();
    uint64_t u1 = get_time_us();
    double avg_undo_us = (double)(u1 - u0) / 50.0;

    we_brush_stats_t st;
    we_gpu_brush_get_stats(bench_ctx, &st);
    printf("    -> 4K Color Dab (r=128): %.3f ms/dab | Dual-Kawase Blur: %.3f ms/dab\n",
           avg_color_ms, avg_kawase_ms);
    printf("    -> 50-Stroke 4K CoW Undo (r=48): %.1f us/stroke | Active Pool Slots: %u / %u\n",
           avg_undo_us, st.active_pool_slots, st.max_pool_slots);

    we_gpu_brush_destroy(bench_ctx);
    destroy_test_canvas(&canvas);
    assert_no_gl_error("benchmark_4k");
}

static void check_uniform_canvas_blur_smudge(we_gpu_brush_ctx_t *ctx, uint32_t w, uint32_t h)
{
    uint8_t *init_rgba = malloc((size_t)w * h * 4u);
    assert(init_rgba != NULL);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        init_rgba[i * 4u + 0u] = 200u;
        init_rgba[i * 4u + 1u] = 150u;
        init_rgba[i * 4u + 2u] = 100u;
        init_rgba[i * 4u + 3u] = 255u;
    }

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGBA8, GL_RGBA, init_rgba, false);
    free(init_rgba);

    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    const float blur_radii[2] = {3.0f, 16.0f};
    for (int bi = 0; bi < 2; bi++) {
        we_brush_params_t bp = {
            .channel = WE_BRUSH_CHAN_BLUR,
            .op_mode = WE_BRUSH_OP_BLUR_MASKED,
            .radius_px = 512.0f,
            .hardness = 1.0f,
            .falloff_power = 1.0f,
            .opacity = 1.0f,
            .blur_radius_px = blur_radii[bi],
        };
        assert(we_gpu_brush_begin_stroke(ctx, (uint64_t)(9500 + bi), &bp));
        we_brush_dab_t dab = {
            .ax = 0.0f,
            .ay = 0.0f,
            .bx = (float)w,
            .by = (float)h,
            .radius_a = 512.0f,
            .radius_b = 512.0f,
            .opacity_eff = 1.0f,
        };
        assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
        assert(we_gpu_brush_end_stroke(ctx, NULL));
    }

    we_brush_params_t sp = {
        .channel = WE_BRUSH_CHAN_SMUDGE,
        .op_mode = WE_BRUSH_OP_SMUDGE_ADV,
        .radius_px = 512.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .blur_radius_px = 6.0f,
    };
    assert(we_gpu_brush_begin_stroke(ctx, 9510u, &sp));
    we_brush_dab_t sdab = {
        .ax = 0.0f,
        .ay = 0.0f,
        .bx = (float)w,
        .by = (float)h,
        .radius_a = 512.0f,
        .radius_b = 512.0f,
        .opacity_eff = 1.0f,
        .drag_dx_px = 4.0f,
        .drag_dy_px = 4.0f,
    };
    assert(we_gpu_brush_dispatch_dabs(ctx, &sdab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *pixels = NULL;
    size_t byte_len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &pixels, &byte_len));
    assert(byte_len == (size_t)w * h * 4u);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int dr = abs((int)pixels[i * 4u + 0u] - 200);
        int dg = abs((int)pixels[i * 4u + 1u] - 150);
        int db = abs((int)pixels[i * 4u + 2u] - 100);
        assert(dr <= 2 && dg <= 2 && db <= 2);
        assert(pixels[i * 4u + 3u] == 255u);
    }
    free(pixels);
    we_gpu_brush_unbind_scene(ctx, &canvas.scene);
    destroy_test_canvas(&canvas);
    assert_no_gl_error("odd_width_blur_smudge");
}

static void test_odd_width_blur_and_smudge(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing odd-width (pw=1, pw=255) blur and smudge UV clamping...\n");
    check_uniform_canvas_blur_smudge(ctx, 1u, 255u);
    check_uniform_canvas_blur_smudge(ctx, 255u, 255u);
}

static void test_swizzled_rgba8_and_straight_alpha(we_gpu_brush_ctx_t *ctx)
{
    printf(
        "[+] Testing swizzled RGBA8888 promotion, straight-alpha fringe, and readback guards...\n");
    const uint32_t w = 128u, h = 128u;
    uint8_t *init_rgba = calloc((size_t)w * h, 4u);
    assert(init_rgba != NULL);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        init_rgba[i * 4u + 0u] = 42u;
        init_rgba[i * 4u + 1u] = 84u;
        init_rgba[i * 4u + 2u] = 126u;
        init_rgba[i * 4u + 3u] = 255u;
    }

    test_canvas_t swiz_canvas;
    init_test_canvas(&swiz_canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGBA8, GL_RGBA, init_rgba, false);
    free(init_rgba);

    /* Non-identity swizzle ONLY on G/B while leaving R=GL_RED so multi-channel detection is
     * exercised. */
    glBindTexture(GL_TEXTURE_2D, (GLuint)swiz_canvas.tex.gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    glBindTexture(GL_TEXTURE_2D, 0);

    uint32_t old_tex = swiz_canvas.tex.gl_tex;
    assert(we_gpu_brush_bind_target(ctx, &swiz_canvas.scene, 42, 0xFFFFFFFFu, 0));
    assert(swiz_canvas.tex.gl_tex != 0 && swiz_canvas.tex.gl_tex != old_tex);

    GLint sr = 0, sg = 0, sb = 0, sa = 0;
    glBindTexture(GL_TEXTURE_2D, (GLuint)swiz_canvas.tex.gl_tex);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sr);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sg);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sb);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sa);
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(sr == GL_RED && sg == GL_GREEN && sb == GL_BLUE && sa == GL_ALPHA);

    /* Mismatched readback format must be rejected cleanly. */
    uint8_t *bad_pixels = NULL;
    size_t bad_len = 0;
    assert(!we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_R8, &bad_pixels, &bad_len));
    assert(bad_pixels == NULL);

    /* Multiple consecutive readbacks exercise both slots of the 2-PBO ring. */
    for (int r = 0; r < 3; r++) {
        uint8_t *px = NULL;
        size_t len = 0;
        assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &px, &len));
        assert(len == (size_t)w * h * 4u);
        assert(px[0] == 42u && px[1] == 42u && px[2] == 42u && px[3] == 255u);
        free(px);
    }

    we_gpu_brush_unbind_scene(ctx, &swiz_canvas.scene);
    destroy_test_canvas(&swiz_canvas);

    /* Straight-alpha COLOR_RGBA onto transparent (0,0,0,0) must not darken soft fringe RGB. */
    uint8_t *clear_rgba = calloc((size_t)w * h, 4u);
    assert(clear_rgba != NULL);
    test_canvas_t fringe_canvas;
    init_test_canvas(&fringe_canvas, WE_TEX_FMT_RGBA8888, w, h, GL_RGBA8, GL_RGBA, clear_rgba,
                     false);
    free(clear_rgba);

    assert(we_gpu_brush_bind_target(ctx, &fringe_canvas.scene, 42, 0xFFFFFFFFu, 0));
    we_brush_params_t wp = {
        .channel = WE_BRUSH_CHAN_COLOR_RGBA8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 24.0f,
        .hardness = 0.2f,
        .falloff_power = 1.0f,
        .opacity = 0.5f,
        .color_rgba = {1.0f, 1.0f, 1.0f, 1.0f},
    };
    assert(we_gpu_brush_begin_stroke(ctx, 9601u, &wp));
    we_brush_dab_t dab = {.ax = 64.0f,
                          .ay = 64.0f,
                          .bx = 64.0f,
                          .by = 64.0f,
                          .radius_a = 24.0f,
                          .radius_b = 24.0f,
                          .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(ctx, &dab, 1u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *px = NULL;
    size_t len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RGBA8888, &px, &len));
    size_t semi_count = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t a = px[i * 4u + 3u];
        if (a > 8u && a < 245u) {
            semi_count++;
            assert(px[i * 4u + 0u] >= 250u);
            assert(px[i * 4u + 1u] >= 250u);
            assert(px[i * 4u + 2u] >= 250u);
        }
    }
    assert(semi_count > 10u);
    free(px);
    we_gpu_brush_unbind_scene(ctx, &fringe_canvas.scene);
    destroy_test_canvas(&fringe_canvas);
    assert_no_gl_error("swizzled_rgba8_straight_alpha");
}

static void test_validation_guards_and_atomic_cow(we_gpu_brush_ctx_t *ctx)
{
    printf("[+] Testing Inf/NaN flow_dir/params guards & atomic mid-stroke CoW failure...\n");
    const uint32_t w = 256u, h = 256u;
    uint8_t *init_rg = malloc((size_t)w * h * 2u);
    assert(init_rg != NULL);
    memset(init_rg, 128, (size_t)w * h * 2u);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_RG88, w, h, GL_RG8, GL_RG, init_rg, false);
    free(init_rg);

    assert(we_gpu_brush_bind_target(ctx, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    /* begin_stroke must reject non-finite floats and invalid channel/op_mode enums. */
    we_brush_params_t bad = {
        .channel = WE_BRUSH_CHAN_FLOW_RG88,
        .op_mode = WE_BRUSH_OP_FLOW_PAINT,
        .radius_px = NAN,
        .hardness = 0.5f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .flow_strength = 1.0f,
    };
    assert(!we_gpu_brush_begin_stroke(ctx, 9701u, &bad));
    bad.radius_px = 16.0f;
    bad.flow_strength = INFINITY;
    assert(!we_gpu_brush_begin_stroke(ctx, 9702u, &bad));
    bad.flow_strength = 1.0f;
    bad.color_rgba[3] = NAN;
    assert(!we_gpu_brush_begin_stroke(ctx, 9703u, &bad));
    bad.color_rgba[3] = 1.0f;
    bad.channel = 99u;
    assert(!we_gpu_brush_begin_stroke(ctx, 9704u, &bad));
    bad.channel = WE_BRUSH_CHAN_FLOW_RG88;
    bad.op_mode = 99u;
    assert(!we_gpu_brush_begin_stroke(ctx, 9705u, &bad));

    /* flow_dir = NaN / Inf dabs are skipped cleanly while valid dabs in the same batch apply. */
    we_brush_params_t good = {
        .channel = WE_BRUSH_CHAN_FLOW_RG88,
        .op_mode = WE_BRUSH_OP_FLOW_PAINT,
        .radius_px = 8.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
        .flow_strength = 1.0f,
    };
    assert(we_gpu_brush_begin_stroke(ctx, 9706u, &good));
    we_brush_dab_t batch[3] = {
        {.ax = 32.0f,
         .ay = 32.0f,
         .bx = 32.0f,
         .by = 32.0f,
         .radius_a = 8.0f,
         .radius_b = 8.0f,
         .opacity_eff = 1.0f,
         .flow_dir_x = NAN,
         .flow_dir_y = 0.0f},
        {.ax = 96.0f,
         .ay = 32.0f,
         .bx = 96.0f,
         .by = 32.0f,
         .radius_a = 8.0f,
         .radius_b = 8.0f,
         .opacity_eff = 1.0f,
         .flow_dir_x = 0.0f,
         .flow_dir_y = INFINITY},
        {.ax = 160.0f,
         .ay = 160.0f,
         .bx = 160.0f,
         .by = 160.0f,
         .radius_a = 8.0f,
         .radius_b = 8.0f,
         .opacity_eff = 1.0f,
         .flow_dir_x = 1.0f,
         .flow_dir_y = 0.0f},
    };
    assert(we_gpu_brush_dispatch_dabs(ctx, batch, 3u, NULL));
    assert(we_gpu_brush_end_stroke(ctx, NULL));

    uint8_t *px = NULL;
    size_t len = 0;
    assert(we_gpu_brush_readback_texels(ctx, WE_TEX_FMT_RG88, &px, &len));
    size_t i_nan = ((size_t)32u * w + 32u) * 2u;
    size_t i_inf = ((size_t)32u * w + 96u) * 2u;
    size_t i_ok = ((size_t)160u * w + 160u) * 2u;
    assert(px[i_nan] == 128u && px[i_nan + 1u] == 128u);
    assert(px[i_inf] == 128u && px[i_inf + 1u] == 128u);
    assert(px[i_ok] >= 253u && px[i_ok + 1u] == 128u);
    free(px);
    we_gpu_brush_unbind_scene(ctx, &canvas.scene);
    destroy_test_canvas(&canvas);

    /* Atomic mid-stroke CoW rollback when pool_cap=1 runs out on dab #2 of a 2-dab batch. */
    we_gpu_brush_ctx_t *tiny = we_gpu_brush_create(1u);
    assert(tiny != NULL);
    test_canvas_t r8c;
    init_test_canvas(&r8c, WE_TEX_FMT_R8, 128u, 128u, GL_R8, GL_RED, NULL, false);
    assert(we_gpu_brush_bind_target(tiny, &r8c.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t r8p = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 4.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
    };
    assert(we_gpu_brush_begin_stroke(tiny, 9710u, &r8p));
    we_brush_dab_t two_tiles[2] = {
        {.ax = 16.0f,
         .ay = 16.0f,
         .bx = 16.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
        {.ax = 96.0f,
         .ay = 16.0f,
         .bx = 96.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
    };
    assert(!we_gpu_brush_dispatch_dabs(tiny, two_tiles, 2u, NULL));

    we_brush_stats_t st;
    we_gpu_brush_get_stats(tiny, &st);
    assert(st.active_stroke_tiles == 0u);
    assert(st.active_pool_slots == 0u);

    uint8_t *r8px = NULL;
    size_t r8len = 0;
    assert(we_gpu_brush_readback_texels(tiny, WE_TEX_FMT_R8, &r8px, &r8len));
    assert(r8px[16u * 128u + 16u] == 0u);
    assert(r8px[16u * 128u + 96u] == 0u);
    free(r8px);

    we_gpu_brush_destroy(tiny);
    destroy_test_canvas(&r8c);
    assert_no_gl_error("validation_guards_atomic_cow");
}

static void test_live_stroke0_undo_and_unbind_scene(void)
{
    printf("[+] Testing live strokes[0] undo under full CoW pool & we_gpu_brush_unbind_scene...\n");
    we_gpu_brush_ctx_t *pool4 = we_gpu_brush_create(4u);
    assert(pool4 != NULL);

    test_canvas_t canvas;
    init_test_canvas(&canvas, WE_TEX_FMT_R8, 256u, 256u, GL_R8, GL_RED, NULL, false);
    assert(we_gpu_brush_bind_target(pool4, &canvas.scene, 42, 0xFFFFFFFFu, 0));

    we_brush_params_t p = {
        .channel = WE_BRUSH_CHAN_MASK_R8,
        .op_mode = WE_BRUSH_OP_ADD_BUILDUP,
        .radius_px = 4.0f,
        .hardness = 1.0f,
        .falloff_power = 1.0f,
        .opacity = 1.0f,
    };

    /* Stroke 10 occupies 1 slot (tile 0,0); Stroke 11 occupies 3 slots (tiles 1..3,0) -> 4/4 full.
     */
    assert(we_gpu_brush_begin_stroke(pool4, 10u, &p));
    we_brush_dab_t d0 = {.ax = 16.0f,
                         .ay = 16.0f,
                         .bx = 16.0f,
                         .by = 16.0f,
                         .radius_a = 4.0f,
                         .radius_b = 4.0f,
                         .opacity_eff = 1.0f};
    assert(we_gpu_brush_dispatch_dabs(pool4, &d0, 1u, NULL));
    assert(we_gpu_brush_end_stroke(pool4, NULL));

    assert(we_gpu_brush_begin_stroke(pool4, 11u, &p));
    we_brush_dab_t d123[3] = {
        {.ax = 80.0f,
         .ay = 16.0f,
         .bx = 80.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
        {.ax = 144.0f,
         .ay = 16.0f,
         .bx = 144.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
        {.ax = 208.0f,
         .ay = 16.0f,
         .bx = 208.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
    };
    assert(we_gpu_brush_dispatch_dabs(pool4, d123, 3u, NULL));
    assert(we_gpu_brush_end_stroke(pool4, NULL));

    /* Undo live strokes[0] (id=10): must evict id=11 for post-tile headroom, NOT id=10! */
    assert(we_gpu_brush_undo_stroke(pool4, 10u));
    uint8_t *px = NULL;
    size_t len = 0;
    assert(we_gpu_brush_readback_texels(pool4, WE_TEX_FMT_R8, &px, &len));
    assert(px[16u * 256u + 16u] == 0u);
    free(px);

    /* Stroke 20 occupies all 4 slots itself -> undo lacks headroom and must fail without
     * self-eviction. */
    we_gpu_brush_unbind_scene(pool4, &canvas.scene);
    assert(we_gpu_brush_bind_target(pool4, &canvas.scene, 42, 0xFFFFFFFFu, 0));
    assert(we_gpu_brush_begin_stroke(pool4, 20u, &p));
    we_brush_dab_t d0123[4] = {
        {.ax = 16.0f,
         .ay = 16.0f,
         .bx = 16.0f,
         .by = 16.0f,
         .radius_a = 4.0f,
         .radius_b = 4.0f,
         .opacity_eff = 1.0f},
        d123[0],
        d123[1],
        d123[2],
    };
    assert(we_gpu_brush_dispatch_dabs(pool4, d0123, 4u, NULL));
    assert(we_gpu_brush_end_stroke(pool4, NULL));

    assert(!we_gpu_brush_undo_stroke(pool4, 20u));
    we_brush_stats_t st;
    we_gpu_brush_get_stats(pool4, &st);
    assert(st.active_pool_slots == 4u);

    /* Verify scene-scoped eviction and active stroke abort on we_gpu_brush_unbind_scene. */
    test_canvas_t canvas2;
    init_test_canvas(&canvas2, WE_TEX_FMT_R8, 256u, 256u, GL_R8, GL_RED, NULL, false);
    assert(we_gpu_brush_bind_target(pool4, &canvas2.scene, 42, 0xFFFFFFFFu, 0));
    assert(we_gpu_brush_begin_stroke(pool4, 30u, &p));
    assert(we_gpu_brush_dispatch_dabs(pool4, &d0, 1u, NULL));
    assert(we_gpu_brush_end_stroke(pool4, NULL));

    assert(we_gpu_brush_bind_target(pool4, &canvas.scene, 42, 0xFFFFFFFFu, 0));
    assert(we_gpu_brush_begin_stroke(pool4, 21u, &p));
    assert(we_gpu_brush_dispatch_dabs(pool4, &d0, 1u, NULL));
    we_gpu_brush_unbind_scene(pool4, &canvas.scene);

    assert(!we_gpu_brush_end_stroke(pool4, NULL));
    assert(we_gpu_brush_bind_target(pool4, &canvas2.scene, 42, 0xFFFFFFFFu, 0));
    assert(we_gpu_brush_undo_stroke(pool4, 30u));

    we_gpu_brush_destroy(pool4);
    destroy_test_canvas(&canvas);
    destroy_test_canvas(&canvas2);
    assert_no_gl_error("live_stroke0_undo_unbind_scene");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  GPU Brush Engine Test Suite (test_we_gpu_brush)\n");
    printf("==========================================================\n");

    test_abi_and_flow_helpers();

    headless_gles_t gl;
    if (!init_headless_gles31(&gl)) {
        fprintf(stderr, "Headless GBM/EGL OpenGL ES 3.1 context initialization failed\n");
        return 1;
    }

    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    const char *gl_ren = (const char *)glGetString(GL_RENDERER);
    printf("    -> Active GLES 3.1 Driver: %s (%s)\n", gl_ver ? gl_ver : "unknown",
           gl_ren ? gl_ren : "unknown");

    we_gpu_brush_ctx_t *ctx = we_gpu_brush_create(WE_BRUSH_POOL_DEFAULT_CAPS);
    assert(ctx != NULL);
    assert_no_gl_error("we_gpu_brush_create");

    test_r8_mask_capsule_and_ops(ctx);
    test_rg88_swizzle_and_rest_preservation(ctx);
    test_rgba8_fbo_promotion_fallback(ctx);
    test_blur_and_smudge(ctx);
    test_odd_width_blur_and_smudge(ctx);
    test_swizzled_rgba8_and_straight_alpha(ctx);
    test_validation_guards_and_atomic_cow(ctx);
    test_cow_undo_redo_and_chunking(ctx);
    we_gpu_brush_destroy(ctx);
    assert_no_gl_error("we_gpu_brush_destroy");

    test_cow_pool_eviction_on_overflow();
    test_live_stroke0_undo_and_unbind_scene();
    benchmark_4k_brush_and_undo();

    destroy_headless_gles31(&gl);

    printf("==========================================================\n");
    printf("  >>> ALL GPU BRUSH TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
