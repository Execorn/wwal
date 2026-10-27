// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/audio_fft.h"

#include "waywal/log.h"

#include <dlfcn.h>
#if defined(__x86_64__) || defined(_M_X64)
#pragma GCC push_options
#pragma GCC target("avx2,fma")
#include <immintrin.h>
#pragma GCC pop_options
#define WAYWAL_HAVE_X86_AVX2_TARGET 1
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COMPLEX_FFT_N 1024u

struct spa_chunk {
    uint32_t offset;
    uint32_t size;
    int32_t stride;
    int32_t flags;
};

struct spa_data {
    uint32_t type;
    uint32_t flags;
    int64_t fd;
    uint32_t mapoffset;
    uint32_t maxsize;
    void *data;
    struct spa_chunk *chunk;
};

struct spa_buffer {
    uint32_t n_metas;
    uint32_t n_datas;
    void *metas;
    struct spa_data *datas;
};

struct pw_buffer {
    struct spa_buffer *buffer;
    void *user_data;
    uint64_t size;
    uint64_t requested;
    uint64_t time;
};

struct spa_pod {
    uint32_t size;
    uint32_t type;
};

struct pw_stream_events {
    uint32_t version;
    void (*destroy)(void *data);
    void (*state_changed)(void *data, int old_state, int state, const char *error);
    void (*control_info)(void *data, uint32_t id, const void *control);
    void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
    void (*param_changed)(void *data, uint32_t id, const struct spa_pod *param);
    void (*add_buffer)(void *data, struct pw_buffer *buffer);
    void (*remove_buffer)(void *data, struct pw_buffer *buffer);
    void (*process)(void *data);
    void (*drained)(void *data);
    void (*command)(void *data, const void *command);
    void (*trigger_done)(void *data);
};

struct spa_pod_prop_u32 {
    uint32_t key;
    uint32_t flags;
    struct spa_pod value;
    uint32_t val;
    uint32_t pad;
};

struct spa_audio_format_pod {
    struct spa_pod pod;
    uint32_t body_type;
    uint32_t body_id;
    struct spa_pod_prop_u32 media_type;
    struct spa_pod_prop_u32 media_subtype;
    struct spa_pod_prop_u32 audio_format;
    struct spa_pod_prop_u32 audio_rate;
    struct spa_pod_prop_u32 audio_channels;
};

struct spa_dict_item {
    const char *key;
    const char *value;
};

struct spa_dict {
    uint32_t flags;
    uint32_t n_items;
    const struct spa_dict_item *items;
};

struct spa_list {
    struct spa_list *next;
    struct spa_list *prev;
};

struct spa_callbacks {
    const void *funcs;
    void *data;
};

struct spa_hook {
    struct spa_list link;
    struct spa_callbacks cb;
    void (*removed)(struct spa_hook *hook);
    void *priv;
};

struct spa_interface {
    const char *type;
    uint32_t version;
    struct spa_callbacks cb;
};

struct pw_core_methods {
    uint32_t version;
    int (*add_listener)(void *object, struct spa_hook *listener, const void *events, void *data);
    int (*hello)(void *object, uint32_t version);
    int (*sync)(void *object, uint32_t id, int seq);
    int (*pong)(void *object, uint32_t id, int seq);
    int (*error)(void *object, uint32_t id, int seq, int res, const char *message);
    void *(*get_registry)(void *object, uint32_t version, size_t user_data_size);
    void *(*create_object)(void *object, const char *factory_name, const char *type,
                           uint32_t version, const struct spa_dict *props, size_t user_data_size);
    int (*destroy)(void *object, void *proxy);
};

struct pw_registry_events {
    uint32_t version;
    void (*global)(void *data, uint32_t id, uint32_t permissions, const char *type,
                   uint32_t version, const struct spa_dict *props);
    void (*global_remove)(void *data, uint32_t id);
};

struct pw_registry_methods {
    uint32_t version;
    int (*add_listener)(void *object, struct spa_hook *listener,
                        const struct pw_registry_events *events, void *data);
    void *(*bind)(void *object, uint32_t id, const char *type, uint32_t version,
                  size_t user_data_size);
    int (*destroy)(void *object, uint32_t id);
};

struct pw_node_info {
    uint32_t id;
    uint32_t max_input_ports;
    uint32_t max_output_ports;
    uint64_t change_mask;
    uint32_t n_input_ports;
    uint32_t n_output_ports;
    int state; /* PW_NODE_STATE_RUNNING = 3 */
    const char *error;
    struct spa_dict *props;
    void *params;
    uint32_t n_params;
};

struct pw_node_events {
    uint32_t version;
    void (*info)(void *data, const struct pw_node_info *info);
    void (*param)(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
                  const struct spa_pod *param);
};

typedef struct {
    uint32_t id;
    void *proxy;
    struct spa_hook listener;
    bool running;
    bool active;
} pw_tracked_node_t;

struct waywal_audio_fft {
    uint32_t sample_rate;
    bool has_avx2;

    _Alignas(64) float ring[WAYWAL_AUDIO_RING_SIZE];
    _Atomic uint32_t write_pos;
    _Atomic uint32_t total_pushed;
    uint32_t last_consumed;

    _Alignas(32) float scratch_samples[WAYWAL_AUDIO_FFT_SIZE];
    _Alignas(32) float history[WAYWAL_AUDIO_FFT_SIZE];
    _Alignas(32) float hann_window[WAYWAL_AUDIO_FFT_SIZE];
    _Alignas(32) float twiddle_re[COMPLEX_FFT_N];
    _Alignas(32) float twiddle_im[COMPLEX_FFT_N];
    _Alignas(32) float unpack_re[COMPLEX_FFT_N];
    _Alignas(32) float unpack_im[COMPLEX_FFT_N];
    uint16_t bit_rev[COMPLEX_FFT_N];

    uint16_t bark_bin_start[WAYWAL_AUDIO_BARK_BANDS];
    uint16_t bark_bin_end[WAYWAL_AUDIO_BARK_BANDS];

    float smoothed_bands[WAYWAL_AUDIO_BARK_BANDS];
    float prev_log_bins[16];
    float bass_avg;
    float flux_avg;
    float beat_val;
    waywal_audio_frame_t current_frame;

    float user_volume;
    float current_gain;
    bool mute_on_audio;
    _Atomic bool external_audio_active;
    _Atomic bool test_override_enabled;
    _Atomic bool test_external_audio_active;

    void *pw_lib;
    void *pw_loop;
    void *pw_context;
    void *pw_core;
    void *pw_registry;
    struct spa_hook pw_registry_listener;
    void *pw_stream;
    struct pw_stream_events pw_events;
    pw_tracked_node_t tracked_nodes[32];
    void (*fn_pw_thread_loop_stop)(void *loop);
    void (*fn_pw_thread_loop_destroy)(void *loop);
    void (*fn_pw_thread_loop_lock)(void *loop);
    void (*fn_pw_thread_loop_unlock)(void *loop);
    void *(*fn_pw_context_new)(void *main_loop, void *props, size_t user_data_size);
    void (*fn_pw_context_destroy)(void *context);
    void *(*fn_pw_context_connect)(void *context, void *properties, size_t user_data_size);
    int (*fn_pw_core_disconnect)(void *core);
    void (*fn_pw_proxy_destroy)(void *proxy);
    void (*fn_pw_proxy_add_object_listener)(void *proxy, struct spa_hook *listener,
                                            const void *funcs, void *data);
    void (*fn_pw_stream_destroy)(void *stream);
    struct pw_buffer *(*fn_pw_stream_dequeue_buffer)(void *stream);
    int (*fn_pw_stream_queue_buffer)(void *stream, struct pw_buffer *buffer);
    bool pw_available;
};

