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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================= */
/* Error Strings & Diagnostic Utilities                                      */
/* ========================================================================= */

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
        return "Zstandard decompression error or missing libzstd";
    case WWPKG_ERR_BAD_MANIFEST:
        return "Invalid or malformed manifest.json schema";
    case WWPKG_ERR_BAD_SIGNATURE:
        return "Cryptographic signature or Merkle root verification failed";
    case WWPKG_ERR_UNTRUSTED:
        return "Author public key not in trusted_keys.json";
    case WWPKG_ERR_CAPABILITY:
        return "Forbidden capability claimed (network or filesystem must be false)";
    case WWPKG_ERR_PATH_TRAVERSAL:
        return "Archive path traversal attempt rejected";
    default:
        return "Unknown error";
    }
}

/* ========================================================================= */
/* Dynamic Library Loading (Lazy dlopen for Zero DT_NEEDED Compliance)       */
/* ========================================================================= */

/* Zstandard API function pointers */
typedef struct {
    size_t (*compress)(void *dst, size_t dstCapacity, const void *src, size_t srcSize,
                       int compressionLevel);
    size_t (*decompress)(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
    unsigned long long (*getFrameContentSize)(const void *src, size_t srcSize);
    unsigned (*isError)(size_t code);
    const char *(*getErrorName)(size_t code);
    size_t (*compressBound)(size_t srcSize);
} zstd_funcs_t;

static void *g_zstd_handle = nullptr;
static bool g_zstd_probed = false;
static bool g_zstd_available = false;
static zstd_funcs_t g_zstd;

static void zstd_probe(void)
{
    if (g_zstd_probed) {
        return;
    }
    g_zstd_probed = true;

    g_zstd_handle = dlopen("libzstd.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!g_zstd_handle) {
        g_zstd_handle = dlopen("libzstd.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!g_zstd_handle) {
        return;
    }

    *(void **)(&g_zstd.compress) = dlsym(g_zstd_handle, "ZSTD_compress");
    *(void **)(&g_zstd.decompress) = dlsym(g_zstd_handle, "ZSTD_decompress");
    *(void **)(&g_zstd.getFrameContentSize) = dlsym(g_zstd_handle, "ZSTD_getFrameContentSize");
    *(void **)(&g_zstd.isError) = dlsym(g_zstd_handle, "ZSTD_isError");
    *(void **)(&g_zstd.getErrorName) = dlsym(g_zstd_handle, "ZSTD_getErrorName");
    *(void **)(&g_zstd.compressBound) = dlsym(g_zstd_handle, "ZSTD_compressBound");

    if (g_zstd.compress && g_zstd.decompress && g_zstd.getFrameContentSize && g_zstd.isError &&
        g_zstd.compressBound) {
        g_zstd_available = true;
    }
}

/* Libsodium API function pointers (optional acceleration fast-path) */
typedef struct {
    int (*crypto_sign_ed25519_keypair)(uint8_t *pk, uint8_t *sk);
    int (*crypto_sign_ed25519_detached)(uint8_t *sig, unsigned long long *siglen_p,
                                        const uint8_t *m, unsigned long long mlen,
                                        const uint8_t *sk);
    int (*crypto_sign_ed25519_verify_detached)(const uint8_t *sig, const uint8_t *m,
                                               unsigned long long mlen, const uint8_t *pk);
    int (*crypto_hash_sha256)(uint8_t *out, const uint8_t *in, unsigned long long inlen);
} sodium_funcs_t;

static void *g_sodium_handle = nullptr;
static bool g_sodium_probed = false;
static bool g_sodium_available = false;
static sodium_funcs_t g_sodium;

static void sodium_probe(void)
{
    if (g_sodium_probed) {
        return;
    }
    g_sodium_probed = true;

    g_sodium_handle = dlopen("libsodium.so.26", RTLD_LAZY | RTLD_LOCAL);
    if (!g_sodium_handle) {
        g_sodium_handle = dlopen("libsodium.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!g_sodium_handle) {
        return;
    }

    *(void **)(&g_sodium.crypto_sign_ed25519_keypair) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_keypair");
    *(void **)(&g_sodium.crypto_sign_ed25519_detached) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_detached");
    *(void **)(&g_sodium.crypto_sign_ed25519_verify_detached) =
        dlsym(g_sodium_handle, "crypto_sign_ed25519_verify_detached");
    *(void **)(&g_sodium.crypto_hash_sha256) = dlsym(g_sodium_handle, "crypto_hash_sha256");

    if (g_sodium.crypto_sign_ed25519_keypair && g_sodium.crypto_sign_ed25519_detached &&
        g_sodium.crypto_sign_ed25519_verify_detached) {
        g_sodium_available = true;
    }
}

/* Secure Random Bytes Generator */
static int secure_random_bytes(uint8_t *buf, size_t len)
{
    ssize_t ret = getrandom(buf, len, 0);
    if (ret == (ssize_t)len) {
        return 0;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t r = read(fd, buf + total, len - total);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        total += (size_t)r;
    }
    close(fd);
    return 0;
}

/* ========================================================================= */
/* Self-Contained FIPS 180-4 SHA-256 Implementation                          */
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

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
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

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t idx = (size_t)(ctx->count % 64);
    ctx->count += len;
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[idx++] = data[i];
        if (idx == 64) {
            sha256_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    size_t idx = (size_t)(ctx->count % 64);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        sha256_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    uint64_t bit_len = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha256_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = (uint8_t)(ctx->state[i] >> (24 - j * 8));
        }
    }
}

void wwpkg_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sodium_probe();
    if (g_sodium_available && g_sodium.crypto_hash_sha256) {
        g_sodium.crypto_hash_sha256(out, data, (unsigned long long)len);
        return;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
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
/* Self-Contained TweetNaCl RFC 8032 Ed25519 Implementation                  */
/* ========================================================================= */

typedef int64_t gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                     0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf146, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                      0xd130, 0xef73, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                     0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                     0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xa45d, 0x344d, 0xd00f,
                     0x7c69, 0x8820, 0x9cc5, 0x6504, 0x77e0, 0x41e5, 0x16be, 0x2b83};

static const uint8_t L25519[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                   0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

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
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
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
        for (int j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L25519[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[i - 12] += carry;
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
        carry = x[j] >> 8;
        x[j] &= 0xff;
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
}

static int unpackneg(gf r[4], const uint8_t p[32])
{
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
    M(t, t, den);
    M(t, t, num);
    M(r[0], t, den);

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
    for (int i = 0; i < 32; i++) {
        sk[32 + i] = pk[i];
    }
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
    return 0;
}

static int tweetnacl_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen,
                                    const uint8_t pk[32])
{
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

int wwpkg_ed25519_keypair(uint8_t pk[32], uint8_t sk[64])
{
    sodium_probe();
    if (g_sodium_available && g_sodium.crypto_sign_ed25519_keypair) {
        return g_sodium.crypto_sign_ed25519_keypair(pk, sk);
    }
    if (secure_random_bytes(sk, 32) != 0) {
        return -1;
    }
    return tweetnacl_ed25519_keypair(pk, sk);
}

int wwpkg_ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t mlen, const uint8_t sk[64])
{
    sodium_probe();
    if (g_sodium_available && g_sodium.crypto_sign_ed25519_detached) {
        unsigned long long slen = 64;
        return g_sodium.crypto_sign_ed25519_detached(sig, &slen, m, (unsigned long long)mlen, sk);
    }
    return tweetnacl_ed25519_sign(sig, m, mlen, sk);
}

int wwpkg_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen, const uint8_t pk[32])
{
    sodium_probe();
    if (g_sodium_available && g_sodium.crypto_sign_ed25519_verify_detached) {
        return g_sodium.crypto_sign_ed25519_verify_detached(sig, m, (unsigned long long)mlen, pk);
    }
    return tweetnacl_ed25519_verify(sig, m, mlen, pk);
}

void wwpkg_generate_keypair(uint8_t pub[32], uint8_t priv[64]) { wwpkg_ed25519_keypair(pub, priv); }

/* ========================================================================= */
/* RFC 8785 JSON Canonicalization Scheme (JCS) Engine                        */
/* ========================================================================= */

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
    jcs_val_t *val;
} jcs_kv_t;

struct jcs_val {
    jcs_type_t type;
    bool bval;
    double dval;
    int64_t ival;
    bool is_int;
    char *sval;
    struct {
        jcs_val_t **items;
        size_t count;
        size_t cap;
    } arr;
    struct {
        jcs_kv_t *entries;
        size_t count;
        size_t cap;
    } obj;
};

