// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli_parser.h"
#include "waywal/gif_transcode.h"
#include "waywal/image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/palette_hct.h"
#include "waywal/solar_schedule.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"
#include "waywal/we_pkg.h"
#include "waywal/we_scene.h"

#ifndef WAYWAL_VERSION
#define WAYWAL_VERSION "0.2.0"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static void client_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    if (dst == src) {
        dst[cap - 1] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool argv_has_flag(int argc, char *argv[], const char *flag)
{
    size_t flen = strlen(flag);
    for (int i = 1; i < argc; ++i) {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], flag) == 0)
            return true;
        if (strncmp(argv[i], flag, flen) == 0 && argv[i][flen] == '=')
            return true;
    }
    return false;
}

static waywal_solar_config_t g_client_solar_cfg;

static void apply_config_defaults_to_cli(int argc, char *argv[], const waywal_config_t *cfg,
                                         cli_options_t *opts)
{
    if (!cfg || !opts)
        return;

    const char *env_awww_ns = getenv("AWWW_NAMESPACE");
    const char *env_swww_ns = getenv("SWWW_NAMESPACE");
    bool has_env_ns =
        (env_awww_ns && env_awww_ns[0] != '\0') || (env_swww_ns && env_swww_ns[0] != '\0');
    if (!has_env_ns && !argv_has_flag(argc, argv, "-n") &&
        !argv_has_flag(argc, argv, "--namespace") && cfg->daemon.namespace_str[0] != '\0' &&
        strcmp(cfg->daemon.namespace_str, "waywal") != 0) {
        client_copy_cstr(opts->namespace_str, sizeof(opts->namespace_str),
                         cfg->daemon.namespace_str);
    }

    if (!opts->has_transition_type && !argv_has_flag(argc, argv, "--transition-type") &&
        !argv_has_flag(argc, argv, "-t") && !argv_has_flag(argc, argv, "--transition-shader")) {
        opts->transition_type = cfg->transition.type_enum;
    }
    if (!opts->has_transition_duration && opts->transition_step == 0 &&
        !argv_has_flag(argc, argv, "--transition-duration") &&
        !argv_has_flag(argc, argv, "--transition-step")) {
        opts->transition_duration = cfg->transition.duration;
        opts->transition_duration_ms = (uint32_t)(cfg->transition.duration * 1000.0f);
    }
    if (!opts->has_transition_fps && !argv_has_flag(argc, argv, "--transition-fps") &&
        !argv_has_flag(argc, argv, "--fps")) {
        if (cfg->transition.fps > 0)
            opts->transition_fps = cfg->transition.fps;
    }
    if (opts->transition_step > 0 && !opts->has_transition_duration) {
        uint32_t fps = opts->transition_fps > 0 ? opts->transition_fps : 60u;
        uint32_t frames = (255u + opts->transition_step - 1u) / opts->transition_step;
        uint32_t approx_ms = (frames * 1000u) / fps;
        if (approx_ms < 100u)
            approx_ms = 100u;
        if (approx_ms > 10000u)
            approx_ms = 10000u;
        opts->transition_duration_ms = approx_ms;
        opts->transition_duration = (float)approx_ms / 1000.0f;
    }
    if (!opts->has_transition_angle && !opts->has_transition_type &&
        !argv_has_flag(argc, argv, "--transition-angle") &&
        !argv_has_flag(argc, argv, "--transition-type") && !argv_has_flag(argc, argv, "-t")) {
        opts->transition_angle = cfg->transition.angle;
        opts->transition_angle_rad = cfg->transition.angle * 3.1415926535f / 180.0f;
    }
    if (!opts->invert_y && !argv_has_flag(argc, argv, "--invert-y") && cfg->transition.invert_y) {
        opts->invert_y = true;
    }
    if (!opts->has_transition_pos && !opts->has_transition_type &&
        !argv_has_flag(argc, argv, "--transition-pos") &&
        !argv_has_flag(argc, argv, "--transition-type") && !argv_has_flag(argc, argv, "-t")) {
        opts->transition_pos_x = cfg->transition.pos_x;
        opts->transition_pos_y = cfg->transition.pos_y;
        opts->pos_y_inverted = cfg->transition.pos_y_inverted;
    }
    if (opts->invert_y && !opts->pos_y_inverted && opts->transition_pos_y >= 0.0f &&
        opts->transition_pos_y <= 1.0f) {
        opts->transition_pos_y = 1.0f - opts->transition_pos_y;
        opts->pos_y_inverted = true;
    }
    if (!opts->has_transition_wave && !argv_has_flag(argc, argv, "--transition-wave")) {
        opts->transition_wave_freq = cfg->transition.wave_x;
        opts->transition_wave_amp = cfg->transition.wave_y;
    }
    if (!opts->has_transition_bezier && !argv_has_flag(argc, argv, "--transition-bezier")) {
        opts->bezier = bezier_create(cfg->transition.bezier[0], cfg->transition.bezier[1],
                                     cfg->transition.bezier[2], cfg->transition.bezier[3]);
        opts->has_custom_bezier = true;
    }
    if (!opts->has_crop_gravity) {
        if (cfg->transition.has_crop_gravity) {
            opts->crop_gravity_x = cfg->transition.crop_gravity_x;
            opts->crop_gravity_y = cfg->transition.crop_gravity_y;
            opts->has_crop_gravity = true;
        } else if (cfg->daemon.has_crop_gravity) {
            opts->crop_gravity_x = cfg->daemon.crop_gravity_x;
            opts->crop_gravity_y = cfg->daemon.crop_gravity_y;
            opts->has_crop_gravity = true;
        }
    }
    if (!opts->has_10bit && !argv_has_flag(argc, argv, "--10bit") && cfg->daemon.ten_bit) {
        opts->enable_10bit = true;
    }
    if (!opts->has_auto_day_night && !opts->has_theme_mode &&
        (cfg->solar.auto_day_night || cfg->theme.auto_day_night || cfg->auto_day_night)) {
        opts->auto_day_night = true;
    }
    if (cfg->solar.has_coordinates) {
        if (!opts->has_solar_lat) {
            opts->solar_latitude = cfg->solar.latitude;
            opts->has_solar_lat = true;
        }
        if (!opts->has_solar_lon) {
            opts->solar_longitude = cfg->solar.longitude;
            opts->has_solar_lon = true;
        }
        opts->has_solar_coords = true;
    } else if (opts->has_solar_coords && !(opts->has_solar_lat && opts->has_solar_lon)) {
        fprintf(stderr, "Warning: Ignoring incomplete solar coordinates (both --latitude and "
                        "--longitude are required)\n");
        opts->has_solar_coords = false;
        opts->has_solar_lat = false;
        opts->has_solar_lon = false;
        opts->solar_latitude = 0.0;
        opts->solar_longitude = 0.0;
    }
    g_client_solar_cfg = cfg->solar;
    if (opts->has_solar_coords) {
        g_client_solar_cfg.latitude = opts->solar_latitude;
        g_client_solar_cfg.longitude = opts->solar_longitude;
        g_client_solar_cfg.has_coordinates = true;
    }
    if (!opts->has_theme_mode) {
        if (opts->auto_day_night) {
            time_t now_utc = time(NULL);
            struct tm local_tm;
            localtime_r(&now_utc, &local_tm);
            opts->theme_light_mode = waywal_solar_is_daytime_at_ex(
                opts->solar_latitude, opts->solar_longitude, opts->has_solar_coords,
                cfg->solar.sunrise_fallback_min, cfg->solar.sunset_fallback_min, now_utc, &local_tm,
                NULL);
        } else {
            opts->theme_light_mode = (cfg->theme.theme_mode == WAYWAL_THEME_LIGHT);
        }
    }
    if (!opts->has_source_color_index && cfg->theme.source_color_index <= 3U) {
        opts->source_color_index = cfg->theme.source_color_index;
    }
    if (!opts->has_seed_color && cfg->theme.has_seed_color) {
        opts->seed_color = cfg->theme.seed_color;
        opts->has_seed_color = true;
    }
    if (!opts->has_sync_mode && !argv_has_flag(argc, argv, "--sync-mode") &&
        cfg->transition.sync_mode[0] != '\0') {
        opts->sync_mode = (strcasecmp(cfg->transition.sync_mode, "staggered") == 0) ? 1U : 0U;
    }
    if (!opts->has_stagger_delay && !argv_has_flag(argc, argv, "--stagger-delay") &&
        cfg->transition.stagger_delay_ms > 0) {
        opts->stagger_delay_ms = cfg->transition.stagger_delay_ms;
    }

    const waywal_output_rule_t *out_rule =
        waywal_config_match_output(cfg, opts->num_outputs > 0 ? opts->outputs[0] : "*");
    if (!opts->has_scaling_mode && out_rule && out_rule->has_scaling) {
        opts->scaling_mode = out_rule->scaling;
        opts->has_scaling_mode = true;
    }
    if (!opts->has_filter_mode && out_rule && out_rule->has_filter) {
        opts->filter = out_rule->filter;
        opts->has_filter_mode = true;
    }
    if (!opts->has_fill_color && out_rule && out_rule->has_fill_color) {
        opts->clear_color.r = (uint8_t)((out_rule->fill_color >> 24) & 0xFFU);
        opts->clear_color.g = (uint8_t)((out_rule->fill_color >> 16) & 0xFFU);
        opts->clear_color.b = (uint8_t)((out_rule->fill_color >> 8) & 0xFFU);
        opts->clear_color.a = (uint8_t)(out_rule->fill_color & 0xFFU);
        opts->has_fill_color = true;
    }
    if (!opts->has_slideshow_interval) {
        if (out_rule && out_rule->has_interval) {
            opts->slideshow_interval_s = out_rule->interval;
        } else if (cfg->daemon.has_slideshow_interval) {
            opts->slideshow_interval_s = cfg->daemon.slideshow_interval;
        }
    }
    if (!opts->has_slideshow_sort) {
        if (out_rule && (out_rule->has_sort_mode || out_rule->has_shuffle)) {
            opts->slideshow_sort = out_rule->sort_mode;
            opts->slideshow_random = (opts->slideshow_sort == WAYWAL_SORT_RANDOM);
            opts->has_slideshow_sort = true;
        } else if (cfg->daemon.has_slideshow_sort || cfg->daemon.has_slideshow_shuffle) {
            opts->slideshow_sort = cfg->daemon.slideshow_sort;
            opts->slideshow_random = (opts->slideshow_sort == WAYWAL_SORT_RANDOM);
            opts->has_slideshow_sort = true;
        }
    }
}

static bool extract_palette_from_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                      waywal_theme_mode_t mode, const cli_options_t *opts,
                                      const waywal_config_t *cfg, const char *wallpaper_path,
                                      waywal_palette_t *out_pal)
{
    if (!out_pal)
        return false;

    waywal_scheme_variant_t variant =
        waywal_scheme_variant_from_str(cfg ? cfg->theme.scheme_variant : NULL);
    float contrast = cfg ? cfg->theme.contrast : 0.0f;
    uint8_t src_idx = (opts && opts->has_source_color_index)
                          ? opts->source_color_index
                          : (cfg ? cfg->theme.source_color_index : 0U);
    bool has_seed = false;
    uint32_t seed_rgb = 0U;
    if (opts && opts->has_seed_color) {
        has_seed = true;
        seed_rgb = opts->seed_color;
    } else if (cfg && cfg->theme.has_seed_color) {
        has_seed = true;
        seed_rgb = cfg->theme.seed_color;
    }
    static const uint8_t dummy_bgra[4] = {0, 0, 0, 255};
    if (!bgra_pixels || width == 0 || height == 0) {
        if (!has_seed)
            return false;
        bgra_pixels = dummy_bgra;
        width = 1;
        height = 1;
    }
    if (!waywal_palette_extract_seed_ex(bgra_pixels, width, height, width * 4U, true, mode, variant,
                                        contrast, src_idx, has_seed, seed_rgb, out_pal)) {
        return false;
    }
    if (opts && opts->no_cache) {
        out_pal->wallpaper_path[0] = '\0';
    } else if (wallpaper_path && wallpaper_path[0] != '\0') {
        char resolved_wp[PATH_MAX];
        if (realpath(wallpaper_path, resolved_wp)) {
            client_copy_cstr(out_pal->wallpaper_path, sizeof(out_pal->wallpaper_path), resolved_wp);
        } else {
            client_copy_cstr(out_pal->wallpaper_path, sizeof(out_pal->wallpaper_path),
                             wallpaper_path);
        }
    }
    return true;
}

static void apply_extracted_palette_client(const waywal_palette_t *pal, const cli_options_t *opts,
                                           const waywal_config_t *cfg)
{
    if (!pal)
        return;
    bool no_cache = (opts && opts->no_cache);
    if (!no_cache) {
        (void)waywal_palette_export_filtered(pal, NULL, cfg ? cfg->theme.caelestia_sync : true,
                                             cfg ? cfg->theme.export_css : true,
                                             cfg ? cfg->theme.export_hyprland : true);
        if (cfg) {
            (void)waywal_palette_apply_configured_templates(pal, NULL, NULL, cfg);
        } else {
            (void)waywal_palette_export_pywal(pal, NULL);
        }
    } else if (cfg && cfg->template_count > 0) {
        waywal_config_t cfg_nc = *cfg;
        cfg_nc.theme.pywal_compat = false;
        (void)waywal_palette_apply_configured_templates(pal, NULL, NULL, &cfg_nc);
    }
    if (!cfg || cfg->theme.tty_sequences) {
        (void)waywal_palette_broadcast_tty(pal);
    }
    if (cfg && cfg->hooks.on_theme_change[0] != '\0') {
        (void)waywal_palette_run_hook(cfg->hooks.on_theme_change);
    }
}

static bool resolve_wwal_cache_root(path_buf_t *out_root)
{
    if (!out_root)
        return false;
    path_buf_init(out_root);
    const char *env_cache = getenv("WWAL_CACHE_DIR");
    if (env_cache && env_cache[0] != '\0') {
        path_buf_from_str(out_root, env_cache);
        return out_root->len > 0;
    }
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0] != '\0') {
        path_buf_from_str(out_root, xdg_cache);
        return out_root->len > 0 && path_buf_push(out_root, "wwal");
    }
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return false;
    }
    path_buf_from_str(out_root, home);
    return out_root->len > 0 && path_buf_push(out_root, ".cache/wwal");
}

static uint64_t hash_file_meta_u64(const char *path)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)path; p && *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    struct stat st;
    if (path && stat(path, &st) == 0) {
        uint64_t sz = (uint64_t)st.st_size;
        uint64_t mt = (uint64_t)st.st_mtim.tv_sec ^ ((uint64_t)st.st_mtim.tv_nsec << 32);
        for (int i = 0; i < 8; ++i) {
            h ^= (sz >> (i * 8)) & 0xFFULL;
            h *= 1099511628211ULL;
            h ^= (mt >> (i * 8)) & 0xFFULL;
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static bool is_video_container_file(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mkv") == 0 ||
           strcasecmp(dot, ".webm") == 0 || strcasecmp(dot, ".mov") == 0 ||
           strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".m4v") == 0;
}

static bool is_shader_source_file(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
           strcasecmp(dot, ".comp") == 0 || strcasecmp(dot, ".shader") == 0;
}

static bool is_supported_image_file_ext(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".webp") == 0 ||
           strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".avif") == 0 ||
           strcasecmp(dot, ".jxl") == 0 || strcasecmp(dot, ".heic") == 0 ||
           strcasecmp(dot, ".heif") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".qoi") == 0 || strcasecmp(dot, ".svg") == 0 ||
           strcasecmp(dot, ".ppm") == 0 || strcasecmp(dot, ".pgm") == 0 ||
           strcasecmp(dot, ".pnm") == 0;
}

static bool executable_exists_in_path(const char *bin_name)
{
    if (!bin_name || bin_name[0] == '\0')
        return false;
    struct stat st;
    if (strchr(bin_name, '/') != NULL) {
        return stat(bin_name, &st) == 0 && S_ISREG(st.st_mode) && access(bin_name, X_OK) == 0;
    }
    const char *path_env = getenv("PATH");
    if (!path_env || path_env[0] == '\0')
        return false;

    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 3800) {
            char candidate[4096];
            snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, p, bin_name);
            if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && access(candidate, X_OK) == 0) {
                return true;
            }
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

static bool client_poll_fd(int fd, short events, int timeout_ms)
{
    struct timespec start;
    if (timeout_ms > 0) {
        (void)clock_gettime(CLOCK_MONOTONIC, &start);
    }
    for (;;) {
        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            struct timespec now;
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (long)(now.tv_sec - start.tv_sec) * 1000L +
                              (now.tv_nsec - start.tv_nsec) / 1000000L;
            if (elapsed_ms >= timeout_ms) {
                errno = ETIMEDOUT;
                return false;
            }
            remaining = timeout_ms - (int)elapsed_ms;
        }
        struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
        int rc = poll(&pfd, 1, remaining);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        return true;
    }
}

static bool client_write_exact(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < len) {
        ssize_t w = write(fd, p + total, len - total);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (client_poll_fd(fd, POLLOUT, 5000)) {
                    continue;
                }
            }
            return false;
        }
        if (w == 0) {
            return false;
        }
        total += (size_t)w;
    }
    return true;
}

static bool client_read_up_to_eof(int fd, void *buf, size_t cap, size_t *out_read)
{
    if (out_read) {
        *out_read = 0;
    }
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < cap) {
        ssize_t r = read(fd, p + total, cap - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (client_poll_fd(fd, POLLIN, 5000)) {
                    continue;
                }
            }
            if (out_read) {
                *out_read = total;
            }
            return false;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    if (out_read) {
        *out_read = total;
    }
    return true;
}

static bool atomic_write_file_bytes(const char *dst_path, const void *data, size_t len)
{
    if (!dst_path || dst_path[0] == '\0' || (len > 0 && !data)) {
        return false;
    }
    char tmp_path[PATH_MAX + 64];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld.XXXXXX", dst_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return false;
    }
    (void)fchmod(fd, 0600);
    bool ok = (len == 0 || client_write_exact(fd, data, len)) && (fsync(fd) == 0);
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok || rename(tmp_path, dst_path) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    return true;
}

static bool atomic_save_png_bgra(const char *dst_path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    if (!dst_path || dst_path[0] == '\0' || !bgra || w == 0 || h == 0) {
        return false;
    }
    char tmp_path[PATH_MAX + 64];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld.XXXXXX", dst_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return false;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return false;
    }
    (void)close(fd);
    if (!image_save_png_bgra(tmp_path, bgra, w, h)) {
        (void)unlink(tmp_path);
        return false;
    }
    (void)chmod(tmp_path, 0600);
    int rfd = open(tmp_path, O_RDONLY | O_CLOEXEC);
    if (rfd >= 0) {
        (void)fsync(rfd);
        close(rfd);
    }
    if (rename(tmp_path, dst_path) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    return true;
}

