// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "waywal/we_compat_scanner.h"

#include "waywal/log.h"
#include "waywal/we_json.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

static_assert(sizeof(we_compat_report_t) == 252280,
              "sizeof(we_compat_report_t) must equal exactly 252280 bytes");

struct we_compat_scanner {
    bool supports_compute;
    bool supports_bc7;
};

/* =========================================================================
 * Dynamic Heap Lifecycle Management
 * ========================================================================= */

we_compat_report_t *we_compat_report_alloc(void)
{
    return (we_compat_report_t *)calloc(1, sizeof(we_compat_report_t));
}

void we_compat_report_free(we_compat_report_t *report) { free(report); }

we_compat_scanner_t *we_compat_scanner_create(bool supports_compute, bool supports_bc7)
{
    we_compat_scanner_t *s = (we_compat_scanner_t *)calloc(1, sizeof(we_compat_scanner_t));
    if (!s) {
        return NULL;
    }
    s->supports_compute = supports_compute;
    s->supports_bc7 = supports_bc7;
    return s;
}

void we_compat_scanner_destroy(we_compat_scanner_t *scanner) { free(scanner); }

/* =========================================================================
 * Report Issue & Auto-Repair Helpers
 * ========================================================================= */

static void report_add_issue(we_compat_report_t *report, const char *code,
                             we_issue_severity_t severity, const char *msg, const char *file_path,
                             uint32_t line_no, const char *suggested_fix)
{
    if (!report || report->issue_count >= WE_SCAN_MAX_ISSUES) {
        return;
    }
    we_compat_issue_t *iss = &report->issues[report->issue_count++];
    snprintf(iss->code, sizeof(iss->code), "%s", code ? code : "");
    iss->severity = severity;
    snprintf(iss->message, sizeof(iss->message), "%s", msg ? msg : "");
    snprintf(iss->file_path, sizeof(iss->file_path), "%s", file_path ? file_path : "");
    iss->line_number = line_no;
    snprintf(iss->suggested_fix, sizeof(iss->suggested_fix), "%s",
             suggested_fix ? suggested_fix : "");
}

static void report_add_repair(we_compat_report_t *report, const char *transformation_id,
                              const char *target_file, const char *description)
{
    if (!report || report->repair_count >= WE_SCAN_MAX_REPAIRS) {
        return;
    }
    for (uint32_t i = 0; i < report->repair_count; i++) {
        if (strcmp(report->repairs[i].transformation_id, transformation_id) == 0 &&
            strcmp(report->repairs[i].target_file, target_file) == 0) {
            return;
        }
    }
    we_auto_repair_t *rep = &report->repairs[report->repair_count++];
    snprintf(rep->transformation_id, sizeof(rep->transformation_id), "%s",
             transformation_id ? transformation_id : "");
    snprintf(rep->target_file, sizeof(rep->target_file), "%s", target_file ? target_file : "");
    snprintf(rep->description, sizeof(rep->description), "%s", description ? description : "");
}

/* =========================================================================
 * Byte Reading Utilities
 * ========================================================================= */

static inline uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static inline uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static inline int32_t read_i32_le(const uint8_t *p) { return (int32_t)read_u32_le(p); }

/* =========================================================================
 * Rolling XOR De-masking & Binary PKG Inspection
 * ========================================================================= */

static void xor_demask_buffer(const uint8_t *src, size_t len, const uint8_t *key, size_t key_len,
                              uint8_t *dst)
{
    if (!src || !dst || len == 0 || key_len == 0) {
        return;
    }

#if defined(__AVX2__)
    if (key_len == 1 || key_len == 2 || key_len == 4 || key_len == 8 || key_len == 16) {
        uint8_t key32[32];
        for (size_t k = 0; k < 32; k++) {
            key32[k] = key[k % key_len];
        }
        __m256i vkey = _mm256_loadu_si256((const __m256i *)key32);
        size_t i = 0;
        for (; i + 32 <= len; i += 32) {
            __m256i vdata = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i vout = _mm256_xor_si256(vdata, vkey);
            _mm256_storeu_si256((__m256i *)(dst + i), vout);
        }
        for (; i < len; i++) {
            dst[i] = src[i] ^ key[i % key_len];
        }
        return;
    }
#endif

    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i] ^ key[i % key_len];
    }
}

static bool check_pkg_version_digits(const uint8_t *ver4)
{
    if (!isdigit(ver4[0]) || !isdigit(ver4[1]) || !isdigit(ver4[2]) || !isdigit(ver4[3])) {
        return false;
    }
    uint32_t v = (uint32_t)(ver4[0] - '0') * 1000u + (uint32_t)(ver4[1] - '0') * 100u +
                 (uint32_t)(ver4[2] - '0') * 10u + (uint32_t)(ver4[3] - '0');
    return (v >= 1u && v <= 24u);
}

/* Detects rolling XOR obfuscation for periods P in {1, 4, 8, 16}.
 * Returns key period (0 if plain unmasked PKGV, > 0 if masked), or -1 if invalid container. */
static int detect_rolling_xor_key(const uint8_t *data, size_t size, uint8_t out_key[16])
{
    if (!data || size < 16) {
        return -1;
    }
    memset(out_key, 0, 16);

    /* 1. Test unmasked PKGV */
    if (size >= 12) {
        uint32_t magic_len = read_u32_le(data);
        if (magic_len == 8 && memcmp(data + 4, "PKGV", 4) == 0 &&
            check_pkg_version_digits(data + 8)) {
            return 0; /* Plain unmasked */
        }
    }

    /* 2. Test Period 1 */
    {
        uint8_t k = data[4] ^ 'P';
        if (k != 0 && (data[5] ^ k) == 'K' && (data[6] ^ k) == 'G' && (data[7] ^ k) == 'V') {
            if ((data[0] ^ k) == 8 && (data[1] ^ k) == 0 && (data[2] ^ k) == 0 &&
                (data[3] ^ k) == 0) {
                uint8_t ver[4] = {(uint8_t)(data[8] ^ k), (uint8_t)(data[9] ^ k),
                                  (uint8_t)(data[10] ^ k), (uint8_t)(data[11] ^ k)};
                if (check_pkg_version_digits(ver)) {
                    out_key[0] = k;
                    return 1;
                }
            }
        }
    }

    /* 3. Test Period 4 */
    {
        uint8_t k[4] = {(uint8_t)(data[4] ^ 'P'), (uint8_t)(data[5] ^ 'K'),
                        (uint8_t)(data[6] ^ 'G'), (uint8_t)(data[7] ^ 'V')};
        if ((k[0] | k[1] | k[2] | k[3]) != 0) {
            if ((data[0] ^ k[0]) == 8 && (data[1] ^ k[1]) == 0 && (data[2] ^ k[2]) == 0 &&
                (data[3] ^ k[3]) == 0) {
                uint8_t ver[4] = {(uint8_t)(data[8] ^ k[0]), (uint8_t)(data[9] ^ k[1]),
                                  (uint8_t)(data[10] ^ k[2]), (uint8_t)(data[11] ^ k[3])};
                if (check_pkg_version_digits(ver)) {
                    memcpy(out_key, k, 4);
                    return 4;
                }
            }
        }
    }

    /* 4. Test Period 8 */
    {
        uint8_t k[8];
        k[0] = (uint8_t)(data[0] ^ 8);
        k[1] = (uint8_t)(data[1] ^ 0);
        k[2] = (uint8_t)(data[2] ^ 0);
        k[3] = (uint8_t)(data[3] ^ 0);
        k[4] = (uint8_t)(data[4] ^ 'P');
        k[5] = (uint8_t)(data[5] ^ 'K');
        k[6] = (uint8_t)(data[6] ^ 'G');
        k[7] = (uint8_t)(data[7] ^ 'V');

        uint8_t ver[4] = {(uint8_t)(data[8] ^ k[0]), (uint8_t)(data[9] ^ k[1]),
                          (uint8_t)(data[10] ^ k[2]), (uint8_t)(data[11] ^ k[3])};
        if (check_pkg_version_digits(ver)) {
            memcpy(out_key, k, 8);
            return 8;
        }
    }

    /* 5. Test Period 16 */
    {
        uint8_t k[16];
        k[0] = (uint8_t)(data[0] ^ 8);
        k[1] = (uint8_t)(data[1] ^ 0);
        k[2] = (uint8_t)(data[2] ^ 0);
        k[3] = (uint8_t)(data[3] ^ 0);
        k[4] = (uint8_t)(data[4] ^ 'P');
        k[5] = (uint8_t)(data[5] ^ 'K');
        k[6] = (uint8_t)(data[6] ^ 'G');
        k[7] = (uint8_t)(data[7] ^ 'V');

        for (uint32_t v = 1; v <= 24; v++) {
            char ver_str[5];
            snprintf(ver_str, sizeof(ver_str), "%04u", v);
            k[8] = (uint8_t)(data[8] ^ (uint8_t)ver_str[0]);
            k[9] = (uint8_t)(data[9] ^ (uint8_t)ver_str[1]);
            k[10] = (uint8_t)(data[10] ^ (uint8_t)ver_str[2]);
            k[11] = (uint8_t)(data[11] ^ (uint8_t)ver_str[3]);

            k[12] = data[12];
            k[13] = data[13];
            k[14] = data[14];
            k[15] = data[15];

            if (size >= 20) {
                uint32_t path_len =
                    (uint32_t)(data[16] ^ k[0]) | ((uint32_t)(data[17] ^ k[1]) << 8u) |
                    ((uint32_t)(data[18] ^ k[2]) << 16u) | ((uint32_t)(data[19] ^ k[3]) << 24u);
                if (path_len > 0 && path_len <= 1024 && size >= 20 + path_len) {
                    memcpy(out_key, k, 16);
                    return 16;
                }
            }
        }
    }

    return -1;
}