static void jcs_val_free(jcs_val_t *v)
{
    if (!v)
        return;
    if (v->type == JCS_TYPE_STR) {
        free(v->sval);
    } else if (v->type == JCS_TYPE_ARR) {
        for (size_t i = 0; i < v->arr.count; i++) {
            jcs_val_free(v->arr.items[i]);
        }
        free(v->arr.items);
    } else if (v->type == JCS_TYPE_OBJ) {
        for (size_t i = 0; i < v->obj.count; i++) {
            free(v->obj.entries[i].key);
            jcs_val_free(v->obj.entries[i].val);
        }
        free(v->obj.entries);
    }
    free(v);
}

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} jcs_lexer_t;

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

static jcs_val_t *jcs_parse_val(jcs_lexer_t *lx);

static char *jcs_parse_str_raw(jcs_lexer_t *lx)
{
    jcs_skip_ws(lx);
    if (lx->pos >= lx->len || lx->src[lx->pos] != '"') {
        return nullptr;
    }
    lx->pos++;

    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return nullptr;

    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos++];
        if (c == '"') {
            buf[len] = '\0';
            return buf;
        }
        if (c == '\\') {
            if (lx->pos >= lx->len)
                break;
            char esc = lx->src[lx->pos++];
            switch (esc) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u': {
                if (lx->pos + 4 > lx->len) {
                    free(buf);
                    return nullptr;
                }
                char hex[5] = {lx->src[lx->pos], lx->src[lx->pos + 1], lx->src[lx->pos + 2],
                               lx->src[lx->pos + 3], '\0'};
                lx->pos += 4;
                uint32_t cp = (uint32_t)strtoul(hex, nullptr, 16);

                /* Handle UTF-16 surrogate pairs */
                if (cp >= 0xD800 && cp <= 0xDBFF && lx->pos + 6 <= lx->len &&
                    lx->src[lx->pos] == '\\' && lx->src[lx->pos + 1] == 'u') {
                    char hex2[5] = {lx->src[lx->pos + 2], lx->src[lx->pos + 3],
                                    lx->src[lx->pos + 4], lx->src[lx->pos + 5], '\0'};
                    uint32_t cp2 = (uint32_t)strtoul(hex2, nullptr, 16);
                    if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                        lx->pos += 6;
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
                    }
                }

                /* Encode UTF-8 */
                char u8[4];
                size_t u8len = 0;
                if (cp <= 0x7F) {
                    u8[0] = (char)cp;
                    u8len = 1;
                } else if (cp <= 0x7FF) {
                    u8[0] = (char)(0xC0 | (cp >> 6));
                    u8[1] = (char)(0x80 | (cp & 0x3F));
                    u8len = 2;
                } else if (cp <= 0xFFFF) {
                    u8[0] = (char)(0xE0 | (cp >> 12));
                    u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[2] = (char)(0x80 | (cp & 0x3F));
                    u8len = 3;
                } else {
                    u8[0] = (char)(0xF0 | (cp >> 18));
                    u8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    u8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[3] = (char)(0x80 | (cp & 0x3F));
                    u8len = 4;
                }

                if (len + u8len + 1 >= cap) {
                    cap = (cap * 2) + u8len + 16;
                    char *nb = (char *)realloc(buf, cap);
                    if (!nb) {
                        free(buf);
                        return nullptr;
                    }
                    buf = nb;
                }
                memcpy(buf + len, u8, u8len);
                len += u8len;
                continue;
            }
            default:
                break;
            }
        }
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return nullptr;
            }
            buf = nb;
        }
        buf[len++] = c;
    }
    free(buf);
    return nullptr;
}

static jcs_val_t *jcs_parse_val(jcs_lexer_t *lx)
{
    jcs_skip_ws(lx);
    if (lx->pos >= lx->len)
        return nullptr;
    char c = lx->src[lx->pos];

    if (c == '"') {
        char *str = jcs_parse_str_raw(lx);
        if (!str)
            return nullptr;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v) {
            free(str);
            return nullptr;
        }
        v->type = JCS_TYPE_STR;
        v->sval = str;
        return v;
    }
    if (c == '{') {
        lx->pos++;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_OBJ;
        jcs_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
            lx->pos++;
            return v;
        }
        while (lx->pos < lx->len) {
            char *key = jcs_parse_str_raw(lx);
            if (!key) {
                jcs_val_free(v);
                return nullptr;
            }
            jcs_skip_ws(lx);
            if (lx->pos >= lx->len || lx->src[lx->pos] != ':') {
                free(key);
                jcs_val_free(v);
                return nullptr;
            }
            lx->pos++;
            jcs_val_t *val = jcs_parse_val(lx);
            if (!val) {
                free(key);
                jcs_val_free(v);
                return nullptr;
            }
            if (v->obj.count >= v->obj.cap) {
                size_t ncap = v->obj.cap ? v->obj.cap * 2 : 8;
                jcs_kv_t *ne = (jcs_kv_t *)realloc(v->obj.entries, ncap * sizeof(jcs_kv_t));
                if (!ne) {
                    free(key);
                    jcs_val_free(val);
                    jcs_val_free(v);
                    return nullptr;
                }
                v->obj.entries = ne;
                v->obj.cap = ncap;
            }
            v->obj.entries[v->obj.count].key = key;
            v->obj.entries[v->obj.count].val = val;
            v->obj.count++;

            jcs_skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
                lx->pos++;
                continue;
            }
            if (lx->pos < lx->len && lx->src[lx->pos] == '}') {
                lx->pos++;
                break;
            }
            jcs_val_free(v);
            return nullptr;
        }
        return v;
    }
    if (c == '[') {
        lx->pos++;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_ARR;
        jcs_skip_ws(lx);
        if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
            lx->pos++;
            return v;
        }
        while (lx->pos < lx->len) {
            jcs_val_t *item = jcs_parse_val(lx);
            if (!item) {
                jcs_val_free(v);
                return nullptr;
            }
            if (v->arr.count >= v->arr.cap) {
                size_t ncap = v->arr.cap ? v->arr.cap * 2 : 8;
                jcs_val_t **ni = (jcs_val_t **)realloc(v->arr.items, ncap * sizeof(jcs_val_t *));
                if (!ni) {
                    jcs_val_free(item);
                    jcs_val_free(v);
                    return nullptr;
                }
                v->arr.items = ni;
                v->arr.cap = ncap;
            }
            v->arr.items[v->arr.count++] = item;

            jcs_skip_ws(lx);
            if (lx->pos < lx->len && lx->src[lx->pos] == ',') {
                lx->pos++;
                continue;
            }
            if (lx->pos < lx->len && lx->src[lx->pos] == ']') {
                lx->pos++;
                break;
            }
            jcs_val_free(v);
            return nullptr;
        }
        return v;
    }
    if (strncmp(lx->src + lx->pos, "true", 4) == 0) {
        lx->pos += 4;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_BOOL;
        v->bval = true;
        return v;
    }
    if (strncmp(lx->src + lx->pos, "false", 5) == 0) {
        lx->pos += 5;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_BOOL;
        v->bval = false;
        return v;
    }
    if (strncmp(lx->src + lx->pos, "null", 4) == 0) {
        lx->pos += 4;
        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_NULL;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = lx->pos;
        bool is_float = false;
        if (lx->src[lx->pos] == '-')
            lx->pos++;
        while (lx->pos < lx->len && (lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9')) {
            lx->pos++;
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
            is_float = true;
            lx->pos++;
            while (lx->pos < lx->len && (lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9')) {
                lx->pos++;
            }
        }
        if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
            is_float = true;
            lx->pos++;
            if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-')) {
                lx->pos++;
            }
            while (lx->pos < lx->len && (lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9')) {
                lx->pos++;
            }
        }
        size_t nlen = lx->pos - start;
        char numbuf[64];
        if (nlen >= sizeof(numbuf))
            return nullptr;
        memcpy(numbuf, lx->src + start, nlen);
        numbuf[nlen] = '\0';

        jcs_val_t *v = (jcs_val_t *)calloc(1, sizeof(jcs_val_t));
        if (!v)
            return nullptr;
        v->type = JCS_TYPE_NUM;
        if (is_float) {
            v->dval = strtod(numbuf, nullptr);
            v->is_int = false;
        } else {
            v->ival = strtoll(numbuf, nullptr, 10);
            v->dval = (double)v->ival;
            v->is_int = true;
        }
        return v;
    }

    return nullptr;
}

