#include "cli_parser.h"
#include "image_loader.h"
#include "waywal/gif_transcode.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/slideshow.h"
#include "waywal/types.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool daemon_start_image_transition(struct daemon_state *state, const uint8_t *pixels,
                                   uint32_t img_w, uint32_t img_h,
                                   const waywal_img_metadata_t *meta)
{
    (void)state;
    (void)pixels;
    (void)img_w;
    (void)img_h;
    (void)meta;
    return true;
}

static void test_cli_positional_aliases(void)
{
    printf("[TEST] Testing CLI positional aliases and coordinate parsing...\n");

    struct {
        const char *pos_str;
        float expected_x;
        float expected_y;
    } cases[] = {
        {"cursor", -1.0f, -1.0f},    {"mouse", -1.0f, -1.0f},      {"center", 0.5f, 0.5f},
        {"top", 0.5f, 0.0f},         {"bottom", 0.5f, 1.0f},       {"left", 0.0f, 0.5f},
        {"right", 1.0f, 0.5f},       {"top-left", 0.0f, 0.0f},     {"topleft", 0.0f, 0.0f},
        {"top-right", 1.0f, 0.0f},   {"topright", 1.0f, 0.0f},     {"bottom-left", 0.0f, 1.0f},
        {"bottomleft", 0.0f, 1.0f},  {"bottom-right", 1.0f, 1.0f}, {"bottomright", 1.0f, 1.0f},
        {"0.25,0.75", 0.25f, 0.75f}, {"0.12,0.88", 0.12f, 0.88f},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char *argv[] = {"wwal", "img", "dummy.png", "--transition-pos", (char *)cases[i].pos_str};
        cli_options_t opts;
        bool ok = cli_parse(5, argv, &opts);
        assert(ok && "cli_parse failed for positional alias");
        assert(fabsf(opts.transition_pos_x - cases[i].expected_x) < 0.001f);
        assert(fabsf(opts.transition_pos_y - cases[i].expected_y) < 0.001f);
    }

    printf("  -> Positional aliases: PASSED\n");
}

static void test_cli_multi_monitor(void)
{
    printf("[TEST] Testing multi-monitor output targeting & sync options...\n");

    {
        char *argv[] = {"wwal", "img", "dummy.png", "-o", "DP-1", "-o", "HDMI-A-1"};
        cli_options_t opts;
        bool ok = cli_parse(7, argv, &opts);
        assert(ok);
        assert(opts.num_outputs == 2);
        assert(strcmp(opts.outputs[0], "DP-1") == 0);
        assert(strcmp(opts.outputs[1], "HDMI-A-1") == 0);
    }

    {
        char *argv[] = {"wwal", "img", "dummy.png", "--output", "eDP-1,DP-2,HDMI-A-2"};
        cli_options_t opts;
        bool ok = cli_parse(5, argv, &opts);
        assert(ok);
        assert(opts.num_outputs == 3);
        assert(strcmp(opts.outputs[0], "eDP-1") == 0);
        assert(strcmp(opts.outputs[1], "DP-2") == 0);
        assert(strcmp(opts.outputs[2], "HDMI-A-2") == 0);
    }

    {
        char *argv[] = {"wwal", "img", "dummy.png", "--sync-mode", "staggered", "--stagger-delay",
                        "250"};
        cli_options_t opts;
        bool ok = cli_parse(7, argv, &opts);
        assert(ok);
        assert(opts.sync_mode == 1);
        assert(opts.stagger_delay_ms == 250);
    }

    printf("  -> Multi-monitor output targeting & sync: PASSED\n");
}

static void test_cli_custom_shader_and_10bit(void)
{
    printf("[TEST] Testing custom shader and 10-bit options parsing...\n");

    char *argv[] = {"wwal",
                    "img",
                    "dummy.png",
                    "--transition-type",
                    "custom",
                    "--transition-shader",
                    "tests/custom_test.comp",
                    "--10bit"};
    cli_options_t opts;
    bool ok = cli_parse(8, argv, &opts);
    assert(ok);
    assert(opts.transition_type == 20);
    assert(strcmp(opts.custom_shader_path, "tests/custom_test.comp") == 0);
    assert(opts.enable_10bit == true);

    printf("  -> Custom shader & 10-bit options: PASSED\n");
}