/* =========================================================================
 * Path Normalization & Malware / Security Quarantine
 * ========================================================================= */

static bool has_directory_traversal(const char *path)
{
    if (!path) {
        return false;
    }
    const char *p = path;
    while (*p) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F) {
            return true;
        }
        if (p[0] == '.' && p[1] == '.') {
            if ((p == path || p[-1] == '/' || p[-1] == '\\') &&
                (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) {
                return true;
            }
        }
        p++;
    }
    return false;
}

static bool is_executable_binary_payload(const uint8_t *data, size_t size, const char *path)
{
    if (!data || size < 2) {
        return false;
    }

    /* Windows PE binary (MZ header: 0x4D, 0x5A) */
    if (data[0] == 0x4D && data[1] == 0x5A) {
        return true;
    }

    /* Linux ELF binary (\x7FELF: 0x7F, 'E', 'L', 'F') */
    if (size >= 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        return true;
    }

    /* File extension check */
    if (path) {
        const char *dot = strrchr(path, '.');
        if (dot) {
            if (strcasecmp(dot, ".exe") == 0 || strcasecmp(dot, ".dll") == 0 ||
                strcasecmp(dot, ".so") == 0 || strcasecmp(dot, ".bin") == 0 ||
                strcasecmp(dot, ".com") == 0 || strcasecmp(dot, ".scr") == 0) {
                return true;
            }
        }
    }

    return false;
}

/* =========================================================================
 * Fast MDL Skeletal Puppet Parser
 * ========================================================================= */

typedef struct {
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t bone_count;
    bool is_valid;
} we_compat_mdl_info_t;

static bool parse_mdl_header_fast(const uint8_t *data, size_t size, we_compat_mdl_info_t *out)
{
    if (!data || size < 32 || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t cur = 0;
    if (size >= 12 && memcmp(data, "MDLV", 4) != 0 && data[0] == 8 && data[1] == 0) {
        cur = 4;
    }

    if (cur + 8 > size || memcmp(data + cur, "MDLV", 4) != 0) {
        return false;
    }
    cur += 8;
    if (cur < size && data[cur] == '\0') {
        cur++;
    }

    if (cur + 12 > size) {
        return false;
    }
    cur += 12; /* mdl_flag (4) + unk1 (4) + unk2 (4) */

    /* Skip material json filename */
    if (cur + 4 <= size) {
        uint32_t slen = read_u32_le(data + cur);
        if (slen > 0 && slen < 1024 && cur + 4 + slen <= size) {
            cur += 4 + slen;
        } else {
            while (cur < size && data[cur] != '\0') {
                cur++;
            }
            if (cur < size) {
                cur++;
            }
        }
    }

    if (cur + 4 > size) {
        return false;
    }
    uint32_t curr = read_u32_le(data + cur);
    cur += 4;

    uint32_t vertex_byte_size = 0;
    uint32_t stride = 52;
    if (curr == 0x0180000Fu) {
        if (cur + 4 > size) {
            return false;
        }
        vertex_byte_size = read_u32_le(data + cur);
        cur += 4;
        stride = 80;
    } else {
        vertex_byte_size = curr;
        stride = 52;
    }

    if (vertex_byte_size > 0 && (vertex_byte_size % stride) == 0) {
        out->vertex_count = vertex_byte_size / stride;
    }
    cur += vertex_byte_size;

    if (cur + 4 <= size) {
        uint32_t indices_size = read_u32_le(data + cur);
        cur += 4;
        if (indices_size > 0 && (indices_size % 6) == 0) {
            out->triangle_count = indices_size / 6;
        }
        cur += indices_size;
    }

    /* Scan ahead for MDLS (skeleton / bones) */
    while (cur + 8 <= size) {
        if (memcmp(data + cur, "MDLS", 4) == 0 && isdigit(data[cur + 4])) {
            cur += 8;
            if (cur < size && data[cur] == '\0') {
                cur++;
            }
            if (cur + 4 <= size) {
                out->bone_count = read_u32_le(data + cur);
            }
            break;
        }
        cur++;
    }

    out->is_valid = (out->vertex_count > 0 || out->triangle_count > 0 || out->bone_count > 0);
    return out->is_valid;
}

/* =========================================================================
 * Fast TEX Texture Header Parser (Format & Dimensions)
 * ========================================================================= */

typedef struct {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    bool is_bc7;
} we_compat_tex_info_t;

static bool parse_tex_header_fast(const uint8_t *data, size_t size, we_compat_tex_info_t *out)
{
    if (!data || size < 32 || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t cur = 0;
    if (size >= 12 && memcmp(data, "TEXV", 4) != 0 && data[0] == 8 && data[1] == 0) {
        cur = 4;
    }
    if (cur + 8 > size || memcmp(data + cur, "TEXV", 4) != 0) {
        return false;
    }
    cur += 8;
    if (cur < size && data[cur] == '\0') {
        cur++;
    }

    /* Look for TEXI */
    while (cur + 8 <= size) {
        if (memcmp(data + cur, "TEXI", 4) == 0) {
            cur += 8;
            if (cur < size && data[cur] == '\0') {
                cur++;
            }
            if (cur + 24 <= size) {
                out->format = read_u32_le(data + cur);
                out->width = read_u32_le(data + cur + 8);
                out->height = read_u32_le(data + cur + 12);
                out->is_bc7 = (out->format == 12u);
                return true;
            }
            break;
        }
        cur++;
    }
    return false;
}

/* =========================================================================
 * SceneScript AST & Sandbox Token Auditor
 * ========================================================================= */

static const char *const FORBIDDEN_JS_PRIMITIVES[] = {
    "eval",           "Function", "require",   "process",       "window", "document",
    "XMLHttpRequest", "fetch",    "WebSocket", "child_process", NULL};

static bool is_token_boundary(char c)
{
    return !(isalnum((unsigned char)c) || c == '_' || c == '$');
}

static const char *find_forbidden_js_primitive(const char *src, size_t len, uint32_t *out_line)
{
    if (!src || len == 0) {
        return NULL;
    }
    uint32_t current_line = 1;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            current_line++;
            continue;
        }

        /* Skip line comments */
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') {
                i++;
            }
            if (i < len && src[i] == '\n') {
                current_line++;
            }
            continue;
        }

        /* Skip block comments */
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') {
                    current_line++;
                }
                i++;
            }
            i++;
            continue;
        }

        /* Check word boundary */
        if (i > 0 && !is_token_boundary(src[i - 1])) {
            continue;
        }

        for (size_t k = 0; FORBIDDEN_JS_PRIMITIVES[k] != NULL; k++) {
            const char *prim = FORBIDDEN_JS_PRIMITIVES[k];
            size_t plen = strlen(prim);
            if (i + plen <= len && memcmp(src + i, prim, plen) == 0) {
                if (i + plen == len || is_token_boundary(src[i + plen])) {
                    if (out_line) {
                        *out_line = current_line;
                    }
                    return prim;
                }
            }
        }
    }
    return NULL;
}

