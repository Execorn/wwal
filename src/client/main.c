#include "cli_parser.h"
#include "image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

int main(int argc, char *argv[])
{
    cli_options_t opts;
    if (!cli_parse(argc, argv, &opts)) {
        cli_print_usage(argv[0]);
        return 1;
    }

    if (opts.cmd == CLI_CMD_HELP || opts.cmd == CLI_CMD_NONE) {
        cli_print_usage(argv[0]);
        return 0;
    }

    if (opts.verbose) {
        waywal_log_set_level(WAYWAL_LOG_LEVEL_DEBUG);
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
        int video_fd = open(opts.filepath, O_RDONLY | O_CLOEXEC);
        req_hdr.opcode = WAYWAL_REQ_SET_VIDEO;

        if (video_fd >= 0) {
            req_hdr.payload_size = 0;
            if (!waywal_ipc_send(socket_fd, &req_hdr, video_fd)) {
                fprintf(stderr, "Error: Failed to send video fd to daemon\n");
                close(video_fd);
                ret_code = 1;
                break;
            }
            close(video_fd);
        } else {
            waywal_video_payload_t vpay;
            memset(&vpay, 0, sizeof(vpay));
            vpay.loop_count = opts.video_loop_count;
            vpay.playback_speed = opts.video_speed;
            strncpy(vpay.filepath, opts.filepath, sizeof(vpay.filepath) - 1);
            req_hdr.payload_size = sizeof(vpay);

            if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
                fprintf(stderr, "Error: Failed to send video request to daemon\n");
                ret_code = 1;
                break;
            }
            (void)write(socket_fd, &vpay, sizeof(vpay));
        }

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
        if (!image_load(opts.filepath, &pixels, &width, &height)) {
            fprintf(stderr, "Error: Failed to decode image file '%s'\n", opts.filepath);
            free(shader_src);
            ret_code = 1;
            break;
        }

        size_t pixel_bytes = (size_t)width * height * 4;
        size_t total_payload_size = sizeof(waywal_img_metadata_t) + extra_bytes + pixel_bytes;

        int memfd = waywal_create_memfd("wwal-client-img", total_payload_size,
                                        F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
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
        meta.num_target_outputs = opts.num_outputs;
        meta.sync_mode = opts.sync_mode;
        meta.stagger_delay_ms = opts.stagger_delay_ms;
        meta.custom_shader_len = (uint32_t)shader_len;
        meta.extra_data_len = (uint32_t)extra_bytes;
        meta.scaling_mode = opts.scaling_mode;

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

    default:
        break;
    }

    close(socket_fd);
    return ret_code;
}
