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
#include "waywal/path.h"
#include "waywal/power_governor.h"
#include "waywal/shader_engine.h"
#include "waywal/solar_schedule.h"
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

    snprintf(path_buf, sizeof(path_buf), "%s/wal/colors.json", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wal/colors", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wal/sequences", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wal/colors.sh", tmp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wal/colors.css", tmp_dir);
    unlink(path_buf);

    snprintf(path_buf, sizeof(path_buf), "%s/wal", tmp_dir);
    rmdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/caelestia", tmp_dir);
    rmdir(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/wwal", tmp_dir);
    rmdir(path_buf);
    assert(rmdir(tmp_dir) == 0);
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

static void test_cam16_shaders_and_governor(void)
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

static void test_swww_awww_compat(void)
{
    printf("[TEST] Testing swww/awww v0.12.1 CLI/Env Parity, --crop-gravity, --invert-y & "
           "cli_auto_dispatch_img...\n");

    /* 1. Crop gravity parser & config.toml integration */
    float gx = 0.0f, gy = 0.0f;
    assert(waywal_parse_crop_gravity("NE", &gx, &gy) && fabsf(gx - 1.0f) < 1e-5f &&
           fabsf(gy - 0.0f) < 1e-5f);
    assert(waywal_parse_crop_gravity("south-west", &gx, &gy) && fabsf(gx - 0.0f) < 1e-5f &&
           fabsf(gy - 1.0f) < 1e-5f);
    assert(waywal_parse_crop_gravity("center", &gx, &gy) && fabsf(gx - 0.5f) < 1e-5f &&
           fabsf(gy - 0.5f) < 1e-5f);
    assert(waywal_parse_crop_gravity("25%, 75%", &gx, &gy) && fabsf(gx - 0.25f) < 1e-5f &&
           fabsf(gy - 0.75f) < 1e-5f);
    assert(waywal_parse_crop_gravity("0.1, 0.9", &gx, &gy) && fabsf(gx - 0.1f) < 1e-5f &&
           fabsf(gy - 0.9f) < 1e-5f);
    assert(!waywal_parse_crop_gravity("invalid_gravity", &gx, &gy));
    assert(!waywal_parse_crop_gravity("nan, 0.5", &gx, &gy));
    assert(!waywal_parse_crop_gravity("0.5, inf", &gx, &gy));

    waywal_config_t cg_cfg;
    char cg_err[128] = {0};
    assert(waywal_config_parse_string("[transition]\ncrop_gravity = \"NW\"\n"
                                      "[output.\"DP-1\"]\npath = \"/tmp/a.png\"\n"
                                      "crop_gravity = \"75%, 25%\"\n",
                                      &cg_cfg, cg_err, sizeof(cg_err)));
    assert(cg_cfg.transition.has_crop_gravity);
    assert(fabsf(cg_cfg.transition.crop_gravity_x - 0.0f) < 1e-5f);
    assert(fabsf(cg_cfg.transition.crop_gravity_y - 0.0f) < 1e-5f);
    assert(cg_cfg.output_count == 1U && cg_cfg.outputs[0].has_crop_gravity);
    assert(fabsf(cg_cfg.outputs[0].crop_gravity_x - 0.75f) < 1e-5f);
    assert(fabsf(cg_cfg.outputs[0].crop_gravity_y - 0.25f) < 1e-5f);

    /* 2. AWWW_* / SWWW_* environment variable defaults & CLI override */
    setenv("AWWW_NAMESPACE", "env_ns", 1);
    setenv("AWWW_TRANSITION", "outer", 1);
    setenv("AWWW_TRANSITION_DURATION", "1.5", 1);
    setenv("AWWW_TRANSITION_FPS", "144", 1);
    setenv("AWWW_TRANSITION_POS", "20%, 80%", 1);
    setenv("AWWW_INVERT_Y", "1", 1);

    cli_options_t opts;
    char *argv_env[] = {"wwal", "img", "/tmp/wall.png"};
    assert(cli_parse(3, argv_env, &opts));
    assert(strcmp(opts.namespace_str, "env_ns") == 0);
    assert(opts.transition_type == WAYWAL_TRANSITION_OUTER);
    assert(opts.transition_duration_ms == 1500U);
    assert(opts.transition_fps == 144U);
    assert(opts.invert_y);
    assert(fabsf(opts.transition_pos_x - 0.20f) < 1e-4f);
    /* 80% inverted by AWWW_INVERT_Y=1 -> 1.0 - 0.80 = 0.20 */
    assert(fabsf(opts.transition_pos_y - 0.20f) < 1e-4f);

    unsetenv("AWWW_NAMESPACE");
    unsetenv("AWWW_TRANSITION");
    unsetenv("AWWW_TRANSITION_DURATION");
    unsetenv("AWWW_TRANSITION_FPS");
    unsetenv("AWWW_TRANSITION_POS");
    unsetenv("AWWW_INVERT_Y");

    /* 3. Pixel coordinates > 1.0f in --transition-pos + --crop-gravity + --no-cache + -a */
    char *argv_px[] = {"wwal",
                       "img",
                       "/tmp/wall.png",
                       "-a",
                       "--no-cache",
                       "--crop-gravity",
                       "SE",
                       "--transition-pos",
                       "1200, 600",
                       "--invert-y"};
    assert(cli_parse(10, argv_px, &opts));
    assert(opts.all_namespaces);
    assert(opts.no_cache);
    assert(opts.has_crop_gravity);
    assert(fabsf(opts.crop_gravity_x - 1.0f) < 1e-5f);
    assert(fabsf(opts.crop_gravity_y - 1.0f) < 1e-5f);
    assert(fabsf(opts.transition_pos_x - 1200.0f) < 1e-3f);
    assert(fabsf(opts.transition_pos_y - 600.0f) < 1e-3f);
    assert(opts.invert_y);

    /* 4. Solid hex color passed to wwal img (0xRRGGBB and #RRGGBB) */
    char *argv_hex1[] = {"wwal", "img", "0x1e1e2e"};
    assert(cli_parse(3, argv_hex1, &opts));
    assert(opts.cmd == CLI_CMD_IMG && opts.is_solid_color);
    assert(opts.clear_color.r == 0x1e && opts.clear_color.g == 0x1e && opts.clear_color.b == 0x2e);

    char *argv_hex2[] = {"wwal", "img", "#2e3440"};
    assert(cli_parse(3, argv_hex2, &opts));
    assert(opts.cmd == CLI_CMD_IMG && opts.is_solid_color);
    assert(opts.clear_color.r == 0x2e && opts.clear_color.g == 0x34 && opts.clear_color.b == 0x40);

    /* 5. Universal wwal img auto-dispatch for video, shader, and symlinks with non-media names */
    char *argv_vid[] = {"wwal", "img", "/tmp/clip.webm"};
    assert(cli_parse(3, argv_vid, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_VIDEO);

    char *argv_sh[] = {"wwal", "img", "/tmp/effect.frag"};
    assert(cli_parse(3, argv_sh, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_SHADER);

    const char *sym_target = "/tmp/wwal_unit_sym_target.frag";
    const char *sym_link = "/tmp/wwal_unit_sym.current_wallpaper";
    FILE *stf = fopen(sym_target, "w");
    assert(stf != NULL);
    fputs("void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }\n", stf);
    fclose(stf);
    unlink(sym_link);
    assert(symlink(sym_target, sym_link) == 0);
    char *argv_sym[] = {"wwal", "img", (char *)sym_link};
    assert(cli_parse(3, argv_sym, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_SHADER);
    unlink(sym_link);
    unlink(sym_target);

    /* 6. SWWW_TRANSITION_INVERT_Y env var & --transition-duration vs --transition-step precedence
     */
    setenv("SWWW_TRANSITION_INVERT_Y", "1", 1);
    setenv("SWWW_TRANSITION_POS", "0.25,0.75", 1);
    char *argv_swww_inv[] = {"swww", "img", "/tmp/wall.png"};
    assert(cli_parse(3, argv_swww_inv, &opts));
    assert(opts.invert_y);
    assert(fabsf(opts.transition_pos_y - 0.25f) < 1e-4f);
    unsetenv("SWWW_TRANSITION_INVERT_Y");
    unsetenv("SWWW_TRANSITION_POS");

    char *argv_dur_step[] = {"swww", "img",  "/tmp/wall.png",     "--transition-duration",
                             "2.0",  "--transition-step", "90"};
    assert(cli_parse(7, argv_dur_step, &opts));
    assert(opts.has_transition_duration);
    assert(opts.transition_duration_ms == 2000U);

    char *argv_step_only[] = {"swww", "img", "/tmp/wall.png", "--transition-step", "90"};
    assert(cli_parse(5, argv_step_only, &opts));
    assert(!opts.has_transition_duration);
    assert(opts.transition_step == 90U);
    assert(opts.transition_duration_ms == 90U * 16U);

    /* 7. init / daemon subcommands and -d / --no-daemon flags */
    char *argv_init[] = {"swww", "init"};
    assert(cli_parse(2, argv_init, &opts));
    assert(opts.cmd == CLI_CMD_DAEMON_ALIAS && opts.daemonize && !opts.no_daemon);

    char *argv_init_fg[] = {"swww", "init", "--no-daemon"};
    assert(cli_parse(3, argv_init_fg, &opts));
    assert(opts.cmd == CLI_CMD_DAEMON_ALIAS && !opts.daemonize && opts.no_daemon);

    char *argv_swwwd_fg[] = {"swww-daemon"};
    assert(cli_parse(1, argv_swwwd_fg, &opts));
    assert(opts.cmd == CLI_CMD_DAEMON_ALIAS && !opts.daemonize);

    char *argv_swwwd_bg[] = {"swww-daemon", "-d"};
    assert(cli_parse(2, argv_swwwd_bg, &opts));
    assert(opts.cmd == CLI_CMD_DAEMON_ALIAS && opts.daemonize);

    /* 8. Trailing flags on pause, kill, slideshow next, and clear-cache */
    char *argv_pause[] = {"wwal", "pause", "-n", "custom_ns", "-a"};
    assert(cli_parse(5, argv_pause, &opts));
    assert(opts.cmd == CLI_CMD_PAUSE && strcmp(opts.namespace_str, "custom_ns") == 0 &&
           opts.all_namespaces);

    char *argv_snext[] = {"wwal", "slideshow", "next", "-n", "ss_ns"};
    assert(cli_parse(5, argv_snext, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW_CTRL && opts.slideshow_action == WAYWAL_SLIDESHOW_NEXT &&
           strcmp(opts.namespace_str, "ss_ns") == 0);

    char *argv_cc[] = {"wwal", "clear-cache"};
    assert(cli_parse(2, argv_cc, &opts));
    assert(opts.cmd == CLI_CMD_CLEAR_CACHE);

    /* 9. Solid-color transition vs instant clear, mixed --invert-y,
     *    regular-file symlink preservation, TOML invert_y ordering, per-namespace state_store,
     *    and WAYLAND_DISPLAY slash sanitization */
    char *argv_hex_anim[] = {"wwal", "img",  "0x1e1e2e",              "--transition-type",
                             "fade", "--transition-duration", "0.5"};
    assert(cli_parse(7, argv_hex_anim, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_IMG && opts.is_solid_color);

    char *argv_hex_none[] = {"wwal", "img", "0x1e1e2e", "--transition-type", "none"};
    assert(cli_parse(5, argv_hex_none, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_CLEAR && opts.is_solid_color);

    char *argv_mixed_inv[] = {"wwal", "img", "/tmp/wall.png", "--transition-pos",
                              "500, 0.8", "--invert-y"};
    assert(cli_parse(6, argv_mixed_inv, &opts));
    assert(fabsf(opts.transition_pos_x - 500.0f) < 1e-3f);
    assert(fabsf(opts.transition_pos_y - 0.2f) < 1e-4f);
    assert(opts.pos_y_inverted);

    const char *img_sym_target = "/tmp/wwal_unit_img_target.png";
    const char *img_sym_link = "/tmp/wwal_unit_img_sym.png";
    FILE *istf = fopen(img_sym_target, "wb");
    assert(istf != NULL);
    fputs("PNG", istf);
    fclose(istf);
    unlink(img_sym_link);
    assert(symlink(img_sym_target, img_sym_link) == 0);
    char *argv_img_sym[] = {"wwal", "img", (char *)img_sym_link};
    assert(cli_parse(3, argv_img_sym, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_IMG);
    unlink(img_sym_link);
    unlink(img_sym_target);

    waywal_config_t inv_cfg;
    char inv_err[128] = {0};
    assert(waywal_config_parse_string("[transition]\npos = \"0.25, 0.75\"\ninvert_y = true\n"
                                      "[output.\"DP-1\"]\npath = \"/tmp/a.png\"\n"
                                      "invert_y = true\n",
                                      &inv_cfg, inv_err, sizeof(inv_err)));
    assert(inv_cfg.transition.invert_y && inv_cfg.transition.pos_y_inverted);
    assert(fabsf(inv_cfg.transition.pos_x - 0.25f) < 1e-4f);
    assert(fabsf(inv_cfg.transition.pos_y - 0.25f) < 1e-4f);
    assert(inv_cfg.output_count == 1U && inv_cfg.outputs[0].has_invert_y &&
           inv_cfg.outputs[0].invert_y);
    assert(waywal_config_parse_string("[transition]\npos = \"-10, 20\"\n", &inv_cfg, inv_err,
                                      sizeof(inv_err)));
    assert(fabsf(inv_cfg.transition.pos_x - 0.5f) < 1e-4f &&
           fabsf(inv_cfg.transition.pos_y - 0.5f) < 1e-4f);

    /* Per-namespace state store + crop_gravity persistence (including default vs non-default) */
    setenv("WWAL_STATE_DIR", "/tmp/wwal_ns_state_test", 1);
    mkdir("/tmp/wwal_ns_state_test", 0700);
    waywal_state_store_t st_def, st_a, st_b, st_load;
    waywal_state_store_init(&st_def);
    waywal_state_store_init(&st_a);
    waywal_state_store_init(&st_b);
    st_def.count = 1;
    strncpy(st_def.outputs[0].output_name, "DP-1", sizeof(st_def.outputs[0].output_name) - 1);
    strncpy(st_def.outputs[0].media_path, "/tmp/ns_default.png",
            sizeof(st_def.outputs[0].media_path) - 1);

    st_a.count = 1;
    strncpy(st_a.outputs[0].output_name, "DP-1", sizeof(st_a.outputs[0].output_name) - 1);
    strncpy(st_a.outputs[0].media_path, "/tmp/ns_alpha.png",
            sizeof(st_a.outputs[0].media_path) - 1);
    st_a.outputs[0].has_crop_gravity = true;
    st_a.outputs[0].crop_gravity_x = 0.2f;
    st_a.outputs[0].crop_gravity_y = 0.8f;

    st_b.count = 1;
    strncpy(st_b.outputs[0].output_name, "DP-1", sizeof(st_b.outputs[0].output_name) - 1);
    strncpy(st_b.outputs[0].media_path, "/tmp/ns_beta.png",
            sizeof(st_b.outputs[0].media_path) - 1);

    waywal_state_store_set_namespace("default");
    assert(waywal_state_store_save(&st_def));
    waywal_state_store_set_namespace("alpha");
    assert(waywal_state_store_save(&st_a));
    waywal_state_store_set_namespace("beta");
    assert(waywal_state_store_save(&st_b));

    waywal_state_store_set_namespace("default");
    assert(waywal_state_store_load(&st_load));
    assert(strcmp(st_load.outputs[0].media_path, "/tmp/ns_default.png") == 0);

    waywal_state_store_set_namespace("alpha");
    assert(waywal_state_store_load(&st_load));
    assert(strcmp(st_load.outputs[0].media_path, "/tmp/ns_alpha.png") == 0);
    assert(st_load.outputs[0].has_crop_gravity);
    assert(fabsf(st_load.outputs[0].crop_gravity_x - 0.2f) < 1e-5f);
    assert(fabsf(st_load.outputs[0].crop_gravity_y - 0.8f) < 1e-5f);
    waywal_state_store_set_namespace("default");
    unsetenv("WWAL_STATE_DIR");

    /* WAYLAND_DISPLAY with leading slash must not produce nested subdir in socket path */
    char old_wd[128] = {0};
    const char *ewd = getenv("WAYLAND_DISPLAY");
    if (ewd)
        strncpy(old_wd, ewd, sizeof(old_wd) - 1);
    setenv("WAYLAND_DISPLAY", "/run/user/1000/wayland-9", 1);
    path_buf_t sock_buf;
    memset(&sock_buf, 0, sizeof(sock_buf));
    assert(waywal_ipc_resolve_socket_path(&sock_buf, "ns1"));
    assert(strstr(sock_buf.data, "wayland-9-wwald.ns1.sock") != NULL);
    if (old_wd[0])
        setenv("WAYLAND_DISPLAY", old_wd, 1);
    else
        unsetenv("WAYLAND_DISPLAY");

    printf("  -> swww/awww v0.12.1 CLI/Env parity & auto-dispatch: PASSED\n");
}

static void test_ecosystem_and_picker(void)
{
    printf("[TEST] Testing Fast PNG Encoder, Ecosystem Cache Sync, & wwal pick CLI...\n");

    char tmp_dir[] = "/tmp/wwal_pick_unit_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    /* 1. Fast PNG encoder (BGRA & RGBA) + box-downscaling to <= 1280x720 */
    uint8_t bgra32[32 * 16 * 4];
    for (size_t i = 0; i < 32 * 16; ++i) {
        bgra32[i * 4 + 0] = 40;  /* B */
        bgra32[i * 4 + 1] = 120; /* G */
        bgra32[i * 4 + 2] = 220; /* R */
        bgra32[i * 4 + 3] = 255; /* A */
    }
    char png_path[512];
    snprintf(png_path, sizeof(png_path), "%s/saved_bgra.png", tmp_dir);
    assert(image_save_png_bgra(png_path, bgra32, 32, 16));

    raw_image_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    assert(image_load(png_path, &loaded));
    assert(loaded.width == 32 && loaded.height == 16 && loaded.data != NULL);
    /* image_load returns BGRA pixels */
    assert(loaded.data[0] == 40 && loaded.data[1] == 120 && loaded.data[2] == 220);
    image_free(&loaded);

    /* Test > 1280x720 downscaling via image_save_png_rgba */
    uint32_t big_w = 2560, big_h = 1440;
    uint8_t *big_rgba = (uint8_t *)malloc((size_t)big_w * big_h * 4);
    assert(big_rgba != NULL);
    for (size_t i = 0; i < (size_t)big_w * big_h; ++i) {
        big_rgba[i * 4 + 0] = 180;
        big_rgba[i * 4 + 1] = 90;
        big_rgba[i * 4 + 2] = 30;
        big_rgba[i * 4 + 3] = 255;
    }
    char big_png_path[512];
    snprintf(big_png_path, sizeof(big_png_path), "%s/saved_downscaled.png", tmp_dir);
    assert(image_save_png_rgba(big_png_path, big_rgba, big_w, big_h));
    free(big_rgba);

    memset(&loaded, 0, sizeof(loaded));
    assert(image_load(big_png_path, &loaded));
    assert(loaded.width == 1280 && loaded.height == 720);
    assert(loaded.data[0] == 30 && loaded.data[1] == 90 && loaded.data[2] == 180);
    image_free(&loaded);

    /* 2. Ecosystem cache sync (current_wallpaper, current_wallpaper.png, swww, awww, caelestia) */
    char old_home[256] = {0}, old_xcache[256] = {0}, old_xstate[256] = {0}, old_wcache[256] = {0};
    const char *eh = getenv("HOME");
    const char *exc = getenv("XDG_CACHE_HOME");
    const char *exs = getenv("XDG_STATE_HOME");
    const char *ewc = getenv("WWAL_CACHE_DIR");
    if (eh)
        strncpy(old_home, eh, sizeof(old_home) - 1);
    if (exc)
        strncpy(old_xcache, exc, sizeof(old_xcache) - 1);
    if (exs)
        strncpy(old_xstate, exs, sizeof(old_xstate) - 1);
    if (ewc)
        strncpy(old_wcache, ewc, sizeof(old_wcache) - 1);

    char fake_home[512], fake_xcache[512], fake_xstate[512], fake_wcache[512];
    snprintf(fake_home, sizeof(fake_home), "%s/home", tmp_dir);
    snprintf(fake_xcache, sizeof(fake_xcache), "%s/xdg_cache", tmp_dir);
    snprintf(fake_xstate, sizeof(fake_xstate), "%s/xdg_state", tmp_dir);
    snprintf(fake_wcache, sizeof(fake_wcache), "%s/xdg_cache/waywal", tmp_dir);
    mkdir(fake_home, 0700);
    mkdir(fake_xcache, 0700);
    mkdir(fake_xstate, 0700);
    mkdir(fake_wcache, 0700);

    setenv("HOME", fake_home, 1);
    setenv("XDG_CACHE_HOME", fake_xcache, 1);
    setenv("XDG_STATE_HOME", fake_xstate, 1);
    setenv("WWAL_CACHE_DIR", fake_wcache, 1);

    assert(waywal_ecosystem_cache_sync("DP-1", png_path, "Lanczos3", bgra32, 32, 16, true, false));

    char cur_sym[600], cur_png[600], swww_dp1[600], awww_dp1[600], cael_txt[600];
    snprintf(cur_sym, sizeof(cur_sym), "%s/current_wallpaper", fake_wcache);
    snprintf(cur_png, sizeof(cur_png), "%s/current_wallpaper.png", fake_wcache);
    snprintf(swww_dp1, sizeof(swww_dp1), "%s/swww/DP-1", fake_xcache);
    snprintf(awww_dp1, sizeof(awww_dp1), "%s/awww/DP-1", fake_xcache);
    snprintf(cael_txt, sizeof(cael_txt), "%s/caelestia/wallpaper/path.txt", fake_xstate);

    char sym_target[512] = {0};
    ssize_t slen = readlink(cur_sym, sym_target, sizeof(sym_target) - 1);
    assert(slen > 0);
    sym_target[slen] = '\0';
    assert(strcmp(sym_target, png_path) == 0);

    memset(&loaded, 0, sizeof(loaded));
    assert(image_load(cur_png, &loaded));
    assert(loaded.width == 32 && loaded.height == 16);
    image_free(&loaded);

    char expected_swww[600];
    snprintf(expected_swww, sizeof(expected_swww), "Lanczos3\n%s\n", png_path);
    assert_file_contains(swww_dp1, expected_swww);
    assert_file_contains(awww_dp1, expected_swww);
    assert_file_contains(cael_txt, png_path);

    /* Shader preview fallback in waywal_ecosystem_cache_sync */
    char frag_path[512];
    snprintf(frag_path, sizeof(frag_path), "%s/test_shader.frag", tmp_dir);
    FILE *ffp = fopen(frag_path, "wb");
    assert(ffp != NULL);
    fputs("void mainImage(out vec4 c, in vec2 f) { c = vec4(0.1, 0.8, 0.3, 1.0); }\n", ffp);
    fclose(ffp);
    unlink(cur_png);
    assert(
        waywal_ecosystem_cache_sync("HDMI-A-1", frag_path, "Bilinear", NULL, 0, 0, true, false));
    memset(&loaded, 0, sizeof(loaded));
    assert(image_load(cur_png, &loaded));
    assert(loaded.width == 320 && loaded.height == 180);
    image_free(&loaded);

    /* no_cache = true must be a no-op */
    char swww_dp2[600];
    snprintf(swww_dp2, sizeof(swww_dp2), "%s/swww/DP-2", fake_xcache);
    assert(waywal_ecosystem_cache_sync("DP-2", png_path, "Nearest", bgra32, 32, 16, true, true));
    assert(access(swww_dp2, F_OK) != 0);

    /* Path traversal output names ("../pwn", "*", "unknown") must not create swww files */
    char swww_pwn[600];
    snprintf(swww_pwn, sizeof(swww_pwn), "%s/pwn", fake_xcache);
    assert(waywal_ecosystem_cache_sync("../pwn", png_path, "Lanczos3", bgra32, 32, 16, true, false));
    assert(access(swww_pwn, F_OK) != 0);

    /* Comma-separated output_name ("DP-3,HDMI-A-2") writes separate per-output swww/awww files */
    char swww_dp3[600], awww_hdmi2[600], swww_comma[600];
    snprintf(swww_dp3, sizeof(swww_dp3), "%s/swww/DP-3", fake_xcache);
    snprintf(awww_hdmi2, sizeof(awww_hdmi2), "%s/awww/HDMI-A-2", fake_xcache);
    snprintf(swww_comma, sizeof(swww_comma), "%s/swww/DP-3,HDMI-A-2", fake_xcache);
    assert(waywal_ecosystem_cache_sync("DP-3,HDMI-A-2", png_path, "Lanczos3", bgra32, 32, 16, true,
                                       false));
    assert_file_contains(swww_dp3, expected_swww);
    assert_file_contains(awww_hdmi2, expected_swww);
    assert(access(swww_comma, F_OK) != 0);

    /* Self-copy & self-symlink guards when current_wallpaper.png or current_wallpaper is passed */
    assert(waywal_ecosystem_cache_sync("DP-1", cur_png, "Lanczos3", NULL, 0, 0, true, false));
    memset(&loaded, 0, sizeof(loaded));
    assert(image_load(cur_png, &loaded));
    assert(loaded.width == 32 && loaded.height == 16);
    image_free(&loaded);
    assert(waywal_ecosystem_cache_sync("DP-1", cur_sym, "Lanczos3", NULL, 0, 0, true, false));
    assert(access(cur_sym, R_OK) == 0);

    /* Box-filter area averaging on high-frequency checkerboard downscale (2560x1440 -> 1280x720) */
    {
        uint32_t cw = 2560, ch = 1440;
        uint8_t *chk = (uint8_t *)malloc((size_t)cw * ch * 4);
        assert(chk != NULL);
        for (uint32_t y = 0; y < ch; ++y) {
            for (uint32_t x = 0; x < cw; ++x) {
                uint8_t *p = chk + ((size_t)y * cw + x) * 4;
                bool odd = ((x + y) & 1U) != 0;
                p[0] = odd ? 200 : 0;
                p[1] = odd ? 100 : 0;
                p[2] = odd ? 60 : 0;
                p[3] = 255;
            }
        }
        char nested_png[600];
        snprintf(nested_png, sizeof(nested_png), "%s/nested/sub/checker_downscaled.png", tmp_dir);
        assert(image_save_png_rgba(nested_png, chk, cw, ch));
        free(chk);

        memset(&loaded, 0, sizeof(loaded));
        assert(image_load(nested_png, &loaded));
        assert(loaded.width == 1280 && loaded.height == 720);
        /* Box average of (200,100,60) and (0,0,0) in BGRA is B=30, G=50, R=100 */
        assert(abs((int)loaded.data[0] - 30) <= 2);
        assert(abs((int)loaded.data[1] - 50) <= 2);
        assert(abs((int)loaded.data[2] - 100) <= 2);
        image_free(&loaded);
    }

    /* Reject oversized PNG fast-path copy in waywal_ecosystem_cache_sync */
    {
        uint32_t ow = 2560, oh = 1440;
        uint8_t *obgra = (uint8_t *)calloc((size_t)ow * oh * 4, 1);
        assert(obgra != NULL);
        assert(
            waywal_ecosystem_cache_sync("DP-1", png_path, "Lanczos3", obgra, ow, oh, true, false));
        free(obgra);
        memset(&loaded, 0, sizeof(loaded));
        assert(image_load(cur_png, &loaded));
        assert(loaded.width == 1280 && loaded.height == 720);
        image_free(&loaded);

        /* Also verify an on-disk PNG whose IHDR dimensions exceed 1280x720 is never fast-copied
         * even if caller passes small preview buffer dimensions (32x16) */
        char fake_4k_png[600];
        snprintf(fake_4k_png, sizeof(fake_4k_png), "%s/fake_4k_ihdr.png", tmp_dir);
        FILE *src_fp = fopen(png_path, "rb");
        assert(src_fp != NULL);
        uint8_t raw_png[4096];
        size_t raw_len = fread(raw_png, 1, sizeof(raw_png), src_fp);
        fclose(src_fp);
        assert(raw_len >= 24);
        /* Patch big-endian IHDR width=3840 (0x00000F00), height=2160 (0x00000870) */
        raw_png[16] = 0x00;
        raw_png[17] = 0x00;
        raw_png[18] = 0x0F;
        raw_png[19] = 0x00;
        raw_png[20] = 0x00;
        raw_png[21] = 0x00;
        raw_png[22] = 0x08;
        raw_png[23] = 0x70;
        FILE *dst_fp = fopen(fake_4k_png, "wb");
        assert(dst_fp != NULL);
        assert(fwrite(raw_png, 1, raw_len, dst_fp) == raw_len);
        fclose(dst_fp);

        assert(waywal_ecosystem_cache_sync("DP-1", fake_4k_png, "Lanczos3", bgra32, 32, 16, true,
                                           false));
        memset(&loaded, 0, sizeof(loaded));
        assert(image_load(cur_png, &loaded));
        assert(loaded.width == 32 && loaded.height == 16);
        image_free(&loaded);
    }

    /* Workshop directory passed as media_path must resolve current_wallpaper & caelestia to an
     * image file, never a directory */
    {
        char ws_mock_dir[600], ws_mock_json[640], ws_mock_prev[640];
        snprintf(ws_mock_dir, sizeof(ws_mock_dir), "%s/ws_item_dir", tmp_dir);
        mkdir(ws_mock_dir, 0700);
        snprintf(ws_mock_json, sizeof(ws_mock_json), "%s/project.json", ws_mock_dir);
        snprintf(ws_mock_prev, sizeof(ws_mock_prev), "%s/preview.png", ws_mock_dir);
        FILE *jfp = fopen(ws_mock_json, "wb");
        assert(jfp != NULL);
        fputs("{\"title\":\"DirSyncTest\",\"type\":\"scene\",\"preview\":\"preview.png\"}\n", jfp);
        fclose(jfp);
        assert(image_save_png_bgra(ws_mock_prev, bgra32, 32, 16));

        assert(waywal_ecosystem_cache_sync("DP-1", ws_mock_dir, "Lanczos3", NULL, 0, 0, true,
                                           false));
        struct stat st_sym;
        assert(stat(cur_sym, &st_sym) == 0 && S_ISREG(st_sym.st_mode));
        assert_file_contains(cael_txt, ws_mock_prev);
    }

    /* Reject colon/whitespace/desc: in swww output names */
    {
        char bad_swww[600];
        snprintf(bad_swww, sizeof(bad_swww), "%s/swww/desc:Microstep", fake_xcache);
        assert(waywal_ecosystem_cache_sync("desc:Microstep", png_path, "Lanczos3", bgra32, 32, 16,
                                           false, false));
        assert(access(bad_swww, F_OK) != 0);
    }

    if (old_home[0])
        setenv("HOME", old_home, 1);
    if (old_xcache[0])
        setenv("XDG_CACHE_HOME", old_xcache, 1);
    else
        unsetenv("XDG_CACHE_HOME");
    if (old_xstate[0])
        setenv("XDG_STATE_HOME", old_xstate, 1);
    else
        unsetenv("XDG_STATE_HOME");
    if (old_wcache[0])
        setenv("WWAL_CACHE_DIR", old_wcache, 1);
    else
        unsetenv("WWAL_CACHE_DIR");

    /* 3. CLI parsing for wwal pick and wwal workshop sync-previews */
    cli_options_t opts;
    char *argv_pick1[] = {"wwal",  "pick",    "--menu", "rofi",    "--dir", "/tmp",
                          "--theme", "-n",    "pick_ns", "--loop", "4",     "--speed",
                          "1.75"};
    assert(cli_parse(13, argv_pick1, &opts));
    assert(opts.cmd == CLI_CMD_PICK);
    assert(opts.pick_menu == WAYWAL_PICK_MENU_ROFI);
    assert(strcmp(opts.pick_dir, "/tmp") == 0);
    assert(opts.extract_theme);
    assert(strcmp(opts.namespace_str, "pick_ns") == 0);
    assert(opts.video_loop_count == 4U);
    assert(fabsf(opts.video_speed - 1.75f) < 1e-5f);

    char *argv_img_vid[] = {"wwal", "img", "/tmp/clip.mp4", "--loop", "2", "--speed", "0.5"};
    assert(cli_parse(7, argv_img_vid, &opts));
    cli_auto_dispatch_img(&opts);
    assert(opts.cmd == CLI_CMD_VIDEO);
    assert(opts.video_loop_count == 2U);
    assert(fabsf(opts.video_speed - 0.5f) < 1e-5f);

    char *argv_pick2[] = {"wwal", "pick", "--menu", "json", "/var/wallpapers"};
    assert(cli_parse(5, argv_pick2, &opts));
    assert(opts.cmd == CLI_CMD_PICK);
    assert(opts.pick_menu == WAYWAL_PICK_MENU_JSON);
    assert(strcmp(opts.pick_dir, "/var/wallpapers") == 0);

    char *argv_ws_sync[] = {"wwal", "workshop", "sync-previews", "/tmp/ws_previews"};
    assert(cli_parse(4, argv_ws_sync, &opts));
    assert(opts.cmd == CLI_CMD_WORKSHOP);
    assert(opts.workshop_subcmd == WAYWAL_WORKSHOP_SYNC_PREVIEWS);
    assert(strcmp(opts.filepath, "/tmp/ws_previews") == 0);

    char *argv_ws_sync_def[] = {"wwal", "workshop", "sync-previews"};
    assert(cli_parse(3, argv_ws_sync_def, &opts));
    assert(opts.cmd == CLI_CMD_WORKSHOP);
    assert(opts.workshop_subcmd == WAYWAL_WORKSHOP_SYNC_PREVIEWS);
    assert(opts.filepath[0] == '\0');

    printf("  -> Fast PNG encoder, ecosystem cache sync & wwal pick CLI: PASSED\n");
}

static void test_superset_template_engine(void)
{
    printf("[TEST] Testing Superset Template Engine (matugen + wallust + pywal)...\n");

    /* 1. waywal_expand_path expansion (~, $HOME, $XDG_CONFIG_HOME, ${XDG_CACHE_HOME}) */
    char old_home[512] = {0};
    char old_xcfg[512] = {0};
    char old_xcache[512] = {0};
    if (getenv("HOME"))
        snprintf(old_home, sizeof(old_home), "%s", getenv("HOME"));
    if (getenv("XDG_CONFIG_HOME"))
        snprintf(old_xcfg, sizeof(old_xcfg), "%s", getenv("XDG_CONFIG_HOME"));
    if (getenv("XDG_CACHE_HOME"))
        snprintf(old_xcache, sizeof(old_xcache), "%s", getenv("XDG_CACHE_HOME"));

    char tmp_dir[] = "/tmp/wwal_test_tmpl_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);
    setenv("HOME", tmp_dir, 1);
    char xcfg_dir[600];
    snprintf(xcfg_dir, sizeof(xcfg_dir), "%s/custom_cfg", tmp_dir);
    setenv("XDG_CONFIG_HOME", xcfg_dir, 1);
    char xcache_dir[600];
    snprintf(xcache_dir, sizeof(xcache_dir), "%s/custom_cache", tmp_dir);
    setenv("XDG_CACHE_HOME", xcache_dir, 1);

    char expanded[1024];
    assert(waywal_expand_path("~/kitty/colors.conf", expanded, sizeof(expanded)));
    char expected_path[1024];
    snprintf(expected_path, sizeof(expected_path), "%s/kitty/colors.conf", tmp_dir);
    assert(strcmp(expanded, expected_path) == 0);

    assert(waywal_expand_path("$XDG_CONFIG_HOME/waybar/colors.css", expanded, sizeof(expanded)));
    snprintf(expected_path, sizeof(expected_path), "%s/waybar/colors.css", xcfg_dir);
    assert(strcmp(expanded, expected_path) == 0);

    assert(waywal_expand_path("${XDG_CACHE_HOME}/wal/colors.json", expanded, sizeof(expanded)));
    snprintf(expected_path, sizeof(expected_path), "%s/wal/colors.json", xcache_dir);
    assert(strcmp(expanded, expected_path) == 0);

    unsetenv("XDG_CONFIG_HOME");
    assert(waywal_expand_path("$XDG_CONFIG_HOME/rofi/theme.rasi", expanded, sizeof(expanded)));
    snprintf(expected_path, sizeof(expected_path), "%s/.config/rofi/theme.rasi", tmp_dir);
    assert(strcmp(expanded, expected_path) == 0);
    setenv("XDG_CONFIG_HOME", xcfg_dir, 1);

    /* 2. TOML [template.<name>] / [[templates]] and [theme] seed/pywal_compat parsing */
    const char *toml_tpl =
        "[theme]\n"
        "enabled = true\n"
        "mode = \"dark\"\n"
        "scheme_variant = \"tonal_spot\"\n"
        "source_color_index = 2\n"
        "seed_color = \"#ff5500\"\n"
        "pywal_compat = true\n"
        "\n"
        "[template.kitty]\n"
        "input_path = \"~/templates/kitty.conf\"\n"
        "output_path = \"$XDG_CONFIG_HOME/kitty/colors.conf\"\n"
        "post_hook = \"touch $HOME/kitty_hook_ran\"\n"
        "\n"
        "[[templates]]\n"
        "name = \"waybar\"\n"
        "input = \"~/templates/waybar.css\"\n"
        "output = \"~/out/waybar.css\"\n";

    waywal_config_t cfg;
    waywal_config_init_defaults(&cfg);
    assert(waywal_config_parse_string(toml_tpl, &cfg, NULL, 0));
    assert(cfg.theme.source_color_index == 2U);
    assert(cfg.theme.has_seed_color);
    assert(cfg.theme.seed_color == 0xFF5500U);
    assert(cfg.theme.pywal_compat);
    assert(cfg.template_count == 2U);
    assert(strcmp(cfg.templates[0].name, "kitty") == 0);
    snprintf(expected_path, sizeof(expected_path), "%s/templates/kitty.conf", tmp_dir);
    assert(strcmp(cfg.templates[0].input_path, expected_path) == 0);
    snprintf(expected_path, sizeof(expected_path), "%s/kitty/colors.conf", xcfg_dir);
    assert(strcmp(cfg.templates[0].output_path, expected_path) == 0);
    assert(strcmp(cfg.templates[1].name, "waybar") == 0);

    /* 3. CLI parsing for --source-color-index and --seed-color */
    cli_options_t opts;
    char *argv_theme[] = {"wwal", "theme", "--source-color-index", "3", "--seed-color", "#336699"};
    assert(cli_parse(6, argv_theme, &opts));
    assert(opts.cmd == CLI_CMD_THEME);
    assert(opts.has_source_color_index);
    assert(opts.source_color_index == 3U);
    assert(opts.has_seed_color);
    assert(opts.seed_color == 0x336699U);

    char *argv_bad_idx[] = {"wwal", "theme", "--source-color-index", "4"};
    assert(!cli_parse(4, argv_bad_idx, &opts));

    char *argv_bad_seed[] = {"wwal", "theme", "--seed-color", "not_a_hex"};
    assert(!cli_parse(4, argv_bad_seed, &opts));

    /* 4. Multi-seed cluster ranking & explicit seed override */
    uint8_t multi_img[64 * 64 * 4];
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            size_t idx = ((size_t)y * 64U + (size_t)x) * 4U;
            if (y < 32 && x < 32) {
                /* Red quadrant (BGRA) */
                multi_img[idx + 0] = 20;
                multi_img[idx + 1] = 20;
                multi_img[idx + 2] = 235;
                multi_img[idx + 3] = 255;
            } else if (y < 32 && x >= 32) {
                /* Green quadrant */
                multi_img[idx + 0] = 25;
                multi_img[idx + 1] = 220;
                multi_img[idx + 2] = 30;
                multi_img[idx + 3] = 255;
            } else if (y >= 32 && x < 32) {
                /* Blue quadrant */
                multi_img[idx + 0] = 240;
                multi_img[idx + 1] = 40;
                multi_img[idx + 2] = 20;
                multi_img[idx + 3] = 255;
            } else {
                /* Yellow/Orange quadrant */
                multi_img[idx + 0] = 15;
                multi_img[idx + 1] = 200;
                multi_img[idx + 2] = 245;
                multi_img[idx + 3] = 255;
            }
        }
    }

    waywal_palette_t pal_s0, pal_s1, pal_seed, pal_light;
    assert(waywal_palette_extract_seed_ex(multi_img, 64, 64, 64 * 4, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 0, false, 0, &pal_s0));
    assert(waywal_palette_extract_seed_ex(multi_img, 64, 64, 64 * 4, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 1, false, 0, &pal_s1));
    assert(pal_s0.seed_count >= 2U);
    assert(pal_s0.source_color != pal_s1.source_color);
    assert(pal_s0.primary.r != pal_s1.primary.r || pal_s0.primary.g != pal_s1.primary.g ||
           pal_s0.primary.b != pal_s1.primary.b);

    assert(waywal_palette_extract_seed_ex(multi_img, 64, 64, 64 * 4, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 0, true, 0xFF5500U,
                                          &pal_seed));
    assert(pal_seed.source_color == 0xFF5500U);
    snprintf(pal_seed.wallpaper_path, sizeof(pal_seed.wallpaper_path), "/tmp/test_wallpaper.png");
    waywal_palette_derive_opposite(&pal_seed, &pal_light);
    assert(pal_light.is_light_mode);
    assert(pal_light.background.r > pal_seed.background.r);

    /* 4b. Verify pure-black (#000000) seed_color override preserves source_color == 0x000000 */
    waywal_palette_t pal_black_dark, pal_black_light;
    assert(waywal_palette_extract_seed_ex(multi_img, 64, 64, 64 * 4, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 0, true, 0x000000U,
                                          &pal_black_dark));
    assert(pal_black_dark.source_color == 0x000000U);
    assert(pal_black_dark.seed_count >= 1U);
    waywal_palette_derive_opposite(&pal_black_dark, &pal_black_light);
    assert(pal_black_light.source_color == 0x000000U);

    /* 5. Superset Template Engine (matugen + wallust + pywal + HSL filters + edge cases) */
    const char *tpl_src =
        "matugen_def={{colors.primary.default.hex}}\n"
        "matugen_dark={{colors.primary.dark.hex}}\n"
        "matugen_light={{colors.primary.light.hex}}\n"
        "matugen_strip={{colors.primary_container.default.hex_stripped}}\n"
        "matugen_rgb={{colors.on_primary.default.rgb}}\n"
        "matugen_rgba={{colors.surface_container_high.default.rgba}}\n"
        "matugen_tone={{palettes.primary.40.hex}}\n"
        "matugen_src={{colors.source_color.default.hex}}\n"
        "matugen_img={{image}}\n"
        "matugen_mode={{mode}}\n"
        "wallust_hex={{primary.hex}}\n"
        "wallust_strip={{primary.strip}}\n"
        "wallust_hsl={{primary.hsl}}\n"
        "wallust_oklch={{primary.oklch}}\n"
        "wallust_r={{primary.red}}\n"
        "wallust_g={{primary.green}}\n"
        "wallust_b={{primary.blue}}\n"
        "wallust_a={{primary.alpha}}\n"
        "wallust_hue={{primary.hue}}\n"
        "wallust_sat={{primary.saturation}}\n"
        "wallust_light={{primary.lightness}}\n"
        "wallust_c0={{color0.strip}}\n"
        "pywal_bg={background}\n"
        "pywal_c1_strip={color1.strip}\n"
        "pywal_c2_xrgba={color2.xrgba}\n"
        "pywal_c3_octal={color3.octal}\n"
        "pywal_wp={wallpaper}\n"
        "css_preserve=body { color: #fff; background: {{background.hex}}; }\n"
        "css_media=@media (min-width: 600px) { body { color: #{color0.strip}; } }\n"
        "filter_lighten={{ colors.primary.default.hex | lighten(0.2) }}\n"
        "filter_neg_lighten={{ #ff0000 | lighten(-20) | hex }}\n"
        "filter_paren={{ (colors.primary.dark | darken(0.15)).hex_stripped }}\n"
        "filter_nested_paren={{ ((colors.primary.dark | lighten(0.1)) | darken(0.05)).hex }}\n"
        "filter_paren_pipe={{ (colors.primary.dark | lighten(0.1)) | strip }}\n"
        "filter_paren_prop_pipe={{ (colors.primary.dark | lighten(0.1)).hex | darken(0.05) }}\n"
        "filter_chain={{ primary | saturate(0.2) | set_alpha(0.5) | rgba }}\n"
        "filter_rgb_alpha={{ colors.primary.default.rgb | alpha(0.8) }}\n"
        "filter_hsl_comp={{ #ff0000 | complementary | hex }}\n"
        "filter_hsl_setl={{ #336699 | set_lightness(50) | lightness }}\n"
        "filter_hsl_desat={{ #ff0000 | desaturate(0.5) | saturation }}\n"
        "filter_blend={{ primary | blend(#ffffff, 0.5) | hex }}\n"
        "filter_inv={{ primary | invert | complementary | hex }}\n"
        "malformed_empty_pipe={{ primary | | hex }}\n"
        "malformed_nan={{ primary | lighten(NaN) }}\n"
        "malformed_blend_nan={{ primary | blend(#ffffff, NaN) }}\n"
        "malformed_garbage={{ primary | lighten(0.2abc) }}\n"
        "malformed_pal_extra={{ palettes.primary.40.hex.extra }}\n"
        "malformed_unmatched_paren={{ (colors.primary.dark | lighten(0.1) }}\n"
        "malformed_unclosed={{ colors.primary.default.hex\n";

    char unit_tpl_in[640], unit_tpl_out[640];
    snprintf(unit_tpl_in, sizeof(unit_tpl_in), "%s/unit_superset.in", tmp_dir);
    snprintf(unit_tpl_out, sizeof(unit_tpl_out), "%s/unit_superset.out", tmp_dir);
    FILE *ufp = fopen(unit_tpl_in, "wb");
    assert(ufp != NULL);
    fputs(tpl_src, ufp);
    fclose(ufp);
    assert(waywal_palette_render_template_file(&pal_seed, &pal_seed, &pal_light, unit_tpl_in,
                                               unit_tpl_out));
    char rendered[8192] = {0};
    ufp = fopen(unit_tpl_out, "rb");
    assert(ufp != NULL);
    size_t rlen = fread(rendered, 1, sizeof(rendered) - 1, ufp);
    rendered[rlen] = '\0';
    fclose(ufp);

    char expected_line[256];
    snprintf(expected_line, sizeof(expected_line), "matugen_def=#%02x%02x%02x", pal_seed.primary.r,
             pal_seed.primary.g, pal_seed.primary.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line), "matugen_dark=#%02x%02x%02x", pal_seed.primary.r,
             pal_seed.primary.g, pal_seed.primary.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line), "matugen_light=#%02x%02x%02x",
             pal_light.primary.r, pal_light.primary.g, pal_light.primary.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line), "matugen_strip=%02x%02x%02x",
             pal_seed.primaryContainer.r, pal_seed.primaryContainer.g,
             pal_seed.primaryContainer.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line), "matugen_rgba=rgba(%u, %u, %u, 1.00)",
             pal_seed.surfaceContainerHigh.r, pal_seed.surfaceContainerHigh.g,
             pal_seed.surfaceContainerHigh.b);
    assert(strstr(rendered, expected_line) != NULL);

    assert(strstr(rendered, "matugen_src=#ff5500\n") != NULL);
    assert(strstr(rendered, "matugen_img=/tmp/test_wallpaper.png") != NULL);
    assert(strstr(rendered, "matugen_mode=dark") != NULL);
    assert(strstr(rendered, "wallust_a=1.00\n") != NULL);
    assert(strstr(rendered, "pywal_wp=/tmp/test_wallpaper.png") != NULL);

    snprintf(expected_line, sizeof(expected_line), "pywal_bg=#%02x%02x%02x",
             pal_seed.background.r, pal_seed.background.g, pal_seed.background.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line), "pywal_c1_strip=%02x%02x%02x",
             pal_seed.term[1].r, pal_seed.term[1].g, pal_seed.term[1].b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line),
             "css_preserve=body { color: #fff; background: #%02x%02x%02x; }",
             pal_seed.background.r, pal_seed.background.g, pal_seed.background.b);
    assert(strstr(rendered, expected_line) != NULL);

    snprintf(expected_line, sizeof(expected_line),
             "css_media=@media (min-width: 600px) { body { color: #%02x%02x%02x; } }",
             pal_seed.term[0].r, pal_seed.term[0].g, pal_seed.term[0].b);
    assert(strstr(rendered, expected_line) != NULL);

    assert(strstr(rendered, ", 0.50)") != NULL);
    snprintf(expected_line, sizeof(expected_line), "filter_rgb_alpha=rgba(%u, %u, %u, 0.80)",
             pal_seed.primary.r, pal_seed.primary.g, pal_seed.primary.b);
    assert(strstr(rendered, expected_line) != NULL);

    assert(strstr(rendered, "filter_hsl_comp=#00ffff\n") != NULL);
    assert(strstr(rendered, "filter_hsl_setl=50\n") != NULL);
    assert(strstr(rendered, "filter_hsl_desat=50\n") != NULL);
    assert(strstr(rendered, "filter_neg_lighten=#990000\n") != NULL);
    assert(strstr(rendered, "filter_nested_paren=#") != NULL);
    assert(strstr(rendered, "filter_paren_pipe={{") == NULL);
    assert(strstr(rendered, "filter_paren_prop_pipe=#") != NULL);

    /* Verify malformed expressions are left untouched */
    assert(strstr(rendered, "malformed_empty_pipe={{ primary | | hex }}") != NULL);
    assert(strstr(rendered, "malformed_nan={{ primary | lighten(NaN) }}") != NULL);
    assert(strstr(rendered, "malformed_blend_nan={{ primary | blend(#ffffff, NaN) }}") != NULL);
    assert(strstr(rendered, "malformed_garbage={{ primary | lighten(0.2abc) }}") != NULL);
    assert(strstr(rendered, "malformed_pal_extra={{ palettes.primary.40.hex.extra }}") != NULL);
    assert(strstr(rendered, "malformed_unmatched_paren={{ (colors.primary.dark | lighten(0.1) }}") !=
           NULL);
    assert(strstr(rendered, "malformed_unclosed={{ colors.primary.default.hex") != NULL);

    /* Verify #000000 source_color renders #000000 in both dark and light contexts */
    ufp = fopen(unit_tpl_in, "wb");
    assert(ufp != NULL);
    fputs("src_dark={{colors.source_color.dark.hex}}\nsrc_light={{colors.source_color.light.hex}}\n",
          ufp);
    fclose(ufp);
    assert(waywal_palette_render_template_file(&pal_black_dark, &pal_black_dark, &pal_black_light,
                                               unit_tpl_in, unit_tpl_out));
    assert_file_contains(unit_tpl_out, "src_dark=#000000");
    assert_file_contains(unit_tpl_out, "src_light=#000000");

    /* 6. End-to-end file template rendering + post_hook + pywal cache export */
    char tpl_dir[600];
    snprintf(tpl_dir, sizeof(tpl_dir), "%s/templates", tmp_dir);
    mkdir(tpl_dir, 0755);

    char kitty_tpl_path[640];
    snprintf(kitty_tpl_path, sizeof(kitty_tpl_path), "%s/kitty.conf", tpl_dir);
    FILE *fp = fopen(kitty_tpl_path, "wb");
    assert(fp != NULL);
    fprintf(fp, "background {{colors.background.default.hex}}\nforeground {foreground}\n");
    fclose(fp);

    char waybar_tpl_path[640];
    snprintf(waybar_tpl_path, sizeof(waybar_tpl_path), "%s/waybar.css", tpl_dir);
    fp = fopen(waybar_tpl_path, "wb");
    assert(fp != NULL);
    fprintf(fp, "@define-color primary {{ (colors.primary.default | lighten(0.1)).hex }};\n");
    fclose(fp);

    /* 6a. Verify pywal_compat = false suppresses ~/.cache/wal/ creation */
    char pywal_json[640];
    snprintf(pywal_json, sizeof(pywal_json), "%s/wal/colors.json", xcache_dir);
    cfg.theme.pywal_compat = false;
    assert(waywal_palette_apply_configured_templates(&pal_seed, &pal_seed, &pal_light, &cfg) == 2U);
    assert(access(pywal_json, F_OK) != 0);

    /* 6b. Verify pywal_compat = true exports ~/.cache/wal/ files */
    cfg.theme.pywal_compat = true;
    assert(waywal_palette_apply_configured_templates(&pal_seed, &pal_seed, &pal_light, &cfg) == 2U);

    char kitty_out[640];
    snprintf(kitty_out, sizeof(kitty_out), "%s/kitty/colors.conf", xcfg_dir);
    assert_file_contains(kitty_out, "background #");

    char waybar_out[640];
    snprintf(waybar_out, sizeof(waybar_out), "%s/out/waybar.css", tmp_dir);
    assert_file_contains(waybar_out, "@define-color primary #");

    char hook_marker[640];
    snprintf(hook_marker, sizeof(hook_marker), "%s/kitty_hook_ran", tmp_dir);
    for (int retry = 0; retry < 50 && access(hook_marker, F_OK) != 0; ++retry) {
        usleep(2000);
    }
    assert(access(hook_marker, F_OK) == 0);

    assert_file_contains(pywal_json, "\"wallpaper\": \"/tmp/test_wallpaper.png\"");
    assert_file_contains(pywal_json, "\"color0\": \"#");

    char pywal_sh[640];
    snprintf(pywal_sh, sizeof(pywal_sh), "%s/wal/colors.sh", xcache_dir);
    assert_file_contains(pywal_sh, "wallpaper='/tmp/test_wallpaper.png'");

    char pywal_seq[640];
    snprintf(pywal_seq, sizeof(pywal_seq), "%s/wal/sequences", xcache_dir);
    assert(access(pywal_seq, F_OK) == 0);

    if (old_home[0])
        setenv("HOME", old_home, 1);
    if (old_xcfg[0])
        setenv("XDG_CONFIG_HOME", old_xcfg, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    if (old_xcache[0])
        setenv("XDG_CACHE_HOME", old_xcache, 1);
    else
        unsetenv("XDG_CACHE_HOME");

    printf("  -> Superset Template Engine (matugen + wallust + pywal) & Seed Override: PASSED\n");
}

static void test_template_security_and_edge_cases(void)
{
    printf("[TEST] Template Security & Palette Edge Cases...\n");

    /* 1. waywal_normalize_path lexical canonicalization */
    char norm_buf[256];
    snprintf(norm_buf, sizeof(norm_buf), "/home/user/.config/../../.bashrc");
    waywal_normalize_path(norm_buf);
    assert(strcmp(norm_buf, "/home/.bashrc") == 0);

    snprintf(norm_buf, sizeof(norm_buf), "/home/user/./a/b/../c//d/");
    waywal_normalize_path(norm_buf);
    assert(strcmp(norm_buf, "/home/user/a/c/d") == 0);

    snprintf(norm_buf, sizeof(norm_buf), "/../../../etc/passwd");
    waywal_normalize_path(norm_buf);
    assert(strcmp(norm_buf, "/etc/passwd") == 0);

    /* 2. waywal_expand_path strict failure & lexical normalization */
    unsetenv("WWAL_TEST_NONEXISTENT_ENV_VAR");
    char exp_buf[512];
    snprintf(exp_buf, sizeof(exp_buf), "sentinel");
    assert(!waywal_expand_path("${WWAL_TEST_NONEXISTENT_ENV_VAR}/colors.conf", exp_buf,
                               sizeof(exp_buf)));
    assert(exp_buf[0] == '\0');

    snprintf(exp_buf, sizeof(exp_buf), "sentinel");
    assert(!waywal_expand_path("$WWAL_TEST_NONEXISTENT_ENV_VAR/colors.conf", exp_buf,
                               sizeof(exp_buf)));
    assert(exp_buf[0] == '\0');

    snprintf(exp_buf, sizeof(exp_buf), "sentinel");
    assert(!waywal_expand_path("${UNCLOSED_VAR/colors.conf", exp_buf, sizeof(exp_buf)));
    assert(exp_buf[0] == '\0');

    snprintf(exp_buf, sizeof(exp_buf), "sentinel");
    assert(!waywal_expand_path("${INVALID/VAR}/colors.conf", exp_buf, sizeof(exp_buf)));
    assert(exp_buf[0] == '\0');

    snprintf(exp_buf, sizeof(exp_buf), "sentinel");
    assert(!waywal_expand_path("${123}/colors.conf", exp_buf, sizeof(exp_buf)));
    assert(exp_buf[0] == '\0');

    char old_xdg_rt[256] = {0};
    const char *env_xdg_rt = getenv("XDG_RUNTIME_DIR");
    if (env_xdg_rt) {
        snprintf(old_xdg_rt, sizeof(old_xdg_rt), "%s", env_xdg_rt);
    }
    unsetenv("XDG_RUNTIME_DIR");
    assert(waywal_expand_path("$XDG_RUNTIME_DIR/wwal/../wwal/sock", exp_buf, sizeof(exp_buf)));
    char expected_rt[256];
    snprintf(expected_rt, sizeof(expected_rt), "/run/user/%u/wwal/sock", (unsigned int)getuid());
    assert(strcmp(exp_buf, expected_rt) == 0);
    if (old_xdg_rt[0] != '\0') {
        setenv("XDG_RUNTIME_DIR", old_xdg_rt, 1);
    }

    /* 3. Temporary sandbox directory for template safety, symlink preservation, & escaping */
    char tmp_dir[] = "/tmp/wwal_test_sec_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char old_home[512] = {0};
    char old_xcache[512] = {0};
    if (getenv("HOME"))
        snprintf(old_home, sizeof(old_home), "%s", getenv("HOME"));
    if (getenv("XDG_CACHE_HOME"))
        snprintf(old_xcache, sizeof(old_xcache), "%s", getenv("XDG_CACHE_HOME"));

    setenv("HOME", tmp_dir, 1);
    char xcache_dir[600];
    snprintf(xcache_dir, sizeof(xcache_dir), "%s/.cache", tmp_dir);
    mkdir(xcache_dir, 0755);
    setenv("XDG_CACHE_HOME", xcache_dir, 1);

    /* 4. HCT zero-sample monochrome, self-aliasing derive_opposite, and seed override index 0 */
    uint8_t multi_bgra[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; ++i) {
        if (i < 128) {
            multi_bgra[i * 4 + 0] = 0x10;
            multi_bgra[i * 4 + 1] = 0x20;
            multi_bgra[i * 4 + 2] = 0xE0;
            multi_bgra[i * 4 + 3] = 0xFF;
        } else {
            multi_bgra[i * 4 + 0] = 0xE0;
            multi_bgra[i * 4 + 1] = 0x80;
            multi_bgra[i * 4 + 2] = 0x10;
            multi_bgra[i * 4 + 3] = 0xFF;
        }
    }
    waywal_palette_t pal;
    assert(waywal_palette_extract_seed_ex(multi_bgra, 16, 16, 16 * 4, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_TONAL_SPOT, 0.0f, 255U, true, 0x123456U,
                                          &pal));
    assert(pal.seeds[0] == 0x123456U);
    assert(pal.source_color == 0x123456U);

    /* Self-aliasing waywal_palette_derive_opposite(&pal_self, &pal_self) */
    waywal_palette_t pal_self = pal;
    waywal_palette_t pal_sep;
    waywal_palette_derive_opposite(&pal, &pal_sep);
    waywal_palette_derive_opposite(&pal_self, &pal_self);
    assert(pal_self.mode == WAYWAL_THEME_LIGHT);
    assert(pal_self.primary.r == pal_sep.primary.r && pal_self.primary.g == pal_sep.primary.g &&
           pal_self.primary.b == pal_sep.primary.b);

    /* Zero-seed fallback with MONOCHROME scheme */
    waywal_palette_t pal_zero_mono;
    memset(&pal_zero_mono, 0, sizeof(pal_zero_mono));
    pal_zero_mono.mode = WAYWAL_THEME_DARK;
    pal_zero_mono.variant = WAYWAL_SCHEME_MONOCHROME;
    waywal_palette_derive_opposite(&pal_zero_mono, &pal_zero_mono);
    assert(pal_zero_mono.primary.r == pal_zero_mono.primary.g &&
           pal_zero_mono.primary.g == pal_zero_mono.primary.b);

    /* NULL pixel buffer with has_seed_override = true and MONOCHROME */
    waywal_palette_t pal_null_px;
    assert(waywal_palette_extract_seed_ex(NULL, 0, 0, 0, true, WAYWAL_THEME_DARK,
                                          WAYWAL_SCHEME_MONOCHROME, 0.0f, 0U, true, 0xFF0055U,
                                          &pal_null_px));
    assert(pal_null_px.primary.r == pal_null_px.primary.g &&
           pal_null_px.primary.g == pal_null_px.primary.b);

    /* 5. Template scanner escapes (\{{, \{), ${var} shell protection, {background}}, 8-digit hex
     * alpha, 3-digit hex, and strict parentheses */
    char tpl_in[640], tpl_out[640];
    snprintf(tpl_in, sizeof(tpl_in), "%s/sec_tpl.in", tmp_dir);
    snprintf(tpl_out, sizeof(tpl_out), "%s/sub/sec_tpl.out", tmp_dir);
    FILE *fp = fopen(tpl_in, "wb");
    assert(fp != NULL);
    fputs("esc_double=\\{{colors.primary.default.hex}}\n"
          "esc_single=\\{background}\n"
          "shell_var=${HOME:-/tmp}\n"
          "compact_css=.box{color:{background}}\n"
          "hex8_rgba={{ #ff000080 | rgba }}\n"
          "hex3={{ #f80 | hex }}\n"
          "bad_tone={{ palettes.primary.40x.hex }}\n"
          "bad_parens={{ (colors.primary.default) | (lighten(0.1)) }}\n",
          fp);
    fclose(fp);

    assert(waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, tpl_out));
    assert_file_contains(tpl_out, "esc_double={{colors.primary.default.hex}}\n");
    assert_file_contains(tpl_out, "esc_single={background}\n");
    assert_file_contains(tpl_out, "shell_var=${HOME:-/tmp}\n");
    char expected_css[128];
    snprintf(expected_css, sizeof(expected_css), "compact_css=.box{color:#%02x%02x%02x}\n",
             pal.background.r, pal.background.g, pal.background.b);
    assert_file_contains(tpl_out, expected_css);
    assert_file_contains(tpl_out, "hex8_rgba=rgba(255, 0, 0, 0.50)\n");
    assert_file_contains(tpl_out, "hex3=#ff8800\n");
    assert_file_contains(tpl_out, "bad_tone={{ palettes.primary.40x.hex }}\n");
    assert_file_contains(tpl_out, "bad_parens={{ (colors.primary.default) | (lighten(0.1)) }}\n");

    /* 6. Unsafe template target rejection ($HOME/.bashrc, $HOME/.config/../.profile, /etc/passwd,
     * self-clobber, and symlinks pointing to sensitive targets or input_path) */
    char unsafe_bashrc[640], unsafe_traversal[640];
    snprintf(unsafe_bashrc, sizeof(unsafe_bashrc), "%s/.bashrc", tmp_dir);
    snprintf(unsafe_traversal, sizeof(unsafe_traversal), "%s/.config/../.profile", tmp_dir);
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, unsafe_bashrc));
    assert(access(unsafe_bashrc, F_OK) != 0);
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, unsafe_traversal));
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, "/etc/passwd"));
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, tpl_in));

    char sym_to_bashrc[680], ssh_dir[640], sym_to_ssh_dir[680], sym_ssh_target[720],
        sym_to_input[680];
    snprintf(sym_to_bashrc, sizeof(sym_to_bashrc), "%s/sub/link_bashrc.conf", tmp_dir);
    unlink(sym_to_bashrc);
    assert(symlink(unsafe_bashrc, sym_to_bashrc) == 0);
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, sym_to_bashrc));
    assert(access(unsafe_bashrc, F_OK) != 0);

    snprintf(ssh_dir, sizeof(ssh_dir), "%s/.ssh", tmp_dir);
    mkdir(ssh_dir, 0700);
    snprintf(sym_to_ssh_dir, sizeof(sym_to_ssh_dir), "%s/sub/ssh_link", tmp_dir);
    unlink(sym_to_ssh_dir);
    assert(symlink(ssh_dir, sym_to_ssh_dir) == 0);
    snprintf(sym_ssh_target, sizeof(sym_ssh_target), "%s/authorized_keys", sym_to_ssh_dir);
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, sym_ssh_target));

    snprintf(sym_to_input, sizeof(sym_to_input), "%s/sub/link_input.conf", tmp_dir);
    unlink(sym_to_input);
    assert(symlink(tpl_in, sym_to_input) == 0);
    assert(!waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, sym_to_input));

    /* 7. Symlink preservation in atomic_write_file (both existing and non-existent target leaf) */
    char real_target_dir[640], real_target[680], sym_target[680];
    snprintf(real_target_dir, sizeof(real_target_dir), "%s/real_dir", tmp_dir);
    mkdir(real_target_dir, 0755);
    snprintf(real_target, sizeof(real_target), "%s/target.conf", real_target_dir);
    fp = fopen(real_target, "wb");
    assert(fp != NULL);
    fputs("initial", fp);
    fclose(fp);
    snprintf(sym_target, sizeof(sym_target), "%s/sub/symlink.conf", tmp_dir);
    unlink(sym_target);
    assert(symlink(real_target, sym_target) == 0);
    assert(waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, sym_target));
    struct stat st_sym;
    assert(lstat(sym_target, &st_sym) == 0);
    assert(S_ISLNK(st_sym.st_mode));
    assert_file_contains(real_target, "hex3=#ff8800");

    char real_new_target[680], sym_new_target[680];
    snprintf(real_new_target, sizeof(real_new_target), "%s/new_target.conf", real_target_dir);
    snprintf(sym_new_target, sizeof(sym_new_target), "%s/sub/symlink_new.conf", tmp_dir);
    unlink(real_new_target);
    unlink(sym_new_target);
    assert(symlink(real_new_target, sym_new_target) == 0);
    assert(waywal_palette_render_template_file(&pal, &pal, &pal_sep, tpl_in, sym_new_target));
    assert(lstat(sym_new_target, &st_sym) == 0);
    assert(S_ISLNK(st_sym.st_mode));
    assert_file_contains(real_new_target, "hex3=#ff8800");

    /* 8. Pywal export escaping of quotes, single quotes, and backslashes in wallpaper_path */
    waywal_palette_t pal_esc = pal;
    snprintf(pal_esc.wallpaper_path, sizeof(pal_esc.wallpaper_path),
             "/tmp/wall\"quote'single\\slash.png");
    assert(waywal_palette_export_pywal(&pal_esc, NULL));
    char pywal_json[640], pywal_sh[640], pywal_css[640];
    snprintf(pywal_json, sizeof(pywal_json), "%s/wal/colors.json", xcache_dir);
    snprintf(pywal_sh, sizeof(pywal_sh), "%s/wal/colors.sh", xcache_dir);
    snprintf(pywal_css, sizeof(pywal_css), "%s/wal/colors.css", xcache_dir);
    assert_file_contains(pywal_json, "\"wallpaper\": \"/tmp/wall\\\"quote'single\\\\slash.png\"");
    assert_file_contains(pywal_css, "url(\"/tmp/wall\\\"quote'single\\\\slash.png\")");
    assert_file_contains(pywal_sh, "wallpaper='/tmp/wall\"quote'\\''single\\slash.png'");

    /* 9. Unified hex color parsing in CLI & TOML + has_theme_mode flag tracking */
    cli_options_t cli_opts;
    char *argv_mode_fit[] = {"wwal", "img", "/tmp/x.png", "--mode", "fit", "--seed-color", "#f80"};
    assert(cli_parse(7, argv_mode_fit, &cli_opts));
    assert(!cli_opts.has_theme_mode);
    assert(cli_opts.has_seed_color && cli_opts.seed_color == 0xFF8800U);

    char *argv_mode_light[] = {"wwal", "img", "0x336699", "--mode", "light", "--theme"};
    assert(cli_parse(6, argv_mode_light, &cli_opts));
    assert(cli_opts.has_theme_mode && cli_opts.theme_light_mode);
    assert(cli_opts.extract_theme);

    char toml_path[640];
    snprintf(toml_path, sizeof(toml_path), "%s/sec_config.toml", tmp_dir);
    fp = fopen(toml_path, "wb");
    assert(fp != NULL);
    fputs("[daemon]\nbackdrop = \"~/my_backdrop.png\"\nworkshop_dir = \"$HOME/ws\"\n"
          "[theme]\nseed_color = \"#f80\"\n"
          "[output.DP-1]\npath = \"~/wall.png\"\n",
          fp);
    fclose(fp);

    waywal_config_t sec_cfg;
    waywal_config_init_defaults(&sec_cfg);
    assert(waywal_config_load_file(toml_path, &sec_cfg, NULL, 0));
    assert(sec_cfg.theme.has_seed_color && sec_cfg.theme.seed_color == 0xFF8800U);
    char expected_bd[640];
    snprintf(expected_bd, sizeof(expected_bd), "%s/my_backdrop.png", tmp_dir);
    assert(strcmp(sec_cfg.daemon.backdrop, expected_bd) == 0);
    snprintf(expected_bd, sizeof(expected_bd), "%s/wall.png", tmp_dir);
    assert(sec_cfg.output_count == 1 && strcmp(sec_cfg.outputs[0].path, expected_bd) == 0);

    if (old_home[0])
        setenv("HOME", old_home, 1);
    if (old_xcache[0])
        setenv("XDG_CACHE_HOME", old_xcache, 1);
    else
        unsetenv("XDG_CACHE_HOME");

    printf("  -> Template Security & Palette Edge Cases: PASSED\n");
}

