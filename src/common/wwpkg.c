// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/wwpkg.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define WWPKG_HAS_OPENAT2_HDR 1
#endif
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#endif

/* ========================================================================= */
/* Error Strings & Thread-Local Diagnostic Context                           */
/* ========================================================================= */

static _Thread_local int g_tls_last_error = WWPKG_OK;

static inline void set_tls_error(int err) { g_tls_last_error = err; }

const char *wwpkg_error_str(int err)
{
    switch ((wwpkg_error_t)err) {
    case WWPKG_OK:
        return "Success";
    case WWPKG_ERR_INVALID_PARAM:
        return "Invalid parameter or null pointer";
    case WWPKG_ERR_IO:
        return "Filesystem I/O error";
    case WWPKG_ERR_CORRUPT:
        return "Corrupt package or tar header checksum mismatch";
    case WWPKG_ERR_DECOMPRESS:
        return "Zstandard decompression error or archive size limit exceeded";
    case WWPKG_ERR_BAD_MANIFEST:
        return "Invalid or malformed manifest.json schema";
    case WWPKG_ERR_BAD_SIGNATURE:
        return "Cryptographic signature or Merkle root verification failed";
    case WWPKG_ERR_UNTRUSTED:
        return "Author public key not in trusted_keys.json";
    case WWPKG_ERR_CAPABILITY:
        return "Forbidden capability claimed (network or filesystem must be false)";
    case WWPKG_ERR_PATH_TRAVERSAL:
        return "Archive path traversal or symlink attempt rejected";
    default:
        return "Unknown error";
    }
}

/* ========================================================================= */
/* Thread-Safe Lazy Dynamic Loading (Zero DT_NEEDED Daemon Guardrail)        */
/* ========================================================================= */

typedef struct {
    const void *src;
    size_t size;
    size_t pos;
} zstd_in_buffer_t;

typedef struct {
    void *dst;
    size_t size;
    size_t pos;
} zstd_out_buffer_t;

typedef struct zstd_dstream_s zstd_dstream_t;
typedef struct zstd_dctx_s zstd_dctx_t;

typedef struct {
    size_t (*compress)(void *dst, size_t dstCapacity, const void *src, size_t srcSize,
                       int compressionLevel);
    size_t (*decompress)(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
    unsigned long long (*getFrameContentSize)(const void *src, size_t srcSize);
    size_t (*findFrameCompressedSize)(const void *src, size_t srcSize);
    unsigned (*isError)(size_t code);
    const char *(*getErrorName)(size_t code);
    size_t (*compressBound)(size_t srcSize);
    zstd_dstream_t *(*createDStream)(void);
    size_t (*freeDStream)(zstd_dstream_t *zds);
    size_t (*initDStream)(zstd_dstream_t *zds);
    size_t (*decompressStream)(zstd_dstream_t *zds, zstd_out_buffer_t *output,
                               zstd_in_buffer_t *input);
    zstd_dctx_t *(*createDCtx)(void);
    size_t (*freeDCtx)(zstd_dctx_t *dctx);
    size_t (*decompressDCtx)(zstd_dctx_t *dctx, void *dst, size_t dstCapacity, const void *src,
                             size_t srcSize);
} zstd_funcs_t;

static pthread_once_t g_zstd_once = PTHREAD_ONCE_INIT;
static void *g_zstd_handle = nullptr;
static bool g_zstd_available = false;
static zstd_funcs_t g_zstd;
static pthread_mutex_t g_shared_dctx_mu = PTHREAD_MUTEX_INITIALIZER;
static zstd_dctx_t *g_shared_dctx = nullptr;

static void zstd_init_once(void)
{
    const int flags = RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE;
    g_zstd_handle = dlopen("libzstd.so.1", flags);
    if (!g_zstd_handle) {
        g_zstd_handle = dlopen("libzstd.so", flags);
    }
    if (!g_zstd_handle) {
        return;
    }

    *(void **)(&g_zstd.compress) = dlsym(g_zstd_handle, "ZSTD_compress");
    *(void **)(&g_zstd.decompress) = dlsym(g_zstd_handle, "ZSTD_decompress");
    *(void **)(&g_zstd.getFrameContentSize) = dlsym(g_zstd_handle, "ZSTD_getFrameContentSize");
    *(void **)(&g_zstd.findFrameCompressedSize) =
        dlsym(g_zstd_handle, "ZSTD_findFrameCompressedSize");
    *(void **)(&g_zstd.isError) = dlsym(g_zstd_handle, "ZSTD_isError");
    *(void **)(&g_zstd.getErrorName) = dlsym(g_zstd_handle, "ZSTD_getErrorName");
    *(void **)(&g_zstd.compressBound) = dlsym(g_zstd_handle, "ZSTD_compressBound");
    *(void **)(&g_zstd.createDStream) = dlsym(g_zstd_handle, "ZSTD_createDStream");
    *(void **)(&g_zstd.freeDStream) = dlsym(g_zstd_handle, "ZSTD_freeDStream");
    *(void **)(&g_zstd.initDStream) = dlsym(g_zstd_handle, "ZSTD_initDStream");
    *(void **)(&g_zstd.decompressStream) = dlsym(g_zstd_handle, "ZSTD_decompressStream");
    *(void **)(&g_zstd.createDCtx) = dlsym(g_zstd_handle, "ZSTD_createDCtx");
    *(void **)(&g_zstd.freeDCtx) = dlsym(g_zstd_handle, "ZSTD_freeDCtx");
    *(void **)(&g_zstd.decompressDCtx) = dlsym(g_zstd_handle, "ZSTD_decompressDCtx");

    if (g_zstd.compress && g_zstd.decompress && g_zstd.getFrameContentSize &&
        g_zstd.findFrameCompressedSize && g_zstd.isError && g_zstd.compressBound &&
        g_zstd.createDStream && g_zstd.freeDStream && g_zstd.initDStream &&
        g_zstd.decompressStream) {
        g_zstd_available = true;
    }
}

static inline void zstd_probe(void) { pthread_once(&g_zstd_once, zstd_init_once); }

/* Reuses a shared ZSTD_DCtx when uncontended to avoid 160 KiB context allocation per Frame 0 open
 */
static size_t zstd_decompress_fast(void *dst, size_t dst_cap, const void *src, size_t src_size)
{
    if (g_zstd.createDCtx && g_zstd.decompressDCtx &&
        pthread_mutex_trylock(&g_shared_dctx_mu) == 0) {
        if (!g_shared_dctx) {
            g_shared_dctx = g_zstd.createDCtx();
        }
        if (g_shared_dctx) {
            size_t res = g_zstd.decompressDCtx(g_shared_dctx, dst, dst_cap, src, src_size);
            pthread_mutex_unlock(&g_shared_dctx_mu);
            return res;
        }
        pthread_mutex_unlock(&g_shared_dctx_mu);
    }
    return g_zstd.decompress(dst, dst_cap, src, src_size);
}

typedef struct {
    int (*sodium_init_fn)(void);
    int (*crypto_sign_ed25519_keypair)(uint8_t *pk, uint8_t *sk);
    int (*crypto_sign_ed25519_seed_keypair)(uint8_t *pk, uint8_t *sk, const uint8_t *seed);
    int (*crypto_sign_ed25519_detached)(uint8_t *sig, unsigned long long *siglen_p,
                                        const uint8_t *m, unsigned long long mlen,
                                        const uint8_t *sk);
    int (*crypto_sign_ed25519_verify_detached)(const uint8_t *sig, const uint8_t *m,
                                               unsigned long long mlen, const uint8_t *pk);
} sodium_funcs_t;

static pthread_once_t g_sodium_once = PTHREAD_ONCE_INIT;
static void *g_sodium_handle = nullptr;
static bool g_sodium_available = false;
static sodium_funcs_t g_sodium;

static void sodium_init_once(void)
{
    const int flags = RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE;
    g_sodium_handle = dlopen("libsodium.so.26", flags);
    if (!g_sodium_handle) {
        g_sodium_handle = dlopen("libsodium.so", flags);
    }
    if (!g_sodium_handle) {
        return;
    }

    *(void **)(&g_sodium.sodium_init_fn) = dlsym(g_sodium_handle, "sodium_init");
    *(void **)(&g_sodium.crypto_sign_ed25519_keypair) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_keypair");
    *(void **)(&g_sodium.crypto_sign_ed25519_seed_keypair) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_seed_keypair");
    *(void **)(&g_sodium.crypto_sign_ed25519_detached) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_detached");
    *(void **)(&g_sodium.crypto_sign_ed25519_verify_detached) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_verify_detached");

    if (g_sodium.sodium_init_fn && g_sodium.sodium_init_fn() < 0) {
        return;
    }

    if (g_sodium.crypto_sign_ed25519_keypair && g_sodium.crypto_sign_ed25519_seed_keypair &&
        g_sodium.crypto_sign_ed25519_detached && g_sodium.crypto_sign_ed25519_verify_detached) {
        g_sodium_available = true;
    }
}

static inline void sodium_probe(void) { pthread_once(&g_sodium_once, sodium_init_once); }

static pthread_once_t g_c_locale_once = PTHREAD_ONCE_INIT;
static locale_t g_c_locale = (locale_t)0;

static void c_locale_init_once(void) { g_c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0); }

static inline locale_t get_c_locale(void)
{
    pthread_once(&g_c_locale_once, c_locale_init_once);
    return g_c_locale;
}

__attribute__((destructor)) static void wwpkg_global_cleanup(void)
{
    pthread_mutex_lock(&g_shared_dctx_mu);
    if (g_shared_dctx && g_zstd.freeDCtx) {
        g_zstd.freeDCtx(g_shared_dctx);
        g_shared_dctx = nullptr;
    }
    pthread_mutex_unlock(&g_shared_dctx_mu);
    if (g_c_locale != (locale_t)0) {
        freelocale(g_c_locale);
        g_c_locale = (locale_t)0;
    }
}

static int secure_random_bytes(uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t ret = getrandom(buf + done, len - done, 0);
        if (ret > 0) {
            done += (size_t)ret;
            continue;
        }
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (done == len) {
        return 0;
    }

    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    while (done < len) {
        ssize_t r = read(fd, buf + done, len - done);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        done += (size_t)r;
    }
    close(fd);
    return 0;
}

/* ========================================================================= */
/* FIPS 180-4 SHA-256 with Runtime x86_64 SHA-NI Acceleration                */
/* ========================================================================= */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

static inline uint32_t ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static void sha256_blocks_scalar(uint32_t state[8], const uint8_t *data, size_t num_blocks)
{
    for (size_t blk = 0; blk < num_blocks; blk++) {
        const uint8_t *block = data + (blk * 64);
        uint32_t W[64];
        for (int i = 0; i < 16; i++) {
            W[i] = ((uint32_t)block[i * 4 + 0] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror32(W[i - 15], 7) ^ ror32(W[i - 15], 18) ^ (W[i - 15] >> 3);
            uint32_t s1 = ror32(W[i - 2], 17) ^ ror32(W[i - 2], 19) ^ (W[i - 2] >> 10);
            W[i] = W[i - 16] + s0 + W[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K256[i] + W[i];
            uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
static pthread_once_t g_shani_once = PTHREAD_ONCE_INIT;
static bool g_shani_supported = false;

static void shani_detect_once(void)
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return;
    }
    /* Require SSSE3 (bit 9) and SSE4.1 (bit 19) alongside SHA-NI */
    const unsigned int need_ecx = (1u << 9) | (1u << 19);
    if ((ecx & need_ecx) != need_ecx) {
        return;
    }
    unsigned int eax7 = 0, ebx7 = 0, ecx7 = 0, edx7 = 0;
    if (!__get_cpuid_count(7, 0, &eax7, &ebx7, &ecx7, &edx7)) {
        return;
    }
    if ((ebx7 & (1u << 29)) != 0) {
        g_shani_supported = true;
    }
}

__attribute__((target("sha,sse4.1,ssse3"))) static void
sha256_blocks_shani(uint32_t state[8], const uint8_t *data, size_t num_blocks)
{
    const __m128i bswap_mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    __m128i abef = _mm_set_epi32((int)state[0], (int)state[1], (int)state[4], (int)state[5]);
    __m128i cdgh = _mm_set_epi32((int)state[2], (int)state[3], (int)state[6], (int)state[7]);

#define SHANI_4ROUNDS(m, g)                                                                        \
    do {                                                                                           \
        __m128i wk =                                                                               \
            _mm_add_epi32((m), _mm_set_epi32((int)K256[(g) * 4 + 3], (int)K256[(g) * 4 + 2],       \
                                             (int)K256[(g) * 4 + 1], (int)K256[(g) * 4 + 0]));     \
        __m128i stmp = abef;                                                                       \
        abef = _mm_sha256rnds2_epu32(cdgh, abef, wk);                                              \
        cdgh = stmp;                                                                               \
        wk = _mm_shuffle_epi32(wk, 0x0E);                                                          \
        stmp = abef;                                                                               \
        abef = _mm_sha256rnds2_epu32(cdgh, abef, wk);                                              \
        cdgh = stmp;                                                                               \
    } while (0)

#define SHANI_NEXT_MSG(w0, w1, w2, w3)                                                             \
    do {                                                                                           \
        (w0) = _mm_sha256msg1_epu32((w0), (w1));                                                   \
        (w0) = _mm_add_epi32((w0), _mm_alignr_epi8((w3), (w2), 4));                                \
        (w0) = _mm_sha256msg2_epu32((w0), (w3));                                                   \
    } while (0)

    for (size_t blk = 0; blk < num_blocks; blk++) {
        const uint8_t *ptr = data + (blk * 64);
        __m128i abef_save = abef;
        __m128i cdgh_save = cdgh;

        __m128i msg0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(ptr + 0)), bswap_mask);
        __m128i msg1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(ptr + 16)), bswap_mask);
        __m128i msg2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(ptr + 32)), bswap_mask);
        __m128i msg3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(ptr + 48)), bswap_mask);

        SHANI_4ROUNDS(msg0, 0);
        SHANI_4ROUNDS(msg1, 1);
        SHANI_4ROUNDS(msg2, 2);
        SHANI_4ROUNDS(msg3, 3);

        SHANI_NEXT_MSG(msg0, msg1, msg2, msg3);
        SHANI_4ROUNDS(msg0, 4);
        SHANI_NEXT_MSG(msg1, msg2, msg3, msg0);
        SHANI_4ROUNDS(msg1, 5);
        SHANI_NEXT_MSG(msg2, msg3, msg0, msg1);
        SHANI_4ROUNDS(msg2, 6);
        SHANI_NEXT_MSG(msg3, msg0, msg1, msg2);
        SHANI_4ROUNDS(msg3, 7);

        SHANI_NEXT_MSG(msg0, msg1, msg2, msg3);
        SHANI_4ROUNDS(msg0, 8);
        SHANI_NEXT_MSG(msg1, msg2, msg3, msg0);
        SHANI_4ROUNDS(msg1, 9);
        SHANI_NEXT_MSG(msg2, msg3, msg0, msg1);
        SHANI_4ROUNDS(msg2, 10);
        SHANI_NEXT_MSG(msg3, msg0, msg1, msg2);
        SHANI_4ROUNDS(msg3, 11);

        SHANI_NEXT_MSG(msg0, msg1, msg2, msg3);
        SHANI_4ROUNDS(msg0, 12);
        SHANI_NEXT_MSG(msg1, msg2, msg3, msg0);
        SHANI_4ROUNDS(msg1, 13);
        SHANI_NEXT_MSG(msg2, msg3, msg0, msg1);
        SHANI_4ROUNDS(msg2, 14);
        SHANI_NEXT_MSG(msg3, msg0, msg1, msg2);
        SHANI_4ROUNDS(msg3, 15);

        abef = _mm_add_epi32(abef, abef_save);
        cdgh = _mm_add_epi32(cdgh, cdgh_save);
    }

#undef SHANI_NEXT_MSG
#undef SHANI_4ROUNDS

    state[0] = (uint32_t)_mm_extract_epi32(abef, 3);
    state[1] = (uint32_t)_mm_extract_epi32(abef, 2);
    state[2] = (uint32_t)_mm_extract_epi32(cdgh, 3);
    state[3] = (uint32_t)_mm_extract_epi32(cdgh, 2);
    state[4] = (uint32_t)_mm_extract_epi32(abef, 1);
    state[5] = (uint32_t)_mm_extract_epi32(abef, 0);
    state[6] = (uint32_t)_mm_extract_epi32(cdgh, 1);
    state[7] = (uint32_t)_mm_extract_epi32(cdgh, 0);
}
#endif

static inline void sha256_process_blocks(uint32_t state[8], const uint8_t *data, size_t num_blocks)
{
    if (num_blocks == 0) {
        return;
    }
#if defined(__x86_64__) || defined(_M_X64)
    pthread_once(&g_shani_once, shani_detect_once);
    if (g_shani_supported) {
        sha256_blocks_shani(state, data, num_blocks);
        return;
    }
#endif
    sha256_blocks_scalar(state, data, num_blocks);
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->count = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (len == 0 || !data) {
        return;
    }
    size_t idx = (size_t)(ctx->count & 63u);
    ctx->count += len;

    if (idx > 0) {
        size_t fill = 64u - idx;
        if (len < fill) {
            memcpy(ctx->buffer + idx, data, len);
            return;
        }
        memcpy(ctx->buffer + idx, data, fill);
        sha256_process_blocks(ctx->state, ctx->buffer, 1);
        data += fill;
        len -= fill;
    }

    size_t blocks = len / 64u;
    if (blocks > 0) {
        sha256_process_blocks(ctx->state, data, blocks);
        data += blocks * 64u;
        len -= blocks * 64u;
    }

    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    size_t idx = (size_t)(ctx->count & 63u);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        sha256_process_blocks(ctx->state, ctx->buffer, 1);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    uint64_t bit_len = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha256_process_blocks(ctx->state, ctx->buffer, 1);
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void wwpkg_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    if (!out) {
        return;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (len > 0 && data) {
        sha256_update(&ctx, data, len);
    }
    sha256_final(&ctx, out);
}

/* ========================================================================= */
/* Self-Contained FIPS 180-4 SHA-512 Implementation (For Ed25519)            */
/* ========================================================================= */

typedef struct {
    uint64_t state[8];
    uint64_t count;
    uint8_t buffer[128];
} sha512_ctx_t;

static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static void sha512_init(sha512_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->count = 0;
}

static void sha512_transform(uint64_t state[8], const uint8_t block[128])
{
    uint64_t W[80];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint64_t)block[i * 8 + 0] << 56) | ((uint64_t)block[i * 8 + 1] << 48) |
               ((uint64_t)block[i * 8 + 2] << 40) | ((uint64_t)block[i * 8 + 3] << 32) |
               ((uint64_t)block[i * 8 + 4] << 24) | ((uint64_t)block[i * 8 + 5] << 16) |
               ((uint64_t)block[i * 8 + 6] << 8) | ((uint64_t)block[i * 8 + 7]);
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(W[i - 15], 1) ^ ror64(W[i - 15], 8) ^ (W[i - 15] >> 7);
        uint64_t s1 = ror64(W[i - 2], 19) ^ ror64(W[i - 2], 61) ^ (W[i - 2] >> 6);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t temp1 = h + S1 + ch + K512[i] + W[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }
    size_t idx = (size_t)(ctx->count % 128);
    ctx->count += len;
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[idx++] = data[i];
        if (idx == 128) {
            sha512_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha512_final(sha512_ctx_t *ctx, uint8_t out[64])
{
    size_t idx = (size_t)(ctx->count % 128);
    ctx->buffer[idx++] = 0x80;
    if (idx > 112) {
        memset(ctx->buffer + idx, 0, 128 - idx);
        sha512_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 112 - idx);
    uint64_t bit_len = ctx->count * 8;
    memset(ctx->buffer + 112, 0, 8);
    for (int i = 0; i < 8; i++) {
        ctx->buffer[120 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha512_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(ctx->state[i] >> (56 - j * 8));
        }
    }
}

static void sha512_hash(uint8_t *out, const uint8_t *m, size_t n)
{
    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, m, n);
    sha512_final(&ctx, out);
}

/* ========================================================================= */
/* RFC 8032 Ed25519 Primitives (Strict Canonical S < L & Libsodium Fastpath) */
/* ========================================================================= */

typedef int64_t gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                     0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                      0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                     0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                     0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                     0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static const uint8_t L25519[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                   0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

/* RFC 8032 requires 0 <= S < L in constant time to prevent signature malleability */
static bool ed25519_scalar_is_canonical(const uint8_t s[32])
{
    if ((s[31] & 0xE0u) != 0) {
        return false;
    }
    uint32_t borrow = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t diff = (uint32_t)s[i] - (uint32_t)L25519[i] - borrow;
        borrow = (diff >> 31) & 1u;
    }
    return borrow == 1u;
}

/* Rejects non-canonical field encodings y >= 2^255 - 19 */
static bool ed25519_field_is_canonical(const uint8_t enc[32])
{
    uint32_t borrow = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t p_i = (i == 0) ? 0xedu : ((i == 31) ? 0x7fu : 0xffu);
        uint32_t y_i = (i == 31) ? ((uint32_t)enc[31] & 0x7fu) : (uint32_t)enc[i];
        uint32_t diff = y_i - p_i - borrow;
        borrow = (diff >> 31) & 1u;
    }
    return borrow == 1u;
}

static void set25519(gf r, const gf a)
{
    for (int i = 0; i < 16; i++) {
        r[i] = a[i];
    }
}

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (int64_t)((uint64_t)c << 16);
    }
}

static void sel25519(gf p, gf q, int b)
{
    uint64_t c = ~(uint64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        uint64_t t = c & ((uint64_t)p[i] ^ (uint64_t)q[i]);
        p[i] ^= (int64_t)t;
        q[i] ^= (int64_t)t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) {
        t[i] = n[i];
    }
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)t[i];
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) {
        o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] + b[i];
    }
}

static void Z(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] - b[i];
    }
}