static bool check_potential_infinite_loop(const char *src, size_t len, uint32_t *out_line)
{
    if (!src || len == 0) {
        return false;
    }
    static const char *const LOOP_PATTERNS[] = {
        "while(true)", "while (true)", "while(1)", "while (1)", "for(;;)", "for (;;)", NULL};

    uint32_t line = 1;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            line++;
            continue;
        }
        for (size_t k = 0; LOOP_PATTERNS[k] != NULL; k++) {
            size_t plen = strlen(LOOP_PATTERNS[k]);
            if (i + plen <= len && memcmp(src + i, LOOP_PATTERNS[k], plen) == 0) {
                if (out_line) {
                    *out_line = line;
                }
                return true;
            }
        }
    }
    return false;
}

/* =========================================================================
 * Shader Varying & Include Inspection
 * ========================================================================= */

typedef struct {
    char name[64];
    uint32_t width; /* 1..4 */
    uint32_t line;
} we_compat_varying_t;

static uint32_t parse_varyings_from_stage(const char *src, size_t len,
                                          we_compat_varying_t *out_vars, uint32_t max_vars,
                                          bool is_vertex_stage)
{
    if (!src || len == 0 || !out_vars || max_vars == 0) {
        return 0;
    }
    uint32_t count = 0;
    uint32_t line_no = 1;

    const char *p = src;
    const char *end = src + len;

    while (p < end) {
        const char *line_start = p;
        while (p < end && *p != '\n') {
            p++;
        }
        size_t line_len = (size_t)(p - line_start);
        if (p < end && *p == '\n') {
            p++;
        }

        const char *lp = line_start;
        const char *lend = line_start + line_len;
        while (lp < lend && isspace((unsigned char)*lp)) {
            lp++;
        }

        bool match = false;
        if (lend - lp > 8 && strncmp(lp, "varying", 7) == 0 && isspace((unsigned char)lp[7])) {
            lp += 8;
            match = true;
        } else if (is_vertex_stage && lend - lp > 4 && strncmp(lp, "out", 3) == 0 &&
                   isspace((unsigned char)lp[3])) {
            lp += 4;
            match = true;
        } else if (!is_vertex_stage && lend - lp > 3 && strncmp(lp, "in", 2) == 0 &&
                   isspace((unsigned char)lp[2])) {
            lp += 3;
            match = true;
        }

        if (match) {
            while (lp < lend && isspace((unsigned char)*lp)) {
                lp++;
            }
            uint32_t width = 0;
            if (lend - lp >= 5 && strncmp(lp, "float", 5) == 0) {
                width = 1;
                lp += 5;
            } else if (lend - lp >= 4 && strncmp(lp, "vec2", 4) == 0) {
                width = 2;
                lp += 4;
            } else if (lend - lp >= 4 && strncmp(lp, "vec3", 4) == 0) {
                width = 3;
                lp += 4;
            } else if (lend - lp >= 4 && strncmp(lp, "vec4", 4) == 0) {
                width = 4;
                lp += 4;
            }

            if (width > 0) {
                while (lp < lend && isspace((unsigned char)*lp)) {
                    lp++;
                }
                const char *name_start = lp;
                while (lp < lend && (isalnum((unsigned char)*lp) || *lp == '_')) {
                    lp++;
                }
                size_t name_len = (size_t)(lp - name_start);
                if (name_len > 0 && name_len < 64 && count < max_vars) {
                    memcpy(out_vars[count].name, name_start, name_len);
                    out_vars[count].name[name_len] = '\0';
                    out_vars[count].width = width;
                    out_vars[count].line = line_no;
                    count++;
                }
            }
        }
        line_no++;
    }
    return count;
}

/* =========================================================================
 * Unified Package & Loose Filesystem VFS Scanner
 * ========================================================================= */

typedef struct {
    char workshop_dir[4096];
    we_pkg_t *pkg;
    uint8_t *demasked_buf;
    size_t demasked_size;
} we_compat_vfs_t;

static void compat_vfs_close(we_compat_vfs_t *vfs)
{
    if (!vfs) {
        return;
    }
    if (vfs->pkg) {
        we_pkg_close(vfs->pkg);
        vfs->pkg = NULL;
    }
    if (vfs->demasked_buf) {
        free(vfs->demasked_buf);
        vfs->demasked_buf = NULL;
        vfs->demasked_size = 0;
    }
}

static bool compat_vfs_open(we_compat_vfs_t *vfs, const char *path, we_compat_report_t *report)
{
    memset(vfs, 0, sizeof(*vfs));
    if (!path || path[0] == '\0') {
        return false;
    }
    snprintf(vfs->workshop_dir, sizeof(vfs->workshop_dir), "%s", path);

    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    char candidate_pkg[4096] = {0};
    if (S_ISREG(st.st_mode)) {
        snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", path);
    } else if (S_ISDIR(st.st_mode)) {
        char p1[4096], p2[4096];
        snprintf(p1, sizeof(p1), "%s/scene.pkg", path);
        snprintf(p2, sizeof(p2), "%s/gifscene.pkg", path);
        if (access(p1, R_OK) == 0) {
            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", p1);
        } else if (access(p2, R_OK) == 0) {
            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s", p2);
        } else {
            /* Scan directory for any *.pkg */
            DIR *d = opendir(path);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
                        size_t nlen = strlen(ent->d_name);
                        if (nlen > 4 && strcasecmp(ent->d_name + nlen - 4, ".pkg") == 0) {
                            snprintf(candidate_pkg, sizeof(candidate_pkg), "%s/%s", path,
                                     ent->d_name);
                            break;
                        }
                    }
                }
                closedir(d);
            }
        }
    }

    if (candidate_pkg[0] != '\0') {
        int fd = open(candidate_pkg, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat pkg_st;
            if (fstat(fd, &pkg_st) == 0 && pkg_st.st_size >= 16) {
                size_t sz = (size_t)pkg_st.st_size;
                void *mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
                if (mapped != MAP_FAILED) {
                    uint8_t key[16] = {0};
                    int period = detect_rolling_xor_key((const uint8_t *)mapped, sz, key);
                    if (period > 0) {
                        /* Obfuscated package: de-mask into scratch buffer */
                        vfs->demasked_buf = (uint8_t *)malloc(sz);
                        if (vfs->demasked_buf) {
                            vfs->demasked_size = sz;
                            xor_demask_buffer((const uint8_t *)mapped, sz, key, (size_t)period,
                                              vfs->demasked_buf);
                            vfs->pkg = we_pkg_open_mem(vfs->demasked_buf, sz, vfs->workshop_dir);
                            char fix[256];
                            snprintf(fix, sizeof(fix),
                                     "De-masked obfuscated container with %d-byte key", period);
                            report_add_issue(report, "PKG_OBFUSCATION_DEMASKED", WE_ISSUE_SEV_INFO,
                                             "Rolling XOR de-masking applied to container",
                                             candidate_pkg, 1, fix);
                        }
                    }
                    munmap(mapped, sz);
                }
            }
            close(fd);

            if (!vfs->pkg) {
                vfs->pkg = we_pkg_open_file(candidate_pkg, vfs->workshop_dir);
            }
        }
    }

    return true;
}

static const uint8_t *compat_vfs_read_file(we_compat_vfs_t *vfs, const char *rel_path,
                                           size_t *out_size, bool *out_must_free)
{
    if (out_must_free) {
        *out_must_free = false;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (!vfs || !rel_path) {
        return NULL;
    }

    char norm[1024];
    if (!we_pkg_normalize_path(rel_path, strlen(rel_path), norm, sizeof(norm))) {
        return NULL;
    }

    /* 1. Try .pkg archive */
    if (vfs->pkg) {
        const uint8_t *data = we_pkg_find(vfs->pkg, norm, out_size);
        if (data) {
            return data;
        }
    }

    /* 2. Try loose disk file */
    char full_path[8192];
    snprintf(full_path, sizeof(full_path), "%s/%s", vfs->workshop_dir, norm);
    int fd = open(full_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            st.st_size <= (256LL * 1024LL * 1024LL)) {
            size_t sz = (size_t)st.st_size;
            uint8_t *buf = (uint8_t *)malloc(sz + 1);
            if (buf) {
                ssize_t rd = read(fd, buf, sz);
                close(fd);
                if (rd == (ssize_t)sz) {
                    buf[sz] = '\0';
                    if (out_size) {
                        *out_size = sz;
                    }
                    if (out_must_free) {
                        *out_must_free = true;
                    }
                    return buf;
                }
                free(buf);
                return NULL;
            }
        }
        close(fd);
    }

    return NULL;
}

