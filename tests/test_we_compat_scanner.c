// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waywal/we_compat_scanner.h"

static void cleanup_dir(const char *path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void write_bytes_to_path(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (len > 0u) {
        size_t wr = fwrite(data, 1u, len, f);
        assert(wr == len);
    }
    fclose(f);
}

static char *read_text_from_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long sz = ftell(f);
    assert(sz >= 0);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    assert(buf != NULL);
    size_t rd = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

static void test_heap_report_lifecycle(void)
{
    printf("[Compat Scanner 1/9] Testing heap allocation & free lifecycle...\n");

    we_compat_report_t *report = we_compat_report_alloc();
    assert(report != NULL && "we_compat_report_alloc must succeed on heap");
    assert(report->tier == WE_TIER_UNKNOWN);
    assert(report->compatibility_score == 0);
    assert(report->issue_count == 0);
    assert(report->repair_count == 0);

    we_compat_report_free(report);
    printf("  -> Heap report lifecycle passed.\n");
}

static void test_pe_elf_malware_quarantine(we_compat_scanner_t *scanner)
{
    printf("[Compat Scanner 2/9] Testing PE and ELF executable malware quarantine...\n");

    const char *pe_dir = "/tmp/test_we_malware_pe";
    cleanup_dir(pe_dir);
    mkdir(pe_dir, 0755);

    uint8_t pe_header[64] = {0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00};
    write_bytes_to_path("/tmp/test_we_malware_pe/payload.exe", pe_header, sizeof(pe_header));

    const char *pj1_str = "{\"title\":\"Trojan Wallpaper\",\"type\":\"scene\"}";
    write_bytes_to_path("/tmp/test_we_malware_pe/project.json", pj1_str, strlen(pj1_str));

    we_compat_report_t *rep_pe = we_compat_report_alloc();
    bool scan_pe_ok = we_compat_scan_directory(scanner, pe_dir, rep_pe);
    assert(scan_pe_ok);
    assert(rep_pe->tier == WE_TIER_4_BLOCKED && "PE binary must be locked to WE_TIER_4_BLOCKED");
    assert(rep_pe->compatibility_score == 0 && "Malware score must be 0");

    bool found_pe_alert = false;
    for (uint32_t i = 0; i < rep_pe->issue_count; ++i) {
        if (strcmp(rep_pe->issues[i].code, "SECURITY_ALERT_MALICIOUS") == 0) {
            found_pe_alert = true;
            break;
        }
    }
    assert(found_pe_alert && "Must raise SECURITY_ALERT_MALICIOUS on PE binary");
    we_compat_report_free(rep_pe);
    cleanup_dir(pe_dir);

    const char *elf_dir = "/tmp/test_we_malware_elf";
    cleanup_dir(elf_dir);
    mkdir(elf_dir, 0755);

    uint8_t elf_header[64] = {0x7F, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00};
    write_bytes_to_path("/tmp/test_we_malware_elf/miner.bin", elf_header, sizeof(elf_header));
    write_bytes_to_path("/tmp/test_we_malware_elf/project.json", pj1_str, strlen(pj1_str));

    we_compat_report_t *rep_elf = we_compat_report_alloc();
    bool scan_elf_ok = we_compat_scan_directory(scanner, elf_dir, rep_elf);
    assert(scan_elf_ok);
    assert(rep_elf->tier == WE_TIER_4_BLOCKED && "ELF binary must be locked to WE_TIER_4_BLOCKED");
    assert(rep_elf->compatibility_score == 0);

    bool found_elf_alert = false;
    for (uint32_t i = 0; i < rep_elf->issue_count; ++i) {
        if (strcmp(rep_elf->issues[i].code, "SECURITY_ALERT_MALICIOUS") == 0) {
            found_elf_alert = true;
            break;
        }
    }
    assert(found_elf_alert && "Must raise SECURITY_ALERT_MALICIOUS on ELF binary");
    we_compat_report_free(rep_elf);
    cleanup_dir(elf_dir);

    printf("  -> 100%% PE and ELF malware quarantine passed.\n");
}

static void test_100_package_synthetic_matrix(we_compat_scanner_t *scanner)
{
    printf("[Compat Scanner 3/9] Evaluating 100-package synthetic matrix...\n");

    const char *matrix_root = "/tmp/test_we_matrix";
    cleanup_dir(matrix_root);
    mkdir(matrix_root, 0755);

    int count_tier1 = 0;
    int count_tier2 = 0;
    int count_tier3 = 0;
    int count_tier4 = 0;

    const int TOTAL_PACKAGES = 100;
    we_compat_report_t *rep = we_compat_report_alloc();

    for (int i = 0; i < TOTAL_PACKAGES; ++i) {
        char pkg_dir[256];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/pkg_%03d", matrix_root, i);
        mkdir(pkg_dir, 0755);

        char proj_path[512];
        snprintf(proj_path, sizeof(proj_path), "%s/project.json", pkg_dir);
        FILE *pf = fopen(proj_path, "wb");
        assert(pf != NULL);

        if (i < 55) {
            if (i % 2 == 0) {
                fprintf(
                    pf,
                    "{\"title\":\"Native Scene %d\",\"type\":\"scene\",\"file\":\"scene.json\"}\n",
                    i);
                fclose(pf);
                char scene_path[512];
                snprintf(scene_path, sizeof(scene_path), "%s/scene.json", pkg_dir);
                FILE *sf = fopen(scene_path, "wb");
                assert(sf != NULL);
                fprintf(sf, "{\"objects\":[{\"name\":\"bg\",\"type\":\"image\"}]}\n");
                fclose(sf);
            } else {
                fprintf(
                    pf,
                    "{\"title\":\"Native Video %d\",\"type\":\"video\",\"file\":\"video.mp4\"}\n",
                    i);
                fclose(pf);
            }
        } else if (i < 88) {
            fprintf(
                pf,
                "{\"title\":\"Transpiled Shader %d\",\"type\":\"scene\",\"file\":\"scene.json\"}\n",
                i);
            fclose(pf);
            char scene_path[512];
            snprintf(scene_path, sizeof(scene_path), "%s/scene.json", pkg_dir);
            FILE *sf = fopen(scene_path, "wb");
            assert(sf != NULL);
            fprintf(sf, "{\"objects\":[{\"name\":\"fx\",\"type\":\"shader\",\"shader\":\"effect."
                        "frag\"}]}\n");
            fclose(sf);
            char fx_path[512];
            snprintf(fx_path, sizeof(fx_path), "%s/effect.frag", pkg_dir);
            FILE *ff = fopen(fx_path, "wb");
            assert(ff != NULL);
            fprintf(ff, "// Transpiled varying shader\nvarying vec2 v_TexCoord;\nvoid main() { "
                        "gl_FragColor = vec4(v_TexCoord, 0.0, 1.0); }\n");
            fclose(ff);
        } else if (i < 95) {
            fprintf(
                pf,
                "{\"title\":\"Heavy Particles %d\",\"type\":\"scene\",\"file\":\"scene.json\"}\n",
                i);
            fclose(pf);
            char scene_path[512];
            snprintf(scene_path, sizeof(scene_path), "%s/scene.json", pkg_dir);
            FILE *sf = fopen(scene_path, "wb");
            assert(sf != NULL);
            fprintf(
                sf,
                "{\"objects\":[{\"name\":\"emitter\",\"type\":\"particle\",\"count\":25000}]}\n");
            fclose(sf);
        } else {
            fprintf(pf, "{\"title\":\"Blocked Package %d\",\"type\":\"web\"}\n", i);
            fclose(pf);
            char exe_path[512];
            snprintf(exe_path, sizeof(exe_path), "%s/unauthorized.exe", pkg_dir);
            uint8_t mz[4] = {'M', 'Z', 0x90, 0x00};
            write_bytes_to_path(exe_path, mz, sizeof(mz));
        }

        bool ok = we_compat_scan_directory(scanner, pkg_dir, rep);
        assert(ok);

        if (rep->tier == WE_TIER_1_NATIVE)
            count_tier1++;
        else if (rep->tier == WE_TIER_2_TRANSPILED)
            count_tier2++;
        else if (rep->tier == WE_TIER_3_DEGRADED)
            count_tier3++;
        else if (rep->tier == WE_TIER_4_BLOCKED)
            count_tier4++;
    }

    we_compat_report_free(rep);
    cleanup_dir(matrix_root);

    printf("  -> Classification Breakdown across 100 packages:\n");
    printf("     Tier 1 (Native):      %d\n", count_tier1);
    printf("     Tier 2 (Transpiled):  %d\n", count_tier2);
    printf("     Tier 3 (Degraded):    %d\n", count_tier3);
    printf("     Tier 4 (Blocked):     %d\n", count_tier4);

    assert(count_tier4 >= 5 && "All malicious packages must be quarantined to Tier 4");

    int valid_packages = TOTAL_PACKAGES - 5;
    int tier1_and_2 = count_tier1 + count_tier2;
    double ratio = (double)tier1_and_2 / valid_packages;
    printf("  -> Valid Package Tier 1/2 Ratio: %.1f%% (threshold >= 85.0%%)\n", ratio * 100.0);
    assert(ratio >= 0.85 && "Must achieve >= 85% Tier 1/2 classification on valid packages");

    printf("  -> 100-package synthetic matrix passed.\n");
}

static void test_short_filenames_and_dotfiles(we_compat_scanner_t *scanner)
{
    printf(
        "[Compat Scanner 4/9] Testing 'abcd' 4-char filenames, .hidden.js, and .hlsl/.glsl...\n");

    const char *dir = "/tmp/test_we_short_and_hidden";
    cleanup_dir(dir);
    mkdir(dir, 0755);

    const char *proj = "{\"title\":\"Short & Hidden\",\"type\":\"scene\",\"file\":\"scene.json\"}";
    const char *scene = "{\"objects\":[]}";
    write_bytes_to_path("/tmp/test_we_short_and_hidden/project.json", proj, strlen(proj));
    write_bytes_to_path("/tmp/test_we_short_and_hidden/scene.json", scene, strlen(scene));

    /* 1..4 char extensionless filenames that previously triggered ent->d_name[-1] underflow */
    write_bytes_to_path("/tmp/test_we_short_and_hidden/a", "1", 1);
    write_bytes_to_path("/tmp/test_we_short_and_hidden/ab", "12", 2);
    write_bytes_to_path("/tmp/test_we_short_and_hidden/abc", "123", 3);
    write_bytes_to_path("/tmp/test_we_short_and_hidden/abcd", "1234", 4);

    /* Benign .hlsl / .glsl shaders must be discovered */
    const char *glsl_src = "void main() { }\n";
    write_bytes_to_path("/tmp/test_we_short_and_hidden/pass.glsl", glsl_src, strlen(glsl_src));
    write_bytes_to_path("/tmp/test_we_short_and_hidden/pass.hlsl", glsl_src, strlen(glsl_src));

    we_compat_report_t *rep = we_compat_report_alloc();
    assert(we_compat_scan_directory(scanner, dir, rep));
    assert(rep->tier == WE_TIER_2_TRANSPILED);

    /* Now drop a malicious .hidden.js with "http://x"; eval(1) string-comment trick */
    const char *sneaky_js = "const endpoint = \"http://example.com/api\"; eval(\"alert(1)\");\n";
    write_bytes_to_path("/tmp/test_we_short_and_hidden/.hidden.js", sneaky_js, strlen(sneaky_js));

    assert(we_compat_scan_directory(scanner, dir, rep));
    assert(rep->tier == WE_TIER_4_BLOCKED);
    assert(rep->compatibility_score == 0);

    bool found_script_violation = false;
    for (uint32_t i = 0; i < rep->issue_count; i++) {
        if (strcmp(rep->issues[i].code, "SECURITY_SCRIPT_VIOLATION") == 0 &&
            strstr(rep->issues[i].file_path, ".hidden.js") != NULL) {
            found_script_violation = true;
            break;
        }
    }
    assert(found_script_violation && "Must detect eval() after 'http://...' inside .hidden.js");

    /* Verify benign string literal containing 'eval' does NOT false-positive */
    const char *benign_str_js = "const label = \"http://example.com eval(1) fetch(2)\";\n"
                                "const tmpl = `https://example.com window.location`;\n";
    write_bytes_to_path("/tmp/test_we_short_and_hidden/.hidden.js", benign_str_js,
                        strlen(benign_str_js));
    assert(we_compat_scan_directory(scanner, dir, rep));
    assert(rep->tier != WE_TIER_4_BLOCKED);

    /* Verify template interpolation ${eval(1)} IS caught */
    const char *tmpl_evil_js = "const msg = `http://example.com ${fetch('https://bad')}`;\n";
    write_bytes_to_path("/tmp/test_we_short_and_hidden/.hidden.js", tmpl_evil_js,
                        strlen(tmpl_evil_js));
    assert(we_compat_scan_directory(scanner, dir, rep));
    assert(rep->tier == WE_TIER_4_BLOCKED);

    we_compat_report_free(rep);
    cleanup_dir(dir);
    printf("  -> 'abcd' bounds, .hidden.js, .glsl/.hlsl, and stateful JS lexer passed.\n");
}

static size_t build_two_entry_pkg(uint8_t *out, size_t cap, const char *p0, const void *d0,
                                  size_t l0, const char *p1, const void *d1, size_t l1)
{
    size_t p0_len = strlen(p0);
    size_t p1_len = strlen(p1);
    size_t need = 16u + (4u + p0_len + 8u) + (4u + p1_len + 8u) + l0 + l1;
    assert(need <= cap);

    size_t c = 0;
    uint32_t mlen = 8u;
    memcpy(out + c, &mlen, 4u);
    c += 4u;
    memcpy(out + c, "PKGV0001", 8u);
    c += 8u;
    uint32_t ec = 2u;
    memcpy(out + c, &ec, 4u);
    c += 4u;

    uint32_t u_p0 = (uint32_t)p0_len;
    uint32_t off0 = 0u;
    uint32_t len0 = (uint32_t)l0;
    memcpy(out + c, &u_p0, 4u);
    c += 4u;
    memcpy(out + c, p0, p0_len);
    c += p0_len;
    memcpy(out + c, &off0, 4u);
    c += 4u;
    memcpy(out + c, &len0, 4u);
    c += 4u;

    uint32_t u_p1 = (uint32_t)p1_len;
    uint32_t off1 = (uint32_t)l0;
    uint32_t len1 = (uint32_t)l1;
    memcpy(out + c, &u_p1, 4u);
    c += 4u;
    memcpy(out + c, p1, p1_len);
    c += p1_len;
    memcpy(out + c, &off1, 4u);
    c += 4u;
    memcpy(out + c, &len1, 4u);
    c += 4u;

    memcpy(out + c, d0, l0);
    c += l0;
    memcpy(out + c, d1, l1);
    c += l1;
    return c;
}

static void test_period8_and_period16_xor_pkg_quarantine(we_compat_scanner_t *scanner)
{
    printf("[Compat Scanner 5/9] Testing Period-8 and Period-16 XOR .pkg malware quarantine...\n");

    const char *scene_json = "{\"objects\":[]}";
    uint8_t pe_payload[64] = {'M', 'Z', 0x90, 0x00, 0x03, 0x00, 0x00, 0x00};

    uint8_t plain_pkg[512];
    size_t pkg_sz = build_two_entry_pkg(plain_pkg, sizeof(plain_pkg), "scene.json", scene_json,
                                        strlen(scene_json), "assets/camouflaged.png", pe_payload,
                                        sizeof(pe_payload));

    const uint8_t key8[8] = {0xA5, 0x3C, 0x71, 0xE2, 0x19, 0x84, 0x5B, 0xD0};
    /* Period-16 key intentionally repeats k[0..3] == k[8..11] with distinct k[12..15] */
    const uint8_t key16[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                               0x11, 0x22, 0x33, 0x44, 0xDE, 0xAD, 0xBE, 0xEF};

    const uint8_t *keys[2] = {key8, key16};
    const size_t periods[2] = {8u, 16u};

    for (int t = 0; t < 2; t++) {
        const char *dir = "/tmp/test_we_xor_pkg";
        cleanup_dir(dir);
        mkdir(dir, 0755);

        uint8_t masked[512];
        for (size_t i = 0; i < pkg_sz; i++) {
            masked[i] = (uint8_t)(plain_pkg[i] ^ keys[t][i % periods[t]]);
        }

        const char *proj =
            "{\"title\":\"Obfuscated PKG\",\"type\":\"scene\",\"file\":\"scene.json\"}";
        write_bytes_to_path("/tmp/test_we_xor_pkg/project.json", proj, strlen(proj));
        write_bytes_to_path("/tmp/test_we_xor_pkg/scene.pkg", masked, pkg_sz);

        we_compat_report_t *rep = we_compat_report_alloc();
        assert(we_compat_scan_directory(scanner, dir, rep));
        assert(rep->tier == WE_TIER_4_BLOCKED);
        assert(rep->compatibility_score == 0);

        bool demasked = false;
        bool quarantined = false;
        for (uint32_t i = 0; i < rep->issue_count; i++) {
            if (strcmp(rep->issues[i].code, "PKG_OBFUSCATION_DEMASKED") == 0) {
                demasked = true;
            }
            if (strcmp(rep->issues[i].code, "SECURITY_ALERT_MALICIOUS") == 0) {
                quarantined = true;
            }
        }
        assert(demasked && "Must detect and demask Period-8/16 rolling XOR .pkg");
        assert(quarantined && "Must quarantine PE payload inside Period-8/16 XOR .pkg");

        we_compat_report_free(rep);
        cleanup_dir(dir);
    }

    printf("  -> Period-8 and Period-16 rolling XOR .pkg quarantine passed.\n");
}

static void test_repair_path_confinement_and_symlinks(void)
{
    printf("[Compat Scanner 6/9] Testing openat2 repair confinement & symlink rejection...\n");

    const char *base = "/tmp/test_we_confinement";
    const char *src_dir = "/tmp/test_we_confinement/src";
    const char *dst_dir = "/tmp/test_we_confinement/dst";
    const char *secret_file = "/tmp/test_we_confinement/outside_secret.txt";

    cleanup_dir(base);
    mkdir(base, 0755);
    mkdir(src_dir, 0755);
    mkdir(dst_dir, 0755);

    const char *sentinel = "SENSITIVE_HOST_DATA_DO_NOT_OVERWRITE";
    write_bytes_to_path(secret_file, sentinel, strlen(sentinel));

    /* Symlink loop and external symlink inside src_dir must be skipped by copy_dir_recursive */
    assert(symlink(".", "/tmp/test_we_confinement/src/self_loop") == 0);
    assert(symlink(secret_file, "/tmp/test_we_confinement/src/ext_link.js") == 0);
    write_bytes_to_path("/tmp/test_we_confinement/src/legit.txt", "ok", 2);

    /* Symlink inside dst_dir pointing outside */
    assert(symlink(secret_file, "/tmp/test_we_confinement/dst/symlink_escape.js") == 0);

    we_compat_report_t *rep = we_compat_report_alloc();

    /* 1. Reject '../outside_secret.txt' traversal */
    rep->repair_count = 1;
    strcpy(rep->repairs[0].transformation_id, "REPAIR_STUB_SCRIPT");
    strcpy(rep->repairs[0].target_file, "../outside_secret.txt");
    assert(we_compat_apply_repairs(rep, dst_dir) == 0u);
    assert(!we_compat_apply_auto_repairs(rep, src_dir, dst_dir));

    /* 2. Reject absolute path '/tmp/test_we_confinement/outside_secret.txt' */
    strcpy(rep->repairs[0].target_file, secret_file);
    assert(we_compat_apply_repairs(rep, dst_dir) == 0u);

    /* 3. Reject symlink target inside dst_dir */
    strcpy(rep->repairs[0].target_file, "symlink_escape.js");
    assert(we_compat_apply_repairs(rep, dst_dir) == 0u);

    /* Verify sentinel file outside target_dir is 100% untouched */
    char *after = read_text_from_path(secret_file);
    assert(strcmp(after, sentinel) == 0);
    free(after);

    we_compat_report_free(rep);
    cleanup_dir(base);
    printf("  -> Path traversal ('../', absolute) and symlink repair confinement passed.\n");
}

static void load_dl_fn(void *handle, const char *sym, void *out_fn, size_t fn_size)
{
    void *raw = dlsym(handle, sym);
    memcpy(out_fn, &raw, fn_size);
}

static void maybe_verify_gles_shader_link(const char *vert_src, const char *frag_src)
{
#if defined(__SANITIZE_ADDRESS__)
    /* Mesa DRI / libdbus-1 retain process-global driver singletons across dlclose() */
    (void)vert_src;
    (void)frag_src;
    (void)load_dl_fn;
    return;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    (void)vert_src;
    (void)frag_src;
    (void)load_dl_fn;
    return;
#endif
#endif
    void *egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    void *gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!egl || !gles) {
        if (egl)
            dlclose(egl);
        if (gles)
            dlclose(gles);
        return;
    }

    typedef void *(*pfn_eglGetDisplay)(void *);
    typedef unsigned int (*pfn_eglInitialize)(void *, int *, int *);
    typedef unsigned int (*pfn_eglBindAPI)(unsigned int);
    typedef unsigned int (*pfn_eglChooseConfig)(void *, const int *, void **, int, int *);
    typedef void *(*pfn_eglCreateContext)(void *, void *, void *, const int *);
    typedef unsigned int (*pfn_eglMakeCurrent)(void *, void *, void *, void *);
    typedef unsigned int (*pfn_eglDestroyContext)(void *, void *);
    typedef unsigned int (*pfn_eglTerminate)(void *);

    typedef unsigned int (*pfn_glCreateShader)(unsigned int);
    typedef void (*pfn_glShaderSource)(unsigned int, int, const char *const *, const int *);
    typedef void (*pfn_glCompileShader)(unsigned int);
    typedef void (*pfn_glGetShaderiv)(unsigned int, unsigned int, int *);
    typedef unsigned int (*pfn_glCreateProgram)(void);
    typedef void (*pfn_glAttachShader)(unsigned int, unsigned int);
    typedef void (*pfn_glLinkProgram)(unsigned int);
    typedef void (*pfn_glGetProgramiv)(unsigned int, unsigned int, int *);
    typedef void (*pfn_glDeleteShader)(unsigned int);
    typedef void (*pfn_glDeleteProgram)(unsigned int);

    pfn_eglGetDisplay fn_get_dpy = NULL;
    pfn_eglInitialize fn_init = NULL;
    pfn_eglBindAPI fn_bind = NULL;
    pfn_eglChooseConfig fn_choose = NULL;
    pfn_eglCreateContext fn_ctx = NULL;
    pfn_eglMakeCurrent fn_curr = NULL;
    pfn_eglDestroyContext fn_dctx = NULL;
    pfn_eglTerminate fn_term = NULL;

    pfn_glCreateShader fn_cs = NULL;
    pfn_glShaderSource fn_ss = NULL;
    pfn_glCompileShader fn_comp = NULL;
    pfn_glGetShaderiv fn_gsi = NULL;
    pfn_glCreateProgram fn_cp = NULL;
    pfn_glAttachShader fn_att = NULL;
    pfn_glLinkProgram fn_lnk = NULL;
    pfn_glGetProgramiv fn_gpi = NULL;
    pfn_glDeleteShader fn_ds = NULL;
    pfn_glDeleteProgram fn_dp = NULL;

    load_dl_fn(egl, "eglGetDisplay", &fn_get_dpy, sizeof(fn_get_dpy));
    load_dl_fn(egl, "eglInitialize", &fn_init, sizeof(fn_init));
    load_dl_fn(egl, "eglBindAPI", &fn_bind, sizeof(fn_bind));
    load_dl_fn(egl, "eglChooseConfig", &fn_choose, sizeof(fn_choose));
    load_dl_fn(egl, "eglCreateContext", &fn_ctx, sizeof(fn_ctx));
    load_dl_fn(egl, "eglMakeCurrent", &fn_curr, sizeof(fn_curr));
    load_dl_fn(egl, "eglDestroyContext", &fn_dctx, sizeof(fn_dctx));
    load_dl_fn(egl, "eglTerminate", &fn_term, sizeof(fn_term));

    load_dl_fn(gles, "glCreateShader", &fn_cs, sizeof(fn_cs));
    load_dl_fn(gles, "glShaderSource", &fn_ss, sizeof(fn_ss));
    load_dl_fn(gles, "glCompileShader", &fn_comp, sizeof(fn_comp));
    load_dl_fn(gles, "glGetShaderiv", &fn_gsi, sizeof(fn_gsi));
    load_dl_fn(gles, "glCreateProgram", &fn_cp, sizeof(fn_cp));
    load_dl_fn(gles, "glAttachShader", &fn_att, sizeof(fn_att));
    load_dl_fn(gles, "glLinkProgram", &fn_lnk, sizeof(fn_lnk));
    load_dl_fn(gles, "glGetProgramiv", &fn_gpi, sizeof(fn_gpi));
    load_dl_fn(gles, "glDeleteShader", &fn_ds, sizeof(fn_ds));
    load_dl_fn(gles, "glDeleteProgram", &fn_dp, sizeof(fn_dp));

    if (fn_get_dpy && fn_init && fn_bind && fn_choose && fn_ctx && fn_curr && fn_cs && fn_ss &&
        fn_comp && fn_gsi && fn_cp && fn_att && fn_lnk && fn_gpi && fn_ds && fn_dp) {
        void *dpy = fn_get_dpy(NULL);
        int maj = 0, min = 0;
        if (dpy && fn_init(dpy, &maj, &min) && fn_bind(0x30A0u)) {
            const int cfg_attr[] = {0x3040, 0x0040, 0x3038};
            void *cfg = NULL;
            int ncfg = 0;
            const int ctx_attr[] = {0x3098, 3, 0x3038};
            if (fn_choose(dpy, cfg_attr, &cfg, 1, &ncfg) && ncfg > 0) {
                void *ctx = fn_ctx(dpy, cfg, NULL, ctx_attr);
                if (ctx && fn_curr(dpy, NULL, NULL, ctx)) {
                    unsigned int vs = fn_cs(0x8B31u);
                    unsigned int fs = fn_cs(0x8B30u);
                    fn_ss(vs, 1, &vert_src, NULL);
                    fn_comp(vs);
                    int v_ok = 0;
                    fn_gsi(vs, 0x8B81u, &v_ok);

                    fn_ss(fs, 1, &frag_src, NULL);
                    fn_comp(fs);
                    int f_ok = 0;
                    fn_gsi(fs, 0x8B81u, &f_ok);

                    unsigned int prog = fn_cp();
                    fn_att(prog, vs);
                    fn_att(prog, fs);
                    fn_lnk(prog);
                    int l_ok = 0;
                    fn_gpi(prog, 0x8B82u, &l_ok);

                    assert(v_ok == 1 && "Repaired vertex shader must pass glCompileShader");
                    assert(f_ok == 1 && "Repaired fragment shader must pass glCompileShader");
                    assert(l_ok == 1 && "Repaired shader pair must pass glLinkProgram");
                    printf("     [GLES] Verified glCompileShader + glLinkProgram on repaired "
                           "shaders.\n");

                    fn_dp(prog);
                    fn_ds(vs);
                    fn_ds(fs);
                    fn_curr(dpy, NULL, NULL, NULL);
                }
                if (ctx && fn_dctx)
                    fn_dctx(dpy, ctx);
            }
            if (fn_term)
                fn_term(dpy);
        }
    }

    dlclose(gles);
    dlclose(egl);
}