static void M(gf o, const gf a, const gf b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) {
        o[i] = t[i];
    }
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++) {
        c[a] = i[a];
    }
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) {
            M(c, c, i);
        }
    }
    for (int a = 0; a < 16; a++) {
        o[a] = c[a];
    }
}

static int neq25519(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (c[i] ^ d[i]);
    }
    return diff != 0;
}

static void cswap(gf p[4], gf q[4], uint8_t b)
{
    for (int i = 0; i < 4; i++) {
        sel25519(p[i], q[i], b);
    }
}

static void pack(uint8_t *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void pow2523(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++) {
        c[a] = i[a];
    }
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) {
            M(c, c, i);
        }
    }
    for (int a = 0; a < 16; a++) {
        o[a] = c[a];
    }
}

static void add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t *s)
{
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static void modL(uint8_t *r, int64_t x[64])
{
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L25519[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= (int64_t)((uint64_t)carry << 8);
        }
        x[j] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L25519[j];
        carry = x[j] >> 8;
        x[j] &= 0xff;
    }
    for (int j = 0; j < 32; j++) {
        x[j] -= carry * L25519[j];
    }
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 0xff);
    }
}

static void reduce(uint8_t *r)
{
    int64_t x[64] = {0};
    for (int i = 0; i < 64; i++) {
        x[i] = (uint64_t)r[i];
    }
    for (int i = 0; i < 64; i++) {
        r[i] = 0;
    }
    modL(r, x);
    explicit_bzero(x, sizeof(x));
}

static int unpackneg(gf r[4], const uint8_t p[32])
{
    if (!ed25519_field_is_canonical(p)) {
        return -1;
    }

    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(r[0], t, den2);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) {
        M(r[0], r[0], I);
    }

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) {
        return -1;
    }

    if (par25519(r[0]) == (p[31] >> 7)) {
        Z(r[0], gf0, r[0]);
    }

    M(r[3], r[0], r[1]);
    return 0;
}

static int tweetnacl_ed25519_keypair(uint8_t pk[32], uint8_t sk[64])
{
    uint8_t d[64];
    gf p[4];
    sha512_hash(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
    scalarbase(p, d);
    pack(pk, p);
    memcpy(sk + 32, pk, 32);
    explicit_bzero(d, sizeof(d));
    return 0;
}

static int tweetnacl_ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t mlen,
                                  const uint8_t sk[64])
{
    uint8_t d[64], hram[64], r[64];
    gf p[4];
    sha512_hash(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, d + 32, 32);
    sha512_update(&ctx, m, mlen);
    sha512_final(&ctx, r);

    reduce(r);
    scalarbase(p, r);
    pack(sig, p);

    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, sk + 32, 32);
    sha512_update(&ctx, m, mlen);
    sha512_final(&ctx, hram);

    reduce(hram);
    int64_t x[64] = {0};
    for (int i = 0; i < 32; i++) {
        x[i] = (uint64_t)r[i];
    }
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) {
            x[i + j] += (uint64_t)hram[i] * (uint64_t)d[j];
        }
    }
    modL(sig + 32, x);
    explicit_bzero(d, sizeof(d));
    explicit_bzero(r, sizeof(r));
    explicit_bzero(hram, sizeof(hram));
    explicit_bzero(x, sizeof(x));
    return 0;
}

static int tweetnacl_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen,
                                    const uint8_t pk[32])
{
    if (!ed25519_field_is_canonical(sig)) {
        return -1;
    }

    uint8_t h[64], rcopy[32];
    gf p[4], q[4];

    if (unpackneg(q, pk) != 0) {
        return -1;
    }

    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    sha512_update(&ctx, m, mlen);
    sha512_final(&ctx, h);

    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    pack(rcopy, p);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (rcopy[i] ^ sig[i]);
    }
    return (diff == 0) ? 0 : -1;
}

int wwpkg_ed25519_seed_keypair(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32])
{
    if (!pk || !sk || !seed) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return -1;
    }
    uint8_t seed_copy[32];
    memcpy(seed_copy, seed, 32);

    sodium_probe();
    int rc = 0;
    if (g_sodium_available) {
        rc = g_sodium.crypto_sign_ed25519_seed_keypair(pk, sk, seed_copy);
    } else {
        memcpy(sk, seed_copy, 32);
        rc = tweetnacl_ed25519_keypair(pk, sk);
    }
    explicit_bzero(seed_copy, sizeof(seed_copy));
    return rc;
}

int wwpkg_ed25519_keypair(uint8_t pk[32], uint8_t sk[64])
{
    if (!pk || !sk) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return -1;
    }
    uint8_t seed[32];
    if (secure_random_bytes(seed, sizeof(seed)) != 0) {
        set_tls_error(WWPKG_ERR_IO);
        return -1;
    }
    int rc = wwpkg_ed25519_seed_keypair(pk, sk, seed);
    explicit_bzero(seed, sizeof(seed));
    return rc;
}

int wwpkg_ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t mlen, const uint8_t sk[64])
{
    if (!sig || !sk || (!m && mlen > 0)) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return -1;
    }

    /* Expand 32-byte seed into 64-byte sk if caller supplied zeroed pubkey half */
    uint8_t effective_sk[64];
    bool pub_zero = true;
    for (int i = 0; i < 32; i++) {
        if (sk[32 + i] != 0) {
            pub_zero = false;
            break;
        }
    }
    if (pub_zero) {
        uint8_t derived_pk[32];
        if (wwpkg_ed25519_seed_keypair(derived_pk, effective_sk, sk) != 0) {
            explicit_bzero(effective_sk, sizeof(effective_sk));
            return -1;
        }
    } else {
        memcpy(effective_sk, sk, 64);
    }

    static const uint8_t empty_msg = 0;
    const uint8_t *msg_ptr = m ? m : &empty_msg;

    sodium_probe();
    int rc = 0;
    if (g_sodium_available) {
        rc = g_sodium.crypto_sign_ed25519_detached(sig, nullptr, msg_ptr, (unsigned long long)mlen,
                                                   effective_sk);
    } else {
        rc = tweetnacl_ed25519_sign(sig, msg_ptr, mlen, effective_sk);
    }
    explicit_bzero(effective_sk, sizeof(effective_sk));
    return rc;
}

int wwpkg_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen, const uint8_t pk[32])
{
    if (!sig || !pk || (!m && mlen > 0)) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return -1;
    }

    if (!ed25519_scalar_is_canonical(sig + 32)) {
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return -1;
    }

    static const uint8_t empty_msg = 0;
    const uint8_t *msg_ptr = m ? m : &empty_msg;

    sodium_probe();
    if (g_sodium_available) {
        int rc = g_sodium.crypto_sign_ed25519_verify_detached(sig, msg_ptr,
                                                              (unsigned long long)mlen, pk);
        if (rc != 0) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        }
        return rc;
    }

    int rc = tweetnacl_ed25519_verify(sig, msg_ptr, mlen, pk);
    if (rc != 0) {
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
    }
    return rc;
}

void wwpkg_generate_keypair(uint8_t pub[32], uint8_t priv[64]) { wwpkg_ed25519_keypair(pub, priv); }

/* ========================================================================= */
/* Hardened RFC 8785 JSON Canonicalization Scheme (JCS) Engine               */
/* ========================================================================= */

#define JCS_MAX_DEPTH 64u

typedef struct jcs_arena_chunk {
    struct jcs_arena_chunk *next;
    size_t used;
    size_t cap;
    alignas(8) uint8_t data[];
} jcs_arena_chunk_t;

typedef struct {
    jcs_arena_chunk_t *head;
    uint8_t *fast_buf;
    size_t fast_cap;
    size_t fast_used;
} jcs_arena_t;

static _Thread_local alignas(8) uint8_t tls_jcs_arena_buf[16384];
static _Thread_local bool tls_jcs_arena_busy = false;

static inline void jcs_arena_init(jcs_arena_t *arena)
{
    arena->head = nullptr;
    arena->fast_used = 0;
    if (!tls_jcs_arena_busy) {
        tls_jcs_arena_busy = true;
        arena->fast_buf = tls_jcs_arena_buf;
        arena->fast_cap = sizeof(tls_jcs_arena_buf);
    } else {
        arena->fast_buf = nullptr;
        arena->fast_cap = 0;
    }
}

static void *jcs_arena_alloc_raw(jcs_arena_t *arena, size_t size, bool zero_init)
{
    size_t aligned = (size + 7u) & ~7u;
    if (arena->fast_buf && arena->fast_cap - arena->fast_used >= aligned) {
        void *ptr = arena->fast_buf + arena->fast_used;
        arena->fast_used += aligned;
        if (zero_init && size > 0) {
            memset(ptr, 0, size);
        }
        return ptr;
    }
    if (!arena->head || arena->head->cap - arena->head->used < aligned) {
        size_t chunk_cap = aligned > 4096u ? (aligned + 1024u) : 4096u;
        jcs_arena_chunk_t *chunk =
            (jcs_arena_chunk_t *)malloc(sizeof(jcs_arena_chunk_t) + chunk_cap);
        if (!chunk) {
            return nullptr;
        }
        chunk->next = arena->head;
        chunk->used = 0;
        chunk->cap = chunk_cap;
        arena->head = chunk;
    }
    void *ptr = arena->head->data + arena->head->used;
    arena->head->used += aligned;
    if (zero_init && size > 0) {
        memset(ptr, 0, size);
    }
    return ptr;
}

static inline void *jcs_arena_alloc(jcs_arena_t *arena, size_t size)
{
    return jcs_arena_alloc_raw(arena, size, true);
}

static inline void *jcs_arena_alloc_nozero(jcs_arena_t *arena, size_t size)
{
    return jcs_arena_alloc_raw(arena, size, false);
}

static void jcs_arena_free(jcs_arena_t *arena)
{
    jcs_arena_chunk_t *cur = arena->head;
    while (cur) {
        jcs_arena_chunk_t *next = cur->next;
        free(cur);
        cur = next;
    }
    if (arena->fast_buf == tls_jcs_arena_buf) {
        tls_jcs_arena_busy = false;
    }
    arena->head = nullptr;
    arena->fast_buf = nullptr;
    arena->fast_cap = 0;
    arena->fast_used = 0;
}

typedef enum {
    JCS_TYPE_NULL = 0,
    JCS_TYPE_BOOL,
    JCS_TYPE_NUM,
    JCS_TYPE_STR,
    JCS_TYPE_ARR,
    JCS_TYPE_OBJ,
} jcs_type_t;

typedef struct jcs_val jcs_val_t;

typedef struct {
    char *key;
    size_t key_len;
    jcs_val_t *val;
} jcs_kv_t;

struct jcs_val {
    jcs_type_t type;
    bool bval;
    double dval;
    int64_t ival;
    uint64_t uval;
    bool is_int;
    bool is_uint;
    char *sval;
    size_t slen;
    struct {
        jcs_val_t **items;
        size_t count;
    } arr;
    struct {
        jcs_kv_t *entries;
        size_t count;
    } obj;
};

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    jcs_arena_t *arena;
} jcs_lexer_t;

static inline int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static bool parse_hex4(const char *p, uint32_t *out_cp)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int nib = hex_nibble(p[i]);
        if (nib < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)nib;
    }
    *out_cp = v;
    return true;
}

static void jcs_skip_ws(jcs_lexer_t *lx)
{
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lx->pos++;
        } else {
            break;
        }
    }
}

/* Validates raw UTF-8 sequence inside unescaped JSON string bytes */
static size_t valid_utf8_seq_len(const uint8_t *s, size_t rem)
{
    if (rem == 0)
        return 0;
    uint8_t b0 = s[0];
    if (b0 < 0x80u) {
        return b0 >= 0x20u ? 1u : 0u;
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        if (rem < 2 || b0 < 0xC2u || (s[1] & 0xC0u) != 0x80u)
            return 0;
        return 2u;
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        if (rem < 3 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u)
            return 0;
        uint32_t cp =
            ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            return 0;
        return 3u;
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        if (rem < 4 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u ||
            (s[3] & 0xC0u) != 0x80u)
            return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
                      ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu)
            return 0;
        return 4u;
    }
    return 0;
}

__attribute__((noinline)) static bool jcs_parse_str_slow(jcs_lexer_t *lx, char **out_str,
                                                         size_t *out_len)
{
    size_t cap = 64;
    size_t len = 0;
    char stack_tmp[256];
    char *heap_tmp = nullptr;
    char *buf = stack_tmp;
    cap = sizeof(stack_tmp);

    while (lx->pos < lx->len) {
        uint8_t c = (uint8_t)lx->src[lx->pos];
        if (c == '"') {
            lx->pos++;
            char *res = (char *)jcs_arena_alloc_nozero(lx->arena, len + 1);
            if (!res) {
                free(heap_tmp);
                return false;
            }
            if (len > 0) {
                memcpy(res, buf, len);
            }
            res[len] = '\0';
            free(heap_tmp);
            *out_str = res;
            *out_len = len;
            return true;
        }

        if (c < 0x20u) {
            /* RFC 8259 forbids raw unescaped control characters in JSON strings */
            free(heap_tmp);
            return false;
        }

        if (c == '\\') {
            lx->pos++;
            if (lx->pos >= lx->len) {
                free(heap_tmp);
                return false;
            }
            char esc = lx->src[lx->pos++];
            char decoded_c = 0;
            if (esc == '"')
                decoded_c = '"';
            else if (esc == '\\')
                decoded_c = '\\';
            else if (esc == '/')
                decoded_c = '/';
            else if (esc == 'b')
                decoded_c = '\b';
            else if (esc == 'f')
                decoded_c = '\f';
            else if (esc == 'n')
                decoded_c = '\n';
            else if (esc == 'r')
                decoded_c = '\r';
            else if (esc == 't')
                decoded_c = '\t';
            else if (esc == 'u') {
                if (lx->len - lx->pos < 4) {
                    free(heap_tmp);
                    return false;
                }
                uint32_t cp = 0;
                if (!parse_hex4(lx->src + lx->pos, &cp)) {
                    free(heap_tmp);
                    return false;
                }
                lx->pos += 4;

                /* Reject lone low surrogates and require paired low surrogate for high surrogate */
                if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    free(heap_tmp);
                    return false;
                }
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    if (lx->len - lx->pos < 6 || lx->src[lx->pos] != '\\' ||
                        lx->src[lx->pos + 1] != 'u') {
                        free(heap_tmp);
                        return false;
                    }
                    uint32_t cp2 = 0;
                    if (!parse_hex4(lx->src + lx->pos + 2, &cp2) || cp2 < 0xDC00u ||
                        cp2 > 0xDFFFu) {
                        free(heap_tmp);
                        return false;
                    }
                    lx->pos += 6;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (cp2 - 0xDC00u));
                }

                char u8[4];
                size_t u8len = 0;
                if (cp <= 0x7Fu) {
                    u8[0] = (char)cp;
                    u8len = 1;
                } else if (cp <= 0x7FFu) {
                    u8[0] = (char)(0xC0u | (cp >> 6));
                    u8[1] = (char)(0x80u | (cp & 0x3Fu));
                    u8len = 2;
                } else if (cp <= 0xFFFFu) {
                    u8[0] = (char)(0xE0u | (cp >> 12));
                    u8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    u8[2] = (char)(0x80u | (cp & 0x3Fu));
                    u8len = 3;
                } else {
                    u8[0] = (char)(0xF0u | (cp >> 18));
                    u8[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                    u8[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    u8[3] = (char)(0x80u | (cp & 0x3Fu));
                    u8len = 4;
                }

                if (len + u8len + 1 >= cap) {
                    size_t ncap = (cap * 2) + u8len + 16;
                    char *nb = (char *)(heap_tmp ? realloc(heap_tmp, ncap) : malloc(ncap));
                    if (!nb) {
                        free(heap_tmp);
                        return false;
                    }
                    if (!heap_tmp && len > 0) {
                        memcpy(nb, stack_tmp, len);
                    }
                    heap_tmp = nb;
                    buf = nb;
                    cap = ncap;
                }
                memcpy(buf + len, u8, u8len);
                len += u8len;
                continue;
            } else {
                free(heap_tmp);
                return false;
            }

            if (len + 2 >= cap) {
                size_t ncap = cap * 2;
                char *nb = (char *)(heap_tmp ? realloc(heap_tmp, ncap) : malloc(ncap));
                if (!nb) {
                    free(heap_tmp);
                    return false;
                }
                if (!heap_tmp && len > 0) {
                    memcpy(nb, stack_tmp, len);
                }
                heap_tmp = nb;
                buf = nb;
                cap = ncap;
            }
            buf[len++] = decoded_c;
            continue;
        }

        size_t seq = valid_utf8_seq_len((const uint8_t *)(lx->src + lx->pos), lx->len - lx->pos);
        if (seq == 0) {
            free(heap_tmp);
            return false;
        }
        if (len + seq + 1 >= cap) {
            size_t ncap = (cap * 2) + seq + 16;
            char *nb = (char *)(heap_tmp ? realloc(heap_tmp, ncap) : malloc(ncap));
            if (!nb) {
                free(heap_tmp);
                return false;
            }
            if (!heap_tmp && len > 0) {
                memcpy(nb, stack_tmp, len);
            }
            heap_tmp = nb;
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, lx->src + lx->pos, seq);
        len += seq;
        lx->pos += seq;
    }

    free(heap_tmp);
    return false;
}

static bool jcs_parse_str_raw(jcs_lexer_t *lx, char **out_str, size_t *out_len)
{
    jcs_skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
        return false;
    }
    lx->pos++;
    size_t start = lx->pos;
    while (lx->pos < lx->len) {
        uint8_t c = (uint8_t)lx->src[lx->pos];
        if (c == '"') {
            size_t slen = lx->pos - start;
            lx->pos++;
            char *res = (char *)jcs_arena_alloc_nozero(lx->arena, slen + 1);
            if (!res) {
                return false;
            }
            if (slen > 0) {
                memcpy(res, lx->src + start, slen);
            }
            res[slen] = '\0';
            *out_str = res;
            *out_len = slen;
            return true;
        }
        if (c < 0x20u) {
            return false;
        }
        if (c == '\\' || c >= 0x80u) {
            lx->pos = start;
            return jcs_parse_str_slow(lx, out_str, out_len);
        }
        lx->pos++;
    }
    return false;
}

/* RFC 8785 UTF-16 code unit key ordering using explicit byte lengths */
static int jcs_key_cmp(const void *a, const void *b)
{
    const jcs_kv_t *ka = (const jcs_kv_t *)a;
    const jcs_kv_t *kb = (const jcs_kv_t *)b;
    const uint8_t *s1 = (const uint8_t *)ka->key;
    const uint8_t *e1 = s1 + ka->key_len;
    const uint8_t *s2 = (const uint8_t *)kb->key;
    const uint8_t *e2 = s2 + kb->key_len;

    while (s1 < e1 && s2 < e2 && ((*s1 | *s2) < 0x80u)) {
        if (*s1 != *s2) {
            return *s1 < *s2 ? -1 : 1;
        }
        s1++;
        s2++;
    }

    while (s1 < e1 && s2 < e2) {
        uint32_t cp1 = 0, cp2 = 0;
        size_t l1 = 1, l2 = 1;

        if (*s1 < 0x80u) {
            cp1 = *s1;
        } else if ((*s1 & 0xE0u) == 0xC0u) {
            cp1 = ((uint32_t)(*s1 & 0x1Fu) << 6) | (s1[1] & 0x3Fu);
            l1 = 2;
        } else if ((*s1 & 0xF0u) == 0xE0u) {
            cp1 = ((uint32_t)(*s1 & 0x0Fu) << 12) | ((uint32_t)(s1[1] & 0x3Fu) << 6) |
                  (s1[2] & 0x3Fu);
            l1 = 3;
        } else {
            cp1 = ((uint32_t)(*s1 & 0x07u) << 18) | ((uint32_t)(s1[1] & 0x3Fu) << 12) |
                  ((uint32_t)(s1[2] & 0x3Fu) << 6) | (s1[3] & 0x3Fu);
            l1 = 4;
        }

        if (*s2 < 0x80u) {
            cp2 = *s2;
        } else if ((*s2 & 0xE0u) == 0xC0u) {
            cp2 = ((uint32_t)(*s2 & 0x1Fu) << 6) | (s2[1] & 0x3Fu);
            l2 = 2;
        } else if ((*s2 & 0xF0u) == 0xE0u) {
            cp2 = ((uint32_t)(*s2 & 0x0Fu) << 12) | ((uint32_t)(s2[1] & 0x3Fu) << 6) |
                  (s2[2] & 0x3Fu);
            l2 = 3;
        } else {
            cp2 = ((uint32_t)(*s2 & 0x07u) << 18) | ((uint32_t)(s2[1] & 0x3Fu) << 12) |
                  ((uint32_t)(s2[2] & 0x3Fu) << 6) | (s2[3] & 0x3Fu);
            l2 = 4;
        }

        uint16_t cu1[2], cu2[2];
        size_t n1 = 1, n2 = 1;
        if (cp1 <= 0xFFFFu) {
            cu1[0] = (uint16_t)cp1;
        } else {
            cu1[0] = (uint16_t)(0xD800u + ((cp1 - 0x10000u) >> 10));
            cu1[1] = (uint16_t)(0xDC00u + ((cp1 - 0x10000u) & 0x3FFu));
            n1 = 2;
        }
        if (cp2 <= 0xFFFFu) {
            cu2[0] = (uint16_t)cp2;
        } else {
            cu2[0] = (uint16_t)(0xD800u + ((cp2 - 0x10000u) >> 10));
            cu2[1] = (uint16_t)(0xDC00u + ((cp2 - 0x10000u) & 0x3FFu));
            n2 = 2;
        }

        size_t min_n = n1 < n2 ? n1 : n2;
        for (size_t i = 0; i < min_n; i++) {
            if (cu1[i] != cu2[i]) {
                return cu1[i] < cu2[i] ? -1 : 1;
            }
        }
        if (n1 != n2) {
            return n1 < n2 ? -1 : 1;
        }
        s1 += l1;
        s2 += l2;
    }
    if (s1 < e1)
        return 1;
    if (s2 < e2)
        return -1;
    return 0;
}