/* RFC 8785 UTF-16 Code Unit Comparison for JSON Object Keys */
static int jcs_key_cmp(const void *a, const void *b)
{
    const jcs_kv_t *ka = (const jcs_kv_t *)a;
    const jcs_kv_t *kb = (const jcs_kv_t *)b;
    const uint8_t *s1 = (const uint8_t *)ka->key;
    const uint8_t *s2 = (const uint8_t *)kb->key;

    while (*s1 && *s2) {
        uint32_t cp1 = 0, cp2 = 0;
        size_t l1 = 0, l2 = 0;

        if (*s1 < 0x80) {
            cp1 = *s1;
            l1 = 1;
        } else if ((*s1 & 0xE0) == 0xC0) {
            cp1 = ((*s1 & 0x1F) << 6) | (s1[1] & 0x3F);
            l1 = 2;
        } else if ((*s1 & 0xF0) == 0xE0) {
            cp1 = ((*s1 & 0x0F) << 12) | ((s1[1] & 0x3F) << 6) | (s1[2] & 0x3F);
            l1 = 3;
        } else {
            cp1 = ((*s1 & 0x07) << 18) | ((s1[1] & 0x3F) << 12) | ((s1[2] & 0x3F) << 6) |
                  (s1[3] & 0x3F);
            l1 = 4;
        }

        if (*s2 < 0x80) {
            cp2 = *s2;
            l2 = 1;
        } else if ((*s2 & 0xE0) == 0xC0) {
            cp2 = ((*s2 & 0x1F) << 6) | (s2[1] & 0x3F);
            l2 = 2;
        } else if ((*s2 & 0xF0) == 0xE0) {
            cp2 = ((*s2 & 0x0F) << 12) | ((s2[1] & 0x3F) << 6) | (s2[2] & 0x3F);
            l2 = 3;
        } else {
            cp2 = ((*s2 & 0x07) << 18) | ((s2[1] & 0x3F) << 12) | ((s2[2] & 0x3F) << 6) |
                  (s2[3] & 0x3F);
            l2 = 4;
        }

        uint16_t cu1[2], cu2[2];
        size_t n1 = 0, n2 = 0;
        if (cp1 <= 0xFFFF) {
            cu1[0] = (uint16_t)cp1;
            n1 = 1;
        } else {
            cu1[0] = (uint16_t)(0xD800 + ((cp1 - 0x10000) >> 10));
            cu1[1] = (uint16_t)(0xDC00 + ((cp1 - 0x10000) & 0x3FF));
            n1 = 2;
        }
        if (cp2 <= 0xFFFF) {
            cu2[0] = (uint16_t)cp2;
            n2 = 1;
        } else {
            cu2[0] = (uint16_t)(0xD800 + ((cp2 - 0x10000) >> 10));
            cu2[1] = (uint16_t)(0xDC00 + ((cp2 - 0x10000) & 0x3FF));
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
    if (*s1)
        return 1;
    if (*s2)
        return -1;
    return 0;
}

/* Dynamic Buffer for JCS Serialization */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} jcs_out_buf_t;

static bool jcs_out_append(jcs_out_buf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 >= b->cap) {
        size_t ncap = (b->cap * 2) + n + 64;
        char *nb = (char *)realloc(b->buf, ncap);
        if (!nb)
            return false;
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return true;
}

static bool jcs_out_append_str(jcs_out_buf_t *b, const char *s)
{
    if (!jcs_out_append(b, "\"", 1))
        return false;
    for (const char *p = s; *p; p++) {
        uint8_t c = (uint8_t)*p;
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
        } else if (c < 0x20) {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            if (!jcs_out_append(b, esc, 6))
                return false;
        } else {
            if (!jcs_out_append(b, (const char *)p, 1))
                return false;
        }
    }
    return jcs_out_append(b, "\"", 1);
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
        return jcs_out_append_str(b, v->sval);
    case JCS_TYPE_NUM: {
        char nbuf[64];
        if (v->is_int) {
            snprintf(nbuf, sizeof(nbuf), "%lld", (long long)v->ival);
        } else {
            if (v->dval == 0.0) {
                snprintf(nbuf, sizeof(nbuf), "0");
            } else {
                snprintf(nbuf, sizeof(nbuf), "%.17g", v->dval);
            }
        }
        return jcs_out_append(b, nbuf, strlen(nbuf));
    }
    case JCS_TYPE_ARR: {
        if (!jcs_out_append(b, "[", 1))
            return false;
        for (size_t i = 0; i < v->arr.count; i++) {
            if (i > 0) {
                if (!jcs_out_append(b, ",", 1))
                    return false;
            }
            if (!jcs_serialize_val(b, v->arr.items[i]))
                return false;
        }
        return jcs_out_append(b, "]", 1);
    }
    case JCS_TYPE_OBJ: {
        /* RFC 8785 requires object keys to be sorted lexicographically */
        jcs_val_t *mutable_v = (jcs_val_t *)v;
        if (mutable_v->obj.count > 1) {
            qsort(mutable_v->obj.entries, mutable_v->obj.count, sizeof(jcs_kv_t), jcs_key_cmp);
        }
        if (!jcs_out_append(b, "{", 1))
            return false;
        for (size_t i = 0; i < mutable_v->obj.count; i++) {
            if (i > 0) {
                if (!jcs_out_append(b, ",", 1))
                    return false;
            }
            if (!jcs_out_append_str(b, mutable_v->obj.entries[i].key))
                return false;
            if (!jcs_out_append(b, ":", 1))
                return false;
            if (!jcs_serialize_val(b, mutable_v->obj.entries[i].val))
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
    if (!json_src)
        return nullptr;
    jcs_lexer_t lx = {.src = json_src, .len = len ? len : strlen(json_src), .pos = 0};
    jcs_val_t *root = jcs_parse_val(&lx);
    if (!root)
        return nullptr;

    jcs_out_buf_t b = {.buf = (char *)malloc(128), .len = 0, .cap = 128};
    if (!b.buf) {
        jcs_val_free(root);
        return nullptr;
    }
    b.buf[0] = '\0';

    if (!jcs_serialize_val(&b, root)) {
        free(b.buf);
        jcs_val_free(root);
        return nullptr;
    }
    jcs_val_free(root);
    if (out_len)
        *out_len = b.len;
    return b.buf;
}

/* ========================================================================= */
/* Manifest Parser and Validator (`manifest.json`)                          */
/* ========================================================================= */

static const jcs_val_t *jcs_find_member(const jcs_val_t *obj, const char *key)
{
    if (!obj || obj->type != JCS_TYPE_OBJ)
        return nullptr;
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (strcmp(obj->obj.entries[i].key, key) == 0) {
            return obj->obj.entries[i].val;
        }
    }
    return nullptr;
}

static bool jcs_get_str_field(const jcs_val_t *obj, const char *key, char *dst, size_t max_len,
                              bool required)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v || v->type != JCS_TYPE_STR) {
        return !required;
    }
    strncpy(dst, v->sval, max_len - 1);
    dst[max_len - 1] = '\0';
    return true;
}

static bool jcs_get_bool_field(const jcs_val_t *obj, const char *key, bool *dst, bool required,
                               bool def)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v || v->type != JCS_TYPE_BOOL) {
        *dst = def;
        return !required;
    }
    *dst = v->bval;
    return true;
}

static bool jcs_get_uint_field(const jcs_val_t *obj, const char *key, uint32_t *dst, bool required,
                               uint32_t def)
{
    const jcs_val_t *v = jcs_find_member(obj, key);
    if (!v || v->type != JCS_TYPE_NUM) {
        *dst = def;
        return !required;
    }
    *dst = (uint32_t)(v->is_int ? v->ival : (int64_t)v->dval);
    return true;
}

