// SPDX-License-Identifier: GPL-3.0-or-later
// @name: Audio Bars
// @description: 64-band log-frequency Bark FFT spectrum analyzer with peak hold and floor reflection
// @author: WayWal
// @audio_reactive: true

const vec3 PAL_AB_BG = vec3(0.03, 0.04, 0.08);
const vec3 PAL_AB_LOW = vec3(0.12, 0.82, 0.96);
const vec3 PAL_AB_HIGH = vec3(0.96, 0.24, 0.68);

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 res = max(iResolution.xy, vec2(1.0));
    vec2 uv = fragCoord / res;

    vec3 bg_col = PAL_AB_BG;
    vec3 col_low = PAL_AB_LOW;
    vec3 col_high = PAL_AB_HIGH;

    if (length(u_palette_primary) > 0.05) {
        col_low = mix(col_low, u_palette_primary, 0.45);
        col_high = mix(col_high, u_palette_secondary, 0.45);
    }

    float floor_y = 0.24;
    bool is_refl = (uv.y < floor_y);
    float y_dist = is_refl ? ((floor_y - uv.y) / floor_y * 1.45)
                           : ((uv.y - floor_y) / (1.0 - floor_y));

    float num_bars = 64.0;
    float bar_pos = uv.x * num_bars;
    int bar_idx = clamp(int(bar_pos), 0, 63);
    float cell_x = abs(fract(bar_pos) - 0.5);
    float mask_x = 1.0 - smoothstep(0.32, 0.40, cell_x);

    float safe_beat =
        (isnan(u_audio_beat) || isinf(u_audio_beat)) ? 0.0 : clamp(u_audio_beat, 0.0, 1.0);
    vec3 col = mix(bg_col, bg_col * 1.8, uv.y);

    if (mask_x > 0.0 && y_dist >= 0.0) {
        float x_norm = float(bar_idx) * (1.0 / 63.0);
        float log_u = pow(max(x_norm, 1e-4), 1.65) * 0.85 + 0.01;
        float raw_tex = textureLod(iChannel0, vec2(log_u, 0.25), 0.0).r;
        float tex_amp = (isnan(raw_tex) || isinf(raw_tex)) ? 0.0 : clamp(raw_tex, 0.0, 1.0);
        float raw_band = u_audio_bands[bar_idx];
        float safe_band = (isnan(raw_band) || isinf(raw_band)) ? 0.0 : clamp(raw_band, 0.0, 1.0);
        float safe_env =
            (isnan(u_audio_envelope) || isinf(u_audio_envelope)) ? 0.0 : clamp(u_audio_envelope, 0.0, 1.0);
        float band_amp = max(safe_band, tex_amp);

        float synth_amp = 0.28 + 0.22 * sin(iTime * 2.6 + x_norm * 7.5) * cos(iTime * 1.7 - x_norm * 4.2)
                               + 0.14 * sin(iTime * 4.8 + x_norm * 15.0);
        synth_amp *= smoothstep(0.0, 0.15, x_norm) * (1.0 - smoothstep(0.65, 1.0, x_norm)) + 0.25;

        float active_audio = smoothstep(0.01, 0.08, safe_env + tex_amp * 0.5);
        float live_amp = max(band_amp * 0.92, synth_amp * 0.22) + safe_beat * 0.08;
        float amp = clamp(mix(synth_amp, live_amp, active_audio), 0.02, 0.94);

        float segs = 32.0;
        float cell_y = abs(fract(y_dist * segs) - 0.5);
        float seg_top = (floor(y_dist * segs) + 0.5) * (1.0 / segs);

        float mask_y = 1.0 - smoothstep(0.325, 0.44, cell_y);
        float lit = step(seg_top, amp) * mask_x * mask_y;

        float peak_y = min(amp + 0.035 + 0.02 * abs(sin(iTime * 2.2 + x_norm * 9.0)), 0.96);
        float peak_cap = (1.0 - smoothstep(0.004, 0.018, abs(y_dist - peak_y))) * mask_x;

        float glow = exp(-10.0 * max(y_dist - amp, 0.0)) * amp * 0.45 * mask_x;
        vec3 bar_col = mix(col_low, col_high, clamp(x_norm * 0.75 + y_dist * 0.35, 0.0, 1.0));

        float beat_boost = 1.0 + safe_beat * 0.35;
        float refl_atten = is_refl ? (0.24 * exp(-y_dist * 2.8)) : 1.0;

        col += bar_col * (lit + glow) * refl_atten * beat_boost;
        col += vec3(0.95, 0.97, 1.0) * peak_cap * refl_atten * 0.85;
    }

    col += col_low * exp(-abs(uv.y - floor_y) * 90.0) * 0.35;

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
