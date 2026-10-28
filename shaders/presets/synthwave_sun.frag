// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Synthwave Sun
// @description: Segmented retrowave sunset with ocean reflections and palm silhouettes
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_SW_SUN_TOP = vec3(1.00, 0.84, 0.18);
const vec3 PAL_SW_SUN_BOT = vec3(0.98, 0.12, 0.54);
const vec3 PAL_SW_SKY_VIOLET = vec3(0.14, 0.04, 0.28);

float sw_hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float sw_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(sw_hash(i), sw_hash(i + vec2(1.0, 0.0)), f.x),
               mix(sw_hash(i + vec2(0.0, 1.0)), sw_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float sw_palm(vec2 uv, vec2 base, float scale, float lean)
{
    vec2 p = (uv - base) / scale;
    if (p.y < 0.0 || p.y > 1.35) {
        return 0.0;
    }
    float trunk_x = lean * p.y * p.y;
    float trunk_w = mix(0.028, 0.008, clamp(p.y, 0.0, 1.0));
    float trunk_lo = trunk_w * 0.5;
    float trunk_hi = max(trunk_lo + 1e-4, trunk_w);
    float trunk = step(p.y, 1.0) * (1.0 - smoothstep(trunk_lo, trunk_hi, abs(p.x - trunk_x)));

    vec2 crown = p - vec2(trunk_x, 1.0);
    float r = length(crown);
    float fronds = 0.0;
    if (r < 0.38 && r > 1e-4) {
        float ang = atan(crown.y, crown.x);
        float leaf = cos(ang * 7.0 + sin(r * 10.0 - iTime * 1.2) * 0.4);
        float arch = (1.0 - smoothstep(0.05, 0.36, r)) * smoothstep(-0.7, 0.2, crown.y + r * 0.6);
        fronds = smoothstep(0.35, 0.75, leaf) * arch;
    }
    return clamp(trunk + fronds, 0.0, 1.0);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    uv.x += mouse.x * 0.06 + u_workspace_offset.x * 0.03;

    vec3 sun_top = PAL_SW_SUN_TOP;
    vec3 sun_bot = PAL_SW_SUN_BOT;
    vec3 sky_violet = PAL_SW_SKY_VIOLET;

    if (length(u_palette_primary) > 0.05) {
        sun_bot = mix(sun_bot, u_palette_primary, 0.35);
        sky_violet = mix(sky_violet, u_palette_tertiary * 0.4, 0.35);
    }

    vec3 col = mix(vec3(0.45, 0.08, 0.38), sky_violet, clamp(uv.y * 1.8 + 0.2, 0.0, 1.0));

    vec2 sun_p = uv - vec2(0.0, 0.10);
    float sun_r = length(sun_p);
    float sun_mask = 1.0 - smoothstep(0.278, 0.285, sun_r);

    float stripe_phase = (sun_p.y + 0.28) * 28.0 - iTime * 0.8;
    float stripe_cut = 1.0 - smoothstep(-0.22, 0.05, sun_p.y);
    float stripe_bar = step(stripe_cut * 0.55, fract(stripe_phase));
    sun_mask *= mix(1.0, stripe_bar, 1.0 - smoothstep(-0.18, 0.08, sun_p.y));

    vec3 sun_col = mix(sun_bot, sun_top, clamp((sun_p.y + 0.28) / 0.56, 0.0, 1.0));
    col = mix(col, sun_col, sun_mask);
    col += sun_bot * exp(-sun_r * 5.0) * 0.48;

    float m_ridge = -0.04 + (0.09 * sw_noise(vec2(uv.x * 4.5 + 2.0, 1.0)) +
                             0.04 * sw_noise(vec2(uv.x * 11.0, 3.0))) *
                            smoothstep(0.12, 0.48, abs(uv.x));
    if (uv.y > -0.05 && uv.y < m_ridge) {
        float rim = smoothstep(m_ridge - 0.012, m_ridge, uv.y);
        col = mix(vec3(0.04, 0.01, 0.09), sun_bot, rim * 0.75);
    }

    if (uv.y < -0.05) {
        float dy = -0.05 - uv.y;
        float z = 0.25 / max(dy, 0.002);
        float gx = uv.x * z * 2.0;
        float gz = z + iTime * 2.4;

        vec2 g = abs(fract(vec2(gx, gz) - 0.5) - 0.5);
        float line = 1.0 - smoothstep(0.0, 0.06, min(g.x, g.y));
        float fog = exp(-z * 0.09);

        vec3 sea = vec3(0.05, 0.02, 0.12);
        float refl = exp(-abs(uv.x) * 4.8) * (0.6 + 0.4 * sin(z * 3.0 - iTime * 5.0));
        sea += mix(sun_bot, sun_top, 0.4) * refl * 0.55;
        sea += vec3(0.12, 0.88, 0.96) * line * fog * 0.75;
        col = sea;
    }

    float palm_l = sw_palm(uv, vec2(-0.52, -0.06), 0.34, 0.24);
    float palm_r = sw_palm(uv, vec2(0.54, -0.06), 0.30, -0.22);
    col = mix(col, vec3(0.02, 0.005, 0.05), clamp(palm_l + palm_r, 0.0, 1.0));

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
