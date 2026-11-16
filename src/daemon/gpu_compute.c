// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gpu_compute.h"

#include <math.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
#include <cpuid.h>
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
    GLint last_scale_src_w;
    GLint last_scale_src_h;
    GLint last_scale_dst_w;
    GLint last_scale_dst_h;
    GLint last_scale_mode;
    uint32_t last_scale_clear;
    GLint last_scale_has_src;
    int32_t last_scale_span_bounds[4];
    float last_scale_crop_gravity[2];
    GLint loc_kawase_src_res;
    GLint loc_kawase_dst_res;
    GLint loc_kawase_radius;
    GLint last_kawase_src_w;
    GLint last_kawase_src_h;
    GLint last_kawase_dst_w;
    GLint last_kawase_dst_h;
    float last_kawase_radius;

    GLuint upload_src_tex;
    uint32_t upload_src_w;
    uint32_t upload_src_h;
    const void *last_upload_ptr;
    uint32_t last_upload_w;
    uint32_t last_upload_h;
    uint64_t upload_gen;
    uint64_t last_upload_gen;
    uint64_t last_upload_hash0;
    uint64_t last_upload_hash1;

    GLuint kawase_half_tex;
    uint32_t kawase_half_w;
    uint32_t kawase_half_h;

    GLuint scratch_tex;
    uint32_t scratch_w;
    uint32_t scratch_h;
    GLuint scratch_pool_tex[4];
    uint32_t scratch_pool_w[4];
    uint32_t scratch_pool_h[4];
    uint32_t scratch_pool_next;
    wwal_scratch_present_state_t present_state;

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

    uint32_t rng_state;
    waywal_transition_type_t random_resolved_type;
    const void *random_old_key;
    const void *random_new_key;
    float random_last_progress;
    bool random_active;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
    wwal_egl_sync_procs_t sync_procs;
};

static uint32_t wwal_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    if (x == 0u) {
        x = 0x9E3779B9u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline uint32_t load_u32_unaligned(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void store_u32_unaligned(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("xsave"))) static uint64_t read_xcr0_xsave(void)
{
    uint32_t eax = 0u, edx = 0u;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | (uint64_t)eax;
}

static bool cpu_supports_avx2(void)
{
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    /* OSXSAVE (bit 27) must be enabled before querying XCR0 for YMM state (bits 1 and 2). */
    if ((ecx & (1u << 27)) == 0u) {
        return false;
    }
    if ((read_xcr0_xsave() & 0x6ULL) != 0x6ULL) {
        return false;
    }
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    return (ebx & (1u << 5)) != 0u;
}
#else
static bool cpu_supports_avx2(void) { return false; }
#endif

static inline uint64_t splitmix64_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static void
hash_rgba_buffer_128_avx2(const uint8_t *data, size_t len_bytes, uint64_t *out_h0, uint64_t *out_h1)
{
    __m256i acc0 =
        _mm256_set_epi64x((long long)0xD6E8FEB86659FD93ULL, (long long)0x94D049BB133111EBULL,
                          (long long)0xBF58476D1CE4E5B9ULL, (long long)0x9E3779B97F4A7C15ULL);
    __m256i acc1 =
        _mm256_set_epi64x((long long)0xA0761D6478BD642FULL, (long long)0xE7037ED1A0B428DBULL,
                          (long long)0x8EBC6AF09C88C6E3ULL, (long long)0x589965CC75374CC3ULL);
    const __m256i mul_c = _mm256_set1_epi32((int)0x85EBCA77u);

    size_t i = 0u;
    for (; i + 32u <= len_bytes; i += 32u) {
        __m256i v = _mm256_loadu_si256((const __m256i_u *)(data + i));
        __m256i mixed = _mm256_mullo_epi32(_mm256_xor_si256(acc0, v), mul_c);
        acc0 = _mm256_add_epi64(_mm256_xor_si256(mixed, _mm256_srli_epi64(mixed, 17)), v);
        acc1 = _mm256_add_epi64(acc1, acc0);
    }

    uint64_t lanes0[4];
    uint64_t lanes1[4];
    memcpy(lanes0, &acc0, sizeof(lanes0));
    memcpy(lanes1, &acc1, sizeof(lanes1));

    uint64_t h0 = splitmix64_mix(lanes0[0] ^ lanes0[2] ^ (uint64_t)len_bytes);
    uint64_t h1 = splitmix64_mix(lanes1[1] ^ lanes1[3] ^ (~(uint64_t)len_bytes));
    h0 = splitmix64_mix(h0 ^ lanes0[1] ^ lanes1[0]);
    h1 = splitmix64_mix(h1 ^ lanes0[3] ^ lanes1[2]);

    for (; i + 8u <= len_bytes; i += 8u) {
        uint64_t w;
        memcpy(&w, data + i, sizeof(w));
        h0 = splitmix64_mix(h0 ^ w);
        h1 = splitmix64_mix(h1 + w * 0x9E3779B97F4A7C15ULL);
    }
    if (i < len_bytes) {
        uint64_t tail = 0u;
        memcpy(&tail, data + i, len_bytes - i);
        h0 = splitmix64_mix(h0 ^ tail);
        h1 = splitmix64_mix(h1 + tail * 0xBF58476D1CE4E5B9ULL);
    }
    *out_h0 = h0;
    *out_h1 = h1;
}
#endif

static void hash_rgba_buffer_128(const uint8_t *data, size_t len_bytes, uint64_t *out_h0,
                                 uint64_t *out_h1)
{
    if (!data || len_bytes == 0u) {
        *out_h0 = 0u;
        *out_h1 = 0u;
        return;
    }
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
    if (len_bytes >= 64u && cpu_supports_avx2()) {
        hash_rgba_buffer_128_avx2(data, len_bytes, out_h0, out_h1);
        return;
    }
#endif
    uint64_t h0 = 0x9E3779B97F4A7C15ULL ^ (uint64_t)len_bytes;
    uint64_t h1 = 0xBF58476D1CE4E5B9ULL ^ (~(uint64_t)len_bytes);
    size_t i = 0u;
    for (; i + 8u <= len_bytes; i += 8u) {
        uint64_t w;
        memcpy(&w, data + i, sizeof(w));
        h0 = (h0 ^ w) * 0x9E3779B97F4A7C15ULL;
        h0 ^= h0 >> 29;
        h1 = (h1 + w) * 0x94D049BB133111EBULL;
        h1 ^= h1 >> 31;
    }
    if (i < len_bytes) {
        uint64_t tail = 0u;
        memcpy(&tail, data + i, len_bytes - i);
        h0 ^= tail;
        h1 += tail;
    }
    *out_h0 = splitmix64_mix(h0);
    *out_h1 = splitmix64_mix(h1);
}

static void invalidate_program_uniforms(gpu_program_uniforms_t *u)
{
    if (!u) {
        return;
    }
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

static void query_program_uniforms(GLuint prog, gpu_program_uniforms_t *u)
{
    if (!prog || !u) {
        return;
    }
    u->loc_prog = glGetUniformLocation(prog, "u_progress");
    u->loc_res = glGetUniformLocation(prog, "u_resolution");
    u->loc_angle = glGetUniformLocation(prog, "u_angle");
    u->loc_wave_freq = glGetUniformLocation(prog, "u_wave_freq");
    u->loc_wave_amp = glGetUniformLocation(prog, "u_wave_amp");
    u->loc_center = glGetUniformLocation(prog, "u_center");
    u->loc_smoothness = glGetUniformLocation(prog, "u_smoothness");
    u->loc_step = glGetUniformLocation(prog, "u_step");
    invalidate_program_uniforms(u);
}

static void trim_driver_compiler_pages(void)
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

static GLuint compile_shader_program(const char *src, const char *name)
{
    GLuint prog = wwal_gl_compile_compute_program_cached(src, name, NULL);
    trim_driver_compiler_pages();
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
    "        imageStore(u_target, coord, vec4(u_clear_color.rgb, 1.0).bgra);\n"
    "        return;\n"
    "    }\n"
    "    if (u_dst_res.x == u_src_res.x && u_dst_res.y == u_src_res.y && u_scaling_mode != 4 && "
    "u_scaling_mode != 5) {\n"
    "        vec4 c = texelFetch(u_src, coord, 0);\n"
    "        imageStore(u_target, coord, vec4(c.rgb, 1.0).bgra);\n"
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
    "    imageStore(u_target, coord, vec4(out_col.rgb, 1.0).bgra);\n"
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
    "    vec2 step_uv = max(u_radius, 0.0) / src_f;\n"
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
    "    imageStore(u_target, coord, vec4(out_col.rgb, 1.0).bgra);\n"
    "}\n";

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
    if (!egl_exts || !wwal_egl_has_extension(egl_exts, "EGL_EXT_image_dma_buf_import")) {
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

    gpu_compute_ctx_t *ctx = (gpu_compute_ctx_t *)calloc(1u, sizeof(gpu_compute_ctx_t));
    if (!ctx) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, egl_ctx);
        eglTerminate(dpy);
        return NULL;
    }

    ctx->display = dpy;
    ctx->context = egl_ctx;
    ctx->present_state.loc_tex = -1;
    ctx->present_state.loc_swap_rb = -1;
    ctx->present_state.loc_force_opaque = -1;
    ctx->present_state.last_swap_rb = -1;
    ctx->present_state.last_force_opaque = -1;

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

    /* Only compile scale compute shader at startup; transition & blur shaders compile lazily. */
    ctx->prog_scale = compile_shader_program(SHADER_SRC_SCALE, "scale");
    if (!ctx->prog_scale) {
        WAYWAL_LOG_WARN("Compute scale shader program failed to compile");
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

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t seed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^ (uintptr_t)ctx ^
                    ((uint64_t)(uint32_t)getpid() << 16);
    ctx->rng_state = (uint32_t)(seed ^ (seed >> 32));
    if (ctx->rng_state == 0u) {
        ctx->rng_state = 0xA5A55A5Au;
    }

    wwal_egl_init_sync_procs(dpy, &ctx->sync_procs);

    WAYWAL_LOG_INFO("GPU compute transition engine initialized (GLES 3.1 compute on GBM, "
                    "native_fence_sync=%s)",
                    ctx->sync_procs.has_native_fence_sync ? "yes" : "no");
    gpu_compute_invalidate_state_cache(ctx);
    return ctx;
}

void gpu_compute_invalidate_state_cache(gpu_compute_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->current_prog = 0u;
    ctx->bound_target_tex = 0u;
    ctx->bound_old_tex = 0u;
    ctx->bound_new_tex = 0u;
    ctx->present_state.bound_fbo_tex = 0u;

    invalidate_program_uniforms(&ctx->unif_fade);
    invalidate_program_uniforms(&ctx->unif_wipe);
    invalidate_program_uniforms(&ctx->unif_grow);
    invalidate_program_uniforms(&ctx->unif_outer);
    invalidate_program_uniforms(&ctx->unif_wave);
    invalidate_program_uniforms(&ctx->unif_noise);
    invalidate_program_uniforms(&ctx->unif_crosszoom);
    invalidate_program_uniforms(&ctx->unif_slide);
    invalidate_program_uniforms(&ctx->unif_glitch);
    invalidate_program_uniforms(&ctx->unif_burn);
    invalidate_program_uniforms(&ctx->unif_ripple);
    invalidate_program_uniforms(&ctx->unif_pixelate);
    invalidate_program_uniforms(&ctx->unif_doom);
    invalidate_program_uniforms(&ctx->unif_swirl);
    invalidate_program_uniforms(&ctx->unif_cube);
    invalidate_program_uniforms(&ctx->unif_luma);
    invalidate_program_uniforms(&ctx->unif_light_leak);
    invalidate_program_uniforms(&ctx->unif_page_curl);
    invalidate_program_uniforms(&ctx->unif_custom);

    ctx->last_scale_src_w = -1;
    ctx->last_scale_src_h = -1;
    ctx->last_scale_dst_w = -1;
    ctx->last_scale_dst_h = -1;
    ctx->last_scale_mode = -1;
    ctx->last_scale_clear = 0xFFFFFFFFu;
    ctx->last_scale_has_src = -1;
    ctx->last_scale_span_bounds[0] = -1;
    ctx->last_scale_span_bounds[1] = -1;
    ctx->last_scale_span_bounds[2] = -1;
    ctx->last_scale_span_bounds[3] = -1;
    ctx->last_scale_crop_gravity[0] = -1e9f;
    ctx->last_scale_crop_gravity[1] = -1e9f;

    ctx->last_kawase_src_w = -1;
    ctx->last_kawase_src_h = -1;
    ctx->last_kawase_dst_w = -1;
    ctx->last_kawase_dst_h = -1;
    ctx->last_kawase_radius = -1e9f;
}

static bool gpu_compute_needs_scratch(const gpu_compute_ctx_t *ctx, const dmabuf_bo_t *bo)
{
    if (!ctx || !bo) {
        return true;
    }
    wwal_target_bo_t tbo = wwal_target_bo_from_dmabuf(bo);
    if (wwal_bo_requires_scratch_blit(&tbo)) {
        return true;
    }
    if (bo->is_10bit || ctx->glEGLImageTargetTexStorageEXT == NULL || bo->num_planes > 1u) {
        return true;
    }
    return false;
}

static bool gpu_compute_import_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *bo)
{
    if (!ctx || !bo || bo->width == 0u || bo->height == 0u) {
        return false;
    }
    if (bo->egl_image && bo->gl_tex != 0u) {
        return true;
    }

    gpu_compute_make_current(ctx);

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

    uint32_t num_planes = bo->num_planes > 0u ? bo->num_planes : 1u;
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

    for (uint32_t p = 0u; p < num_planes; ++p) {
        if (bo->fd[p] < 0) {
            continue;
        }
        if (n + 11 >= (int)(sizeof(attribs) / sizeof(attribs[0]))) {
            break;
        }
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_offset_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];

        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo_keys[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFFULL);
            attribs[n++] = plane_mod_hi_keys[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32) & 0xFFFFFFFFULL);
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

    GLuint tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) {
        ctx->eglDestroyImageKHR(ctx->display, img);
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (!gpu_compute_needs_scratch(ctx, bo) && ctx->glEGLImageTargetTexStorageEXT) {
        ctx->glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    }
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        ctx->eglDestroyImageKHR(ctx->display, img);
        return false;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    bo->egl_image = img;
    bo->gl_tex = tex;
    ctx->bound_target_tex = 0u;
    ctx->bound_old_tex = 0u;
    ctx->bound_new_tex = 0u;
    ctx->present_state.bound_fbo_tex = 0u;
    return true;
}