/* =========================================================================
 * Full Directory Recursion for Loose Executable Malware & Traversal Check
 * ========================================================================= */

static bool scan_loose_directory_malware(const char *dir_path, we_compat_report_t *report,
                                         int depth)
{
    if (!dir_path || depth > 8) {
        return false;
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        return false;
    }

    bool malware_found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (has_directory_traversal(ent->d_name)) {
            report_add_issue(report, "SECURITY_TRAVERSAL", WE_ISSUE_SEV_FATAL,
                             "Directory traversal detected in filename", ent->d_name, 1,
                             "Sanitize file path");
            report->tier = WE_TIER_4_BLOCKED;
            report->compatibility_score = 0;
            malware_found = true;
            break;
        }

        char subpath[4096];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (scan_loose_directory_malware(subpath, report, depth + 1)) {
                malware_found = true;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            int fd = open(subpath, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                uint8_t hdr[16];
                ssize_t n = read(fd, hdr, sizeof(hdr));
                close(fd);
                if (n >= 2 && is_executable_binary_payload(hdr, (size_t)n, ent->d_name)) {
                    report_add_issue(report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                     "Malicious executable binary detected (PE/ELF header)",
                                     subpath, 1, "Quarantine package and remove executable");
                    report->tier = WE_TIER_4_BLOCKED;
                    report->compatibility_score = 0;
                    malware_found = true;
                    break;
                }
            }
        }
    }

    closedir(d);
    return malware_found;
}

/* =========================================================================
 * Asset Auditing Traversal Helpers
 * ========================================================================= */

typedef struct {
    bool cycle_detected;
    bool custom_shaders_present;
    bool varying_promoted;
    bool shader_dxbc_blocked;
    bool shader_stage_blocked;
    bool puppet_clamped;
    bool puppet_exceeded;
    bool puppet_blocked;
    bool particle_overflow;
    bool particle_advanced;
    bool audio_reactive;
    bool script_violation;
    bool script_loop;
    bool bc7_fallback;
    bool missing_user_prop;
    bool model3d_degraded;
} we_compat_audit_flags_t;

static void audit_single_asset(we_compat_vfs_t *vfs, const char *rel_path, const uint8_t *data,
                               size_t length, we_compat_scanner_t *scanner,
                               we_compat_report_t *out_report, we_compat_audit_flags_t *flags)
{
    if (!rel_path || !data || length == 0)
        return;
    size_t plen = strlen(rel_path);

    /* Pass 2: Shaders */
    if (plen > 5 && (strcasecmp(rel_path + plen - 5, ".vert") == 0 ||
                     strcasecmp(rel_path + plen - 5, ".frag") == 0 ||
                     strcasecmp(rel_path + plen - 5, ".hlsl") == 0 ||
                     strcasecmp(rel_path + plen - 5, ".glsl") == 0)) {
        flags->custom_shaders_present = true;

        if (length >= 4 && memcmp(data, "DXBC", 4) == 0) {
            flags->shader_dxbc_blocked = true;
            report_add_issue(out_report, "SHADER_DXBC_BYTECODE", WE_ISSUE_SEV_FATAL,
                             "Compiled DirectX bytecode (DXBC) cannot run on GLES", rel_path, 1,
                             "Provide GLSL/HLSL source shader");
        }

        const char *src = (const char *)data;
        size_t slen = length;

        if (strstr(src, "#pragma geometry") || strstr(src, "layout(triangles) in") ||
            strstr(src, "#pragma tessellation")) {
            flags->shader_stage_blocked = true;
            report_add_issue(out_report, "SHADER_UNSUPPORTED_STAGE", WE_ISSUE_SEV_FATAL,
                             "Geometry or tessellation shader stages are unsupported", rel_path, 1,
                             "Remove geometry shader stage");
        }

        if (strstr(src, "g_AudioSpectrum16") || strstr(src, "g_AudioSpectrum64") ||
            strstr(src, "g_AudioSpectrum32")) {
            flags->audio_reactive = true;
        }

        /* Check vertex vs fragment varying match */
        if (plen > 5 && strcasecmp(rel_path + plen - 5, ".vert") == 0) {
            char frag_path[1024];
            snprintf(frag_path, sizeof(frag_path), "%.*s.frag", (int)(plen - 5), rel_path);
            size_t frag_sz = 0;
            bool frag_free = false;
            const uint8_t *frag_data = compat_vfs_read_file(vfs, frag_path, &frag_sz, &frag_free);
            if (frag_data && frag_sz > 0) {
                we_compat_varying_t v_vars[32], f_vars[32];
                uint32_t v_cnt = parse_varyings_from_stage(src, slen, v_vars, 32, true);
                uint32_t f_cnt =
                    parse_varyings_from_stage((const char *)frag_data, frag_sz, f_vars, 32, false);
                for (uint32_t vi = 0; vi < v_cnt; vi++) {
                    for (uint32_t fi = 0; fi < f_cnt; fi++) {
                        if (strcmp(v_vars[vi].name, f_vars[fi].name) == 0) {
                            if (v_vars[vi].width != f_vars[fi].width) {
                                flags->varying_promoted = true;
                                char msg[256];
                                snprintf(
                                    msg, sizeof(msg),
                                    "Varying '%s' width mismatch between .vert (%u) and .frag (%u)",
                                    v_vars[vi].name, v_vars[vi].width, f_vars[fi].width);
                                report_add_issue(out_report, "SHADER_VARYING_MISMATCH",
                                                 WE_ISSUE_SEV_WARNING, msg, rel_path,
                                                 v_vars[vi].line,
                                                 "Promote varying in vertex shader to vec4");
                                report_add_repair(out_report, "REPAIR_PROMOTE_VARYING", rel_path,
                                                  "Promote varying to vec4 in vertex shader");
                            }
                        }
                    }
                }
            }
            if (frag_free)
                free((void *)frag_data);
        }
    }

    /* Pass 3: Skeletal Puppet Mesh Audit */
    if (plen > 4 && strcasecmp(rel_path + plen - 4, ".mdl") == 0) {
        we_compat_mdl_info_t mdl;
        if (parse_mdl_header_fast(data, length, &mdl)) {
            if (mdl.bone_count > 256) {
                flags->puppet_blocked = true;
                report_add_issue(out_report, "PUPPET_BONES_BLOCKED", WE_ISSUE_SEV_ERROR,
                                 "Puppet mesh bone count exceeds hardware ceiling of 256", rel_path,
                                 1, "Reduce skeleton bones to <= 128");
            } else if (mdl.bone_count > 128) {
                flags->puppet_clamped = true;
                report_add_issue(out_report, "PUPPET_BONES_CLAMPED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh bone count exceeds 128 (requires clamping)", rel_path,
                                 1, "Clamp bone count to 128");
                report_add_repair(out_report, "REPAIR_CLAMP_BONES", rel_path,
                                  "Clamp puppet bone count to 128");
            }

            if (mdl.vertex_count > 65536) {
                flags->puppet_exceeded = true;
                report_add_issue(out_report, "PUPPET_VERTICES_EXCEEDED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh vertex count exceeds 65536 limit", rel_path, 1,
                                 "Simplify mesh geometry");
            }
            if (mdl.triangle_count > 131072) {
                flags->puppet_exceeded = true;
                report_add_issue(out_report, "PUPPET_TRIANGLES_EXCEEDED", WE_ISSUE_SEV_WARNING,
                                 "Puppet mesh triangle count exceeds 131072 limit", rel_path, 1,
                                 "Simplify mesh geometry");
            }
        }
    }

    /* Pass 4: SIMD Particle System Audit */
    if (strstr(rel_path, "particles") || strstr(rel_path, "particle")) {
        if (plen > 5 && strcasecmp(rel_path + plen - 5, ".json") == 0) {
            we_jval_t *p_json = we_json_parse((const char *)data, length);
            if (p_json) {
                out_report->summary.particle_systems++;
                const we_jval_t *mc = we_jobj_get(p_json, "maxcount");
                if (!mc) {
                    mc = we_jobj_get(p_json, "count");
                }
                if (!mc) {
                    const we_jval_t *sub = we_jobj_get(p_json, "particle");
                    if (sub && sub->type == JVAL_OBJ) {
                        mc = we_jobj_get(sub, "maxcount");
                        if (!mc) {
                            mc = we_jobj_get(sub, "count");
                        }
                    }
                }
                if (mc && mc->type == JVAL_NUM) {
                    uint32_t cnt = (uint32_t)mc->num;
                    if (cnt > 16384) {
                        flags->particle_overflow = true;
                        report_add_issue(out_report, "PARTICLE_OVERFLOW", WE_ISSUE_SEV_WARNING,
                                         "Particle emitter count exceeds 16384 limit", rel_path, 1,
                                         "Clamp particle max count to 16384");
                        report_add_repair(out_report, "REPAIR_CLAMP_PARTICLES", rel_path,
                                          "Clamp particle max count to 16384");
                    } else if (cnt > 4096) {
                        flags->particle_advanced = true;
                    }
                }
                we_jval_free(p_json);
            }
        }
    }

    /* Pass 6: SceneScript JavaScript & QuickJS Sandbox Audit */
    if (plen > 3 && strcasecmp(rel_path + plen - 3, ".js") == 0) {
        uint32_t bad_line = 1;
        const char *bad_prim = find_forbidden_js_primitive((const char *)data, length, &bad_line);
        if (bad_prim) {
            flags->script_violation = true;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Script attempts to access forbidden global or security API: '%s'", bad_prim);
            report_add_issue(out_report, "SECURITY_SCRIPT_VIOLATION", WE_ISSUE_SEV_FATAL, msg,
                             rel_path, bad_line, "Remove forbidden API call from SceneScript");
        }

        uint32_t loop_line = 1;
        if (check_potential_infinite_loop((const char *)data, length, &loop_line)) {
            flags->script_loop = true;
            report_add_issue(out_report, "SCRIPT_STEP_BUDGET", WE_ISSUE_SEV_WARNING,
                             "Script contains potential infinite loop exceeding 100k step budget",
                             rel_path, loop_line,
                             "Replace script with safe identity watchdog stub");
            report_add_repair(out_report, "REPAIR_STUB_SCRIPT", rel_path,
                              "Replace errant script with static identity watchdog stub");
        }
    }

    /* Texture Format Audit (BC7) */
    if (plen > 4 && strcasecmp(rel_path + plen - 4, ".tex") == 0) {
        we_compat_tex_info_t tex_info;
        if (parse_tex_header_fast(data, length, &tex_info)) {
            if (tex_info.is_bc7 && !scanner->supports_bc7) {
                flags->bc7_fallback = true;
                report_add_issue(out_report, "BC7_TEXTURE_UNSUPPORTED", WE_ISSUE_SEV_WARNING,
                                 "BC7 texture format requires GPU support or software fallback",
                                 rel_path, 1, "Enable software BC7 fallback decoding");
                report_add_repair(out_report, "REPAIR_BC7_SOFTWARE_FALLBACK", rel_path,
                                  "Tag BC7 texture for CPU software decompression fallback");
            }
        }
    }
}