static jcs_val_t *jcs_parse_val(jcs_lexer_t *lx, uint32_t depth);

__attribute__((noinline)) static jcs_val_t *jcs_parse_obj(jcs_lexer_t *lx, uint32_t depth)
{
    lx->pos++;
    jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
    if (!v) {
        return nullptr;
    }
    v->type = JCS_TYPE_OBJ;
    jcs_skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
        lx->pos++;
        return v;
    }

    size_t cap = 24;
    jcs_kv_t *arena_init_entries =
        (jcs_kv_t *)jcs_arena_alloc_nozero(lx->arena, cap * sizeof(jcs_kv_t));
    if (!arena_init_entries) {
        return nullptr;
    }
    jcs_kv_t *heap_entries = nullptr;
    jcs_kv_t *tmp_entries = arena_init_entries;
    size_t count = 0;

    while (lx->pos < lx->len) {
        char *key = nullptr;
        size_t key_len = 0;
        if (!jcs_parse_str_raw(lx, &key, &key_len)) {
            free(heap_entries);
            return nullptr;
        }
        jcs_skip_ws(lx);
        if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
            free(heap_entries);
            return nullptr;
        }
        lx->pos++;

        jcs_val_t *val = jcs_parse_val(lx, depth + 1);
        if (!val) {
            free(heap_entries);
            return nullptr;
        }

        if (count >= cap) {
            size_t ncap = cap * 2;
            jcs_kv_t *ne =
                (jcs_kv_t *)(heap_entries ? realloc(heap_entries, ncap * sizeof(jcs_kv_t))
                                          : malloc(ncap * sizeof(jcs_kv_t)));
            if (!ne) {
                free(heap_entries);
                return nullptr;
            }
            if (!heap_entries) {
                memcpy(ne, arena_init_entries, count * sizeof(jcs_kv_t));
            }
            heap_entries = ne;
            tmp_entries = ne;
            cap = ncap;
        }
        tmp_entries[count].key = key;
        tmp_entries[count].key_len = key_len;
        tmp_entries[count].val = val;
        count++;

        jcs_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
            jcs_skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
                free(heap_entries);
                return nullptr;
            }
            continue;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
            lx->pos++;
            break;
        }
        free(heap_entries);
        return nullptr;
    }

    if (count > 1) {
        qsort(tmp_entries, count, sizeof(jcs_kv_t), jcs_key_cmp);
        for (size_t i = 1; i < count; i++) {
            if (tmp_entries[i - 1].key_len == tmp_entries[i].key_len &&
                memcmp(tmp_entries[i - 1].key, tmp_entries[i].key, tmp_entries[i].key_len) == 0) {
                /* RFC 8785 / I-JSON rejects duplicate object member names */
                free(heap_entries);
                return nullptr;
            }
        }
    }

    if (!heap_entries) {
        v->obj.entries = arena_init_entries;
        v->obj.count = count;
        return v;
    }

    jcs_kv_t *arena_entries =
        (jcs_kv_t *)jcs_arena_alloc_nozero(lx->arena, count * sizeof(jcs_kv_t));
    if (!arena_entries) {
        free(heap_entries);
        return nullptr;
    }
    memcpy(arena_entries, heap_entries, count * sizeof(jcs_kv_t));
    free(heap_entries);
    v->obj.entries = arena_entries;
    v->obj.count = count;
    return v;
}

__attribute__((noinline)) static jcs_val_t *jcs_parse_arr(jcs_lexer_t *lx, uint32_t depth)
{
    lx->pos++;
    jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
    if (!v) {
        return nullptr;
    }
    v->type = JCS_TYPE_ARR;
    jcs_skip_ws(lx);
    if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
        lx->pos++;
        return v;
    }

    size_t cap = 16;
    jcs_val_t **arena_init_items =
        (jcs_val_t **)jcs_arena_alloc_nozero(lx->arena, cap * sizeof(jcs_val_t *));
    if (!arena_init_items) {
        return nullptr;
    }
    jcs_val_t **heap_items = nullptr;
    jcs_val_t **tmp_items = arena_init_items;
    size_t count = 0;

    while (lx->pos < lx->len) {
        jcs_val_t *item = jcs_parse_val(lx, depth + 1);
        if (!item) {
            free(heap_items);
            return nullptr;
        }
        if (count >= cap) {
            size_t ncap = cap * 2;
            jcs_val_t **ni =
                (jcs_val_t **)(heap_items ? realloc(heap_items, ncap * sizeof(jcs_val_t *))
                                          : malloc(ncap * sizeof(jcs_val_t *)));
            if (!ni) {
                free(heap_items);
                return nullptr;
            }
            if (!heap_items) {
                memcpy(ni, arena_init_items, count * sizeof(jcs_val_t *));
            }
            heap_items = ni;
            tmp_items = ni;
            cap = ncap;
        }
        tmp_items[count++] = item;

        jcs_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
            lx->pos++;
            jcs_skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] == ']') {
                free(heap_items);
                return nullptr;
            }
            continue;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
            lx->pos++;
            break;
        }
        free(heap_items);
        return nullptr;
    }

    if (!heap_items) {
        v->arr.items = arena_init_items;
        v->arr.count = count;
        return v;
    }

    jcs_val_t **arena_items =
        (jcs_val_t **)jcs_arena_alloc_nozero(lx->arena, count * sizeof(jcs_val_t *));
    if (!arena_items) {
        free(heap_items);
        return nullptr;
    }
    memcpy(arena_items, heap_items, count * sizeof(jcs_val_t *));
    free(heap_items);
    v->arr.items = arena_items;
    v->arr.count = count;
    return v;
}

static jcs_val_t *jcs_parse_val(jcs_lexer_t *lx, uint32_t depth)
{
    if (depth > JCS_MAX_DEPTH) {
        return nullptr;
    }
    jcs_skip_ws(lx);
    if (lx->pos >= lx->len) {
        return nullptr;
    }
    char c = lx->src[lx->pos];

    if (c == '"') {
        char *str = nullptr;
        size_t slen = 0;
        if (!jcs_parse_str_raw(lx, &str, &slen)) {
            return nullptr;
        }
        jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
        if (!v) {
            return nullptr;
        }
        v->type = JCS_TYPE_STR;
        v->sval = str;
        v->slen = slen;
        return v;
    }

    if (c == '{') {
        return jcs_parse_obj(lx, depth);
    }

    if (c == '[') {
        return jcs_parse_arr(lx, depth);
    }

    size_t rem = lx->len - lx->pos;
    if (rem >= 4 && memcmp(lx->src + lx->pos, "true", 4) == 0) {
        lx->pos += 4;
        jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_BOOL;
        v->bval = true;
        return v;
    }
    if (rem >= 5 && memcmp(lx->src + lx->pos, "false", 5) == 0) {
        lx->pos += 5;
        jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_BOOL;
        v->bval = false;
        return v;
    }
    if (rem >= 4 && memcmp(lx->src + lx->pos, "null", 4) == 0) {
        lx->pos += 4;
        jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_NULL;
        return v;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = lx->pos;
        bool neg = false;
        bool is_float = false;

        if (lx->src[lx->pos] == '-') {
            neg = true;
            lx->pos++;
        }
        if (lx->pos >= lx->len || lx->src[lx->pos] < '0' || lx->src[lx->pos] > '9') {
            return nullptr;
        }
        if (lx->src[lx->pos] == '0') {
            lx->pos++;
            if (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') {
                /* Leading zeros prohibited by RFC 8259 */
                return nullptr;
            }
        } else {
            while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') {
                lx->pos++;
            }
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
            is_float = true;
            lx->pos++;
            if (lx->pos >= lx->len || lx->src[lx->pos] < '0' || lx->src[lx->pos] > '9') {
                return nullptr;
            }
            while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') {
                lx->pos++;
            }
        }
        if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
            is_float = true;
            lx->pos++;
            if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-')) {
                lx->pos++;
            }
            if (lx->pos >= lx->len || lx->src[lx->pos] < '0' || lx->src[lx->pos] > '9') {
                return nullptr;
            }
            while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') {
                lx->pos++;
            }
        }

        size_t nlen = lx->pos - start;
        char numbuf[128];
        if (nlen >= sizeof(numbuf)) {
            return nullptr;
        }
        memcpy(numbuf, lx->src + start, nlen);
        numbuf[nlen] = '\0';

        locale_t cloc = get_c_locale();
        errno = 0;
        char *endp = nullptr;
        double dval = cloc ? strtod_l(numbuf, &endp, cloc) : strtod(numbuf, &endp);
        if (endp != numbuf + nlen || !isfinite(dval)) {
            return nullptr;
        }

        jcs_val_t *v = (jcs_val_t *)jcs_arena_alloc(lx->arena, sizeof(jcs_val_t));
        if (!v) {
            return nullptr;
        }
        v->type = JCS_TYPE_NUM;
        v->dval = dval;

        if (!is_float) {
            errno = 0;
            if (!neg) {
                unsigned long long uv = strtoull(numbuf, &endp, 10);
                if (errno == 0 && endp == numbuf + nlen) {
                    v->uval = (uint64_t)uv;
                    v->is_uint = true;
                    if (uv <= (unsigned long long)INT64_MAX) {
                        v->ival = (int64_t)uv;
                        v->is_int = true;
                    }
                }
            } else {
                long long iv = strtoll(numbuf, &endp, 10);
                if (errno == 0 && endp == numbuf + nlen) {
                    v->ival = (int64_t)iv;
                    v->is_int = true;
                }
            }
        }
        return v;
    }

    return nullptr;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} jcs_out_buf_t;

static bool jcs_out_append(jcs_out_buf_t *b, const char *s, size_t n)
{
    if (n == 0) {
        return true;
    }
    if (n > SIZE_MAX - b->len - 65u) {
        return false;
    }
    if (b->len + n + 1u >= b->cap) {
        size_t min_need = b->len + n + 65u;
        size_t doubled = (b->cap <= SIZE_MAX / 2u) ? (b->cap * 2u) : SIZE_MAX;
        size_t ncap = (doubled >= min_need) ? doubled : min_need;
        char *nb = (char *)realloc(b->buf, ncap);
        if (!nb) {
            return false;
        }
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return true;
}

static inline bool jcs_out_append_lit(jcs_out_buf_t *b, const char *lit)
{
    return jcs_out_append(b, lit, strlen(lit));
}

static bool jcs_out_append_str(jcs_out_buf_t *b, const char *s, size_t slen)
{
    if (!jcs_out_append(b, "\"", 1))
        return false;
    for (size_t i = 0; i < slen; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '"') {
            if (!jcs_out_append(b, "\\\"", 2))
                return false;
        } else if (c == '\\') {
            if (!jcs_out_append(b, "\\\\", 2))
                return false;
        } else if (c == '\b') {
            if (!jcs_out_append(b, "\\b", 2))
                return false;
        } else if (c == '\f') {
            if (!jcs_out_append(b, "\\f", 2))
                return false;
        } else if (c == '\n') {
            if (!jcs_out_append(b, "\\n", 2))
                return false;
        } else if (c == '\r') {
            if (!jcs_out_append(b, "\\r", 2))
                return false;
        } else if (c == '\t') {
            if (!jcs_out_append(b, "\\t", 2))
                return false;
        } else if (c < 0x20u) {
            char esc[7];
            static const char hex[] = "0123456789abcdef";
            esc[0] = '\\';
            esc[1] = 'u';
            esc[2] = '0';
            esc[3] = '0';
            esc[4] = hex[(c >> 4) & 0x0Fu];
            esc[5] = hex[c & 0x0Fu];
            esc[6] = '\0';
            if (!jcs_out_append(b, esc, 6))
                return false;
        } else {
            if (!jcs_out_append(b, s + i, 1))
                return false;
        }
    }
    return jcs_out_append(b, "\"", 1);
}

static inline bool jcs_out_append_cstr(jcs_out_buf_t *b, const char *s)
{
    return jcs_out_append_str(b, s ? s : "", s ? strlen(s) : 0);
}

/* Locale-independent RFC 8785 / ECMA-262 IEEE-754 number formatter */
static bool jcs_format_ieee754_double(double val, char *out, size_t out_cap)
{
    if (!isfinite(val) || out_cap < 40) {
        return false;
    }
    if (val == 0.0) {
        out[0] = '0';
        out[1] = '\0';
        return true;
    }

    bool neg = signbit(val) != 0;
    double abs_v = fabs(val);

    locale_t cloc = get_c_locale();
    locale_t prev_loc = (locale_t)0;
    if (cloc != (locale_t)0) {
        prev_loc = uselocale(cloc);
    }

    char sci[64] = {0};
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec - 1, abs_v);
        if (prec == 17) {
            break;
        }
        double rt = cloc ? strtod_l(sci, nullptr, cloc) : strtod(sci, nullptr);
        if (rt == abs_v) {
            break;
        }
    }

    if (cloc != (locale_t)0) {
        uselocale(prev_loc);
    }

    char *epos = strchr(sci, 'e');
    if (!epos) {
        epos = strchr(sci, 'E');
    }
    if (!epos) {
        return false;
    }
    int exp10 = (int)strtol(epos + 1, nullptr, 10);

    char digits[24];
    int k = 0;
    for (char *p = sci; p < epos && k < 20; p++) {
        if (*p >= '0' && *p <= '9') {
            digits[k++] = *p;
        }
    }
    while (k > 1 && digits[k - 1] == '0') {
        k--;
    }
    digits[k] = '\0';

    int n = exp10 + 1;
    size_t pos = 0;
    if (neg) {
        out[pos++] = '-';
    }

    if (k <= n && n <= 21) {
        memcpy(out + pos, digits, (size_t)k);
        pos += (size_t)k;
        memset(out + pos, '0', (size_t)(n - k));
        pos += (size_t)(n - k);
        out[pos] = '\0';
    } else if (0 < n && n < k) {
        memcpy(out + pos, digits, (size_t)n);
        pos += (size_t)n;
        out[pos++] = '.';
        memcpy(out + pos, digits + n, (size_t)(k - n));
        pos += (size_t)(k - n);
        out[pos] = '\0';
    } else if (-5 <= n && n <= 0) {
        out[pos++] = '0';
        out[pos++] = '.';
        memset(out + pos, '0', (size_t)(-n));
        pos += (size_t)(-n);
        memcpy(out + pos, digits, (size_t)k);
        pos += (size_t)k;
        out[pos] = '\0';
    } else {
        out[pos++] = digits[0];
        if (k > 1) {
            out[pos++] = '.';
            memcpy(out + pos, digits + 1, (size_t)(k - 1));
            pos += (size_t)(k - 1);
        }
        out[pos++] = 'e';
        int e_val = n - 1;
        if (e_val >= 0) {
            out[pos++] = '+';
        } else {
            out[pos++] = '-';
            e_val = -e_val;
        }
        int written = snprintf(out + pos, out_cap - pos, "%d", e_val);
        if (written <= 0) {
            return false;
        }
        pos += (size_t)written;
    }
    return true;
}

static bool jcs_serialize_val(jcs_out_buf_t *b, const jcs_val_t *v)
{
    if (!v)
        return false;
    switch (v->type) {
    case JCS_TYPE_NULL:
        return jcs_out_append(b, "null", 4);
    case JCS_TYPE_BOOL:
        return v->bval ? jcs_out_append(b, "true", 4) : jcs_out_append(b, "false", 5);
    case JCS_TYPE_STR:
        return jcs_out_append_str(b, v->sval, v->slen);
    case JCS_TYPE_NUM: {
        char nbuf[64];
        /* Preserve exact integer formatting within IEEE-754 safe integer range [-2^53+1, 2^53-1] */
        if (v->is_int && v->ival >= -9007199254740991LL && v->ival <= 9007199254740991LL) {
            int n = snprintf(nbuf, sizeof(nbuf), "%lld", (v->ival == 0) ? 0LL : (long long)v->ival);
            if (n <= 0)
                return false;
            return jcs_out_append(b, nbuf, (size_t)n);
        }
        if (!jcs_format_ieee754_double(v->dval, nbuf, sizeof(nbuf))) {
            return false;
        }
        return jcs_out_append(b, nbuf, strlen(nbuf));
    }
    case JCS_TYPE_ARR: {
        if (!jcs_out_append(b, "[", 1))
            return false;
        for (size_t i = 0; i < v->arr.count; i++) {
            if (i > 0 && !jcs_out_append(b, ",", 1))
                return false;
            if (!jcs_serialize_val(b, v->arr.items[i]))
                return false;
        }
        return jcs_out_append(b, "]", 1);
    }
    case JCS_TYPE_OBJ: {
        if (!jcs_out_append(b, "{", 1))
            return false;
        for (size_t i = 0; i < v->obj.count; i++) {
            if (i > 0 && !jcs_out_append(b, ",", 1))
                return false;
            if (!jcs_out_append_str(b, v->obj.entries[i].key, v->obj.entries[i].key_len))
                return false;
            if (!jcs_out_append(b, ":", 1))
                return false;
            if (!jcs_serialize_val(b, v->obj.entries[i].val))
                return false;
        }
        return jcs_out_append(b, "}", 1);
    }
    default:
        return false;
    }
}

char *wwpkg_canonicalize_json(const char *json_src, size_t len, size_t *out_len)
{
    if (!json_src) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return nullptr;
    }
    size_t actual_len = len ? len : strlen(json_src);
    if (actual_len == 0) {
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return nullptr;
    }

    jcs_arena_t arena;
    jcs_arena_init(&arena);
    jcs_lexer_t lx = {.src = json_src, .len = actual_len, .pos = 0, .arena = &arena};
    jcs_val_t *root = jcs_parse_val(&lx, 0);
    if (!root) {
        jcs_arena_free(&arena);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return nullptr;
    }
    jcs_skip_ws(&lx);
    if (lx.pos != lx.len) {
        /* Trailing non-whitespace garbage after root JSON value is prohibited */
        jcs_arena_free(&arena);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return nullptr;
    }

    size_t init_cap = actual_len + 64;
    jcs_out_buf_t b = {.buf = (char *)malloc(init_cap), .len = 0, .cap = init_cap};
    if (!b.buf) {
        jcs_arena_free(&arena);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    b.buf[0] = '\0';

    if (!jcs_serialize_val(&b, root)) {
        free(b.buf);
        jcs_arena_free(&arena);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return nullptr;
    }
    jcs_arena_free(&arena);
    if (out_len) {
        *out_len = b.len;
    }
    return b.buf;
}

char *wwpkg_jcs_canonicalize(const char *json_src, size_t len, size_t *out_len)
{
    return wwpkg_canonicalize_json(json_src, len, out_len);
}

/* ========================================================================= */
/* Manifest Parser, Validator, and Escaped Canonical Serializer              */
/* ========================================================================= */

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t read_u64_le(const uint8_t *p)
{
    return ((uint64_t)read_u32_le(p)) | ((uint64_t)read_u32_le(p + 4) << 32);
}

static inline void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void write_u64_le(uint8_t *p, uint64_t v)
{
    write_u32_le(p, (uint32_t)(v & 0xFFFFFFFFULL));
    write_u32_le(p + 4, (uint32_t)(v >> 32));
}

static inline void write_u64_be(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - i * 8));
    }
}

static bool decode_hex32(const char *hex, size_t len, uint8_t out[32])
{
    if (!hex || len != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool sanitize_archive_path(const char *in, char *out, size_t out_cap);

static const jcs_val_t *jcs_find_member(const jcs_val_t *obj, const char *key)
{
    if (!obj || obj->type != JCS_TYPE_OBJ || !key) {
        return nullptr;
    }
    size_t klen = strlen(key);
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (obj->obj.entries[i].key_len == klen &&
            memcmp(obj->obj.entries[i].key, key, klen) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return nullptr;
}

static bool jcs_get_str_field(const jcs_val_t *obj, const char *key, char *dst, size_t max_len,
                              bool required)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v) {
        return !required;
    }
    if (v->type != JCS_TYPE_STR || v->slen >= max_len || strlen(v->sval) != v->slen) {
        return false;
    }
    memcpy(dst, v->sval, v->slen + 1);
    return true;
}

static bool jcs_get_bool_field(const jcs_val_t *obj, const char *key, bool *dst, bool required,
                               bool def)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v) {
        *dst = def;
        return !required;
    }
    if (v->type != JCS_TYPE_BOOL) {
        return false;
    }
    *dst = v->bval;
    return true;
}

static bool jcs_get_uint32_field(const jcs_val_t *obj, const char *key, uint32_t *dst,
                                 bool required, uint32_t def)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v) {
        *dst = def;
        return !required;
    }
    if (v->type != JCS_TYPE_NUM) {
        return false;
    }
    if (v->is_uint) {
        if (v->uval > UINT32_MAX) {
            return false;
        }
        *dst = (uint32_t)v->uval;
        return true;
    }
    if (v->is_int) {
        if (v->ival < 0 || v->ival > (int64_t)UINT32_MAX) {
            return false;
        }
        *dst = (uint32_t)v->ival;
        return true;
    }
    if (v->dval < 0.0 || v->dval > (double)UINT32_MAX || floor(v->dval) != v->dval) {
        return false;
    }
    *dst = (uint32_t)v->dval;
    return true;
}

static bool jcs_get_uint64_field(const jcs_val_t *obj, const char *key, uint64_t *dst,
                                 bool required, uint64_t def)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v) {
        *dst = def;
        return !required;
    }
    if (v->type != JCS_TYPE_NUM) {
        return false;
    }
    if (v->is_uint) {
        *dst = v->uval;
        return true;
    }
    if (v->is_int) {
        if (v->ival < 0) {
            return false;
        }
        *dst = (uint64_t)v->ival;
        return true;
    }
    if (v->dval < 0.0 || v->dval > 9007199254740991.0 || floor(v->dval) != v->dval) {
        return false;
    }
    *dst = (uint64_t)v->dval;
    return true;
}

