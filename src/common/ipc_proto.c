// SPDX-License-Identifier: GPL-3.0-or-later
#include "waywal/ipc_proto.h"

#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool g_socket_activated = false;

bool waywal_ipc_is_socket_activated(void) { return g_socket_activated; }

bool waywal_ipc_resolve_socket_path(path_buf_t *out_path, const char *namespace_str)
{
    if (!out_path)
        return false;

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

    if (!namespace_str || namespace_str[0] == '\0') {
        namespace_str = "default";
    }

    path_buf_init(out_path);
    path_buf_from_str(out_path, xdg);

    char filename[256];
    int n = snprintf(filename, sizeof(filename), "%s-wwald.%s.sock", wl_disp, namespace_str);
    if (n < 0 || (size_t)n >= sizeof(filename)) {
        return false;
    }

    return path_buf_push(out_path, filename);
}

int waywal_ipc_server_create(const char *namespace_str)
{
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (listen_pid && listen_fds && atoi(listen_pid) == (int)getpid() && atoi(listen_fds) >= 1) {
        int sfd = 3;
        int fd_flags = fcntl(sfd, F_GETFD, 0);
        if (fd_flags >= 0) {
            fcntl(sfd, F_SETFD, fd_flags | FD_CLOEXEC);
        }
        int fl_flags = fcntl(sfd, F_GETFL, 0);
        if (fl_flags >= 0) {
            fcntl(sfd, F_SETFL, fl_flags | O_NONBLOCK);
        }
        g_socket_activated = true;
        WAYWAL_LOG_INFO("IPC server using systemd socket activation (fd=%d)", sfd);
        return sfd;
    }

    g_socket_activated = false;

    path_buf_t sock_path;
    if (!waywal_ipc_resolve_socket_path(&sock_path, namespace_str)) {
        WAYWAL_LOG_ERR("Failed to resolve IPC socket path");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    const char *path_cstr = path_buf_cstr(&sock_path);
    if (strlen(path_cstr) >= sizeof(addr.sun_path)) {
        WAYWAL_LOG_ERR("IPC socket path exceeds max UNIX socket path: %s", path_cstr);
        return -1;
    }
    strncpy(addr.sun_path, path_cstr, sizeof(addr.sun_path) - 1);

    /* Check if socket exists and handle stale socket recovery */
    if (access(addr.sun_path, F_OK) == 0) {
        int test_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (test_fd >= 0) {
            if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(test_fd);
                WAYWAL_LOG_ERR("Another wwald daemon is already listening on %s", addr.sun_path);
                return -1;
            }
            int err = errno;
            close(test_fd);

            if (err == ECONNREFUSED || err == ENOENT) {
                WAYWAL_LOG_WARN("Unlinking stale IPC socket at %s", addr.sun_path);
                unlink(addr.sun_path);
            } else {
                WAYWAL_LOG_ERR("Failed to test existing socket %s: %s", addr.sun_path,
                               strerror(err));
                return -1;
            }
        }
    }

    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sfd < 0) {
        WAYWAL_LOG_ERR("socket(AF_UNIX) failed: %s", strerror(errno));
        return -1;
    }

    int sbuf = 262144;
    setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &sbuf, sizeof(sbuf));
    setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &sbuf, sizeof(sbuf));

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        WAYWAL_LOG_ERR("bind(%s) failed: %s", addr.sun_path, strerror(errno));
        close(sfd);
        return -1;
    }

    if (listen(sfd, 16) < 0) {
        WAYWAL_LOG_ERR("listen() failed: %s", strerror(errno));
        unlink(addr.sun_path);
        close(sfd);
        return -1;
    }

    WAYWAL_LOG_INFO("IPC server listening on %s", addr.sun_path);
    return sfd;
}

