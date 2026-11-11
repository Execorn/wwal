// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "waywal/wwpkg.h"

/* Independent FIPS 180-4 reference SHA-256 for cross-checking runtime SHA-NI */
static uint32_t ref_rotr32(uint32_t x, unsigned int n) { return (x >> n) | (x << (32u - n)); }

static void ref_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u,
    };
    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    size_t padded = ((len + 9u + 63u) / 64u) * 64u;
    uint8_t *buf = (uint8_t *)calloc(1, padded);
    assert(buf != NULL);
    if (len > 0) {
        memcpy(buf, msg, len);
    }
    buf[len] = 0x80u;
    uint64_t bit_len = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) {
        buf[padded - 8u + (size_t)i] = (uint8_t)(bit_len >> (56 - i * 8));
    }

    for (size_t off = 0; off < padded; off += 64u) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = buf + off + (size_t)i * 4u;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
                   (uint32_t)p[3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ref_rotr32(w[i - 15], 7) ^ ref_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ref_rotr32(w[i - 2], 17) ^ ref_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ref_rotr32(e, 6) ^ ref_rotr32(e, 11) ^ ref_rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ref_rotr32(a, 2) ^ ref_rotr32(a, 13) ^ ref_rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned int v = 0;
        int matched = sscanf(hex + i * 2, "%2x", &v);
        assert(matched == 1);
        out[i] = (uint8_t)v;
    }
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char lut[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = lut[(in[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = lut[in[i] & 0x0Fu];
    }
    out[len * 2] = '\0';
}

static void write_file_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (len > 0) {
        assert(fwrite(data, 1, len, f) == len);
    }
    fclose(f);
}

static uint8_t *read_file_bytes(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long sz = ftell(f);
    assert(sz >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1u);
    assert(buf != NULL);
    if (sz > 0) {
        assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    }
    fclose(f);
    buf[sz] = '\0';
    *out_len = (size_t)sz;
    return buf;
}

typedef size_t (*zstd_compress_fn)(void *, size_t, const void *, size_t, int);
typedef size_t (*zstd_bound_fn)(size_t);
typedef size_t (*zstd_decompress_fn)(void *, size_t, const void *, size_t);
typedef size_t (*zstd_find_comp_size_fn)(const void *, size_t);
typedef unsigned (*zstd_is_error_fn)(size_t);

#define LOAD_DLSYM(dst_fn, fn_type, handle, sym_name)                                              \
    do {                                                                                           \
        union {                                                                                    \
            void *obj;                                                                             \
            fn_type fn;                                                                            \
        } u_;                                                                                      \
        u_.obj = dlsym((handle), (sym_name));                                                      \
        (dst_fn) = u_.fn;                                                                          \
    } while (0)

static uint8_t *zstd_compress_buf(const uint8_t *src, size_t src_len, size_t *out_len)
{
    void *h = dlopen("libzstd.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        h = dlopen("libzstd.so", RTLD_LAZY | RTLD_LOCAL);
    }
    assert(h != NULL);
    zstd_bound_fn fn_bound = NULL;
    zstd_compress_fn fn_comp = NULL;
    zstd_is_error_fn fn_err = NULL;
    LOAD_DLSYM(fn_bound, zstd_bound_fn, h, "ZSTD_compressBound");
    LOAD_DLSYM(fn_comp, zstd_compress_fn, h, "ZSTD_compress");
    LOAD_DLSYM(fn_err, zstd_is_error_fn, h, "ZSTD_isError");
    assert(fn_bound && fn_comp && fn_err);

    size_t cap = fn_bound(src_len);
    uint8_t *dst = (uint8_t *)malloc(cap);
    assert(dst != NULL);
    size_t csz = fn_comp(dst, cap, src, src_len, 3);
    assert(!fn_err(csz));
    *out_len = csz;
    return dst;
}

/* Recompute standard POSIX ustar header checksum */
static void recompute_tar_chksum(uint8_t hdr[512])
{
    memset(hdr + 148, ' ', 8);
    unsigned int sum = 0;
    for (size_t i = 0; i < 512; i++) {
        sum += hdr[i];
    }
    snprintf((char *)(hdr + 148), 8, "%06o", sum);
    hdr[154] = '\0';
    hdr[155] = ' ';
}

static void init_sample_manifest(wwpkg_manifest_t *m, const char *slug, const char *pipeline)
{
    memset(m, 0, sizeof(*m));
    strcpy(m->schema_version, "wwpkg/v2");
    strcpy(m->id, "a1b2c3d4-e5f6-7890-abcd-ef1234567890");
    strncpy(m->slug, slug, sizeof(m->slug) - 1);
    strcpy(m->name, "Neon Fractal 4K");
    strcpy(m->version, "1.0.0");
    strcpy(m->author_name, "WayWal Contributor");
    strcpy(m->author_email, "dev@waywal.org");
    strcpy(m->license, "MIT");
    strcpy(m->description, "Test wallpaper container");
    strncpy(m->entrypoint_pipeline, pipeline, sizeof(m->entrypoint_pipeline) - 1);
    strcpy(m->entrypoint_preview, "preview.webp");
    m->type = WWPKG_TYPE_SHADER_FRAGMENT;
    strcpy(m->min_wwal_version, "1.0.0");
    m->abi_version = 1;
    strcpy(m->gl_profile, "gles_31");
    m->budgets.max_vram_mb = 128;
    m->budgets.target_fps = 60;
}

static void test_sha256_and_shani(void)
{
    printf("[WWPKG 1/9] Testing SHA-256 NIST vectors & multi-block SHA-NI consistency...\n");

    uint8_t digest[32];
    uint8_t expected[32];

    wwpkg_sha256((const uint8_t *)"", 0, digest);
    hex_to_bytes("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, 32);
    assert(memcmp(digest, expected, 32) == 0);

    wwpkg_sha256((const uint8_t *)"abc", 3, digest);
    hex_to_bytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, 32);
    assert(memcmp(digest, expected, 32) == 0);

    const size_t big_len = 131079u;
    uint8_t *big = (uint8_t *)malloc(big_len);
    assert(big != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (uint8_t)((i * 131u + 17u) & 0xFFu);
    }
    wwpkg_sha256(big, big_len, digest);
    ref_sha256(big, big_len, expected);
    assert(memcmp(digest, expected, 32) == 0 && "SHA-NI digest must match FIPS 180-4 reference");
    free(big);

    printf("  -> SHA-256 & runtime SHA-NI verified.\n");
}

static void test_ed25519_seed_and_malleability(void)
{
    printf("[WWPKG 2/9] Testing RFC 8032 Ed25519 vectors, seed derivation & S < L check...\n");

    /* RFC 8032 Section 7.1 TEST 1 */
    uint8_t rfc_seed[32], rfc_exp_pk[32], rfc_exp_sig[64];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", rfc_seed, 32);
    hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", rfc_exp_pk,
                 32);
    hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                 "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                 rfc_exp_sig, 64);

    uint8_t pk[32] = {0};
    uint8_t sk[64] = {0};
    assert(wwpkg_ed25519_seed_keypair(pk, sk, rfc_seed) == 0);
    assert(memcmp(pk, rfc_exp_pk, 32) == 0 && "RFC 8032 Test 1 derived pubkey mismatch");
    assert(memcmp(sk, rfc_seed, 32) == 0 && memcmp(sk + 32, rfc_exp_pk, 32) == 0);

    uint8_t sig[64] = {0};
    assert(wwpkg_ed25519_sign(sig, (const uint8_t *)"", 0, sk) == 0);
    assert(memcmp(sig, rfc_exp_sig, 64) == 0 && "RFC 8032 Test 1 deterministic signature mismatch");
    assert(wwpkg_ed25519_verify(sig, (const uint8_t *)"", 0, pk) == 0);

    /* Non-canonical scalar S >= L malleability checks: S' = S + L */
    static const uint8_t ed25519_L[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
        0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    };
    uint8_t malleable_sig[64];
    memcpy(malleable_sig, sig, 64);
    uint16_t carry = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t sum = (uint16_t)malleable_sig[32 + i] + (uint16_t)ed25519_L[i] + carry;
        malleable_sig[32 + i] = (uint8_t)(sum & 0xFFu);
        carry = sum >> 8;
    }
    assert(wwpkg_ed25519_verify(malleable_sig, (const uint8_t *)"", 0, pk) != 0 &&
           "Malleable signature S + L must be rejected");

    /* Exact S == L must be rejected */
    memcpy(malleable_sig, sig, 32);
    memcpy(malleable_sig + 32, ed25519_L, 32);
    assert(wwpkg_ed25519_verify(malleable_sig, (const uint8_t *)"", 0, pk) != 0 &&
           "S == L must be rejected");

    /* High bits in S[31] must be rejected */
    memcpy(malleable_sig, sig, 64);
    malleable_sig[63] |= 0x20u;
    assert(wwpkg_ed25519_verify(malleable_sig, (const uint8_t *)"", 0, pk) != 0);

    /* Message and bit-flip tamper checks on random keypair */
    uint8_t rpk[32] = {0}, rsk[64] = {0};
    wwpkg_generate_keypair(rpk, rsk);
    const uint8_t msg[] = "WayWal Open Hub Cryptographic Integrity Payload";
    assert(wwpkg_ed25519_sign(sig, msg, sizeof(msg) - 1, rsk) == 0);
    assert(wwpkg_ed25519_verify(sig, msg, sizeof(msg) - 1, rpk) == 0);
    sig[0] ^= 0x01u;
    assert(wwpkg_ed25519_verify(sig, msg, sizeof(msg) - 1, rpk) != 0);

    printf("  -> RFC 8032 Ed25519 & canonical S < L checks passed.\n");
}