static bool parse_manifest_ast(const jcs_val_t *root, wwpkg_manifest_t *m, int *out_err)
{
    if (!root || root->type != JCS_TYPE_OBJ) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    memset(m, 0, sizeof(*m));

    if (!jcs_get_str_field(root, "schema_version", m->schema_version, sizeof(m->schema_version),
                           true)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    bool is_v2 = (strcmp(m->schema_version, "wwpkg/v2") == 0);
    bool is_v1 = (strcmp(m->schema_version, "wwpkg/v1") == 0);
    if (!is_v2 && !is_v1) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (!jcs_get_str_field(root, "id", m->id, sizeof(m->id), true) || m->id[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (!jcs_get_str_field(root, "slug", m->slug, sizeof(m->slug), true) || m->slug[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    for (size_t i = 0; m->slug[i]; i++) {
        char c = m->slug[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
        if (c == '-' && (i == 0 || m->slug[i + 1] == '\0' || m->slug[i + 1] == '-')) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
    }

    if (!jcs_get_str_field(root, "name", m->name, sizeof(m->name), true) || m->name[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (!jcs_get_str_field(root, "version", m->version, sizeof(m->version), true) ||
        m->version[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    const jcs_val_t *v_auth = jcs_find_member(root, "author");
    if (!v_auth || v_auth->type != JCS_TYPE_OBJ) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_str_field(v_auth, "name", m->author_name, sizeof(m->author_name), true) ||
        m->author_name[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_str_field(v_auth, "email", m->author_email, sizeof(m->author_email), false)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    char pubkey_hex[128] = {0};
    if (!jcs_get_str_field(v_auth, "pubkey", pubkey_hex, sizeof(pubkey_hex), true) ||
        !decode_hex32(pubkey_hex, strlen(pubkey_hex), m->author_pubkey)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (!jcs_get_str_field(root, "license", m->license, sizeof(m->license), true) ||
        m->license[0] == '\0') {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (!jcs_get_str_field(root, "description", m->description, sizeof(m->description), false)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    const jcs_val_t *v_tags = jcs_find_member(root, "tags");
    if (v_tags) {
        if (v_tags->type != JCS_TYPE_ARR) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
        size_t count = v_tags->arr.count < WWPKG_MAX_TAGS ? v_tags->arr.count : WWPKG_MAX_TAGS;
        m->tag_count = (uint32_t)count;
        for (size_t i = 0; i < count; i++) {
            const jcs_val_t *tv = v_tags->arr.items[i];
            if (!tv || tv->type != JCS_TYPE_STR || tv->slen >= sizeof(m->tags[i]) ||
                strlen(tv->sval) != tv->slen) {
                if (out_err)
                    *out_err = WWPKG_ERR_BAD_MANIFEST;
                return false;
            }
            memcpy(m->tags[i], tv->sval, tv->slen + 1);
        }
    }

    char type_str[64] = {0};
    if (!jcs_get_str_field(root, "type", type_str, sizeof(type_str), true)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (strcmp(type_str, "shader_fragment") == 0)
        m->type = WWPKG_TYPE_SHADER_FRAGMENT;
    else if (strcmp(type_str, "shader_compute") == 0)
        m->type = WWPKG_TYPE_SHADER_COMPUTE;
    else if (strcmp(type_str, "scene_2d") == 0)
        m->type = WWPKG_TYPE_SCENE_2D;
    else if (strcmp(type_str, "scene_puppet") == 0)
        m->type = WWPKG_TYPE_SCENE_PUPPET;
    else if (strcmp(type_str, "scene_particles") == 0)
        m->type = WWPKG_TYPE_SCENE_PARTICLES;
    else if (strcmp(type_str, "video_stream") == 0)
        m->type = WWPKG_TYPE_VIDEO_STREAM;
    else {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    const jcs_val_t *v_ep = jcs_find_member(root, "entrypoint");
    if (!v_ep || v_ep->type != JCS_TYPE_OBJ) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    char raw_pipe[WWPKG_MAX_PATH_LEN] = {0};
    char raw_prev[WWPKG_MAX_PATH_LEN] = {0};
    char raw_aud[WWPKG_MAX_PATH_LEN] = {0};
    if (!jcs_get_str_field(v_ep, "pipeline", raw_pipe, sizeof(raw_pipe), true) ||
        !sanitize_archive_path(raw_pipe, m->entrypoint_pipeline, sizeof(m->entrypoint_pipeline))) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_str_field(v_ep, "preview", raw_prev, sizeof(raw_prev), true) ||
        !sanitize_archive_path(raw_prev, m->entrypoint_preview, sizeof(m->entrypoint_preview))) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_str_field(v_ep, "audio_track", raw_aud, sizeof(raw_aud), false)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (raw_aud[0] != '\0' &&
        !sanitize_archive_path(raw_aud, m->entrypoint_audio, sizeof(m->entrypoint_audio))) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    const jcs_val_t *v_eng = jcs_find_member(root, "engine_requirements");
    if (!v_eng || v_eng->type != JCS_TYPE_OBJ) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_str_field(v_eng, "min_wwal_version", m->min_wwal_version,
                           sizeof(m->min_wwal_version), true) ||
        !jcs_get_uint32_field(v_eng, "abi_version", &m->abi_version, true, 1) ||
        !jcs_get_str_field(v_eng, "gl_profile", m->gl_profile, sizeof(m->gl_profile), true)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    const jcs_val_t *v_cap = jcs_find_member(root, "capabilities");
    if (!v_cap || v_cap->type != JCS_TYPE_OBJ) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (!jcs_get_bool_field(v_cap, "compute_shaders", &m->capabilities.compute_shaders, true,
                            false) ||
        !jcs_get_bool_field(v_cap, "audio_spectrum", &m->capabilities.audio_spectrum, true,
                            false) ||
        !jcs_get_bool_field(v_cap, "audio_playback", &m->capabilities.audio_playback, true,
                            false) ||
        !jcs_get_bool_field(v_cap, "scripting", &m->capabilities.scripting, true, false) ||
        !jcs_get_bool_field(v_cap, "foreign_toplevel", &m->capabilities.foreign_toplevel, true,
                            false) ||
        !jcs_get_bool_field(v_cap, "network", &m->capabilities.network, true, false) ||
        !jcs_get_bool_field(v_cap, "filesystem", &m->capabilities.filesystem, true, false)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    if (m->capabilities.network || m->capabilities.filesystem) {
        if (out_err)
            *out_err = WWPKG_ERR_CAPABILITY;
        return false;
    }

    const jcs_val_t *v_bud = jcs_find_member(root, "resource_budgets");
    if (v_bud) {
        if (v_bud->type != JCS_TYPE_OBJ) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
        if (!jcs_get_uint32_field(v_bud, "max_vram_mb", &m->budgets.max_vram_mb, false, 256) ||
            !jcs_get_uint32_field(v_bud, "max_ram_mb", &m->budgets.max_ram_mb, false, 64) ||
            !jcs_get_uint32_field(v_bud, "target_fps", &m->budgets.target_fps, false, 60)) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
    } else {
        m->budgets.max_vram_mb = 256;
        m->budgets.max_ram_mb = 64;
        m->budgets.target_fps = 60;
    }

    if (!jcs_get_uint32_field(root, "file_count", &m->file_count, is_v2, 0) ||
        !jcs_get_uint64_field(root, "total_bytes", &m->total_bytes, is_v2, 0)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (m->file_count > WWPKG_MAX_FILE_COUNT || m->total_bytes > WWPKG_MAX_ARCHIVE_BYTES) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    char prev_hash_hex[128] = {0};
    if (!jcs_get_str_field(root, "preview_sha256", prev_hash_hex, sizeof(prev_hash_hex), false)) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    if (prev_hash_hex[0] != '\0') {
        if (!decode_hex32(prev_hash_hex, strlen(prev_hash_hex), m->preview_sha256)) {
            if (out_err)
                *out_err = WWPKG_ERR_BAD_MANIFEST;
            return false;
        }
        m->has_preview_sha256 = true;
    }

    return true;
}

typedef struct {
    bool valid;
    uint8_t sha256[32];
    uint16_t desc_len;
    wwpkg_manifest_t parsed;
} verified_manifest_slot_t;

static pthread_mutex_t g_mf_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static verified_manifest_slot_t g_mf_cache[32];

static void copy_manifest_fast(wwpkg_manifest_t *dst, const wwpkg_manifest_t *src, size_t desc_len)
{
    memcpy(dst, src, offsetof(wwpkg_manifest_t, description));
    memcpy(dst->description, src->description, desc_len + 1u);
    memcpy(dst->tags, src->tags, sizeof(wwpkg_manifest_t) - offsetof(wwpkg_manifest_t, tags));
}

static bool parse_manifest_from_string_ex(const char *json_str, size_t len, wwpkg_manifest_t *m,
                                          uint8_t out_sha256[32], int *out_err)
{
    if (!json_str || !m) {
        if (out_err)
            *out_err = WWPKG_ERR_INVALID_PARAM;
        return false;
    }
    size_t actual_len = len ? len : strlen(json_str);
    if (actual_len == 0) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    uint8_t h_raw[32];
    wwpkg_sha256((const uint8_t *)json_str, actual_len, h_raw);
    if (out_sha256) {
        memcpy(out_sha256, h_raw, 32);
    }

    uint32_t mslot = read_u32_le(h_raw) & 31u;
    pthread_mutex_lock(&g_mf_cache_mu);
    if (g_mf_cache[mslot].valid && memcmp(g_mf_cache[mslot].sha256, h_raw, 32) == 0) {
        copy_manifest_fast(m, &g_mf_cache[mslot].parsed, g_mf_cache[mslot].desc_len);
        pthread_mutex_unlock(&g_mf_cache_mu);
        return true;
    }
    pthread_mutex_unlock(&g_mf_cache_mu);

    jcs_arena_t arena;
    jcs_arena_init(&arena);
    jcs_lexer_t lx = {.src = json_str, .len = actual_len, .pos = 0, .arena = &arena};
    jcs_val_t *root = jcs_parse_val(&lx, 0);
    if (!root) {
        jcs_arena_free(&arena);
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }
    jcs_skip_ws(&lx);
    if (lx.pos != lx.len) {
        jcs_arena_free(&arena);
        if (out_err)
            *out_err = WWPKG_ERR_BAD_MANIFEST;
        return false;
    }

    bool ok = parse_manifest_ast(root, m, out_err);
    jcs_arena_free(&arena);
    if (ok) {
        size_t dlen = strlen(m->description);
        pthread_mutex_lock(&g_mf_cache_mu);
        memcpy(g_mf_cache[mslot].sha256, h_raw, 32);
        g_mf_cache[mslot].desc_len = (uint16_t)dlen;
        copy_manifest_fast(&g_mf_cache[mslot].parsed, m, dlen);
        g_mf_cache[mslot].valid = true;
        pthread_mutex_unlock(&g_mf_cache_mu);
    }
    return ok;
}

static bool parse_manifest_from_string(const char *json_str, size_t len, wwpkg_manifest_t *m,
                                       int *out_err)
{
    return parse_manifest_from_string_ex(json_str, len, m, nullptr, out_err);
}

static char *manifest_to_canonical_json(const wwpkg_manifest_t *m, size_t *out_len)
{
    if (!m) {
        return nullptr;
    }

    char pubkey_hex[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(pubkey_hex + i * 2, 3, "%02x", m->author_pubkey[i]);
    }

    const char *type_str = nullptr;
    switch (m->type) {
    case WWPKG_TYPE_SHADER_FRAGMENT:
        type_str = "shader_fragment";
        break;
    case WWPKG_TYPE_SHADER_COMPUTE:
        type_str = "shader_compute";
        break;
    case WWPKG_TYPE_SCENE_2D:
        type_str = "scene_2d";
        break;
    case WWPKG_TYPE_SCENE_PUPPET:
        type_str = "scene_puppet";
        break;
    case WWPKG_TYPE_SCENE_PARTICLES:
        type_str = "scene_particles";
        break;
    case WWPKG_TYPE_VIDEO_STREAM:
        type_str = "video_stream";
        break;
    default:
        return nullptr;
    }

    jcs_out_buf_t raw = {.buf = (char *)malloc(2048), .len = 0, .cap = 2048};
    if (!raw.buf) {
        return nullptr;
    }
    raw.buf[0] = '\0';

    char num_tmp[512];
    bool ok = true;

#define APPEND_KV_STR(k, v)                                                                        \
    do {                                                                                           \
        ok = ok && jcs_out_append_lit(&raw, "\"" k "\":") && jcs_out_append_cstr(&raw, (v));       \
    } while (0)

    ok = ok && jcs_out_append_lit(&raw, "{");
    APPEND_KV_STR("schema_version", "wwpkg/v2");
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("id", m->id);
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("slug", m->slug);
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("name", m->name);
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("version", m->version);

    ok = ok && jcs_out_append_lit(&raw, ",\"author\":{");
    APPEND_KV_STR("name", m->author_name);
    if (m->author_email[0] != '\0') {
        ok = ok && jcs_out_append_lit(&raw, ",");
        APPEND_KV_STR("email", m->author_email);
    }
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("pubkey", pubkey_hex);
    ok = ok && jcs_out_append_lit(&raw, "},");

    APPEND_KV_STR("license", m->license);

    if (m->description[0] != '\0') {
        ok = ok && jcs_out_append_lit(&raw, ",");
        APPEND_KV_STR("description", m->description);
    }

    if (m->tag_count > 0) {
        ok = ok && jcs_out_append_lit(&raw, ",\"tags\":[");
        uint32_t tc = m->tag_count < WWPKG_MAX_TAGS ? m->tag_count : WWPKG_MAX_TAGS;
        for (uint32_t i = 0; i < tc; i++) {
            if (i > 0) {
                ok = ok && jcs_out_append_lit(&raw, ",");
            }
            ok = ok && jcs_out_append_cstr(&raw, m->tags[i]);
        }
        ok = ok && jcs_out_append_lit(&raw, "]");
    }

    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("type", type_str);

    ok = ok && jcs_out_append_lit(&raw, ",\"entrypoint\":{");
    APPEND_KV_STR("pipeline", m->entrypoint_pipeline);
    ok = ok && jcs_out_append_lit(&raw, ",");
    APPEND_KV_STR("preview", m->entrypoint_preview);
    if (m->entrypoint_audio[0] != '\0') {
        ok = ok && jcs_out_append_lit(&raw, ",");
        APPEND_KV_STR("audio_track", m->entrypoint_audio);
    }
    ok = ok && jcs_out_append_lit(&raw, "}");

    ok = ok && jcs_out_append_lit(&raw, ",\"engine_requirements\":{");
    APPEND_KV_STR("min_wwal_version", m->min_wwal_version[0] ? m->min_wwal_version : "1.2.0");
    snprintf(num_tmp, sizeof(num_tmp), ",\"abi_version\":%u,",
             m->abi_version ? m->abi_version : 1u);
    ok = ok && jcs_out_append_lit(&raw, num_tmp);
    APPEND_KV_STR("gl_profile", m->gl_profile[0] ? m->gl_profile : "gles_31");
    ok = ok && jcs_out_append_lit(&raw, "}");

    snprintf(num_tmp, sizeof(num_tmp),
             ",\"capabilities\":{\"compute_shaders\":%s,\"audio_spectrum\":%s,"
             "\"audio_playback\":%s,\"scripting\":%s,\"foreign_toplevel\":%s,"
             "\"network\":false,\"filesystem\":false}",
             m->capabilities.compute_shaders ? "true" : "false",
             m->capabilities.audio_spectrum ? "true" : "false",
             m->capabilities.audio_playback ? "true" : "false",
             m->capabilities.scripting ? "true" : "false",
             m->capabilities.foreign_toplevel ? "true" : "false");
    ok = ok && jcs_out_append_lit(&raw, num_tmp);

    snprintf(num_tmp, sizeof(num_tmp),
             ",\"resource_budgets\":{\"max_vram_mb\":%u,\"max_ram_mb\":%u,\"target_fps\":%u}",
             m->budgets.max_vram_mb ? m->budgets.max_vram_mb : 256u,
             m->budgets.max_ram_mb ? m->budgets.max_ram_mb : 64u,
             m->budgets.target_fps ? m->budgets.target_fps : 60u);
    ok = ok && jcs_out_append_lit(&raw, num_tmp);

    if (m->has_preview_sha256) {
        char prev_hex[65] = {0};
        for (int i = 0; i < 32; i++) {
            snprintf(prev_hex + i * 2, 3, "%02x", m->preview_sha256[i]);
        }
        ok = ok && jcs_out_append_lit(&raw, ",");
        APPEND_KV_STR("preview_sha256", prev_hex);
    }

    snprintf(num_tmp, sizeof(num_tmp), ",\"file_count\":%u,\"total_bytes\":%llu}", m->file_count,
             (unsigned long long)m->total_bytes);
    ok = ok && jcs_out_append_lit(&raw, num_tmp);

#undef APPEND_KV_STR

    if (!ok) {
        free(raw.buf);
        return nullptr;
    }

    char *canonical = wwpkg_canonicalize_json(raw.buf, raw.len, out_len);
    free(raw.buf);
    return canonical;
}

/* ========================================================================= */
/* Path + Length-Bound RFC 6962 Domain-Separated SHA-256 Merkle Tree         */
/* ========================================================================= */

typedef struct {
    char name[WWPKG_MAX_PATH_LEN];
    const uint8_t *data;
    size_t size;
} merkle_file_t;

static int merkle_file_cmp(const void *a, const void *b)
{
    const merkle_file_t *fa = (const merkle_file_t *)a;
    const merkle_file_t *fb = (const merkle_file_t *)b;
    return strcmp(fa->name, fb->name);
}

/* v2 leaf = SHA256(0x00 || u32le(path_len) || norm_path || u64le(file_len) || file_data);
 * v1 leaf = SHA256(0x00 || file_data) for backward compatibility */
static void compute_merkle_leaf(const char *norm_path, const uint8_t *data, size_t size,
                                bool v2_leaves, uint8_t out_hash[32])
{
    uint8_t domain = 0x00;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, &domain, 1);
    if (v2_leaves) {
        uint32_t plen = (uint32_t)strlen(norm_path);
        uint8_t plen_le[4];
        uint8_t flen_le[8];
        write_u32_le(plen_le, plen);
        write_u64_le(flen_le, (uint64_t)size);
        sha256_update(&ctx, plen_le, sizeof(plen_le));
        sha256_update(&ctx, (const uint8_t *)norm_path, plen);
        sha256_update(&ctx, flen_le, sizeof(flen_le));
    }
    if (size > 0 && data != nullptr) {
        sha256_update(&ctx, data, size);
    }
    sha256_final(&ctx, out_hash);
}

static bool compute_merkle_root(merkle_file_t *files, size_t file_count, bool v2_leaves,
                                uint8_t out_root[32])
{
    if (file_count == 0) {
        wwpkg_sha256((const uint8_t *)"", 0, out_root);
        return true;
    }

    if (file_count > 1) {
        qsort(files, file_count, sizeof(merkle_file_t), merkle_file_cmp);
    }

    uint8_t (*nodes)[32] = (uint8_t (*)[32])malloc(file_count * 32);
    if (!nodes) {
        return false;
    }

    for (size_t i = 0; i < file_count; i++) {
        compute_merkle_leaf(files[i].name, files[i].data, files[i].size, v2_leaves, nodes[i]);
    }

    size_t current_count = file_count;
    while (current_count > 1) {
        size_t next_count = (current_count + 1) / 2;
        for (size_t i = 0; i < current_count; i += 2) {
            size_t parent_idx = i / 2;
            if (i + 1 < current_count) {
                uint8_t node_buf[65];
                node_buf[0] = 0x01;
                memcpy(node_buf + 1, nodes[i], 32);
                memcpy(node_buf + 33, nodes[i + 1], 32);
                wwpkg_sha256(node_buf, sizeof(node_buf), nodes[parent_idx]);
            } else {
                memcpy(nodes[parent_idx], nodes[i], 32);
            }
        }
        current_count = next_count;
    }

    memcpy(out_root, nodes[0], 32);
    free(nodes);
    return true;
}

/* ========================================================================= */
/* POSIX ustar Archive Reader & Writer + Bounded Streaming Zstd Decompressor */
/* ========================================================================= */

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed)) ustar_hdr_t;
static_assert(sizeof(ustar_hdr_t) == 512, "ustar header must be exactly 512 bytes");

static uint32_t ustar_calc_chksum(const ustar_hdr_t *hdr)
{
    const uint8_t *p = (const uint8_t *)hdr;
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; i++) {
        if (i >= offsetof(ustar_hdr_t, chksum) && i < offsetof(ustar_hdr_t, chksum) + 8) {
            sum += ' ';
        } else {
            sum += p[i];
        }
    }
    return sum;
}

static bool ustar_parse_octal(const char *field, size_t field_len, uint64_t *out_val)
{
    size_t i = 0;
    while (i < field_len && field[i] == ' ') {
        i++;
    }
    if (i >= field_len || field[i] < '0' || field[i] > '7') {
        return false;
    }
    uint64_t val = 0;
    while (i < field_len && field[i] >= '0' && field[i] <= '7') {
        uint64_t d = (uint64_t)(field[i] - '0');
        if (val > (UINT64_MAX >> 3)) {
            return false;
        }
        val = (val << 3) | d;
        i++;
    }
    while (i < field_len && (field[i] == ' ' || field[i] == '\0')) {
        i++;
    }
    if (i != field_len) {
        return false;
    }
    *out_val = val;
    return true;
}

static bool sanitize_archive_path(const char *in, char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0 || in[0] == '/' || in[0] == '\0') {
        return false;
    }

    size_t out_len = 0;
    const char *p = in;
    while (*p) {
        const char *comp_start = p;
        while (*p && *p != '/') {
            if ((uint8_t)*p < 0x20u || *p == 0x7F || *p == '\\') {
                return false;
            }
            p++;
        }
        size_t comp_len = (size_t)(p - comp_start);
        if (comp_len == 0) {
            if (*p == '/') {
                p++;
                continue;
            }
            break;
        }
        if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            return false;
        }
        if (comp_len == 1 && comp_start[0] == '.') {
            if (*p == '/') {
                p++;
            }
            continue;
        }
        if (out_len > 0) {
            if (out_len + 1 >= out_cap) {
                return false;
            }
            out[out_len++] = '/';
        }
        if (out_len + comp_len >= out_cap) {
            return false;
        }
        memcpy(out + out_len, comp_start, comp_len);
        out_len += comp_len;
        if (*p == '/') {
            p++;
        }
    }
    out[out_len] = '\0';
    return out_len > 0;
}

/* Rejects prefix > 155 or basename > 100 instead of silently truncating ustar names */
static bool tar_split_ustar_name(const char *name, char out_prefix[155], char out_name[100])
{
    if (!name || name[0] == '\0') {
        return false;
    }
    memset(out_prefix, 0, 155);
    memset(out_name, 0, 100);

    size_t nlen = strlen(name);
    const char *slash = strrchr(name, '/');
    if (!slash) {
        if (nlen > 100) {
            return false;
        }
        memcpy(out_name, name, nlen);
        return true;
    }

    size_t prefix_len = (size_t)(slash - name);
    size_t base_len = nlen - prefix_len - 1;
    if (base_len == 0 || base_len > 100 || prefix_len > 155) {
        return false;
    }
    if (nlen <= 100) {
        memcpy(out_name, name, nlen);
        return true;
    }
    if (prefix_len == 0) {
        return false;
    }
    memcpy(out_prefix, name, prefix_len);
    memcpy(out_name, slash + 1, base_len);
    return true;
}

static bool tar_write_entry(uint8_t *dst, size_t dst_cap, const char *name, const uint8_t *data,
                            size_t size, uint64_t mtime, size_t *out_written)
{
    if (!dst || !name || size > WWPKG_MAX_ARCHIVE_BYTES || size > 077777777777ULL) {
        return false;
    }
    size_t pad = ((size + 511u) / 512u) * 512u;
    if (dst_cap < 512u || pad > dst_cap - 512u) {
        return false;
    }

    ustar_hdr_t *hdr = (ustar_hdr_t *)dst;
    memset(hdr, 0, sizeof(*hdr));

    if (!tar_split_ustar_name(name, hdr->prefix, hdr->name)) {
        return false;
    }

    snprintf(hdr->mode, sizeof(hdr->mode), "%07o", 0644u);
    snprintf(hdr->uid, sizeof(hdr->uid), "%07o", 0u);
    snprintf(hdr->gid, sizeof(hdr->gid), "%07o", 0u);
    snprintf(hdr->size, sizeof(hdr->size), "%011zo", size);
    snprintf(hdr->mtime, sizeof(hdr->mtime), "%011llo",
             (unsigned long long)(mtime & 077777777777ULL));
    hdr->typeflag = '0';
    memcpy(hdr->magic, "ustar\0", 6);
    memcpy(hdr->version, "00", 2);
    memcpy(hdr->uname, "waywal", 6);
    memcpy(hdr->gname, "waywal", 6);

    uint32_t csum = ustar_calc_chksum(hdr);
    snprintf(hdr->chksum, sizeof(hdr->chksum), "%06o", csum);
    hdr->chksum[6] = '\0';
    hdr->chksum[7] = ' ';

    if (size > 0 && data != nullptr) {
        memcpy(dst + 512u, data, size);
    }
    if (pad > size) {
        memset(dst + 512u + size, 0, pad - size);
    }
    *out_written = 512u + pad;
    return true;
}

static bool zstd_decompress_bounded(const uint8_t *src, size_t src_len, size_t max_bytes,
                                    uint8_t **out_buf, size_t *out_len, int *out_err)
{
    zstd_probe();
    if (!g_zstd_available) {
        if (out_err)
            *out_err = WWPKG_ERR_DECOMPRESS;
        return false;
    }

    zstd_dstream_t *dstream = g_zstd.createDStream();
    if (!dstream) {
        if (out_err)
            *out_err = WWPKG_ERR_DECOMPRESS;
        return false;
    }
    size_t init_rc = g_zstd.initDStream(dstream);
    if (g_zstd.isError(init_rc)) {
        g_zstd.freeDStream(dstream);
        if (out_err)
            *out_err = WWPKG_ERR_DECOMPRESS;
        return false;
    }

    size_t cap = 65536u;
    if (max_bytes < cap) {
        cap = max_bytes ? max_bytes : 1u;
    }
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        g_zstd.freeDStream(dstream);
        if (out_err)
            *out_err = WWPKG_ERR_IO;
        return false;
    }

    uint8_t scratch[65536];
    size_t total_out = 0;
    zstd_in_buffer_t in_buf = {.src = src, .size = src_len, .pos = 0};
    size_t last_ret = 0;

    while (in_buf.pos < in_buf.size || last_ret != 0) {
        size_t prev_in = in_buf.pos;
        zstd_out_buffer_t out_chunk = {.dst = scratch, .size = sizeof(scratch), .pos = 0};
        last_ret = g_zstd.decompressStream(dstream, &out_chunk, &in_buf);
        if (g_zstd.isError(last_ret)) {
            free(buf);
            g_zstd.freeDStream(dstream);
            if (out_err)
                *out_err = WWPKG_ERR_DECOMPRESS;
            return false;
        }

        if (out_chunk.pos > 0) {
            if (out_chunk.pos > max_bytes - total_out) {
                /* Reject zip-bomb exceeding hard uncompressed byte cap */
                free(buf);
                g_zstd.freeDStream(dstream);
                if (out_err)
                    *out_err = WWPKG_ERR_DECOMPRESS;
                return false;
            }
            size_t needed = total_out + out_chunk.pos;
            if (needed > cap) {
                size_t ncap = cap;
                while (ncap < needed) {
                    if (ncap > max_bytes / 2) {
                        ncap = max_bytes;
                        break;
                    }
                    ncap *= 2;
                }
                if (ncap < needed) {
                    free(buf);
                    g_zstd.freeDStream(dstream);
                    if (out_err)
                        *out_err = WWPKG_ERR_DECOMPRESS;
                    return false;
                }
                uint8_t *nb = (uint8_t *)realloc(buf, ncap);
                if (!nb) {
                    free(buf);
                    g_zstd.freeDStream(dstream);
                    if (out_err)
                        *out_err = WWPKG_ERR_IO;
                    return false;
                }
                buf = nb;
                cap = ncap;
            }
            memcpy(buf + total_out, scratch, out_chunk.pos);
            total_out += out_chunk.pos;
        }

        if (in_buf.pos == prev_in && out_chunk.pos == 0) {
            /* Truncated incomplete Zstd frame */
            if (last_ret != 0 || in_buf.pos < in_buf.size) {
                free(buf);
                g_zstd.freeDStream(dstream);
                if (out_err)
                    *out_err = WWPKG_ERR_DECOMPRESS;
                return false;
            }
            break;
        }
    }

    g_zstd.freeDStream(dstream);
    *out_buf = buf;
    *out_len = total_out;
    return true;
}

/* ========================================================================= */
/* In-Memory Package Representation & Cryptographic Envelope Helpers         */
/* ========================================================================= */

typedef struct {
    char name[WWPKG_MAX_PATH_LEN];
    const uint8_t *data;
    size_t size;
} pkg_asset_entry_t;

#define WWPKG_INLINE_ASSET_CAP    4u
#define WWPKG_INLINE_MANIFEST_CAP 1536u
#define WWPKG_INLINE_PATH_CAP     256u
#define WWPKG_INLINE_F0_CAP       5120u

struct wwpkg_package {
    wwpkg_signature_t signature;
    wwpkg_outer_envelope_t outer_env;
    bool has_manifest;
    bool has_signature;
    bool has_outer_env;
    bool is_metadata_only;
    bool outer_auth_verified;
    int last_error;

    char *source_path;
    uint8_t *decompressed_buf;
    size_t decompressed_size;

    pkg_asset_entry_t *assets;
    size_t asset_count;
    size_t asset_cap;

    char *raw_manifest_str;
    size_t raw_manifest_len;
    uint8_t raw_manifest_sha256[32];
    bool has_raw_manifest_sha256;

    wwpkg_manifest_t manifest;
    pkg_asset_entry_t inline_assets[WWPKG_INLINE_ASSET_CAP];
    char inline_source_path[WWPKG_INLINE_PATH_CAP];
    char inline_manifest_str[WWPKG_INLINE_MANIFEST_CAP];
    uint8_t inline_f0_buf[WWPKG_INLINE_F0_CAP];
};

#define WWPKG_FAST_PKG_SLOTS 8u
static wwpkg_package_t g_pkg_pool[WWPKG_FAST_PKG_SLOTS];
static _Atomic uint32_t g_pkg_pool_mask = 0u;

static wwpkg_package_t *alloc_empty_package(void)
{
    wwpkg_package_t *pkg = nullptr;
    uint32_t mask = atomic_load_explicit(&g_pkg_pool_mask, memory_order_relaxed);
    while (mask != ((1u << WWPKG_FAST_PKG_SLOTS) - 1u)) {
        uint32_t free_idx = (uint32_t)__builtin_ctz(~mask);
        uint32_t bit = 1u << free_idx;
        if (atomic_compare_exchange_weak_explicit(&g_pkg_pool_mask, &mask, mask | bit,
                                                  memory_order_acquire, memory_order_relaxed)) {
            pkg = &g_pkg_pool[free_idx];
            break;
        }
    }
    if (!pkg) {
        pkg = (wwpkg_package_t *)malloc(sizeof(wwpkg_package_t));
        if (!pkg) {
            return nullptr;
        }
    }
    memset(pkg, 0, offsetof(wwpkg_package_t, manifest));
    pkg->manifest.schema_version[0] = '\0';
    pkg->assets = pkg->inline_assets;
    pkg->asset_cap = WWPKG_INLINE_ASSET_CAP;
    return pkg;
}

static bool pkg_set_source_path(wwpkg_package_t *pkg, const char *path)
{
    if (!pkg || !path) {
        return false;
    }
    if (pkg->source_path && pkg->source_path != pkg->inline_source_path) {
        free(pkg->source_path);
        pkg->source_path = nullptr;
    }
    size_t plen = strlen(path);
    if (plen < WWPKG_INLINE_PATH_CAP) {
        memcpy(pkg->inline_source_path, path, plen + 1);
        pkg->source_path = pkg->inline_source_path;
        return true;
    }
    pkg->source_path = strdup(path);
    return pkg->source_path != nullptr;
}

int wwpkg_get_last_error(const wwpkg_package_t *pkg)
{
    return pkg ? pkg->last_error : g_tls_last_error;
}

const wwpkg_manifest_t *wwpkg_get_manifest(const wwpkg_package_t *pkg)
{
    return (pkg && pkg->has_manifest) ? &pkg->manifest : nullptr;
}

const wwpkg_signature_t *wwpkg_get_signature(const wwpkg_package_t *pkg)
{
    return (pkg && pkg->has_signature) ? &pkg->signature : nullptr;
}

void wwpkg_close(wwpkg_package_t *pkg)
{
    if (!pkg) {
        return;
    }
    if (pkg->source_path != pkg->inline_source_path) {
        free(pkg->source_path);
    }
    if (pkg->decompressed_buf != pkg->inline_f0_buf) {
        free(pkg->decompressed_buf);
    }
    if (pkg->assets != pkg->inline_assets) {
        free(pkg->assets);
    }
    if (pkg->raw_manifest_str != pkg->inline_manifest_str) {
        free(pkg->raw_manifest_str);
    }
    if (pkg >= &g_pkg_pool[0] && pkg < &g_pkg_pool[WWPKG_FAST_PKG_SLOTS]) {
        uint32_t idx = (uint32_t)(pkg - &g_pkg_pool[0]);
        atomic_fetch_and_explicit(&g_pkg_pool_mask, ~(1u << idx), memory_order_release);
        return;
    }
    free(pkg);
}

static void build_sign_payload(uint32_t sig_version, const uint8_t h_manifest[32],
                               const uint8_t merkle_root[32], uint64_t timestamp, uint8_t out[85])
{
    const char *prefix = (sig_version >= 2) ? "WWPKG-SIG-V2\n" : "WWPKG-SIG-V1\n";
    memcpy(out, prefix, 13);
    memcpy(out + 13, h_manifest, 32);
    memcpy(out + 45, merkle_root, 32);
    write_u64_be(out + 77, timestamp);
}

static void build_notary_payload(const uint8_t author_pk[32], const uint8_t h_manifest[32],
                                 const uint8_t merkle_root[32], uint64_t timestamp,
                                 const uint8_t author_sig[64], uint8_t out[184])
{
    memcpy(out, "WWPKG-NOTARY-V1\n", 16);
    memcpy(out + 16, author_pk, 32);
    memcpy(out + 48, h_manifest, 32);
    memcpy(out + 80, merkle_root, 32);
    write_u64_be(out + 112, timestamp);
    memcpy(out + 120, author_sig, 64);
}

typedef struct {
    bool valid;
    uint8_t raw[sizeof(wwpkg_outer_envelope_t)];
    wwpkg_outer_envelope_t parsed;
} verified_env_slot_t;

static pthread_mutex_t g_env_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static verified_env_slot_t g_env_cache[64];

static bool verify_outer_envelope_bytes(const uint8_t *raw, size_t len,
                                        wwpkg_outer_envelope_t *out_env, int *out_err)
{
    if (len < sizeof(wwpkg_outer_envelope_t)) {
        if (out_err)
            *out_err = WWPKG_ERR_CORRUPT;
        return false;
    }

    uint32_t magic = read_u32_le(raw);
    uint32_t fsize = read_u32_le(raw + 4);
    if (magic != WWPKG_MAGIC_SKIPPABLE_ENV || fsize != WWPKG_OUTER_ENV_PAYLOAD) {
        if (out_err)
            *out_err = WWPKG_ERR_CORRUPT;
        return false;
    }

    uint32_t slot = read_u32_le(raw + offsetof(wwpkg_outer_envelope_t, signature)) & 63u;
    pthread_mutex_lock(&g_env_cache_mu);
    if (g_env_cache[slot].valid &&
        memcmp(g_env_cache[slot].raw, raw, sizeof(wwpkg_outer_envelope_t)) == 0) {
        if (out_env) {
            *out_env = g_env_cache[slot].parsed;
        }
        pthread_mutex_unlock(&g_env_cache_mu);
        return true;
    }
    pthread_mutex_unlock(&g_env_cache_mu);

    wwpkg_outer_envelope_t env;
    memcpy(&env, raw, sizeof(env));
    uint64_t ts = read_u64_le(raw + offsetof(wwpkg_outer_envelope_t, timestamp_le));
    env.magic_le = magic;
    env.frame_size_le = fsize;
    env.timestamp_le = ts;

    uint8_t sign_payload[85];
    build_sign_payload(2, env.manifest_sha256, env.merkle_root, ts, sign_payload);
    if (wwpkg_ed25519_verify(env.signature, sign_payload, sizeof(sign_payload),
                             env.author_pubkey) != 0) {
        if (out_err)
            *out_err = WWPKG_ERR_BAD_SIGNATURE;
        return false;
    }

    pthread_mutex_lock(&g_env_cache_mu);
    memcpy(g_env_cache[slot].raw, raw, sizeof(wwpkg_outer_envelope_t));
    g_env_cache[slot].parsed = env;
    g_env_cache[slot].valid = true;
    pthread_mutex_unlock(&g_env_cache_mu);

    if (out_env) {
        *out_env = env;
    }
    return true;
}

static bool parse_ustar_into_package(wwpkg_package_t *pkg, const uint8_t *tar_buf, size_t tar_len,
                                     bool is_partial_frame0)
{
    size_t offset = 0;
    uint32_t payload_files = 0;
    uint64_t payload_bytes = 0;

    while (offset < tar_len) {
        if (tar_len - offset < 512u) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }

        const ustar_hdr_t *hdr = (const ustar_hdr_t *)(tar_buf + offset);
        bool is_zero = true;
        for (size_t i = 0; i < 512u; i++) {
            if (((const uint8_t *)hdr)[i] != 0) {
                is_zero = false;
                break;
            }
        }
        if (is_zero) {
            break;
        }

        if (memcmp(hdr->magic, "ustar", 5) != 0) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }

        uint64_t stored_sum = 0;
        if (!ustar_parse_octal(hdr->chksum, sizeof(hdr->chksum), &stored_sum) ||
            (uint64_t)ustar_calc_chksum(hdr) != stored_sum) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }

        uint64_t fsize_u64 = 0;
        if (!ustar_parse_octal(hdr->size, sizeof(hdr->size), &fsize_u64)) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }
        if (fsize_u64 > WWPKG_MAX_ARCHIVE_BYTES || (size_t)fsize_u64 > tar_len - offset - 512u) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }
        size_t fsize = (size_t)fsize_u64;

        size_t rem = fsize & 511u;
        size_t pad = rem ? (512u - rem) : 0u;
        if (pad > (tar_len - offset - 512u) - fsize) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            return false;
        }

        if (hdr->typeflag == '5') {
            if (fsize != 0) {
                pkg->last_error = WWPKG_ERR_CORRUPT;
                return false;
            }
        } else if (hdr->typeflag != '0' && hdr->typeflag != '\0') {
            pkg->last_error = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }
        if (hdr->linkname[0] != '\0') {
            pkg->last_error = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }

        char full_path[260];
        if (hdr->prefix[0] != '\0') {
            size_t plen = strnlen(hdr->prefix, 155u);
            size_t nlen = strnlen(hdr->name, 100u);
            memcpy(full_path, hdr->prefix, plen);
            full_path[plen] = '/';
            memcpy(full_path + plen + 1u, hdr->name, nlen);
            full_path[plen + 1u + nlen] = '\0';
        } else {
            size_t nlen = strnlen(hdr->name, 100u);
            memcpy(full_path, hdr->name, nlen);
            full_path[nlen] = '\0';
        }

        char clean_path[WWPKG_MAX_PATH_LEN];
        if (!sanitize_archive_path(full_path, clean_path, sizeof(clean_path))) {
            pkg->last_error = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }

        const uint8_t *entry_data = tar_buf + offset + 512u;

        if (hdr->typeflag != '5') {
            for (size_t i = 0; i < pkg->asset_count; i++) {
                if (strcmp(pkg->assets[i].name, clean_path) == 0) {
                    /* Reject duplicate normalized paths inside archive */
                    pkg->last_error = WWPKG_ERR_CORRUPT;
                    return false;
                }
            }

            if (strcmp(clean_path, "signature.sig") == 0) {
                if (fsize != sizeof(wwpkg_signature_t)) {
                    pkg->last_error = WWPKG_ERR_CORRUPT;
                    return false;
                }
                memcpy(&pkg->signature, entry_data, sizeof(wwpkg_signature_t));
                if (pkg->signature.has_notary_countersign > 1u) {
                    pkg->last_error = WWPKG_ERR_BAD_SIGNATURE;
                    return false;
                }
                pkg->has_signature = true;
            } else if (strcmp(clean_path, "manifest.json") == 0) {
                if (fsize == 0 || fsize > WWPKG_FRAME0_MAX_BYTES) {
                    pkg->last_error = WWPKG_ERR_BAD_MANIFEST;
                    return false;
                }
                if (fsize < WWPKG_INLINE_MANIFEST_CAP) {
                    pkg->raw_manifest_str = pkg->inline_manifest_str;
                } else {
                    pkg->raw_manifest_str = (char *)malloc(fsize + 1);
                    if (!pkg->raw_manifest_str) {
                        pkg->last_error = WWPKG_ERR_IO;
                        return false;
                    }
                }
                memcpy(pkg->raw_manifest_str, entry_data, fsize);
                pkg->raw_manifest_str[fsize] = '\0';
                pkg->raw_manifest_len = fsize;

                int mf_err = WWPKG_ERR_BAD_MANIFEST;
                if (!parse_manifest_from_string_ex(pkg->raw_manifest_str, fsize, &pkg->manifest,
                                                   pkg->raw_manifest_sha256, &mf_err)) {
                    pkg->last_error = mf_err;
                    return false;
                }
                pkg->has_raw_manifest_sha256 = true;
                pkg->has_manifest = true;
            } else {
                payload_files++;
                if (payload_files > WWPKG_MAX_FILE_COUNT ||
                    fsize_u64 > WWPKG_MAX_ARCHIVE_BYTES - payload_bytes) {
                    pkg->last_error = WWPKG_ERR_DECOMPRESS;
                    return false;
                }
                payload_bytes += fsize_u64;
            }

            if (pkg->asset_count >= WWPKG_MAX_FILE_COUNT) {
                pkg->last_error = WWPKG_ERR_DECOMPRESS;
                return false;
            }
            if (pkg->asset_count >= pkg->asset_cap) {
                size_t ncap = pkg->asset_cap ? pkg->asset_cap * 2 : 16;
                if (ncap > WWPKG_MAX_FILE_COUNT + 2u) {
                    ncap = WWPKG_MAX_FILE_COUNT + 2u;
                }
                pkg_asset_entry_t *na = nullptr;
                if (pkg->assets == pkg->inline_assets) {
                    na = (pkg_asset_entry_t *)malloc(ncap * sizeof(pkg_asset_entry_t));
                    if (na && pkg->asset_count > 0) {
                        memcpy(na, pkg->inline_assets,
                               pkg->asset_count * sizeof(pkg_asset_entry_t));
                    }
                } else {
                    na =
                        (pkg_asset_entry_t *)realloc(pkg->assets, ncap * sizeof(pkg_asset_entry_t));
                }
                if (!na) {
                    pkg->last_error = WWPKG_ERR_IO;
                    return false;
                }
                pkg->assets = na;
                pkg->asset_cap = ncap;
            }
            size_t nlen = strlen(clean_path);
            memcpy(pkg->assets[pkg->asset_count].name, clean_path, nlen + 1);
            pkg->assets[pkg->asset_count].data = entry_data;
            pkg->assets[pkg->asset_count].size = fsize;
            pkg->asset_count++;
        }

        offset += 512u + fsize + pad;
    }

    if (!pkg->has_manifest || !pkg->has_signature) {
        pkg->last_error = WWPKG_ERR_CORRUPT;
        return false;
    }

    if (!is_partial_frame0 && strcmp(pkg->manifest.schema_version, "wwpkg/v2") == 0) {
        if (payload_files != pkg->manifest.file_count ||
            payload_bytes != pkg->manifest.total_bytes) {
            pkg->last_error = WWPKG_ERR_BAD_SIGNATURE;
            return false;
        }
    }

    return true;
}

wwpkg_package_t *wwpkg_open_memory(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return nullptr;
    }

    wwpkg_outer_envelope_t outer_env;
    memset(&outer_env, 0, sizeof(outer_env));
    bool has_outer = false;
    const uint8_t *stream_ptr = data;
    size_t stream_len = size;

    if (size >= 4 && read_u32_le(data) == WWPKG_MAGIC_SKIPPABLE_ENV) {
        int env_err = WWPKG_ERR_BAD_SIGNATURE;
        if (!verify_outer_envelope_bytes(data, size, &outer_env, &env_err)) {
            set_tls_error(env_err);
            return nullptr;
        }
        has_outer = true;
        stream_ptr = data + sizeof(wwpkg_outer_envelope_t);
        stream_len = size - sizeof(wwpkg_outer_envelope_t);
    }

    uint8_t *tar_buf = nullptr;
    size_t tar_size = 0;

    if (stream_len >= 4 && read_u32_le(stream_ptr) == WWPKG_MAGIC_ZSTD) {
        int derr = WWPKG_ERR_DECOMPRESS;
        if (!zstd_decompress_bounded(stream_ptr, stream_len, WWPKG_MAX_ARCHIVE_BYTES, &tar_buf,
                                     &tar_size, &derr)) {
            set_tls_error(derr);
            return nullptr;
        }
    } else if (stream_len >= 512 && stream_len <= WWPKG_MAX_ARCHIVE_BYTES &&
               memcmp(stream_ptr + 257, "ustar", 5) == 0) {
        tar_buf = (uint8_t *)malloc(stream_len);
        if (!tar_buf) {
            set_tls_error(WWPKG_ERR_IO);
            return nullptr;
        }
        memcpy(tar_buf, stream_ptr, stream_len);
        tar_size = stream_len;
    } else {
        set_tls_error(WWPKG_ERR_CORRUPT);
        return nullptr;
    }

    wwpkg_package_t *pkg = alloc_empty_package();
    if (!pkg) {
        free(tar_buf);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    pkg->decompressed_buf = tar_buf;
    pkg->decompressed_size = tar_size;
    pkg->has_outer_env = has_outer;
    pkg->outer_auth_verified = has_outer;
    if (has_outer) {
        pkg->outer_env = outer_env;
    }

    if (!parse_ustar_into_package(pkg, tar_buf, tar_size, false)) {
        set_tls_error(pkg->last_error ? pkg->last_error : WWPKG_ERR_CORRUPT);
        wwpkg_close(pkg);
        return nullptr;
    }

    set_tls_error(WWPKG_OK);
    return pkg;
}

wwpkg_package_t *wwpkg_open_fd(int fd)
{
    if (fd < 0) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    if ((uint64_t)st.st_size > WWPKG_MAX_ARCHIVE_BYTES) {
        set_tls_error(WWPKG_ERR_DECOMPRESS);
        return nullptr;
    }

    size_t file_size = (size_t)st.st_size;
    void *mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped != MAP_FAILED) {
        wwpkg_package_t *pkg = wwpkg_open_memory((const uint8_t *)mapped, file_size);
        munmap(mapped, file_size);
        return pkg;
    }

    uint8_t *raw_data = (uint8_t *)malloc(file_size);
    if (!raw_data) {
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    size_t rd = 0;
    while (rd < file_size) {
        ssize_t r = pread(fd, raw_data + rd, file_size - rd, (off_t)rd);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            free(raw_data);
            set_tls_error(WWPKG_ERR_IO);
            return nullptr;
        }
        rd += (size_t)r;
    }
    wwpkg_package_t *pkg = wwpkg_open_memory(raw_data, file_size);
    free(raw_data);
    return pkg;
}

wwpkg_package_t *wwpkg_open(const char *pkg_path)
{
    if (!pkg_path) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return nullptr;
    }

    int fd = open(pkg_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    wwpkg_package_t *pkg = wwpkg_open_fd(fd);
    close(fd);
    if (pkg && !pkg_set_source_path(pkg, pkg_path)) {
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    return pkg;
}

/* Pre-decompression Ed25519 + Frame 0 (< 64 KiB) fast-path via mmap without touching bulk frames */
wwpkg_package_t *wwpkg_open_metadata_only(const char *pkg_path)
{
    if (!pkg_path) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return nullptr;
    }

    int fd = open(pkg_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }
    if ((uint64_t)st.st_size > WWPKG_MAX_ARCHIVE_BYTES) {
        close(fd);
        set_tls_error(WWPKG_ERR_DECOMPRESS);
        return nullptr;
    }

    size_t file_size = (size_t)st.st_size;
    const size_t k_init_map = 16384u;
    const size_t k_fast_map_max = sizeof(wwpkg_outer_envelope_t) + WWPKG_FRAME0_MAX_BYTES + 8192u;
    size_t map_len = file_size < k_init_map ? file_size : k_init_map;
    const uint8_t *map = (const uint8_t *)mmap(nullptr, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }

    if (file_size < 4 || read_u32_le(map) != WWPKG_MAGIC_SKIPPABLE_ENV) {
        /* Backward-compatible fallback for single-frame .wwpkg or uncompressed ustar archives */
        munmap((void *)map, map_len);
        wwpkg_package_t *pkg = wwpkg_open_fd(fd);
        close(fd);
        if (!pkg) {
            return nullptr;
        }
        if (!pkg_set_source_path(pkg, pkg_path)) {
            wwpkg_close(pkg);
            set_tls_error(WWPKG_ERR_IO);
            return nullptr;
        }
        if (!wwpkg_verify_signature(pkg, nullptr)) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
            wwpkg_close(pkg);
            return nullptr;
        }
        set_tls_error(WWPKG_OK);
        return pkg;
    }

    wwpkg_outer_envelope_t env;
    int env_err = WWPKG_ERR_BAD_SIGNATURE;
    if (!verify_outer_envelope_bytes(map, map_len, &env, &env_err)) {
        munmap((void *)map, map_len);
        close(fd);
        set_tls_error(env_err);
        return nullptr;
    }

    wwpkg_package_t *pkg = alloc_empty_package();
    if (!pkg) {
        munmap((void *)map, map_len);
        close(fd);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }

    const uint8_t *f0_src = map + sizeof(wwpkg_outer_envelope_t);
    size_t rem_size = map_len - sizeof(wwpkg_outer_envelope_t);
    uint8_t *f0_buf = nullptr;
    size_t f0_len = 0;

    if (rem_size >= 4 && read_u32_le(f0_src) == WWPKG_MAGIC_ZSTD) {
        zstd_probe();
        if (!g_zstd_available) {
            munmap((void *)map, map_len);
            close(fd);
            wwpkg_close(pkg);
            set_tls_error(WWPKG_ERR_DECOMPRESS);
            return nullptr;
        }
        size_t f0_csize = g_zstd.findFrameCompressedSize(f0_src, rem_size);
        if ((g_zstd.isError(f0_csize) || f0_csize > rem_size) && file_size > map_len) {
            size_t big_len = file_size < k_fast_map_max ? file_size : k_fast_map_max;
            const uint8_t *big_map =
                (const uint8_t *)mmap(nullptr, big_len, PROT_READ, MAP_PRIVATE, fd, 0);
            if (big_map != MAP_FAILED) {
                munmap((void *)map, map_len);
                map = big_map;
                map_len = big_len;
                f0_src = map + sizeof(wwpkg_outer_envelope_t);
                rem_size = map_len - sizeof(wwpkg_outer_envelope_t);
                f0_csize = g_zstd.findFrameCompressedSize(f0_src, rem_size);
            }
        }
        close(fd);
        if (g_zstd.isError(f0_csize) || f0_csize == 0 || f0_csize > rem_size) {
            munmap((void *)map, map_len);
            wwpkg_close(pkg);
            set_tls_error(WWPKG_ERR_DECOMPRESS);
            return nullptr;
        }
        unsigned long long content_sz = g_zstd.getFrameContentSize(f0_src, f0_csize);
        if (content_sz != 0xFFFFFFFFFFFFFFFFULL && content_sz != 0xFFFFFFFFFFFFFFFEULL) {
            if (content_sz == 0 || content_sz > WWPKG_FRAME0_MAX_BYTES) {
                munmap((void *)map, map_len);
                wwpkg_close(pkg);
                set_tls_error(WWPKG_ERR_DECOMPRESS);
                return nullptr;
            }
            if (content_sz <= WWPKG_INLINE_F0_CAP) {
                f0_buf = pkg->inline_f0_buf;
            } else {
                f0_buf = (uint8_t *)malloc((size_t)content_sz);
                if (!f0_buf) {
                    munmap((void *)map, map_len);
                    wwpkg_close(pkg);
                    set_tls_error(WWPKG_ERR_IO);
                    return nullptr;
                }
            }
            size_t d_res = zstd_decompress_fast(f0_buf, (size_t)content_sz, f0_src, f0_csize);
            if (g_zstd.isError(d_res) || d_res != (size_t)content_sz) {
                if (f0_buf != pkg->inline_f0_buf) {
                    free(f0_buf);
                }
                munmap((void *)map, map_len);
                wwpkg_close(pkg);
                set_tls_error(WWPKG_ERR_DECOMPRESS);
                return nullptr;
            }
            f0_len = d_res;
        } else {
            int derr = WWPKG_ERR_DECOMPRESS;
            if (!zstd_decompress_bounded(f0_src, f0_csize, WWPKG_FRAME0_MAX_BYTES, &f0_buf, &f0_len,
                                         &derr)) {
                munmap((void *)map, map_len);
                wwpkg_close(pkg);
                set_tls_error(derr);
                return nullptr;
            }
        }
    } else if (rem_size >= 512 && memcmp(f0_src + 257, "ustar", 5) == 0) {
        close(fd);
        size_t copy_sz = rem_size < WWPKG_FRAME0_MAX_BYTES ? rem_size : WWPKG_FRAME0_MAX_BYTES;
        if (copy_sz <= WWPKG_INLINE_F0_CAP) {
            f0_buf = pkg->inline_f0_buf;
        } else {
            f0_buf = (uint8_t *)malloc(copy_sz);
            if (!f0_buf) {
                munmap((void *)map, map_len);
                wwpkg_close(pkg);
                set_tls_error(WWPKG_ERR_IO);
                return nullptr;
            }
        }
        memcpy(f0_buf, f0_src, copy_sz);
        f0_len = copy_sz;
    } else {
        close(fd);
        munmap((void *)map, map_len);
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_CORRUPT);
        return nullptr;
    }

    munmap((void *)map, map_len);

    pkg->decompressed_buf = f0_buf;
    pkg->decompressed_size = f0_len;
    pkg->outer_env = env;
    pkg->has_outer_env = true;
    pkg->outer_auth_verified = true;
    pkg->is_metadata_only = true;
    if (!pkg_set_source_path(pkg, pkg_path)) {
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_IO);
        return nullptr;
    }

    if (!parse_ustar_into_package(pkg, f0_buf, f0_len, true)) {
        set_tls_error(pkg->last_error ? pkg->last_error : WWPKG_ERR_CORRUPT);
        wwpkg_close(pkg);
        return nullptr;
    }

    /* Cross-check Frame 0 manifest and signature.sig against the verified outer Ed25519 header */
    if (pkg->signature.version < 2 || pkg->signature.timestamp != env.timestamp_le ||
        memcmp(pkg->signature.author_pubkey, env.author_pubkey, 32) != 0 ||
        memcmp(pkg->manifest.author_pubkey, env.author_pubkey, 32) != 0 ||
        memcmp(pkg->signature.merkle_root, env.merkle_root, 32) != 0 ||
        memcmp(pkg->signature.signature, env.signature, 64) != 0) {
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        wwpkg_close(pkg);
        return nullptr;
    }

    /* Fast-path SHA-NI check when manifest.json is already stored in canonical JCS form */
    uint8_t h_manifest[32];
    if (pkg->has_raw_manifest_sha256) {
        memcpy(h_manifest, pkg->raw_manifest_sha256, 32);
    } else {
        wwpkg_sha256((const uint8_t *)pkg->raw_manifest_str, pkg->raw_manifest_len, h_manifest);
    }
    if (memcmp(h_manifest, env.manifest_sha256, 32) != 0) {
        size_t canon_len = 0;
        char *canon_mf =
            wwpkg_canonicalize_json(pkg->raw_manifest_str, pkg->raw_manifest_len, &canon_len);
        if (!canon_mf) {
            set_tls_error(WWPKG_ERR_BAD_MANIFEST);
            wwpkg_close(pkg);
            return nullptr;
        }
        wwpkg_sha256((const uint8_t *)canon_mf, canon_len, h_manifest);
        free(canon_mf);

        if (memcmp(h_manifest, env.manifest_sha256, 32) != 0) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
            wwpkg_close(pkg);
            return nullptr;
        }
    }

    /* Authenticate any preview or auxiliary entry present in Frame 0 before lazy promotion */
    for (size_t i = 0; i < pkg->asset_count; i++) {
        const char *aname = pkg->assets[i].name;
        if (strcmp(aname, "manifest.json") == 0 || strcmp(aname, "signature.sig") == 0) {
            continue;
        }
        if (!pkg->manifest.has_preview_sha256 ||
            strcmp(aname, pkg->manifest.entrypoint_preview) != 0) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
            wwpkg_close(pkg);
            return nullptr;
        }
        uint8_t actual_prev_sha[32];
        wwpkg_sha256(pkg->assets[i].data, pkg->assets[i].size, actual_prev_sha);
        if (memcmp(actual_prev_sha, pkg->manifest.preview_sha256, 32) != 0) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
            wwpkg_close(pkg);
            return nullptr;
        }
    }

    /* Also verify Frame 0 leaf integrity if zero bulk files exist */
    if (pkg->manifest.file_count == 0) {
        pkg->is_metadata_only = false;
        if (!wwpkg_verify_signature(pkg, nullptr)) {
            set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
            wwpkg_close(pkg);
            return nullptr;
        }
    }

    set_tls_error(WWPKG_OK);
    return pkg;
}

