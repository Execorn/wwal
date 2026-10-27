// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cli_parser.h"
#include "gpu_compute.h"
#include "wayland_core.h"
#include "waywal/anim_engine.h"
#include "waywal/audio_fft.h"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/palette_hct.h"
#include "waywal/power_governor.h"
#include "waywal/shader_engine.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void assert_file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    assert(sz > 0 && sz < 1024 * 1024);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf != NULL);
    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);
    assert(strstr(buf, needle) != NULL);
    free(buf);
}

static void test_palette_hct_engine(void)
{
    printf("[TEST] Testing OKLab Wu + Weighted K-Means++ + CAM16/HCT Palette Engine...\n");

    const uint32_t w = 256;
    const uint32_t h = 256;
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    assert(rgba != NULL);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *px = rgba + ((size_t)y * w + x) * 4;
            if (x < 128 && y < 128) {
                px[0] = 230;
                px[1] = 60;
                px[2] = 90;
                px[3] = 255;
            } else if (x >= 128 && y < 128) {
                px[0] = 40;
                px[1] = 140;
                px[2] = 240;
                px[3] = 255;
            } else if (x < 128 && y >= 128) {
                px[0] = 50;
                px[1] = 210;
                px[2] = 130;
                px[3] = 255;
            } else {
                px[0] = (uint8_t)(x & 0xFF);
                px[1] = (uint8_t)(y & 0xFF);
                px[2] = 180;
                px[3] = 255;
            }
        }
    }

    waywal_palette_t dark_pal;
    waywal_palette_t light_pal;
    memset(&dark_pal, 0, sizeof(dark_pal));
    memset(&light_pal, 0, sizeof(light_pal));

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    bool ok_dark = waywal_palette_extract(rgba, w, h, w * 4, WAYWAL_THEME_DARK, &dark_pal);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double elapsed_us =
        (double)(ts1.tv_sec - ts0.tv_sec) * 1e6 + (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e3;

    assert(ok_dark);
    assert(dark_pal.mode == WAYWAL_THEME_DARK);
    assert(!dark_pal.is_monochrome);
    assert(dark_pal.primary.a == 255);
    assert(dark_pal.background.r < 64 && dark_pal.background.g < 64 && dark_pal.background.b < 64);

    bool ok_light = waywal_palette_extract(rgba, w, h, w * 4, WAYWAL_THEME_LIGHT, &light_pal);
    assert(ok_light);
    assert(light_pal.mode == WAYWAL_THEME_LIGHT);
    assert(light_pal.background.r > 200 && light_pal.background.g > 200 &&
           light_pal.background.b > 200);

    char tmp_dir[] = "/tmp/wwal_test_theme_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    assert(waywal_palette_export_all(&dark_pal, tmp_dir));

    char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s/caelestia/scheme.json", tmp_dir);
    assert_file_contains(path_buf, "\"tonalspot\"");
    assert_file_contains(path_buf, "\"primary\"");
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/caelestia/sequences.txt", tmp_dir);
    struct stat st;
    assert(stat(path_buf, &st) == 0 && st.st_size > 16);
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/wwal/colors.json", tmp_dir);
    assert_file_contains(path_buf, "\"mode\": \"dark\"");
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/wwal/colors.css", tmp_dir);
    assert_file_contains(path_buf, "--primary: #");
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/wwal/colors-hyprland.conf", tmp_dir);
    assert_file_contains(path_buf, "$primary = rgba(");
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/scheme.json", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/sequences.txt", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/colors.json", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/colors.css", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/colors-hyprland.conf", tmp_dir);
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/caelestia", tmp_dir);
    rmdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wwal", tmp_dir);
    rmdir(path_buf);
    rmdir(tmp_dir);
    free(rgba);

    printf("  -> HCT palette extracted in %.1f us and exported to 5 theme formats: PASSED\n",
           elapsed_us);
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} png_mem_writer_t;

static void png_mem_write_cb(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_mem_writer_t *w = (png_mem_writer_t *)png_get_io_ptr(png_ptr);
    if (w->len + length > w->cap) {
        size_t next_cap = (w->cap + length) * 2;
        uint8_t *next = (uint8_t *)realloc(w->buf, next_cap);
        assert(next != NULL);
        w->buf = next;
        w->cap = next_cap;
    }
    memcpy(w->buf + w->len, data, length);
    w->len += length;
}

static void png_mem_flush_cb(png_structp png_ptr) { (void)png_ptr; }

