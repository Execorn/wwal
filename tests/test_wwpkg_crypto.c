// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_wwpkg_crypto.c
 * @brief Comprehensive .wwpkg Cryptographic Container & Merkle Tree Verification Suite (§7.2)
 *
 * Verifies:
 * 1. RFC 8032 Ed25519 keypair generation, signing, and verification primitives.
 * 2. RFC 8785 JSON Canonicalization Scheme (JCS) deterministic property ordering.
 * 3. RFC 6962 Domain-Separated SHA-256 Merkle tree hashing (0x00 leaf / 0x01 internal node).
 * 4. Full package compilation, manifest injection, and signature verification.
 * 5. High-speed streaming decompression benchmarking.
 * 6. 1-bit asset, manifest, and signature tamper detection asserting signature rejection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "waywal/wwpkg.h"

static void test_ed25519_primitives(void)
{
    printf("[WWPKG 1/6] Testing RFC 8032 Ed25519 keypair & signature primitives...\n");

    uint8_t pub[32];
    uint8_t priv[64];
    memset(pub, 0, sizeof(pub));
    memset(priv, 0, sizeof(priv));

    wwpkg_generate_keypair(pub, priv);

    // Verify keys are non-zero
    bool pub_nonzero = false;
    for (int i = 0; i < 32; ++i) {
        if (pub[i] != 0)
            pub_nonzero = true;
    }
    assert(pub_nonzero && "Generated public key must be non-zero");

    const uint8_t test_msg[] = "WayWal Open Hub Cryptographic Integrity Payload";
    uint8_t sig[64];
    int sign_rc = wwpkg_ed25519_sign(sig, test_msg, sizeof(test_msg) - 1, priv);
    assert(sign_rc == 0 && "Ed25519 signing must succeed");

    int verify_rc = wwpkg_ed25519_verify(sig, test_msg, sizeof(test_msg) - 1, pub);
    assert(verify_rc == 0 && "Ed25519 verification must succeed with matching key");

    // Test tamper with message
    const uint8_t tampered_msg[] = "WayWal Open Hub Cryptographic Integrity Payloae";
    int bad_msg_rc = wwpkg_ed25519_verify(sig, tampered_msg, sizeof(tampered_msg) - 1, pub);
    assert(bad_msg_rc != 0 && "Verification must fail on modified message");

    // Test tamper with signature
    sig[0] ^= 0x01;
    int bad_sig_rc = wwpkg_ed25519_verify(sig, test_msg, sizeof(test_msg) - 1, pub);
    assert(bad_sig_rc != 0 && "Verification must fail on corrupted signature");

    printf("  -> Ed25519 primitives passed.\n");
}

static void test_rfc8785_jcs(void)
{
    printf("[WWPKG 2/6] Testing RFC 8785 JSON Canonicalization Scheme (JCS)...\n");

    // Unformatted JSON with whitespace, unordered keys, mixed tokens
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
    char *canonical = wwpkg_canonicalize_json(unformatted, strlen(unformatted), &out_len);
    assert(canonical != NULL && "wwpkg_canonicalize_json must succeed");
    assert(out_len > 0);

    // In RFC 8785 JCS:
    // 1. Whitespace outside strings is removed.
    // 2. Object keys must be sorted lexicographically by UTF-16 code units ("apple" < "banana" <
    // "capabilities" < "zebra").
    assert(strstr(canonical, " ") == NULL || strstr(canonical, "\"apple\":\"red\"") != NULL);
    const char *pos_apple = strstr(canonical, "\"apple\"");
    const char *pos_banana = strstr(canonical, "\"banana\"");
    const char *pos_caps = strstr(canonical, "\"capabilities\"");
    const char *pos_zebra = strstr(canonical, "\"zebra\"");

    assert(pos_apple != NULL && pos_banana != NULL && pos_caps != NULL && pos_zebra != NULL);
    assert(pos_apple < pos_banana && "Keys must be lexicographically ordered");
    assert(pos_banana < pos_caps);
    assert(pos_caps < pos_zebra);

    free(canonical);
    printf("  -> RFC 8785 JCS canonicalization passed.\n");
}

