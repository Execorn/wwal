// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc_client.hpp"

#include "waywal/log.h"
#include "waywal/wwpkg.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaType>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace waywal::studio {

namespace {

bool expectsGenericOkReply(uint16_t reqOpcode) noexcept
{
    return IpcClient::expectsGenericOkForOpcode(reqOpcode);
}

bool isFireAndForgetNoReplyOpcode(uint16_t reqOpcode) noexcept
{
    switch (reqOpcode) {
    case WAYWAL_REQ_SUBSCRIBE:
    case WAYWAL_REQ_SET_UNIFORM:
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
        return true;
    default:
        return false;
    }
}

bool readExactNonBlocking(int fd, void *buf, size_t count, int timeoutMs)
{
    uint8_t *ptr = static_cast<uint8_t *>(buf);
    size_t total = 0;

    while (total < count) {
        ssize_t n = recv(fd, ptr + total, count - total, 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, timeoutMs);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

bool writeExactNonBlocking(int fd, const void *buf, size_t count, int timeoutMs)
{
    const uint8_t *ptr = static_cast<const uint8_t *>(buf);
    size_t total = 0;

    while (total < count) {
        ssize_t n = send(fd, ptr + total, count - total, MSG_NOSIGNAL);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, timeoutMs);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

bool parseUuidString(const char *str, uint8_t outUuid[16])
{
    if (!str || !outUuid) {
        return false;
    }
    uint8_t tmp[16]{};
    size_t nibbles = 0;
    int hi = -1;
    for (const char *p = str; *p != '\0'; ++p) {
        if (*p == '-') {
            continue;
        }
        int v = hexNibble(*p);
        if (v < 0 || nibbles >= 32) {
            return false;
        }
        if ((nibbles & 1u) == 0u) {
            hi = v;
        } else {
            tmp[nibbles / 2] = static_cast<uint8_t>((hi << 4) | v);
        }
        ++nibbles;
    }
    if (nibbles != 32) {
        return false;
    }
    std::memcpy(outUuid, tmp, 16);
    return true;
}

QString uuidBytesToDashedHex(const uint8_t uuid[16])
{
    char buf[37]{};
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
                  uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
                  uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return QString::fromLatin1(buf);
}

bool isAllZeroUuid(const uint8_t uuid[16])
{
    if (!uuid)
        return true;
    uint8_t acc = 0;
    for (size_t i = 0; i < 16; ++i) {
        acc |= uuid[i];
    }
    return acc == 0;
}

int tryOpenHubPackageByUuid(const uint8_t uuid[16])
{
    if (!uuid || isAllZeroUuid(uuid)) {
        return -1;
    }
    const QString targetId = uuidBytesToDashedHex(uuid).toLower();
    QStringList dirs;
    const QString xdgHub = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                           QStringLiteral("/waywal/hub");
    if (!xdgHub.isEmpty()) {
        dirs << xdgHub;
    }
    const QString homeHub = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
    if (!dirs.contains(homeHub)) {
        dirs << homeHub;
    }

    for (const QString &dirStr : dirs) {
        std::error_code ec;
        std::filesystem::path dirPath(dirStr.toStdString());
        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec)) {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".wwpkg") {
                continue;
            }
            const std::string p = entry.path().string();
            wwpkg_package_t *pkg = wwpkg_open(p.c_str());
            if (!pkg) {
                continue;
            }
            const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
            bool matched = false;
            if (man && QString::fromUtf8(man->id).trimmed().toLower() == targetId) {
                matched = true;
            }
            wwpkg_close(pkg);
            if (matched) {
                return ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
            }
        }
    }
    return -1;
}

int dupCloexec(int fd)
{
    if (fd < 0) {
        return -1;
    }
    int duped = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0) {
        duped = ::dup(fd);
        if (duped >= 0) {
            int fl = fcntl(duped, F_GETFD, 0);
            if (fl >= 0) {
                (void)fcntl(duped, F_SETFD, fl | FD_CLOEXEC);
            }
        }
    }
    return duped;
}

struct ScopedFdArrayGuard {
    int *fds;
    size_t count;
    ~ScopedFdArrayGuard()
    {
        if (!fds) {
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            if (fds[i] >= 0) {
                ::close(fds[i]);
                fds[i] = -1;
            }
        }
    }
};

} // namespace

IpcClient::IpcClient(QString namespaceStr, QString socketPath, QObject *parent, bool autoStart)
    : QObject(parent), m_namespaceStr(std::move(namespaceStr)),
      m_customSocketPath(std::move(socketPath)), m_lastUniformFlush{}
{
    qRegisterMetaType<TelemetrySnapshot>("TelemetrySnapshot");
    qRegisterMetaType<waywal_export_dmabuf_resp_t>("waywal_export_dmabuf_resp_t");
    qRegisterMetaType<waywal_ipc_resp_workshop_sync_t>("waywal_ipc_resp_workshop_sync_t");
    qRegisterMetaType<waywal_ipc_resp_workshop_download_t>("waywal_ipc_resp_workshop_download_t");
    qRegisterMetaType<waywal_ipc_resp_wwpkg_install_t>("waywal_ipc_resp_wwpkg_install_t");
    qRegisterMetaType<waywal_ipc_resp_scan_compat_t>("waywal_ipc_resp_scan_compat_t");
    qRegisterMetaType<waywal_ipc_resp_compile_shader_graph_t>(
        "waywal_ipc_resp_compile_shader_graph_t");
    qRegisterMetaType<waywal_ipc_resp_set_camera_3d_t>("waywal_ipc_resp_set_camera_3d_t");
    qRegisterMetaType<waywal_ipc_resp_set_physics_params_t>("waywal_ipc_resp_set_physics_params_t");
    qRegisterMetaType<waywal_ipc_resp_set_scene_lights_t>("waywal_ipc_resp_set_scene_lights_t");
    qRegisterMetaType<waywal_ipc_puppet_mesh_ack_t>("waywal_ipc_puppet_mesh_ack_t");
    qRegisterMetaType<waywal_ipc_puppet_weights_resp_t>("waywal_ipc_puppet_weights_resp_t");
    qRegisterMetaType<std::vector<waywal_ipc_vertex_weight_t>>(
        "std::vector<waywal_ipc_vertex_weight_t>");
    qRegisterMetaType<waywal_ipc_puppet_ik_pose_resp_t>("waywal_ipc_puppet_ik_pose_resp_t");
    qRegisterMetaType<std::vector<waywal_ipc_bone_trs_t>>("std::vector<waywal_ipc_bone_trs_t>");
    qRegisterMetaType<waywal_ipc_export_puppet_mdl_resp_t>("waywal_ipc_export_puppet_mdl_resp_t");

    if (m_namespaceStr.isEmpty()) {
        m_namespaceStr = QStringLiteral("default");
    }

    m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeupFd < 0) {
        WAYWAL_LOG_ERR("IpcClient: eventfd creation failed: %s", strerror(errno));
    }

    if (autoStart) {
        start();
    }
}

IpcClient::~IpcClient()
{
    stop();
    if (m_wakeupFd >= 0) {
        ::close(m_wakeupFd);
        m_wakeupFd = -1;
    }
}

void IpcClient::start()
{
    if (m_running.exchange(true)) {
        return;
    }

    m_workerThread = std::thread(&IpcClient::workerLoop, this);
}

void IpcClient::stop()
{
    if (!m_running.exchange(false)) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto &pkt : m_outQueue) {
            if (pkt.attachedFd >= 0) {
                ::close(pkt.attachedFd);
                pkt.attachedFd = -1;
            }
        }
        m_outQueue.clear();
        m_pendingUniforms.clear();
        return;
    }

    wakeupWorker();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto &pkt : m_outQueue) {
            if (pkt.attachedFd >= 0) {
                ::close(pkt.attachedFd);
                pkt.attachedFd = -1;
            }
        }
        m_outQueue.clear();
        m_pendingUniforms.clear();
    }

    closeSocket();
}

bool IpcClient::isConnected() const { return m_connected.load(); }

QString IpcClient::socketPath() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_customSocketPath.isEmpty()) {
        return m_customSocketPath;
    }
    if (!m_resolvedSocketPath.isEmpty()) {
        return m_resolvedSocketPath;
    }
    path_buf_t pb;
    if (waywal_ipc_resolve_socket_path(&pb, m_namespaceStr.toUtf8().constData())) {
        return QString::fromUtf8(path_buf_cstr(&pb));
    }
    return QString();
}

void IpcClient::wakeupWorker()
{
    if (m_wakeupFd >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] ssize_t nw = ::write(m_wakeupFd, &val, sizeof(val));
    }
}

void IpcClient::dispatchRequestFailure(PendingRequest &&req, const QString &reason,
                                       bool emitErrorSignal)
{
    if (!m_running.load()) {
        return;
    }

    if (req.wwpkgInstallCallback || req.workshopSyncCallback || req.workshopDownloadCallback ||
        req.scanCompatCallback || req.jsonCallback || emitErrorSignal) {
        QMetaObject::invokeMethod(
            this,
            [this, reason, emitErrorSignal, wwpkgCb = std::move(req.wwpkgInstallCallback),
             syncCb = std::move(req.workshopSyncCallback),
             dlCb = std::move(req.workshopDownloadCallback),
             compatCb = std::move(req.scanCompatCallback), jsonCb = std::move(req.jsonCallback)]() {
                if (wwpkgCb) {
                    waywal_ipc_resp_wwpkg_install_t resp{};
                    resp.status_code = 1u;
                    wwpkgCb(resp, reason);
                } else if (syncCb) {
                    waywal_ipc_resp_workshop_sync_t resp{};
                    resp.status_code = 1u;
                    syncCb(resp);
                } else if (dlCb) {
                    waywal_ipc_resp_workshop_download_t resp{};
                    resp.task_state = 7u;
                    QByteArray errBa = reason.toUtf8();
                    std::strncpy(resp.error_msg, errBa.constData(), sizeof(resp.error_msg) - 1);
                    dlCb(resp);
                } else if (compatCb) {
                    waywal_ipc_resp_scan_compat_t resp{};
                    resp.status_code = 1u;
                    compatCb(resp);
                } else if (jsonCb) {
                    jsonCb(QString());
                }
                if (emitErrorSignal) {
                    emit errorOccurred(reason);
                }
            },
            Qt::QueuedConnection);
    }
}