static bool wwpkg_promote_metadata_pkg(wwpkg_package_t *pkg)
{
    if (!pkg) {
        return false;
    }
    if (!pkg->is_metadata_only) {
        return true;
    }
    if (!pkg->source_path) {
        pkg->last_error = WWPKG_ERR_IO;
        return false;
    }

    wwpkg_package_t *full = wwpkg_open(pkg->source_path);
    if (!full) {
        pkg->last_error = g_tls_last_error;
        return false;
    }

    if (pkg->decompressed_buf != pkg->inline_f0_buf) {
        free(pkg->decompressed_buf);
    }
    if (pkg->assets != pkg->inline_assets) {
        free(pkg->assets);
    }
    if (pkg->raw_manifest_str != pkg->inline_manifest_str) {
        free(pkg->raw_manifest_str);
    }

    pkg->manifest = full->manifest;
    pkg->signature = full->signature;
    pkg->outer_env = full->outer_env;
    pkg->has_manifest = full->has_manifest;
    pkg->has_signature = full->has_signature;
    pkg->has_outer_env = full->has_outer_env;
    pkg->outer_auth_verified = full->outer_auth_verified;
    pkg->is_metadata_only = false;

    pkg->decompressed_buf = full->decompressed_buf;
    pkg->decompressed_size = full->decompressed_size;
    full->decompressed_buf = nullptr;

    if (full->assets == full->inline_assets) {
        if (full->asset_count > 0) {
            memcpy(pkg->inline_assets, full->inline_assets,
                   full->asset_count * sizeof(pkg_asset_entry_t));
        }
        pkg->assets = pkg->inline_assets;
        pkg->asset_cap = WWPKG_INLINE_ASSET_CAP;
    } else {
        pkg->assets = full->assets;
        pkg->asset_cap = full->asset_cap;
        full->assets = full->inline_assets;
    }
    pkg->asset_count = full->asset_count;

    if (full->raw_manifest_str == full->inline_manifest_str) {
        memcpy(pkg->inline_manifest_str, full->inline_manifest_str, full->raw_manifest_len + 1);
        pkg->raw_manifest_str = pkg->inline_manifest_str;
    } else {
        pkg->raw_manifest_str = full->raw_manifest_str;
        full->raw_manifest_str = full->inline_manifest_str;
    }
    pkg->raw_manifest_len = full->raw_manifest_len;

    wwpkg_close(full);
    return true;
}