static void test_seed_only_package_signing(const char *base_tmp)
{
    printf("[WWPKG 3/9] Testing 32-byte seed-only wwpkg_build_package -> verify round-trip...\n");

    char src_dir[256], pkg_path[256], frag_path[256], prev_path[256];
    snprintf(src_dir, sizeof(src_dir), "%s/seed_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/seed_pkg.wwpkg", base_tmp);
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(prev_path, sizeof(prev_path), "%s/preview.webp", src_dir);
    mkdir(src_dir, 0755);

    const char *frag =
        "#version 310 es\nprecision highp float;\nout vec4 o;\nvoid main(){o=vec4(1);}\n";
    const char *webp = "RIFF\x10\x00\x00\x00WEBPVP8 ";
    write_file_bytes(frag_path, frag, strlen(frag));
    write_file_bytes(prev_path, webp, 16);

    uint8_t seed[32];
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 7));
    }
    uint8_t expected_pk[32] = {0}, full_sk[64] = {0};
    assert(wwpkg_ed25519_seed_keypair(expected_pk, full_sk, seed) == 0);

    /* Pass 32-byte seed in [0..31] with [32..63] all-zero */
    uint8_t seed_only_sk[64];
    memset(seed_only_sk, 0, sizeof(seed_only_sk));
    memcpy(seed_only_sk, seed, 32);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "seed-signed", "shader.frag");
    assert(wwpkg_build_package(src_dir, &mf, seed_only_sk, pkg_path));

    wwpkg_package_t *pkg = wwpkg_open(pkg_path);
    assert(pkg != NULL);
    const wwpkg_manifest_t *got_mf = wwpkg_get_manifest(pkg);
    assert(got_mf != NULL);
    assert(memcmp(got_mf->author_pubkey, expected_pk, 32) == 0 &&
           "Manifest author_pubkey must equal derived seed public key");

    bool is_trusted = false;
    assert(wwpkg_verify_signature(pkg, &is_trusted) &&
           "Package signed with 32-byte seed must verify cleanly");
    wwpkg_close(pkg);

    unlink(frag_path);
    unlink(prev_path);
    rmdir(src_dir);
    unlink(pkg_path);

    printf("  -> 32-byte seed-only build & verify round-trip passed.\n");
}

static uint8_t *zstd_decompress_frame_exact(const uint8_t *src, size_t src_len, size_t max_out,
                                            size_t *out_len, size_t *out_comp_frame_len)
{
    void *h = dlopen("libzstd.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        h = dlopen("libzstd.so", RTLD_LAZY | RTLD_LOCAL);
    }
    assert(h != NULL);
    zstd_decompress_fn fn_dec = NULL;
    zstd_find_comp_size_fn fn_find = NULL;
    zstd_is_error_fn fn_err = NULL;
    LOAD_DLSYM(fn_dec, zstd_decompress_fn, h, "ZSTD_decompress");
    LOAD_DLSYM(fn_find, zstd_find_comp_size_fn, h, "ZSTD_findFrameCompressedSize");
    LOAD_DLSYM(fn_err, zstd_is_error_fn, h, "ZSTD_isError");
    assert(fn_dec && fn_find && fn_err);

    size_t frame_clen = fn_find(src, src_len);
    assert(!fn_err(frame_clen) && frame_clen <= src_len);

    uint8_t *dst = (uint8_t *)malloc(max_out);
    assert(dst != NULL);
    size_t dsz = fn_dec(dst, max_out, src, frame_clen);
    assert(!fn_err(dsz));
    *out_len = dsz;
    if (out_comp_frame_len) {
        *out_comp_frame_len = frame_clen;
    }
    return dst;
}