void IpcClient::failInFlightRequests(const QString &reason)
{
    std::deque<PendingRequest> stale;
    {
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        stale.swap(m_pendingRequests);
    }

    if (!m_running.load()) {
        return;
    }

    for (auto &req : stale) {
        dispatchRequestFailure(std::move(req), reason, false);
    }
}

void IpcClient::sweepExpiredRequests()
{
    const int timeoutMs = m_requestTimeoutMs.load();
    if (timeoutMs <= 0) {
        return;
    }

    const auto deadline = std::chrono::milliseconds(timeoutMs);
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingRequest> expired;
    {
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        auto it = m_pendingRequests.begin();
        while (it != m_pendingRequests.end()) {
            if (now - it->sentAt >= deadline) {
                expired.push_back(std::move(*it));
                it = m_pendingRequests.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (expired.empty() || !m_running.load()) {
        return;
    }

    for (auto &req : expired) {
        const QString reason = QStringLiteral("IPC request 0x%1 timed out after %2 ms")
                                   .arg(req.expectedOpcode, 4, 16, QLatin1Char('0'))
                                   .arg(timeoutMs);
        dispatchRequestFailure(std::move(req), reason, true);
    }
}

void IpcClient::closeSocket()
{
    if (m_socketFd >= 0) {
        ::close(m_socketFd);
        m_socketFd = -1;
    }
    failInFlightRequests(QStringLiteral("IPC socket disconnected"));
    if (m_connected.exchange(false) && m_running.load()) {
        QMetaObject::invokeMethod(
            this, [this]() { emit connectionStateChanged(false); }, Qt::QueuedConnection);
    }
}

bool IpcClient::connectSocket()
{
    QString targetPath;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        targetPath = m_customSocketPath;
    }
    if (targetPath.isEmpty()) {
        path_buf_t pb;
        if (waywal_ipc_resolve_socket_path(&pb, m_namespaceStr.toUtf8().constData())) {
            targetPath = QString::fromUtf8(path_buf_cstr(&pb));
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_resolvedSocketPath = targetPath;
        } else {
            return false;
        }
    }

    QByteArray pathBa = targetPath.toUtf8();
    if (pathBa.isEmpty()) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (static_cast<size_t>(pathBa.size()) >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, pathBa.constData(), static_cast<size_t>(pathBa.size()));

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return false;
    }

    int bufSize = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

    int res = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return false;
    }

    if (res < 0 && errno == EINPROGRESS) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = poll(&pfd, 1, 150);
        if (pr <= 0) {
            ::close(fd);
            return false;
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) {
            ::close(fd);
            return false;
        }
    }

    m_socketFd = fd;
    m_connected.store(true);

    QMetaObject::invokeMethod(
        this, [this]() { emit connectionStateChanged(true); }, Qt::QueuedConnection);

    return true;
}

void IpcClient::workerLoop()
{
    int backoffMs = 50;

    while (m_running.load()) {
        if (!m_connected.load()) {
            if (!connectSocket()) {
                struct pollfd pfd{};
                pfd.fd = m_wakeupFd;
                pfd.events = POLLIN;
                poll(&pfd, 1, backoffMs);

                if (backoffMs < 1500) {
                    backoffMs = std::min(backoffMs * 2, 1500);
                }
                continue;
            }

            backoffMs = 50;
            subscribeEvents();
        }

        struct pollfd pfds[2];
        pfds[0].fd = m_wakeupFd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = m_socketFd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pollTimeoutMs = 250;
        {
            std::lock_guard<std::mutex> lock(m_requestsMutex);
            if (!m_pendingRequests.empty()) {
                const int reqTimeout = m_requestTimeoutMs.load();
                if (reqTimeout > 0) {
                    pollTimeoutMs = std::min(pollTimeoutMs, std::max(10, reqTimeout));
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_outQueue.empty()) {
                pollTimeoutMs = 0;
            } else if (!m_pendingUniforms.empty()) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUniformFlush);
                const int uniformWait = (elapsed >= std::chrono::milliseconds(7))
                                            ? 0
                                            : static_cast<int>(7 - elapsed.count());
                pollTimeoutMs = std::min(pollTimeoutMs, uniformWait);
            }
        }
        int pr = poll(pfds, 2, pollTimeoutMs);

        if (pr < 0) {
            if (errno == EINTR)
                continue;
            closeSocket();
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            uint64_t val = 0;
            [[maybe_unused]] ssize_t nr = ::read(m_wakeupFd, &val, sizeof(val));
        }

        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            while (m_socketFd >= 0 && (pfds[1].revents & POLLIN)) {
                char peekByte = 0;
                ssize_t peekRes = ::recv(m_socketFd, &peekByte, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peekRes <= 0) {
                    break;
                }
                handleIncomingPacket();
            }
            closeSocket();
            continue;
        }

        if (pfds[1].revents & POLLIN) {
            handleIncomingPacket();
        }

        flushOutboundQueue();
        sweepExpiredRequests();
    }
}

void IpcClient::flushOutboundQueue()
{
    if (m_socketFd < 0 || !m_connected.load()) {
        return;
    }

    std::deque<OutboundPacket> packets;
    std::vector<waywal_set_uniform_payload_t> uniformBatch;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        packets.swap(m_outQueue);
        if (!m_pendingUniforms.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (m_lastUniformFlush == std::chrono::steady_clock::time_point{} ||
                now - m_lastUniformFlush >= std::chrono::milliseconds(7)) {
                uniformBatch.reserve(m_pendingUniforms.size());
                for (auto &[key, payload] : m_pendingUniforms) {
                    uniformBatch.push_back(payload);
                }
                m_pendingUniforms.clear();
                m_lastUniformFlush = now;
            }
        }
    }

    auto closeRemainingFds = [](std::deque<OutboundPacket> &q) {
        for (auto &rem : q) {
            if (rem.attachedFd >= 0) {
                ::close(rem.attachedFd);
                rem.attachedFd = -1;
            }
        }
    };

    while (!packets.empty()) {
        OutboundPacket pkt = std::move(packets.front());
        packets.pop_front();

        bool sentHdr = false;
        if (pkt.attachedFd >= 0) {
            sentHdr = waywal_ipc_send(m_socketFd, &pkt.hdr, pkt.attachedFd);
            ::close(pkt.attachedFd);
            pkt.attachedFd = -1;
        } else {
            sentHdr = writeExactNonBlocking(m_socketFd, &pkt.hdr, sizeof(pkt.hdr), 200);
        }

        if (!sentHdr) {
            closeRemainingFds(packets);
            closeSocket();
            return;
        }

        if (pkt.hdr.payload_size > 0 && !pkt.payload.empty()) {
            if (!writeExactNonBlocking(m_socketFd, pkt.payload.data(), pkt.payload.size(), 500)) {
                closeRemainingFds(packets);
                closeSocket();
                return;
            }
        }

        /* Track every sent frame in FIFO order so generic WAYWAL_RESP_OK / WAYWAL_RESP_ERR replies
         * align 1-to-1 with outbound requests and never pop a subsequent RPC's callback. */
        PendingRequest req{};
        req.expectedOpcode = pkt.hdr.opcode;
        req.jsonCallback = std::move(pkt.jsonCallback);
        req.telemetryCallback = std::move(pkt.telemetryCallback);
        req.dmabufCallback = std::move(pkt.dmabufCallback);
        req.workshopSyncCallback = std::move(pkt.workshopSyncCallback);
        req.workshopDownloadCallback = std::move(pkt.workshopDownloadCallback);
        req.wwpkgInstallCallback = std::move(pkt.wwpkgInstallCallback);
        req.scanCompatCallback = std::move(pkt.scanCompatCallback);
        req.sentAt = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_requestsMutex);
        m_pendingRequests.push_back(std::move(req));
    }

    for (const auto &payload : uniformBatch) {
        waywal_ipc_hdr_t hdr{};
        hdr.magic = WAYWAL_IPC_MAGIC;
        hdr.version = WAYWAL_IPC_VERSION;
        hdr.opcode = WAYWAL_REQ_SET_UNIFORM;
        hdr.payload_size = sizeof(waywal_set_uniform_payload_t);

        if (!writeExactNonBlocking(m_socketFd, &hdr, sizeof(hdr), 100)) {
            closeSocket();
            return;
        }
        if (!writeExactNonBlocking(m_socketFd, &payload, sizeof(payload), 100)) {
            closeSocket();
            return;
        }

        PendingRequest req{};
        req.expectedOpcode = WAYWAL_REQ_SET_UNIFORM;
        req.sentAt = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        m_pendingRequests.push_back(std::move(req));
    }
}

