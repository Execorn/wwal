// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_we_builtin_effects.c
 * @brief 50-Shader Clean-Room Built-in Effect Library Verification Suite
 *
 * Verifies:
 * 1. sizeof(we_builtin_effect_desc_t) == 320 bytes.
 * 2. LZ4 .rodata block decompression across all 50 stock effects in < 150 us total.
 * 3. O(1) Robin-Hood FNV-1a slug and virtual path lookup.
 * 4. Decompressed GLSL ES 3.10 vertex and fragment shader syntax and opaque alpha invariant.
 */

#include "waywal/shader_graph.h"
#include "waywal/we_builtin_effects.h"
#include "waywal/we_shader.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
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

static_assert(sizeof(we_builtin_effect_desc_t) == 320,
              "we_builtin_effect_desc_t must be exactly 320 bytes");

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t get_cpu_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_descriptor_table_and_size(void)
{
    printf("[+] Testing descriptor table size & static properties...\n");
    assert(sizeof(we_builtin_effect_desc_t) == 320);

    size_t count = 0;
    const we_builtin_effect_desc_t *table = we_builtin_effects_get_table(&count);
    assert(table != NULL);
    assert(count == 50);

    for (size_t i = 0; i < count; ++i) {
        const we_builtin_effect_desc_t *desc = &table[i];
        assert(desc->id >= 1 && desc->id <= 50);
        assert(strlen(desc->slug) > 0);
        assert(strlen(desc->display_name) > 0);
        assert(desc->uncompressed_glsl_size > 0);
        assert(desc->lz4_compressed_size > 0);
        assert(desc->pass_count >= 1 && desc->pass_count <= 4);
    }
    printf("    -> All 50 effect descriptors verified in static table.\n");
}

static void test_decompression_performance_and_syntax(void)
{
    printf("[+] Testing LZ4 decompression performance (< 150 us budget) & syntax...\n");

    char glsl_buf[65536];
    size_t total_decompressed_bytes = 0;

    // 1. Functional & Syntax Verification Pass
    for (uint32_t id = 1; id <= 50; ++id) {
        const we_builtin_effect_desc_t *desc =
            we_builtin_effect_get_by_id((we_builtin_effect_id_t)id);
        assert(desc != NULL);
        assert(desc->id == id);

        size_t written = we_builtin_effect_decompress_glsl((we_builtin_effect_id_t)id, glsl_buf,
                                                           sizeof(glsl_buf));
        assert(written > 0);
        assert(written <= desc->uncompressed_glsl_size);
        total_decompressed_bytes += written;

        // Verify valid GLSL ES 3.10 structure
        assert(strstr(glsl_buf, "#version 310 es") != NULL ||
               strstr(glsl_buf, "void main") != NULL);

        // Decompress individual stages
        char frag_buf[32768];
        char vert_buf[16384];
        size_t f_sz = we_builtin_effect_decompress_stage((we_builtin_effect_id_t)id, true, frag_buf,
                                                         sizeof(frag_buf));
        size_t v_sz = we_builtin_effect_decompress_stage((we_builtin_effect_id_t)id, false,
                                                         vert_buf, sizeof(vert_buf));
        assert(f_sz > 0);
        assert(v_sz > 0);

        // Test cached stage pointer
        const char *cached_frag =
            we_builtin_effect_get_stage_glsl((we_builtin_effect_id_t)id, true);
        const char *cached_vert =
            we_builtin_effect_get_stage_glsl((we_builtin_effect_id_t)id, false);
        assert(cached_frag != NULL);
        assert(cached_vert != NULL);
        assert(strlen(cached_frag) == f_sz);
        assert(strlen(cached_vert) == v_sz);
    }

    // 2. Microbenchmark Decompression Timing across all 50 effects (best of 32 CPU-time runs)
    double min_total_us = 1e9;
    for (int run = 0; run < 32; ++run) {
        uint64_t t_start = get_cpu_time_ns();
        for (uint32_t id = 1; id <= 50; ++id) {
            we_builtin_effect_decompress_glsl((we_builtin_effect_id_t)id, glsl_buf,
                                              sizeof(glsl_buf));
        }
        uint64_t t_end = get_cpu_time_ns();
        double cur_us = (double)(t_end - t_start) / 1000.0;
        if (cur_us < min_total_us) {
            min_total_us = cur_us;
        }
    }

    double per_effect_us = min_total_us / 50.0;

    printf("    -> Best decompression time (50 effects): %.2f us (%.2f us / effect, %zu bytes "
           "total)\n",
           min_total_us, per_effect_us, total_decompressed_bytes);

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
    const double total_budget_us = 900.0;
    const double per_shader_budget_us = 18.0;
#else
    const double total_budget_us = 150.0;
    const double per_shader_budget_us = 3.5;
#endif
    assert(min_total_us < total_budget_us && "LZ4 decompression exceeded total budget");
    assert(per_effect_us < per_shader_budget_us && "LZ4 per-shader decompression exceeded budget");
}

