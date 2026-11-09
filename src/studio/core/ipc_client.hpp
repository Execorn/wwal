// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_IPC_CLIENT_HPP
#define WAYWAL_STUDIO_IPC_CLIENT_HPP

#include "services.hpp"
#include "waywal/ipc_proto.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

/* ========================================================================= */
/* Asynchronous Unix Domain Socket IPC Client with 144 Hz Coalescing         */
/* ========================================================================= */

class IpcClient : public QObject, public IIpcClientService {
    Q_OBJECT

  public:
    explicit IpcClient(QString namespaceStr = QString(), QString socketPath = QString(),
                       QObject *parent = nullptr);
    ~IpcClient() override;

    // Service Interface Implementation
    [[nodiscard]] bool isConnected() const override;
    void sendSetUniform(const QString &output, const QString &name, const float *vals,
                        uint32_t count, bool persist = false) override;
    void sendSetImage(const waywal_img_metadata_t &meta, int memfd = -1) override;
    void sendAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                              uint8_t batteryPolicy = 0) override;
    void requestOutputsJson(std::function<void(const QString &json)> callback = nullptr) override;
    void requestTelemetry(
        std::function<void(const TelemetrySnapshot &snapshot)> callback = nullptr) override;
    void requestOutputDmabuf(
        const QString &output,
        std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)>
            callback = nullptr) override;
    void subscribeEvents() override;

    // Steam Workshop & Open Hub Operations
    void requestWorkshopSync(
        uint32_t flags, const QString &customRoot,
        std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> callback = nullptr);
    void requestWorkshopDownload(
        uint64_t publishedFileId, uint32_t priority = 50, uint32_t preferredBackend = 0,
        std::function<void(const waywal_ipc_resp_workshop_download_t &resp)> callback = nullptr);
    void requestWwpkgInstall(
        const uint8_t packageUuid[16], uint32_t flags, const QString &targetOutput,
        std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
            callback = nullptr);
    void requestScanCompat(
        const QString &targetPath, uint32_t autoRepairFlags = 0,
        std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback = nullptr);

    // Lifecycle
    void start();
    void stop();

    [[nodiscard]] QString socketPath() const;
    [[nodiscard]] QString namespaceStr() const { return m_namespaceStr; }

  signals:
    void connectionStateChanged(bool connected);
    void outputsJsonReceived(const QString &json);
    void telemetryReceived(const TelemetrySnapshot &snapshot);
    void dmabufReceived(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds);
    void eventReceived(const QString &eventType, const QJsonObject &data);
    void errorOccurred(const QString &error);
    void workshopSyncReceived(const waywal_ipc_resp_workshop_sync_t &resp);
    void workshopDownloadReceived(const waywal_ipc_resp_workshop_download_t &resp);
    void wwpkgInstallReceived(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg);
    void scanCompatReceived(const waywal_ipc_resp_scan_compat_t &resp);

  private:
    struct OutboundPacket {
        enum class Type {
            Raw,
            SetUniform,
            QueryJson,
            Telemetry,
            ExportDmabuf,
            Subscribe,
            AudioPower,
            WorkshopSync,
            WorkshopDownload,
            WwpkgInstall,
            ScanCompat
        };

        Type type{Type::Raw};
        waywal_ipc_hdr_t hdr{};
        std::vector<uint8_t> payload;
        int attachedFd{-1};

        // Callbacks for request-response tracking
        std::function<void(const QString &json)> jsonCallback;
        std::function<void(const TelemetrySnapshot &snapshot)> telemetryCallback;
        std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)>
            dmabufCallback;
        std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> workshopSyncCallback;
        std::function<void(const waywal_ipc_resp_workshop_download_t &resp)>
            workshopDownloadCallback;
        std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
            wwpkgInstallCallback;
        std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> scanCompatCallback;
    };

    struct PendingRequest {
        uint16_t expectedOpcode{0};
        std::function<void(const QString &json)> jsonCallback;
        std::function<void(const TelemetrySnapshot &snapshot)> telemetryCallback;
        std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)>
            dmabufCallback;
        std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> workshopSyncCallback;
        std::function<void(const waywal_ipc_resp_workshop_download_t &resp)>
            workshopDownloadCallback;
        std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
            wwpkgInstallCallback;
        std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> scanCompatCallback;
        std::chrono::steady_clock::time_point sentAt;
    };

    void workerLoop();
    bool connectSocket();
    void closeSocket();
    void handleIncomingPacket();
    void flushOutboundQueue();
    void wakeupWorker();

    QString m_namespaceStr;
    QString m_customSocketPath;
    QString m_resolvedSocketPath;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    int m_socketFd{-1};
    int m_wakeupFd{-1}; // eventfd for waking up poll

    std::thread m_workerThread;

    // MPSC Outbound Queue & 144 Hz Coalescing
    mutable std::mutex m_queueMutex;
    std::deque<OutboundPacket> m_outQueue;
    // Map of uniform key ("output:uniform_name") -> coalesced payload
    std::unordered_map<std::string, waywal_set_uniform_payload_t> m_pendingUniforms;

    // Request tracking queue for in-flight queries
    std::mutex m_requestsMutex;
    std::deque<PendingRequest> m_pendingRequests;

    std::chrono::steady_clock::time_point m_lastUniformFlush;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_IPC_CLIENT_HPP