static void test_merkle_path_swap_rejection(const char *base_tmp, const uint8_t priv_key[64])
{
    printf("[WWPKG 4/9] Testing RFC 6962 domain separation & Merkle file-path swap rejection...\n");

    /* Domain separation: leaf (0x00) != internal node (0x01) */
    uint8_t leaf_in[65], node_in[65], h_leaf[32], h_node[32];
    memset(leaf_in, 0x42, sizeof(leaf_in));
    memcpy(node_in, leaf_in, sizeof(node_in));
    leaf_in[0] = 0x00u;
    node_in[0] = 0x01u;
    wwpkg_sha256(leaf_in, sizeof(leaf_in), h_leaf);
    wwpkg_sha256(node_in, sizeof(node_in), h_node);
    assert(memcmp(h_leaf, h_node, 32) != 0);

    char src_dir[256], pkg_path[256], path_a[256], path_b[256], path_prev[256];
    snprintf(src_dir, sizeof(src_dir), "%s/swap_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/swap_pkg.wwpkg", base_tmp);
    snprintf(path_a, sizeof(path_a), "%s/a.frag", src_dir);
    snprintf(path_b, sizeof(path_b), "%s/b.frag", src_dir);
    snprintf(path_prev, sizeof(path_prev), "%s/preview.webp", src_dir);
    mkdir(src_dir, 0755);

    const char *shader_a = "void mainImage(out vec4 c, in vec2 f){ c = vec4(1,0,0,1); }\n";
    const char *shader_b = "void mainImage(out vec4 c, in vec2 f){ c = vec4(0,1,0,1); }\n";
    const char *webp = "RIFF\x14\x00\x00\x00WEBPVP8 mock";
    write_file_bytes(path_a, shader_a, strlen(shader_a));
    write_file_bytes(path_b, shader_b, strlen(shader_b));
    write_file_bytes(path_prev, webp, 20);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "swap-guard", "a.frag");
    assert(wwpkg_build_package(src_dir, &mf, priv_key, pkg_path));

    size_t raw_pkg_len = 0;
    uint8_t *raw_pkg = read_file_bytes(pkg_path, &raw_pkg_len);
    assert(raw_pkg_len > sizeof(wwpkg_outer_envelope_t));

    wwpkg_outer_envelope_t env;
    memcpy(&env, raw_pkg, sizeof(env));
    assert(env.magic_le == WWPKG_MAGIC_SKIPPABLE_ENV);
    assert(env.frame_size_le == WWPKG_OUTER_ENV_PAYLOAD);

    size_t f0_off = sizeof(wwpkg_outer_envelope_t);
    size_t f0_clen = 0, f1_clen = 0;
    size_t f0_ulen = 0, f1_ulen = 0;
    uint8_t *f0_tar = zstd_decompress_frame_exact(raw_pkg + f0_off, raw_pkg_len - f0_off, 131072,
                                                  &f0_ulen, &f0_clen);
    size_t f1_off = f0_off + f0_clen;
    assert(f1_off < raw_pkg_len);
    uint8_t *f1_tar = zstd_decompress_frame_exact(raw_pkg + f1_off, raw_pkg_len - f1_off, 131072,
                                                  &f1_ulen, &f1_clen);
    assert(f0_ulen >= 1024 && f1_ulen >= 1536);

    /* Frame 0 + Frame 1 concatenate directly into one valid POSIX ustar stream */
    size_t combined_len = f0_ulen + f1_ulen;
    uint8_t *combined_tar = (uint8_t *)malloc(combined_len);
    assert(combined_tar != NULL);
    memcpy(combined_tar, f0_tar, f0_ulen);
    memcpy(combined_tar + f0_ulen, f1_tar, f1_ulen);

    /* Baseline: unmodified single-frame .wwpkg archive must open & verify cleanly */
    size_t sf_clen = 0;
    uint8_t *sf_valid = zstd_compress_buf(combined_tar, combined_len, &sf_clen);
    wwpkg_package_t *sf_pkg = wwpkg_open_memory(sf_valid, sf_clen);
    assert(sf_pkg != NULL && "Single-frame fallback wwpkg_open_memory must succeed");
    bool trusted = false;
    assert(wwpkg_verify_signature(sf_pkg, &trusted) &&
           "Unmodified single-frame .wwpkg must pass signature & Merkle verification");
    wwpkg_close(sf_pkg);
    free(sf_valid);

    /* Swap filenames a.frag <-> b.frag in ustar headers while keeping data & checksums valid */
    int swapped_headers = 0;
    for (size_t off = 0; off + 512 <= combined_len; off += 512) {
        uint8_t *hdr = combined_tar + off;
        if (hdr[0] == '\0') {
            break;
        }
        if (strcmp((const char *)hdr, "a.frag") == 0) {
            memset(hdr, 0, 100);
            strcpy((char *)hdr, "b.frag");
            recompute_tar_chksum(hdr);
            swapped_headers++;
        } else if (strcmp((const char *)hdr, "b.frag") == 0) {
            memset(hdr, 0, 100);
            strcpy((char *)hdr, "a.frag");
            recompute_tar_chksum(hdr);
            swapped_headers++;
        }
    }
    assert(swapped_headers == 2);

    size_t swapped_clen = 0;
    uint8_t *sf_swapped = zstd_compress_buf(combined_tar, combined_len, &swapped_clen);
    wwpkg_package_t *bad_pkg = wwpkg_open_memory(sf_swapped, swapped_clen);
    assert(bad_pkg != NULL);
    assert(!wwpkg_verify_signature(bad_pkg, &trusted) &&
           "Merkle path-bound leaves MUST reject swapped a.frag <-> b.frag filenames!");
    wwpkg_close(bad_pkg);

    free(sf_swapped);
    free(combined_tar);
    free(f0_tar);
    free(f1_tar);
    free(raw_pkg);
    unlink(path_a);
    unlink(path_b);
    unlink(path_prev);
    rmdir(src_dir);
    unlink(pkg_path);

    /* Verify backward-compatible wwpkg/v1 Merkle leaf (0x00 || data) and signature.version == 1 */
    uint8_t v1_pk[32] = {0}, v1_sk[64] = {0};
    wwpkg_generate_keypair(v1_pk, v1_sk);
    char v1_pk_hex[65];
    bytes_to_hex(v1_pk, 32, v1_pk_hex);

    char v1_raw_mf[1024];
    snprintf(v1_raw_mf, sizeof(v1_raw_mf),
             "{\"schema_version\":\"wwpkg/v1\",\"id\":\"11111111-2222-3333-4444-555555555555\","
             "\"slug\":\"legacy-v1\",\"name\":\"Legacy V1\",\"version\":\"1.0.0\","
             "\"author\":{\"name\":\"Legacy\",\"pubkey\":\"%s\"},\"license\":\"MIT\","
             "\"type\":\"shader_fragment\","
             "\"entrypoint\":{\"pipeline\":\"a.frag\",\"preview\":\"preview.webp\"},"
             "\"engine_requirements\":{\"min_wwal_version\":\"1.0.0\",\"abi_version\":1,"
             "\"gl_profile\":\"gles_31\"},"
             "\"capabilities\":{\"compute_shaders\":false,\"audio_spectrum\":false,"
             "\"audio_playback\":false,\"scripting\":false,\"foreign_toplevel\":false,"
             "\"network\":false,\"filesystem\":false}}",
             v1_pk_hex);
    size_t v1_mf_len = 0;
    char *v1_canon_mf = wwpkg_canonicalize_json(v1_raw_mf, strlen(v1_raw_mf), &v1_mf_len);
    assert(v1_canon_mf != NULL);

    /* Sorted leaves: [0] = a.frag, [1] = manifest.json, [2] = preview.webp */
    size_t len_a = strlen(shader_a);
    size_t len_p = 20;
    uint8_t *l0_in = (uint8_t *)malloc(1 + len_a);
    uint8_t *l1_in = (uint8_t *)malloc(1 + v1_mf_len);
    uint8_t *l2_in = (uint8_t *)malloc(1 + len_p);
    assert(l0_in && l1_in && l2_in);
    l0_in[0] = 0x00u;
    memcpy(l0_in + 1, shader_a, len_a);
    l1_in[0] = 0x00u;
    memcpy(l1_in + 1, v1_canon_mf, v1_mf_len);
    l2_in[0] = 0x00u;
    memcpy(l2_in + 1, webp, len_p);

    uint8_t h0[32], h1[32], h2[32], h01[32], v1_root[32];
    wwpkg_sha256(l0_in, 1 + len_a, h0);
    wwpkg_sha256(l1_in, 1 + v1_mf_len, h1);
    wwpkg_sha256(l2_in, 1 + len_p, h2);
    free(l0_in);
    free(l1_in);
    free(l2_in);

    uint8_t nbuf[65];
    nbuf[0] = 0x01u;
    memcpy(nbuf + 1, h0, 32);
    memcpy(nbuf + 33, h1, 32);
    wwpkg_sha256(nbuf, 65, h01);
    memcpy(nbuf + 1, h01, 32);
    memcpy(nbuf + 33, h2, 32);
    wwpkg_sha256(nbuf, 65, v1_root);

    uint8_t v1_h_mf[32];
    wwpkg_sha256((const uint8_t *)v1_canon_mf, v1_mf_len, v1_h_mf);

    wwpkg_signature_t v1_sig;
    memset(&v1_sig, 0, sizeof(v1_sig));
    v1_sig.version = 1;
    v1_sig.timestamp = 1700000000ULL;
    memcpy(v1_sig.author_pubkey, v1_pk, 32);
    memcpy(v1_sig.merkle_root, v1_root, 32);

    uint8_t v1_sign_msg[85];
    memcpy(v1_sign_msg, "WWPKG-SIG-V1\n", 13);
    memcpy(v1_sign_msg + 13, v1_h_mf, 32);
    memcpy(v1_sign_msg + 45, v1_root, 32);
    for (int i = 0; i < 8; i++) {
        v1_sign_msg[77 + i] = (uint8_t)(v1_sig.timestamp >> (56 - i * 8));
    }
    assert(wwpkg_ed25519_sign(v1_sig.signature, v1_sign_msg, sizeof(v1_sign_msg), v1_sk) == 0);

    /* Build single-frame ustar for v1: manifest.json, signature.sig, a.frag, preview.webp */
    size_t mf_blocks = ((v1_mf_len + 511u) / 512u) * 512u;
    size_t v1_tar_len = (512u + mf_blocks) + 1024u + 1024u + 1024u + 1024u;
    uint8_t *v1_tar = (uint8_t *)calloc(1, v1_tar_len);
    assert(v1_tar != NULL);

    size_t off = 0;
    const char *names[4] = {"manifest.json", "signature.sig", "a.frag", "preview.webp"};
    const void *blobs[4] = {v1_canon_mf, &v1_sig, shader_a, webp};
    const size_t sizes[4] = {v1_mf_len, sizeof(v1_sig), len_a, len_p};
    size_t sig_hdr_off = 0;
    for (int idx = 0; idx < 4; idx++) {
        if (idx == 1) {
            sig_hdr_off = off;
        }
        uint8_t *th = v1_tar + off;
        strcpy((char *)th, names[idx]);
        strcpy((char *)(th + 100), "0000644");
        strcpy((char *)(th + 108), "0000000");
        strcpy((char *)(th + 116), "0000000");
        snprintf((char *)(th + 124), 12, "%011llo", (unsigned long long)sizes[idx]);
        strcpy((char *)(th + 136), "00000000000");
        th[156] = '0';
        memcpy(th + 257, "ustar", 6);
        memcpy(th + 263, "00", 2);
        recompute_tar_chksum(th);
        memcpy(v1_tar + off + 512u, blobs[idx], sizes[idx]);
        off += 512u + (((sizes[idx] + 511u) / 512u) * 512u);
    }

    size_t v1_clen = 0;
    uint8_t *v1_zstd = zstd_compress_buf(v1_tar, v1_tar_len, &v1_clen);
    wwpkg_package_t *v1_pkg = wwpkg_open_memory(v1_zstd, v1_clen);
    assert(v1_pkg != NULL);
    assert(wwpkg_verify_signature(v1_pkg, &trusted) &&
           "Valid wwpkg/v1 archive with v1 Merkle leaves & signature.version==1 must verify");
    wwpkg_close(v1_pkg);
    free(v1_zstd);

    /* Downgrade/mismatch check: wwpkg/v1 manifest with signature.version == 2 must be rejected */
    v1_sig.version = 2;
    memcpy(v1_sign_msg, "WWPKG-SIG-V2\n", 13);
    assert(wwpkg_ed25519_sign(v1_sig.signature, v1_sign_msg, sizeof(v1_sign_msg), v1_sk) == 0);
    memcpy(v1_tar + sig_hdr_off + 512u, &v1_sig, sizeof(v1_sig));
    uint8_t *v1_bad_zstd = zstd_compress_buf(v1_tar, v1_tar_len, &v1_clen);
    wwpkg_package_t *v1_bad_pkg = wwpkg_open_memory(v1_bad_zstd, v1_clen);
    assert(v1_bad_pkg != NULL);
    assert(!wwpkg_verify_signature(v1_bad_pkg, &trusted) &&
           "wwpkg/v1 manifest paired with signature.version != 1 must be rejected");
    wwpkg_close(v1_bad_pkg);
    free(v1_bad_zstd);
    free(v1_tar);
    free(v1_canon_mf);

    printf("  -> Single-frame v1/v2 fallback & Merkle file-path swap rejection verified.\n");
}

