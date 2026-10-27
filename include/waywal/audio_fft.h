// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_AUDIO_FFT_H
#define WAYWAL_AUDIO_FFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWAL_AUDIO_RING_SIZE  8192u
#define WAYWAL_AUDIO_FFT_SIZE   2048u
#define WAYWAL_AUDIO_FREQ_BINS  1024u
#define WAYWAL_AUDIO_BARK_BANDS 64u
#define WAYWAL_AUDIO_TEX_WIDTH  512u
#define WAYWAL_AUDIO_TEX_HEIGHT 2u

typedef struct {
    float bands[WAYWAL_AUDIO_BARK_BANDS];
    float linear_bins[WAYWAL_AUDIO_FREQ_BINS];
    float envelope;
    float bass_energy;
    float beat_pulse;
    uint8_t shadertoy_tex[WAYWAL_AUDIO_TEX_HEIGHT * WAYWAL_AUDIO_TEX_WIDTH];
} waywal_audio_frame_t;

typedef struct waywal_audio_fft waywal_audio_fft_t;

waywal_audio_fft_t *waywal_audio_fft_create(uint32_t sample_rate, bool enable_pipewire);
void waywal_audio_fft_destroy(waywal_audio_fft_t *fft);

size_t waywal_audio_fft_push_samples(waywal_audio_fft_t *fft, const float *samples, size_t count);
void waywal_audio_fft_synthesize_signal(waywal_audio_fft_t *fft, float freq_hz, float amplitude,
                                        float time_sec);

bool waywal_audio_fft_process(waywal_audio_fft_t *fft, waywal_audio_frame_t *out_frame);
const waywal_audio_frame_t *waywal_audio_fft_get_frame(const waywal_audio_fft_t *fft);

void waywal_audio_fft_set_volume(waywal_audio_fft_t *fft, float volume);
float waywal_audio_fft_get_volume(const waywal_audio_fft_t *fft);

void waywal_audio_fft_set_mute_on_audio(waywal_audio_fft_t *fft, bool mute_on_audio);
bool waywal_audio_fft_get_mute_on_audio(const waywal_audio_fft_t *fft);

bool waywal_audio_fft_is_pipewire_active(const waywal_audio_fft_t *fft);
bool waywal_audio_fft_is_external_audio_active(const waywal_audio_fft_t *fft);
bool waywal_audio_fft_is_ducked(const waywal_audio_fft_t *fft);
float waywal_audio_fft_get_effective_gain(const waywal_audio_fft_t *fft);
float waywal_audio_fft_get_effective_volume(const waywal_audio_fft_t *fft);
float waywal_audio_fft_update_ducking(waywal_audio_fft_t *fft, float dt_sec);
void waywal_audio_fft_set_external_audio_for_test(waywal_audio_fft_t *fft, bool active);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_AUDIO_FFT_H */
