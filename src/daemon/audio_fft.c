// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/audio_fft.h"

#include "waywal/log.h"

#include <dlfcn.h>
#include <immintrin.h>
#include <math.h>
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

struct waywal_audio_fft {
    uint32_t sample_rate;

    _Alignas(64) float ring[WAYWAL_AUDIO_RING_SIZE];
    _Atomic uint32_t write_idx;
    _Atomic uint32_t read_idx;

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
    float bass_avg;
    float beat_val;
    waywal_audio_frame_t current_frame;

    void *pw_lib;
    void *pw_loop;
    void *pw_stream;
    struct pw_stream_events pw_events;
    void (*fn_pw_thread_loop_stop)(void *loop);
    void (*fn_pw_thread_loop_destroy)(void *loop);
    void (*fn_pw_thread_loop_lock)(void *loop);
    void (*fn_pw_thread_loop_unlock)(void *loop);
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

static void init_fft_tables(waywal_audio_fft_t *fft)
{
    for (uint32_t n = 0; n < WAYWAL_AUDIO_FFT_SIZE; ++n) {
        fft->hann_window[n] =
            0.5f *
            (1.0f - cosf((2.0f * (float)M_PI * (float)n) / (float)(WAYWAL_AUDIO_FFT_SIZE - 1u)));
    }

    for (uint32_t i = 0; i < COMPLEX_FFT_N; ++i) {
        uint32_t rev = 0;
        uint32_t val = i;
        for (uint32_t bit = 0; bit < 10u; ++bit) {
            rev = (rev << 1u) | (val & 1u);
            val >>= 1u;
        }
        fft->bit_rev[i] = (uint16_t)rev;
    }

    for (uint32_t stage = 1; stage <= 10u; ++stage) {
        uint32_t m = 1u << stage;
        uint32_t half = m >> 1u;
        for (uint32_t j = 0; j < half; ++j) {
            float angle = -2.0f * (float)M_PI * (float)j / (float)m;
            fft->twiddle_re[half + j] = cosf(angle);
            fft->twiddle_im[half + j] = sinf(angle);
        }
    }

    for (uint32_t k = 0; k < COMPLEX_FFT_N; ++k) {
        float angle = -(float)M_PI * (float)k / (float)COMPLEX_FFT_N;
        fft->unpack_re[k] = cosf(angle);
        fft->unpack_im[k] = sinf(angle);
    }

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

static void execute_complex_fft_1024(const waywal_audio_fft_t *fft, float *restrict zr,
                                     float *restrict zi)
{
    for (uint32_t stage = 1; stage <= 10u; ++stage) {
        uint32_t m = 1u << stage;
        uint32_t half = m >> 1u;
        const float *w_re = &fft->twiddle_re[half];
        const float *w_im = &fft->twiddle_im[half];

#ifdef __AVX2__
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
#endif

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
    atomic_init(&fft->write_idx, 0u);
    atomic_init(&fft->read_idx, 0u);

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
            memcpy(&fn_pw_properties_new, &s_props_new, sizeof(fn_pw_properties_new));
            memcpy(&fn_pw_stream_new_simple, &s_st_new, sizeof(fn_pw_stream_new_simple));
            memcpy(&fn_pw_stream_connect, &s_st_conn, sizeof(fn_pw_stream_connect));
            memcpy(&fft->fn_pw_stream_destroy, &s_st_destroy, sizeof(fft->fn_pw_stream_destroy));
            memcpy(&fft->fn_pw_stream_dequeue_buffer, &s_st_deq,
                   sizeof(fft->fn_pw_stream_dequeue_buffer));
            memcpy(&fft->fn_pw_stream_queue_buffer, &s_st_q,
                   sizeof(fft->fn_pw_stream_queue_buffer));

            if (fn_pw_init) {
                fft->pw_available = true;
                WAYWAL_LOG_INFO("Audio FFT engine probed libpipewire-0.3.so.0 successfully");
            }

            const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
            char pw_sock[512];
            bool daemon_running = false;
            if (xdg_rt && xdg_rt[0] != '\0') {
                snprintf(pw_sock, sizeof(pw_sock), "%s/pipewire-0", xdg_rt);
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
                    void *props = fn_pw_properties_new("media.type", "Audio", "media.category",
                                                       "Capture", "media.role", "Music",
                                                       "stream.capture.sink", "true", NULL);
                    fft->pw_events.version = 2u;
                    fft->pw_events.process = on_pw_process;
                    fft->pw_stream = fn_pw_stream_new_simple(
                        fn_pw_thread_loop_get_loop(fft->pw_loop), "waywal-audio-capture", props,
                        &fft->pw_events, fft);
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
                                WAYWAL_LOG_INFO("PipeWire audio capture stream started (%u Hz F32)",
                                                fft->sample_rate);
                            } else {
                                fft->fn_pw_stream_destroy(fft->pw_stream);
                                fft->pw_stream = NULL;
                                fft->fn_pw_thread_loop_destroy(fft->pw_loop);
                                fft->pw_loop = NULL;
                            }
                        } else {
                            fft->fn_pw_stream_destroy(fft->pw_stream);
                            fft->pw_stream = NULL;
                            fft->fn_pw_thread_loop_destroy(fft->pw_loop);
                            fft->pw_loop = NULL;
                        }
                    } else {
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

    uint32_t w = atomic_load_explicit(&fft->write_idx, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&fft->read_idx, memory_order_acquire);

    for (size_t i = 0; i < count; ++i) {
        if (w - r >= WAYWAL_AUDIO_RING_SIZE) {
            r++;
            atomic_store_explicit(&fft->read_idx, r, memory_order_release);
        }
        fft->ring[w & (WAYWAL_AUDIO_RING_SIZE - 1u)] = samples[i];
        w++;
    }

    atomic_store_explicit(&fft->write_idx, w, memory_order_release);
    return count;
}

void waywal_audio_fft_synthesize_signal(waywal_audio_fft_t *fft, float freq_hz, float amplitude,
                                        float time_sec)
{
    if (!fft) {
        return;
    }
    float buf[WAYWAL_AUDIO_FFT_SIZE];
    float omega = 2.0f * (float)M_PI * freq_hz / (float)fft->sample_rate;
    float phase0 = 2.0f * (float)M_PI * freq_hz * time_sec;

    for (uint32_t i = 0; i < WAYWAL_AUDIO_FFT_SIZE; ++i) {
        buf[i] = amplitude * sinf(phase0 + omega * (float)i);
    }
    waywal_audio_fft_push_samples(fft, buf, WAYWAL_AUDIO_FFT_SIZE);
}

bool waywal_audio_fft_process(waywal_audio_fft_t *fft, waywal_audio_frame_t *out_frame)
{
    if (!fft) {
        return false;
    }

    uint32_t r = atomic_load_explicit(&fft->read_idx, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&fft->write_idx, memory_order_acquire);
    uint32_t avail = w - r;

    if (avail > 0) {
        if (avail >= WAYWAL_AUDIO_FFT_SIZE) {
            r = w - WAYWAL_AUDIO_FFT_SIZE;
            for (uint32_t i = 0; i < WAYWAL_AUDIO_FFT_SIZE; ++i) {
                fft->history[i] = fft->ring[(r + i) & (WAYWAL_AUDIO_RING_SIZE - 1u)];
            }
            r = w;
        } else {
            uint32_t keep = WAYWAL_AUDIO_FFT_SIZE - avail;
            memmove(fft->history, fft->history + avail, keep * sizeof(float));
            for (uint32_t i = 0; i < avail; ++i) {
                fft->history[keep + i] = fft->ring[(r + i) & (WAYWAL_AUDIO_RING_SIZE - 1u)];
            }
            r += avail;
        }
        atomic_store_explicit(&fft->read_idx, r, memory_order_release);
    }

    _Alignas(32) float zr[COMPLEX_FFT_N];
    _Alignas(32) float zi[COMPLEX_FFT_N];

    for (uint32_t k = 0; k < COMPLEX_FFT_N; ++k) {
        uint16_t j = fft->bit_rev[k];
        zr[j] = fft->history[2u * k] * fft->hann_window[2u * k];
        zi[j] = fft->history[2u * k + 1u] * fft->hann_window[2u * k + 1u];
    }

    execute_complex_fft_1024(fft, zr, zi);

    const float norm = 2.0f / (float)WAYWAL_AUDIO_FFT_SIZE;
    fft->current_frame.linear_bins[0] = fabsf(zr[0] + zi[0]) * (0.5f * norm);

    uint32_t k = 1;
#ifdef __AVX2__
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

    for (uint32_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
        uint16_t start = fft->bark_bin_start[b];
        uint16_t end = fft->bark_bin_end[b];
        float peak = 0.0f;
        float sum = 0.0f;
        uint32_t cnt = 0;
        for (uint16_t k = start; k < end && k < WAYWAL_AUDIO_FREQ_BINS; ++k) {
            float v = fft->current_frame.linear_bins[k];
            if (v > peak) {
                peak = v;
            }
            sum += v;
            cnt++;
        }
        float raw_band = (cnt > 0) ? (0.7f * peak + 0.3f * (sum / (float)cnt)) : 0.0f;
        raw_band = clampf(raw_band * 2.0f, 0.0f, 1.0f);

        float prev = fft->smoothed_bands[b];
        float alpha = (raw_band >= prev) ? alpha_att : alpha_dec;
        float cur = prev + alpha * (raw_band - prev);
        fft->smoothed_bands[b] = cur;
        fft->current_frame.bands[b] = cur;
        env_sum += cur;
    }

    fft->current_frame.envelope = clampf(env_sum / (float)WAYWAL_AUDIO_BARK_BANDS, 0.0f, 1.0f);

    /* Bass energy spans 20..180 Hz (bins 1..8 at 48 kHz / 2048). */
    float bass_sum = 0.0f;
    uint32_t bass_bins = 0;
    for (uint32_t k = 1; k <= 8u && k < WAYWAL_AUDIO_FREQ_BINS; ++k) {
        bass_sum += fft->current_frame.linear_bins[k];
        bass_bins++;
    }
    float bass_inst =
        (bass_bins > 0) ? clampf((bass_sum / (float)bass_bins) * 2.5f, 0.0f, 1.0f) : 0.0f;
    fft->current_frame.bass_energy = bass_inst;

    if (bass_inst > fft->bass_avg * 1.35f && bass_inst > 0.08f) {
        fft->beat_val = clampf((bass_inst - fft->bass_avg) * 2.5f, 0.0f, 1.0f);
    } else {
        fft->beat_val *= 0.85f;
    }
    fft->bass_avg = 0.92f * fft->bass_avg + 0.08f * bass_inst;
    fft->current_frame.beat_pulse = clampf(fft->beat_val, 0.0f, 1.0f);

    /* Shadertoy iChannel0 layout: row 0 = 512 FFT magnitudes, row 1 = 512 waveform samples. */
    uint8_t *row_fft = &fft->current_frame.shadertoy_tex[0];
    uint8_t *row_wav = &fft->current_frame.shadertoy_tex[WAYWAL_AUDIO_TEX_WIDTH];

    for (uint32_t x = 0; x < WAYWAL_AUDIO_TEX_WIDTH; ++x) {
        float mag = 0.5f * (fft->current_frame.linear_bins[2u * x] +
                            fft->current_frame.linear_bins[2u * x + 1u]);
        float scaled = clampf(mag * 512.0f, 0.0f, 255.0f);
        row_fft[x] = (uint8_t)(scaled + 0.5f);

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