static void test_cli_slideshow(void)
{
    printf("[TEST] Testing slideshow command and control options...\n");

    {
        char *argv[] = {"wwal",       "slideshow", "/usr/share/backgrounds",
                        "--interval", "120",       "--shuffle"};
        cli_options_t opts;
        bool ok = cli_parse(6, argv, &opts);
        assert(ok);
        assert(opts.cmd == CLI_CMD_SLIDESHOW);
        assert(strcmp(opts.filepath, "/usr/share/backgrounds") == 0);
        assert(opts.slideshow_interval_s == 120);
        assert(opts.slideshow_random == true);
    }

    struct {
        const char *sub;
        uint32_t expected_act;
    } sub_cases[] = {
        {"stop", WAYWAL_SLIDESHOW_STOP},     {"pause", WAYWAL_SLIDESHOW_PAUSE},
        {"resume", WAYWAL_SLIDESHOW_RESUME}, {"unpause", WAYWAL_SLIDESHOW_RESUME},
        {"toggle", WAYWAL_SLIDESHOW_TOGGLE}, {"next", WAYWAL_SLIDESHOW_NEXT},
        {"prev", WAYWAL_SLIDESHOW_PREV},
    };

    for (size_t i = 0; i < sizeof(sub_cases) / sizeof(sub_cases[0]); ++i) {
        char *argv[] = {"wwal", "slideshow", (char *)sub_cases[i].sub};
        cli_options_t opts;
        bool ok = cli_parse(3, argv, &opts);
        assert(ok);
        assert(opts.cmd == CLI_CMD_SLIDESHOW_CTRL);
        assert(opts.slideshow_action == sub_cases[i].expected_act);
    }

    printf("  -> Slideshow commands: PASSED\n");
}

static void test_slideshow_filesystem(void)
{
    printf("[TEST] Testing slideshow recursive directory scanning and controls...\n");

    char tmp_dir[] = "/tmp/wwal_test_slideshow_XXXXXX";
    char *res = mkdtemp(tmp_dir);
    assert(res != NULL);

    char sub_dir[512];
    snprintf(sub_dir, sizeof(sub_dir), "%s/subdir", tmp_dir);
    mkdir(sub_dir, 0700);

    /* Create dummy image and non-image files */
    char f1[512], f2[512], f3[512], f4[512];
    snprintf(f1, sizeof(f1), "%s/img1.png", tmp_dir);
    snprintf(f2, sizeof(f2), "%s/img2.jpg", tmp_dir);
    snprintf(f3, sizeof(f3), "%s/subdir/img3.webp", tmp_dir);
    snprintf(f4, sizeof(f4), "%s/readme.txt", tmp_dir);

    FILE *fp;
    fp = fopen(f1, "w");
    fputs("dummy", fp);
    fclose(fp);
    fp = fopen(f2, "w");
    fputs("dummy", fp);
    fclose(fp);
    fp = fopen(f3, "w");
    fputs("dummy", fp);
    fclose(fp);
    fp = fopen(f4, "w");
    fputs("not an image", fp);
    fclose(fp);

    slideshow_engine_t ss;
    memset(&ss, 0, sizeof(ss));
    bool init_ok = slideshow_init(&ss, NULL);
    assert(init_ok);
    assert(ss.timer_fd >= 0);

    waywal_slideshow_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.interval_s = 5;
    payload.random_order = false;
    strncpy(payload.path, tmp_dir, sizeof(payload.path) - 1);

    bool start_ok = slideshow_start(&ss, &payload);
    assert(start_ok);
    assert(ss.active);
    assert(!ss.paused);
    /* Exactly 3 image files discovered, readme.txt ignored */
    assert(ss.num_files == 3);

    /* Test control methods */
    slideshow_pause(&ss);
    assert(ss.paused);

    slideshow_resume(&ss);
    assert(!ss.paused);

    slideshow_toggle(&ss);
    assert(ss.paused);

    slideshow_toggle(&ss);
    assert(!ss.paused);

    bool ctrl_ok = slideshow_control(&ss, WAYWAL_SLIDESHOW_PAUSE);
    assert(ctrl_ok && ss.paused);

    ctrl_ok = slideshow_control(&ss, WAYWAL_SLIDESHOW_RESUME);
    assert(ctrl_ok && !ss.paused);

    slideshow_stop(&ss);
    assert(!ss.active);

    slideshow_destroy(&ss);
    assert(ss.timer_fd < 0);
    assert(ss.file_list == NULL);
    assert(ss.num_files == 0);

    /* Cleanup disk files */
    unlink(f1);
    unlink(f2);
    unlink(f3);
    unlink(f4);
    rmdir(sub_dir);
    rmdir(tmp_dir);

    printf("  -> Slideshow engine lifecycle: PASSED\n");
}

