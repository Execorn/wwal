// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli_parser.h"
#include "waywal/audio_fft.h"
#include "waywal/dmabuf.h"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/mpris.h"
#include "waywal/palette_hct.h"
#include "waywal/shader_engine.h"
#include "waywal/toml_parser.h"
#include "waywal/we_json.h"
#include "waywal/we_particle.h"
#include "waywal/we_pkg.h"
#include "waywal/we_puppet.h"
#include "waywal/we_scene.h"
#include "waywal/we_script.h"
#include "waywal/we_shader.h"
#include "waywal/we_tex.h"

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * Binary Serialization Helpers for Synthetic PKGV / TEXV / MDLV Containers
 * ========================================================================= */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_buf_t;

static void bb_init(byte_buf_t *b) { memset(b, 0, sizeof(*b)); }

static void bb_free(byte_buf_t *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static void bb_append(byte_buf_t *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t ncap = b->cap == 0 ? 256 : b->cap * 2;
        while (ncap < b->len + n) {
            ncap *= 2;
        }
        uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
        assert(nd != NULL);
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void bb_u16(byte_buf_t *b, uint16_t v) { bb_append(b, &v, 2); }

static void bb_u32(byte_buf_t *b, uint32_t v) { bb_append(b, &v, 4); }

static void bb_i32(byte_buf_t *b, int32_t v) { bb_append(b, &v, 4); }

static void bb_f32(byte_buf_t *b, float v) { bb_append(b, &v, 4); }

static void bb_cstr(byte_buf_t *b, const char *s) { bb_append(b, s, strlen(s) + 1u); }

static void bb_str_len32(byte_buf_t *b, const char *s)
{
    uint32_t slen = (uint32_t)strlen(s);
    bb_u32(b, slen);
    bb_append(b, s, slen);
}

static void bb_magic9(byte_buf_t *b, const char *m8)
{
    assert(strlen(m8) == 8);
    bb_append(b, m8, 9); /* includes trailing NUL */
}

static void bb_magic8_nonul(byte_buf_t *b, const char *m8)
{
    assert(strlen(m8) == 8);
    bb_append(b, m8, 8); /* 8 bytes without trailing NUL */
}

typedef struct {
    const char *path;
    const void *data;
    size_t len;
} pkg_test_item_t;

static void build_synthetic_pkg(byte_buf_t *out, const char *version_magic,
                                const pkg_test_item_t *items, size_t count)
{
    bb_init(out);
    bb_str_len32(out, version_magic);
    bb_u32(out, (uint32_t)count);

    uint32_t cur_off = 0;
    for (size_t i = 0; i < count; ++i) {
        bb_str_len32(out, items[i].path);
        bb_u32(out, cur_off);
        bb_u32(out, (uint32_t)items[i].len);
        cur_off += (uint32_t)items[i].len;
    }
    for (size_t i = 0; i < count; ++i) {
        bb_append(out, items[i].data, items[i].len);
    }
}

/* =========================================================================
 * 1. scene.pkg VFS Unit Tests
 * ========================================================================= */

static void test_pkg_vfs(void)
{
    printf("  [TEST] Testing scene.pkg VFS (PKGV0001..PKGV0022, FNV-1a, path normalization)...\n");

    char norm[256];
    assert(we_pkg_normalize_path("\\Materials\\Models//Bg.json", 26, norm, sizeof(norm)));
    assert(strcmp(norm, "materials/models/bg.json") == 0);
    assert(we_pkg_normalize_path("./shaders/effects/bloom.frag", 28, norm, sizeof(norm)));
    assert(strcmp(norm, "shaders/effects/bloom.frag") == 0);
    assert(!we_pkg_normalize_path("../etc/passwd", 13, norm, sizeof(norm)));
    assert(!we_pkg_normalize_path("materials/../../secret.txt", 26, norm, sizeof(norm)));

    const char *f1 = "{\"general\":{\"orthogonalprojection\":{\"width\":1920,\"height\":1080}}}";
    const char *f2 = "void main() { gl_FragColor = vec4(1.0); }";
    const pkg_test_item_t items[] = {
        {.path = "scene.json", .data = f1, .len = strlen(f1)},
        {.path = "Shaders\\Custom\\Test.frag", .data = f2, .len = strlen(f2)},
    };

    const char *versions[] = {"PKGV0001", "PKGV0002", "PKGV0014", "PKGV0022"};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); ++v) {
        byte_buf_t pkg_buf;
        build_synthetic_pkg(&pkg_buf, versions[v], items, 2);

        we_pkg_t *pkg = we_pkg_open_mem(pkg_buf.data, pkg_buf.len, NULL);
        assert(pkg != NULL);
        assert(pkg->entry_count == 2);
        assert(strcmp(pkg->version_magic, versions[v]) == 0);

        size_t sz = 0;
        const uint8_t *d1 = we_pkg_find(pkg, "SCENE.JSON", &sz);
        assert(d1 != NULL && sz == strlen(f1));
        assert(memcmp(d1, f1, sz) == 0);

        const uint8_t *d2 = we_pkg_find(pkg, "/shaders/custom/test.frag", &sz);
        assert(d2 != NULL && sz == strlen(f2));
        assert(memcmp(d2, f2, sz) == 0);

        assert(we_pkg_find(pkg, "nonexistent.txt", &sz) == NULL);

        /* Test we_pkg_resolve zero-copy lookup + Layer 4 built-in shader fallback */
        bool owns = false;
        const uint8_t *res1 = we_pkg_resolve(pkg, "SCENE.JSON", &sz, &owns);
        assert(res1 != NULL && sz == strlen(f1) && !owns);
        const uint8_t *res_layer4 = we_pkg_resolve(pkg, "common.h", &sz, &owns);
        assert(res_layer4 != NULL && sz > 0 && !owns);
        assert(strstr((const char *)res_layer4, "rgb2hsv") != NULL);
        const uint8_t *res_vert4 = we_pkg_resolve(pkg, "shaders/genericimage2.vert", &sz, &owns);
        assert(res_vert4 != NULL && sz > 0 && !owns);

        we_pkg_close(pkg);
        bb_free(&pkg_buf);
    }

    /* Long path (> 256 bytes, up to 1024 bytes) in PKGV archive */
    {
        char long_path[320];
        memset(long_path, 'a', 300);
        memcpy(long_path, "materials/deep/", 15);
        strcpy(long_path + 300, ".json");
        const char *payload = "{\"long\":true}";
        const pkg_test_item_t long_item = {
            .path = long_path, .data = payload, .len = strlen(payload)};
        byte_buf_t lbuf;
        build_synthetic_pkg(&lbuf, "PKGV0018", &long_item, 1);
        we_pkg_t *lpkg = we_pkg_open_mem(lbuf.data, lbuf.len, NULL);
        assert(lpkg != NULL && lpkg->entry_count == 1);
        size_t lsz = 0;
        const uint8_t *ldata = we_pkg_find(lpkg, long_path, &lsz);
        assert(ldata != NULL && lsz == strlen(payload));
        we_pkg_close(lpkg);
        bb_free(&lbuf);
    }

    /* Malformed / out-of-bounds PKGV rejection + UINT32_MAX overflow rejection */
    byte_buf_t bad;
    bb_init(&bad);
    bb_str_len32(&bad, "PKGV0001");
    bb_u32(&bad, 1);
    bb_str_len32(&bad, "evil.bin");
    bb_u32(&bad, 0xFFFFFFF0u);
    bb_u32(&bad, 0x00000020u); /* 32-bit wrap-around overflow */
    assert(we_pkg_open_mem(bad.data, bad.len, NULL) == NULL);
    bb_free(&bad);
}

/* =========================================================================
 * 2. .tex Container & LZ4 / DXT1 / DXT3 / DXT5 / BC7 Tests
 * ========================================================================= */

