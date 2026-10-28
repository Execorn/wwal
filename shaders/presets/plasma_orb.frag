// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Plasma Orb
// @description: Raymarched volumetric energy orb with internal plasma filaments and mouse tracking
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_PO_BG = vec3(0.02, 0.03, 0.07);
const vec3 PAL_PO_CORE = vec3(0.20, 0.78, 1.00);
const vec3 PAL_PO_ARC = vec3(0.96, 0.22, 0.72);

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

float po_density(vec3 p, float t)
{
    p.xz = po_rot(t * 0.45) * p.xz;
    p.xy = po_rot(t * 0.30) * p.xy;
    float g1 = abs(po_gyroid(p * 3.6 + t * 1.2));
    float g2 = abs(po_gyroid(p * 7.2 - t * 1.8));
    float filaments = 0.06 / max(g1 * g2 + 0.015, 0.015);
    float shell = exp(-abs(length(p) - 0.92) * 14.0) * 1.4;
    return (filaments * 0.25 + shell) * (1.0 - smoothstep(0.2, 1.05, length(p)));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    vec3 ro = vec3(0.0, 0.0, -2.55);
    vec3 rd = normalize(vec3(uv - mouse * 0.14, 1.35));

    vec3 col_bg = PAL_PO_BG;
    vec3 col_core = PAL_PO_CORE;
    vec3 col_arc = PAL_PO_ARC;

    if (length(u_palette_primary) > 0.05) {
        col_core = mix(col_core, u_palette_primary, 0.45);
        col_arc = mix(col_arc, u_palette_secondary, 0.45);
    }

    vec3 col = col_bg + col_core * exp(-length(uv) * 3.2) * 0.15;

    float b = dot(ro, rd);
    float c = dot(ro, ro) - 1.15;
    float h = b * b - c;

    if (h > 0.0) {
        float sq = sqrt(h);
        float t0 = max(-b - sq, 0.0);
        float t1 = -b + sq;
        float step_sz = (t1 - t0) / 24.0;
        float t_march = t0;
        vec3 acc = vec3(0.0);

        for (int i = 0; i < 24; ++i) {
            vec3 pos = ro + rd * (t_march + 0.5 * step_sz);
            float d = po_density(pos, iTime + u_audio_envelope * 0.4);
            float r = length(pos);
            vec3 sample_col = mix(col_core, col_arc, clamp(r * 0.9, 0.0, 1.0));
            acc += sample_col * d * step_sz * (0.52 + u_audio_beat * 0.15);
            t_march += step_sz;
        }
        col += acc;
    }

    float halo = exp(-max(length(uv - mouse * 0.14) - 0.42, 0.0) * 6.5);
    col += mix(col_arc, col_core, 0.5) * halo * 0.25;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
