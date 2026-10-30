// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Cyber Grid
// @description: Outrun perspective neon grid with wireframe terrain, sun, and audio reactivity
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_CG_BG = vec3(0.03, 0.02, 0.08);
const vec3 PAL_CG_CYAN = vec3(0.05, 0.88, 0.98);
const vec3 PAL_CG_MAGENTA = vec3(0.96, 0.14, 0.68);

float cg_hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    uv.x += mouse.x * 0.08 + u_workspace_offset.x * 0.04;

    vec3 col_bg = PAL_CG_BG;
    vec3 col_cyan = PAL_CG_CYAN;
    vec3 col_magenta = PAL_CG_MAGENTA;

    if (length(u_palette_primary) > 0.05) {
        col_cyan = mix(col_cyan, u_palette_primary, 0.35);
        col_magenta = mix(col_magenta, u_palette_secondary, 0.35);
    }

    float audio_boost = clamp(u_audio_envelope * 0.5 + u_audio_beat * 0.5, 0.0, 1.0);
    float horizon = -0.02 + mouse.y * 0.05;
    float dy = uv.y - horizon;
    vec3 col = col_bg;

    if (dy < 0.0) {
        float z = 0.32 / max(-dy, 0.002);
        float x = uv.x * z * 2.2;
        float scroll = mod(iTime * 2.4, 359.03916) + u_audio_envelope * 0.35;

        vec2 g = vec2(x, z + scroll);
        float terrain = waywal_noise2d(g * vec2(0.35, 0.18)) * smoothstep(0.8, 3.2, abs(x));
        g.y += terrain * 0.45;
        vec2 fw = fwidth(g);

        vec2 grid_d = abs(fract(g - 0.5) - 0.5);
        vec2 grid_dist = grid_d / max(fw, vec2(0.001));
        float line = 1.0 - min(min(grid_dist.x, grid_dist.y), 1.0);
        float glow = exp(-14.0 * min(grid_d.x, grid_d.y));

        float pulse = smoothstep(0.92, 1.0, sin(g.y * 0.35 - mod(iTime * 4.0, 628.31853) + floor(g.x) * 1.7));
        float fog = exp(-z * 0.075);

        vec3 grid_col = mix(col_cyan, col_magenta, clamp(z * 0.045 + terrain * 0.4, 0.0, 1.0));
        col += grid_col * (line * 0.95 + glow * (0.45 + audio_boost * 0.35) + pulse * 0.65) * fog;
        col += col_magenta * exp(dy * 9.0) * (0.42 + audio_boost * 0.2);
    } else {
        float sky_grad = clamp(dy * 1.8, 0.0, 1.0);
        col = mix(vec3(0.14, 0.03, 0.24), col_bg, sky_grad);

        vec2 sun_uv = uv - vec2(0.0, horizon + 0.14);
        float sun_d = length(sun_uv);
        if (sun_d < 0.18) {
            float scan = step(0.25, fract(sun_uv.y * 45.0 - fract(iTime * 0.6)));
            vec3 sun_c = mix(col_magenta, vec3(1.0, 0.78, 0.22), clamp((sun_uv.y + 0.18) / 0.36, 0.0, 1.0));
            col = mix(col, sun_c, (1.0 - smoothstep(0.172, 0.18, sun_d)) * mix(1.0, scan, 1.0 - smoothstep(-0.14, 0.04, sun_uv.y)));
        }
        col += col_magenta * exp(-sun_d * 6.5) * 0.35;

        if (dy < 0.17) {
            float city_x = floor(uv.x * 38.0);
            float b_height = 0.03 + 0.14 * cg_hash(vec2(city_x, 4.0)) * (1.0 - smoothstep(0.1, 0.9, abs(uv.x)));
            if (dy < b_height) {
                vec3 b_col = vec3(0.02, 0.01, 0.05);
                vec2 win_uv = fract(vec2(uv.x * 38.0, dy * 65.0));
                float win_on = step(0.62, cg_hash(vec2(city_x, floor(dy * 65.0))));
                float win_mask = step(0.2, win_uv.x) * step(win_uv.x, 0.8) * step(0.25, win_uv.y) * step(win_uv.y, 0.75);
                b_col += mix(col_cyan, col_magenta, cg_hash(vec2(city_x, 9.0))) * win_on * win_mask * 0.55;
                col = b_col;
            }
        }
    }

    float h_beam = exp(-abs(dy) * 55.0);
    col += mix(col_magenta, vec3(1.0, 0.85, 0.95), 0.5) * h_beam * (0.85 + u_audio_beat * 0.25);

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
