#include "cli_parser.h"
#include "image_loader.h"
#include "waywal/ipc_proto.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
        waywal_clear_payload_t payload = {
            .color = {opts.clear_color.r, opts.clear_color.g, opts.clear_color.b,
                      opts.clear_color.a},
            .num_target_outputs = 0,
        };
        req_hdr.opcode = WAYWAL_REQ_CLEAR;
        req_hdr.payload_size = sizeof(payload);

        if (!waywal_ipc_send(socket_fd, &req_hdr, -1)) {
            fprintf(stderr, "Error: Failed to send clear request\n");
            ret_code = 1;
            break;
        }
        (void)write(socket_fd, &payload, sizeof(payload));

        waywal_ipc_hdr_t resp;
        int attached_fd = -1;
        if (waywal_ipc_recv(socket_fd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_OK) {
            printf("Wallpaper cleared to #%02x%02x%02x%02x\n", payload.color[0], payload.color[1],
                   payload.color[2], payload.color[3]);
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
        uint8_t *pixels = NULL;
        uint32_t width = 0, height = 0;
        if (!image_load(opts.filepath, &pixels, &width, &height)) {
            fprintf(stderr, "Error: Failed to decode image file '%s'\n", opts.filepath);
            ret_code = 1;
            break;
        }

        size_t pixel_bytes = (size_t)width * height * 4;
        size_t total_payload_size = sizeof(waywal_img_metadata_t) + pixel_bytes;

        int memfd = waywal_create_memfd("wwal-client-img", total_payload_size,
                                        F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
        if (memfd < 0) {
            fprintf(stderr, "Error: Failed to create sealed memfd for image\n");
            image_free(pixels);
            ret_code = 1;
            break;
        }

        void *mapped = mmap(NULL, total_payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (mapped == MAP_FAILED) {
            fprintf(stderr, "Error: mmap failed on client memfd\n");
            close(memfd);
            image_free(pixels);
            ret_code = 1;
            break;
        }

        waywal_img_metadata_t meta;
        memset(&meta, 0, sizeof(meta));
        meta.width = width;
        meta.height = height;
        meta.pixel_format = WAYWAL_FORMAT_ARGB8888;
        meta.transition_type = opts.transition_type;
        meta.transition_duration_ms = opts.transition_duration_ms;
        meta.transition_fps = opts.transition_fps;
        meta.transition_angle_rad = opts.transition_angle_rad;
        meta.transition_wave_freq = opts.transition_wave_freq;
        meta.transition_wave_amp = opts.transition_wave_amp;
        meta.transition_center_x = opts.transition_pos_x;
        meta.transition_center_y = opts.transition_pos_y;
        meta.num_target_outputs = 0;

        memcpy(mapped, &meta, sizeof(meta));
        memcpy((uint8_t *)mapped + sizeof(meta), pixels, pixel_bytes);
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

    default:
        break;
    }

    close(socket_fd);
    return ret_code;
}