static void test_tar_hardening_and_symlink_rejection(const char *base_tmp,
                                                     const uint8_t priv_key[64])
{
    printf(
        "[WWPKG 5/9] Testing tar 64-bit overflow, typeflag=='5', long path & symlink guards...\n");

    /* 1. 64-bit size overflow / wraparound in ustar header */
    uint8_t bad_tar[1536];
    memset(bad_tar, 0, sizeof(bad_tar));
    strcpy((char *)bad_tar, "evil.bin");
    strcpy((char *)(bad_tar + 100), "0000644");
    strcpy((char *)(bad_tar + 124), "77777777777"); /* ~8 GiB size in 1.5 KiB buffer */
    bad_tar[156] = '0';
    memcpy(bad_tar + 257, "ustar", 6);
    memcpy(bad_tar + 263, "00", 2);
    recompute_tar_chksum(bad_tar);

    size_t clen = 0;
    uint8_t *cbuf = zstd_compress_buf(bad_tar, sizeof(bad_tar), &clen);
    assert(wwpkg_open_memory(cbuf, clen) == NULL &&
           "Oversized tar entry must be rejected without wraparound");
    assert(wwpkg_get_last_error(NULL) != WWPKG_OK);
    assert(strlen(wwpkg_error_str(wwpkg_get_last_error(NULL))) > 0);
    free(cbuf);

    /* 2. Directory header (typeflag == '5') with non-zero size must be rejected */
    memset(bad_tar, 0, sizeof(bad_tar));
    strcpy((char *)bad_tar, "subdir/");
    strcpy((char *)(bad_tar + 100), "0000755");
    strcpy((char *)(bad_tar + 124), "00000000010"); /* non-zero fsize=8 on directory */
    bad_tar[156] = '5';
    memcpy(bad_tar + 257, "ustar", 6);
    memcpy(bad_tar + 263, "00", 2);
    recompute_tar_chksum(bad_tar);

    cbuf = zstd_compress_buf(bad_tar, sizeof(bad_tar), &clen);
    assert(wwpkg_open_memory(cbuf, clen) == NULL &&
           "Directory tar header with non-zero size must be rejected");
    free(cbuf);

    /* 3. Basename > 100 bytes must be rejected during wwpkg_build_package */
    char src_dir[256], pkg_path[256], long_file[512];
    snprintf(src_dir, sizeof(src_dir), "%s/tar_guard_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/tar_guard.wwpkg", base_tmp);
    mkdir(src_dir, 0755);

    char name105[106];
    memset(name105, 'a', 100);
    memcpy(name105 + 100, ".frag", 6);
    snprintf(long_file, sizeof(long_file), "%s/%s", src_dir, name105);
    write_file_bytes(long_file, "void main(){}", 13);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "tar-guard", "shader.frag");
    assert(!wwpkg_build_package(src_dir, &mf, priv_key, pkg_path) &&
           "Basename > 100 bytes must be rejected");
    unlink(long_file);

    /* 4. Symlink inside source_dir must be rejected (lstat + O_NOFOLLOW) */
    char frag_path[256], sym_path[256];
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(sym_path, sizeof(sym_path), "%s/leak.txt", src_dir);
    write_file_bytes(frag_path, "void main(){}", 13);
    assert(symlink("/etc/hosts", sym_path) == 0);

    assert(!wwpkg_build_package(src_dir, &mf, priv_key, pkg_path) &&
           "Symlinks inside source_dir must be rejected");

    unlink(sym_path);
    unlink(frag_path);
    rmdir(src_dir);
    unlink(pkg_path);

    printf("  -> Tar overflow, typeflag=='5', basename > 100, and symlink guards passed.\n");
}

static void test_rfc8785_jcs_hardening_and_locale(const char *base_tmp, const uint8_t priv_key[64])
{
    printf("[WWPKG 6/9] Testing RFC 8785 JCS hardening, UTF-16 surrogates & comma LC_NUMERIC...\n");

    const char *unformatted = "{\n"
                              "  \"zebra\": 1,\n"
                              "  \"apple\": \"red\",\n"
                              "  \"banana\": true,\n"
                              "  \"capabilities\": {\n"
                              "    \"network\": false,\n"
                              "    \"compute_shaders\": true\n"
                              "  }\n"
                              "}";
    size_t out_len = 0;
    char *canon = wwpkg_canonicalize_json(unformatted, strlen(unformatted), &out_len);
    assert(canon != NULL && out_len > 0);
    assert(strcmp(canon, "{\"apple\":\"red\",\"banana\":true,\"capabilities\":{"
                         "\"compute_shaders\":true,\"network\":false},\"zebra\":1}") == 0);
    free(canon);

    /* UTF-16 code unit key ordering: U+10000 (0xD800 0xDC00) < U+E000 (0xE000) */
    const char *utf16_order = "{\"\\uE000\":1,\"\\uD800\\uDC00\":2}";
    canon = wwpkg_canonicalize_json(utf16_order, strlen(utf16_order), &out_len);
    assert(canon != NULL);
    const char *p_supp = strstr(canon, ":2");
    const char *p_bmp = strstr(canon, ":1");
    assert(p_supp != NULL && p_bmp != NULL && p_supp < p_bmp &&
           "RFC 8785 requires UTF-16 code unit ordering");
    free(canon);

    /* Reject trailing non-whitespace bytes */
    assert(wwpkg_canonicalize_json("{\"a\":1} trailing", 16, &out_len) == NULL);

    /* Reject duplicate object keys */
    assert(wwpkg_canonicalize_json("{\"a\":1,\"a\":2}", 13, &out_len) == NULL);

    /* Reject lone high/low UTF-16 surrogates */
    assert(wwpkg_canonicalize_json("{\"bad\":\"\\uD800\"}", 16, &out_len) == NULL);
    assert(wwpkg_canonicalize_json("{\"bad\":\"\\uDC00\"}", 16, &out_len) == NULL);
    assert(wwpkg_canonicalize_json("{\"bad\":\"\\uD800x\"}", 17, &out_len) == NULL);

    /* Reject nesting depth > 64 */
    char deep[140];
    for (int i = 0; i < 66; i++) {
        deep[i] = '[';
        deep[131 - i] = ']';
    }
    deep[132] = '\0';
    assert(wwpkg_canonicalize_json(deep, 132, &out_len) == NULL);

    /* Control character escaping (0x01, 0x1F) + 17-digit IEEE-754 float formatting */
    const char *ctrl_and_nums =
        "{\"ctrl\":\"\\u0001\\u001F\\n\",\"f\":0.30000000000000004,\"negz\":-0.0,\"small\":1e-7}";
    /* Test under comma decimal locale if available on host */
    static const char *const comma_locales[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "ru_RU.UTF-8",
                                                "es_ES.UTF-8"};
    for (size_t i = 0; i < sizeof(comma_locales) / sizeof(comma_locales[0]); i++) {
        if (setlocale(LC_NUMERIC, comma_locales[i]) != NULL) {
            break;
        }
    }
    canon = wwpkg_canonicalize_json(ctrl_and_nums, strlen(ctrl_and_nums), &out_len);
    setlocale(LC_NUMERIC, "C");
    assert(canon != NULL);
    assert(strcmp(canon, "{\"ctrl\":\"\\u0001\\u001f\\n\",\"f\":0.30000000000000004,"
                         "\"negz\":0,\"small\":1e-7}") == 0);
    free(canon);

    /* JSON-escaping of special characters inside manifest strings */
    char src_dir[256], pkg_path[256], frag_path[256], prev_path[256];
    snprintf(src_dir, sizeof(src_dir), "%s/esc_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/esc_pkg.wwpkg", base_tmp);
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(prev_path, sizeof(prev_path), "%s/preview.webp", src_dir);
    mkdir(src_dir, 0755);
    write_file_bytes(frag_path, "void main(){}", 13);
    write_file_bytes(prev_path, "RIFF0000WEBPVP8 ", 16);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "escaped-meta", "shader.frag");
    strcpy(mf.name, "Neon \"Quote\" \\ Backslash\nLine\tTab");
    strcpy(mf.author_name, "O'Connor \"Dev\"");
    assert(wwpkg_build_package(src_dir, &mf, priv_key, pkg_path));

    wwpkg_package_t *pkg = wwpkg_open(pkg_path);
    assert(pkg != NULL);
    bool is_trusted = false;
    assert(wwpkg_verify_signature(pkg, &is_trusted));
    const wwpkg_manifest_t *got = wwpkg_get_manifest(pkg);
    assert(strcmp(got->name, mf.name) == 0);
    assert(strcmp(got->author_name, mf.author_name) == 0);
    wwpkg_close(pkg);

    unlink(frag_path);
    unlink(prev_path);
    rmdir(src_dir);
    unlink(pkg_path);

    printf("  -> RFC 8785 JCS hardening, surrogate, float & locale tests passed.\n");
}