static void test_toml_cli_formats(void)
{
    printf("[TEST] Testing TOML Parser, swww/awww CLI Compatibility & In-Memory SVG/PNG/BMP...\n");

    const char *toml_doc = "# WayWal Declarative Configuration\n"
                           "[daemon]\n"
                           "layer = \"bottom\"\n"
                           "namespace = \"custom-ns\"\n"
                           "battery_policy = \"pause-video\"\n"
                           "auto_pause_fullscreen = true\n"
                           "auto_pause_maximized = true\n"
                           "idle_timeout_sec = 180\n"
                           "ten_bit = true\n"
                           "backdrop = \"auto-blur\"\n\n"
                           "[theme]\n"
                           "enabled = true\n"
                           "mode = \"light\"\n"
                           "caelestia_sync = true\n"
                           "tty_sequences = false\n\n"
                           "[transition]\n"
                           "type = \"grow\"\n"
                           "duration = 1.25\n"
                           "fps = 165\n"
                           "bezier = [0.22, 1.0, 0.36, 1.0]\n"
                           "angle = 45.0\n"
                           "pos_x = 0.25\n"
                           "pos_y = 0.75\n\n"
                           "[output.\"DP-1\"]\n"
                           "path = \"/tmp/dp1_wall.png\"\n"
                           "scaling = \"fit\"\n"
                           "filter = \"mitchell\"\n"
                           "fill_color = \"#1e1e2e\"\n"
                           "fps = 144\n\n"
                           "[workspace.\"1\"]\n"
                           "output = \"DP-1\"\n"
                           "path = \"/tmp/ws1.png\"\n";

    waywal_config_t cfg;
    char err_buf[256] = {0};
    bool parsed = waywal_config_parse_string(toml_doc, &cfg, err_buf, sizeof(err_buf));
    assert(parsed);
    assert(cfg.daemon.layer_pos == WAYWAL_LAYER_BOTTOM);
    assert(strcmp(cfg.daemon.namespace_str, "custom-ns") == 0);
    assert(cfg.daemon.battery_policy_mode == WAYWAL_BATTERY_PAUSE_VIDEO);
    assert(cfg.daemon.auto_pause_fullscreen == true);
    assert(cfg.daemon.auto_pause_maximized == true);
    assert(cfg.daemon.idle_timeout_sec == 180U);
    assert(cfg.daemon.ten_bit == true);
    assert(strcmp(cfg.daemon.backdrop, "auto-blur") == 0);

    assert(cfg.theme.enabled == true);
    assert(cfg.theme.theme_mode == WAYWAL_THEME_LIGHT);
    assert(cfg.theme.caelestia_sync == true);
    assert(cfg.theme.tty_sequences == false);

    assert(cfg.transition.type_enum == 4U);
    assert(fabsf(cfg.transition.duration - 1.25f) < 1e-4f);
    assert(cfg.transition.fps == 165U);
    assert(fabsf(cfg.transition.bezier[0] - 0.22f) < 1e-4f);
    assert(fabsf(cfg.transition.bezier[1] - 1.0f) < 1e-4f);
    assert(fabsf(cfg.transition.angle - 45.0f) < 1e-4f);

    assert(cfg.output_count == 1U);
    assert(strcmp(cfg.outputs[0].output_name, "DP-1") == 0);
    assert(cfg.outputs[0].scaling == WAYWAL_SCALING_FIT);
    assert(cfg.outputs[0].filter == WAYWAL_FILTER_MITCHELL);
    assert(cfg.outputs[0].fill_color == 0x1E1E2EFFU);
    assert(cfg.outputs[0].fps == 144U);

    assert(cfg.workspace_count == 1U);
    assert(strcmp(cfg.workspaces[0].workspace_name, "1") == 0);
    assert(strcmp(cfg.workspaces[0].output_name, "DP-1") == 0);
    assert(strcmp(cfg.workspaces[0].path, "/tmp/ws1.png") == 0);

    {
        char *argv[] = {"swww",
                        "img",
                        "-",
                        "--resize",
                        "crop",
                        "--fill-color",
                        "1e1e2e",
                        "--filter",
                        "Lanczos3",
                        "--transition-type",
                        "left",
                        "--transition-pos",
                        "tr",
                        "--transition-bezier",
                        "0.4,0.0,0.2,1.0"};
        cli_options_t opts;
        assert(cli_parse(15, argv, &opts));
        assert(opts.is_swww_compat);
        assert(opts.cmd == CLI_CMD_IMG);
        assert(opts.read_stdin);
        assert(opts.scaling_mode == WAYWAL_SCALING_FILL);
        assert(opts.clear_color.r == 0x1e && opts.clear_color.g == 0x1e &&
               opts.clear_color.b == 0x2e);
        assert(opts.filter == WAYWAL_FILTER_LANCZOS3);
        assert(opts.transition_type == WAYWAL_TRANSITION_SLIDE);
        assert(fabsf(opts.transition_angle - 180.0f) < 0.01f);
        assert(fabsf(opts.transition_pos_x - 1.0f) < 0.01f);
        assert(fabsf(opts.transition_pos_y - 0.0f) < 0.01f);
        assert(opts.has_custom_bezier);
    }

    {
        char *argv_shader[] = {"wwal", "shader", "/tmp/plasma.frag", "--audio", "--fps", "144"};
        cli_options_t opts;
        assert(cli_parse(6, argv_shader, &opts));
        assert(opts.cmd == CLI_CMD_SHADER);
        assert(opts.audio_reactive);
        assert(opts.transition_fps == 144);
    }

    {
        char *argv_stats[] = {"wwal", "stats"};
        cli_options_t opts;
        assert(cli_parse(2, argv_stats, &opts));
        assert(opts.cmd == CLI_CMD_STATS);
    }

    {
        char *argv_theme[] = {"wwal", "theme", "/tmp/wall.png", "--light"};
        cli_options_t opts;
        assert(cli_parse(4, argv_theme, &opts));
        assert(opts.cmd == CLI_CMD_THEME);
        assert(opts.theme_light_mode);
    }

    {
        char *argv_restore[] = {"awww", "restore"};
        cli_options_t opts;
        assert(cli_parse(2, argv_restore, &opts));
        assert(opts.is_swww_compat);
        assert(opts.cmd == CLI_CMD_RESTORE);
    }

    const char *svg_xml = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" "
                          "viewBox=\"0 0 64 64\">"
                          "<rect x=\"0\" y=\"0\" width=\"64\" height=\"64\" fill=\"#ff3366\"/>"
                          "<circle cx=\"32\" cy=\"32\" r=\"16\" fill=\"#33ccff\"/>"
                          "</svg>";
    raw_image_t svg_img;
    memset(&svg_img, 0, sizeof(svg_img));
    assert(image_load_from_memory((const uint8_t *)svg_xml, strlen(svg_xml), &svg_img));
    assert(svg_img.width == 64 && svg_img.height == 64 && svg_img.data != NULL);
    raw_image_free(&svg_img);

    png_mem_writer_t pw = {.buf = (uint8_t *)malloc(4096), .len = 0, .cap = 4096};
    assert(pw.buf != NULL);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    assert(setjmp(png_jmpbuf(png)) == 0);
    png_set_write_fn(png, &pw, png_mem_write_cb, png_mem_flush_cb);
    png_set_IHDR(png, info, 4, 4, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    uint8_t row[16];
    for (int i = 0; i < 4; ++i) {
        row[i * 4 + 0] = 0x12;
        row[i * 4 + 1] = 0x34;
        row[i * 4 + 2] = 0x56;
        row[i * 4 + 3] = 0xFF;
    }
    png_bytep rows[4] = {row, row, row, row};
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);

    raw_image_t png_img;
    memset(&png_img, 0, sizeof(png_img));
    assert(image_load_from_memory(pw.buf, pw.len, &png_img));
    assert(png_img.width == 4 && png_img.height == 4 && png_img.data != NULL);
    raw_image_free(&png_img);
    free(pw.buf);

    uint8_t bmp_buf[70];
    memset(bmp_buf, 0, sizeof(bmp_buf));
    bmp_buf[0] = 'B';
    bmp_buf[1] = 'M';
    bmp_buf[2] = sizeof(bmp_buf);
    bmp_buf[10] = 54;
    bmp_buf[14] = 40;
    bmp_buf[18] = 2;
    bmp_buf[22] = 2;
    bmp_buf[26] = 1;
    bmp_buf[28] = 32;
    for (int i = 0; i < 4; ++i) {
        bmp_buf[54 + i * 4 + 0] = 0xAA;
        bmp_buf[54 + i * 4 + 1] = 0xBB;
        bmp_buf[54 + i * 4 + 2] = 0xCC;
        bmp_buf[54 + i * 4 + 3] = 0xFF;
    }
    raw_image_t bmp_img;
    memset(&bmp_img, 0, sizeof(bmp_img));
    assert(image_load_from_memory(bmp_buf, sizeof(bmp_buf), &bmp_img));
    assert(bmp_img.width == 2 && bmp_img.height == 2 && bmp_img.data != NULL);
    raw_image_free(&bmp_img);

    printf("  -> TOML parser, swww/awww CLI flags & SVG/PNG/BMP memory loaders: PASSED\n");
}