static void test_tex_loader(void)
{
    printf("  [TEST] Testing .tex container loader (TEXV/TEXI/TEXB/TEXS, LZ4, BC1/2/3/7)...\n");

    /* Dual-mode magic reader test (9-byte NUL-padded, 8-byte unpadded, and [u32 len=8]["TEX..."])
     */
    byte_buf_t mb;
    bb_init(&mb);
    bb_magic9(&mb, "TEXV0003");
    bb_magic8_nonul(&mb, "TEXI0001");
    bb_str_len32(&mb, "TEXV0005");
    bb_u32(&mb, 8);
    bb_magic9(&mb, "TEXB0004"); /* [u32 len=8]["TEXB0004\0"] with trailing NUL */
    size_t pos = 0;
    char m_out[16];
    assert(we_tex_read_magic(mb.data, mb.len, &pos, m_out) && strcmp(m_out, "TEXV0003") == 0);
    assert(pos == 9);
    assert(we_tex_read_magic(mb.data, mb.len, &pos, m_out) && strcmp(m_out, "TEXI0001") == 0);
    assert(pos == 17);
    assert(we_tex_read_magic(mb.data, mb.len, &pos, m_out) && strcmp(m_out, "TEXV0005") == 0);
    assert(pos == 29);
    assert(we_tex_read_magic(mb.data, mb.len, &pos, m_out) && strcmp(m_out, "TEXB0004") == 0);
    assert(pos == 42);
    bb_free(&mb);

    /* Unpadded 8-byte "TEXI0001" immediately followed by format = 0 (WE_TEX_FMT_RGBA8888,
     * first byte 0x00) must NOT eat the first byte of format! */
    {
        byte_buf_t ub;
        bb_init(&ub);
        bb_magic8_nonul(&ub, "TEXV0003");
        bb_magic8_nonul(&ub, "TEXI0001");
        bb_u32(&ub, WE_TEX_FMT_RGBA8888); /* 0x00000000 */
        bb_u32(&ub, 0);
        bb_u32(&ub, 1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 0);
        bb_magic8_nonul(&ub, "TEXB0003");
        bb_u32(&ub, 1);
        bb_i32(&ub, -1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 1);
        bb_u32(&ub, 0);
        bb_u32(&ub, 0);
        bb_i32(&ub, 4);
        const uint8_t px1[4] = {12, 34, 56, 255};
        bb_append(&ub, px1, 4);

        we_tex_t *utex = we_tex_load(ub.data, ub.len, false);
        assert(utex != NULL);
        assert(utex->format == WE_TEX_FMT_RGBA8888 && utex->texture_width == 1 &&
               utex->texture_height == 1);
        uint8_t *udec = NULL;
        uint32_t uw = 0, uh = 0;
        assert(we_tex_decode_rgba8(utex, &udec, &uw, &uh) && udec != NULL);
        assert(udec[0] == 12 && udec[1] == 34 && udec[2] == 56 && udec[3] == 255);
        free(udec);
        we_tex_destroy(utex);
        bb_free(&ub);
    }

    /* LZ4 safe block decompressor test:
     * 1. Tiny block (pure-C fallback): Token 0x40 -> 4 literal bytes ("ABCD"), match length 0+4=4
     *    with offset=4 -> "ABCDABCD", followed by final token 0x50 -> 5 literal bytes ("ABCDE").
     * 2. Standard MFLIMIT-compliant block (liblz4.so.1 fast path): Token 0x40 ("ABCD"), match 4
     *    at offset=4 ("ABCD"), followed by final token 0xC0 -> 12 literal bytes ("0123456789AB").
     */
    const uint8_t lz4_block[] = {0x40, 'A', 'B', 'C', 'D', 0x04, 0x00,
                                 0x50, 'A', 'B', 'C', 'D', 'E'};
    uint8_t lz4_out[13] = {0};
    assert(we_lz4_decompress_safe(lz4_block, sizeof(lz4_block), lz4_out, sizeof(lz4_out)) == 13);
    assert(memcmp(lz4_out, "ABCDABCDABCDE", 13) == 0);

    const uint8_t lz4_std[] = {0x40, 'A', 'B', 'C', 'D', 0x04, 0x00, 0xC0, '0', '1',
                               '2',  '3', '4', '5', '6', '7',  '8',  '9',  'A', 'B'};
    uint8_t lz4_std_out[20] = {0};
    assert(we_lz4_decompress_safe(lz4_std, sizeof(lz4_std), lz4_std_out, sizeof(lz4_std_out)) ==
           20);
    assert(memcmp(lz4_std_out, "ABCDABCD0123456789AB", 20) == 0);

    /* Out-of-bounds LZ4 offset and > 256 MiB decomp_size must fail safely */
    const uint8_t lz4_bad[] = {0x11, 'X', 0x10, 0x00};
    assert(we_lz4_decompress_safe(lz4_bad, sizeof(lz4_bad), lz4_out, sizeof(lz4_out)) < 0);
    assert(we_lz4_decompress_safe(lz4_block, sizeof(lz4_block), lz4_out,
                                  WE_TEX_MAX_DECOMP_SIZE + 1u) < 0);

    /* DXT1 (BC1) 4x4 block decode test: pure red (0xF800) on all 16 pixels */
    const uint8_t dxt1_red[8] = {0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rgba4x4[4 * 4 * 4] = {0};
    assert(we_decode_dxt1_rgba8(dxt1_red, sizeof(dxt1_red), 4, 4, rgba4x4));
    assert(rgba4x4[0] == 255 && rgba4x4[1] == 0 && rgba4x4[2] == 0 && rgba4x4[3] == 255);

    /* DXT3 (BC2) 4x4 block decode test: 4-bit alpha 0xF (255) on pixel 0, pure blue (0x001F) */
    const uint8_t dxt3_blue[16] = {
        0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* explicit 4-bit alpha block */
        0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* color block */
    };
    memset(rgba4x4, 0, sizeof(rgba4x4));
    assert(we_decode_dxt3_rgba8(dxt3_blue, sizeof(dxt3_blue), 4, 4, rgba4x4));
    assert(rgba4x4[0] == 0 && rgba4x4[1] == 0 && rgba4x4[2] == 255 && rgba4x4[3] == 255);
    assert(rgba4x4[4] == 0 && rgba4x4[5] == 0 && rgba4x4[6] == 255 && rgba4x4[7] == 0);

    /* DXT5 (BC3) 4x4 block decode test: alpha0=128, alpha1=128, pure green (0x07E0) */
    const uint8_t dxt5_green[16] = {
        128,  128,  0,    0,    0,    0,    0,    0,   /* alpha block */
        0xE0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 /* color block */
    };
    memset(rgba4x4, 0, sizeof(rgba4x4));
    assert(we_decode_dxt5_rgba8(dxt5_green, sizeof(dxt5_green), 4, 4, rgba4x4));
    assert(rgba4x4[0] == 0 && rgba4x4[1] == 255 && rgba4x4[2] == 0 && rgba4x4[3] == 128);

    /* BC7 Mode 6 (bit 6 = 0x40): 1 subset, 7-bit RGBA endpoints + p-bits + 4-bit indices.
     * Construct a Mode 6 block where endpoint 0 and endpoint 1 are both (255, 0, 255, 255)
     * and all indices are 0: */
    uint8_t bc7_blk[16];
    memset(bc7_blk, 0, sizeof(bc7_blk));
    uint64_t lo = (1ULL << 6) | (0x7FULL << 7) | (0x7FULL << 14) | (0x7FULL << 35) |
                  (0x7FULL << 42) | (0x7FULL << 49) | (0x7FULL << 56) | (1ULL << 63);
    uint64_t hi = 1ULL; /* P1 = 1, all 4-bit indices = 0 */
    memcpy(bc7_blk, &lo, 8);
    memcpy(bc7_blk + 8, &hi, 8);
    memset(rgba4x4, 0, sizeof(rgba4x4));
    assert(we_decode_bc7_rgba8(bc7_blk, sizeof(bc7_blk), 4, 4, rgba4x4));
    assert(rgba4x4[0] == 255 && rgba4x4[1] == 1 && rgba4x4[2] == 255 && rgba4x4[3] == 255);

    /* Synthetic .tex container (TEXV0003 + TEXI0001 + TEXB0003 + TEXS0003) with RG88 format */
    byte_buf_t tb;
    bb_init(&tb);
    bb_magic9(&tb, "TEXV0003");
    bb_magic9(&tb, "TEXI0001");
    bb_u32(&tb, WE_TEX_FMT_RG88);
    bb_u32(&tb, WE_TEX_FLAG_CLAMP_UVS | WE_TEX_FLAG_IS_GIF);
    bb_u32(&tb, 2); /* tex_width */
    bb_u32(&tb, 2); /* tex_height */
    bb_u32(&tb, 2); /* img_width */
    bb_u32(&tb, 2); /* img_height */
    bb_u32(&tb, 0); /* unk_int0 */
    bb_magic9(&tb, "TEXB0003");
    bb_u32(&tb, 1);  /* image_count */
    bb_i32(&tb, -1); /* free_image_format = -1 (raw) */
    bb_u32(&tb, 1);  /* mipmap_count */
    bb_u32(&tb, 2);  /* mip width */
    bb_u32(&tb, 2);  /* mip height */
    bb_u32(&tb, 0);  /* lz4_compressed = 0 */
    bb_u32(&tb, 0);  /* decompressed_size */
    bb_i32(&tb, 8);  /* byte_count = 2*2*2 = 8 bytes */
    const uint8_t rg_px[8] = {10, 200, 20, 210, 30, 220, 40, 230};
    bb_append(&tb, rg_px, 8);
    /* Append TEXS0003 spritesheet section */
    bb_magic9(&tb, "TEXS0003");
    bb_u32(&tb, 2); /* 2 frames */
    bb_u32(&tb, 2); /* gif_width */
    bb_u32(&tb, 2); /* gif_height */
    /* Frame 0 */
    bb_u32(&tb, 0);
    bb_f32(&tb, 0.5f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 1.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 1.0f);
    /* Frame 1 */
    bb_u32(&tb, 0);
    bb_f32(&tb, 0.5f);
    bb_f32(&tb, 1.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 1.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 0.0f);
    bb_f32(&tb, 1.0f);

    we_tex_t *tex = we_tex_load(tb.data, tb.len, false);
    assert(tex != NULL);
    assert(tex->format == WE_TEX_FMT_RG88);
    assert((tex->flags & WE_TEX_FLAG_IS_GIF) != 0 && tex->frame_count == 2);

    float uv_tr[2] = {0}, uv_rot[4] = {0};
    assert(we_tex_get_frame_transform(tex, 0, uv_tr, uv_rot));
    assert(fabsf(uv_tr[0] - 0.0f) < 1e-5f && fabsf(uv_rot[0] - 0.5f) < 1e-5f);
    assert(we_tex_get_frame_transform(tex, 1, uv_tr, uv_rot));
    assert(fabsf(uv_tr[0] - 0.5f) < 1e-5f && fabsf(uv_rot[0] - 0.5f) < 1e-5f);

    uint8_t *dec_rgba = NULL;
    uint32_t dw = 0, dh = 0;
    assert(we_tex_decode_rgba8(tex, &dec_rgba, &dw, &dh));
    assert(dec_rgba != NULL && dw == 2 && dh == 2);
    /* RG88 expands to R=r, G=r, B=r, A=g */
    assert(dec_rgba[0] == 10 && dec_rgba[1] == 10 && dec_rgba[2] == 10 && dec_rgba[3] == 200);
    free(dec_rgba);

    we_tex_destroy(tex);
    bb_free(&tb);

    /* Embedded MP4 video .tex container detection (TEXB0004 + ftyp box) */
    {
        byte_buf_t vb;
        bb_init(&vb);
        bb_magic9(&vb, "TEXV0004");
        bb_magic9(&vb, "TEXI0001");
        bb_u32(&vb, WE_TEX_FMT_RGBA8888);
        bb_u32(&vb, WE_TEX_FLAG_IS_VIDEO_TEXTURE);
        bb_u32(&vb, 64);
        bb_u32(&vb, 64);
        bb_u32(&vb, 64);
        bb_u32(&vb, 64);
        bb_u32(&vb, 0);
        bb_magic9(&vb, "TEXB0004");
        bb_u32(&vb, 1);
        bb_i32(&vb, WE_FIF_MP4); /* mp4 freeimage format */
        bb_u32(&vb, 1);          /* is_video_mp4 = 1 */
        bb_u32(&vb, 1);
        bb_u32(&vb, 64);
        bb_u32(&vb, 64);
        bb_u32(&vb, 0);
        bb_u32(&vb, 0);
        const uint8_t fake_mp4[16] = {0,   0,   0,   16,  'f', 't', 'y', 'p',
                                      'i', 's', 'o', 'm', 0,   0,   0,   0};
        bb_i32(&vb, (int32_t)sizeof(fake_mp4));
        bb_append(&vb, fake_mp4, sizeof(fake_mp4));

        we_tex_t *vtex = we_tex_load(vb.data, vb.len, false);
        assert(vtex != NULL);
        assert(vtex->is_video_mp4);
        (void)we_tex_advance_video(vtex, 0.016f);
        we_tex_destroy(vtex);
        bb_free(&vb);
    }
}

/* =========================================================================
 * 3. Wallpaper Engine GLSL/HLSL Shader Preprocessor Tests
 * ========================================================================= */

static void test_shader_preprocessor(void)
{
    printf("  [TEST] Testing GLSL/HLSL shader preprocessor (combos, #include, varying "
           "promotion)...\n");

    const char *vert_src =
        "// [COMBO] {\"material\":\"ui_mode\",\"combo\":\"MODE\",\"default\":1}\n"
        "#include \"common_vertex.h\"\n"
        "attribute vec3 a_Position;\n"
        "attribute vec2 a_TexCoord;\n"
        "varying vec4 v_TexCoord;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "void main() {\n"
        "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
        "    v_TexCoord = vec4(a_TexCoord, 0.0, 1.0);\n"
        "}\n";

    const char *frag_src =
        "// [COMBO] {\"material\":\"ui_blend\",\"combo\":\"BLENDMODE\",\"default\":0}\n"
        "#include \"common.h\"\n"
        "#include \"common_blending.h\"\n"
        "varying vec2 v_TexCoord;\n"
        "uniform sampler2D g_Texture0;\n"
        "uniform sampler2D g_Texture1; // {\"material\":\"mask\",\"combo\":\"THETEXTURE1\"}\n"
        "uniform float u_Gain; // {\"material\":\"gain\",\"default\":2.5}\n"
        "uniform vec3 u_Tint; // {\"material\":\"tint\",\"default\":\"0.25 0.5 0.75\"}\n"
        "void main() {\n"
        "    vec2 ruv = rotateVec2(v_TexCoord, 0.0);\n"
        "    vec4 base = texSample2D(g_Texture0, ruv);\n"
        "    vec3 hsv = rgb2hsv(base.rgb);\n"
        "    vec3 rgb = ApplyBlending(BLENDMODE, hsv, u_Tint, saturate(u_Gain * 0.2));\n"
        "    gl_FragColor = vec4(rgb, base.a);\n"
        "}\n";

    const we_shader_combo_t override_combos[] = {
        {.name = "BLENDMODE", .value = 7},
    };

    we_shader_preprocessed_t prep;
    assert(we_shader_preprocess_pair(vert_src, frag_src, NULL, override_combos, 1, 0x3u, false,
                                     &prep));
    assert(prep.vert_glsl != NULL && prep.frag_glsl != NULL);

    /* Verify GLSL ES 3.10 header and HLSL compatibility helpers */
    assert(strstr(prep.vert_glsl, "#version 310 es") != NULL);
    assert(strstr(prep.frag_glsl, "#version 310 es") != NULL);
    assert(strstr(prep.frag_glsl, "#define BLENDMODE 7") != NULL);
    /* Auto-enabled sampler combo for g_Texture1 -> THETEXTURE1 */
    assert(strstr(prep.frag_glsl, "#define THETEXTURE1 1") != NULL);
    /* Verify rgb2hsv in common.h uses step(c.b, c.g) */
    assert(strstr(prep.frag_glsl, "step(c.b, c.g)") != NULL);
    /* Verify cross-stage varying v_TexCoord was promoted to vec4 in BOTH stages */
    assert(strstr(prep.vert_glsl, "v_TexCoord") != NULL);
    assert(strstr(prep.frag_glsl, "in vec4 v_TexCoord;") != NULL);

    /* Verify inline JSON uniform defaults were extracted */
    bool found_gain = false, found_tint = false;
    for (uint32_t i = 0; i < prep.meta.uniform_count; ++i) {
        if (strcmp(prep.meta.uniforms[i].name, "u_Gain") == 0) {
            found_gain = true;
            assert(fabsf(prep.meta.uniforms[i].default_val[0] - 2.5f) < 1e-5f);
        } else if (strcmp(prep.meta.uniforms[i].name, "u_Tint") == 0) {
            found_tint = true;
            assert(prep.meta.uniforms[i].num_components == 3);
            assert(fabsf(prep.meta.uniforms[i].default_val[1] - 0.5f) < 1e-5f);
        }
    }
    assert(found_gain && found_tint);

    we_shader_preprocessed_free(&prep);

    /* Verify cyclic #include deduplication terminates cleanly */
    {
        const char *inc_a = "#include \"cycle_b.h\"\nfloat fn_a(float x) { return x + 1.0; }\n";
        const char *inc_b = "#include \"cycle_a.h\"\nfloat fn_b(float x) { return x * 2.0; }\n";
        const pkg_test_item_t cyc_items[] = {
            {.path = "shaders/cycle_a.h", .data = inc_a, .len = strlen(inc_a)},
            {.path = "shaders/cycle_b.h", .data = inc_b, .len = strlen(inc_b)},
        };
        byte_buf_t cbuf;
        build_synthetic_pkg(&cbuf, "PKGV0002", cyc_items, 2);
        we_pkg_t *cpkg = we_pkg_open_mem(cbuf.data, cbuf.len, NULL);
        assert(cpkg != NULL);
        const char *cyc_frag =
            "#include \"cycle_a.h\"\nvoid main() { gl_FragColor = vec4(fn_a(fn_b(1.0))); }\n";
        char *c_out = we_shader_preprocess_stage(cyc_frag, true, cpkg, NULL, 0, 0, false, NULL);
        assert(c_out != NULL);
        assert(strstr(c_out, "fn_a") != NULL && strstr(c_out, "fn_b") != NULL);
        free(c_out);
        we_pkg_close(cpkg);
        bb_free(&cbuf);
    }
}

/* =========================================================================
 * 4. Scene Graph Renderer & Top-Down Orientation Invariant E2E Test
 * ========================================================================= */

static void build_top_blue_bottom_red_tex(byte_buf_t *tb)
{
    /* 2x2 RGBA8888 texture:
     * Row 0 (top row):    Blue (R=0, G=0, B=255, A=255)
     * Row 1 (bottom row): Red  (R=255, G=0, B=0, A=255) */
    bb_init(tb);
    bb_magic9(tb, "TEXV0003");
    bb_magic9(tb, "TEXI0001");
    bb_u32(tb, WE_TEX_FMT_RGBA8888);
    bb_u32(tb, WE_TEX_FLAG_CLAMP_UVS | WE_TEX_FLAG_NO_INTERPOLATION);
    bb_u32(tb, 2);
    bb_u32(tb, 2);
    bb_u32(tb, 2);
    bb_u32(tb, 2);
    bb_u32(tb, 0);
    bb_magic9(tb, "TEXB0003");
    bb_u32(tb, 1);
    bb_i32(tb, -1);
    bb_u32(tb, 1);
    bb_u32(tb, 2);
    bb_u32(tb, 2);
    bb_u32(tb, 0);
    bb_u32(tb, 0);
    bb_i32(tb, 16);
    const uint8_t px[16] = {
        0,   0, 255, 255, 0,   0, 255, 255, /* Row 0: Blue */
        255, 0, 0,   255, 255, 0, 0,   255, /* Row 1: Red */
    };
    bb_append(tb, px, 16);
}

static void test_scene_graph_and_cli(void)
{
    printf("  [TEST] Testing project.json, scene.json renderer & Top-Down Orientation "
           "Invariant...\n");

    char tmp_root[] = "/tmp/wwal_test_workshop_XXXXXX";
    assert(mkdtemp(tmp_root) != NULL);

    char item_dir[4096];
    snprintf(item_dir, sizeof(item_dir), "%s/987654321", tmp_root);
    assert(mkdir(item_dir, 0755) == 0);

    const char *proj_json = "{\n"
                            "  \"title\": \"Cyberpunk Neon Test\",\n"
                            "  \"type\": \"scene\",\n"
                            "  \"file\": \"scene.json\",\n"
                            "  \"preview\": \"preview.jpg\",\n"
                            "  \"general\": {\n"
                            "    \"properties\": {\n"
                            "      \"tintcolor\": {\n"
                            "        \"text\": \"Tint Color\",\n"
                            "        \"type\": \"color\",\n"
                            "        \"value\": \"1.0 1.0 1.0\"\n"
                            "      },\n"
                            "      \"showlayer\": {\n"
                            "        \"text\": \"Show Layer\",\n"
                            "        \"type\": \"bool\",\n"
                            "        \"value\": true\n"
                            "      }\n"
                            "    }\n"
                            "  }\n"
                            "}\n";

    char proj_path[4096];
    snprintf(proj_path, sizeof(proj_path), "%s/project.json", item_dir);
    FILE *fp = fopen(proj_path, "wb");
    assert(fp != NULL);
    fwrite(proj_json, 1, strlen(proj_json), fp);
    fclose(fp);

    const char *scene_json =
        "{\n"
        "  \"camera\": {\n"
        "    \"center\": \"0.0 0.0 -1.0\",\n"
        "    \"eye\": \"0.0 0.0 0.0\",\n"
        "    \"up\": \"0.0 1.0 0.0\"\n"
        "  },\n"
        "  \"general\": {\n"
        "    \"clearcolor\": \"0.0 0.0 0.0\",\n"
        "    \"clearenabled\": true,\n"
        "    \"orthogonalprojection\": {\"width\": 1920, \"height\": 1080},\n"
        "    \"cameraparallax\": false\n"
        "  },\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"id\": 10,\n"
        "      \"name\": \"RootAnchor\",\n"
        "      \"origin\": \"960.0 540.0 0.0\",\n"
        "      \"scale\": \"1.0 1.0 1.0\",\n"
        "      \"angles\": \"0.0 0.0 0.0\",\n"
        "      \"visible\": false\n"
        "    },\n"
        "    {\n"
        "      \"id\": 1,\n"
        "      \"parent\": 10,\n"
        "      \"name\": \"Background\",\n"
        "      \"image\": \"models/bg.json\",\n"
        "      \"origin\": \"0.0 0.0 0.0\",\n"
        "      \"scale\": \"1.0 1.0 1.0\",\n"
        "      \"angles\": \"0.0 0.0 0.0\",\n"
        "      \"size\": \"1920.0 1080.0\",\n"
        "      \"alpha\": 1.0,\n"
        "      \"brightness\": 1.0,\n"
        "      \"color\": {\"user\": \"tintcolor\", \"value\": \"1.0 1.0 1.0\"},\n"
        "      \"visible\": {\"user\": \"showlayer\", \"value\": true},\n"
        "      \"effects\": [\n"
        "        {\n"
        "          \"file\": \"effects/copy_test.json\",\n"
        "          \"name\": \"copy_test\",\n"
        "          \"visible\": true\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const char *eff_json =
        "{\n"
        "  \"fbos\": [{\"name\": \"_rt_effect_saved\", \"scale\": 1}],\n"
        "  \"passes\": [\n"
        "    {\"command\": \"copy\", \"target\": \"_rt_effect_saved\"},\n"
        "    {\"command\": \"copy\", \"target\": \"_rt_imageLayerComposite_1_a\"},\n"
        "    {\n"
        "      \"material\": \"materials/bg.json\",\n"
        "      \"bind\": [{\"name\": \"_rt_effect_saved\", \"index\": 0}]\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const char *mdl_json = "{\"material\": \"materials/bg.json\"}\n";
    const char *mat_json = "{\n"
                           "  \"passes\": [\n"
                           "    {\n"
                           "      \"blending\": \"normal\",\n"
                           "      \"depthtest\": \"disabled\",\n"
                           "      \"cullmode\": \"nocull\",\n"
                           "      \"shader\": \"genericimage2\",\n"
                           "      \"textures\": [\"bg\"]\n"
                           "    }\n"
                           "  ]\n"
                           "}\n";

    byte_buf_t bg_tex;
    build_top_blue_bottom_red_tex(&bg_tex);

    const pkg_test_item_t pkg_items[] = {
        {.path = "scene.json", .data = scene_json, .len = strlen(scene_json)},
        {.path = "models/bg.json", .data = mdl_json, .len = strlen(mdl_json)},
        {.path = "materials/bg.json", .data = mat_json, .len = strlen(mat_json)},
        {.path = "materials/bg.tex", .data = bg_tex.data, .len = bg_tex.len},
        {.path = "effects/copy_test.json", .data = eff_json, .len = strlen(eff_json)},
    };

    byte_buf_t pkg_buf;
    build_synthetic_pkg(&pkg_buf, "PKGV0002", pkg_items, 5);

    char pkg_path[4096];
    snprintf(pkg_path, sizeof(pkg_path), "%s/scene.pkg", item_dir);
    fp = fopen(pkg_path, "wb");
    assert(fp != NULL);
    fwrite(pkg_buf.data, 1, pkg_buf.len, fp);
    fclose(fp);

    bb_free(&bg_tex);
    bb_free(&pkg_buf);

    /* Test custom workshop_dir via config.toml (WWAL_CONFIG_PATH) */
    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.toml", tmp_root);
    fp = fopen(cfg_path, "wb");
    assert(fp != NULL);
    fprintf(fp, "[daemon]\nworkshop_dir = \"%s\"\n", tmp_root);
    fclose(fp);
    unsetenv("WWAL_WORKSHOP_DIR");
    setenv("WWAL_CONFIG_PATH", cfg_path, 1);

    char resolved_dir[4096];
    assert(we_workshop_resolve_dir("987654321", resolved_dir, sizeof(resolved_dir)));
    assert(strcmp(resolved_dir, item_dir) == 0);

    /* Also test WWAL_WORKSHOP_DIR env var */
    setenv("WWAL_WORKSHOP_DIR", tmp_root, 1);

    we_project_t *scanned = NULL;
    size_t scanned_count = 0;
    assert(we_workshop_scan_all(&scanned, &scanned_count));
    assert(scanned_count >= 1);
    bool found_id = false;
    for (size_t i = 0; i < scanned_count; ++i) {
        if (strcmp(scanned[i].workshop_id, "987654321") == 0) {
            found_id = true;
            assert(strcmp(scanned[i].title, "Cyberpunk Neon Test") == 0);
        }
    }
    assert(found_id);
    free(scanned);

    /* Load and render the scene! */
    we_scene_t *scene = we_scene_create(NULL, item_dir, NULL);
    assert(scene != NULL);
    assert(scene->object_count == 2);

    uint8_t fb[16 * 16 * 4];
    memset(fb, 0, sizeof(fb));
    waywal_shader_frame_params_t fparams;
    memset(&fparams, 0, sizeof(fparams));
    fparams.time_sec = 0.5f;
    fparams.time_delta_sec = 0.016f;

    assert(we_scene_render_rgba8(scene, 16, 16, &fparams, fb));

    /* Verify Top-Down Orientation Invariant:
     * Row 2 (near top of screen) must be Blue (R=0, G=0, B=255, A=255).
     * Row 13 (near bottom of screen) must be Red (R=255, G=0, B=0, A=255). */
    const uint8_t *top_px = &fb[(2 * 16 + 8) * 4];
    const uint8_t *bot_px = &fb[(13 * 16 + 8) * 4];
    assert(top_px[2] > 200 && top_px[0] < 30 && top_px[3] == 255);
    assert(bot_px[0] > 200 && bot_px[2] < 30 && bot_px[3] == 255);

    /* Verify scaled readback when requested size (8x8) differs from scene_fbo size (16x16) */
    uint8_t fb8[8 * 8 * 4];
    memset(fb8, 0, sizeof(fb8));
    assert(we_scene_render_rgba8(scene, 8, 8, &fparams, fb8));
    const uint8_t *top8 = &fb8[(1 * 8 + 4) * 4];
    const uint8_t *bot8 = &fb8[(6 * 8 + 4) * 4];
    assert(top8[2] > 200 && top8[0] < 30 && top8[3] == 255);
    assert(bot8[0] > 200 && bot8[2] < 30 && bot8[3] == 255);
    we_scene_destroy(scene);

    /* Test user property override (--prop tintcolor="0.0 1.0 0.0") zeroing out Blue and Red */
    we_scene_t *scene_ov = we_scene_create(NULL, item_dir, "tintcolor=0.0 1.0 0.0");
    assert(scene_ov != NULL);
    memset(fb, 0, sizeof(fb));
    assert(we_scene_render_rgba8(scene_ov, 16, 16, &fparams, fb));
    assert(top_px[2] == 0 && bot_px[0] == 0 && top_px[3] == 255);
    we_scene_destroy(scene_ov);

    printf("  [TEST] Testing wwal workshop CLI parser and subcommands...\n");
    cli_options_t copts;
    char *argv_list[] = {"wwal", "workshop", "list", NULL};
    assert(cli_parse(3, argv_list, &copts));
    assert(copts.cmd == CLI_CMD_WORKSHOP && copts.workshop_subcmd == WAYWAL_WORKSHOP_LIST);

    char *argv_info[] = {"wwal", "workshop", "info", "987654321", NULL};
    assert(cli_parse(4, argv_info, &copts));
    assert(copts.cmd == CLI_CMD_WORKSHOP && copts.workshop_subcmd == WAYWAL_WORKSHOP_INFO);
    assert(strcmp(copts.filepath, "987654321") == 0);

    char *argv_apply[] = {"wwal",      "workshop", "apply",      "987654321", "--prop",
                          "speed=2.0", "--prop",   "bloom=true", "--audio",   NULL};
    assert(cli_parse(9, argv_apply, &copts));
    assert(copts.cmd == CLI_CMD_WORKSHOP && copts.workshop_subcmd == WAYWAL_WORKSHOP_APPLY);
    assert(strcmp(copts.filepath, "987654321") == 0);
    assert(strcmp(copts.workshop_props, "speed=2.0;bloom=true") == 0);
    assert(copts.audio_reactive);

    /* Cleanup temporary files */
    unsetenv("WWAL_CONFIG_PATH");
    unlink(cfg_path);
    unlink(pkg_path);
    unlink(proj_path);
    rmdir(item_dir);
    rmdir(tmp_root);
}

static void test_pkg_tex_shader_and_scene_edge_cases(void)
{
    printf("  [TEST] Testing VFS path traversal guards, unpadded TEX magic, shader combos, and "
           "effect chains...\n");

    /* Segment-aware '..' path validation */
    char norm[256];
    assert(we_pkg_normalize_path("textures/noise..tex", 19, norm, sizeof(norm)));
    assert(strcmp(norm, "textures/noise..tex") == 0);
    assert(we_pkg_normalize_path("textures/..hidden/a..b.tex", 26, norm, sizeof(norm)));
    assert(strcmp(norm, "textures/..hidden/a..b.tex") == 0);
    assert(!we_pkg_normalize_path("textures/../secret.tex", 22, norm, sizeof(norm)));
    assert(!we_pkg_normalize_path("../secret.tex", 13, norm, sizeof(norm)));
    assert(!we_pkg_normalize_path("textures/..", 11, norm, sizeof(norm)));

    /* Fallback to "shaders/<path>" for bare shader filenames */
    size_t b_sz = 0;
    bool b_owned = true;
    const uint8_t *b_ptr = we_pkg_resolve(NULL, "genericimage2.vert", &b_sz, &b_owned);
    assert(b_ptr != NULL && b_sz > 0 && !b_owned);

    /* 8-byte unpadded magic followed by uint32 whose low byte is 0x00 (e.g. 256) */
    byte_buf_t tb_bad;
    bb_init(&tb_bad);
    bb_magic8_nonul(&tb_bad, "TEXV0005");
    bb_magic8_nonul(&tb_bad, "TEXI0001");
    bb_u32(&tb_bad, WE_TEX_FMT_RGBA8888);
    bb_u32(&tb_bad, 0);
    bb_u32(&tb_bad, 2);
    bb_u32(&tb_bad, 2);
    bb_u32(&tb_bad, 2);
    bb_u32(&tb_bad, 2);
    bb_u32(&tb_bad, 0);
    bb_magic8_nonul(&tb_bad, "TEXB0003"); /* 8-byte unpadded magic */
    bb_u32(&tb_bad, 256);                 /* 0x00000100: low byte is 0x00! */
    bb_i32(&tb_bad, -1);
    /* Must reject image_count=256 rather than skipping 0x00 and reading image_count=1 */
    assert(we_tex_parse_cpu(tb_bad.data, tb_bad.len) == NULL);
    bb_free(&tb_bad);

    /* Also test unpadded TEXS0003 with frame_count = 256 (0x00000100) */
    byte_buf_t tb_sp;
    bb_init(&tb_sp);
    bb_magic8_nonul(&tb_sp, "TEXV0005");
    bb_magic8_nonul(&tb_sp, "TEXI0001");
    bb_u32(&tb_sp, WE_TEX_FMT_RGBA8888);
    bb_u32(&tb_sp, WE_TEX_FLAG_IS_GIF);
    bb_u32(&tb_sp, 1);
    bb_u32(&tb_sp, 1);
    bb_u32(&tb_sp, 1);
    bb_u32(&tb_sp, 1);
    bb_u32(&tb_sp, 0);
    bb_magic8_nonul(&tb_sp, "TEXB0003"); /* 8-byte unpadded magic */
    bb_u32(&tb_sp, 1);                   /* image_count = 1 */
    bb_i32(&tb_sp, -1);                  /* free_image_format = -1 */
    bb_u32(&tb_sp, 1);                   /* mipmap_count = 1 */
    bb_u32(&tb_sp, 1);                   /* width = 1 */
    bb_u32(&tb_sp, 1);                   /* height = 1 */
    bb_u32(&tb_sp, 0);                   /* lz4_compressed = 0 */
    bb_u32(&tb_sp, 4);                   /* decompressed_size = 4 */
    bb_i32(&tb_sp, 4);                   /* byte_count = 4 */
    const uint8_t px1[4] = {255, 0, 0, 255};
    bb_append(&tb_sp, px1, 4);
    bb_magic8_nonul(&tb_sp, "TEXS0003"); /* 8-byte unpadded magic */
    bb_u32(&tb_sp, 256);                 /* frame_count = 256 (0x00000100, low byte 0x00!) */
    bb_u32(&tb_sp, 64);
    bb_u32(&tb_sp, 64);
    for (int f = 0; f < 256; ++f) {
        bb_u32(&tb_sp, 0);
        bb_f32(&tb_sp, 0.1f);
        bb_f32(&tb_sp, 0.0f);
        bb_f32(&tb_sp, 0.0f);
        bb_f32(&tb_sp, 1.0f);
        bb_f32(&tb_sp, 0.0f);
        bb_f32(&tb_sp, 0.0f);
        bb_f32(&tb_sp, 1.0f);
    }
    we_tex_t *sp_tex = we_tex_parse_cpu(tb_sp.data, tb_sp.len);
    assert(sp_tex != NULL);
    assert((sp_tex->flags & WE_TEX_FLAG_IS_GIF) != 0);
    assert(sp_tex->frame_count == 256);
    we_tex_destroy(sp_tex);
    bb_free(&tb_sp);

    /* Shader combo key matching, identifier sanitization, cross-stage varying promotion, and
     * included #version stripping */
    const char *inc_ver_h = "#version 130\nfloat inc_helper(float v) { return saturate(v); }\n";
    const pkg_test_item_t inc_items[] = {
        {.path = "shaders/inc_ver.h", .data = inc_ver_h, .len = strlen(inc_ver_h)},
    };
    byte_buf_t pkg_buf;
    build_synthetic_pkg(&pkg_buf, "PKGV0001", inc_items, 1);
    we_pkg_t *pkg = we_pkg_open_mem(pkg_buf.data, pkg_buf.len, NULL);
    assert(pkg != NULL);
    const uint8_t *pkg_sh_ptr = we_pkg_resolve(pkg, "inc_ver.h", &b_sz, &b_owned);
    assert(pkg_sh_ptr != NULL && b_sz == strlen(inc_ver_h) && !b_owned);

    const char *vs_mismatch = "// [COMBO] "
                              "{\"my_material\":\"Fake\",\"material\":\"Real\",\"not_default\":9,"
                              "\"default\":1,\"combo\":\"USE_FX\"}\n"
                              "attribute vec3 a_Position;\n"
                              "attribute vec2 a_TexCoord;\n"
                              "varying vec2 v_TexCoord;\n"
                              "void main() {\n"
                              "    gl_Position = vec4(a_Position, 1.0);\n"
                              "    v_TexCoord = a_TexCoord;\n"
                              "}\n";
    const char *fs_mismatch = "#include <shaders/inc_ver.h>\n"
                              "varying vec4 v_TexCoord;\n"
                              "void main() {\n"
                              "    float k = inc_helper(v_TexCoord.x + v_TexCoord.z);\n"
                              "    gl_FragColor = vec4(k, k, k, 1.0);\n"
                              "}\n";

    we_shader_combo_t bad_combo;
    memset(&bad_combo, 0, sizeof(bad_combo));
    strncpy(bad_combo.name, "BAD;INJECT", sizeof(bad_combo.name) - 1);
    bad_combo.value = 1;

    we_shader_preprocessed_t prep;
    assert(
        we_shader_preprocess_pair(vs_mismatch, fs_mismatch, pkg, &bad_combo, 1, 0u, false, &prep));
    assert(prep.vert_glsl != NULL && prep.frag_glsl != NULL);
    /* Verify exact JSON key matching extracted "USE_FX" = 1 (not "not_default":9) */
    assert(strstr(prep.vert_glsl, "#define USE_FX 1") != NULL);
    /* Verify identifier sanitizer rejected "BAD;INJECT" */
    assert(strstr(prep.vert_glsl, "BAD;INJECT") == NULL);
    /* Verify cross-stage varying promotion upgraded v_TexCoord to vec4 in VS with .xy swizzle */
    assert(strstr(prep.vert_glsl, "#undef v_TexCoord") != NULL);
    assert(strstr(prep.vert_glsl, "out vec4 v_TexCoord;") != NULL);
    assert(strstr(prep.vert_glsl, "#define v_TexCoord v_TexCoord.xy") != NULL);
    /* Verify preprocessor stripped #version 130 from included header */
    assert(strstr(prep.frag_glsl, "#version 130") == NULL);
    assert(strstr(prep.frag_glsl, "inc_helper") != NULL);

    we_shader_preprocessed_free(&prep);
    we_pkg_close(pkg);
    bb_free(&pkg_buf);

    /* Scene rendering & effect chain regression tests */
    {
        char tmp_dir[] = "/tmp/wwal_test_fxchain_XXXXXX";
        assert(mkdtemp(tmp_dir) != NULL);

        const char *proj_s = "{\"title\":\"FxChain\",\"type\":\"scene\",\"file\":\"scene.json\"}";
        char ppath[4096];
        snprintf(ppath, sizeof(ppath), "%s/project.json", tmp_dir);
        FILE *f = fopen(ppath, "wb");
        assert(f != NULL);
        fwrite(proj_s, 1, strlen(proj_s), f);
        fclose(f);

        /* Object 100 has singular scale (0,0,0) to exercise early continue cleanup.
         * Object 1 has an active pass-through effect whose material declares "textures":["dummy"]
         * on slot 0 to verify prev_tex priority on unit 0 and base_pass seeding. */
        const char *scn_s = "{\n"
                            "  \"general\": {\n"
                            "    \"clearcolor\": \"0.0 0.0 0.0\",\n"
                            "    \"clearenabled\": true,\n"
                            "    \"orthogonalprojection\": {\"width\": 1920, \"height\": 1080},\n"
                            "    \"cameraparallax\": true,\n"
                            "    \"cameraparallaxamount\": 1.0,\n"
                            "    \"cameraparallaxdelay\": 0.0,\n"
                            "    \"cameraparallaxmouseinfluence\": 1.0\n"
                            "  },\n"
                            "  \"objects\": [\n"
                            "    {\n"
                            "      \"id\": 100,\n"
                            "      \"name\": \"SingularObj\",\n"
                            "      \"image\": \"models/bg.json\",\n"
                            "      \"origin\": \"960.0 540.0 0.0\",\n"
                            "      \"scale\": \"0.0 0.0 0.0\",\n"
                            "      \"size\": \"1920.0 1080.0\",\n"
                            "      \"visible\": true\n"
                            "    },\n"
                            "    {\n"
                            "      \"id\": 1,\n"
                            "      \"name\": \"EffectObj\",\n"
                            "      \"image\": \"models/bg.json\",\n"
                            "      \"origin\": \"960.0 540.0 0.0\",\n"
                            "      \"scale\": \"1.0 1.0 1.0\",\n"
                            "      \"size\": \"1920.0 1080.0\",\n"
                            "      \"visible\": true,\n"
                            "      \"effects\": [\n"
                            "        {\n"
                            "          \"file\": \"effects/passthru.json\",\n"
                            "          \"visible\": true\n"
                            "        }\n"
                            "      ]\n"
                            "    }\n"
                            "  ]\n"
                            "}\n";
        const char *mdl_s = "{\"material\": \"materials/bg.json\"}\n";
        const char *mat_s = "{\n"
                            "  \"passes\": [\n"
                            "    {\n"
                            "      \"blending\": \"normal\",\n"
                            "      \"shader\": \"genericimage2\",\n"
                            "      \"textures\": [\"bg\"]\n"
                            "    }\n"
                            "  ]\n"
                            "}\n";
        /* Effect pass material has "textures": ["dummy"] on slot 0 (which is solid black),
         * verifying that bind_pass_uniforms binds prev_tex on unit 0 instead of dummy.tex! */
        const char *eff_s = "{\n"
                            "  \"passes\": [\n"
                            "    {\"material\": \"materials/eff_pass.json\"}\n"
                            "  ]\n"
                            "}\n";
        const char *eff_mat_s = "{\n"
                                "  \"passes\": [\n"
                                "    {\n"
                                "      \"blending\": \"disabled\",\n"
                                "      \"shader\": \"genericimage2\",\n"
                                "      \"textures\": [\"dummy\"]\n"
                                "    }\n"
                                "  ]\n"
                                "}\n";

        byte_buf_t bg_t, dummy_t;
        build_top_blue_bottom_red_tex(&bg_t);
        bb_init(&dummy_t);
        bb_magic9(&dummy_t, "TEXV0003");
        bb_magic9(&dummy_t, "TEXI0001");
        bb_u32(&dummy_t, WE_TEX_FMT_RGBA8888);
        bb_u32(&dummy_t, WE_TEX_FLAG_CLAMP_UVS);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 0);
        bb_magic9(&dummy_t, "TEXB0003");
        bb_u32(&dummy_t, 1);
        bb_i32(&dummy_t, -1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 1);
        bb_u32(&dummy_t, 0);
        bb_u32(&dummy_t, 0);
        bb_i32(&dummy_t, 4);
        const uint8_t blk_px[4] = {0, 255, 0, 255}; /* pure green dummy texture */
        bb_append(&dummy_t, blk_px, 4);

        const pkg_test_item_t pitems[] = {
            {.path = "scene.json", .data = scn_s, .len = strlen(scn_s)},
            {.path = "models/bg.json", .data = mdl_s, .len = strlen(mdl_s)},
            {.path = "materials/bg.json", .data = mat_s, .len = strlen(mat_s)},
            {.path = "materials/bg.tex", .data = bg_t.data, .len = bg_t.len},
            {.path = "effects/passthru.json", .data = eff_s, .len = strlen(eff_s)},
            {.path = "materials/eff_pass.json", .data = eff_mat_s, .len = strlen(eff_mat_s)},
            {.path = "materials/dummy.tex", .data = dummy_t.data, .len = dummy_t.len},
        };
        byte_buf_t spkg;
        build_synthetic_pkg(&spkg, "PKGV0002", pitems, 7);
        char spkg_path[4096];
        snprintf(spkg_path, sizeof(spkg_path), "%s/scene.pkg", tmp_dir);
        f = fopen(spkg_path, "wb");
        assert(f != NULL);
        fwrite(spkg.data, 1, spkg.len, f);
        fclose(f);
        bb_free(&bg_t);
        bb_free(&dummy_t);
        bb_free(&spkg);

        /* 1. Test CPU fallback directly (gl_initialized == false) */
        we_scene_t *cpu_scn = we_scene_load_cpu(tmp_dir, NULL);
        assert(cpu_scn != NULL && !cpu_scn->gl_initialized);
        uint8_t cpu_fb[16 * 16 * 4] = {0};
        waywal_shader_frame_params_t fp;
        memset(&fp, 0, sizeof(fp));
        fp.mouse[0] = 8.0f;
        fp.mouse[1] = 8.0f;
        assert(we_scene_render_rgba8(cpu_scn, 16, 16, &fp, cpu_fb));
        assert(cpu_fb[(2 * 16 + 8) * 4 + 2] == 255);  /* top row Blue */
        assert(cpu_fb[(13 * 16 + 8) * 4 + 0] == 255); /* bottom row Red */
        we_scene_destroy(cpu_scn);

        /* 2. Test full scene (GL if available) */
        we_scene_t *gl_scn = we_scene_create(NULL, tmp_dir, NULL);
        assert(gl_scn != NULL);
        memset(cpu_fb, 0, sizeof(cpu_fb));
        assert(we_scene_render_rgba8(gl_scn, 16, 16, &fp, cpu_fb));
        /* Must render Blue on top and Red on bottom (NOT green from dummy.tex!) */
        assert(cpu_fb[(2 * 16 + 8) * 4 + 2] > 200 && cpu_fb[(2 * 16 + 8) * 4 + 1] < 30);
        assert(cpu_fb[(13 * 16 + 8) * 4 + 0] > 200 && cpu_fb[(13 * 16 + 8) * 4 + 1] < 30);
        if (gl_scn->gl_initialized) {
            /* last_pointer must be normalized by target_w=16, target_h=16 -> (0.5, 0.5) */
            assert(fabsf(gl_scn->last_pointer[0] - 0.5f) < 1e-4f);
            assert(fabsf(gl_scn->last_pointer[1] - 0.5f) < 1e-4f);
        }

        /* waywal_we_scene_forget_bo must not clobber foreign bo->gl_tex/bo->egl_image */
        dmabuf_bo_t fake_bo;
        memset(&fake_bo, 0, sizeof(fake_bo));
        fake_bo.gl_tex = 777u;
        fake_bo.egl_image = (void *)0x12345678ULL;
        gl_scn->imported_bos[0].bo = &fake_bo;
        gl_scn->imported_bos[0].gl_tex = 0;
        gl_scn->imported_bos[0].egl_image = NULL;
        gl_scn->imported_bos[0].owns_egl = false;
        gl_scn->num_imported_bos = 1;
        waywal_we_scene_forget_bo(gl_scn, &fake_bo);
        assert(gl_scn->num_imported_bos == 0);
        assert(fake_bo.gl_tex == 777u && fake_bo.egl_image == (void *)0x12345678ULL);

        we_scene_destroy(gl_scn);

        unlink(spkg_path);
        unlink(ppath);
        rmdir(tmp_dir);
    }
}

/* =========================================================================
 * 6. 2D Puppet Warp (.mdl) Skeletal Mesh & Two-Pass Skinning Tests
 * ========================================================================= */

static void build_synthetic_mdl(byte_buf_t *out, int32_t mdl_flag, bool alt_80b_stride)
{
    bb_init(out);
    bb_magic9(out, "MDLV0013");
    bb_i32(out, mdl_flag);
    bb_i32(out, 1);
    bb_i32(out, 1);
    bb_cstr(out, "materials/hero.json");
    bb_i32(out, 0);
    bb_u32(out, alt_80b_stride ? 0x0180000Fu : 0x01800009u);

    uint32_t stride = alt_80b_stride ? 80u : 52u;
    uint32_t vcount = 4u;
    bb_u32(out, vcount * stride);

    /* 4 quad vertices covering (-8..8, -8..8):
     * v0, v1 weighted 100% to Bone 0 (root);
     * v2 weighted 100% to Bone 1 (child);
     * v3 weighted 50% Bone 0 + 50% Bone 1. */
    const float vx[4] = {-8.0f, 8.0f, 8.0f, -8.0f};
    const float vy[4] = {-8.0f, -8.0f, 8.0f, 8.0f};
    const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const uint32_t bi[4][4] = {
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        {1, 0, 0, 0},
        {0, 1, 0, 0},
    };
    const float bw[4][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 0.0f},
    };

    for (uint32_t i = 0; i < vcount; i++) {
        bb_f32(out, vx[i]);
        bb_f32(out, vy[i]);
        bb_f32(out, 0.0f);
        if (alt_80b_stride) {
            for (int p = 0; p < 7; p++) {
                bb_u32(out, 0u);
            }
        }
        for (int k = 0; k < 4; k++) {
            bb_u32(out, bi[i][k]);
        }
        for (int k = 0; k < 4; k++) {
            bb_f32(out, bw[i][k]);
        }
        bb_f32(out, uvs[i][0]);
        bb_f32(out, uvs[i][1]);
    }

    /* 2 triangles = 6 uint16_t indices = 12 bytes */
    bb_u32(out, 12u);
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        bb_u16(out, indices[i]);
    }

    /* MDAT0001 with 1 attachment */
    bb_magic9(out, "MDAT0001");
    bb_u32(out, 1u);
    bb_u16(out, 0u);
    bb_cstr(out, "weapon_mount");
    for (int m = 0; m < 16; m++) {
        bb_f32(out, (m % 5 == 0) ? 1.0f : 0.0f);
    }

    /* MDLS0001 with 2 bones:
     * Bone 0 (root): parent = 0xFFFFFFFF, local_trans = translate(10, 0, 0)
     * Bone 1 (child): parent = 0, local_trans = translate(5, 0, 0) -> world_bind = translate(15, 0,
     * 0) */
    bb_magic9(out, "MDLS0001");
    bb_u32(out, 2u);

    /* Bone 0 */
    bb_cstr(out, "root_bone");
    bb_i32(out, 0);
    bb_u32(out, 0xFFFFFFFFu);
    bb_u32(out, 64u);
    float b0_mat[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f,
    };
    for (int m = 0; m < 16; m++) {
        bb_f32(out, b0_mat[m]);
    }
    bb_cstr(out, "");

    /* Bone 1 */
    bb_cstr(out, "child_bone");
    bb_i32(out, 0);
    bb_u32(out, 0u);
    bb_u32(out, 64u);
    float b1_mat[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 5.0f, 0.0f, 0.0f, 1.0f,
    };
    for (int m = 0; m < 16; m++) {
        bb_f32(out, b1_mat[m]);
    }
    bb_cstr(out, "");

    /* MDLA0003 with 3 animations:
     * Anim 0 ("idle", "loop", fps=1, length=2):
     *   2 keyframes per bone (at t=0.0s and t=1.0s):
     *   kf 0 (rest pose): Bone 0 pos=(10,0,0), Bone 1 pos=(5,0,0)
     *   kf 1 (translated +20 on Bone 0): Bone 0 pos=(30,0,0), Bone 1 pos=(5,0,0)
     * Anim 1 ("pingpong", "mirror", fps=1, length=2)
     * Anim 2 ("oneshot", "single", fps=1, length=2) */
    bb_magic9(out, "MDLA0003");
    bb_u32(out, 3u);
    const char *anames[3] = {"idle", "pingpong", "oneshot"};
    const char *amodes[3] = {"loop", "mirror", "single"};
    for (int a = 0; a < 3; a++) {
        bb_i32(out, a + 100);
        bb_i32(out, 0);
        bb_cstr(out, anames[a]);
        bb_cstr(out, amodes[a]);
        bb_f32(out, 1.0f); /* fps = 1 keyframe/sec */
        bb_i32(out, 2);    /* length = 2 keyframes */
        bb_i32(out, 0);
        bb_u32(out, 2u); /* 2 bone tracks */

        /* Track 0: Bone 0 */
        bb_i32(out, 0);
        bb_i32(out, 0);
        bb_u32(out, 2u * 36u);
        /* kf 0: pos=(10,0,0), angle=(0,0,0), scale=(1,1,1) */
        bb_f32(out, 10.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 1.0f);
        bb_f32(out, 1.0f);
        bb_f32(out, 1.0f);
        /* kf 1: pos=(30,0,0), angle=(0,0,0), scale=(1,1,1) */
        bb_f32(out, 30.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 0.0f);
        bb_f32(out, 1.0f);
        bb_f32(out, 1.0f);
        bb_f32(out, 1.0f);

        /* Track 1: Bone 1 */
        bb_i32(out, 1);
        bb_i32(out, 0);
        bb_u32(out, 2u * 36u);
        for (int k = 0; k < 2; k++) {
            bb_f32(out, 5.0f);
            bb_f32(out, 0.0f);
            bb_f32(out, 0.0f);
            bb_f32(out, 0.0f);
            bb_f32(out, 0.0f);
            bb_f32(out, 0.0f);
            bb_f32(out, 1.0f);
            bb_f32(out, 1.0f);
            bb_f32(out, 1.0f);
        }
    }
}

