// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_BRUSH_DAB_ACCUM_GLSL_HPP
#define WAYWAL_STUDIO_BRUSH_DAB_ACCUM_GLSL_HPP

#include <cstdint>
#include <string_view>

namespace waywal::studio {

struct alignas(16) BrushDabUbo {
    float p0_r0[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float p1_r1[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float brush_rgba[4]{1.0f, 1.0f, 1.0f, 1.0f};
    int32_t tile_origin_size[4]{0, 0, 0, 0};
    float flow_dir_pad[4]{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t op_mode{0u};
    uint32_t channel{0u};
    float hardness{0.65f};
    float opacity{1.0f};
    float target_value{1.0f};
    float falloff_power{1.5f};
    float pad0{0.0f};
    float pad1{0.0f};
};
static_assert(sizeof(BrushDabUbo) == 112, "BrushDabUbo must match std140 layout (112 bytes)");

// Embedded copy of brush_dab_accum.comp so meson.build needs no shader build step changes.
inline constexpr const char *kBrushDabAccumCompGlsl = R"GLSL(#version 310 es
precision highp float;
precision highp int;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(rgba8, binding = 0) uniform readonly highp image2D u_canvas_src;
layout(rgba8, binding = 1) uniform writeonly highp image2D u_canvas_dst;

layout(std140, binding = 2) uniform BrushDabUbo {
    vec4  p0_r0;
    vec4  p1_r1;
    vec4  brush_rgba;
    ivec4 tile_origin_size;
    vec4  flow_dir_pad;
    uint  op_mode;
    uint  channel;
    float hardness;
    float opacity;
    float target_value;
    float falloff_power;
    float pad0;
    float pad1;
};

float uneven_capsule_sdf(vec2 p, vec2 a, vec2 b, float ra, float rb) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float l2 = dot(ba, ba);
    float rr = ra - rb;
    /* Short-circuit when one endpoint encloses the other so degenerate cones never produce NaN. */
    if (l2 < 1e-6 || l2 <= rr * rr) {
        return (ra >= rb) ? (length(pa) - ra) : (length(p - b) - rb);
    }
    float a2 = max(l2 - rr * rr, 1e-6);
    float il2 = 1.0 / l2;
    float y = dot(pa, ba);
    float z = y - l2;
    vec2 d = pa * l2 - ba * y;
    float x2 = dot(d, d);
    float y2 = y * y * l2;
    float z2 = z * z * l2;
    float k = sign(rr) * rr * rr * x2;
    if (sign(z) * a2 * z2 > k) {
        return sqrt(x2 + z2) * il2 - rb;
    }
    if (sign(y) * a2 * y2 < k) {
        return sqrt(x2 + y2) * il2 - ra;
    }
    return (sqrt(x2 * a2 * il2) + y * rr) * il2 - ra;
}

void main() {
    ivec2 local_xy = ivec2(gl_GlobalInvocationID.xy);
    if (local_xy.x >= tile_origin_size.z || local_xy.y >= tile_origin_size.w) {
        return;
    }
    ivec2 pix = tile_origin_size.xy + local_xy;
    vec2 sample_pos = vec2(pix) + vec2(0.5);
    float dist = uneven_capsule_sdf(sample_pos, p0_r0.xy, p1_r1.xy, p0_r0.z, p1_r1.z);
    float feather = max((1.0 - clamp(hardness, 0.0, 0.999)) * max(p0_r0.z, p1_r1.z), 0.75);
    float mask = 1.0 - smoothstep(-feather, 0.0, dist);
    /* Guard mask > 0 before pow() because some GLES drivers evaluate pow(0.0, y) via log2 as NaN. */
    float shaped = (mask > 0.0) ? pow(clamp(mask, 0.0, 1.0), clamp(falloff_power, 0.25, 4.0)) : 0.0;
    float w = clamp(shaped * opacity, 0.0, 1.0);
    vec4 prev = imageLoad(u_canvas_src, pix);
    vec4 out_px = prev;

    if (w > 0.0) {
        if (channel == 1u) {
            /* Encode neutral flow rest at exact 128/255 rather than 0.5 to avoid 127/128 rounding drift. */
            const vec2 kFlowNeutral = vec2(128.0 / 255.0);
            const float kFlowScale = 127.0 / 255.0;
            vec2 dst_v = (prev.rg - kFlowNeutral) / kFlowScale;
            vec2 src_v = flow_dir_pad.xy;
            vec2 res_v = dst_v;
            if (op_mode == 4u) {
                res_v = mix(dst_v, src_v, w);
            } else if (op_mode == 5u) {
                res_v = mix(dst_v, vec2(0.0), w);
            } else if (op_mode == 6u) {
                vec2 combed = dst_v + src_v * w;
                float len_v = length(combed);
                res_v = (len_v > 1.0) ? (combed / len_v) : combed;
            }
            out_px = vec4(clamp(kFlowNeutral + res_v * kFlowScale, 0.0, 1.0), prev.b, 1.0);
        } else if (channel == 0u) {
            float d = prev.r;
            float res = d;
            if (op_mode == 0u) {
                res = d + (clamp(target_value, 0.0, 1.0) - d) * w;
            } else if (op_mode == 1u) {
                res = max(d, clamp(target_value, 0.0, 1.0) * w);
            } else if (op_mode == 2u) {
                res = clamp(d - w, 0.0, 1.0);
            } else if (op_mode == 3u) {
                res = d * (1.0 - w);
            }
            out_px = vec4(clamp(res, 0.0, 1.0), prev.g, prev.b, prev.a);
        } else {
            float ca = clamp(w * brush_rgba.a, 0.0, 1.0);
            if (op_mode == 0u) {
                float out_a = clamp(prev.a + ca * (1.0 - prev.a), 0.0, 1.0);
                vec3 out_rgb = (out_a > 1e-6)
                                   ? (prev.rgb * prev.a * (1.0 - ca) + brush_rgba.rgb * ca) / out_a
                                   : vec3(0.0);
                out_px = vec4(clamp(out_rgb, 0.0, 1.0), out_a);
            } else if (op_mode == 1u) {
                out_px = max(prev, vec4(brush_rgba.rgb, 1.0) * ca);
            } else if (op_mode == 2u) {
                out_px = clamp(prev - brush_rgba * w, 0.0, 1.0);
            } else if (op_mode == 3u) {
                out_px = vec4(prev.rgb, clamp(prev.a * (1.0 - w), 0.0, 1.0));
            } else {
                out_px = vec4(mix(prev.rgb, brush_rgba.rgb, ca), max(prev.a, ca));
            }
        }
    }
    imageStore(u_canvas_dst, pix, out_px);
}
)GLSL";

inline constexpr std::string_view kBrushDabAccumCompSource{kBrushDabAccumCompGlsl};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_BRUSH_DAB_ACCUM_GLSL_HPP