static bool parse_manifest_ast(const jcs_val_t *root, wwpkg_manifest_t *m)
{
    if (!root || root->type != JCS_TYPE_OBJ)
        return false;
    memset(m, 0, sizeof(*m));

    /* schema_version */
    if (!jcs_get_str_field(root, "schema_version", m->schema_version, sizeof(m->schema_version),
                           true))
        return false;
    if (strcmp(m->schema_version, "wwpkg/v1") != 0)
        return false;

    /* id */
    if (!jcs_get_str_field(root, "id", m->id, sizeof(m->id), true))
        return false;
    if (m->id[0] == '\0')
        return false;

    /* slug */
    if (!jcs_get_str_field(root, "slug", m->slug, sizeof(m->slug), true))
        return false;
    if (m->slug[0] == '\0')
        return false;
    /* validate slug pattern ^[a-z0-9]+(-[a-z0-9]+)*$ */
    for (size_t i = 0; m->slug[i]; i++) {
        char c = m->slug[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
        if (c == '-' && (i == 0 || m->slug[i + 1] == '\0' || m->slug[i + 1] == '-'))
            return false;
    }

    /* name */
    if (!jcs_get_str_field(root, "name", m->name, sizeof(m->name), true))
        return false;
    if (m->name[0] == '\0')
        return false;

    /* version */
    if (!jcs_get_str_field(root, "version", m->version, sizeof(m->version), true))
        return false;
    if (m->version[0] == '\0')
        return false;

    /* author object */
    const jcs_val_t *v_auth = jcs_find_member(root, "author");
    if (!v_auth || v_auth->type != JCS_TYPE_OBJ)
        return false;
    if (!jcs_get_str_field(v_auth, "name", m->author_name, sizeof(m->author_name), true))
        return false;
    jcs_get_str_field(v_auth, "email", m->author_email, sizeof(m->author_email), false);

    char pubkey_hex[128] = {0};
    if (!jcs_get_str_field(v_auth, "pubkey", pubkey_hex, sizeof(pubkey_hex), true))
        return false;
    if (strlen(pubkey_hex) != 64)
        return false;
    for (int i = 0; i < 32; i++) {
        unsigned int byte_val = 0;
        if (sscanf(pubkey_hex + i * 2, "%02x", &byte_val) != 1)
            return false;
        m->author_pubkey[i] = (uint8_t)byte_val;
    }

    /* license */
    if (!jcs_get_str_field(root, "license", m->license, sizeof(m->license), true))
        return false;

    /* description (optional) */
    jcs_get_str_field(root, "description", m->description, sizeof(m->description), false);

    /* tags (optional array) */
    const jcs_val_t *v_tags = jcs_find_member(root, "tags");
    if (v_tags && v_tags->type == JCS_TYPE_ARR) {
        size_t count = v_tags->arr.count < WWPKG_MAX_TAGS ? v_tags->arr.count : WWPKG_MAX_TAGS;
        m->tag_count = (uint32_t)count;
        for (size_t i = 0; i < count; i++) {
            if (v_tags->arr.items[i]->type == JCS_TYPE_STR) {
                strncpy(m->tags[i], v_tags->arr.items[i]->sval, sizeof(m->tags[i]) - 1);
                m->tags[i][sizeof(m->tags[i]) - 1] = '\0';
            }
        }
    }

    /* type */
    char type_str[64] = {0};
    if (!jcs_get_str_field(root, "type", type_str, sizeof(type_str), true))
        return false;
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
    else
        m->type = WWPKG_TYPE_UNKNOWN;

    /* entrypoint object */
    const jcs_val_t *v_ep = jcs_find_member(root, "entrypoint");
    if (!v_ep || v_ep->type != JCS_TYPE_OBJ)
        return false;
    if (!jcs_get_str_field(v_ep, "pipeline", m->entrypoint_pipeline, sizeof(m->entrypoint_pipeline),
                           true))
        return false;
    if (!jcs_get_str_field(v_ep, "preview", m->entrypoint_preview, sizeof(m->entrypoint_preview),
                           true))
        return false;
    jcs_get_str_field(v_ep, "audio_track", m->entrypoint_audio, sizeof(m->entrypoint_audio), false);

    /* engine_requirements object */
    const jcs_val_t *v_eng = jcs_find_member(root, "engine_requirements");
    if (!v_eng || v_eng->type != JCS_TYPE_OBJ)
        return false;
    if (!jcs_get_str_field(v_eng, "min_wwal_version", m->min_wwal_version,
                           sizeof(m->min_wwal_version), true))
        return false;
    if (!jcs_get_uint_field(v_eng, "abi_version", &m->abi_version, true, 1))
        return false;
    if (!jcs_get_str_field(v_eng, "gl_profile", m->gl_profile, sizeof(m->gl_profile), true))
        return false;

    /* capabilities object */
    const jcs_val_t *v_cap = jcs_find_member(root, "capabilities");
    if (!v_cap || v_cap->type != JCS_TYPE_OBJ)
        return false;
    jcs_get_bool_field(v_cap, "compute_shaders", &m->capabilities.compute_shaders, true, false);
    jcs_get_bool_field(v_cap, "audio_spectrum", &m->capabilities.audio_spectrum, true, false);
    jcs_get_bool_field(v_cap, "audio_playback", &m->capabilities.audio_playback, true, false);
    jcs_get_bool_field(v_cap, "scripting", &m->capabilities.scripting, true, false);
    jcs_get_bool_field(v_cap, "foreign_toplevel", &m->capabilities.foreign_toplevel, true, false);
    jcs_get_bool_field(v_cap, "network", &m->capabilities.network, true, false);
    jcs_get_bool_field(v_cap, "filesystem", &m->capabilities.filesystem, true, false);

    /* CRITICAL AIR-GAP SECURITY CONSTRAINT: network and filesystem MUST BE FALSE */
    if (m->capabilities.network || m->capabilities.filesystem) {
        return false;
    }

    /* resource_budgets object (optional) */
    const jcs_val_t *v_bud = jcs_find_member(root, "resource_budgets");
    if (v_bud && v_bud->type == JCS_TYPE_OBJ) {
        jcs_get_uint_field(v_bud, "max_vram_mb", &m->budgets.max_vram_mb, false, 256);
        jcs_get_uint_field(v_bud, "max_ram_mb", &m->budgets.max_ram_mb, false, 64);
        jcs_get_uint_field(v_bud, "target_fps", &m->budgets.target_fps, false, 60);
    } else {
        m->budgets.max_vram_mb = 256;
        m->budgets.max_ram_mb = 64;
        m->budgets.target_fps = 60;
    }

    return true;
}

static bool parse_manifest_from_string(const char *json_str, size_t len, wwpkg_manifest_t *m)
{
    jcs_lexer_t lx = {.src = json_str, .len = len ? len : strlen(json_str), .pos = 0};
    jcs_val_t *root = jcs_parse_val(&lx);
    if (!root)
        return false;
    bool ok = parse_manifest_ast(root, m);
    jcs_val_free(root);
    return ok;
}

/* Serializes a wwpkg_manifest_t structure directly to canonical RFC 8785 JSON */
static char *manifest_to_canonical_json(const wwpkg_manifest_t *m, size_t *out_len)
{
    char pubkey_hex[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(pubkey_hex + i * 2, 3, "%02x", m->author_pubkey[i]);
    }

    const char *type_str = "shader_fragment";
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
        break;
    }

    jcs_out_buf_t raw = {.buf = (char *)malloc(2048), .len = 0, .cap = 2048};
    if (!raw.buf)
        return nullptr;
    raw.buf[0] = '\0';

    char tmp[2048];
    jcs_out_append(&raw, "{\"schema_version\":\"wwpkg/v1\",",
                   strlen("{\"schema_version\":\"wwpkg/v1\","));

    snprintf(tmp, sizeof(tmp), "\"id\":\"%s\",", m->id);
    jcs_out_append(&raw, tmp, strlen(tmp));

    snprintf(tmp, sizeof(tmp), "\"slug\":\"%s\",", m->slug);
    jcs_out_append(&raw, tmp, strlen(tmp));

    snprintf(tmp, sizeof(tmp), "\"name\":\"%s\",", m->name);
    jcs_out_append(&raw, tmp, strlen(tmp));

    snprintf(tmp, sizeof(tmp), "\"version\":\"%s\",", m->version);
    jcs_out_append(&raw, tmp, strlen(tmp));

    jcs_out_append(&raw, "\"author\":{", strlen("\"author\":{"));
    snprintf(tmp, sizeof(tmp), "\"name\":\"%s\",", m->author_name);
    jcs_out_append(&raw, tmp, strlen(tmp));
    if (m->author_email[0]) {
        snprintf(tmp, sizeof(tmp), "\"email\":\"%s\",", m->author_email);
        jcs_out_append(&raw, tmp, strlen(tmp));
    }
    snprintf(tmp, sizeof(tmp), "\"pubkey\":\"%s\"},", pubkey_hex);
    jcs_out_append(&raw, tmp, strlen(tmp));

    snprintf(tmp, sizeof(tmp), "\"license\":\"%s\",", m->license);
    jcs_out_append(&raw, tmp, strlen(tmp));

    if (m->description[0]) {
        jcs_out_append(&raw, "\"description\":", strlen("\"description\":"));
        jcs_out_append_str(&raw, m->description);
        jcs_out_append(&raw, ",", 1);
    }

    if (m->tag_count > 0) {
        jcs_out_append(&raw, "\"tags\":[", strlen("\"tags\":["));
        for (uint32_t i = 0; i < m->tag_count; i++) {
            if (i > 0)
                jcs_out_append(&raw, ",", 1);
            jcs_out_append_str(&raw, m->tags[i]);
        }
        jcs_out_append(&raw, "],", 2);
    }

    snprintf(tmp, sizeof(tmp), "\"type\":\"%s\",", type_str);
    jcs_out_append(&raw, tmp, strlen(tmp));

    jcs_out_append(&raw, "\"entrypoint\":{", strlen("\"entrypoint\":{"));
    jcs_out_append(&raw, "\"pipeline\":", strlen("\"pipeline\":"));
    jcs_out_append_str(&raw, m->entrypoint_pipeline);
    jcs_out_append(&raw, ",\"preview\":", strlen(",\"preview\":"));
    jcs_out_append_str(&raw, m->entrypoint_preview);
    if (m->entrypoint_audio[0]) {
        jcs_out_append(&raw, ",\"audio_track\":", strlen(",\"audio_track\":"));
        jcs_out_append_str(&raw, m->entrypoint_audio);
    }
    jcs_out_append(&raw, "},", 2);

    jcs_out_append(&raw, "\"engine_requirements\":{", strlen("\"engine_requirements\":{"));
    snprintf(tmp, sizeof(tmp),
             "\"min_wwal_version\":\"%s\",\"abi_version\":%u,\"gl_profile\":\"%s\"},",
             m->min_wwal_version[0] ? m->min_wwal_version : "1.2.0",
             m->abi_version ? m->abi_version : 1, m->gl_profile[0] ? m->gl_profile : "gles_31");
    jcs_out_append(&raw, tmp, strlen(tmp));

    jcs_out_append(&raw, "\"capabilities\":{", strlen("\"capabilities\":{"));
    snprintf(tmp, sizeof(tmp),
             "\"compute_shaders\":%s,\"audio_spectrum\":%s,\"audio_playback\":%s,\"scripting\":%s,"
             "\"foreign_toplevel\":%s,\"network\":false,\"filesystem\":false},",
             m->capabilities.compute_shaders ? "true" : "false",
             m->capabilities.audio_spectrum ? "true" : "false",
             m->capabilities.audio_playback ? "true" : "false",
             m->capabilities.scripting ? "true" : "false",
             m->capabilities.foreign_toplevel ? "true" : "false");
    jcs_out_append(&raw, tmp, strlen(tmp));

    jcs_out_append(&raw, "\"resource_budgets\":{", strlen("\"resource_budgets\":{"));
    snprintf(tmp, sizeof(tmp), "\"max_vram_mb\":%u,\"max_ram_mb\":%u,\"target_fps\":%u}}",
             m->budgets.max_vram_mb ? m->budgets.max_vram_mb : 256,
             m->budgets.max_ram_mb ? m->budgets.max_ram_mb : 64,
             m->budgets.target_fps ? m->budgets.target_fps : 60);
    jcs_out_append(&raw, tmp, strlen(tmp));

    char *canonical = wwpkg_canonicalize_json(raw.buf, raw.len, out_len);
    free(raw.buf);
    return canonical;
}