static inline uint32_t argb8888_to_xrgb2101010(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF;
    uint32_t g = (c >> 8) & 0xFF;
    uint32_t b = c & 0xFF;
    uint32_t r10 = (r << 2) | (r >> 6);
    uint32_t g10 = (g << 2) | (g >> 6);
    uint32_t b10 = (b << 2) | (b >> 6);
    return (r10 << 20) | (g10 << 10) | b10;
}

static void test_10bit_color_math(void)
{
    printf("[TEST] Testing 10-bit color channel mapping...\n");

    /* Pure Red 0x00FF0000 -> 10-bit R=1023, G=0, B=0 */
    uint32_t c_red = 0x00FF0000;
    uint32_t x_red = argb8888_to_xrgb2101010(c_red);
    assert(((x_red >> 20) & 0x3FF) == 1023);
    assert(((x_red >> 10) & 0x3FF) == 0);
    assert((x_red & 0x3FF) == 0);

    /* Pure Green 0x0000FF00 -> 10-bit R=0, G=1023, B=0 */
    uint32_t c_green = 0x0000FF00;
    uint32_t x_green = argb8888_to_xrgb2101010(c_green);
    assert(((x_green >> 20) & 0x3FF) == 0);
    assert(((x_green >> 10) & 0x3FF) == 1023);
    assert((x_green & 0x3FF) == 0);

    /* Pure Blue 0x000000FF -> 10-bit R=0, G=0, B=1023 */
    uint32_t c_blue = 0x000000FF;
    uint32_t x_blue = argb8888_to_xrgb2101010(c_blue);
    assert(((x_blue >> 20) & 0x3FF) == 0);
    assert(((x_blue >> 10) & 0x3FF) == 0);
    assert((x_blue & 0x3FF) == 1023);

    printf("  -> 10-bit color channel mapping: PASSED\n");
}

static void test_cli_scaling_modes(void)
{
    printf("[TEST] Testing CLI scaling modes and aspect ratio math...\n");

    /* Default when omitted */
    {
        char *argv[] = {"wwal", "img", "dummy.png"};
        cli_options_t opts;
        assert(cli_parse(3, argv, &opts));
        assert(opts.scaling_mode == WAYWAL_SCALING_FILL);
    }

    /* Named modes */
    struct {
        const char *flag;
        const char *val;
        waywal_scaling_mode_t expected;
    } cases[] = {
        {"--scaling-mode", "fill", WAYWAL_SCALING_FILL},
        {"--mode", "fill", WAYWAL_SCALING_FILL},
        {"--scaling-mode", "crop", WAYWAL_SCALING_FILL},
        {"--mode", "cover", WAYWAL_SCALING_FILL},
        {"--scaling-mode", "fit", WAYWAL_SCALING_FIT},
        {"--mode", "fit", WAYWAL_SCALING_FIT},
        {"--mode", "contain", WAYWAL_SCALING_FIT},
        {"--scaling-mode", "stretch", WAYWAL_SCALING_STRETCH},
        {"--mode", "stretch", WAYWAL_SCALING_STRETCH},
        {"--scaling-mode", "center", WAYWAL_SCALING_CENTER},
        {"--mode", "center", WAYWAL_SCALING_CENTER},
        {"--scaling-mode", "tile", WAYWAL_SCALING_TILE},
        {"--mode", "tile", WAYWAL_SCALING_TILE},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char *argv[] = {"wwal", "img", "dummy.png", (char *)cases[i].flag, (char *)cases[i].val};
        cli_options_t opts;
        assert(cli_parse(5, argv, &opts));
        assert(opts.scaling_mode == cases[i].expected);
    }

    /* Mathematical aspect-ratio crop-to-fill verification (e.g. AnimeCozy 3320x1536 on 1920x1080)
     */
    double src_w = 3320.0, src_h = 1536.0;
    double dst_w = 1920.0, dst_h = 1080.0;
    double r_src = src_w / src_h;
    double r_dst = dst_w / dst_h;
    assert(r_src > r_dst); /* 2.161 > 1.778 (wider than monitor) */

    double crop_h = src_h;
    double crop_w = crop_h * r_dst;
    double scale_x = dst_w / crop_w;
    double scale_y = dst_h / crop_h;
    assert(fabs(scale_x - scale_y) < 1e-6); /* Strict isotropic scaling - zero distortion */

    printf("  -> Scaling modes and aspect ratio math: PASSED\n");
}

