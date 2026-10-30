// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Aurora Borealis
// @description: Northern Lights curtains over an arctic night sky with stars and audio reactivity
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_SKY_BOTTOM = vec3(0.03, 0.07, 0.16);
const vec3 PAL_AURORA_GREEN = vec3(0.10, 0.92, 0.58);
const vec3 PAL_AURORA_VIOLET = vec3(0.56, 0.20, 0.92);

float aurora_hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

const mat2 AURORA_ROT = mat2(0.93937271, -0.34289781, 0.34289781, 0.93937271);

float aurora_curtain(vec2 p, float t, float base_wave, int octaves)
{
    float val = 0.0;
    float amp = 0.55;
    float freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        p = AURORA_ROT * p;
        float w = abs(sin(p.x * freq * 2.2 + t * 0.45 + base_wave * 0.15 + waywal_noise2d(p * freq + t * 0.15) * 3.14159));
        float w_shaped = w / (0.35 + 0.65 * w);
        val += (1.0 - w_shaped) * amp;
        freq *= 1.85;
        amp *= 0.52;
    }
    return val;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;
    vec2 screen_uv = fragCoord / res;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    uv += mouse * 0.06 + u_workspace_offset * 0.03;

    vec3 sky_bottom = PAL_SKY_BOTTOM;
    vec3 aurora_green = PAL_AURORA_GREEN;
    vec3 aurora_violet = PAL_AURORA_VIOLET;

    if (length(u_palette_primary) > 0.05) {
        aurora_green = mix(aurora_green, u_palette_primary, 0.35);
        aurora_violet = mix(aurora_violet, u_palette_tertiary, 0.35);
    }

    vec3 sky_top = vec3(0.01, 0.02, 0.06);
    vec3 col = mix(sky_bottom, sky_top, clamp(screen_uv.y, 0.0, 1.0));

    vec2 star_cell = floor(uv * 110.0);
    vec2 star_gv = fract(uv * 110.0) - 0.5;
    float sh = aurora_hash21(star_cell);
    if (sh > 0.92 && uv.y > -0.25) {
        float d = length(star_gv - (vec2(fract(sh * 13.7), fract(sh * 29.3)) - 0.5) * 0.6);
        float twinkle = 0.55 + 0.45 * sin(mod(iTime, 3600.0) * 2.5 + sh * 60.0);
        col += vec3(0.85, 0.92, 1.0) * (1.0 - smoothstep(0.0, 0.08, d)) * twinkle * smoothstep(-0.2, 0.3, uv.y);
    }

    float audio_energy = clamp(u_audio_envelope * 0.6 + u_audio_beat * 0.4, 0.0, 1.0);
    float t = mod(iTime * 0.42, 628.31853) + audio_energy * 0.35;
    float base_wave = 0.25 * sin(uv.x * 3.0 + t);
    float v_env = smoothstep(-0.18, 0.15, uv.y) * (1.0 - smoothstep(0.05, 0.58, uv.y));
    vec3 acc = vec3(0.0);
    if (v_env > 1e-4) {
        float layer_weight = 0.1575 * (2.0 + audio_energy * 0.88);
        for (int i = 0; i < 8; i++) {
            float fi = float(i) * 0.125;
            float depth = mix(0.45, 2.4, fi);
            vec2 p = vec2(uv.x * depth * 1.4, (uv.y + 0.15) * depth + fi * 1.3);
            p.x += sin(t * 0.3 + fi * 4.0) * 0.35;
            int octs = (i < 4) ? 3 : 2;
            float c = aurora_curtain(p, t + fi * 1.7, base_wave, octs);
            float band = smoothstep(0.32, 0.95, c) * v_env;
            vec3 layer_col = mix(aurora_green, aurora_violet, clamp(fi + base_wave, 0.0, 1.0));
            acc += layer_col * band * (1.0 - fi * 0.55) * layer_weight;
        }
    }

    col += acc;

    float horizon_glow = exp(-abs(uv.y + 0.22) * 8.5) * (0.28 + u_audio_beat * 0.12);
    col += mix(aurora_green, vec3(0.15, 0.55, 0.85), 0.5) * horizon_glow;

    float mtn_mask = 1.0;
    if (uv.y < -0.165) {
        float mtn_h = -0.26 + 0.06 * waywal_noise2d(vec2(uv.x * 4.5 + 1.2, 0.0))
                            + 0.025 * waywal_noise2d(vec2(uv.x * 12.0, 1.0));
        mtn_mask = smoothstep(mtn_h - 0.004, mtn_h + 0.006, uv.y);
        vec3 mountain_col = vec3(0.015, 0.022, 0.045) + acc * 0.08;
        col = mix(mountain_col, col, mtn_mask);
    }

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
