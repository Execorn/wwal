// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc_client.hpp"

#include "waywal/log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace waywal::studio {

/* ========================================================================= */
/* Helper Functions                                                          */
/* ========================================================================= */

static bool readExactNonBlocking(int fd, void *buf, size_t count, int timeoutMs)
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
            return false; // Remote closed
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
            return false; // Timeout or error
        }
        return false;
    }
    return true;
}

static bool writeExactNonBlocking(int fd, const void *buf, size_t count, int timeoutMs)
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

/* ========================================================================= */
/* IpcClient Implementation                                                  */
/* ========================================================================= */

IpcClient::IpcClient(QString namespaceStr, QString socketPath, QObject *parent)
    : QObject(parent), m_namespaceStr(std::move(namespaceStr)),
      m_customSocketPath(std::move(socketPath)),
      m_lastUniformFlush(std::chrono::steady_clock::now())
{
    if (m_namespaceStr.isEmpty()) {
        m_namespaceStr = QStringLiteral("default");
    }

    m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeupFd < 0) {
        WAYWAL_LOG_ERR("IpcClient: eventfd creation failed: %s", strerror(errno));
    }

    start();
}

IpcClient::~IpcClient()
{
    stop();
    if (m_wakeupFd >= 0) {
        close(m_wakeupFd);
        m_wakeupFd = -1;
    }
}

void IpcClient::start()
{
    if (m_running.exchange(true)) {
        return; // Already running
    }

    m_workerThread = std::thread(&IpcClient::workerLoop, this);
}

void IpcClient::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }

    wakeupWorker();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    closeSocket();
}

bool IpcClient::isConnected() const { return m_connected.load(); }

QString IpcClient::socketPath() const
{
    if (!m_customSocketPath.isEmpty()) {
        return m_customSocketPath;
    }
    return m_resolvedSocketPath;
}

void IpcClient::wakeupWorker()
{
    if (m_wakeupFd >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] ssize_t nw = write(m_wakeupFd, &val, sizeof(val));
    }
}

void IpcClient::closeSocket()
{
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
    if (m_connected.exchange(false)) {
        QMetaObject::invokeMethod(
            this, [this]() { emit connectionStateChanged(false); }, Qt::QueuedConnection);
    }
}

bool IpcClient::connectSocket()
{
    QString targetPath = m_customSocketPath;
    if (targetPath.isEmpty()) {
        path_buf_t pb;
        if (waywal_ipc_resolve_socket_path(&pb, m_namespaceStr.toUtf8().constData())) {
            targetPath = QString::fromUtf8(path_buf_cstr(&pb));
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
    std::memcpy(addr.sun_path, pathBa.constData(), pathBa.size());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return false;
    }

    int bufSize = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

    int res = ::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    if (res < 0 && errno == EINPROGRESS) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = poll(&pfd, 1, 150);
        if (pr <= 0) {
            close(fd);
            return false;
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) {
            close(fd);
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
                // Sleep with backoff waiting on wakeupFd
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
            // Connected! Subscribe to async events
            subscribeEvents();
        }

        struct pollfd pfds[2];
        pfds[0].fd = m_wakeupFd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        pfds[1].fd = m_socketFd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        // Rate-limit uniform flushing to 144 Hz (~6.9 ms)
        int pollTimeoutMs = 7;
        int pr = poll(pfds, 2, pollTimeoutMs);

        if (pr < 0) {
            if (errno == EINTR)
                continue;
            closeSocket();
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            uint64_t val = 0;
            [[maybe_unused]] ssize_t nr = read(m_wakeupFd, &val, sizeof(val));
        }

        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            closeSocket();
            continue;
        }

        if (pfds[1].revents & POLLIN) {
            handleIncomingPacket();
        }

        // Flush outbound packets and coalesced uniforms
        flushOutboundQueue();
    }
}

