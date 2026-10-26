// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli_parser.h"
#include "image_loader.h"
#include "waywal/gif_transcode.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/palette_hct.h"
#include "waywal/state_store.h"
#include "waywal/toml_parser.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void apply_config_defaults_to_cli(int argc, char *argv[], const waywal_config_t *cfg,
                                         cli_options_t *opts)
{
    if (!cfg || !opts)
        return;

    if (!argv_has_flag(argc, argv, "--transition-type") && !argv_has_flag(argc, argv, "-t") &&
        !argv_has_flag(argc, argv, "--transition-shader")) {
        opts->transition_type = cfg->transition.type_enum;
    }
    if (!argv_has_flag(argc, argv, "--transition-duration") &&
        !argv_has_flag(argc, argv, "--transition-step")) {
        opts->transition_duration = cfg->transition.duration;
        opts->transition_duration_ms = (uint32_t)(cfg->transition.duration * 1000.0f);
    }
    if (!argv_has_flag(argc, argv, "--transition-fps") && !argv_has_flag(argc, argv, "--fps")) {
        if (cfg->transition.fps > 0)
            opts->transition_fps = cfg->transition.fps;
    }
    if (!argv_has_flag(argc, argv, "--transition-angle") &&
        !argv_has_flag(argc, argv, "--transition-type") && !argv_has_flag(argc, argv, "-t")) {
        opts->transition_angle = cfg->transition.angle;
        opts->transition_angle_rad = cfg->transition.angle * 3.1415926535f / 180.0f;
    }
    if (!argv_has_flag(argc, argv, "--transition-pos") &&
        !argv_has_flag(argc, argv, "--transition-type") && !argv_has_flag(argc, argv, "-t")) {
        opts->transition_pos_x = cfg->transition.pos_x;
        opts->transition_pos_y = cfg->transition.pos_y;
    }
    if (!argv_has_flag(argc, argv, "--transition-wave")) {
        opts->transition_wave_freq = cfg->transition.wave_x;
        opts->transition_wave_amp = cfg->transition.wave_y;
    }
    if (!argv_has_flag(argc, argv, "--transition-bezier")) {
        opts->bezier = bezier_create(cfg->transition.bezier[0], cfg->transition.bezier[1],
                                     cfg->transition.bezier[2], cfg->transition.bezier[3]);
    }
    if (!argv_has_flag(argc, argv, "--10bit") && cfg->daemon.ten_bit) {
        opts->enable_10bit = true;
    }
    if (!argv_has_flag(argc, argv, "--theme-mode") && !argv_has_flag(argc, argv, "--light") &&
        !argv_has_flag(argc, argv, "--dark") && !argv_has_flag(argc, argv, "--mode")) {
        opts->theme_light_mode = (cfg->theme.theme_mode == WAYWAL_THEME_LIGHT);
    }
    if (!argv_has_flag(argc, argv, "--sync") && cfg->transition.sync_mode[0] != '\0') {
        opts->sync_mode = (strcasecmp(cfg->transition.sync_mode, "staggered") == 0) ? 1U : 0U;
    }
    if (!argv_has_flag(argc, argv, "--stagger") && cfg->transition.stagger_delay_ms > 0) {
        opts->stagger_delay_ms = cfg->transition.stagger_delay_ms;
    }
}

static bool extract_palette_from_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                      waywal_theme_mode_t mode, waywal_palette_t *out_pal)
{
    if (!bgra_pixels || width == 0 || height == 0 || !out_pal)
        return false;

    return waywal_palette_extract_bgra(bgra_pixels, width, height, width * 4U, mode, out_pal);
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
        size_t nlen = strlen(ent->d_name);
        bool is_cached_ext = (nlen > 4 && (strcasecmp(ent->d_name + nlen - 4, ".mp4") == 0 ||
                                           strcasecmp(ent->d_name + nlen - 4, ".bin") == 0));
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
    path_buf_init(&cache_root);

    const char *env_cache = getenv("WWAL_CACHE_DIR");
    if (env_cache && env_cache[0] != '\0') {
        path_buf_from_str(&cache_root, env_cache);
    } else {
        const char *xdg_cache = getenv("XDG_CACHE_HOME");
        if (xdg_cache && xdg_cache[0] != '\0') {
            path_buf_from_str(&cache_root, xdg_cache);
            path_buf_push(&cache_root, "wwal");
        } else {
            const char *home = getenv("HOME");
            if (!home || home[0] == '\0') {
                fprintf(stderr, "Error: Cannot determine cache directory\n");
                return 1;
            }
            path_buf_from_str(&cache_root, home);
            path_buf_push(&cache_root, ".cache/wwal");
        }
    }

    size_t removed = clear_cache_dir_files(path_buf_cstr(&cache_root));

    const char *subdirs[] = {"shaders", "transcode", "transcoded"};
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
                strncpy(out_path, st.outputs[i].media_path, cap - 1);
                out_path[cap - 1] = '\0';
                return true;
            }
        }
    }

    return false;
}

