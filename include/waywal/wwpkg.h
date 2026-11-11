// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WWPKG_H
#define WAYWAL_WWPKG_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WWPKG_MAGIC_ZSTD          0xFD2FB528u
#define WWPKG_MAGIC_SKIPPABLE_ENV 0x184D2A50u
#define WWPKG_OUTER_ENV_PAYLOAD   168u
#define WWPKG_FRAME0_MAX_BYTES    (64u * 1024u)
#define WWPKG_MAX_ARCHIVE_BYTES   (512ULL * 1024ULL * 1024ULL)
#define WWPKG_MAX_FILE_COUNT      4096u
#define WWPKG_MAX_SLUG_LEN        128u
#define WWPKG_MAX_NAME_LEN        256u
#define WWPKG_MAX_PATH_LEN        1024u
#define WWPKG_MAX_USER_PROPS      128u
#define WWPKG_MAX_TAGS            16u
#define WWPKG_PUBKEY_BYTES        32u
#define WWPKG_SIGNATURE_BYTES     64u
#define WWPKG_MERKLE_ROOT_BYTES   32u

typedef enum {
    WWPKG_TYPE_UNKNOWN = 0,
    WWPKG_TYPE_SHADER_FRAGMENT = 1,
    WWPKG_TYPE_SHADER_COMPUTE = 2,
    WWPKG_TYPE_SCENE_2D = 3,
    WWPKG_TYPE_SCENE_PUPPET = 4,
    WWPKG_TYPE_SCENE_PARTICLES = 5,
    WWPKG_TYPE_VIDEO_STREAM = 6,
} wwpkg_type_t;

typedef enum {
    WWPKG_OK = 0,
    WWPKG_ERR_INVALID_PARAM = -1,
    WWPKG_ERR_IO = -2,
    WWPKG_ERR_CORRUPT = -3,
    WWPKG_ERR_DECOMPRESS = -4,
    WWPKG_ERR_BAD_MANIFEST = -5,
    WWPKG_ERR_BAD_SIGNATURE = -6,
    WWPKG_ERR_UNTRUSTED = -7,
    WWPKG_ERR_CAPABILITY = -8,
    WWPKG_ERR_PATH_TRAVERSAL = -9,
} wwpkg_error_t;

typedef enum {
    WWPKG_TRUST_INVALID = 0,
    WWPKG_TRUST_UNTRUSTED = 1,
    WWPKG_TRUST_AUTHOR_TRUSTED = 2,
    WWPKG_TRUST_NOTARIZED = 3,
} wwpkg_trust_level_t;

typedef struct {
    bool compute_shaders;
    bool audio_spectrum;
    bool audio_playback;
    bool scripting;
    bool foreign_toplevel;
    bool network;    /* Must be false */
    bool filesystem; /* Must be false */
} wwpkg_capabilities_t;

typedef struct {
    uint32_t max_vram_mb;
    uint32_t max_ram_mb;
    uint32_t target_fps;
    uint32_t max_compute_workgroups[3];
} wwpkg_resource_budgets_t;

typedef struct {
    char schema_version[16];
    char id[40]; /* UUID string */
    char slug[WWPKG_MAX_SLUG_LEN];
    char name[WWPKG_MAX_NAME_LEN];
    char version[32]; /* SemVer */
    char author_name[64];
    char author_email[128];
    uint8_t author_pubkey[WWPKG_PUBKEY_BYTES];
    char license[64];
    char description[4096];
    char tags[WWPKG_MAX_TAGS][32];
    uint32_t tag_count;

    wwpkg_type_t type;
    char entrypoint_pipeline[WWPKG_MAX_PATH_LEN];
    char entrypoint_preview[WWPKG_MAX_PATH_LEN];
    char entrypoint_audio[WWPKG_MAX_PATH_LEN];

    char min_wwal_version[32];
    uint32_t abi_version;
    char gl_profile[16];

    wwpkg_capabilities_t capabilities;
    wwpkg_resource_budgets_t budgets;

    uint32_t file_count;
    uint64_t total_bytes;
    bool has_preview_sha256;
    uint8_t preview_sha256[32];
} wwpkg_manifest_t;