static float clampf(float x, float lo, float hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static float hz_to_bark(float f)
{
    float term1 = 13.0f * atanf(0.00076f * f);
    float ratio = f / 7500.0f;
    float term2 = 3.5f * atanf(ratio * ratio);
    return term1 + term2;
}

static float bark_to_hz(float z)
{
    /* Inverse Bark approximation (Traunmüller / Schroeder) */
    return 600.0f * sinhf(z / 6.0f);
}

static float catmull_rom_sample(const float *bins, uint32_t num_bins, float k_frac)
{
    if (num_bins == 0)
        return 0.0f;
    float kf = clampf(k_frac, 1.0f, (float)(num_bins - 1u));
    int k1 = (int)kf;
    float t = kf - (float)k1;
    int k0 = (k1 > 1) ? (k1 - 1) : 1;
    int k2 = (k1 + 1 < (int)num_bins) ? (k1 + 1) : (int)num_bins - 1;
    int k3 = (k1 + 2 < (int)num_bins) ? (k1 + 2) : (int)num_bins - 1;

    float p0 = bins[k0];
    float p1 = bins[k1];
    float p2 = bins[k2];
    float p3 = bins[k3];

    float t2 = t * t;
    float t3 = t2 * t;
    float v = 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                      (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    return v > 0.0f ? v : 0.0f;
}

static _Alignas(32) float s_hann_window[WAYWAL_AUDIO_FFT_SIZE];
static _Alignas(32) float s_twiddle_re[COMPLEX_FFT_N];
static _Alignas(32) float s_twiddle_im[COMPLEX_FFT_N];
static _Alignas(32) float s_unpack_re[COMPLEX_FFT_N];
static _Alignas(32) float s_unpack_im[COMPLEX_FFT_N];
static uint16_t s_bit_rev[COMPLEX_FFT_N];
static pthread_once_t s_fft_tables_once = PTHREAD_ONCE_INIT;

static void init_static_fft_tables_once(void)
{
    /* 4-Term Blackman-Harris window (-92 dB sidelobe suppression, Harris 1978) */
    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;
    const float inv_n = (2.0f * (float)M_PI) / (float)(WAYWAL_AUDIO_FFT_SIZE - 1u);
    for (uint32_t n = 0; n < WAYWAL_AUDIO_FFT_SIZE; ++n) {
        float phi = inv_n * (float)n;
        s_hann_window[n] = a0 - a1 * cosf(phi) + a2 * cosf(2.0f * phi) - a3 * cosf(3.0f * phi);
    }

    for (uint32_t i = 0; i < COMPLEX_FFT_N; ++i) {
        uint32_t rev = 0;
        uint32_t val = i;
        for (uint32_t bit = 0; bit < 10u; ++bit) {
            rev = (rev << 1u) | (val & 1u);
            val >>= 1u;
        }
        s_bit_rev[i] = (uint16_t)rev;
    }

    for (uint32_t stage = 1; stage <= 10u; ++stage) {
        uint32_t m = 1u << stage;
        uint32_t half = m >> 1u;
        for (uint32_t j = 0; j < half; ++j) {
            float angle = -2.0f * (float)M_PI * (float)j / (float)m;
            s_twiddle_re[half + j] = cosf(angle);
            s_twiddle_im[half + j] = sinf(angle);
        }
    }

    for (uint32_t k = 0; k < COMPLEX_FFT_N; ++k) {
        float angle = -(float)M_PI * (float)k / (float)COMPLEX_FFT_N;
        s_unpack_re[k] = cosf(angle);
        s_unpack_im[k] = sinf(angle);
    }
}

static void init_fft_tables(waywal_audio_fft_t *fft)
{
    pthread_once(&s_fft_tables_once, init_static_fft_tables_once);
    memcpy(fft->hann_window, s_hann_window, sizeof(s_hann_window));
    memcpy(fft->twiddle_re, s_twiddle_re, sizeof(s_twiddle_re));
    memcpy(fft->twiddle_im, s_twiddle_im, sizeof(s_twiddle_im));
    memcpy(fft->unpack_re, s_unpack_re, sizeof(s_unpack_re));
    memcpy(fft->unpack_im, s_unpack_im, sizeof(s_unpack_im));
    memcpy(fft->bit_rev, s_bit_rev, sizeof(s_bit_rev));

    float max_freq = 0.5f * (float)fft->sample_rate;
    float max_bark = hz_to_bark(max_freq);
    if (max_bark <= 0.1f) {
        max_bark = 24.0f;
    }

    for (uint32_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
        fft->bark_bin_start[b] = 0xFFFFu;
        fft->bark_bin_end[b] = 0;
    }

    for (uint32_t k = 1; k < WAYWAL_AUDIO_FREQ_BINS; ++k) {
        float freq = (float)k * (float)fft->sample_rate / (float)WAYWAL_AUDIO_FFT_SIZE;
        float z = hz_to_bark(freq);
        int band = (int)((z / max_bark) * (float)WAYWAL_AUDIO_BARK_BANDS);
        if (band < 0) {
            band = 0;
        }
        if (band >= (int)WAYWAL_AUDIO_BARK_BANDS) {
            band = (int)WAYWAL_AUDIO_BARK_BANDS - 1;
        }
        if (k < fft->bark_bin_start[band]) {
            fft->bark_bin_start[band] = (uint16_t)k;
        }
        if (k + 1u > fft->bark_bin_end[band]) {
            fft->bark_bin_end[band] = (uint16_t)(k + 1u);
        }
    }

    /* Low Bark bands at 48 kHz with 2048-pt FFT (~23.4 Hz/bin) can share adjacent bins. */
    uint16_t last_valid = 1;
    for (uint32_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
        if (fft->bark_bin_start[b] == 0xFFFFu) {
            fft->bark_bin_start[b] = last_valid;
            fft->bark_bin_end[b] = (uint16_t)(last_valid + 1u);
        } else {
            last_valid = fft->bark_bin_start[b];
        }
    }
}

#ifdef WAYWAL_HAVE_X86_AVX2_TARGET
__attribute__((target("avx2,fma"))) static void
execute_complex_fft_1024_avx2(const waywal_audio_fft_t *fft, float *restrict zr, float *restrict zi)
{
    for (uint32_t stage = 1; stage <= 10u; ++stage) {
        uint32_t m = 1u << stage;
        uint32_t half = m >> 1u;
        const float *w_re = &fft->twiddle_re[half];
        const float *w_im = &fft->twiddle_im[half];

        if (half == 1u) {
            const __m256i idx_u = _mm256_setr_epi32(0, 0, 2, 2, 4, 4, 6, 6);
            const __m256i idx_v = _mm256_setr_epi32(1, 1, 3, 3, 5, 5, 7, 7);
            const __m256 sign = _mm256_setr_ps(1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f);
            for (uint32_t k = 0; k < COMPLEX_FFT_N; k += 8u) {
                __m256 r = _mm256_load_ps(zr + k);
                __m256 i = _mm256_load_ps(zi + k);
                __m256 ur = _mm256_permutevar8x32_ps(r, idx_u);
                __m256 ui = _mm256_permutevar8x32_ps(i, idx_u);
                __m256 vr = _mm256_permutevar8x32_ps(r, idx_v);
                __m256 vi = _mm256_permutevar8x32_ps(i, idx_v);
                _mm256_store_ps(zr + k, _mm256_fmadd_ps(sign, vr, ur));
                _mm256_store_ps(zi + k, _mm256_fmadd_ps(sign, vi, ui));
            }
            continue;
        }
        if (half == 2u) {
            const __m256i idx_u = _mm256_setr_epi32(0, 1, 0, 1, 4, 5, 4, 5);
            const __m256i idx_v = _mm256_setr_epi32(2, 3, 2, 3, 6, 7, 6, 7);
            const __m256 wr = _mm256_setr_ps(w_re[0], w_re[1], -w_re[0], -w_re[1], w_re[0], w_re[1],
                                             -w_re[0], -w_re[1]);
            const __m256 wi = _mm256_setr_ps(w_im[0], w_im[1], -w_im[0], -w_im[1], w_im[0], w_im[1],
                                             -w_im[0], -w_im[1]);
            for (uint32_t k = 0; k < COMPLEX_FFT_N; k += 8u) {
                __m256 r = _mm256_load_ps(zr + k);
                __m256 i = _mm256_load_ps(zi + k);
                __m256 ur = _mm256_permutevar8x32_ps(r, idx_u);
                __m256 ui = _mm256_permutevar8x32_ps(i, idx_u);
                __m256 vr = _mm256_permutevar8x32_ps(r, idx_v);
                __m256 vi = _mm256_permutevar8x32_ps(i, idx_v);
                __m256 tr = _mm256_fmsub_ps(wr, vr, _mm256_mul_ps(wi, vi));
                __m256 ti = _mm256_fmadd_ps(wr, vi, _mm256_mul_ps(wi, vr));
                _mm256_store_ps(zr + k, _mm256_add_ps(ur, tr));
                _mm256_store_ps(zi + k, _mm256_add_ps(ui, ti));
            }
            continue;
        }
        if (half == 4u) {
            const __m256i idx_u = _mm256_setr_epi32(0, 1, 2, 3, 0, 1, 2, 3);
            const __m256i idx_v = _mm256_setr_epi32(4, 5, 6, 7, 4, 5, 6, 7);
            const __m256 wr = _mm256_setr_ps(w_re[0], w_re[1], w_re[2], w_re[3], -w_re[0], -w_re[1],
                                             -w_re[2], -w_re[3]);
            const __m256 wi = _mm256_setr_ps(w_im[0], w_im[1], w_im[2], w_im[3], -w_im[0], -w_im[1],
                                             -w_im[2], -w_im[3]);
            for (uint32_t k = 0; k < COMPLEX_FFT_N; k += 8u) {
                __m256 r = _mm256_load_ps(zr + k);
                __m256 i = _mm256_load_ps(zi + k);
                __m256 ur = _mm256_permutevar8x32_ps(r, idx_u);
                __m256 ui = _mm256_permutevar8x32_ps(i, idx_u);
                __m256 vr = _mm256_permutevar8x32_ps(r, idx_v);
                __m256 vi = _mm256_permutevar8x32_ps(i, idx_v);
                __m256 tr = _mm256_fmsub_ps(wr, vr, _mm256_mul_ps(wi, vi));
                __m256 ti = _mm256_fmadd_ps(wr, vi, _mm256_mul_ps(wi, vr));
                _mm256_store_ps(zr + k, _mm256_add_ps(ur, tr));
                _mm256_store_ps(zi + k, _mm256_add_ps(ui, ti));
            }
            continue;
        }
        if (half >= 8u) {
            for (uint32_t k = 0; k < COMPLEX_FFT_N; k += m) {
                for (uint32_t j = 0; j < half; j += 8u) {
                    __m256 wr = _mm256_loadu_ps(w_re + j);
                    __m256 wi = _mm256_loadu_ps(w_im + j);

                    __m256 ur = _mm256_load_ps(zr + k + j);
                    __m256 ui = _mm256_load_ps(zi + k + j);
                    __m256 vr = _mm256_load_ps(zr + k + j + half);
                    __m256 vi = _mm256_load_ps(zi + k + j + half);

                    __m256 tr = _mm256_fmsub_ps(wr, vr, _mm256_mul_ps(wi, vi));
                    __m256 ti = _mm256_fmadd_ps(wr, vi, _mm256_mul_ps(wi, vr));

                    _mm256_store_ps(zr + k + j, _mm256_add_ps(ur, tr));
                    _mm256_store_ps(zi + k + j, _mm256_add_ps(ui, ti));
                    _mm256_store_ps(zr + k + j + half, _mm256_sub_ps(ur, tr));
                    _mm256_store_ps(zi + k + j + half, _mm256_sub_ps(ui, ti));
                }
            }
            continue;
        }
    }
}

__attribute__((target("avx2,fma"))) static uint32_t
unpack_real_fft_avx2(waywal_audio_fft_t *fft, const float *zr, const float *zi, float norm)
{
    uint32_t k = 1;
    const __m256i rev_idx = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
    const __m256 v_half = _mm256_set1_ps(0.5f);
    const __m256 v_neghalf = _mm256_set1_ps(-0.5f);
    const __m256 v_norm = _mm256_set1_ps(norm);
    for (; k + 8u <= COMPLEX_FFT_N; k += 8u) {
        uint32_t nk_base = COMPLEX_FFT_N - k - 7u;
        __m256 zrk = _mm256_loadu_ps(zr + k);
        __m256 zik = _mm256_loadu_ps(zi + k);
        __m256 zrnk = _mm256_permutevar8x32_ps(_mm256_loadu_ps(zr + nk_base), rev_idx);
        __m256 zink = _mm256_permutevar8x32_ps(_mm256_loadu_ps(zi + nk_base), rev_idx);

        __m256 er = _mm256_mul_ps(v_half, _mm256_add_ps(zrk, zrnk));
        __m256 ei = _mm256_mul_ps(v_half, _mm256_sub_ps(zik, zink));
        __m256 or_v = _mm256_mul_ps(v_half, _mm256_add_ps(zik, zink));
        __m256 oi_v = _mm256_mul_ps(v_neghalf, _mm256_sub_ps(zrk, zrnk));

        __m256 wr = _mm256_loadu_ps(fft->unpack_re + k);
        __m256 wi = _mm256_loadu_ps(fft->unpack_im + k);

        __m256 xr = _mm256_add_ps(er, _mm256_fmsub_ps(wr, or_v, _mm256_mul_ps(wi, oi_v)));
        __m256 xi = _mm256_add_ps(ei, _mm256_fmadd_ps(wr, oi_v, _mm256_mul_ps(wi, or_v)));

        __m256 mag =
            _mm256_mul_ps(_mm256_sqrt_ps(_mm256_fmadd_ps(xr, xr, _mm256_mul_ps(xi, xi))), v_norm);
        _mm256_storeu_ps(fft->current_frame.linear_bins + k, mag);
    }
    return k;
}
#endif

static void execute_complex_fft_1024(const waywal_audio_fft_t *fft, float *restrict zr,
                                     float *restrict zi)
{
#ifdef WAYWAL_HAVE_X86_AVX2_TARGET
    if (fft->has_avx2) {
        execute_complex_fft_1024_avx2(fft, zr, zi);
        return;
    }
#endif
    for (uint32_t stage = 1; stage <= 10u; ++stage) {
        uint32_t m = 1u << stage;
        uint32_t half = m >> 1u;
        const float *w_re = &fft->twiddle_re[half];
        const float *w_im = &fft->twiddle_im[half];

        for (uint32_t k = 0; k < COMPLEX_FFT_N; k += m) {
            for (uint32_t j = 0; j < half; ++j) {
                float wr = w_re[j];
                float wi = w_im[j];
                uint32_t idx0 = k + j;
                uint32_t idx1 = idx0 + half;
                float vr = zr[idx1];
                float vi = zi[idx1];
                float tr = wr * vr - wi * vi;
                float ti = wr * vi + wi * vr;
                float ur = zr[idx0];
                float ui = zi[idx0];
                zr[idx0] = ur + tr;
                zi[idx0] = ui + ti;
                zr[idx1] = ur - tr;
                zi[idx1] = ui - ti;
            }
        }
    }
}

static const char *spa_dict_lookup_str(const struct spa_dict *dict, const char *key)
{
    if (!dict || !dict->items || !key) {
        return NULL;
    }
    for (uint32_t i = 0; i < dict->n_items; ++i) {
        if (dict->items[i].key && strcmp(dict->items[i].key, key) == 0) {
            return dict->items[i].value;
        }
    }
    return NULL;
}

static void recompute_pw_external_active(waywal_audio_fft_t *fft)
{
    if (!fft || atomic_load_explicit(&fft->test_override_enabled, memory_order_acquire)) {
        return;
    }
    bool any_running = false;
    for (size_t i = 0; i < 32; ++i) {
        if (fft->tracked_nodes[i].active && fft->tracked_nodes[i].running) {
            any_running = true;
            break;
        }
    }
    atomic_store_explicit(&fft->external_audio_active, any_running, memory_order_release);
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
    waywal_audio_fft_t *fft = (waywal_audio_fft_t *)data;
    if (!fft || !info) {
        return;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (fft->tracked_nodes[i].active && fft->tracked_nodes[i].id == info->id) {
            fft->tracked_nodes[i].running = (info->state == 3 /* PW_NODE_STATE_RUNNING */);
            recompute_pw_external_active(fft);
            return;
        }
    }
}

static const struct pw_node_events s_node_events = {
    .version = 0u,
    .info = on_node_info,
    .param = NULL,
};

static void *pw_core_get_registry_inline(void *core, uint32_t version, size_t user_data_size)
{
    const struct spa_interface *iface = (const struct spa_interface *)core;
    if (!iface || !iface->cb.funcs) {
        return NULL;
    }
    const struct pw_core_methods *funcs = (const struct pw_core_methods *)iface->cb.funcs;
    if (!funcs->get_registry) {
        return NULL;
    }
    return funcs->get_registry(iface->cb.data, version, user_data_size);
}

static int pw_registry_add_listener_inline(void *registry, struct spa_hook *listener,
                                           const struct pw_registry_events *events, void *data)
{
    const struct spa_interface *iface = (const struct spa_interface *)registry;
    if (!iface || !iface->cb.funcs) {
        return -1;
    }
    const struct pw_registry_methods *funcs = (const struct pw_registry_methods *)iface->cb.funcs;
    if (!funcs->add_listener) {
        return -1;
    }
    return funcs->add_listener(iface->cb.data, listener, events, data);
}

static void *pw_registry_bind_inline(void *registry, uint32_t id, const char *type,
                                     uint32_t version, size_t user_data_size)
{
    const struct spa_interface *iface = (const struct spa_interface *)registry;
    if (!iface || !iface->cb.funcs) {
        return NULL;
    }
    const struct pw_registry_methods *funcs = (const struct pw_registry_methods *)iface->cb.funcs;
    if (!funcs->bind) {
        return NULL;
    }
    return funcs->bind(iface->cb.data, id, type, version, user_data_size);
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                               uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    waywal_audio_fft_t *fft = (waywal_audio_fft_t *)data;
    if (!fft || !fft->pw_registry || !fft->fn_pw_proxy_add_object_listener || !type || !props) {
        return;
    }
    if (strcmp(type, "PipeWire:Interface:Node") != 0) {
        return;
    }
    const char *mclass = spa_dict_lookup_str(props, "media.class");
    if (!mclass || strcmp(mclass, "Stream/Output/Audio") != 0) {
        return;
    }
    const char *pid_str = spa_dict_lookup_str(props, "application.process.id");
    if (pid_str && pid_str[0] != '\0') {
        long node_pid = strtol(pid_str, NULL, 10);
        if (node_pid == (long)getpid()) {
            return;
        }
    }
    const char *app_name = spa_dict_lookup_str(props, "application.name");
    if (app_name && (strstr(app_name, "waywal") != NULL || strstr(app_name, "wwald") != NULL)) {
        return;
    }
    const char *node_name = spa_dict_lookup_str(props, "node.name");
    if (node_name && (strstr(node_name, "waywal") != NULL || strstr(node_name, "wwald") != NULL)) {
        return;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (fft->tracked_nodes[i].active && fft->tracked_nodes[i].id == id) {
            return;
        }
    }
    pw_tracked_node_t *slot = NULL;
    for (size_t i = 0; i < 32; ++i) {
        if (!fft->tracked_nodes[i].active) {
            slot = &fft->tracked_nodes[i];
            break;
        }
    }
    if (!slot) {
        return;
    }
    void *proxy = pw_registry_bind_inline(fft->pw_registry, id, type, 3u /* PW_VERSION_NODE */, 0);
    if (!proxy) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->proxy = proxy;
    slot->running = false;
    slot->active = true;
    fft->fn_pw_proxy_add_object_listener(proxy, &slot->listener, &s_node_events, fft);
}

static void spa_hook_remove_inline(struct spa_hook *hook)
{
    if (!hook) {
        return;
    }
    if (hook->link.next && hook->link.prev) {
        hook->link.prev->next = hook->link.next;
        hook->link.next->prev = hook->link.prev;
        hook->link.next = NULL;
        hook->link.prev = NULL;
    }
    if (hook->removed) {
        hook->removed(hook);
    }
}

static void on_registry_global_remove(void *data, uint32_t id)
{
    waywal_audio_fft_t *fft = (waywal_audio_fft_t *)data;
    if (!fft) {
        return;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (fft->tracked_nodes[i].active && fft->tracked_nodes[i].id == id) {
            spa_hook_remove_inline(&fft->tracked_nodes[i].listener);
            if (fft->tracked_nodes[i].proxy && fft->fn_pw_proxy_destroy) {
                fft->fn_pw_proxy_destroy(fft->tracked_nodes[i].proxy);
            }
            memset(&fft->tracked_nodes[i], 0, sizeof(fft->tracked_nodes[i]));
            recompute_pw_external_active(fft);
            return;
        }
    }
}

static const struct pw_registry_events s_registry_events = {
    .version = 0u,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void on_pw_process(void *userdata)
{
    waywal_audio_fft_t *fft = (waywal_audio_fft_t *)userdata;
    if (!fft || !fft->pw_stream || !fft->fn_pw_stream_dequeue_buffer ||
        !fft->fn_pw_stream_queue_buffer) {
        return;
    }
    struct pw_buffer *b = fft->fn_pw_stream_dequeue_buffer(fft->pw_stream);
    if (!b) {
        return;
    }
    struct spa_buffer *buf = b->buffer;
    if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
        uint32_t offset = buf->datas[0].chunk->offset;
        uint32_t size = buf->datas[0].chunk->size;
        uint32_t maxsize = buf->datas[0].maxsize;
        if (maxsize == 0 || offset < maxsize) {
            if (maxsize > 0 && offset + size > maxsize) {
                size = maxsize - offset;
            }
            const float *samples = (const float *)((const uint8_t *)buf->datas[0].data + offset);
            size_t n_samples = size / sizeof(float);
            if (n_samples > 0) {
                waywal_audio_fft_push_samples(fft, samples, n_samples);
            }
        }
    }
    fft->fn_pw_stream_queue_buffer(fft->pw_stream, b);
}

waywal_audio_fft_t *waywal_audio_fft_create(uint32_t sample_rate, bool enable_pipewire)
{
    waywal_audio_fft_t *fft = NULL;
    if (posix_memalign((void **)&fft, 64, sizeof(waywal_audio_fft_t)) != 0 || !fft) {
        return NULL;
    }
    memset(fft, 0, sizeof(*fft));
    fft->sample_rate = (sample_rate == 0) ? 48000u : sample_rate;
#ifdef WAYWAL_HAVE_X86_AVX2_TARGET
    fft->has_avx2 = (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"));
#endif
    atomic_init(&fft->write_pos, 0u);
    atomic_init(&fft->total_pushed, 0u);
    atomic_init(&fft->external_audio_active, false);
    atomic_init(&fft->test_override_enabled, false);
    atomic_init(&fft->test_external_audio_active, false);
    fft->last_consumed = 0u;
    fft->user_volume = 0.0f;
    fft->current_gain = 0.0f;
    fft->mute_on_audio = true;

    init_fft_tables(fft);

    for (uint32_t x = 0; x < WAYWAL_AUDIO_TEX_WIDTH; ++x) {
        fft->current_frame.shadertoy_tex[WAYWAL_AUDIO_TEX_WIDTH + x] = 128u;
    }

    if (enable_pipewire) {
        fft->pw_lib = dlopen("libpipewire-0.3.so.0", RTLD_LAZY | RTLD_NODELETE);
        if (fft->pw_lib) {
            void (*fn_pw_init)(int *, char ***) = NULL;
            void *(*fn_pw_thread_loop_new)(const char *, const void *) = NULL;
            void *(*fn_pw_thread_loop_get_loop)(void *) = NULL;
            int (*fn_pw_thread_loop_start)(void *) = NULL;
            void *(*fn_pw_properties_new)(const char *, ...) = NULL;
            void *(*fn_pw_stream_new_simple)(void *, const char *, void *,
                                             const struct pw_stream_events *, void *) = NULL;
            int (*fn_pw_stream_connect)(void *, int, uint32_t, int, const struct spa_pod **,
                                        uint32_t) = NULL;

            void *s_init = dlsym(fft->pw_lib, "pw_init");
            void *s_tl_new = dlsym(fft->pw_lib, "pw_thread_loop_new");
            void *s_tl_get = dlsym(fft->pw_lib, "pw_thread_loop_get_loop");
            void *s_tl_start = dlsym(fft->pw_lib, "pw_thread_loop_start");
            void *s_tl_stop = dlsym(fft->pw_lib, "pw_thread_loop_stop");
            void *s_tl_destroy = dlsym(fft->pw_lib, "pw_thread_loop_destroy");
            void *s_tl_lock = dlsym(fft->pw_lib, "pw_thread_loop_lock");
            void *s_tl_unlock = dlsym(fft->pw_lib, "pw_thread_loop_unlock");
            void *s_ctx_new = dlsym(fft->pw_lib, "pw_context_new");
            void *s_ctx_destroy = dlsym(fft->pw_lib, "pw_context_destroy");
            void *s_ctx_conn = dlsym(fft->pw_lib, "pw_context_connect");
            void *s_core_disc = dlsym(fft->pw_lib, "pw_core_disconnect");
            void *s_proxy_destroy = dlsym(fft->pw_lib, "pw_proxy_destroy");
            void *s_proxy_add_l = dlsym(fft->pw_lib, "pw_proxy_add_object_listener");
            void *s_props_new = dlsym(fft->pw_lib, "pw_properties_new");
            void *s_st_new = dlsym(fft->pw_lib, "pw_stream_new_simple");
            void *s_st_conn = dlsym(fft->pw_lib, "pw_stream_connect");
            void *s_st_destroy = dlsym(fft->pw_lib, "pw_stream_destroy");
            void *s_st_deq = dlsym(fft->pw_lib, "pw_stream_dequeue_buffer");
            void *s_st_q = dlsym(fft->pw_lib, "pw_stream_queue_buffer");

            memcpy(&fn_pw_init, &s_init, sizeof(fn_pw_init));
            memcpy(&fn_pw_thread_loop_new, &s_tl_new, sizeof(fn_pw_thread_loop_new));
            memcpy(&fn_pw_thread_loop_get_loop, &s_tl_get, sizeof(fn_pw_thread_loop_get_loop));
            memcpy(&fn_pw_thread_loop_start, &s_tl_start, sizeof(fn_pw_thread_loop_start));
            memcpy(&fft->fn_pw_thread_loop_stop, &s_tl_stop, sizeof(fft->fn_pw_thread_loop_stop));
            memcpy(&fft->fn_pw_thread_loop_destroy, &s_tl_destroy,
                   sizeof(fft->fn_pw_thread_loop_destroy));
            memcpy(&fft->fn_pw_thread_loop_lock, &s_tl_lock, sizeof(fft->fn_pw_thread_loop_lock));
            memcpy(&fft->fn_pw_thread_loop_unlock, &s_tl_unlock,
                   sizeof(fft->fn_pw_thread_loop_unlock));
            memcpy(&fft->fn_pw_context_new, &s_ctx_new, sizeof(fft->fn_pw_context_new));
            memcpy(&fft->fn_pw_context_destroy, &s_ctx_destroy, sizeof(fft->fn_pw_context_destroy));
            memcpy(&fft->fn_pw_context_connect, &s_ctx_conn, sizeof(fft->fn_pw_context_connect));
            memcpy(&fft->fn_pw_core_disconnect, &s_core_disc, sizeof(fft->fn_pw_core_disconnect));
            memcpy(&fft->fn_pw_proxy_destroy, &s_proxy_destroy, sizeof(fft->fn_pw_proxy_destroy));
            memcpy(&fft->fn_pw_proxy_add_object_listener, &s_proxy_add_l,
                   sizeof(fft->fn_pw_proxy_add_object_listener));
            memcpy(&fn_pw_properties_new, &s_props_new, sizeof(fn_pw_properties_new));
            memcpy(&fn_pw_stream_new_simple, &s_st_new, sizeof(fn_pw_stream_new_simple));
            memcpy(&fn_pw_stream_connect, &s_st_conn, sizeof(fn_pw_stream_connect));
            memcpy(&fft->fn_pw_stream_destroy, &s_st_destroy, sizeof(fft->fn_pw_stream_destroy));
            memcpy(&fft->fn_pw_stream_dequeue_buffer, &s_st_deq,
                   sizeof(fft->fn_pw_stream_dequeue_buffer));
            memcpy(&fft->fn_pw_stream_queue_buffer, &s_st_q,
                   sizeof(fft->fn_pw_stream_queue_buffer));

            if (fn_pw_init) {
                WAYWAL_LOG_INFO("Audio FFT engine probed libpipewire-0.3.so.0 successfully");
            }

            const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
            const char *pw_remote = getenv("PIPEWIRE_REMOTE");
            if (!pw_remote || pw_remote[0] == '\0') {
                pw_remote = "pipewire-0";
            }
            char pw_sock[512];
            bool daemon_running = false;
            if (pw_remote[0] == '/') {
                if (access(pw_remote, F_OK) == 0) {
                    daemon_running = true;
                }
            } else if (xdg_rt && xdg_rt[0] != '\0') {
                snprintf(pw_sock, sizeof(pw_sock), "%s/%s", xdg_rt, pw_remote);
                if (access(pw_sock, F_OK) == 0) {
                    daemon_running = true;
                }
            }

            if (daemon_running && fn_pw_init && fn_pw_thread_loop_new &&
                fn_pw_thread_loop_get_loop && fn_pw_thread_loop_start &&
                fft->fn_pw_thread_loop_stop && fft->fn_pw_thread_loop_destroy &&
                fn_pw_properties_new && fn_pw_stream_new_simple && fn_pw_stream_connect &&
                fft->fn_pw_stream_destroy && fft->fn_pw_stream_dequeue_buffer &&
                fft->fn_pw_stream_queue_buffer) {
                fn_pw_init(NULL, NULL);
                fft->pw_loop = fn_pw_thread_loop_new("waywal-audio-fft", NULL);
                if (fft->pw_loop) {
                    void *main_loop = fn_pw_thread_loop_get_loop(fft->pw_loop);
                    if (fft->fn_pw_context_new && fft->fn_pw_context_connect &&
                        fft->fn_pw_proxy_add_object_listener) {
                        fft->pw_context = fft->fn_pw_context_new(main_loop, NULL, 0);
                        if (fft->pw_context) {
                            fft->pw_core = fft->fn_pw_context_connect(fft->pw_context, NULL, 0);
                            if (fft->pw_core) {
                                fft->pw_registry = pw_core_get_registry_inline(fft->pw_core, 3u, 0);
                                if (fft->pw_registry) {
                                    pw_registry_add_listener_inline(fft->pw_registry,
                                                                    &fft->pw_registry_listener,
                                                                    &s_registry_events, fft);
                                }
                            }
                        }
                    }

                    void *props = fn_pw_properties_new("media.type", "Audio", "media.category",
                                                       "Capture", "media.role", "Music",
                                                       "stream.capture.sink", "true", NULL);
                    fft->pw_events.version = 2u;
                    fft->pw_events.process = on_pw_process;
                    fft->pw_stream = fn_pw_stream_new_simple(main_loop, "waywal-audio-capture",
                                                             props, &fft->pw_events, fft);
                    if (fft->pw_stream) {
                        struct spa_audio_format_pod fmt_pod = {
                            .pod = {.size = sizeof(struct spa_audio_format_pod) -
                                            sizeof(struct spa_pod),
                                    .type = 15u /* SPA_TYPE_Object */},
                            .body_type = 0x40003u /* SPA_TYPE_OBJECT_Format */,
                            .body_id = 3u /* SPA_PARAM_EnumFormat */,
                            .media_type = {.key = 1u /* SPA_FORMAT_mediaType */,
                                           .flags = 0u,
                                           .value = {.size = 4u, .type = 3u /* SPA_TYPE_Id */},
                                           .val = 1u /* SPA_MEDIA_TYPE_audio */,
                                           .pad = 0u},
                            .media_subtype = {.key = 2u /* SPA_FORMAT_mediaSubtype */,
                                              .flags = 0u,
                                              .value = {.size = 4u, .type = 3u /* SPA_TYPE_Id */},
                                              .val = 1u /* SPA_MEDIA_SUBTYPE_raw */,
                                              .pad = 0u},
                            .audio_format = {.key = 0x10001u /* SPA_FORMAT_AUDIO_format */,
                                             .flags = 0u,
                                             .value = {.size = 4u, .type = 3u /* SPA_TYPE_Id */},
                                             .val = 0x10Bu /* SPA_AUDIO_FORMAT_F32_LE */,
                                             .pad = 0u},
                            .audio_rate = {.key = 0x10003u /* SPA_FORMAT_AUDIO_rate */,
                                           .flags = 0u,
                                           .value = {.size = 4u, .type = 4u /* SPA_TYPE_Int */},
                                           .val = fft->sample_rate,
                                           .pad = 0u},
                            .audio_channels = {.key = 0x10004u /* SPA_FORMAT_AUDIO_channels */,
                                               .flags = 0u,
                                               .value = {.size = 4u, .type = 4u /* SPA_TYPE_Int */},
                                               .val = 1u,
                                               .pad = 0u},
                        };
                        const struct spa_pod *params[1] = {&fmt_pod.pod};
                        int flags = (1 << 0) | (1 << 2) |
                                    (1 << 4); /* AUTOCONNECT | MAP_BUFFERS | RT_PROCESS */
                        if (fn_pw_stream_connect(fft->pw_stream, 0 /* PW_DIRECTION_INPUT */,
                                                 0xFFFFFFFFu /* PW_ID_ANY */, flags, params,
                                                 1u) >= 0) {
                            if (fn_pw_thread_loop_start(fft->pw_loop) >= 0) {
                                fft->pw_available = true;
                                WAYWAL_LOG_INFO("PipeWire audio capture stream started (%u Hz F32)",
                                                fft->sample_rate);
                            } else {
                                fft->fn_pw_stream_destroy(fft->pw_stream);
                                fft->pw_stream = NULL;
                                if (fft->pw_registry && fft->fn_pw_proxy_destroy) {
                                    spa_hook_remove_inline(&fft->pw_registry_listener);
                                    fft->fn_pw_proxy_destroy(fft->pw_registry);
                                    fft->pw_registry = NULL;
                                }
                                if (fft->pw_core && fft->fn_pw_core_disconnect) {
                                    fft->fn_pw_core_disconnect(fft->pw_core);
                                    fft->pw_core = NULL;
                                }
                                if (fft->pw_context && fft->fn_pw_context_destroy) {
                                    fft->fn_pw_context_destroy(fft->pw_context);
                                    fft->pw_context = NULL;
                                }
                                fft->fn_pw_thread_loop_destroy(fft->pw_loop);
                                fft->pw_loop = NULL;
                            }
                        } else {
                            fft->fn_pw_stream_destroy(fft->pw_stream);
                            fft->pw_stream = NULL;
                            if (fft->pw_registry && fft->fn_pw_proxy_destroy) {
                                spa_hook_remove_inline(&fft->pw_registry_listener);
                                fft->fn_pw_proxy_destroy(fft->pw_registry);
                                fft->pw_registry = NULL;
                            }
                            if (fft->pw_core && fft->fn_pw_core_disconnect) {
                                fft->fn_pw_core_disconnect(fft->pw_core);
                                fft->pw_core = NULL;
                            }
                            if (fft->pw_context && fft->fn_pw_context_destroy) {
                                fft->fn_pw_context_destroy(fft->pw_context);
                                fft->pw_context = NULL;
                            }
                            fft->fn_pw_thread_loop_destroy(fft->pw_loop);
                            fft->pw_loop = NULL;
                        }
                    } else {
                        if (fft->pw_registry && fft->fn_pw_proxy_destroy) {
                            spa_hook_remove_inline(&fft->pw_registry_listener);
                            fft->fn_pw_proxy_destroy(fft->pw_registry);
                            fft->pw_registry = NULL;
                        }
                        if (fft->pw_core && fft->fn_pw_core_disconnect) {
                            fft->fn_pw_core_disconnect(fft->pw_core);
                            fft->pw_core = NULL;
                        }
                        if (fft->pw_context && fft->fn_pw_context_destroy) {
                            fft->fn_pw_context_destroy(fft->pw_context);
                            fft->pw_context = NULL;
                        }
                        fft->fn_pw_thread_loop_destroy(fft->pw_loop);
                        fft->pw_loop = NULL;
                    }
                }
            }
        }
    }

    return fft;
}

void waywal_audio_fft_destroy(waywal_audio_fft_t *fft)
{
    if (!fft) {
        return;
    }
    if (fft->pw_loop) {
        if (fft->fn_pw_thread_loop_stop) {
            fft->fn_pw_thread_loop_stop(fft->pw_loop);
        }
        if (fft->pw_stream && fft->fn_pw_stream_destroy) {
            fft->fn_pw_stream_destroy(fft->pw_stream);
            fft->pw_stream = NULL;
        }
        for (size_t i = 0; i < 32; ++i) {
            if (fft->tracked_nodes[i].active) {
                spa_hook_remove_inline(&fft->tracked_nodes[i].listener);
                if (fft->tracked_nodes[i].proxy && fft->fn_pw_proxy_destroy) {
                    fft->fn_pw_proxy_destroy(fft->tracked_nodes[i].proxy);
                }
            }
            memset(&fft->tracked_nodes[i], 0, sizeof(fft->tracked_nodes[i]));
        }
        if (fft->pw_registry && fft->fn_pw_proxy_destroy) {
            spa_hook_remove_inline(&fft->pw_registry_listener);
            fft->fn_pw_proxy_destroy(fft->pw_registry);
            fft->pw_registry = NULL;
        }
        if (fft->pw_core && fft->fn_pw_core_disconnect) {
            fft->fn_pw_core_disconnect(fft->pw_core);
            fft->pw_core = NULL;
        }
        if (fft->pw_context && fft->fn_pw_context_destroy) {
            fft->fn_pw_context_destroy(fft->pw_context);
            fft->pw_context = NULL;
        }
        if (fft->fn_pw_thread_loop_destroy) {
            fft->fn_pw_thread_loop_destroy(fft->pw_loop);
        }
        fft->pw_loop = NULL;
    }
    if (fft->pw_lib) {
        dlclose(fft->pw_lib);
    }
    free(fft);
}

size_t waywal_audio_fft_push_samples(waywal_audio_fft_t *fft, const float *samples, size_t count)
{
    if (!fft || !samples || count == 0) {
        return 0;
    }

    uint32_t wp = atomic_load_explicit(&fft->write_pos, memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        fft->ring[(wp + (uint32_t)i) & (WAYWAL_AUDIO_RING_SIZE - 1u)] = samples[i];
    }
    atomic_store_explicit(&fft->write_pos, wp + (uint32_t)count, memory_order_release);
    atomic_fetch_add_explicit(&fft->total_pushed, (uint32_t)count, memory_order_release);
    return count;
}

void waywal_audio_fft_synthesize_signal(waywal_audio_fft_t *fft, float freq_hz, float amplitude,
                                        float time_sec)
{
    if (!fft) {
        return;
    }
    float omega = 2.0f * (float)M_PI * freq_hz / (float)fft->sample_rate;
    float phase0 = 2.0f * (float)M_PI * freq_hz * time_sec;

    for (uint32_t i = 0; i < WAYWAL_AUDIO_FFT_SIZE; ++i) {
        fft->scratch_samples[i] = amplitude * sinf(phase0 + omega * (float)i);
    }
    waywal_audio_fft_push_samples(fft, fft->scratch_samples, WAYWAL_AUDIO_FFT_SIZE);
}

bool waywal_audio_fft_process(waywal_audio_fft_t *fft, waywal_audio_frame_t *out_frame)
{
    if (!fft) {
        return false;
    }

    uint32_t wp = atomic_load_explicit(&fft->write_pos, memory_order_acquire);
    uint32_t avail = wp - fft->last_consumed;

    if (avail == 0) {
        /* Early-return without redundant 2048-pt FFT when no new audio samples arrived */
        float env_sum = 0.0f;
        for (uint32_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
            fft->smoothed_bands[b] *= 0.94f;
            fft->current_frame.bands[b] = fft->smoothed_bands[b];
            env_sum += fft->smoothed_bands[b];
        }
        fft->current_frame.envelope = clampf(env_sum / (float)WAYWAL_AUDIO_BARK_BANDS, 0.0f, 1.0f);
        fft->current_frame.bass_energy *= 0.94f;
        fft->beat_val *= 0.85f;
        fft->current_frame.beat_pulse = clampf(fft->beat_val, 0.0f, 1.0f);
        if (!atomic_load_explicit(&fft->test_override_enabled, memory_order_acquire) &&
            !fft->pw_registry) {
            atomic_store_explicit(&fft->external_audio_active, fft->current_frame.envelope > 0.008f,
                                  memory_order_release);
        }
        if (out_frame) {
            *out_frame = fft->current_frame;
        }
        return true;
    }

    if (avail >= WAYWAL_AUDIO_FFT_SIZE) {
        uint32_t r = wp - WAYWAL_AUDIO_FFT_SIZE;
        for (uint32_t i = 0; i < WAYWAL_AUDIO_FFT_SIZE; ++i) {
            fft->history[i] = fft->ring[(r + i) & (WAYWAL_AUDIO_RING_SIZE - 1u)];
        }
    } else {
        uint32_t keep = WAYWAL_AUDIO_FFT_SIZE - avail;
        uint32_t r = wp - avail;
        memmove(fft->history, fft->history + avail, keep * sizeof(float));
        for (uint32_t i = 0; i < avail; ++i) {
            fft->history[keep + i] = fft->ring[(r + i) & (WAYWAL_AUDIO_RING_SIZE - 1u)];
        }
    }
    fft->last_consumed = wp;

    _Alignas(32) float zr[COMPLEX_FFT_N];
    _Alignas(32) float zi[COMPLEX_FFT_N];

    for (uint32_t k = 0; k < COMPLEX_FFT_N; ++k) {
        uint16_t j = fft->bit_rev[k];
        zr[j] = fft->history[2u * k] * fft->hann_window[2u * k];
        zi[j] = fft->history[2u * k + 1u] * fft->hann_window[2u * k + 1u];
    }

    execute_complex_fft_1024(fft, zr, zi);

    /* Coherent gain compensation for 4-term Blackman-Harris (a0 = 0.35875) */
    const float norm = 2.0f / ((float)WAYWAL_AUDIO_FFT_SIZE * 0.35875f);
    fft->current_frame.linear_bins[0] = fabsf(zr[0] + zi[0]) * (0.5f * norm);

    uint32_t k = 1;
#ifdef WAYWAL_HAVE_X86_AVX2_TARGET
    if (fft->has_avx2) {
        k = unpack_real_fft_avx2(fft, zr, zi, norm);
    }
#endif
    for (; k < COMPLEX_FFT_N; ++k) {
        uint32_t nk = COMPLEX_FFT_N - k;
        float er = 0.5f * (zr[k] + zr[nk]);
        float ei = 0.5f * (zi[k] - zi[nk]);
        float or_v = 0.5f * (zi[k] + zi[nk]);
        float oi_v = -0.5f * (zr[k] - zr[nk]);

        float wr = fft->unpack_re[k];
        float wi = fft->unpack_im[k];

        float xr = er + wr * or_v - wi * oi_v;
        float xi = ei + wr * oi_v + wi * or_v;

        fft->current_frame.linear_bins[k] = sqrtf(xr * xr + xi * xi) * norm;
    }

    const float alpha_att = 0.65f;
    const float alpha_dec = 0.18f;
    float env_sum = 0.0f;
    float max_bark = hz_to_bark(0.5f * (float)fft->sample_rate);
    if (max_bark <= 0.1f) {
        max_bark = 24.0f;
    }

    for (uint32_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
        uint16_t start = fft->bark_bin_start[b];
        uint16_t end = fft->bark_bin_end[b];
        float raw_band = 0.0f;

        if (end <= start + 1u && b < 16u) {
            /* Fractional-bin Catmull-Rom interpolation for sub-bass/low-Bark bands */
            float z_center = (((float)b + 0.5f) / (float)WAYWAL_AUDIO_BARK_BANDS) * max_bark;
            float f_center = bark_to_hz(z_center);
            float k_frac = (f_center * (float)WAYWAL_AUDIO_FFT_SIZE) / (float)fft->sample_rate;
            raw_band =
                catmull_rom_sample(fft->current_frame.linear_bins, WAYWAL_AUDIO_FREQ_BINS, k_frac);
        } else {
            float peak = 0.0f;
            float sum = 0.0f;
            uint32_t cnt = 0;
            for (uint16_t bin = start; bin < end && bin < WAYWAL_AUDIO_FREQ_BINS; ++bin) {
                float v = fft->current_frame.linear_bins[bin];
                if (v > peak) {
                    peak = v;
                }
                sum += v;
                cnt++;
            }
            raw_band = (cnt > 0) ? (0.7f * peak + 0.3f * (sum / (float)cnt)) : 0.0f;
        }
        raw_band = clampf(raw_band * 2.0f, 0.0f, 1.0f);

        float prev = fft->smoothed_bands[b];
        float alpha = (raw_band >= prev) ? alpha_att : alpha_dec;
        float cur = prev + alpha * (raw_band - prev);
        fft->smoothed_bands[b] = cur;
        fft->current_frame.bands[b] = cur;
        env_sum += cur;
    }

    fft->current_frame.envelope = clampf(env_sum / (float)WAYWAL_AUDIO_BARK_BANDS, 0.0f, 1.0f);
    if (!atomic_load_explicit(&fft->test_override_enabled, memory_order_acquire) &&
        !fft->pw_registry) {
        atomic_store_explicit(&fft->external_audio_active, fft->current_frame.envelope > 0.008f,
                              memory_order_release);
    }

    /* Bass energy + half-wave rectified log-spectral flux onset detection (Bock & Widmer 2013) */
    float bass_sum = 0.0f;
    float log_flux = 0.0f;
    uint32_t bass_bins = 0;
    const float gamma_comp = 100.0f;
    for (uint32_t bin = 1; bin <= 12u && bin < WAYWAL_AUDIO_FREQ_BINS; ++bin) {
        float mag = fft->current_frame.linear_bins[bin];
        if (bin <= 8u) {
            bass_sum += mag;
            bass_bins++;
        }
        float log_mag = log1pf(gamma_comp * mag);
        float diff = log_mag - fft->prev_log_bins[bin];
        if (diff > 0.0f) {
            log_flux += diff;
        }
        fft->prev_log_bins[bin] = log_mag;
    }
    float bass_inst =
        (bass_bins > 0) ? clampf((bass_sum / (float)bass_bins) * 2.5f, 0.0f, 1.0f) : 0.0f;
    fft->current_frame.bass_energy = bass_inst;

    if ((log_flux > fft->flux_avg * 1.4f && log_flux > 0.15f) ||
        (bass_inst > fft->bass_avg * 1.35f && bass_inst > 0.08f)) {
        float flux_beat = (log_flux - fft->flux_avg) * 0.6f;
        float bass_beat = (bass_inst - fft->bass_avg) * 2.5f;
        fft->beat_val = clampf(flux_beat > bass_beat ? flux_beat : bass_beat, 0.0f, 1.0f);
    } else {
        fft->beat_val *= 0.85f;
    }
    fft->flux_avg = 0.90f * fft->flux_avg + 0.10f * log_flux;
    fft->bass_avg = 0.92f * fft->bass_avg + 0.08f * bass_inst;
    fft->current_frame.beat_pulse = clampf(fft->beat_val, 0.0f, 1.0f);

    /* Shadertoy iChannel0 layout:
     * row 0 = 512 W3C WebAudio log-dBFS [-100 dB, -30 dB] -> [0, 255] FFT magnitudes,
     * row 1 = 512 waveform samples. */
    uint8_t *row_fft = &fft->current_frame.shadertoy_tex[0];
    uint8_t *row_wav = &fft->current_frame.shadertoy_tex[WAYWAL_AUDIO_TEX_WIDTH];

    const float min_db = -100.0f;
    const float max_db = -30.0f;
    const float inv_db_range = 1.0f / (max_db - min_db);

    for (uint32_t x = 0; x < WAYWAL_AUDIO_TEX_WIDTH; ++x) {
        float mag = 0.5f * (fft->current_frame.linear_bins[2u * x] +
                            fft->current_frame.linear_bins[2u * x + 1u]);
        float db = 20.0f * log10f(mag > 1e-6f ? mag : 1e-6f);
        float norm_db = clampf((db - min_db) * inv_db_range, 0.0f, 1.0f);
        row_fft[x] = (uint8_t)(norm_db * 255.0f + 0.5f);

        float sample = fft->history[(WAYWAL_AUDIO_FFT_SIZE - WAYWAL_AUDIO_TEX_WIDTH) + x];
        float wav_u8 = clampf(128.0f + sample * 127.0f, 0.0f, 255.0f);
        row_wav[x] = (uint8_t)(wav_u8 + 0.5f);
    }

    if (out_frame) {
        *out_frame = fft->current_frame;
    }
    return true;
}

const waywal_audio_frame_t *waywal_audio_fft_get_frame(const waywal_audio_fft_t *fft)
{
    return fft ? &fft->current_frame : NULL;
}

void waywal_audio_fft_set_volume(waywal_audio_fft_t *fft, float volume)
{
    if (!fft)
        return;
    float clamped = clampf(volume, 0.0f, 1.0f);
    fft->user_volume = clamped;
    if (!(fft->mute_on_audio && waywal_audio_fft_is_external_audio_active(fft))) {
        fft->current_gain = clamped;
    }
}

float waywal_audio_fft_get_volume(const waywal_audio_fft_t *fft)
{
    return fft ? fft->user_volume : 0.0f;
}

void waywal_audio_fft_set_mute_on_audio(waywal_audio_fft_t *fft, bool mute_on_audio)
{
    if (!fft)
        return;
    fft->mute_on_audio = mute_on_audio;
    if (!mute_on_audio || !waywal_audio_fft_is_external_audio_active(fft)) {
        fft->current_gain = fft->user_volume;
    }
}

bool waywal_audio_fft_get_mute_on_audio(const waywal_audio_fft_t *fft)
{
    return fft ? fft->mute_on_audio : false;
}

bool waywal_audio_fft_is_pipewire_active(const waywal_audio_fft_t *fft)
{
    return fft ? fft->pw_available : false;
}

bool waywal_audio_fft_is_external_audio_active(const waywal_audio_fft_t *fft)
{
    if (!fft)
        return false;
    if (atomic_load_explicit(&fft->test_override_enabled, memory_order_acquire))
        return atomic_load_explicit(&fft->test_external_audio_active, memory_order_acquire);
    return atomic_load_explicit(&fft->external_audio_active, memory_order_acquire);
}

bool waywal_audio_fft_is_ducked(const waywal_audio_fft_t *fft)
{
    if (!fft)
        return false;
    return fft->mute_on_audio && waywal_audio_fft_is_external_audio_active(fft);
}

float waywal_audio_fft_get_effective_gain(const waywal_audio_fft_t *fft)
{
    return fft ? fft->current_gain : 0.0f;
}

float waywal_audio_fft_get_effective_volume(const waywal_audio_fft_t *fft)
{
    return fft ? fft->current_gain : 0.0f;
}

float waywal_audio_fft_update_ducking(waywal_audio_fft_t *fft, float dt_sec)
{
    if (!fft)
        return 0.0f;
    if (dt_sec < 0.0f)
        dt_sec = 0.0f;
    bool ext_active = waywal_audio_fft_is_external_audio_active(fft);
    float target_gain = (fft->mute_on_audio && ext_active) ? 0.0f : fft->user_volume;
    float alpha = 1.0f - expf(-dt_sec * 12.0f);
    fft->current_gain += (target_gain - fft->current_gain) * alpha;
    fft->current_gain = clampf(fft->current_gain, 0.0f, 1.0f);
    return fft->current_gain;
}

void waywal_audio_fft_set_external_audio_for_test(waywal_audio_fft_t *fft, bool active)
{
    if (!fft)
        return;
    atomic_store_explicit(&fft->test_external_audio_active, active, memory_order_release);
    atomic_store_explicit(&fft->test_override_enabled, true, memory_order_release);
    atomic_store_explicit(&fft->external_audio_active, active, memory_order_release);
}