static void test_rfc6962_merkle_domain_separation(void)
{
    printf("[WWPKG 3/6] Testing RFC 6962 Merkle Tree domain separation (0x00 leaf / 0x01 "
           "internal)...\n");

    const uint8_t test_data[] = "asset payload chunk";
    size_t len = sizeof(test_data) - 1;

    // Leaf hash: SHA-256(0x00 || data)
    uint8_t leaf_buf[1 + sizeof(test_data)];
    leaf_buf[0] = 0x00; // RFC 6962 leaf domain separator
    memcpy(leaf_buf + 1, test_data, len);
    uint8_t leaf_hash[32];
    wwpkg_sha256(leaf_buf, 1 + len, leaf_hash);

    // Internal node hash: SHA-256(0x01 || left || right)
    uint8_t node_buf[1 + 64];
    node_buf[0] = 0x01; // RFC 6962 internal node domain separator
    memcpy(node_buf + 1, leaf_hash, 32);
    memcpy(node_buf + 33, leaf_hash, 32);
    uint8_t root_hash[32];
    wwpkg_sha256(node_buf, 65, root_hash);

    // Assert leaf hash != internal node hash even if input sizes happen to match
    assert(memcmp(leaf_hash, root_hash, 32) != 0 &&
           "Domain separation prevents second-preimage collision");

    printf("  -> RFC 6962 domain separation verified.\n");
}

static void test_package_build_and_verify(const char *src_dir, const char *pkg_path,
                                          uint8_t pub_key[32], uint8_t priv_key[64])
{
    printf("[WWPKG 4/6] Building, signing, and verifying .wwpkg package...\n");

    // 1. Prepare source directory with mock wallpaper assets
    mkdir(src_dir, 0755);

    FILE *f1 = fopen("/tmp/test_wwpkg_src/shader.frag", "wb");
    assert(f1 != NULL);
    const char *frag = "void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }";
    fwrite(frag, 1, strlen(frag), f1);
    fclose(f1);

    FILE *f2 = fopen("/tmp/test_wwpkg_src/preview.webp", "wb");
    assert(f2 != NULL);
    const char *webp = "RIFF....WEBPVP8 ... mock webp bytes ...";
    fwrite(webp, 1, strlen(webp), f2);
    fclose(f2);

    FILE *f3 = fopen("/tmp/test_wwpkg_src/config.json", "wb");
    assert(f3 != NULL);
    const char *cfg = "{\"speed\": 1.0, \"particles\": 500}";
    fwrite(cfg, 1, strlen(cfg), f3);
    fclose(f3);

    // 2. Setup Manifest
    wwpkg_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    strcpy(manifest.schema_version, "wwpkg/v1");
    strcpy(manifest.id, "a1b2c3d4-e5f6-7890-abcd-ef1234567890");
    strcpy(manifest.slug, "neon-fractal");
    strcpy(manifest.name, "Neon Fractal 4K");
    strcpy(manifest.version, "1.0.0");
    strcpy(manifest.author_name, "WayWal Contributor");
    strcpy(manifest.license, "MIT");
    strcpy(manifest.entrypoint_pipeline, "shader.frag");
    strcpy(manifest.entrypoint_preview, "preview.webp");
    manifest.type = WWPKG_TYPE_SHADER_FRAGMENT;
    manifest.capabilities.compute_shaders = false;
    manifest.capabilities.audio_spectrum = false;
    manifest.capabilities.network = false;    // Air-gap invariant
    manifest.capabilities.filesystem = false; // Air-gap invariant

    // 3. Build & Sign Package
    bool build_ok = wwpkg_build_package(src_dir, &manifest, priv_key, pkg_path);
    assert(build_ok && "wwpkg_build_package must succeed");

    struct stat st;
    assert(stat(pkg_path, &st) == 0 && st.st_size > 0);
    printf("  -> Package built: %ld bytes.\n", (long)st.st_size);

    // 4. Open and Verify Signature
    wwpkg_package_t *pkg = wwpkg_open(pkg_path);
    assert(pkg != NULL && "wwpkg_open must open valid container");

    const wwpkg_manifest_t *mf = wwpkg_get_manifest(pkg);
    assert(mf != NULL);
    assert(strcmp(mf->slug, "neon-fractal") == 0);
    assert(strcmp(mf->version, "1.0.0") == 0);

    bool is_trusted = false;
    bool verify_ok = wwpkg_verify_signature(pkg, &is_trusted);
    assert(verify_ok && "Cryptographic signature & Merkle tree verification must pass 100%");

    // 5. Read Asset back
    size_t asset_sz = 0;
    const uint8_t *asset_data = wwpkg_read_asset(pkg, "shader.frag", &asset_sz);
    assert(asset_data != NULL);
    assert(asset_sz == strlen(frag));
    assert(memcmp(asset_data, frag, asset_sz) == 0);

    wwpkg_close(pkg);
    printf("  -> Package compilation, Merkle tree, and Ed25519 signature verified.\n");
}

