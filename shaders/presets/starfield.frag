// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Starfield
// @description: Multi-layer 3D parallax starfield with cosmic nebula clouds and warp speed
// @author: WayWal
// @audio_reactive: false

const vec3 SF_SPACE_BG = vec3(0.01, 0.02, 0.05);
const vec3 SF_NEB_BLUE = vec3(0.14, 0.42, 0.88);
const vec3 SF_NEB_ROSE = vec3(0.86, 0.24, 0.58);

vec2 sf_hash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

mat2 sf_rot(float a)
{
    float c = cos(a);
    float s = sin(a);
    return mat2(c, -s, s, c);
}

float sf_star_layer(vec2 uv, float flare, float t_twinkle)
{
    vec2 gv = fract(uv) - 0.5;
    vec2 id = floor(uv);
    float acc = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 n = sf_hash22(id + neighbor);
            vec2 offs = gv - neighbor - n + 0.5;
            float d2 = dot(offs, offs);
            if (d2 >= 0.7225) {
                continue;
            }
            float size = fract(n.x * 345.32);
            bool has_flare = (size > 0.6 && flare > 0.0);
            float ray_prod = abs(offs.x * offs.y * 2800.0);
            if (d2 > 0.16 && (!has_flare || ray_prod >= 1.0)) {
                continue;
            }
            float d = sqrt(d2);
            float star = 0.0018 / max(d, 0.001);
            if (has_flare && ray_prod < 1.0) {
                star += (1.0 - ray_prod) * flare * smoothstep(0.6, 1.0, size);
            }
            star *= 1.0 - smoothstep(0.15, 0.85, d);
            float twinkle = 0.6 + 0.4 * sin(t_twinkle + n.y * 62.8);
            acc += star * size * twinkle;
        }
    }
    return acc;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    vec2 parallax = mouse * 0.25 + u_workspace_offset * 0.12;

    vec3 space_bg = SF_SPACE_BG;
    vec3 neb_blue = SF_NEB_BLUE;
    vec3 neb_rose = SF_NEB_ROSE;

    if (length(u_palette_primary) > 0.05) {
        neb_blue = mix(neb_blue, u_palette_primary, 0.4);
        neb_rose = mix(neb_rose, u_palette_secondary, 0.4);
    }

    float warp = 1.0 + u_audio_envelope * 1.5 + u_audio_beat * 0.8;
    float t = mod(iTime * 0.05, 100.0) + (warp - 1.0) * 0.08;
    float t_twinkle = mod(iTime, 3600.0) * 2.5;
    float neb1 = 0.5 + 0.5 * sin(uv.x * 2.5 + cos(uv.y * 2.2 + t * 2.0));
    float neb2 = 0.5 + 0.5 * cos(uv.y * 3.1 - sin(uv.x * 2.8 - t * 1.5));
    vec3 col = space_bg + neb_blue * (neb1 * 0.14) + neb_rose * (neb1 * neb2 * 0.12);

    for (int i = 0; i < 4; ++i) {
        float fi = float(i) * 0.25;
        float depth = fract(fi + t);
        float scale = mix(16.0, 1.2, depth);
        float fade = depth * (1.0 - smoothstep(0.82, 1.0, depth));
        mat2 rot = sf_rot(float(i) * 0.785398 + t * 0.2);
        vec2 st_uv = (rot * uv) * scale + vec2(fi * 37.2, fi * 19.8) + parallax * (1.5 - depth);
        float s = sf_star_layer(st_uv, smoothstep(0.4, 0.9, depth), t_twinkle);
        vec3 s_col = mix(vec3(0.65, 0.82, 1.0), vec3(1.0, 0.82, 0.68), fract(fi * 3.7));
        col += s * fade * s_col;
    }

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