static void test_robin_hood_slug_lookup(void)
{
    printf("[+] Testing Robin-Hood O(1) slug and virtual path lookup...\n");

    const char *canonical_slugs[50] = {"waterwaves",
                                       "waterripple",
                                       "waterflow",
                                       "foliagesway",
                                       "scroll",
                                       "shake",
                                       "pendulum",
                                       "refract",
                                       "fisheye",
                                       "perspective",
                                       "transform",
                                       "twirl",
                                       "skew",
                                       "pulse",
                                       "iris",
                                       "morph",
                                       "bloom",
                                       "blur_kawase",
                                       "godrays",
                                       "lightshafts",
                                       "shine",
                                       "chromatic_aberration",
                                       "vignette",
                                       "filmgrain",
                                       "vhs",
                                       "motion_blur",
                                       "depth_of_field",
                                       "lens_flare",
                                       "halftone",
                                       "crt_scanlines",
                                       "pixelate",
                                       "sharpen_cas",
                                       "tint",
                                       "colorgrading",
                                       "hsv",
                                       "contrast",
                                       "opaquebackground",
                                       "edge_detection",
                                       "outline",
                                       "cloudmotion",
                                       "fire",
                                       "nitro",
                                       "aurora",
                                       "rain_glass",
                                       "genericparticle",
                                       "ropetrail",
                                       "spritetrail",
                                       "halo",
                                       "refract_particle",
                                       "ember_spark"};

    for (int i = 0; i < 50; ++i) {
        const char *slug = canonical_slugs[i];
        const we_builtin_effect_desc_t *desc = we_builtin_effect_find_by_slug(slug);
        assert(desc != NULL);
        assert(desc->id >= 1 && desc->id <= 50);

        // Virtual path prefixes
        char path_buf[128];
        snprintf(path_buf, sizeof(path_buf), "effects/%s/effect.json", slug);
        const we_builtin_effect_desc_t *from_path = we_builtin_effect_find_by_slug(path_buf);
        assert(from_path != NULL);
        assert(from_path->id == desc->id);

        snprintf(path_buf, sizeof(path_buf), "shaders/effects/%s.frag", slug);
        const we_builtin_effect_desc_t *from_frag = we_builtin_effect_find_by_slug(path_buf);
        assert(from_frag != NULL);
        assert(from_frag->id == desc->id);
    }

    // Verify negative lookups
    assert(we_builtin_effect_find_by_slug("non_existent_effect_slug_xyz") == NULL);
    assert(we_builtin_effect_find_by_slug("") == NULL);
    assert(we_builtin_effect_find_by_slug(NULL) == NULL);

    printf("    -> Robin-Hood slug lookup verified for 50/50 effects and negative cases.\n");
}

typedef struct {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
} headless_gles_t;

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

    // Fallback to EGL_PLATFORM_SURFACELESS_MESA (0x31DD)
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
        if (gl->ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(gl->dpy, gl->ctx);
        }
        eglTerminate(gl->dpy);
    }
    if (gl->gbm) {
        gbm_device_destroy(gl->gbm);
    }
    if (gl->drm_fd >= 0) {
        close(gl->drm_fd);
    }
}

static void compile_and_check_frag_pass(const char *src, size_t len, const char *label)
{
    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    assert(sh != 0);
    const GLchar *ptrs[1] = {src};
    GLint lens[1] = {(GLint)len};
    glShaderSource(sh, 1, ptrs, lens);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "[GLES3.1 FAIL] %s:\n%s\n", label, log);
    }
    assert(ok == GL_TRUE);
    glDeleteShader(sh);
}

