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

    vec2 safe_mouse = (isnan(u_mouse_smooth.x) || isnan(u_mouse_smooth.y) ||
                       isinf(u_mouse_smooth.x) || isinf(u_mouse_smooth.y))
                          ? (0.5 * res)
                          : u_mouse_smooth.xy;
    vec2 mouse = (safe_mouse / res) - 0.5;
    p -= mouse * 0.08;

    vec3 bg_dark = PAL_AR_BG;
    vec3 ring_cyan = PAL_AR_CYAN;
    vec3 ring_magenta = PAL_AR_MAGENTA;

    if (length(u_palette_primary) > 0.05) {
        ring_cyan = mix(ring_cyan, u_palette_primary, 0.45);
        ring_magenta = mix(ring_magenta, u_palette_secondary, 0.45);
    }

    float r = length(p);
    float safe_beat =
        (isnan(u_audio_beat) || isinf(u_audio_beat)) ? 0.0 : clamp(u_audio_beat, 0.0, 1.0);
    vec3 col = bg_dark;

    if (r >= 0.12 && r <= 0.72) {
        float safe_env =
            (isnan(u_audio_envelope) || isinf(u_audio_envelope)) ? 0.0 : clamp(u_audio_envelope, 0.0, 1.0);
        float ang = atan(p.y, (r > 1e-5) ? p.x : 1.0);
        float norm_ang = clamp((ang + 3.14159265) * (1.0 / 6.28318530), 0.0, 0.999999);

        float band_pos = norm_ang * 64.0;
        int bin = clamp(int(band_pos), 0, 63);
        float spoke = abs(fract(band_pos) - 0.5);

        float log_u = pow(norm_ang, 1.8) * 0.85 + 0.01;
        float raw_tex = textureLod(iChannel0, vec2(log_u, 0.25), 0.0).r;
        float tex_fft = (isnan(raw_tex) || isinf(raw_tex)) ? 0.0 : clamp(raw_tex, 0.0, 1.0);
        float raw_band = u_audio_bands[bin];
        float safe_band = (isnan(raw_band) || isinf(raw_band)) ? 0.0 : clamp(raw_band, 0.0, 1.0);
        float band = max(safe_band, tex_fft);

        float idle_wave = 0.12 + 0.07 * sin(norm_ang * 18.8495 + iTime * 2.4)
                               + 0.04 * cos(norm_ang * 37.6991 - iTime * 3.6);
        float idle_pulse = 0.03 * sin(iTime * 2.0);

        float has_audio = smoothstep(0.01, 0.08, safe_env + tex_fft * 0.5);
        float live_amp = max(band * 0.28, idle_wave * 0.35) + safe_beat * 0.03;
        float amp = max(mix(idle_wave, live_amp, has_audio), 0.02);
        float base_r = 0.22 + mix(idle_pulse, safe_beat * 0.045 + safe_env * 0.04, has_audio);

        float target_r = base_r + amp;
        float dist = abs(r - target_r);

        float core_line = 0.0035 / max(dist, 0.0015);
        float fill_lo = base_r * 0.85;
        float fill_hi = max(fill_lo + 1e-4, target_r);
        float inner_lo = base_r * 0.78;
        float inner_hi = max(inner_lo + 1e-4, base_r);
        float inner_fill = (1.0 - smoothstep(fill_lo, fill_hi, r)) * smoothstep(inner_lo, inner_hi, r);

        float spoke_mask = 1.0 - smoothstep(0.55, 0.85, spoke * 2.0);
        float radial_bar = smoothstep(base_r, base_r + 0.005, r) *
                           (1.0 - smoothstep(target_r - 0.004, target_r + 0.005, r)) * spoke_mask;

        float shock_phase = fract(iTime * 0.45);
        float shock_r = base_r + shock_phase * 0.55;
        float shockwave = exp(-abs(r - shock_r) * 55.0) * (1.0 - shock_phase) * (0.35 + safe_beat * 0.65);

        float ring_win = smoothstep(0.12, 0.15, r) * (1.0 - smoothstep(0.66, 0.72, r));
        vec3 ring_col = mix(ring_cyan, ring_magenta, norm_ang);
        col += ring_col * (core_line * 0.45 + radial_bar * 0.85 + inner_fill * 0.25 + shockwave) * ring_win;
    }
    col += ring_magenta * exp(-r * 6.5) * (0.18 + safe_beat * 0.25);

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