static bool load_image_or_video_first_frame(const char *path, bool no_cache, uint8_t **out_px,
                                            uint32_t *out_w, uint32_t *out_h,
                                            const waywal_solar_config_t *solar_cfg)
{
    if (!path || !out_px || !out_w || !out_h) {
        return false;
    }
    struct stat path_st;
    bool have_stat = (stat(path, &path_st) == 0);
    const char *dot = strrchr(path, '.');
    bool is_ws_meta =
        (dot != NULL && (strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".pkg") == 0));
    bool should_try_ws = (have_stat && S_ISDIR(path_st.st_mode)) ||
                         (!have_stat && strchr(path, '/') == NULL) || is_ws_meta;

    char ws_dir[4096];
    if (should_try_ws && we_workshop_resolve_dir(path, ws_dir, sizeof(ws_dir))) {
        char ws_prev[4096];
        if (we_workshop_find_preview_file(ws_dir, ws_prev, sizeof(ws_prev)) &&
            image_load(ws_prev, out_px, out_w, out_h)) {
            return true;
        }
        uint8_t *sbuf = (uint8_t *)malloc(128U * 128U * 4U);
        if (!sbuf) {
            return false;
        }
        if (!waywal_render_shader_preview_bgra(ws_dir, sbuf, 128U, 128U)) {
            free(sbuf);
            return false;
        }
        *out_px = sbuf;
        *out_w = 128U;
        *out_h = 128U;
        return true;
    }
    if (is_shader_source_file(path)) {
        uint8_t *sbuf = (uint8_t *)malloc(128U * 128U * 4U);
        if (!sbuf) {
            return false;
        }
        if (!waywal_render_shader_preview_bgra(path, sbuf, 128U, 128U)) {
            free(sbuf);
            return false;
        }
        *out_px = sbuf;
        *out_w = 128U;
        *out_h = 128U;
        return true;
    }
    if (waywal_dynwall_is_manifest_path(path)) {
        waywal_dynwall_manifest_t manifest;
        waywal_dynwall_resolved_t res;
        time_t now_utc = time(NULL);
        struct tm local_tm;
        localtime_r(&now_utc, &local_tm);
        waywal_solar_state_t solar;
        const waywal_solar_config_t *scfg = solar_cfg ? solar_cfg : &g_client_solar_cfg;
        if (scfg->has_coordinates) {
            waywal_solar_compute(scfg->latitude, scfg->longitude, now_utc, &local_tm, &solar);
        } else {
            waywal_solar_synthesize_local(scfg->sunrise_fallback_min, scfg->sunset_fallback_min,
                                          now_utc, &local_tm, &solar);
        }
        if (waywal_dynwall_load_file(path, &manifest) &&
            waywal_dynwall_resolve(&manifest, now_utc, &local_tm, &solar, &res)) {
            const char *eff_p = waywal_dynwall_effective_path(&res);
            if (eff_p[0] != '\0' && image_load(eff_p, out_px, out_w, out_h)) {
                return true;
            }
        }
    }
    if (!have_stat) {
        char resolved_sh[4096];
        if (waywal_shader_resolve_path(path, resolved_sh, sizeof(resolved_sh))) {
            uint8_t *sbuf = (uint8_t *)malloc(128U * 128U * 4U);
            if (!sbuf) {
                return false;
            }
            if (!waywal_render_shader_preview_bgra(resolved_sh, sbuf, 128U, 128U)) {
                free(sbuf);
                return false;
            }
            *out_px = sbuf;
            *out_w = 128U;
            *out_h = 128U;
            return true;
        }
    }
    if (!is_video_container_file(path) && image_load(path, out_px, out_w, out_h)) {
        return true;
    }

    const size_t thumb_bytes = 128U * 128U * 4U;
    path_buf_t cache_root;
    path_buf_t thumbs_dir;
    path_buf_t thumb_file;
    path_buf_t thumb_png_file;
    bool have_thumb_path = false;
    uint64_t fhash = hash_file_meta_u64(path);

    if (!no_cache && resolve_wwal_cache_root(&cache_root)) {
        thumbs_dir = cache_root;
        if (path_buf_push(&thumbs_dir, "thumbs")) {
            char fname[48];
            snprintf(fname, sizeof(fname), "%016llx.bgra", (unsigned long long)fhash);
            thumb_file = thumbs_dir;
            char pfname[48];
            snprintf(pfname, sizeof(pfname), "%016llx.png", (unsigned long long)fhash);
            thumb_png_file = thumbs_dir;
            if (path_buf_push(&thumb_file, fname) && path_buf_push(&thumb_png_file, pfname)) {
                have_thumb_path = true;
                int cfd = open(path_buf_cstr(&thumb_file), O_RDONLY | O_CLOEXEC);
                if (cfd >= 0) {
                    struct stat st;
                    if (fstat(cfd, &st) == 0 && (size_t)st.st_size == thumb_bytes) {
                        uint8_t *cached = (uint8_t *)malloc(thumb_bytes);
                        if (cached) {
                            size_t got = 0;
                            if (client_read_up_to_eof(cfd, cached, thumb_bytes, &got) &&
                                got == thumb_bytes) {
                                close(cfd);
                                if (access(path_buf_cstr(&thumb_png_file), R_OK) != 0) {
                                    (void)atomic_save_png_bgra(path_buf_cstr(&thumb_png_file),
                                                               cached, 128U, 128U);
                                }
                                *out_px = cached;
                                *out_w = 128U;
                                *out_h = 128U;
                                return true;
                            }
                            free(cached);
                        }
                    }
                    close(cfd);
                }
            }
        }
    }

    if (!executable_exists_in_path("ffmpeg")) {
        return false;
    }

    int pfd[2];
    if (pipe(pfd) != 0) {
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        if (pfd[1] != STDOUT_FILENO) {
            if (dup2(pfd[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            close(pfd[1]);
        }
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            if (devnull != STDIN_FILENO)
                dup2(devnull, STDIN_FILENO);
            if (devnull != STDERR_FILENO)
                dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }
        char *const ff_argv[] = {
            "ffmpeg",     "-v",        "error",    "-nostdin", "-i",
            (char *)path, "-frames:v", "1",        "-vf",      "scale=128:128:flags=fast_bilinear",
            "-f",         "rawvideo",  "-pix_fmt", "bgra",     "pipe:1",
            NULL,
        };
        execvp("ffmpeg", ff_argv);
        _exit(127);
    }
    close(pfd[1]);

    uint8_t *buf = (uint8_t *)malloc(thumb_bytes);
    size_t len = 0;
    bool read_ok = (buf != NULL) && client_read_up_to_eof(pfd[0], buf, thumb_bytes, &len);
    close(pfd[0]);
    int status = 0;
    pid_t wres;
    do {
        wres = waitpid(pid, &status, 0);
    } while (wres < 0 && errno == EINTR);
    bool child_ok = (wres == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (!read_ok || !child_ok || len != thumb_bytes) {
        free(buf);
        return false;
    }

    if (have_thumb_path) {
        (void)mkdir(path_buf_cstr(&cache_root), 0700);
        (void)mkdir(path_buf_cstr(&thumbs_dir), 0700);
        (void)atomic_write_file_bytes(path_buf_cstr(&thumb_file), buf, thumb_bytes);
        (void)atomic_save_png_bgra(path_buf_cstr(&thumb_png_file), buf, 128U, 128U);
    }

    *out_px = buf;
    *out_w = 128U;
    *out_h = 128U;
    return true;
}

static bool ensure_video_thumbnail_png(const char *path, bool no_cache, char *out_png,
                                       size_t out_cap)
{
    if (out_png && out_cap > 0) {
        out_png[0] = '\0';
    }
    if (!path || path[0] == '\0' || no_cache) {
        return false;
    }

    path_buf_t cache_root;
    if (!resolve_wwal_cache_root(&cache_root)) {
        return false;
    }
    path_buf_t thumbs_dir = cache_root;
    if (!path_buf_push(&thumbs_dir, "thumbs")) {
        return false;
    }
    (void)mkdir(path_buf_cstr(&cache_root), 0700);
    (void)mkdir(path_buf_cstr(&thumbs_dir), 0700);

    uint64_t h = hash_file_meta_u64(path);
    char fname[48];
    snprintf(fname, sizeof(fname), "%016llx.png", (unsigned long long)h);
    path_buf_t png_file = thumbs_dir;
    if (!path_buf_push(&png_file, fname)) {
        return false;
    }

    if (access(path_buf_cstr(&png_file), R_OK) == 0) {
        if (out_png && out_cap > 0) {
            snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
        }
        return true;
    }

    uint8_t *px = NULL;
    uint32_t w = 0, h_px = 0;
    bool ok = false;
    if (load_image_or_video_first_frame(path, no_cache, &px, &w, &h_px, NULL) && px && w > 0 &&
        h_px > 0) {
        ok = atomic_save_png_bgra(path_buf_cstr(&png_file), px, w, h_px);
        char real_p[PATH_MAX];
        if (ok && realpath(path, real_p) != NULL && strcmp(real_p, path) != 0) {
            uint64_t hr = hash_file_meta_u64(real_p);
            char rfname[48];
            snprintf(rfname, sizeof(rfname), "%016llx.png", (unsigned long long)hr);
            path_buf_t rpng = thumbs_dir;
            if (path_buf_push(&rpng, rfname)) {
                (void)atomic_save_png_bgra(path_buf_cstr(&rpng), px, w, h_px);
            }
        }
        image_free(px);
    } else {
        uint8_t *fallback = (uint8_t *)malloc(160U * 90U * 4U);
        if (fallback) {
            if (waywal_render_shader_preview_bgra(path, fallback, 160U, 90U)) {
                ok = atomic_save_png_bgra(path_buf_cstr(&png_file), fallback, 160U, 90U);
            }
            free(fallback);
        }
    }

    if (ok && out_png && out_cap > 0) {
        snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
    }
    return ok;
}

static bool ensure_shader_thumbnail_png(const char *path, bool no_cache, char *out_png,
                                        size_t out_cap)
{
    if (out_png && out_cap > 0) {
        out_png[0] = '\0';
    }
    if (!path || path[0] == '\0' || no_cache) {
        return false;
    }

    path_buf_t cache_root;
    if (!resolve_wwal_cache_root(&cache_root)) {
        return false;
    }
    path_buf_t thumbs_dir = cache_root;
    if (!path_buf_push(&thumbs_dir, "thumbs")) {
        return false;
    }
    (void)mkdir(path_buf_cstr(&cache_root), 0700);
    (void)mkdir(path_buf_cstr(&thumbs_dir), 0700);

    uint64_t h = hash_file_meta_u64(path);
    char fname[48];
    snprintf(fname, sizeof(fname), "%016llx.png", (unsigned long long)h);
    path_buf_t png_file = thumbs_dir;
    if (!path_buf_push(&png_file, fname)) {
        return false;
    }

    if (access(path_buf_cstr(&png_file), R_OK) == 0) {
        if (out_png && out_cap > 0) {
            snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
        }
        return true;
    }

    uint8_t *buf = (uint8_t *)malloc(160U * 90U * 4U);
    if (!buf) {
        return false;
    }
    bool ok = waywal_render_shader_preview_bgra(path, buf, 160U, 90U) &&
              atomic_save_png_bgra(path_buf_cstr(&png_file), buf, 160U, 90U);
    if (ok) {
        char real_p[PATH_MAX];
        if (realpath(path, real_p) != NULL && strcmp(real_p, path) != 0) {
            uint64_t hr = hash_file_meta_u64(real_p);
            char rfname[48];
            snprintf(rfname, sizeof(rfname), "%016llx.png", (unsigned long long)hr);
            path_buf_t rpng = thumbs_dir;
            if (path_buf_push(&rpng, rfname)) {
                (void)atomic_save_png_bgra(path_buf_cstr(&rpng), buf, 160U, 90U);
            }
        }
    }
    free(buf);
    if (ok && out_png && out_cap > 0) {
        snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
    }
    return ok;
}

static bool ensure_image_thumbnail_png(const char *path, bool no_cache, char *out_png,
                                       size_t out_cap)
{
    if (out_png && out_cap > 0) {
        out_png[0] = '\0';
    }
    if (!path || path[0] == '\0' || no_cache) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".svg") == 0)) {
        if (out_png && out_cap > 0) {
            snprintf(out_png, out_cap, "%s", path);
        }
        return true;
    }

    path_buf_t cache_root;
    if (!resolve_wwal_cache_root(&cache_root)) {
        return false;
    }
    path_buf_t thumbs_dir = cache_root;
    if (!path_buf_push(&thumbs_dir, "thumbs")) {
        return false;
    }
    (void)mkdir(path_buf_cstr(&cache_root), 0700);
    (void)mkdir(path_buf_cstr(&thumbs_dir), 0700);

    uint64_t h = hash_file_meta_u64(path);
    char fname[48];
    snprintf(fname, sizeof(fname), "%016llx.png", (unsigned long long)h);
    path_buf_t png_file = thumbs_dir;
    if (!path_buf_push(&png_file, fname)) {
        return false;
    }
    if (access(path_buf_cstr(&png_file), R_OK) == 0) {
        if (out_png && out_cap > 0) {
            snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
        }
        return true;
    }

    uint8_t *px = NULL;
    uint32_t w = 0, h_px = 0;
    if (!image_load(path, &px, &w, &h_px) || !px || w == 0 || h_px == 0) {
        return false;
    }
    bool ok = atomic_save_png_bgra(path_buf_cstr(&png_file), px, w, h_px);
    image_free(px);
    if (ok && out_png && out_cap > 0) {
        snprintf(out_png, out_cap, "%s", path_buf_cstr(&png_file));
    }
    return ok;
}

static size_t clear_cache_dir_files(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0')
        return 0;
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;

    size_t removed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, "current_wallpaper.png") == 0)
            continue;
        size_t nlen = strlen(ent->d_name);
        bool is_cached_ext =
            (nlen > 4 && (strcasecmp(ent->d_name + nlen - 4, ".mp4") == 0 ||
                          strcasecmp(ent->d_name + nlen - 4, ".bin") == 0 ||
                          strcasecmp(ent->d_name + nlen - 4, ".png") == 0 ||
                          strcasecmp(ent->d_name + nlen - 4, ".jpg") == 0)) ||
            (nlen > 5 && (strcasecmp(ent->d_name + nlen - 5, ".bgra") == 0 ||
                          strcasecmp(ent->d_name + nlen - 5, ".webp") == 0 ||
                          strcasecmp(ent->d_name + nlen - 5, ".webm") == 0 ||
                          strcasecmp(ent->d_name + nlen - 5, ".jpeg") == 0 ||
                          strcasecmp(ent->d_name + nlen - 5, ".lock") == 0));
        if (!is_cached_ext)
            continue;

        path_buf_t fpath;
        path_buf_from_str(&fpath, dir_path);
        if (path_buf_push(&fpath, ent->d_name)) {
            if (unlink(path_buf_cstr(&fpath)) == 0) {
                removed++;
            }
        }
    }
    closedir(dir);
    return removed;
}

static int handle_cmd_clear_cache(void)
{
    path_buf_t cache_root;
    if (!resolve_wwal_cache_root(&cache_root)) {
        fprintf(stderr, "Error: Cannot determine cache directory\n");
        return 1;
    }

    size_t removed = clear_cache_dir_files(path_buf_cstr(&cache_root));

    const char *subdirs[] = {"shaders", "transcode", "transcoded", "thumbs"};
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); ++i) {
        path_buf_t sub = cache_root;
        if (path_buf_push(&sub, subdirs[i])) {
            removed += clear_cache_dir_files(path_buf_cstr(&sub));
        }
    }

    printf("Cleared %zu cached file(s) from %s\n", removed, path_buf_cstr(&cache_root));
    return 0;
}

static bool resolve_theme_default_wallpaper(char *out_path, size_t cap)
{
    if (!out_path || cap == 0)
        return false;
    out_path[0] = '\0';

    path_buf_t cael_path;
    path_buf_init(&cael_path);
    const char *xdg_state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg_state && xdg_state[0] != '\0') {
        path_buf_from_str(&cael_path, xdg_state);
        path_buf_push(&cael_path, "caelestia/wallpaper/path.txt");
    } else if (home && home[0] != '\0') {
        path_buf_from_str(&cael_path, home);
        path_buf_push(&cael_path, ".local/state/caelestia/wallpaper/path.txt");
    }

    if (cael_path.len > 0) {
        FILE *fp = fopen(path_buf_cstr(&cael_path), "r");
        if (fp) {
            if (fgets(out_path, (int)cap, fp)) {
                size_t len = strlen(out_path);
                while (len > 0 && (out_path[len - 1] == '\n' || out_path[len - 1] == '\r')) {
                    out_path[--len] = '\0';
                }
            }
            fclose(fp);
            if (out_path[0] != '\0' && access(out_path, R_OK) == 0) {
                return true;
            }
            out_path[0] = '\0';
        }
    }

    waywal_state_store_t st;
    if (waywal_state_store_load(&st, NULL)) {
        for (uint32_t i = 0; i < st.count; ++i) {
            if (st.outputs[i].media_path[0] != '\0' &&
                access(st.outputs[i].media_path, R_OK) == 0) {
                client_copy_cstr(out_path, cap, st.outputs[i].media_path);
                return true;
            }
        }
    }

    path_buf_t cache_root;
    if (resolve_wwal_cache_root(&cache_root)) {
        path_buf_t cur_link = cache_root;
        if (path_buf_push(&cur_link, "current_wallpaper") &&
            access(path_buf_cstr(&cur_link), R_OK) == 0) {
            client_copy_cstr(out_path, cap, path_buf_cstr(&cur_link));
            return true;
        }
        path_buf_t cur_png = cache_root;
        if (path_buf_push(&cur_png, "current_wallpaper.png") &&
            access(path_buf_cstr(&cur_png), R_OK) == 0) {
            client_copy_cstr(out_path, cap, path_buf_cstr(&cur_png));
            return true;
        }
    }

    return false;
}