static void test_puppet_warp_mdl(void)
{
    printf("  [TEST] Testing 2D Puppet Warp (.mdl) parser & Two-Pass Skinning Invariant...\n");

    /* 1. mdl_flag == 9 stub mesh must be rejected gracefully */
    byte_buf_t stub_mdl;
    build_synthetic_mdl(&stub_mdl, 9, false);
    assert(we_puppet_parse_cpu(stub_mdl.data, stub_mdl.len) == NULL);
    bb_free(&stub_mdl);

    /* 2. Parse 52-byte stride and 80-byte stride .mdl binaries */
    for (int alt = 0; alt < 2; alt++) {
        byte_buf_t mdl_buf;
        build_synthetic_mdl(&mdl_buf, 0, alt == 1);
        we_puppet_t *p = we_puppet_parse_cpu(mdl_buf.data, mdl_buf.len);
        assert(p != NULL);
        assert(p->alt_mdl_format == (alt == 1));
        assert(p->vertex_count == 4u && p->index_count == 6u && p->triangle_count == 2u);
        assert(p->bone_count == 2u && p->anim_count == 3u);
        assert(strcmp(p->mat_json_file, "materials/hero.json") == 0);

        /* Verify world_bind and inverse bind matrix (offset_trans) for Bone 0 & Bone 1 */
        assert(fabsf(p->bones[0].world_bind[12] - 10.0f) < 1e-5f);
        assert(fabsf(p->bones[0].offset_trans[12] - (-10.0f)) < 1e-5f);
        assert(fabsf(p->bones[1].world_bind[12] - 15.0f) < 1e-5f);
        assert(fabsf(p->bones[1].offset_trans[12] - (-15.0f)) < 1e-5f);

        /* Evaluate at t = 0.0s (rest pose): skin_mats[0] and skin_mats[1] MUST be identity */
        we_puppet_layer_t play = {
            .anim_id = 100,
            .rate = 1.0f,
            .blend = 1.0f,
            .visible = true,
            .anim_name = "idle",
        };
        we_puppet_eval_skin_matrices(p, &play, 1u, 0.0, p->skin_mats);
        we_puppet_skin_vertices_cpu(p, (const float (*)[16])p->skin_mats, p->skinned_verts);
        assert(fabsf(p->skin_mats[0][12]) < 1e-5f);
        assert(fabsf(p->skin_mats[1][12]) < 1e-5f);
        assert(fabsf(p->skinned_verts[0].pos[0] - (-8.0f)) < 1e-5f);
        assert(fabsf(p->skinned_verts[2].pos[0] - 8.0f) < 1e-5f);

        /* Evaluate at t = 0.5s (halfway between kf 0 and kf 1 in loop mode):
         * Bone 0 moves by +10 in X -> Child Bone 1 also moves by +10 in world X!
         * Two-pass skinning invariant ensures skin_mats[1][12] == +10.0f (NOT multiplied by
         * offset_trans of parent!) */
        we_puppet_eval_skin_matrices(p, &play, 1u, 0.5, p->skin_mats);
        we_puppet_skin_vertices_cpu(p, (const float (*)[16])p->skin_mats, p->skinned_verts);
        assert(fabsf(p->skin_mats[0][12] - 10.0f) < 1e-4f);
        assert(fabsf(p->skin_mats[1][12] - 10.0f) < 1e-4f);
        assert(fabsf(p->skinned_verts[0].pos[0] - 2.0f) < 1e-4f);  /* -8 + 10 = +2 */
        assert(fabsf(p->skinned_verts[2].pos[0] - 18.0f) < 1e-4f); /* +8 + 10 = +18 */
        assert(fabsf(p->skinned_verts[3].pos[0] - 2.0f) < 1e-4f);  /* -8 + (0.5*10 + 0.5*10) = +2 */

        /* Evaluate "mirror" mode at t = 1.75s (period 1.0s -> cycle 1 mirrors to 1.0 - 0.75 = 0.25s
         * -> +5.0f X) */
        snprintf(play.anim_name, sizeof(play.anim_name), "pingpong");
        we_puppet_eval_skin_matrices(p, &play, 1u, 1.75, p->skin_mats);
        assert(fabsf(p->skin_mats[1][12] - 5.0f) < 1e-4f);

        /* Evaluate "single" mode at t = 5.0s (clamped to frame_count - 1 = kf 1 -> +20.0f X) */
        snprintf(play.anim_name, sizeof(play.anim_name), "oneshot");
        we_puppet_eval_skin_matrices(p, &play, 1u, 5.0, p->skin_mats);
        assert(fabsf(p->skin_mats[0][12] - 20.0f) < 1e-4f);
        assert(fabsf(p->skin_mats[1][12] - 20.0f) < 1e-4f);

        we_puppet_destroy(p);
        bb_free(&mdl_buf);
    }

    /* 3. Regression test: [u32 len][chars + '\0'] length-prefixed .mdl strings (where ulen includes
     * the trailing NUL byte) must parse cleanly without desynchronizing the binary stream */
    {
        byte_buf_t len_mdl;
        bb_init(&len_mdl);
        bb_magic9(&len_mdl, "MDLV0013");
        bb_i32(&len_mdl, 0);
        bb_i32(&len_mdl, 1);
        bb_i32(&len_mdl, 1);
        const char *mpath = "materials/hero.json";
        uint32_t mlen_with_nul = (uint32_t)strlen(mpath) + 1u;
        bb_u32(&len_mdl, mlen_with_nul);
        bb_append(&len_mdl, mpath, mlen_with_nul);
        bb_i32(&len_mdl, 0);
        bb_u32(&len_mdl, 0x01800009u);
        bb_u32(&len_mdl, 3u * 52u);
        for (int v = 0; v < 3; v++) {
            bb_f32(&len_mdl, (float)v);
            bb_f32(&len_mdl, 0.0f);
            bb_f32(&len_mdl, 0.0f);
            for (int k = 0; k < 4; k++) {
                bb_u32(&len_mdl, 0u);
            }
            bb_f32(&len_mdl, 1.0f);
            bb_f32(&len_mdl, 0.0f);
            bb_f32(&len_mdl, 0.0f);
            bb_f32(&len_mdl, 0.0f);
            bb_f32(&len_mdl, 0.0f);
            bb_f32(&len_mdl, 0.0f);
        }
        bb_u32(&len_mdl, 6u);
        bb_u16(&len_mdl, 0u);
        bb_u16(&len_mdl, 1u);
        bb_u16(&len_mdl, 2u);
        we_puppet_t *lp = we_puppet_parse_cpu(len_mdl.data, len_mdl.len);
        assert(lp != NULL);
        assert(strcmp(lp->mat_json_file, "materials/hero.json") == 0);
        assert(lp->vertex_count == 3u && lp->triangle_count == 1u);
        we_puppet_destroy(lp);
        bb_free(&len_mdl);
    }

    /* 4. Regression test: Workshop hardware skinning vertex shader with float4x3 g_Bones */
    {
        const char *skin_vert_src =
            "uniform mat4 g_ModelViewProjectionMatrix;\n"
            "uniform float4x3 g_Bones[16];\n"
            "attribute vec3 a_Position;\n"
            "attribute uvec4 a_BlendIndices;\n"
            "attribute vec4 a_BlendWeights;\n"
            "void main() {\n"
            "    vec3 p = mul(vec4(a_Position, 1.0), g_Bones[a_BlendIndices.x]) * "
            "a_BlendWeights.x;\n"
            "    gl_Position = mul(vec4(p, 1.0), g_ModelViewProjectionMatrix);\n"
            "}\n";
        char *glsl =
            we_shader_preprocess_stage(skin_vert_src, false, NULL, NULL, 0, 0, false, NULL);
        assert(glsl != NULL);
        assert(strstr(glsl, "#define float4x3 mat4x3") != NULL);
        assert(strstr(glsl, "vec3 we_mul(vec4 a, mat4x3 b)") != NULL);
        free(glsl);
    }
}