static GLuint ensure_linear_scratch_target(gpu_compute_ctx_t *ctx, uint32_t width, uint32_t height)
{
    if (ctx->scratch_tex != 0u && ctx->scratch_w == width && ctx->scratch_h == height) {
        return ctx->scratch_tex;
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (ctx->scratch_pool_tex[i] != 0u && ctx->scratch_pool_w[i] == width &&
            ctx->scratch_pool_h[i] == height) {
            ctx->scratch_tex = ctx->scratch_pool_tex[i];
            ctx->scratch_w = width;
            ctx->scratch_h = height;
            return ctx->scratch_tex;
        }
    }
    uint32_t slot = ctx->scratch_pool_next & 3u;
    ctx->scratch_pool_next++;
    if (ctx->scratch_pool_tex[slot] != 0u) {
        if (ctx->bound_target_tex == ctx->scratch_pool_tex[slot]) {
            ctx->bound_target_tex = 0u;
        }
        glDeleteTextures(1, &ctx->scratch_pool_tex[slot]);
        ctx->scratch_pool_tex[slot] = 0u;
    }

    /* Compute shaders always write layout(rgba8) with .bgra channel order;
     * wwal_gl_present_via_scratch reverses R/B via u_swap_rb=1 so the scratch texture keeps
     * identity swizzle. */
    GLuint tex = wwal_gl_alloc_linear_scratch_tex(width, height, GL_RGBA8, GL_NEAREST, false);
    if (tex == 0u) {
        return 0u;
    }
    ctx->scratch_pool_tex[slot] = tex;
    ctx->scratch_pool_w[slot] = width;
    ctx->scratch_pool_h[slot] = height;
    ctx->scratch_tex = tex;
    ctx->scratch_w = width;
    ctx->scratch_h = height;
    ctx->bound_target_tex = 0u;
    return ctx->scratch_tex;
}

static bool present_scratch_to_bo(gpu_compute_ctx_t *ctx, GLuint scratch_tex,
                                  dmabuf_bo_t *target_bo)
{
    wwal_target_bo_t tbo = wwal_target_bo_from_dmabuf(target_bo);
    bool ok = wwal_gl_present_via_scratch(&ctx->present_state, scratch_tex, &tbo, true, true);
    ctx->current_prog = 0u;
    ctx->bound_target_tex = 0u;
    return ok;
}

