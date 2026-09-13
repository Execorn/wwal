#include "waywal/ipc_proto.h"
#include "waywal/log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>

bool waywal_ipc_resolve_socket_path(path_buf_t *out_path, const char *namespace_str) {
    if (!out_path) return false;

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || xdg[0] == '\0') {
        xdg = "/tmp";
    }

    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    if (!wl_disp || wl_disp[0] == '\0') {
        wl_disp = "wayland-0";
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

int waywal_ipc_server_create(const char *namespace_str) {
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
                WAYWAL_LOG_ERR("Failed to test existing socket %s: %s", addr.sun_path, strerror(err));
                return -1;
            }
        }
    }

    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sfd < 0) {
        WAYWAL_LOG_ERR("socket(AF_UNIX) failed: %s", strerror(errno));
        return -1;
    }

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

int waywal_ipc_client_connect(const char *namespace_str, int timeout_ms) {
    path_buf_t sock_path;
    if (!waywal_ipc_resolve_socket_path(&sock_path, namespace_str)) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_buf_cstr(&sock_path), sizeof(addr.sun_path) - 1);

    int cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (cfd < 0) {
        return -1;
    }

    if (timeout_ms <= 0) {
        if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(cfd);
            return -1;
        }
        return cfd;
    }

    /* Non-blocking connect with timeout */
    int flags = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

    int res = connect(cfd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(cfd);
        return -1;
    }

    if (res == 0) {
        fcntl(cfd, F_SETFL, flags);
        return cfd;
    }

    struct pollfd pfd = { .fd = cfd, .events = POLLOUT };
    int poll_res = poll(&pfd, 1, timeout_ms);
    if (poll_res <= 0) {
        close(cfd);
        return -1;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(cfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
        close(cfd);
        return -1;
    }

    fcntl(cfd, F_SETFL, flags);
    return cfd;
}

bool waywal_ipc_send(int socket_fd, const waywal_ipc_hdr_t *hdr, int attached_fd) {
    if (socket_fd < 0 || !hdr) return false;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov = {
        .iov_base = (void *)hdr,
        .iov_len  = sizeof(waywal_ipc_hdr_t)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;

    if (attached_fd >= 0) {
        memset(u.buf, 0, sizeof(u.buf));
        msg.msg_control = u.buf;
        msg.msg_controllen = sizeof(u.buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &attached_fd, sizeof(int));
    }

    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    return (sent == (ssize_t)sizeof(waywal_ipc_hdr_t));
}

bool waywal_ipc_recv(int socket_fd, waywal_ipc_hdr_t *out_hdr, int *out_attached_fd) {
    if (socket_fd < 0 || !out_hdr) return false;
    if (out_attached_fd) *out_attached_fd = -1;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov = {
        .iov_base = (void *)out_hdr,
        .iov_len  = sizeof(waywal_ipc_hdr_t)
    };
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int)) * 2];
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

    /* Extract any SCM_RIGHTS FD delivered on the first byte immediately */
    int received_fd = -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    /* Complete reading the full header if initial recvmsg was partial */
    size_t total_read = (size_t)r;
    uint8_t *hdr_bytes = (uint8_t *)out_hdr;
    while (total_read < sizeof(waywal_ipc_hdr_t)) {
        ssize_t rem = recv(socket_fd, hdr_bytes + total_read, sizeof(waywal_ipc_hdr_t) - total_read, 0);
        if (rem < 0) {
            if (errno == EINTR) continue;
            if (received_fd >= 0) close(received_fd);
            return false;
        }
        if (rem == 0) {
            if (received_fd >= 0) close(received_fd);
            return false;
        }
        total_read += (size_t)rem;
    }

    if (out_hdr->magic != WAYWAL_IPC_MAGIC || out_hdr->version != WAYWAL_IPC_VERSION) {
        WAYWAL_LOG_ERR("IPC header magic/version mismatch: magic=0x%08x ver=%u",
                     out_hdr->magic, out_hdr->version);
        if (received_fd >= 0) close(received_fd);
        return false;
    }

    if (out_attached_fd) {
        *out_attached_fd = received_fd;
    } else if (received_fd >= 0) {
        close(received_fd);
    }

    return true;
}
