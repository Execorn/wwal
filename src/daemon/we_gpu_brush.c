// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/we_gpu_brush.h"

#include "waywal/log.h"
#include "waywal/we_scene.h"
#include "waywal/we_tex.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

/* ========================================================================= */
/* Embedded GLSL ES 3.10 Brush Shaders (#version 310 es strictly on line 1)  */
/* ========================================================================= */

static const char *k_brush_quad_vert =
    "#version 310 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec2 a_Pos;\n"
    "uniform vec4 u_DstRectNorm;\n"
    "uniform vec4 u_SrcUvRect;\n"
    "uniform vec2 u_DstSizePx;\n"
    "out vec2 v_Uv;\n"
    "out vec2 v_LocalPx;\n"
    "void main() {\n"
    "    vec2 t = a_Pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(mix(u_DstRectNorm.xy, u_DstRectNorm.zw, t), 0.0, 1.0);\n"
    "    v_Uv = mix(u_SrcUvRect.xy, u_SrcUvRect.zw, t);\n"
    "    v_LocalPx = t * u_DstSizePx;\n"
    "}\n";

static const char *k_brush_copy_in_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "uniform sampler2D u_CanvasTex;\n"
    "uniform int u_SrcSwizzleMode;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 s = texture(u_CanvasTex, v_Uv);\n"
    "    if (u_SrcSwizzleMode == 1)      fragColor = vec4(s.r, s.a, 0.0, 1.0);\n"
    "    else if (u_SrcSwizzleMode == 2) fragColor = vec4(s.r, 0.0, 0.0, 1.0);\n"
    "    else                            fragColor = s;\n"
    "}\n";

static const char *k_brush_r8_mask_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "in vec2 v_LocalPx;\n"
    "uniform sampler2D u_ScratchSrc;\n"
    "uniform vec2 u_PointA;\n"
    "uniform vec2 u_PointB;\n"
    "uniform float u_RadiusA;\n"
    "uniform float u_RadiusB;\n"
    "uniform float u_Hardness;\n"
    "uniform float u_FalloffPower;\n"
    "uniform float u_Opacity;\n"
    "uniform float u_TargetValue;\n"
    "uniform int u_Mode;\n"
    "uniform int u_IsColorRgba;\n"
    "uniform vec4 u_ColorRgba;\n"
    "out vec4 fragColor;\n"
    "float sdUnevenCapsule(vec2 p, vec2 pa, vec2 pb, float ra, float rb) {\n"
    "    p -= pa;\n"
    "    pb -= pa;\n"
    "    float h = dot(pb, pb);\n"
    "    if (h < 1e-6) return length(p) - max(ra, rb);\n"
    "    vec2 q = vec2(dot(p, vec2(pb.y, -pb.x)), dot(p, pb)) / h;\n"
    "    q.x = abs(q.x);\n"
    "    float b = ra - rb;\n"
    "    vec2 c = vec2(sqrt(max(h - b * b, 0.0)), b);\n"
    "    float k = c.x * q.y - c.y * q.x;\n"
    "    float m = dot(c, q);\n"
    "    float n = dot(q, q);\n"
    "    if (k < 0.0) return sqrt(h * n) - ra;\n"
    "    if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;\n"
    "    return m - ra;\n"
    "}\n"
    "void main() {\n"
    "    vec2 ba = u_PointB - u_PointA;\n"
    "    float denom = dot(ba, ba);\n"
    "    float t = (denom > 1e-6) ? clamp(dot(v_LocalPx - u_PointA, ba) / denom, 0.0, 1.0) : 0.0;\n"
    "    float rLoc = max(mix(u_RadiusA, u_RadiusB, t), 0.5);\n"
    "    float dist = sdUnevenCapsule(v_LocalPx, u_PointA, u_PointB, u_RadiusA, u_RadiusB) + "
    "rLoc;\n"
    "    float inner = clamp(u_Hardness, 0.0, 0.999);\n"
    "    float edgeT = clamp((dist / rLoc - inner) / max(1.0 - inner, 1.0 / rLoc), 0.0, 1.0);\n"
    "    float shaped = (edgeT <= 0.0) ? 0.0 : pow(edgeT, max(u_FalloffPower, 0.05));\n"
    "    float falloff = 1.0 - smoothstep(0.0, 1.0, shaped);\n"
    "    float aa = 1.0 - smoothstep(rLoc - 0.75, rLoc + 0.75, dist);\n"
    "    float w = clamp(falloff * aa, 0.0, 1.0) * u_Opacity;\n"
    "    if (u_IsColorRgba != 0) {\n"
    "        vec4 dst = texture(u_ScratchSrc, v_Uv);\n"
    "        float effW = clamp(w * u_ColorRgba.a, 0.0, 1.0);\n"
    "        if (u_Mode == 0) {\n"
    "            float outA = clamp(dst.a + effW * (1.0 - dst.a), 0.0, 1.0);\n"
    "            vec3 outRgb = (outA > 1e-6)\n"
    "                        ? (dst.rgb * dst.a * (1.0 - effW) + u_ColorRgba.rgb * effW) / outA\n"
    "                        : vec3(0.0);\n"
    "            fragColor = vec4(clamp(outRgb, 0.0, 1.0), outA);\n"
    "        } else if (u_Mode == 1) {\n"
    "            fragColor = max(dst, vec4(u_ColorRgba.rgb, 1.0) * effW);\n"
    "        } else if (u_Mode == 2) {\n"
    "            fragColor = clamp(dst - u_ColorRgba * w, 0.0, 1.0);\n"
    "        } else {\n"
    "            fragColor = vec4(dst.rgb, clamp(dst.a * (1.0 - w), 0.0, 1.0));\n"
    "        }\n"
    "        return;\n"
    "    }\n"
    "    float dst = texture(u_ScratchSrc, v_Uv).r;\n"
    "    float res = dst;\n"
    "    if (u_Mode == 0)      res = clamp(dst + (u_TargetValue - dst) * w, 0.0, 1.0);\n"
    "    else if (u_Mode == 1) res = max(dst, u_TargetValue * w);\n"
    "    else if (u_Mode == 2) res = clamp(dst - w, 0.0, 1.0);\n"
    "    else                  res = clamp(dst * (1.0 - w), 0.0, 1.0);\n"
    "    fragColor = vec4(res, res, res, res);\n"
    "}\n";

static const char *k_brush_rg88_flow_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "in vec2 v_LocalPx;\n"
    "uniform sampler2D u_ScratchSrc;\n"
    "uniform vec2 u_PointA;\n"
    "uniform vec2 u_PointB;\n"
    "uniform vec2 u_FlowDir;\n"
    "uniform float u_RadiusA;\n"
    "uniform float u_RadiusB;\n"
    "uniform float u_Hardness;\n"
    "uniform float u_FalloffPower;\n"
    "uniform float u_Opacity;\n"
    "uniform int u_Mode;\n"
    "out vec4 fragColor;\n"
    "const vec2 kFlowNeutral = vec2(128.0 / 255.0);\n"
    "const float kFlowScale  = 127.0 / 255.0;\n"
    "void main() {\n"
    "    vec2 dstVec = (texture(u_ScratchSrc, v_Uv).rg - kFlowNeutral) / kFlowScale;\n"
    "    vec2 pa = v_LocalPx - u_PointA;\n"
    "    vec2 ba = u_PointB - u_PointA;\n"
    "    float denom = dot(ba, ba);\n"
    "    float segT = (denom > 1e-6) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;\n"
    "    float rLoc = max(mix(u_RadiusA, u_RadiusB, segT), 0.5);\n"
    "    float dist = length(pa - ba * segT);\n"
    "    float inner = clamp(u_Hardness, 0.0, 0.999);\n"
    "    float edgeT = clamp((dist / rLoc - inner) / max(1.0 - inner, 1.0 / rLoc), 0.0, 1.0);\n"
    "    float shaped = (edgeT <= 0.0) ? 0.0 : pow(edgeT, max(u_FalloffPower, 0.05));\n"
    "    float w = (1.0 - smoothstep(0.0, 1.0, shaped))\n"
    "            * (1.0 - smoothstep(rLoc - 0.75, rLoc + 0.75, dist)) * u_Opacity;\n"
    "    vec2 outVec = dstVec;\n"
    "    if (u_Mode == 0)      outVec = mix(dstVec, u_FlowDir, w);\n"
    "    else if (u_Mode == 1) outVec = mix(dstVec, vec2(0.0), w);\n"
    "    else {\n"
    "        outVec = dstVec + u_FlowDir * w;\n"
    "        float mag = length(outVec);\n"
    "        if (mag > 1.0) outVec /= mag;\n"
    "    }\n"
    "    fragColor = vec4(clamp(kFlowNeutral + outVec * kFlowScale, 0.0, 1.0), 0.0, 1.0);\n"
    "}\n";

static const char *k_brush_kawase_down_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "uniform sampler2D u_SrcTex;\n"
    "uniform vec2 u_HalfPixel;\n"
    "uniform float u_Offset;\n"
    "uniform vec4 u_UvClamp;\n"
    "uniform int u_PassKind;\n"
    "out vec4 fragColor;\n"
    "vec2 clampUv(vec2 uv) {\n"
    "    return clamp(uv, u_UvClamp.xy, u_UvClamp.zw);\n"
    "}\n"
    "void main() {\n"
    "    if (u_PassKind == 0) {\n"
    "        vec2 d = u_HalfPixel * u_Offset;\n"
    "        vec4 sum = texture(u_SrcTex, clampUv(v_Uv)) * 4.0;\n"
    "        sum += texture(u_SrcTex, clampUv(v_Uv - d)) + texture(u_SrcTex, clampUv(v_Uv + d));\n"
    "        sum += texture(u_SrcTex, clampUv(v_Uv + vec2(d.x, -d.y)))\n"
    "             + texture(u_SrcTex, clampUv(v_Uv + vec2(-d.x, d.y)));\n"
    "        fragColor = sum * 0.125;\n"
    "        return;\n"
    "    }\n"
    "    vec2 axis = (u_PassKind == 1) ? vec2(u_HalfPixel.x * 2.0, 0.0)\n"
    "                                  : vec2(0.0, u_HalfPixel.y * 2.0);\n"
    "    vec2 o1 = axis * (1.384615 * u_Offset);\n"
    "    vec2 o2 = axis * (3.230769 * u_Offset);\n"
    "    vec4 c = texture(u_SrcTex, clampUv(v_Uv)) * 0.227027;\n"
    "    c += (texture(u_SrcTex, clampUv(v_Uv + o1))\n"
    "        + texture(u_SrcTex, clampUv(v_Uv - o1))) * 0.316216;\n"
    "    c += (texture(u_SrcTex, clampUv(v_Uv + o2))\n"
    "        + texture(u_SrcTex, clampUv(v_Uv - o2))) * 0.070270;\n"
    "    fragColor = c;\n"
    "}\n";

static const char *k_brush_kawase_up_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "uniform sampler2D u_SrcTex;\n"
    "uniform vec2 u_HalfPixel;\n"
    "uniform float u_Offset;\n"
    "uniform vec4 u_UvClamp;\n"
    "out vec4 fragColor;\n"
    "vec2 clampUv(vec2 uv) {\n"
    "    return clamp(uv, u_UvClamp.xy, u_UvClamp.zw);\n"
    "}\n"
    "void main() {\n"
    "    vec2 d = u_HalfPixel * u_Offset;\n"
    "    vec4 sum  = texture(u_SrcTex, clampUv(v_Uv + vec2(-d.x * 2.0, 0.0)));\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(-d.x, d.y))) * 2.0;\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(0.0, d.y * 2.0)));\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(d.x, d.y))) * 2.0;\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(d.x * 2.0, 0.0)));\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(d.x, -d.y))) * 2.0;\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(0.0, -d.y * 2.0)));\n"
    "    sum += texture(u_SrcTex, clampUv(v_Uv + vec2(-d.x, -d.y))) * 2.0;\n"
    "    fragColor = sum * (1.0 / 12.0);\n"
    "}\n";