static int handle_cmd_theme(const cli_options_t *opts, const waywal_config_t *cfg)
{
    char target_path[4096];
    target_path[0] = '\0';
    bool has_seed_ovr = (opts && opts->has_seed_color) || (cfg && cfg->theme.has_seed_color);

    if (opts->filepath[0] != '\0') {
        client_copy_cstr(target_path, sizeof(target_path), opts->filepath);
    } else if (!resolve_theme_default_wallpaper(target_path, sizeof(target_path)) &&
               !has_seed_ovr) {
        fprintf(stderr, "Error: No image path specified and no active wallpaper found in state.\n");
        return 1;
    }

    if (target_path[0] != '\0') {
        struct stat st_theme;
        if (stat(target_path, &st_theme) != 0) {
            char resolved_sh[4096];
            if (waywal_shader_resolve_path(target_path, resolved_sh, sizeof(resolved_sh))) {
                snprintf(target_path, sizeof(target_path), "%s", resolved_sh);
            }
        }
    }

    uint8_t *pixels = NULL;
    uint32_t width = 0, height = 0;
    uint8_t dummy_px[4] = {0, 0, 0, 255};
    if (target_path[0] != '\0' && !has_seed_ovr) {
        if (!load_image_or_video_first_frame(target_path, opts->no_cache, &pixels, &width, &height,
                                             cfg ? &cfg->solar : NULL)) {
            fprintf(stderr, "Error: Failed to decode media '%s' for theme extraction\n",
                    target_path);
            return 1;
        }
    } else {
        width = 1;
        height = 1;
    }

    waywal_theme_mode_t mode = opts->theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    waywal_palette_t pal;
    bool ok = extract_palette_from_bgra(pixels ? pixels : dummy_px, width, height, mode, opts, cfg,
                                        target_path, &pal);
    if (pixels) {
        image_free(pixels);
    }

    if (!ok) {
        fprintf(stderr, "Error: Failed to extract HCT palette from '%s'\n", target_path);
        return 1;
    }

    apply_extracted_palette_client(&pal, opts, cfg);

    printf("Material You Palette (%s):\n"
           "  primary:    #%02x%02x%02x\n"
           "  secondary:  #%02x%02x%02x\n"
           "  tertiary:   #%02x%02x%02x\n"
           "  background: #%02x%02x%02x\n"
           "  surface:    #%02x%02x%02x\n",
           mode == WAYWAL_THEME_LIGHT ? "light" : "dark", pal.primary.r, pal.primary.g,
           pal.primary.b, pal.secondary.r, pal.secondary.g, pal.secondary.b, pal.tertiary.r,
           pal.tertiary.g, pal.tertiary.b, pal.background.r, pal.background.g, pal.background.b,
           pal.surface.r, pal.surface.g, pal.surface.b);
    return 0;
}

static bool drain_and_print_reply(int socket_fd, size_t payload_size, bool print_out)
{
    char buf[4096];
    size_t remaining = payload_size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t r = read(socket_fd, buf, to_read);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                client_poll_fd(socket_fd, POLLIN, 5000)) {
                continue;
            }
            return false;
        }
        if (r == 0)
            return false;
        if (print_out) {
            (void)fwrite(buf, 1, (size_t)r, stdout);
        }
        remaining -= (size_t)r;
    }
    return true;
}

static bool cli_ipc_send_framed(int socket_fd, const waywal_ipc_hdr_t *hdr, const void *payload,
                                size_t payload_len, int attached_fd)
{
    return waywal_ipc_send_payload_fds(socket_fd, hdr, payload, payload_len,
                                       attached_fd >= 0 ? &attached_fd : NULL,
                                       attached_fd >= 0 ? 1U : 0U);
}

static bool cli_recv_reply(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_attached_fd)
{
    if (!out_hdr || !out_attached_fd) {
        return false;
    }
    for (int iter = 0; iter < 64; ++iter) {
        waywal_ipc_hdr_t hdr;
        int afd = -1;
        if (!waywal_ipc_recv(socket_fd, &hdr, &afd)) {
            return false;
        }
        if (hdr.opcode == WAYWAL_RESP_EVENT) {
            if (afd >= 0) {
                close(afd);
            }
            if (hdr.payload_size > 65536U) {
                return false;
            }
            if (hdr.payload_size > 0 &&
                !drain_and_print_reply(socket_fd, (size_t)hdr.payload_size, false)) {
                return false;
            }
            continue;
        }
        *out_hdr = hdr;
        *out_attached_fd = afd;
        return true;
    }
    return false;
}

static char *read_shader_file(const char *path, size_t *out_len)
{
    if (!path || !out_len)
        return NULL;
    char resolved_path[4096];
    const char *open_path = path;
    if (waywal_shader_resolve_path(path, resolved_path, sizeof(resolved_path))) {
        open_path = resolved_path;
    }
    FILE *f = fopen(open_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Failed to open custom shader file '%s': %s\n", path,
                strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fprintf(stderr, "Error: Invalid shader file size (%ld bytes)\n", sz);
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    buf[read_bytes] = '\0';
    fclose(f);
    *out_len = read_bytes + 1; /* Include null terminator */
    return buf;
}

static void exec_sibling_or_path_wwald(char *const daemon_argv[])
{
    char self_path[PATH_MAX];
    ssize_t slen = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (slen > 0) {
        self_path[slen] = '\0';
        char *slash = strrchr(self_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - self_path);
            char sibling[PATH_MAX + 16];
            if (snprintf(sibling, sizeof(sibling), "%.*s/wwald", (int)dir_len, self_path) <
                    (int)sizeof(sibling) &&
                access(sibling, X_OK) == 0) {
                execv(sibling, daemon_argv);
            }
        }
    }
    execvp("wwald", daemon_argv);
}

static bool is_auto_spawn_eligible_cmd(cli_cmd_type_t cmd)
{
    switch (cmd) {
    case CLI_CMD_IMG:
    case CLI_CMD_VIDEO:
    case CLI_CMD_SHADER:
    case CLI_CMD_WORKSHOP:
    case CLI_CMD_SLIDESHOW:
    case CLI_CMD_CLEAR:
    case CLI_CMD_RESTORE:
    case CLI_CMD_QUERY:
    case CLI_CMD_STATUS:
        return true;
    default:
        return false;
    }
}

static int client_connect_or_spawn(const cli_options_t *opts, const char *ns)
{
    int fd = waywal_ipc_client_connect(ns, 500);
    if (fd >= 0) {
        return fd;
    }

    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    if (!opts || opts->no_daemon || !is_auto_spawn_eligible_cmd(opts->cmd) || !wl_disp ||
        wl_disp[0] == '\0') {
        return waywal_ipc_client_connect(ns, 2500);
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        (void)setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }
        char *daemon_argv[10];
        int d_argc = 0;
        daemon_argv[d_argc++] = "wwald";
        daemon_argv[d_argc++] = "-d";
        if (ns && ns[0] != '\0') {
            daemon_argv[d_argc++] = "-n";
            daemon_argv[d_argc++] = (char *)ns;
        }
        daemon_argv[d_argc] = NULL;
        exec_sibling_or_path_wwald(daemon_argv);
        _exit(127);
    }

    int st = 0;
    pid_t wres;
    do {
        wres = waitpid(pid, &st, 0);
    } while (wres < 0 && errno == EINTR);
    if (wres != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return -1;
    }

    for (int attempt = 0; attempt < 200; ++attempt) {
        fd = waywal_ipc_client_connect(ns, 100);
        if (fd >= 0) {
            return fd;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    return -1;
}

typedef struct {
    char ns[64];
    int fd;
    bool persistent;
} cli_ipc_slot_t;

typedef struct {
    cli_ipc_slot_t slots[16];
    size_t count;
} cli_ipc_pool_t;

static void cli_ipc_pool_init(cli_ipc_pool_t *pool)
{
    if (!pool) {
        return;
    }
    memset(pool, 0, sizeof(*pool));
    for (size_t i = 0; i < 16U; ++i) {
        pool->slots[i].fd = -1;
    }
}

static void cli_ipc_pool_close_all(cli_ipc_pool_t *pool)
{
    if (!pool) {
        return;
    }
    for (size_t i = 0; i < pool->count; ++i) {
        if (pool->slots[i].fd >= 0) {
            close(pool->slots[i].fd);
            pool->slots[i].fd = -1;
        }
        pool->slots[i].persistent = false;
    }
    pool->count = 0;
}

static bool cli_ipc_pool_upgrade_persistent(int fd)
{
    waywal_ipc_hdr_t sub = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SUBSCRIBE,
        .payload_size = 0,
    };
    if (!cli_ipc_send_framed(fd, &sub, NULL, 0, -1)) {
        return false;
    }
    waywal_ipc_hdr_t resp;
    int attached_fd = -1;
    bool ok = cli_recv_reply(fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK;
    if (attached_fd >= 0) {
        close(attached_fd);
    }
    if (!ok || resp.payload_size > 65536U) {
        return false;
    }
    if (resp.payload_size > 0 &&
        !drain_and_print_reply(fd, (size_t)resp.payload_size, false)) {
        return false;
    }
    return true;
}

static int cli_ipc_pool_acquire(cli_ipc_pool_t *pool, const cli_options_t *opts, const char *ns,
                                bool allow_spawn, bool want_persistent, int timeout_ms)
{
    const char *eff_ns = (ns && ns[0] != '\0') ? ns : (opts ? opts->namespace_str : "waywal");
    cli_ipc_slot_t *slot = NULL;
    if (pool) {
        for (size_t i = 0; i < pool->count; ++i) {
            if (strcmp(pool->slots[i].ns, eff_ns) == 0) {
                slot = &pool->slots[i];
                break;
            }
        }
        if (slot && slot->fd >= 0) {
            char peek = 0;
            ssize_t pr = recv(slot->fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
            if (pr == 0 || (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                close(slot->fd);
                slot->fd = -1;
                slot->persistent = false;
            } else {
                if (want_persistent && !slot->persistent) {
                    if (cli_ipc_pool_upgrade_persistent(slot->fd)) {
                        slot->persistent = true;
                    } else {
                        close(slot->fd);
                        slot->fd = -1;
                    }
                }
                if (slot->fd >= 0) {
                    return slot->fd;
                }
            }
        }
    }

    int fd = allow_spawn ? client_connect_or_spawn(opts, eff_ns)
                         : waywal_ipc_client_connect(eff_ns, timeout_ms);
    if (fd < 0) {
        return -1;
    }

    bool is_persistent = false;
    if (want_persistent) {
        if (cli_ipc_pool_upgrade_persistent(fd)) {
            is_persistent = true;
        } else {
            /* Fall back cleanly to a fresh single-shot connection if peer rejects SUBSCRIBE. */
            close(fd);
            fd = allow_spawn ? client_connect_or_spawn(opts, eff_ns)
                             : waywal_ipc_client_connect(eff_ns, timeout_ms);
            if (fd < 0) {
                return -1;
            }
        }
    }

    if (pool) {
        if (!slot && pool->count < 16U) {
            slot = &pool->slots[pool->count++];
            client_copy_cstr(slot->ns, sizeof(slot->ns), eff_ns);
        }
        if (slot) {
            slot->fd = fd;
            slot->persistent = is_persistent;
        }
    }
    return fd;
}

static void cli_ipc_pool_release(cli_ipc_pool_t *pool, const char *ns, int fd)
{
    if (fd < 0) {
        return;
    }
    if (!pool) {
        close(fd);
        return;
    }
    const char *eff_ns = (ns && ns[0] != '\0') ? ns : "waywal";
    for (size_t i = 0; i < pool->count; ++i) {
        if (strcmp(pool->slots[i].ns, eff_ns) == 0 && pool->slots[i].fd == fd) {
            if (!pool->slots[i].persistent) {
                close(pool->slots[i].fd);
                pool->slots[i].fd = -1;
            }
            return;
        }
    }
    close(fd);
}

static bool send_audio_ctrl_request(cli_ipc_pool_t *pool, const cli_options_t *opts, const char *ns,
                                    bool persistent, bool print_reply)
{
    if (!opts)
        return false;
    const char *eff_ns = (ns && ns[0] != '\0') ? ns : opts->namespace_str;
    int afd = cli_ipc_pool_acquire(pool, opts, eff_ns, false, persistent, 3000);
    if (afd < 0)
        return false;

    waywal_audio_ctrl_payload_t apay;
    memset(&apay, 0, sizeof(apay));
    apay.volume = opts->volume;
    apay.mute_on_audio = opts->mute_on_audio;
    apay.theme_from_media = opts->theme_from_media;

    waywal_ipc_hdr_t req = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_AUDIO_CTRL,
        .payload_size = sizeof(apay),
    };
    if (!cli_ipc_send_framed(afd, &req, &apay, sizeof(apay), -1)) {
        cli_ipc_pool_release(pool, eff_ns, afd);
        return false;
    }

    waywal_ipc_hdr_t resp;
    int attached_fd = -1;
    bool ok = cli_recv_reply(afd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK;
    if (ok && print_reply && resp.payload_size > 0 && resp.payload_size <= 16384U) {
        (void)drain_and_print_reply(afd, (size_t)resp.payload_size, true);
    } else if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
        (void)drain_and_print_reply(afd, (size_t)resp.payload_size, false);
    }
    if (attached_fd >= 0)
        close(attached_fd);
    cli_ipc_pool_release(pool, eff_ns, afd);
    return ok;
}

static void query_max_target_resolution(cli_ipc_pool_t *pool, const cli_options_t *opts,
                                        const char *ns, bool persistent, uint32_t *out_w,
                                        uint32_t *out_h)
{
    *out_w = 2560;
    *out_h = 1440;

    const char *eff_ns = (ns && ns[0] != '\0') ? ns : opts->namespace_str;
    int qfd = cli_ipc_pool_acquire(pool, opts, eff_ns, false, persistent, 500);
    if (qfd < 0)
        return;

    waywal_ipc_hdr_t req = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_QUERY,
        .payload_size = 0,
    };
    if (!cli_ipc_send_framed(qfd, &req, NULL, 0, -1)) {
        cli_ipc_pool_release(pool, eff_ns, qfd);
        return;
    }

    waywal_ipc_hdr_t resp;
    int attached_fd = -1;
    if (!cli_recv_reply(qfd, &resp, &attached_fd) || resp.opcode != WAYWAL_RESP_INFO ||
        resp.payload_size == 0 || resp.payload_size >= 16384U) {
        if (attached_fd >= 0)
            close(attached_fd);
        cli_ipc_pool_release(pool, eff_ns, qfd);
        return;
    }
    if (attached_fd >= 0)
        close(attached_fd);

    char buf[4096];
    size_t to_read = resp.payload_size < sizeof(buf) - 1 ? resp.payload_size : sizeof(buf) - 1;
    size_t total = 0;
    (void)client_read_up_to_eof(qfd, buf, to_read, &total);
    if (resp.payload_size > total) {
        (void)drain_and_print_reply(qfd, (size_t)resp.payload_size - total, false);
    }
    cli_ipc_pool_release(pool, eff_ns, qfd);
    buf[total] = '\0';

    uint32_t max_w = 0;
    uint32_t max_h = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(buf, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        const char *p = line;
        while ((p = strstr(p, ": ")) != NULL) {
            p += 2;
            unsigned int w = 0, h = 0;
            int consumed = 0;
            if (sscanf(p, "%ux%u, scale:%n", &w, &h, &consumed) == 2 && consumed > 0 && w > 0 &&
                h > 0) {
                if (w > max_w)
                    max_w = w;
                if (h > max_h)
                    max_h = h;
                break;
            }
        }
    }

    if (max_w > 0 && max_h > 0) {
        *out_w = max_w;
        *out_h = max_h;
    }
}

static size_t collect_active_namespaces(cli_ipc_pool_t *pool, const cli_options_t *opts,
                                        bool persistent_probe, char out_ns[16][64])
{
    if (!opts) {
        return 0;
    }
    if (!opts->all_namespaces) {
        client_copy_cstr(out_ns[0], 64, opts->namespace_str);
        return 1;
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || xdg[0] == '\0') {
        xdg = "/tmp";
    }
    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    if (!wl_disp || wl_disp[0] == '\0') {
        wl_disp = "wayland-0";
    } else {
        const char *slash = strrchr(wl_disp, '/');
        if (slash) {
            wl_disp = (slash[1] != '\0') ? (slash + 1) : "wayland-0";
        }
    }

    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s-wwald.", wl_disp);
    size_t plen = strlen(prefix);
    const char *ns_filter = getenv("WWAL_NAMESPACE_PREFIX");
    size_t ns_filter_len = (ns_filter && ns_filter[0] != '\0') ? strlen(ns_filter) : 0;

    size_t count = 0;
    DIR *dir = opendir(xdg);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < 16) {
            if (strncmp(ent->d_name, prefix, plen) != 0) {
                continue;
            }
            size_t nlen = strlen(ent->d_name);
            if (nlen <= plen + 5 || strcmp(ent->d_name + nlen - 5, ".sock") != 0) {
                continue;
            }
            size_t ns_len = nlen - plen - 5;
            if (ns_len == 0 || ns_len >= 64) {
                continue;
            }
            char cand[64];
            memcpy(cand, ent->d_name + plen, ns_len);
            cand[ns_len] = '\0';
            if (ns_filter_len > 0 && strncmp(cand, ns_filter, ns_filter_len) != 0) {
                continue;
            }
            bool dup = false;
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(out_ns[i], cand) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                bool alive = false;
                if (persistent_probe && pool) {
                    int fd = cli_ipc_pool_acquire(pool, opts, cand, false, true, 150);
                    alive = (fd >= 0);
                } else {
                    alive = waywal_ipc_namespace_is_alive(cand);
                }
                if (alive) {
                    client_copy_cstr(out_ns[count], 64, cand);
                    count++;
                }
            }
        }
        closedir(dir);
    }

    if (count == 0) {
        client_copy_cstr(out_ns[0], 64, opts->namespace_str);
        return 1;
    }
    return count;
}

static int handle_cmd_workshop_list(void)
{
    we_project_t *items = NULL;
    size_t count = 0;
    if (!we_workshop_scan_all(&items, &count) || count == 0) {
        printf("ID             TYPE       TITLE                                PATH\n");
        printf("(No Steam Workshop 431960 items found)\n");
        free(items);
        return 0;
    }

    printf("%-14s %-10s %-36s %s\n", "ID", "TYPE", "TITLE", "PATH");
    for (size_t i = 0; i < count; ++i) {
        printf("%-14s %-10s %-36.36s %s\n", items[i].workshop_id,
               items[i].type[0] ? items[i].type : "scene",
               items[i].title[0] ? items[i].title : "Untitled", items[i].workshop_dir);
    }
    free(items);
    return 0;
}

static const char *prop_type_to_str(we_prop_type_t t)
{
    switch (t) {
    case WE_PROP_BOOL:
        return "bool";
    case WE_PROP_INT:
        return "int";
    case WE_PROP_COLOR:
        return "color";
    case WE_PROP_TEXT:
        return "text";
    case WE_PROP_FLOAT:
    default:
        return "float";
    }
}