static GLuint ensure_transition_prog(GLuint *prog_slot, gpu_program_uniforms_t *unif_slot,
                                     const char *src, const char *name)
{
    if (*prog_slot == 0u) {
        *prog_slot = compile_shader_program(src, name);
        if (*prog_slot != 0u) {
            query_program_uniforms(*prog_slot, unif_slot);
        }
    }
    return *prog_slot;
}

static waywal_transition_type_t resolve_transition_type(gpu_compute_ctx_t *ctx,
                                                        waywal_transition_type_t type,
                                                        float progress, const void *old_key,
                                                        const void *new_key)
{
    if (type != WAYWAL_TRANSITION_RANDOM) {
        ctx->random_active = false;
        return type;
    }

    bool start_new = !ctx->random_active || old_key != ctx->random_old_key ||
                     new_key != ctx->random_new_key ||
                     (progress + 1e-4f < ctx->random_last_progress);
    if (start_new) {
        uint32_t roll = wwal_xorshift32(&ctx->rng_state);
        ctx->random_resolved_type = (waywal_transition_type_t)(1u + (roll % 19u));
        ctx->random_old_key = old_key;
        ctx->random_new_key = new_key;
        ctx->random_active = true;
    }
    ctx->random_last_progress = progress;

    waywal_transition_type_t resolved = ctx->random_resolved_type;
    if (progress >= 1.0f) {
        ctx->random_active = false;
    }
    return resolved;
}

static void select_transition_program(gpu_compute_ctx_t *ctx, waywal_transition_type_t type,
                                      GLuint *out_prog, gpu_program_uniforms_t **out_u)
{
    GLuint prog = 0u;
    gpu_program_uniforms_t *u = &ctx->unif_fade;
    switch (type) {
    case WAYWAL_TRANSITION_WIPE:
        prog = ensure_transition_prog(&ctx->prog_wipe, &ctx->unif_wipe, SHADER_SRC_WIPE, "wipe");
        u = &ctx->unif_wipe;
        break;
    case WAYWAL_TRANSITION_GROW:
        prog = ensure_transition_prog(&ctx->prog_grow, &ctx->unif_grow, SHADER_SRC_GROW, "grow");
        u = &ctx->unif_grow;
        break;
    case WAYWAL_TRANSITION_OUTER:
        prog =
            ensure_transition_prog(&ctx->prog_outer, &ctx->unif_outer, SHADER_SRC_OUTER, "outer");
        u = &ctx->unif_outer;
        break;
    case WAYWAL_TRANSITION_WAVE:
        prog = ensure_transition_prog(&ctx->prog_wave, &ctx->unif_wave, SHADER_SRC_WAVE, "wave");
        u = &ctx->unif_wave;
        break;
    case WAYWAL_TRANSITION_NOISE:
        prog =
            ensure_transition_prog(&ctx->prog_noise, &ctx->unif_noise, SHADER_SRC_NOISE, "noise");
        u = &ctx->unif_noise;
        break;
    case WAYWAL_TRANSITION_CROSSZOOM:
        prog = ensure_transition_prog(&ctx->prog_crosszoom, &ctx->unif_crosszoom,
                                      SHADER_SRC_CROSSZOOM, "crosszoom");
        u = &ctx->unif_crosszoom;
        break;
    case WAYWAL_TRANSITION_SLIDE:
        prog =
            ensure_transition_prog(&ctx->prog_slide, &ctx->unif_slide, SHADER_SRC_SLIDE, "slide");
        u = &ctx->unif_slide;
        break;
    case WAYWAL_TRANSITION_GLITCH:
        prog = ensure_transition_prog(&ctx->prog_glitch, &ctx->unif_glitch, SHADER_SRC_GLITCH,
                                      "glitch");
        u = &ctx->unif_glitch;
        break;
    case WAYWAL_TRANSITION_BURN:
        prog = ensure_transition_prog(&ctx->prog_burn, &ctx->unif_burn, SHADER_SRC_BURN, "burn");
        u = &ctx->unif_burn;
        break;
    case WAYWAL_TRANSITION_RIPPLE:
        prog = ensure_transition_prog(&ctx->prog_ripple, &ctx->unif_ripple, SHADER_SRC_RIPPLE,
                                      "ripple");
        u = &ctx->unif_ripple;
        break;
    case WAYWAL_TRANSITION_PIXELATE:
        prog = ensure_transition_prog(&ctx->prog_pixelate, &ctx->unif_pixelate, SHADER_SRC_PIXELATE,
                                      "pixelate");
        u = &ctx->unif_pixelate;
        break;
    case WAYWAL_TRANSITION_DOOM:
        prog = ensure_transition_prog(&ctx->prog_doom, &ctx->unif_doom, SHADER_SRC_DOOM, "doom");
        u = &ctx->unif_doom;
        break;
    case WAYWAL_TRANSITION_SWIRL:
        prog =
            ensure_transition_prog(&ctx->prog_swirl, &ctx->unif_swirl, SHADER_SRC_SWIRL, "swirl");
        u = &ctx->unif_swirl;
        break;
    case WAYWAL_TRANSITION_CUBE:
        prog = ensure_transition_prog(&ctx->prog_cube, &ctx->unif_cube, SHADER_SRC_CUBE, "cube");
        u = &ctx->unif_cube;
        break;
    case WAYWAL_TRANSITION_LUMA:
        prog = ensure_transition_prog(&ctx->prog_luma, &ctx->unif_luma, SHADER_SRC_LUMA, "luma");
        u = &ctx->unif_luma;
        break;
    case WAYWAL_TRANSITION_LIGHT_LEAK:
        prog = ensure_transition_prog(&ctx->prog_light_leak, &ctx->unif_light_leak,
                                      SHADER_SRC_LIGHT_LEAK, "light_leak");
        u = &ctx->unif_light_leak;
        break;
    case WAYWAL_TRANSITION_PAGE_CURL:
        prog = ensure_transition_prog(&ctx->prog_page_curl, &ctx->unif_page_curl,
                                      SHADER_SRC_PAGE_CURL, "page_curl");
        u = &ctx->unif_page_curl;
        break;
    case WAYWAL_TRANSITION_CUSTOM:
        if (ctx->prog_custom) {
            prog = ctx->prog_custom;
            u = &ctx->unif_custom;
        } else {
            prog =
                ensure_transition_prog(&ctx->prog_fade, &ctx->unif_fade, SHADER_SRC_FADE, "fade");
            u = &ctx->unif_fade;
        }
        break;
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE:
    default:
        prog = ensure_transition_prog(&ctx->prog_fade, &ctx->unif_fade, SHADER_SRC_FADE, "fade");
        u = &ctx->unif_fade;
        break;
    }
    *out_prog = prog;
    *out_u = u;
}

static void dispatch_transition_compute(gpu_compute_ctx_t *ctx, GLuint prog,
                                        gpu_program_uniforms_t *u, GLuint dst_image_tex,
                                        GLuint old_tex, GLuint new_tex, uint32_t width,
                                        uint32_t height, float progress,
                                        const waywal_transition_params_t *params)
{
    if (ctx->current_prog != prog) {
        glUseProgram(prog);
        ctx->current_prog = prog;
    }

    if (ctx->bound_target_tex != dst_image_tex) {
        glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = dst_image_tex;
    }

    if (ctx->bound_old_tex != old_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, old_tex);
        ctx->bound_old_tex = old_tex;
    }

    if (ctx->bound_new_tex != new_tex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, new_tex);
        ctx->bound_new_tex = new_tex;
    }

    float angle_rad = isfinite(params->angle_rad) ? params->angle_rad : 0.0f;
    float wave_freq = isfinite(params->wave_freq) ? params->wave_freq : 20.0f;
    float wave_amp = isfinite(params->wave_amp) ? params->wave_amp : 0.05f;
    float center_x = isfinite(params->center_x) ? params->center_x : 0.5f;
    float center_y = isfinite(params->center_y) ? params->center_y : 0.5f;

    if (u->loc_prog >= 0 && u->last_prog != progress) {
        glUniform1f(u->loc_prog, progress);
        u->last_prog = progress;
    }
    if (u->loc_res >= 0 && (u->last_w != (GLint)width || u->last_h != (GLint)height)) {
        glUniform2i(u->loc_res, (GLint)width, (GLint)height);
        u->last_w = (GLint)width;
        u->last_h = (GLint)height;
    }
    if (u->loc_angle >= 0 && u->last_angle != angle_rad) {
        glUniform1f(u->loc_angle, angle_rad);
        u->last_angle = angle_rad;
    }
    if (u->loc_wave_freq >= 0 && u->last_wave_freq != wave_freq) {
        glUniform1f(u->loc_wave_freq, wave_freq);
        u->last_wave_freq = wave_freq;
    }
    if (u->loc_wave_amp >= 0 && u->last_wave_amp != wave_amp) {
        glUniform1f(u->loc_wave_amp, wave_amp);
        u->last_wave_amp = wave_amp;
    }
    if (u->loc_center >= 0 && (u->last_center_x != center_x || u->last_center_y != center_y)) {
        glUniform2f(u->loc_center, center_x, center_y);
        u->last_center_x = center_x;
        u->last_center_y = center_y;
    }
    if (u->loc_smoothness >= 0) {
        float sm = wave_amp > 0.0f ? wave_amp : 0.2f;
        if (u->last_smoothness != sm) {
            glUniform1f(u->loc_smoothness, sm);
            u->last_smoothness = sm;
        }
    }
    if (u->loc_step >= 0) {
        float st = wave_freq > 0.0f ? wave_freq : 1.0f;
        if (u->last_step != st) {
            glUniform1f(u->loc_step, st);
            u->last_step = st;
        }
    }

    GLuint groups_x = (width + 15u) / 16u;
    GLuint groups_y = (height + 15u) / 16u;
    glDispatchCompute(groups_x, groups_y, 1u);
}