static const char *k_brush_smudge_blur_composite_frag =
    "#version 310 es\n"
    "precision highp float;\n"
    "in vec2 v_Uv;\n"
    "in vec2 v_LocalPx;\n"
    "uniform sampler2D u_OrigTex;\n"
    "uniform sampler2D u_BlurredTex;\n"
    "uniform vec2 u_TexelSize;\n"
    "uniform vec2 u_PointA;\n"
    "uniform vec2 u_PointB;\n"
    "uniform vec2 u_DragPx;\n"
    "uniform float u_RadiusA;\n"
    "uniform float u_RadiusB;\n"
    "uniform float u_Hardness;\n"
    "uniform float u_FalloffPower;\n"
    "uniform float u_Opacity;\n"
    "uniform vec4 u_UvClamp;\n"
    "uniform vec4 u_BlurUvClamp;\n"
    "uniform vec2 u_BlurUvScale;\n"
    "uniform int u_OpMode;\n"
    "out vec4 fragColor;\n"
    "vec2 clampUv(vec2 uv) {\n"
    "    return clamp(uv, u_UvClamp.xy, u_UvClamp.zw);\n"
    "}\n"
    "vec2 clampBlurUv(vec2 uv) {\n"
    "    return clamp(uv, u_BlurUvClamp.xy, u_BlurUvClamp.zw);\n"
    "}\n"
    "void main() {\n"
    "    vec4 orig = texture(u_OrigTex, clampUv(v_Uv));\n"
    "    vec2 pa = v_LocalPx - u_PointA;\n"
    "    vec2 ba = u_PointB - u_PointA;\n"
    "    float denom = dot(ba, ba);\n"
    "    float segT = (denom > 1e-6) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;\n"
    "    float rLoc = max(mix(u_RadiusA, u_RadiusB, segT), 0.5);\n"
    "    float dist = length(pa - ba * segT);\n"
    "    float inner = clamp(u_Hardness, 0.0, 0.999);\n"
    "    float edgeT = clamp((dist / rLoc - inner) / max(1.0 - inner, 1.0 / rLoc), 0.0, 1.0);\n"
    "    float shaped = (edgeT <= 0.0) ? 0.0 : pow(edgeT, max(u_FalloffPower, 0.05));\n"
    "    float w = (1.0 - smoothstep(0.0, 1.0, shaped))\n"
    "            * (1.0 - smoothstep(rLoc - 0.75, rLoc + 0.75, dist)) * u_Opacity;\n"
    "    if (u_OpMode == 0) {\n"
    "        fragColor = mix(orig, texture(u_BlurredTex, clampBlurUv(v_Uv * u_BlurUvScale)), w);\n"
    "    } else {\n"
    "        vec2 pull = u_DragPx * u_TexelSize;\n"
    "        vec4 adv = 0.40 * texture(u_OrigTex, clampUv(v_Uv - pull))\n"
    "                 + 0.30 * texture(u_OrigTex, clampUv(v_Uv - pull * 0.66))\n"
    "                 + 0.20 * texture(u_OrigTex, clampUv(v_Uv - pull * 0.33))\n"
    "                 + 0.10 * texture(u_BlurredTex, clampBlurUv((v_Uv - pull * 0.50) * "
    "u_BlurUvScale));\n"
    "        fragColor = mix(orig, adv, w);\n"
    "    }\n"
    "}\n";

/* ========================================================================= */
/* Internal Context, GL State Guard & CoW Tile History                       */
/* ========================================================================= */

typedef struct {
    uint16_t tx;
    uint16_t ty;
    uint32_t pre_slot;
    uint32_t post_slot; /* UINT32_MAX until first undo captures post-stroke pixels */
} brush_tile_record_t;

#define WE_BRUSH_MAX_CANVAS_DIM 16384u

typedef struct {
    uint64_t stroke_id;
    const struct we_scene *scene;
    GLuint target_gl_tex;
    uint32_t canvas_w;
    uint32_t canvas_h;
    bool is_undone;
    brush_tile_record_t *tiles;
    uint32_t tile_count;
    uint32_t tile_cap;
} brush_stroke_record_t;

typedef struct {
    GLint prev_prog;
    GLint prev_vao;
    GLint prev_vbo;
    GLint prev_read_fbo;
    GLint prev_draw_fbo;
    GLint prev_active_tex;
    GLint prev_tex0;
    GLint prev_tex1;
    GLint prev_tex_array;
    GLint prev_unpack_align;
    GLint prev_viewport[4];
    GLint prev_scissor_box[4];
    GLboolean prev_scissor_test;
    GLboolean prev_blend;
    GLboolean prev_depth_test;
    GLboolean prev_cull_face;
    GLboolean prev_stencil_test;
    GLboolean prev_dither;
    GLboolean prev_color_mask[4];
} brush_gl_state_t;

struct we_gpu_brush_ctx {
    /* Shared quad geometry */
    GLuint vao;
    GLuint vbo;

    /* 6 linked programs built from the 7 GLSL ES 3.10 stages */
    GLuint prog_copy_in;
    GLint loc_copy_dst_rect;
    GLint loc_copy_src_uv;
    GLint loc_copy_dst_size;
    GLint loc_copy_tex;
    GLint loc_copy_swizzle;

    GLuint prog_r8_mask;
    GLint loc_mask_dst_rect;
    GLint loc_mask_src_uv;
    GLint loc_mask_dst_size;
    GLint loc_mask_src;
    GLint loc_mask_pt_a;
    GLint loc_mask_pt_b;
    GLint loc_mask_rad_a;
    GLint loc_mask_rad_b;
    GLint loc_mask_hard;
    GLint loc_mask_falloff;
    GLint loc_mask_opacity;
    GLint loc_mask_target;
    GLint loc_mask_mode;
    GLint loc_mask_is_rgba;
    GLint loc_mask_color;

    GLuint prog_rg88_flow;
    GLint loc_flow_dst_rect;
    GLint loc_flow_src_uv;
    GLint loc_flow_dst_size;
    GLint loc_flow_src;
    GLint loc_flow_pt_a;
    GLint loc_flow_pt_b;
    GLint loc_flow_dir;
    GLint loc_flow_rad_a;
    GLint loc_flow_rad_b;
    GLint loc_flow_hard;
    GLint loc_flow_falloff;
    GLint loc_flow_opacity;
    GLint loc_flow_mode;

    GLuint prog_kawase_down;
    GLint loc_kdown_dst_rect;
    GLint loc_kdown_src_uv;
    GLint loc_kdown_dst_size;
    GLint loc_kdown_src;
    GLint loc_kdown_half_px;
    GLint loc_kdown_offset;
    GLint loc_kdown_uv_clamp;
    GLint loc_kdown_pass_kind;

    GLuint prog_kawase_up;
    GLint loc_kup_dst_rect;
    GLint loc_kup_src_uv;
    GLint loc_kup_dst_size;
    GLint loc_kup_src;
    GLint loc_kup_half_px;
    GLint loc_kup_offset;
    GLint loc_kup_uv_clamp;

    GLuint prog_smudge_blur;
    GLint loc_sb_dst_rect;
    GLint loc_sb_src_uv;
    GLint loc_sb_dst_size;
    GLint loc_sb_orig;
    GLint loc_sb_blurred;
    GLint loc_sb_texel;
    GLint loc_sb_pt_a;
    GLint loc_sb_pt_b;
    GLint loc_sb_drag;
    GLint loc_sb_rad_a;
    GLint loc_sb_rad_b;
    GLint loc_sb_hard;
    GLint loc_sb_falloff;
    GLint loc_sb_opacity;
    GLint loc_sb_uv_clamp;
    GLint loc_sb_blur_uv_clamp;
    GLint loc_sb_blur_uv_scale;
    GLint loc_sb_op_mode;

    /* Scratch ping-pong & Dual-Kawase pyramid FBOs (allocated lazily on first stroke) */
    bool scratch_allocated;
    GLuint scratch_tex[2]; /* S_ping, S_pong (1024x1024 GL_RGBA8) */
    GLuint scratch_fbo[2];
    GLuint kawase_tex[WE_BRUSH_KAWASE_LEVELS]; /* 512, 256, 128, 64 */
    GLuint kawase_fbo[WE_BRUSH_KAWASE_LEVELS];
    GLuint canvas_fbo;
    GLuint pool_fbo;

    /* 2-PBO fence-synced ring for non-stalling readback */
    GLuint readback_pbo[2];
    size_t readback_pbo_bytes[2];
    uint32_t readback_pbo_idx;
    EGLDisplay egl_dpy;
    PFNEGLCREATESYNCKHRPROC fn_eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC fn_eglDestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC fn_eglClientWaitSyncKHR;

    /* 64x64 CoW GL_TEXTURE_2D_ARRAY undo/redo pool */
    GLuint pool_tex;
    uint32_t pool_cap;
    uint32_t *free_slots;
    uint32_t free_count;
    uint32_t next_fresh_slot;

    /* Committed stroke history */
    brush_stroke_record_t *strokes;
    uint32_t stroke_count;
    uint32_t stroke_cap;

    /* Active target binding */
    struct we_scene *bound_scene;
    struct we_tex *bound_tex;
    uint32_t canvas_w;
    uint32_t canvas_h;
    GLint saved_max_level;
    GLint saved_min_filter;
    GLint saved_mag_filter;
    GLint saved_wrap_s;
    GLint saved_wrap_t;
    bool has_mipmaps;

    /* Active stroke transaction */
    bool in_stroke;
    uint64_t active_stroke_id;
    we_brush_params_t active_params;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint64_t *dirty_bitset;
    size_t dirty_bitset_words;
    brush_tile_record_t *active_tiles;
    uint32_t active_tile_count;
    uint32_t active_tile_cap;

    /* Telemetry */
    uint32_t last_dab_gpu_us;
};

static uint64_t brush_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void save_gl_state(brush_gl_state_t *st)
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &st->prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &st->prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->prev_vbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &st->prev_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &st->prev_draw_fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &st->prev_active_tex);

    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->prev_tex1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->prev_tex0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &st->prev_tex_array);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &st->prev_unpack_align);

    glGetIntegerv(GL_VIEWPORT, st->prev_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, st->prev_scissor_box);
    st->prev_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    st->prev_blend = glIsEnabled(GL_BLEND);
    st->prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
    st->prev_cull_face = glIsEnabled(GL_CULL_FACE);
    st->prev_stencil_test = glIsEnabled(GL_STENCIL_TEST);
    st->prev_dither = glIsEnabled(GL_DITHER);
    glGetBooleanv(GL_COLOR_WRITEMASK, st->prev_color_mask);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void restore_gl_state(const brush_gl_state_t *st)
{
    glColorMask(st->prev_color_mask[0], st->prev_color_mask[1], st->prev_color_mask[2],
                st->prev_color_mask[3]);
    if (st->prev_dither)
        glEnable(GL_DITHER);
    else
        glDisable(GL_DITHER);
    if (st->prev_stencil_test)
        glEnable(GL_STENCIL_TEST);
    else
        glDisable(GL_STENCIL_TEST);
    if (st->prev_cull_face)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (st->prev_depth_test)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (st->prev_blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (st->prev_scissor_test)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    glScissor(st->prev_scissor_box[0], st->prev_scissor_box[1], st->prev_scissor_box[2],
              st->prev_scissor_box[3]);
    glViewport(st->prev_viewport[0], st->prev_viewport[1], st->prev_viewport[2],
               st->prev_viewport[3]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)st->prev_tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)st->prev_tex0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)st->prev_tex_array);
    glActiveTexture((GLenum)st->prev_active_tex);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)st->prev_read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)st->prev_draw_fbo);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)st->prev_vbo);
    glBindVertexArray((GLuint)st->prev_vao);
    glPixelStorei(GL_UNPACK_ALIGNMENT, st->prev_unpack_align);
    glUseProgram((GLuint)st->prev_prog);
}