static int connect_unix_socket_path(const char *sock_path, int timeout_ms)
{
    if (!sock_path || sock_path[0] == '\0') {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    int cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (cfd < 0) {
        return -1;
    }

    int cbuf = 262144;
    setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &cbuf, sizeof(cbuf));
    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &cbuf, sizeof(cbuf));

    if (timeout_ms <= 0) {
        if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            int saved_err = errno;
            close(cfd);
            errno = saved_err;
            return -1;
        }
        return cfd;
    }

    int flags = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

    int res = connect(cfd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        int saved_err = errno;
        close(cfd);
        errno = saved_err;
        return -1;
    }

    if (res == 0) {
        fcntl(cfd, F_SETFL, flags);
        return cfd;
    }

    struct pollfd pfd = {.fd = cfd, .events = POLLOUT};
    int poll_res = poll(&pfd, 1, timeout_ms);
    if (poll_res <= 0) {
        close(cfd);
        errno = ETIMEDOUT;
        return -1;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(cfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
        close(cfd);
        errno = so_error ? so_error : ECONNREFUSED;
        return -1;
    }

    fcntl(cfd, F_SETFL, flags);
    return cfd;
}

bool waywal_ipc_namespace_is_alive(const char *namespace_str)
{
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (listen_pid && listen_fds && atoi(listen_pid) == (int)getpid() && atoi(listen_fds) >= 1) {
        return false;
    }

    path_buf_t sock_path;
    if (!waywal_ipc_resolve_socket_path(&sock_path, namespace_str)) {
        return false;
    }

    int cfd = connect_unix_socket_path(path_buf_cstr(&sock_path), 100);
    if (cfd < 0) {
        return false;
    }

    waywal_ipc_hdr_t ping_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_PING,
        .payload_size = 0,
    };
    bool alive = false;
    if (waywal_ipc_send(cfd, &ping_hdr, -1)) {
        struct pollfd pfd = {.fd = cfd, .events = POLLIN};
        int pr = poll(&pfd, 1, 1000);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            waywal_ipc_hdr_t resp;
            int attached_fd = -1;
            if (waywal_ipc_recv(cfd, &resp, &attached_fd) && resp.opcode == WAYWAL_RESP_PONG) {
                alive = true;
            }
            if (attached_fd >= 0) {
                close(attached_fd);
            }
        } else if (pr == 0) {
            /* Connected to an active kernel listening socket whose main thread is temporarily busy
             */
            alive = true;
        }
    }
    close(cfd);
    return alive;
}

int waywal_ipc_client_connect(const char *namespace_str, int timeout_ms)
{
    path_buf_t sock_path;
    if (!waywal_ipc_resolve_socket_path(&sock_path, namespace_str)) {
        return -1;
    }

    int cfd = connect_unix_socket_path(path_buf_cstr(&sock_path), timeout_ms);
    if (cfd >= 0) {
        return cfd;
    }

    bool is_default_ns =
        (!namespace_str || namespace_str[0] == '\0' || strcmp(namespace_str, "default") == 0);
    if (is_default_ns && (errno == ENOENT || errno == ECONNREFUSED)) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg || xdg[0] == '\0') {
            xdg = "/tmp";
        }
        path_buf_t fallback_path;
        path_buf_from_str(&fallback_path, xdg);
        if (path_buf_push(&fallback_path, "wwald.sock")) {
            cfd = connect_unix_socket_path(path_buf_cstr(&fallback_path), timeout_ms);
            if (cfd >= 0) {
                return cfd;
            }
        }
    }

    return -1;
}

bool waywal_ipc_send_fds(int socket_fd, const waywal_ipc_hdr_t *hdr, const int *fds, size_t num_fds)
{
    if (socket_fd < 0 || !hdr)
        return false;

    struct msghdr msg = {0};

    struct iovec iov = {.iov_base = (void *)hdr, .iov_len = sizeof(waywal_ipc_hdr_t)};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int) * 4)];
        struct cmsghdr align;
    } u;

    if (fds && num_fds > 0) {
        if (num_fds > 4) {
            num_fds = 4;
        }
        memset(u.buf, 0, sizeof(u.buf));
        msg.msg_control = u.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * num_fds);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);
    }

    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    return (sent == (ssize_t)sizeof(waywal_ipc_hdr_t));
}