static int handle_cmd_workshop_info(const cli_options_t *opts)
{
    char dir[4096];
    if (!we_workshop_resolve_dir(opts->filepath, dir, sizeof(dir))) {
        fprintf(stderr, "Error: Workshop item '%s' not found\n", opts->filepath);
        return 1;
    }

    we_project_t proj;
    if (!we_project_load_dir(dir, &proj)) {
        fprintf(stderr, "Error: Failed to parse project.json in '%s'\n", dir);
        return 1;
    }
    if (opts->workshop_props[0] != '\0') {
        we_project_apply_overrides_str(&proj, opts->workshop_props);
    }

    we_pkg_t *pkg = we_pkg_open_dir(dir);
    uint32_t pkg_entries = pkg ? pkg->entry_count : 0;
    char pkg_magic[16];
    snprintf(pkg_magic, sizeof(pkg_magic), "%s", pkg ? pkg->version_magic : "NONE");
    if (pkg) {
        we_pkg_close(pkg);
    }

    printf("Workshop ID:   %s\n", proj.workshop_id);
    printf("Title:         %s\n", proj.title);
    printf("Type:          %s\n", proj.type);
    printf("File:          %s\n", proj.file);
    printf("Preview:       %s\n", proj.preview);
    printf("Directory:     %s\n", dir);
    printf("scene.pkg:     %s (%u entries)\n", pkg_magic, pkg_entries);
    printf("Properties (%u):\n", proj.prop_count);
    for (uint32_t i = 0; i < proj.prop_count; ++i) {
        const we_user_prop_t *p = &proj.props[i];
        printf("  - %s (%s) [\"%s\"]: ", p->key, prop_type_to_str(p->type), p->text);
        if (p->type == WE_PROP_BOOL) {
            printf("%s\n", (p->val[0] != 0.0f) ? "true" : "false");
        } else if (p->type == WE_PROP_TEXT && p->str_val[0] != '\0') {
            printf("\"%s\"\n", p->str_val);
        } else if (p->num_components >= 3) {
            printf("%.3f %.3f %.3f\n", (double)p->val[0], (double)p->val[1], (double)p->val[2]);
        } else {
            printf("%.3f\n", (double)p->val[0]);
        }
    }
    return 0;
}

static int handle_cmd_workshop_sync_previews(const cli_options_t *opts)
{
    const char *target_dir = (opts && opts->filepath[0] != '\0') ? opts->filepath : NULL;
    size_t synced = 0;
    size_t pruned = 0;
    if (!we_workshop_sync_previews(target_dir, &synced, &pruned)) {
        fprintf(stderr, "Error: Failed to sync Steam Workshop previews\n");
        return 1;
    }

    char resolved_dir[4096] = {0};
    if (target_dir && target_dir[0] != '\0') {
        snprintf(resolved_dir, sizeof(resolved_dir), "%s", target_dir);
    } else {
        path_buf_t cache_root;
        if (resolve_wwal_cache_root(&cache_root)) {
            snprintf(resolved_dir, sizeof(resolved_dir), "%s/workshop-previews",
                     path_buf_cstr(&cache_root));
        }
    }
    printf("Synced %zu Workshop preview symlink(s) (pruned %zu stale) in %s\n", synced, pruned,
           resolved_dir[0] ? resolved_dir : "~/.cache/wwal/workshop-previews");
    return 0;
}

typedef struct {
    char display_title[256];
    char type_str[16];
    char display_label[512];
    char target_path[4096];
    char thumb_path[4096];
} pick_item_t;

typedef struct {
    dev_t dev;
    ino_t ino;
} pick_dir_id_t;

typedef struct {
    pick_item_t *items;
    size_t count;
    size_t cap;
    size_t *target_idx_table;
    size_t target_table_cap;
    size_t *shader_idx_table;
    size_t shader_table_cap;
    pick_dir_id_t *dir_table;
    uint8_t *dir_used;
    size_t dir_count;
    size_t dir_cap;
} pick_list_t;