const uint8_t *wwpkg_read_asset(wwpkg_package_t *pkg, const char *rel_path, size_t *out_size)
{
    if (!pkg || !rel_path) {
        if (out_size)
            *out_size = 0;
        return nullptr;
    }
    char clean[WWPKG_MAX_PATH_LEN];
    if (!sanitize_archive_path(rel_path, clean, sizeof(clean))) {
        if (out_size)
            *out_size = 0;
        return nullptr;
    }

    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, clean) == 0) {
            if (out_size)
                *out_size = pkg->assets[i].size;
            return pkg->assets[i].data;
        }
    }

    if (pkg->is_metadata_only && wwpkg_promote_metadata_pkg(pkg)) {
        for (size_t i = 0; i < pkg->asset_count; i++) {
            if (strcmp(pkg->assets[i].name, clean) == 0) {
                if (out_size)
                    *out_size = pkg->assets[i].size;
                return pkg->assets[i].data;
            }
        }
    }

    if (out_size)
        *out_size = 0;
    return nullptr;
}

/* ========================================================================= */
/* Structural JSON/JSONC Trust Store & Notary Countersignature Verification  */
/* ========================================================================= */

typedef struct {
    uint8_t (*author_pks)[32];
    size_t author_count;
    size_t author_cap;
    uint8_t (*notary_pks)[32];
    size_t notary_count;
    size_t notary_cap;
    uint8_t (*revoked_pks)[32];
    size_t revoked_count;
    size_t revoked_cap;
} parsed_trust_store_t;

static void trust_store_free(parsed_trust_store_t *ts)
{
    free(ts->author_pks);
    free(ts->notary_pks);
    free(ts->revoked_pks);
    memset(ts, 0, sizeof(*ts));
}

static bool pk_list_append(uint8_t (**list)[32], size_t *count, size_t *cap, const uint8_t pk[32])
{
    if (*count >= *cap) {
        size_t ncap = *cap ? (*cap * 2) : 8;
        uint8_t (*nl)[32] = (uint8_t (*)[32])realloc(*list, ncap * 32);
        if (!nl) {
            return false;
        }
        *list = nl;
        *cap = ncap;
    }
    memcpy((*list)[*count], pk, 32);
    (*count)++;
    return true;
}

static bool pk_list_contains_ct(uint8_t (*list)[32], size_t count, const uint8_t pk[32])
{
    bool matched = false;
    for (size_t i = 0; i < count; i++) {
        uint8_t diff = 0;
        for (int b = 0; b < 32; b++) {
            diff |= (list[i][b] ^ pk[b]);
        }
        if (diff == 0) {
            matched = true;
        }
    }
    return matched;
}

/* Strips // line comments and block comments outside JSON string literals */
static char *strip_json_comments(const char *src, size_t len, size_t *out_len)
{
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return nullptr;
    }
    size_t opos = 0;
    size_t i = 0;
    bool in_str = false;

    while (i < len) {
        char c = src[i];
        if (in_str) {
            out[opos++] = c;
            if (c == '\\' && i + 1 < len) {
                out[opos++] = src[i + 1];
                i += 2;
                continue;
            }
            if (c == '"') {
                in_str = false;
            }
            i++;
            continue;
        }

        if (c == '"') {
            in_str = true;
            out[opos++] = c;
            i++;
            continue;
        }

        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            i += 2;
            while (i < len && src[i] != '\n') {
                i++;
            }
            continue;
        }

        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i + 1 < len) {
                if (src[i] == '*' && src[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                i++;
            }
            if (!closed) {
                free(out);
                return nullptr;
            }
            out[opos++] = ' ';
            continue;
        }

        out[opos++] = c;
        i++;
    }
    out[opos] = '\0';
    *out_len = opos;
    return out;
}