static void write_dummy_file(const char *path, const char *content, time_t mtime_sec)
{
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    if (content && content[0]) {
        fputs(content, fp);
    } else {
        fputc('x', fp);
    }
    fclose(fp);
    if (mtime_sec > 0) {
        struct timespec ts[2] = {
            {.tv_sec = mtime_sec, .tv_nsec = 0},
            {.tv_sec = mtime_sec, .tv_nsec = 0},
        };
        assert(utimensat(AT_FDCWD, path, ts, 0) == 0);
    }
}

static void test_slideshow_and_wpaperd(void)
{
    printf("[TEST] Testing wpaperd Superset, Mixed-Media Slideshows & Workspace Rules...\n");

    /* 1. Human-readable duration string parser */
    uint32_t secs = 0;
    assert(waywal_parse_duration_seconds("300", &secs) && secs == 300U);
    assert(waywal_parse_duration_seconds("30s", &secs) && secs == 30U);
    assert(waywal_parse_duration_seconds("15m", &secs) && secs == 900U);
    assert(waywal_parse_duration_seconds("2h", &secs) && secs == 7200U);
    assert(waywal_parse_duration_seconds("1h30m", &secs) && secs == 5400U);
    assert(waywal_parse_duration_seconds("1d", &secs) && secs == 86400U);
    assert(waywal_parse_duration_seconds("1m 30s", &secs) && secs == 90U);
    assert(waywal_parse_duration_seconds("2h 15m 10s", &secs) && secs == 8110U);
    assert(!waywal_parse_duration_seconds("", &secs));
    assert(!waywal_parse_duration_seconds("0", &secs));
    assert(!waywal_parse_duration_seconds("0s", &secs));
    assert(!waywal_parse_duration_seconds("-5m", &secs));
    assert(!waywal_parse_duration_seconds("15x", &secs));
    assert(!waywal_parse_duration_seconds("9999999999d", &secs));

    /* 2. Sort mode parser */
    waywal_sort_mode_t sm = WAYWAL_SORT_RANDOM;
    assert(waywal_parse_sort_mode("ascending", &sm) && sm == WAYWAL_SORT_ASCENDING);
    assert(waywal_parse_sort_mode("descending", &sm) && sm == WAYWAL_SORT_DESCENDING);
    assert(waywal_parse_sort_mode("mtime", &sm) && sm == WAYWAL_SORT_MTIME);
    assert(waywal_parse_sort_mode("random", &sm) && sm == WAYWAL_SORT_RANDOM);
    assert(!waywal_parse_sort_mode("invalid_sort", &sm));

    /* 3. Create temp directory with mixed media for wpaperd auto-dir & slideshow engine tests */
    char tmp_dir[] = "/tmp/wwal_test_ss_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char p_a[600], p_b[600], p_c[600], p_d[600], p_ws[600], p_ws_json[640], p_ws_mat[640],
        p_ws_nested[680], p_sub[640], p_sub_img[680];
    snprintf(p_a, sizeof(p_a), "%s/a_first.jpg", tmp_dir);
    snprintf(p_b, sizeof(p_b), "%s/b_second.png", tmp_dir);
    snprintf(p_c, sizeof(p_c), "%s/c_third.mp4", tmp_dir);
    snprintf(p_d, sizeof(p_d), "%s/d_fourth.frag", tmp_dir);
    snprintf(p_ws, sizeof(p_ws), "%s/e_workshop_scene", tmp_dir);
    snprintf(p_ws_json, sizeof(p_ws_json), "%s/project.json", p_ws);
    snprintf(p_ws_mat, sizeof(p_ws_mat), "%s/materials", p_ws);
    snprintf(p_ws_nested, sizeof(p_ws_nested), "%s/internal_tex.png", p_ws_mat);
    snprintf(p_sub, sizeof(p_sub), "%s/f_subdir", tmp_dir);
    snprintf(p_sub_img, sizeof(p_sub_img), "%s/sub_wall.png", p_sub);

    assert(mkdir(p_ws, 0755) == 0);
    assert(mkdir(p_ws_mat, 0755) == 0);
    assert(mkdir(p_sub, 0755) == 0);
    write_dummy_file(p_a, "jpg", 1000);
    write_dummy_file(p_b, "png", 3000);
    write_dummy_file(p_c, "mp4", 5000);
    write_dummy_file(p_d, "void mainImage(out vec4 c, in vec2 u){c=vec4(1.0);}", 2000);
    write_dummy_file(p_ws_json, "{\"type\":\"scene\",\"file\":\"scene.json\"}", 4000);
    write_dummy_file(p_ws_nested, "nested_tex", 6000);
    write_dummy_file(p_sub_img, "sub_png", 500);
    struct timespec ws_ts[2] = {{.tv_sec = 4000, .tv_nsec = 0}, {.tv_sec = 4000, .tv_nsec = 0}};
    assert(utimensat(AT_FDCWD, p_ws, ws_ts, 0) == 0);

    char p_git[640], p_git_img[680];
    snprintf(p_git, sizeof(p_git), "%s/.git", tmp_dir);
    snprintf(p_git_img, sizeof(p_git_img), "%s/ignored.png", p_git);
    assert(mkdir(p_git, 0755) == 0);
    write_dummy_file(p_git_img, "hidden", 7000);

    /* 4. wpaperd superset TOML parsing ([default], [any], [DP-1], [output.*], [workspace.*]) */
    char toml_ss[4096];
    snprintf(toml_ss, sizeof(toml_ss),
             "[general]\n"
             "duration = 15m\n"
             "sorting = \"ascending\"\n"
             "recursive = true\n"
             "idle_timeout_sec = 0\n"
             "\n"
             "[default]\n"
             "path = \"%s\"\n"
             "duration = \"1h30m\"\n"
             "mode = \"fit-border-color\"\n"
             "sorting = \"mtime\"\n"
             "recursive = false\n"
             "\n"
             "[DP-1]\n"
             "path = \"%s\"\n"
             "mode = \"fit\"\n"
             "\n"
             "[HDMI-A-1]\n"
             "color = \"#1e1e2e\"\n"
             "\n"
             "[output.\"eDP-2\"]\n"
             "path = \"%s\"\n"
             "shuffle = false\n"
             "\n"
             "[DP-3]\n"
             "fill_color = \"#112233\"\n"
             "\n"
             "[workspace.\"1\"]\n"
             "path = \"%s\"\n"
             "transition = \"slide\"\n"
             "transition_duration = \"400ms \"\n"
             "transition_angle = 180\n"
             "\n"
             "[workspace.\"2\"]\n"
             "path = \"%s\"\n"
             "\n"
             "[workspace.\"3\"]\n"
             "path = \"%s\"\n"
             "\n"
             "[workspace.\"4\"]\n"
             "color = \"#336699\"\n",
             tmp_dir, p_d, p_b, p_c, p_d, tmp_dir);

    waywal_config_t cfg;
    waywal_config_init_defaults(&cfg);
    assert(waywal_config_parse_string(toml_ss, &cfg, NULL, 0));
    assert(cfg.daemon.slideshow_interval == 900U);
    assert(cfg.daemon.slideshow_sort == WAYWAL_SORT_ASCENDING);
    assert(cfg.daemon.idle_timeout_sec == 0U);
    assert(cfg.output_count == 5U);

    /* [default] mapped to "*" with auto-detected directory -> slideshow, recursive = false */
    const waywal_output_rule_t *r_def = waywal_config_match_output(&cfg, "eDP-1");
    assert(r_def != NULL);
    assert(strcmp(r_def->output_name, "*") == 0);
    assert(strcmp(r_def->mode, "slideshow") == 0);
    assert(r_def->interval == 5400U && r_def->has_interval);
    assert(r_def->scaling == WAYWAL_SCALING_FIT && r_def->has_scaling);
    assert(r_def->sort_mode == WAYWAL_SORT_MTIME && r_def->has_sort_mode);
    assert(r_def->has_recursive && r_def->recursive == false);

    /* Native wpaperd top-level [DP-1] section overrides [default] and inherits interval/sort/recursive */
    const waywal_output_rule_t *r_dp1 = waywal_config_match_output(&cfg, "DP-1");
    assert(r_dp1 != NULL);
    assert(strcmp(r_dp1->output_name, "DP-1") == 0);
    assert(strcmp(r_dp1->mode, "shader") == 0);
    assert(r_dp1->scaling == WAYWAL_SCALING_FIT && r_dp1->has_scaling);
    assert(r_dp1->interval == 5400U && r_dp1->has_interval);
    assert(r_dp1->sort_mode == WAYWAL_SORT_MTIME && r_dp1->has_sort_mode);
    assert(r_dp1->has_recursive && r_dp1->recursive == false);

    /* Image path with shuffle = false and inherited interval stays mode = "image" */
    const waywal_output_rule_t *r_edp2 = waywal_config_match_output(&cfg, "eDP-2");
    assert(r_edp2 != NULL);
    assert(strcmp(r_edp2->output_name, "eDP-2") == 0);
    assert(strcmp(r_edp2->mode, "image") == 0);
    assert(r_edp2->scaling == WAYWAL_SCALING_FIT && r_edp2->has_scaling);

    /* Standalone [HDMI-A-1] color = "#1e1e2e" becomes mode = "color" */
    const waywal_output_rule_t *r_hdmi = waywal_config_match_output(&cfg, "HDMI-A-1");
    assert(r_hdmi != NULL);
    assert(strcmp(r_hdmi->mode, "color") == 0);
    assert(r_hdmi->has_color && r_hdmi->fill_color == 0x1E1E2EFFU);

    /* [DP-3] fill_color = "#112233" inherits path & mode from [default] instead of becoming color */
    const waywal_output_rule_t *r_dp3 = waywal_config_match_output(&cfg, "DP-3");
    assert(r_dp3 != NULL);
    assert(strcmp(r_dp3->output_name, "DP-3") == 0);
    assert(strcmp(r_dp3->path, tmp_dir) == 0);
    assert(strcmp(r_dp3->mode, "slideshow") == 0);
    assert(r_dp3->has_fill_color && r_dp3->fill_color == 0x112233FFU);

    /* Workspace rules auto-inferred modes & transition overrides */
    assert(cfg.workspace_count == 4U);
    assert(strcmp(cfg.workspaces[0].workspace_name, "1") == 0);
    assert(strcmp(cfg.workspaces[0].mode, "video") == 0);
    assert(cfg.workspaces[0].has_transition &&
           cfg.workspaces[0].transition_type_enum == WAYWAL_TRANSITION_SLIDE);
    assert(cfg.workspaces[0].has_transition_duration &&
           fabsf(cfg.workspaces[0].transition_duration - 0.4f) < 1e-4f);
    assert(cfg.workspaces[0].has_transition_angle &&
           fabsf(cfg.workspaces[0].transition_angle - 180.0f) < 1e-4f);

    assert(strcmp(cfg.workspaces[1].workspace_name, "2") == 0);
    assert(strcmp(cfg.workspaces[1].mode, "shader") == 0);

    assert(strcmp(cfg.workspaces[2].workspace_name, "3") == 0);
    assert(strcmp(cfg.workspaces[2].mode, "slideshow") == 0);
    assert(cfg.workspaces[2].has_recursive && cfg.workspaces[2].recursive == false);

    assert(strcmp(cfg.workspaces[3].workspace_name, "4") == 0);
    assert(strcmp(cfg.workspaces[3].mode, "color") == 0);
    assert(cfg.workspaces[3].has_color && cfg.workspaces[3].fill_color == 0x336699FFU);

    /* 4b. Validate rejection of negative/NaN/Inf/overflow float durations and 0 slideshow interval */
    waywal_config_t bad_dur_cfg;
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[workspace.\"1\"]\npath = \"/tmp/a.png\"\ntransition_duration = -5\n",
                                       &bad_dur_cfg, NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[workspace.\"1\"]\npath = \"/tmp/a.png\"\ntransition_duration = nan\n",
                                       &bad_dur_cfg, NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[transition]\nduration = -1.0\n", &bad_dur_cfg, NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[transition]\nduration = inf\n", &bad_dur_cfg, NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[default]\npath = \"/tmp\"\nduration = 0\n", &bad_dur_cfg,
                                       NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[default]\npath = \"/tmp\"\nduration = \"0\"\n",
                                       &bad_dur_cfg, NULL, 0));
    waywal_config_init_defaults(&bad_dur_cfg);
    assert(!waywal_config_parse_string("[template]\ninput = \"/tmp/a\"\n", &bad_dur_cfg, NULL, 0));

    /* Verify built-in shader preset names in [output.*] / [workspace.*] auto-infer mode = "shader" */
    waywal_config_t preset_mode_cfg;
    waywal_config_init_defaults(&preset_mode_cfg);
    assert(waywal_config_parse_string("[DP-1]\npath = \"aurora\"\n[workspace.\"2\"]\npath = \"preset:cyber_grid\"\n",
                                      &preset_mode_cfg, NULL, 0));
    assert(preset_mode_cfg.output_count == 1U &&
           strcmp(preset_mode_cfg.outputs[0].mode, "shader") == 0);
    assert(preset_mode_cfg.workspace_count == 1U &&
           strcmp(preset_mode_cfg.workspaces[0].mode, "shader") == 0);

    /* 5. Mixed-media slideshow scanner & sorting modes */
    slideshow_engine_t ss;
    assert(slideshow_init(&ss, NULL));

    /* Control actions on inactive slideshow must return false (except STOP) */
    assert(!slideshow_control(&ss, WAYWAL_SLIDESHOW_NEXT));
    assert(!slideshow_control(&ss, WAYWAL_SLIDESHOW_PREV));
    assert(!slideshow_control(&ss, WAYWAL_SLIDESHOW_PAUSE));
    assert(!slideshow_control(&ss, WAYWAL_SLIDESHOW_RESUME));
    assert(!slideshow_control(&ss, WAYWAL_SLIDESHOW_TOGGLE));
    assert(slideshow_control(&ss, WAYWAL_SLIDESHOW_STOP));

    waywal_slideshow_payload_t spay;
    memset(&spay, 0, sizeof(spay));
    strncpy(spay.path, tmp_dir, sizeof(spay.path) - 1);
    spay.interval_s = 60;
    spay.has_sort_mode = 1;
    spay.auto_day_night = 1;

    /* 5a. Non-recursive scan (no_recursive = 1): skips f_subdir, keeps top-level files + Workshop dir */
    spay.no_recursive = 1;
    spay.sort_mode = WAYWAL_SORT_ASCENDING;
    assert(slideshow_start(&ss, &spay));
    assert(ss.auto_day_night == 1U);
    assert(ss.num_files == 5U);
    assert(strcmp(ss.file_list[0], p_a) == 0);
    assert(strcmp(ss.file_list[1], p_b) == 0);
    assert(strcmp(ss.file_list[2], p_c) == 0);
    assert(strcmp(ss.file_list[3], p_d) == 0);
    assert(strcmp(ss.file_list[4], p_ws) == 0);

    /* 5b. Recursive scan (no_recursive = 0): includes f_subdir/sub_wall.png, skips Workshop internals & .git */
    spay.no_recursive = 0;
    spay.sort_mode = WAYWAL_SORT_ASCENDING;
    assert(slideshow_start(&ss, &spay));
    assert(ss.num_files == 6U);
    assert(strcmp(ss.file_list[0], p_a) == 0);
    assert(strcmp(ss.file_list[1], p_b) == 0);
    assert(strcmp(ss.file_list[2], p_c) == 0);
    assert(strcmp(ss.file_list[3], p_d) == 0);
    assert(strcmp(ss.file_list[4], p_ws) == 0);
    assert(strcmp(ss.file_list[5], p_sub_img) == 0);

    /* 5c. Descending sort (Z-A), including when has_sort_mode is omitted (0) */
    spay.no_recursive = 1;
    spay.has_sort_mode = 0;
    spay.sort_mode = WAYWAL_SORT_DESCENDING;
    assert(slideshow_start(&ss, &spay));
    assert(ss.num_files == 5U);
    assert(strcmp(ss.file_list[0], p_ws) == 0);
    assert(strcmp(ss.file_list[1], p_d) == 0);
    assert(strcmp(ss.file_list[2], p_c) == 0);
    assert(strcmp(ss.file_list[3], p_b) == 0);
    assert(strcmp(ss.file_list[4], p_a) == 0);

    /* 5d. Mtime sort (newest first: p_c=5000, p_ws=4000, p_b=3000, p_d=2000, p_a=1000) */
    spay.has_sort_mode = 1;
    spay.sort_mode = WAYWAL_SORT_MTIME;
    assert(slideshow_start(&ss, &spay));
    assert(ss.num_files == 5U);
    assert(strcmp(ss.file_list[0], p_c) == 0);
    assert(strcmp(ss.file_list[1], p_ws) == 0);
    assert(strcmp(ss.file_list[2], p_b) == 0);
    assert(strcmp(ss.file_list[3], p_d) == 0);
    assert(strcmp(ss.file_list[4], p_a) == 0);

    /* 5e. Fisher-Yates non-repeating random shuffle across 50 full cycles */
    spay.sort_mode = WAYWAL_SORT_RANDOM;
    spay.random_order = true;
    assert(slideshow_start(&ss, &spay));
    assert(ss.num_files == 5U);
    char prev_item[4096];
    strncpy(prev_item, ss.file_list[ss.current_file_idx], sizeof(prev_item) - 1);
    prev_item[sizeof(prev_item) - 1] = '\0';
    for (size_t cycle = 0; cycle < 50; ++cycle) {
        for (size_t step = 0; step < 5; ++step) {
            assert(slideshow_control(&ss, WAYWAL_SLIDESHOW_NEXT));
            const char *cur_item = ss.file_list[ss.current_file_idx];
            /* Non-repeating invariant: consecutive items (including across cycle wrap) never match */
            assert(strcmp(cur_item, prev_item) != 0);
            strncpy(prev_item, cur_item, sizeof(prev_item) - 1);
            prev_item[sizeof(prev_item) - 1] = '\0';
        }
    }
    slideshow_destroy(&ss);

    /* 6. CLI parsing for wwal next, wwal prev, --duration, --sort (flags before & after path) */
    cli_options_t opts;
    char *argv_next[] = {"wwal", "next", "-o", "DP-1"};
    assert(cli_parse(4, argv_next, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW_CTRL);
    assert(opts.slideshow_action == WAYWAL_SLIDESHOW_NEXT);
    assert(opts.num_outputs == 1 && strcmp(opts.outputs[0], "DP-1") == 0);

    char *argv_prev[] = {"wwal", "prev", "--output", "HDMI-A-1"};
    assert(cli_parse(4, argv_prev, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW_CTRL);
    assert(opts.slideshow_action == WAYWAL_SLIDESHOW_PREV);
    assert(opts.num_outputs == 1 && strcmp(opts.outputs[0], "HDMI-A-1") == 0);

    char *argv_ss[] = {"wwal", "slideshow", tmp_dir, "--duration", "1h30m", "--sort", "mtime"};
    assert(cli_parse(7, argv_ss, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW);
    assert(strcmp(opts.filepath, tmp_dir) == 0);
    assert(opts.slideshow_interval_s == 5400U);
    assert(opts.has_slideshow_sort && opts.slideshow_sort == WAYWAL_SORT_MTIME);

    char *argv_ss_flags_first[] = {"wwal",   "slideshow",    "--duration", "10m",
                                   "--sort", "descending",   tmp_dir};
    assert(cli_parse(7, argv_ss_flags_first, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW);
    assert(strcmp(opts.filepath, tmp_dir) == 0);
    assert(opts.slideshow_interval_s == 600U);
    assert(opts.has_slideshow_sort && opts.slideshow_sort == WAYWAL_SORT_DESCENDING);

    char *argv_ss_dir_flag[] = {"wwal", "slideshow", "--dir", tmp_dir, "-d", "45s"};
    assert(cli_parse(6, argv_ss_dir_flag, &opts));
    assert(opts.cmd == CLI_CMD_SLIDESHOW);
    assert(strcmp(opts.filepath, tmp_dir) == 0);
    assert(opts.slideshow_interval_s == 45U);

    char *argv_bad_tdur[] = {"wwal", "img", p_a, "--transition-duration", "-2.5"};
    assert(!cli_parse(5, argv_bad_tdur, &opts));
    char *argv_nan_tdur[] = {"wwal", "img", p_a, "--transition-duration", "nan"};
    assert(!cli_parse(5, argv_nan_tdur, &opts));

    setenv("AWWW_TRANSITION_DURATION", "invalid", 1);
    char *argv_env_tdur[] = {"wwal", "img", p_a};
    assert(cli_parse(3, argv_env_tdur, &opts));
    assert(!opts.has_transition_duration);
    assert(fabsf(opts.transition_duration - 1.0f) < 1e-4f);
    unsetenv("AWWW_TRANSITION_DURATION");

    /* Cleanup temp directory */
    unlink(p_git_img);
    rmdir(p_git);
    unlink(p_sub_img);
    rmdir(p_sub);
    unlink(p_ws_nested);
    rmdir(p_ws_mat);
    unlink(p_ws_json);
    rmdir(p_ws);
    unlink(p_a);
    unlink(p_b);
    unlink(p_c);
    unlink(p_d);
    rmdir(tmp_dir);

    printf("  -> wpaperd Superset, Mixed-Media Slideshows & Workspace Rules: PASSED\n");
}

static void test_solar_and_dynamic_wallpapers(void)
{
    printf("[TEST] Solar Schedule, GNOME XML & JSON Dynamic Wallpapers...\n");

    /* 1. Astronomical solar calculator: NYC (40.7128, -74.0060) on summer & winter solstices */
    struct tm tm_summer = {0};
    tm_summer.tm_year = 2026 - 1900;
    tm_summer.tm_mon = 6 - 1;
    tm_summer.tm_mday = 21;
    tm_summer.tm_hour = 17; /* ~13:00 EDT (solar noon in NYC is ~16:58 UTC) */
    tm_summer.tm_min = 0;
    tm_summer.tm_sec = 0;
    time_t utc_summer_noon = timegm(&tm_summer);

    waywal_solar_state_t nyc_summer =
        waywal_solar_calculate(40.7128, -74.0060, utc_summer_noon);
    assert(isfinite(nyc_summer.elevation_deg));
    assert(isfinite(nyc_summer.azimuth_deg));
    assert(nyc_summer.sunrise_utc > 0 && nyc_summer.sunset_utc > nyc_summer.sunrise_utc);
    double summer_day_hours =
        (double)(nyc_summer.sunset_utc - nyc_summer.sunrise_utc) / 3600.0;
    assert(summer_day_hours > 14.0 && summer_day_hours < 16.0);
    assert(nyc_summer.elevation_deg > 65.0);
    assert(nyc_summer.is_daytime);
    assert(nyc_summer.phase == WAYWAL_SOLAR_DAY);

    struct tm tm_winter = {0};
    tm_winter.tm_year = 2026 - 1900;
    tm_winter.tm_mon = 12 - 1;
    tm_winter.tm_mday = 21;
    tm_winter.tm_hour = 17; /* ~12:00 EST */
    tm_winter.tm_min = 0;
    tm_winter.tm_sec = 0;
    time_t utc_winter_noon = timegm(&tm_winter);

    waywal_solar_state_t nyc_winter =
        waywal_solar_calculate(40.7128, -74.0060, utc_winter_noon);
    assert(isfinite(nyc_winter.elevation_deg));
    assert(nyc_winter.sunrise_utc > 0 && nyc_winter.sunset_utc > nyc_winter.sunrise_utc);
    double winter_day_hours =
        (double)(nyc_winter.sunset_utc - nyc_winter.sunrise_utc) / 3600.0;
    assert(winter_day_hours > 8.5 && winter_day_hours < 10.5);
    assert(summer_day_hours > winter_day_hours + 4.5);
    assert(nyc_winter.elevation_deg > 20.0 && nyc_winter.elevation_deg < 35.0);
    assert(nyc_winter.is_daytime);
    assert(nyc_winter.phase == WAYWAL_SOLAR_DAY);

    /* Verify midnight in NYC (05:00 UTC) is NIGHT with negative elevation */
    time_t utc_summer_midnight = utc_summer_noon - 12 * 3600;
    waywal_solar_state_t nyc_midnight =
        waywal_solar_calculate(40.7128, -74.0060, utc_summer_midnight);
    assert(nyc_midnight.elevation_deg < -15.0);
    assert(!nyc_midnight.is_daytime);
    assert(nyc_midnight.phase == WAYWAL_SOLAR_NIGHT);

    /* Verify DAWN and DUSK phase transitions around sunrise and sunset */
    waywal_solar_state_t nyc_dawn =
        waywal_solar_calculate(40.7128, -74.0060, nyc_summer.sunrise_utc);
    assert(nyc_dawn.phase == WAYWAL_SOLAR_DAWN);
    waywal_solar_state_t nyc_dusk =
        waywal_solar_calculate(40.7128, -74.0060, nyc_summer.sunset_utc);
    assert(nyc_dusk.phase == WAYWAL_SOLAR_DUSK);

    /* Polar edge cases: lat = +89.0 and Longyearbyen (+78.22, 15.65) on summer/winter solstices */
    waywal_solar_state_t polar_day = waywal_solar_calculate(89.0, 0.0, utc_summer_noon);
    assert(isfinite(polar_day.elevation_deg) && isfinite(polar_day.azimuth_deg));
    assert(polar_day.elevation_deg > 10.0);
    assert(polar_day.is_daytime);
    assert(polar_day.phase == WAYWAL_SOLAR_DAY);

    waywal_solar_state_t polar_night = waywal_solar_calculate(89.0, 0.0, utc_winter_noon);
    assert(isfinite(polar_night.elevation_deg) && isfinite(polar_night.azimuth_deg));
    assert(polar_night.elevation_deg < -10.0);
    assert(!polar_night.is_daytime);
    assert(polar_night.phase == WAYWAL_SOLAR_NIGHT);

    waywal_solar_state_t svalbard_summer =
        waywal_solar_calculate(78.22, 15.65, utc_summer_noon);
    assert(svalbard_summer.is_daytime && svalbard_summer.phase == WAYWAL_SOLAR_DAY);
    waywal_solar_state_t svalbard_winter =
        waywal_solar_calculate(78.22, 15.65, utc_winter_noon);
    assert(!svalbard_winter.is_daytime && svalbard_winter.phase == WAYWAL_SOLAR_NIGHT);

    /* Vernal equinox at (0.0, 0.0) around 12:00 UTC: sun near zenith (~90 deg) */
    struct tm tm_equinox = {0};
    tm_equinox.tm_year = 2026 - 1900;
    tm_equinox.tm_mon = 3 - 1;
    tm_equinox.tm_mday = 20;
    tm_equinox.tm_hour = 12;
    time_t utc_equinox_noon = timegm(&tm_equinox);
    waywal_solar_state_t eq_noon = waywal_solar_calculate(0.0, 0.0, utc_equinox_noon);
    assert(eq_noon.elevation_deg > 85.0 && eq_noon.is_daytime);
    double eq_day_hours = (double)(eq_noon.sunset_utc - eq_noon.sunrise_utc) / 3600.0;
    assert(eq_day_hours > 11.8 && eq_day_hours < 12.5);

    /* Western hemisphere across UTC midnight: 2026-06-22 00:15 UTC is 20:15 EDT on 2026-06-21 (before sunset ~00:31 UTC) */
    struct tm tm_nyc_dusk_utc = {0};
    tm_nyc_dusk_utc.tm_year = 2026 - 1900;
    tm_nyc_dusk_utc.tm_mon = 6 - 1;
    tm_nyc_dusk_utc.tm_mday = 22;
    tm_nyc_dusk_utc.tm_hour = 0;
    tm_nyc_dusk_utc.tm_min = 15;
    time_t utc_nyc_evening = timegm(&tm_nyc_dusk_utc);
    waywal_solar_state_t nyc_evening =
        waywal_solar_calculate(40.7128, -74.0060, utc_nyc_evening);
    assert(nyc_evening.is_daytime);
    assert(nyc_evening.sunrise_utc <= utc_nyc_evening &&
           utc_nyc_evening <= nyc_evening.sunset_utc);

    /* 2. GNOME .xml dynamic wallpaper manifest parsing & evaluation */
    const char *gnome_xml =
        "<?xml version=\"1.0\"?>\n"
        "<background>\n"
        "  <!-- Mojave test schedule -->\n"
        "  <static_meta>ignored_prefix_tag.png</static_meta>\n"
        "  <starttime>\n"
        "    <year>2026</year><month>06</month><day>21</day>\n"
        "    <hour>06</hour><minute>00</minute><second>00</second>\n"
        "  </starttime>\n"
        "  <static>\n"
        "    <duration>21600.0</duration>\n"
        "    <file type=\"image\">morning.png</file>\n"
        "  </static>\n"
        "  <transition type=\"overlay\">\n"
        "    <duration>7200.0</duration>\n"
        "    <from>morning.png</from>\n"
        "    <to>afternoon.png</to>\n"
        "  </transition>\n"
        "  <static>\n"
        "    <duration>57600.0</duration>\n"
        "    <file>/var/wallpapers/night.png</file>\n"
        "  </static>\n"
        "</background>\n";

    waywal_dynwall_manifest_t xml_man;
    assert(waywal_dynwall_parse_xml_string(gnome_xml, strlen(gnome_xml),
                                           "/usr/share/backgrounds/mojave", &xml_man));
    assert(xml_man.kind == WAYWAL_DYNWALL_KIND_XML_TIMED);
    assert(xml_man.step_count == 3);
    assert(xml_man.start_tm.tm_hour == 6 && xml_man.start_tm.tm_min == 0 &&
           xml_man.start_tm.tm_sec == 0);
    assert(strcmp(xml_man.steps[0].from_path, "/usr/share/backgrounds/mojave/morning.png") == 0);
    assert(strcmp(xml_man.steps[1].to_path, "/usr/share/backgrounds/mojave/afternoon.png") == 0);
    assert(strcmp(xml_man.steps[2].from_path, "/var/wallpapers/night.png") == 0);

    /* Evaluate at 08:00 local time (inside first static step 06:00..12:00) */
    time_t now_ref = time(NULL);
    struct tm lt;
    localtime_r(&now_ref, &lt);
    lt.tm_hour = 8;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    time_t t_morning = mktime(&lt);
    waywal_dynwall_resolved_t eval_res;
    assert(waywal_dynwall_resolve(&xml_man, t_morning, &lt, NULL, &eval_res));
    assert(!eval_res.is_transition);
    assert(strcmp(eval_res.primary_path, "/usr/share/backgrounds/mojave/morning.png") == 0);
    assert(strcmp(waywal_dynwall_effective_path(&eval_res),
                  "/usr/share/backgrounds/mojave/morning.png") == 0);

    /* Evaluate at 12:30 local time (first half of 12:00..14:00 transition: blend_factor = 0.25) */
    lt.tm_hour = 12;
    lt.tm_min = 30;
    lt.tm_sec = 0;
    time_t t_trans_early = mktime(&lt);
    assert(waywal_dynwall_resolve(&xml_man, t_trans_early, &lt, NULL, &eval_res));
    assert(eval_res.is_transition);
    assert(fabsf(eval_res.blend_factor - 0.25f) < 0.05f);
    assert(strcmp(waywal_dynwall_effective_path(&eval_res),
                  "/usr/share/backgrounds/mojave/morning.png") == 0);
    /* Next change in first half must wake at the midpoint (13:00 -> 1800s away) */
    assert(eval_res.next_change_sec == 1800);

    /* Evaluate at 13:00 local time (midpoint of 12:00..14:00 transition: blend_factor = 0.50 -> to_path) */
    lt.tm_hour = 13;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    time_t t_trans = mktime(&lt);
    assert(waywal_dynwall_resolve(&xml_man, t_trans, &lt, NULL, &eval_res));
    assert(eval_res.is_transition);
    assert(fabsf(eval_res.blend_factor - 0.5f) < 0.05f);
    assert(strcmp(eval_res.from_path, "/usr/share/backgrounds/mojave/morning.png") == 0);
    assert(strcmp(eval_res.to_path, "/usr/share/backgrounds/mojave/afternoon.png") == 0);
    assert(strcmp(waywal_dynwall_effective_path(&eval_res),
                  "/usr/share/backgrounds/mojave/afternoon.png") == 0);
    assert(eval_res.next_change_sec == 3600);

    /* Evaluate at 02:00 local time (wrap-around into night step 14:00..06:00) */
    lt.tm_hour = 2;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    time_t t_night = mktime(&lt);
    assert(waywal_dynwall_resolve(&xml_man, t_night, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/var/wallpapers/night.png") == 0);

    /* Reject path traversal, entity evasion, and env-var traversal in XML manifest */
    const char *bad_xml =
        "<background><static><duration>3600</duration><file>../etc/passwd</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml, strlen(bad_xml), "/usr/share/backgrounds",
                                            &xml_man));
    const char *bad_xml_entity =
        "<background><static><duration>3600</duration><file>&#46;&#46;/etc/passwd</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml_entity, strlen(bad_xml_entity),
                                            "/usr/share/backgrounds", &xml_man));
    setenv("WWAL_TEST_BAD_DIR", "/tmp/../../etc", 1);
    const char *bad_xml_env =
        "<background><static><duration>3600</duration><file>$WWAL_TEST_BAD_DIR/passwd</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml_env, strlen(bad_xml_env),
                                            "/usr/share/backgrounds", &xml_man));
    unsetenv("WWAL_TEST_BAD_DIR");

    /* 3. JSON dynamic wallpaper manifest (both solar-elevation-keyed and time-keyed) */
    const char *solar_json =
        "{\n"
        "  \"type\": \"solar\",\n"
        "  \"frames\": [\n"
        "    { \"file\": \"night_deep.png\", \"elevation\": -25.0 },\n"
        "    { \"file\": \"dawn_glow.png\",  \"elevation\": 0.0 },\n"
        "    { \"file\": \"noon_bright.png\", \"elevation\": 60.0 }\n"
        "  ]\n"
        "}\n";
    waywal_dynwall_manifest_t json_man;
    assert(waywal_dynwall_parse_json_string(solar_json, strlen(solar_json), "/opt/dynwall",
                                            &json_man));
    assert(json_man.kind == WAYWAL_DYNWALL_KIND_JSON_SOLAR);
    assert(json_man.step_count == 3);
    assert(waywal_dynwall_resolve(&json_man, utc_summer_noon, NULL, &nyc_summer, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/noon_bright.png") == 0);
    assert(waywal_dynwall_resolve(&json_man, utc_summer_midnight, NULL, &nyc_midnight, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/night_deep.png") == 0);

    const char *time_json =
        "{\n"
        "  \"frames\": [\n"
        "    { \"file\": \"t_morning.png\", \"time\": \"06:00\" },\n"
        "    { \"file\": \"t_evening.png\", \"time\": \"18:00\" }\n"
        "  ]\n"
        "}\n";
    assert(waywal_dynwall_parse_json_string(time_json, strlen(time_json), "/opt/dynwall",
                                            &json_man));
    assert(json_man.step_count == 2);
    lt.tm_hour = 8;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/t_morning.png") == 0);

    /* Wrap-around midnight in time-keyed JSON manifest (02:00 -> 18:00 frame) */
    lt.tm_hour = 2;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_night, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/t_evening.png") == 0);

    /* Reject empty/malformed XML and JSON manifests and path traversal */
    assert(!waywal_dynwall_parse_xml_string("", 0, "/opt/dynwall", &xml_man));
    assert(!waywal_dynwall_parse_json_string("", 0, "/opt/dynwall", &json_man));
    assert(!waywal_dynwall_parse_json_string("{\"frames\":[]}", 13, "/opt/dynwall", &json_man));
    const char *bad_json =
        "{\"frames\":[{\"file\":\"../../etc/shadow\",\"time\":\"06:00\"}]}";
    assert(!waywal_dynwall_parse_json_string(bad_json, strlen(bad_json), "/opt/dynwall",
                                             &json_man));

    /* 4. [solar] and [schedule] TOML configuration & CLI overrides */
    char tmp_dir[] = "/tmp/wwal_solar_test_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);
    char toml_path[640];
    snprintf(toml_path, sizeof(toml_path), "%s/solar_config.toml", tmp_dir);
    FILE *fp = fopen(toml_path, "wb");
    assert(fp != NULL);
    fputs("[solar]\n"
          "latitude = 40.7128\n"
          "longitude = -74.0060\n"
          "auto_day_night = true\n"
          "sunrise = \"06:15\"\n"
          "sunset = \"19:45\"\n"
          "dawn_path = \"/tmp/dawn.png\"\n"
          "day_path = \"/tmp/day.png\"\n"
          "dusk_path = \"/tmp/dusk.png\"\n"
          "night_path = \"/tmp/night.png\"\n"
          "\n"
          "[theme]\n"
          "mode = \"auto\"\n"
          "\n"
          "[[schedule]]\n"
          "phase = \"dawn\"\n"
          "path = \"/tmp/sched_dawn.png\"\n"
          "mode = \"light\"\n"
          "\n"
          "[schedule.night]\n"
          "time = \"21:30\"\n"
          "path = \"/tmp/sched_night.png\"\n"
          "mode = \"dark\"\n",
          fp);
    fclose(fp);

    waywal_config_t solar_cfg;
    waywal_config_init_defaults(&solar_cfg);
    assert(waywal_config_load_file(toml_path, &solar_cfg, NULL, 0));
    assert(solar_cfg.solar.has_coordinates);
    assert(fabs(solar_cfg.solar.latitude - 40.7128) < 1e-4);
    assert(fabs(solar_cfg.solar.longitude - (-74.0060)) < 1e-4);
    assert(solar_cfg.solar.auto_day_night);
    assert(solar_cfg.theme.auto_day_night);
    assert(solar_cfg.solar.sunrise_fallback_min == (6 * 60 + 15));
    assert(solar_cfg.solar.sunset_fallback_min == (19 * 60 + 45));
    assert(strcmp(solar_cfg.solar.dawn_path, "/tmp/dawn.png") == 0);
    assert(strcmp(solar_cfg.solar.day_path, "/tmp/day.png") == 0);
    assert(strcmp(solar_cfg.solar.dusk_path, "/tmp/dusk.png") == 0);
    assert(strcmp(solar_cfg.solar.night_path, "/tmp/night.png") == 0);
    assert(solar_cfg.schedule_count == 2);
    assert(strcmp(solar_cfg.schedule[0].phase, "dawn") == 0);
    assert(strcmp(solar_cfg.schedule[0].path, "/tmp/sched_dawn.png") == 0);
    assert(solar_cfg.schedule[0].has_theme_mode && solar_cfg.schedule[0].theme_light_mode);
    assert(solar_cfg.schedule[1].has_time &&
           solar_cfg.schedule[1].time_minutes == (21 * 60 + 30));
    assert(strcmp(solar_cfg.schedule[1].path, "/tmp/sched_night.png") == 0);
    assert(solar_cfg.schedule[1].has_theme_mode && !solar_cfg.schedule[1].theme_light_mode);

    /* Reject out-of-bounds latitude in TOML */
    fp = fopen(toml_path, "wb");
    assert(fp != NULL);
    fputs("[solar]\nlatitude = 95.0\n", fp);
    fclose(fp);
    waywal_config_init_defaults(&solar_cfg);
    assert(!waywal_config_load_file(toml_path, &solar_cfg, NULL, 0));

    cli_options_t cli_opts;
    char *argv_solar[] = {"wwal",           "img",           "/tmp/mojave.xml",
                          "--auto-day-night", "--latitude",  "35.6762",
                          "--longitude",    "139.6503"};
    assert(cli_parse(8, argv_solar, &cli_opts));
    cli_auto_dispatch_img(&cli_opts);
    assert(cli_opts.cmd == CLI_CMD_IMG);
    assert(cli_opts.has_auto_day_night && cli_opts.auto_day_night);
    assert(cli_opts.has_solar_coords);
    assert(fabs(cli_opts.solar_latitude - 35.6762) < 1e-4);
    assert(fabs(cli_opts.solar_longitude - 139.6503) < 1e-4);

    char *argv_mode_auto[] = {"wwal", "img", "/tmp/dyn.json", "--mode", "auto"};
    assert(cli_parse(5, argv_mode_auto, &cli_opts));
    cli_auto_dispatch_img(&cli_opts);
    assert(cli_opts.cmd == CLI_CMD_IMG);
    assert(cli_opts.has_auto_day_night && cli_opts.auto_day_night);

    char *argv_no_adn[] = {"wwal", "img", "/tmp/wall.png", "--no-auto-day-night"};
    assert(cli_parse(4, argv_no_adn, &cli_opts));
    assert(cli_opts.has_auto_day_night && !cli_opts.auto_day_night);

    /* Reject out-of-bounds CLI coordinates */
    char *argv_bad_lat[] = {"wwal", "img", "/tmp/wall.png", "--latitude", "91.0"};
    assert(!cli_parse(5, argv_bad_lat, &cli_opts));
    char *argv_bad_lon[] = {"wwal", "img", "/tmp/wall.png", "--longitude", "-181.0"};
    assert(!cli_parse(5, argv_bad_lon, &cli_opts));

    /* 5. Manifest security & XML duration/truncation hardening */
    setenv("WWAL_TEST_DOT", ".", 1);
    const char *bad_xml_comp_env =
        "<background><static><duration>3600</duration><file>/.$WWAL_TEST_DOT/etc/passwd</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml_comp_env, strlen(bad_xml_comp_env),
                                            "/usr/share/backgrounds", &xml_man));
    unsetenv("WWAL_TEST_DOT");

    setenv("WWAL_TEST_REL_TRAV", "..", 1);
    const char *bad_json_rel_env =
        "{\"frames\":[{\"file\":\"sub/$WWAL_TEST_REL_TRAV/etc/passwd\",\"time\":\"06:00\"}]}";
    assert(!waywal_dynwall_parse_json_string(bad_json_rel_env, strlen(bad_json_rel_env),
                                             "/opt/dynwall", &json_man));
    unsetenv("WWAL_TEST_REL_TRAV");

    const char *bad_xml_dur_garbage =
        "<background><static><duration>100abc</duration><file>a.png</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml_dur_garbage, strlen(bad_xml_dur_garbage),
                                            "/opt/dynwall", &xml_man));
    const char *bad_xml_dur_huge =
        "<background><static><duration>1e300</duration><file>a.png</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(bad_xml_dur_huge, strlen(bad_xml_dur_huge),
                                            "/opt/dynwall", &xml_man));

    char *overlong_xml = (char *)malloc(5000);
    assert(overlong_xml != NULL);
    strcpy(overlong_xml, "<background><static><duration>3600</duration><file>/opt/");
    size_t pfx_len = strlen(overlong_xml);
    memset(overlong_xml + pfx_len, 'a', 4100);
    strcpy(overlong_xml + pfx_len + 4100, ".png</file></static></background>");
    assert(!waywal_dynwall_parse_xml_string(overlong_xml, strlen(overlong_xml), "/opt/dynwall",
                                            &xml_man));
    free(overlong_xml);

    /* 6. Sub-phase precedence, non-overlapping windows & custom fallback */
    const char *subphase_json =
        "{\n"
        "  \"frames\": [\n"
        "    { \"file\": \"coarse_day.png\",   \"phase\": \"day\" },\n"
        "    { \"file\": \"sub_morning.png\",  \"phase\": \"morning\" },\n"
        "    { \"file\": \"sub_noon.png\",     \"phase\": \"noon\" },\n"
        "    { \"file\": \"sub_afternoon.png\",\"phase\": \"afternoon\" },\n"
        "    { \"file\": \"coarse_dusk.png\",  \"phase\": \"dusk\" },\n"
        "    { \"file\": \"sub_sunset.png\",   \"phase\": \"sunset\" },\n"
        "    { \"file\": \"sub_evening.png\",  \"phase\": \"evening\" },\n"
        "    { \"file\": \"coarse_night.png\", \"phase\": \"night\" }\n"
        "  ]\n"
        "}\n";
    assert(waywal_dynwall_parse_json_string(subphase_json, strlen(subphase_json), "/opt/dynwall",
                                            &json_man));

    /* Verify fine-grained subphases are not shadowed by earlier coarse 'day'/'dusk' entries */
    lt.tm_hour = 9;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_morning.png") == 0);

    lt.tm_hour = 12;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_noon.png") == 0);

    lt.tm_hour = 15;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_afternoon.png") == 0);

    lt.tm_hour = 18;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_sunset.png") == 0);

    lt.tm_hour = 19;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, NULL, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_evening.png") == 0);

    /* Verify HOME with '..' is rejected even via '~' expansion */
    const char *orig_home = getenv("HOME");
    char saved_home[4096] = {0};
    if (orig_home) {
        snprintf(saved_home, sizeof(saved_home), "%s", orig_home);
    }
    setenv("HOME", "/tmp/foo/../..", 1);
    const char *tilde_trav_xml =
        "<background><static><duration>3600</duration>"
        "<file>~/etc/passwd</file></static></background>";
    assert(!waywal_dynwall_parse_xml_string(tilde_trav_xml, strlen(tilde_trav_xml), "/opt/dynwall",
                                            &xml_man));
    if (saved_home[0] != '\0') {
        setenv("HOME", saved_home, 1);
    }

    /* Verify waywal_solar_is_daytime_at_ex and waywal_solar_synthesize_local honor custom sunrise/sunset */
    waywal_solar_phase_t fb_ph = WAYWAL_SOLAR_NIGHT;
    lt.tm_hour = 7;
    lt.tm_min = 15;
    assert(!waywal_solar_is_daytime_at_ex(0.0, 0.0, false, 480, 1200, t_morning, &lt, &fb_ph));
    assert(fb_ph == WAYWAL_SOLAR_DAWN);
    lt.tm_hour = 8;
    lt.tm_min = 15;
    assert(waywal_solar_is_daytime_at_ex(0.0, 0.0, false, 480, 1200, t_morning, &lt, &fb_ph));
    lt.tm_hour = 19;
    lt.tm_min = 15;
    assert(waywal_solar_is_daytime_at_ex(0.0, 0.0, false, 480, 1200, t_morning, &lt, &fb_ph));
    lt.tm_hour = 20;
    lt.tm_min = 15;
    assert(!waywal_solar_is_daytime_at_ex(0.0, 0.0, false, 480, 1200, t_morning, &lt, &fb_ph));

    /* Verify waywal_solar_synthesize_local drives waywal_dynwall_resolve with custom sunrise=05:00 (300) */
    waywal_solar_state_t custom_fb_solar;
    lt.tm_hour = 5;
    lt.tm_min = 45;
    lt.tm_sec = 0;
    waywal_solar_synthesize_local(300, 1260, t_morning, &lt, &custom_fb_solar);
    assert(custom_fb_solar.is_daylight);
    assert(custom_fb_solar.phase == WAYWAL_SOLAR_DAY);
    assert(waywal_dynwall_resolve(&json_man, t_morning, &lt, &custom_fb_solar, &eval_res));
    assert(strcmp(eval_res.primary_path, "/opt/dynwall/sub_morning.png") == 0);

    /* 7. Explicit --light / --dark / --mode / --theme-mode overrides auto_day_night */
    char *argv_dark[] = {"wwal", "img", "/tmp/wall.png", "--auto-day-night", "--dark"};
    assert(cli_parse(5, argv_dark, &cli_opts));
    assert(cli_opts.has_theme_mode && !cli_opts.theme_light_mode);
    assert(cli_opts.has_auto_day_night && !cli_opts.auto_day_night);

    char *argv_light[] = {"wwal", "img", "/tmp/wall.png", "--light"};
    assert(cli_parse(4, argv_light, &cli_opts));
    assert(cli_opts.has_theme_mode && cli_opts.theme_light_mode);
    assert(cli_opts.has_auto_day_night && !cli_opts.auto_day_night);

    char *argv_tmode_dark[] = {"wwal", "theme", "--mode", "dark"};
    assert(cli_parse(4, argv_tmode_dark, &cli_opts));
    assert(cli_opts.has_theme_mode && !cli_opts.theme_light_mode);
    assert(cli_opts.has_auto_day_night && !cli_opts.auto_day_night);

    char *argv_lat_only[] = {"wwal", "img", "/tmp/wall.png", "--latitude", "40.7"};
    assert(cli_parse(5, argv_lat_only, &cli_opts));
    assert(cli_opts.has_coordinates && cli_opts.has_solar_lat && !cli_opts.has_solar_lon);
    assert(fabs(cli_opts.latitude - 40.7) < 1e-6);

    unlink(toml_path);
    rmdir(tmp_dir);

    printf("  -> Solar Schedule & Dynamic Wallpapers: PASSED\n");
}