static GLuint compile_stage(GLenum stage, const char *src, const char *label)
{
    GLuint sh = glCreateShader(stage);
    if (!sh) {
        return 0;
    }
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log_buf[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("we_gpu_brush: failed to compile %s: %s", label, log_buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_brush_program(GLuint vs, const char *frag_src, const char *label)
{
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, frag_src, label);
    if (!fs) {
        return 0;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_Pos");
    glLinkProgram(prog);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log_buf[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
        WAYWAL_LOG_ERR("we_gpu_brush: failed to link %s: %s", label, log_buf);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool init_brush_programs(we_gpu_brush_ctx_t *ctx)
{
    GLuint vs = compile_stage(GL_VERTEX_SHADER, k_brush_quad_vert, "brush_quad.vert");
    if (!vs) {
        return false;
    }

    ctx->prog_copy_in = link_brush_program(vs, k_brush_copy_in_frag, "brush_copy_in.frag");
    ctx->prog_r8_mask = link_brush_program(vs, k_brush_r8_mask_frag, "brush_r8_mask.frag");
    ctx->prog_rg88_flow = link_brush_program(vs, k_brush_rg88_flow_frag, "brush_rg88_flow.frag");
    ctx->prog_kawase_down =
        link_brush_program(vs, k_brush_kawase_down_frag, "brush_kawase_down.frag");
    ctx->prog_kawase_up = link_brush_program(vs, k_brush_kawase_up_frag, "brush_kawase_up.frag");
    ctx->prog_smudge_blur = link_brush_program(vs, k_brush_smudge_blur_composite_frag,
                                               "brush_smudge_blur_composite.frag");
    glDeleteShader(vs);

    if (!ctx->prog_copy_in || !ctx->prog_r8_mask || !ctx->prog_rg88_flow ||
        !ctx->prog_kawase_down || !ctx->prog_kawase_up || !ctx->prog_smudge_blur) {
        return false;
    }

    ctx->loc_copy_dst_rect = glGetUniformLocation(ctx->prog_copy_in, "u_DstRectNorm");
    ctx->loc_copy_src_uv = glGetUniformLocation(ctx->prog_copy_in, "u_SrcUvRect");
    ctx->loc_copy_dst_size = glGetUniformLocation(ctx->prog_copy_in, "u_DstSizePx");
    ctx->loc_copy_tex = glGetUniformLocation(ctx->prog_copy_in, "u_CanvasTex");
    ctx->loc_copy_swizzle = glGetUniformLocation(ctx->prog_copy_in, "u_SrcSwizzleMode");

    ctx->loc_mask_dst_rect = glGetUniformLocation(ctx->prog_r8_mask, "u_DstRectNorm");
    ctx->loc_mask_src_uv = glGetUniformLocation(ctx->prog_r8_mask, "u_SrcUvRect");
    ctx->loc_mask_dst_size = glGetUniformLocation(ctx->prog_r8_mask, "u_DstSizePx");
    ctx->loc_mask_src = glGetUniformLocation(ctx->prog_r8_mask, "u_ScratchSrc");
    ctx->loc_mask_pt_a = glGetUniformLocation(ctx->prog_r8_mask, "u_PointA");
    ctx->loc_mask_pt_b = glGetUniformLocation(ctx->prog_r8_mask, "u_PointB");
    ctx->loc_mask_rad_a = glGetUniformLocation(ctx->prog_r8_mask, "u_RadiusA");
    ctx->loc_mask_rad_b = glGetUniformLocation(ctx->prog_r8_mask, "u_RadiusB");
    ctx->loc_mask_hard = glGetUniformLocation(ctx->prog_r8_mask, "u_Hardness");
    ctx->loc_mask_falloff = glGetUniformLocation(ctx->prog_r8_mask, "u_FalloffPower");
    ctx->loc_mask_opacity = glGetUniformLocation(ctx->prog_r8_mask, "u_Opacity");
    ctx->loc_mask_target = glGetUniformLocation(ctx->prog_r8_mask, "u_TargetValue");
    ctx->loc_mask_mode = glGetUniformLocation(ctx->prog_r8_mask, "u_Mode");
    ctx->loc_mask_is_rgba = glGetUniformLocation(ctx->prog_r8_mask, "u_IsColorRgba");
    ctx->loc_mask_color = glGetUniformLocation(ctx->prog_r8_mask, "u_ColorRgba");

    ctx->loc_flow_dst_rect = glGetUniformLocation(ctx->prog_rg88_flow, "u_DstRectNorm");
    ctx->loc_flow_src_uv = glGetUniformLocation(ctx->prog_rg88_flow, "u_SrcUvRect");
    ctx->loc_flow_dst_size = glGetUniformLocation(ctx->prog_rg88_flow, "u_DstSizePx");
    ctx->loc_flow_src = glGetUniformLocation(ctx->prog_rg88_flow, "u_ScratchSrc");
    ctx->loc_flow_pt_a = glGetUniformLocation(ctx->prog_rg88_flow, "u_PointA");
    ctx->loc_flow_pt_b = glGetUniformLocation(ctx->prog_rg88_flow, "u_PointB");
    ctx->loc_flow_dir = glGetUniformLocation(ctx->prog_rg88_flow, "u_FlowDir");
    ctx->loc_flow_rad_a = glGetUniformLocation(ctx->prog_rg88_flow, "u_RadiusA");
    ctx->loc_flow_rad_b = glGetUniformLocation(ctx->prog_rg88_flow, "u_RadiusB");
    ctx->loc_flow_hard = glGetUniformLocation(ctx->prog_rg88_flow, "u_Hardness");
    ctx->loc_flow_falloff = glGetUniformLocation(ctx->prog_rg88_flow, "u_FalloffPower");
    ctx->loc_flow_opacity = glGetUniformLocation(ctx->prog_rg88_flow, "u_Opacity");
    ctx->loc_flow_mode = glGetUniformLocation(ctx->prog_rg88_flow, "u_Mode");

    ctx->loc_kdown_dst_rect = glGetUniformLocation(ctx->prog_kawase_down, "u_DstRectNorm");
    ctx->loc_kdown_src_uv = glGetUniformLocation(ctx->prog_kawase_down, "u_SrcUvRect");
    ctx->loc_kdown_dst_size = glGetUniformLocation(ctx->prog_kawase_down, "u_DstSizePx");
    ctx->loc_kdown_src = glGetUniformLocation(ctx->prog_kawase_down, "u_SrcTex");
    ctx->loc_kdown_half_px = glGetUniformLocation(ctx->prog_kawase_down, "u_HalfPixel");
    ctx->loc_kdown_offset = glGetUniformLocation(ctx->prog_kawase_down, "u_Offset");
    ctx->loc_kdown_uv_clamp = glGetUniformLocation(ctx->prog_kawase_down, "u_UvClamp");
    ctx->loc_kdown_pass_kind = glGetUniformLocation(ctx->prog_kawase_down, "u_PassKind");

    ctx->loc_kup_dst_rect = glGetUniformLocation(ctx->prog_kawase_up, "u_DstRectNorm");
    ctx->loc_kup_src_uv = glGetUniformLocation(ctx->prog_kawase_up, "u_SrcUvRect");
    ctx->loc_kup_dst_size = glGetUniformLocation(ctx->prog_kawase_up, "u_DstSizePx");
    ctx->loc_kup_src = glGetUniformLocation(ctx->prog_kawase_up, "u_SrcTex");
    ctx->loc_kup_half_px = glGetUniformLocation(ctx->prog_kawase_up, "u_HalfPixel");
    ctx->loc_kup_offset = glGetUniformLocation(ctx->prog_kawase_up, "u_Offset");
    ctx->loc_kup_uv_clamp = glGetUniformLocation(ctx->prog_kawase_up, "u_UvClamp");

    ctx->loc_sb_dst_rect = glGetUniformLocation(ctx->prog_smudge_blur, "u_DstRectNorm");
    ctx->loc_sb_src_uv = glGetUniformLocation(ctx->prog_smudge_blur, "u_SrcUvRect");
    ctx->loc_sb_dst_size = glGetUniformLocation(ctx->prog_smudge_blur, "u_DstSizePx");
    ctx->loc_sb_orig = glGetUniformLocation(ctx->prog_smudge_blur, "u_OrigTex");
    ctx->loc_sb_blurred = glGetUniformLocation(ctx->prog_smudge_blur, "u_BlurredTex");
    ctx->loc_sb_texel = glGetUniformLocation(ctx->prog_smudge_blur, "u_TexelSize");
    ctx->loc_sb_pt_a = glGetUniformLocation(ctx->prog_smudge_blur, "u_PointA");
    ctx->loc_sb_pt_b = glGetUniformLocation(ctx->prog_smudge_blur, "u_PointB");
    ctx->loc_sb_drag = glGetUniformLocation(ctx->prog_smudge_blur, "u_DragPx");
    ctx->loc_sb_rad_a = glGetUniformLocation(ctx->prog_smudge_blur, "u_RadiusA");
    ctx->loc_sb_rad_b = glGetUniformLocation(ctx->prog_smudge_blur, "u_RadiusB");
    ctx->loc_sb_hard = glGetUniformLocation(ctx->prog_smudge_blur, "u_Hardness");
    ctx->loc_sb_falloff = glGetUniformLocation(ctx->prog_smudge_blur, "u_FalloffPower");
    ctx->loc_sb_opacity = glGetUniformLocation(ctx->prog_smudge_blur, "u_Opacity");
    ctx->loc_sb_uv_clamp = glGetUniformLocation(ctx->prog_smudge_blur, "u_UvClamp");
    ctx->loc_sb_blur_uv_clamp = glGetUniformLocation(ctx->prog_smudge_blur, "u_BlurUvClamp");
    ctx->loc_sb_blur_uv_scale = glGetUniformLocation(ctx->prog_smudge_blur, "u_BlurUvScale");
    ctx->loc_sb_op_mode = glGetUniformLocation(ctx->prog_smudge_blur, "u_OpMode");

    return true;
}

static bool create_rgba8_fbo_tex(uint32_t w, uint32_t h, GLuint *out_tex, GLuint *out_fbo)
{
    while (glGetError() != GL_NO_ERROR) {
    }

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glGenFramebuffers(1, &fbo);
    if (!tex || !fbo) {
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (tex)
            glDeleteTextures(1, &tex);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    if (glGetError() != GL_NO_ERROR) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return false;
    }

    *out_tex = tex;
    *out_fbo = fbo;
    return true;
}

static void destroy_scratch_fbos(we_gpu_brush_ctx_t *ctx)
{
    for (uint32_t i = 0; i < 2u; i++) {
        if (ctx->scratch_fbo[i]) {
            glDeleteFramebuffers(1, &ctx->scratch_fbo[i]);
            ctx->scratch_fbo[i] = 0;
        }
        if (ctx->scratch_tex[i]) {
            glDeleteTextures(1, &ctx->scratch_tex[i]);
            ctx->scratch_tex[i] = 0;
        }
    }
    for (uint32_t l = 0; l < WE_BRUSH_KAWASE_LEVELS; l++) {
        if (ctx->kawase_fbo[l]) {
            glDeleteFramebuffers(1, &ctx->kawase_fbo[l]);
            ctx->kawase_fbo[l] = 0;
        }
        if (ctx->kawase_tex[l]) {
            glDeleteTextures(1, &ctx->kawase_tex[l]);
            ctx->kawase_tex[l] = 0;
        }
    }
    ctx->scratch_allocated = false;
}

static bool ensure_scratch_fbos(we_gpu_brush_ctx_t *ctx)
{
    if (ctx->scratch_allocated) {
        return true;
    }
    for (uint32_t i = 0; i < 2u; i++) {
        if (!create_rgba8_fbo_tex(WE_BRUSH_SCRATCH_MAX_DIM, WE_BRUSH_SCRATCH_MAX_DIM,
                                  &ctx->scratch_tex[i], &ctx->scratch_fbo[i])) {
            destroy_scratch_fbos(ctx);
            return false;
        }
    }
    for (uint32_t l = 0; l < WE_BRUSH_KAWASE_LEVELS; l++) {
        uint32_t dim = WE_BRUSH_SCRATCH_MAX_DIM >> (l + 1u);
        if (!create_rgba8_fbo_tex(dim, dim, &ctx->kawase_tex[l], &ctx->kawase_fbo[l])) {
            destroy_scratch_fbos(ctx);
            return false;
        }
    }
    ctx->scratch_allocated = true;
    return true;
}

static void release_stroke_record_slots(we_gpu_brush_ctx_t *ctx, brush_stroke_record_t *rec)
{
    for (uint32_t i = 0; i < rec->tile_count; i++) {
        if (rec->tiles[i].pre_slot != UINT32_MAX && ctx->free_count < ctx->pool_cap) {
            ctx->free_slots[ctx->free_count++] = rec->tiles[i].pre_slot;
        }
        if (rec->tiles[i].post_slot != UINT32_MAX && ctx->free_count < ctx->pool_cap) {
            ctx->free_slots[ctx->free_count++] = rec->tiles[i].post_slot;
        }
    }
    free(rec->tiles);
    rec->tiles = NULL;
    rec->tile_count = 0;
    rec->tile_cap = 0;
}

static uint32_t alloc_pool_slot(we_gpu_brush_ctx_t *ctx, bool has_exempt, uint64_t exempt_stroke_id)
{
    if (ctx->free_count > 0) {
        return ctx->free_slots[--ctx->free_count];
    }
    if (ctx->next_fresh_slot < ctx->pool_cap) {
        return ctx->next_fresh_slot++;
    }
    /* Evict oldest committed non-exempt strokes until a pool slot is freed. */
    while (ctx->free_count == 0) {
        uint32_t evict_idx = UINT32_MAX;
        for (uint32_t i = 0; i < ctx->stroke_count; i++) {
            if (has_exempt && ctx->strokes[i].stroke_id == exempt_stroke_id) {
                continue;
            }
            evict_idx = i;
            break;
        }
        if (evict_idx == UINT32_MAX) {
            break;
        }
        release_stroke_record_slots(ctx, &ctx->strokes[evict_idx]);
        if (evict_idx + 1u < ctx->stroke_count) {
            memmove(&ctx->strokes[evict_idx], &ctx->strokes[evict_idx + 1u],
                    (size_t)(ctx->stroke_count - evict_idx - 1u) * sizeof(brush_stroke_record_t));
        }
        ctx->stroke_count--;
    }
    if (ctx->free_count > 0) {
        return ctx->free_slots[--ctx->free_count];
    }
    return UINT32_MAX;
}

static void free_pool_slot(we_gpu_brush_ctx_t *ctx, uint32_t slot)
{
    if (slot != UINT32_MAX && ctx->free_count < ctx->pool_cap) {
        ctx->free_slots[ctx->free_count++] = slot;
    }
}

we_gpu_brush_ctx_t *we_gpu_brush_create(uint32_t pool_slot_capacity)
{
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return NULL;
    }

    if (pool_slot_capacity == 0) {
        pool_slot_capacity = WE_BRUSH_POOL_DEFAULT_CAPS;
    }

    GLint max_layers = 256;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    if (max_layers < 1) {
        max_layers = 256;
    }
    if (pool_slot_capacity > (uint32_t)max_layers) {
        pool_slot_capacity = (uint32_t)max_layers;
    }

    we_gpu_brush_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->pool_cap = pool_slot_capacity;
    ctx->free_slots = malloc((size_t)pool_slot_capacity * sizeof(uint32_t));
    if (!ctx->free_slots) {
        free(ctx);
        return NULL;
    }

    ctx->egl_dpy = eglGetCurrentDisplay();
    ctx->fn_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    ctx->fn_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    ctx->fn_eglClientWaitSyncKHR =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    static const float k_quad_verts[8] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
    };
    glGenVertexArrays(1, &ctx->vao);
    glGenBuffers(1, &ctx->vbo);
    if (!ctx->vao || !ctx->vbo) {
        restore_gl_state(&gl_st);
        we_gpu_brush_destroy(ctx);
        return NULL;
    }

    glBindVertexArray(ctx->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_quad_verts), k_quad_verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!init_brush_programs(ctx)) {
        restore_gl_state(&gl_st);
        we_gpu_brush_destroy(ctx);
        return NULL;
    }

    glGenFramebuffers(1, &ctx->canvas_fbo);
    glGenFramebuffers(1, &ctx->pool_fbo);
    glGenTextures(1, &ctx->pool_tex);
    if (!ctx->canvas_fbo || !ctx->pool_fbo || !ctx->pool_tex) {
        restore_gl_state(&gl_st);
        we_gpu_brush_destroy(ctx);
        return NULL;
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx->pool_tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, (GLsizei)WE_BRUSH_TILE_SIZE,
                   (GLsizei)WE_BRUSH_TILE_SIZE, (GLsizei)ctx->pool_cap);
    GLenum storage_err = glGetError();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    if (storage_err != GL_NO_ERROR) {
        restore_gl_state(&gl_st);
        we_gpu_brush_destroy(ctx);
        return NULL;
    }

    restore_gl_state(&gl_st);
    return ctx;
}

void we_gpu_brush_destroy(we_gpu_brush_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (uint32_t i = 0; i < ctx->stroke_count; i++) {
        free(ctx->strokes[i].tiles);
    }
    free(ctx->strokes);
    free(ctx->active_tiles);
    free(ctx->dirty_bitset);
    free(ctx->free_slots);

    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        if (ctx->prog_copy_in)
            glDeleteProgram(ctx->prog_copy_in);
        if (ctx->prog_r8_mask)
            glDeleteProgram(ctx->prog_r8_mask);
        if (ctx->prog_rg88_flow)
            glDeleteProgram(ctx->prog_rg88_flow);
        if (ctx->prog_kawase_down)
            glDeleteProgram(ctx->prog_kawase_down);
        if (ctx->prog_kawase_up)
            glDeleteProgram(ctx->prog_kawase_up);
        if (ctx->prog_smudge_blur)
            glDeleteProgram(ctx->prog_smudge_blur);

        destroy_scratch_fbos(ctx);
        if (ctx->canvas_fbo)
            glDeleteFramebuffers(1, &ctx->canvas_fbo);
        if (ctx->pool_fbo)
            glDeleteFramebuffers(1, &ctx->pool_fbo);
        for (uint32_t i = 0; i < 2u; i++) {
            if (ctx->readback_pbo[i])
                glDeleteBuffers(1, &ctx->readback_pbo[i]);
        }
        if (ctx->pool_tex)
            glDeleteTextures(1, &ctx->pool_tex);
        if (ctx->vbo)
            glDeleteBuffers(1, &ctx->vbo);
        if (ctx->vao)
            glDeleteVertexArrays(1, &ctx->vao);
    }

    free(ctx);
}

static we_material_pass_t *find_target_pass(we_scene_t *scene, int32_t object_id,
                                            uint32_t pass_index)
{
    if (!scene || !scene->objects) {
        return NULL;
    }
    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->id != object_id) {
            continue;
        }
        if (pass_index == 0xFFFFFFFFu) {
            return obj->has_base_pass ? &obj->base_pass : NULL;
        }
        uint32_t linear_idx = 0;
        for (uint32_t e = 0; e < obj->effect_count; e++) {
            for (uint32_t p = 0; p < obj->effects[e].pass_count; p++) {
                if (linear_idx == pass_index) {
                    return &obj->effects[e].passes[p];
                }
                linear_idx++;
            }
        }
        return NULL;
    }
    return NULL;
}