static void collect_key_array(const jcs_val_t *arr, bool default_notary, bool force_revoked,
                              parsed_trust_store_t *store)
{
    if (!arr || arr->type != JCS_TYPE_ARR) {
        return;
    }
    for (size_t i = 0; i < arr->arr.count; i++) {
        const jcs_val_t *item = arr->arr.items[i];
        if (!item) {
            continue;
        }
        uint8_t pk[32];
        if (item->type == JCS_TYPE_STR) {
            if (!decode_hex32(item->sval, item->slen, pk)) {
                continue;
            }
            if (force_revoked) {
                (void)pk_list_append(&store->revoked_pks, &store->revoked_count,
                                     &store->revoked_cap, pk);
            } else if (default_notary) {
                (void)pk_list_append(&store->notary_pks, &store->notary_count, &store->notary_cap,
                                     pk);
            } else {
                (void)pk_list_append(&store->author_pks, &store->author_count, &store->author_cap,
                                     pk);
            }
        } else if (item->type == JCS_TYPE_OBJ) {
            char hex[128] = {0};
            bool got_hex = jcs_get_str_field(item, "pubkey", hex, sizeof(hex), false) && hex[0];
            if (!got_hex) {
                got_hex = jcs_get_str_field(item, "public_key", hex, sizeof(hex), false) && hex[0];
            }
            if (!got_hex) {
                got_hex = jcs_get_str_field(item, "key", hex, sizeof(hex), false) && hex[0];
            }
            if (!got_hex || !decode_hex32(hex, strlen(hex), pk)) {
                continue;
            }

            bool revoked = force_revoked;
            bool rev_flag = false;
            if (jcs_get_bool_field(item, "revoked", &rev_flag, false, false) && rev_flag) {
                revoked = true;
            }
            bool enabled_flag = true;
            if (jcs_get_bool_field(item, "enabled", &enabled_flag, false, true) && !enabled_flag) {
                revoked = true;
            }
            char status_str[32] = {0};
            if (jcs_get_str_field(item, "status", status_str, sizeof(status_str), false) &&
                status_str[0] != '\0') {
                if (strcasecmp(status_str, "revoked") == 0 ||
                    strcasecmp(status_str, "disabled") == 0) {
                    revoked = true;
                }
            }

            if (revoked) {
                (void)pk_list_append(&store->revoked_pks, &store->revoked_count,
                                     &store->revoked_cap, pk);
                continue;
            }

            bool is_notary = default_notary;
            bool notary_flag = false;
            if (jcs_get_bool_field(item, "notary", &notary_flag, false, false) && notary_flag) {
                is_notary = true;
            }
            char role_str[32] = {0};
            if (jcs_get_str_field(item, "role", role_str, sizeof(role_str), false) ||
                jcs_get_str_field(item, "type", role_str, sizeof(role_str), false)) {
                if (strcasecmp(role_str, "notary") == 0) {
                    is_notary = true;
                }
            }

            if (is_notary) {
                (void)pk_list_append(&store->notary_pks, &store->notary_count, &store->notary_cap,
                                     pk);
            } else {
                (void)pk_list_append(&store->author_pks, &store->author_count, &store->author_cap,
                                     pk);
            }
        }
    }
}

static bool load_trust_store_file(const char *path, parsed_trust_store_t *store)
{
    memset(store, 0, sizeof(*store));
    if (!path || path[0] == '\0') {
        return false;
    }

    int kfd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (kfd < 0) {
        return false;
    }

    struct stat kst;
    if (fstat(kfd, &kst) != 0 || !S_ISREG(kst.st_mode) || kst.st_size <= 0 ||
        kst.st_size > 1048576) {
        close(kfd);
        return false;
    }

    size_t fsz = (size_t)kst.st_size;
    char *kbuf = (char *)malloc(fsz + 1);
    if (!kbuf) {
        close(kfd);
        return false;
    }

    size_t rd = 0;
    while (rd < fsz) {
        ssize_t r = read(kfd, kbuf + rd, fsz - rd);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            free(kbuf);
            close(kfd);
            return false;
        }
        rd += (size_t)r;
    }
    close(kfd);
    kbuf[rd] = '\0';

    size_t stripped_len = 0;
    char *stripped = strip_json_comments(kbuf, rd, &stripped_len);
    free(kbuf);
    if (!stripped || stripped_len == 0) {
        free(stripped);
        return false;
    }

    jcs_arena_t arena;
    jcs_arena_init(&arena);
    jcs_lexer_t lx = {.src = stripped, .len = stripped_len, .pos = 0, .arena = &arena};
    jcs_val_t *root = jcs_parse_val(&lx, 0);
    if (root) {
        jcs_skip_ws(&lx);
        if (lx.pos != lx.len) {
            root = nullptr;
        }
    }
    if (!root) {
        jcs_arena_free(&arena);
        free(stripped);
        return false;
    }

    if (root->type == JCS_TYPE_ARR) {
        collect_key_array(root, false, false, store);
    } else if (root->type == JCS_TYPE_OBJ) {
        collect_key_array(jcs_find_member(root, "trusted_keys"), false, false, store);
        collect_key_array(jcs_find_member(root, "keys"), false, false, store);
        collect_key_array(jcs_find_member(root, "authors"), false, false, store);
        collect_key_array(jcs_find_member(root, "notary_keys"), true, false, store);
        collect_key_array(jcs_find_member(root, "notaries"), true, false, store);
        collect_key_array(jcs_find_member(root, "trusted_notaries"), true, false, store);
        collect_key_array(jcs_find_member(root, "revoked_keys"), false, true, store);
        collect_key_array(jcs_find_member(root, "revoked"), false, true, store);
    }

    jcs_arena_free(&arena);
    free(stripped);
    return true;
}

static bool verify_package_crypto_core(const wwpkg_package_t *pkg, uint8_t out_h_manifest[32])
{
    if (!pkg || !pkg->has_manifest || !pkg->has_signature) {
        return false;
    }

    if (pkg->is_metadata_only) {
        if (!wwpkg_promote_metadata_pkg((wwpkg_package_t *)pkg)) {
            return false;
        }
    }

    bool is_v2 = (strcmp(pkg->manifest.schema_version, "wwpkg/v2") == 0);
    bool is_v1 = (strcmp(pkg->manifest.schema_version, "wwpkg/v1") == 0);
    if (!is_v2 && !is_v1) {
        return false;
    }
    if (is_v2 && pkg->signature.version < 2) {
        return false;
    }
    if (is_v1 && pkg->signature.version != 1) {
        return false;
    }
    if (pkg->manifest.capabilities.network || pkg->manifest.capabilities.filesystem) {
        return false;
    }
    if (pkg->signature.has_notary_countersign > 1u) {
        return false;
    }
    if (memcmp(pkg->manifest.author_pubkey, pkg->signature.author_pubkey, WWPKG_PUBKEY_BYTES) !=
        0) {
        return false;
    }

    size_t merkle_count = 0;
    uint32_t payload_files = 0;
    uint64_t payload_bytes = 0;
    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "signature.sig") != 0) {
            merkle_count++;
            if (strcmp(pkg->assets[i].name, "manifest.json") != 0) {
                payload_files++;
                payload_bytes += pkg->assets[i].size;
            }
        }
    }

    if (is_v2 &&
        (payload_files != pkg->manifest.file_count || payload_bytes != pkg->manifest.total_bytes)) {
        return false;
    }

    merkle_file_t *mfiles = nullptr;
    if (merkle_count > 0) {
        mfiles = (merkle_file_t *)malloc(merkle_count * sizeof(merkle_file_t));
        if (!mfiles) {
            return false;
        }
    }

    size_t midx = 0;
    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "signature.sig") != 0) {
            size_t nlen = strlen(pkg->assets[i].name);
            memcpy(mfiles[midx].name, pkg->assets[i].name, nlen + 1);
            mfiles[midx].data = pkg->assets[i].data;
            mfiles[midx].size = pkg->assets[i].size;
            midx++;
        }
    }

    if (pkg->manifest.has_preview_sha256) {
        bool found_prev = false;
        for (size_t i = 0; i < pkg->asset_count; i++) {
            if (strcmp(pkg->assets[i].name, pkg->manifest.entrypoint_preview) == 0) {
                uint8_t prev_sha[32];
                wwpkg_sha256(pkg->assets[i].data, pkg->assets[i].size, prev_sha);
                if (memcmp(prev_sha, pkg->manifest.preview_sha256, 32) != 0) {
                    free(mfiles);
                    return false;
                }
                found_prev = true;
                break;
            }
        }
        if (!found_prev) {
            free(mfiles);
            return false;
        }
    }

    uint8_t computed_root[32];
    bool merkle_ok = compute_merkle_root(mfiles, merkle_count, is_v2, computed_root);
    free(mfiles);
    if (!merkle_ok || memcmp(computed_root, pkg->signature.merkle_root, 32) != 0) {
        return false;
    }

    size_t canonical_manifest_len = 0;
    char *canonical_manifest = wwpkg_canonicalize_json(pkg->raw_manifest_str, pkg->raw_manifest_len,
                                                       &canonical_manifest_len);
    if (!canonical_manifest) {
        return false;
    }
    uint8_t h_manifest[32];
    wwpkg_sha256((const uint8_t *)canonical_manifest, canonical_manifest_len, h_manifest);
    free(canonical_manifest);

    if (pkg->has_outer_env) {
        if (pkg->outer_env.timestamp_le != pkg->signature.timestamp ||
            memcmp(pkg->outer_env.author_pubkey, pkg->signature.author_pubkey, 32) != 0 ||
            memcmp(pkg->outer_env.merkle_root, pkg->signature.merkle_root, 32) != 0 ||
            memcmp(pkg->outer_env.manifest_sha256, h_manifest, 32) != 0 ||
            memcmp(pkg->outer_env.signature, pkg->signature.signature, 64) != 0) {
            return false;
        }
    }

    uint8_t sign_payload[85];
    build_sign_payload(pkg->signature.version, h_manifest, pkg->signature.merkle_root,
                       pkg->signature.timestamp, sign_payload);

    if (wwpkg_ed25519_verify(pkg->signature.signature, sign_payload, sizeof(sign_payload),
                             pkg->signature.author_pubkey) != 0) {
        return false;
    }

    if (pkg->signature.has_notary_countersign == 1u) {
        if (!ed25519_scalar_is_canonical(pkg->signature.notary_signature + 32)) {
            return false;
        }
        bool all_zero = true;
        for (int i = 0; i < 64; i++) {
            if (pkg->signature.notary_signature[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            return false;
        }
    }

    if (out_h_manifest) {
        memcpy(out_h_manifest, h_manifest, 32);
    }
    return true;
}

wwpkg_trust_level_t wwpkg_verify_trust_with_store(const wwpkg_package_t *pkg,
                                                  const char *trusted_keys_path)
{
    uint8_t h_manifest[32];
    if (!verify_package_crypto_core(pkg, h_manifest)) {
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return WWPKG_TRUST_INVALID;
    }

    char default_path[PATH_MAX] = {0};
    if (!trusted_keys_path) {
        const char *home = getenv("HOME");
        if (home && home[0] != '\0') {
            int n = snprintf(default_path, sizeof(default_path),
                             "%s/.config/waywal/trusted_keys.json", home);
            if (n > 0 && (size_t)n < sizeof(default_path)) {
                trusted_keys_path = default_path;
            }
        }
    }

    parsed_trust_store_t store;
    if (!load_trust_store_file(trusted_keys_path, &store)) {
        set_tls_error(WWPKG_ERR_UNTRUSTED);
        return WWPKG_TRUST_UNTRUSTED;
    }

    if (pk_list_contains_ct(store.revoked_pks, store.revoked_count, pkg->signature.author_pubkey)) {
        trust_store_free(&store);
        set_tls_error(WWPKG_ERR_UNTRUSTED);
        return WWPKG_TRUST_UNTRUSTED;
    }

    if (pkg->signature.has_notary_countersign == 1u && store.notary_count > 0) {
        uint8_t notary_payload[184];
        build_notary_payload(pkg->signature.author_pubkey, h_manifest, pkg->signature.merkle_root,
                             pkg->signature.timestamp, pkg->signature.signature, notary_payload);
        for (size_t i = 0; i < store.notary_count; i++) {
            if (pk_list_contains_ct(store.revoked_pks, store.revoked_count, store.notary_pks[i])) {
                continue;
            }
            if (wwpkg_ed25519_verify(pkg->signature.notary_signature, notary_payload,
                                     sizeof(notary_payload), store.notary_pks[i]) == 0) {
                trust_store_free(&store);
                set_tls_error(WWPKG_OK);
                return WWPKG_TRUST_NOTARIZED;
            }
        }
    }

    bool author_trusted =
        pk_list_contains_ct(store.author_pks, store.author_count, pkg->signature.author_pubkey);
    trust_store_free(&store);

    if (author_trusted) {
        set_tls_error(WWPKG_OK);
        return WWPKG_TRUST_AUTHOR_TRUSTED;
    }
    set_tls_error(WWPKG_ERR_UNTRUSTED);
    return WWPKG_TRUST_UNTRUSTED;
}

wwpkg_trust_level_t wwpkg_verify_trust(const wwpkg_package_t *pkg)
{
    return wwpkg_verify_trust_with_store(pkg, nullptr);
}

bool wwpkg_verify_signature(const wwpkg_package_t *pkg, bool *out_is_trusted)
{
    if (!out_is_trusted) {
        bool ok = verify_package_crypto_core(pkg, nullptr);
        set_tls_error(ok ? WWPKG_OK : WWPKG_ERR_BAD_SIGNATURE);
        return ok;
    }

    wwpkg_trust_level_t lvl = wwpkg_verify_trust(pkg);
    if (lvl == WWPKG_TRUST_INVALID) {
        *out_is_trusted = false;
        return false;
    }
    *out_is_trusted = (lvl == WWPKG_TRUST_AUTHOR_TRUSTED || lvl == WWPKG_TRUST_NOTARIZED);
    return true;
}

/* ========================================================================= */
/* Confined Extraction (`wwpkg_extract_all`)                                 */
/* ========================================================================= */

#if !defined(WWPKG_HAS_OPENAT2_HDR)
struct wwpkg_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};
#define WWPKG_RESOLVE_NO_MAGICLINKS 0x02ULL
#define WWPKG_RESOLVE_NO_SYMLINKS   0x04ULL
#define WWPKG_RESOLVE_BENEATH       0x08ULL
#else
#define wwpkg_open_how              open_how
#define WWPKG_RESOLVE_NO_MAGICLINKS RESOLVE_NO_MAGICLINKS
#define WWPKG_RESOLVE_NO_SYMLINKS   RESOLVE_NO_SYMLINKS
#define WWPKG_RESOLVE_BENEATH       RESOLVE_BENEATH
#endif

static int open_confined_write(int root_dfd, const char *rel_path)
{
    /* Walk and create intermediate directories with O_NOFOLLOW to prevent symlink escapes */
    int cur_dfd = fcntl(root_dfd, F_DUPFD_CLOEXEC, 0);
    if (cur_dfd < 0) {
        return -1;
    }

    const char *p = rel_path;
    while (true) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            break;
        }
        size_t clen = (size_t)(slash - p);
        if (clen == 0 || clen >= NAME_MAX) {
            close(cur_dfd);
            return -1;
        }
        char comp[NAME_MAX + 1];
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        if (mkdirat(cur_dfd, comp, 0755) != 0 && errno != EEXIST) {
            close(cur_dfd);
            return -1;
        }
        int next_dfd = openat(cur_dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(cur_dfd);
        if (next_dfd < 0) {
            return -1;
        }
        cur_dfd = next_dfd;
        p = slash + 1;
    }

#ifdef SYS_openat2
    struct wwpkg_open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
    how.mode = 0644;
    how.resolve = WWPKG_RESOLVE_BENEATH | WWPKG_RESOLVE_NO_SYMLINKS | WWPKG_RESOLVE_NO_MAGICLINKS;
    long fd2 = syscall(SYS_openat2, root_dfd, rel_path, &how, sizeof(how));
    if (fd2 >= 0) {
        close(cur_dfd);
        return (int)fd2;
    }
    if (errno != ENOSYS && errno != EPERM) {
        close(cur_dfd);
        return -1;
    }
#endif

    int leaf_fd = openat(cur_dfd, p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    close(cur_dfd);
    return leaf_fd;
}

bool wwpkg_extract_all(wwpkg_package_t *pkg, const char *dest_dir)
{
    if (!pkg || !dest_dir || dest_dir[0] == '\0') {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return false;
    }

    if (!wwpkg_verify_signature(pkg, nullptr)) {
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return false;
    }

    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    int root_dfd = open(dest_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_dfd < 0) {
        set_tls_error(WWPKG_ERR_PATH_TRAVERSAL);
        return false;
    }

    for (size_t i = 0; i < pkg->asset_count; i++) {
        char clean[WWPKG_MAX_PATH_LEN];
        if (!sanitize_archive_path(pkg->assets[i].name, clean, sizeof(clean))) {
            close(root_dfd);
            set_tls_error(WWPKG_ERR_PATH_TRAVERSAL);
            return false;
        }

        int out_fd = open_confined_write(root_dfd, clean);
        if (out_fd < 0) {
            close(root_dfd);
            set_tls_error(WWPKG_ERR_PATH_TRAVERSAL);
            return false;
        }

        size_t wr = 0;
        const uint8_t *src = pkg->assets[i].data;
        size_t sz = pkg->assets[i].size;
        while (wr < sz) {
            ssize_t w = write(out_fd, src + wr, sz - wr);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) {
                    continue;
                }
                close(out_fd);
                close(root_dfd);
                set_tls_error(WWPKG_ERR_IO);
                return false;
            }
            wr += (size_t)w;
        }
        close(out_fd);
    }

    close(root_dfd);
    set_tls_error(WWPKG_OK);
    return true;
}

/* ========================================================================= */
/* Dual-Frame Package Compiler, Builder, and Notary Signer                   */
/* ========================================================================= */

typedef struct {
    char name[WWPKG_MAX_PATH_LEN];
    uint8_t *data;
    size_t size;
} build_file_entry_t;

typedef struct {
    build_file_entry_t *entries;
    size_t count;
    size_t cap;
    uint64_t total_bytes;
} build_file_list_t;

static void build_list_free(build_file_list_t *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->entries[i].data);
    }
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

static bool build_list_add(build_file_list_t *list, const char *name, uint8_t *data, size_t size)
{
    if (!name || list->count >= WWPKG_MAX_FILE_COUNT + 2u) {
        return false;
    }
    if (list->count >= list->cap) {
        size_t ncap = list->cap ? list->cap * 2 : 16;
        build_file_entry_t *ne =
            (build_file_entry_t *)realloc(list->entries, ncap * sizeof(build_file_entry_t));
        if (!ne) {
            return false;
        }
        list->entries = ne;
        list->cap = ncap;
    }
    size_t nlen = strlen(name);
    if (nlen >= sizeof(list->entries[list->count].name)) {
        return false;
    }
    memcpy(list->entries[list->count].name, name, nlen + 1);
    list->entries[list->count].data = data;
    list->entries[list->count].size = size;
    list->count++;
    return true;
}

static int open_atomic_temp_file(const char *target_path, char out_tmp_path[PATH_MAX], mode_t mode)
{
    for (int attempt = 0; attempt < 16; attempt++) {
        uint32_t rnd = 0;
        if (secure_random_bytes((uint8_t *)&rnd, sizeof(rnd)) != 0) {
            rnd = (uint32_t)attempt ^ (uint32_t)getpid();
        }
        int n = snprintf(out_tmp_path, PATH_MAX, "%s.tmp.%d.%08x", target_path, (int)getpid(), rnd);
        if (n <= 0 || (size_t)n >= PATH_MAX) {
            return -1;
        }
        int fd = open(out_tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd >= 0) {
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
}

static bool write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t wr = 0;
    while (wr < len) {
        ssize_t w = write(fd, buf + wr, len - wr);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        wr += (size_t)w;
    }
    return true;
}

#define WWPKG_MAX_SOURCE_DEPTH 32u

static bool collect_source_files(const char *root_dir, const char *rel_dir, uint32_t depth,
                                 build_file_list_t *list, int *out_err)
{
    if (depth > WWPKG_MAX_SOURCE_DEPTH) {
        if (out_err)
            *out_err = WWPKG_ERR_PATH_TRAVERSAL;
        return false;
    }

    char dir_path[PATH_MAX];
    int dn = (rel_dir && rel_dir[0])
                 ? snprintf(dir_path, sizeof(dir_path), "%s/%s", root_dir, rel_dir)
                 : snprintf(dir_path, sizeof(dir_path), "%s", root_dir);
    if (dn <= 0 || (size_t)dn >= sizeof(dir_path)) {
        if (out_err)
            *out_err = WWPKG_ERR_PATH_TRAVERSAL;
        return false;
    }

    int dfd_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (depth > 0 ? O_NOFOLLOW : 0);
    int dfd = open(dir_path, dfd_flags);
    if (dfd < 0) {
        if (out_err)
            *out_err = (errno == ELOOP) ? WWPKG_ERR_PATH_TRAVERSAL : WWPKG_ERR_IO;
        return false;
    }

    DIR *d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        if (out_err)
            *out_err = WWPKG_ERR_IO;
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char sub_rel[WWPKG_MAX_PATH_LEN];
        int sn = (rel_dir && rel_dir[0])
                     ? snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_dir, ent->d_name)
                     : snprintf(sub_rel, sizeof(sub_rel), "%s", ent->d_name);
        if (sn <= 0 || (size_t)sn >= sizeof(sub_rel)) {
            closedir(d);
            if (out_err)
                *out_err = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }

        char full_entry[PATH_MAX];
        int fn = snprintf(full_entry, sizeof(full_entry), "%s/%s", root_dir, sub_rel);
        if (fn <= 0 || (size_t)fn >= sizeof(full_entry)) {
            closedir(d);
            if (out_err)
                *out_err = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }

        struct stat st;
        if (lstat(full_entry, &st) != 0) {
            closedir(d);
            if (out_err)
                *out_err = WWPKG_ERR_IO;
            return false;
        }

        if (S_ISLNK(st.st_mode)) {
            /* Never follow symlinks out of source root */
            closedir(d);
            if (out_err)
                *out_err = WWPKG_ERR_PATH_TRAVERSAL;
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!collect_source_files(root_dir, sub_rel, depth + 1, list, out_err)) {
                closedir(d);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (strcmp(sub_rel, "signature.sig") == 0 || strcmp(sub_rel, "manifest.json") == 0) {
                continue;
            }

            char clean[WWPKG_MAX_PATH_LEN];
            char check_prefix[155], check_name[100];
            if (!sanitize_archive_path(sub_rel, clean, sizeof(clean)) ||
                !tar_split_ustar_name(clean, check_prefix, check_name)) {
                closedir(d);
                if (out_err)
                    *out_err = WWPKG_ERR_PATH_TRAVERSAL;
                return false;
            }

            if (st.st_size < 0 ||
                (uint64_t)st.st_size > WWPKG_MAX_ARCHIVE_BYTES - list->total_bytes ||
                list->count >= WWPKG_MAX_FILE_COUNT) {
                closedir(d);
                if (out_err)
                    *out_err = WWPKG_ERR_DECOMPRESS;
                return false;
            }

            int fd = open(full_entry, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                closedir(d);
                if (out_err)
                    *out_err = (errno == ELOOP) ? WWPKG_ERR_PATH_TRAVERSAL : WWPKG_ERR_IO;
                return false;
            }

            struct stat fst;
            if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size != st.st_size) {
                close(fd);
                closedir(d);
                if (out_err)
                    *out_err = WWPKG_ERR_IO;
                return false;
            }

            size_t sz = (size_t)fst.st_size;
            uint8_t *fbuf = (uint8_t *)malloc(sz ? sz : 1u);
            if (!fbuf) {
                close(fd);
                closedir(d);
                if (out_err)
                    *out_err = WWPKG_ERR_IO;
                return false;
            }

            size_t rd = 0;
            while (rd < sz) {
                ssize_t r = read(fd, fbuf + rd, sz - rd);
                if (r <= 0) {
                    if (r < 0 && errno == EINTR) {
                        continue;
                    }
                    free(fbuf);
                    close(fd);
                    closedir(d);
                    if (out_err)
                        *out_err = WWPKG_ERR_IO;
                    return false;
                }
                rd += (size_t)r;
            }
            close(fd);

            if (!build_list_add(list, clean, fbuf, sz)) {
                free(fbuf);
                closedir(d);
                if (out_err)
                    *out_err = WWPKG_ERR_IO;
                return false;
            }
            list->total_bytes += sz;
        }
    }

    closedir(d);
    return true;
}