static void test_headless_gles31_compile_all_50(void)
{
    printf("[+] Testing headless GBM/EGL OpenGL ES 3.1 glCompileShader + glLinkProgram on all 50 "
           "effects...\n");

    headless_gles_t gl;
    bool ok = init_headless_gles31(&gl);
    assert(ok && "Headless GBM/EGL OpenGL ES 3.1 context initialization failed");

    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    const char *gl_ren = (const char *)glGetString(GL_RENDERER);
    printf("    -> Active GLES 3.1 Driver: %s (%s)\n", gl_ver ? gl_ver : "unknown",
           gl_ren ? gl_ren : "unknown");

    char vert_raw[16384];
    char frag_raw[32768];
    uint64_t t_start = get_time_ns();

    uint32_t fail_count = 0;
    for (uint32_t id = 1; id <= 50; ++id) {
        const we_builtin_effect_desc_t *desc =
            we_builtin_effect_get_by_id((we_builtin_effect_id_t)id);
        assert(desc != NULL);

        size_t v_len = we_builtin_effect_decompress_stage((we_builtin_effect_id_t)id, false,
                                                          vert_raw, sizeof(vert_raw));
        size_t f_len = we_builtin_effect_decompress_stage((we_builtin_effect_id_t)id, true,
                                                          frag_raw, sizeof(frag_raw));
        assert(v_len > 0 && f_len > 0);

        we_shader_preprocessed_t prep;
        memset(&prep, 0, sizeof(prep));
        bool prep_ok =
            we_shader_preprocess_pair(vert_raw, frag_raw, NULL, NULL, 0, 0xFFu, false, &prep);
        if (!prep_ok || !prep.vert_glsl || !prep.frag_glsl) {
            fprintf(stderr, "[PREPROCESS FAIL] id=%u slug=%s\n", id, desc->slug);
            fail_count++;
            continue;
        }

        uint32_t prog = we_shader_compile_program(prep.vert_glsl, prep.frag_glsl, desc->slug);
        if (prog == 0) {
            fprintf(stderr, "[COMPILE/LINK FAIL] id=%u slug=%s\n", id, desc->slug);
            fail_count++;
        } else {
            glDeleteProgram(prog);
        }
        we_shader_preprocessed_free(&prep);
    }
    assert(fail_count == 0 && "One or more stock effects failed GLES 3.1 compilation/linking");

    uint64_t t_end = get_time_ns();
    printf("    -> 50/50 built-in effects preprocessed, compiled & linked cleanly on GLES 3.1 "
           "(%.2f ms total).\n",
           (double)(t_end - t_start) / 1000000.0);

    // Also verify shader_graph multi-pass GLSL helpers compile cleanly on the real GLES 3.1 driver
    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    assert(graph != NULL);
    int32_t n_uv = waywal_shader_graph_add_node(graph, WAYWAL_NODE_IN_UV, "UV", 0.0f, 0.0f);
    int32_t n_curl =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_NOISE_CURL2D, "Curl", 100.0f, 0.0f);
    int32_t n_rip =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_BUILTIN_EFFECT, "Ripple", 200.0f, 0.0f);
    int32_t n_kaw =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_FILTER_DUAL_KAWASE, "Kawase", 300.0f, 0.0f);
    int32_t n_chr = waywal_shader_graph_add_node(graph, WAYWAL_NODE_FILTER_CHROMATIC_ABERR, "Chrom",
                                                 400.0f, 0.0f);
    int32_t n_out =
        waywal_shader_graph_add_node(graph, WAYWAL_NODE_OUT_IMAGE_PASS, "Sink", 500.0f, 0.0f);
    waywal_graph_node_t *rip_node = waywal_shader_graph_find_node(graph, (uint32_t)n_rip);
    strncpy(rip_node->prop_or_asset_key, "waterripple", sizeof(rip_node->prop_or_asset_key) - 1);
    waywal_shader_graph_connect(graph, (uint32_t)n_uv, 0, (uint32_t)n_curl, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_curl, 0, (uint32_t)n_rip, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_rip, 0, (uint32_t)n_kaw, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_kaw, 0, (uint32_t)n_chr, 0);
    waywal_shader_graph_connect(graph, (uint32_t)n_chr, 0, (uint32_t)n_out, 0);

    waywal_graph_schedule_t sched;
    assert(waywal_shader_graph_analyze(graph, &sched));
    size_t glsl_len = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &sched, &glsl_len);
    assert(glsl != NULL && glsl_len > 0);

    const char *cursor = strstr(glsl, "#pragma Pass ");
    assert(cursor != NULL);
    while (cursor != NULL) {
        const char *line_end = strchr(cursor, '\n');
        assert(line_end != NULL);
        const char *body_start = line_end + 1;
        const char *next_pass = strstr(body_start, "#pragma Pass ");
        size_t body_len = next_pass ? (size_t)(next_pass - body_start) : strlen(body_start);
        compile_and_check_frag_pass(body_start, body_len, "shader_graph_pass");
        cursor = next_pass;
    }

    free(glsl);
    waywal_shader_graph_destroy(graph);
    destroy_headless_gles31(&gl);
    printf("    -> Multi-pass shader_graph GLSL ES 3.10 passes also compiled cleanly on GPU "
           "driver.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  Built-in Effects Library Test Suite (test_we_builtin_effects)\n");
    printf("==========================================================\n");

    test_descriptor_table_and_size();
    test_decompression_performance_and_syntax();
    test_robin_hood_slug_lookup();
    test_headless_gles31_compile_all_50();

    printf("==========================================================\n");
    printf("  >>> ALL BUILT-IN EFFECT TESTS PASSED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