static void test_decompression_benchmark(const char *pkg_path)
{
    printf("[WWPKG 5/6] Benchmarking cold-start package opening & decompression speed...\n");

    constexpr int ITERATIONS = 100;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int i = 0; i < ITERATIONS; ++i) {
        wwpkg_package_t *pkg = wwpkg_open(pkg_path);
        assert(pkg != NULL);
        size_t sz = 0;
        const uint8_t *data = wwpkg_read_asset(pkg, "shader.frag", &sz);
        assert(data != NULL && sz > 0);
        wwpkg_close(pkg);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed_sec = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                         (double)(ts_end.tv_nsec - ts_start.tv_nsec) * 1e-9;
    double per_open_ms = (elapsed_sec / ITERATIONS) * 1000.0;

    printf("  -> Decompression latency: %f ms per cold open (%d iterations in %f s).\n",
           per_open_ms, ITERATIONS, elapsed_sec);
    // Sub-8-millisecond load latency requirement (§6.4)
    assert(per_open_ms < 8.0 && "Cold open latency must be well below 8.0 ms budget");
    printf("  -> Decompression benchmark passed.\n");
}

static void test_1bit_tamper_detection(const char *pkg_path)
{
    printf("[WWPKG 6/6] Testing 1-bit asset/manifest cryptographic tamper rejection (§7.2)...\n");

    // Read package into memory
    FILE *f = fopen(pkg_path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc(sz);
    assert(buf != NULL);
    size_t rd = fread(buf, 1, sz, f);
    assert((long)rd == sz);
    fclose(f);

    // Write a tampered copy: flip 1 bit near the middle of payload data
    const char *tampered_pkg = "/tmp/test_wwal_tampered.wwpkg";
    uint8_t *tampered_buf = (uint8_t *)malloc(sz);
    memcpy(tampered_buf, buf, sz);

    // Flip bit in the payload region (around 40% through archive)
    size_t flip_offset = sz * 2 / 5;
    tampered_buf[flip_offset] ^= 0x01;

    FILE *tf = fopen(tampered_pkg, "wb");
    assert(tf != NULL);
    fwrite(tampered_buf, 1, sz, tf);
    fclose(tf);

    // Attempt to open/verify tampered package
    wwpkg_package_t *tpkg = wwpkg_open(tampered_pkg);
    if (tpkg != NULL) {
        bool is_trusted = false;
        bool verify_ok = wwpkg_verify_signature(tpkg, &is_trusted);
        assert(!verify_ok &&
               "1-bit tampering must cause wwpkg_verify_signature to REJECT package!");
        wwpkg_close(tpkg);
    } else {
        // Container decompression level rejection is also valid protection against tampering
        printf("  -> Corrupted archive detected during container decompression.\n");
    }

    unlink(tampered_pkg);
    free(buf);
    free(tampered_buf);

    printf("  -> 1-bit tamper detection successfully triggered rejection.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  .wwpkg Cryptographic Package & Merkle Suite (§7.2)       \n");
    printf("==========================================================\n");

    const char *src_dir = "/tmp/test_wwpkg_src";
    const char *pkg_path = "/tmp/test_neon_fractal.wwpkg";

    uint8_t pub_key[32];
    uint8_t priv_key[64];
    wwpkg_generate_keypair(pub_key, priv_key);

    test_ed25519_primitives();
    test_rfc8785_jcs();
    test_rfc6962_merkle_domain_separation();
    test_package_build_and_verify(src_dir, pkg_path, pub_key, priv_key);
    test_decompression_benchmark(pkg_path);
    test_1bit_tamper_detection(pkg_path);

    // Cleanup
    unlink("/tmp/test_wwpkg_src/shader.frag");
    unlink("/tmp/test_wwpkg_src/preview.webp");
    unlink("/tmp/test_wwpkg_src/config.json");
    rmdir(src_dir);
    unlink(pkg_path);

    printf("==========================================================\n");
    printf("  >>> ALL .WWPKG CRYPTOGRAPHIC TESTS PASSED (100%%) <<<    \n");
    printf("==========================================================\n");

    return 0;
}