static int swizzle_mode_for_format(we_tex_format_t fmt)
{
    if (fmt == WE_TEX_FMT_RG88) {
        return 1;
    }
    if (fmt == WE_TEX_FMT_R8) {
        return 2;
    }
    return 0;
}

static bool promote_target_to_rgba8(we_gpu_brush_ctx_t *ctx, we_tex_t *wt, uint32_t w, uint32_t h,
                                    GLint saved_min, GLint saved_mag, GLint saved_wrap_s,
                                    GLint saved_wrap_t, GLint saved_max_lvl, bool has_mipmaps,
                                    GLint swiz_r, GLint swiz_g, GLint swiz_b, GLint swiz_a,
                                    brush_gl_state_t *gl_st)
{
    GLuint old_tex = (GLuint)wt->gl_tex;
    GLuint new_tex = 0;
    glGenTextures(1, &new_tex);
    if (!new_tex) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, new_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->canvas_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, new_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &new_tex);
        return false;
    }

    /* Sample old_tex (with its existing swizzle) into unswizzled GL_RGBA8 new_tex. */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, old_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glUseProgram(ctx->prog_copy_in);
    glUniform4f(ctx->loc_copy_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_copy_src_uv, 0.0f, 0.0f, 1.0f, 1.0f);
    glUniform2f(ctx->loc_copy_dst_size, (float)w, (float)h);
    glUniform1i(ctx->loc_copy_tex, 0);
    glUniform1i(ctx->loc_copy_swizzle, swizzle_mode_for_format(wt->format));

    glBindVertexArray(ctx->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Preserve R8/RG88 shader sampling swizzles on the promoted RGBA8 backing. */
    glBindTexture(GL_TEXTURE_2D, new_tex);
    if (wt->format == WE_TEX_FMT_RG88) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
    } else if (wt->format == WE_TEX_FMT_R8) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
    } else if (wt->format == WE_TEX_FMT_RGBA8888) {
        (void)swiz_r;
        (void)swiz_g;
        (void)swiz_b;
        (void)swiz_a;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    } else {
        wt->format = WE_TEX_FMT_RGBA8888;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, saved_min);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, saved_mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, saved_wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, saved_wrap_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, saved_max_lvl);
    if (has_mipmaps && saved_max_lvl > 0) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    wt->gl_tex = (uint32_t)new_tex;
    if (wt->frame_gl_texs) {
        for (uint32_t i = 0; i < wt->image_count; i++) {
            if (wt->frame_gl_texs[i] == (uint32_t)old_tex) {
                wt->frame_gl_texs[i] = (uint32_t)new_tex;
            }
        }
    }
    if (gl_st) {
        if ((GLuint)gl_st->prev_tex0 == old_tex) {
            gl_st->prev_tex0 = 0;
        }
        if ((GLuint)gl_st->prev_tex1 == old_tex) {
            gl_st->prev_tex1 = 0;
        }
    }
    glDeleteTextures(1, &old_tex);
    return true;
}

bool we_gpu_brush_bind_target(we_gpu_brush_ctx_t *ctx, struct we_scene *scene, int32_t object_id,
                              uint32_t pass_index, uint32_t tex_slot)
{
    if (!ctx || !scene || ctx->in_stroke || tex_slot >= WE_MAX_PASS_TEXTURES ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }

    we_material_pass_t *pass = find_target_pass(scene, object_id, pass_index);
    if (!pass || !pass->textures[tex_slot] || pass->textures[tex_slot]->gl_tex == 0) {
        return false;
    }

    we_tex_t *wt = pass->textures[tex_slot];
    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)wt->gl_tex);

    uint32_t w = wt->texture_width;
    uint32_t h = wt->texture_height;
    if (w == 0 || h == 0) {
        GLint gw = 0, gh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &gw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &gh);
        if (gw <= 0 || gh <= 0) {
            restore_gl_state(&gl_st);
            return false;
        }
        if (gw > (GLint)WE_BRUSH_MAX_CANVAS_DIM)
            gw = (GLint)WE_BRUSH_MAX_CANVAS_DIM;
        if (gh > (GLint)WE_BRUSH_MAX_CANVAS_DIM)
            gh = (GLint)WE_BRUSH_MAX_CANVAS_DIM;
        w = (uint32_t)gw;
        h = (uint32_t)gh;
    } else {
        if (w > WE_BRUSH_MAX_CANVAS_DIM)
            w = WE_BRUSH_MAX_CANVAS_DIM;
        if (h > WE_BRUSH_MAX_CANVAS_DIM)
            h = WE_BRUSH_MAX_CANVAS_DIM;
    }

    GLint internal_fmt = 0, lvl1_w = 0;
    GLint saved_max_lvl = 0, saved_min = GL_LINEAR, saved_mag = GL_LINEAR;
    GLint saved_wrap_s = GL_REPEAT, saved_wrap_t = GL_REPEAT;
    GLint swiz_r = GL_RED, swiz_g = GL_GREEN, swiz_b = GL_BLUE, swiz_a = GL_ALPHA;

    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_fmt);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &lvl1_w);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &saved_max_lvl);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &saved_min);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &saved_mag);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &saved_wrap_s);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &saved_wrap_t);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &swiz_r);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &swiz_g);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &swiz_b);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &swiz_a);

    bool has_mipmaps = (lvl1_w > 0) && (saved_max_lvl > 0);
    bool needs_promote =
        (wt->format == WE_TEX_FMT_DXT1 || wt->format == WE_TEX_FMT_DXT3 ||
         wt->format == WE_TEX_FMT_DXT5 || wt->format == WE_TEX_FMT_BC7) ||
        (internal_fmt != GL_RGBA8 && internal_fmt != GL_RG8 && internal_fmt != GL_R8) ||
        (wt->format == WE_TEX_FMT_RGBA8888 &&
         (swiz_r != GL_RED || swiz_g != GL_GREEN || swiz_b != GL_BLUE || swiz_a != GL_ALPHA));

    if (!needs_promote) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->canvas_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               (GLuint)wt->gl_tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            needs_promote = true;
        }
    }

    if (needs_promote) {
        if (!promote_target_to_rgba8(ctx, wt, w, h, saved_min, saved_mag, saved_wrap_s,
                                     saved_wrap_t, saved_max_lvl, has_mipmaps, swiz_r, swiz_g,
                                     swiz_b, swiz_a, &gl_st)) {
            restore_gl_state(&gl_st);
            return false;
        }
    }

    ctx->bound_scene = scene;
    ctx->bound_tex = wt;
    ctx->canvas_w = w;
    ctx->canvas_h = h;
    ctx->saved_max_level = saved_max_lvl;
    ctx->saved_min_filter = saved_min;
    ctx->saved_mag_filter = saved_mag;
    ctx->saved_wrap_s = saved_wrap_s;
    ctx->saved_wrap_t = saved_wrap_t;
    ctx->has_mipmaps = has_mipmaps;

    /* Invalidate scene texture unit cache in case wt->gl_tex was promoted. */
    memset(scene->current_tex2d, 0xFF, sizeof(scene->current_tex2d));
    scene->current_active_unit = 0xFFFFFFFFu;

    restore_gl_state(&gl_st);
    return true;
}