bool waywal_ipc_send(int socket_fd, const waywal_ipc_hdr_t *hdr, int attached_fd)
{
    if (attached_fd >= 0) {
        return waywal_ipc_send_fds(socket_fd, hdr, &attached_fd, 1);
    }
    return waywal_ipc_send_fds(socket_fd, hdr, NULL, 0);
}

bool waywal_ipc_recv_fds(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_fds, size_t max_fds,
                         size_t *out_num_fds)
{
    if (socket_fd < 0 || !out_hdr)
        return false;
    if (out_num_fds)
        *out_num_fds = 0;
    if (out_fds) {
        for (size_t i = 0; i < max_fds; ++i) {
            out_fds[i] = -1;
        }
    }

    struct msghdr msg = {0};

    struct iovec iov = {.iov_base = (void *)out_hdr, .iov_len = sizeof(waywal_ipc_hdr_t)};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Accommodate Linux SCM_MAX_FD (253) so excess ancillary FDs are never truncated and leaked */
    union {
        char buf[CMSG_SPACE(sizeof(int) * 253)];
        struct cmsghdr align;
    } u;
    memset(u.buf, 0, sizeof(u.buf));
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);

    ssize_t r;
    do {
        r = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (r < 0 && errno == EINTR);

    if (r <= 0) {
        return false;
    }

    size_t extracted = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(0)) {
            size_t fd_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const unsigned char *data = CMSG_DATA(cmsg);
            for (size_t i = 0; i < fd_count; ++i) {
                int fd = -1;
                memcpy(&fd, data + i * sizeof(int), sizeof(int));
                if (fd >= 0) {
                    if (out_fds && extracted < max_fds) {
                        out_fds[extracted++] = fd;
                    } else {
                        close(fd);
                    }
                }
            }
        }
    }
    if (out_num_fds) {
        *out_num_fds = extracted;
    }

    /* Complete reading the full header if initial recvmsg was partial */
    size_t total_read = (size_t)r;
    uint8_t *hdr_bytes = (uint8_t *)out_hdr;
    while (total_read < sizeof(waywal_ipc_hdr_t)) {
        ssize_t rem =
            recv(socket_fd, hdr_bytes + total_read, sizeof(waywal_ipc_hdr_t) - total_read, 0);
        if (rem < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = socket_fd, .events = POLLIN};
                if (poll(&pfd, 1, 100) > 0)
                    continue;
            }
            if (out_fds) {
                for (size_t i = 0; i < extracted; ++i) {
                    if (out_fds[i] >= 0) {
                        close(out_fds[i]);
                        out_fds[i] = -1;
                    }
                }
            }
            if (out_num_fds)
                *out_num_fds = 0;
            return false;
        }
        if (rem == 0) {
            if (out_fds) {
                for (size_t i = 0; i < extracted; ++i) {
                    if (out_fds[i] >= 0) {
                        close(out_fds[i]);
                        out_fds[i] = -1;
                    }
                }
            }
            if (out_num_fds)
                *out_num_fds = 0;
            return false;
        }
        total_read += (size_t)rem;
    }

    if (out_hdr->magic != WAYWAL_IPC_MAGIC || out_hdr->version != WAYWAL_IPC_VERSION) {
        WAYWAL_LOG_ERR("IPC header magic/version mismatch: magic=0x%08x ver=%u", out_hdr->magic,
                       out_hdr->version);
        if (out_fds) {
            for (size_t i = 0; i < extracted; ++i) {
                if (out_fds[i] >= 0) {
                    close(out_fds[i]);
                    out_fds[i] = -1;
                }
            }
        }
        if (out_num_fds)
            *out_num_fds = 0;
        return false;
    }

    return true;
}