static int handle_cmd_theme(const cli_options_t *opts, const waywal_config_t *cfg)
{
    char target_path[4096];
    target_path[0] = '\0';

    if (opts->filepath[0] != '\0') {
        strncpy(target_path, opts->filepath, sizeof(target_path) - 1);
        target_path[sizeof(target_path) - 1] = '\0';
    } else if (!resolve_theme_default_wallpaper(target_path, sizeof(target_path))) {
        fprintf(stderr, "Error: No image path specified and no active wallpaper found in state.\n");
        return 1;
    }

    uint8_t *pixels = NULL;
    uint32_t width = 0, height = 0;
    if (!image_load(target_path, &pixels, &width, &height)) {
        fprintf(stderr, "Error: Failed to decode image '%s' for theme extraction\n", target_path);
        return 1;
    }

    waywal_theme_mode_t mode = opts->theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
    waywal_palette_t pal;
    bool ok = extract_palette_from_bgra(pixels, width, height, mode, &pal);
    image_free(pixels);

    if (!ok) {
        fprintf(stderr, "Error: Failed to extract HCT palette from '%s'\n", target_path);
        return 1;
    }

    (void)waywal_palette_export_all(&pal, NULL);
    if (!cfg || cfg->theme.tty_sequences) {
        (void)waywal_palette_broadcast_tty(&pal);
    }
    if (cfg && cfg->hooks.on_theme_change[0] != '\0') {
        (void)waywal_palette_run_hook(cfg->hooks.on_theme_change);
    }

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

static void drain_and_print_reply(int socket_fd, size_t payload_size)
{
    char buf[4096];
    size_t remaining = payload_size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) - 1 ? remaining : sizeof(buf) - 1;
        ssize_t r = read(socket_fd, buf, to_read);
        if (r <= 0)
            break;
        buf[r] = '\0';
        fputs(buf, stdout);
        remaining -= (size_t)r;
    }
}