/* =========================================================================
 * 7. 32-Byte-Aligned AVX2 SoA Particle Simulator Tests
 * ========================================================================= */

static void test_particle_simulator_avx2(void)
{
    printf("  [TEST] Testing 32-Byte-Aligned AVX2 SoA Particle Simulator & Rope Compaction...\n");

    const char *pjson =
        "{\n"
        "  \"maxcount\": 64,\n"
        "  \"starttime\": 0,\n"
        "  \"material\": \"materials/spark.json\",\n"
        "  \"renderer\": [\n"
        "    {\"name\": \"ropetrail\", \"subdivision\": 2}\n"
        "  ],\n"
        "  \"controlpoint\": [\n"
        "    {\"id\": 0, \"offset\": \"0 0 0\"},\n"
        "    {\"id\": 1, \"offset\": \"10 20 0\", \"locktopointer\": true}\n"
        "  ],\n"
        "  \"emitter\": [\n"
        "    {\n"
        "      \"name\": \"boxrandom\",\n"
        "      \"rate\": 120.0,\n"
        "      \"instantaneous\": 16,\n"
        "      \"distancemin\": \"-5 -5 0\",\n"
        "      \"distancemax\": \"5 5 0\",\n"
        "      \"controlpoint\": 0\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"sphererandom\",\n"
        "      \"rate\": 60.0,\n"
        "      \"distancemin\": \"0 0 0\",\n"
        "      \"distancemax\": \"10 10 0\",\n"
        "      \"directions\": \"1 1 0\",\n"
        "      \"speedmin\": 5.0,\n"
        "      \"speedmax\": 15.0,\n"
        "      \"controlpoint\": 1\n"
        "    }\n"
        "  ],\n"
        "  \"initializer\": [\n"
        "    {\"name\": \"lifetimerandom\", \"min\": 1.0, \"max\": 2.0},\n"
        "    {\"name\": \"sizerandom\", \"min\": 10.0, \"max\": 10.0},\n"
        "    {\"name\": \"velocityrandom\", \"min\": \"1 2 0\", \"max\": \"3 4 0\"},\n"
        "    {\"name\": \"colorrandom\", \"min\": \"255 128 0\", \"max\": \"255 255 128\"},\n"
        "    {\"name\": \"alpharandom\", \"min\": 1.0, \"max\": 1.0},\n"
        "    {\"name\": \"rotationrandom\", \"min\": \"0 0 -1\", \"max\": \"0 0 1\"},\n"
        "    {\"name\": \"turbulentvelocityrandom\", \"speedmin\": 2.0, \"speedmax\": 4.0, "
        "\"scale\": 0.5}\n"
        "  ],\n"
        "  \"operator\": [\n"
        "    {\"name\": \"movement\", \"drag\": 0.2, \"gravity\": \"0 -9.8 0\", \"speed\": 1.0},\n"
        "    {\"name\": \"angularmovement\", \"drag\": 0.1, \"force\": \"0 0 1\"},\n"
        "    {\"name\": \"alphafade\", \"fadeintime\": 0.2, \"fadeouttime\": 0.8},\n"
        "    {\"name\": \"sizechange\", \"starttime\": 0.0, \"endtime\": 1.0, \"startvalue\": 1.0, "
        "\"endvalue\": 2.0},\n"
        "    {\"name\": \"colorchange\", \"starttime\": 0.0, \"endtime\": 1.0, \"startvalue\": \"1 "
        "1 1\", \"endvalue\": \"0.5 0.5 1\"},\n"
        "    {\"name\": \"oscillatesize\", \"scalemin\": 0.8, \"scalemax\": 1.2, \"frequencymin\": "
        "1.0, \"frequencymax\": 2.0},\n"
        "    {\"name\": \"vortex\", \"controlpoint\": 1, \"axis\": \"0 0 1\", \"distanceinner\": "
        "10.0, \"distanceouter\": 100.0, \"speedinner\": 25.0, \"speedouter\": 5.0},\n"
        "    {\"name\": \"turbulence\", \"scale\": 0.05, \"speedmin\": 5.0, \"speedmax\": 10.0, "
        "\"timescale\": 1.0, \"mask\": \"1 1 0\"}\n"
        "  ]\n"
        "}\n";

    we_particle_sys_t *sys = we_particle_create_from_json(pjson, strlen(pjson));
    assert(sys != NULL);
    assert(sys->max_count == 64u);
    assert(sys->padded_cap == 64u);
    assert(sys->renderer_type == WE_RENDERER_ROPETRAIL);

    /* Verify strict 32-byte alignment of all 31 SoA arrays */
    float *const soa_ptrs[31] = {sys->pos_x,      sys->pos_y,      sys->pos_z,     sys->init_pos_x,
                                 sys->init_pos_y, sys->init_pos_z, sys->vel_x,     sys->vel_y,
                                 sys->vel_z,      sys->acc_x,      sys->acc_y,     sys->acc_z,
                                 sys->r,          sys->g,          sys->b,         sys->a,
                                 sys->init_r,     sys->init_g,     sys->init_b,    sys->init_a,
                                 sys->size,       sys->init_size,  sys->rot_x,     sys->rot_y,
                                 sys->rot_z,      sys->ang_vel_x,  sys->ang_vel_y, sys->ang_vel_z,
                                 sys->age,        sys->max_life,   sys->rng_phase};
    for (int i = 0; i < 31; i++) {
        assert(soa_ptrs[i] != NULL);
        assert(((uintptr_t)soa_ptrs[i] & 31u) == 0u);
    }

    /* Step simulation and verify non-compounding sizechange / alphafade invariant */
    we_particle_set_pointer(sys, 150.0f, 250.0f, 0.0f);
    /* Control point 1 has lock_to_pointer == true and offset (10, 20, 0) */
    assert(fabsf(sys->control_points[1].pos[0] - 160.0f) < 1e-4f);
    assert(fabsf(sys->control_points[1].pos[1] - 270.0f) < 1e-4f);

    we_particle_update(sys, 1.0f / 60.0f);
    assert(sys->alive_count >= 16u);

    /* Step 30 more frames: verify size stays bounded by init_size * [0.8..2.4] and NEVER compounds
     * exponentially */
    for (int step = 0; step < 30; step++) {
        we_particle_update(sys, 1.0f / 60.0f);
    }
    for (uint32_t i = 0; i < sys->alive_count; i++) {
        assert(isfinite(sys->pos_x[i]) && isfinite(sys->pos_y[i]));
        assert(sys->init_size[i] == 10.0f);
        /* sizechange endvalue=2.0 * oscillatesize scalemax=1.2 => max possible size is 24.0f.
         * If size were compounded across 31 frames without pre-operator reset, it would exceed
         * 1000.0f! */
        assert(sys->size[i] >= 5.0f && sys->size[i] <= 25.0f);
        assert(sys->a[i] >= 0.0f && sys->a[i] <= 1.001f);
    }

    /* Test order-preserving compaction for ropetrail renderer */
    uint32_t before_count = sys->alive_count;
    assert(before_count >= 10u);
    /* Tag pos_x coordinates of indices 0, 1, 2, 3 and kill particle 1 */
    sys->pos_x[0] = 1000.0f;
    sys->pos_x[1] = 2000.0f;
    sys->pos_x[2] = 3000.0f;
    sys->pos_x[3] = 4000.0f;
    sys->age[1] = sys->max_life[1] + 10.0f; /* force death of particle 1 */
    /* Disable emitters and operators temporarily to isolate compaction */
    sys->emitter_count = 0u;
    sys->operator_count = 0u;
    we_particle_update(sys, 0.001f);
    assert(sys->alive_count == before_count - 1u);
    /* Since renderer_type == WE_RENDERER_ROPETRAIL, particle 2 (3000.0f) and 3 (4000.0f) shift down
     * in order! */
    assert(fabsf(sys->pos_x[0] - 1000.0f) < 1e-4f);
    assert(fabsf(sys->pos_x[1] - 3000.0f) < 1e-4f);
    assert(fabsf(sys->pos_x[2] - 4000.0f) < 1e-4f);

    /* Verify vertex buffer generation (4 vertices per rope segment quad) */
    we_particle_vertex_t *verts = NULL;
    uint32_t nverts = we_particle_build_vertices(sys, &verts);
    assert(verts != NULL);
    assert(nverts == (sys->alive_count - 1u) * 4u);

    we_particle_destroy(sys);

    /* Regression test: oscillateposition drift-free analytical integration & AVX2 vortex near-zero
     * distance masking */
    {
        const char *osc_json =
            "{\n"
            "  \"maxcount\": 8,\n"
            "  \"emitter\": [{\"name\": \"boxrandom\", \"rate\": 0, \"instantaneous\": 4, "
            "\"distancemin\": \"0 0 0\", \"distancemax\": \"0 0 0\"}],\n"
            "  \"initializer\": [{\"name\": \"lifetimerandom\", \"min\": 1000.0, \"max\": "
            "1000.0}],\n"
            "  \"operator\": [\n"
            "    {\"name\": \"oscillateposition\", \"scalemin\": 10.0, \"scalemax\": 10.0, "
            "\"frequencymin\": 6.0, \"frequencymax\": 6.0, \"mask\": \"1 0 0\"},\n"
            "    {\"name\": \"vortex\", \"controlpoint\": 0, \"axis\": \"0 0 1\", "
            "\"distanceinner\": 1.0, \"distanceouter\": 10.0, \"speedinner\": 50.0, "
            "\"speedouter\": 50.0, \"centerforce\": 20.0}\n"
            "  ]\n"
            "}\n";
        we_particle_sys_t *osys = we_particle_create_from_json(osc_json, strlen(osc_json));
        assert(osys != NULL);
        we_particle_update(osys, 0.001f);
        assert(osys->alive_count == 4u);
        /* Place particle 0 within 0.5e-4f of vortex origin and disable oscillateposition for a step
         * to verify near-zero distance masking in AVX2 vortex */
        osys->operator_count = 1u;
        osys->operators[0].type = WE_OP_VORTEX;
        osys->pos_x[0] = 0.5e-4f;
        osys->pos_y[0] = 0.0f;
        osys->pos_z[0] = 0.0f;
        osys->vel_x[0] = 0.0f;
        osys->vel_y[0] = 0.0f;
        we_particle_update(osys, 0.01f);
        assert(fabsf(osys->vel_x[0]) < 1e-6f && fabsf(osys->vel_y[0]) < 1e-6f);

        /* Now test oscillateposition over 1,000 variable-dt steps: position must remain bounded
         * within [-20, +20] with zero unbounded drift */
        osys->operators[0].type = WE_OP_OSCILLATE_POSITION;
        osys->operators[0].scale_min = 10.0f;
        osys->operators[0].scale_max = 10.0f;
        osys->operators[0].freq_min = 6.0f;
        osys->operators[0].freq_max = 6.0f;
        osys->operators[0].mask[0] = 1.0f;
        osys->pos_x[0] = 0.0f;
        for (int s = 0; s < 1000; s++) {
            float dt = (s & 1) ? (1.0f / 30.0f) : (1.0f / 120.0f);
            we_particle_update(osys, dt);
            assert(fabsf(osys->pos_x[0]) <= 20.01f);
        }
        we_particle_destroy(osys);
    }

    /* Verify AVX2 8-wide sprite vertex builder + scalar tail (27 particles = 3x8 + 3) */
    {
        const char *sprite_json =
            "{\n"
            "  \"maxcount\": 32,\n"
            "  \"renderer\": [{\"name\": \"sprite\"}],\n"
            "  \"emitter\": [{\"name\": \"boxrandom\", \"rate\": 0, \"instantaneous\": 27, "
            "\"distancemin\": \"0 0 0\", \"distancemax\": \"0 0 0\"}],\n"
            "  \"initializer\": [{\"name\": \"lifetimerandom\", \"min\": 100.0, \"max\": 100.0}]\n"
            "}\n";
        we_particle_sys_t *ssys = we_particle_create_from_json(sprite_json, strlen(sprite_json));
        assert(ssys != NULL);
        we_particle_update(ssys, 0.001f);
        assert(ssys->alive_count == 27u);
        for (uint32_t i = 0; i < 27u; ++i) {
            ssys->pos_x[i] = (float)i * 10.0f;
            ssys->pos_y[i] = (float)i * 5.0f;
            ssys->pos_z[i] = (float)i;
            ssys->size[i] = 4.0f; /* half-size = 2.0f */
            ssys->rot_z[i] = (float)i * 0.25f;
            ssys->r[i] = 0.25f;
            ssys->g[i] = 0.5f;
            ssys->b[i] = 0.75f;
            ssys->a[i] = 1.0f;
        }
        we_particle_vertex_t *sverts = NULL;
        uint32_t snverts = we_particle_build_vertices(ssys, &sverts);
        assert(snverts == 27u * 4u && sverts != NULL);
        for (uint32_t i = 0; i < 27u; ++i) {
            float c = cosf(ssys->rot_z[i]) * 2.0f;
            float s = sinf(ssys->rot_z[i]) * 2.0f;
            float px = ssys->pos_x[i];
            float py = ssys->pos_y[i];
            const we_particle_vertex_t *v = &sverts[i * 4u];
            assert(fabsf(v[0].pos[0] - (px - c + s)) < 1e-3f);
            assert(fabsf(v[0].pos[1] - (py - s - c)) < 1e-3f);
            assert(fabsf(v[1].pos[0] - (px + c + s)) < 1e-3f);
            assert(fabsf(v[1].pos[1] - (py + s - c)) < 1e-3f);
            assert(fabsf(v[2].pos[0] - (px + c - s)) < 1e-3f);
            assert(fabsf(v[2].pos[1] - (py + s + c)) < 1e-3f);
            assert(fabsf(v[3].pos[0] - (px - c - s)) < 1e-3f);
            assert(fabsf(v[3].pos[1] - (py - s + c)) < 1e-3f);
            assert(fabsf(v[0].color[0] - 0.25f) < 1e-5f && fabsf(v[0].color[3] - 1.0f) < 1e-5f);
        }
        we_particle_destroy(ssys);
    }
}