/* ========================================================================= */
/* RFC 6962 Domain-Separated SHA-256 Merkle Tree                             */
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

static void compute_merkle_root(merkle_file_t *files, size_t file_count, uint8_t out_root[32])
{
    if (file_count == 0) {
        memset(out_root, 0, 32);
        return;
    }

    /* Sort files lexicographically by normalized relative path */
    if (file_count > 1) {
        qsort(files, file_count, sizeof(merkle_file_t), merkle_file_cmp);
    }

    /* Compute leaf hashes with RFC 6962 domain byte 0x00 */
    uint8_t (*nodes)[32] = (uint8_t (*)[32])malloc(file_count * 32);
    if (!nodes) {
        memset(out_root, 0, 32);
        return;
    }

    for (size_t i = 0; i < file_count; i++) {
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        uint8_t domain = 0x00;
        sha256_update(&ctx, &domain, 1);
        if (files[i].size > 0 && files[i].data != nullptr) {
            sha256_update(&ctx, files[i].data, files[i].size);
        }
        sha256_final(&ctx, nodes[i]);
    }

    /* Compute internal parent nodes with RFC 6962 domain byte 0x01 */
    size_t current_count = file_count;
    while (current_count > 1) {
        size_t next_count = (current_count + 1) / 2;
        for (size_t i = 0; i < current_count; i += 2) {
            size_t parent_idx = i / 2;
            if (i + 1 < current_count) {
                sha256_ctx_t ctx;
                sha256_init(&ctx);
                uint8_t domain = 0x01;
                sha256_update(&ctx, &domain, 1);
                sha256_update(&ctx, nodes[i], 32);
                sha256_update(&ctx, nodes[i + 1], 32);
                sha256_final(&ctx, nodes[parent_idx]);
            } else {
                /* Odd node promoted to next level */
                memcpy(nodes[parent_idx], nodes[i], 32);
            }
        }
        current_count = next_count;
    }

    memcpy(out_root, nodes[0], 32);
    free(nodes);
}

/* ========================================================================= */
/* POSIX ustar Archive Reader & Writer with Strict Path Sanitization         */
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

/* Sanitizes archive relative paths, strictly rejecting directory traversal or leading slash */
static bool sanitize_archive_path(const char *in, char *out, size_t out_cap)
{
    if (!in || !out || out_cap == 0)
        return false;

    /* Strictly reject absolute paths (leading '/') per ORIGINAL_REQUEST.md */
    if (in[0] == '/')
        return false;
    if (*in == '\0')
        return false;

    size_t out_len = 0;
    const char *p = in;
    while (*p) {
        /* Component boundary */
        const char *comp_start = p;
        while (*p && *p != '/') {
            if ((uint8_t)*p < 0x20 || *p == 0x7F)
                return false;
            p++;
        }
        size_t comp_len = (size_t)(p - comp_start);
        if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            /* Path traversal attempt */
            return false;
        }
        /* Skip current-directory '.' components */
        if (comp_len == 1 && comp_start[0] == '.') {
            if (*p == '/')
                p++;
            continue;
        }
        if (comp_len > 0) {
            if (out_len > 0) {
                if (out_len + 1 >= out_cap)
                    return false;
                out[out_len++] = '/';
            }
            if (out_len + comp_len >= out_cap)
                return false;
            memcpy(out + out_len, comp_start, comp_len);
            out_len += comp_len;
        }
        if (*p == '/')
            p++;
    }
    out[out_len] = '\0';
    return out_len > 0;
}

/* In-memory package representation */
typedef struct {
    char name[WWPKG_MAX_PATH_LEN];
    const uint8_t *data;
    size_t size;
} pkg_asset_entry_t;

struct wwpkg_package {
    wwpkg_manifest_t manifest;
    wwpkg_signature_t signature;
    bool has_manifest;
    bool has_signature;
    int last_error;

    uint8_t *decompressed_buf;
    size_t decompressed_size;

    pkg_asset_entry_t *assets;
    size_t asset_count;
    size_t asset_cap;

    char *raw_manifest_str;
    size_t raw_manifest_len;
};

int wwpkg_get_last_error(const wwpkg_package_t *pkg)
{
    return pkg ? pkg->last_error : WWPKG_ERR_INVALID_PARAM;
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
    if (!pkg)
        return;
    free(pkg->decompressed_buf);
    free(pkg->assets);
    free(pkg->raw_manifest_str);
    free(pkg);
}