static void test_state_and_kawase_blur(void)
{
    printf("[TEST] Testing Per-Output Persistent State Store & Dual-Kawase Blur...\n");

    char tmp_dir[] = "/tmp/wwal_test_state_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    waywal_state_store_t store;
    waywal_state_store_init(&store);

    waywal_output_saved_state_t wildcard = {0};
    strncpy(wildcard.output_name, "*", sizeof(wildcard.output_name) - 1);
    wildcard.kind = WAYWAL_CONTENT_SOLID_COLOR;
    wildcard.solid_color = 0x1E1E2EFFU;
    assert(waywal_state_store_update_output(&store, &wildcard));

    waywal_output_saved_state_t dp1 = {0};
    strncpy(dp1.output_name, "DP-1", sizeof(dp1.output_name) - 1);
    dp1.kind = WAYWAL_CONTENT_STATIC_IMAGE;
    strncpy(dp1.media_path, "/tmp/wallpaper_dp1.png", sizeof(dp1.media_path) - 1);
    dp1.scaling = WAYWAL_SCALING_FIT;
    dp1.ten_bit = true;
    assert(waywal_state_store_update_output(&store, &dp1));

    assert(waywal_state_store_save(&store, tmp_dir));

    waywal_state_store_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    assert(waywal_state_store_load(&loaded, tmp_dir));
    assert(loaded.count == 2U);

    const waywal_output_saved_state_t *found_dp1 = waywal_state_store_find_output(&loaded, "DP-1");
    assert(found_dp1 != NULL);
    assert(found_dp1->kind == WAYWAL_CONTENT_STATIC_IMAGE);
    assert(strcmp(found_dp1->media_path, "/tmp/wallpaper_dp1.png") == 0);
    assert(found_dp1->scaling == WAYWAL_SCALING_FIT);
    assert(found_dp1->ten_bit == true);

    const waywal_output_saved_state_t *found_hdmi =
        waywal_state_store_find_output(&loaded, "HDMI-A-1");
    assert(found_hdmi != NULL);
    assert(found_hdmi->kind == WAYWAL_CONTENT_SOLID_COLOR);
    assert(found_hdmi->solid_color == 0x1E1E2EFFU);

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/state.bin", tmp_dir);
    unlink(bin_path);
    rmdir(tmp_dir);

    const uint32_t bw = 64, bh = 64;
    uint8_t *src = (uint8_t *)calloc((size_t)bw * bh * 4, 1);
    uint8_t *dst = (uint8_t *)calloc((size_t)bw * bh * 4, 1);
    assert(src != NULL && dst != NULL);

    for (uint32_t y = 24; y < 40; ++y) {
        for (uint32_t x = 24; x < 40; ++x) {
            uint8_t *p = src + ((size_t)y * bw + x) * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = 255;
        }
    }

    layer_shell_blur_rgba_cpu(src, dst, bw, bh);
    uint8_t *outside_edge = dst + ((size_t)22 * bw + 32) * 4;
    uint8_t *center_px = dst + ((size_t)32 * bw + 32) * 4;
    assert(center_px[0] > 200);
    assert(outside_edge[0] > 0 && outside_edge[0] < 255);

    free(src);
    free(dst);
    printf("  -> Persistent state store & Dual-Kawase blur: PASSED\n");
}

static void test_anim_engine(void)
{
    printf("[TEST] Testing Native Zero-Dependency Animated GIF89a Delta-Frame Engine...\n");

    static const uint8_t anim_gif89a[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04,
        0x01, 0x05, 0x00, 0x03, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
        0x00, 0x08, 0x07, 0x00, 0x01, 0x04, 0x10, 0x30, 0x20, 0x20, 0x00, 0x21, 0xF9, 0x04,
        0x01, 0x08, 0x00, 0x03, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
        0x00, 0x08, 0x07, 0x00, 0x01, 0x04, 0x10, 0x30, 0x20, 0x20, 0x00, 0x3B,
    };

    assert(waywal_anim_probe(anim_gif89a, sizeof(anim_gif89a)));
    assert(waywal_anim_detect_format(anim_gif89a, sizeof(anim_gif89a)) == WAYWAL_ANIM_FORMAT_GIF);

    waywal_anim_decoder_t *dec = waywal_anim_create(anim_gif89a, sizeof(anim_gif89a));
    assert(dec != NULL);
    assert(waywal_anim_frame_count(dec) == 2U);
    assert(waywal_anim_get_format(dec) == WAYWAL_ANIM_FORMAT_GIF);

    const uint8_t *rgba = NULL;
    uint32_t fw = 0, fh = 0, delay_ms = 0;
    assert(waywal_anim_next_frame(dec, &rgba, &fw, &fh, &delay_ms));
    assert(rgba != NULL && fw == 2U && fh == 2U && delay_ms == 50U);

    assert(waywal_anim_next_frame(dec, &rgba, &fw, &fh, &delay_ms));
    assert(rgba != NULL && fw == 2U && fh == 2U && delay_ms == 80U);

    waywal_anim_destroy(dec);
    printf("  -> Multi-frame GIF89a probe & LZW delta-frame decode: PASSED\n");
}

static void test_power_governor(void)
{
    printf("[TEST] Testing Zero-Watt Smart Occlusion & Laptop Battery Governor...\n");

    waywal_power_governor_t *gov = waywal_power_governor_create(WAYWAL_BATTERY_REDUCE_FPS);
    assert(gov != NULL);
    waywal_power_governor_set_on_battery(gov, false);

    assert(!waywal_power_governor_should_pause_output(gov, "DP-1"));
    assert(waywal_power_governor_effective_fps(gov, "DP-1", 60) == 60U);

    waywal_power_governor_set_output_reason(gov, "DP-1", WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, true);
    assert(waywal_power_governor_should_pause_output(gov, "DP-1"));
    assert(!waywal_power_governor_should_pause_output(gov, "HDMI-A-1"));
    assert((waywal_power_governor_get_output_mask(gov, "DP-1") &
            WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN) != 0);
    assert(waywal_power_governor_effective_fps(gov, "DP-1", 60) == 0U);

    waywal_power_governor_set_output_reason(gov, "DP-1", WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, false);
    assert(!waywal_power_governor_should_pause_output(gov, "DP-1"));

    waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_SESSION_IDLE, true);
    assert(waywal_power_governor_should_pause_all(gov));
    assert(waywal_power_governor_should_pause_output(gov, "DP-1"));
    assert(waywal_power_governor_should_pause_output(gov, "HDMI-A-1"));
    waywal_power_governor_set_global_reason(gov, WAYWAL_PAUSE_SESSION_IDLE, false);

    waywal_power_governor_set_on_battery(gov, true);
    assert(waywal_power_governor_is_on_battery(gov));
    assert(waywal_power_governor_effective_fps(gov, "DP-1", 60) == 24U);

    waywal_power_governor_set_battery_policy(gov, WAYWAL_BATTERY_PAUSE_VIDEO);
    assert(waywal_power_governor_should_pause_all(gov));
    assert((waywal_power_governor_get_output_mask(gov, "DP-1") & WAYWAL_PAUSE_BATTERY_SAVER) != 0);
    assert(waywal_power_governor_effective_fps(gov, "DP-1", 60) == 0U);

    waywal_power_governor_set_on_battery(gov, false);
    assert(!waywal_power_governor_should_pause_all(gov));
    assert(waywal_power_governor_effective_fps(gov, "DP-1", 60) == 60U);

    /* Verify hotplug output removal and slot reuse */
    waywal_power_governor_set_output_reason(gov, "DP-1", WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN, true);
    waywal_power_governor_set_output_reason(gov, "HDMI-A-1", WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN,
                                            false);
    assert(!waywal_power_governor_should_pause_all(gov));
    waywal_power_governor_remove_output(gov, "HDMI-A-1");
    assert(waywal_power_governor_should_pause_all(gov));
    waywal_power_governor_remove_output(gov, "DP-1");
    assert(!waywal_power_governor_should_pause_all(gov));

    waywal_power_governor_destroy(gov);
    printf("  -> Per-output/global pause bitmasks & battery FPS governor: PASSED\n");
}