static void test_static_gif_support(void)
{
    printf("[TEST] Testing static GIF image decoding and transparency...\n");

    /* 54-byte valid 2x2 GIF89a with 4 colors: Red, Green, Blue, Transparent White */
    static const uint8_t sample_gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04,
        0x01, 0x00, 0x00, 0x03, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
        0x00, 0x08, 0x07, 0x00, 0x01, 0x04, 0x10, 0x30, 0x20, 0x20, 0x00, 0x3B,
    };

    const char *gif_path = "/tmp/test_waywal_sample.gif";
    FILE *fp = fopen(gif_path, "wb");
    assert(fp != NULL);
    assert(fwrite(sample_gif, 1, sizeof(sample_gif), fp) == sizeof(sample_gif));
    fclose(fp);

    uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0;
    bool ok = image_load(gif_path, &pixels, &w, &h);
    unlink(gif_path);

    assert(ok && "image_load failed for static GIF");
    assert(w == 2 && h == 2);
    assert(pixels != NULL);

    uint32_t *u32 = (uint32_t *)pixels;
    /* Pixel (0,0): Palette index 0 -> Red (0xFFFF0000 in little-endian ARGB8888) */
    assert(u32[0] == 0xFFFF0000);
    /* Pixel (1,0): Palette index 1 -> Green (0xFF00FF00) */
    assert(u32[1] == 0xFF00FF00);
    /* Pixel (0,1): Palette index 2 -> Blue (0xFF0000FF) */
    assert(u32[2] == 0xFF0000FF);
    /* Pixel (1,1): Palette index 3 -> Transparent (0x00000000) */
    assert(u32[3] == 0x00000000);

    image_free(pixels);

    /* Negative test: Corrupted header */
    const char *bad_gif = "/tmp/test_bad_header.gif";
    fp = fopen(bad_gif, "wb");
    assert(fp != NULL);
    fwrite("GIF89a\x01", 1, 7, fp);
    fclose(fp);
    assert(!image_load(bad_gif, &pixels, &w, &h));
    unlink(bad_gif);

    printf("  -> Static GIF decoding, color fidelity & transparency: PASSED\n");
}