static void test_trust_store_and_notary(const char *base_tmp)
{
    printf("[WWPKG 7/9] Testing structural trusted_keys.json & Notary Ed25519 verification...\n");

    uint8_t author_pk[32] = {0}, author_sk[64] = {0};
    uint8_t notary_pk[32] = {0}, notary_sk[64] = {0};
    uint8_t rogue_pk[32] = {0}, rogue_sk[64] = {0};
    wwpkg_generate_keypair(author_pk, author_sk);
    wwpkg_generate_keypair(notary_pk, notary_sk);
    wwpkg_generate_keypair(rogue_pk, rogue_sk);

    char author_hex[65], notary_hex[65];
    bytes_to_hex(author_pk, 32, author_hex);
    bytes_to_hex(notary_pk, 32, notary_hex);

    char src_dir[256], pkg_path[256], frag_path[256], prev_path[256], store_path[256];
    snprintf(src_dir, sizeof(src_dir), "%s/trust_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/trust_pkg.wwpkg", base_tmp);
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(prev_path, sizeof(prev_path), "%s/preview.webp", src_dir);
    snprintf(store_path, sizeof(store_path), "%s/trusted_keys.json", base_tmp);
    mkdir(src_dir, 0755);
    write_file_bytes(frag_path, "void main(){}", 13);
    write_file_bytes(prev_path, "RIFF0000WEBPVP8 ", 16);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "trust-suite", "shader.frag");
    assert(wwpkg_build_package(src_dir, &mf, author_sk, pkg_path));

    /* 1. Commented-out and revoked author keys must NOT be trusted */
    char revoked_json[1024];
    snprintf(revoked_json, sizeof(revoked_json),
             "{\n"
             "  // \"%s\"\n"
             "  /* \"%s\" */\n"
             "  \"trusted_keys\": [\n"
             "    {\"pubkey\": \"%s\", \"revoked\": true},\n"
             "    {\"pubkey\": \"%s\", \"status\": \"revoked\"}\n"
             "  ],\n"
             "  \"notary_keys\": [\"%s\"]\n"
             "}\n",
             author_hex, author_hex, author_hex, author_hex, notary_hex);
    write_file_bytes(store_path, revoked_json, strlen(revoked_json));

    wwpkg_package_t *pkg = wwpkg_open(pkg_path);
    assert(pkg != NULL);
    assert(wwpkg_verify_trust_with_store(pkg, store_path) == WWPKG_TRUST_UNTRUSTED &&
           "Commented or revoked key in trusted_keys.json must NOT grant trust");

    /* 2. Active author key -> WWPKG_TRUST_AUTHOR_TRUSTED */
    char active_json[1024];
    snprintf(active_json, sizeof(active_json),
             "{\n"
             "  \"trusted_keys\": [{\"pubkey\": \"%s\", \"revoked\": false}],\n"
             "  \"notary_keys\": [{\"pubkey\": \"%s\", \"role\": \"notary\"}]\n"
             "}\n",
             author_hex, notary_hex);
    write_file_bytes(store_path, active_json, strlen(active_json));
    assert(wwpkg_verify_trust_with_store(pkg, store_path) == WWPKG_TRUST_AUTHOR_TRUSTED);
    wwpkg_close(pkg);

    /* 3. Countersigned by an UNTRUSTED rogue notary key must NEVER return WWPKG_TRUST_NOTARIZED */
    size_t orig_pkg_len = 0;
    uint8_t *orig_pkg_bytes = read_file_bytes(pkg_path, &orig_pkg_len);
    char rogue_pkg_path[256];
    snprintf(rogue_pkg_path, sizeof(rogue_pkg_path), "%s/rogue_notarized.wwpkg", base_tmp);
    write_file_bytes(rogue_pkg_path, orig_pkg_bytes, orig_pkg_len);
    assert(wwpkg_notarize_package(rogue_pkg_path, rogue_sk));
    pkg = wwpkg_open(rogue_pkg_path);
    assert(pkg != NULL);
    wwpkg_trust_level_t rogue_lvl = wwpkg_verify_trust_with_store(pkg, store_path);
    assert(rogue_lvl == WWPKG_TRUST_AUTHOR_TRUSTED &&
           "Signature from untrusted notary key must NOT elevate to WWPKG_TRUST_NOTARIZED");
    wwpkg_close(pkg);
    unlink(rogue_pkg_path);

    /* 4. Genuine countersignature by trusted notary key -> WWPKG_TRUST_NOTARIZED */
    char not_pkg_path[256];
    snprintf(not_pkg_path, sizeof(not_pkg_path), "%s/notarized.wwpkg", base_tmp);
    write_file_bytes(not_pkg_path, orig_pkg_bytes, orig_pkg_len);
    free(orig_pkg_bytes);
    assert(wwpkg_notarize_package(not_pkg_path, notary_sk));
    pkg = wwpkg_open(not_pkg_path);
    assert(pkg != NULL);
    assert(wwpkg_verify_trust_with_store(pkg, store_path) == WWPKG_TRUST_NOTARIZED &&
           "Valid notary countersignature must return WWPKG_TRUST_NOTARIZED");
    wwpkg_close(pkg);

    unlink(not_pkg_path);
    unlink(store_path);
    unlink(frag_path);
    unlink(prev_path);
    rmdir(src_dir);
    unlink(pkg_path);

    printf("  -> Structural trusted_keys.json & Notary Ed25519 verification passed.\n");
}