static bool write_dual_frame_archive(const char *out_pkg_path, const wwpkg_outer_envelope_t *env,
                                     const uint8_t *frame0_tar, size_t frame0_tar_len,
                                     const uint8_t *bulk_tar, size_t bulk_tar_len)
{
    zstd_probe();

    char tmp_out_path[PATH_MAX];
    int out_fd = open_atomic_temp_file(out_pkg_path, tmp_out_path, 0644);
    if (out_fd < 0) {
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    if (!g_zstd_available) {
        /* Uncompressed ustar fallback when libzstd is absent */
        bool ok = write_all_fd(out_fd, frame0_tar, frame0_tar_len) &&
                  (bulk_tar_len == 0 || write_all_fd(out_fd, bulk_tar, bulk_tar_len));
        close(out_fd);
        if (!ok || rename(tmp_out_path, out_pkg_path) != 0) {
            unlink(tmp_out_path);
            set_tls_error(WWPKG_ERR_IO);
            return false;
        }
        return true;
    }

    uint8_t env_bytes[sizeof(wwpkg_outer_envelope_t)];
    write_u32_le(env_bytes + 0, WWPKG_MAGIC_SKIPPABLE_ENV);
    write_u32_le(env_bytes + 4, WWPKG_OUTER_ENV_PAYLOAD);
    memcpy(env_bytes + 8, env->author_pubkey, 32);
    memcpy(env_bytes + 40, env->manifest_sha256, 32);
    memcpy(env_bytes + 72, env->merkle_root, 32);
    write_u64_le(env_bytes + 104, env->timestamp_le);
    memcpy(env_bytes + 112, env->signature, 64);

    if (!write_all_fd(out_fd, env_bytes, sizeof(env_bytes))) {
        close(out_fd);
        unlink(tmp_out_path);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    /* Frame 0 (< 64 KiB): fast Zstd level 3 for sub-60us metadata inspection */
    size_t f0_bound = g_zstd.compressBound(frame0_tar_len);
    uint8_t *f0_comp = (uint8_t *)malloc(f0_bound);
    if (!f0_comp) {
        close(out_fd);
        unlink(tmp_out_path);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }
    size_t f0_csize = g_zstd.compress(f0_comp, f0_bound, frame0_tar, frame0_tar_len, 3);
    if (g_zstd.isError(f0_csize) || !write_all_fd(out_fd, f0_comp, f0_csize)) {
        free(f0_comp);
        close(out_fd);
        unlink(tmp_out_path);
        set_tls_error(WWPKG_ERR_DECOMPRESS);
        return false;
    }
    free(f0_comp);

    /* Frame 1..K: compress bulk payload in 1 MiB Zstd frames */
    const size_t chunk_max = 1024u * 1024u;
    size_t bulk_off = 0;
    uint8_t *chunk_comp = nullptr;
    size_t chunk_comp_cap = 0;

    while (bulk_off < bulk_tar_len) {
        size_t chunk_len = bulk_tar_len - bulk_off;
        if (chunk_len > chunk_max) {
            chunk_len = chunk_max;
        }
        size_t cbound = g_zstd.compressBound(chunk_len);
        if (cbound > chunk_comp_cap) {
            uint8_t *nc = (uint8_t *)realloc(chunk_comp, cbound);
            if (!nc) {
                free(chunk_comp);
                close(out_fd);
                unlink(tmp_out_path);
                set_tls_error(WWPKG_ERR_IO);
                return false;
            }
            chunk_comp = nc;
            chunk_comp_cap = cbound;
        }
        size_t csize =
            g_zstd.compress(chunk_comp, chunk_comp_cap, bulk_tar + bulk_off, chunk_len, 12);
        if (g_zstd.isError(csize) || !write_all_fd(out_fd, chunk_comp, csize)) {
            free(chunk_comp);
            close(out_fd);
            unlink(tmp_out_path);
            set_tls_error(WWPKG_ERR_DECOMPRESS);
            return false;
        }
        bulk_off += chunk_len;
    }
    free(chunk_comp);
    close(out_fd);

    if (rename(tmp_out_path, out_pkg_path) != 0) {
        unlink(tmp_out_path);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }
    return true;
}

bool wwpkg_build_package(const char *source_dir, const wwpkg_manifest_t *manifest,
                         const uint8_t priv_key[64], const char *out_pkg_path)
{
    if (!source_dir || !priv_key || !out_pkg_path) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return false;
    }

    wwpkg_manifest_t active_manifest;
    memset(&active_manifest, 0, sizeof(active_manifest));

    if (manifest) {
        active_manifest = *manifest;
    } else {
        char mf_path[PATH_MAX];
        int mn = snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", source_dir);
        if (mn <= 0 || (size_t)mn >= sizeof(mf_path)) {
            set_tls_error(WWPKG_ERR_PATH_TRAVERSAL);
            return false;
        }
        int mfd = open(mf_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (mfd < 0) {
            set_tls_error(WWPKG_ERR_IO);
            return false;
        }
        struct stat mst;
        if (fstat(mfd, &mst) != 0 || !S_ISREG(mst.st_mode) || mst.st_size <= 0 ||
            mst.st_size > (off_t)WWPKG_FRAME0_MAX_BYTES) {
            close(mfd);
            set_tls_error(WWPKG_ERR_BAD_MANIFEST);
            return false;
        }
        size_t msz = (size_t)mst.st_size;
        char *mbuf = (char *)malloc(msz + 1);
        if (!mbuf) {
            close(mfd);
            set_tls_error(WWPKG_ERR_IO);
            return false;
        }
        size_t rd = 0;
        while (rd < msz) {
            ssize_t r = read(mfd, mbuf + rd, msz - rd);
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                break;
            }
            rd += (size_t)r;
        }
        close(mfd);
        if (rd != msz) {
            free(mbuf);
            set_tls_error(WWPKG_ERR_IO);
            return false;
        }
        mbuf[msz] = '\0';
        int mf_err = WWPKG_ERR_BAD_MANIFEST;
        bool parsed = parse_manifest_from_string(mbuf, msz, &active_manifest, &mf_err);
        free(mbuf);
        if (!parsed) {
            set_tls_error(mf_err);
            return false;
        }
    }

    if (active_manifest.capabilities.network || active_manifest.capabilities.filesystem) {
        set_tls_error(WWPKG_ERR_CAPABILITY);
        return false;
    }

    /* Derive effective_sk = seed || derived_pk; never pass zeroed [32..63] to signing */
    uint8_t effective_sk[64];
    uint8_t derived_pubkey[32];
    bool caller_supplied_pub = false;
    for (int i = 0; i < 32; i++) {
        if (priv_key[32 + i] != 0) {
            caller_supplied_pub = true;
            break;
        }
    }
    if (wwpkg_ed25519_seed_keypair(derived_pubkey, effective_sk, priv_key) != 0) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return false;
    }
    if (caller_supplied_pub && memcmp(derived_pubkey, priv_key + 32, 32) != 0) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return false;
    }

    build_file_list_t payload_list = {0};
    int collect_err = WWPKG_ERR_IO;
    if (!collect_source_files(source_dir, "", 0, &payload_list, &collect_err)) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        build_list_free(&payload_list);
        set_tls_error(collect_err);
        return false;
    }

    memcpy(active_manifest.author_pubkey, derived_pubkey, 32);
    memcpy(active_manifest.schema_version, "wwpkg/v2", 9);
    active_manifest.file_count = (uint32_t)payload_list.count;
    active_manifest.total_bytes = payload_list.total_bytes;

    char norm_prev[WWPKG_MAX_PATH_LEN] = {0};
    const char *raw_prev =
        active_manifest.entrypoint_preview[0] ? active_manifest.entrypoint_preview : "preview.webp";
    if (!sanitize_archive_path(raw_prev, norm_prev, sizeof(norm_prev))) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return false;
    }
    memcpy(active_manifest.entrypoint_preview, norm_prev, strlen(norm_prev) + 1);

    ssize_t preview_idx = -1;
    for (size_t i = 0; i < payload_list.count; i++) {
        if (strcmp(payload_list.entries[i].name, active_manifest.entrypoint_preview) == 0) {
            preview_idx = (ssize_t)i;
            break;
        }
    }
    if (preview_idx >= 0) {
        wwpkg_sha256(payload_list.entries[preview_idx].data, payload_list.entries[preview_idx].size,
                     active_manifest.preview_sha256);
        active_manifest.has_preview_sha256 = true;
    } else {
        memset(active_manifest.preview_sha256, 0, sizeof(active_manifest.preview_sha256));
        active_manifest.has_preview_sha256 = false;
    }

    size_t canonical_manifest_len = 0;
    char *canonical_manifest =
        manifest_to_canonical_json(&active_manifest, &canonical_manifest_len);
    if (!canonical_manifest) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return false;
    }

    /* Validate generated manifest round-trips cleanly through strict schema validator */
    wwpkg_manifest_t check_mf;
    int check_err = WWPKG_ERR_BAD_MANIFEST;
    if (!parse_manifest_from_string(canonical_manifest, canonical_manifest_len, &check_mf,
                                    &check_err)) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(check_err);
        return false;
    }

    size_t merkle_count = payload_list.count + 1u;
    merkle_file_t *mfiles = (merkle_file_t *)malloc(merkle_count * sizeof(merkle_file_t));
    if (!mfiles) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }
    for (size_t i = 0; i < payload_list.count; i++) {
        size_t nlen = strlen(payload_list.entries[i].name);
        memcpy(mfiles[i].name, payload_list.entries[i].name, nlen + 1);
        mfiles[i].data = payload_list.entries[i].data;
        mfiles[i].size = payload_list.entries[i].size;
    }
    memcpy(mfiles[payload_list.count].name, "manifest.json", 14);
    mfiles[payload_list.count].data = (const uint8_t *)canonical_manifest;
    mfiles[payload_list.count].size = canonical_manifest_len;

    uint8_t merkle_root[32];
    bool merkle_ok = compute_merkle_root(mfiles, merkle_count, true, merkle_root);
    free(mfiles);
    if (!merkle_ok) {
        explicit_bzero(effective_sk, sizeof(effective_sk));
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    uint8_t h_manifest[32];
    wwpkg_sha256((const uint8_t *)canonical_manifest, canonical_manifest_len, h_manifest);

    wwpkg_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    sig.version = 2;
    sig.timestamp = (uint64_t)time(nullptr);
    memcpy(sig.author_pubkey, derived_pubkey, 32);
    memcpy(sig.merkle_root, merkle_root, 32);

    uint8_t sign_payload[85];
    build_sign_payload(2, h_manifest, merkle_root, sig.timestamp, sign_payload);

    int sign_rc =
        wwpkg_ed25519_sign(sig.signature, sign_payload, sizeof(sign_payload), effective_sk);
    explicit_bzero(effective_sk, sizeof(effective_sk));
    if (sign_rc != 0) {
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return false;
    }

    /* Partition ustar entries into Frame 0 (< 64 KiB metadata + preview) and Frame 1+ (bulk) */

    size_t mf_tar_sz = 512u + (((canonical_manifest_len + 511u) / 512u) * 512u);
    size_t sig_tar_sz = 512u + 512u;
    size_t f0_tar_len = mf_tar_sz + sig_tar_sz;
    bool preview_in_f0 = false;
    size_t bulk_file_count = payload_list.count;

    if (preview_idx >= 0) {
        size_t prev_sz = payload_list.entries[preview_idx].size;
        size_t prev_tar_sz = 512u + (((prev_sz + 511u) / 512u) * 512u);
        size_t eof_if_no_bulk = (payload_list.count == 1u) ? 1024u : 0u;
        if (f0_tar_len + prev_tar_sz + eof_if_no_bulk <= WWPKG_FRAME0_MAX_BYTES) {
            preview_in_f0 = true;
            f0_tar_len += prev_tar_sz;
            bulk_file_count--;
        }
    }

    if (bulk_file_count == 0) {
        f0_tar_len += 1024u;
    }
    if (f0_tar_len > WWPKG_FRAME0_MAX_BYTES) {
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_BAD_MANIFEST);
        return false;
    }

    size_t bulk_tar_len = 0;
    if (bulk_file_count > 0) {
        for (size_t i = 0; i < payload_list.count; i++) {
            if (preview_in_f0 && (ssize_t)i == preview_idx) {
                continue;
            }
            size_t pad = ((payload_list.entries[i].size + 511u) / 512u) * 512u;
            bulk_tar_len += 512u + pad;
        }
        bulk_tar_len += 1024u;
    }

    uint8_t *f0_tar = (uint8_t *)calloc(1, f0_tar_len);
    uint8_t *bulk_tar = bulk_tar_len ? (uint8_t *)calloc(1, bulk_tar_len) : nullptr;
    if (!f0_tar || (bulk_tar_len > 0 && !bulk_tar)) {
        free(f0_tar);
        free(bulk_tar);
        free(canonical_manifest);
        build_list_free(&payload_list);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    size_t f0_off = 0;
    size_t written = 0;
    bool tar_ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off, "manifest.json",
                                  (const uint8_t *)canonical_manifest, canonical_manifest_len,
                                  sig.timestamp, &written);
    f0_off += written;
    if (tar_ok) {
        tar_ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off, "signature.sig",
                                 (const uint8_t *)&sig, sizeof(sig), sig.timestamp, &written);
        f0_off += written;
    }
    if (tar_ok && preview_in_f0) {
        tar_ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off,
                                 payload_list.entries[preview_idx].name,
                                 payload_list.entries[preview_idx].data,
                                 payload_list.entries[preview_idx].size, sig.timestamp, &written);
        f0_off += written;
    }

    size_t bulk_off = 0;
    for (size_t i = 0; tar_ok && i < payload_list.count; i++) {
        if (preview_in_f0 && (ssize_t)i == preview_idx) {
            continue;
        }
        tar_ok = tar_write_entry(bulk_tar + bulk_off, bulk_tar_len - bulk_off,
                                 payload_list.entries[i].name, payload_list.entries[i].data,
                                 payload_list.entries[i].size, sig.timestamp, &written);
        bulk_off += written;
    }

    free(canonical_manifest);
    build_list_free(&payload_list);

    if (!tar_ok) {
        free(f0_tar);
        free(bulk_tar);
        set_tls_error(WWPKG_ERR_PATH_TRAVERSAL);
        return false;
    }

    wwpkg_outer_envelope_t env;
    memset(&env, 0, sizeof(env));
    env.magic_le = WWPKG_MAGIC_SKIPPABLE_ENV;
    env.frame_size_le = WWPKG_OUTER_ENV_PAYLOAD;
    memcpy(env.author_pubkey, derived_pubkey, 32);
    memcpy(env.manifest_sha256, h_manifest, 32);
    memcpy(env.merkle_root, merkle_root, 32);
    env.timestamp_le = sig.timestamp;
    memcpy(env.signature, sig.signature, 64);

    bool wrote =
        write_dual_frame_archive(out_pkg_path, &env, f0_tar, f0_tar_len, bulk_tar, bulk_tar_len);
    free(f0_tar);
    free(bulk_tar);
    if (wrote) {
        set_tls_error(WWPKG_OK);
    }
    return wrote;
}

bool wwpkg_notarize_package(const char *pkg_path, const uint8_t notary_sk[64])
{
    if (!pkg_path || !notary_sk) {
        set_tls_error(WWPKG_ERR_INVALID_PARAM);
        return false;
    }

    wwpkg_package_t *pkg = wwpkg_open(pkg_path);
    if (!pkg) {
        return false;
    }

    uint8_t h_manifest[32];
    if (!verify_package_crypto_core(pkg, h_manifest)) {
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return false;
    }

    uint8_t notary_payload[184];
    build_notary_payload(pkg->signature.author_pubkey, h_manifest, pkg->signature.merkle_root,
                         pkg->signature.timestamp, pkg->signature.signature, notary_payload);

    wwpkg_signature_t new_sig = pkg->signature;
    new_sig.has_notary_countersign = 1u;
    if (wwpkg_ed25519_sign(new_sig.notary_signature, notary_payload, sizeof(notary_payload),
                           notary_sk) != 0) {
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_BAD_SIGNATURE);
        return false;
    }

    /* Rebuild Frame 0 + bulk frames preserving original manifest, timestamp, and author sig */
    const char *preview_rel =
        pkg->manifest.entrypoint_preview[0] ? pkg->manifest.entrypoint_preview : "preview.webp";
    ssize_t preview_idx = -1;
    size_t payload_count = 0;
    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "manifest.json") == 0 ||
            strcmp(pkg->assets[i].name, "signature.sig") == 0) {
            continue;
        }
        if (strcmp(pkg->assets[i].name, preview_rel) == 0) {
            preview_idx = (ssize_t)i;
        }
        payload_count++;
    }

    size_t mf_tar_sz = 512u + (((pkg->raw_manifest_len + 511u) / 512u) * 512u);
    size_t f0_tar_len = mf_tar_sz + 1024u;
    bool preview_in_f0 = false;
    size_t bulk_count = payload_count;

    if (preview_idx >= 0) {
        size_t psz = pkg->assets[preview_idx].size;
        size_t p_tar = 512u + (((psz + 511u) / 512u) * 512u);
        size_t eof_pad = (payload_count == 1u) ? 1024u : 0u;
        if (f0_tar_len + p_tar + eof_pad <= WWPKG_FRAME0_MAX_BYTES) {
            preview_in_f0 = true;
            f0_tar_len += p_tar;
            bulk_count--;
        }
    }
    if (bulk_count == 0) {
        f0_tar_len += 1024u;
    }

    size_t bulk_tar_len = 0;
    if (bulk_count > 0) {
        for (size_t i = 0; i < pkg->asset_count; i++) {
            if (strcmp(pkg->assets[i].name, "manifest.json") == 0 ||
                strcmp(pkg->assets[i].name, "signature.sig") == 0 ||
                (preview_in_f0 && (ssize_t)i == preview_idx)) {
                continue;
            }
            bulk_tar_len += 512u + (((pkg->assets[i].size + 511u) / 512u) * 512u);
        }
        bulk_tar_len += 1024u;
    }

    uint8_t *f0_tar = (uint8_t *)calloc(1, f0_tar_len);
    uint8_t *bulk_tar = bulk_tar_len ? (uint8_t *)calloc(1, bulk_tar_len) : nullptr;
    if (!f0_tar || (bulk_tar_len > 0 && !bulk_tar)) {
        free(f0_tar);
        free(bulk_tar);
        wwpkg_close(pkg);
        set_tls_error(WWPKG_ERR_IO);
        return false;
    }

    size_t f0_off = 0, written = 0;
    bool ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off, "manifest.json",
                              (const uint8_t *)pkg->raw_manifest_str, pkg->raw_manifest_len,
                              new_sig.timestamp, &written);
    f0_off += written;
    if (ok) {
        ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off, "signature.sig",
                             (const uint8_t *)&new_sig, sizeof(new_sig), new_sig.timestamp,
                             &written);
        f0_off += written;
    }
    if (ok && preview_in_f0) {
        ok = tar_write_entry(f0_tar + f0_off, f0_tar_len - f0_off, pkg->assets[preview_idx].name,
                             pkg->assets[preview_idx].data, pkg->assets[preview_idx].size,
                             new_sig.timestamp, &written);
        f0_off += written;
    }

    size_t bulk_off = 0;
    for (size_t i = 0; ok && i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "manifest.json") == 0 ||
            strcmp(pkg->assets[i].name, "signature.sig") == 0 ||
            (preview_in_f0 && (ssize_t)i == preview_idx)) {
            continue;
        }
        ok = tar_write_entry(bulk_tar + bulk_off, bulk_tar_len - bulk_off, pkg->assets[i].name,
                             pkg->assets[i].data, pkg->assets[i].size, new_sig.timestamp, &written);
        bulk_off += written;
    }

    wwpkg_outer_envelope_t env;
    memset(&env, 0, sizeof(env));
    env.magic_le = WWPKG_MAGIC_SKIPPABLE_ENV;
    env.frame_size_le = WWPKG_OUTER_ENV_PAYLOAD;
    memcpy(env.author_pubkey, new_sig.author_pubkey, 32);
    memcpy(env.manifest_sha256, h_manifest, 32);
    memcpy(env.merkle_root, new_sig.merkle_root, 32);
    env.timestamp_le = new_sig.timestamp;
    memcpy(env.signature, new_sig.signature, 64);

    bool wrote =
        ok && write_dual_frame_archive(pkg_path, &env, f0_tar, f0_tar_len, bulk_tar, bulk_tar_len);
    free(f0_tar);
    free(bulk_tar);
    wwpkg_close(pkg);
    return wrote;
}