static void test_shader_presets(void)
{
    printf("[TEST] Testing Curated Built-In Shader Preset Library & CLI...\n");

    /* 1. Verify waywal_shader_preset_list() discovers all 10 built-in presets */
    assert(waywal_shader_preset_list(NULL, 0) == 10U);
    waywal_shader_preset_info_t presets[16];
    size_t count = waywal_shader_preset_list(presets, 16);
    assert(count == 10U);

    const char *expected_names[10] = {
        "aurora",    "cyber_grid", "fluid_ink",  "synthwave_sun", "starfield",
        "rain_glass", "audio_bars", "audio_ring", "crt_retro",     "plasma_orb",
    };

    for (size_t i = 0; i < 10; ++i) {
        assert(strcmp(presets[i].name, expected_names[i]) == 0);
        assert(presets[i].title[0] != '\0');
        assert(presets[i].description[0] != '\0');
        assert(presets[i].filename[0] != '\0');
        assert(strcmp(presets[i].author, "WayWal") == 0);
        assert(presets[i].path[0] != '\0');
        assert(access(presets[i].path, R_OK) == 0);
        assert(presets[i].is_builtin);

        /* Scan preset GLSL source to verify 0 constant smoothstep(edge0, edge1, ...) with edge0 >= edge1 */
        FILE *sf = fopen(presets[i].path, "rb");
        assert(sf != NULL);
        char sbuf[16384];
        size_t slen = fread(sbuf, 1, sizeof(sbuf) - 1, sf);
        fclose(sf);
        assert(slen > 0);
        sbuf[slen] = '\0';

        const char *sp = sbuf;
        while ((sp = strstr(sp, "smoothstep(")) != NULL) {
            sp += 11;
            while (*sp == ' ' || *sp == '\t')
                sp++;
            char *end0 = NULL;
            float e0 = strtof(sp, &end0);
            if (end0 && end0 > sp) {
                const char *after0 = end0;
                while (*after0 == ' ' || *after0 == '\t')
                    after0++;
                if (*after0 == ',') {
                    const char *arg1 = after0 + 1;
                    while (*arg1 == ' ' || *arg1 == '\t')
                        arg1++;
                    char *end1 = NULL;
                    float e1 = strtof(arg1, &end1);
                    if (end1 && end1 > arg1) {
                        const char *after1 = end1;
                        while (*after1 == ' ' || *after1 == '\t')
                            after1++;
                        if (*after1 == ',') {
                            assert(e0 < e1);
                        }
                    }
                }
            }
        }

        char resolved[4096];
        assert(waywal_shader_resolve_path(expected_names[i], resolved, sizeof(resolved)));
        assert(strcmp(resolved, presets[i].path) == 0);

        uint8_t preview[32 * 32 * 4];
        uint8_t preview_by_path[32 * 32 * 4];
        memset(preview, 0, sizeof(preview));
        memset(preview_by_path, 0, sizeof(preview_by_path));
        assert(waywal_render_shader_preview_bgra(expected_names[i], preview, 32, 32));
        assert(waywal_render_shader_preview_bgra(presets[i].path, preview_by_path, 32, 32));
        assert(memcmp(preview, preview_by_path, sizeof(preview)) == 0);
        bool any_non_zero = false;
        for (size_t b = 0; b < sizeof(preview); ++b) {
            if (preview[b] != 0) {
                any_non_zero = true;
                break;
            }
        }
        assert(any_non_zero);
    }

    /* 2. Verify aliases (lava_lamp, lava-lamp -> plasma_orb; cyber-grid -> cyber_grid) */
    char resolved_alias[4096];
    assert(waywal_shader_resolve_path("lava_lamp", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "plasma_orb.frag") != NULL);

    assert(waywal_shader_resolve_path("lava-lamp", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "plasma_orb.frag") != NULL);

    assert(waywal_shader_resolve_path("cyber-grid", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "cyber_grid.frag") != NULL);

    assert(waywal_shader_resolve_path("preset:aurora", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "aurora.frag") != NULL);

    assert(waywal_shader_resolve_path("aurora.frag", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "aurora.frag") != NULL);

    assert(!waywal_shader_resolve_path("preset:/etc/hosts", resolved_alias, sizeof(resolved_alias)));

    /* 3. Verify custom $WWAL_SHADER_PRESET_DIR resolution, metadata parsing & override deduplication */
    const char *custom_dir = "/tmp/wwal_test_preset_dir";
    const char *custom_shader_dir = "/tmp/wwal_test_shader_dir";
    const char *custom_shader_presets_dir = "/tmp/wwal_test_shader_dir/presets";
    mkdir(custom_dir, 0755);
    mkdir(custom_shader_dir, 0755);
    mkdir(custom_shader_presets_dir, 0755);

    char custom_frag[512];
    snprintf(custom_frag, sizeof(custom_frag), "%s/custom_neon.frag", custom_dir);
    FILE *cf = fopen(custom_frag, "wb");
    assert(cf != NULL);
    fputs("/*\n"
          "  Multi-line license header without leading asterisks\n"
          "*/\n"
          "// @name: Custom Neon\n"
          "// @description: Neon custom procedural shader\n"
          "// @audio_reactive: true\n"
          "/* inline block */ void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }\n"
          "// @name: Fake Trailing Comment In Code\n",
          cf);
    fclose(cf);

    /* Duplicate custom preset in WWAL_SHADER_DIR/presets to verify count-only deduplication */
    char dup_custom_frag[512];
    snprintf(dup_custom_frag, sizeof(dup_custom_frag), "%s/custom_neon.frag",
             custom_shader_presets_dir);
    FILE *dcf = fopen(dup_custom_frag, "wb");
    assert(dcf != NULL);
    fputs("// @name: Duplicate Custom Neon\n"
          "void mainImage(out vec4 c, in vec2 f) { c = vec4(0.2); }\n",
          dcf);
    fclose(dcf);

    char override_aurora[512];
    snprintf(override_aurora, sizeof(override_aurora), "%s/aurora.frag", custom_dir);
    FILE *of = fopen(override_aurora, "wb");
    assert(of != NULL);
    fputs("// @name: Custom Aurora\n"
          "// @description: User override aurora shader\n"
          "// @author: CustomUser\n"
          "// @audio_reactive: true\n"
          "void mainImage(out vec4 c, in vec2 f) { c = vec4(0.5); }\n",
          of);
    fclose(of);

    setenv("WWAL_SHADER_PRESET_DIR", custom_dir, 1);
    setenv("WWAL_SHADER_DIR", custom_shader_dir, 1);
    assert(waywal_shader_resolve_path("custom-neon", resolved_alias, sizeof(resolved_alias)));
    assert(strstr(resolved_alias, "custom_neon.frag") != NULL);

    /* Verify count-only mode (out_list == NULL) deduplicates custom presets across directories */
    assert(waywal_shader_preset_list(NULL, 0) == 11U);

    waywal_shader_preset_info_t ext_presets[20];
    size_t ext_count = waywal_shader_preset_list(ext_presets, 20);
    assert(ext_count == 11U);
    assert(strcmp(ext_presets[0].name, "aurora") == 0);
    assert(strcmp(ext_presets[0].title, "Custom Aurora") == 0);
    assert(strcmp(ext_presets[0].description, "User override aurora shader") == 0);
    assert(strcmp(ext_presets[0].author, "CustomUser") == 0);
    assert(ext_presets[0].audio_reactive);
    assert(strcmp(ext_presets[10].name, "custom_neon") == 0);
    assert(!ext_presets[10].is_builtin);
    assert(strcmp(ext_presets[10].title, "Custom Neon") == 0);
    assert(strcmp(ext_presets[10].description, "Neon custom procedural shader") == 0);
    assert(strcmp(ext_presets[10].filename, "custom_neon.frag") == 0);
    assert(ext_presets[10].audio_reactive);

    /* Verify value-copying waywal_shader_preset_info_t does not dangle after clearing source */
    waywal_shader_preset_info_t copy = ext_presets[10];
    memset(&ext_presets[10], 0, sizeof(ext_presets[10]));
    assert(strcmp(copy.name, "custom_neon") == 0);
    assert(strcmp(copy.title, "Custom Neon") == 0);
    assert(strcmp(copy.description, "Neon custom procedural shader") == 0);
    assert(strcmp(copy.filename, "custom_neon.frag") == 0);
    assert(copy.audio_reactive);

    /* Verify CLI auto-detects @audio_reactive: true metadata for custom and overridden presets */
    cli_options_t opts;
    char *argv_custom_audio1[] = {"wwal", "shader", "custom_neon"};
    assert(cli_parse(3, argv_custom_audio1, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);

    char *argv_custom_audio2[] = {"wwal", "custom_neon"};
    assert(cli_parse(2, argv_custom_audio2, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);

    char *argv_custom_audio3[] = {"wwal", "img", "custom_neon"};
    assert(cli_parse(3, argv_custom_audio3, &opts));
    assert(cli_auto_dispatch_img(&opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);

    /* Verify cli_auto_dispatch_img on an existing local .frag path (no overlapping strncpy) */
    char *argv_custom_audio_file[] = {"wwal", "img", custom_frag};
    assert(cli_parse(3, argv_custom_audio_file, &opts));
    assert(cli_auto_dispatch_img(&opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);
    assert(strcmp(opts.filepath, custom_frag) == 0);

    char *argv_override_audio[] = {"wwal", "shader", "aurora"};
    assert(cli_parse(3, argv_override_audio, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);

    unsetenv("WWAL_SHADER_DIR");
    unsetenv("WWAL_SHADER_PRESET_DIR");
    unlink(dup_custom_frag);
    rmdir(custom_shader_presets_dir);
    rmdir(custom_shader_dir);
    unlink(override_aurora);
    unlink(custom_frag);
    rmdir(custom_dir);

    assert(!waywal_shader_resolve_path(NULL, resolved_alias, sizeof(resolved_alias)));
    assert(!waywal_shader_resolve_path("", resolved_alias, sizeof(resolved_alias)));
    assert(!waywal_shader_resolve_path("nonexistent_preset_xyz", resolved_alias,
                                       sizeof(resolved_alias)));
    assert(!waywal_shader_resolve_path("/tmp/nonexistent_shader_xyz.frag", resolved_alias,
                                       sizeof(resolved_alias)));
    assert(!waywal_shader_resolve_path("../aurora", resolved_alias, sizeof(resolved_alias)));
    assert(!waywal_shader_resolve_path("aurora", resolved_alias, 4));

    /* 4. Verify CLI parsing: wwal shader list, wwal shader --list, wwal shader preset <name> */
    char *argv_list[] = {"wwal", "shader", "list"};
    assert(cli_parse(3, argv_list, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.shader_list);

    char *argv_flag_list[] = {"wwal", "shader", "--list", "--json"};
    assert(cli_parse(4, argv_flag_list, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.shader_list);
    assert(opts.json_output);

    char *argv_preset[] = {"wwal", "shader", "preset", "aurora", "--audio"};
    assert(cli_parse(5, argv_preset, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(!opts.shader_list);
    assert(opts.shader_preset_explicit);
    assert(opts.audio_reactive);
    assert(strstr(opts.filepath, "aurora.frag") != NULL);

    char *argv_bare[] = {"wwal", "shader", "audio_bars"};
    assert(cli_parse(3, argv_bare, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);
    assert(strstr(opts.filepath, "audio_bars.frag") != NULL);

    char *argv_img_preset[] = {"wwal", "img", "cyber-grid"};
    assert(cli_parse(3, argv_img_preset, &opts));
    assert(cli_auto_dispatch_img(&opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(strstr(opts.filepath, "cyber_grid.frag") != NULL);

    char *argv_top_preset[] = {"wwal", "aurora"};
    assert(cli_parse(2, argv_top_preset, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(strstr(opts.filepath, "aurora.frag") != NULL);

    char *argv_top_audio_preset[] = {"wwal", "audio_bars"};
    assert(cli_parse(2, argv_top_audio_preset, &opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.audio_reactive);
    assert(strstr(opts.filepath, "audio_bars.frag") != NULL);

    char *argv_theme_img_preset[] = {"wwal", "theme", "--img", "aurora"};
    assert(cli_parse(4, argv_theme_img_preset, &opts));
    assert(opts.cmd == CLI_CMD_THEME);
    assert(strcmp(opts.filepath, "aurora") == 0);

    char *argv_bad_preset_path[] = {"wwal", "shader", "preset", "/etc/hosts"};
    assert(!cli_parse(4, argv_bad_preset_path, &opts));

    char *argv_missing_preset[] = {"wwal", "shader", "preset"};
    assert(!cli_parse(3, argv_missing_preset, &opts));

    /* Verify 'wwal shader preset nonexistent_local.frag' fails even if ./nonexistent_local.frag exists in CWD */
    const char *local_non_preset = "nonexistent_local.frag";
    FILE *lf = fopen(local_non_preset, "wb");
    assert(lf != NULL);
    fputs("void mainImage(out vec4 c, in vec2 f) { c = vec4(1.0); }\n", lf);
    fclose(lf);

    char *argv_preset_local_cwd[] = {"wwal", "shader", "preset", "nonexistent_local.frag"};
    assert(!cli_parse(4, argv_preset_local_cwd, &opts));
    assert(opts.shader_preset_explicit);
    assert(strcmp(opts.filepath, "preset:nonexistent_local.frag") == 0);
    assert(!waywal_shader_resolve_path(opts.filepath, resolved_alias, sizeof(resolved_alias)));
    unlink(local_non_preset);

    /* Verify 'wwal shader preset:nonexistent' sets shader_preset_explicit and fails cleanly */
    char *argv_preset_colon_missing[] = {"wwal", "shader", "preset:nonexistent"};
    assert(!cli_parse(3, argv_preset_colon_missing, &opts));
    assert(opts.shader_preset_explicit);
    assert(strcmp(opts.filepath, "preset:nonexistent") == 0);

    /* Verify 'wwal img preset:nonexistent' dispatches to CLI_CMD_SHADER with shader_preset_explicit */
    char *argv_img_preset_colon_missing[] = {"wwal", "img", "preset:nonexistent"};
    assert(cli_parse(3, argv_img_preset_colon_missing, &opts));
    assert(cli_auto_dispatch_img(&opts));
    assert(opts.cmd == CLI_CMD_SHADER);
    assert(opts.shader_preset_explicit);
    assert(strcmp(opts.filepath, "preset:nonexistent") == 0);

    printf("  -> Curated Built-In Shader Preset Library & CLI: PASSED\n");
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
    test_cam16_shaders_and_governor();
    test_swww_awww_compat();
    test_ecosystem_and_picker();
    test_superset_template_engine();
    test_template_security_and_edge_cases();
    test_slideshow_and_wpaperd();
    test_solar_and_dynamic_wallpapers();
    test_shader_presets();

    printf("\n>>> ALL CORE SUBSYSTEMS UNIT TESTS PASSED! <<<\n\n");
    return 0;
}