static void test_auto_repairs_transformation(we_compat_scanner_t *scanner)
{
    printf(
        "[Compat Scanner 7/9] Testing swizzle-safe varying promotion & user-prop synthesis...\n");

    const char *src_dir = "/tmp/test_we_repair_src";
    const char *dst_dir = "/tmp/test_we_repair_dst";
    cleanup_dir(src_dir);
    cleanup_dir(dst_dir);
    mkdir(src_dir, 0755);
    mkdir(dst_dir, 0755);

    const char *proj =
        "{\"title\":\"Auto Repair Target\",\"type\":\"scene\",\"file\":\"scene.json\"}\n";
    const char *scene = "{\"objects\":[{\"name\":\"layer1\",\"type\":\"image\",\"alpha\":{\"user\":"
                        "\"u_speed\"}}]}\n";
    const char *vert = "#version 310 es\n"
                       "precision highp float;\n"
                       "in vec4 a_Pos;\n"
                       "layout(location = 0) smooth mediump out vec2 v_TexCoord;\n"
                       "void main() {\n"
                       "    v_TexCoord = a_Pos.xy;\n"
                       "    v_TexCoord.x = v_TexCoord.x * u_speed;\n"
                       "    gl_Position = a_Pos;\n"
                       "}\n";
    const char *frag = "#version 310 es\n"
                       "precision mediump float;\n"
                       "layout(location = 0) smooth mediump in vec4 v_TexCoord;\n"
                       "out vec4 fragColor;\n"
                       "void main() {\n"
                       "    vec2 uv = v_TexCoord.xy;\n"
                       "    fragColor = vec4(uv, v_TexCoord.z, 1.0);\n"
                       "}\n";

    write_bytes_to_path("/tmp/test_we_repair_src/project.json", proj, strlen(proj));
    write_bytes_to_path("/tmp/test_we_repair_src/scene.json", scene, strlen(scene));
    write_bytes_to_path("/tmp/test_we_repair_src/effect.vert", vert, strlen(vert));
    write_bytes_to_path("/tmp/test_we_repair_src/effect.frag", frag, strlen(frag));

    we_compat_report_t *rep = we_compat_report_alloc();
    assert(we_compat_scan_directory(scanner, src_dir, rep));
    assert(rep->repair_count >= 2u);

    bool repair_ok = we_compat_apply_auto_repairs(rep, src_dir, dst_dir);
    assert(repair_ok && "we_compat_apply_auto_repairs must succeed");

    char *rep_proj = read_text_from_path("/tmp/test_we_repair_dst/project.json");
    char *rep_vert = read_text_from_path("/tmp/test_we_repair_dst/effect.vert");
    char *rep_frag = read_text_from_path("/tmp/test_we_repair_dst/effect.frag");

    assert(strstr(rep_proj, "\"u_speed\"") != NULL);
    assert(strstr(rep_vert, "uniform float u_speed;") != NULL);
    assert(strstr(rep_vert, "vec4 _wwal_v4_v_TexCoord;") != NULL);
    assert(strstr(rep_vert, "_wwal_v4_v_TexCoord.xy = a_Pos.xy;") != NULL);
    assert(strstr(rep_vert, "_wwal_v4_v_TexCoord.x") != NULL);
    assert(strstr(rep_frag, "vec4 _wwal_v4_v_TexCoord;") != NULL);

    maybe_verify_gles_shader_link(rep_vert, rep_frag);

    free(rep_proj);
    free(rep_vert);
    free(rep_frag);

    /* Re-scan dst_dir: both SHADER_VARYING_MISMATCH and MISSING_USER_PROPERTY must be resolved */
    we_compat_report_t *rep_after = we_compat_report_alloc();
    assert(we_compat_scan_directory(scanner, dst_dir, rep_after));
    for (uint32_t i = 0; i < rep_after->issue_count; i++) {
        assert(strcmp(rep_after->issues[i].code, "SHADER_VARYING_MISMATCH") != 0);
        assert(strcmp(rep_after->issues[i].code, "MISSING_USER_PROPERTY") != 0);
    }

    we_compat_report_free(rep_after);
    we_compat_report_free(rep);
    cleanup_dir(src_dir);
    cleanup_dir(dst_dir);

    printf("  -> Swizzle-safe varying promotion & user-prop synthesis passed.\n");
}