static uint64_t pick_fnv1a_64_str(const char *s, bool ascii_lower)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    if (!s) {
        return h;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        unsigned char c = *p;
        if (ascii_lower && c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t pick_fnv1a_64_title_type(const char *title, const char *type_str)
{
    uint64_t h = pick_fnv1a_64_str(title, false);
    h ^= 0xffU;
    h *= 0x100000001b3ULL;
    for (const unsigned char *p = (const unsigned char *)(type_str ? type_str : ""); *p != '\0';
         ++p) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void pick_list_free(pick_list_t *list)
{
    if (!list) {
        return;
    }
    free(list->items);
    free(list->target_idx_table);
    free(list->shader_idx_table);
    free(list->dir_table);
    free(list->dir_used);
    memset(list, 0, sizeof(*list));
}

static bool pick_list_mark_dir_visited(pick_list_t *list, dev_t dev, ino_t ino)
{
    if (!list) {
        return true;
    }
    if (list->dir_cap == 0 || (list->dir_count + 1U) * 10U >= list->dir_cap * 7U) {
        size_t new_cap = (list->dir_cap == 0) ? 32U : (list->dir_cap * 2U);
        pick_dir_id_t *ntab = (pick_dir_id_t *)calloc(new_cap, sizeof(*ntab));
        uint8_t *nused = (uint8_t *)calloc(new_cap, sizeof(*nused));
        if (!ntab || !nused) {
            free(ntab);
            free(nused);
            return true;
        }
        size_t mask = new_cap - 1U;
        for (size_t i = 0; i < list->dir_cap; ++i) {
            if (!list->dir_used[i]) {
                continue;
            }
            uint64_t hv = (uint64_t)list->dir_table[i].dev ^
                          ((uint64_t)list->dir_table[i].ino * 0x9e3779b97f4a7c15ULL);
            hv ^= hv >> 30;
            size_t slot = (size_t)(hv & mask);
            while (nused[slot]) {
                slot = (slot + 1U) & mask;
            }
            nused[slot] = 1U;
            ntab[slot] = list->dir_table[i];
        }
        free(list->dir_table);
        free(list->dir_used);
        list->dir_table = ntab;
        list->dir_used = nused;
        list->dir_cap = new_cap;
    }

    size_t mask = list->dir_cap - 1U;
    uint64_t hv = (uint64_t)dev ^ ((uint64_t)ino * 0x9e3779b97f4a7c15ULL);
    hv ^= hv >> 30;
    size_t slot = (size_t)(hv & mask);
    while (list->dir_used[slot]) {
        if (list->dir_table[slot].dev == dev && list->dir_table[slot].ino == ino) {
            return false;
        }
        slot = (slot + 1U) & mask;
    }
    list->dir_used[slot] = 1U;
    list->dir_table[slot].dev = dev;
    list->dir_table[slot].ino = ino;
    list->dir_count++;
    return true;
}

static bool pick_list_ensure_string_tables(pick_list_t *list)
{
    if (list->target_table_cap > 0 &&
        (list->count + 1U) * 10U < list->target_table_cap * 7U) {
        return true;
    }
    size_t new_cap = (list->target_table_cap == 0) ? 64U : (list->target_table_cap * 2U);
    size_t *ntarget = (size_t *)calloc(new_cap, sizeof(size_t));
    size_t *nshader = (size_t *)calloc(new_cap, sizeof(size_t));
    if (!ntarget || !nshader) {
        free(ntarget);
        free(nshader);
        return false;
    }
    size_t mask = new_cap - 1U;
    for (size_t i = 0; i < list->count; ++i) {
        uint64_t th = pick_fnv1a_64_str(list->items[i].target_path, false);
        size_t tslot = (size_t)(th & mask);
        while (ntarget[tslot] != 0) {
            tslot = (tslot + 1U) & mask;
        }
        ntarget[tslot] = i + 1U;

        if (strcmp(list->items[i].type_str, "shader") == 0) {
            uint64_t sh = pick_fnv1a_64_str(list->items[i].display_title, true);
            size_t sslot = (size_t)(sh & mask);
            while (nshader[sslot] != 0) {
                sslot = (sslot + 1U) & mask;
            }
            nshader[sslot] = i + 1U;
        }
    }
    free(list->target_idx_table);
    free(list->shader_idx_table);
    list->target_idx_table = ntarget;
    list->shader_idx_table = nshader;
    list->target_table_cap = new_cap;
    list->shader_table_cap = new_cap;
    return true;
}

static void sanitize_pick_title(const char *raw, char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return;
    out[0] = '\0';
    if (!raw)
        return;
    size_t j = 0;
    size_t i = 0;
    while (raw[i] != '\0' && j + 1 < out_cap) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '\n' || c == '\r' || c == '\t' || c < 0x20 || c == 0x7F) {
            if (j > 0 && out[j - 1] != ' ') {
                out[j++] = ' ';
            }
            i++;
        } else if (c < 0x80) {
            out[j++] = (char)c;
            i++;
        } else {
            size_t seq_len = 0;
            if ((c & 0xE0) == 0xC0) {
                seq_len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                seq_len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                seq_len = 4;
            } else {
                i++;
                continue;
            }
            bool valid = true;
            for (size_t k = 1; k < seq_len; ++k) {
                unsigned char cc = (unsigned char)raw[i + k];
                if (cc == '\0' || (cc & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                i++;
                continue;
            }
            if (j + seq_len >= out_cap) {
                break;
            }
            for (size_t k = 0; k < seq_len; ++k) {
                out[j++] = raw[i + k];
            }
            i += seq_len;
        }
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, out_cap, "Untitled");
    }
}

static void strip_file_extension_title(const char *filename, char *out, size_t out_cap)
{
    if (!filename || !out || out_cap == 0)
        return;
    const char *slash = strrchr(filename, '/');
    const char *base = slash ? (slash + 1) : filename;
    const char *dot = strrchr(base, '.');
    size_t len = (dot && dot > base) ? (size_t)(dot - base) : strlen(base);
    char tmp[512];
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
        while (len > 0 && ((unsigned char)base[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(tmp, base, len);
    tmp[len] = '\0';
    sanitize_pick_title(tmp, out, out_cap);
}

static bool pick_list_add(pick_list_t *list, const char *title, const char *type_str,
                          const char *target_path, const char *thumb_path)
{
    if (!list || !target_path || target_path[0] == '\0' || !type_str)
        return false;

    char canon_target[4096];
    if (realpath(target_path, canon_target) == NULL) {
        snprintf(canon_target, sizeof(canon_target), "%s", target_path);
    }

    char clean_title[256];
    sanitize_pick_title(title, clean_title, sizeof(clean_title));

    if (!pick_list_ensure_string_tables(list)) {
        return false;
    }
    size_t mask = list->target_table_cap - 1U;
    uint64_t th = pick_fnv1a_64_str(canon_target, false);
    size_t tslot = (size_t)(th & mask);
    while (list->target_idx_table[tslot] != 0U) {
        size_t idx = list->target_idx_table[tslot] - 1U;
        if (strcmp(list->items[idx].target_path, canon_target) == 0) {
            return false;
        }
        tslot = (tslot + 1U) & mask;
    }

    bool is_shader = (strcmp(type_str, "shader") == 0);
    size_t sslot = 0;
    if (is_shader) {
        uint64_t sh = pick_fnv1a_64_str(clean_title, true);
        sslot = (size_t)(sh & mask);
        while (list->shader_idx_table[sslot] != 0U) {
            size_t idx = list->shader_idx_table[sslot] - 1U;
            if (strcmp(list->items[idx].type_str, "shader") == 0 &&
                strcasecmp(list->items[idx].display_title, clean_title) == 0) {
                return false;
            }
            sslot = (sslot + 1U) & mask;
        }
    }

    if (list->count + 1 > list->cap) {
        size_t ncap = list->cap == 0 ? 32 : list->cap * 2;
        pick_item_t *ni = (pick_item_t *)realloc(list->items, ncap * sizeof(pick_item_t));
        if (!ni) {
            return false;
        }
        list->items = ni;
        list->cap = ncap;
    }

    size_t new_idx = list->count++;
    pick_item_t *it = &list->items[new_idx];
    memset(it, 0, sizeof(*it));
    snprintf(it->display_title, sizeof(it->display_title), "%s", clean_title);
    snprintf(it->type_str, sizeof(it->type_str), "%s", type_str);
    snprintf(it->display_label, sizeof(it->display_label), "%s [%s]", it->display_title,
             it->type_str);
    snprintf(it->target_path, sizeof(it->target_path), "%s", canon_target);
    if (thumb_path && thumb_path[0] != '\0') {
        char canon_thumb[4096];
        if (realpath(thumb_path, canon_thumb) != NULL) {
            snprintf(it->thumb_path, sizeof(it->thumb_path), "%s", canon_thumb);
        } else {
            snprintf(it->thumb_path, sizeof(it->thumb_path), "%s", thumb_path);
        }
    } else {
        snprintf(it->thumb_path, sizeof(it->thumb_path), "%s", canon_target);
    }

    list->target_idx_table[tslot] = new_idx + 1U;
    if (is_shader) {
        list->shader_idx_table[sslot] = new_idx + 1U;
    }
    return true;
}

static bool pick_dir_has_workshop_marker(const char *dir)
{
    char probe[4096];
    struct stat st;
    if (snprintf(probe, sizeof(probe), "%s/project.json", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/scene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    if (snprintf(probe, sizeof(probe), "%s/gifscene.pkg", dir) < (int)sizeof(probe) &&
        stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    return false;
}

static void pick_add_workshop_dir(pick_list_t *list, const char *ws_dir, bool no_cache)
{
    if (!list || !ws_dir || ws_dir[0] == '\0')
        return;

    we_project_t proj;
    char title[256] = "Workshop Scene";
    if (we_project_load_dir(ws_dir, &proj) && proj.title[0] != '\0') {
        snprintf(title, sizeof(title), "%s", proj.title);
    } else {
        strip_file_extension_title(ws_dir, title, sizeof(title));
    }

    char thumb[4096] = {0};
    if (!we_workshop_find_preview_file(ws_dir, thumb, sizeof(thumb))) {
        (void)ensure_shader_thumbnail_png(ws_dir, no_cache, thumb, sizeof(thumb));
    }
    (void)pick_list_add(list, title, "workshop", ws_dir, thumb);
}

static void scan_pick_directory(const char *dir_path, int depth, pick_list_t *list, bool no_cache)
{
    if (!dir_path || dir_path[0] == '\0' || depth > 3 || !list)
        return;

    struct stat dst;
    if (stat(dir_path, &dst) != 0 || !S_ISDIR(dst.st_mode))
        return;
    if (!pick_list_mark_dir_visited(list, dst.st_dev, dst.st_ino))
        return;

    if (pick_dir_has_workshop_marker(dir_path)) {
        pick_add_workshop_dir(list, dir_path, no_cache);
        return;
    }

    DIR *d = opendir(dir_path);
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char full[4096];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full))
            continue;

        struct stat st;
        if (stat(full, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (pick_dir_has_workshop_marker(full)) {
                pick_add_workshop_dir(list, full, no_cache);
            } else {
                scan_pick_directory(full, depth + 1, list, no_cache);
            }
            continue;
        }

        if (!S_ISREG(st.st_mode))
            continue;

        struct stat lst;
        bool is_link = (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode));
        if (is_link) {
            char ws_dir[4096];
            if (we_workshop_resolve_dir(full, ws_dir, sizeof(ws_dir))) {
                pick_add_workshop_dir(list, ws_dir, no_cache);
                continue;
            }
        }

        char title[256];
        strip_file_extension_title(de->d_name, title, sizeof(title));

        if (is_supported_image_file_ext(full)) {
            (void)pick_list_add(list, title, "image", full, full);
        } else if (is_video_container_file(full)) {
            char thumb[4096] = {0};
            (void)ensure_video_thumbnail_png(full, no_cache, thumb, sizeof(thumb));
            (void)pick_list_add(list, title, "video", full, thumb[0] ? thumb : full);
        } else if (is_shader_source_file(full)) {
            char thumb[4096] = {0};
            (void)ensure_shader_thumbnail_png(full, no_cache, thumb, sizeof(thumb));
            (void)pick_list_add(list, title, "shader", full, thumb[0] ? thumb : full);
        }
    }
    closedir(d);
}

typedef struct {
    size_t first_idx_plus_one;
    size_t count;
} pick_label_slot_t;

static void pick_disambiguate_labels(pick_list_t *list)
{
    if (!list || list->count <= 1U) {
        return;
    }
    size_t cap = 16U;
    while (cap < list->count * 2U) {
        if (cap > SIZE_MAX / 2U) {
            return;
        }
        cap *= 2U;
    }
    pick_label_slot_t *slots = (pick_label_slot_t *)calloc(cap, sizeof(*slots));
    if (!slots) {
        return;
    }
    size_t mask = cap - 1U;

    for (size_t i = 0; i < list->count; ++i) {
        uint64_t h = pick_fnv1a_64_title_type(list->items[i].display_title, list->items[i].type_str);
        size_t s = (size_t)(h & mask);
        while (slots[s].first_idx_plus_one != 0U) {
            const pick_item_t *ref = &list->items[slots[s].first_idx_plus_one - 1U];
            if (strcmp(ref->display_title, list->items[i].display_title) == 0 &&
                strcmp(ref->type_str, list->items[i].type_str) == 0) {
                break;
            }
            s = (s + 1U) & mask;
        }
        if (slots[s].first_idx_plus_one == 0U) {
            slots[s].first_idx_plus_one = i + 1U;
        }
        slots[s].count++;
    }

    for (size_t i = 0; i < list->count; ++i) {
        pick_item_t *it = &list->items[i];
        uint64_t h = pick_fnv1a_64_title_type(it->display_title, it->type_str);
        size_t s = (size_t)(h & mask);
        while (slots[s].first_idx_plus_one != 0U) {
            const pick_item_t *ref = &list->items[slots[s].first_idx_plus_one - 1U];
            if (strcmp(ref->display_title, it->display_title) == 0 &&
                strcmp(ref->type_str, it->type_str) == 0) {
                break;
            }
            s = (s + 1U) & mask;
        }
        if (slots[s].count <= 1U) {
            continue;
        }
        const char *slash = strrchr(it->target_path, '/');
        const char *hint = slash ? (slash + 1) : it->target_path;
        char hint_buf[128];
        if (strcmp(it->type_str, "workshop") != 0 && slash && slash > it->target_path) {
            const char *prev = slash - 1;
            while (prev > it->target_path && *prev != '/') {
                prev--;
            }
            const char *parent_start = (*prev == '/') ? (prev + 1) : prev;
            if (parent_start < slash) {
                snprintf(hint_buf, sizeof(hint_buf), "%.*s/%s", (int)(slash - parent_start),
                         parent_start, slash + 1);
                hint = hint_buf;
            }
        }
        char clean_hint[128];
        sanitize_pick_title(hint, clean_hint, sizeof(clean_hint));
        snprintf(it->display_label, sizeof(it->display_label), "%s [%s - %s]", it->display_title,
                 it->type_str, clean_hint);
    }

    memset(slots, 0, cap * sizeof(*slots));
    for (size_t i = 0; i < list->count; ++i) {
        pick_item_t *it = &list->items[i];
        uint64_t h = pick_fnv1a_64_str(it->display_label, false);
        size_t s = (size_t)(h & mask);
        while (slots[s].first_idx_plus_one != 0U) {
            const pick_item_t *ref = &list->items[slots[s].first_idx_plus_one - 1U];
            if (strcmp(ref->display_label, it->display_label) == 0) {
                break;
            }
            s = (s + 1U) & mask;
        }
        if (slots[s].first_idx_plus_one == 0U) {
            slots[s].first_idx_plus_one = i + 1U;
            slots[s].count = 1U;
            continue;
        }
        char base_label[512];
        client_copy_cstr(base_label, sizeof(base_label), it->display_label);
        size_t dup_idx = ++slots[s].count;
        for (;;) {
            snprintf(it->display_label, sizeof(it->display_label), "%.480s #%zu", base_label,
                     dup_idx);
            uint64_t dh = pick_fnv1a_64_str(it->display_label, false);
            size_t ds = (size_t)(dh & mask);
            while (slots[ds].first_idx_plus_one != 0U) {
                const pick_item_t *ref = &list->items[slots[ds].first_idx_plus_one - 1U];
                if (strcmp(ref->display_label, it->display_label) == 0) {
                    break;
                }
                ds = (ds + 1U) & mask;
            }
            if (slots[ds].first_idx_plus_one == 0U) {
                slots[ds].first_idx_plus_one = i + 1U;
                slots[ds].count = 1U;
                break;
            }
            dup_idx++;
        }
    }
    free(slots);
}

static int cmp_pick_items(const void *a, const void *b)
{
    const pick_item_t *pa = (const pick_item_t *)a;
    const pick_item_t *pb = (const pick_item_t *)b;
    int c = strcasecmp(pa->display_label, pb->display_label);
    if (c != 0)
        return c;
    return strcmp(pa->target_path, pb->target_path);
}

static void print_json_escaped(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        unsigned char c = *p;
        if (c == '"') {
            fputs("\\\"", out);
        } else if (c == '\\') {
            fputs("\\\\", out);
        } else if (c == '\n') {
            fputs("\\n", out);
        } else if (c == '\r') {
            fputs("\\r", out);
        } else if (c == '\t') {
            fputs("\\t", out);
        } else if (c >= 0x20) {
            fputc((int)c, out);
        }
    }
    fputc('"', out);
}

static volatile sig_atomic_t g_pick_child_interrupted = 0;
static void pick_child_sig_handler(int sig)
{
    (void)sig;
    g_pick_child_interrupted = 1;
}

static int handle_cmd_pick(cli_options_t *opts, bool *out_should_execute)
{
    if (out_should_execute) {
        *out_should_execute = false;
    }
    if (!opts) {
        return 1;
    }

    pick_list_t list = {0};

    if (opts->pick_dir[0] != '\0') {
        scan_pick_directory(opts->pick_dir, 0, &list, opts->no_cache);
    } else {
        we_project_t *ws_items = NULL;
        size_t ws_count = 0;
        if (we_workshop_scan_all(&ws_items, &ws_count)) {
            for (size_t i = 0; i < ws_count; ++i) {
                char thumb[4096] = {0};
                if (!we_workshop_find_preview_file(ws_items[i].workshop_dir, thumb,
                                                   sizeof(thumb))) {
                    (void)ensure_shader_thumbnail_png(ws_items[i].workshop_dir, opts->no_cache,
                                                      thumb, sizeof(thumb));
                }
                (void)pick_list_add(
                    &list, ws_items[i].title[0] ? ws_items[i].title : ws_items[i].workshop_id,
                    "workshop", ws_items[i].workshop_dir, thumb);
            }
            free(ws_items);
        }

        const char *home = getenv("HOME");
        const char *xdg_pics = getenv("XDG_PICTURES_DIR");
        const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
        char candidate[4096];

        path_buf_t cache_root;
        if (resolve_wwal_cache_root(&cache_root)) {
            path_buf_t ws_prev_dir = cache_root;
            if (path_buf_push(&ws_prev_dir, "workshop-previews")) {
                scan_pick_directory(path_buf_cstr(&ws_prev_dir), 0, &list, opts->no_cache);
            }
            path_buf_t ws_alias_dir = cache_root;
            if (path_buf_push(&ws_alias_dir, "workshop")) {
                scan_pick_directory(path_buf_cstr(&ws_alias_dir), 0, &list, opts->no_cache);
            }
        }

        if (xdg_pics && xdg_pics[0] != '\0') {
            snprintf(candidate, sizeof(candidate), "%s/Wallpapers", xdg_pics);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            scan_pick_directory(xdg_pics, 0, &list, opts->no_cache);
        }
        waywal_shader_preset_info_t presets[32];
        size_t preset_count = waywal_shader_preset_list(presets, 32);
        for (size_t i = 0; i < preset_count; ++i) {
            if (presets[i].path[0] != '\0' && access(presets[i].path, R_OK) == 0) {
                char thumb[4096] = {0};
                (void)ensure_shader_thumbnail_png(presets[i].path, opts->no_cache, thumb,
                                                  sizeof(thumb));
                (void)pick_list_add(&list, presets[i].name, "shader", presets[i].path,
                                    thumb[0] ? thumb : presets[i].path);
            }
        }

        if (home && home[0] != '\0') {
            snprintf(candidate, sizeof(candidate), "%s/.cache/wwal/workshop-previews", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            snprintf(candidate, sizeof(candidate), "%s/.cache/wwal/workshop", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            snprintf(candidate, sizeof(candidate), "%s/Pictures/Wallpapers", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            snprintf(candidate, sizeof(candidate), "%s/Wallpapers", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            snprintf(candidate, sizeof(candidate), "%s/.local/share/wallpapers", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
            snprintf(candidate, sizeof(candidate), "%s/.config/wwal/shaders", home);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
        }
        if (xdg_cfg && xdg_cfg[0] != '\0') {
            snprintf(candidate, sizeof(candidate), "%s/wwal/shaders", xdg_cfg);
            scan_pick_directory(candidate, 0, &list, opts->no_cache);
        }
        scan_pick_directory("contrib/shaders", 0, &list, opts->no_cache);
        scan_pick_directory("/usr/share/wwal/shaders", 0, &list, opts->no_cache);
    }

    if (list.count > 1) {
        qsort(list.items, list.count, sizeof(pick_item_t), cmp_pick_items);
        pick_disambiguate_labels(&list);
    }

    waywal_pick_menu_t mode = opts->pick_menu;
    if (opts->json_output && mode == WAYWAL_PICK_MENU_AUTO) {
        mode = WAYWAL_PICK_MENU_JSON;
    }

    if (mode == WAYWAL_PICK_MENU_STDOUT) {
        for (size_t i = 0; i < list.count; ++i) {
            const pick_item_t *it = &list.items[i];
            fwrite(it->display_label, 1, strlen(it->display_label), stdout);
            fputc('\0', stdout);
            fprintf(stdout, "icon\x1f%s\x1finfo\x1f%s\n", it->thumb_path, it->target_path);
        }
        fflush(stdout);
        pick_list_free(&list);
        return 0;
    }

    if (mode == WAYWAL_PICK_MENU_JSON) {
        fputs("[", stdout);
        for (size_t i = 0; i < list.count; ++i) {
            const pick_item_t *it = &list.items[i];
            if (i > 0)
                fputs(",", stdout);
            fputs("{\"title\":", stdout);
            print_json_escaped(stdout, it->display_title);
            fputs(",\"type\":", stdout);
            print_json_escaped(stdout, it->type_str);
            fputs(",\"path\":", stdout);
            print_json_escaped(stdout, it->target_path);
            fputs(",\"icon\":", stdout);
            print_json_escaped(stdout, it->thumb_path);
            fputs("}", stdout);
        }
        fputs("]\n", stdout);
        fflush(stdout);
        pick_list_free(&list);
        return 0;
    }

    if (list.count == 0) {
        fprintf(stderr, "Error: No wallpapers, Workshop items, videos, or shaders found to pick\n");
        pick_list_free(&list);
        return 1;
    }

    if (mode == WAYWAL_PICK_MENU_AUTO) {
        if (executable_exists_in_path("rofi")) {
            mode = WAYWAL_PICK_MENU_ROFI;
        } else if (executable_exists_in_path("fuzzel")) {
            mode = WAYWAL_PICK_MENU_FUZZEL;
        } else if (executable_exists_in_path("walker")) {
            mode = WAYWAL_PICK_MENU_WALKER;
        } else if (executable_exists_in_path("dmenu")) {
            mode = WAYWAL_PICK_MENU_DMENU;
        } else {
            fprintf(stderr, "Error: No supported launcher (rofi, fuzzel, walker, dmenu) found in "
                            "PATH. Use --menu stdout or --menu json.\n");
            pick_list_free(&list);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    g_pick_child_interrupted = 0;
    struct sigaction sa, old_sa_int, old_sa_term;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = pick_child_sig_handler;
    sigaction(SIGINT, &sa, &old_sa_int);
    sigaction(SIGTERM, &sa, &old_sa_term);

    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0) {
        sigaction(SIGINT, &old_sa_int, NULL);
        sigaction(SIGTERM, &old_sa_term, NULL);
        pick_list_free(&list);
        return 1;
    }
    if (pipe(from_child) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        sigaction(SIGINT, &old_sa_int, NULL);
        sigaction(SIGTERM, &old_sa_term, NULL);
        pick_list_free(&list);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        sigaction(SIGINT, &old_sa_int, NULL);
        sigaction(SIGTERM, &old_sa_term, NULL);
        pick_list_free(&list);
        return 1;
    }

    if (pid == 0) {
        sigaction(SIGINT, &old_sa_int, NULL);
        sigaction(SIGTERM, &old_sa_term, NULL);
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGPIPE, SIG_DFL);
        close(to_child[1]);
        close(from_child[0]);
        if (to_child[0] != STDIN_FILENO) {
            dup2(to_child[0], STDIN_FILENO);
            close(to_child[0]);
        }
        if (from_child[1] != STDOUT_FILENO) {
            dup2(from_child[1], STDOUT_FILENO);
            close(from_child[1]);
        }
        if (mode == WAYWAL_PICK_MENU_ROFI) {
            char *const m_argv[] = {"rofi", "-dmenu", "-i", "-show-icons", "-p", "Wallpaper", NULL};
            execvp("rofi", m_argv);
        } else if (mode == WAYWAL_PICK_MENU_FUZZEL) {
            char *const m_argv[] = {"fuzzel", "--dmenu", "-p", "Wallpaper: ", NULL};
            execvp("fuzzel", m_argv);
        } else if (mode == WAYWAL_PICK_MENU_WALKER) {
            char *const m_argv[] = {"walker", "--dmenu", NULL};
            execvp("walker", m_argv);
        } else {
            char *const m_argv[] = {"dmenu", "-i", "-p", "Wallpaper", NULL};
            execvp("dmenu", m_argv);
        }
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);

    for (size_t i = 0; i < list.count; ++i) {
        const pick_item_t *it = &list.items[i];
        char row[9000];
        if (mode == WAYWAL_PICK_MENU_DMENU || mode == WAYWAL_PICK_MENU_WALKER) {
            int rlen = snprintf(row, sizeof(row), "%s\n", it->display_label);
            if (rlen > 0 && (size_t)rlen < sizeof(row)) {
                if (!client_write_exact(to_child[1], row, (size_t)rlen)) {
                    break;
                }
            }
        } else if (mode == WAYWAL_PICK_MENU_FUZZEL) {
            char fuzzel_png[4096] = {0};
            const char *icon_path = it->thumb_path;
            if (ensure_image_thumbnail_png(it->thumb_path, opts->no_cache, fuzzel_png,
                                           sizeof(fuzzel_png)) &&
                fuzzel_png[0] != '\0') {
                icon_path = fuzzel_png;
            }
            size_t l_len = strlen(it->display_label);
            if (l_len + 2 >= sizeof(row)) {
                continue;
            }
            memcpy(row, it->display_label, l_len);
            row[l_len] = '\0';
            int tail =
                snprintf(row + l_len + 1, sizeof(row) - l_len - 1, "icon\x1f%s\n", icon_path);
            if (tail > 0 && l_len + 1 + (size_t)tail < sizeof(row)) {
                size_t total_row = l_len + 1 + (size_t)tail;
                if (!client_write_exact(to_child[1], row, total_row)) {
                    break;
                }
            }
        } else {
            size_t l_len = strlen(it->display_label);
            if (l_len + 2 >= sizeof(row)) {
                continue;
            }
            memcpy(row, it->display_label, l_len);
            row[l_len] = '\0';
            int tail = snprintf(row + l_len + 1, sizeof(row) - l_len - 1,
                                "icon\x1f%s\x1finfo\x1f%s\n", it->thumb_path, it->target_path);
            if (tail > 0 && l_len + 1 + (size_t)tail < sizeof(row)) {
                size_t total_row = l_len + 1 + (size_t)tail;
                if (!client_write_exact(to_child[1], row, total_row)) {
                    break;
                }
            }
        }
    }
    close(to_child[1]);

    char sel_buf[8192];
    size_t sel_len = 0;
    while (sel_len + 1 < sizeof(sel_buf)) {
        if (g_pick_child_interrupted)
            break;
        ssize_t r = read(from_child[0], sel_buf + sel_len, sizeof(sel_buf) - 1 - sel_len);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_pick_child_interrupted)
                    break;
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                client_poll_fd(from_child[0], POLLIN, 500)) {
                continue;
            }
            break;
        }
        if (r == 0)
            break;
        sel_len += (size_t)r;
    }
    close(from_child[0]);
    sel_buf[sel_len] = '\0';

    int status = 0;
    pid_t wres = -1;
    bool reaped = false;
    bool signaled = false;
    while (!reaped) {
        if (g_pick_child_interrupted && !signaled) {
            (void)kill(pid, SIGTERM);
            signaled = true;
        }
        wres = waitpid(pid, &status, 0);
        if (wres == pid) {
            reaped = true;
            break;
        }
        if (wres < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    sigaction(SIGINT, &old_sa_int, NULL);
    sigaction(SIGTERM, &old_sa_term, NULL);

    if (g_pick_child_interrupted || !reaped) {
        if (!reaped) {
            (void)kill(pid, SIGTERM);
            do {
                wres = waitpid(pid, &status, 0);
            } while (wres < 0 && errno == EINTR);
        }
        pick_list_free(&list);
        return 130;
    }

    if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || sel_len == 0) {
        pick_list_free(&list);
        return 0;
    }

    while (sel_len > 0 && (sel_buf[sel_len - 1] == '\n' || sel_buf[sel_len - 1] == '\r')) {
        sel_buf[--sel_len] = '\0';
    }

    const char *selected_target = NULL;

    if (sel_len > 5) {
        const char *info_marker = (const char *)memmem(sel_buf, sel_len, "info\x1f", 5);
        if (info_marker) {
            const char *val_start = info_marker + 5;
            size_t rem = sel_len - (size_t)(val_start - sel_buf);
            const char *end = (const char *)memchr(val_start, '\x1f', rem);
            if (!end)
                end = (const char *)memchr(val_start, '\n', rem);
            if (!end)
                end = (const char *)memchr(val_start, '\0', rem);
            size_t vlen = end ? (size_t)(end - val_start) : rem;
            if (vlen > 0 && vlen < sizeof(opts->filepath)) {
                memcpy(opts->filepath, val_start, vlen);
                opts->filepath[vlen] = '\0';
                selected_target = opts->filepath;
            }
        }
    }

    if (!selected_target) {
        char clean_sel[1024];
        client_copy_cstr(clean_sel, sizeof(clean_sel), sel_buf);
        size_t clen = strlen(clean_sel);
        while (clen > 0 && (clean_sel[clen - 1] == '\n' || clean_sel[clen - 1] == '\r' ||
                            clean_sel[clen - 1] == ' ')) {
            clean_sel[--clen] = '\0';
        }
        for (size_t i = 0; i < list.count; ++i) {
            if (strcmp(list.items[i].display_label, clean_sel) == 0 ||
                strcmp(list.items[i].target_path, clean_sel) == 0) {
                selected_target = list.items[i].target_path;
                break;
            }
        }
        if (!selected_target) {
            for (size_t i = 0; i < list.count; ++i) {
                if (strcmp(list.items[i].display_title, clean_sel) == 0) {
                    selected_target = list.items[i].target_path;
                    break;
                }
            }
        }
        if (!selected_target && clean_sel[0] == '/' && access(clean_sel, F_OK) == 0) {
            snprintf(opts->filepath, sizeof(opts->filepath), "%s", clean_sel);
            selected_target = opts->filepath;
        }
    }

    if (!selected_target && getenv("ROFI_RETV") != NULL) {
        const char *rofi_info = getenv("ROFI_INFO");
        if (rofi_info && rofi_info[0] != '\0' && access(rofi_info, F_OK) == 0) {
            selected_target = rofi_info;
        }
    }

    if (!selected_target || selected_target[0] == '\0') {
        fprintf(stderr, "Error: Could not resolve selected wallpaper '%s'\n", sel_buf);
        pick_list_free(&list);
        return 1;
    }

    if (selected_target != opts->filepath) {
        snprintf(opts->filepath, sizeof(opts->filepath), "%s", selected_target);
    }
    opts->cmd = CLI_CMD_IMG;
    if (out_should_execute) {
        *out_should_execute = true;
    }
    pick_list_free(&list);
    return 0;
}

static int handle_cmd_shader_list(const cli_options_t *opts)
{
    waywal_shader_preset_info_t presets[32];
    size_t count = waywal_shader_preset_list(presets, 32);

    if (opts && opts->json_output) {
        printf("{\"presets\":[");
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) {
                putchar(',');
            }
            printf("{\"name\":");
            print_json_escaped(stdout, presets[i].name);
            printf(",\"title\":");
            print_json_escaped(stdout, presets[i].title[0] ? presets[i].title : presets[i].name);
            printf(",\"description\":");
            print_json_escaped(stdout, presets[i].description);
            printf(",\"author\":");
            print_json_escaped(stdout, presets[i].author[0] ? presets[i].author : "WayWal");
            printf(",\"audio_reactive\":%s,\"is_builtin\":%s,\"path\":",
                   presets[i].audio_reactive ? "true" : "false",
                   presets[i].is_builtin ? "true" : "false");
            print_json_escaped(stdout, presets[i].path);
            putchar('}');
        }
        printf("]}\n");
        return 0;
    }

    printf("Built-in Procedural Shader Presets (%zu):\n", count);
    printf("  %-16s %-6s %s\n", "NAME", "AUDIO", "DESCRIPTION");
    for (size_t i = 0; i < count; ++i) {
        printf("  %-16s %-6s %s\n", presets[i].name, presets[i].audio_reactive ? "yes" : "no",
               presets[i].description);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    waywal_config_t cfg;
    (void)waywal_config_load_default(&cfg);

    cli_options_t opts;
    if (!cli_parse(argc, argv, &opts)) {
        cli_print_usage(argv[0]);
        return 1;
    }

    if (opts.cmd == CLI_CMD_VERSION) {
        printf("wwal %s\n", WAYWAL_VERSION);
        return 0;
    }

    if (opts.cmd == CLI_CMD_HELP || opts.cmd == CLI_CMD_NONE) {
        cli_print_usage(argv[0]);
        return 0;
    }

    if (opts.cmd == CLI_CMD_SHADER && opts.shader_list) {
        return handle_cmd_shader_list(&opts);
    }

    apply_config_defaults_to_cli(argc, argv, &cfg, &opts);
    if (opts.has_coordinates) {
        cfg.solar.latitude = opts.latitude;
        cfg.solar.longitude = opts.longitude;
        cfg.solar.has_coordinates = true;
    }
    waywal_state_store_set_namespace(opts.namespace_str);

    if (opts.verbose) {
        waywal_log_set_level(WAYWAL_LOG_LEVEL_DEBUG);
    }

    if (opts.clear_cache && opts.cmd != CLI_CMD_CLEAR_CACHE) {
        (void)handle_cmd_clear_cache();
    }

    if (opts.cmd == CLI_CMD_PICK) {
        bool execute_picked = false;
        int pick_rc = handle_cmd_pick(&opts, &execute_picked);
        if (pick_rc != 0 || !execute_picked) {
            return pick_rc;
        }
    }

    (void)cli_auto_dispatch_img(&opts);

    if (opts.cmd == CLI_CMD_THEME) {
        return handle_cmd_theme(&opts, &cfg);
    }

    if (opts.cmd == CLI_CMD_CLEAR_CACHE) {
        return handle_cmd_clear_cache();
    }

    bool workshop_theme_applied = false;
    if (opts.cmd == CLI_CMD_WORKSHOP) {
        if (opts.workshop_subcmd == WAYWAL_WORKSHOP_LIST) {
            return handle_cmd_workshop_list();
        }
        if (opts.workshop_subcmd == WAYWAL_WORKSHOP_INFO) {
            return handle_cmd_workshop_info(&opts);
        }
        if (opts.workshop_subcmd == WAYWAL_WORKSHOP_SYNC_PREVIEWS) {
            return handle_cmd_workshop_sync_previews(&opts);
        }

        char resolved_dir[4096];
        if (!we_workshop_resolve_dir(opts.filepath, resolved_dir, sizeof(resolved_dir))) {
            fprintf(stderr, "Error: Workshop item '%s' not found\n", opts.filepath);
            return 1;
        }
        char abs_dir[PATH_MAX];
        if (realpath(resolved_dir, abs_dir)) {
            snprintf(resolved_dir, sizeof(resolved_dir), "%s", abs_dir);
        }

        we_project_t proj;
        if (!we_project_load_dir(resolved_dir, &proj)) {
            fprintf(stderr, "Error: Failed to parse project.json in '%s'\n", resolved_dir);
            return 1;
        }

        if ((opts.extract_theme || opts.has_seed_color || cfg.theme.enabled) && !opts.no_theme) {
            waywal_theme_mode_t tmode =
                opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
            waywal_palette_t pal;
            if (opts.has_seed_color || cfg.theme.has_seed_color) {
                if (extract_palette_from_bgra(NULL, 0, 0, tmode, &opts, &cfg, resolved_dir, &pal)) {
                    apply_extracted_palette_client(&pal, &opts, &cfg);
                    workshop_theme_applied = true;
                }
            } else {
                char prev_path[4600];
                uint8_t *px = NULL;
                uint32_t pw = 0, ph = 0;
                bool loaded_prev =
                    (we_workshop_find_preview_file(resolved_dir, prev_path, sizeof(prev_path)) &&
                     image_load(prev_path, &px, &pw, &ph) && px && pw > 0 && ph > 0);
                if (!loaded_prev) {
                    loaded_prev = (load_image_or_video_first_frame(resolved_dir, opts.no_cache, &px,
                                                                   &pw, &ph, &cfg.solar) &&
                                   px && pw > 0 && ph > 0);
                }
                if (loaded_prev) {
                    if (extract_palette_from_bgra(px, pw, ph, tmode, &opts, &cfg, resolved_dir,
                                                  &pal)) {
                        apply_extracted_palette_client(&pal, &opts, &cfg);
                        workshop_theme_applied = true;
                    }
                }
                if (px) {
                    image_free(px);
                }
            }
        }

        if (strcasecmp(proj.type, "video") == 0 && proj.file[0] != '\0') {
            int n =
                snprintf(opts.filepath, sizeof(opts.filepath), "%s/%s", resolved_dir, proj.file);
            if (n < 0 || (size_t)n >= sizeof(opts.filepath)) {
                opts.filepath[sizeof(opts.filepath) - 1] = '\0';
            }
            opts.cmd = CLI_CMD_VIDEO;
        } else {
            client_copy_cstr(opts.filepath, sizeof(opts.filepath), resolved_dir);
        }
    }

    if (opts.cmd == CLI_CMD_DAEMON_ALIAS) {
        if (waywal_ipc_namespace_is_alive(opts.namespace_str)) {
            return 0;
        }
        char vol_buf[32];
        char *daemon_argv[32];
        int d_argc = 0;
        daemon_argv[d_argc++] = "wwald";
        if (opts.daemonize && !opts.no_daemon) {
            daemon_argv[d_argc++] = "-d";
        }
        if (opts.namespace_str[0] != '\0') {
            daemon_argv[d_argc++] = "-n";
            daemon_argv[d_argc++] = opts.namespace_str;
        }
        if (opts.verbose) {
            daemon_argv[d_argc++] = "-v";
        }
        if (opts.quiet) {
            daemon_argv[d_argc++] = "-q";
        }
        if (opts.pixel_format[0] != '\0') {
            daemon_argv[d_argc++] = "-f";
            daemon_argv[d_argc++] = opts.pixel_format;
        }
        if (opts.layer_name[0] != '\0') {
            daemon_argv[d_argc++] = "--layer";
            daemon_argv[d_argc++] = opts.layer_name;
        }
        if (opts.backdrop[0] != '\0') {
            daemon_argv[d_argc++] = "--backdrop";
            daemon_argv[d_argc++] = opts.backdrop;
        }
        if (opts.battery_policy[0] != '\0') {
            daemon_argv[d_argc++] = "--battery-policy";
            daemon_argv[d_argc++] = opts.battery_policy;
        }
        if (opts.volume >= 0.0f) {
            snprintf(vol_buf, sizeof(vol_buf), "%.2f", opts.volume);
            daemon_argv[d_argc++] = "--volume";
            daemon_argv[d_argc++] = vol_buf;
        }
        if (opts.mute_on_audio == 1) {
            daemon_argv[d_argc++] = "--mute-on-audio";
        } else if (opts.mute_on_audio == 0) {
            daemon_argv[d_argc++] = "--no-mute-on-audio";
        }
        if (opts.theme_from_media == 1) {
            daemon_argv[d_argc++] = "--theme-from-media";
        } else if (opts.theme_from_media == 0) {
            daemon_argv[d_argc++] = "--no-theme-from-media";
        }
        daemon_argv[d_argc] = NULL;
        exec_sibling_or_path_wwald(daemon_argv);
        fprintf(stderr, "Error: Failed to exec wwald: %s\n", strerror(errno));
        return 1;
    }

    bool promoted_gif = false;
    if (opts.cmd == CLI_CMD_IMG && !opts.read_stdin && !opts.is_solid_color && opts.animate &&
        is_gif_file(opts.filepath)) {
        opts.cmd = CLI_CMD_VIDEO;
        promoted_gif = true;
    }

    bool has_audio_override =
        (opts.cmd != CLI_CMD_AUDIO) &&
        (opts.volume >= 0.0f || opts.mute_on_audio >= 0 || opts.theme_from_media >= 0);
    bool want_persistent = (opts.cmd != CLI_CMD_SUBSCRIBE) &&
                           (opts.all_namespaces || has_audio_override ||
                            (opts.cmd == CLI_CMD_VIDEO && !opts.no_cache));

    cli_ipc_pool_t ipc_pool;
    cli_ipc_pool_init(&ipc_pool);

    char target_namespaces[16][64];
    size_t ns_count = collect_active_namespaces(
        &ipc_pool, &opts, want_persistent && opts.all_namespaces, target_namespaces);

    char effective_video_path[4096];
    effective_video_path[0] = '\0';
    if (opts.cmd == CLI_CMD_VIDEO) {
        bool is_gif = is_gif_file(opts.filepath);
        bool vid_ok = false;
        if (opts.no_cache) {
            if (!is_gif) {
                client_copy_cstr(effective_video_path, sizeof(effective_video_path), opts.filepath);
                vid_ok = true;
            } else {
                fprintf(stderr,
                        "Note: --no-cache skips cached GIF-to-MP4 video transcoding for '%s'; "
                        "using uncached native GIF animation playback instead%s.\n",
                        opts.filepath,
                        (promoted_gif || opts.video_loop_count > 0 || opts.video_speed != 1.0f)
                            ? " (--loop/--speed apply only to cached video playback)"
                            : "");
                opts.cmd = CLI_CMD_IMG;
            }
        } else {
            uint32_t max_w = 2560, max_h = 1440;
            query_max_target_resolution(&ipc_pool, &opts, target_namespaces[0], want_persistent,
                                        &max_w, &max_h);
            if (max_w == 0 || max_h == 0) {
                max_w = 2560;
                max_h = 1440;
            }
            vid_ok = video_transcode_optimize(opts.filepath, max_w, max_h, effective_video_path,
                                              sizeof(effective_video_path));
            if (!vid_ok) {
                if (is_gif) {
                    fprintf(stderr,
                            "Warning: GIF video transcoding failed for '%s'; falling back to "
                            "native GIF image playback.\n",
                            opts.filepath);
                    opts.cmd = CLI_CMD_IMG;
                } else {
                    fprintf(stderr, "Error: Failed to process video/GIF input: %s\n",
                            opts.filepath);
                    cli_ipc_pool_close_all(&ipc_pool);
                    return 1;
                }
            }
        }
        if (vid_ok) {
            if (!opts.no_cache) {
                char vthumb[4096] = {0};
                (void)ensure_video_thumbnail_png(opts.filepath, false, vthumb, sizeof(vthumb));
                if (effective_video_path[0] != '\0' &&
                    strcmp(effective_video_path, opts.filepath) != 0) {
                    if (vthumb[0] != '\0') {
                        uint8_t *tpx = NULL;
                        uint32_t tw = 0, th = 0;
                        path_buf_t croot;
                        if (image_load(vthumb, &tpx, &tw, &th) && tpx &&
                            resolve_wwal_cache_root(&croot)) {
                            path_buf_t tdir = croot;
                            if (path_buf_push(&tdir, "thumbs")) {
                                uint64_t he = hash_file_meta_u64(effective_video_path);
                                char efname[48];
                                snprintf(efname, sizeof(efname), "%016llx.png",
                                         (unsigned long long)he);
                                path_buf_t epng = tdir;
                                if (path_buf_push(&epng, efname)) {
                                    (void)atomic_save_png_bgra(path_buf_cstr(&epng), tpx, tw, th);
                                }
                                char real_e[PATH_MAX];
                                if (realpath(effective_video_path, real_e) != NULL &&
                                    strcmp(real_e, effective_video_path) != 0) {
                                    uint64_t hre = hash_file_meta_u64(real_e);
                                    snprintf(efname, sizeof(efname), "%016llx.png",
                                             (unsigned long long)hre);
                                    path_buf_t repng = tdir;
                                    if (path_buf_push(&repng, efname)) {
                                        (void)atomic_save_png_bgra(path_buf_cstr(&repng), tpx, tw,
                                                                   th);
                                    }
                                }
                            }
                            image_free(tpx);
                        }
                    } else {
                        (void)ensure_video_thumbnail_png(effective_video_path, false, vthumb,
                                                         sizeof(vthumb));
                    }
                }
            }
            if (!workshop_theme_applied &&
                (opts.extract_theme || opts.has_seed_color || cfg.theme.enabled) &&
                !opts.no_theme) {
                waywal_theme_mode_t tmode =
                    opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
                waywal_palette_t pal;
                if (opts.has_seed_color || cfg.theme.has_seed_color) {
                    if (extract_palette_from_bgra(NULL, 0, 0, tmode, &opts, &cfg, opts.filepath,
                                                  &pal)) {
                        apply_extracted_palette_client(&pal, &opts, &cfg);
                    }
                } else {
                    uint8_t *vpx = NULL;
                    uint32_t vpw = 0, vph = 0;
                    if (load_image_or_video_first_frame(opts.filepath, opts.no_cache, &vpx, &vpw,
                                                        &vph, &cfg.solar) &&
                        vpx && vpw > 0 && vph > 0) {
                        if (extract_palette_from_bgra(vpx, vpw, vph, tmode, &opts, &cfg,
                                                      opts.filepath, &pal)) {
                            apply_extracted_palette_client(&pal, &opts, &cfg);
                        }
                    }
                    if (vpx) {
                        image_free(vpx);
                    }
                }
            }
        }
    }

    if (opts.cmd == CLI_CMD_SHADER) {
        char resolve_query[4200];
        if (opts.shader_preset_explicit && strncasecmp(opts.filepath, "preset:", 7) != 0 &&
            strchr(opts.filepath, '/') == NULL && strchr(opts.filepath, '\\') == NULL) {
            snprintf(resolve_query, sizeof(resolve_query), "preset:%s", opts.filepath);
        } else {
            snprintf(resolve_query, sizeof(resolve_query), "%s", opts.filepath);
        }
        char resolved_shader[4096];
        if (waywal_shader_resolve_path(resolve_query, resolved_shader, sizeof(resolved_shader))) {
            snprintf(opts.filepath, sizeof(opts.filepath), "%s", resolved_shader);
            snprintf(opts.custom_shader_path, sizeof(opts.custom_shader_path), "%s",
                     resolved_shader);
        } else if (opts.shader_preset_explicit || strncasecmp(opts.filepath, "preset:", 7) == 0) {
            fprintf(stderr,
                    "Error: Shader preset '%s' not found (run 'wwal shader list' to see presets)\n",
                    opts.filepath);
            cli_ipc_pool_close_all(&ipc_pool);
            return 1;
        }
        if (waywal_shader_is_audio_reactive(opts.filepath)) {
            opts.audio_reactive = true;
        }
        if (!opts.no_cache) {
            char sthumb[4096];
            (void)ensure_shader_thumbnail_png(opts.filepath, false, sthumb, sizeof(sthumb));
        }
        if ((opts.extract_theme || opts.has_seed_color || cfg.theme.enabled) && !opts.no_theme) {
            waywal_theme_mode_t tmode =
                opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
            waywal_palette_t pal;
            if (opts.has_seed_color || cfg.theme.has_seed_color) {
                if (extract_palette_from_bgra(NULL, 0, 0, tmode, &opts, &cfg, opts.filepath,
                                              &pal)) {
                    apply_extracted_palette_client(&pal, &opts, &cfg);
                }
            } else {
                uint8_t *spx = NULL;
                uint32_t spw = 0, sph = 0;
                if (load_image_or_video_first_frame(opts.filepath, opts.no_cache, &spx, &spw, &sph,
                                                    &cfg.solar) &&
                    spx && spw > 0 && sph > 0) {
                    if (extract_palette_from_bgra(spx, spw, sph, tmode, &opts, &cfg, opts.filepath,
                                                  &pal)) {
                        apply_extracted_palette_client(&pal, &opts, &cfg);
                    }
                    image_free(spx);
                }
            }
        }
    }

    if (opts.cmd == CLI_CMD_CLEAR && (opts.extract_theme || opts.has_seed_color) &&
        !opts.no_theme) {
        uint8_t solid_px[4] = {
            opts.clear_color.b,
            opts.clear_color.g,
            opts.clear_color.r,
            255U,
        };
        waywal_theme_mode_t tmode = opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
        waywal_palette_t pal;
        if (extract_palette_from_bgra(solid_px, 1, 1, tmode, &opts, &cfg, NULL, &pal)) {
            apply_extracted_palette_client(&pal, &opts, &cfg);
        }
    }

    char *img_shader_src = NULL;
    size_t img_shader_len = 0;
    uint8_t *img_pixels = NULL;
    uint32_t img_width = 0, img_height = 0;
    bool img_theme_exported = false;

    if (opts.cmd == CLI_CMD_IMG) {
        if (opts.custom_shader_path[0] != '\0') {
            img_shader_src = read_shader_file(opts.custom_shader_path, &img_shader_len);
            if (!img_shader_src) {
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
        }

        if (opts.is_solid_color) {
            img_width = 16;
            img_height = 16;
            img_pixels = (uint8_t *)malloc((size_t)img_width * img_height * 4);
            if (!img_pixels) {
                free(img_shader_src);
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
            for (size_t pi = 0; pi < (size_t)img_width * img_height; ++pi) {
                img_pixels[pi * 4 + 0] = opts.clear_color.b;
                img_pixels[pi * 4 + 1] = opts.clear_color.g;
                img_pixels[pi * 4 + 2] = opts.clear_color.r;
                img_pixels[pi * 4 + 3] = opts.clear_color.a;
            }
        } else if (opts.read_stdin) {
            size_t cap = 65536;
            size_t len = 0;
            uint8_t *stdin_buf = (uint8_t *)malloc(cap);
            if (!stdin_buf) {
                free(img_shader_src);
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
            bool read_ok = true;
            for (;;) {
                if (len == cap) {
                    if (cap >= 256U * 1024U * 1024U) {
                        read_ok = false;
                        break;
                    }
                    size_t next_cap = cap * 2U;
                    uint8_t *next_buf = (uint8_t *)realloc(stdin_buf, next_cap);
                    if (!next_buf) {
                        read_ok = false;
                        break;
                    }
                    stdin_buf = next_buf;
                    cap = next_cap;
                }
                ssize_t n = read(STDIN_FILENO, stdin_buf + len, cap - len);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                        client_poll_fd(STDIN_FILENO, POLLIN, 5000)) {
                        continue;
                    }
                    read_ok = false;
                    break;
                }
                if (n == 0)
                    break;
                len += (size_t)n;
            }
            raw_image_t raw_img;
            memset(&raw_img, 0, sizeof(raw_img));
            if (!read_ok || !image_load_from_memory(stdin_buf, len, &raw_img)) {
                fprintf(stderr, "Error: Failed to decode image from stdin\n");
                free(stdin_buf);
                free(img_shader_src);
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
            free(stdin_buf);
            img_pixels = raw_img.data;
            img_width = raw_img.width;
            img_height = raw_img.height;
        } else if (waywal_dynwall_is_manifest_path(opts.filepath)) {
            waywal_dynwall_manifest_t manifest;
            waywal_dynwall_resolved_t res;
            time_t now_utc = time(NULL);
            struct tm local_tm;
            localtime_r(&now_utc, &local_tm);
            waywal_solar_state_t solar;
            if (opts.has_solar_coords) {
                waywal_solar_compute(opts.solar_latitude, opts.solar_longitude, now_utc, &local_tm,
                                     &solar);
            } else {
                waywal_solar_synthesize_local(cfg.solar.sunrise_fallback_min,
                                              cfg.solar.sunset_fallback_min, now_utc, &local_tm,
                                              &solar);
            }
            if (!waywal_dynwall_load_file(opts.filepath, &manifest) ||
                !waywal_dynwall_resolve(&manifest, now_utc, &local_tm, &solar, &res) ||
                !image_load(waywal_dynwall_effective_path(&res), &img_pixels, &img_width,
                            &img_height)) {
                fprintf(stderr,
                        "Error: Failed to resolve or decode dynamic wallpaper manifest '%s'\n",
                        opts.filepath);
                free(img_shader_src);
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
            if (res.has_theme_mode && !opts.has_theme_mode && !opts.auto_day_night) {
                opts.theme_light_mode = res.theme_light_mode;
                opts.has_theme_mode = true;
            }
        } else {
            if (!image_load(opts.filepath, &img_pixels, &img_width, &img_height)) {
                fprintf(stderr, "Error: Failed to decode image file '%s'\n", opts.filepath);
                free(img_shader_src);
                cli_ipc_pool_close_all(&ipc_pool);
                return 1;
            }
        }

        bool should_extract_img_theme =
            (opts.extract_theme || opts.has_seed_color || cfg.theme.enabled) && !opts.no_theme;
        if (should_extract_img_theme) {
            waywal_theme_mode_t tmode =
                opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
            waywal_palette_t pal;
            const char *wp_path = opts.is_solid_color ? NULL : opts.filepath;
            uint8_t solid_theme_px[4] = {
                opts.clear_color.b,
                opts.clear_color.g,
                opts.clear_color.r,
                255U,
            };
            const uint8_t *theme_px =
                opts.has_seed_color ? NULL : (opts.is_solid_color ? solid_theme_px : img_pixels);
            uint32_t theme_w = opts.has_seed_color ? 0U : (opts.is_solid_color ? 1U : img_width);
            uint32_t theme_h = opts.has_seed_color ? 0U : (opts.is_solid_color ? 1U : img_height);
            if (extract_palette_from_bgra(theme_px, theme_w, theme_h, tmode, &opts, &cfg, wp_path,
                                          &pal)) {
                apply_extracted_palette_client(&pal, &opts, &cfg);
                img_theme_exported = true;
            }
        }
    }

    int ret_code = 0;

    if (has_audio_override) {
        for (size_t ns_idx = 0; ns_idx < ns_count; ++ns_idx) {
            (void)send_audio_ctrl_request(&ipc_pool, &opts, target_namespaces[ns_idx],
                                          want_persistent, false);
        }
    }

    size_t connected_ns = 0;
    bool wallpaper_hook_ran = false;

    for (size_t ns_idx = 0; ns_idx < ns_count; ++ns_idx) {
        const char *active_ns = target_namespaces[ns_idx];
        int socket_fd =
            cli_ipc_pool_acquire(&ipc_pool, &opts, active_ns, true, want_persistent, 2500);
        if (socket_fd < 0) {
            fprintf(stderr,
                    "Error: Unable to connect to wwald (namespace: %s).\n"
                    "Make sure wwald is running on the current Wayland compositor session.\n",
                    active_ns);
            if (ns_count == 1) {
                ret_code = 1;
                break;
            }
            continue;
        }
        connected_ns++;

        waywal_ipc_hdr_t req_hdr = {
            .magic = WAYWAL_IPC_MAGIC,
            .version = WAYWAL_IPC_VERSION,
            .opcode = 0,
            .payload_size = 0,
        };

        switch (opts.cmd) {
        case CLI_CMD_PING: {
            req_hdr.opcode = WAYWAL_REQ_PING;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send ping request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_PONG) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("wwald is alive (pong)\n");
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Unexpected response from daemon\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_STATUS:
        case CLI_CMD_QUERY: {
            bool is_json_q = (opts.cmd == CLI_CMD_STATUS || opts.json_output);
            req_hdr.opcode = is_json_q ? WAYWAL_REQ_QUERY_JSON : WAYWAL_REQ_QUERY;
            size_t q_outputs_len = 0;
            if (!is_json_q && opts.num_outputs > 0) {
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    q_outputs_len += strlen(opts.outputs[i]) + 1;
                }
            }
            char *q_outputs_buf = NULL;
            if (q_outputs_len > 0) {
                q_outputs_buf = (char *)malloc(q_outputs_len);
                if (!q_outputs_buf) {
                    ret_code = 1;
                    break;
                }
                size_t qpos = 0;
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    size_t slen = strlen(opts.outputs[i]) + 1;
                    memcpy(q_outputs_buf + qpos, opts.outputs[i], slen);
                    qpos += slen;
                }
            }
            req_hdr.payload_size = (uint32_t)q_outputs_len;
            bool sent = cli_ipc_send_framed(socket_fd, &req_hdr, q_outputs_buf, q_outputs_len, -1);
            free(q_outputs_buf);
            if (!sent) {
                fprintf(stderr, "Error: Failed to send query request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) &&
                (resp.opcode == WAYWAL_RESP_INFO || resp.opcode == WAYWAL_RESP_OK)) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Failed to query outputs from daemon\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_CLEAR: {
            size_t outputs_len = 0;
            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                outputs_len += strlen(opts.outputs[i]) + 1;
            }
            size_t clear_payload_size = sizeof(waywal_clear_payload_t) + outputs_len;
            uint8_t *clear_buf = (uint8_t *)calloc(1, clear_payload_size);
            if (!clear_buf) {
                ret_code = 1;
                break;
            }
            waywal_clear_payload_t *cpay = (waywal_clear_payload_t *)clear_buf;
            cpay->color[0] = opts.clear_color.r;
            cpay->color[1] = opts.clear_color.g;
            cpay->color[2] = opts.clear_color.b;
            cpay->color[3] = opts.clear_color.a;
            cpay->num_target_outputs = opts.num_outputs;

            char *optr = (char *)clear_buf + sizeof(waywal_clear_payload_t);
            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                size_t slen = strlen(opts.outputs[i]) + 1;
                memcpy(optr, opts.outputs[i], slen);
                optr += slen;
            }

            req_hdr.opcode = WAYWAL_REQ_CLEAR;
            req_hdr.payload_size = clear_payload_size;

            bool sent =
                cli_ipc_send_framed(socket_fd, &req_hdr, clear_buf, clear_payload_size, -1);
            free(clear_buf);
            if (!sent) {
                fprintf(stderr, "Error: Failed to send clear request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Wallpaper cleared to #%02x%02x%02x%02x\n", opts.clear_color.r,
                       opts.clear_color.g, opts.clear_color.b, opts.clear_color.a);
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon failed to clear wallpaper\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_VIDEO: {
            int video_fd = open(effective_video_path, O_RDONLY | O_CLOEXEC);
            req_hdr.opcode = WAYWAL_REQ_SET_VIDEO;

            size_t outputs_len = 0;
            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                outputs_len += strlen(opts.outputs[i]) + 1;
            }
            size_t video_payload_size = sizeof(waywal_video_payload_t) + outputs_len;
            uint8_t *video_buf = (uint8_t *)calloc(1, video_payload_size);
            if (!video_buf) {
                if (video_fd >= 0)
                    close(video_fd);
                ret_code = 1;
                break;
            }

            waywal_video_payload_t *vpay = (waywal_video_payload_t *)video_buf;
            vpay->loop_count = opts.video_loop_count;
            vpay->playback_speed = opts.video_speed;
            vpay->num_target_outputs = opts.num_outputs;
            vpay->scaling_mode = opts.scaling_mode;
            vpay->no_cache = opts.no_cache ? 1u : 0u;
            vpay->volume = opts.volume;
            vpay->mute_on_audio = (opts.mute_on_audio == 1);
            char resolved_vid[PATH_MAX];
            if (realpath(effective_video_path, resolved_vid)) {
                client_copy_cstr(vpay->filepath, sizeof(vpay->filepath), resolved_vid);
            } else {
                client_copy_cstr(vpay->filepath, sizeof(vpay->filepath), effective_video_path);
            }

            char *optr = (char *)video_buf + sizeof(waywal_video_payload_t);
            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                size_t slen = strlen(opts.outputs[i]) + 1;
                memcpy(optr, opts.outputs[i], slen);
                optr += slen;
            }

            req_hdr.payload_size = video_payload_size;

            bool sent = cli_ipc_send_framed(socket_fd, &req_hdr, video_buf, video_payload_size,
                                            video_fd);
            if (video_fd >= 0) {
                close(video_fd);
            }
            free(video_buf);
            if (!sent) {
                fprintf(stderr, "Error: Failed to send video request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Playing video wallpaper: %s\n", opts.filepath);
                if (!wallpaper_hook_ran && cfg.hooks.on_wallpaper_change[0] != '\0') {
                    wallpaper_hook_ran = true;
                    (void)waywal_palette_run_hook(cfg.hooks.on_wallpaper_change);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(
                    stderr,
                    "Error: Daemon rejected video request (check if video format is supported)\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_PAUSE: {
            req_hdr.opcode = WAYWAL_REQ_PAUSE;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send pause request\n");
                ret_code = 1;
                break;
            }
            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Video playback paused\n");
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Unexpected response to pause request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_UNPAUSE: {
            req_hdr.opcode = WAYWAL_REQ_UNPAUSE;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send unpause request\n");
                ret_code = 1;
                break;
            }
            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Video playback resumed\n");
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Unexpected response to unpause request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_TOGGLE: {
            req_hdr.opcode = WAYWAL_REQ_TOGGLE;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send toggle request\n");
                ret_code = 1;
                break;
            }
            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Video playback toggled\n");
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Unexpected response to toggle request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_KILL: {
            req_hdr.opcode = WAYWAL_REQ_KILL;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send kill request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                printf("wwald daemon terminated cleanly\n");
            } else {
                fprintf(stderr, "Error: Unexpected response to kill command\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_IMG: {
            size_t outputs_len = 0;
            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                outputs_len += strlen(opts.outputs[i]) + 1;
            }
            size_t extra_bytes = outputs_len + img_shader_len;
            size_t pixel_bytes = (size_t)img_width * img_height * 4;
            size_t total_payload_size = sizeof(waywal_img_metadata_t) + extra_bytes + pixel_bytes;

            int memfd = waywal_create_memfd("wwal-client-img", total_payload_size,
                                            F_SEAL_SHRINK | F_SEAL_GROW);
            if (memfd < 0) {
                fprintf(stderr, "Error: Failed to create sealed memfd for image\n");
                ret_code = 1;
                break;
            }

            void *mapped =
                mmap(NULL, total_payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
            if (mapped == MAP_FAILED) {
                fprintf(stderr, "Error: mmap failed on client memfd\n");
                close(memfd);
                ret_code = 1;
                break;
            }
#ifdef MADV_HUGEPAGE
            (void)madvise(mapped, total_payload_size, MADV_HUGEPAGE);
#endif

            waywal_img_metadata_t meta;
            memset(&meta, 0, sizeof(meta));
            meta.width = img_width;
            meta.height = img_height;
            if (opts.enable_10bit) {
                meta.pixel_format = WAYWAL_FORMAT_XRGB2101010;
            } else if (opts.pixel_format[0] != '\0') {
                if (strcasecmp(opts.pixel_format, "xrgb") == 0 ||
                    strcasecmp(opts.pixel_format, "xrgb8888") == 0 ||
                    strcasecmp(opts.pixel_format, "xbgr") == 0) {
                    meta.pixel_format = WAYWAL_FORMAT_XRGB8888;
                } else if (strcasecmp(opts.pixel_format, "xrgb2101010") == 0) {
                    meta.pixel_format = WAYWAL_FORMAT_XRGB2101010;
                } else if (strcasecmp(opts.pixel_format, "argb2101010") == 0) {
                    meta.pixel_format = WAYWAL_FORMAT_ARGB2101010;
                } else {
                    meta.pixel_format = WAYWAL_FORMAT_ARGB8888;
                }
            } else {
                meta.pixel_format = WAYWAL_FORMAT_ARGB8888;
            }
            meta.transition_type = opts.transition_type;
            meta.transition_duration_ms = opts.transition_duration_ms;
            meta.transition_fps = opts.transition_fps;
            meta.transition_angle_rad = opts.transition_angle_rad;
            meta.transition_wave_freq = opts.transition_wave_freq;
            meta.transition_wave_amp = opts.transition_wave_amp;
            meta.transition_center_x = opts.transition_pos_x;
            meta.transition_center_y = opts.transition_pos_y;
            meta.color[0] = opts.clear_color.r;
            meta.color[1] = opts.clear_color.g;
            meta.color[2] = opts.clear_color.b;
            meta.color[3] = opts.clear_color.a;
            meta.num_target_outputs = opts.num_outputs;
            meta.sync_mode = opts.sync_mode;
            meta.stagger_delay_ms = opts.stagger_delay_ms;
            meta.custom_shader_len = (uint32_t)img_shader_len;
            meta.extra_data_len = (uint32_t)extra_bytes;
            meta.scaling_mode = opts.scaling_mode;
            meta.filter_mode = (uint32_t)opts.filter;
            meta.bezier = opts.bezier;
            meta.has_custom_bezier = opts.has_custom_bezier;
            meta.ten_bit = opts.ten_bit;
            meta.extract_theme = opts.extract_theme;
            meta.no_theme = opts.no_theme;
            meta.theme_light_mode = opts.theme_light_mode;
            if (opts.depth_map_path[0] != '\0') {
                char resolved_dm[PATH_MAX];
                if (realpath(opts.depth_map_path, resolved_dm)) {
                    client_copy_cstr(meta.depth_map_path, sizeof(meta.depth_map_path), resolved_dm);
                } else {
                    client_copy_cstr(meta.depth_map_path, sizeof(meta.depth_map_path),
                                     opts.depth_map_path);
                }
            }
            client_copy_cstr(meta.backdrop, sizeof(meta.backdrop), opts.backdrop);
            client_copy_cstr(meta.layer_name, sizeof(meta.layer_name), opts.layer_name);
            client_copy_cstr(meta.battery_policy, sizeof(meta.battery_policy), opts.battery_policy);
            if (!opts.read_stdin && !opts.is_solid_color && opts.filepath[0] != '\0') {
                char resolved_img[PATH_MAX];
                if (realpath(opts.filepath, resolved_img)) {
                    client_copy_cstr(meta.filepath, sizeof(meta.filepath), resolved_img);
                } else {
                    client_copy_cstr(meta.filepath, sizeof(meta.filepath), opts.filepath);
                }
            }
            meta.depth_strength = opts.depth_strength > 0.0f ? opts.depth_strength : 1.0f;
            meta.crop_gravity_x = opts.crop_gravity_x;
            meta.crop_gravity_y = opts.crop_gravity_y;
            meta.has_crop_gravity = opts.has_crop_gravity ? 1 : 0;
            meta.invert_y = opts.invert_y ? 1 : 0;
            meta.no_cache = opts.no_cache ? 1 : 0;
            meta.is_solid_color = opts.is_solid_color ? 1 : 0;
            meta.pos_y_inverted = opts.pos_y_inverted ? 1 : 0;
            meta.source_color_index = opts.source_color_index;
            meta.has_source_color_index = opts.has_source_color_index ? 1 : 0;
            meta.has_seed_color = opts.has_seed_color ? 1 : 0;
            meta.seed_color = opts.seed_color;
            meta.has_theme_mode = opts.has_theme_mode ? 1 : 0;
            meta.client_exported_theme = img_theme_exported ? 1 : 0;
            meta.has_auto_day_night = opts.has_auto_day_night ? 1 : 0;
            meta.auto_day_night = opts.auto_day_night ? 1 : 0;
            meta.has_solar_coords = opts.has_solar_coords ? 1 : 0;
            meta.solar_latitude = opts.solar_latitude;
            meta.solar_longitude = opts.solar_longitude;

            uint8_t *dst = (uint8_t *)mapped;
            memcpy(dst, &meta, sizeof(meta));
            dst += sizeof(meta);

            for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                size_t slen = strlen(opts.outputs[i]) + 1;
                memcpy(dst, opts.outputs[i], slen);
                dst += slen;
            }

            if (img_shader_src && img_shader_len > 0) {
                memcpy(dst, img_shader_src, img_shader_len);
                dst += img_shader_len;
            }

            memcpy(dst, img_pixels, pixel_bytes);
            munmap(mapped, total_payload_size);
            (void)fcntl(memfd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL);

            req_hdr.opcode = WAYWAL_REQ_SET_IMAGE;
            req_hdr.payload_size = total_payload_size;

            bool sent = cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, memfd);
            close(memfd);
            if (!sent) {
                fprintf(stderr, "Error: Failed to send image to daemon via SCM_RIGHTS\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                if (opts.is_solid_color) {
                    printf("Wallpaper cleared to #%02x%02x%02x%02x\n", opts.clear_color.r,
                           opts.clear_color.g, opts.clear_color.b, opts.clear_color.a);
                } else {
                    printf("Wallpaper set: %s (%ux%u)\n", opts.filepath, img_width, img_height);
                }
                if (!wallpaper_hook_ran && cfg.hooks.on_wallpaper_change[0] != '\0') {
                    wallpaper_hook_ran = true;
                    (void)waywal_palette_run_hook(cfg.hooks.on_wallpaper_change);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected image request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_SLIDESHOW: {
            waywal_slideshow_payload_t spay;
            memset(&spay, 0, sizeof(spay));
            spay.interval_s = opts.slideshow_interval_s;
            spay.transition_type = opts.transition_type;
            spay.transition_duration_ms = opts.transition_duration_ms;
            spay.transition_fps = opts.transition_fps;
            spay.transition_angle_rad = opts.transition_angle_rad;
            spay.transition_wave_freq = opts.transition_wave_freq;
            spay.transition_wave_amp = opts.transition_wave_amp;
            spay.transition_center_x = opts.transition_pos_x;
            spay.transition_center_y = opts.transition_pos_y;
            spay.random_order = opts.slideshow_random;
            spay.num_target_outputs = opts.num_outputs;
            for (uint32_t i = 0; i < opts.num_outputs && i < 16; ++i) {
                client_copy_cstr(spay.target_outputs[i], sizeof(spay.target_outputs[i]),
                                 opts.outputs[i]);
            }
            spay.scaling_mode = opts.scaling_mode;
            spay.filter_mode = (uint32_t)opts.filter;
            spay.bezier = opts.bezier;
            spay.has_custom_bezier = opts.has_custom_bezier;
            spay.color[0] = opts.clear_color.r;
            spay.color[1] = opts.clear_color.g;
            spay.color[2] = opts.clear_color.b;
            spay.color[3] = opts.clear_color.a;
            spay.ten_bit = opts.ten_bit;
            spay.crop_gravity_x = opts.crop_gravity_x;
            spay.crop_gravity_y = opts.crop_gravity_y;
            spay.has_crop_gravity = opts.has_crop_gravity ? 1 : 0;
            spay.invert_y = opts.invert_y ? 1 : 0;
            spay.pos_y_inverted = opts.pos_y_inverted ? 1 : 0;
            spay.no_cache = opts.no_cache ? 1u : 0u;
            spay.extract_theme = opts.extract_theme ? 1u : 0u;
            spay.no_theme = opts.no_theme ? 1u : 0u;
            spay.has_theme_mode = opts.has_theme_mode ? 1u : 0u;
            spay.theme_light_mode = opts.theme_light_mode ? 1u : 0u;
            spay.has_source_color_index = opts.has_source_color_index ? 1u : 0u;
            spay.source_color_index = opts.source_color_index;
            spay.has_seed_color = opts.has_seed_color ? 1u : 0u;
            spay.seed_color = opts.seed_color;
            spay.sort_mode = (uint8_t)opts.slideshow_sort;
            spay.has_sort_mode = opts.has_slideshow_sort ? 1u : 0u;
            if (opts.has_slideshow_sort) {
                spay.random_order = (opts.slideshow_sort == WAYWAL_SORT_RANDOM);
            }
            const char *ss_target_out = (opts.num_outputs > 0) ? opts.outputs[0] : "*";
            const waywal_output_rule_t *ss_rule = waywal_config_match_output(&cfg, ss_target_out);
            if (opts.has_slideshow_recursive) {
                spay.no_recursive = opts.slideshow_recursive ? 0u : 1u;
            } else if (ss_rule && ss_rule->has_recursive) {
                spay.no_recursive = ss_rule->recursive ? 0u : 1u;
            } else if (cfg.daemon.has_slideshow_recursive) {
                spay.no_recursive = cfg.daemon.slideshow_recursive ? 0u : 1u;
            }
            spay.auto_day_night = opts.has_auto_day_night ? (opts.auto_day_night ? 1u : 2u) : 0u;
            spay.has_solar_coords = opts.has_solar_coords ? 1u : 0u;
            spay.solar_latitude = opts.solar_latitude;
            spay.solar_longitude = opts.solar_longitude;
            char resolved_ss[PATH_MAX];
            if (realpath(opts.filepath, resolved_ss)) {
                client_copy_cstr(spay.path, sizeof(spay.path), resolved_ss);
            } else {
                client_copy_cstr(spay.path, sizeof(spay.path), opts.filepath);
            }

            req_hdr.opcode = WAYWAL_REQ_SLIDESHOW;
            req_hdr.payload_size = sizeof(spay);

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &spay, sizeof(spay), -1)) {
                fprintf(stderr, "Error: Failed to send slideshow request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                printf("Slideshow started on '%s' (interval: %us, shuffle: %s)\n", opts.filepath,
                       opts.slideshow_interval_s, opts.slideshow_random ? "yes" : "no");
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr,
                        "Error: Daemon rejected slideshow request (check directory path)\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_SLIDESHOW_CTRL: {
            waywal_slideshow_ctrl_t sctrl;
            memset(&sctrl, 0, sizeof(sctrl));
            sctrl.action = opts.slideshow_action;
            if (opts.num_outputs > 0) {
                size_t pos = 0;
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    int n = snprintf(sctrl.target_output + pos, sizeof(sctrl.target_output) - pos,
                                     "%s%s", (i > 0) ? "," : "", opts.outputs[i]);
                    if (n < 0 || (size_t)n >= sizeof(sctrl.target_output) - pos)
                        break;
                    pos += (size_t)n;
                }
            } else {
                client_copy_cstr(sctrl.target_output, sizeof(sctrl.target_output), "*");
            }

            if (opts.slideshow_action == WAYWAL_SLIDESHOW_NEXT) {
                req_hdr.opcode = WAYWAL_REQ_SLIDESHOW_NEXT;
            } else if (opts.slideshow_action == WAYWAL_SLIDESHOW_PREV) {
                req_hdr.opcode = WAYWAL_REQ_SLIDESHOW_PREV;
            } else {
                req_hdr.opcode = WAYWAL_REQ_SLIDESHOW_CTRL;
            }
            req_hdr.payload_size = sizeof(sctrl);

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &sctrl, sizeof(sctrl), -1)) {
                fprintf(stderr, "Error: Failed to send slideshow control request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else {
                    const char *act_name = "action performed";
                    switch (opts.slideshow_action) {
                    case WAYWAL_SLIDESHOW_STOP:
                        act_name = "stopped";
                        break;
                    case WAYWAL_SLIDESHOW_PAUSE:
                        act_name = "paused";
                        break;
                    case WAYWAL_SLIDESHOW_RESUME:
                        act_name = "resumed";
                        break;
                    case WAYWAL_SLIDESHOW_TOGGLE:
                        act_name = "toggled";
                        break;
                    case WAYWAL_SLIDESHOW_NEXT:
                        act_name = "advanced to next";
                        break;
                    case WAYWAL_SLIDESHOW_PREV:
                        act_name = "stepped to previous";
                        break;
                    }
                    printf("Slideshow %s\n", act_name);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected slideshow control request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_SHADER: {
            waywal_shader_payload_t spay;
            memset(&spay, 0, sizeof(spay));

            if (opts.num_outputs > 0) {
                size_t pos = 0;
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    int n = snprintf(spay.output + pos, sizeof(spay.output) - pos, "%s%s",
                                     (i > 0) ? "," : "", opts.outputs[i]);
                    if (n < 0 || (size_t)n >= sizeof(spay.output) - pos)
                        break;
                    pos += (size_t)n;
                }
            } else {
                client_copy_cstr(spay.output, sizeof(spay.output), "*");
            }

            char resolved[PATH_MAX];
            if (realpath(opts.filepath, resolved)) {
                client_copy_cstr(spay.shader_path, sizeof(spay.shader_path), resolved);
            } else {
                client_copy_cstr(spay.shader_path, sizeof(spay.shader_path), opts.filepath);
            }
            spay.fps = opts.transition_fps > 0 ? opts.transition_fps : 60;
            spay.audio_reactive = opts.audio_reactive;
            spay.scaling_mode = opts.scaling_mode;
            spay.transition_type = opts.transition_type;
            spay.transition_duration_ms = opts.transition_duration_ms;
            spay.transition_fps = opts.transition_fps;
            spay.transition_angle_rad = opts.transition_angle_rad;
            spay.transition_wave_freq = opts.transition_wave_freq;
            spay.transition_wave_amp = opts.transition_wave_amp;
            spay.transition_center_x = opts.transition_pos_x;
            spay.transition_center_y = opts.transition_pos_y;
            spay.bezier = opts.bezier;
            spay.has_custom_bezier = opts.has_custom_bezier;
            spay.invert_y = opts.invert_y ? 1 : 0;
            spay.pos_y_inverted = opts.pos_y_inverted ? 1 : 0;
            spay.no_cache = opts.no_cache ? 1u : 0u;
            spay.volume = opts.volume;
            spay.mute_on_audio = (opts.mute_on_audio == 1);
            spay.speed = opts.video_speed;
            client_copy_cstr(spay.backdrop, sizeof(spay.backdrop), opts.backdrop);

            req_hdr.opcode = WAYWAL_REQ_SHADER;
            req_hdr.payload_size = sizeof(spay);

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &spay, sizeof(spay), -1)) {
                fprintf(stderr, "Error: Failed to send shader request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else {
                    if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                        (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                    }
                    printf("Shader wallpaper active: %s\n", spay.shader_path);
                }
                if (!wallpaper_hook_ran && cfg.hooks.on_wallpaper_change[0] != '\0') {
                    wallpaper_hook_ran = true;
                    (void)waywal_palette_run_hook(cfg.hooks.on_wallpaper_change);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected shader request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_WORKSHOP: {
            waywal_workshop_payload_t wpay;
            memset(&wpay, 0, sizeof(wpay));

            if (opts.num_outputs > 0) {
                size_t pos = 0;
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    int n = snprintf(wpay.output + pos, sizeof(wpay.output) - pos, "%s%s",
                                     (i > 0) ? "," : "", opts.outputs[i]);
                    if (n < 0 || (size_t)n >= sizeof(wpay.output) - pos)
                        break;
                    pos += (size_t)n;
                }
            } else {
                client_copy_cstr(wpay.output, sizeof(wpay.output), "*");
            }

            client_copy_cstr(wpay.workshop_dir, sizeof(wpay.workshop_dir), opts.filepath);
            client_copy_cstr(wpay.prop_overrides, sizeof(wpay.prop_overrides), opts.workshop_props);
            wpay.fps = opts.transition_fps > 0 ? opts.transition_fps : 60;
            wpay.audio_reactive = opts.audio_reactive;
            wpay.scaling_mode = opts.scaling_mode;
            wpay.transition_type = opts.transition_type;
            wpay.transition_duration_ms = opts.transition_duration_ms;
            wpay.transition_fps = opts.transition_fps;
            wpay.transition_angle_rad = opts.transition_angle_rad;
            wpay.transition_wave_freq = opts.transition_wave_freq;
            wpay.transition_wave_amp = opts.transition_wave_amp;
            wpay.transition_center_x = opts.transition_pos_x;
            wpay.transition_center_y = opts.transition_pos_y;
            wpay.bezier = opts.bezier;
            wpay.has_custom_bezier = opts.has_custom_bezier;
            wpay.invert_y = opts.invert_y ? 1 : 0;
            wpay.pos_y_inverted = opts.pos_y_inverted ? 1 : 0;
            wpay.no_cache = opts.no_cache ? 1u : 0u;
            wpay.extract_theme = opts.extract_theme ? 1u : 0u;
            wpay.no_theme = opts.no_theme ? 1u : 0u;
            wpay.has_theme_mode = opts.has_theme_mode ? 1u : 0u;
            wpay.theme_light_mode = opts.theme_light_mode ? 1u : 0u;
            wpay.has_source_color_index = opts.has_source_color_index ? 1u : 0u;
            wpay.source_color_index = opts.source_color_index;
            wpay.has_seed_color = opts.has_seed_color ? 1u : 0u;
            wpay.client_exported_theme = workshop_theme_applied ? 1u : 0u;
            wpay.seed_color = opts.seed_color;
            wpay.volume = opts.volume;
            wpay.mute_on_audio = (opts.mute_on_audio == 1);
            client_copy_cstr(wpay.backdrop, sizeof(wpay.backdrop), opts.backdrop);
            client_copy_cstr(wpay.layer_name, sizeof(wpay.layer_name), opts.layer_name);

            req_hdr.opcode = WAYWAL_REQ_WORKSHOP;
            req_hdr.payload_size = sizeof(wpay);

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &wpay, sizeof(wpay), -1)) {
                fprintf(stderr, "Error: Failed to send workshop request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else {
                    if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                        (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                    }
                    printf("Workshop wallpaper active: %s\n", wpay.workshop_dir);
                }
                if (!wallpaper_hook_ran && cfg.hooks.on_wallpaper_change[0] != '\0') {
                    wallpaper_hook_ran = true;
                    (void)waywal_palette_run_hook(cfg.hooks.on_wallpaper_change);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected workshop request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_STATS: {
            req_hdr.opcode = opts.json_output ? WAYWAL_REQ_QUERY_JSON : WAYWAL_REQ_STATS;
            req_hdr.payload_size = 0;

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send stats request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) &&
                (resp.opcode == WAYWAL_RESP_OK || resp.opcode == WAYWAL_RESP_INFO)) {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Failed to query telemetry stats from daemon\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_SET_UNIFORM: {
            waywal_set_uniform_payload_t upay;
            memset(&upay, 0, sizeof(upay));
            if (opts.num_outputs > 0) {
                size_t pos = 0;
                for (uint32_t i = 0; i < opts.num_outputs; ++i) {
                    int n = snprintf(upay.output + pos, sizeof(upay.output) - pos, "%s%s",
                                     (i > 0) ? "," : "", opts.outputs[i]);
                    if (n < 0 || (size_t)n >= sizeof(upay.output) - pos)
                        break;
                    pos += (size_t)n;
                }
            } else {
                client_copy_cstr(upay.output, sizeof(upay.output), "*");
            }
            client_copy_cstr(upay.name, sizeof(upay.name), opts.uniform_name);
            for (uint32_t i = 0; i < opts.uniform_components && i < 4; ++i) {
                upay.values[i] = opts.uniform_values[i];
            }
            upay.num_components = opts.uniform_components;
            if (strlen(upay.output) < sizeof(upay.structured.target_output) &&
                strlen(opts.uniform_name) < sizeof(upay.structured.uniform_name) &&
                opts.uniform_components >= 1U && opts.uniform_components <= 4U) {
                upay.structured.struct_size = (uint32_t)sizeof(upay.structured);
                upay.structured.flags = WAYWAL_UNIFORM_FLAG_STRUCTURED;
                upay.structured.uniform_components = opts.uniform_components;
                client_copy_cstr(upay.structured.target_output,
                                 sizeof(upay.structured.target_output), upay.output);
                client_copy_cstr(upay.structured.uniform_name,
                                 sizeof(upay.structured.uniform_name), opts.uniform_name);
                for (uint32_t i = 0; i < opts.uniform_components && i < 4; ++i) {
                    upay.structured.uniform_values[i] = opts.uniform_values[i];
                }
            }

            req_hdr.opcode = WAYWAL_REQ_SET_UNIFORM;
            req_hdr.payload_size = sizeof(upay);
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &upay, sizeof(upay), -1)) {
                fprintf(stderr, "Error: Failed to send set-uniform request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected set-uniform request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_AUDIO: {
            waywal_audio_ctrl_payload_t apay;
            memset(&apay, 0, sizeof(apay));
            apay.volume = opts.volume;
            apay.mute_on_audio = opts.mute_on_audio;
            apay.theme_from_media = opts.theme_from_media;

            req_hdr.opcode = WAYWAL_REQ_AUDIO_CTRL;
            req_hdr.payload_size = sizeof(apay);
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, &apay, sizeof(apay), -1)) {
                fprintf(stderr, "Error: Failed to send audio control request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon rejected audio control request\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        case CLI_CMD_SUBSCRIBE: {
            req_hdr.opcode = WAYWAL_REQ_SUBSCRIBE;
            req_hdr.payload_size = 0;
            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send subscribe request\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (!waywal_ipc_recv(socket_fd, &resp, &attached_fd) || resp.opcode != WAYWAL_RESP_OK) {
                if (attached_fd >= 0)
                    close(attached_fd);
                fprintf(stderr, "Error: Daemon rejected event subscription\n");
                ret_code = 1;
                break;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            if (resp.payload_size > 65536) {
                fprintf(stderr, "Error: Oversized initial subscribe payload\n");
                ret_code = 1;
                break;
            }
            if (resp.payload_size > 0) {
                if (!drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true)) {
                    ret_code = 1;
                    break;
                }
                fflush(stdout);
            }

            for (;;) {
                waywal_ipc_hdr_t ev_hdr;
                int ev_fd = -1;
                if (!waywal_ipc_recv(socket_fd, &ev_hdr, &ev_fd)) {
                    break;
                }
                if (ev_fd >= 0)
                    close(ev_fd);
                if (ev_hdr.payload_size > 65536) {
                    break;
                }
                if (ev_hdr.payload_size > 0) {
                    bool print_ev = (ev_hdr.opcode == WAYWAL_RESP_EVENT);
                    if (!drain_and_print_reply(socket_fd, (size_t)ev_hdr.payload_size, print_ev)) {
                        break;
                    }
                    if (print_ev) {
                        fflush(stdout);
                    }
                }
            }
            ns_idx = ns_count;
            break;
        }

        case CLI_CMD_RESTORE: {
            req_hdr.opcode = WAYWAL_REQ_RESTORE;
            req_hdr.payload_size = 0;

            if (!cli_ipc_send_framed(socket_fd, &req_hdr, NULL, 0, -1)) {
                fprintf(stderr, "Error: Failed to send restore request to daemon\n");
                ret_code = 1;
                break;
            }

            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (cli_recv_reply(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
                if (resp.payload_size > 0 && resp.payload_size <= 16384U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, true);
                } else {
                    if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                        (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                    }
                    printf("OK: State restored\n");
                }
            } else {
                if (resp.payload_size > 0 && resp.payload_size <= 65536U) {
                    (void)drain_and_print_reply(socket_fd, (size_t)resp.payload_size, false);
                }
                fprintf(stderr, "Error: Daemon failed to restore state\n");
                ret_code = 1;
            }
            if (attached_fd >= 0)
                close(attached_fd);
            break;
        }

        default:
            break;
        }

        cli_ipc_pool_release(&ipc_pool, active_ns, socket_fd);
    }

    cli_ipc_pool_close_all(&ipc_pool);

    if (img_pixels) {
        if (opts.is_solid_color) {
            free(img_pixels);
        } else {
            image_free(img_pixels);
        }
    }
    free(img_shader_src);

    if (connected_ns == 0) {
        ret_code = 1;
    }

    return ret_code;
}