bool IpcClient::popPendingForOpcode(uint16_t reqOpcode, PendingRequest &outReq)
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    for (size_t idx = 0; idx < m_pendingRequests.size(); ++idx) {
        if (m_pendingRequests[idx].expectedOpcode == reqOpcode) {
            outReq = std::move(m_pendingRequests[idx]);
            m_pendingRequests.erase(m_pendingRequests.begin() + static_cast<std::ptrdiff_t>(idx));
            /* Drop any earlier callback-less fire-and-forget entries that a mock peer chose not to
             * acknowledge explicitly before replying to this typed RPC. */
            size_t scan = 0;
            while (scan < idx && scan < m_pendingRequests.size()) {
                const auto &entry = m_pendingRequests[scan];
                const bool hasCb = static_cast<bool>(entry.jsonCallback) ||
                                   static_cast<bool>(entry.telemetryCallback) ||
                                   static_cast<bool>(entry.dmabufCallback) ||
                                   static_cast<bool>(entry.workshopSyncCallback) ||
                                   static_cast<bool>(entry.workshopDownloadCallback) ||
                                   static_cast<bool>(entry.wwpkgInstallCallback) ||
                                   static_cast<bool>(entry.scanCompatCallback);
                if (expectsGenericOkReply(entry.expectedOpcode) && !hasCb) {
                    m_pendingRequests.erase(m_pendingRequests.begin() +
                                            static_cast<std::ptrdiff_t>(scan));
                    --idx;
                } else {
                    ++scan;
                }
            }
            return true;
        }
    }
    return false;
}