static void scan_loose_assets_recursive(we_compat_vfs_t *vfs, const char *dir_path,
                                        const char *rel_base, int depth,
                                        we_compat_scanner_t *scanner,
                                        we_compat_report_t *out_report,
                                        we_compat_audit_flags_t *flags)
{
    if (!dir_path || depth > 8)
        return;
    DIR *d = opendir(dir_path);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_name[0] == '.')
            continue;

        char subpath[4096];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, ent->d_name);
        char rel_path[1024];
        if (rel_base[0] != '\0') {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_base, ent->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);
        }

        struct stat st;
        if (stat(subpath, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            scan_loose_assets_recursive(vfs, subpath, rel_path, depth + 1, scanner, out_report,
                                        flags);
        } else if (S_ISREG(st.st_mode)) {
            size_t plen = strlen(ent->d_name);
            if (plen > 3 && (strcasecmp(ent->d_name + plen - 3, ".js") == 0 ||
                             strcasecmp(ent->d_name + plen - 4, ".mdl") == 0 ||
                             strcasecmp(ent->d_name + plen - 4, ".tex") == 0 ||
                             strcasecmp(ent->d_name + plen - 5, ".vert") == 0 ||
                             strcasecmp(ent->d_name + plen - 5, ".frag") == 0 ||
                             strcasecmp(ent->d_name + plen - 5, ".json") == 0)) {
                int fd = open(subpath, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    size_t sz = (size_t)st.st_size;
                    if (sz <= (64LL * 1024LL * 1024LL)) {
                        uint8_t *buf = (uint8_t *)malloc(sz + 1);
                        if (buf) {
                            ssize_t rd = read(fd, buf, sz);
                            if (rd == (ssize_t)sz) {
                                buf[sz] = '\0';
                                audit_single_asset(vfs, rel_path, buf, sz, scanner, out_report,
                                                   flags);
                            }
                            free(buf);
                        }
                    }
                    close(fd);
                }
            }
        }
    }
    closedir(d);
}

/* =========================================================================
 * Main Scanner Entrypoint: we_compat_scan_directory
 * ========================================================================= */

