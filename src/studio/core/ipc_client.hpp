// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_IPC_CLIENT_HPP
#define WAYWAL_STUDIO_IPC_CLIENT_HPP

#include "services.hpp"
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/ipc_proto_phase3.h"

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
                       QObject *parent = nullptr, bool autoStart = true);
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
            callback = nullptr,
        int wwpkgFd = -1);
    void installWwpkg(
        const QString &packagePath, const QString &targetOutput = QStringLiteral("*"),
        uint32_t flags = WAYWAL_WWPKG_INSTALL_FLAG_AUTO_APPLY |
                         WAYWAL_WWPKG_INSTALL_FLAG_FORCE_OVERWRITE,
        std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
            callback = nullptr);
    void requestScanCompat(
        const QString &targetPath, uint32_t autoRepairFlags = 0,
        std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback = nullptr);

    bool requestCompileShaderGraph(const QString &output, const QString &path,
                                   const QString &inlineJson, uint32_t flags,
                                   waywal_ipc_resp_compile_shader_graph_t *outResp = nullptr);
    bool sendSetCamera3D(const QString &output, const we_camera3d_t &cam);
    bool sendSetPhysicsParams(const QString &output, int32_t objectId, int32_t boneIndex,
                              const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                              const we_cursor_force_t &cursor);
    bool sendSetSceneLights(const QString &output, const we_lighting_uniforms_t &lights);

    // Authoring Scene, Code, Brush, Particle, Audio & Export IPC
    bool sendSceneMutate(const waywal_ipc_req_scene_mutate_t &req,
                         const QString &payloadJson = QString());
    bool sendHotReloadCode(const waywal_ipc_req_hot_reload_code_t &req,
                           const QByteArray &utf8Source);
    bool sendQuerySceneTree(const QString &output = QString(), uint32_t flags = 0);
    bool sendSetShaderCombo(const QString &output, int32_t objectId, uint32_t passIndex,
                            const QString &comboName, int32_t comboValue, uint32_t flags = 0);
    bool sendBrushStroke(const waywal_ipc_req_brush_stroke_t &req,
                         const std::vector<we_brush_dab_t> &dabs);
    bool sendParticleConfig(const QString &output, int32_t objectId, uint32_t configOp,
                            uint32_t presetId, uint32_t flags, const void *payload,
                            uint32_t payloadLen);
    bool sendAudioBindings(const QString &output, uint32_t bindOp,
                           const we_audio_binding_t *bindings, uint32_t count,
                           uint32_t removeId = 0);
    bool sendExportScenePkg(const QString &output, const QString &destPath, uint32_t flags = 0);
    bool sendLintScenescript(const QString &sourceText, uint32_t flags = 0);
    bool sendReadbackTexture(const QString &output, int32_t objectId, uint32_t passIndex,
                             uint32_t texSlot, uint32_t formatMode = 0);

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
    void shaderGraphCompiled(const waywal_ipc_resp_compile_shader_graph_t &resp);
    void camera3DUpdated(const waywal_ipc_resp_set_camera_3d_t &resp);
    void physicsParamsUpdated(const waywal_ipc_resp_set_physics_params_t &resp);
    void sceneLightsUpdated(const waywal_ipc_resp_set_scene_lights_t &resp);
    void sceneMutated(const waywal_ipc_resp_scene_mutate_t &resp);
    void codeHotReloaded(const waywal_ipc_resp_hot_reload_code_t &resp, const QString &metaJson);
    void sceneTreeQueried(const waywal_ipc_resp_query_scene_tree_t &resp, const QString &treeJson);
    void shaderComboUpdated(const waywal_ipc_resp_set_shader_combo_t &resp);
    void brushStrokeCompleted(const waywal_ipc_resp_brush_stroke_t &resp);
    void particleConfigUpdated(const waywal_ipc_resp_particle_config_t &resp);
    void audioBindingsUpdated(const waywal_ipc_resp_set_audio_bindings_t &resp);
    void scenePkgExported(const waywal_ipc_resp_export_scene_pkg_t &resp);
    void sceneScriptLinted(const waywal_ipc_resp_lint_scenescript_t &resp);
    void textureReadbackReceived(const waywal_ipc_resp_readback_texture_t &resp,
                                 const QByteArray &data);

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
    bool popPendingForOpcode(uint16_t reqOpcode, PendingRequest &outReq);
    void failInFlightRequests(const QString &reason);

    mutable std::mutex m_stateMutex;
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
