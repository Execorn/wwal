// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Fluid Ink
// @description: Domain-warped FBM liquid marble ink currents with mouse ripple and audio reactivity
// @author: WayWal
// @audio_reactive: false

const vec3 PAL_INK_BASE = vec3(0.04, 0.06, 0.12);
const vec3 PAL_INK_TEAL = vec3(0.12, 0.64, 0.78);
const vec3 PAL_INK_GOLD = vec3(0.92, 0.48, 0.28);

float fi_hash(vec2 p)
{
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float fi_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(fi_hash(i + vec2(0.0, 0.0)), fi_hash(i + vec2(1.0, 0.0)), u.x),
               mix(fi_hash(i + vec2(0.0, 1.0)), fi_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fi_fbm(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.80, -0.60, 0.60, 0.80);
    for (int i = 0; i < 5; ++i) {
        v += a * fi_noise(p);
        p = rot * p * 2.02 + vec2(1.7, 9.2);
        a *= 0.5;
    }
    return v;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = (fragCoord - 0.5 * res) / res.y;
    vec2 mouse = (u_mouse_smooth.xy - 0.5 * res) / res.y;

    float m_dist = length(uv - mouse);
    vec2 m_dir = (m_dist > 1e-4) ? ((uv - mouse) / m_dist) : vec2(0.0);
    float m_ripple = sin(m_dist * 24.0 - iTime * 4.5) * exp(-m_dist * 5.5) * 0.08;

    vec2 p = (uv + m_dir * m_ripple + u_workspace_offset * 0.08) * 2.4;

    float audio_flow = clamp(u_audio_envelope * 0.5 + u_audio_beat * 0.4, 0.0, 1.0);
    float t = iTime * (0.18 + audio_flow * 0.06);

    vec2 q = vec2(fi_fbm(p + vec2(0.0, 0.0) + t * 0.4),
                  fi_fbm(p + vec2(5.2, 1.3) - t * 0.3));

    vec2 r = vec2(fi_fbm(p + 3.8 * q + vec2(1.7, 9.2) + 0.15 * t),
                  fi_fbm(p + 3.8 * q + vec2(8.3, 2.8) + 0.126 * t));

    float f = fi_fbm(p + (3.5 + audio_flow * 0.6) * r);

    vec3 ink_base = PAL_INK_BASE;
    vec3 ink_teal = PAL_INK_TEAL;
    vec3 ink_gold = PAL_INK_GOLD;

    if (length(u_palette_primary) > 0.05) {
        ink_teal = mix(ink_teal, u_palette_primary, 0.45);
        ink_gold = mix(ink_gold, u_palette_secondary, 0.45);
    }

    vec3 col = mix(ink_base, ink_teal, clamp(f * f * 2.2, 0.0, 1.0));
    col = mix(col, vec3(0.08, 0.16, 0.36), clamp(length(q), 0.0, 1.0));
    col = mix(col, ink_gold, clamp(length(r.x) * 0.85, 0.0, 1.0));

    float vein = smoothstep(0.48, 0.52, f) - smoothstep(0.52, 0.56, f);
    col += ink_gold * vein * (0.55 + u_audio_beat * 0.35);
    col *= (0.35 + 0.85 * f * f + 0.45 * f);

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