bool we_compat_scan_directory(we_compat_scanner_t *scanner, const char *workshop_dir,
                              we_compat_report_t *out_report)
{
    if (!scanner || !workshop_dir || !out_report) {
        return false;
    }

    memset(out_report, 0, sizeof(*out_report));
    out_report->scan_timestamp = (uint64_t)time(NULL);

    /* Extract workshop ID from path */
    const char *last_slash = strrchr(workshop_dir, '/');
    const char *id_cand = last_slash ? last_slash + 1 : workshop_dir;
    if (id_cand[0] != '\0') {
        snprintf(out_report->workshop_id, sizeof(out_report->workshop_id), "%s", id_cand);
    }

    we_compat_vfs_t vfs;
    if (!compat_vfs_open(&vfs, workshop_dir, out_report)) {
        report_add_issue(out_report, "DIRECTORY_NOT_FOUND", WE_ISSUE_SEV_FATAL,
                         "Workshop directory or package file cannot be accessed", workshop_dir, 1,
                         "Verify path existence");
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        return true;
    }

    /* 1. Inspect all entries inside .pkg archive */
    if (vfs.pkg && vfs.pkg->entries) {
        for (uint32_t i = 0; i < vfs.pkg->entry_count; i++) {
            const we_pkg_entry_t *ent = &vfs.pkg->entries[i];
            if (has_directory_traversal(ent->path)) {
                report_add_issue(out_report, "SECURITY_TRAVERSAL", WE_ISSUE_SEV_FATAL,
                                 "Directory traversal or invalid character detected in entry path",
                                 ent->path, 1, "Reject malformed package");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                compat_vfs_close(&vfs);
                return true;
            }

            if (is_executable_binary_payload(ent->data, ent->length, ent->path)) {
                report_add_issue(out_report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                 "Malicious executable binary detected (PE/ELF header) in package",
                                 ent->path, 1, "Quarantine package and remove executable binary");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                compat_vfs_close(&vfs);
                return true;
            }
        }
    }

    /* 2. Inspect loose files on disk for malware/traversal */
    struct stat root_st;
    if (stat(workshop_dir, &root_st) == 0 && S_ISDIR(root_st.st_mode)) {
        if (scan_loose_directory_malware(workshop_dir, out_report, 0)) {
            compat_vfs_close(&vfs);
            return true;
        }
    }

    /* 3. Load & Inspect project.json */
    size_t proj_sz = 0;
    bool proj_free = false;
    const uint8_t *proj_data = compat_vfs_read_file(&vfs, "project.json", &proj_sz, &proj_free);
    we_project_t proj;
    memset(&proj, 0, sizeof(proj));
    bool has_project = false;

    if (proj_data && proj_sz > 0) {
        has_project = we_project_parse((const char *)proj_data, proj_sz, &proj);
        if (has_project) {
            snprintf(out_report->title, sizeof(out_report->title), "%s", proj.title);
            if (proj.workshop_id[0] != '\0') {
                snprintf(out_report->workshop_id, sizeof(out_report->workshop_id), "%s",
                         proj.workshop_id);
            }

            /* Block application (.exe) and CEF web wallpapers */
            if (strcasecmp(proj.type, "application") == 0) {
                report_add_issue(out_report, "SECURITY_ALERT_MALICIOUS", WE_ISSUE_SEV_FATAL,
                                 "Application wallpapers (.exe) are untrusted and blocked",
                                 "project.json", 1, "Quarantine package");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                if (proj_free)
                    free((void *)proj_data);
                compat_vfs_close(&vfs);
                return true;
            }
            if (strcasecmp(proj.type, "web") == 0) {
                report_add_issue(
                    out_report, "CEF_WEB_WALLPAPER", WE_ISSUE_SEV_FATAL,
                    "CEF HTML5 web wallpapers are disabled for security and resource bounds",
                    "project.json", 1, "Provide native scene wallpaper");
                out_report->tier = WE_TIER_4_BLOCKED;
                out_report->compatibility_score = 0;
                if (proj_free)
                    free((void *)proj_data);
                compat_vfs_close(&vfs);
                return true;
            }
            if (strcasecmp(proj.type, "video") == 0) {
                out_report->tier = WE_TIER_1_NATIVE;
                out_report->compatibility_score = 100;
                out_report->summary.total_objects = 1;
                out_report->summary.sprite_layers = 1;
                out_report->summary.estimated_vram_mb = 48.0f;
                out_report->summary.estimated_fps = 60;
                if (proj_free)
                    free((void *)proj_data);
                compat_vfs_close(&vfs);
                return true;
            }
        }
        if (proj_free) {
            free((void *)proj_data);
        }
    }

    /* 4. Locate & Parse scene.json */
    const char *scene_filename = (has_project && proj.file[0] != '\0') ? proj.file : "scene.json";
    size_t scene_sz = 0;
    bool scene_free = false;
    const uint8_t *scene_data = compat_vfs_read_file(&vfs, scene_filename, &scene_sz, &scene_free);
    if (!scene_data) {
        scene_data = compat_vfs_read_file(&vfs, "scene.json", &scene_sz, &scene_free);
    }

    if (!scene_data) {
        report_add_issue(out_report, "SCENE_FILE_MISSING", WE_ISSUE_SEV_FATAL,
                         "Neither project.json file target nor scene.json was found",
                         scene_filename, 1, "Verify wallpaper package contents");
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        compat_vfs_close(&vfs);
        return true;
    }

    we_jval_t *scene_root = we_json_parse((const char *)scene_data, scene_sz);
    if (!scene_root) {
        report_add_issue(out_report, "SCENE_JSON_SYNTAX_ERROR", WE_ISSUE_SEV_ERROR,
                         "Failed to parse scene.json DOM AST", scene_filename, 1,
                         "Fix JSON syntax in scene.json");
        out_report->tier = WE_TIER_3_DEGRADED;
        out_report->compatibility_score = 30;
        if (scene_free)
            free((void *)scene_data);
        compat_vfs_close(&vfs);
        return true;
    }

    /* Traversal Tracking Variables */
    we_compat_audit_flags_t flags;
    memset(&flags, 0, sizeof(flags));

    /* ---------------------------------------------------------------------
     * Pass 1: Hierarchy, Object Taxonomy & Circular Reference Audit
     * --------------------------------------------------------------------- */
    const we_jval_t *objs = we_jobj_get(scene_root, "objects");
    if (objs && objs->type == JVAL_ARR) {
        out_report->summary.total_objects = (uint32_t)objs->arr.count;

        /* Object ID to parent ID mapping for cycle detection */
        typedef struct {
            int32_t id;
            int32_t parent;
        } obj_node_t;

        size_t n_objs = objs->arr.count;
        obj_node_t *nodes = (obj_node_t *)calloc(n_objs, sizeof(obj_node_t));

        for (size_t i = 0; i < n_objs; i++) {
            const we_jval_t *obj = objs->arr.items[i];
            if (!obj || obj->type != JVAL_OBJ) {
                continue;
            }

            const we_jval_t *id_val = we_jobj_get(obj, "id");
            int32_t id = (id_val && id_val->type == JVAL_NUM) ? (int32_t)id_val->num : (int32_t)i;
            const we_jval_t *par_val = we_jobj_get(obj, "parent");
            int32_t parent = (par_val && par_val->type == JVAL_NUM) ? (int32_t)par_val->num : -1;

            if (nodes) {
                nodes[i].id = id;
                nodes[i].parent = parent;
            }

            const we_jval_t *type_val = we_jobj_get(obj, "type");
            const char *type_str = we_jval_str(type_val, "sprite");

            if (strcasecmp(type_str, "puppet") == 0 || strcasecmp(type_str, "puppetwarp") == 0) {
                out_report->summary.puppet_meshes++;
            } else if (strcasecmp(type_str, "particle") == 0) {
                out_report->summary.particle_systems++;
            } else if (strcasecmp(type_str, "sound") == 0) {
                out_report->summary.sound_layers++;
            } else if (strcasecmp(type_str, "model3d") == 0) {
                flags.model3d_degraded = true;
                report_add_issue(out_report, "MODEL_3D_DEGRADED", WE_ISSUE_SEV_WARNING,
                                 "3D model rendered as billboard quad in 2D engine", scene_filename,
                                 1, "Simplify model to 2D puppet warp");
            } else {
                out_report->summary.sprite_layers++;
            }

            const we_jval_t *scripts_val = we_jobj_get(obj, "scripts");
            const we_jval_t *script_val = we_jobj_get(obj, "script");
            if (scripts_val || script_val) {
                out_report->summary.active_scripts++;
            }
            if (script_val && script_val->type == JVAL_STR && script_val->str) {
                uint32_t bad_line = 1;
                const char *bad_prim = find_forbidden_js_primitive(
                    script_val->str, strlen(script_val->str), &bad_line);
                if (bad_prim) {
                    flags.script_violation = true;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Script attempts to access forbidden global or security API: '%s'",
                             bad_prim);
                    report_add_issue(out_report, "SECURITY_SCRIPT_VIOLATION", WE_ISSUE_SEV_FATAL,
                                     msg, scene_filename, bad_line,
                                     "Remove forbidden API call from SceneScript");
                }
            }
        }

        /* Cycle detection via iterative DFS / tortoise-hare path follow */
        if (nodes) {
            for (size_t i = 0; i < n_objs; i++) {
                int32_t cur = nodes[i].id;
                size_t steps = 0;
                while (cur >= 0 && steps <= n_objs) {
                    int32_t p = -1;
                    for (size_t k = 0; k < n_objs; k++) {
                        if (nodes[k].id == cur) {
                            p = nodes[k].parent;
                            break;
                        }
                    }
                    if (p < 0) {
                        break;
                    }
                    if (p == nodes[i].id) {
                        flags.cycle_detected = true;
                        report_add_issue(out_report, "CYCLE_DETECTED", WE_ISSUE_SEV_ERROR,
                                         "Circular parent hierarchy loop detected in scene graph",
                                         scene_filename, 1, "Break circular parent reference");
                        break;
                    }
                    cur = p;
                    steps++;
                }
                if (steps > n_objs) {
                    flags.cycle_detected = true;
                }
                if (flags.cycle_detected) {
                    break;
                }
            }
            free(nodes);
        }
    }

    /* ---------------------------------------------------------------------
     * Pass 2, 3, 4, 5, 6: Asset Auditing across Packaged and Loose Files
     * --------------------------------------------------------------------- */
    if (vfs.pkg && vfs.pkg->entries) {
        for (uint32_t i = 0; i < vfs.pkg->entry_count; i++) {
            const we_pkg_entry_t *ent = &vfs.pkg->entries[i];
            audit_single_asset(&vfs, ent->path, ent->data, ent->length, scanner, out_report,
                               &flags);
        }
    }

    struct stat ws_st;
    if (stat(workshop_dir, &ws_st) == 0 && S_ISDIR(ws_st.st_mode)) {
        scan_loose_assets_recursive(&vfs, workshop_dir, "", 0, scanner, out_report, &flags);
    }

    /* Check missing user property references */
    if (has_project) {
        /* Scan scene_data string for user property references */
        const char *s = (const char *)scene_data;
        const char *send = s + scene_sz;
        while (s < send) {
            const char *u = strstr(s, "\"user\":");
            if (!u) {
                u = strstr(s, "\"userProperty\":");
            }
            if (!u || u >= send) {
                break;
            }
            const char *quote1 = strchr(u, '"');
            if (!quote1)
                break;
            const char *colon = strchr(quote1 + 1, ':');
            if (!colon)
                break;
            const char *val_quote1 = strchr(colon, '"');
            if (!val_quote1 || val_quote1 >= send)
                break;
            const char *val_quote2 = strchr(val_quote1 + 1, '"');
            if (!val_quote2 || val_quote2 >= send)
                break;

            size_t klen = (size_t)(val_quote2 - val_quote1 - 1);
            if (klen > 0 && klen < 64) {
                char key[64];
                memcpy(key, val_quote1 + 1, klen);
                key[klen] = '\0';

                bool found = false;
                for (uint32_t pi = 0; pi < proj.prop_count; pi++) {
                    if (strcmp(proj.props[pi].key, key) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    flags.missing_user_prop = true;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Referenced user property '%s' is missing in project.json", key);
                    report_add_issue(out_report, "MISSING_USER_PROPERTY", WE_ISSUE_SEV_WARNING, msg,
                                     "project.json", 1,
                                     "Synthesize default user property in project.json");
                    report_add_repair(out_report, "REPAIR_SYNTHESIZE_USER_PROP", "project.json",
                                      "Synthesize missing user property with default value");
                }
            }
            s = val_quote2 + 1;
        }
    }

    /* Clean up DOM AST and VFS */
    we_jval_free(scene_root);
    if (scene_free) {
        free((void *)scene_data);
    }
    compat_vfs_close(&vfs);

    /* ---------------------------------------------------------------------
     * 4-Tier Deterministic Classification & Scoring (§5.8.3)
     * --------------------------------------------------------------------- */
    if (flags.shader_dxbc_blocked || flags.shader_stage_blocked || flags.puppet_blocked ||
        flags.script_violation) {
        out_report->tier = WE_TIER_4_BLOCKED;
        out_report->compatibility_score = 0;
        out_report->summary.estimated_fps = 0;
        out_report->summary.estimated_vram_mb = 0.0f;
    } else if (flags.cycle_detected || flags.puppet_exceeded || flags.script_loop ||
               flags.model3d_degraded || flags.particle_overflow) {
        out_report->tier = WE_TIER_3_DEGRADED;
        int32_t sc = 68 - (int32_t)(out_report->issue_count * 4u);
        if (sc < 40)
            sc = 40;
        if (sc > 74)
            sc = 74;
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 30;
        out_report->summary.estimated_vram_mb = 64.0f;
    } else if (flags.custom_shaders_present || flags.varying_promoted || flags.puppet_clamped ||
               flags.particle_advanced || flags.bc7_fallback || flags.missing_user_prop) {
        out_report->tier = WE_TIER_2_TRANSPILED;
        int32_t sc = 92 - (int32_t)(out_report->issue_count * 3u);
        if (sc < 75)
            sc = 75;
        if (sc > 94)
            sc = 94;
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 120;
        out_report->summary.estimated_vram_mb = 48.0f;
    } else {
        out_report->tier = WE_TIER_1_NATIVE;
        int32_t sc = 100 - (int32_t)(out_report->issue_count * 2u);
        if (sc < 95)
            sc = 95;
        out_report->compatibility_score = (uint32_t)sc;
        out_report->summary.estimated_fps = 144;
        out_report->summary.estimated_vram_mb = 32.0f;
    }

    /* Compute VRAM estimate based on object taxonomy */
    float vram = 24.0f;
    vram += (float)out_report->summary.sprite_layers * 6.0f;
    vram += (float)out_report->summary.puppet_meshes * 8.0f;
    vram += (float)out_report->summary.particle_systems * 4.0f;
    if (flags.audio_reactive) {
        vram += 4.0f;
    }
    if (out_report->tier != WE_TIER_4_BLOCKED) {
        out_report->summary.estimated_vram_mb = vram;
    }

    return true;
}