wwpkg_package_t *wwpkg_open(const char *pkg_path)
{
    if (!pkg_path)
        return nullptr;

    int fd = open(pkg_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return nullptr;
    }
    size_t file_size = (size_t)st.st_size;

    uint8_t *raw_data = (uint8_t *)malloc(file_size);
    if (!raw_data) {
        close(fd);
        return nullptr;
    }

    size_t read_bytes = 0;
    while (read_bytes < file_size) {
        ssize_t r = read(fd, raw_data + read_bytes, file_size - read_bytes);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            free(raw_data);
            close(fd);
            return nullptr;
        }
        read_bytes += (size_t)r;
    }
    close(fd);

    uint8_t *tar_buf = nullptr;
    size_t tar_size = 0;

    /* Check if compressed with Zstandard (0xFD2FB528 little-endian: 0x28 0xB5 0x2F 0xFD) */
    if (file_size >= 4 && raw_data[0] == 0x28 && raw_data[1] == 0xB5 && raw_data[2] == 0x2F &&
        raw_data[3] == 0xFD) {
        zstd_probe();
        if (!g_zstd_available) {
            free(raw_data);
            return nullptr;
        }

        unsigned long long orig_sz = g_zstd.getFrameContentSize(raw_data, file_size);
        if (orig_sz == 0xFFFFFFFFFFFFFFFFULL || orig_sz == 0xFFFFFFFFFFFFFFFEULL) {
            /* Unknown size, allocate growing buffer */
            size_t cap = file_size * 4 + 65536;
            tar_buf = (uint8_t *)malloc(cap);
            if (!tar_buf) {
                free(raw_data);
                return nullptr;
            }
            size_t d_res = g_zstd.decompress(tar_buf, cap, raw_data, file_size);
            if (g_zstd.isError(d_res)) {
                free(tar_buf);
                free(raw_data);
                return nullptr;
            }
            tar_size = d_res;
        } else {
            tar_buf = (uint8_t *)malloc(orig_sz ? orig_sz : 1);
            if (!tar_buf) {
                free(raw_data);
                return nullptr;
            }
            size_t d_res = g_zstd.decompress(tar_buf, orig_sz, raw_data, file_size);
            if (g_zstd.isError(d_res)) {
                free(tar_buf);
                free(raw_data);
                return nullptr;
            }
            tar_size = d_res;
        }
        free(raw_data);
    } else if (file_size >= 512 && memcmp(raw_data + 257, "ustar", 5) == 0) {
        /* Raw uncompressed ustar tarball fallback */
        tar_buf = raw_data;
        tar_size = file_size;
    } else {
        free(raw_data);
        return nullptr;
    }

    wwpkg_package_t *pkg = (wwpkg_package_t *)calloc(1, sizeof(wwpkg_package_t));
    if (!pkg) {
        free(tar_buf);
        return nullptr;
    }
    pkg->decompressed_buf = tar_buf;
    pkg->decompressed_size = tar_size;

    /* Parse ustar archive blocks */
    size_t cur = 0;
    while (cur + 512 <= tar_size) {
        const ustar_hdr_t *hdr = (const ustar_hdr_t *)(tar_buf + cur);

        /* Check for archive termination (two consecutive 512-byte zero blocks) */
        bool is_zero = true;
        for (size_t i = 0; i < 512; i++) {
            if (((const uint8_t *)hdr)[i] != 0) {
                is_zero = false;
                break;
            }
        }
        if (is_zero) {
            break;
        }

        /* Verify magic */
        if (memcmp(hdr->magic, "ustar", 5) != 0) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            wwpkg_close(pkg);
            return nullptr;
        }

        /* Verify checksum */
        uint32_t calc_sum = ustar_calc_chksum(hdr);
        uint32_t stored_sum = (uint32_t)strtoul(hdr->chksum, nullptr, 8);
        if (calc_sum != stored_sum) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            wwpkg_close(pkg);
            return nullptr;
        }

        /* Size */
        size_t entry_size = (size_t)strtoull(hdr->size, nullptr, 8);
        if (cur + 512 + entry_size > tar_size) {
            pkg->last_error = WWPKG_ERR_CORRUPT;
            wwpkg_close(pkg);
            return nullptr;
        }

        /* Reconstruct path */
        char full_path[WWPKG_MAX_PATH_LEN] = {0};
        if (hdr->prefix[0] != '\0') {
            snprintf(full_path, sizeof(full_path), "%.155s/%.100s", hdr->prefix, hdr->name);
        } else {
            snprintf(full_path, sizeof(full_path), "%.100s", hdr->name);
        }

        char clean_path[WWPKG_MAX_PATH_LEN] = {0};
        if (!sanitize_archive_path(full_path, clean_path, sizeof(clean_path))) {
            pkg->last_error = WWPKG_ERR_PATH_TRAVERSAL;
            wwpkg_close(pkg);
            return nullptr;
        }

        /* Enforce file type: regular files only (no symlinks or device nodes) */
        if (hdr->typeflag != '0' && hdr->typeflag != '\0' && hdr->typeflag != '5') {
            pkg->last_error = WWPKG_ERR_PATH_TRAVERSAL;
            wwpkg_close(pkg);
            return nullptr;
        }

        const uint8_t *entry_data = tar_buf + cur + 512;

        /* Check for signature.sig */
        if (strcmp(clean_path, "signature.sig") == 0) {
            if (entry_size != sizeof(wwpkg_signature_t)) {
                pkg->last_error = WWPKG_ERR_CORRUPT;
                wwpkg_close(pkg);
                return nullptr;
            }
            memcpy(&pkg->signature, entry_data, sizeof(wwpkg_signature_t));
            pkg->has_signature = true;
        } else if (strcmp(clean_path, "manifest.json") == 0) {
            pkg->raw_manifest_str = (char *)malloc(entry_size + 1);
            if (!pkg->raw_manifest_str) {
                wwpkg_close(pkg);
                return nullptr;
            }
            memcpy(pkg->raw_manifest_str, entry_data, entry_size);
            pkg->raw_manifest_str[entry_size] = '\0';
            pkg->raw_manifest_len = entry_size;

            if (!parse_manifest_from_string(pkg->raw_manifest_str, entry_size, &pkg->manifest)) {
                pkg->last_error = WWPKG_ERR_BAD_MANIFEST;
                wwpkg_close(pkg);
                return nullptr;
            }
            pkg->has_manifest = true;
        }

        /* Add to assets list */
        if (hdr->typeflag != '5') {
            if (pkg->asset_count >= pkg->asset_cap) {
                size_t ncap = pkg->asset_cap ? pkg->asset_cap * 2 : 16;
                pkg_asset_entry_t *na =
                    (pkg_asset_entry_t *)realloc(pkg->assets, ncap * sizeof(pkg_asset_entry_t));
                if (!na) {
                    wwpkg_close(pkg);
                    return nullptr;
                }
                pkg->assets = na;
                pkg->asset_cap = ncap;
            }
            strncpy(pkg->assets[pkg->asset_count].name, clean_path,
                    sizeof(pkg->assets[pkg->asset_count].name) - 1);
            pkg->assets[pkg->asset_count].name[sizeof(pkg->assets[pkg->asset_count].name) - 1] =
                '\0';
            pkg->assets[pkg->asset_count].data = entry_data;
            pkg->assets[pkg->asset_count].size = entry_size;
            pkg->asset_count++;
        }

        size_t pad_size = ((entry_size + 511) / 512) * 512;
        cur += 512 + pad_size;
    }

    if (!pkg->has_manifest || !pkg->has_signature) {
        pkg->last_error = WWPKG_ERR_CORRUPT;
        wwpkg_close(pkg);
        return nullptr;
    }

    return pkg;
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
    if (out_size)
        *out_size = 0;
    return nullptr;
}