void IpcClient::flushOutboundQueue()
{
    if (m_socketFd < 0 || !m_connected.load()) {
        return;
    }

    // 1. Drain & send coalesced uniforms
    std::vector<waywal_set_uniform_payload_t> uniformBatch;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pendingUniforms.empty()) {
            uniformBatch.reserve(m_pendingUniforms.size());
            for (auto &[key, payload] : m_pendingUniforms) {
                uniformBatch.push_back(payload);
            }
            m_pendingUniforms.clear();
        }
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
    }

    // 2. Drain & send generic queued packets
    std::deque<OutboundPacket> packets;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        packets.swap(m_outQueue);
    }

    for (auto &pkt : packets) {
        bool sentHdr = false;
        if (pkt.attachedFd >= 0) {
            sentHdr = waywal_ipc_send(m_socketFd, &pkt.hdr, pkt.attachedFd);
            close(pkt.attachedFd);
            pkt.attachedFd = -1;
        } else {
            sentHdr = writeExactNonBlocking(m_socketFd, &pkt.hdr, sizeof(pkt.hdr), 200);
        }

        if (!sentHdr) {
            closeSocket();
            return;
        }

        if (pkt.hdr.payload_size > 0 && !pkt.payload.empty()) {
            if (!writeExactNonBlocking(m_socketFd, pkt.payload.data(), pkt.payload.size(), 500)) {
                closeSocket();
                return;
            }
        }

        // Register pending request if callbacks are attached
        if (pkt.jsonCallback || pkt.telemetryCallback || pkt.dmabufCallback ||
            pkt.workshopSyncCallback || pkt.workshopDownloadCallback || pkt.wwpkgInstallCallback ||
            pkt.scanCompatCallback) {
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
    }
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

    std::vector<uint8_t> payload;
    if (hdr.payload_size > 0) {
        if (hdr.payload_size > 64 * 1024 * 1024) { // 64 MB security limit
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

    // Process responses based on opcode
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

    // Match against in-flight requests
    PendingRequest req{};
    bool hasReq = false;
    {
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        if (!m_pendingRequests.empty()) {
            req = std::move(m_pendingRequests.front());
            m_pendingRequests.pop_front();
            hasReq = true;
        }
    }

    if (hdr.opcode == WAYWAL_RESP_DMABUF ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_EXPORT_OUTPUT_DMABUF &&
         hdr.opcode == WAYWAL_RESP_OK)) {
        if (payload.size() >= sizeof(waywal_export_dmabuf_resp_t)) {
            waywal_export_dmabuf_resp_t desc{};
            std::memcpy(&desc, payload.data(), sizeof(desc));

            QVector<int> qfds;
            for (size_t i = 0; i < numFds; ++i) {
                if (fds[i] >= 0) {
                    qfds.append(fds[i]);
                }
            }

            if (req.dmabufCallback) {
                req.dmabufCallback(desc, qfds);
            }

            QMetaObject::invokeMethod(
                this, [this, desc, qfds]() { emit dmabufReceived(desc, qfds); },
                Qt::QueuedConnection);
        }
        return;
    }

    // Free any unextracted file descriptors
    for (size_t i = 0; i < numFds; ++i) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }

    if (hdr.opcode == WAYWAL_RESP_TELEMETRY ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_GET_TELEMETRY &&
         hdr.opcode == WAYWAL_RESP_OK)) {
        if (payload.size() >= sizeof(waywal_telemetry_snapshot_t)) {
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
                ot.name = QString::fromUtf8(ro.name);
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

            if (req.telemetryCallback) {
                req.telemetryCallback(snap);
            }

            QMetaObject::invokeMethod(
                this, [this, snap]() { emit telemetryReceived(snap); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hasReq && req.expectedOpcode == WAYWAL_REQ_QUERY_JSON) {
        QString jsonStr = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                            static_cast<qsizetype>(payload.size()));

        if (req.jsonCallback) {
            req.jsonCallback(jsonStr);
        }

        QMetaObject::invokeMethod(
            this, [this, jsonStr]() { emit outputsJsonReceived(jsonStr); }, Qt::QueuedConnection);
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_SYNC ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_WORKSHOP_SYNC)) {
        if (payload.size() >= sizeof(waywal_ipc_resp_workshop_sync_t)) {
            waywal_ipc_resp_workshop_sync_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (req.workshopSyncCallback) {
                req.workshopSyncCallback(resp);
            }
            QMetaObject::invokeMethod(
                this, [this, resp]() { emit workshopSyncReceived(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_DOWNLOAD ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_WORKSHOP_DOWNLOAD)) {
        if (payload.size() >= sizeof(waywal_ipc_resp_workshop_download_t)) {
            waywal_ipc_resp_workshop_download_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (req.workshopDownloadCallback) {
                req.workshopDownloadCallback(resp);
            }
            QMetaObject::invokeMethod(
                this, [this, resp]() { emit workshopDownloadReceived(resp); },
                Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WWPKG_INSTALL ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_WWPKG_INSTALL)) {
        if (payload.size() >= sizeof(waywal_ipc_resp_wwpkg_install_t)) {
            waywal_ipc_resp_wwpkg_install_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            QString errMsg;
            if (resp.error_msg_len > 0 && payload.size() >= sizeof(resp) + resp.error_msg_len) {
                errMsg =
                    QString::fromUtf8(reinterpret_cast<const char *>(payload.data() + sizeof(resp)),
                                      static_cast<qsizetype>(resp.error_msg_len));
            }
            if (req.wwpkgInstallCallback) {
                req.wwpkgInstallCallback(resp, errMsg);
            }
            QMetaObject::invokeMethod(
                this, [this, resp, errMsg]() { emit wwpkgInstallReceived(resp, errMsg); },
                Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SCAN_COMPAT ||
        (hasReq && req.expectedOpcode == WAYWAL_REQ_SCAN_COMPAT)) {
        if (payload.size() >= sizeof(waywal_ipc_resp_scan_compat_t)) {
            waywal_ipc_resp_scan_compat_t resp{};
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (req.scanCompatCallback) {
                req.scanCompatCallback(resp);
            }
            QMetaObject::invokeMethod(
                this, [this, resp]() { emit scanCompatReceived(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_ERR) {
        QString err = QStringLiteral("Daemon returned error");
        if (!payload.empty()) {
            err = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                    static_cast<qsizetype>(payload.size()));
        }
        QMetaObject::invokeMethod(
            this, [this, err]() { emit errorOccurred(err); }, Qt::QueuedConnection);
    }
}

/* ========================================================================= */
/* Public API Methods                                                        */
/* ========================================================================= */

void IpcClient::sendSetUniform(const QString &output, const QString &name, const float *vals,
                               uint32_t count, bool persist)
{
    if (!vals || count == 0)
        return;

    waywal_set_uniform_payload_t payload{};
    std::memset(&payload, 0, sizeof(payload));

    QByteArray outBa = output.toUtf8();
    QByteArray nameBa = name.toUtf8();

    // Standard layout (64/64)
    std::strncpy(payload.output, outBa.constData(), sizeof(payload.output) - 1);
    std::strncpy(payload.name, nameBa.constData(), sizeof(payload.name) - 1);
    payload.num_components = std::min(count, 4U);
    for (uint32_t i = 0; i < payload.num_components; ++i) {
        payload.values[i] = vals[i];
    }

    // Structured layout with persistence flags
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

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // Overwrite in-place: coalesces continuous slider/color drags at 144 Hz
        m_pendingUniforms[key] = payload;
    }

    wakeupWorker();
}

void IpcClient::sendSetImage(const waywal_img_metadata_t &meta, int memfd)
{
    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::Raw;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SET_IMAGE;
    pkt.hdr.payload_size = sizeof(waywal_img_metadata_t) + meta.extra_data_len;

    pkt.payload.resize(pkt.hdr.payload_size);
    std::memcpy(pkt.payload.data(), &meta, sizeof(waywal_img_metadata_t));

    if (memfd >= 0) {
        pkt.attachedFd = fcntl(memfd, F_DUPFD_CLOEXEC, 0);
        if (pkt.attachedFd < 0) {
            pkt.attachedFd = dup(memfd);
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
    (void)batteryPolicy; // Reserved for power governor packet extension
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
    waywal_ipc_req_workshop_sync_t req{};
    req.flags = flags;
    QByteArray rootBa = customRoot.toUtf8();
    std::strncpy(req.custom_root, rootBa.constData(), sizeof(req.custom_root) - 1);

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WorkshopSync;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_WORKSHOP_SYNC;
    pkt.hdr.payload_size = sizeof(req);
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
    waywal_ipc_req_workshop_download_t req{};
    req.published_file_id = publishedFileId;
    req.priority = priority;
    req.preferred_backend = preferredBackend;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WorkshopDownload;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_WORKSHOP_DOWNLOAD;
    pkt.hdr.payload_size = sizeof(req);
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
        callback)
{
    waywal_ipc_req_wwpkg_install_t req{};
    if (packageUuid) {
        std::memcpy(req.package_uuid, packageUuid, 16);
    }
    req.flags = flags;
    QByteArray outBa = targetOutput.toUtf8();
    req.target_output_len = static_cast<uint32_t>(outBa.size());

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::WwpkgInstall;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_WWPKG_INSTALL;
    pkt.hdr.payload_size = sizeof(req) + req.target_output_len;
    pkt.payload.resize(pkt.hdr.payload_size);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (req.target_output_len > 0) {
        std::memcpy(pkt.payload.data() + sizeof(req), outBa.constData(), req.target_output_len);
    }
    pkt.wwpkgInstallCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

void IpcClient::requestScanCompat(
    const QString &targetPath, uint32_t autoRepairFlags,
    std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback)
{
    waywal_ipc_req_scan_compat_t req{};
    QByteArray pathBa = targetPath.toUtf8();
    std::strncpy(req.target_path, pathBa.constData(), sizeof(req.target_path) - 1);
    req.auto_repair_flags = autoRepairFlags;

    OutboundPacket pkt{};
    pkt.type = OutboundPacket::Type::ScanCompat;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SCAN_COMPAT;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.scanCompatCallback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_outQueue.push_back(std::move(pkt));
    }

    wakeupWorker();
}

} // namespace waywal::studio