static void test_non_nul_pkg_slices(we_compat_scanner_t *scanner)
{
    printf("[Compat Scanner 8/9] Testing bounded memmem/memchr on non-NUL .pkg slices...\n");

    const char *dir = "/tmp/test_we_pkg_slices";
    cleanup_dir(dir);
    mkdir(dir, 0755);

    const char *proj = "{\"title\":\"Slice Bounds\",\"type\":\"scene\",\"file\":\"scene.json\"}";
    const char *scene = "{\"objects\":[{\"name\":\"s\",\"alpha\":{\"user\":\"missing_prop\"}}]}";
    const char *frag = "varying vec2 v;\nvoid main() { float a = g_AudioSpectrum16[0]; }";

    uint8_t pkg[512];
    size_t sz = build_two_entry_pkg(pkg, sizeof(pkg), "scene.json", scene, strlen(scene),
                                    "audio.frag", frag, strlen(frag));

    write_bytes_to_path("/tmp/test_we_pkg_slices/project.json", proj, strlen(proj));
    write_bytes_to_path("/tmp/test_we_pkg_slices/scene.pkg", pkg, sz);

    we_compat_report_t *rep = we_compat_report_alloc();
    assert(we_compat_scan_directory(scanner, dir, rep));
    assert(rep->tier == WE_TIER_2_TRANSPILED);

    bool found_missing = false;
    for (uint32_t i = 0; i < rep->issue_count; i++) {
        if (strcmp(rep->issues[i].code, "MISSING_USER_PROPERTY") == 0) {
            found_missing = true;
            break;
        }
    }
    assert(found_missing);

    we_compat_report_free(rep);
    cleanup_dir(dir);
    printf("  -> Non-NUL .pkg slice scanning passed cleanly under ASan.\n");
}