bool waywal_ipc_recv(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_attached_fd)
{
    int fd = -1;
    size_t count = 0;
    bool ok = waywal_ipc_recv_fds(socket_fd, out_hdr, &fd, 1, &count);
    if (out_attached_fd) {
        *out_attached_fd = (ok && count > 0) ? fd : -1;
    } else if (count > 0 && fd >= 0) {
        close(fd);
    }
    return ok;
}

/* ========================================================================= */
/* Workshop, Open Hub & Compatibility IPC Serialization Helpers             */
/* ========================================================================= */

static inline size_t ipc_bounded_cstr_len(const char *s, size_t max_len)
{
    size_t n = 0;
    while (n < max_len && s[n] != '\0') {
        ++n;
    }
    return n;
}

bool waywal_ipc_validate_req_workshop_sync(const waywal_ipc_req_workshop_sync_t *req,
                                           size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_workshop_sync_t)) {
        return false;
    }
    if (memchr(req->custom_root, '\0', sizeof(req->custom_root)) == NULL) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_req_workshop_sync(uint32_t flags, const char *custom_root,
                                            waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_req_workshop_sync_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }
    size_t root_len = 0;
    if (custom_root) {
        root_len = ipc_bounded_cstr_len(custom_root, sizeof(out_req->custom_root));
        if (root_len >= sizeof(out_req->custom_root)) {
            return false;
        }
    }
    memset(out_req, 0, sizeof(*out_req));
    out_req->flags = flags;
    if (root_len > 0) {
        memcpy(out_req->custom_root, custom_root, root_len);
    }
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_WORKSHOP_SYNC;
    out_hdr->payload_size = sizeof(waywal_ipc_req_workshop_sync_t);
    return true;
}

bool waywal_ipc_validate_resp_workshop_sync(const waywal_ipc_resp_workshop_sync_t *resp,
                                            size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_workshop_sync_t)) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_resp_workshop_sync(uint32_t status_code, uint32_t total_libraries_found,
                                             uint32_t total_items_indexed,
                                             uint64_t total_size_bytes, waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_workshop_sync_t *out_resp)
{
    if (!out_hdr || !out_resp) {
        return false;
    }
    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    out_resp->total_libraries_found = total_libraries_found;
    out_resp->total_items_indexed = total_items_indexed;
    out_resp->_reserved = 0;
    out_resp->total_size_bytes = total_size_bytes;
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_WORKSHOP_SYNC;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_workshop_sync_t);
    return true;
}

bool waywal_ipc_validate_req_workshop_download(const waywal_ipc_req_workshop_download_t *req,
                                               size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_workshop_download_t)) {
        return false;
    }
    waywal_ipc_req_workshop_download_t copy;
    memcpy(&copy, req, sizeof(copy));
    if (copy.published_file_id == 0) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_req_workshop_download(uint64_t published_file_id, uint32_t priority,
                                                uint32_t preferred_backend,
                                                waywal_ipc_hdr_t *out_hdr,
                                                waywal_ipc_req_workshop_download_t *out_req)
{
    if (!out_hdr || !out_req || published_file_id == 0) {
        return false;
    }
    memset(out_req, 0, sizeof(*out_req));
    out_req->published_file_id = published_file_id;
    out_req->priority = priority;
    out_req->preferred_backend = preferred_backend;
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_WORKSHOP_DOWNLOAD;
    out_hdr->payload_size = sizeof(waywal_ipc_req_workshop_download_t);
    return true;
}