bool gpu_compute_has_native_fence(const gpu_compute_ctx_t *ctx)
{
    return ctx && ctx->sync_procs.has_native_fence_sync;
}

int gpu_compute_export_fence_fd(gpu_compute_ctx_t *ctx)
{
    if (!ctx || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT) {
        return -1;
    }
    gpu_compute_make_current(ctx);
    return wwal_egl_create_native_fence_fd(ctx->display, &ctx->sync_procs);
}

bool gpu_compute_wait_idle(gpu_compute_ctx_t *ctx)
{
    if (!ctx || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT) {
        return false;
    }
    gpu_compute_make_current(ctx);
    wwal_egl_wait_sync_fallback(ctx->display, &ctx->sync_procs);
    return true;
}

waywal_transition_type_t gpu_compute_get_active_random_transition(const gpu_compute_ctx_t *ctx)
{
    if (!ctx || !ctx->random_active) {
        return WAYWAL_TRANSITION_NONE;
    }
    return ctx->random_resolved_type;
}

bool gpu_compute_dispatch_transition_async(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                           dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                           const waywal_transition_params_t *params,
                                           int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!ctx || !target_bo || !old_bo || !new_bo || !params || target_bo->width == 0u ||
        target_bo->height == 0u) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (!gpu_compute_import_bo(ctx, target_bo) || !gpu_compute_import_bo(ctx, old_bo) ||
        !gpu_compute_import_bo(ctx, new_bo)) {
        return false;
    }

    bool use_scratch = gpu_compute_needs_scratch(ctx, target_bo);
    GLuint dst_image_tex = target_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_linear_scratch_target(ctx, target_bo->width, target_bo->height);
        if (!dst_image_tex) {
            return false;
        }
    }

    float progress = isfinite(params->progress) ? params->progress : 1.0f;
    if (progress < 0.0f) {
        progress = 0.0f;
    } else if (progress > 1.0f) {
        progress = 1.0f;
    }

    waywal_transition_type_t eff_type =
        resolve_transition_type(ctx, params->type, progress, old_bo, new_bo);
    if (progress <= 0.0f || progress >= 1.0f) {
        eff_type = WAYWAL_TRANSITION_FADE;
    }

    GLuint prog = 0u;
    gpu_program_uniforms_t *u = NULL;
    select_transition_program(ctx, eff_type, &prog, &u);
    if (!prog || !u) {
        return false;
    }

    dispatch_transition_compute(ctx, prog, u, dst_image_tex, old_bo->gl_tex, new_bo->gl_tex,
                                target_bo->width, target_bo->height, progress, params);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!present_scratch_to_bo(ctx, dst_image_tex, target_bo)) {
            return false;
        }
    }

    wwal_egl_sync_before_commit(ctx->display, &ctx->sync_procs, out_fence_fd);
    return true;
}

bool gpu_compute_dispatch_transition(gpu_compute_ctx_t *ctx, dmabuf_bo_t *target_bo,
                                     dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                     const waywal_transition_params_t *params)
{
    return gpu_compute_dispatch_transition_async(ctx, target_bo, old_bo, new_bo, params, NULL);
}

bool gpu_compute_prepare_shm(gpu_compute_ctx_t *ctx, uint32_t *tex_old, uint32_t *tex_new,
                             uint32_t *tex_target, uint32_t *fbo, uint32_t *cur_w, uint32_t *cur_h,
                             uint32_t width, uint32_t height, const uint32_t *old_pixels,
                             const uint32_t *new_pixels)
{
    if (!ctx || !tex_old || !tex_new || !tex_target || !fbo || !cur_w || !cur_h || width == 0u ||
        height == 0u || !old_pixels || !new_pixels) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (*cur_w != width || *cur_h != height || *tex_old == 0u || *tex_new == 0u ||
        *tex_target == 0u || *fbo == 0u) {
        gpu_compute_release_shm_res(ctx, tex_old, tex_new, tex_target, fbo);

        GLuint texs[3] = {0u, 0u, 0u};
        glGenTextures(3, texs);
        *tex_old = texs[0];
        *tex_new = texs[1];
        *tex_target = texs[2];

        glActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < 3; ++i) {
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height);
        }

        /* WL_SHM ARGB8888 in little-endian memory is [B, G, R, A]; swizzle sampler channels so
         * compute shaders observe R, G, B, A. */
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint my_fbo = 0u;
        glGenFramebuffers(1, &my_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, my_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex_target, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        *fbo = my_fbo;

        *cur_w = width;
        *cur_h = height;
    }

    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, *tex_old);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA,
                    GL_UNSIGNED_BYTE, old_pixels);

    glBindTexture(GL_TEXTURE_2D, *tex_new);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA,
                    GL_UNSIGNED_BYTE, new_pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->bound_target_tex = 0u;
    ctx->bound_old_tex = 0u;
    ctx->bound_new_tex = 0u;
    return true;
}

bool gpu_compute_dispatch_transition_shm(gpu_compute_ctx_t *ctx, uint32_t tex_old, uint32_t tex_new,
                                         uint32_t tex_target, uint32_t fbo, uint32_t width,
                                         uint32_t height, const waywal_transition_params_t *params,
                                         uint32_t *dst_pixels)
{
    if (!ctx || tex_old == 0u || tex_new == 0u || tex_target == 0u || fbo == 0u || width == 0u ||
        height == 0u || !params || !dst_pixels) {
        return false;
    }

    gpu_compute_make_current(ctx);

    float progress = isfinite(params->progress) ? params->progress : 1.0f;
    if (progress < 0.0f) {
        progress = 0.0f;
    } else if (progress > 1.0f) {
        progress = 1.0f;
    }

    waywal_transition_type_t eff_type =
        resolve_transition_type(ctx, params->type, progress, (const void *)(uintptr_t)tex_old,
                                (const void *)(uintptr_t)tex_new);
    if (progress <= 0.0f || progress >= 1.0f) {
        eff_type = WAYWAL_TRANSITION_FADE;
    }

    GLuint prog = 0u;
    gpu_program_uniforms_t *u = NULL;
    select_transition_program(ctx, eff_type, &prog, &u);
    if (!prog || !u) {
        return false;
    }

    dispatch_transition_compute(ctx, prog, u, tex_target, tex_old, tex_new, width, height, progress,
                                params);

    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
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
    if (ctx && tex_old && *tex_old != 0u &&
        ctx->random_old_key == (const void *)(uintptr_t)(*tex_old)) {
        ctx->random_active = false;
    }
    if (ctx && tex_new && *tex_new != 0u &&
        ctx->random_new_key == (const void *)(uintptr_t)(*tex_new)) {
        ctx->random_active = false;
    }
    if (tex_old && *tex_old != 0u) {
        glDeleteTextures(1, tex_old);
        *tex_old = 0u;
    }
    if (tex_new && *tex_new != 0u) {
        glDeleteTextures(1, tex_new);
        *tex_new = 0u;
    }
    if (tex_target && *tex_target != 0u) {
        glDeleteTextures(1, tex_target);
        *tex_target = 0u;
    }
    if (fbo && *fbo != 0u) {
        glDeleteFramebuffers(1, fbo);
        *fbo = 0u;
    }
    if (ctx) {
        ctx->bound_target_tex = 0u;
        ctx->bound_old_tex = 0u;
        ctx->bound_new_tex = 0u;
    }
}