void IpcClient::handleIncomingPacket()
{
    waywal_ipc_hdr_t hdr{};
    int fds[4] = {-1, -1, -1, -1};
    size_t numFds = 0;

    if (!waywal_ipc_recv_fds(m_socketFd, &hdr, fds, 4, &numFds)) {
        closeSocket();
        return;
    }

    /* Guarantee received SCM_RIGHTS FDs are closed across all return paths unless explicitly
     * transferred to a validated WAYWAL_RESP_DMABUF consumer. */
    ScopedFdArrayGuard fdGuard{fds, numFds};

    std::vector<uint8_t> payload;
    if (hdr.payload_size > 0) {
        const size_t maxAllowed =
            sizeof(waywal_ipc_resp_readback_texture_t) + WAYWAL_IPC_MAX_READBACK_BYTES;
        if (hdr.payload_size > maxAllowed) {
            WAYWAL_LOG_ERR("IpcClient: Payload size exceeds limit: %lu",
                           static_cast<unsigned long>(hdr.payload_size));
            closeSocket();
            return;
        }

        payload.resize(hdr.payload_size);
        if (!readExactNonBlocking(m_socketFd, payload.data(), hdr.payload_size, 500)) {
            closeSocket();
            return;
        }
    }

    /* Unsolicited daemon broadcasts must never touch m_pendingRequests. */
    if (hdr.opcode == WAYWAL_RESP_EVENT) {
        QString jsonStr = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                            static_cast<qsizetype>(payload.size()));

        QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QString eventType =
                obj.value(QStringLiteral("event")).toString(QStringLiteral("unknown"));
            QMetaObject::invokeMethod(
                this, [this, eventType, obj]() { emit eventReceived(eventType, obj); },
                Qt::QueuedConnection);
        }
        return;
    }

    /* Route WAYWAL_RESP_ERR before any binary struct deserialization so text error payloads are
     * never misinterpreted as fixed binary response structs. */
    if (hdr.opcode == WAYWAL_RESP_ERR) {
        QString err = QStringLiteral("Daemon returned error");
        if (!payload.empty()) {
            size_t len = strnlen(reinterpret_cast<const char *>(payload.data()), payload.size());
            err = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                    static_cast<qsizetype>(len));
        }

        PendingRequest failedReq{};
        bool hadFailedReq = false;
        {
            std::lock_guard<std::mutex> lock(m_requestsMutex);
            // Skip earlier callback-less fire-and-forget writes that a peer may not reply to
            // individually so an error packet lands on the subsequent RPC in flight.
            while (m_pendingRequests.size() > 1) {
                const auto &front = m_pendingRequests.front();
                const bool hasCb = static_cast<bool>(front.jsonCallback) ||
                                   static_cast<bool>(front.telemetryCallback) ||
                                   static_cast<bool>(front.dmabufCallback) ||
                                   static_cast<bool>(front.workshopSyncCallback) ||
                                   static_cast<bool>(front.workshopDownloadCallback) ||
                                   static_cast<bool>(front.wwpkgInstallCallback) ||
                                   static_cast<bool>(front.scanCompatCallback);
                if (!hasCb && isFireAndForgetNoReplyOpcode(front.expectedOpcode)) {
                    m_pendingRequests.pop_front();
                } else {
                    break;
                }
            }
            if (!m_pendingRequests.empty()) {
                failedReq = std::move(m_pendingRequests.front());
                m_pendingRequests.pop_front();
                hadFailedReq = true;
            }
        }

        if (hadFailedReq) {
            dispatchRequestFailure(std::move(failedReq), err, true);
        } else {
            QMetaObject::invokeMethod(
                this, [this, err]() { emit errorOccurred(err); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_DMABUF) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_EXPORT_OUTPUT_DMABUF, req);
        if (payload.size() == sizeof(waywal_export_dmabuf_resp_t)) {
            waywal_export_dmabuf_resp_t desc{};
            std::memcpy(&desc, payload.data(), sizeof(desc));
            if (desc.width > 0 && desc.height > 0 && desc.num_planes <= 4) {
                struct QueuedFdGuard {
                    QVector<int> fds;
                    bool handedOff{false};
                    ~QueuedFdGuard()
                    {
                        if (!handedOff) {
                            for (int fd : fds) {
                                if (fd >= 0) {
                                    ::close(fd);
                                }
                            }
                        }
                    }
                };

                auto fdGuard = std::make_shared<QueuedFdGuard>();
                size_t planeLimit = std::min<size_t>(numFds, desc.num_planes);
                for (size_t i = 0; i < planeLimit; ++i) {
                    if (fds[i] >= 0) {
                        fdGuard->fds.append(fds[i]);
                        fds[i] = -1;
                    }
                }

                // Close queued FDs automatically if IpcClient is destroyed before
                // the main-thread lambda runs or if no consumer takes ownership.
                QMetaObject::invokeMethod(
                    this,
                    [this, desc, fdGuard, cb = std::move(req.dmabufCallback)]() {
                        const bool hasSignalListeners =
                            receivers(SIGNAL(
                                dmabufReceived(waywal_export_dmabuf_resp_t, QVector<int>))) > 0;
                        if (!cb && !hasSignalListeners) {
                            return;
                        }
                        fdGuard->handedOff = true;
                        if (cb) {
                            cb(desc, fdGuard->fds);
                        }
                        if (hasSignalListeners) {
                            emit dmabufReceived(desc, fdGuard->fds);
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_TELEMETRY) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_GET_TELEMETRY, req);
        if (payload.size() == sizeof(waywal_telemetry_snapshot_t)) {
            waywal_telemetry_snapshot_t raw{};
            std::memcpy(&raw, payload.data(), sizeof(raw));

            TelemetrySnapshot snap{};
            snap.timestampNs = raw.timestamp_ns;
            snap.rssKiB = raw.rss_kib;
            snap.pssKiB = raw.pss_kib;
            snap.totalVramKiB = raw.total_vram_kib;
            snap.gpuActive = (raw.gpu_active != 0);
            snap.onBattery = (raw.on_battery != 0);
            snap.batteryPolicy = raw.battery_policy;
            snap.globalPauseMask = raw.global_pause_mask;
            snap.barkBands.assign(raw.bark_bands, raw.bark_bands + 64);
            snap.rmsLevel = raw.rms_level;
            snap.beatPulse = raw.beat_pulse;
            snap.beatOnset = (raw.beat_onset != 0);
            snap.audioActive = (raw.audio_active != 0);
            snap.pipewireActive = (raw.pipewire_active != 0);
            snap.muteOnAudio = (raw.mute_on_audio != 0);
            snap.audioVolume = raw.audio_volume;

            uint32_t outCount =
                std::min(raw.num_outputs, static_cast<uint32_t>(WAYWAL_MAX_TELEMETRY_OUTPUTS));
            for (uint32_t i = 0; i < outCount; ++i) {
                const auto &ro = raw.outputs[i];
                OutputTelemetry ot{};
                size_t nameLen = strnlen(ro.name, sizeof(ro.name));
                ot.name = QString::fromUtf8(ro.name, static_cast<qsizetype>(nameLen));
                ot.scanoutFps = ro.scanout_fps;
                ot.lastRenderUs = ro.last_render_us;
                ot.vramKiB = ro.vram_kib;
                std::memcpy(ot.boStates, ro.bo_states, sizeof(ot.boStates));
                ot.engineKind = ro.engine_kind;
                ot.isPaused = (ro.is_paused != 0);
                ot.pauseMask = ro.pause_mask;
                ot.width = ro.width;
                ot.height = ro.height;
                ot.logicalWidth = ro.logical_width;
                ot.logicalHeight = ro.logical_height;
                ot.scale = (ro.scale_120 > 0) ? (static_cast<double>(ro.scale_120) / 120.0) : 1.0;
                snap.outputs.push_back(ot);
            }

            QMetaObject::invokeMethod(
                this,
                [this, snap, cb = std::move(req.telemetryCallback)]() {
                    if (cb) {
                        cb(snap);
                    }
                    emit telemetryReceived(snap);
                },
                Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_SYNC) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_WORKSHOP_SYNC, req);
        if (payload.size() == sizeof(waywal_ipc_resp_workshop_sync_t)) {
            waywal_ipc_resp_workshop_sync_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_workshop_sync(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.workshopSyncCallback)]() {
                        if (cb) {
                            cb(resp);
                        }
                        emit workshopSyncReceived(resp);
                    },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        emit errorOccurred(QStringLiteral("Invalid WAYWAL_RESP_WORKSHOP_SYNC"));
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_DOWNLOAD) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_WORKSHOP_DOWNLOAD, req);
        if (payload.size() == sizeof(waywal_ipc_resp_workshop_download_t)) {
            waywal_ipc_resp_workshop_download_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_workshop_download(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.workshopDownloadCallback)]() {
                        if (cb) {
                            cb(resp);
                        }
                        emit workshopDownloadReceived(resp);
                    },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        emit errorOccurred(QStringLiteral("Invalid WAYWAL_RESP_WORKSHOP_DOWNLOAD"));
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WWPKG_INSTALL) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_WWPKG_INSTALL, req);
        const char *tailErr = nullptr;
        if (payload.size() >= sizeof(waywal_ipc_resp_wwpkg_install_t) &&
            waywal_ipc_validate_resp_wwpkg_install(
                reinterpret_cast<const waywal_ipc_resp_wwpkg_install_t *>(payload.data()),
                payload.size(), &tailErr)) {
            waywal_ipc_resp_wwpkg_install_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            QString errMsg = tailErr ? QString::fromUtf8(tailErr) : QString();
            QMetaObject::invokeMethod(
                this,
                [this, resp, errMsg, cb = std::move(req.wwpkgInstallCallback)]() {
                    if (cb) {
                        cb(resp, errMsg);
                    }
                    emit wwpkgInstallReceived(resp, errMsg);
                },
                Qt::QueuedConnection);
        } else {
            QString malErr = QStringLiteral("Malformed WAYWAL_RESP_WWPKG_INSTALL payload");
            QMetaObject::invokeMethod(
                this,
                [this, malErr, cb = std::move(req.wwpkgInstallCallback)]() {
                    if (cb) {
                        waywal_ipc_resp_wwpkg_install_t resp{};
                        resp.status_code = 1u;
                        cb(resp, malErr);
                    }
                    emit errorOccurred(malErr);
                },
                Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SCAN_COMPAT) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SCAN_COMPAT, req);
        if (payload.size() == sizeof(waywal_ipc_resp_scan_compat_t)) {
            waywal_ipc_resp_scan_compat_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_scan_compat(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.scanCompatCallback)]() {
                        if (cb) {
                            cb(resp);
                        }
                        emit scanCompatReceived(resp);
                    },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        emit errorOccurred(QStringLiteral("Invalid WAYWAL_RESP_SCAN_COMPAT"));
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_COMPILE_SHADER_GRAPH) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_COMPILE_SHADER_GRAPH, req);
        if (payload.size() == sizeof(waywal_ipc_resp_compile_shader_graph_t)) {
            waywal_ipc_resp_compile_shader_graph_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_compile_shader_graph(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit shaderGraphCompiled(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_CAMERA_3D) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SET_CAMERA_3D, req);
        if (payload.size() == sizeof(waywal_ipc_resp_set_camera_3d_t)) {
            waywal_ipc_resp_set_camera_3d_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_set_camera_3d(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit camera3DUpdated(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_PHYSICS_PARAMS) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SET_PHYSICS_PARAMS, req);
        if (payload.size() == sizeof(waywal_ipc_resp_set_physics_params_t)) {
            waywal_ipc_resp_set_physics_params_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_set_physics_params(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit physicsParamsUpdated(resp); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_SCENE_LIGHTS) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SET_SCENE_LIGHTS, req);
        if (payload.size() == sizeof(waywal_ipc_resp_set_scene_lights_t)) {
            waywal_ipc_resp_set_scene_lights_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_set_scene_lights(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneLightsUpdated(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SCENE_MUTATE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SCENE_MUTATE, req);
        if (payload.size() == sizeof(waywal_ipc_resp_scene_mutate_t)) {
            waywal_ipc_resp_scene_mutate_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_scene_mutate(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneMutated(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_HOT_RELOAD_CODE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_HOT_RELOAD_CODE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_hot_reload_code_t)) {
            waywal_ipc_resp_hot_reload_code_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const char *metaPtr = nullptr;
            if (waywal_ipc_validate_resp_hot_reload_code(
                    reinterpret_cast<const waywal_ipc_resp_hot_reload_code_t *>(payload.data()),
                    payload.size(), &metaPtr)) {
                QString metaJson =
                    (metaPtr && resp.meta_json_len > 0)
                        ? QString::fromUtf8(metaPtr, static_cast<qsizetype>(resp.meta_json_len))
                        : QString();
                QMetaObject::invokeMethod(
                    this, [this, resp, metaJson]() { emit codeHotReloaded(resp, metaJson); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_QUERY_SCENE_TREE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_QUERY_SCENE_TREE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_query_scene_tree_t)) {
            waywal_ipc_resp_query_scene_tree_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const char *jsonPtr = nullptr;
            if (waywal_ipc_validate_resp_query_scene_tree(
                    reinterpret_cast<const waywal_ipc_resp_query_scene_tree_t *>(payload.data()),
                    payload.size(), &jsonPtr)) {
                QString treeJson =
                    (jsonPtr && resp.json_len > 0)
                        ? QString::fromUtf8(jsonPtr, static_cast<qsizetype>(resp.json_len))
                        : QString();
                QMetaObject::invokeMethod(
                    this, [this, resp, treeJson]() { emit sceneTreeQueried(resp, treeJson); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_SHADER_COMBO) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SET_SHADER_COMBO, req);
        if (payload.size() == sizeof(waywal_ipc_resp_set_shader_combo_t)) {
            waywal_ipc_resp_set_shader_combo_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_set_shader_combo(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit shaderComboUpdated(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_BRUSH_STROKE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_BRUSH_STROKE, req);
        if (payload.size() == sizeof(waywal_ipc_resp_brush_stroke_t)) {
            waywal_ipc_resp_brush_stroke_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_brush_stroke(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit brushStrokeCompleted(resp); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PARTICLE_CONFIG) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_PARTICLE_CONFIG, req);
        if (payload.size() == sizeof(waywal_ipc_resp_particle_config_t)) {
            waywal_ipc_resp_particle_config_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_particle_config(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit particleConfigUpdated(resp); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_AUDIO_BINDINGS) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_SET_AUDIO_BINDINGS, req);
        if (payload.size() == sizeof(waywal_ipc_resp_set_audio_bindings_t)) {
            waywal_ipc_resp_set_audio_bindings_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_set_audio_bindings(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit audioBindingsUpdated(resp); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_EXPORT_SCENE_PKG) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_EXPORT_SCENE_PKG, req);
        if (payload.size() == sizeof(waywal_ipc_resp_export_scene_pkg_t)) {
            waywal_ipc_resp_export_scene_pkg_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_export_scene_pkg(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit scenePkgExported(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_LINT_SCENESCRIPT) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_LINT_SCENESCRIPT, req);
        if (payload.size() == sizeof(waywal_ipc_resp_lint_scenescript_t)) {
            waywal_ipc_resp_lint_scenescript_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_lint_scenescript(&resp, payload.size())) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneScriptLinted(resp); }, Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_READBACK_TEXTURE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_READBACK_TEXTURE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_readback_texture_t)) {
            waywal_ipc_resp_readback_texture_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const void *dataPtr = nullptr;
            uint32_t dataLen = 0;
            if (waywal_ipc_validate_resp_readback_texture(
                    reinterpret_cast<const waywal_ipc_resp_readback_texture_t *>(payload.data()),
                    payload.size(), &dataPtr, &dataLen)) {
                QByteArray bytes;
                if (dataPtr && dataLen > 0) {
                    bytes = QByteArray(static_cast<const char *>(dataPtr),
                                       static_cast<qsizetype>(dataLen));
                }
                QMetaObject::invokeMethod(
                    this, [this, resp, bytes]() { emit textureReadbackReceived(resp, bytes); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_MESH_ACK) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_PUPPET_SET_MESH, req);
        if (payload.size() == sizeof(waywal_ipc_puppet_mesh_ack_t)) {
            waywal_ipc_puppet_mesh_ack_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            QMetaObject::invokeMethod(
                this, [this, resp]() { emit puppetMeshAckReceived(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS, req);
        if (payload.size() >= sizeof(waywal_ipc_puppet_weights_resp_t)) {
            waywal_ipc_puppet_weights_resp_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const size_t tailBytes = payload.size() - sizeof(waywal_ipc_puppet_weights_resp_t);
            const uint64_t expectedTail =
                static_cast<uint64_t>(resp.entry_count) * sizeof(waywal_ipc_vertex_weight_t);
            if (resp.entry_count <= WAYWAL_IPC_RIGGING_MAX_VERTICES &&
                expectedTail == static_cast<uint64_t>(tailBytes)) {
                std::vector<waywal_ipc_vertex_weight_t> entries(resp.entry_count);
                if (resp.entry_count > 0) {
                    std::memcpy(entries.data(),
                                payload.data() + sizeof(waywal_ipc_puppet_weights_resp_t),
                                static_cast<size_t>(expectedTail));
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, entries = std::move(entries)]() {
                        emit puppetWeightsSolved(resp, entries);
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_PUPPET_PREVIEW_IK, req);
        if (payload.size() >= sizeof(waywal_ipc_puppet_ik_pose_resp_t)) {
            waywal_ipc_puppet_ik_pose_resp_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const size_t tailBytes = payload.size() - sizeof(waywal_ipc_puppet_ik_pose_resp_t);
            const uint64_t expectedTail =
                static_cast<uint64_t>(resp.bone_count) * sizeof(waywal_ipc_bone_trs_t);
            if (resp.bone_count <= WAYWAL_IPC_RIGGING_MAX_BONES &&
                expectedTail == static_cast<uint64_t>(tailBytes)) {
                std::vector<waywal_ipc_bone_trs_t> bones(resp.bone_count);
                if (resp.bone_count > 0) {
                    std::memcpy(bones.data(),
                                payload.data() + sizeof(waywal_ipc_puppet_ik_pose_resp_t),
                                static_cast<size_t>(expectedTail));
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, bones = std::move(bones)]() {
                        emit puppetIkPoseReceived(resp, bones);
                    },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_EXPORT_PUPPET_MDL) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_EXPORT_PUPPET_MDL, req);
        if (payload.size() >= sizeof(waywal_ipc_export_puppet_mdl_resp_t)) {
            waywal_ipc_export_puppet_mdl_resp_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            const size_t tailBytes = payload.size() - sizeof(waywal_ipc_export_puppet_mdl_resp_t);
            const bool inlineMatches = resp.mdl_byte_size <= WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES &&
                                       static_cast<size_t>(resp.mdl_byte_size) == tailBytes;
            const bool diskOnlyOrError =
                (tailBytes == 0u) && (resp.status != 0 || resp.mdl_byte_size > 0u);
            if (inlineMatches || diskOnlyOrError) {
                QByteArray mdlBytes;
                if (tailBytes > 0u && static_cast<size_t>(resp.mdl_byte_size) == tailBytes) {
                    mdlBytes = QByteArray(
                        reinterpret_cast<const char *>(payload.data() +
                                                       sizeof(waywal_ipc_export_puppet_mdl_resp_t)),
                        static_cast<qsizetype>(tailBytes));
                }
                QMetaObject::invokeMethod(
                    this, [this, resp, mdlBytes]() { emit puppetMdlExported(resp, mdlBytes); },
                    Qt::QueuedConnection);
            }
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_INFO) {
        PendingRequest req{};
        (void)popPendingForOpcode(WAYWAL_REQ_QUERY_JSON, req);
        QString jsonStr = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                            static_cast<qsizetype>(payload.size()));
        QMetaObject::invokeMethod(
            this,
            [this, jsonStr, cb = std::move(req.jsonCallback)]() {
                if (cb) {
                    cb(jsonStr);
                }
                emit outputsJsonReceived(jsonStr);
            },
            Qt::QueuedConnection);
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_OK) {
        PendingRequest req{};
        bool hasReq = false;
        {
            std::lock_guard<std::mutex> lock(m_requestsMutex);
            for (size_t idx = 0; idx < m_pendingRequests.size(); ++idx) {
                if (expectsGenericOkReply(m_pendingRequests[idx].expectedOpcode)) {
                    req = std::move(m_pendingRequests[idx]);
                    m_pendingRequests.erase(m_pendingRequests.begin() +
                                            static_cast<std::ptrdiff_t>(idx));
                    hasReq = true;
                    break;
                }
            }
        }
        if (hasReq && req.expectedOpcode == WAYWAL_REQ_QUERY_JSON) {
            QString jsonStr = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                                static_cast<qsizetype>(payload.size()));
            QMetaObject::invokeMethod(
                this,
                [this, jsonStr, cb = std::move(req.jsonCallback)]() {
                    if (cb) {
                        cb(jsonStr);
                    }
                    emit outputsJsonReceived(jsonStr);
                },
                Qt::QueuedConnection);
        }
        return;
    }
}

void IpcClient::sendSetUniform(const QString &output, const QString &name, const float *vals,
                               uint32_t count, bool persist)
{
    if (!vals || count == 0)
        return;

    waywal_set_uniform_payload_t payload{};
    std::memset(&payload, 0, sizeof(payload));

    QByteArray outBa = output.toUtf8();
    QByteArray nameBa = name.toUtf8();

    std::strncpy(payload.output, outBa.constData(), sizeof(payload.output) - 1);
    std::strncpy(payload.name, nameBa.constData(), sizeof(payload.name) - 1);
    payload.num_components = std::min(count, 4U);
    for (uint32_t i = 0; i < payload.num_components; ++i) {
        payload.values[i] = vals[i];
    }

    std::strncpy(payload.target_output, outBa.constData(), sizeof(payload.target_output) - 1);
    std::strncpy(payload.uniform_name, nameBa.constData(), sizeof(payload.uniform_name) - 1);
    payload.uniform_components = payload.num_components;
    for (uint32_t i = 0; i < payload.num_components; ++i) {
        payload.uniform_values[i] = vals[i];
    }
    if (persist) {
        payload.flags |= WAYWAL_UNIFORM_FLAG_PERSIST;
    }

    std::string key = output.toStdString() + ":" + name.toStdString();

    bool shouldWake = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        shouldWake = m_pendingUniforms.empty();
        m_pendingUniforms[key] = payload;
    }

    if (shouldWake) {
        wakeupWorker();
    }
}

void IpcClient::sendSetImage(const waywal_img_metadata_t &meta, int memfd)
{
    if (meta.extra_data_len != 0) {
        emit errorOccurred(
            QStringLiteral("sendSetImage rejected inconsistent extra_data_len without payload"));
        return;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SET_IMAGE;
    pkt.hdr.payload_size = sizeof(waywal_img_metadata_t);

    pkt.payload.resize(pkt.hdr.payload_size);
    std::memcpy(pkt.payload.data(), &meta, sizeof(waywal_img_metadata_t));

    if (memfd >= 0) {
        pkt.attachedFd = dupCloexec(memfd);
        if (pkt.attachedFd < 0) {
            emit errorOccurred(QStringLiteral("Failed to duplicate image memfd"));
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::sendAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                                     uint8_t batteryPolicy)
{
    (void)batteryPolicy;
    waywal_audio_ctrl_payload_t apay{};
    apay.volume = volume;
    apay.mute_on_audio = muteOnAudio;
    apay.theme_from_media = themeFromMedia;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::AudioPower;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_AUDIO_CTRL;
    pkt.hdr.payload_size = sizeof(apay);

    pkt.payload.resize(sizeof(apay));
    std::memcpy(pkt.payload.data(), &apay, sizeof(apay));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestOutputsJson(std::function<void(const QString &json)> callback)
{
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::QueryJson;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_QUERY_JSON;
    pkt.hdr.payload_size = 0;
    pkt.jsonCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestTelemetry(std::function<void(const TelemetrySnapshot &snapshot)> callback)
{
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Telemetry;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_GET_TELEMETRY;
    pkt.hdr.payload_size = 0;
    pkt.telemetryCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestOutputDmabuf(
    const QString &output,
    std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)> callback)
{
    waywal_export_dmabuf_req_t req{};
    QByteArray outBa = output.toUtf8();
    std::strncpy(req.output, outBa.constData(), sizeof(req.output) - 1);

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::ExportDmabuf;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_EXPORT_OUTPUT_DMABUF;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.dmabufCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::subscribeEvents()
{
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Subscribe;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SUBSCRIBE;
    pkt.hdr.payload_size = 0;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestWorkshopSync(
    uint32_t flags, const QString &customRoot,
    std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> callback)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_workshop_sync_t req{};
    QByteArray rootBa = customRoot.toUtf8();
    if (!waywal_ipc_serialize_req_workshop_sync(
            flags, rootBa.isEmpty() ? nullptr : rootBa.constData(), &hdr, &req)) {
        if (callback) {
            waywal_ipc_resp_workshop_sync_t resp{};
            resp.status_code = 1u;
            QMetaObject::invokeMethod(
                this, [cb = std::move(callback), resp]() { cb(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WorkshopSync;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.workshopSyncCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestWorkshopDownload(
    uint64_t publishedFileId, uint32_t priority, uint32_t preferredBackend,
    std::function<void(const waywal_ipc_resp_workshop_download_t &resp)> callback)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_workshop_download_t req{};
    if (!waywal_ipc_serialize_req_workshop_download(publishedFileId, priority, preferredBackend,
                                                    &hdr, &req)) {
        if (callback) {
            waywal_ipc_resp_workshop_download_t resp{};
            resp.published_file_id = publishedFileId;
            resp.task_state = 7u;
            std::strncpy(resp.error_msg, "Invalid workshop download request",
                         sizeof(resp.error_msg) - 1);
            QMetaObject::invokeMethod(
                this, [cb = std::move(callback), resp]() { cb(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WorkshopDownload;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.workshopDownloadCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestWwpkgInstall(
    const uint8_t packageUuid[16], uint32_t flags, const QString &targetOutput,
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
        callback,
    int wwpkgFd)
{
    uint8_t zeroUuid[16]{};
    const uint8_t *uuidPtr = packageUuid ? packageUuid : zeroUuid;
    QByteArray outBa = targetOutput.toUtf8();

    const size_t bufCap =
        sizeof(waywal_ipc_req_wwpkg_install_t) + static_cast<size_t>(outBa.size()) + 2u;
    std::vector<uint8_t> payloadBuf(bufCap, 0);
    size_t totalLen = 0;
    waywal_ipc_hdr_t hdr{};

    if (!waywal_ipc_serialize_req_wwpkg_install(
            uuidPtr, flags, outBa.isEmpty() ? nullptr : outBa.constData(), &hdr, nullptr,
            payloadBuf.data(), payloadBuf.size(), &totalLen)) {
        QString errMsg = QStringLiteral("Target output exceeds maximum IPC string length");
        QMetaObject::invokeMethod(
            this,
            [this, uuidVec = std::vector<uint8_t>(uuidPtr, uuidPtr + 16), errMsg,
             cb = std::move(callback)]() {
                waywal_ipc_resp_wwpkg_install_t resp{};
                std::memcpy(resp.package_uuid, uuidVec.data(), 16);
                resp.status_code = 1u;
                if (cb) {
                    cb(resp, errMsg);
                }
                emit wwpkgInstallReceived(resp, errMsg);
                emit errorOccurred(errMsg);
            },
            Qt::QueuedConnection);
        return;
    }

    int attached = -1;
    if (wwpkgFd >= 0) {
        attached = dupCloexec(wwpkgFd);
    } else {
        attached = tryOpenHubPackageByUuid(uuidPtr);
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WwpkgInstall;
    pkt.hdr = hdr;
    pkt.payload.assign(payloadBuf.begin(), payloadBuf.begin() + static_cast<ptrdiff_t>(totalLen));
    pkt.attachedFd = attached;
    pkt.wwpkgInstallCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::installWwpkg(
    const QString &packagePath, const QString &targetOutput, uint32_t flags,
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
        callback)
{
    QByteArray pathBa = packagePath.toUtf8();
    int fd = pathBa.isEmpty() ? -1 : ::open(pathBa.constData(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            ::close(fd);
        }
        QString errMsg = QStringLiteral("Unable to open .wwpkg file: %1").arg(packagePath);
        QMetaObject::invokeMethod(
            this,
            [this, errMsg, cb = std::move(callback)]() {
                waywal_ipc_resp_wwpkg_install_t resp{};
                resp.status_code = 2u;
                if (cb) {
                    cb(resp, errMsg);
                }
                emit wwpkgInstallReceived(resp, errMsg);
                emit errorOccurred(errMsg);
            },
            Qt::QueuedConnection);
        return;
    }

    uint8_t pkgUuid[16]{};
    wwpkg_package_t *pkg = wwpkg_open(pathBa.constData());
    if (pkg) {
        const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
        if (man) {
            (void)parseUuidString(man->id, pkgUuid);
        }
        wwpkg_close(pkg);
    }

    QString normOutput = targetOutput.trimmed();
    if (normOutput.isEmpty() ||
        normOutput.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0) {
        normOutput = QStringLiteral("*");
    }

    requestWwpkgInstall(pkgUuid, flags, normOutput, std::move(callback), fd);
    ::close(fd);
}

void IpcClient::requestScanCompat(
    const QString &targetPath, uint32_t autoRepairFlags,
    std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_scan_compat_t req{};
    QByteArray pathBa = targetPath.toUtf8();
    if (!waywal_ipc_serialize_req_scan_compat(pathBa.constData(), autoRepairFlags, &hdr, &req)) {
        if (callback) {
            waywal_ipc_resp_scan_compat_t resp{};
            resp.status_code = 1u;
            QMetaObject::invokeMethod(
                this, [cb = std::move(callback), resp]() { cb(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::ScanCompat;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.scanCompatCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

bool IpcClient::requestCompileShaderGraph(const QString &output, const QString &path,
                                          const QString &inlineJson, uint32_t flags,
                                          waywal_ipc_resp_compile_shader_graph_t *outResp)
{
    QByteArray outBa = output.toUtf8();
    QByteArray pathBa = path.toUtf8();
    QByteArray jsonBa = inlineJson.toUtf8();

    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_compile_shader_graph_t req{};

    std::vector<uint8_t> sendBuf(sizeof(hdr) + sizeof(req) + static_cast<size_t>(jsonBa.size()));
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_compile_shader_graph(
            outBa.isEmpty() ? "*" : outBa.constData(), pathBa.constData(),
            jsonBa.isEmpty() ? nullptr : jsonBa.constData(), flags, &hdr, &req, sendBuf.data(),
            sendBuf.size(), &totalLen)) {
        return false;
    }

    // Never perform synchronous socket I/O on the UI thread; responses arrive
    // asynchronously on the worker thread and emit shaderGraphCompiled(resp).
    if (outResp) {
        std::memset(outResp, 0, sizeof(*outResp));
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr = hdr;
    pkt.payload.resize(totalLen - sizeof(hdr));
    std::memcpy(pkt.payload.data(), sendBuf.data() + sizeof(hdr), pkt.payload.size());

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
    return true;
}

bool IpcClient::sendSetCamera3D(const QString &output, const we_camera3d_t &cam)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_camera_3d_t req{};
    QByteArray outBa = output.toUtf8();
    uint32_t flags = 0;
    if (cam.dof_enabled > 0.5f) {
        flags |= WAYWAL_CAMERA_FLAG_ENABLE_DOF;
    }
    if (cam.path_enabled != 0) {
        flags |= WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES;
    }

    if (!waywal_ipc_serialize_req_set_camera_3d(
            outBa.isEmpty() ? "*" : outBa.constData(), cam.projection_mode, flags, cam.eye_pos,
            cam.target_pos, cam.up_vec, cam.fov_y_deg, cam.near_z, cam.far_z, cam.parallax_amount,
            cam.parallax_smoothing, cam.dof_focal_dist, cam.dof_focal_region,
            cam.dof_bokeh_radius_px, &hdr, &req)) {
        return false;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
    return true;
}

bool IpcClient::sendSetPhysicsParams(const QString &output, int32_t objectId, int32_t boneIndex,
                                     const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                                     const we_cursor_force_t &cursor)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_physics_params_t req{};
    QByteArray outBa = output.toUtf8();

    if (!waywal_ipc_serialize_req_set_physics_params(outBa.isEmpty() ? "*" : outBa.constData(),
                                                     objectId, boneIndex, &cfg, &wind, &cursor,
                                                     &hdr, &req)) {
        return false;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
    return true;
}

bool IpcClient::sendSetSceneLights(const QString &output, const we_lighting_uniforms_t &lights)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_scene_lights_t req{};
    QByteArray outBa = output.toUtf8();

    if (!waywal_ipc_serialize_req_set_scene_lights(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   &lights, &hdr, &req)) {
        return false;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
    return true;
}

bool IpcClient::sendSceneMutate(const waywal_ipc_req_scene_mutate_t &req,
                                const QString &payloadJson)
{
    const QByteArray jsonBa = payloadJson.toUtf8();
    const char *jsonPtr = jsonBa.isEmpty() ? nullptr : jsonBa.constData();
    const size_t cap = sizeof(waywal_ipc_req_scene_mutate_t) + static_cast<size_t>(jsonBa.size());
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_scene_mutate(&req, jsonPtr, &pkt.hdr, nullptr, pkt.payload.data(),
                                               pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendHotReloadCode(const waywal_ipc_req_hot_reload_code_t &req,
                                  const QByteArray &utf8Source)
{
    const char *targetOut = req.target_output[0] != '\0' ? req.target_output : "*";
    const size_t cap =
        sizeof(waywal_ipc_req_hot_reload_code_t) + static_cast<size_t>(utf8Source.size());
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_hot_reload_code(
            targetOut, req.code_kind, req.flags, req.object_id, req.pass_index, req.script_slot,
            req.uniform_name, utf8Source.isEmpty() ? "" : utf8Source.constData(),
            static_cast<size_t>(utf8Source.size()), &pkt.hdr, nullptr, pkt.payload.data(),
            pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendQuerySceneTree(const QString &output, uint32_t flags)
{
    waywal_ipc_req_query_scene_tree_t req{};
    QByteArray outBa = output.toUtf8();
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    if (!waywal_ipc_serialize_req_query_scene_tree(outBa.isEmpty() ? "*" : outBa.constData(), flags,
                                                   &pkt.hdr, &req)) {
        return false;
    }
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendSetShaderCombo(const QString &output, int32_t objectId, uint32_t passIndex,
                                   const QString &comboName, int32_t comboValue, uint32_t flags)
{
    waywal_ipc_req_set_shader_combo_t req{};
    QByteArray outBa = output.toUtf8();
    QByteArray comboBa = comboName.toUtf8();
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    if (!waywal_ipc_serialize_req_set_shader_combo(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   objectId, passIndex, comboBa.constData(),
                                                   comboValue, flags, &pkt.hdr, &req)) {
        return false;
    }
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendBrushStroke(const waywal_ipc_req_brush_stroke_t &req,
                                const std::vector<we_brush_dab_t> &dabs)
{
    const char *targetOut = req.target_output[0] != '\0' ? req.target_output : "*";
    const size_t cap = sizeof(waywal_ipc_req_brush_stroke_t) + dabs.size() * sizeof(we_brush_dab_t);
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_brush_stroke(
            targetOut, req.action, req.object_id, req.pass_index, req.tex_slot, req.stroke_id,
            &req.params, dabs.empty() ? nullptr : dabs.data(), static_cast<uint32_t>(dabs.size()),
            &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendParticleConfig(const QString &output, int32_t objectId, uint32_t configOp,
                                   uint32_t presetId, uint32_t flags, const void *payload,
                                   uint32_t payloadLen)
{
    QByteArray outBa = output.toUtf8();
    const size_t cap = sizeof(waywal_ipc_req_particle_config_t) + static_cast<size_t>(payloadLen);
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_particle_config(
            outBa.isEmpty() ? "*" : outBa.constData(), objectId, configOp, presetId, flags, payload,
            payloadLen, &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendAudioBindings(const QString &output, uint32_t bindOp,
                                  const we_audio_binding_t *bindings, uint32_t count,
                                  uint32_t removeId)
{
    QByteArray outBa = output.toUtf8();
    const size_t cap = sizeof(waywal_ipc_req_set_audio_bindings_t) +
                       static_cast<size_t>(count) * sizeof(we_audio_binding_t);
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_set_audio_bindings(
            outBa.isEmpty() ? "*" : outBa.constData(), bindOp, removeId, bindings, count, &pkt.hdr,
            nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendExportScenePkg(const QString &output, const QString &destPath, uint32_t flags)
{
    waywal_ipc_req_export_scene_pkg_t req{};
    QByteArray outBa = output.toUtf8();
    QByteArray dstBa = destPath.toUtf8();
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    if (!waywal_ipc_serialize_req_export_scene_pkg(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   dstBa.constData(), flags, &pkt.hdr, &req)) {
        return false;
    }
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendLintScenescript(const QString &sourceText, uint32_t flags)
{
    QByteArray srcBa = sourceText.toUtf8();
    const size_t cap =
        sizeof(waywal_ipc_req_lint_scenescript_t) + static_cast<size_t>(srcBa.size());
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_lint_scenescript(
            srcBa.isEmpty() ? "" : srcBa.constData(), static_cast<size_t>(srcBa.size()), flags,
            &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendReadbackTexture(const QString &output, int32_t objectId, uint32_t passIndex,
                                    uint32_t texSlot, uint32_t formatMode)
{
    waywal_ipc_req_readback_texture_t req{};
    QByteArray outBa = output.toUtf8();
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    if (!waywal_ipc_serialize_req_readback_texture(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   objectId, passIndex, texSlot, formatMode,
                                                   &pkt.hdr, &req)) {
        return false;
    }
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

QString IpcClient::stagePuppetMdlExportPath(const QString &fileHint)
{
    QString cacheRoot = qEnvironmentVariable("XDG_CACHE_HOME");
    if (cacheRoot.isEmpty()) {
        cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    }
    if (cacheRoot.isEmpty()) {
        cacheRoot = QDir::homePath() + QStringLiteral("/.cache");
    }
    const QString exportStageRoot = cacheRoot + QStringLiteral("/wwal/exports");
    if (!QDir().mkpath(exportStageRoot)) {
        return {};
    }

    static std::atomic<uint64_t> s_mdlExportSeq{0};
    QString safeLeaf = QFileInfo(fileHint).fileName();
    safeLeaf.replace(QLatin1Char('/'), QLatin1Char('_'));
    safeLeaf.replace(QLatin1Char('\\'), QLatin1Char('_'));
    safeLeaf.replace(QLatin1Char(':'), QLatin1Char('_'));
    safeLeaf.remove(QStringLiteral(".."));
    if (safeLeaf.isEmpty()) {
        safeLeaf = QStringLiteral("puppet_%1_%2.mdl")
                       .arg(QCoreApplication::applicationPid())
                       .arg(++s_mdlExportSeq);
    } else if (!safeLeaf.endsWith(QStringLiteral(".mdl"), Qt::CaseInsensitive)) {
        safeLeaf += QStringLiteral(".mdl");
    }

    const QString candidate = exportStageRoot + QLatin1Char('/') + safeLeaf;
    const QByteArray candidateUtf8 = candidate.toUtf8();
    char resolved[512] = {};
    if (!waywal_ipc_export_path_is_allowed(candidateUtf8.constData(), resolved, sizeof(resolved))) {
        return {};
    }
    return QString::fromUtf8(resolved);
}

bool IpcClient::sendPuppetSetMesh(uint32_t objectId, uint32_t flags,
                                  const std::vector<waywal_ipc_mdl_vertex_t> &vertices,
                                  const std::vector<uint16_t> &indices)
{
    const bool clearMesh = vertices.empty() && indices.empty();
    if (!clearMesh) {
        if (vertices.size() < 3 || vertices.size() > WAYWAL_IPC_RIGGING_MAX_VERTICES) {
            return false;
        }
        if (indices.size() < 3 || (indices.size() % 3) != 0) {
            return false;
        }
    }

    const uint32_t vertCount = static_cast<uint32_t>(vertices.size());
    const uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);
    if (triCount > WAYWAL_IPC_RIGGING_MAX_TRIANGLES) {
        return false;
    }

    size_t totalBytes = sizeof(waywal_ipc_puppet_set_mesh_t);
    if (!clearMesh) {
        if (!waywal_ipc_rigging_calc_mesh_bytes(vertCount, triCount, &totalBytes) ||
            totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
            return false;
        }
    }

    for (const auto &v : vertices) {
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(v.pos[k])) {
                return false;
            }
        }
        for (int k = 0; k < 4; ++k) {
            if (!std::isfinite(v.weight[k])) {
                return false;
            }
        }
        if (!std::isfinite(v.texcoord[0]) || !std::isfinite(v.texcoord[1])) {
            return false;
        }
    }
    for (uint16_t idx : indices) {
        if (idx >= vertCount) {
            return false;
        }
    }

    waywal_ipc_puppet_set_mesh_t req{};
    req.object_id = objectId;
    req.vertex_count = vertCount;
    req.triangle_count = triCount;
    req.flags = flags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_MESH;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);

    uint8_t *dst = pkt.payload.data();
    std::memcpy(dst, &req, sizeof(req));
    dst += sizeof(req);
    if (!clearMesh) {
        const size_t vBytes = static_cast<size_t>(vertCount) * sizeof(waywal_ipc_mdl_vertex_t);
        std::memcpy(dst, vertices.data(), vBytes);
        dst += vBytes;
        const size_t iBytes = static_cast<size_t>(triCount) * 3u * sizeof(uint16_t);
        std::memcpy(dst, indices.data(), iBytes);
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetSetMesh(uint32_t objectId, const std::vector<we_mdl_vertex_t> &vertices,
                                  const std::vector<uint16_t> &indices, uint32_t flags)
{
    std::vector<waywal_ipc_mdl_vertex_t> wireVerts(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto &src = vertices[i];
        auto &dst = wireVerts[i];
        for (int k = 0; k < 3; ++k) {
            dst.pos[k] = src.pos[k];
        }
        for (int k = 0; k < 4; ++k) {
            dst.blend_indices[k] = src.blend_indices[k];
            dst.weight[k] = src.weight[k];
        }
        dst.texcoord[0] = src.texcoord[0];
        dst.texcoord[1] = src.texcoord[1];
    }
    return sendPuppetSetMesh(objectId, flags, wireVerts, indices);
}

bool IpcClient::sendPuppetSetSkeleton(uint32_t objectId, uint32_t flags,
                                      const std::vector<waywal_ipc_mdl_bone_t> &bones)
{
    if (bones.size() > WAYWAL_IPC_RIGGING_MAX_BONES) {
        return false;
    }

    const size_t boneBytes = bones.size() * sizeof(waywal_ipc_mdl_bone_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_set_skeleton_t) + boneBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
        return false;
    }

    for (size_t i = 0; i < bones.size(); ++i) {
        const auto &b = bones[i];
        if (i == 0 && b.parent != 0xFFFFFFFFu) {
            return false;
        }
        if (i > 0 && b.parent != 0xFFFFFFFFu && b.parent >= i) {
            return false;
        }
        if (std::memchr(b.name, '\0', sizeof(b.name)) == nullptr ||
            std::memchr(b.simulation_json, '\0', sizeof(b.simulation_json)) == nullptr) {
            return false;
        }
        for (float f : b.local_trans) {
            if (!std::isfinite(f)) {
                return false;
            }
        }
    }

    waywal_ipc_puppet_set_skeleton_t req{};
    req.object_id = objectId;
    req.bone_count = static_cast<uint32_t>(bones.size());
    req.flags = flags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_SKELETON;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (boneBytes > 0) {
        std::memcpy(pkt.payload.data() + sizeof(req), bones.data(), boneBytes);
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetSetSkeleton(const waywal_ipc_puppet_set_skeleton_t &req,
                                      const std::vector<waywal_ipc_mdl_bone_t> &bones)
{
    return sendPuppetSetSkeleton(req.object_id, req.flags, bones);
}

bool IpcClient::sendPuppetSetSkeleton(uint32_t objectId,
                                      const std::vector<waywal_ipc_mdl_bone_t> &bones,
                                      uint32_t flags)
{
    return sendPuppetSetSkeleton(objectId, flags, bones);
}

bool IpcClient::sendPuppetSolveWeights(const waywal_ipc_puppet_solve_weights_t &req,
                                       const std::vector<uint64_t> &lockedVertexBitset)
{
    if (!std::isfinite(req.falloff_power) || !std::isfinite(req.smoothness) ||
        req.max_influences < 1u || req.max_influences > 4u || req.locked_vert_word_count > 1024u) {
        return false;
    }
    if ((req.flags & 0x2u) != 0u) {
        if (lockedVertexBitset.size() != req.locked_vert_word_count) {
            return false;
        }
    } else if (req.locked_vert_word_count != 0u || !lockedVertexBitset.empty()) {
        return false;
    }

    const size_t tailBytes = lockedVertexBitset.size() * sizeof(uint64_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_solve_weights_t) + tailBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
        return false;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (tailBytes > 0) {
        std::memcpy(pkt.payload.data() + sizeof(req), lockedVertexBitset.data(), tailBytes);
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetPaintWeights(uint32_t objectId, uint32_t flags,
                                       const std::vector<waywal_ipc_vertex_weight_t> &entries)
{
    if (entries.empty() || entries.size() > WAYWAL_IPC_RIGGING_MAX_VERTICES) {
        return false;
    }

    const size_t entryBytes = entries.size() * sizeof(waywal_ipc_vertex_weight_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_paint_weights_t) + entryBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
        return false;
    }

    for (const auto &e : entries) {
        for (float w : e.weight) {
            if (!std::isfinite(w)) {
                return false;
            }
        }
    }

    waywal_ipc_puppet_paint_weights_t req{};
    req.object_id = objectId;
    req.entry_count = static_cast<uint32_t>(entries.size());
    req.flags = flags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_PAINT_WEIGHTS;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    std::memcpy(pkt.payload.data() + sizeof(req), entries.data(), entryBytes);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetHeatmapMode(uint32_t objectId, bool enabled, uint16_t palette,
                                      uint32_t activeBoneIdx, float opacity)
{
    if (palette > 2u || !std::isfinite(opacity)) {
        return false;
    }

    waywal_ipc_puppet_heatmap_mode_t req{};
    req.object_id = objectId;
    req.enabled = enabled ? 1u : 0u;
    req.palette = palette;
    req.active_bone_idx = activeBoneIdx;
    req.opacity = std::clamp(opacity, 0.0f, 1.0f);

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetHeatmapMode(const waywal_ipc_puppet_heatmap_mode_t &req)
{
    return sendPuppetHeatmapMode(req.object_id, req.enabled != 0u, req.palette, req.active_bone_idx,
                                 req.opacity);
}

bool IpcClient::sendPuppetHeatmapMode(uint32_t objectId, bool enabled, uint32_t activeBoneIdx,
                                      float opacity, we_heatmap_palette_t palette)
{
    return sendPuppetHeatmapMode(objectId, enabled, static_cast<uint16_t>(palette), activeBoneIdx,
                                 opacity);
}

bool IpcClient::sendPuppetPreviewIk(const waywal_ipc_puppet_preview_ik_t &req,
                                    const std::vector<waywal_ipc_bone_trs_t> &overrides)
{
    if (overrides.size() != req.override_count ||
        req.override_count > WAYWAL_IPC_RIGGING_MAX_BONES) {
        return false;
    }
    for (float f : req.target_world) {
        if (!std::isfinite(f)) {
            return false;
        }
    }
    for (const auto &ov : overrides) {
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(ov.local_pos[k]) || !std::isfinite(ov.local_angle[k]) ||
                !std::isfinite(ov.local_scale[k])) {
                return false;
            }
        }
    }

    const size_t tailBytes = overrides.size() * sizeof(waywal_ipc_bone_trs_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_preview_ik_t) + tailBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
        return false;
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_PREVIEW_IK;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (tailBytes > 0) {
        std::memcpy(pkt.payload.data() + sizeof(req), overrides.data(), tailBytes);
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetSetAnimTracks(uint32_t objectId, uint32_t flags,
                                        const std::vector<PuppetAnimClipData> &clips)
{
    if (clips.size() > WAYWAL_IPC_RIGGING_MAX_ANIMS) {
        return false;
    }

    size_t totalBytes = sizeof(waywal_ipc_puppet_set_anim_tracks_t);
    size_t totalKeyframes = 0;
    for (const auto &clip : clips) {
        if (std::memchr(clip.desc.name, '\0', sizeof(clip.desc.name)) == nullptr ||
            std::memchr(clip.desc.mode_str, '\0', sizeof(clip.desc.mode_str)) == nullptr ||
            !std::isfinite(clip.desc.fps) || !std::isfinite(clip.desc.layer_blend) ||
            !std::isfinite(clip.desc.layer_rate)) {
            return false;
        }
        totalBytes += sizeof(waywal_ipc_anim_clip_desc_t);
        for (const auto &track : clip.tracks) {
            totalBytes += sizeof(waywal_ipc_anim_track_desc_t);
            totalKeyframes += track.keyframes.size();
            if (totalKeyframes > WAYWAL_IPC_RIGGING_MAX_KEYFRAMES) {
                return false;
            }
            for (const auto &kf : track.keyframes) {
                for (int k = 0; k < 3; ++k) {
                    if (!std::isfinite(kf.pos[k]) || !std::isfinite(kf.angle[k]) ||
                        !std::isfinite(kf.scale[k])) {
                        return false;
                    }
                }
            }
            totalBytes += track.keyframes.size() * sizeof(waywal_ipc_mdl_keyframe_t);
            if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
                return false;
            }
        }
    }

    waywal_ipc_puppet_set_anim_tracks_t req{};
    req.object_id = objectId;
    req.clip_count = static_cast<uint32_t>(clips.size());
    req.flags = flags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS;
    pkt.hdr.payload_size = static_cast<uint32_t>(totalBytes);
    pkt.payload.resize(totalBytes);

    uint8_t *dst = pkt.payload.data();
    std::memcpy(dst, &req, sizeof(req));
    dst += sizeof(req);
    for (const auto &clip : clips) {
        waywal_ipc_anim_clip_desc_t clipDesc = clip.desc;
        clipDesc.track_count = static_cast<uint32_t>(clip.tracks.size());
        std::memcpy(dst, &clipDesc, sizeof(clipDesc));
        dst += sizeof(clipDesc);
        for (const auto &track : clip.tracks) {
            waywal_ipc_anim_track_desc_t trackDesc = track.desc;
            trackDesc.keyframe_count = static_cast<uint32_t>(track.keyframes.size());
            std::memcpy(dst, &trackDesc, sizeof(trackDesc));
            dst += sizeof(trackDesc);
            if (!track.keyframes.empty()) {
                const size_t kfBytes = track.keyframes.size() * sizeof(waywal_ipc_mdl_keyframe_t);
                std::memcpy(dst, track.keyframes.data(), kfBytes);
                dst += kfBytes;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::sendPuppetScrubTime(uint32_t objectId, float timeSec, bool pausePlayback,
                                    uint32_t flags)
{
    if (!std::isfinite(timeSec)) {
        return false;
    }

    waywal_ipc_puppet_scrub_t req{};
    req.object_id = objectId;
    req.time_sec = timeSec;
    req.pause_playback = pausePlayback ? 1u : 0u;
    req.flags = flags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SCRUB_TIME;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

bool IpcClient::expectsGenericOkForOpcode(uint16_t reqOpcode) noexcept
{
    // Allowlist only opcodes whose wire contract returns WAYWAL_RESP_OK so
    // unrecognized or dedicated-response opcodes never desync the pending queue.
    switch (reqOpcode) {
    case WAYWAL_REQ_CLEAR:
    case WAYWAL_REQ_SET_IMAGE:
    case WAYWAL_REQ_SET_VIDEO:
    case WAYWAL_REQ_TOGGLE:
    case WAYWAL_REQ_PAUSE:
    case WAYWAL_REQ_UNPAUSE:
    case WAYWAL_REQ_KILL:
    case WAYWAL_REQ_SLIDESHOW:
    case WAYWAL_REQ_SHADER:
    case WAYWAL_REQ_RESTORE:
    case WAYWAL_REQ_SLIDESHOW_CTRL:
    case WAYWAL_REQ_WORKSHOP:
    case WAYWAL_REQ_SET_UNIFORM:
    case WAYWAL_REQ_SUBSCRIBE:
    case WAYWAL_REQ_AUDIO_CTRL:
    case WAYWAL_REQ_SLIDESHOW_NEXT:
    case WAYWAL_REQ_SLIDESHOW_PREV:
    case WAYWAL_REQ_SET_SHADER_PARAM:
    case WAYWAL_REQ_PUPPET_SET_SKELETON:
    case WAYWAL_REQ_PUPPET_PAINT_WEIGHTS:
    case WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE:
    case WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS:
    case WAYWAL_REQ_PUPPET_SCRUB_TIME:
        return true;
    case WAYWAL_REQ_SCENE_MUTATE:
    case WAYWAL_REQ_HOT_RELOAD_CODE:
    case WAYWAL_REQ_QUERY_SCENE_TREE:
    case WAYWAL_REQ_SET_SHADER_COMBO:
    case WAYWAL_REQ_BRUSH_STROKE:
    case WAYWAL_REQ_PARTICLE_CONFIG:
    case WAYWAL_REQ_SET_AUDIO_BINDINGS:
    case WAYWAL_REQ_EXPORT_SCENE_PKG:
    case WAYWAL_REQ_LINT_SCENESCRIPT:
    case WAYWAL_REQ_READBACK_TEXTURE:
    case WAYWAL_REQ_PUPPET_SET_MESH:
    case WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS:
    case WAYWAL_REQ_PUPPET_PREVIEW_IK:
    case WAYWAL_REQ_EXPORT_PUPPET_MDL:
    default:
        return false;
    }
}

bool IpcClient::sendExportPuppetMdl(uint32_t objectId, uint32_t flags, const QString &outPath)
{
    waywal_ipc_export_puppet_mdl_t req{};
    req.object_id = objectId;
    req.flags = flags;

    if ((flags & 0x2u) != 0u) {
        const QString effectivePath = outPath.isEmpty() ? stagePuppetMdlExportPath() : outPath;
        const QByteArray pathBa = effectivePath.toUtf8();
        char resolved[sizeof(req.out_path)] = {};
        if (effectivePath.isEmpty() || static_cast<size_t>(pathBa.size()) >= sizeof(req.out_path) ||
            !waywal_ipc_export_path_is_allowed(pathBa.constData(), resolved, sizeof(resolved))) {
            emit errorOccurred(
                QStringLiteral("Rejected puppet .mdl export path outside Landlock staging root"));
            return false;
        }
        const size_t resolvedLen = strnlen(resolved, sizeof(req.out_path) - 1u);
        std::memcpy(req.out_path, resolved, resolvedLen);
        req.out_path[resolvedLen] = '\0';
    }

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_EXPORT_PUPPET_MDL;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }
    wakeupWorker();
    return true;
}

} // namespace waywal::studio