static void test_audio_fft_and_shader(void)
{
    printf("[TEST] Testing 64-Bin Bark Audio FFT & Shadertoy Compute Auto-Wrapper...\n");

    waywal_audio_fft_t *fft = waywal_audio_fft_create(48000, false);
    assert(fft != NULL);

    float samples[2048];
    for (size_t i = 0; i < 2048; ++i) {
        float t = (float)i / 48000.0f;
        samples[i] = 0.7f * sinf(2.0f * 3.14159265f * 100.0f * t) +
                     0.5f * sinf(2.0f * 3.14159265f * 440.0f * t);
    }

    assert(waywal_audio_fft_push_samples(fft, samples, 2048) == 2048U);

    waywal_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    assert(waywal_audio_fft_process(fft, &frame));

    float max_band = 0.0f;
    for (size_t b = 0; b < WAYWAL_AUDIO_BARK_BANDS; ++b) {
        if (frame.bands[b] > max_band) {
            max_band = frame.bands[b];
        }
    }
    assert(max_band > 0.01f);
    assert(frame.bass_energy > 0.01f);
    assert(frame.envelope > 0.01f);

    uint32_t non_zero_tex = 0;
    for (size_t i = 0; i < WAYWAL_AUDIO_TEX_WIDTH * WAYWAL_AUDIO_TEX_HEIGHT; ++i) {
        if (frame.shadertoy_tex[i] > 0) {
            non_zero_tex++;
        }
    }
    assert(non_zero_tex > 64U);

    /* Verify zero-push idle frame exponentially decays without re-running DFT */
    waywal_audio_frame_t idle_frame;
    memset(&idle_frame, 0, sizeof(idle_frame));
    assert(waywal_audio_fft_process(fft, &idle_frame));
    assert(idle_frame.envelope < frame.envelope && idle_frame.envelope > 0.0f);
    waywal_audio_fft_destroy(fft);

    const char *shadertoy_frag = "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
                                 "    vec2 uv = fragCoord / iResolution.xy;\n"
                                 "    float fft = texture(iChannel0, vec2(uv.x, 0.25)).r;\n"
                                 "    fragColor = vec4(uv, fft + u_audio_bands[0], 1.0);\n"
                                 "}\n";

    char *wrapped = waywal_shader_wrap_source(shadertoy_frag);
    assert(wrapped != NULL);
    assert(strstr(wrapped, "#version 310 es") != NULL);
    assert(strstr(wrapped, "u_audio_bands[64]") != NULL);
    assert(strstr(wrapped, "iChannel0") != NULL);
    assert(strstr(wrapped, "void main()") != NULL);
    free(wrapped);

    /* Verify shader wrapper handles hundreds of #pragma waywal lines without overflow */
    char many_lines[32768];
    size_t off = 0;
    for (int i = 0; i < 600 && off + 48 < sizeof(many_lines); ++i) {
        off += (size_t)snprintf(many_lines + off, sizeof(many_lines) - off,
                                "#pragma waywal channel0 = audio // %d\n", i + 1);
    }
    snprintf(many_lines + off, sizeof(many_lines) - off,
             "void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }\n");
    char *wrapped_many = waywal_shader_wrap_source(many_lines);
    assert(wrapped_many != NULL);
    assert(strstr(wrapped_many, "//pragma waywal channel0 = audio // 600") != NULL);
    free(wrapped_many);

    printf("  -> 2048-pt AVX2 Bark FFT & Shadertoy GLES 3.1 compute transpiler: PASSED\n");
}

static void test_extended_subsystems(void)
{
    printf("[TEST] Testing BGRA Palette, Template Engine, Full TOML Schema & Workspace IPC...\n");

    /* 1. Verify waywal_palette_extract_bgra matches waywal_palette_extract on swapped R/B */
    uint8_t rgba_img[32 * 32 * 4];
    uint8_t bgra_img[32 * 32 * 4];
    for (size_t i = 0; i < 32 * 32; ++i) {
        uint8_t r = (i < 512) ? 220 : 30;
        uint8_t g = (i < 512) ? 60 : 140;
        uint8_t b = (i < 512) ? 40 : 210;
        rgba_img[i * 4 + 0] = r;
        rgba_img[i * 4 + 1] = g;
        rgba_img[i * 4 + 2] = b;
        rgba_img[i * 4 + 3] = 255;
        bgra_img[i * 4 + 0] = b;
        bgra_img[i * 4 + 1] = g;
        bgra_img[i * 4 + 2] = r;
        bgra_img[i * 4 + 3] = 255;
    }
    waywal_palette_t pal_rgba, pal_bgra;
    assert(waywal_palette_extract(rgba_img, 32, 32, 32 * 4, WAYWAL_THEME_DARK, &pal_rgba));
    assert(waywal_palette_extract_bgra(bgra_img, 32, 32, 32 * 4, WAYWAL_THEME_DARK, &pal_bgra));
    assert(pal_rgba.primary.r == pal_bgra.primary.r);
    assert(pal_rgba.primary.g == pal_bgra.primary.g);
    assert(pal_rgba.primary.b == pal_bgra.primary.b);

    /* 2. Verify all 15 man-page TOML keys & 64-bit integer overflow guard */
    const char *full_toml = "[daemon]\n"
                            "restore_on_start = false\n"
                            "pause_on_occluded_workspace = true\n"
                            "pause_on_idle = true\n"
                            "idle_timeout_sec = 180\n"
                            "[theme]\n"
                            "scheme_variant = \"vibrant\"\n"
                            "contrast = 0.5\n"
                            "export_css = true\n"
                            "export_hyprland = true\n"
                            "[transition]\n"
                            "pos = \"top-right\"\n"
                            "sync_mode = \"staggered\"\n"
                            "stagger_delay_ms = 200\n"
                            "[hooks]\n"
                            "on_theme_change = \"pkill -USR2 waybar\"\n"
                            "on_wallpaper_change = \"notify-send wwal\"\n"
                            "[output.\"DP-2\"]\n"
                            "path = \"/tmp/shader.frag\"\n"
                            "mode = \"shader\"\n"
                            "interval = 120\n"
                            "shuffle = true\n"
                            "audio = true\n"
                            "[workspace.\"3\"]\n"
                            "path = \"/tmp/ws3.png\"\n"
                            "transition_type = \"wipe\"\n";

    waywal_config_t cfg;
    char err[256] = {0};
    assert(waywal_config_parse_string(full_toml, &cfg, err, sizeof(err)));
    assert(cfg.daemon.restore_on_start == false);
    assert(cfg.daemon.pause_on_occluded_workspace == true);
    assert(cfg.daemon.pause_on_idle == true);
    assert(cfg.daemon.idle_timeout_sec == 180U);
    assert(strcmp(cfg.theme.scheme_variant, "vibrant") == 0);
    assert(fabsf(cfg.theme.contrast - 0.5f) < 1e-4f);
    assert(cfg.theme.export_css == true);
    assert(cfg.theme.export_hyprland == true);
    assert(strcmp(cfg.transition.pos, "top-right") == 0);
    assert(strcmp(cfg.transition.sync_mode, "staggered") == 0);
    assert(cfg.transition.stagger_delay_ms == 200U);
    assert(strcmp(cfg.hooks.on_theme_change, "pkill -USR2 waybar") == 0);
    assert(strcmp(cfg.hooks.on_wallpaper_change, "notify-send wwal") == 0);
    assert(cfg.output_count == 1U);
    assert(strcmp(cfg.outputs[0].mode, "shader") == 0);
    assert(cfg.outputs[0].interval == 120U);
    assert(cfg.outputs[0].shuffle == true);
    assert(cfg.outputs[0].audio == true);
    assert(cfg.workspace_count == 1U);
    assert(strcmp(cfg.workspaces[0].transition_type, "wipe") == 0);

    /* Verify values exceeding UINT32_MAX are rejected by the TOML parser */
    waywal_config_t bad_cfg;
    assert(!waywal_config_parse_string("[transition]\nfps = 4294967356\n", &bad_cfg, err,
                                       sizeof(err)));

    /* 3. Verify Power Governor workspace change tracking */
    waywal_power_governor_t *gov = waywal_power_governor_create(WAYWAL_BATTERY_IGNORE);
    assert(gov != NULL);
    waywal_power_governor_feed_hyprland_event(gov, "focusedmon>>DP-1,2");
    waywal_power_governor_feed_hyprland_event(gov, "workspace>>3");
    char ws[64] = {0}, mon[64] = {0};
    assert(waywal_power_governor_pop_workspace_change(gov, ws, sizeof(ws), mon, sizeof(mon)));
    assert(strcmp(ws, "3") == 0);
    assert(!waywal_power_governor_pop_workspace_change(gov, ws, sizeof(ws), mon, sizeof(mon)));
    waywal_power_governor_destroy(gov);

    printf("  -> Extended subsystem verifications: PASSED\n");
}