void gpu_compute_destroy(gpu_compute_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

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
        if (ctx->upload_src_tex)
            glDeleteTextures(1, &ctx->upload_src_tex);
        if (ctx->kawase_half_tex)
            glDeleteTextures(1, &ctx->kawase_half_tex);
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (ctx->scratch_pool_tex[i])
                glDeleteTextures(1, &ctx->scratch_pool_tex[i]);
        }
        wwal_scratch_present_destroy(&ctx->present_state);

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
    if (!ctx || !bo || (bo->gl_tex == 0u && bo->egl_image == NULL)) {
        return;
    }
    if (ctx->random_old_key == bo || ctx->random_new_key == bo) {
        ctx->random_active = false;
    }
    if (ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != ctx->context) {
            eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        }
    }
    if (bo->gl_tex != 0u) {
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (ctx->present_state.draw_fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, ctx->present_state.draw_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            ctx->present_state.bound_fbo_tex = 0u;
        }
        glDeleteTextures(1, &bo->gl_tex);
        bo->gl_tex = 0u;
    }
    if (bo->egl_image != NULL && ctx->eglDestroyImageKHR && ctx->display != EGL_NO_DISPLAY) {
        ctx->eglDestroyImageKHR(ctx->display, bo->egl_image);
        bo->egl_image = NULL;
    }
    gpu_compute_invalidate_state_cache(ctx);
    glFlush();
    glFinish();
}

bool gpu_compute_load_custom_shader(gpu_compute_ctx_t *ctx, const char *src)
{
    if (!ctx || !src || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT) {
        return false;
    }

    eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);

    if (ctx->prog_custom) {
        glDeleteProgram(ctx->prog_custom);
        ctx->prog_custom = 0u;
    }
    ctx->current_prog = 0u;

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
    if (!ctx) {
        return;
    }
    ctx->upload_gen++;
    if (ctx->upload_gen == 0u) {
        ctx->upload_gen = 1u;
    }
}

static void gpu_compute_release_scratch(gpu_compute_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    bool any = (ctx->kawase_half_tex != 0u || ctx->scratch_tex != 0u);
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (ctx->scratch_pool_tex[i] != 0u) {
            any = true;
        }
    }
    if (!any) {
        return;
    }

    gpu_compute_make_current(ctx);

    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (ctx->present_state.draw_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->present_state.draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ctx->present_state.bound_fbo_tex = 0u;
    }

    if (ctx->kawase_half_tex != 0u) {
        glDeleteTextures(1, &ctx->kawase_half_tex);
        ctx->kawase_half_tex = 0u;
        ctx->kawase_half_w = 0u;
        ctx->kawase_half_h = 0u;
    }

    for (uint32_t i = 0u; i < 4u; ++i) {
        if (ctx->scratch_pool_tex[i] != 0u) {
            glDeleteTextures(1, &ctx->scratch_pool_tex[i]);
            ctx->scratch_pool_tex[i] = 0u;
            ctx->scratch_pool_w[i] = 0u;
            ctx->scratch_pool_h[i] = 0u;
        }
    }
    ctx->scratch_tex = 0u;
    ctx->scratch_w = 0u;
    ctx->scratch_h = 0u;
    gpu_compute_invalidate_state_cache(ctx);
    glFlush();
    glFinish();
}

void gpu_compute_release_upload_cache(gpu_compute_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->upload_src_tex != 0u) {
        gpu_compute_make_current(ctx);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        ctx->bound_old_tex = 0u;
        ctx->bound_new_tex = 0u;
        glDeleteTextures(1, &ctx->upload_src_tex);
        ctx->upload_src_tex = 0u;
        ctx->upload_src_w = 0u;
        ctx->upload_src_h = 0u;
        ctx->last_upload_ptr = NULL;
        ctx->last_upload_w = 0u;
        ctx->last_upload_h = 0u;
        ctx->last_upload_gen = 0u;
        ctx->last_upload_hash0 = 0u;
        ctx->last_upload_hash1 = 0u;
        glFlush();
        glFinish();
    }
    gpu_compute_release_scratch(ctx);
}

static bool upload_source_rgba_dedup(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                     uint32_t src_w, uint32_t src_h)
{
    if (!ctx || !src_rgba || src_w == 0u || src_h == 0u ||
        (size_t)src_w > SIZE_MAX / 4u / (size_t)src_h) {
        return false;
    }
    size_t byte_len = (size_t)src_w * (size_t)src_h * 4u;

    if (ctx->upload_src_tex == 0u || ctx->upload_src_w != src_w || ctx->upload_src_h != src_h) {
        if (ctx->upload_src_tex != 0u) {
            if (ctx->bound_old_tex == ctx->upload_src_tex) {
                ctx->bound_old_tex = 0u;
            }
            if (ctx->bound_new_tex == ctx->upload_src_tex) {
                ctx->bound_new_tex = 0u;
            }
            glDeleteTextures(1, &ctx->upload_src_tex);
            ctx->upload_src_tex = 0u;
        }
        glGenTextures(1, &ctx->upload_src_tex);
        if (ctx->upload_src_tex == 0u) {
            return false;
        }
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
        glBindTexture(GL_TEXTURE_2D, 0);
        ctx->upload_src_w = src_w;
        ctx->upload_src_h = src_h;
        ctx->last_upload_ptr = NULL;
        ctx->last_upload_gen = 0u;
        ctx->last_upload_hash0 = 0u;
        ctx->last_upload_hash1 = 0u;
    }

    uint64_t h0 = 0u, h1 = 0u;
    hash_rgba_buffer_128(src_rgba, byte_len, &h0, &h1);

    /* Deduplicate only when pointer, dimensions, generation, and full pixel digest all match so
     * in-place caller mutations are always uploaded. */
    if (ctx->last_upload_ptr == (const void *)src_rgba && ctx->last_upload_gen == ctx->upload_gen &&
        ctx->last_upload_w == src_w && ctx->last_upload_h == src_h &&
        ctx->last_upload_hash0 == h0 && ctx->last_upload_hash1 == h1) {
        return true;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)src_w, (GLsizei)src_h, GL_RGBA,
                    GL_UNSIGNED_BYTE, src_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->last_upload_ptr = (const void *)src_rgba;
    ctx->last_upload_w = src_w;
    ctx->last_upload_h = src_h;
    ctx->last_upload_gen = ctx->upload_gen;
    ctx->last_upload_hash0 = h0;
    ctx->last_upload_hash1 = h1;
    return true;
}

static GLuint ensure_kawase_half_tex(gpu_compute_ctx_t *ctx, uint32_t width, uint32_t height)
{
    if (ctx->kawase_half_tex != 0u && ctx->kawase_half_w == width && ctx->kawase_half_h == height) {
        return ctx->kawase_half_tex;
    }
    if (ctx->kawase_half_tex != 0u) {
        if (ctx->bound_target_tex == ctx->kawase_half_tex) {
            ctx->bound_target_tex = 0u;
        }
        if (ctx->bound_old_tex == ctx->kawase_half_tex) {
            ctx->bound_old_tex = 0u;
        }
        glDeleteTextures(1, &ctx->kawase_half_tex);
        ctx->kawase_half_tex = 0u;
    }
    glGenTextures(1, &ctx->kawase_half_tex);
    if (ctx->kawase_half_tex == 0u) {
        return 0u;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->kawase_half_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)width, (GLsizei)height);
    glBindTexture(GL_TEXTURE_2D, 0);
    ctx->kawase_half_w = width;
    ctx->kawase_half_h = height;
    return ctx->kawase_half_tex;
}

void gpu_compute_set_span_bounds(gpu_compute_ctx_t *ctx, int32_t off_x, int32_t off_y,
                                 int32_t total_w, int32_t total_h)
{
    if (!ctx) {
        return;
    }
    ctx->span_bounds[0] = off_x;
    ctx->span_bounds[1] = off_y;
    ctx->span_bounds[2] = total_w;
    ctx->span_bounds[3] = total_h;
}

