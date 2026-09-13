#include "wayland_core.h"
#include "waywal/log.h"
#include "waywal/ipc_proto.h"
#include "waywal/arena.h"
#include "waywal/security.h"
#include "waywal/uring_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>

static void print_usage(const char *prog) {
    printf("wwald - High-performance Wayland wallpaper daemon (C23)\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -n, --namespace <NAME>  Socket namespace (default: \"default\")\n");
    printf("  -v, --verbose           Enable debug logging\n");
    printf("  -h, --help              Print help information\n");
}

static bool read_exact(int fd, void *buf, size_t len) {
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < len) {
        ssize_t r = read(fd, p + total, len - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        total += (size_t)r;
    }
    return true;
}

static bool write_exact(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < len) {
        ssize_t w = write(fd, p + total, len - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += (size_t)w;
    }
    return true;
}

void daemon_handle_ipc_connection(daemon_state_t *state) {
    int client_fd = accept4(state->server_socket_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            WAYWAL_LOG_ERR("accept4 failed: %s", strerror(errno));
        }
        return;
    }

    waywal_ipc_hdr_t req_hdr;
    int attached_fd = -1;
    if (!waywal_ipc_recv(client_fd, &req_hdr, &attached_fd)) {
        close(client_fd);
        return;
    }

    waywal_ipc_hdr_t resp_hdr = {
        .magic        = WAYWAL_IPC_MAGIC,
        .version      = WAYWAL_IPC_VERSION,
        .opcode       = WAYWAL_RESP_OK,
        .payload_size = 0,
    };

    switch (req_hdr.opcode) {
        case WAYWAL_REQ_PING: {
            resp_hdr.opcode = WAYWAL_RESP_PONG;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_QUERY: {
            /* Build output list */
            char query_buf[4096];
            size_t off = 0;
            off += (size_t)snprintf(query_buf + off, sizeof(query_buf) - off,
                                    "Connected outputs (%zu):\n", state->num_outputs);

            for (output_node_t *out = state->outputs; out != NULL; out = out->next) {
                if (off < sizeof(query_buf)) {
                    off += (size_t)snprintf(query_buf + off, sizeof(query_buf) - off,
                                            "  - %s (%s): %dx%d (scale: %d)\n",
                                            out->name[0] ? out->name : "unknown",
                                            out->description[0] ? out->description : "unknown",
                                            out->width, out->height, out->scale_factor);
                }
            }

            resp_hdr.opcode = WAYWAL_RESP_INFO;
            resp_hdr.payload_size = off;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            (void)write_exact(client_fd, query_buf, off);
            break;
        }

        case WAYWAL_REQ_CLEAR: {
            waywal_clear_payload_t clear_payload;
            if (req_hdr.payload_size >= sizeof(clear_payload)) {
                if (read_exact(client_fd, &clear_payload, sizeof(clear_payload))) {
                    color_rgba_t color = {
                        .r = clear_payload.color[0],
                        .g = clear_payload.color[1],
                        .b = clear_payload.color[2],
                        .a = clear_payload.color[3],
                    };
                    daemon_clear_all_outputs(state, color);
                    WAYWAL_LOG_INFO("Cleared screen with color: #%02x%02x%02x%02x",
                                  color.r, color.g, color.b, color.a);
                }
            }
            resp_hdr.opcode = WAYWAL_RESP_OK;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_SET_IMAGE: {
            if (attached_fd >= 0 && req_hdr.payload_size >= sizeof(waywal_img_metadata_t)) {
                void *mapped = mmap(NULL, req_hdr.payload_size, PROT_READ, MAP_SHARED, attached_fd, 0);
                if (mapped != MAP_FAILED) {
                    waywal_img_metadata_t *meta = (waywal_img_metadata_t *)mapped;
                    const uint8_t *pixels = (const uint8_t *)mapped + sizeof(waywal_img_metadata_t);

                    WAYWAL_LOG_INFO("Rendering received image: %ux%u", meta->width, meta->height);

                    bool ok = daemon_start_image_transition(state, pixels, meta->width, meta->height, meta);
                    resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;

                    munmap(mapped, req_hdr.payload_size);
                } else {
                    WAYWAL_LOG_ERR("mmap attached memfd failed: %s", strerror(errno));
                    resp_hdr.opcode = WAYWAL_RESP_ERR;
                }
            } else {
                resp_hdr.opcode = WAYWAL_RESP_ERR;
            }

            if (attached_fd >= 0) {
                close(attached_fd);
            }
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_SET_VIDEO: {
            bool ok = false;
            if (attached_fd >= 0) {
                struct stat st;
                if (fstat(attached_fd, &st) == 0 && st.st_size > 0) {
                    ok = video_engine_load_mem(&state->video_engine, attached_fd, (size_t)st.st_size, NULL);
                }
                close(attached_fd);
                attached_fd = -1;
            } else if (req_hdr.payload_size >= sizeof(waywal_video_payload_t)) {
                waywal_video_payload_t vpay;
                if (read_exact(client_fd, &vpay, sizeof(vpay))) {
                    state->video_engine.loop_count = vpay.loop_count;
                    state->video_engine.playback_speed = vpay.playback_speed > 0.01f ? vpay.playback_speed : 1.0f;
                    ok = video_engine_load_file(&state->video_engine, vpay.filepath, NULL);
                }
            }
            resp_hdr.opcode = ok ? WAYWAL_RESP_OK : WAYWAL_RESP_ERR;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_PAUSE: {
            video_engine_pause(&state->video_engine);
            resp_hdr.opcode = WAYWAL_RESP_OK;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_UNPAUSE: {
            video_engine_resume(&state->video_engine);
            resp_hdr.opcode = WAYWAL_RESP_OK;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_TOGGLE: {
            if (state->video_engine.state == WAYWAL_VIDEO_STATE_PLAYING) {
                video_engine_pause(&state->video_engine);
            } else if (state->video_engine.state == WAYWAL_VIDEO_STATE_PAUSED) {
                video_engine_resume(&state->video_engine);
            }
            resp_hdr.opcode = WAYWAL_RESP_OK;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
        }

        case WAYWAL_REQ_KILL: {
            WAYWAL_LOG_INFO("Received kill request from client");
            resp_hdr.opcode = WAYWAL_RESP_OK;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            state->running = false;
            if (state->uring_loop) {
                uring_loop_stop(state->uring_loop);
            }
            break;
        }

        default:
            resp_hdr.opcode = WAYWAL_RESP_ERR;
            waywal_ipc_send(client_fd, &resp_hdr, -1);
            break;
    }

    close(client_fd);
}

static bool g_daemon_wl_read_ready = false;

static void daemon_uring_event_callback(uring_event_type_t type, int fd, uint32_t res, void *user_data) {
    (void)res;
    daemon_state_t *st = (daemon_state_t *)user_data;
    if (!st) return;

    switch (type) {
        case URING_EV_WAYLAND_READ:
            g_daemon_wl_read_ready = true;
            break;
        case URING_EV_IPC_ACCEPT:
            daemon_handle_ipc_connection(st);
            break;
        case URING_EV_TIMER:
            if (fd == st->transition_timer_fd) {
                transition_engine_dispatch_tick(st);
            } else if (fd == st->video_engine.timer_fd) {
                video_engine_dispatch_frame(&st->video_engine);
            }
            break;
        case URING_EV_SIGNAL: {
            struct signalfd_siginfo fdsi;
            ssize_t s = read(fd, &fdsi, sizeof(fdsi));
            if (s == (ssize_t)sizeof(fdsi)) {
                WAYWAL_LOG_INFO("Received signal %u, shutting down cleanly", fdsi.ssi_signo);
                st->running = false;
                if (st->uring_loop) {
                    uring_loop_stop(st->uring_loop);
                }
            }
            break;
        }
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    daemon_state_t state;
    memset(&state, 0, sizeof(state));
    strncpy(state.namespace_str, "default", sizeof(state.namespace_str) - 1);
    state.current_color = (color_rgba_t){ .r = 0, .g = 0, .b = 0, .a = 255 };
    state.running = true;

    static const struct option long_options[] = {
        {"namespace", required_argument, NULL, 'n'},
        {"verbose",   no_argument,       NULL, 'v'},
        {"help",      no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'n':
                strncpy(state.namespace_str, optarg, sizeof(state.namespace_str) - 1);
                state.namespace_str[sizeof(state.namespace_str) - 1] = '\0';
                break;
            case 'v':
                waywal_log_set_level(WAYWAL_LOG_LEVEL_DEBUG);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
            }
    }

    /* Initialize Dual Arena memory architecture */
    if (!arena_init(&state.persistent_arena, WAYWAL_PERSISTENT_ARENA_CAPACITY)) {
        fprintf(stderr, "Fatal: failed to initialize persistent memory arena\n");
        return 1;
    }
    if (!arena_init(&state.frame_arena, WAYWAL_FRAME_ARENA_CAPACITY)) {
        fprintf(stderr, "Fatal: failed to initialize frame memory arena\n");
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    /* Signal handling with signalfd */
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        WAYWAL_LOG_ERR("sigprocmask failed: %s", strerror(errno));
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    state.signal_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (state.signal_fd < 0) {
        WAYWAL_LOG_ERR("signalfd failed: %s", strerror(errno));
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    /* Initialize Wayland display & layer shell */
    if (!daemon_wayland_init(&state)) {
        WAYWAL_LOG_ERR("Failed to initialize Wayland subsystem");
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    /* Initialize IPC server socket */
    state.server_socket_fd = waywal_ipc_server_create(state.namespace_str);
    if (state.server_socket_fd < 0) {
        WAYWAL_LOG_ERR("Failed to create IPC server socket");
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    /* Setup epoll event multiplexer */
    state.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (state.epoll_fd < 0) {
        WAYWAL_LOG_ERR("epoll_create1 failed: %s", strerror(errno));
        close(state.server_socket_fd);
        daemon_wayland_destroy(&state);
        close(state.signal_fd);
        arena_destroy(&state.frame_arena);
        arena_destroy(&state.persistent_arena);
        return 1;
    }

    int wl_fd = wl_display_get_fd(state.display);

    struct epoll_event ev_wl = { .events = EPOLLIN, .data.fd = wl_fd };
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev_wl);

    struct epoll_event ev_ipc = { .events = EPOLLIN, .data.fd = state.server_socket_fd };
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.server_socket_fd, &ev_ipc);

    struct epoll_event ev_sig = { .events = EPOLLIN, .data.fd = state.signal_fd };
    epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.signal_fd, &ev_sig);

    /* Initialize hardware video wallpaper engine */
    if (!video_engine_init(&state.video_engine, &state)) {
        WAYWAL_LOG_WARN("Hardware video engine initialization failed (video playback will be unavailable)");
    }

    /* Initialize asynchronous transition engine (BUG-03, BUG-04) */
    transition_engine_init(&state);

    if (state.transition_timer_fd >= 0) {
        struct epoll_event ev_trans = { .events = EPOLLIN, .data.fd = state.transition_timer_fd };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.transition_timer_fd, &ev_trans);
    }
    if (state.video_engine.timer_fd >= 0) {
        struct epoll_event ev_vid = { .events = EPOLLIN, .data.fd = state.video_engine.timer_fd };
        epoll_ctl(state.epoll_fd, EPOLL_CTL_ADD, state.video_engine.timer_fd, &ev_vid);
    }

    /* Modern Linux io_uring asynchronous event loop with cooperative kernel polling */
    uring_loop_t uring_loop;
    state.use_uring = uring_loop_init(&uring_loop, WAYWAL_URING_QUEUE_DEPTH);
    if (state.use_uring) {
        state.uring_loop = &uring_loop;
        uring_loop_add_poll(&uring_loop, wl_fd, POLLIN, URING_EV_WAYLAND_READ, &state);
        uring_loop_add_poll(&uring_loop, state.server_socket_fd, POLLIN, URING_EV_IPC_ACCEPT, &state);
        uring_loop_add_poll(&uring_loop, state.signal_fd, POLLIN, URING_EV_SIGNAL, &state);
        if (state.transition_timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.transition_timer_fd, POLLIN, URING_EV_TIMER, &state);
        }
        if (state.video_engine.timer_fd >= 0) {
            uring_loop_add_poll(&uring_loop, state.video_engine.timer_fd, POLLIN, URING_EV_TIMER, &state);
        }
        WAYWAL_LOG_INFO("Event loop running via Linux io_uring (zero-syscall polling)");
    } else {
        WAYWAL_LOG_INFO("Event loop running via epoll fallback");
    }

    /* Apply security hardening (PR_SET_NO_NEW_PRIVS & Landlock LSM) */
    security_sandbox_apply(NULL);

    WAYWAL_LOG_INFO("wwald daemon running successfully (namespace: %s)", state.namespace_str);

    /* Main event loop */
    while (state.running) {
        while (wl_display_prepare_read(state.display) != 0) {
            wl_display_dispatch_pending(state.display);
        }
        wl_display_flush(state.display);

        bool wl_read_ready = false;

        if (state.use_uring) {
            g_daemon_wl_read_ready = false;
            int ret = uring_loop_dispatch(&uring_loop, daemon_uring_event_callback);
            if (ret < 0 && ret != -EINTR) {
                wl_display_cancel_read(state.display);
                WAYWAL_LOG_ERR("io_uring dispatch failed: %s", strerror(-ret));
                break;
            }
            wl_read_ready = g_daemon_wl_read_ready;
        } else {
            struct epoll_event events[16];
            int nfds = epoll_wait(state.epoll_fd, events, 16, -1);
            if (nfds < 0) {
                wl_display_cancel_read(state.display);
                if (errno == EINTR) continue;
                WAYWAL_LOG_ERR("epoll_wait failed: %s", strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == wl_fd) {
                    wl_read_ready = true;
                } else if (events[i].data.fd == state.server_socket_fd) {
                    daemon_handle_ipc_connection(&state);
                } else if (events[i].data.fd == state.transition_timer_fd) {
                    transition_engine_dispatch_tick(&state);
                } else if (events[i].data.fd == state.video_engine.timer_fd) {
                    video_engine_dispatch_frame(&state.video_engine);
                } else if (events[i].data.fd == state.signal_fd) {
                    struct signalfd_siginfo fdsi;
                    ssize_t s = read(state.signal_fd, &fdsi, sizeof(fdsi));
                    if (s == sizeof(fdsi)) {
                        WAYWAL_LOG_INFO("Received signal %u, shutting down cleanly", fdsi.ssi_signo);
                        state.running = false;
                    }
                }
            }
        }

        if (wl_read_ready) {
            wl_display_read_events(state.display);
        } else {
            wl_display_cancel_read(state.display);
        }
        wl_display_dispatch_pending(state.display);

        /* Monotonic linear frame arena reset: zero fragmentation in steady-state loop */
        arena_reset(&state.frame_arena);
    }

    WAYWAL_LOG_INFO("Shutting down wwald...");

    transition_engine_destroy(&state);
    video_engine_destroy(&state.video_engine);

    if (state.use_uring) {
        uring_loop_destroy(&uring_loop);
        state.uring_loop = NULL;
    }

    /* Clean up IPC socket file */
    path_buf_t sock_path;
    if (waywal_ipc_resolve_socket_path(&sock_path, state.namespace_str)) {
        unlink(path_buf_cstr(&sock_path));
    }

    close(state.server_socket_fd);
    close(state.epoll_fd);
    close(state.signal_fd);
    daemon_wayland_destroy(&state);
    arena_destroy(&state.frame_arena);
    arena_destroy(&state.persistent_arena);

    WAYWAL_LOG_INFO("Clean shutdown complete.");
    return 0;
}
