// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Rain on Glass
// @description: Procedural refractive raindrops and condensation trails sliding down bokeh glass
// @author: WayWal
// @audio_reactive: false

const vec3 RG_BG_DARK = vec3(0.05, 0.08, 0.14);
const vec3 RG_BOKEH_AMBER = vec3(0.96, 0.62, 0.22);
const vec3 RG_BOKEH_CYAN = vec3(0.18, 0.72, 0.92);

vec3 rg_hash32(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}

vec2 rg_drop_layer(vec2 uv, float t)
{
    vec2 aspect = vec2(6.0, 1.0);
    vec2 st = uv * aspect;
    st.y += t * 0.28;
    vec2 id = floor(st);
    vec2 gv = fract(st) - 0.5;

    vec3 rnd = rg_hash32(id);
    float phase = t * 1.6 + rnd.z * 6.28318;
    float y = -sin(phase + sin(phase + sin(phase) * 0.5)) * 0.42;
    float x = (rnd.x - 0.5) * 0.55;
    float s_y = sin(uv.y);
    x += (0.35 - abs(x)) * sin(3.0 * uv.y) * (s_y * s_y) * 0.25;

    vec2 drop_pos = (gv - vec2(x, y)) / aspect;
    float drop = 1.0 - smoothstep(0.025, 0.055, length(drop_pos));

    vec2 trail_pos = (gv - vec2(x, t * 0.28)) / aspect;
    trail_pos.y = (fract(trail_pos.y * 8.0) - 0.5) / 8.0;
    float trail = 1.0 - smoothstep(0.01, 0.03, length(trail_pos));
    float trail_hi = max(y + 1e-4, 0.45);
    trail *= smoothstep(-0.05, 0.05, drop_pos.y) * (1.0 - smoothstep(y, trail_hi, gv.y));

    return drop_pos * drop * 1.4 + trail_pos * trail * 0.8;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    float t_wrap = mod(iTime, 3600.0);
    float t = t_wrap * 0.65;
    vec2 n = rg_drop_layer(uv * 1.35, t) + rg_drop_layer(uv * 2.4 + vec2(3.7, 1.9), t * 1.15);

    vec2 ref_uv = uv + n * 1.8;

    vec3 bg_dark = RG_BG_DARK;
    vec3 bokeh_amber = RG_BOKEH_AMBER;
    vec3 bokeh_cyan = RG_BOKEH_CYAN;

    if (length(u_palette_primary) > 0.05) {
        bokeh_amber = mix(bokeh_amber, u_palette_primary, 0.35);
        bokeh_cyan = mix(bokeh_cyan, u_palette_secondary, 0.35);
    }

    vec3 col = mix(bg_dark, vec3(0.09, 0.13, 0.22), clamp(ref_uv.y + 0.5, 0.0, 1.0));

    for (int i = 0; i < 18; ++i) {
        float fi = float(i);
        vec3 h = rg_hash32(vec2(fi, fi * 2.7));
        vec2 bpos = vec2((h.x - 0.5) * 1.6, (h.y - 0.5) * 0.85);
        bpos.x += sin(t_wrap * 0.2 + fi) * 0.08;
        float rad = mix(0.08, 0.22, h.z);
        float d = length(ref_uv - bpos);
        float rad_lo = rad * 0.35;
        float rad_hi = max(rad_lo + 1e-4, rad);
        float bokeh = 1.0 - smoothstep(rad_lo, rad_hi, d);
        vec3 bcol = mix(bokeh_amber, bokeh_cyan, fract(fi * 0.37));
        col += bcol * bokeh * 0.18;
    }

    float spec = clamp(length(n) * 9.0, 0.0, 1.0);
    col += vec3(0.85, 0.92, 1.0) * pow(max(spec, 1e-4), 2.2) * 0.35;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