/* =========================================================================
 * 8. Sandboxed SceneScript Engine (QuickJS-NG) & Watchdog Tests
 * ========================================================================= */

static void test_scenescript_engine_and_watchdog(void)
{
    printf("  [TEST] Testing Sandboxed SceneScript Engine (QuickJS-NG ES2023) & Infinite-Loop "
           "Watchdog...\n");

    we_script_shared_t shared;
    memset(&shared, 0, sizeof(shared));

    we_script_layer_state_t state = {
        .origin = {100.0f, 200.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f},
        .angles = {0.0f, 0.0f, 0.0f},
        .size = {1920.0f, 1080.0f},
        .color = {1.0f, 1.0f, 1.0f},
        .alpha = 1.0f,
        .visible = true,
        .name = "test_layer",
        .id = 1,
    };

    const char *origin_src =
        "'use strict';\n"
        "export var scriptProperties = createScriptProperties()\n"
        "  .addSlider({name: 'speed', label: 'Speed', value: 2.0, min: 0, max: 10})\n"
        "  .finish();\n"
        "let audioBuf = engine.registerAudioBuffers(16);\n"
        "export function init(value) {\n"
        "  shared.baseOffset = new Vec3(10, 20, 0);\n"
        "  let c = WEColor.hsv2rgb(WEColor.rgb2hsv(new Vec3(0.2, 0.6, 0.8)));\n"
        "  let m = WEMath.smoothStep(0.0, 10.0, 5.0) * WEMath.deg2rad * WEMath.rad2deg;\n"
        "  return value.add(shared.baseOffset).add(new Vec3(m * 0.0 + c.x * 0.0, 0, 0));\n"
        "}\n"
        "export function update(value) {\n"
        "  let cursor = input.cursorWorldPosition;\n"
        "  let bass = audioBuf.average[0];\n"
        "  thisLayer.alpha = WEMath.mix(0.5, 1.0, bass);\n"
        "  return new Vec3(100 + cursor.x * 0.1 * scriptProperties.speed, 200 + bass * 50.0, 0);\n"
        "}\n"
        "export function applyUserProperties(props) {\n"
        "  if (props.speed !== undefined) scriptProperties.speed = props.speed.value;\n"
        "}\n";

    we_script_t *vm = we_script_create(origin_src, strlen(origin_src), &shared);
    assert(vm != NULL);
    assert(we_script_uses_audio(vm));
    assert(we_script_arena_used(vm) > 0u && we_script_arena_used(vm) < WE_SCRIPT_ARENA_SIZE);

    we_script_env_t env;
    memset(&env, 0, sizeof(env));
    env.frametime = 1.0f / 60.0f;
    env.runtime = 1.5f;
    env.time_of_day = 0.5f;
    env.screen_resolution[0] = 1920.0f;
    env.screen_resolution[1] = 1080.0f;
    env.canvas_size[0] = 1920.0f;
    env.canvas_size[1] = 1080.0f;
    env.cursor_world[0] = 50.0f;
    env.cursor_world[1] = 100.0f;
    env.audio_average[0] = 0.8f;
    env.audio_left[0] = 0.8f;
    env.audio_right[0] = 0.8f;

    float out_origin[3] = {0};
    assert(we_script_eval_init(vm, &env, &state, state.origin, 3u, out_origin));
    assert(!we_script_was_aborted(vm));
    assert(fabsf(out_origin[0] - 110.0f) < 1e-4f);
    assert(fabsf(out_origin[1] - 220.0f) < 1e-4f);
    /* Verify shared.baseOffset was synced back to C shared table */
    assert(shared.count >= 1u);

    assert(we_script_eval_update(vm, &env, &state, state.origin, 3u, out_origin));
    assert(!we_script_was_aborted(vm));
    /* cursor.x = 50, speed = 2.0 -> x = 100 + 50 * 0.1 * 2.0 = 110.0f
     * bass = 0.8 -> y = 200 + 0.8 * 50 = 240.0f
     * thisLayer.alpha = mix(0.5, 1.0, 0.8) = 0.9f */
    assert(fabsf(out_origin[0] - 110.0f) < 1e-4f);
    assert(fabsf(out_origin[1] - 240.0f) < 1e-4f);
    assert(fabsf(state.alpha - 0.9f) < 1e-4f);

    /* Test applyUserProperties live update */
    const float new_speed = 4.0f;
    we_script_set_prop(vm, "speed", &new_speed, 1u, NULL);
    assert(we_script_eval_apply_user_properties(vm, &env, &state));
    assert(we_script_eval_update(vm, &env, &state, state.origin, 3u, out_origin));
    assert(fabsf(out_origin[0] - 120.0f) < 1e-4f); /* 100 + 50 * 0.1 * 4.0 = 120.0f */

    /* Regression test: Global object/array mutations, custom `this.<prop>` instance properties,
     * undeclared global variable assignment inside init(), function expressions, and
     * engine.timeOfDay() across 50 consecutive update() ticks with arena copy-compaction */
    {
        const char *stateful_src =
            "'use strict';\n"
            "var cfg = {};\n"
            "var arr = [];\n"
            "var helper = function(x) { return x + 1.0; };\n"
            "export function init(value) {\n"
            "  cfg.step = 2.0;\n"
            "  cfg.vec = new Vec3(5.0, 6.0, 7.0);\n"
            "  arr.push(10.0);\n"
            "  this.customBase = 40.0;\n"
            "  undeclaredGlobal = 3.0;\n"
            "  return value;\n"
            "}\n"
            "export function update(value) {\n"
            "  cfg.step = helper(cfg.step);\n"
            "  cfg.dynamicObj = { val: cfg.step };\n"
            "  this.customBase += 1.0;\n"
            "  return this.customBase + cfg.dynamicObj.val + arr[0] + cfg.vec.x + "
            "undeclaredGlobal + engine.timeOfDay() * 2.0;\n"
            "}\n";
        we_script_t *svm = we_script_create(stateful_src, strlen(stateful_src), &shared);
        assert(svm != NULL);
        float sv_in = 1.0f, sv_out = 0.0f;
        assert(we_script_eval_init(svm, &env, &state, &sv_in, 1u, &sv_out));
        size_t steady_arena = 0;
        for (int tick = 1; tick <= 50; tick++) {
            assert(we_script_eval_update(svm, &env, &state, &sv_in, 1u, &sv_out));
            assert(!we_script_was_aborted(svm));
            if (tick == 5) {
                steady_arena = we_script_arena_used(svm);
            } else if (tick > 5) {
                /* Arena usage must remain bounded across frames (copy-compaction reclaims dead
                 * frame temporaries) */
                assert(we_script_arena_used(svm) == steady_arena);
            }
        }
        /* After 50 ticks:
         * customBase = 40 + 50 = 90
         * cfg.step = 2 + 50 = 52
         * arr[0] = 10, cfg.vec.x = 5, undeclaredGlobal = 3, engine.timeOfDay() * 2 = 1.0
         * total = 90 + 52 + 10 + 5 + 3 + 1 = 161.0f */
        assert(fabsf(sv_out - 161.0f) < 1e-4f);
        we_script_destroy(svm);
    }

    /* Test Pre-emptive Infinite-Loop Watchdog:
     * A runaway `while (true) {}` script must be interrupted within 100,000 steps / 2.0ms
     * without hanging or corrupting state! */
    const char *runaway_src = "'use strict';\n"
                              "export function update(value) {\n"
                              "  while (true) {}\n"
                              "  return 0.0;\n"
                              "}\n";
    we_script_t *bad_vm = we_script_create(runaway_src, strlen(runaway_src), &shared);
    assert(bad_vm != NULL);
    float in_alpha = 0.9f;
    float out_alpha = 0.0f;
    assert(!we_script_eval_update(bad_vm, &env, &state, &in_alpha, 1u, &out_alpha));
    assert(we_script_was_aborted(bad_vm));
    /* Alpha must fall back to in_alpha (0.9f) despite the runaway script being aborted */
    assert(fabsf(out_alpha - 0.9f) < 1e-4f);

    we_script_destroy(bad_vm);
    we_script_destroy(vm);
}

/* =========================================================================
 * 9. End-to-End Scene Integration (Puppet + Particles + SceneScript)
 * ========================================================================= */