void we_gpu_brush_unbind_scene(we_gpu_brush_ctx_t *ctx, const struct we_scene *scene)
{
    if (!ctx) {
        return;
    }

    if (scene == NULL || ctx->bound_scene == scene) {
        if (ctx->in_stroke) {
            for (uint32_t i = 0; i < ctx->active_tile_count; i++) {
                free_pool_slot(ctx, ctx->active_tiles[i].pre_slot);
            }
            ctx->in_stroke = false;
            ctx->active_tile_count = 0;
            if (ctx->dirty_bitset && ctx->dirty_bitset_words > 0) {
                memset(ctx->dirty_bitset, 0, ctx->dirty_bitset_words * sizeof(uint64_t));
            }
        }
        ctx->bound_scene = NULL;
        ctx->bound_tex = NULL;
        ctx->canvas_w = 0;
        ctx->canvas_h = 0;
        ctx->has_mipmaps = false;
    }

    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < ctx->stroke_count; i++) {
        brush_stroke_record_t *rec = &ctx->strokes[i];
        if (scene == NULL || rec->scene == scene) {
            release_stroke_record_slots(ctx, rec);
        } else {
            if (write_idx != i) {
                ctx->strokes[write_idx] = *rec;
            }
            write_idx++;
        }
    }
    ctx->stroke_count = write_idx;
}

static bool brush_params_valid(const we_brush_params_t *p)
{
    if (!p || p->channel > WE_BRUSH_CHAN_SMUDGE || p->op_mode > WE_BRUSH_OP_SMUDGE_ADV) {
        return false;
    }
    if (!isfinite(p->radius_px) || !isfinite(p->hardness) || !isfinite(p->falloff_power) ||
        !isfinite(p->opacity) || !isfinite(p->target_value) || !isfinite(p->flow_strength) ||
        !isfinite(p->blur_radius_px) || !isfinite(p->pressure_size_weight) ||
        !isfinite(p->pressure_alpha_weight) || !isfinite(p->velocity_damp_weight)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!isfinite(p->color_rgba[i])) {
            return false;
        }
    }
    return true;
}

bool we_gpu_brush_begin_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id,
                               const we_brush_params_t *params)
{
    if (!ctx || !ctx->bound_tex || ctx->bound_tex->gl_tex == 0 || !brush_params_valid(params) ||
        ctx->in_stroke || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    if (!ensure_scratch_fbos(ctx)) {
        restore_gl_state(&gl_st);
        return false;
    }

    uint32_t tx_cnt = (ctx->canvas_w + WE_BRUSH_TILE_SIZE - 1u) / WE_BRUSH_TILE_SIZE;
    uint32_t ty_cnt = (ctx->canvas_h + WE_BRUSH_TILE_SIZE - 1u) / WE_BRUSH_TILE_SIZE;
    size_t total_tiles = (size_t)tx_cnt * (size_t)ty_cnt;
    size_t words = (total_tiles + 63u) / 64u;
    if (words == 0) {
        words = 1u;
    }

    if (words > ctx->dirty_bitset_words) {
        uint64_t *new_bs = calloc(words, sizeof(uint64_t));
        if (!new_bs) {
            restore_gl_state(&gl_st);
            return false;
        }
        free(ctx->dirty_bitset);
        ctx->dirty_bitset = new_bs;
        ctx->dirty_bitset_words = words;
    } else {
        memset(ctx->dirty_bitset, 0, words * sizeof(uint64_t));
    }

    /* Clamp GL_TEXTURE_MAX_LEVEL = 0 during active strokes to prevent mipmap bleed. */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)ctx->bound_tex->gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    GLint stroke_filter =
        (ctx->bound_tex->flags & WE_TEX_FLAG_NO_INTERPOLATION) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, stroke_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, stroke_filter);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->in_stroke = true;
    ctx->active_stroke_id = stroke_id;
    ctx->active_params = *params;
    ctx->tiles_x = tx_cnt;
    ctx->tiles_y = ty_cnt;
    ctx->active_tile_count = 0;

    restore_gl_state(&gl_st);
    return true;
}

static void copy_canvas_tile_to_pool_slot(we_gpu_brush_ctx_t *ctx, GLuint target_tex,
                                          uint32_t canvas_w, uint32_t canvas_h, uint16_t tx,
                                          uint16_t ty, uint32_t slot)
{
    int32_t px = (int32_t)tx * (int32_t)WE_BRUSH_TILE_SIZE;
    int32_t py = (int32_t)ty * (int32_t)WE_BRUSH_TILE_SIZE;
    int32_t cw = (int32_t)canvas_w - px;
    int32_t ch = (int32_t)canvas_h - py;
    if (cw > (int32_t)WE_BRUSH_TILE_SIZE)
        cw = (int32_t)WE_BRUSH_TILE_SIZE;
    if (ch > (int32_t)WE_BRUSH_TILE_SIZE)
        ch = (int32_t)WE_BRUSH_TILE_SIZE;
    if (cw <= 0 || ch <= 0) {
        return;
    }

    /* Blit instead of glCopyTexSubImage3D: ES 3.1 forbids copying R8/RG8 into RGBA8 array. */
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx->pool_tex, 0,
                              (GLint)slot);
    glBlitFramebuffer(px, py, px + cw, py + ch, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static bool capture_cow_tiles_in_rect(we_gpu_brush_ctx_t *ctx, int32_t dx0, int32_t dy0,
                                      int32_t dx1, int32_t dy1)
{
    if (dx0 >= dx1 || dy0 >= dy1) {
        return true;
    }
    uint32_t tx0 = (uint32_t)dx0 / WE_BRUSH_TILE_SIZE;
    uint32_t ty0 = (uint32_t)dy0 / WE_BRUSH_TILE_SIZE;
    uint32_t tx1 = (uint32_t)(dx1 - 1) / WE_BRUSH_TILE_SIZE;
    uint32_t ty1 = (uint32_t)(dy1 - 1) / WE_BRUSH_TILE_SIZE;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->canvas_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->pool_fbo);

    bool ok = true;
    for (uint32_t ty = ty0; ty <= ty1 && ok; ty++) {
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            size_t tid = (size_t)ty * (size_t)ctx->tiles_x + (size_t)tx;
            size_t widx = tid >> 6u;
            uint64_t bit = 1ULL << (tid & 63u);
            if ((ctx->dirty_bitset[widx] & bit) != 0) {
                continue;
            }

            uint32_t slot = alloc_pool_slot(ctx, false, 0);
            if (slot == UINT32_MAX) {
                ok = false;
                break;
            }

            if (ctx->active_tile_count == ctx->active_tile_cap) {
                uint32_t new_cap = ctx->active_tile_cap ? ctx->active_tile_cap * 2u : 64u;
                brush_tile_record_t *grown =
                    realloc(ctx->active_tiles, (size_t)new_cap * sizeof(brush_tile_record_t));
                if (!grown) {
                    free_pool_slot(ctx, slot);
                    ok = false;
                    break;
                }
                ctx->active_tiles = grown;
                ctx->active_tile_cap = new_cap;
            }

            ctx->dirty_bitset[widx] |= bit;

            copy_canvas_tile_to_pool_slot(ctx, (GLuint)ctx->bound_tex->gl_tex, ctx->canvas_w,
                                          ctx->canvas_h, (uint16_t)tx, (uint16_t)ty, slot);

            ctx->active_tiles[ctx->active_tile_count++] = (brush_tile_record_t){
                .tx = (uint16_t)tx,
                .ty = (uint16_t)ty,
                .pre_slot = slot,
                .post_slot = UINT32_MAX,
            };
        }
    }

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

static void set_scratch_filter(GLuint tex, GLint filter)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