bool waywal_ipc_validate_resp_workshop_download(const waywal_ipc_resp_workshop_download_t *resp,
                                                size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_workshop_download_t)) {
        return false;
    }
    waywal_ipc_resp_workshop_download_t copy;
    memcpy(&copy, resp, sizeof(copy));
    if (!isfinite(copy.progress_pct) || !isfinite(copy.speed_bytes_per_sec)) {
        return false;
    }
    if (memchr(copy.error_msg, '\0', sizeof(copy.error_msg)) == NULL) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_resp_workshop_download(uint64_t published_file_id, uint32_t task_state,
                                                 float progress_pct, float speed_bytes_per_sec,
                                                 uint32_t eta_seconds, const char *error_msg,
                                                 waywal_ipc_hdr_t *out_hdr,
                                                 waywal_ipc_resp_workshop_download_t *out_resp)
{
    if (!out_hdr || !out_resp || !isfinite(progress_pct) || !isfinite(speed_bytes_per_sec)) {
        return false;
    }
    size_t err_len = 0;
    if (error_msg) {
        err_len = ipc_bounded_cstr_len(error_msg, sizeof(out_resp->error_msg));
        if (err_len >= sizeof(out_resp->error_msg)) {
            return false;
        }
    }
    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->published_file_id = published_file_id;
    out_resp->task_state = task_state;
    out_resp->progress_pct = progress_pct;
    out_resp->speed_bytes_per_sec = speed_bytes_per_sec;
    out_resp->eta_seconds = eta_seconds;
    if (err_len > 0) {
        memcpy(out_resp->error_msg, error_msg, err_len);
    }
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_WORKSHOP_DOWNLOAD;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_workshop_download_t);
    return true;
}

bool waywal_ipc_validate_req_wwpkg_install(const waywal_ipc_req_wwpkg_install_t *req,
                                           size_t payload_size, const char **out_target_output)
{
    if (!req || payload_size < sizeof(waywal_ipc_req_wwpkg_install_t)) {
        return false;
    }
    waywal_ipc_req_wwpkg_install_t hdr_copy;
    memcpy(&hdr_copy, req, sizeof(hdr_copy));
    if (hdr_copy.target_output_len > WAYWAL_IPC_MAX_TAIL_STR) {
        return false;
    }
    if (payload_size !=
        sizeof(waywal_ipc_req_wwpkg_install_t) + (size_t)hdr_copy.target_output_len) {
        return false;
    }
    const char *resolved = "";
    if (hdr_copy.target_output_len > 0) {
        const char *tail = (const char *)req + sizeof(waywal_ipc_req_wwpkg_install_t);
        if (tail[hdr_copy.target_output_len - 1] != '\0' ||
            ipc_bounded_cstr_len(tail, hdr_copy.target_output_len) !=
                (size_t)(hdr_copy.target_output_len - 1)) {
            return false;
        }
        resolved = tail;
    }
    if (out_target_output) {
        *out_target_output = resolved;
    }
    return true;
}

bool waywal_ipc_serialize_req_wwpkg_install(const uint8_t package_uuid[16], uint32_t flags,
                                            const char *target_output, waywal_ipc_hdr_t *out_hdr,
                                            waywal_ipc_req_wwpkg_install_t *out_req, void *out_buf,
                                            size_t out_buf_cap, size_t *out_total_len)
{
    if (!out_hdr) {
        return false;
    }
    size_t wire_tail_len = 0;
    if (target_output && target_output[0] != '\0') {
        size_t str_len = ipc_bounded_cstr_len(target_output, WAYWAL_IPC_MAX_TAIL_STR);
        if (str_len >= WAYWAL_IPC_MAX_TAIL_STR) {
            return false;
        }
        /* Wire tail carries the trailing NUL so C-string consumers cannot read past payload_size */
        wire_tail_len = str_len + 1;
    }
    size_t total_len = sizeof(waywal_ipc_req_wwpkg_install_t) + wire_tail_len;
    if (out_total_len) {
        *out_total_len = total_len;
    }
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_WWPKG_INSTALL;
    out_hdr->payload_size = (uint32_t)total_len;

    if (out_req) {
        memset(out_req, 0, sizeof(*out_req));
        if (package_uuid) {
            memcpy(out_req->package_uuid, package_uuid, 16);
        }
        out_req->flags = flags;
        out_req->target_output_len = (uint32_t)wire_tail_len;
    }
    if (out_buf) {
        if (out_buf_cap < total_len) {
            return false;
        }
        waywal_ipc_req_wwpkg_install_t *hdr_part = (waywal_ipc_req_wwpkg_install_t *)out_buf;
        memset(hdr_part, 0, sizeof(*hdr_part));
        if (package_uuid) {
            memcpy(hdr_part->package_uuid, package_uuid, 16);
        }
        hdr_part->flags = flags;
        hdr_part->target_output_len = (uint32_t)wire_tail_len;
        if (wire_tail_len > 0) {
            memcpy((char *)out_buf + sizeof(*hdr_part), target_output, wire_tail_len);
        }
    }
    return true;
}