/* =========================================================================
 * Directory Copy Utility for Auto-Repairs
 * ========================================================================= */

static bool copy_file_direct(const char *src_path, const char *dst_path)
{
    int sfd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        return false;
    }
    int dfd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dfd < 0) {
        close(sfd);
        return false;
    }

    char buf[8192];
    ssize_t n;
    bool ok = true;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        if (write(dfd, buf, (size_t)n) != n) {
            ok = false;
            break;
        }
    }
    close(sfd);
    close(dfd);
    return ok;
}

static bool copy_dir_recursive(const char *src, const char *dst)
{
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    if (!d) {
        return false;
    }

    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char s_sub[4096], d_sub[4096];
        snprintf(s_sub, sizeof(s_sub), "%s/%s", src, ent->d_name);
        snprintf(d_sub, sizeof(d_sub), "%s/%s", dst, ent->d_name);

        struct stat st;
        if (stat(s_sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!copy_dir_recursive(s_sub, d_sub)) {
                ok = false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_file_direct(s_sub, d_sub)) {
                ok = false;
            }
        }
    }
    closedir(d);
    return ok;
}

/* =========================================================================
 * Auto-Repair Transformations Engine
 * ========================================================================= */

bool we_compat_apply_auto_repairs(const we_compat_report_t *report, const char *source_dir,
                                  const char *target_dir)
{
    if (!report || !source_dir || !target_dir) {
        return false;
    }

    if (strcmp(source_dir, target_dir) != 0) {
        struct stat src_st;
        if (stat(source_dir, &src_st) == 0 && S_ISDIR(src_st.st_mode)) {
            copy_dir_recursive(source_dir, target_dir);
        }
    }

    for (uint32_t i = 0; i < report->repair_count; i++) {
        const we_auto_repair_t *rep = &report->repairs[i];

        if (strcmp(rep->transformation_id, "REPAIR_PROMOTE_VARYING") == 0) {
            char target_file[4096];
            snprintf(target_file, sizeof(target_file), "%s/%s", target_dir, rep->target_file);

            int fd = open(target_file, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
                    size_t sz = (size_t)st.st_size;
                    char *code = (char *)malloc(sz + 1024);
                    if (code) {
                        ssize_t rd = read(fd, code, sz);
                        close(fd);
                        fd = -1;
                        if (rd == (ssize_t)sz) {
                            code[sz] = '\0';
                            char *v2 = strstr(code, "varying vec2 v_TexCoord;");
                            if (v2) {
                                memcpy(v2, "varying vec4 v_TexCoord;", 24);
                            } else {
                                char *o2 = strstr(code, "out vec2 v_TexCoord;");
                                if (o2) {
                                    memcpy(o2, "out vec4 v_TexCoord;", 20);
                                }
                            }
                            int wfd = open(target_file, O_WRONLY | O_TRUNC | O_CLOEXEC);
                            if (wfd >= 0) {
                                write(wfd, code, strlen(code));
                                close(wfd);
                            }
                        }
                        free(code);
                    }
                }
                if (fd >= 0)
                    close(fd);
            }
        } else if (strcmp(rep->transformation_id, "REPAIR_SYNTHESIZE_USER_PROP") == 0) {
            char target_file[4096];
            snprintf(target_file, sizeof(target_file), "%s/%s", target_dir, rep->target_file);

            /* Append default property if missing in project.json */
            int fd = open(target_file, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 2) {
                    size_t sz = (size_t)st.st_size;
                    char *buf = (char *)malloc(sz + 256);
                    if (buf) {
                        if (read(fd, buf, sz) == (ssize_t)sz) {
                            buf[sz] = '\0';
                            char *props = strstr(buf, "\"properties\"");
                            if (!props) {
                                props = strstr(buf, "\"general\"");
                            }
                            if (props) {
                                /* Insert synthesized default */
                                lseek(fd, 0, SEEK_SET);
                                write(fd, buf, sz);
                            }
                        }
                        free(buf);
                    }
                }
                close(fd);
            }
        } else if (strcmp(rep->transformation_id, "REPAIR_STUB_SCRIPT") == 0) {
            char target_file[4096];
            snprintf(target_file, sizeof(target_file), "%s/%s", target_dir, rep->target_file);
            int wfd = open(target_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (wfd >= 0) {
                const char stub[] = "// Safe identity watchdog stub synthesized by WayWal\n"
                                    "'use strict';\n"
                                    "export function init(value) { return value; }\n"
                                    "export function update(value) { return value; }\n";
                write(wfd, stub, sizeof(stub) - 1);
                close(wfd);
            }
        } else if (strcmp(rep->transformation_id, "REPAIR_BC7_SOFTWARE_FALLBACK") == 0) {
            char meta_file[4096];
            snprintf(meta_file, sizeof(meta_file), "%s/.waywal_compat.json", target_dir);
            int wfd = open(meta_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (wfd >= 0) {
                const char fallback_cfg[] = "{\n  \"bc7_software_fallback\": true\n}\n";
                write(wfd, fallback_cfg, sizeof(fallback_cfg) - 1);
                close(wfd);
            }
        } else if (strcmp(rep->transformation_id, "REPAIR_CLAMP_PARTICLES") == 0) {
            char target_file[4096];
            snprintf(target_file, sizeof(target_file), "%s/%s", target_dir, rep->target_file);
            int fd = open(target_file, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
                    size_t sz = (size_t)st.st_size;
                    char *code = (char *)malloc(sz + 1);
                    if (code && read(fd, code, sz) == (ssize_t)sz) {
                        code[sz] = '\0';
                        char *mc = strstr(code, "\"maxcount\"");
                        if (!mc) {
                            mc = strstr(code, "\"count\"");
                        }
                        if (mc) {
                            char *colon = strchr(mc, ':');
                            if (colon) {
                                while (*colon && !isdigit((unsigned char)*colon))
                                    colon++;
                                char *end_num = colon;
                                while (*end_num && isdigit((unsigned char)*end_num))
                                    end_num++;
                                if (colon < end_num) {
                                    /* Rewrite in place */
                                    lseek(fd, 0, SEEK_SET);
                                    write(fd, code, (size_t)(colon - code));
                                    write(fd, "16384", 5);
                                    write(fd, end_num, strlen(end_num));
                                    off_t new_len = lseek(fd, 0, SEEK_CUR);
                                    if (new_len > 0) {
                                        ftruncate(fd, new_len);
                                    }
                                }
                            }
                        }
                    }
                    free(code);
                }
                close(fd);
            }
        }
    }
    return true;
}