static void test_dual_frame_metadata_fastpath_and_extract(const char *base_tmp,
                                                          const uint8_t priv_key[64])
{
    printf("[WWPKG 8/9] Benchmarking Dual-Frame wwpkg_open_metadata_only (< 60 us)...\n");

    char src_dir[256], assets_dir[256], pkg_path[256], frag_path[256], prev_path[256],
        bulk_path[256], ext_dir[256];
    snprintf(src_dir, sizeof(src_dir), "%s/df_src", base_tmp);
    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", src_dir);
    snprintf(pkg_path, sizeof(pkg_path), "%s/dual_frame_4mb.wwpkg", base_tmp);
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(prev_path, sizeof(prev_path), "%s/preview.webp", src_dir);
    snprintf(bulk_path, sizeof(bulk_path), "%s/bulk.bin", assets_dir);
    snprintf(ext_dir, sizeof(ext_dir), "%s/extracted", base_tmp);

    mkdir(src_dir, 0755);
    mkdir(assets_dir, 0755);

    const char *frag = "void mainImage(out vec4 c, in vec2 f){ c = vec4(0.2, 0.5, 0.9, 1.0); }\n";
    uint8_t webp[1024];
    memcpy(webp, "RIFF\xf8\x03\x00\x00WEBPVP8 ", 16);
    for (size_t i = 16; i < sizeof(webp); i++) {
        webp[i] = (uint8_t)(i & 0xFFu);
    }

    /* 4 MiB pseudo-random high-entropy payload so full decompression is measurably expensive */
    const size_t bulk_sz = 4u * 1024u * 1024u;
    uint8_t *bulk = (uint8_t *)malloc(bulk_sz);
    assert(bulk != NULL);
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < bulk_sz; i++) {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        bulk[i] = (uint8_t)((rng * 0x2545F4914F6CDD1DULL) >> 56);
    }

    write_file_bytes(frag_path, frag, strlen(frag));
    write_file_bytes(prev_path, webp, sizeof(webp));
    write_file_bytes(bulk_path, bulk, bulk_sz);

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "dual-frame-bench", "shader.frag");
    assert(wwpkg_build_package(src_dir, &mf, priv_key, pkg_path));

    /* Warm up once */
    wwpkg_package_t *warm = wwpkg_open_metadata_only(pkg_path);
    assert(warm != NULL);
    size_t prev_sz = 0;
    const uint8_t *prev_ptr = wwpkg_read_asset(warm, "preview.webp", &prev_sz);
    assert(prev_ptr != NULL && prev_sz == sizeof(webp));
    assert(memcmp(prev_ptr, webp, sizeof(webp)) == 0);
    wwpkg_close(warm);

    /* Benchmark 5 batches x 40 opens (thread CPU+syscall clock avoids multi-process runqueue noise)
     */
    const int BATCHES = 5;
    const int BATCH_ITERS = 40;
    double elapsed_us = 1e9;
    for (int b = 0; b < BATCHES; b++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
        for (int i = 0; i < BATCH_ITERS; i++) {
            wwpkg_package_t *mp = wwpkg_open_metadata_only(pkg_path);
            assert(mp != NULL);
            const wwpkg_manifest_t *gm = wwpkg_get_manifest(mp);
            assert(gm != NULL && gm->file_count == 3);
            wwpkg_close(mp);
        }
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
        double batch_us =
            ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-3) /
            (double)BATCH_ITERS;
        if (batch_us < elapsed_us) {
            elapsed_us = batch_us;
        }
    }
    printf(
        "  -> wwpkg_open_metadata_only latency (4 MiB archive): %.2f us/open (budget < 60 us).\n",
        elapsed_us);
    assert(elapsed_us < 60.0 &&
           "wwpkg_open_metadata_only must complete in < 60 us without bulk payload decompression");

    /* Verify lazy promotion when reading bulk asset from a metadata-only handle */
    wwpkg_package_t *lazy_pkg = wwpkg_open_metadata_only(pkg_path);
    assert(lazy_pkg != NULL);
    size_t got_bulk_sz = 0;
    const uint8_t *got_bulk = wwpkg_read_asset(lazy_pkg, "assets/bulk.bin", &got_bulk_sz);
    assert(got_bulk != NULL && got_bulk_sz == bulk_sz);
    assert(memcmp(got_bulk, bulk, bulk_sz) == 0);
    wwpkg_close(lazy_pkg);

    /* Verify pre-decompression outer Ed25519 tamper rejection */
    size_t raw_len = 0;
    uint8_t *raw_bytes = read_file_bytes(pkg_path, &raw_len);
    char bad_env_path[256];
    snprintf(bad_env_path, sizeof(bad_env_path), "%s/bad_outer_sig.wwpkg", base_tmp);
    raw_bytes[120] ^= 0x01u; /* Flip 1 bit inside outer_signature */
    write_file_bytes(bad_env_path, raw_bytes, raw_len);
    assert(wwpkg_open_metadata_only(bad_env_path) == NULL &&
           "Outer envelope signature tamper must be rejected pre-decompression");
    assert(wwpkg_open(bad_env_path) == NULL);
    unlink(bad_env_path);

    /* Restore outer envelope and flip 1 bit inside Frame 0 compressed metadata */
    raw_bytes[120] ^= 0x01u;
    assert(raw_len > sizeof(wwpkg_outer_envelope_t) + 24u);
    raw_bytes[sizeof(wwpkg_outer_envelope_t) + 20u] ^= 0x55u;
    write_file_bytes(bad_env_path, raw_bytes, raw_len);
    assert(wwpkg_open_metadata_only(bad_env_path) == NULL &&
           "Frame 0 tamper must be rejected even with valid outer envelope");
    unlink(bad_env_path);

    /* Recompress Frame 0 with a 1-byte flip in preview.webp + valid ustar checksum */
    raw_bytes[sizeof(wwpkg_outer_envelope_t) + 20u] ^= 0x55u;
    size_t orig_f0_clen = 0, orig_f0_ulen = 0;
    uint8_t *f0_plain = zstd_decompress_frame_exact(
        raw_bytes + sizeof(wwpkg_outer_envelope_t), raw_len - sizeof(wwpkg_outer_envelope_t),
        WWPKG_FRAME0_MAX_BYTES, &orig_f0_ulen, &orig_f0_clen);
    bool flipped_preview = false;
    for (size_t off = 0; off + 1024u <= orig_f0_ulen; off += 512u) {
        uint8_t *th = f0_plain + off;
        if (th[0] == '\0') {
            break;
        }
        if (strcmp((const char *)th, "preview.webp") == 0) {
            f0_plain[off + 512u + 16u] ^= 0xAAu;
            recompute_tar_chksum(th);
            flipped_preview = true;
            break;
        }
    }
    assert(flipped_preview);
    size_t forged_f0_clen = 0;
    uint8_t *forged_f0_comp = zstd_compress_buf(f0_plain, orig_f0_ulen, &forged_f0_clen);
    size_t bulk_rem_off = sizeof(wwpkg_outer_envelope_t) + orig_f0_clen;
    size_t bulk_rem_len = raw_len - bulk_rem_off;
    size_t forged_total = sizeof(wwpkg_outer_envelope_t) + forged_f0_clen + bulk_rem_len;
    uint8_t *forged_pkg = (uint8_t *)malloc(forged_total);
    assert(forged_pkg != NULL);
    memcpy(forged_pkg, raw_bytes, sizeof(wwpkg_outer_envelope_t));
    memcpy(forged_pkg + sizeof(wwpkg_outer_envelope_t), forged_f0_comp, forged_f0_clen);
    memcpy(forged_pkg + sizeof(wwpkg_outer_envelope_t) + forged_f0_clen, raw_bytes + bulk_rem_off,
           bulk_rem_len);
    write_file_bytes(bad_env_path, forged_pkg, forged_total);
    assert(wwpkg_open_metadata_only(bad_env_path) == NULL &&
           "Forged preview.webp in Frame 0 must be rejected by preview_sha256 check");
    unlink(bad_env_path);
    free(forged_pkg);
    free(forged_f0_comp);
    free(f0_plain);
    free(raw_bytes);

    /* Verify wwpkg_open_fd preserves caller file descriptor ownership & wwpkg_extract_all works */
    int fd = open(pkg_path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    wwpkg_package_t *fd_pkg = wwpkg_open_fd(fd);
    assert(fd_pkg != NULL);
    bool trusted = false;
    assert(wwpkg_verify_signature(fd_pkg, &trusted));
    assert(wwpkg_extract_all(fd_pkg, ext_dir));
    wwpkg_close(fd_pkg);
    assert(lseek(fd, 0, SEEK_CUR) >= 0 && "wwpkg_open_fd must not close caller's fd");
    close(fd);

    char ext_frag[256], ext_prev[256], ext_bulk[256], ext_sub[256];
    snprintf(ext_frag, sizeof(ext_frag), "%s/shader.frag", ext_dir);
    snprintf(ext_prev, sizeof(ext_prev), "%s/preview.webp", ext_dir);
    snprintf(ext_sub, sizeof(ext_sub), "%s/assets", ext_dir);
    snprintf(ext_bulk, sizeof(ext_bulk), "%s/assets/bulk.bin", ext_dir);
    size_t check_len = 0;
    uint8_t *check_bulk = read_file_bytes(ext_bulk, &check_len);
    assert(check_len == bulk_sz && memcmp(check_bulk, bulk, bulk_sz) == 0);
    free(check_bulk);

    unlink(ext_frag);
    unlink(ext_prev);
    unlink(ext_bulk);
    rmdir(ext_sub);
    rmdir(ext_dir);

    free(bulk);
    unlink(frag_path);
    unlink(prev_path);
    unlink(bulk_path);
    rmdir(assets_dir);
    rmdir(src_dir);
    unlink(pkg_path);

    printf(
        "  -> Dual-Frame fast-path, lazy promotion, wwpkg_open_fd & wwpkg_extract_all passed.\n");
}