typedef struct {
    uint32_t version;
    uint8_t _pad1[4]; /* Explicit padding before 64-bit timestamp for 8-byte alignment */
    uint64_t timestamp;
    uint8_t author_pubkey[WWPKG_PUBKEY_BYTES];    /* 32 bytes */
    uint8_t signature[WWPKG_SIGNATURE_BYTES];     /* 64 bytes */
    uint8_t merkle_root[WWPKG_MERKLE_ROOT_BYTES]; /* 32 bytes */
    uint8_t has_notary_countersign;               /* 1 byte boolean flag */
    uint8_t _pad2[7];                             /* Explicit trailing padding to 8-byte boundary */
    uint8_t notary_signature[WWPKG_SIGNATURE_BYTES]; /* 64 bytes */
} wwpkg_signature_t;
static_assert(sizeof(wwpkg_signature_t) == 216, "Must be 216 bytes with zero hidden padding");

/* Zstandard RFC 8878 skippable frame (0x184D2A50) carrying pre-decompression Ed25519 envelope */
typedef struct {
    uint32_t magic_le;
    uint32_t frame_size_le;
    uint8_t author_pubkey[WWPKG_PUBKEY_BYTES];
    uint8_t manifest_sha256[32];
    uint8_t merkle_root[WWPKG_MERKLE_ROOT_BYTES];
    uint64_t timestamp_le;
    uint8_t signature[WWPKG_SIGNATURE_BYTES];
} __attribute__((packed)) wwpkg_outer_envelope_t;
static_assert(sizeof(wwpkg_outer_envelope_t) == 176,
              "Outer skippable auth envelope must be 176 bytes");

typedef struct wwpkg_package wwpkg_package_t;

/* Opens and inspects a .wwpkg archive from disk, file descriptor, or memory */
wwpkg_package_t *wwpkg_open(const char *pkg_path);
wwpkg_package_t *wwpkg_open_metadata_only(const char *pkg_path);
wwpkg_package_t *wwpkg_open_fd(int fd);
wwpkg_package_t *wwpkg_open_memory(const uint8_t *data, size_t size);
void wwpkg_close(wwpkg_package_t *pkg);

const wwpkg_manifest_t *wwpkg_get_manifest(const wwpkg_package_t *pkg);
const wwpkg_signature_t *wwpkg_get_signature(const wwpkg_package_t *pkg);

/* Verifies cryptographic signature, Merkle root integrity, and optional trust store membership */
bool wwpkg_verify_signature(const wwpkg_package_t *pkg, bool *out_is_trusted);
wwpkg_trust_level_t wwpkg_verify_trust(const wwpkg_package_t *pkg);
wwpkg_trust_level_t wwpkg_verify_trust_with_store(const wwpkg_package_t *pkg,
                                                  const char *trusted_keys_path);
bool wwpkg_notarize_package(const char *pkg_path, const uint8_t notary_sk[64]);

/* Reads an asset blob directly from the package or extracts all verified entries safely */
const uint8_t *wwpkg_read_asset(wwpkg_package_t *pkg, const char *rel_path, size_t *out_size);
bool wwpkg_extract_all(wwpkg_package_t *pkg, const char *dest_dir);

/* Compiles and signs a directory of assets into a dual-frame .wwpkg container */
bool wwpkg_build_package(const char *source_dir, const wwpkg_manifest_t *manifest,
                         const uint8_t priv_key[64], const char *out_pkg_path);

/* Helper function to generate an Ed25519 keypair */
void wwpkg_generate_keypair(uint8_t pub[32], uint8_t priv[64]);

/* Ed25519 and SHA-256 primitives */
void wwpkg_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
int wwpkg_ed25519_keypair(uint8_t pk[32], uint8_t sk[64]);
int wwpkg_ed25519_seed_keypair(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]);
int wwpkg_ed25519_sign(uint8_t sig[64], const uint8_t *m, size_t mlen, const uint8_t sk[64]);
int wwpkg_ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen,
                         const uint8_t pk[32]);

/* Canonical JSON (RFC 8785 JCS) helpers */
char *wwpkg_canonicalize_json(const char *json_src, size_t len, size_t *out_len);
char *wwpkg_jcs_canonicalize(const char *json_src, size_t len, size_t *out_len);

/* Last error inspection helper (returns thread-local error when pkg is null) */
int wwpkg_get_last_error(const wwpkg_package_t *pkg);
const char *wwpkg_error_str(int err);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WWPKG_H */