bool waywal_ipc_validate_resp_wwpkg_install(const waywal_ipc_resp_wwpkg_install_t *resp,
                                            size_t payload_size, const char **out_error_msg)
{
    if (!resp || payload_size < sizeof(waywal_ipc_resp_wwpkg_install_t)) {
        return false;
    }
    waywal_ipc_resp_wwpkg_install_t hdr_copy;
    memcpy(&hdr_copy, resp, sizeof(hdr_copy));
    if (hdr_copy.error_msg_len > WAYWAL_IPC_MAX_TAIL_STR) {
        return false;
    }
    if (payload_size != sizeof(waywal_ipc_resp_wwpkg_install_t) + (size_t)hdr_copy.error_msg_len) {
        return false;
    }
    const char *resolved = "";
    if (hdr_copy.error_msg_len > 0) {
        const char *tail = (const char *)resp + sizeof(waywal_ipc_resp_wwpkg_install_t);
        if (tail[hdr_copy.error_msg_len - 1] != '\0' ||
            ipc_bounded_cstr_len(tail, hdr_copy.error_msg_len) !=
                (size_t)(hdr_copy.error_msg_len - 1)) {
            return false;
        }
        resolved = tail;
    }
    if (out_error_msg) {
        *out_error_msg = resolved;
    }
    return true;
}

bool waywal_ipc_serialize_resp_wwpkg_install(const uint8_t package_uuid[16], uint32_t status_code,
                                             const char *error_msg, waywal_ipc_hdr_t *out_hdr,
                                             waywal_ipc_resp_wwpkg_install_t *out_resp,
                                             void *out_buf, size_t out_buf_cap,
                                             size_t *out_total_len)
{
    if (!out_hdr) {
        return false;
    }
    size_t wire_tail_len = 0;
    if (status_code != 0 && error_msg && error_msg[0] != '\0') {
        size_t str_len = ipc_bounded_cstr_len(error_msg, WAYWAL_IPC_MAX_TAIL_STR);
        if (str_len >= WAYWAL_IPC_MAX_TAIL_STR) {
            return false;
        }
        wire_tail_len = str_len + 1;
    }
    size_t total_len = sizeof(waywal_ipc_resp_wwpkg_install_t) + wire_tail_len;
    if (out_total_len) {
        *out_total_len = total_len;
    }
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_WWPKG_INSTALL;
    out_hdr->payload_size = (uint32_t)total_len;

    if (out_resp) {
        memset(out_resp, 0, sizeof(*out_resp));
        if (package_uuid) {
            memcpy(out_resp->package_uuid, package_uuid, 16);
        }
        out_resp->status_code = status_code;
        out_resp->error_msg_len = (uint32_t)wire_tail_len;
    }
    if (out_buf) {
        if (out_buf_cap < total_len) {
            return false;
        }
        waywal_ipc_resp_wwpkg_install_t *hdr_part = (waywal_ipc_resp_wwpkg_install_t *)out_buf;
        memset(hdr_part, 0, sizeof(*hdr_part));
        if (package_uuid) {
            memcpy(hdr_part->package_uuid, package_uuid, 16);
        }
        hdr_part->status_code = status_code;
        hdr_part->error_msg_len = (uint32_t)wire_tail_len;
        if (wire_tail_len > 0) {
            memcpy((char *)out_buf + sizeof(*hdr_part), error_msg, wire_tail_len);
        }
    }
    return true;
}

