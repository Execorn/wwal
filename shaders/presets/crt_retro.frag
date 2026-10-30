// SPDX-License-Identifier: GPL-3.0-or-later
// @name: CRT Retro
// @description: Barrel-curved CRT phosphor terminal with radar scope, scanlines, and RGB aberration
// @author: WayWal
// @audio_reactive: false
// #pragma waywal image.channel0 = wallpaper

const vec3 PAL_BG = vec3(0.06, 0.12, 0.20);
const vec3 PAL_PHOSPHOR = vec3(0.16, 0.88, 0.64);
const vec3 PAL_AMBER = vec3(0.92, 0.36, 0.24);

vec2 crt_curve(vec2 uv)
{
    uv = uv * 2.0 - 1.0;
    vec2 offset = abs(uv.yx) / vec2(5.5, 4.2);
    uv = uv + uv * offset * offset;
    return uv * 0.5 + 0.5;
}

vec3 crt_pattern_ca(vec2 uv, float ca, float t, float t_scroll, float t_sweep)
{
    vec3 c1 = PAL_BG;
    vec3 c2 = PAL_PHOSPHOR;
    vec3 c3 = PAL_AMBER;

    if (length(u_palette_primary) > 0.05) {
        c2 = mix(c2, u_palette_primary, 0.4);
        c3 = mix(c3, u_palette_secondary, 0.4);
    }

    vec3 ux = clamp(vec3(uv.x + ca, uv.x, uv.x - ca), 0.0, 1.0);
    vec3 gx = abs(fract(ux * 16.0) - 0.5);
    float gy = abs(fract(uv.y * 9.0 - t_scroll) - 0.5);
    vec3 grid = 1.0 - smoothstep(0.01, 0.04, min(gx, vec3(gy)));

    float wave = sin(uv.x * 10.0 + t * 2.2) * 0.12 + 0.5;
    float beam = exp(-abs(uv.y - wave) * 28.0);

    vec2 p = (uv - 0.5) * vec2(1.777, 1.0);
    float r2 = dot(p, p);
    float radar = 0.0;
    if (r2 < 0.135) {
        float r = sqrt(r2);
        float ang = atan(p.y, (r2 > 1e-10) ? p.x : 1.0);
        float sweep = fract((ang * (1.0 / 6.2831853)) - t_sweep);
        radar = exp(-sweep * 5.0) * (1.0 - smoothstep(0.34, 0.36, r)) +
                (1.0 - smoothstep(0.0, 0.008, abs(r - 0.35))) +
                (1.0 - smoothstep(0.0, 0.006, abs(r - 0.18)));
    }

    return c1 + c2 * (grid * 0.40 + radar * 0.45) + c3 * (beam * 0.70);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 raw_uv = fragCoord / res;
    vec2 uv = crt_curve(raw_uv);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.01, 0.01, 0.015, 1.0);
        return;
    }

    bool has_wp = (iChannelResolution[0].x > 4.0 && iChannelResolution[0].y > 4.0);
    float t = mod(iTime, 3600.0);
    float t_scroll = fract(iTime * 0.08);
    float t_sweep = fract(iTime * 0.28);
    float ca = 0.0022 + 0.0008 * sin(t * 3.0);

    vec3 col;
    if (has_wp) {
        vec3 ux = clamp(vec3(uv.x + ca, uv.x, uv.x - ca), 0.0, 1.0);
        float uy = 1.0 - uv.y;
        col.r = texture(iChannel0, vec2(ux.x, uy)).r;
        col.g = texture(iChannel0, vec2(ux.y, uy)).g;
        col.b = texture(iChannel0, vec2(ux.z, uy)).b;
    } else {
        col = crt_pattern_ca(uv, ca, t, t_scroll, t_sweep);
    }

    float scanline = 0.82 + 0.18 * sin(uv.y * res.y * 1.5 - t * 8.0);
    float grille = 0.90 + 0.10 * cos(fragCoord.x * 3.14159);
    float vig = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
    vig = clamp(pow(max(vig, 1e-4), 0.22), 0.0, 1.0);

    col *= scanline * grille * vig;
    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