void gpu_compute_set_crop_gravity(gpu_compute_ctx_t *ctx, float gx, float gy)
{
    if (!ctx) {
        return;
    }
    if (!isfinite(gx)) {
        gx = 0.5f;
    }
    if (!isfinite(gy)) {
        gy = 0.5f;
    }
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

static void dispatch_scale_compute(gpu_compute_ctx_t *ctx, GLuint dst_image_tex, GLuint src_tex,
                                   uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                   waywal_scaling_mode_t scaling, uint32_t clear_color,
                                   bool has_src)
{
    if (ctx->current_prog != ctx->prog_scale) {
        glUseProgram(ctx->prog_scale);
        ctx->current_prog = ctx->prog_scale;
    }

    if (ctx->bound_target_tex != dst_image_tex) {
        glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = dst_image_tex;
    }

    if (has_src && ctx->bound_old_tex != src_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        ctx->bound_old_tex = src_tex;
    }

    float ca = (float)((clear_color >> 24) & 0xFFu) / 255.0f;
    float cr = (float)((clear_color >> 16) & 0xFFu) / 255.0f;
    float cg = (float)((clear_color >> 8) & 0xFFu) / 255.0f;
    float cb = (float)(clear_color & 0xFFu) / 255.0f;

    if (ctx->loc_scale_src_res >= 0 &&
        (ctx->last_scale_src_w != (GLint)src_w || ctx->last_scale_src_h != (GLint)src_h)) {
        glUniform2i(ctx->loc_scale_src_res, (GLint)src_w, (GLint)src_h);
        ctx->last_scale_src_w = (GLint)src_w;
        ctx->last_scale_src_h = (GLint)src_h;
    }
    if (ctx->loc_scale_dst_res >= 0 &&
        (ctx->last_scale_dst_w != (GLint)dst_w || ctx->last_scale_dst_h != (GLint)dst_h)) {
        glUniform2i(ctx->loc_scale_dst_res, (GLint)dst_w, (GLint)dst_h);
        ctx->last_scale_dst_w = (GLint)dst_w;
        ctx->last_scale_dst_h = (GLint)dst_h;
    }
    if (ctx->loc_scale_mode >= 0 && ctx->last_scale_mode != (GLint)scaling) {
        glUniform1i(ctx->loc_scale_mode, (GLint)scaling);
        ctx->last_scale_mode = (GLint)scaling;
    }
    if (ctx->loc_scale_clear >= 0 && ctx->last_scale_clear != clear_color) {
        glUniform4f(ctx->loc_scale_clear, cr, cg, cb, ca);
        ctx->last_scale_clear = clear_color;
    }
    GLint has_src_val = has_src ? 1 : 0;
    if (ctx->loc_scale_has_src >= 0 && ctx->last_scale_has_src != has_src_val) {
        glUniform1i(ctx->loc_scale_has_src, has_src_val);
        ctx->last_scale_has_src = has_src_val;
    }
    if (ctx->loc_scale_span_bounds >= 0 &&
        (ctx->last_scale_span_bounds[0] != ctx->span_bounds[0] ||
         ctx->last_scale_span_bounds[1] != ctx->span_bounds[1] ||
         ctx->last_scale_span_bounds[2] != ctx->span_bounds[2] ||
         ctx->last_scale_span_bounds[3] != ctx->span_bounds[3])) {
        glUniform4i(ctx->loc_scale_span_bounds, ctx->span_bounds[0], ctx->span_bounds[1],
                    ctx->span_bounds[2], ctx->span_bounds[3]);
        ctx->last_scale_span_bounds[0] = ctx->span_bounds[0];
        ctx->last_scale_span_bounds[1] = ctx->span_bounds[1];
        ctx->last_scale_span_bounds[2] = ctx->span_bounds[2];
        ctx->last_scale_span_bounds[3] = ctx->span_bounds[3];
    }
    if (ctx->loc_scale_gravity >= 0 && (ctx->last_scale_crop_gravity[0] != ctx->crop_gravity[0] ||
                                        ctx->last_scale_crop_gravity[1] != ctx->crop_gravity[1])) {
        glUniform2f(ctx->loc_scale_gravity, ctx->crop_gravity[0], ctx->crop_gravity[1]);
        ctx->last_scale_crop_gravity[0] = ctx->crop_gravity[0];
        ctx->last_scale_crop_gravity[1] = ctx->crop_gravity[1];
    }

    GLuint groups_x = (dst_w + 15u) / 16u;
    GLuint groups_y = (dst_h + 15u) / 16u;
    glDispatchCompute(groups_x, groups_y, 1u);
}

bool gpu_compute_upload_static_async(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                     uint32_t src_w, uint32_t src_h, dmabuf_bo_t *dst_bo,
                                     waywal_scaling_mode_t scaling, uint32_t clear_color,
                                     int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!ctx || !dst_bo || dst_bo->width == 0u || dst_bo->height == 0u || !ctx->prog_scale) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (!gpu_compute_import_bo(ctx, dst_bo)) {
        return false;
    }

    bool has_src = (src_rgba != NULL && src_w > 0u && src_h > 0u);
    if (has_src && !upload_source_rgba_dedup(ctx, src_rgba, src_w, src_h)) {
        return false;
    }

    bool use_scratch = gpu_compute_needs_scratch(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_linear_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex) {
            return false;
        }
    }

    dispatch_scale_compute(ctx, dst_image_tex, has_src ? ctx->upload_src_tex : 0u, src_w, src_h,
                           dst_bo->width, dst_bo->height, scaling, clear_color, has_src);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!present_scratch_to_bo(ctx, dst_image_tex, dst_bo)) {
            return false;
        }
    }

    wwal_egl_sync_before_commit(ctx->display, &ctx->sync_procs, out_fence_fd);
    return true;
}

bool gpu_compute_upload_static(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                               uint32_t src_h, dmabuf_bo_t *dst_bo, waywal_scaling_mode_t scaling,
                               uint32_t clear_color)
{
    return gpu_compute_upload_static_async(ctx, src_rgba, src_w, src_h, dst_bo, scaling,
                                           clear_color, NULL);
}

void gpu_compute_make_current(gpu_compute_ctx_t *ctx)
{
    if (!ctx || ctx->display == EGL_NO_DISPLAY || ctx->context == EGL_NO_CONTEXT) {
        return;
    }
    if (eglGetCurrentContext() != ctx->context) {
        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
        gpu_compute_invalidate_state_cache(ctx);
    }
}

bool gpu_compute_scale_bo_async(gpu_compute_ctx_t *ctx, dmabuf_bo_t *src_bo, dmabuf_bo_t *dst_bo,
                                waywal_scaling_mode_t scaling, uint32_t clear_color,
                                bool release_src_gl, int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!ctx || !src_bo || !dst_bo || src_bo->width == 0u || src_bo->height == 0u ||
        dst_bo->width == 0u || dst_bo->height == 0u || !ctx->prog_scale) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (!gpu_compute_import_bo(ctx, src_bo) || !gpu_compute_import_bo(ctx, dst_bo)) {
        if (release_src_gl) {
            gpu_compute_release_bo(ctx, src_bo);
        }
        return false;
    }

    bool use_scratch = gpu_compute_needs_scratch(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_linear_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex) {
            if (release_src_gl) {
                gpu_compute_release_bo(ctx, src_bo);
            }
            return false;
        }
    }

    dispatch_scale_compute(ctx, dst_image_tex, src_bo->gl_tex, src_bo->width, src_bo->height,
                           dst_bo->width, dst_bo->height, scaling, clear_color, true);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!present_scratch_to_bo(ctx, dst_image_tex, dst_bo)) {
            if (release_src_gl) {
                gpu_compute_release_bo(ctx, src_bo);
            }
            return false;
        }
    }

    if (release_src_gl) {
        /* Synchronize before destroying src_bo's EGLImage/texture so sampling completes first. */
        wwal_egl_wait_sync_fallback(ctx->display, &ctx->sync_procs);
        gpu_compute_release_bo(ctx, src_bo);
    } else {
        wwal_egl_sync_before_commit(ctx->display, &ctx->sync_procs, out_fence_fd);
    }
    return true;
}

bool gpu_compute_scale_bo(gpu_compute_ctx_t *ctx, dmabuf_bo_t *src_bo, dmabuf_bo_t *dst_bo,
                          waywal_scaling_mode_t scaling, uint32_t clear_color, bool release_src_gl)
{
    return gpu_compute_scale_bo_async(ctx, src_bo, dst_bo, scaling, clear_color, release_src_gl,
                                      NULL);
}