bool waywal_ipc_validate_req_scan_compat(const waywal_ipc_req_scan_compat_t *req,
                                         size_t payload_size)
{
    if (!req || payload_size != sizeof(waywal_ipc_req_scan_compat_t)) {
        return false;
    }
    if (memchr(req->target_path, '\0', sizeof(req->target_path)) == NULL) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_req_scan_compat(const char *target_path, uint32_t auto_repair_flags,
                                          waywal_ipc_hdr_t *out_hdr,
                                          waywal_ipc_req_scan_compat_t *out_req)
{
    if (!out_hdr || !out_req) {
        return false;
    }
    size_t path_len = 0;
    if (target_path) {
        path_len = ipc_bounded_cstr_len(target_path, sizeof(out_req->target_path));
        if (path_len >= sizeof(out_req->target_path)) {
            return false;
        }
    }
    memset(out_req, 0, sizeof(*out_req));
    if (path_len > 0) {
        memcpy(out_req->target_path, target_path, path_len);
    }
    out_req->auto_repair_flags = auto_repair_flags;
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_REQ_SCAN_COMPAT;
    out_hdr->payload_size = sizeof(waywal_ipc_req_scan_compat_t);
    return true;
}

bool waywal_ipc_validate_resp_scan_compat(const waywal_ipc_resp_scan_compat_t *resp,
                                          size_t payload_size)
{
    if (!resp || payload_size != sizeof(waywal_ipc_resp_scan_compat_t)) {
        return false;
    }
    waywal_ipc_resp_scan_compat_t copy;
    memcpy(&copy, resp, sizeof(copy));
    if (!isfinite(copy.estimated_vram_mb)) {
        return false;
    }
    if (memchr(copy.report_json_path, '\0', sizeof(copy.report_json_path)) == NULL) {
        return false;
    }
    return true;
}

bool waywal_ipc_serialize_resp_scan_compat(uint32_t status_code, uint32_t compat_tier,
                                           uint32_t compat_score, uint32_t total_issues,
                                           uint32_t repairs_applied, float estimated_vram_mb,
                                           uint32_t estimated_fps, const char *report_json_path,
                                           waywal_ipc_hdr_t *out_hdr,
                                           waywal_ipc_resp_scan_compat_t *out_resp)
{
    if (!out_hdr || !out_resp || !isfinite(estimated_vram_mb)) {
        return false;
    }
    size_t report_len = 0;
    if (report_json_path) {
        report_len = ipc_bounded_cstr_len(report_json_path, sizeof(out_resp->report_json_path));
        if (report_len >= sizeof(out_resp->report_json_path)) {
            return false;
        }
    }
    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->status_code = status_code;
    out_resp->compat_tier = compat_tier;
    out_resp->compat_score = compat_score;
    out_resp->total_issues = total_issues;
    out_resp->repairs_applied = repairs_applied;
    out_resp->estimated_vram_mb = estimated_vram_mb;
    out_resp->estimated_fps = estimated_fps;
    if (report_len > 0) {
        memcpy(out_resp->report_json_path, report_json_path, report_len);
    }
    out_hdr->magic = WAYWAL_IPC_MAGIC;
    out_hdr->version = WAYWAL_IPC_VERSION;
    out_hdr->opcode = WAYWAL_RESP_SCAN_COMPAT;
    out_hdr->payload_size = sizeof(waywal_ipc_resp_scan_compat_t);
    return true;
}