static void test_puppet_particle_script_scene_integration(void)
{
    printf("  [TEST] Testing E2E Scene Integration (.mdl Puppet + Particles + "
           "SceneScript)...\n");

    const char *scn_s =
        "{\n"
        "  \"general\": {\n"
        "    \"orthogonalprojection\": {\"width\": 16, \"height\": 16},\n"
        "    \"clearcolor\": \"0.0 0.0 0.0\"\n"
        "  },\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"hero_puppet\",\n"
        "      \"origin\": {\n"
        "        \"value\": \"8.0 8.0 0.0\",\n"
        "        \"script\": \"'use strict'; export function update(v) { return v; }\"\n"
        "      },\n"
        "      \"size\": \"16.0 16.0\",\n"
        "      \"scale\": \"1.0 1.0 1.0\",\n"
        "      \"angles\": \"0.0 0.0 0.0\",\n"
        "      \"alpha\": 1.0,\n"
        "      \"visible\": true,\n"
        "      \"image\": \"models/hero.json\",\n"
        "      \"animationlayers\": [\n"
        "        {\"animation\": 100, \"rate\": 1.0, \"blend\": 1.0, \"visible\": true, \"name\": "
        "\"idle\"}\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"magic_sparks\",\n"
        "      \"origin\": \"8.0 8.0 0.0\",\n"
        "      \"scale\": \"1.0 1.0 1.0\",\n"
        "      \"angles\": \"0.0 0.0 0.0\",\n"
        "      \"alpha\": {\n"
        "        \"value\": 1.0,\n"
        "        \"script\": \"'use strict'; export function update(v) { return 1.0; }\"\n"
        "      },\n"
        "      \"visible\": true,\n"
        "      \"particle\": \"particles/sparks.json\"\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const char *hero_model_s = "{\n"
                               "  \"material\": \"materials/hero.json\",\n"
                               "  \"puppet\": \"models/hero.mdl\"\n"
                               "}\n";
    const char *hero_mat_s = "{\n"
                             "  \"passes\": [\n"
                             "    {\"blending\": \"normal\", \"textures\": [\"hero\"]}\n"
                             "  ]\n"
                             "}\n";
    const char *sparks_json_s =
        "{\n"
        "  \"maxcount\": 16,\n"
        "  \"material\": \"materials/hero.json\",\n"
        "  \"emitter\": [\n"
        "    {\"name\": \"boxrandom\", \"rate\": 60.0, \"instantaneous\": 8, \"distancemin\": \"-2 "
        "-2 0\", \"distancemax\": \"2 2 0\"}\n"
        "  ],\n"
        "  \"initializer\": [\n"
        "    {\"name\": \"lifetimerandom\", \"min\": 2.0, \"max\": 3.0},\n"
        "    {\"name\": \"sizerandom\", \"min\": 4.0, \"max\": 4.0},\n"
        "    {\"name\": \"alpharandom\", \"min\": 1.0, \"max\": 1.0}\n"
        "  ],\n"
        "  \"operator\": [\n"
        "    {\"name\": \"movement\", \"drag\": 0.1, \"gravity\": \"0 0 0\"}\n"
        "  ]\n"
        "}\n";

    byte_buf_t hero_mdl, hero_tex;
    build_synthetic_mdl(&hero_mdl, 0, false);
    build_top_blue_bottom_red_tex(&hero_tex);

    const pkg_test_item_t pitems[] = {
        {.path = "scene.json", .data = scn_s, .len = strlen(scn_s)},
        {.path = "models/hero.json", .data = hero_model_s, .len = strlen(hero_model_s)},
        {.path = "models/hero.mdl", .data = hero_mdl.data, .len = hero_mdl.len},
        {.path = "materials/hero.json", .data = hero_mat_s, .len = strlen(hero_mat_s)},
        {.path = "materials/hero.tex", .data = hero_tex.data, .len = hero_tex.len},
        {.path = "particles/sparks.json", .data = sparks_json_s, .len = strlen(sparks_json_s)},
    };

    byte_buf_t spkg;
    build_synthetic_pkg(&spkg, "PKGV0014", pitems, 6);

    char tmp_dir[] = "/tmp/wwal_we_e2e_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);
    char spkg_path[4096];
    snprintf(spkg_path, sizeof(spkg_path), "%s/scene.pkg", tmp_dir);
    FILE *f = fopen(spkg_path, "wb");
    assert(f != NULL);
    fwrite(spkg.data, 1, spkg.len, f);
    fclose(f);

    bb_free(&hero_mdl);
    bb_free(&hero_tex);
    bb_free(&spkg);

    /* 1. Test CPU fallback path */
    we_scene_t *cpu_scn = we_scene_load_cpu(tmp_dir, NULL);
    assert(cpu_scn != NULL);
    assert(cpu_scn->object_count == 2u);
    assert(cpu_scn->objects[0].puppet != NULL);
    assert(cpu_scn->objects[0].script_count == 1u && cpu_scn->objects[0].scripts[0].vm != NULL);
    assert(cpu_scn->objects[1].particle_sys != NULL);
    assert(cpu_scn->objects[1].script_count == 1u && cpu_scn->objects[1].scripts[0].vm != NULL);

    uint8_t fb[16 * 16 * 4] = {0};
    waywal_shader_frame_params_t fp;
    memset(&fp, 0, sizeof(fp));
    fp.time_sec = 0.0f;
    fp.time_delta_sec = 1.0f / 60.0f;
    fp.mouse[0] = 8.0f;
    fp.mouse[1] = 8.0f;
    assert(we_scene_render_rgba8(cpu_scn, 16, 16, &fp, fb));
    /* Center pixel must have non-zero color from puppet/particles */
    assert(fb[(8 * 16 + 8) * 4 + 3] == 255);
    assert(fb[(8 * 16 + 8) * 4 + 0] > 0 || fb[(8 * 16 + 8) * 4 + 2] > 0);
    we_scene_destroy(cpu_scn);

    /* 2. Test full scene (GL if available) */
    we_scene_t *gl_scn = we_scene_create(NULL, tmp_dir, NULL);
    assert(gl_scn != NULL);
    memset(fb, 0, sizeof(fb));
    fp.time_sec = 0.0f;
    assert(we_scene_render_rgba8(gl_scn, 16, 16, &fp, fb));
    assert(fb[(8 * 16 + 8) * 4 + 3] == 255);
    assert(fb[(8 * 16 + 8) * 4 + 0] > 0 || fb[(8 * 16 + 8) * 4 + 2] > 0);
    we_scene_destroy(gl_scn);

    unlink(spkg_path);
    rmdir(tmp_dir);
}

/* =========================================================================
 * 10. SceneScript Cyclic GC, Cross-Layer Access, Puppet Pose & Particle Vortex
 * ========================================================================= */

static void test_script_gc_puppet_pose_and_particle_operators(void)
{
    printf("  [TEST] Testing SceneScript Cyclic GC, Recursion Guard, "
           "Cross-Layer Access, Puppet Pose, and Particle Vortex/Oscillate...\n");

    we_script_shared_t shared;
    memset(&shared, 0, sizeof(shared));
    we_script_env_t env;
    memset(&env, 0, sizeof(env));
    env.frametime = 1.0f / 60.0f;
    env.runtime = 1.0f;
    env.screen_resolution[0] = 1920.0f;
    env.screen_resolution[1] = 1080.0f;
    env.canvas_size[0] = 1920.0f;
    env.canvas_size[1] = 1080.0f;

    we_script_layer_state_t state = {
        .origin = {10.0f, 20.0f, 0.0f},
        .scale = {1.0f, 1.0f, 1.0f},
        .angles = {0.0f, 0.0f, 0.0f},
        .size = {100.0f, 100.0f},
        .color = {1.0f, 1.0f, 1.0f},
        .alpha = 1.0f,
        .visible = true,
        .name = "self_layer",
        .id = 1,
    };

    /* 1. Cyclic VAL_OBJ / VAL_ARR promotion across 10 update() frames without infinite recursion or
     * arena leaks */
    {
        const char *cyclic_src = "'use strict';\n"
                                 "var rootObj = {};\n"
                                 "var rootArr = [];\n"
                                 "export function init(value) {\n"
                                 "  rootObj.self = rootObj;\n"
                                 "  rootArr.push(rootArr);\n"
                                 "  rootObj.arr = rootArr;\n"
                                 "  return value;\n"
                                 "}\n"
                                 "export function update(value) {\n"
                                 "  let a = { val: 7.0 };\n"
                                 "  a.self = a;\n"
                                 "  let b = [1.0, 2.0];\n"
                                 "  b.push(b);\n"
                                 "  rootObj.child = a;\n"
                                 "  rootObj.list = b;\n"
                                 "  return value + rootObj.self.child.self.val + "
                                 "rootObj.list[0];\n"
                                 "}\n";
        we_script_t *cvm = we_script_create(cyclic_src, strlen(cyclic_src), &shared);
        assert(cvm != NULL);
        float in_v = 2.0f, out_v = 0.0f;
        assert(we_script_eval_init(cvm, &env, &state, &in_v, 1u, &out_v));
        size_t baseline_arena = 0;
        for (int frame = 0; frame < 10; frame++) {
            assert(we_script_eval_update(cvm, &env, &state, &in_v, 1u, &out_v));
            assert(!we_script_was_aborted(cvm));
            assert(fabsf(out_v - 10.0f) < 1e-4f);
            if (frame == 2) {
                baseline_arena = we_script_arena_used(cvm);
            } else if (frame > 2) {
                assert(we_script_arena_used(cvm) == baseline_arena);
            }
        }
        we_script_destroy(cvm);
    }

    /* 2. Deep AST (10,000-deep nested parens) and infinite recursive function must abort cleanly
     * without SIGSEGV */
    {
        size_t deep_cap = 20064;
        char *deep_src = malloc(deep_cap);
        assert(deep_src != NULL);
        size_t pos = 0;
        const char *prefix = "export function update(v) { return ";
        memcpy(deep_src + pos, prefix, strlen(prefix));
        pos += strlen(prefix);
        for (int i = 0; i < 10000; i++) {
            deep_src[pos++] = '(';
        }
        deep_src[pos++] = '1';
        for (int i = 0; i < 10000; i++) {
            deep_src[pos++] = ')';
        }
        const char *suffix = "; }\n";
        memcpy(deep_src + pos, suffix, strlen(suffix));
        pos += strlen(suffix);
        deep_src[pos] = '\0';

        we_script_t *dvm = we_script_create(deep_src, pos, &shared);
        if (dvm) {
            float in_v = 1.0f, out_v = 0.0f;
            (void)we_script_eval_update(dvm, &env, &state, &in_v, 1u, &out_v);
            we_script_destroy(dvm);
        }
        free(deep_src);

        const char *rec_src = "'use strict';\n"
                              "function f() { return f(); }\n"
                              "export function update(value) {\n"
                              "  return f();\n"
                              "}\n";
        we_script_t *rvm = we_script_create(rec_src, strlen(rec_src), &shared);
        assert(rvm != NULL);
        float in_v = 3.5f, out_v = 0.0f;
        assert(!we_script_eval_update(rvm, &env, &state, &in_v, 1u, &out_v));
        assert(we_script_was_aborted(rvm));
        assert(fabsf(out_v - 3.5f) < 1e-4f);
        we_script_destroy(rvm);

        /* Mutual recursion inside try/catch must abort cleanly without executing catch */
        const char *mrec_src = "'use strict';\n"
                               "var shared;\n"
                               "var shared = shared || {};\n"
                               "function f() { return g(); }\n"
                               "function g() { return h(); }\n"
                               "function h() { return f(); }\n"
                               "export function init(v) {\n"
                               "  shared.keepMe = 42.0;\n"
                               "  return shared.keepMe;\n"
                               "}\n"
                               "export function update(value) {\n"
                               "  try { f(); } catch (e) { shared.keepMe = -1.0; return 999.0; }\n"
                               "  return 888.0;\n"
                               "}\n";
        we_script_t *mrvm = we_script_create(mrec_src, strlen(mrec_src), &shared);
        assert(mrvm != NULL);
        assert(we_script_eval_init(mrvm, &env, &state, &in_v, 1u, &out_v));
        assert(fabsf(out_v - 42.0f) < 1e-4f);
        assert(!we_script_eval_update(mrvm, &env, &state, &in_v, 1u, &out_v));
        assert(we_script_was_aborted(mrvm));
        assert(fabsf(out_v - 3.5f) < 1e-4f);
        we_script_destroy(mrvm);
    }

    /* 3. Cross-layer thisScene.getLayer("other") mutation & non-clobbering of unaccessed sibling
     * layers */
    {
        const char *cross_scn_s =
            "{\n"
            "  \"general\": {\n"
            "    \"orthogonalprojection\": {\"width\": 16, \"height\": 16},\n"
            "    \"clearcolor\": \"0.0 0.0 0.0\"\n"
            "  },\n"
            "  \"objects\": [\n"
            "    {\n"
            "      \"name\": \"controller\",\n"
            "      \"id\": 10,\n"
            "      \"origin\": {\n"
            "        \"value\": \"8.0 8.0 0.0\",\n"
            "        \"script\": \"'use strict'; export function update(v) { let other = "
            "thisScene.getLayer('other'); if (other) { other.origin = new Vec3(4.0, 12.0, 0.0); "
            "other.visible = false; } return v; }\"\n"
            "      },\n"
            "      \"size\": \"16.0 16.0\",\n"
            "      \"visible\": true\n"
            "    },\n"
            "    {\n"
            "      \"name\": \"other\",\n"
            "      \"id\": 20,\n"
            "      \"origin\": \"1.0 2.0 0.0\",\n"
            "      \"size\": \"16.0 16.0\",\n"
            "      \"visible\": true\n"
            "    },\n"
            "    {\n"
            "      \"name\": \"untouched\",\n"
            "      \"id\": 30,\n"
            "      \"origin\": \"3.0 6.0 0.0\",\n"
            "      \"size\": \"16.0 16.0\",\n"
            "      \"visible\": true\n"
            "    }\n"
            "  ]\n"
            "}\n";
        const pkg_test_item_t citems[] = {
            {.path = "scene.json", .data = cross_scn_s, .len = strlen(cross_scn_s)},
        };
        byte_buf_t cpkg;
        build_synthetic_pkg(&cpkg, "PKGV0014", citems, 1);
        char ctmp_dir[] = "/tmp/wwal_we_cross_XXXXXX";
        assert(mkdtemp(ctmp_dir) != NULL);
        char cpkg_path[4096];
        snprintf(cpkg_path, sizeof(cpkg_path), "%s/scene.pkg", ctmp_dir);
        FILE *cf = fopen(cpkg_path, "wb");
        assert(cf != NULL);
        fwrite(cpkg.data, 1, cpkg.len, cf);
        fclose(cf);
        bb_free(&cpkg);

        we_scene_t *cscn = we_scene_load_cpu(ctmp_dir, NULL);
        assert(cscn != NULL);
        assert(cscn->object_count == 3u);
        assert(cscn->objects[1].visible == true);

        /* Mutate the unaccessed sibling layer ("untouched") before rendering and verify that
         * controller's script (which only accesses "other") does NOT clobber "untouched" back to
         * its initial scene_layers snapshot! */
        cscn->objects[2].origin[0] = 77.0f;
        cscn->objects[2].alpha = 0.25f;

        uint8_t cfb[16 * 16 * 4] = {0};
        waywal_shader_frame_params_t cfp;
        memset(&cfp, 0, sizeof(cfp));
        cfp.time_delta_sec = 1.0f / 60.0f;
        assert(we_scene_render_rgba8(cscn, 16, 16, &cfp, cfb));
        assert(fabsf(cscn->objects[1].origin[0] - 4.0f) < 1e-4f);
        assert(fabsf(cscn->objects[1].origin[1] - 12.0f) < 1e-4f);
        assert(cscn->objects[1].visible == false);
        assert(fabsf(cscn->objects[2].origin[0] - 77.0f) < 1e-4f);
        assert(fabsf(cscn->objects[2].alpha - 0.25f) < 1e-4f);

        we_scene_destroy(cscn);
        unlink(cpkg_path);
        rmdir(ctmp_dir);
    }

    /* 4. Puppet rate = 0.0 static pose, anim_id = -1 name resolution, and MDLA multi-bone implicit
     * track ordering */
    {
        byte_buf_t pmdl;
        build_synthetic_mdl(&pmdl, 0, false);

        /* Verify MDLA implicit track ordering by constructing a 2-bone MDLA where both
         * tracks have bone_index = 0 and unk = 0 in the binary stream. */
        we_puppet_t *p = we_puppet_parse_cpu(pmdl.data, pmdl.len);
        assert(p != NULL);
        p->anims[0].tracks[0].keyframes[0].pos[0] = 25.0f;

        we_puppet_layer_t static_layer = {
            .anim_id = 100,
            .rate = 0.0f,
            .blend = 1.0f,
            .visible = true,
            .anim_name = "",
        };
        we_puppet_eval_skin_matrices(p, &static_layer, 1u, 1.0, p->skin_mats);
        assert(fabsf(p->skin_mats[0][12] - 15.0f) < 1e-4f);

        we_puppet_layer_t named_layer = {
            .anim_id = -1,
            .rate = 1.0f,
            .blend = 1.0f,
            .visible = true,
            .anim_name = "idle",
        };
        we_puppet_eval_skin_matrices(p, &named_layer, 1u, 0.5, p->skin_mats);
        assert(fabsf(p->skin_mats[0][12] - 17.5f) < 1e-4f);
        we_puppet_destroy(p);

        /* Test implicit track ordering: patch Track 1's bone_index from 1 to 0 in all 3
         * animations of pmdl and set Track 1's Y translation to +7.0f in Anim 0. */
        byte_buf_t imdl;
        bb_init(&imdl);
        /* Copy everything up to MDLA0003 from pmdl, then write a 1-anim 2-track MDLA where both
         * tracks have bone_index == 0 and unk == 0 */
        size_t mdla_off = 0;
        for (size_t i = 0; i + 8 <= pmdl.len; i++) {
            if (memcmp(pmdl.data + i, "MDLA0003", 8) == 0) {
                mdla_off = i;
                break;
            }
        }
        assert(mdla_off > 0);
        bb_append(&imdl, pmdl.data, mdla_off);
        bb_magic9(&imdl, "MDLA0003");
        bb_u32(&imdl, 1u);
        bb_i32(&imdl, 100);
        bb_i32(&imdl, 0);
        bb_cstr(&imdl, "implicit_bones");
        bb_cstr(&imdl, "loop");
        bb_f32(&imdl, 1.0f);
        bb_i32(&imdl, 1);
        bb_i32(&imdl, 0);
        bb_u32(&imdl, 2u); /* 2 bone tracks, both with bone_index = 0, unk = 0 */
        /* Track 0 -> implicitly Bone 0: pos=(20, 0, 0) (+10 from bind pose 10) */
        bb_i32(&imdl, 0);
        bb_i32(&imdl, 0);
        bb_u32(&imdl, 36u);
        bb_f32(&imdl, 20.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 1.0f);
        bb_f32(&imdl, 1.0f);
        bb_f32(&imdl, 1.0f);
        /* Track 1 -> bone_index = 0 in binary, must be remapped to Bone 1: pos=(5, 7, 0) (+7 Y) */
        bb_i32(&imdl, 0);
        bb_i32(&imdl, 0);
        bb_u32(&imdl, 36u);
        bb_f32(&imdl, 5.0f);
        bb_f32(&imdl, 7.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 0.0f);
        bb_f32(&imdl, 1.0f);
        bb_f32(&imdl, 1.0f);
        bb_f32(&imdl, 1.0f);

        we_puppet_t *ip = we_puppet_parse_cpu(imdl.data, imdl.len);
        assert(ip != NULL);
        assert(ip->anim_count == 1u && ip->anims[0].track_count == 2u);
        assert(ip->anims[0].tracks[0].bone_index == 0);
        assert(ip->anims[0].tracks[1].bone_index == 1);
        we_puppet_layer_t ilayer = {
            .anim_id = 100,
            .rate = 1.0f,
            .blend = 1.0f,
            .visible = true,
            .anim_name = "implicit_bones",
        };
        we_puppet_eval_skin_matrices(ip, &ilayer, 1u, 0.0, ip->skin_mats);
        assert(fabsf(ip->skin_mats[0][12] - 10.0f) < 1e-4f);
        assert(fabsf(ip->skin_mats[1][12] - 10.0f) < 1e-4f);
        assert(fabsf(ip->skin_mats[1][13] - 7.0f) < 1e-4f);
        we_puppet_destroy(ip);
        bb_free(&imdl);
        bb_free(&pmdl);
    }

    /* 5. Particle system with "vortex", "oscillatealpha", "oscillatesize", and 2D "sphererandom"
     * ring (directions = "1 1 0", r_min = r_max = 50) */
    {
        const char *vor_json =
            "{\n"
            "  \"maxcount\": 8,\n"
            "  \"starttime\": 0,\n"
            "  \"emitter\": [\n"
            "    {\"name\": \"boxrandom\", \"rate\": 0, \"instantaneous\": 4, \"distancemin\": \"5 "
            "0 0\", \"distancemax\": \"5 0 0\"}\n"
            "  ],\n"
            "  \"initializer\": [\n"
            "    {\"name\": \"lifetimerandom\", \"min\": 10.0, \"max\": 10.0},\n"
            "    {\"name\": \"sizerandom\", \"min\": 10.0, \"max\": 10.0},\n"
            "    {\"name\": \"alpharandom\", \"min\": 1.0, \"max\": 1.0}\n"
            "  ],\n"
            "  \"operator\": [\n"
            "    {\"name\": \"movement\", \"drag\": 0.0, \"gravity\": \"0 0 0\"},\n"
            "    {\"name\": \"vortex\", \"controlpoint\": 0, \"axis\": \"0 0 1\", "
            "\"distanceinner\": 1.0, \"distanceouter\": 20.0, \"speedinner\": 20.0, "
            "\"speedouter\": 20.0},\n"
            "    {\"name\": \"oscillatealpha\", \"scalemin\": 0.2, \"scalemax\": 0.8, "
            "\"frequencymin\": 4.0, \"frequencymax\": 4.0},\n"
            "    {\"name\": \"oscillatesize\", \"scalemin\": 0.5, \"scalemax\": 1.5, "
            "\"frequencymin\": 4.0, \"frequencymax\": 4.0}\n"
            "  ]\n"
            "}\n";
        we_particle_sys_t *vsys = we_particle_create_from_json(vor_json, strlen(vor_json));
        assert(vsys != NULL);
        we_particle_update(vsys, 0.05f);
        assert(vsys->alive_count == 4u);
        /* Verify positive distancemin == distancemax == 5.0 spawns at +5.0, never -5.0 */
        for (uint32_t i = 0; i < vsys->alive_count; i++) {
            assert(vsys->init_pos_x[i] > 0.0f);
        }
        vsys->rng_phase[0] = 0.0f;
        we_particle_update(vsys, 0.1f);
        /* Particle at +(5, 0, 0) with axis (0, 0, 1) experiences tangential velocity in +Y */
        assert(vsys->vel_y[0] > 0.1f);
        assert(vsys->a[0] >= 0.2f && vsys->a[0] <= 0.8f);
        assert(fabsf(vsys->a[0] - 1.0f) > 1e-3f);
        assert(vsys->size[0] >= 5.0f && vsys->size[0] <= 15.0f);
        assert(fabsf(vsys->size[0] - 10.0f) > 1e-3f);
        we_particle_destroy(vsys);

        /* Verify 2D sphererandom ring (directions = "1 1 0", r_min = r_max = 50):
         * every particle must spawn on the exact 2D circle of radius 50 in the XY plane */
        const char *ring_json =
            "{\n"
            "  \"maxcount\": 32,\n"
            "  \"starttime\": 0,\n"
            "  \"emitter\": [\n"
            "    {\"name\": \"sphererandom\", \"rate\": 0, \"instantaneous\": 16, "
            "\"directions\": \"1 1 0\", \"distancemin\": 50.0, \"distancemax\": 50.0, "
            "\"speedmin\": 10.0, \"speedmax\": 10.0}\n"
            "  ],\n"
            "  \"initializer\": [\n"
            "    {\"name\": \"lifetimerandom\", \"min\": 10.0, \"max\": 10.0}\n"
            "  ]\n"
            "}\n";
        we_particle_sys_t *rsys = we_particle_create_from_json(ring_json, strlen(ring_json));
        assert(rsys != NULL);
        we_particle_update(rsys, 0.001f);
        assert(rsys->alive_count == 16u);
        for (uint32_t i = 0; i < rsys->alive_count; i++) {
            float rad2d = hypotf(rsys->init_pos_x[i], rsys->init_pos_y[i]);
            assert(fabsf(rad2d - 50.0f) < 1e-3f);
            assert(fabsf(rsys->init_pos_z[i]) < 1e-5f);
            float spd2d = hypotf(rsys->vel_x[i], rsys->vel_y[i]);
            assert(fabsf(spd2d - 10.0f) < 1e-3f);
            assert(fabsf(rsys->vel_z[i]) < 1e-5f);
        }
        we_particle_destroy(rsys);
    }
}

typedef struct {
    uint32_t call_count;
    waywal_mpris_state_t last_state;
} test_mpris_ctx_t;

static void on_test_mpris_change(const waywal_mpris_state_t *st, void *userdata)
{
    test_mpris_ctx_t *ctx = (test_mpris_ctx_t *)userdata;
    assert(ctx != NULL && st != NULL);
    ctx->call_count++;
    ctx->last_state = *st;
}

static void write_test_bmp_4x4(const char *path)
{
    /* Minimal 24-bit 4x4 uncompressed BMP (54-byte header + 4 rows * 12 bytes = 102 bytes) */
    uint8_t bmp[102];
    memset(bmp, 0, sizeof(bmp));
    bmp[0] = 'B';
    bmp[1] = 'M';
    uint32_t file_sz = sizeof(bmp);
    uint32_t data_off = 54;
    uint32_t dib_sz = 40;
    int32_t w = 4, h = 4;
    uint16_t planes = 1, bpp = 24;
    memcpy(bmp + 2, &file_sz, 4);
    memcpy(bmp + 10, &data_off, 4);
    memcpy(bmp + 14, &dib_sz, 4);
    memcpy(bmp + 18, &w, 4);
    memcpy(bmp + 22, &h, 4);
    memcpy(bmp + 26, &planes, 2);
    memcpy(bmp + 28, &bpp, 2);
    for (int i = 0; i < 16; ++i) {
        uint8_t *px = bmp + 54 + i * 3;
        px[0] = 240; /* B */
        px[1] = 40;  /* G */
        px[2] = 220; /* R */
    }
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(bmp, 1, sizeof(bmp), f) == sizeof(bmp));
    fclose(f);
}