bool gpu_compute_render_kawase_blur_async(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba,
                                          uint32_t src_w, uint32_t src_h, dmabuf_bo_t *dst_bo,
                                          uint32_t radius, int *out_fence_fd)
{
    if (out_fence_fd) {
        *out_fence_fd = -1;
    }
    if (!ctx || !src_rgba || src_w == 0u || src_h == 0u || !dst_bo || dst_bo->width == 0u ||
        dst_bo->height == 0u) {
        return false;
    }

    gpu_compute_make_current(ctx);

    if (!ctx->prog_kawase) {
        ctx->prog_kawase = compile_shader_program(SHADER_SRC_KAWASE, "kawase");
        if (!ctx->prog_kawase) {
            return false;
        }
        ctx->loc_kawase_src_res = glGetUniformLocation(ctx->prog_kawase, "u_src_res");
        ctx->loc_kawase_dst_res = glGetUniformLocation(ctx->prog_kawase, "u_dst_res");
        ctx->loc_kawase_radius = glGetUniformLocation(ctx->prog_kawase, "u_radius");
    }

    if (!gpu_compute_import_bo(ctx, dst_bo)) {
        return false;
    }

    if (!upload_source_rgba_dedup(ctx, src_rgba, src_w, src_h)) {
        return false;
    }

    uint32_t half_w = dst_bo->width > 1u ? (dst_bo->width / 2u) : 1u;
    uint32_t half_h = dst_bo->height > 1u ? (dst_bo->height / 2u) : 1u;
    GLuint half_tex = ensure_kawase_half_tex(ctx, half_w, half_h);
    if (!half_tex) {
        return false;
    }

    if (ctx->current_prog != ctx->prog_kawase) {
        glUseProgram(ctx->prog_kawase);
        ctx->current_prog = ctx->prog_kawase;
    }

    /* Pass 1: Downsample + Kawase pre-blur into half_tex. */
    if (ctx->bound_target_tex != half_tex) {
        glBindImageTexture(0, half_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = half_tex;
    }
    if (ctx->bound_old_tex != ctx->upload_src_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx->upload_src_tex);
        ctx->bound_old_tex = ctx->upload_src_tex;
    }

    uint32_t clamped_radius = radius > 64u ? 64u : radius;
    float rad = (clamped_radius == 0u) ? 0.0f : (0.5f * (float)clamped_radius);
    if (ctx->loc_kawase_src_res >= 0 &&
        (ctx->last_kawase_src_w != (GLint)src_w || ctx->last_kawase_src_h != (GLint)src_h)) {
        glUniform2i(ctx->loc_kawase_src_res, (GLint)src_w, (GLint)src_h);
        ctx->last_kawase_src_w = (GLint)src_w;
        ctx->last_kawase_src_h = (GLint)src_h;
    }
    if (ctx->loc_kawase_dst_res >= 0 &&
        (ctx->last_kawase_dst_w != (GLint)half_w || ctx->last_kawase_dst_h != (GLint)half_h)) {
        glUniform2i(ctx->loc_kawase_dst_res, (GLint)half_w, (GLint)half_h);
        ctx->last_kawase_dst_w = (GLint)half_w;
        ctx->last_kawase_dst_h = (GLint)half_h;
    }
    if (ctx->loc_kawase_radius >= 0 && ctx->last_kawase_radius != rad) {
        glUniform1f(ctx->loc_kawase_radius, rad);
        ctx->last_kawase_radius = rad;
    }

    glDispatchCompute((half_w + 15u) / 16u, (half_h + 15u) / 16u, 1u);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    /* Pass 2: Upsample + Kawase blur from half_tex into dst_bo / linear scratch. */
    bool use_scratch = gpu_compute_needs_scratch(ctx, dst_bo);
    GLuint dst_image_tex = dst_bo->gl_tex;
    if (use_scratch) {
        dst_image_tex = ensure_linear_scratch_target(ctx, dst_bo->width, dst_bo->height);
        if (!dst_image_tex) {
            return false;
        }
    }

    if (ctx->bound_target_tex != dst_image_tex) {
        glBindImageTexture(0, dst_image_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        ctx->bound_target_tex = dst_image_tex;
    }

    if (ctx->bound_old_tex != half_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, half_tex);
        ctx->bound_old_tex = half_tex;
    }

    if (ctx->loc_kawase_src_res >= 0 &&
        (ctx->last_kawase_src_w != (GLint)half_w || ctx->last_kawase_src_h != (GLint)half_h)) {
        glUniform2i(ctx->loc_kawase_src_res, (GLint)half_w, (GLint)half_h);
        ctx->last_kawase_src_w = (GLint)half_w;
        ctx->last_kawase_src_h = (GLint)half_h;
    }
    if (ctx->loc_kawase_dst_res >= 0 && (ctx->last_kawase_dst_w != (GLint)dst_bo->width ||
                                         ctx->last_kawase_dst_h != (GLint)dst_bo->height)) {
        glUniform2i(ctx->loc_kawase_dst_res, (GLint)dst_bo->width, (GLint)dst_bo->height);
        ctx->last_kawase_dst_w = (GLint)dst_bo->width;
        ctx->last_kawase_dst_h = (GLint)dst_bo->height;
    }

    GLuint groups_x = (dst_bo->width + 15u) / 16u;
    GLuint groups_y = (dst_bo->height + 15u) / 16u;
    glDispatchCompute(groups_x, groups_y, 1u);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (use_scratch) {
        if (!present_scratch_to_bo(ctx, dst_image_tex, dst_bo)) {
            return false;
        }
    }

    wwal_egl_sync_before_commit(ctx->display, &ctx->sync_procs, out_fence_fd);
    return true;
}

bool gpu_compute_render_kawase_blur(gpu_compute_ctx_t *ctx, const uint8_t *src_rgba, uint32_t src_w,
                                    uint32_t src_h, dmabuf_bo_t *dst_bo, uint32_t radius)
{
    return gpu_compute_render_kawase_blur_async(ctx, src_rgba, src_w, src_h, dst_bo, radius, NULL);
}

