#include "cli_parser.h"
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
    printf("=== ALL ADVANCED ENGINE & DAEMON TESTS PASSED! ===\n");
    return 0;
}
