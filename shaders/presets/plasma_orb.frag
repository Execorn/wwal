// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Plasma Orb
// @description: Raymarched volumetric energy orb with internal plasma filaments and mouse tracking
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_BG = vec3(0.02, 0.03, 0.07);
const vec3 PAL_CORE = vec3(0.20, 0.78, 1.00);
const vec3 PAL_ARC = vec3(0.96, 0.22, 0.72);

mat2 po_rot(float a)
{
    float c = cos(a);
    float s = sin(a);
    return mat2(c, -s, s, c);
}

float po_gyroid(vec3 p)
{
    return dot(sin(p), cos(p.yzx));
}

float po_density_fast(vec3 p, float r, float t)
{
    float g1 = abs(po_gyroid(p * 3.6 + t * 1.2));
    float g2 = abs(po_gyroid(p * 7.2 - t * 1.8));
    float filaments = 0.06 / max(g1 * g2 + 0.015, 0.015);
    float dr = (r - 0.92) * 14.0;
    float dr2 = dr * dr;
    float shell = 1.4 / (1.0 + dr2 + 0.45 * dr2 * dr2);
    return (filaments * 0.25 + shell) * (1.0 - smoothstep(0.2, 1.05, r));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    vec3 ro = vec3(0.0, 0.0, -2.55);
    vec3 rd = normalize(vec3(uv - mouse * 0.14, 1.35));

    vec3 col_bg = PAL_BG;
    vec3 col_core = PAL_CORE;
    vec3 col_arc = PAL_ARC;

    if (length(u_palette_primary) > 0.05) {
        col_core = mix(col_core, u_palette_primary, 0.45);
        col_arc = mix(col_arc, u_palette_secondary, 0.45);
    }

    vec3 col = col_bg + col_core * exp(-length(uv) * 3.2) * 0.15;

    float b = dot(ro, rd);
    float c = dot(ro, ro) - 1.1025;
    float h = b * b - c;

    if (h > 0.0) {
        float t_anim = mod(iTime, 3600.0) + u_audio_envelope * 0.4;
        mat2 rot_xz = po_rot(t_anim * 0.45);
        mat2 rot_xy = po_rot(t_anim * 0.30);

        vec3 ro_rot = ro;
        ro_rot.xz = rot_xz * ro_rot.xz;
        ro_rot.xy = rot_xy * ro_rot.xy;

        vec3 rd_rot = rd;
        rd_rot.xz = rot_xz * rd_rot.xz;
        rd_rot.xy = rot_xy * rd_rot.xy;

        float sq = sqrt(h);
        float t0 = max(-b - sq, 0.0);
        float t1 = -b + sq;
        float step_sz = (t1 - t0) * (1.0 / 16.0);
        float step_weight = step_sz * (0.52 + u_audio_beat * 0.15);
        vec3 step_vec = rd_rot * step_sz;
        vec3 pos = ro_rot + rd_rot * (t0 + 0.5 * step_sz);

        float w_core = 0.0;
        float w_arc = 0.0;

        for (int i = 0; i < 16; ++i) {
            float r2 = dot(pos, pos);
            if (r2 < 1.1025) {
                float r = sqrt(r2);
                float d = po_density_fast(pos, r, t_anim) * step_weight;
                float core_glow = (0.015 / (0.04 + r2)) * step_weight;
                float arc_mix = clamp(r * 0.9, 0.0, 1.0);
                w_core += d * (1.0 - arc_mix) + core_glow;
                w_arc += d * arc_mix;
                if (w_core + w_arc > 2.2) {
                    break;
                }
            }
            pos += step_vec;
        }
        col += w_core * col_core + w_arc * col_arc;
    }

    float halo = exp(-max(length(uv - mouse * 0.14) - 0.42, 0.0) * 6.5);
    col += mix(col_arc, col_core, 0.5) * halo * 0.25;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