static void run_copy_in_pass(we_gpu_brush_ctx_t *ctx, int32_t px0, int32_t py0, int32_t px1,
                             int32_t py1)
{
    int32_t pw = px1 - px0;
    int32_t ph = py1 - py0;
    float w = (float)ctx->canvas_w;
    float h = (float)ctx->canvas_h;

    /* Unbind scratch_tex[0] from sampler units before attaching it to FBO. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)ctx->bound_tex->gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->scratch_fbo[0]);
    glViewport(0, 0, pw, ph);

    glUseProgram(ctx->prog_copy_in);
    glUniform4f(ctx->loc_copy_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_copy_src_uv, (float)px0 / w, (float)py0 / h, (float)px1 / w,
                (float)py1 / h);
    glUniform2f(ctx->loc_copy_dst_size, (float)pw, (float)ph);
    glUniform1i(ctx->loc_copy_tex, 0);
    glUniform1i(ctx->loc_copy_swizzle, swizzle_mode_for_format(ctx->bound_tex->format));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* Restore stroke-time filtering on T_canvas before unbinding GL_TEXTURE0. */
    GLint stroke_filter =
        (ctx->bound_tex->flags & WE_TEX_FLAG_NO_INTERPOLATION) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, stroke_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, stroke_filter);

    /* Immediately unbind T_canvas so Pass C cannot observe it on GL_TEXTURE0. */
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void run_write_back_pass(we_gpu_brush_ctx_t *ctx, uint32_t src_idx, int32_t px0, int32_t py0,
                                int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1)
{
    float w = (float)ctx->canvas_w;
    float h = (float)ctx->canvas_h;
    float sw = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float sh = (float)WE_BRUSH_SCRATCH_MAX_DIM;

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    set_scratch_filter(ctx->scratch_tex[src_idx], GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->canvas_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint)ctx->bound_tex->gl_tex, 0);
    glViewport(0, 0, (GLsizei)ctx->canvas_w, (GLsizei)ctx->canvas_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(dx0, dy0, dx1 - dx0, dy1 - dy0);

    glUseProgram(ctx->prog_copy_in);
    glUniform4f(ctx->loc_copy_dst_rect, 2.0f * (float)dx0 / w - 1.0f, 2.0f * (float)dy0 / h - 1.0f,
                2.0f * (float)dx1 / w - 1.0f, 2.0f * (float)dy1 / h - 1.0f);
    glUniform4f(ctx->loc_copy_src_uv, (float)(dx0 - px0) / sw, (float)(dy0 - py0) / sh,
                (float)(dx1 - px0) / sw, (float)(dy1 - py0) / sh);
    glUniform2f(ctx->loc_copy_dst_size, (float)(dx1 - dx0), (float)(dy1 - dy0));
    glUniform1i(ctx->loc_copy_tex, 0);
    glUniform1i(ctx->loc_copy_swizzle, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int32_t clamp_k0_dim(int32_t dim)
{
    int32_t k0 = (dim + 1) / 2;
    if (k0 < 1) {
        return 1;
    }
    if (k0 > 512) {
        return 512;
    }
    return k0;
}

static void run_gaussian_blur_2pass(we_gpu_brush_ctx_t *ctx, uint32_t orig_idx, int32_t pw,
                                    int32_t ph, float blur_radius_px, float *out_uv_clamp,
                                    int32_t *out_k0_w, int32_t *out_k0_h)
{
    float sw = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float sh = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float umin = 0.5f / sw;
    float vmin = 0.5f / sh;
    float umax = ((float)pw - 0.5f) / sw;
    float vmax = ((float)ph - 0.5f) / sh;

    /* 2-pass Rákos 5-tap bilinear Gaussian: S_ping -> S_pong (H) -> K[0] (V). */
    float gauss_scale = fmaxf(blur_radius_px / 3.230769f, 0.35f);
    uint32_t tmp_idx = orig_idx ^ 1u;

    glActiveTexture(GL_TEXTURE0);
    set_scratch_filter(ctx->scratch_tex[orig_idx], GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->scratch_fbo[tmp_idx]);
    glViewport(0, 0, pw, ph);
    glUseProgram(ctx->prog_kawase_down);
    glUniform4f(ctx->loc_kdown_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_kdown_src_uv, 0.0f, 0.0f, (float)pw / sw, (float)ph / sh);
    glUniform2f(ctx->loc_kdown_dst_size, (float)pw, (float)ph);
    glUniform1i(ctx->loc_kdown_src, 0);
    glUniform2f(ctx->loc_kdown_half_px, 0.5f / sw, 0.5f / sh);
    glUniform1f(ctx->loc_kdown_offset, gauss_scale);
    glUniform4f(ctx->loc_kdown_uv_clamp, umin, vmin, umax, vmax);
    glUniform1i(ctx->loc_kdown_pass_kind, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    int32_t k0_w = clamp_k0_dim(pw);
    int32_t k0_h = clamp_k0_dim(ph);

    set_scratch_filter(ctx->scratch_tex[tmp_idx], GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->kawase_fbo[0]);
    glViewport(0, 0, k0_w, k0_h);
    glUniform4f(ctx->loc_kdown_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_kdown_src_uv, 0.0f, 0.0f, (float)pw / sw, (float)ph / sh);
    glUniform2f(ctx->loc_kdown_dst_size, (float)k0_w, (float)k0_h);
    glUniform2f(ctx->loc_kdown_half_px, 0.5f / sw, 0.5f / sh);
    glUniform1f(ctx->loc_kdown_offset, gauss_scale);
    glUniform4f(ctx->loc_kdown_uv_clamp, umin, vmin, umax, vmax);
    glUniform1i(ctx->loc_kdown_pass_kind, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    out_uv_clamp[0] = 0.5f / 512.0f;
    out_uv_clamp[1] = 0.5f / 512.0f;
    out_uv_clamp[2] = ((float)k0_w - 0.5f) / 512.0f;
    out_uv_clamp[3] = ((float)k0_h - 0.5f) / 512.0f;
    *out_k0_w = k0_w;
    *out_k0_h = k0_h;
}

static void run_kawase_blur_pyramid(we_gpu_brush_ctx_t *ctx, uint32_t orig_idx, int32_t pw,
                                    int32_t ph, float blur_radius_px, float *out_uv_clamp,
                                    int32_t *out_k0_w, int32_t *out_k0_h)
{
    int levels = (int)ceilf(log2f(blur_radius_px)) - 1;
    if (levels < 1) {
        levels = 1;
    }
    if (levels > (int)WE_BRUSH_KAWASE_LEVELS) {
        levels = (int)WE_BRUSH_KAWASE_LEVELS;
    }

    int32_t lvl_w[WE_BRUSH_KAWASE_LEVELS];
    int32_t lvl_h[WE_BRUSH_KAWASE_LEVELS];
    const float max_dim[WE_BRUSH_KAWASE_LEVELS] = {512.0f, 256.0f, 128.0f, 64.0f};

    GLuint cur_src_tex = ctx->scratch_tex[orig_idx];
    float cur_src_full_w = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float cur_src_full_h = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    int32_t cur_w = pw;
    int32_t cur_h = ph;

    glActiveTexture(GL_TEXTURE0);
    glUseProgram(ctx->prog_kawase_down);

    for (int l = 0; l < levels; l++) {
        int32_t dw = (cur_w + 1) / 2;
        int32_t dh = (cur_h + 1) / 2;
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
        if (dw > (int32_t)max_dim[l]) {
            dw = (int32_t)max_dim[l];
        }
        if (dh > (int32_t)max_dim[l]) {
            dh = (int32_t)max_dim[l];
        }
        lvl_w[l] = dw;
        lvl_h[l] = dh;

        if (l == 0) {
            set_scratch_filter(cur_src_tex, GL_LINEAR);
        } else {
            glBindTexture(GL_TEXTURE_2D, cur_src_tex);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->kawase_fbo[l]);
        glViewport(0, 0, dw, dh);
        glUniform4f(ctx->loc_kdown_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
        glUniform4f(ctx->loc_kdown_src_uv, 0.0f, 0.0f, (float)cur_w / cur_src_full_w,
                    (float)cur_h / cur_src_full_h);
        glUniform2f(ctx->loc_kdown_dst_size, (float)dw, (float)dh);
        glUniform1i(ctx->loc_kdown_src, 0);
        glUniform2f(ctx->loc_kdown_half_px, 0.5f / cur_src_full_w, 0.5f / cur_src_full_h);
        glUniform1f(ctx->loc_kdown_offset, 1.5f);
        glUniform4f(ctx->loc_kdown_uv_clamp, 0.5f / cur_src_full_w, 0.5f / cur_src_full_h,
                    ((float)cur_w - 0.5f) / cur_src_full_w, ((float)cur_h - 0.5f) / cur_src_full_h);
        glUniform1i(ctx->loc_kdown_pass_kind, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindTexture(GL_TEXTURE_2D, 0);

        cur_src_tex = ctx->kawase_tex[l];
        cur_src_full_w = max_dim[l];
        cur_src_full_h = max_dim[l];
        cur_w = dw;
        cur_h = dh;
    }

    glUseProgram(ctx->prog_kawase_up);
    for (int l = levels - 2; l >= 0; l--) {
        int src_l = l + 1;
        glBindTexture(GL_TEXTURE_2D, ctx->kawase_tex[src_l]);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->kawase_fbo[l]);
        glViewport(0, 0, lvl_w[l], lvl_h[l]);
        glUniform4f(ctx->loc_kup_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
        glUniform4f(ctx->loc_kup_src_uv, 0.0f, 0.0f, (float)lvl_w[src_l] / max_dim[src_l],
                    (float)lvl_h[src_l] / max_dim[src_l]);
        glUniform2f(ctx->loc_kup_dst_size, (float)lvl_w[l], (float)lvl_h[l]);
        glUniform1i(ctx->loc_kup_src, 0);
        glUniform2f(ctx->loc_kup_half_px, 0.5f / max_dim[src_l], 0.5f / max_dim[src_l]);
        glUniform1f(ctx->loc_kup_offset, 1.5f);
        glUniform4f(ctx->loc_kup_uv_clamp, 0.5f / max_dim[src_l], 0.5f / max_dim[src_l],
                    ((float)lvl_w[src_l] - 0.5f) / max_dim[src_l],
                    ((float)lvl_h[src_l] - 0.5f) / max_dim[src_l]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out_uv_clamp[0] = 0.5f / 512.0f;
    out_uv_clamp[1] = 0.5f / 512.0f;
    out_uv_clamp[2] = ((float)lvl_w[0] - 0.5f) / 512.0f;
    out_uv_clamp[3] = ((float)lvl_h[0] - 0.5f) / 512.0f;
    *out_k0_w = lvl_w[0];
    *out_k0_h = lvl_h[0];
}

static uint32_t run_blur_pyramid(we_gpu_brush_ctx_t *ctx, uint32_t orig_idx, int32_t pw, int32_t ph,
                                 float blur_radius_px, float *out_uv_clamp, int32_t *out_k0_w,
                                 int32_t *out_k0_h)
{
    if (blur_radius_px <= 4.0f) {
        run_gaussian_blur_2pass(ctx, orig_idx, pw, ph, blur_radius_px, out_uv_clamp, out_k0_w,
                                out_k0_h);
    } else {
        run_kawase_blur_pyramid(ctx, orig_idx, pw, ph, blur_radius_px, out_uv_clamp, out_k0_w,
                                out_k0_h);
    }
    return 0;
}

static uint32_t run_flow_kernel_pass(we_gpu_brush_ctx_t *ctx, uint32_t src_idx, uint32_t dst_idx,
                                     int32_t pw, int32_t ph, float ax, float ay, float bx, float by,
                                     float ra, float rb, float opacity, const we_brush_dab_t *dab)
{
    float sw = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float sh = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    const we_brush_params_t *p = &ctx->active_params;

    int u_mode = 0;
    if (p->op_mode == WE_BRUSH_OP_FLOW_ERASE) {
        u_mode = 1;
    } else if (p->op_mode == WE_BRUSH_OP_FLOW_COMB) {
        u_mode = 2;
    }

    float fx = dab->flow_dir_x;
    float fy = (dab->flags & 1u) ? -dab->flow_dir_y : dab->flow_dir_y;
    float fscale = fminf(fmaxf(p->flow_strength, 0.0f), 1.0f);
    fx *= fscale;
    fy *= fscale;
    float mag = hypotf(fx, fy);
    if (mag > 1.0f) {
        fx /= mag;
        fy /= mag;
    }

    glActiveTexture(GL_TEXTURE0);
    set_scratch_filter(ctx->scratch_tex[src_idx], GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->scratch_fbo[dst_idx]);
    glViewport(0, 0, pw, ph);

    glUseProgram(ctx->prog_rg88_flow);
    glUniform4f(ctx->loc_flow_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_flow_src_uv, 0.0f, 0.0f, (float)pw / sw, (float)ph / sh);
    glUniform2f(ctx->loc_flow_dst_size, (float)pw, (float)ph);
    glUniform1i(ctx->loc_flow_src, 0);
    glUniform2f(ctx->loc_flow_pt_a, ax, ay);
    glUniform2f(ctx->loc_flow_pt_b, bx, by);
    glUniform2f(ctx->loc_flow_dir, fx, fy);
    glUniform1f(ctx->loc_flow_rad_a, ra);
    glUniform1f(ctx->loc_flow_rad_b, rb);
    glUniform1f(ctx->loc_flow_hard, p->hardness);
    glUniform1f(ctx->loc_flow_falloff, p->falloff_power);
    glUniform1f(ctx->loc_flow_opacity, opacity);
    glUniform1i(ctx->loc_flow_mode, u_mode);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return dst_idx;
}

static uint32_t run_smudge_blur_kernel_pass(we_gpu_brush_ctx_t *ctx, uint32_t src_idx,
                                            uint32_t dst_idx, int32_t pw, int32_t ph, float ax,
                                            float ay, float bx, float by, float ra, float rb,
                                            float opacity, const we_brush_dab_t *dab)
{
    float sw = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float sh = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    const we_brush_params_t *p = &ctx->active_params;

    float blur_r = p->blur_radius_px > 0.0f ? p->blur_radius_px : 4.0f;
    if (blur_r > 64.0f) {
        blur_r = 64.0f;
    }

    float k0_clamp[4];
    int32_t k0_w = 1;
    int32_t k0_h = 1;
    (void)run_blur_pyramid(ctx, src_idx, pw, ph, blur_r, k0_clamp, &k0_w, &k0_h);

    float orig_umin = 0.5f / sw;
    float orig_vmin = 0.5f / sh;
    float orig_umax = ((float)pw - 0.5f) / sw;
    float orig_vmax = ((float)ph - 0.5f) / sh;

    int sb_mode =
        (p->channel == WE_BRUSH_CHAN_SMUDGE || p->op_mode == WE_BRUSH_OP_SMUDGE_ADV) ? 1 : 0;

    glActiveTexture(GL_TEXTURE0);
    set_scratch_filter(ctx->scratch_tex[src_idx], GL_LINEAR);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->kawase_tex[0]);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->scratch_fbo[dst_idx]);
    glViewport(0, 0, pw, ph);

    glUseProgram(ctx->prog_smudge_blur);
    glUniform4f(ctx->loc_sb_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_sb_src_uv, 0.0f, 0.0f, (float)pw / sw, (float)ph / sh);
    glUniform2f(ctx->loc_sb_dst_size, (float)pw, (float)ph);
    glUniform1i(ctx->loc_sb_orig, 0);
    glUniform1i(ctx->loc_sb_blurred, 1);
    glUniform2f(ctx->loc_sb_texel, 1.0f / sw, 1.0f / sh);
    glUniform2f(ctx->loc_sb_pt_a, ax, ay);
    glUniform2f(ctx->loc_sb_pt_b, bx, by);
    glUniform2f(ctx->loc_sb_drag, dab->drag_dx_px, dab->drag_dy_px);
    glUniform1f(ctx->loc_sb_rad_a, ra);
    glUniform1f(ctx->loc_sb_rad_b, rb);
    glUniform1f(ctx->loc_sb_hard, p->hardness);
    glUniform1f(ctx->loc_sb_falloff, p->falloff_power);
    glUniform1f(ctx->loc_sb_opacity, opacity);
    glUniform4f(ctx->loc_sb_uv_clamp, orig_umin, orig_vmin, orig_umax, orig_vmax);
    glUniform4f(ctx->loc_sb_blur_uv_clamp, k0_clamp[0], k0_clamp[1], k0_clamp[2], k0_clamp[3]);
    glUniform2f(ctx->loc_sb_blur_uv_scale, (2.0f * (float)k0_w) / (float)pw,
                (2.0f * (float)k0_h) / (float)ph);
    glUniform1i(ctx->loc_sb_op_mode, sb_mode);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return dst_idx;
}

static uint32_t run_mask_rgba_kernel_pass(we_gpu_brush_ctx_t *ctx, uint32_t src_idx,
                                          uint32_t dst_idx, int32_t pw, int32_t ph, float ax,
                                          float ay, float bx, float by, float ra, float rb,
                                          float opacity)
{
    float sw = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    float sh = (float)WE_BRUSH_SCRATCH_MAX_DIM;
    const we_brush_params_t *p = &ctx->active_params;

    int mask_mode = (int)(p->op_mode <= WE_BRUSH_OP_ERASE ? p->op_mode : WE_BRUSH_OP_ADD_BUILDUP);
    int is_rgba = (p->channel == WE_BRUSH_CHAN_COLOR_RGBA8) ? 1 : 0;

    glActiveTexture(GL_TEXTURE0);
    set_scratch_filter(ctx->scratch_tex[src_idx], GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->scratch_fbo[dst_idx]);
    glViewport(0, 0, pw, ph);

    glUseProgram(ctx->prog_r8_mask);
    glUniform4f(ctx->loc_mask_dst_rect, -1.0f, -1.0f, 1.0f, 1.0f);
    glUniform4f(ctx->loc_mask_src_uv, 0.0f, 0.0f, (float)pw / sw, (float)ph / sh);
    glUniform2f(ctx->loc_mask_dst_size, (float)pw, (float)ph);
    glUniform1i(ctx->loc_mask_src, 0);
    glUniform2f(ctx->loc_mask_pt_a, ax, ay);
    glUniform2f(ctx->loc_mask_pt_b, bx, by);
    glUniform1f(ctx->loc_mask_rad_a, ra);
    glUniform1f(ctx->loc_mask_rad_b, rb);
    glUniform1f(ctx->loc_mask_hard, p->hardness);
    glUniform1f(ctx->loc_mask_falloff, p->falloff_power);
    glUniform1f(ctx->loc_mask_opacity, opacity);
    glUniform1f(ctx->loc_mask_target, p->target_value);
    glUniform1i(ctx->loc_mask_mode, mask_mode);
    glUniform1i(ctx->loc_mask_is_rgba, is_rgba);
    glUniform4fv(ctx->loc_mask_color, 1, p->color_rgba);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return dst_idx;
}

static uint32_t execute_kernel_pass(we_gpu_brush_ctx_t *ctx, uint32_t src_idx, int32_t px0,
                                    int32_t py0, int32_t pw, int32_t ph, const we_brush_dab_t *dab)
{
    uint32_t dst_idx = src_idx ^ 1u;
    float ax = dab->ax - (float)px0;
    float ay = dab->ay - (float)py0;
    float bx = dab->bx - (float)px0;
    float by = dab->by - (float)py0;
    float ra = fminf(fmaxf(dab->radius_a, 0.5f), 512.0f);
    float rb = fminf(fmaxf(dab->radius_b, 0.5f), 512.0f);
    float opacity = fminf(fmaxf(dab->opacity_eff, 0.0f), 1.0f);

    const we_brush_params_t *p = &ctx->active_params;
    if (p->channel == WE_BRUSH_CHAN_FLOW_RG88) {
        return run_flow_kernel_pass(ctx, src_idx, dst_idx, pw, ph, ax, ay, bx, by, ra, rb, opacity,
                                    dab);
    }
    if (p->channel == WE_BRUSH_CHAN_BLUR || p->channel == WE_BRUSH_CHAN_SMUDGE ||
        p->op_mode == WE_BRUSH_OP_BLUR_MASKED || p->op_mode == WE_BRUSH_OP_SMUDGE_ADV) {
        return run_smudge_blur_kernel_pass(ctx, src_idx, dst_idx, pw, ph, ax, ay, bx, by, ra, rb,
                                           opacity, dab);
    }
    return run_mask_rgba_kernel_pass(ctx, src_idx, dst_idx, pw, ph, ax, ay, bx, by, ra, rb,
                                     opacity);
}

static bool dab_compute_clamped_rect(const we_brush_dab_t *dab, int32_t cw, int32_t ch,
                                     int32_t *out_x0, int32_t *out_y0, int32_t *out_x1,
                                     int32_t *out_y1)
{
    if (!isfinite(dab->ax) || !isfinite(dab->ay) || !isfinite(dab->bx) || !isfinite(dab->by) ||
        !isfinite(dab->radius_a) || !isfinite(dab->radius_b) || !isfinite(dab->opacity_eff) ||
        !isfinite(dab->drag_dx_px) || !isfinite(dab->drag_dy_px) || !isfinite(dab->flow_dir_x) ||
        !isfinite(dab->flow_dir_y)) {
        return false;
    }

    float max_r = fmaxf(fmaxf(dab->radius_a, dab->radius_b), 0.5f);
    if (max_r > 512.0f) {
        max_r = 512.0f;
    }

    float fx0 = floorf(fminf(dab->ax, dab->bx) - max_r - 2.0f);
    float fy0 = floorf(fminf(dab->ay, dab->by) - max_r - 2.0f);
    float fx1 = ceilf(fmaxf(dab->ax, dab->bx) + max_r + 2.0f);
    float fy1 = ceilf(fmaxf(dab->ay, dab->by) + max_r + 2.0f);
    if (fx1 <= 0.0f || fy1 <= 0.0f || fx0 >= (float)cw || fy0 >= (float)ch) {
        return false;
    }

    int32_t dx0 = fx0 < 0.0f ? 0 : (int32_t)fx0;
    int32_t dy0 = fy0 < 0.0f ? 0 : (int32_t)fy0;
    int32_t dx1 = fx1 > (float)cw ? cw : (int32_t)fx1;
    int32_t dy1 = fy1 > (float)ch ? ch : (int32_t)fy1;
    if (dx0 >= dx1 || dy0 >= dy1) {
        return false;
    }

    *out_x0 = dx0;
    *out_y0 = dy0;
    *out_x1 = dx1;
    *out_y1 = dy1;
    return true;
}

static void rollback_cow_tiles(we_gpu_brush_ctx_t *ctx, uint32_t prev_tile_count)
{
    for (uint32_t i = prev_tile_count; i < ctx->active_tile_count; i++) {
        const brush_tile_record_t *tr = &ctx->active_tiles[i];
        size_t tid = (size_t)tr->ty * (size_t)ctx->tiles_x + (size_t)tr->tx;
        size_t widx = tid >> 6u;
        uint64_t bit = 1ULL << (tid & 63u);
        if (widx < ctx->dirty_bitset_words) {
            ctx->dirty_bitset[widx] &= ~bit;
        }
        free_pool_slot(ctx, tr->pre_slot);
    }
    ctx->active_tile_count = prev_tile_count;
}

static bool capture_batch_cow_tiles(we_gpu_brush_ctx_t *ctx, const we_brush_dab_t *dabs,
                                    uint32_t dab_count, int32_t cw, int32_t ch, int32_t *union_x0,
                                    int32_t *union_y0, int32_t *union_x1, int32_t *union_y1)
{
    uint32_t prev_active_count = ctx->active_tile_count;
    for (uint32_t i = 0; i < dab_count; i++) {
        int32_t dx0, dy0, dx1, dy1;
        if (!dab_compute_clamped_rect(&dabs[i], cw, ch, &dx0, &dy0, &dx1, &dy1)) {
            continue;
        }
        if (dx0 < *union_x0) {
            *union_x0 = dx0;
        }
        if (dy0 < *union_y0) {
            *union_y0 = dy0;
        }
        if (dx1 > *union_x1) {
            *union_x1 = dx1;
        }
        if (dy1 > *union_y1) {
            *union_y1 = dy1;
        }
        if (!capture_cow_tiles_in_rect(ctx, dx0, dy0, dx1, dy1)) {
            rollback_cow_tiles(ctx, prev_active_count);
            return false;
        }
    }
    return true;
}

static void render_dab_chunks(we_gpu_brush_ctx_t *ctx, const we_brush_dab_t *dab, int32_t dx0,
                              int32_t dy0, int32_t dx1, int32_t dy1, int32_t cw, int32_t ch,
                              bool needs_halo)
{
    const we_brush_params_t *p = &ctx->active_params;
    int32_t halo = 0;
    if (needs_halo) {
        float br = (isfinite(p->blur_radius_px) && p->blur_radius_px > 0.0f)
                       ? fminf(p->blur_radius_px, 256.0f)
                       : 4.0f;
        float drag_mag = fminf(fmaxf(fabsf(dab->drag_dx_px), fabsf(dab->drag_dy_px)),
                               (float)WE_BRUSH_SCRATCH_MAX_DIM);
        int32_t drag_pad = (int32_t)ceilf(drag_mag) + 4;
        halo = (int32_t)ceilf(1.5f * br);
        if (drag_pad > halo) {
            halo = drag_pad;
        }
        if (halo < 16) {
            halo = 16;
        }
        if (halo > 64) {
            halo = 64;
        }
    }

    int32_t chunk_step = (int32_t)WE_BRUSH_SCRATCH_MAX_DIM - 2 * halo;
    if (chunk_step < 128) {
        chunk_step = 128;
    }

    for (int32_t cy0 = dy0; cy0 < dy1; cy0 += chunk_step) {
        int32_t cy1 = cy0 + chunk_step;
        if (cy1 > dy1) {
            cy1 = dy1;
        }
        int32_t py0 = cy0 - halo;
        int32_t py1 = cy1 + halo;
        if (py0 < 0) {
            py0 = 0;
        }
        if (py1 > ch) {
            py1 = ch;
        }

        for (int32_t cx0 = dx0; cx0 < dx1; cx0 += chunk_step) {
            int32_t cx1 = cx0 + chunk_step;
            if (cx1 > dx1) {
                cx1 = dx1;
            }
            int32_t px0 = cx0 - halo;
            int32_t px1 = cx1 + halo;
            if (px0 < 0) {
                px0 = 0;
            }
            if (px1 > cw) {
                px1 = cw;
            }

            run_copy_in_pass(ctx, px0, py0, px1, py1);
            uint32_t out_idx = execute_kernel_pass(ctx, 0u, px0, py0, px1 - px0, py1 - py0, dab);
            run_write_back_pass(ctx, out_idx, px0, py0, cx0, cy0, cx1, cy1);
        }
    }
}

bool we_gpu_brush_dispatch_dabs(we_gpu_brush_ctx_t *ctx, const we_brush_dab_t *dabs,
                                uint32_t dab_count, we_brush_dirty_rect_t *out_dirty_rect)
{
    if (!ctx || !ctx->in_stroke || !ctx->bound_tex || !dabs || dab_count == 0 ||
        dab_count > WE_BRUSH_MAX_STROKE_DABS || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }

    uint64_t t_start_us = brush_monotonic_us();

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);
    glBindVertexArray(ctx->vao);

    int32_t cw = (int32_t)ctx->canvas_w;
    int32_t ch = (int32_t)ctx->canvas_h;
    int32_t union_x0 = cw, union_y0 = ch, union_x1 = 0, union_y1 = 0;

    if (!capture_batch_cow_tiles(ctx, dabs, dab_count, cw, ch, &union_x0, &union_y0, &union_x1,
                                 &union_y1)) {
        glBindVertexArray(0);
        restore_gl_state(&gl_st);
        return false;
    }

    const we_brush_params_t *p = &ctx->active_params;
    bool needs_halo =
        (p->channel == WE_BRUSH_CHAN_BLUR || p->channel == WE_BRUSH_CHAN_SMUDGE ||
         p->op_mode == WE_BRUSH_OP_BLUR_MASKED || p->op_mode == WE_BRUSH_OP_SMUDGE_ADV);

    for (uint32_t i = 0; i < dab_count; i++) {
        int32_t dx0, dy0, dx1, dy1;
        if (!dab_compute_clamped_rect(&dabs[i], cw, ch, &dx0, &dy0, &dx1, &dy1)) {
            continue;
        }
        render_dab_chunks(ctx, &dabs[i], dx0, dy0, dx1, dy1, cw, ch, needs_halo);
    }

    glBindVertexArray(0);
    restore_gl_state(&gl_st);

    uint64_t elapsed_us = brush_monotonic_us() - t_start_us;
    ctx->last_dab_gpu_us = (uint32_t)(elapsed_us > UINT32_MAX ? UINT32_MAX : elapsed_us);

    if (out_dirty_rect) {
        if (union_x0 < union_x1 && union_y0 < union_y1) {
            out_dirty_rect->x = union_x0;
            out_dirty_rect->y = union_y0;
            out_dirty_rect->width = union_x1 - union_x0;
            out_dirty_rect->height = union_y1 - union_y0;
        } else {
            memset(out_dirty_rect, 0, sizeof(*out_dirty_rect));
        }
    }
    return true;
}

bool we_gpu_brush_end_stroke(we_gpu_brush_ctx_t *ctx, uint32_t *out_captured_tile_count)
{
    if (!ctx || !ctx->in_stroke || !ctx->bound_tex) {
        return false;
    }

    /* Allocate stroke commit records before mutating target texture GL state. */
    if (ctx->stroke_count == ctx->stroke_cap) {
        uint32_t new_cap = ctx->stroke_cap ? ctx->stroke_cap * 2u : 32u;
        brush_stroke_record_t *grown =
            realloc(ctx->strokes, (size_t)new_cap * sizeof(brush_stroke_record_t));
        if (!grown) {
            return false;
        }
        ctx->strokes = grown;
        ctx->stroke_cap = new_cap;
    }

    brush_tile_record_t *committed_tiles = NULL;
    if (ctx->active_tile_count > 0) {
        committed_tiles = malloc((size_t)ctx->active_tile_count * sizeof(brush_tile_record_t));
        if (!committed_tiles) {
            return false;
        }
        memcpy(committed_tiles, ctx->active_tiles,
               (size_t)ctx->active_tile_count * sizeof(brush_tile_record_t));
    }

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)ctx->bound_tex->gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, ctx->saved_max_level);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ctx->saved_min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ctx->saved_mag_filter);
    if (ctx->has_mipmaps && ctx->saved_max_level > 0) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->strokes[ctx->stroke_count++] = (brush_stroke_record_t){
        .stroke_id = ctx->active_stroke_id,
        .scene = ctx->bound_scene,
        .target_gl_tex = (GLuint)ctx->bound_tex->gl_tex,
        .canvas_w = ctx->canvas_w,
        .canvas_h = ctx->canvas_h,
        .is_undone = false,
        .tiles = committed_tiles,
        .tile_count = ctx->active_tile_count,
        .tile_cap = ctx->active_tile_count,
    };

    if (out_captured_tile_count) {
        *out_captured_tile_count = ctx->active_tile_count;
    }

    ctx->in_stroke = false;
    ctx->active_tile_count = 0;

    restore_gl_state(&gl_st);
    return true;
}

static brush_stroke_record_t *find_stroke_record(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id)
{
    for (uint32_t i = ctx->stroke_count; i > 0; i--) {
        if (ctx->strokes[i - 1u].stroke_id == stroke_id) {
            return &ctx->strokes[i - 1u];
        }
    }
    return NULL;
}

static void copy_pool_slot_to_canvas(we_gpu_brush_ctx_t *ctx, GLuint target_tex, uint32_t canvas_w,
                                     uint32_t canvas_h, uint16_t tx, uint16_t ty, uint32_t slot)
{
    int32_t px = (int32_t)tx * (int32_t)WE_BRUSH_TILE_SIZE;
    int32_t py = (int32_t)ty * (int32_t)WE_BRUSH_TILE_SIZE;
    int32_t cw = (int32_t)canvas_w - px;
    int32_t ch = (int32_t)canvas_h - py;
    if (cw > (int32_t)WE_BRUSH_TILE_SIZE)
        cw = (int32_t)WE_BRUSH_TILE_SIZE;
    if (ch > (int32_t)WE_BRUSH_TILE_SIZE)
        ch = (int32_t)WE_BRUSH_TILE_SIZE;
    if (cw <= 0 || ch <= 0) {
        return;
    }

    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ctx->pool_tex, 0,
                              (GLint)slot);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
    glBlitFramebuffer(0, 0, cw, ch, px, py, px + cw, py + ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

bool we_gpu_brush_undo_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id)
{
    if (!ctx || ctx->in_stroke || !ctx->bound_tex || eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }
    brush_stroke_record_t *rec = find_stroke_record(ctx, stroke_id);
    if (!rec || rec->is_undone || rec->target_gl_tex != (GLuint)ctx->bound_tex->gl_tex) {
        return false;
    }

    /* Pre-allocate all missing post-tile slots before mutating the stroke record or evicting it. */
    uint32_t missing_posts = 0;
    for (uint32_t i = 0; i < rec->tile_count; i++) {
        if (rec->tiles[i].post_slot == UINT32_MAX) {
            missing_posts++;
        }
    }

    uint32_t *post_slots = NULL;
    if (missing_posts > 0) {
        post_slots = malloc((size_t)missing_posts * sizeof(uint32_t));
        if (!post_slots) {
            return false;
        }
        uint32_t alloc_done = 0;
        for (uint32_t j = 0; j < missing_posts; j++) {
            uint32_t slot = alloc_pool_slot(ctx, true, stroke_id);
            if (slot == UINT32_MAX) {
                for (uint32_t k = 0; k < alloc_done; k++) {
                    free_pool_slot(ctx, post_slots[k]);
                }
                free(post_slots);
                return false;
            }
            post_slots[alloc_done++] = slot;
        }
        rec = find_stroke_record(ctx, stroke_id);
        if (!rec) {
            for (uint32_t k = 0; k < missing_posts; k++) {
                free_pool_slot(ctx, post_slots[k]);
            }
            free(post_slots);
            return false;
        }
    }

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    if (missing_posts > 0) {
        /* Capture post-stroke tiles lazily on first undo so redo remains symmetric and O(K). */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->canvas_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->pool_fbo);

        uint32_t slot_idx = 0;
        for (uint32_t i = 0; i < rec->tile_count; i++) {
            brush_tile_record_t *tr = &rec->tiles[i];
            if (tr->post_slot != UINT32_MAX) {
                continue;
            }
            uint32_t slot = post_slots[slot_idx++];
            copy_canvas_tile_to_pool_slot(ctx, rec->target_gl_tex, rec->canvas_w, rec->canvas_h,
                                          tr->tx, tr->ty, slot);
            tr->post_slot = slot;
        }

        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
        free(post_slots);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->pool_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->canvas_fbo);
    for (uint32_t i = 0; i < rec->tile_count; i++) {
        const brush_tile_record_t *tr = &rec->tiles[i];
        copy_pool_slot_to_canvas(ctx, rec->target_gl_tex, rec->canvas_w, rec->canvas_h, tr->tx,
                                 tr->ty, tr->pre_slot);
    }
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx->has_mipmaps && ctx->saved_max_level > 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, rec->target_gl_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    rec->is_undone = true;
    restore_gl_state(&gl_st);
    return true;
}

bool we_gpu_brush_redo_stroke(we_gpu_brush_ctx_t *ctx, uint64_t stroke_id)
{
    if (!ctx || ctx->in_stroke || !ctx->bound_tex) {
        return false;
    }
    brush_stroke_record_t *rec = find_stroke_record(ctx, stroke_id);
    if (!rec || !rec->is_undone || rec->target_gl_tex != (GLuint)ctx->bound_tex->gl_tex) {
        return false;
    }
    for (uint32_t i = 0; i < rec->tile_count; i++) {
        if (rec->tiles[i].post_slot == UINT32_MAX) {
            return false;
        }
    }

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->pool_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->canvas_fbo);
    for (uint32_t i = 0; i < rec->tile_count; i++) {
        const brush_tile_record_t *tr = &rec->tiles[i];
        copy_pool_slot_to_canvas(ctx, rec->target_gl_tex, rec->canvas_w, rec->canvas_h, tr->tx,
                                 tr->ty, tr->post_slot);
    }
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx->has_mipmaps && ctx->saved_max_level > 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, rec->target_gl_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    rec->is_undone = false;
    restore_gl_state(&gl_st);
    return true;
}

