// SPDX-License-Identifier: GPL-3.0-or-later
// Sample Shadertoy wallpaper for WayWal:
//   wwal shader contrib/shaders/plasma.frag --fps 60

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / max(iResolution.y, 1.0);
    vec2 mouse = (iMouse.xy / max(iResolution.xy, vec2(1.0))) - 0.5;
    float t = iTime * 0.45;

    vec2 p = uv * 2.6 + mouse * 0.6;
    for (int i = 1; i < 5; ++i) {
        float fi = float(i);
        p.x += 0.35 / fi * sin(fi * 2.5 * p.y + t + 0.3 * fi);
        p.y += 0.35 / fi * cos(fi * 2.5 * p.x + t + 0.5 * fi);
    }

    vec3 col = 0.5 + 0.5 * cos(t + vec3(p.x, p.y, p.x + p.y) + vec3(0.0, 2.1, 4.2));
    float vig = smoothstep(1.35, 0.25, length(uv));
    fragColor = vec4(col * vig, 1.0);
}