static void test_phase1_phase2_subsystems(void)
{
    printf("[TEST] Testing CAM16/HCT Solver, 9 Material You Variants, Multi-Pass Shaders & Power "
           "Governor Policies...\n");

    /* 1. Exact CAM16 + CIE L* forward & inverse HCT solver roundtrip */
    color_rgba_t black = waywal_hct_to_rgba(120.0f, 50.0f, 0.0f);
    assert(black.r == 0 && black.g == 0 && black.b == 0 && black.a == 255);

    color_rgba_t white = waywal_hct_to_rgba(240.0f, 50.0f, 100.0f);
    assert(white.r == 255 && white.g == 255 && white.b == 255 && white.a == 255);

    color_rgba_t grey = waywal_hct_to_rgba(45.0f, 0.0f, 50.0f);
    assert(grey.r == grey.g && grey.g == grey.b && grey.r > 100 && grey.r < 140);

    const color_rgba_t test_colors[] = {
        {.r = 220, .g = 50, .b = 70, .a = 255},
        {.r = 40, .g = 180, .b = 90, .a = 255},
        {.r = 50, .g = 110, .b = 230, .a = 255},
        {.r = 240, .g = 180, .b = 40, .a = 255},
    };
    for (size_t i = 0; i < sizeof(test_colors) / sizeof(test_colors[0]); ++i) {
        float h = 0.0f, c = 0.0f, t = 0.0f;
        waywal_rgba_to_hct(test_colors[i], &h, &c, &t);
        assert(h >= 0.0f && h < 360.0f);
        assert(c > 15.0f);
        assert(t > 10.0f && t < 95.0f);

        color_rgba_t rt = waywal_hct_to_rgba(h, c, t);
        float h_rt = 0.0f, c_rt = 0.0f, t_rt = 0.0f;
        waywal_rgba_to_hct(rt, &h_rt, &c_rt, &t_rt);
        assert(fabsf(t - t_rt) < 1.5f);
        assert(abs((int)test_colors[i].r - (int)rt.r) <= 3);
        assert(abs((int)test_colors[i].g - (int)rt.g) <= 3);
        assert(abs((int)test_colors[i].b - (int)rt.b) <= 3);
    }

    /* 2. All 9 Material You scheme variants & contrast levels */
    uint8_t vibrant_img[16 * 16 * 4];
    for (size_t i = 0; i < 16 * 16; ++i) {
        vibrant_img[i * 4 + 0] = 230;
        vibrant_img[i * 4 + 1] = 60;
        vibrant_img[i * 4 + 2] = 90;
        vibrant_img[i * 4 + 3] = 255;
    }

    const char *variants[] = {"tonalspot", "vibrant", "expressive", "neutral",   "monochrome",
                              "fidelity",  "content", "rainbow",    "fruitsalad"};
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        waywal_palette_t pal;
        waywal_scheme_variant_t v_enum = waywal_scheme_variant_from_str(variants[i]);
        assert(waywal_palette_extract_ex(vibrant_img, 16, 16, 16 * 4, false, WAYWAL_THEME_DARK,
                                         v_enum, 0.0f, &pal));
        assert(pal.variant == v_enum);
        if (v_enum == WAYWAL_SCHEME_MONOCHROME) {
            assert(pal.primary.r == pal.primary.g && pal.primary.g == pal.primary.b);
        }
    }

    waywal_palette_t pal_std, pal_hc;
    assert(waywal_palette_extract_ex(vibrant_img, 16, 16, 16 * 4, false, WAYWAL_THEME_DARK,
                                     WAYWAL_SCHEME_TONAL_SPOT, 0.0f, &pal_std));
    assert(waywal_palette_extract_ex(vibrant_img, 16, 16, 16 * 4, false, WAYWAL_THEME_DARK,
                                     WAYWAL_SCHEME_TONAL_SPOT, 1.0f, &pal_hc));
    float h_std = 0.0f, c_std = 0.0f, t_std = 0.0f;
    float h_hc = 0.0f, c_hc = 0.0f, t_hc = 0.0f;
    waywal_rgba_to_hct(pal_std.primary, &h_std, &c_std, &t_std);
    waywal_rgba_to_hct(pal_hc.primary, &h_hc, &c_hc, &t_hc);
    assert(t_hc > t_std);

    /* 3. Multi-pass Shadertoy pass extraction & extended uniforms */
    const char *multipass_src =
        "// #pragma waywal pass BUFFER_A\n"
        "// iChannel0: bufferA\n"
        "// iChannel1: audio\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    fragColor = texture(iChannel0, fragCoord / iResolution.xy);\n"
        "}\n"
        "// #pragma waywal pass IMAGE\n"
        "// iChannel0: bufferA\n"
        "// iChannel1: noise\n"
        "// iChannel2: wallpaper\n"
        "// iChannel3: image(\"/tmp/custom_lut.png\")\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    vec2 uv = (fragCoord + u_mouse_smooth.xy + u_workspace_offset) / iResolution.xy;\n"
        "    fragColor = texture(iChannel0, uv) + vec4(u_palette_primary, 1.0);\n"
        "}\n";

    char *wrapped_buf_a = waywal_shader_wrap_pass_source(multipass_src, 0);
    assert(wrapped_buf_a != NULL);
    assert(strstr(wrapped_buf_a, "#version 310 es") != NULL);
    assert(strstr(wrapped_buf_a, "texture(iChannel0, uv)") == NULL); /* Only in IMAGE pass body */
    free(wrapped_buf_a);

    /* Pass 1 (BUFFER_B) is not defined in multipass_src -> returns NULL */
    assert(waywal_shader_wrap_pass_source(multipass_src, 1) == NULL);

    char *wrapped_img = waywal_shader_wrap_pass_source(multipass_src, 4);
    assert(wrapped_img != NULL);
    assert(strstr(wrapped_img, "uniform highp sampler2D iChannel3;") != NULL);
    assert(strstr(wrapped_img, "uniform vec3 iChannelResolution[4];") != NULL);
    assert(strstr(wrapped_img, "uniform float iChannelTime[4];") != NULL);
    assert(strstr(wrapped_img, "uniform float iSampleRate;") != NULL);
    assert(strstr(wrapped_img, "uniform vec3 u_palette_primary;") != NULL);
    assert(strstr(wrapped_img, "uniform vec4 u_mouse_smooth;") != NULL);
    assert(strstr(wrapped_img, "uniform vec2 u_mouse_vel;") != NULL);
    assert(strstr(wrapped_img, "uniform vec2 u_workspace_offset;") != NULL);
    free(wrapped_img);

    /* 4. Power governor wildcard fullscreen clearing & policy gating */
    waywal_power_governor_t *gov = waywal_power_governor_create(WAYWAL_BATTERY_IGNORE);
    assert(gov != NULL);
    /* Fullscreen event before any focusedmon event uses wildcard output "*" and must clear cleanly
     */
    waywal_power_governor_feed_hyprland_event(gov, "fullscreen>>1");
    assert(waywal_power_governor_should_pause_output(gov, "DP-1"));
    waywal_power_governor_feed_hyprland_event(gov, "fullscreen>>0");
    assert(!waywal_power_governor_should_pause_output(gov, "DP-1"));

    /* Policy gating: disabling auto_pause_fullscreen prevents fullscreen from pausing */
    waywal_power_governor_set_pause_policies(gov, false, false, false);
    waywal_power_governor_feed_hyprland_event(gov, "focusedmon>>DP-1,1");
    waywal_power_governor_feed_hyprland_event(gov, "fullscreen>>1");
    assert(!waywal_power_governor_should_pause_output(gov, "DP-1"));

    /* Niri JSON event parsing */
    waywal_power_governor_set_pause_policies(gov, true, true, true);
    waywal_power_governor_feed_niri_event(gov,
                                          "{\"WorkspaceFocused\":{\"id\":7,\"focused\":true}}\n");
    char niri_ws[64] = {0}, niri_mon[64] = {0};
    assert(waywal_power_governor_pop_workspace_change(gov, niri_ws, sizeof(niri_ws), niri_mon,
                                                      sizeof(niri_mon)));
    assert(strcmp(niri_ws, "7") == 0);
    waywal_power_governor_destroy(gov);

    /* 4b. Power governor Unix domain socket EOF + reconnect + failed reconnect */
    {
        const char *sock_path = "/tmp/wwal_pgov_ipc_test.sock";
        unlink(sock_path);
        int srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        assert(srv_fd >= 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        assert(bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(srv_fd, 4) == 0);

        char old_hypr[256] = {0}, old_niri[256] = {0};
        const char *eh = getenv("HYPRLAND_INSTANCE_SIGNATURE");
        const char *en = getenv("NIRI_SOCKET");
        if (eh)
            strncpy(old_hypr, eh, sizeof(old_hypr) - 1);
        if (en)
            strncpy(old_niri, en, sizeof(old_niri) - 1);
        unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
        setenv("NIRI_SOCKET", sock_path, 1);

        waywal_power_governor_t *ipc_gov = waywal_power_governor_create(WAYWAL_BATTERY_IGNORE);
        assert(ipc_gov != NULL);
        assert(waywal_power_governor_get_ipc_fd(ipc_gov) >= 0);

        int conn1 = accept4(srv_fd, NULL, NULL, SOCK_CLOEXEC);
        assert(conn1 >= 0);
        const char *msg1 = "{\"WorkspaceFocused\":{\"id\":3,\"focused\":true}}\n";
        assert(write(conn1, msg1, strlen(msg1)) == (ssize_t)strlen(msg1));
        waywal_power_governor_dispatch_ipc(ipc_gov);
        assert(waywal_power_governor_pop_workspace_change(ipc_gov, niri_ws, sizeof(niri_ws),
                                                          niri_mon, sizeof(niri_mon)));
        assert(strcmp(niri_ws, "3") == 0);

        /* Close server peer conn1 -> triggers EOF and automatic reconnect to srv_fd */
        close(conn1);
        waywal_power_governor_dispatch_ipc(ipc_gov);
        assert(waywal_power_governor_consume_ipc_reconnected(ipc_gov));
        assert(!waywal_power_governor_consume_ipc_reconnected(ipc_gov));
        assert(waywal_power_governor_get_ipc_fd(ipc_gov) >= 0);

        int conn2 = accept4(srv_fd, NULL, NULL, SOCK_CLOEXEC);
        assert(conn2 >= 0);
        const char *msg2 = "{\"WorkspaceFocused\":{\"id\":9,\"focused\":true}}\n";
        assert(write(conn2, msg2, strlen(msg2)) == (ssize_t)strlen(msg2));
        waywal_power_governor_dispatch_ipc(ipc_gov);
        assert(waywal_power_governor_pop_workspace_change(ipc_gov, niri_ws, sizeof(niri_ws),
                                                          niri_mon, sizeof(niri_mon)));
        assert(strcmp(niri_ws, "9") == 0);

        /* Close listening socket and peer conn2 -> reconnect must fail cleanly with ipc_fd == -1
         * and ipc_reconnected == false */
        close(srv_fd);
        unlink(sock_path);
        close(conn2);
        waywal_power_governor_dispatch_ipc(ipc_gov);
        assert(!waywal_power_governor_consume_ipc_reconnected(ipc_gov));
        assert(waywal_power_governor_get_ipc_fd(ipc_gov) == -1);

        waywal_power_governor_destroy(ipc_gov);
        if (old_hypr[0])
            setenv("HYPRLAND_INSTANCE_SIGNATURE", old_hypr, 1);
        if (old_niri[0])
            setenv("NIRI_SOCKET", old_niri, 1);
        else
            unsetenv("NIRI_SOCKET");
    }

    /* 5. CLI --depth-map, --depth-strength, --backdrop, --layer, --battery-policy & simple/fade */
    {
        char *argv_depth[] = {"wwal",
                              "img",
                              "/tmp/wall.png",
                              "--depth-map",
                              "/tmp/wall_depth.png",
                              "--depth-strength",
                              "2.5",
                              "--backdrop",
                              "auto-blur",
                              "--layer",
                              "bottom",
                              "--battery-policy",
                              "reduce-fps",
                              "--transition-type",
                              "simple"};
        cli_options_t opts;
        assert(cli_parse(15, argv_depth, &opts));
        assert(strcmp(opts.depth_map_path, "/tmp/wall_depth.png") == 0);
        assert(fabsf(opts.depth_strength - 2.5f) < 1e-4f);
        assert(strcmp(opts.backdrop, "auto-blur") == 0);
        assert(strcmp(opts.layer_name, "bottom") == 0);
        assert(strcmp(opts.battery_policy, "reduce-fps") == 0);
        assert(opts.transition_type == WAYWAL_TRANSITION_SIMPLE);

        char *argv_fade[] = {"wwal", "img", "/tmp/wall.png", "--transition-type", "fade"};
        assert(cli_parse(5, argv_fade, &opts));
        assert(opts.transition_type == WAYWAL_TRANSITION_FADE);

        char *argv_span_img[] = {"wwal", "img", "/tmp/wall.png", "--scaling-mode", "span"};
        assert(cli_parse(5, argv_span_img, &opts));
        assert(opts.scaling_mode == WAYWAL_SCALING_SPAN);

        char *argv_span_sh[] = {"wwal", "shader", "/tmp/s.frag", "--scaling-mode", "span"};
        assert(cli_parse(5, argv_span_sh, &opts));
        assert(opts.cmd == CLI_CMD_SHADER);
        assert(opts.scaling_mode == WAYWAL_SCALING_SPAN);

        waywal_config_t span_cfg;
        char span_err[128] = {0};
        assert(waywal_config_parse_string("[output.\"DP-1\"]\npath = \"/tmp/a.png\"\nscaling = "
                                          "\"span\"\n",
                                          &span_cfg, span_err, sizeof(span_err)));
        assert(span_cfg.output_count == 1U);
        assert(span_cfg.outputs[0].scaling == WAYWAL_SCALING_SPAN);
    }

    /* 6. Headless GPU Shader Engine: multi-pass, relative image(), depth-parallax, 10-bit FBO blit,
     *    GL_FRAGMENT_SHADER derivatives (dFdx/dFdy/fwidth) + mipmaps, span_rect, and multi-engine
     *    concurrency */
    {
        dmabuf_context_t dctx;
        memset(&dctx, 0, sizeof(dctx));
        if (dmabuf_context_init(&dctx) && dctx.available) {
            waywal_shader_engine_t *seng = waywal_shader_engine_create(&dctx);
            if (seng) {
                /* Verify // iChannelN: directive target resolution inside multi-pass sections */
                assert(waywal_shader_engine_load_source(seng, multipass_src,
                                                        "/tmp/wwal_mp_Syntax.frag"));
                assert(waywal_shader_engine_get_channel_target(seng, 0, 0) == 1); /* bufferA */
                assert(waywal_shader_engine_get_channel_target(seng, 0, 1) == 0); /* audio */
                assert(waywal_shader_engine_get_channel_target(seng, 4, 0) == 1); /* bufferA */
                assert(waywal_shader_engine_get_channel_target(seng, 4, 1) == 5); /* noise */
                assert(waywal_shader_engine_get_channel_target(seng, 4, 2) == 6); /* wallpaper */
                assert(waywal_shader_engine_get_channel_target(seng, 4, 3) == 8); /* image(...) */

                /* Write a 1x1 uncompressed 32-bit BMP in /tmp for relative image() resolution */
                const char *bmp_path = "/tmp/wwal_rel_lut_test.bmp";
                uint8_t bmp[58] = {'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0,   0,  0,  40,
                                   0,   0,   0,  1, 0, 0, 0, 1, 0, 0, 0,  1,   0,  32, 0,
                                   0,   0,   0,  0, 4, 0, 0, 0, 0, 0, 0,  0,   0,  0,  0,
                                   0,   0,   0,  0, 0, 0, 0, 0, 0, 0, 0,  255, 255};
                FILE *bfp = fopen(bmp_path, "wb");
                assert(bfp != NULL);
                assert(fwrite(bmp, 1, sizeof(bmp), bfp) == sizeof(bmp));
                fclose(bfp);

                uint8_t wall_rgba[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                                         0,   0, 255, 255, 255, 255, 255, 255};
                waywal_shader_engine_set_wallpaper(seng, wall_rgba, 2, 2);

                const char *gpu_mp_src =
                    "// #pragma waywal pass BUFFER_A\n"
                    "// #pragma waywal bufferA.channel0 = wallpaper\n"
                    "// #pragma waywal bufferA.channel1 = image(\"wwal_rel_lut_test.bmp\")\n"
                    "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
                    "    vec2 uv = fragCoord / iResolution.xy;\n"
                    "    fragColor = 0.5 * (texture(iChannel0, uv) + texture(iChannel1, uv));\n"
                    "}\n"
                    "// #pragma waywal pass IMAGE\n"
                    "// #pragma waywal image.channel0 = bufferA\n"
                    "// #pragma waywal image.channel1 = noise\n"
                    "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
                    "    vec2 uv = fragCoord / iResolution.xy;\n"
                    "    fragColor = texture(iChannel0, uv) + 0.01 * texture(iChannel1, uv);\n"
                    "}\n";

                assert(waywal_shader_engine_load_source(seng, gpu_mp_src,
                                                        "/tmp/wwal_rel_shader.frag"));
                assert(waywal_shader_engine_get_channel_target(seng, 0, 0) == 6);
                assert(waywal_shader_engine_get_channel_target(seng, 0, 1) == 8);
                assert(waywal_shader_engine_get_channel_target(seng, 4, 0) == 1);
                assert(waywal_shader_engine_get_channel_target(seng, 4, 1) == 5);

                dmabuf_bo_t bo8;
                memset(&bo8, 0, sizeof(bo8));
                bool has_bo8 = dmabuf_bo_allocate(&dctx, &bo8, 64, 64, DRM_FORMAT_XRGB8888,
                                                  DRM_FORMAT_MOD_LINEAR);
                if (has_bo8) {
                    waywal_shader_frame_params_t fp;
                    memset(&fp, 0, sizeof(fp));
                    fp.time_sec = 0.5f;
                    fp.time_delta_sec = 0.016f;
                    fp.frame_index = 1;
                    assert(waywal_shader_engine_render_frame(seng, &bo8, &fp));
                    bo8.is_10bit = true;
                    assert(waywal_shader_engine_render_frame(seng, &bo8, &fp));
                    bo8.is_10bit = false;

                    waywal_shader_engine_set_depth_strength(seng, 2.5f);
                    assert(waywal_shader_engine_load_depth_parallax(seng, wall_rgba, 2, 2,
                                                                    wall_rgba, 2, 2));
                    assert(waywal_shader_engine_render_frame(seng, &bo8, &fp));

                    /* Test GL_FRAGMENT_SHADER hardware derivatives (dFdx/dFdy/fwidth) + mipmap
                     * LOD bias + span_rect coordinate offset */
                    const char *deriv_shader =
                        "// #pragma waywal image.channel0 = noise\n"
                        "// #pragma waywal image.channel1 = wallpaper\n"
                        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
                        "    vec2 uv = fragCoord / iResolution.xy;\n"
                        "    float dx = dFdx(fragCoord.x);\n"
                        "    float dy = abs(dFdy(fragCoord.y));\n"
                        "    float fw = fwidth(uv.x);\n"
                        "    vec4 mip = textureLod(iChannel0, uv, 1.0) + texture(iChannel1, uv, "
                        "0.5);\n"
                        "    fragColor = vec4(clamp(dx * dy, 0.0, 1.0), uv.x, uv.y, 1.0) + 0.001 * "
                        "(mip + fw);\n"
                        "}\n";
                    assert(waywal_shader_engine_load_source(seng, deriv_shader,
                                                            "/tmp/wwal_deriv_shader.frag"));
                    fp.span_rect[0] = 64.0f;
                    fp.span_rect[1] = 0.0f;
                    fp.span_rect[2] = 128.0f;
                    fp.span_rect[3] = 64.0f;
                    assert(waywal_shader_engine_render_frame(seng, &bo8, &fp));
                    if (bo8.map_data) {
                        const uint8_t *px = (const uint8_t *)bo8.map_data;
                        /* Format is BGRA in little-endian XRGB8888: B=uv.y, G=uv.x, R=dx*dy */
                        assert(px[2] > 200); /* dx * dy ~= 1.0 -> R > 200 */
                        assert(px[1] > 120); /* span_rect.x=64 / 128 -> uv.x >= 0.5 -> G > 120 */
                    }

                    /* Test per-output multi-engine concurrency sharing dmabuf_context_t */
                    waywal_shader_engine_t *seng2 = waywal_shader_engine_create(&dctx);
                    assert(seng2 != NULL);
                    const char *red_sh =
                        "void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0, 0.0, 0.0, 1.0); }\n";
                    const char *green_sh =
                        "void mainImage(out vec4 c, in vec2 f) { c = vec4(0.0, 1.0, 0.0, 1.0); }\n";
                    assert(waywal_shader_engine_load_source(seng, red_sh, "/tmp/wwal_red.frag"));
                    assert(
                        waywal_shader_engine_load_source(seng2, green_sh, "/tmp/wwal_green.frag"));

                    dmabuf_bo_t bo_second;
                    memset(&bo_second, 0, sizeof(bo_second));
                    if (dmabuf_bo_allocate(&dctx, &bo_second, 64, 64, DRM_FORMAT_XRGB8888,
                                           DRM_FORMAT_MOD_LINEAR)) {
                        memset(&fp, 0, sizeof(fp));
                        fp.time_sec = 0.25f;
                        fp.time_delta_sec = 0.016f;
                        for (uint32_t iter = 0; iter < 3; ++iter) {
                            fp.frame_index = iter;
                            assert(waywal_shader_engine_render_frame(seng, &bo8, &fp));
                            assert(waywal_shader_engine_render_frame(seng2, &bo_second, &fp));
                        }
                        if (bo8.map_data && bo_second.map_data) {
                            const uint8_t *p1 = (const uint8_t *)bo8.map_data;
                            const uint8_t *p2 = (const uint8_t *)bo_second.map_data;
                            /* bo8 is pure red (B=0, G=0, R=255), bo_second is pure green (B=0,
                             * G=255, R=0) */
                            assert(p1[2] > 240 && p1[1] < 16 && p1[0] < 16);
                            assert(p2[1] > 240 && p2[2] < 16 && p2[0] < 16);
                        }
                        waywal_shader_engine_forget_bo(seng2, &bo_second);
                        dmabuf_bo_free(&bo_second);
                    }
                    assert(!waywal_shader_engine_uses_mouse(seng));
                    assert(!waywal_shader_engine_uses_mouse(seng2));
                    const char *mouse_sh = "void mainImage(out vec4 c, in vec2 f) { c = "
                                           "vec4(iMouse.xy, 0.0, 1.0); }\n";
                    assert(
                        waywal_shader_engine_load_source(seng2, mouse_sh, "/tmp/wwal_mouse.frag"));
                    assert(waywal_shader_engine_uses_mouse(seng2));
                    assert(waywal_shader_engine_load_depth_parallax_bgra(seng2, wall_rgba, 2, 2,
                                                                         wall_rgba, 2, 2));
                    assert(waywal_shader_engine_uses_mouse(seng2));
                    waywal_shader_engine_destroy(seng2);
                }

                dmabuf_bo_t bo10;
                memset(&bo10, 0, sizeof(bo10));
                bool has_bo10 = dmabuf_bo_allocate(&dctx, &bo10, 64, 64, DRM_FORMAT_ARGB2101010,
                                                   DRM_FORMAT_MOD_INVALID);
                if (has_bo10) {
                    bo10.is_10bit = true;
                    waywal_shader_frame_params_t fp;
                    memset(&fp, 0, sizeof(fp));
                    fp.time_sec = 1.0f;
                    fp.time_delta_sec = 0.016f;
                    fp.frame_index = 2;
                    assert(waywal_shader_engine_render_frame(seng, &bo10, &fp));
                    waywal_shader_engine_forget_bo(seng, &bo10);
                }

                if (has_bo8)
                    waywal_shader_engine_forget_bo(seng, &bo8);
                unlink(bmp_path);
                waywal_shader_engine_destroy(seng);
                if (has_bo10)
                    dmabuf_bo_free(&bo10);
                if (has_bo8)
                    dmabuf_bo_free(&bo8);
            }
            dmabuf_context_destroy(&dctx);
        }
    }

    printf(
        "  -> CAM16/HCT solver, 9 scheme variants, multi-pass shaders & power governor: PASSED\n");
}

int main(void)
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_WARN);
    printf("=================================================================\n");
    printf("  Executing WayWal Core Subsystems Test Suite\n");
    printf("=================================================================\n");

    test_palette_hct_engine();
    test_toml_cli_formats();
    test_state_and_kawase_blur();
    test_anim_engine();
    test_power_governor();
    test_audio_fft_and_shader();
    test_extended_subsystems();
    test_phase1_phase2_subsystems();

    printf("\n>>> ALL CORE SUBSYSTEMS UNIT TESTS PASSED! <<<\n\n");
    return 0;
}