bool we_gpu_brush_readback_texels(we_gpu_brush_ctx_t *ctx, uint32_t expected_format,
                                  uint8_t **out_pixels, size_t *out_byte_len)
{
    if (!ctx || !ctx->bound_tex || ctx->bound_tex->gl_tex == 0 || !out_pixels || !out_byte_len ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return false;
    }

    if (expected_format != (uint32_t)ctx->bound_tex->format) {
        return false;
    }

    size_t bpp = 0;
    if (expected_format == WE_TEX_FMT_R8) {
        bpp = 1u;
    } else if (expected_format == WE_TEX_FMT_RG88) {
        bpp = 2u;
    } else if (expected_format == WE_TEX_FMT_RGBA8888) {
        bpp = 4u;
    } else {
        return false;
    }

    uint64_t npx64 = (uint64_t)ctx->canvas_w * (uint64_t)ctx->canvas_h;
    if (npx64 == 0 || npx64 > (uint64_t)(SIZE_MAX / 4u)) {
        return false;
    }
    size_t npx = (size_t)npx64;
    size_t rgba_bytes = npx * 4u;
    size_t out_bytes = npx * bpp;

    uint8_t *dst = malloc(out_bytes);
    if (!dst) {
        return false;
    }

    brush_gl_state_t gl_st;
    save_gl_state(&gl_st);

    GLint prev_pack_align = 4;
    GLint prev_pbo = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_align);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->canvas_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           (GLuint)ctx->bound_tex->gl_tex, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        restore_gl_state(&gl_st);
        free(dst);
        return false;
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    uint32_t pbo_slot = ctx->readback_pbo_idx & 1u;
    ctx->readback_pbo_idx = (ctx->readback_pbo_idx + 1u) & 1u;
    if (ctx->readback_pbo[pbo_slot] == 0) {
        glGenBuffers(1, &ctx->readback_pbo[pbo_slot]);
    }

    const uint8_t *mapped = NULL;
    bool pbo_ready = false;
    if (ctx->readback_pbo[pbo_slot] != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->readback_pbo[pbo_slot]);
        if (ctx->readback_pbo_bytes[pbo_slot] < rgba_bytes) {
            while (glGetError() != GL_NO_ERROR) {
            }
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)rgba_bytes, NULL, GL_STREAM_READ);
            if (glGetError() == GL_NO_ERROR) {
                ctx->readback_pbo_bytes[pbo_slot] = rgba_bytes;
                pbo_ready = true;
            }
        } else {
            pbo_ready = true;
        }
        if (pbo_ready) {
            while (glGetError() != GL_NO_ERROR) {
            }
            glReadPixels(0, 0, (GLsizei)ctx->canvas_w, (GLsizei)ctx->canvas_h, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            if (glGetError() == GL_NO_ERROR) {
                if (ctx->egl_dpy != EGL_NO_DISPLAY && ctx->fn_eglCreateSyncKHR &&
                    ctx->fn_eglClientWaitSyncKHR && ctx->fn_eglDestroySyncKHR) {
                    EGLSyncKHR sync =
                        ctx->fn_eglCreateSyncKHR(ctx->egl_dpy, EGL_SYNC_FENCE_KHR, NULL);
                    if (sync != EGL_NO_SYNC_KHR) {
                        glFlush();
                        (void)ctx->fn_eglClientWaitSyncKHR(
                            ctx->egl_dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
                        ctx->fn_eglDestroySyncKHR(ctx->egl_dpy, sync);
                    }
                } else {
                    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    if (sync) {
                        glFlush();
                        (void)glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
                        glDeleteSync(sync);
                    }
                }
                mapped = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                           (GLsizeiptr)rgba_bytes, GL_MAP_READ_BIT);
            }
        }
    }

    uint8_t *fallback_rgba = NULL;
    const uint8_t *src_rgba = mapped;
    if (!src_rgba) {
        if (ctx->readback_pbo[pbo_slot] != 0) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        fallback_rgba = malloc(rgba_bytes);
        if (!fallback_rgba) {
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
            glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
            restore_gl_state(&gl_st);
            free(dst);
            return false;
        }
        while (glGetError() != GL_NO_ERROR) {
        }
        glReadPixels(0, 0, (GLsizei)ctx->canvas_w, (GLsizei)ctx->canvas_h, GL_RGBA,
                     GL_UNSIGNED_BYTE, fallback_rgba);
        if (glGetError() != GL_NO_ERROR) {
            free(fallback_rgba);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
            glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
            restore_gl_state(&gl_st);
            free(dst);
            return false;
        }
        src_rgba = fallback_rgba;
    }

    if (expected_format == WE_TEX_FMT_RGBA8888) {
        memcpy(dst, src_rgba, out_bytes);
    } else if (expected_format == WE_TEX_FMT_RG88) {
        for (size_t i = 0; i < npx; i++) {
            dst[i * 2u + 0u] = src_rgba[i * 4u + 0u];
            dst[i * 2u + 1u] = src_rgba[i * 4u + 1u];
        }
    } else {
        for (size_t i = 0; i < npx; i++) {
            dst[i] = src_rgba[i * 4u + 0u];
        }
    }

    if (mapped) {
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    free(fallback_rgba);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
    restore_gl_state(&gl_st);

    *out_pixels = dst;
    *out_byte_len = out_bytes;
    return true;
}

void we_gpu_brush_get_stats(const we_gpu_brush_ctx_t *ctx, we_brush_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx) {
        return;
    }

    out_stats->last_dab_gpu_us = ctx->last_dab_gpu_us;
    out_stats->active_pool_slots = ctx->next_fresh_slot - ctx->free_count;
    out_stats->max_pool_slots = ctx->pool_cap;
    out_stats->active_stroke_tiles = ctx->active_tile_count;
    out_stats->canvas_width = ctx->canvas_w;
    out_stats->canvas_height = ctx->canvas_h;
    if (ctx->bound_tex) {
        out_stats->target_gl_tex = ctx->bound_tex->gl_tex;
        out_stats->target_tex_format = (uint32_t)ctx->bound_tex->format;
    }
}