static void test_cold_open_and_1bit_tamper(const char *base_tmp, const uint8_t priv_key[64])
{
    printf("[WWPKG 9/9] Benchmarking cold full open & 1-bit payload tamper rejection...\n");

    char src_dir[256], pkg_path[256], frag_path[256], prev_path[256], cfg_path[256];
    snprintf(src_dir, sizeof(src_dir), "%s/full_src", base_tmp);
    snprintf(pkg_path, sizeof(pkg_path), "%s/neon_fractal.wwpkg", base_tmp);
    snprintf(frag_path, sizeof(frag_path), "%s/shader.frag", src_dir);
    snprintf(prev_path, sizeof(prev_path), "%s/preview.webp", src_dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", src_dir);
    mkdir(src_dir, 0755);

    const char *frag = "void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }";
    const char *webp = "RIFF....WEBPVP8 ... mock webp bytes ...";
    const char *cfg = "{\"speed\": 1.0, \"particles\": 500}";
    write_file_bytes(frag_path, frag, strlen(frag));
    write_file_bytes(prev_path, webp, strlen(webp));
    write_file_bytes(cfg_path, cfg, strlen(cfg));

    wwpkg_manifest_t mf;
    init_sample_manifest(&mf, "neon-fractal", "shader.frag");
    assert(wwpkg_build_package(src_dir, &mf, priv_key, pkg_path));

    const int ITERATIONS = 100;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    for (int i = 0; i < ITERATIONS; ++i) {
        wwpkg_package_t *pkg = wwpkg_open(pkg_path);
        assert(pkg != NULL);
        size_t sz = 0;
        const uint8_t *data = wwpkg_read_asset(pkg, "shader.frag", &sz);
        assert(data != NULL && sz == strlen(frag));
        wwpkg_close(pkg);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_sec = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                         (double)(ts_end.tv_nsec - ts_start.tv_nsec) * 1e-9;
    double per_open_ms = (elapsed_sec / ITERATIONS) * 1000.0;
    printf("  -> Cold full open latency: %.4f ms/open (%d iterations).\n", per_open_ms, ITERATIONS);
    assert(per_open_ms < 8.0 && "Cold full open latency must be well below 8.0 ms budget");

    /* Flip 1 bit near the end of the bulk payload frame */
    size_t sz = 0;
    uint8_t *buf = read_file_bytes(pkg_path, &sz);
    assert(sz > 200);
    buf[sz - 12] ^= 0x01u;

    char tampered_pkg[256];
    snprintf(tampered_pkg, sizeof(tampered_pkg), "%s/tampered.wwpkg", base_tmp);
    write_file_bytes(tampered_pkg, buf, sz);
    free(buf);

    wwpkg_package_t *tpkg = wwpkg_open(tampered_pkg);
    if (tpkg != NULL) {
        bool is_trusted = false;
        assert(!wwpkg_verify_signature(tpkg, &is_trusted) &&
               "1-bit payload tampering must fail cryptographic verification");
        wwpkg_close(tpkg);
    }
    unlink(tampered_pkg);

    unlink(frag_path);
    unlink(prev_path);
    unlink(cfg_path);
    rmdir(src_dir);
    unlink(pkg_path);

    printf("  -> Cold open benchmark & 1-bit tamper rejection passed.\n");
}

static void test_promotion_retention_toctou_and_confinement(const char *base_tmp,
                                                            const uint8_t priv_key[64])
{
    printf("[WWPKG 10/10] Testing Frame 0 retention, path-swap TOCTOU, skippable frames & confined "
           "extraction...\n");

    char src1[256], src2[256], pkg1_path[256], pkg2_path[256], swap_path[256];
    snprintf(src1, sizeof(src1), "%s/ret_src1", base_tmp);
    snprintf(src2, sizeof(src2), "%s/ret_src2", base_tmp);
    snprintf(pkg1_path, sizeof(pkg1_path), "%s/ret_pkg1.wwpkg", base_tmp);
    snprintf(pkg2_path, sizeof(pkg2_path), "%s/ret_pkg2.wwpkg", base_tmp);
    snprintf(swap_path, sizeof(swap_path), "%s/ret_swap.wwpkg", base_tmp);
    mkdir(src1, 0700);
    mkdir(src2, 0700);

    /* 8 KiB preview exceeds inline Frame 0 buffer (5 KiB) while fitting inside Frame 0 (<64 KiB) */
    const size_t prev_len = 8192u;
    uint8_t *big_prev = (uint8_t *)malloc(prev_len);
    assert(big_prev != NULL);
    for (size_t i = 0; i < prev_len; ++i) {
        big_prev[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }

    char p1_frag[256], p1_prev[256], p2_frag[256], p2_prev[256];
    snprintf(p1_frag, sizeof(p1_frag), "%s/shader.frag", src1);
    snprintf(p1_prev, sizeof(p1_prev), "%s/preview.webp", src1);
    snprintf(p2_frag, sizeof(p2_frag), "%s/shader.frag", src2);
    snprintf(p2_prev, sizeof(p2_prev), "%s/preview.webp", src2);

    const char *frag1 = "void mainImage(out vec4 c, in vec2 f) { c = vec4(0.25, 0.5, 0.75, 1.0); }";
    const char *frag2 = "void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0, 0.0, 0.0, 1.0); }";
    write_file_bytes(p1_frag, frag1, strlen(frag1));
    write_file_bytes(p1_prev, big_prev, prev_len);
    write_file_bytes(p2_frag, frag2, strlen(frag2));
    write_file_bytes(p2_prev, "RIFF....WEBP", 12u);

    wwpkg_manifest_t mf1;
    init_sample_manifest(&mf1, "retention-pkg-one", "shader.frag");
    mf1.budgets.max_compute_workgroups[0] = 64u;
    mf1.budgets.max_compute_workgroups[1] = 32u;
    mf1.budgets.max_compute_workgroups[2] = 2u;
    assert(wwpkg_build_package(src1, &mf1, priv_key, pkg1_path));

    uint8_t pub2[32] = {0}, priv2[64] = {0};
    wwpkg_generate_keypair(pub2, priv2);
    wwpkg_manifest_t mf2;
    init_sample_manifest(&mf2, "retention-pkg-two", "shader.frag");
    assert(wwpkg_build_package(src2, &mf2, priv2, pkg2_path));

    /* 1. Frame 0 pointer stability across lazy promotion + max_compute_workgroups round-trip */
    wwpkg_package_t *meta_pkg = wwpkg_open_metadata_only(pkg1_path);
    assert(meta_pkg != NULL);
    const wwpkg_manifest_t *loaded_mf = wwpkg_get_manifest(meta_pkg);
    assert(loaded_mf != NULL);
    assert(loaded_mf->budgets.max_compute_workgroups[0] == 64u);
    assert(loaded_mf->budgets.max_compute_workgroups[1] == 32u);
    assert(loaded_mf->budgets.max_compute_workgroups[2] == 2u);

    size_t got_prev_sz = 0;
    const uint8_t *prev_before = wwpkg_read_asset(meta_pkg, "preview.webp", &got_prev_sz);
    assert(prev_before != NULL && got_prev_sz == prev_len);
    assert(wwpkg_verify_signature(meta_pkg, NULL));

    size_t got_frag_sz = 0;
    const uint8_t *frag_after = wwpkg_read_asset(meta_pkg, "shader.frag", &got_frag_sz);
    assert(frag_after != NULL && got_frag_sz == strlen(frag1));
    /* Reading prev_before after lazy Frame 1 promotion must not trigger Heap-UAF */
    assert(memcmp(prev_before, big_prev, prev_len) == 0);
    wwpkg_close(meta_pkg);

    /* 2. Path-swap TOCTOU rejection on metadata-only handle */
    size_t raw1_sz = 0, raw2_sz = 0;
    uint8_t *raw1 = read_file_bytes(pkg1_path, &raw1_sz);
    uint8_t *raw2 = read_file_bytes(pkg2_path, &raw2_sz);
    write_file_bytes(swap_path, raw1, raw1_sz);

    wwpkg_package_t *swap_pkg = wwpkg_open_metadata_only(swap_path);
    assert(swap_pkg != NULL);
    char swap_tmp[256];
    snprintf(swap_tmp, sizeof(swap_tmp), "%s/ret_swap.tmp", base_tmp);
    write_file_bytes(swap_tmp, raw2, raw2_sz);
    assert(rename(swap_tmp, swap_path) == 0);

    assert(!wwpkg_verify_signature(swap_pkg, NULL));
    size_t swapped_sz = 0;
    assert(wwpkg_read_asset(swap_pkg, "shader.frag", &swapped_sz) == NULL);
    char swap_ext_dir[256];
    snprintf(swap_ext_dir, sizeof(swap_ext_dir), "%s/ret_swap_ext", base_tmp);
    assert(mkdir(swap_ext_dir, 0700) == 0);
    assert(!wwpkg_extract_all(swap_pkg, swap_ext_dir));
    assert(rmdir(swap_ext_dir) == 0 && "dest_dir must stay empty on path-swap extract failure");
    wwpkg_close(swap_pkg);
    unlink(swap_path);

    /* 2b. Large (>64 KiB) preview.webp in Frame 1 tampered after metadata-only open:
     * wwpkg_extract_all must reject and leave dest_dir completely empty. */
    char f1p_src[256], f1p_frag[256], f1p_prev[256], f1p_pkg_path[256];
    snprintf(f1p_src, sizeof(f1p_src), "%s/f1prev_src", base_tmp);
    snprintf(f1p_frag, sizeof(f1p_frag), "%s/shader.frag", f1p_src);
    snprintf(f1p_prev, sizeof(f1p_prev), "%s/preview.webp", f1p_src);
    snprintf(f1p_pkg_path, sizeof(f1p_pkg_path), "%s/f1prev_base.wwpkg", base_tmp);
    assert(mkdir(f1p_src, 0700) == 0);
    write_file_bytes(f1p_frag, frag1, strlen(frag1));

    const size_t large_prev_len = 70u * 1024u;
    uint8_t *large_prev = (uint8_t *)malloc(large_prev_len);
    assert(large_prev != NULL);
    memcpy(large_prev, "RIFF0000WEBPVP8 ", 16u);
    for (size_t i = 16u; i < large_prev_len; i++) {
        large_prev[i] = (uint8_t)((i * 73u + 19u) & 0xFFu);
    }
    write_file_bytes(f1p_prev, large_prev, large_prev_len);
    assert(wwpkg_build_package(f1p_src, &mf1, priv_key, f1p_pkg_path));

    size_t f1p_raw_sz = 0;
    uint8_t *f1p_raw = read_file_bytes(f1p_pkg_path, &f1p_raw_sz);
    size_t f0_off = sizeof(wwpkg_outer_envelope_t);
    size_t f0_ulen = 0, f0_clen = 0;
    uint8_t *f0_tmp = zstd_decompress_frame_exact(f1p_raw + f0_off, f1p_raw_sz - f0_off,
                                                  WWPKG_FRAME0_MAX_BYTES, &f0_ulen, &f0_clen);
    free(f0_tmp);

    size_t f1_off = f0_off + f0_clen;
    size_t f1_ulen = 0, f1_clen_orig = 0;
    uint8_t *f1_plain = zstd_decompress_frame_exact(f1p_raw + f1_off, f1p_raw_sz - f1_off,
                                                    4u * 1024u * 1024u, &f1_ulen, &f1_clen_orig);
    bool flipped_f1_prev = false;
    for (size_t off = 0; off + 512u <= f1_ulen;) {
        uint8_t *th = f1_plain + off;
        if (th[0] == '\0') {
            break;
        }
        unsigned long long esz = strtoull((const char *)(th + 124), NULL, 8);
        if (strcmp((const char *)th, "preview.webp") == 0) {
            f1_plain[off + 512u + 16u] ^= 0x5Au;
            recompute_tar_chksum(th);
            flipped_f1_prev = true;
            break;
        }
        size_t padded_esz = ((size_t)esz + 511u) & ~511u;
        off += 512u + padded_esz;
    }
    assert(flipped_f1_prev);

    size_t forged_f1_clen = 0;
    uint8_t *forged_f1_comp = zstd_compress_buf(f1_plain, f1_ulen, &forged_f1_clen);
    size_t forged_f1_total = f1_off + forged_f1_clen;
    uint8_t *forged_f1_pkg = (uint8_t *)malloc(forged_f1_total);
    assert(forged_f1_pkg != NULL);
    memcpy(forged_f1_pkg, f1p_raw, f1_off);
    memcpy(forged_f1_pkg + f1_off, forged_f1_comp, forged_f1_clen);

    char tamper_f1_path[256], tamper_ext_dir[256];
    snprintf(tamper_f1_path, sizeof(tamper_f1_path), "%s/tamper_f1_prev.wwpkg", base_tmp);
    snprintf(tamper_ext_dir, sizeof(tamper_ext_dir), "%s/tamper_f1_ext", base_tmp);
    write_file_bytes(tamper_f1_path, forged_f1_pkg, forged_f1_total);

    wwpkg_package_t *tamper_meta = wwpkg_open_metadata_only(tamper_f1_path);
    assert(tamper_meta != NULL);
    assert(mkdir(tamper_ext_dir, 0700) == 0);
    assert(!wwpkg_extract_all(tamper_meta, tamper_ext_dir));
    assert(wwpkg_get_last_error(NULL) == WWPKG_ERR_BAD_SIGNATURE);

    DIR *td = opendir(tamper_ext_dir);
    assert(td != NULL);
    struct dirent *tent;
    while ((tent = readdir(td)) != NULL) {
        assert(strcmp(tent->d_name, ".") == 0 || strcmp(tent->d_name, "..") == 0);
    }
    closedir(td);
    assert(rmdir(tamper_ext_dir) == 0 &&
           "dest_dir must be empty after tampered preview.webp rejection on metadata-only pkg");
    wwpkg_close(tamper_meta);
    unlink(tamper_f1_path);
    unlink(f1p_pkg_path);
    unlink(f1p_frag);
    unlink(f1p_prev);
    rmdir(f1p_src);
    free(large_prev);
    free(f1p_raw);
    free(forged_f1_pkg);
    free(forged_f1_comp);
    free(f1_plain);

    /* 3. Benign RFC 8878 skippable frame (0x184D2A55) prepended before Frame 0 */
    uint8_t skip_hdr[12] = {0x55, 0x2A, 0x4D, 0x18, 0x04, 0x00, 0x00, 0x00, 'W', 'W', 'A', 'L'};
    uint8_t *with_skip = (uint8_t *)malloc(sizeof(skip_hdr) + raw1_sz);
    assert(with_skip != NULL);
    memcpy(with_skip, skip_hdr, sizeof(skip_hdr));
    memcpy(with_skip + sizeof(skip_hdr), raw1, raw1_sz);
    wwpkg_package_t *skip_pkg = wwpkg_open_memory(with_skip, sizeof(skip_hdr) + raw1_sz);
    assert(skip_pkg != NULL);
    assert(wwpkg_verify_signature(skip_pkg, NULL));
    wwpkg_close(skip_pkg);
    free(with_skip);

    /* 4. Zero-byte regular file returns WWPKG_ERR_CORRUPT (distinct from WWPKG_ERR_IO) */
    char empty_path[256];
    snprintf(empty_path, sizeof(empty_path), "%s/empty.wwpkg", base_tmp);
    write_file_bytes(empty_path, "", 0u);
    assert(wwpkg_open(empty_path) == NULL);
    assert(wwpkg_get_last_error(NULL) == WWPKG_ERR_CORRUPT);
    assert(wwpkg_open_metadata_only(empty_path) == NULL);
    assert(wwpkg_get_last_error(NULL) == WWPKG_ERR_CORRUPT);
    unlink(empty_path);

    /* 5. Streaming confined wwpkg_extract_all: nested mkdir -p + hardlink & FIFO immunity */
    char nested_dest[256], victim_path[256], hardlink_dest[256], fifo_dest[256];
    snprintf(nested_dest, sizeof(nested_dest), "%s/nested/a/b", base_tmp);
    snprintf(victim_path, sizeof(victim_path), "%s/external_victim.txt", base_tmp);
    const char *orig_victim = "IMMUTABLE_HOST_SECRET";
    write_file_bytes(victim_path, orig_victim, strlen(orig_victim));

    char parent_nested[256], parent_a[256];
    snprintf(parent_nested, sizeof(parent_nested), "%s/nested", base_tmp);
    snprintf(parent_a, sizeof(parent_a), "%s/nested/a", base_tmp);
    mkdir(parent_nested, 0700);
    mkdir(parent_a, 0700);
    mkdir(nested_dest, 0700);

    snprintf(hardlink_dest, sizeof(hardlink_dest), "%s/shader.frag", nested_dest);
    snprintf(fifo_dest, sizeof(fifo_dest), "%s/preview.webp", nested_dest);
    assert(link(victim_path, hardlink_dest) == 0);
    assert(mkfifo(fifo_dest, 0600) == 0);

    wwpkg_package_t *stream_pkg = wwpkg_open_metadata_only(pkg1_path);
    assert(stream_pkg != NULL);
    assert(wwpkg_extract_all(stream_pkg, nested_dest));
    wwpkg_close(stream_pkg);

    /* External hardlink target must remain untouched; extracted preview must be a regular file */
    size_t vlen = 0;
    uint8_t *vbytes = read_file_bytes(victim_path, &vlen);
    assert(vlen == strlen(orig_victim) && memcmp(vbytes, orig_victim, vlen) == 0);
    free(vbytes);

    struct stat pst;
    assert(lstat(fifo_dest, &pst) == 0 && S_ISREG(pst.st_mode));

    char mf_dest[256], sig_dest[256];
    snprintf(mf_dest, sizeof(mf_dest), "%s/manifest.json", nested_dest);
    snprintf(sig_dest, sizeof(sig_dest), "%s/signature.sig", nested_dest);
    assert(unlink(hardlink_dest) == 0);
    assert(unlink(fifo_dest) == 0);
    assert(unlink(mf_dest) == 0);
    assert(unlink(sig_dest) == 0);
    assert(rmdir(nested_dest) == 0);
    assert(rmdir(parent_a) == 0);
    assert(rmdir(parent_nested) == 0);
    assert(unlink(victim_path) == 0);

    free(raw1);
    free(raw2);
    free(big_prev);
    unlink(p1_frag);
    unlink(p1_prev);
    unlink(p2_frag);
    unlink(p2_prev);
    rmdir(src1);
    rmdir(src2);
    unlink(pkg1_path);
    unlink(pkg2_path);

    printf("  -> Frame 0 retention, TOCTOU defense, skippable frame & confined extract passed.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  .wwpkg Cryptographic, Merkle, JCS & Dual-Frame Suite    \n");
    printf("==========================================================\n");

    char base_tmp[128];
    snprintf(base_tmp, sizeof(base_tmp), "/tmp/test_wwpkg_crypto_%d", (int)getpid());
    mkdir(base_tmp, 0700);

    uint8_t pub_key[32] = {0};
    uint8_t priv_key[64] = {0};
    wwpkg_generate_keypair(pub_key, priv_key);

    test_sha256_and_shani();
    test_ed25519_seed_and_malleability();
    test_seed_only_package_signing(base_tmp);
    test_merkle_path_swap_rejection(base_tmp, priv_key);
    test_tar_hardening_and_symlink_rejection(base_tmp, priv_key);
    test_rfc8785_jcs_hardening_and_locale(base_tmp, priv_key);
    test_trust_store_and_notary(base_tmp);
    test_dual_frame_metadata_fastpath_and_extract(base_tmp, priv_key);
    test_cold_open_and_1bit_tamper(base_tmp, priv_key);
    test_promotion_retention_toctou_and_confinement(base_tmp, priv_key);

    rmdir(base_tmp);

    printf("==========================================================\n");
    printf("  >>> ALL 10/10 .WWPKG CRYPTOGRAPHIC SUITES PASSED (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