static char *read_shader_file(const char *path, size_t *out_len)
{
    if (!path || !out_len)
        return NULL;
    FILE *f = fopen(path, "rb");
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

static void query_max_target_resolution(const cli_options_t *opts, uint32_t *out_w, uint32_t *out_h)
{
    *out_w = 2560;
    *out_h = 1440;

    int qfd = waywal_ipc_client_connect(opts->namespace_str, 500);
    if (qfd < 0)
        return;

    waywal_ipc_hdr_t req = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_QUERY,
        .payload_size = 0,
    };
    if (!waywal_ipc_send(qfd, &req, -1)) {
        close(qfd);
        return;
    }

    waywal_ipc_hdr_t resp;
    int attached_fd = -1;
    if (!waywal_ipc_recv(qfd, &resp, &attached_fd) || resp.opcode != WAYWAL_RESP_INFO ||
        resp.payload_size == 0 || resp.payload_size >= 16384) {
        if (attached_fd >= 0)
            close(attached_fd);
        close(qfd);
        return;
    }
    if (attached_fd >= 0)
        close(attached_fd);

    char buf[4096];
    size_t to_read = resp.payload_size < sizeof(buf) - 1 ? resp.payload_size : sizeof(buf) - 1;
    size_t total = 0;
    while (total < to_read) {
        ssize_t r = read(qfd, buf + total, to_read - total);
        if (r <= 0)
            break;
        total += (size_t)r;
    }
    close(qfd);
    buf[total] = '\0';

    uint32_t max_w = 0;
    uint32_t max_h = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(buf, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        const char *prefix = strstr(line, "  - ");
        if (!prefix)
            continue;
        const char *name_start = prefix + 4;
        const char *name_end = strchr(name_start, ' ');
        if (!name_end)
            continue;

        const char *colon = strstr(name_end, ": ");
        if (!colon)
            continue;
        unsigned int w = 0, h = 0;
        if (sscanf(colon + 2, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
            if (w > max_w)
                max_w = w;
            if (h > max_h)
                max_h = h;
        }
    }

    if (max_w > 0 && max_h > 0) {
        *out_w = max_w;
        *out_h = max_h;
    }
}

int main(int argc, char *argv[])
{
    waywal_config_t cfg;
    (void)waywal_config_load_default(&cfg);

    cli_options_t opts;
    if (!cli_parse(argc, argv, &opts)) {
        cli_print_usage(argv[0]);
        return 1;
    }

    if (opts.cmd == CLI_CMD_HELP || opts.cmd == CLI_CMD_NONE) {
        cli_print_usage(argv[0]);
        return 0;
    }

    apply_config_defaults_to_cli(argc, argv, &cfg, &opts);

    if (opts.verbose) {
        waywal_log_set_level(WAYWAL_LOG_LEVEL_DEBUG);
    }

    if (opts.cmd == CLI_CMD_THEME) {
        return handle_cmd_theme(&opts, &cfg);
    }

    if (opts.cmd == CLI_CMD_CLEAR_CACHE) {
        return handle_cmd_clear_cache();
    }

    if (opts.cmd == CLI_CMD_DAEMON_ALIAS) {
        char *daemon_argv[16];
        int d_argc = 0;
        daemon_argv[d_argc++] = "wwald";
        if (opts.namespace_str[0] != '\0') {
            daemon_argv[d_argc++] = "-n";
            daemon_argv[d_argc++] = opts.namespace_str;
        }
        if (opts.verbose) {
            daemon_argv[d_argc++] = "-v";
        }
        daemon_argv[d_argc] = NULL;
        execvp("wwald", daemon_argv);
        fprintf(stderr, "Error: Failed to exec wwald: %s\n", strerror(errno));
        return 1;
    }

    if (opts.cmd == CLI_CMD_IMG && !opts.read_stdin && opts.animate && is_gif_file(opts.filepath)) {
        opts.cmd = CLI_CMD_VIDEO;
    }

    char effective_video_path[4096];
    effective_video_path[0] = '\0';
    if (opts.cmd == CLI_CMD_VIDEO) {
        uint32_t max_w = 2560, max_h = 1440;
        query_max_target_resolution(&opts, &max_w, &max_h);
        if (!video_transcode_optimize(opts.filepath, max_w, max_h, effective_video_path,
                                      sizeof(effective_video_path))) {
            if (is_gif_file(opts.filepath)) {
                opts.cmd = CLI_CMD_IMG;
            } else {
                fprintf(stderr, "Error: Failed to process video/GIF input: %s\n", opts.filepath);
                return 1;
            }
        }
    }

    int socket_fd = waywal_ipc_client_connect(opts.namespace_str, 3000);
    if (socket_fd < 0) {
        fprintf(stderr,
                "Error: Unable to connect to wwald (namespace: %s).\n"
                "Make sure wwald is running on the current Wayland compositor session.\n",
                opts.namespace_str);
        return 1;
    }

    waywal_ipc_hdr_t req_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = 0,
        .payload_size = 0,
    };

    int ret_code = 0;

    switch (opts.cmd) {
    case CLI_CMD_PING: {
        req_hdr.opcode = WAYWAL_REQ_PING;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send ping request\n");
            ret_code = 1;
            break;
        }

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_PONG) {
            printf("wwald is alive (pong)\n");
        } else {
            fprintf(stderr, "Error: Unexpected response from daemon\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_QUERY: {
        req_hdr.opcode = WAYWAL_REQ_QUERY;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send query request\n");
            ret_code = 1;
            break;
        }

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_INFO) {
            char buf[4096];
            size_t remaining = resp.payload_size;
            while (remaining > 0) {
                size_t to_read = remaining < sizeof(buf) - 1 ? remaining : sizeof(buf) - 1;
                ssize_t r = read(socket_fd, buf, to_read);
                if (r <= 0)
                    break;
                buf[r] = '\0';
                fputs(buf, stdout);
                remaining -= (size_t)r;
            }
        } else {
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

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send clear request\n");
            free(clear_buf);
            ret_code = 1;
            break;
        }
        (void)write(socket_fd, clear_buf, clear_payload_size);
        free(clear_buf);

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Wallpaper cleared to #%02x%02x%02x%02x\n", opts.clear_color.r,
                   opts.clear_color.g, opts.clear_color.b, opts.clear_color.a);
        } else {
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
        strncpy(vpay->filepath, effective_video_path, sizeof(vpay->filepath) - 1);

        char *optr = (char *)video_buf + sizeof(waywal_video_payload_t);
        for (uint32_t i = 0; i < opts.num_outputs; ++i) {
            size_t slen = strlen(opts.outputs[i]) + 1;
            memcpy(optr, opts.outputs[i], slen);
            optr += slen;
        }

        req_hdr.payload_size = video_payload_size;

        if (video_fd >= 0) {
            if (!waywal_ipc_send(socket_fd, &req_hdr, video_fd)) {
                fprintf(stderr, "Error: Failed to send video fd to daemon\n");
                close(video_fd);
                free(video_buf);
                ret_code = 1;
                break;
            }
            close(video_fd);
            (void)write(socket_fd, video_buf, video_payload_size);
        } else {
            if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
                fprintf(stderr, "Error: Failed to send video request to daemon\n");
                free(video_buf);
                ret_code = 1;
                break;
            }
            (void)write(socket_fd, video_buf, video_payload_size);
        }
        free(video_buf);

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Playing video wallpaper: %s\n", opts.filepath);
        } else {
            fprintf(stderr,
                    "Error: Daemon rejected video request (check if video format is supported)\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_PAUSE: {
        req_hdr.opcode = WAYWAL_REQ_PAUSE;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send pause request\n");
            ret_code = 1;
            break;
        }
        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Video playback paused\n");
        } else {
            fprintf(stderr, "Error: Unexpected response to pause request\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_UNPAUSE: {
        req_hdr.opcode = WAYWAL_REQ_UNPAUSE;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send unpause request\n");
            ret_code = 1;
            break;
        }
        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Video playback resumed\n");
        } else {
            fprintf(stderr, "Error: Unexpected response to unpause request\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_TOGGLE: {
        req_hdr.opcode = WAYWAL_REQ_TOGGLE;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send toggle request\n");
            ret_code = 1;
            break;
        }
        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Video playback toggled\n");
        } else {
            fprintf(stderr, "Error: Unexpected response to toggle request\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_KILL: {
        req_hdr.opcode = WAYWAL_REQ_KILL;
        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send kill request\n");
            ret_code = 1;
            break;
        }

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
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
        char *shader_src = NULL;
        size_t shader_len = 0;
        if (opts.custom_shader_path[0] != '\0') {
            shader_src = read_shader_file(opts.custom_shader_path, &shader_len);
            if (!shader_src) {
                ret_code = 1;
                break;
            }
        }

        size_t outputs_len = 0;
        for (uint32_t i = 0; i < opts.num_outputs; ++i) {
            outputs_len += strlen(opts.outputs[i]) + 1;
        }
        size_t extra_bytes = outputs_len + shader_len;

        uint8_t *pixels = NULL;
        uint32_t width = 0, height = 0;
        if (opts.read_stdin) {
            size_t cap = 65536;
            size_t len = 0;
            uint8_t *stdin_buf = (uint8_t *)malloc(cap);
            if (!stdin_buf) {
                free(shader_src);
                ret_code = 1;
                break;
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
                free(shader_src);
                ret_code = 1;
                break;
            }
            free(stdin_buf);
            pixels = raw_img.data;
            width = raw_img.width;
            height = raw_img.height;
        } else {
            if (!image_load(opts.filepath, &pixels, &width, &height)) {
                fprintf(stderr, "Error: Failed to decode image file '%s'\n", opts.filepath);
                free(shader_src);
                ret_code = 1;
                break;
            }
        }

        if (opts.extract_theme || (cfg.theme.enabled && !opts.no_theme)) {
            waywal_theme_mode_t tmode =
                opts.theme_light_mode ? WAYWAL_THEME_LIGHT : WAYWAL_THEME_DARK;
            waywal_palette_t pal;
            if (extract_palette_from_bgra(pixels, width, height, tmode, &pal)) {
                (void)waywal_palette_export_all(&pal, NULL);
                if (cfg.theme.tty_sequences) {
                    (void)waywal_palette_broadcast_tty(&pal);
                }
                if (cfg.hooks.on_theme_change[0] != '\0') {
                    (void)waywal_palette_run_hook(cfg.hooks.on_theme_change);
                }
            }
        }

        size_t pixel_bytes = (size_t)width * height * 4;
        size_t total_payload_size = sizeof(waywal_img_metadata_t) + extra_bytes + pixel_bytes;

        int memfd =
            waywal_create_memfd("wwal-client-img", total_payload_size, F_SEAL_SHRINK | F_SEAL_GROW);
        if (memfd < 0) {
            fprintf(stderr, "Error: Failed to create sealed memfd for image\n");
            image_free(pixels);
            free(shader_src);
            ret_code = 1;
            break;
        }

        void *mapped = mmap(NULL, total_payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (mapped == MAP_FAILED) {
            fprintf(stderr, "Error: mmap failed on client memfd\n");
            close(memfd);
            image_free(pixels);
            free(shader_src);
            ret_code = 1;
            break;
        }
#ifdef MADV_HUGEPAGE
        (void)madvise(mapped, total_payload_size, MADV_HUGEPAGE);
#endif

        waywal_img_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        meta.width = width;
        meta.height = height;
        meta.pixel_format = opts.enable_10bit ? WAYWAL_FORMAT_XRGB2101010 : WAYWAL_FORMAT_ARGB8888;
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
        meta.custom_shader_len = (uint32_t)shader_len;
        meta.extra_data_len = (uint32_t)extra_bytes;
        meta.scaling_mode = opts.scaling_mode;
        meta.filter_mode = (uint32_t)opts.filter;
        meta.bezier = opts.bezier;
        meta.has_custom_bezier = opts.has_custom_bezier;
        meta.ten_bit = opts.ten_bit;
        if (!opts.read_stdin && opts.filepath[0] != '\0') {
            char resolved_img[PATH_MAX];
            if (realpath(opts.filepath, resolved_img)) {
                strncpy(meta.filepath, resolved_img, sizeof(meta.filepath) - 1);
            } else {
                strncpy(meta.filepath, opts.filepath, sizeof(meta.filepath) - 1);
            }
        }

        uint8_t *dst = (uint8_t *)mapped;
        memcpy(dst, &meta, sizeof(meta));
        dst += sizeof(meta);

        for (uint32_t i = 0; i < opts.num_outputs; ++i) {
            size_t slen = strlen(opts.outputs[i]) + 1;
            memcpy(dst, opts.outputs[i], slen);
            dst += slen;
        }

        if (shader_src && shader_len > 0) {
            memcpy(dst, shader_src, shader_len);
            dst += shader_len;
            free(shader_src);
            shader_src = NULL;
        }

        memcpy(dst, pixels, pixel_bytes);
        munmap(mapped, total_payload_size);
        (void)fcntl(memfd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL);
        image_free(pixels);

        req_hdr.opcode = WAYWAL_REQ_SET_IMAGE;
        req_hdr.payload_size = total_payload_size;

        if (!waywal_ipc_send(socket_fd, &req_hdr, memfd)) {
            fprintf(stderr, "Error: Failed to send image to daemon via SCM_RIGHTS\n");
            close(memfd);
            ret_code = 1;
            break;
        }
        close(memfd);

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Wallpaper set: %s (%ux%u)\n", opts.filepath, width, height);
            if (cfg.hooks.on_wallpaper_change[0] != '\0') {
                (void)waywal_palette_run_hook(cfg.hooks.on_wallpaper_change);
            }
        } else {
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
        spay.scaling_mode = opts.scaling_mode;
        spay.filter_mode = (uint32_t)opts.filter;
        spay.bezier = opts.bezier;
        spay.has_custom_bezier = opts.has_custom_bezier;
        spay.color[0] = opts.clear_color.r;
        spay.color[1] = opts.clear_color.g;
        spay.color[2] = opts.clear_color.b;
        spay.color[3] = opts.clear_color.a;
        spay.ten_bit = opts.ten_bit;
        strncpy(spay.path, opts.filepath, sizeof(spay.path) - 1);

        req_hdr.opcode = WAYWAL_REQ_SLIDESHOW;
        req_hdr.payload_size = sizeof(spay);

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send slideshow request to daemon\n");
            ret_code = 1;
            break;
        }
        (void)write(socket_fd, &spay, sizeof(spay));

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Slideshow started on '%s' (interval: %us, shuffle: %s)\n", opts.filepath,
                   opts.slideshow_interval_s, opts.slideshow_random ? "yes" : "no");
        } else {
            fprintf(stderr, "Error: Daemon rejected slideshow request (check directory path)\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_SLIDESHOW_CTRL: {
        waywal_slideshow_ctrl_t sctrl;
        sctrl.action = opts.slideshow_action;
        req_hdr.opcode = WAYWAL_REQ_SLIDESHOW_CTRL;
        req_hdr.payload_size = sizeof(sctrl);

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send slideshow control request\n");
            ret_code = 1;
            break;
        }
        (void)write(socket_fd, &sctrl, sizeof(sctrl));

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
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
        } else {
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
            strncpy(spay.output, opts.outputs[0], sizeof(spay.output) - 1);
        } else {
            strncpy(spay.output, "*", sizeof(spay.output) - 1);
        }

        char resolved[PATH_MAX];
        if (realpath(opts.filepath, resolved)) {
            strncpy(spay.shader_path, resolved, sizeof(spay.shader_path) - 1);
        } else {
            strncpy(spay.shader_path, opts.filepath, sizeof(spay.shader_path) - 1);
        }
        spay.fps = opts.transition_fps > 0 ? opts.transition_fps : 60;
        spay.audio_reactive = opts.audio_reactive;

        req_hdr.opcode = WAYWAL_REQ_SHADER;
        req_hdr.payload_size = sizeof(spay);

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send shader request to daemon\n");
            ret_code = 1;
            break;
        }
        (void)write(socket_fd, &spay, sizeof(spay));

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            if (resp.payload_size > 0 && resp.payload_size <= 16384) {
                drain_and_print_reply(socket_fd, (size_t)resp.payload_size);
            } else {
                printf("Shader wallpaper active: %s\n", spay.shader_path);
            }
        } else {
            fprintf(stderr, "Error: Daemon rejected shader request\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_STATS: {
        req_hdr.opcode = WAYWAL_REQ_STATS;
        req_hdr.payload_size = 0;

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send stats request to daemon\n");
            ret_code = 1;
            break;
        }

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) &&
            (resp.opcode == WAYWAL_RESP_OK || resp.opcode == WAYWAL_RESP_INFO)) {
            if (resp.payload_size > 0 && resp.payload_size <= 65536) {
                drain_and_print_reply(socket_fd, (size_t)resp.payload_size);
            }
        } else {
            fprintf(stderr, "Error: Failed to query telemetry stats from daemon\n");
            ret_code = 1;
        }
        if (attached_fd >= 0)
            close(attached_fd);
        break;
    }

    case CLI_CMD_RESTORE: {
        req_hdr.opcode = WAYWAL_REQ_RESTORE;
        req_hdr.payload_size = 0;

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send restore request to daemon\n");
            ret_code = 1;
            break;
        }

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            if (resp.payload_size > 0 && resp.payload_size <= 16384) {
                drain_and_print_reply(socket_fd, (size_t)resp.payload_size);
            } else {
                printf("OK: State restored\n");
            }
        } else {
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

    close(socket_fd);
    return ret_code;
}