static void test_report_json_serialization(we_compat_scanner_t *scanner)
{
    printf(
        "[Compat Scanner 9/9] Testing report JSON serialization (we_compat_report_to_json)...\n");

    const char *sample_dir = "/tmp/test_we_json_sample";
    cleanup_dir(sample_dir);
    mkdir(sample_dir, 0755);

    const char *pf_str = "{\"title\":\"JSON Serialization Test\",\"type\":\"video\"}\n";
    write_bytes_to_path("/tmp/test_we_json_sample/project.json", pf_str, strlen(pf_str));

    we_compat_report_t *rep = we_compat_report_alloc();
    we_compat_scan_directory(scanner, sample_dir, rep);

    char *json_str = we_compat_report_to_json(rep);
    assert(json_str != NULL && "we_compat_report_to_json must return valid JSON string");
    assert(strstr(json_str, "JSON Serialization Test") != NULL);
    assert(strstr(json_str, "tier") != NULL);
    assert(strstr(json_str, "compatibility_score") != NULL);

    free(json_str);
    we_compat_report_free(rep);
    cleanup_dir(sample_dir);

    printf("  -> Report JSON serialization passed.\n");
}

static void test_struct_field_varying_locale_and_merged_config(we_compat_scanner_t *scanner)
{
    printf("[Compat Scanner 10/10] Testing struct-member varying safety, C-locale JSON & merged "
           "compat config...\n");

    const char *src_dir = "/tmp/test_we_struct_merge_src";
    const char *dst_dir = "/tmp/test_we_struct_merge_dst";
    cleanup_dir(src_dir);
    cleanup_dir(dst_dir);
    mkdir(src_dir, 0755);
    mkdir(dst_dir, 0755);

    const char *proj =
        "{\"title\":\"Struct & Merge\",\"type\":\"scene\",\"file\":\"scene.json\"}\n";
    const char *scene = "{\"objects\":[{\"name\":\"puppet\",\"puppet\":\"model.mdl\"}]}\n";
    const char *vert = "#version 310 es\n"
                       "precision highp float;\n"
                       "struct Surface { vec2 v_TexCoord; };\n"
                       "in vec4 a_Pos;\n"
                       "layout(location = 0) out vec2 v_TexCoord;\n"
                       "void main() {\n"
                       "    Surface s;\n"
                       "    s.v_TexCoord = a_Pos.xy;\n"
                       "    v_TexCoord = s.v_TexCoord;\n"
                       "    gl_Position = a_Pos;\n"
                       "}\n";
    const char *frag = "#version 310 es\n"
                       "precision mediump float;\n"
                       "struct Surface { vec2 v_TexCoord; };\n"
                       "layout(location = 0) in vec4 v_TexCoord;\n"
                       "out vec4 fragColor;\n"
                       "void main() {\n"
                       "    Surface s;\n"
                       "    s.v_TexCoord = v_TexCoord.xy;\n"
                       "    fragColor = vec4(s.v_TexCoord, v_TexCoord.z, 1.0);\n"
                       "}\n";

    write_bytes_to_path("/tmp/test_we_struct_merge_src/project.json", proj, strlen(proj));
    write_bytes_to_path("/tmp/test_we_struct_merge_src/scene.json", scene, strlen(scene));
    write_bytes_to_path("/tmp/test_we_struct_merge_src/item.vert", vert, strlen(vert));
    write_bytes_to_path("/tmp/test_we_struct_merge_src/item.frag", frag, strlen(frag));

    /* Synthesize a TEX header with BC7 format + >128 bone puppet header so both config flags fire
     */
    uint8_t bc7_tex[64] = {'T', 'E', 'X', 'V', '0', '0', '0', '5', 0, 'T', 'E', 'X',
                           'I', '0', '0', '0', '1', 0,   15,  0,   0, 0,   0,   0,
                           0,   0,   16,  0,   0,   0,   16,  0,   0, 0};
    uint8_t dummy_mdl[32] = {'M', 'D', 'L', 'V', '0', '0', '0', '5', 0};
    write_bytes_to_path("/tmp/test_we_struct_merge_src/albedo.tex", bc7_tex, sizeof(bc7_tex));
    write_bytes_to_path("/tmp/test_we_struct_merge_src/model.mdl", dummy_mdl, sizeof(dummy_mdl));

    we_compat_report_t *rep = we_compat_report_alloc();
    we_compat_scanner_t *sw_scanner = we_compat_scanner_create(false, false);
    assert(we_compat_scan_directory(sw_scanner, src_dir, rep));
    we_compat_scanner_destroy(sw_scanner);

    /* Inject both config-writing repairs to verify non-clobber merge */
    if (rep->repair_count + 2u <= WE_SCAN_MAX_REPAIRS) {
        we_auto_repair_t *r0 = &rep->repairs[rep->repair_count++];
        snprintf(r0->transformation_id, sizeof(r0->transformation_id),
                 "REPAIR_BC7_SOFTWARE_FALLBACK");
        snprintf(r0->target_file, sizeof(r0->target_file), "albedo.tex");
        snprintf(r0->description, sizeof(r0->description), "Enable BC7 software fallback");

        we_auto_repair_t *r1 = &rep->repairs[rep->repair_count++];
        snprintf(r1->transformation_id, sizeof(r1->transformation_id), "REPAIR_CLAMP_BONES");
        snprintf(r1->target_file, sizeof(r1->target_file), "model.mdl");
        snprintf(r1->description, sizeof(r1->description), "Clamp puppet bones to 128");
    }

    /* Verify locale-independent '.' decimal serialization even under comma LC_NUMERIC */
    const char *locales[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8"};
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]); ++i) {
        if (setlocale(LC_NUMERIC, locales[i]) != NULL) {
            break;
        }
    }
    rep->summary.estimated_vram_mb = 12.75f;
    char *json_str = we_compat_report_to_json(rep);
    setlocale(LC_NUMERIC, "C");
    assert(json_str != NULL);
    assert(strstr(json_str, "12.75") != NULL);
    assert(strstr(json_str, "12,75") == NULL);
    free(json_str);

    assert(we_compat_apply_auto_repairs(rep, src_dir, dst_dir));

    char *rep_vert = read_text_from_path("/tmp/test_we_struct_merge_dst/item.vert");
    char *rep_frag = read_text_from_path("/tmp/test_we_struct_merge_dst/item.frag");
    char *compat_cfg = read_text_from_path("/tmp/test_we_struct_merge_dst/.waywal_compat.json");

    /* No #define macro emitted; struct member s.v_TexCoord remains intact and compiles under GLES
     */
    assert(strstr(rep_vert, "#define v_TexCoord") == NULL);
    assert(strstr(rep_vert, "struct Surface { vec2 v_TexCoord; };") != NULL);
    assert(strstr(rep_vert, "s.v_TexCoord = a_Pos.xy;") != NULL);
    maybe_verify_gles_shader_link(rep_vert, rep_frag);

    /* Both compat flags must survive in .waywal_compat.json without overwriting one another */
    assert(strstr(compat_cfg, "\"bc7_software_fallback\": true") != NULL);
    assert(strstr(compat_cfg, "\"clamp_puppet_bones\": 128") != NULL);

    free(rep_vert);
    free(rep_frag);
    free(compat_cfg);
    we_compat_report_free(rep);
    cleanup_dir(src_dir);
    cleanup_dir(dst_dir);
    (void)scanner;

    printf("  -> Struct-member varying safety, C-locale JSON & merged compat config passed.\n");
}

int main(void)
{
    printf("==========================================================\n");
    printf("  Wallpaper Engine Capability Scanner Matrix (§7.2)       \n");
    printf("==========================================================\n");

    test_heap_report_lifecycle();

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    assert(scanner != NULL && "we_compat_scanner_create must succeed");

    test_pe_elf_malware_quarantine(scanner);
    test_100_package_synthetic_matrix(scanner);
    test_short_filenames_and_dotfiles(scanner);
    test_period8_and_period16_xor_pkg_quarantine(scanner);
    test_repair_path_confinement_and_symlinks();
    test_auto_repairs_transformation(scanner);
    test_non_nul_pkg_slices(scanner);
    test_report_json_serialization(scanner);
    test_struct_field_varying_locale_and_merged_config(scanner);

    we_compat_scanner_destroy(scanner);

    printf("==========================================================\n");
    printf("  >>> ALL 10 CAPABILITY SCANNER TESTS PASSED (100%%) <<<   \n");
    printf("==========================================================\n");

    return 0;
}