static void test_mpris_audio_ducking_and_live_uniforms(void)
{
    printf("  [TEST] Testing MPRIS2 URI decoding, album art, PipeWire ducking, live uniforms & "
           "CLI...\n");

    /* 1. Safe file:// URI decoding & security boundaries */
    {
        char decoded[512];
        assert(waywal_mpris_decode_file_uri("file:///tmp/my%20album%20%E2%99%AA.png", decoded,
                                            sizeof(decoded)));
        assert(strcmp(decoded, "/tmp/my album \xE2\x99\xAA.png") == 0);

        assert(waywal_mpris_decode_file_uri("file://localhost/tmp/cover.jpg", decoded,
                                            sizeof(decoded)));
        assert(strcmp(decoded, "/tmp/cover.jpg") == 0);

        /* Reject http/https, relative paths, '..' segments, %00, and malformed %XX */
        assert(
            !waywal_mpris_decode_file_uri("http://example.com/art.png", decoded, sizeof(decoded)));
        assert(
            !waywal_mpris_decode_file_uri("https://example.com/art.png", decoded, sizeof(decoded)));
        assert(!waywal_mpris_decode_file_uri("/tmp/not_a_uri.png", decoded, sizeof(decoded)));
        assert(
            !waywal_mpris_decode_file_uri("file:///tmp/../etc/passwd", decoded, sizeof(decoded)));
        assert(!waywal_mpris_decode_file_uri("file:///tmp/%2e%2e/secret.png", decoded,
                                             sizeof(decoded)));
        assert(
            !waywal_mpris_decode_file_uri("file:///tmp/null%00byte.png", decoded, sizeof(decoded)));
        assert(
            !waywal_mpris_decode_file_uri("file:///tmp/bad%ZZhex.png", decoded, sizeof(decoded)));
    }

    /* 2. MPRIS2 listener state injection, album art decode via O_NOFOLLOW fd, & HCT theme
     * extraction */
    {
        char art_dir[] = "/tmp/wwal_test_mpris_XXXXXX";
        assert(mkdtemp(art_dir) != NULL);
        char bmp_path[512];
        snprintf(bmp_path, sizeof(bmp_path), "%s/cover art.bmp", art_dir);
        write_test_bmp_4x4(bmp_path);

        char art_uri[600];
        snprintf(art_uri, sizeof(art_uri), "file://%s/cover%%20art.bmp", art_dir);

        test_mpris_ctx_t mctx;
        memset(&mctx, 0, sizeof(mctx));
        waywal_mpris_t *mpris = waywal_mpris_create(on_test_mpris_change, &mctx);
        assert(mpris != NULL);

        waywal_mpris_state_t fake_st;
        memset(&fake_st, 0, sizeof(fake_st));
        fake_st.playing = true;
        strncpy(fake_st.playback_status, "Playing", sizeof(fake_st.playback_status) - 1);
        strncpy(fake_st.title, "Nightcall", sizeof(fake_st.title) - 1);
        strncpy(fake_st.artist, "Kavinsky", sizeof(fake_st.artist) - 1);
        strncpy(fake_st.album, "OutRun", sizeof(fake_st.album) - 1);
        strncpy(fake_st.art_url, art_uri, sizeof(fake_st.art_url) - 1);
        fake_st.length_us = 258000000;

        mctx.call_count = 0;
        waywal_mpris_inject_state_for_test(mpris, &fake_st);
        assert(mctx.call_count == 1);
        assert(waywal_mpris_is_playing(mpris));
        const waywal_mpris_state_t *cur = waywal_mpris_get_state(mpris);
        assert(cur != NULL && strcmp(cur->title, "Nightcall") == 0);
        assert(strcmp(cur->artist, "Kavinsky") == 0);
        assert(cur->has_art_pixels && cur->art_bgra != NULL && cur->art_w == 4 && cur->art_h == 4);

        char local_art[512];
        assert(waywal_mpris_decode_file_uri(cur->art_url, local_art, sizeof(local_art)));
        assert(strcmp(local_art, bmp_path) == 0);

        int afd = open(local_art, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        assert(afd >= 0);
        uint8_t *bgra = NULL;
        uint32_t aw = 0, ah = 0;
        assert(image_load_fd(afd, &bgra, &aw, &ah));
        close(afd);
        assert(bgra != NULL && aw == 4 && ah == 4);

        waywal_palette_t mpal;
        assert(waywal_palette_extract_ex(bgra, aw, ah, aw * 4u, true, WAYWAL_THEME_DARK,
                                         WAYWAL_SCHEME_TONAL_SPOT, 0.0f, &mpal));
        assert(mpal.primary.a == 255);

        /* Test Shader Engine album art upload & live custom uniform table */
        char *wrapped = waywal_shader_wrap_source("// #pragma waywal iChannel0 = mpris_art\n"
                                                  "// #pragma waywal iChannel1 = album_art\n"
                                                  "void mainImage(out vec4 c, in vec2 f) { c = "
                                                  "texture(u_album_art, vec2(u_media_playing)); }");
        assert(wrapped != NULL);
        assert(strstr(wrapped, "uniform highp sampler2D u_album_art;") != NULL);
        assert(strstr(wrapped, "uniform float u_media_playing;") != NULL);
        free(wrapped);

        waywal_shader_engine_t *seng = waywal_shader_engine_create(NULL);
        assert(seng != NULL);
        waywal_shader_engine_set_album_art_bgra(seng, bgra, aw, ah);
        waywal_shader_engine_set_media_playing(seng, 1.0f);

        const float speed_v[4] = {2.75f, 0.0f, 0.0f, 0.0f};
        const float tint_v[4] = {0.1f, 0.5f, 0.9f, 1.0f};
        assert(waywal_shader_engine_set_custom_uniform(seng, "u_Speed", speed_v, 1));
        assert(waywal_shader_engine_set_custom_uniform(seng, "u_Tint", tint_v, 3));
        assert(!waywal_shader_engine_set_custom_uniform(seng, "", speed_v, 1));
        assert(!waywal_shader_engine_set_custom_uniform(seng, "bad-name!", speed_v, 1));

        float read_v[4] = {0};
        uint32_t read_comp = 0;
        assert(waywal_shader_engine_get_custom_uniform(seng, "u_Speed", read_v, &read_comp));
        assert(read_comp == 1 && fabsf(read_v[0] - 2.75f) < 1e-5f);
        assert(waywal_shader_engine_get_custom_uniform(seng, "u_Tint", read_v, &read_comp));
        assert(read_comp == 3 && fabsf(read_v[2] - 0.9f) < 1e-5f);

        /* Update existing uniform in-place */
        const float speed_v2[4] = {4.0f, 0.0f, 0.0f, 0.0f};
        assert(waywal_shader_engine_set_custom_uniform(seng, "u_Speed", speed_v2, 1));
        assert(waywal_shader_engine_get_custom_uniform(seng, "u_Speed", read_v, &read_comp));
        assert(read_comp == 1 && fabsf(read_v[0] - 4.0f) < 1e-5f);

        waywal_shader_engine_release_scratch(seng);
        waywal_shader_engine_destroy(seng);
        image_free(bgra);
        waywal_mpris_destroy(mpris);
        unlink(bmp_path);
        rmdir(art_dir);
    }

    /* 3. Smart PipeWire Audio Playback & Auto-Mute Exponential Ducking */
    {
        waywal_audio_fft_t *af = waywal_audio_fft_create(48000, false);
        assert(af != NULL);
        assert(fabsf(waywal_audio_fft_get_volume(af) - 0.0f) < 1e-5f);
        assert(waywal_audio_fft_get_mute_on_audio(af) == true);

        waywal_audio_fft_set_volume(af, 0.8f);
        assert(fabsf(waywal_audio_fft_get_volume(af) - 0.8f) < 1e-5f);
        assert(fabsf(waywal_audio_fft_get_effective_volume(af) - 0.8f) < 1e-5f);
        assert(!waywal_audio_fft_is_ducked(af));

        /* External audio starts playing -> smooth exponential ducking toward 0.0 */
        waywal_audio_fft_set_external_audio_for_test(af, true);
        assert(waywal_audio_fft_is_ducked(af));
        float mid_gain = waywal_audio_fft_update_ducking(af, 0.05f);
        assert(mid_gain > 0.05f && mid_gain < 0.75f);
        for (int i = 0; i < 20; ++i) {
            (void)waywal_audio_fft_update_ducking(af, 0.1f);
        }
        assert(waywal_audio_fft_get_effective_volume(af) < 1e-4f);

        /* External audio stops -> smooth exponential recovery toward 0.8 */
        waywal_audio_fft_set_external_audio_for_test(af, false);
        assert(!waywal_audio_fft_is_ducked(af));
        for (int i = 0; i < 20; ++i) {
            (void)waywal_audio_fft_update_ducking(af, 0.1f);
        }
        assert(fabsf(waywal_audio_fft_get_effective_volume(af) - 0.8f) < 1e-3f);

        /* Disabling mute_on_audio prevents ducking even when external audio is active */
        waywal_audio_fft_set_mute_on_audio(af, false);
        waywal_audio_fft_set_external_audio_for_test(af, true);
        assert(!waywal_audio_fft_is_ducked(af));
        (void)waywal_audio_fft_update_ducking(af, 0.5f);
        assert(fabsf(waywal_audio_fft_get_effective_volume(af) - 0.8f) < 1e-3f);

        waywal_audio_fft_destroy(af);
    }

    /* 4. Live Scene Uniform & User Property Override (we_scene_set_uniform_override) */
    {
        char stmp[] = "/tmp/wwal_test_live_scene_XXXXXX";
        assert(mkdtemp(stmp) != NULL);
        const char *pjson =
            "{\n"
            "  \"title\": \"Live Prop Test\",\n"
            "  \"type\": \"scene\",\n"
            "  \"file\": \"scene.json\",\n"
            "  \"general\": {\n"
            "    \"properties\": {\n"
            "      \"tintcolor\": {\"type\": \"color\", \"value\": \"1.0 1.0 1.0\"}\n"
            "    }\n"
            "  }\n"
            "}\n";
        char ppath[512];
        snprintf(ppath, sizeof(ppath), "%s/project.json", stmp);
        FILE *pf = fopen(ppath, "wb");
        assert(pf != NULL);
        fwrite(pjson, 1, strlen(pjson), pf);
        fclose(pf);

        const char *sjson =
            "{\n"
            "  \"general\": {\"orthogonalprojection\": {\"width\": 16, \"height\": 16}},\n"
            "  \"objects\": [\n"
            "    {\n"
            "      \"id\": 1,\n"
            "      \"name\": \"Quad\",\n"
            "      \"origin\": \"8.0 8.0 0.0\",\n"
            "      \"size\": \"16.0 16.0\",\n"
            "      \"color\": {\"user\": \"tintcolor\", \"value\": \"1.0 1.0 1.0\"}\n"
            "    }\n"
            "  ]\n"
            "}\n";
        pkg_test_item_t it = {.path = "scene.json", .data = sjson, .len = strlen(sjson)};
        byte_buf_t pb;
        build_synthetic_pkg(&pb, "PKGV0002", &it, 1);
        char kpath[512];
        snprintf(kpath, sizeof(kpath), "%s/scene.pkg", stmp);
        FILE *kf = fopen(kpath, "wb");
        assert(kf != NULL);
        fwrite(pb.data, 1, pb.len, kf);
        fclose(kf);
        bb_free(&pb);

        we_scene_t *scn = we_scene_load_cpu(stmp, NULL);
        assert(scn != NULL);
        assert(fabsf(scn->objects[0].color[0] - 1.0f) < 1e-5f);

        const float green_rgb[4] = {0.0f, 1.0f, 0.25f, 1.0f};
        assert(we_scene_set_uniform_override(scn, "tintcolor", green_rgb, 3));
        assert(fabsf(scn->objects[0].color[0] - 0.0f) < 1e-5f);
        assert(fabsf(scn->objects[0].color[1] - 1.0f) < 1e-5f);
        assert(fabsf(scn->objects[0].color[2] - 0.25f) < 1e-5f);

        const float custom_u[4] = {3.5f, 0.0f, 0.0f, 0.0f};
        assert(we_scene_set_uniform_override(scn, "u_BloomStrength", custom_u, 1));
        assert(scn->project.prop_count == 2u);
        assert(strcmp(scn->project.props[1].key, "u_BloomStrength") == 0);
        assert(fabsf(scn->project.props[1].val[0] - 3.5f) < 1e-5f);

        /* Verify we_scene_set_album_art_bgra and we_scene_set_media_playing */
        uint8_t art_bgra[16] = {
            10, 20, 240, 255, 10, 20, 240, 255, 10, 20, 240, 255, 10, 20, 240, 255,
        };
        we_scene_set_media_playing(scn, 1.0f);
        we_scene_set_album_art_bgra(scn, art_bgra, 2, 2);
        assert(fabsf(scn->media_playing - 1.0f) < 1e-5f);
        assert(scn->album_art_rgba != NULL);
        assert(scn->album_art_w == 2u && scn->album_art_h == 2u);
        assert(scn->album_art_rgba[0] == 240u && scn->album_art_rgba[1] == 20u &&
               scn->album_art_rgba[2] == 10u && scn->album_art_rgba[3] == 255u);
        we_scene_set_album_art_bgra(scn, NULL, 0, 0);
        assert(scn->album_art_rgba == NULL && scn->album_art_w == 0u);

        assert(WAYWAL_IPC_OP_QUERY_JSON == WAYWAL_REQ_QUERY_JSON);
        assert(WAYWAL_IPC_OP_SET_UNIFORM == WAYWAL_REQ_SET_UNIFORM);
        assert(WAYWAL_IPC_OP_SUBSCRIBE == WAYWAL_REQ_SUBSCRIBE);
        assert(WAYWAL_IPC_OP_AUDIO_CTRL == WAYWAL_REQ_AUDIO_CTRL);

        we_scene_destroy(scn);
        unlink(kpath);
        unlink(ppath);
        rmdir(stmp);
    }

    /* 5. TOML Config & CLI Parser Audio, MPRIS Theme, and JSON Options */
    {
        const char *toml_audio = "[daemon]\n"
                                 "volume = 0.65\n"
                                 "mute_on_audio = false\n\n"
                                 "[theme]\n"
                                 "theme_from_media = true\n";
        waywal_config_t cfg;
        char err[128] = {0};
        assert(waywal_config_parse_string(toml_audio, &cfg, err, sizeof(err)));
        assert(fabsf(cfg.daemon.volume - 0.65f) < 1e-5f);
        assert(cfg.daemon.mute_on_audio == false);
        assert(cfg.theme.theme_from_media == true);

        cli_options_t copts;
        char *argv_status[] = {"wwal", "status", "--json", NULL};
        assert(cli_parse(3, argv_status, &copts));
        assert(copts.cmd == CLI_CMD_STATUS && copts.json_output);

        char *argv_qjson[] = {"wwal", "query", "-j", NULL};
        assert(cli_parse(3, argv_qjson, &copts));
        assert(copts.cmd == CLI_CMD_QUERY && copts.json_output);

        char *argv_unif[] = {"wwal",     "set-uniform", "--outputs", "DP-1",
                             "u_Offset", "-0.5",        "1.25",      NULL};
        assert(cli_parse(7, argv_unif, &copts));
        assert(copts.cmd == CLI_CMD_SET_UNIFORM);
        assert(strcmp(copts.uniform_name, "u_Offset") == 0);
        assert(copts.uniform_components == 2);
        assert(fabsf(copts.uniform_values[0] - (-0.5f)) < 1e-5f);
        assert(fabsf(copts.uniform_values[1] - 1.25f) < 1e-5f);

        char *argv_sub[] = {"wwal", "subscribe", NULL};
        assert(cli_parse(2, argv_sub, &copts));
        assert(copts.cmd == CLI_CMD_SUBSCRIBE);

        char *argv_aud[] = {
            "wwal", "audio", "--volume", "0.45", "--no-mute-on-audio", "--theme-from-media", NULL};
        assert(cli_parse(6, argv_aud, &copts));
        assert(copts.cmd == CLI_CMD_AUDIO);
        assert(fabsf(copts.volume - 0.45f) < 1e-5f);
        assert(copts.mute_on_audio == 0);
        assert(copts.theme_from_media == 1);
    }
}

static void test_mpris_arbitration_and_scene_uniform_overrides(void)
{
    printf("  [TEST] Testing MPRIS multi-player arbitration, audio un-ducking, and scene uniform "
           "overrides...\n");

    /* 0. Multi-player arbitration, duplicate slot merging, and Stopped metadata/art clearing */
    {
        char art_dir[] = "/tmp/wwal_mpris_arb_XXXXXX";
        assert(mkdtemp(art_dir) != NULL);
        char bmp_path[512];
        snprintf(bmp_path, sizeof(bmp_path), "%s/cover.bmp", art_dir);
        write_test_bmp_4x4(bmp_path);
        char art_uri[600];
        snprintf(art_uri, sizeof(art_uri), "file://%s/cover.bmp", art_dir);

        test_mpris_ctx_t mctx;
        memset(&mctx, 0, sizeof(mctx));
        waywal_mpris_t *mpris = waywal_mpris_create(on_test_mpris_change, &mctx);
        assert(mpris != NULL);

        /* Player A (spotify): unnamed owner slot arrives first, then named bus_name slot merges */
        waywal_mpris_state_t st_a;
        memset(&st_a, 0, sizeof(st_a));
        st_a.status = WAYWAL_MPRIS_PLAYING;
        strncpy(st_a.playback_status, "Playing", sizeof(st_a.playback_status) - 1);
        strncpy(st_a.title, "Track A", sizeof(st_a.title) - 1);
        strncpy(st_a.artist, "Artist A", sizeof(st_a.artist) - 1);
        strncpy(st_a.art_url, art_uri, sizeof(st_a.art_url) - 1);
        st_a.length_us = 180000000;
        waywal_mpris_update_player_for_test(mpris, "", ":1.101", &st_a);
        waywal_mpris_update_player_for_test(mpris, "org.mpris.MediaPlayer2.spotify", ":1.101",
                                            &st_a);

        const waywal_mpris_state_t *cur = waywal_mpris_get_state(mpris);
        assert(cur != NULL);
        assert(cur->playing && cur->status == WAYWAL_MPRIS_PLAYING);
        assert(strcmp(cur->player_name, "org.mpris.MediaPlayer2.spotify") == 0);
        assert(strcmp(cur->title, "Track A") == 0);
        assert(cur->has_art_pixels && cur->art_bgra != NULL);
        (void)waywal_mpris_consume_art_changed(mpris);

        /* Player B (firefox) starts Paused in background -> must NOT hijack Playing Player A */
        waywal_mpris_state_t st_b;
        memset(&st_b, 0, sizeof(st_b));
        st_b.status = WAYWAL_MPRIS_PAUSED;
        strncpy(st_b.playback_status, "Paused", sizeof(st_b.playback_status) - 1);
        strncpy(st_b.title, "Video B", sizeof(st_b.title) - 1);
        waywal_mpris_update_player_for_test(mpris, "org.mpris.MediaPlayer2.firefox", ":1.202",
                                            &st_b);
        cur = waywal_mpris_get_state(mpris);
        assert(strcmp(cur->player_name, "org.mpris.MediaPlayer2.spotify") == 0);
        assert(strcmp(cur->title, "Track A") == 0);

        /* Player B transitions to Playing -> preempts Player A */
        st_b.status = WAYWAL_MPRIS_PLAYING;
        strncpy(st_b.playback_status, "Playing", sizeof(st_b.playback_status) - 1);
        waywal_mpris_update_player_for_test(mpris, "org.mpris.MediaPlayer2.firefox", ":1.202",
                                            &st_b);
        cur = waywal_mpris_get_state(mpris);
        assert(strcmp(cur->player_name, "org.mpris.MediaPlayer2.firefox") == 0);
        assert(strcmp(cur->title, "Video B") == 0);

        /* Player A pauses while Player B disconnects -> falls back to Paused Player A */
        st_a.status = WAYWAL_MPRIS_PAUSED;
        strncpy(st_a.playback_status, "Paused", sizeof(st_a.playback_status) - 1);
        waywal_mpris_update_player_for_test(mpris, "org.mpris.MediaPlayer2.spotify", ":1.101",
                                            &st_a);
        waywal_mpris_remove_player_for_test(mpris, "org.mpris.MediaPlayer2.firefox", ":1.202");
        cur = waywal_mpris_get_state(mpris);
        assert(strcmp(cur->player_name, "org.mpris.MediaPlayer2.spotify") == 0);
        assert(cur->status == WAYWAL_MPRIS_PAUSED && !cur->playing);
        assert(strcmp(cur->title, "Track A") == 0);
        assert(cur->has_art_pixels && cur->art_bgra != NULL);
        (void)waywal_mpris_consume_art_changed(mpris);

        /* Player A transitions to Stopped -> metadata and album art MUST be cleared */
        st_a.status = WAYWAL_MPRIS_STOPPED;
        strncpy(st_a.playback_status, "Stopped", sizeof(st_a.playback_status) - 1);
        waywal_mpris_update_player_for_test(mpris, "org.mpris.MediaPlayer2.spotify", ":1.101",
                                            &st_a);
        cur = waywal_mpris_get_state(mpris);
        assert(cur->status == WAYWAL_MPRIS_STOPPED && !cur->playing);
        assert(cur->title[0] == '\0' && cur->artist[0] == '\0' && cur->album[0] == '\0');
        assert(cur->length_us == 0);
        assert(cur->art_url[0] == '\0' && !cur->has_art_pixels && cur->art_bgra == NULL);
        assert(waywal_mpris_consume_art_changed(mpris));

        waywal_mpris_destroy(mpris);
        unlink(bmp_path);
        rmdir(art_dir);
    }

    /* 1. Un-ducking via waywal_audio_fft_set_mute_on_audio(fft, false) immediately restores
     * current_gain = user_volume even before the next update_ducking tick, and
     * waywal_audio_fft_process with empty ring preserves external_audio_active override. */
    {
        waywal_audio_fft_t *fft = waywal_audio_fft_create(48000, false);
        assert(fft != NULL);
        waywal_audio_fft_set_volume(fft, 0.75f);
        waywal_audio_fft_set_mute_on_audio(fft, true);
        waywal_audio_fft_set_external_audio_for_test(fft, true);

        waywal_audio_frame_t empty_frame;
        assert(waywal_audio_fft_process(fft, &empty_frame));
        assert(waywal_audio_fft_is_external_audio_active(fft));

        for (int i = 0; i < 20; ++i) {
            (void)waywal_audio_fft_update_ducking(fft, 0.1f);
        }
        assert(waywal_audio_fft_get_effective_volume(fft) < 1e-4f);

        /* Disable mute_on_audio while external audio is still active -> gain must restore! */
        waywal_audio_fft_set_mute_on_audio(fft, false);
        assert(fabsf(waywal_audio_fft_get_effective_volume(fft) - 0.75f) < 1e-5f);
        waywal_audio_fft_destroy(fft);
    }

    /* 2. CLI parser validation for bare `wwal audio` and malformed uniform names */
    {
        cli_options_t copts;
        char *argv_bare_audio[] = {"wwal", "audio", NULL};
        assert(!cli_parse(2, argv_bare_audio, &copts));

        char *argv_bad_num_name[] = {"wwal", "set-uniform", "1.5", "2.0", NULL};
        assert(!cli_parse(4, argv_bad_num_name, &copts));

        char long_name[80];
        memset(long_name, 'a', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
        char *argv_long_name[] = {"wwal", "set-uniform", long_name, "1.0", NULL};
        assert(!cli_parse(4, argv_long_name, &copts));
    }

    /* 3. waywal_shader_engine_copy_custom_uniforms preserves pre-shader uniforms */
    {
        waywal_shader_engine_t *src = waywal_shader_engine_create(NULL);
        waywal_shader_engine_t *dst = waywal_shader_engine_create(NULL);
        assert(src != NULL && dst != NULL);
        float v[2] = {1.25f, -3.5f};
        assert(waywal_shader_engine_set_custom_uniform(src, "u_PreSet", v, 2));
        waywal_shader_engine_copy_custom_uniforms(dst, src);
        float out_v[4] = {0};
        uint32_t comps = 0;
        assert(waywal_shader_engine_get_custom_uniform(dst, "u_PreSet", out_v, &comps));
        assert(comps == 2);
        assert(fabsf(out_v[0] - 1.25f) < 1e-5f);
        assert(fabsf(out_v[1] - (-3.5f)) < 1e-5f);
        waywal_shader_engine_destroy(src);
        waywal_shader_engine_destroy(dst);
    }

    /* 4. we_scene_set_uniform_override matches GLSL uniform name and material_key, and
     * preserves script state (does not re-run init()) */
    {
        char stmp[] = "/tmp/wwal_uniform_ov_XXXXXX";
        assert(mkdtemp(stmp) != NULL);

        const char *proj_json = "{\n"
                                "  \"title\": \"Uniform Override Test\",\n"
                                "  \"type\": \"scene\",\n"
                                "  \"file\": \"scene.json\",\n"
                                "  \"general\": {\n"
                                "    \"properties\": {\n"
                                "      \"speed\": {\"type\": \"slider\", \"value\": 1.0}\n"
                                "    }\n"
                                "  }\n"
                                "}\n";
        char ppath[4096];
        snprintf(ppath, sizeof(ppath), "%s/project.json", stmp);
        FILE *fp = fopen(ppath, "wb");
        assert(fp != NULL);
        fwrite(proj_json, 1, strlen(proj_json), fp);
        fclose(fp);

        const char *scn_json =
            "{\n"
            "  \"general\": {\"orthogonalprojection\": {\"width\": 64, \"height\": 64}},\n"
            "  \"objects\": [\n"
            "    {\n"
            "      \"id\": 1,\n"
            "      \"name\": \"TestLayer\",\n"
            "      \"origin\": \"32.0 32.0 0.0\",\n"
            "      \"size\": \"64.0 64.0\",\n"
            "      \"alpha\": {\n"
            "        \"value\": 0.5,\n"
            "        \"script\": \"var count = 0; var sp = 1.0; export function init(v) { count "
            "+= 10; return v; } export function applyUserProperties(p) { if (p.speed !== "
            "undefined) sp = p.speed.value; } export function update(v) { return count + sp; }\"\n"
            "      },\n"
            "      \"image\": \"models/quad.json\"\n"
            "    }\n"
            "  ]\n"
            "}\n";
        const char *mdl_json = "{\"material\": \"materials/quad.json\"}\n";
        const char *mat_json = "{\n"
                               "  \"passes\": [\n"
                               "    {\n"
                               "      \"shader\": \"genericimage2\",\n"
                               "      \"constantshadervalues\": {\"speed_prop\": 1.0},\n"
                               "      \"combos\": {}\n"
                               "    }\n"
                               "  ]\n"
                               "}\n";
        const char *frag_src =
            "uniform float g_Speed; // {\"material\":\"speed_prop\",\"default\":1.0}\n"
            "void main() { gl_FragColor = vec4(g_Speed); }\n";

        pkg_test_item_t items[4] = {
            {.path = "scene.json", .data = scn_json, .len = strlen(scn_json)},
            {.path = "models/quad.json", .data = mdl_json, .len = strlen(mdl_json)},
            {.path = "materials/quad.json", .data = mat_json, .len = strlen(mat_json)},
            {.path = "shaders/genericimage2.frag", .data = frag_src, .len = strlen(frag_src)},
        };
        byte_buf_t pkg_bb;
        build_synthetic_pkg(&pkg_bb, "PKGV0002", items, 4);
        char kpath[4096];
        snprintf(kpath, sizeof(kpath), "%s/scene.pkg", stmp);
        fp = fopen(kpath, "wb");
        assert(fp != NULL);
        fwrite(pkg_bb.data, 1, pkg_bb.len, fp);
        fclose(fp);
        bb_free(&pkg_bb);

        we_scene_t *scn = we_scene_load_cpu(stmp, NULL);
        assert(scn != NULL);

        /* Match by GLSL uniform name g_Speed and by material_key speed_prop */
        float new_speed = 4.5f;
        assert(we_scene_set_uniform_override(scn, "g_Speed", &new_speed, 1));
        assert(fabsf(scn->objects[0].base_pass.const_vals[0].val[0] - 4.5f) < 1e-5f);
        float mat_speed = 6.25f;
        assert(we_scene_set_uniform_override(scn, "speed_prop", &mat_speed, 1));
        assert(fabsf(scn->objects[0].base_pass.const_vals[0].val[0] - 6.25f) < 1e-5f);

        /* Update prop "speed" and verify init() was NOT re-executed (count stays 10, not 20) */
        float prop_speed = 2.0f;
        assert(we_scene_set_uniform_override(scn, "speed", &prop_speed, 1));
        we_script_env_t env = {.frametime = 0.016f, .runtime = 1.0f};
        we_script_layer_state_t lst = {.alpha = 0.5f, .visible = true};
        float in_val = 0.5f;
        float out_val[4] = {0};
        assert(
            we_script_eval_update(scn->objects[0].scripts[0].vm, &env, &lst, &in_val, 1u, out_val));
        assert(fabsf(out_val[0] - 12.0f) < 1e-4f);

        we_scene_destroy(scn);
        unlink(kpath);
        unlink(ppath);
        rmdir(stmp);
    }

    /* 5. Particle emitter "speed" fallback and turbulence "minspeed"/"maxspeed" aliases */
    {
        const char *prt_json = "{\n"
                               "  \"maxcount\": 64,\n"
                               "  \"emitter\": [\n"
                               "    {\n"
                               "      \"name\": \"sphererandom\",\n"
                               "      \"rate\": 20.0,\n"
                               "      \"speed\": \"50.0\"\n"
                               "    }\n"
                               "  ],\n"
                               "  \"operator\": [\n"
                               "    {\n"
                               "      \"name\": \"turbulence\",\n"
                               "      \"scale\": 0.01,\n"
                               "      \"minspeed\": 15.0,\n"
                               "      \"maxspeed\": 45.0\n"
                               "    }\n"
                               "  ]\n"
                               "}\n";
        we_particle_sys_t *sys = we_particle_create_from_json(prt_json, strlen(prt_json));
        assert(sys != NULL);
        assert(sys->emitter_count == 1);
        assert(fabsf(sys->emitters[0].speed_min - 50.0f) < 1e-4f);
        assert(fabsf(sys->emitters[0].speed_max - 50.0f) < 1e-4f);
        assert(sys->operator_count == 1);
        assert(sys->operators[0].type == WE_OP_TURBULENCE);
        assert(fabsf(sys->operators[0].turb_speed - 30.0f) < 1e-4f);
        we_particle_destroy(sys);
    }
}

static void test_shared_we_json_dom(void)
{
    printf("  [TEST] Testing shared we_json DOM parser and edge cases...\n");
    assert(we_json_parse_root(NULL, 0) == NULL);
    assert(we_json_parse_root("", 0) == NULL);

    const char *doc_src = "{\n"
                          "  \"name\": \"quote \\\"escaped\\\"\\nline\",\n"
                          "  \"count\": 42,\n"
                          "  \"active\": true,\n"
                          "  \"vec\": \"0.25 0.5 0.75\",\n"
                          "  \"items\": [1.5, -2.5, {\"nested\": false}]\n"
                          "}\n";
    we_jval_t *root = we_json_parse_root(doc_src, strlen(doc_src));
    assert(root != NULL && root->type == JVAL_OBJ);
    assert(strcmp(we_jval_str(we_jobj_get(root, "name"), ""), "quote \"escaped\"\nline") == 0);
    assert(fabsf(we_jval_float(we_jobj_get(root, "count"), NULL, 0.0f, NULL, 0) - 42.0f) < 1e-5f);
    assert(we_jval_bool_resolved(we_jobj_get(root, "active"), NULL, false, NULL, 0) == true);

    float v3[3] = {0.0f, 0.0f, 0.0f};
    assert(we_jval_floats(we_jobj_get(root, "vec"), NULL, v3, 3, NULL, 0) == 3);
    assert(fabsf(v3[0] - 0.25f) < 1e-5f);
    assert(fabsf(v3[1] - 0.50f) < 1e-5f);
    assert(fabsf(v3[2] - 0.75f) < 1e-5f);

    const we_jval_t *items = we_jobj_get(root, "items");
    assert(items != NULL && items->type == JVAL_ARR && items->arr.count == 3);
    const we_jval_t *elem2 = items->arr.items[2];
    assert(elem2 != NULL && elem2->type == JVAL_OBJ);
    assert(we_jval_bool_resolved(we_jobj_get(elem2, "nested"), NULL, true, NULL, 0) == false);
    we_jval_free(root);
}

int main(void)
{
    printf("=== Running Wallpaper Engine Core Test Suite ===\n");
    test_pkg_vfs();
    test_tex_loader();
    test_shader_preprocessor();
    test_scene_graph_and_cli();
    test_pkg_tex_shader_and_scene_edge_cases();
    test_puppet_warp_mdl();
    test_particle_simulator_avx2();
    test_scenescript_engine_and_watchdog();
    test_puppet_particle_script_scene_integration();
    test_script_gc_puppet_pose_and_particle_operators();
    test_mpris_audio_ducking_and_live_uniforms();
    test_mpris_arbitration_and_scene_uniform_overrides();
    test_shared_we_json_dom();
    printf("=== All Wallpaper Engine Core Tests Passed! ===\n");
    return 0;
}