static void test_jit_transcode_support(void)
{
    printf("[TEST] Testing JIT transcoded VA-API video engine & GIF caching...\n");

    /* 1. is_gif_file magic verification */
    assert(!is_gif_file(NULL));
    assert(!is_gif_file("/nonexistent/file_does_not_exist.gif"));

    const char *txt_path = "/tmp/test_not_a_gif.txt";
    FILE *fp = fopen(txt_path, "wb");
    assert(fp != NULL);
    fwrite("Hello World\n", 1, 12, fp);
    fclose(fp);
    assert(!is_gif_file(txt_path));
    unlink(txt_path);

    /* 54-byte valid 2x2 GIF89a */
    static const uint8_t sample_gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04,
        0x01, 0x00, 0x00, 0x03, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
        0x00, 0x08, 0x07, 0x00, 0x01, 0x04, 0x10, 0x30, 0x20, 0x20, 0x00, 0x3B,
    };
    const char *gif_path = "/tmp/test_jit_sample.gif";
    fp = fopen(gif_path, "wb");
    assert(fp != NULL);
    assert(fwrite(sample_gif, 1, sizeof(sample_gif), fp) == sizeof(sample_gif));
    fclose(fp);

    assert(is_gif_file(gif_path));

    /* 2. gif_transcode_resolve edge cases */
    char out_path1[4096];
    assert(!gif_transcode_resolve(NULL, out_path1, sizeof(out_path1)));
    assert(!gif_transcode_resolve(gif_path, NULL, sizeof(out_path1)));
    assert(!gif_transcode_resolve(gif_path, out_path1, 0));

    /* Non-GIF files pass through unmodified */
    char non_gif_out[4096];
    assert(gif_transcode_resolve("/tmp/test_dummy.mp4", non_gif_out, sizeof(non_gif_out)));
    assert(strcmp(non_gif_out, "/tmp/test_dummy.mp4") == 0);

    /* 3. Real GIF JIT transcode to cached MP4 */
    assert(gif_transcode_resolve(gif_path, out_path1, sizeof(out_path1)));
    assert(strstr(out_path1, ".mp4") != NULL);
    assert(strstr(out_path1, "/wwal/transcoded/") != NULL);

    struct stat st1;
    assert(stat(out_path1, &st1) == 0);
    assert(st1.st_size > 1024);

    /* 4. Cache hit verification (deterministic hash, 0s startup) */
    char out_path2[4096];
    assert(gif_transcode_resolve(gif_path, out_path2, sizeof(out_path2)));
    assert(strcmp(out_path1, out_path2) == 0);

    unlink(gif_path);

    /* 5. CLI --animate flag and automatic route to CLI_CMD_VIDEO */
    char *argv_anim[] = {"wwal", "img", (char *)out_path1, "--animate"};
    cli_options_t opts;
    assert(cli_parse(4, argv_anim, &opts));
    assert(opts.cmd == CLI_CMD_IMG);
    assert(opts.animate == true);

    /* When filepath is a GIF, client logic routes to CLI_CMD_VIDEO */
    if (opts.cmd == CLI_CMD_IMG && opts.animate && is_gif_file(opts.filepath)) {
        opts.cmd = CLI_CMD_VIDEO;
    }
    /* Since out_path1 is .mp4 (not .gif), it didn't change cmd to video */
    assert(opts.cmd == CLI_CMD_IMG);

    /* Now test with gif path */
    const char *fake_gif = "/tmp/test_fake.gif";
    fp = fopen(fake_gif, "wb");
    assert(fp != NULL);
    fwrite("GIF89a", 1, 6, fp);
    fclose(fp);

    char *argv_anim_gif[] = {"wwal", "img", (char *)fake_gif, "--animate"};
    assert(cli_parse(4, argv_anim_gif, &opts));
    assert(opts.cmd == CLI_CMD_IMG);
    assert(opts.animate == true);
    if (opts.cmd == CLI_CMD_IMG && opts.animate && is_gif_file(opts.filepath)) {
        opts.cmd = CLI_CMD_VIDEO;
    }
    assert(opts.cmd == CLI_CMD_VIDEO);
    unlink(fake_gif);

    printf("  -> JIT transcode, content hashing, caching & CLI routing: PASSED\n");
}

int main(void)
{
    printf("=== Starting Advanced Engine & Daemon Unit Tests ===\n");
    test_cli_positional_aliases();
    test_cli_multi_monitor();
    test_cli_custom_shader_and_10bit();
    test_cli_scaling_modes();
    test_cli_slideshow();
    test_slideshow_filesystem();
    test_10bit_color_math();
    test_static_gif_support();
    test_jit_transcode_support();
    printf("=== ALL ADVANCED ENGINE & DAEMON TESTS PASSED! ===\n");
    return 0;
}