bool wwpkg_verify_signature(const wwpkg_package_t *pkg, bool *out_is_trusted)
{
    if (!pkg || !pkg->has_manifest || !pkg->has_signature) {
        return false;
    }

    /* 1. Validate manifest schema & air-gap invariants */
    if (strcmp(pkg->manifest.schema_version, "wwpkg/v1") != 0) {
        return false;
    }
    if (pkg->manifest.capabilities.network || pkg->manifest.capabilities.filesystem) {
        return false;
    }

    /* Check author pubkey consistency between manifest and signature */
    if (memcmp(pkg->manifest.author_pubkey, pkg->signature.author_pubkey, WWPKG_PUBKEY_BYTES) !=
        0) {
        return false;
    }

    /* 2. Build RFC 6962 Domain-Separated Merkle Tree across all files excluding signature.sig */
    size_t merkle_count = 0;
    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "signature.sig") != 0) {
            merkle_count++;
        }
    }

    merkle_file_t *mfiles = (merkle_file_t *)malloc(merkle_count * sizeof(merkle_file_t));
    if (!mfiles)
        return false;

    size_t midx = 0;
    for (size_t i = 0; i < pkg->asset_count; i++) {
        if (strcmp(pkg->assets[i].name, "signature.sig") != 0) {
            strncpy(mfiles[midx].name, pkg->assets[i].name, sizeof(mfiles[midx].name) - 1);
            mfiles[midx].name[sizeof(mfiles[midx].name) - 1] = '\0';
            mfiles[midx].data = pkg->assets[i].data;
            mfiles[midx].size = pkg->assets[i].size;
            midx++;
        }
    }

    uint8_t computed_root[32];
    compute_merkle_root(mfiles, merkle_count, computed_root);
    free(mfiles);

    /* Verify computed Merkle root equals signature.sig merkle_root */
    if (memcmp(computed_root, pkg->signature.merkle_root, 32) != 0) {
        return false;
    }

    /* 3. Compute Canonical Manifest Digest: H_manifest = SHA-256(JCS(manifest.json)) */
    size_t canonical_manifest_len = 0;
    char *canonical_manifest = wwpkg_canonicalize_json(pkg->raw_manifest_str, pkg->raw_manifest_len,
                                                       &canonical_manifest_len);
    if (!canonical_manifest) {
        return false;
    }
    uint8_t h_manifest[32];
    wwpkg_sha256((const uint8_t *)canonical_manifest, canonical_manifest_len, h_manifest);
    free(canonical_manifest);

    /* 4. Construct SignPayload = "WWPKG-SIG-V1\n" || H_manifest || RootHash || Timestamp_u64_be */
    uint8_t sign_payload[85];
    const char magic_prefix[] = "WWPKG-SIG-V1\n"; /* exactly 13 bytes */
    memcpy(sign_payload, magic_prefix, 13);
    memcpy(sign_payload + 13, h_manifest, 32);
    memcpy(sign_payload + 45, pkg->signature.merkle_root, 32);

    uint64_t ts = pkg->signature.timestamp;
    sign_payload[77] = (uint8_t)(ts >> 56);
    sign_payload[78] = (uint8_t)(ts >> 48);
    sign_payload[79] = (uint8_t)(ts >> 40);
    sign_payload[80] = (uint8_t)(ts >> 32);
    sign_payload[81] = (uint8_t)(ts >> 24);
    sign_payload[82] = (uint8_t)(ts >> 16);
    sign_payload[83] = (uint8_t)(ts >> 8);
    sign_payload[84] = (uint8_t)(ts & 0xFF);

    /* 5. Cryptographic Ed25519 verification */
    if (wwpkg_ed25519_verify(pkg->signature.signature, sign_payload, sizeof(sign_payload),
                             pkg->signature.author_pubkey) != 0) {
        return false;
    }

    /* 6. Check if author is present in ~/.config/waywal/trusted_keys.json */
    if (out_is_trusted) {
        *out_is_trusted = false;
        char hex_pub[65];
        for (int i = 0; i < 32; i++) {
            snprintf(hex_pub + i * 2, 3, "%02x", pkg->signature.author_pubkey[i]);
        }

        const char *home = getenv("HOME");
        char key_path[PATH_MAX];
        if (home) {
            snprintf(key_path, sizeof(key_path), "%s/.config/waywal/trusted_keys.json", home);
            int kfd = open(key_path, O_RDONLY | O_CLOEXEC);
            if (kfd >= 0) {
                struct stat kst;
                if (fstat(kfd, &kst) == 0 && kst.st_size > 0 && kst.st_size < 1048576) {
                    char *kbuf = (char *)malloc(kst.st_size + 1);
                    if (kbuf) {
                        ssize_t kr = read(kfd, kbuf, kst.st_size);
                        if (kr > 0) {
                            kbuf[kr] = '\0';
                            if (strstr(kbuf, hex_pub) != nullptr) {
                                *out_is_trusted = true;
                            }
                        }
                        free(kbuf);
                    }
                }
                close(kfd);
            }
        }
    }

    return true;
}

/* ========================================================================= */
/* Package Compiler & Builder (`wwpkg_build_package`)                        */
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
} build_file_list_t;

static void build_list_add(build_file_list_t *list, const char *name, uint8_t *data, size_t size)
{
    if (list->count >= list->cap) {
        size_t ncap = list->cap ? list->cap * 2 : 16;
        list->entries =
            (build_file_entry_t *)realloc(list->entries, ncap * sizeof(build_file_entry_t));
        list->cap = ncap;
    }
    strncpy(list->entries[list->count].name, name, sizeof(list->entries[list->count].name) - 1);
    list->entries[list->count].name[sizeof(list->entries[list->count].name) - 1] = '\0';
    list->entries[list->count].data = data;
    list->entries[list->count].size = size;
    list->count++;
}