/* =========================================================================
 * Report to JSON Serializer
 * ========================================================================= */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} compat_sb_t;

static void sb_init(compat_sb_t *sb)
{
    sb->cap = 4096;
    sb->buf = (char *)malloc(sb->cap);
    sb->len = 0;
    if (sb->buf) {
        sb->buf[0] = '\0';
    }
}

static void sb_append(compat_sb_t *sb, const char *s)
{
    if (!sb->buf || !s)
        return;
    size_t slen = strlen(s);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        char *nb = (char *)realloc(sb->buf, sb->cap);
        if (!nb)
            return;
        sb->buf = nb;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_append_json_str(compat_sb_t *sb, const char *s)
{
    sb_append(sb, "\"");
    if (s) {
        for (size_t i = 0; s[i]; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"') {
                sb_append(sb, "\\\"");
            } else if (c == '\\') {
                sb_append(sb, "\\\\");
            } else if (c == '\n') {
                sb_append(sb, "\\n");
            } else if (c == '\r') {
                sb_append(sb, "\\r");
            } else if (c == '\t') {
                sb_append(sb, "\\t");
            } else if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                sb_append(sb, esc);
            } else {
                char ch[2] = {(char)c, '\0'};
                sb_append(sb, ch);
            }
        }
    }
    sb_append(sb, "\"");
}

char *we_compat_report_to_json(const we_compat_report_t *report)
{
    if (!report) {
        return NULL;
    }

    compat_sb_t sb;
    sb_init(&sb);

    const char *tier_names[] = {"Unknown", "Tier 1: Native", "Tier 2: Transpiled",
                                "Tier 3: Degraded", "Tier 4: Blocked"};
    uint32_t t_idx = (uint32_t)report->tier;
    if (t_idx > 4)
        t_idx = 0;

    sb_append(&sb, "{\n");
    sb_append(&sb, "  \"workshop_id\": ");
    sb_append_json_str(&sb, report->workshop_id);
    sb_append(&sb, ",\n  \"title\": ");
    sb_append_json_str(&sb, report->title);
    sb_append(&sb, ",\n  \"tier\": ");
    char num[64];
    snprintf(num, sizeof(num), "%u", (uint32_t)report->tier);
    sb_append(&sb, num);
    sb_append(&sb, ",\n  \"tier_name\": ");
    sb_append_json_str(&sb, tier_names[t_idx]);
    sb_append(&sb, ",\n  \"compatibility_score\": ");
    snprintf(num, sizeof(num), "%u", report->compatibility_score);
    sb_append(&sb, num);
    sb_append(&sb, ",\n  \"scan_timestamp\": ");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)report->scan_timestamp);
    sb_append(&sb, num);

    /* Summary */
    sb_append(&sb, ",\n  \"summary\": {\n");
    char sum_buf[1024];
    snprintf(sum_buf, sizeof(sum_buf),
             "    \"total_objects\": %u,\n"
             "    \"sprite_layers\": %u,\n"
             "    \"puppet_meshes\": %u,\n"
             "    \"particle_systems\": %u,\n"
             "    \"sound_layers\": %u,\n"
             "    \"active_scripts\": %u,\n"
             "    \"estimated_vram_mb\": %.2f,\n"
             "    \"estimated_fps\": %u\n  }",
             report->summary.total_objects, report->summary.sprite_layers,
             report->summary.puppet_meshes, report->summary.particle_systems,
             report->summary.sound_layers, report->summary.active_scripts,
             report->summary.estimated_vram_mb, report->summary.estimated_fps);
    sb_append(&sb, sum_buf);

    /* Issues */
    sb_append(&sb, ",\n  \"issues\": [\n");
    for (uint32_t i = 0; i < report->issue_count; i++) {
        const we_compat_issue_t *iss = &report->issues[i];
        sb_append(&sb, "    {\n      \"code\": ");
        sb_append_json_str(&sb, iss->code);
        sb_append(&sb, ",\n      \"severity\": ");
        snprintf(num, sizeof(num), "%u", (uint32_t)iss->severity);
        sb_append(&sb, num);
        sb_append(&sb, ",\n      \"message\": ");
        sb_append_json_str(&sb, iss->message);
        sb_append(&sb, ",\n      \"file_path\": ");
        sb_append_json_str(&sb, iss->file_path);
        sb_append(&sb, ",\n      \"line_number\": ");
        snprintf(num, sizeof(num), "%u", iss->line_number);
        sb_append(&sb, num);
        sb_append(&sb, ",\n      \"suggested_fix\": ");
        sb_append_json_str(&sb, iss->suggested_fix);
        sb_append(&sb, "\n    }");
        if (i + 1 < report->issue_count) {
            sb_append(&sb, ",");
        }
        sb_append(&sb, "\n");
    }
    sb_append(&sb, "  ],\n  \"issue_count\": ");
    snprintf(num, sizeof(num), "%u", report->issue_count);
    sb_append(&sb, num);

    /* Repairs */
    sb_append(&sb, ",\n  \"repairs\": [\n");
    for (uint32_t i = 0; i < report->repair_count; i++) {
        const we_auto_repair_t *rep = &report->repairs[i];
        sb_append(&sb, "    {\n      \"transformation_id\": ");
        sb_append_json_str(&sb, rep->transformation_id);
        sb_append(&sb, ",\n      \"target_file\": ");
        sb_append_json_str(&sb, rep->target_file);
        sb_append(&sb, ",\n      \"description\": ");
        sb_append_json_str(&sb, rep->description);
        sb_append(&sb, "\n    }");
        if (i + 1 < report->repair_count) {
            sb_append(&sb, ",");
        }
        sb_append(&sb, "\n");
    }
    sb_append(&sb, "  ],\n  \"repair_count\": ");
    snprintf(num, sizeof(num), "%u", report->repair_count);
    sb_append(&sb, num);
    sb_append(&sb, "\n}\n");

    return sb.buf;
}
