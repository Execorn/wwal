// SPDX-License-Identifier: GPL-3.0-or-later
// Sample PipeWire 64-bin Bark FFT audio-reactive Shadertoy wallpaper for WayWal:
//   wwal shader contrib/shaders/audio_spectrum.frag --audio --fps 60

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = fragCoord / max(iResolution.xy, vec2(1.0));
    float bins = 64.0;
    float idx = floor(uv.x * bins);
    float cell_x = fract(uv.x * bins);

    float sample_u = (idx + 0.5) / bins;
    float amp = texture(iChannel0, vec2(sample_u, 0.25)).r;
    amp = clamp(amp, 0.0, 1.0);

    float bar_mask = smoothstep(0.92, 0.78, abs(cell_x - 0.5) * 2.0);
    float fill = smoothstep(amp + 0.015, amp - 0.005, uv.y) * bar_mask;
    float glow = exp(-14.0 * max(uv.y - amp, 0.0)) * amp * 0.45;

    vec3 bg = mix(vec3(0.04, 0.05, 0.09), vec3(0.08, 0.10, 0.18), uv.y);
    vec3 bar_col = mix(vec3(0.18, 0.72, 0.96), vec3(0.96, 0.32, 0.68), uv.x);
    vec3 col = bg + bar_col * (fill + glow);
    fragColor = vec4(col, 1.0);
}