static bool collect_source_files(const char *root_dir, const char *rel_dir, build_file_list_t *list)
{
    char dir_path[PATH_MAX];
    if (rel_dir && rel_dir[0]) {
        snprintf(dir_path, sizeof(dir_path), "%s/%s", root_dir, rel_dir);
    } else {
        snprintf(dir_path, sizeof(dir_path), "%s", root_dir);
    }

    DIR *d = opendir(dir_path);
    if (!d)
        return false;

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char sub_rel[WWPKG_MAX_PATH_LEN];
        if (rel_dir && rel_dir[0]) {
            snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_dir, ent->d_name);
        } else {
            snprintf(sub_rel, sizeof(sub_rel), "%s", ent->d_name);
        }

        char full_entry[PATH_MAX];
        snprintf(full_entry, sizeof(full_entry), "%s/%s", root_dir, sub_rel);

        struct stat st;
        if (stat(full_entry, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (!collect_source_files(root_dir, sub_rel, list)) {
                closedir(d);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Skip existing signature.sig */
            if (strcmp(sub_rel, "signature.sig") == 0)
                continue;

            int fd = open(full_entry, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;

            size_t sz = (size_t)st.st_size;
            uint8_t *fbuf = (uint8_t *)malloc(sz ? sz : 1);
            if (!fbuf) {
                close(fd);
                closedir(d);
                return false;
            }
            size_t rd = 0;
            while (rd < sz) {
                ssize_t r = read(fd, fbuf + rd, sz - rd);
                if (r <= 0) {
                    if (r < 0 && errno == EINTR)
                        continue;
                    break;
                }
                rd += (size_t)r;
            }
            close(fd);

            char clean[WWPKG_MAX_PATH_LEN];
            if (sanitize_archive_path(sub_rel, clean, sizeof(clean))) {
                build_list_add(list, clean, fbuf, rd);
            } else {
                free(fbuf);
            }
        }
    }
    closedir(d);
    return true;
}

bool wwpkg_build_package(const char *source_dir, const wwpkg_manifest_t *manifest,
                         const uint8_t priv_key[64], const char *out_pkg_path)
{
    if (!source_dir || !priv_key || !out_pkg_path) {
        return false;
    }

    wwpkg_manifest_t active_manifest;
    memset(&active_manifest, 0, sizeof(active_manifest));

    if (manifest) {
        active_manifest = *manifest;
    } else {
        /* Read manifest.json from source directory */
        char mf_path[PATH_MAX];
        snprintf(mf_path, sizeof(mf_path), "%s/manifest.json", source_dir);
        int mfd = open(mf_path, O_RDONLY | O_CLOEXEC);
        if (mfd < 0)
            return false;
        struct stat mst;
        if (fstat(mfd, &mst) != 0 || mst.st_size <= 0) {
            close(mfd);
            return false;
        }
        char *mbuf = (char *)malloc(mst.st_size + 1);
        if (!mbuf) {
            close(mfd);
            return false;
        }
        ssize_t mr = read(mfd, mbuf, mst.st_size);
        close(mfd);
        if (mr <= 0) {
            free(mbuf);
            return false;
        }
        mbuf[mr] = '\0';
        bool parsed = parse_manifest_from_string(mbuf, mr, &active_manifest);
        free(mbuf);
        if (!parsed)
            return false;
    }

    /* Enforce capabilities */
    if (active_manifest.capabilities.network || active_manifest.capabilities.filesystem) {
        return false;
    }

    /* Derive or extract public key from private key */
    uint8_t derived_pubkey[32];
    bool has_pub = false;
    for (int i = 0; i < 32; i++) {
        if (priv_key[32 + i] != 0) {
            has_pub = true;
            break;
        }
    }
    if (has_pub) {
        memcpy(derived_pubkey, priv_key + 32, 32);
    } else {
        uint8_t temp_sk[64];
        memcpy(temp_sk, priv_key, 32);
        tweetnacl_ed25519_keypair(derived_pubkey, temp_sk);
    }
    memcpy(active_manifest.author_pubkey, derived_pubkey, 32);

    /* Generate canonical manifest.json */
    size_t canonical_manifest_len = 0;
    char *canonical_manifest =
        manifest_to_canonical_json(&active_manifest, &canonical_manifest_len);
    if (!canonical_manifest)
        return false;

    /* Collect source directory files */
    build_file_list_t file_list = {0};
    if (!collect_source_files(source_dir, "", &file_list)) {
        free(canonical_manifest);
        return false;
    }

    /* Overwrite or insert canonical manifest.json */
    bool found_mf = false;
    for (size_t i = 0; i < file_list.count; i++) {
        if (strcmp(file_list.entries[i].name, "manifest.json") == 0) {
            free(file_list.entries[i].data);
            file_list.entries[i].data = (uint8_t *)malloc(canonical_manifest_len);
            memcpy(file_list.entries[i].data, canonical_manifest, canonical_manifest_len);
            file_list.entries[i].size = canonical_manifest_len;
            found_mf = true;
            break;
        }
    }
    if (!found_mf) {
        uint8_t *mf_copy = (uint8_t *)malloc(canonical_manifest_len);
        memcpy(mf_copy, canonical_manifest, canonical_manifest_len);
        build_list_add(&file_list, "manifest.json", mf_copy, canonical_manifest_len);
    }

    /* Build Merkle tree for all files excluding signature.sig */
    merkle_file_t *mfiles = (merkle_file_t *)malloc(file_list.count * sizeof(merkle_file_t));
    if (!mfiles) {
        free(canonical_manifest);
        for (size_t i = 0; i < file_list.count; i++)
            free(file_list.entries[i].data);
        free(file_list.entries);
        return false;
    }

    for (size_t i = 0; i < file_list.count; i++) {
        strncpy(mfiles[i].name, file_list.entries[i].name, sizeof(mfiles[i].name) - 1);
        mfiles[i].name[sizeof(mfiles[i].name) - 1] = '\0';
        mfiles[i].data = file_list.entries[i].data;
        mfiles[i].size = file_list.entries[i].size;
    }

    uint8_t merkle_root[32];
    compute_merkle_root(mfiles, file_list.count, merkle_root);
    free(mfiles);

    /* Compute H_manifest */
    uint8_t h_manifest[32];
    wwpkg_sha256((const uint8_t *)canonical_manifest, canonical_manifest_len, h_manifest);
    free(canonical_manifest);

    /* Construct wwpkg_signature_t */
    wwpkg_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    sig.version = 1;
    sig.timestamp = (uint64_t)time(nullptr);
    memcpy(sig.author_pubkey, derived_pubkey, 32);
    memcpy(sig.merkle_root, merkle_root, 32);

    /* Construct 85-byte SignPayload */
    uint8_t sign_payload[85];
    const char magic_prefix[] = "WWPKG-SIG-V1\n";
    memcpy(sign_payload, magic_prefix, 13);
    memcpy(sign_payload + 13, h_manifest, 32);
    memcpy(sign_payload + 45, merkle_root, 32);

    uint64_t ts = sig.timestamp;
    sign_payload[77] = (uint8_t)(ts >> 56);
    sign_payload[78] = (uint8_t)(ts >> 48);
    sign_payload[79] = (uint8_t)(ts >> 40);
    sign_payload[80] = (uint8_t)(ts >> 32);
    sign_payload[81] = (uint8_t)(ts >> 24);
    sign_payload[82] = (uint8_t)(ts >> 16);
    sign_payload[83] = (uint8_t)(ts >> 8);
    sign_payload[84] = (uint8_t)(ts & 0xFF);

    /* Sign payload */
    if (wwpkg_ed25519_sign(sig.signature, sign_payload, sizeof(sign_payload), priv_key) != 0) {
        for (size_t i = 0; i < file_list.count; i++)
            free(file_list.entries[i].data);
        free(file_list.entries);
        return false;
    }

    /* Add signature.sig to file list */
    uint8_t *sig_blob = (uint8_t *)malloc(sizeof(sig));
    memcpy(sig_blob, &sig, sizeof(sig));
    build_list_add(&file_list, "signature.sig", sig_blob, sizeof(sig));

    /* Calculate total uncompressed tar size */
    size_t total_tar_size = 0;
    for (size_t i = 0; i < file_list.count; i++) {
        size_t pad = ((file_list.entries[i].size + 511) / 512) * 512;
        total_tar_size += 512 + pad;
    }
    total_tar_size += 1024; /* Two 512-byte zero blocks */

    uint8_t *tar_buf = (uint8_t *)calloc(1, total_tar_size);
    if (!tar_buf) {
        for (size_t i = 0; i < file_list.count; i++)
            free(file_list.entries[i].data);
        free(file_list.entries);
        return false;
    }

    /* Assemble POSIX ustar archive */
    size_t tar_off = 0;
    for (size_t i = 0; i < file_list.count; i++) {
        ustar_hdr_t *hdr = (ustar_hdr_t *)(tar_buf + tar_off);

        const char *name = file_list.entries[i].name;
        size_t nlen = strlen(name);
        if (nlen <= 100) {
            strncpy(hdr->name, name, sizeof(hdr->name));
        } else {
            /* Split into prefix and name */
            const char *slash = strrchr(name, '/');
            if (slash && (size_t)(slash - name) <= 155 &&
                (nlen - (size_t)(slash - name + 1)) <= 100) {
                strncpy(hdr->prefix, name, (size_t)(slash - name));
                strncpy(hdr->name, slash + 1, sizeof(hdr->name));
            } else {
                strncpy(hdr->name, name, sizeof(hdr->name));
            }
        }

        snprintf(hdr->mode, sizeof(hdr->mode), "%07o", 0644);
        snprintf(hdr->uid, sizeof(hdr->uid), "%07o", 0);
        snprintf(hdr->gid, sizeof(hdr->gid), "%07o", 0);
        snprintf(hdr->size, sizeof(hdr->size), "%011zo", file_list.entries[i].size);
        snprintf(hdr->mtime, sizeof(hdr->mtime), "%011lo", (unsigned long)sig.timestamp);
        hdr->typeflag = '0';
        memcpy(hdr->magic, "ustar\0", 6);
        memcpy(hdr->version, "00", 2);
        strncpy(hdr->uname, "waywal", sizeof(hdr->uname));
        strncpy(hdr->gname, "waywal", sizeof(hdr->gname));

        uint32_t csum = ustar_calc_chksum(hdr);
        snprintf(hdr->chksum, sizeof(hdr->chksum), "%06o", csum);
        hdr->chksum[6] = '\0';
        hdr->chksum[7] = ' ';

        tar_off += 512;
        if (file_list.entries[i].size > 0) {
            memcpy(tar_buf + tar_off, file_list.entries[i].data, file_list.entries[i].size);
        }
        size_t pad = ((file_list.entries[i].size + 511) / 512) * 512;
        tar_off += pad;
    }
    /* Trailing 1024 zero bytes already zero-initialized by calloc */

    for (size_t i = 0; i < file_list.count; i++) {
        free(file_list.entries[i].data);
    }
    free(file_list.entries);

    /* Compress with Zstandard level 12 if available */
    zstd_probe();
    uint8_t *final_data = tar_buf;
    size_t final_size = total_tar_size;
    bool compressed = false;

    if (g_zstd_available) {
        size_t bound = g_zstd.compressBound(total_tar_size);
        uint8_t *comp_buf = (uint8_t *)malloc(bound);
        if (comp_buf) {
            size_t c_res = g_zstd.compress(comp_buf, bound, tar_buf, total_tar_size, 12);
            if (!g_zstd.isError(c_res)) {
                final_data = comp_buf;
                final_size = c_res;
                compressed = true;
            } else {
                free(comp_buf);
            }
        }
    }

    /* Write out atomically */
    char tmp_out_path[PATH_MAX];
    snprintf(tmp_out_path, sizeof(tmp_out_path), "%s.tmp.%d", out_pkg_path, (int)getpid());

    int out_fd = open(tmp_out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) {
        if (compressed)
            free(final_data);
        free(tar_buf);
        return false;
    }

    size_t wr = 0;
    while (wr < final_size) {
        ssize_t w = write(out_fd, final_data + wr, final_size - wr);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            close(out_fd);
            unlink(tmp_out_path);
            if (compressed)
                free(final_data);
            free(tar_buf);
            return false;
        }
        wr += (size_t)w;
    }
    close(out_fd);

    if (compressed) {
        free(final_data);
    }
    free(tar_buf);

    if (rename(tmp_out_path, out_pkg_path) != 0) {
        unlink(tmp_out_path);
        return false;
    }

    return true;
}