static inline uint32_t avg_u32_epu8_scalar(uint32_t a, uint32_t b)
{
    uint32_t rb = (((a & 0x00FF00FFu) + (b & 0x00FF00FFu) + 0x00010001u) >> 1) & 0x00FF00FFu;
    uint32_t ag =
        ((((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + 0x00010001u) >> 1) & 0x00FF00FFu;
    return rb | (ag << 8);
}

static inline uint32_t avg4_u32_swar(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3)
{
    uint32_t rb = (p0 & 0x00FF00FFu) + (p1 & 0x00FF00FFu) + (p2 & 0x00FF00FFu) +
                  (p3 & 0x00FF00FFu) + 0x00020002u;
    uint32_t ag = ((p0 >> 8) & 0x00FF00FFu) + ((p1 >> 8) & 0x00FF00FFu) +
                  ((p2 >> 8) & 0x00FF00FFu) + ((p3 >> 8) & 0x00FF00FFu) + 0x00020002u;
    return ((rb >> 2) & 0x00FF00FFu) | (((ag >> 2) & 0x00FF00FFu) << 8);
}

static inline uint32_t kawase_tap5_scalar(uint32_t cc, uint32_t ul, uint32_t ur, uint32_t dl,
                                          uint32_t dr)
{
    uint32_t rb = (cc & 0x00FF00FFu) * 4u + (ul & 0x00FF00FFu) + (ur & 0x00FF00FFu) +
                  (dl & 0x00FF00FFu) + (dr & 0x00FF00FFu) + 0x00040004u;
    uint32_t ag = ((cc >> 8) & 0x00FF00FFu) * 4u + ((ul >> 8) & 0x00FF00FFu) +
                  ((ur >> 8) & 0x00FF00FFu) + ((dl >> 8) & 0x00FF00FFu) +
                  ((dr >> 8) & 0x00FF00FFu) + 0x00040004u;
    return ((rb >> 3) & 0x00FF00FFu) | (((ag >> 3) & 0x00FF00FFu) << 8);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(WWAL_DISABLE_SIMD)
__attribute__((target("avx2"))) static void
kawase_pass_row_avx2(uint32_t *restrict dst_row, const uint32_t *restrict row_up,
                     const uint32_t *restrict row_cc, const uint32_t *restrict row_dn, uint32_t hw)
{
    if (hw == 0u) {
        return;
    }

    uint32_t x1 = (hw > 1u) ? 1u : 0u;
    dst_row[0] = kawase_tap5_scalar(row_cc[0], row_up[0], row_up[x1], row_dn[0], row_dn[x1]);

    const __m256i mask_lo = _mm256_set1_epi16(0x00FF);
    const __m256i bias = _mm256_set1_epi16(4);

    uint32_t x = 1u;
    for (; x + 8u < hw; x += 8u) {
        __m256i v_ul = _mm256_loadu_si256((const __m256i_u *)(row_up + x - 1u));
        __m256i v_ur = _mm256_loadu_si256((const __m256i_u *)(row_up + x + 1u));
        __m256i v_dl = _mm256_loadu_si256((const __m256i_u *)(row_dn + x - 1u));
        __m256i v_dr = _mm256_loadu_si256((const __m256i_u *)(row_dn + x + 1u));
        __m256i v_cc = _mm256_loadu_si256((const __m256i_u *)(row_cc + x));

        __m256i lo_cc = _mm256_slli_epi16(_mm256_and_si256(v_cc, mask_lo), 2);
        __m256i lo_sum = _mm256_add_epi16(
            lo_cc, _mm256_add_epi16(_mm256_add_epi16(_mm256_and_si256(v_ul, mask_lo),
                                                     _mm256_and_si256(v_ur, mask_lo)),
                                    _mm256_add_epi16(_mm256_and_si256(v_dl, mask_lo),
                                                     _mm256_and_si256(v_dr, mask_lo))));
        __m256i lo_out = _mm256_srli_epi16(_mm256_add_epi16(lo_sum, bias), 3);

        __m256i hi_cc = _mm256_slli_epi16(_mm256_srli_epi16(v_cc, 8), 2);
        __m256i hi_sum = _mm256_add_epi16(
            hi_cc, _mm256_add_epi16(
                       _mm256_add_epi16(_mm256_srli_epi16(v_ul, 8), _mm256_srli_epi16(v_ur, 8)),
                       _mm256_add_epi16(_mm256_srli_epi16(v_dl, 8), _mm256_srli_epi16(v_dr, 8))));
        __m256i hi_out = _mm256_slli_epi16(_mm256_srli_epi16(_mm256_add_epi16(hi_sum, bias), 3), 8);

        _mm256_storeu_si256((__m256i_u *)(dst_row + x), _mm256_or_si256(lo_out, hi_out));
    }

    for (; x < hw; ++x) {
        uint32_t xm = (x > 0u) ? (x - 1u) : 0u;
        uint32_t xp = (x + 1u < hw) ? (x + 1u) : (hw - 1u);
        dst_row[x] = kawase_tap5_scalar(row_cc[x], row_up[xm], row_up[xp], row_dn[xm], row_dn[xp]);
    }
}
#endif

static void kawase_pass_row_scalar(uint32_t *restrict dst_row, const uint32_t *restrict row_up,
                                   const uint32_t *restrict row_cc, const uint32_t *restrict row_dn,
                                   uint32_t hw)
{
    for (uint32_t x = 0u; x < hw; ++x) {
        uint32_t xm = (x > 0u) ? (x - 1u) : 0u;
        uint32_t xp = (x + 1u < hw) ? (x + 1u) : (hw - 1u);
        dst_row[x] = kawase_tap5_scalar(row_cc[x], row_up[xm], row_up[xp], row_dn[xm], row_dn[xp]);
    }
}

void layer_shell_blur_rgba_cpu(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    if (!src || !dst || w == 0u || h == 0u || (size_t)w > SIZE_MAX / 4u / (size_t)h) {
        return;
    }

    size_t total_bytes = (size_t)w * (size_t)h * 4u;
    uint32_t ds = (w >= 256u && h >= 256u) ? 4u : ((w >= 16u && h >= 16u) ? 2u : 1u);
    uint32_t hw = (w + ds - 1u) / ds;
    uint32_t hh = (h + ds - 1u) / ds;

    if ((size_t)hw > SIZE_MAX / sizeof(uint32_t) / 2u / (size_t)hh) {
        if (src != dst) {
            memcpy(dst, src, total_bytes);
        }
        return;
    }
    size_t pyr_bytes = (size_t)hw * (size_t)hh * sizeof(uint32_t);
    uint32_t *buf_a = (uint32_t *)malloc(pyr_bytes * 2u);
    if (!buf_a) {
        if (src != dst) {
            memcpy(dst, src, total_bytes);
        }
        return;
    }
    uint32_t *buf_b = buf_a + (size_t)hw * (size_t)hh;
    size_t src_row_bytes = (size_t)w * 4u;
    bool has_avx2 = cpu_supports_avx2();

    /* Stage 1: Exact SWAR box downsample (src -> buf_a) without unaligned u32 casts. */
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (uint32_t hy = 0u; hy < hh; ++hy) {
        uint32_t *out_row = buf_a + (size_t)hy * hw;
        uint32_t y0 = hy * ds;
        uint32_t y1 = (y0 + 1u < h) ? (y0 + 1u) : (h - 1u);
        const uint8_t *r0 = src + (size_t)y0 * src_row_bytes;
        const uint8_t *r1 = src + (size_t)y1 * src_row_bytes;

        if (ds == 4u) {
            uint32_t y2 = (y0 + 2u < h) ? (y0 + 2u) : (h - 1u);
            uint32_t y3 = (y0 + 3u < h) ? (y0 + 3u) : (h - 1u);
            const uint8_t *r2 = src + (size_t)y2 * src_row_bytes;
            const uint8_t *r3 = src + (size_t)y3 * src_row_bytes;
            for (uint32_t hx = 0u; hx < hw; ++hx) {
                uint32_t x0 = hx * 4u;
                uint32_t x1 = (x0 + 1u < w) ? (x0 + 1u) : (w - 1u);
                uint32_t x2 = (x0 + 2u < w) ? (x0 + 2u) : (w - 1u);
                uint32_t x3 = (x0 + 3u < w) ? (x0 + 3u) : (w - 1u);
                const uint8_t *rows[4] = {r0, r1, r2, r3};
                uint32_t xs[4] = {x0, x1, x2, x3};
                uint32_t rb_sum = 0x00080008u;
                uint32_t ag_sum = 0x00080008u;
                for (int ry = 0; ry < 4; ++ry) {
                    for (int rx = 0; rx < 4; ++rx) {
                        uint32_t px = load_u32_unaligned(rows[ry] + (size_t)xs[rx] * 4u);
                        rb_sum += px & 0x00FF00FFu;
                        ag_sum += (px >> 8) & 0x00FF00FFu;
                    }
                }
                out_row[hx] = ((rb_sum >> 4) & 0x00FF00FFu) | (((ag_sum >> 4) & 0x00FF00FFu) << 8);
            }
        } else if (ds == 2u) {
            for (uint32_t hx = 0u; hx < hw; ++hx) {
                uint32_t x0 = hx * 2u;
                uint32_t x1 = (x0 + 1u < w) ? (x0 + 1u) : (w - 1u);
                uint32_t p00 = load_u32_unaligned(r0 + (size_t)x0 * 4u);
                uint32_t p01 = load_u32_unaligned(r0 + (size_t)x1 * 4u);
                uint32_t p10 = load_u32_unaligned(r1 + (size_t)x0 * 4u);
                uint32_t p11 = load_u32_unaligned(r1 + (size_t)x1 * 4u);
                out_row[hx] = avg4_u32_swar(p00, p01, p10, p11);
            }
        } else {
            memcpy(out_row, r0, src_row_bytes);
        }
    }

    /* Stage 2: 2-pass Dual-Filter Kawase pyramid on hw x hh (buf_a -> buf_b -> buf_a). */
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t *in_pyr = (pass == 0) ? buf_a : buf_b;
        uint32_t *out_pyr = (pass == 0) ? buf_b : buf_a;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (uint32_t hy = 0u; hy < hh; ++hy) {
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
            (void)has_avx2;
            kawase_pass_row_scalar(dst_row, row_up, row_cc, row_dn, hw);
        }
    }

    /* Stage 3: Bilinear / Kawase upsample from buf_a (hw x hh) to dst (w x h). */
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (uint32_t y = 0u; y < h; ++y) {
        uint8_t *dst_row = dst + (size_t)y * src_row_bytes;
        if (ds == 1u) {
            memcpy(dst_row, buf_a + (size_t)y * hw, src_row_bytes);
            continue;
        }
        uint32_t hy0 = y / ds;
        if (hy0 >= hh) {
            hy0 = hh - 1u;
        }
        uint32_t hy1 = (hy0 + 1u < hh) ? (hy0 + 1u) : (hh - 1u);
        uint32_t ry = y % ds;
        const uint32_t *r0 = buf_a + (size_t)hy0 * hw;
        const uint32_t *r1 = buf_a + (size_t)hy1 * hw;

        for (uint32_t x = 0u; x < w; ++x) {
            uint32_t hx0 = x / ds;
            if (hx0 >= hw) {
                hx0 = hw - 1u;
            }
            uint32_t hx1 = (hx0 + 1u < hw) ? (hx0 + 1u) : (hw - 1u);
            uint32_t rx = x % ds;

            uint32_t px;
            if (rx != 0u && ry != 0u) {
                px = avg4_u32_swar(r0[hx0], r0[hx1], r1[hx0], r1[hx1]);
            } else if (rx != 0u) {
                px = avg_u32_epu8_scalar(r0[hx0], r0[hx1]);
            } else if (ry != 0u) {
                px = avg_u32_epu8_scalar(r0[hx0], r1[hx0]);
            } else {
                px = r0[hx0];
            }
            store_u32_unaligned(dst_row + (size_t)x * 4u, px);
        }
    }

    free(buf_a);
}
