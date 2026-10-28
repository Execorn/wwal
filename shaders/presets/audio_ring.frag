// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Audio Ring
// @description: Polar circular audio spectrum with reactive bass core and expanding shockwave rings
// @author: WayWal
// @audio_reactive: true

const vec3 PAL_AR_BG = vec3(0.03, 0.03, 0.07);
const vec3 PAL_AR_CYAN = vec3(0.14, 0.86, 0.98);
const vec3 PAL_AR_MAGENTA = vec3(0.94, 0.22, 0.72);

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 p = (fragCoord - 0.5 * res) / res.y;

    vec2 mouse = (u_mouse_smooth.xy / res) - 0.5;
    p -= mouse * 0.08;

    vec3 bg_dark = PAL_AR_BG;
    vec3 ring_cyan = PAL_AR_CYAN;
    vec3 ring_magenta = PAL_AR_MAGENTA;

    if (length(u_palette_primary) > 0.05) {
        ring_cyan = mix(ring_cyan, u_palette_primary, 0.45);
        ring_magenta = mix(ring_magenta, u_palette_secondary, 0.45);
    }

    float r = length(p);
    float ang = (r > 1e-5) ? atan(p.y, p.x) : 0.0;
    float norm_ang = abs(ang) / 3.14159265;

    int bin = clamp(int(norm_ang * 63.0), 0, 63);
    float tex_fft = texture(iChannel0, vec2(norm_ang, 0.25)).r;
    float band = max(u_audio_bands[bin], tex_fft);

    float idle_wave = 0.12 + 0.07 * sin(norm_ang * 18.8495 + iTime * 2.4)
                           + 0.04 * cos(norm_ang * 37.6991 - iTime * 3.6);
    float idle_pulse = 0.03 * sin(iTime * 2.0);

    float has_audio = smoothstep(0.005, 0.04, u_audio_envelope + tex_fft);
    float amp = mix(idle_wave, band * 0.26, has_audio);
    float base_r = 0.22 + mix(idle_pulse, u_audio_beat * 0.045 + u_audio_envelope * 0.04, has_audio);

    float target_r = base_r + amp;
    float dist = abs(r - target_r);

    float core_line = 0.0035 / max(dist, 0.0015);
    float fill_lo = base_r * 0.85;
    float fill_hi = max(fill_lo + 1e-4, target_r);
    float inner_lo = base_r * 0.78;
    float inner_hi = max(inner_lo + 1e-4, base_r);
    float inner_fill = (1.0 - smoothstep(fill_lo, fill_hi, r)) * smoothstep(inner_lo, inner_hi, r);

    float bars_ang = fract(norm_ang * 64.0);
    float spoke_mask = 1.0 - smoothstep(0.55, 0.85, abs(bars_ang - 0.5) * 2.0);
    float radial_bar = smoothstep(base_r, base_r + 0.005, r) *
                       (1.0 - smoothstep(target_r - 0.004, target_r + 0.005, r)) * spoke_mask;

    float shock_phase = fract(iTime * 0.45);
    float shock_r = base_r + shock_phase * 0.55;
    float shockwave = exp(-abs(r - shock_r) * 55.0) * (1.0 - shock_phase) * (0.35 + u_audio_beat * 0.65);

    vec3 ring_col = mix(ring_cyan, ring_magenta, norm_ang);
    vec3 col = bg_dark + ring_col * (core_line * 0.45 + radial_bar * 0.85 + inner_fill * 0.25 + shockwave);
    col += ring_magenta * exp(-r * 6.5) * (0.18 + u_audio_beat * 0.25);

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
