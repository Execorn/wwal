// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_IPC_CLIENT_HPP
#define WAYWAL_STUDIO_IPC_CLIENT_HPP

#include "services.hpp"
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace waywal::studio {

struct PuppetAnimTrackData {
    waywal_ipc_anim_track_desc_t desc{};
    std::vector<waywal_ipc_mdl_keyframe_t> keyframes;
};

struct PuppetAnimClipData {
    waywal_ipc_anim_clip_desc_t desc{};
    std::vector<PuppetAnimTrackData> tracks;
};

/* Owns a single POSIX file descriptor across error and early-return paths. */
class ScopedFd {
  public:
    ScopedFd() noexcept = default;
    explicit ScopedFd(int fd) noexcept : m_fd(fd) {}
    ~ScopedFd() { reset(); }

    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    ScopedFd(ScopedFd &&other) noexcept : m_fd(other.release()) {}
    ScopedFd &operator=(ScopedFd &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    int release() noexcept
    {
        int out = m_fd;
        m_fd = -1;
        return out;
    }

    void reset(int newFd = -1) noexcept;

  private:
    int m_fd{-1};
};

/* Closes received SCM_RIGHTS file descriptors unless transferred to a validated consumer. */
struct ScopedFdArrayGuard {
    static constexpr size_t MAX_ANCILLARY_FDS = 4;

    int *fds{nullptr};
    size_t count{0};

    ScopedFdArrayGuard() noexcept = default;
    ScopedFdArrayGuard(int *fdsPtr, size_t n) noexcept : fds(fdsPtr), count(n) {}
    ~ScopedFdArrayGuard();

    ScopedFdArrayGuard(const ScopedFdArrayGuard &) = delete;
    ScopedFdArrayGuard &operator=(const ScopedFdArrayGuard &) = delete;
};

struct IpcOutboundPacket {
    enum class Kind : uint8_t {
        Standard = 0,
        DeferredSetImage = 1,
        DeferredWwpkgHubLookup = 2,
        Subscribe = 3,
    };

    Kind kind{Kind::Standard};
    waywal_ipc_hdr_t hdr{};
    std::vector<uint8_t> payload;
    ScopedFd attachedFd;

    waywal_img_metadata_t imgMeta{};
    QStringList imgTargetOutputs;
    QString imgCustomShader;
    uint8_t hubPackageUuid[16]{};

    std::function<void(const QString &)> jsonCallback;
    std::function<void(const TelemetrySnapshot &)> telemetryCallback;
    std::function<void(const waywal_export_dmabuf_resp_t &, const QVector<int> &)> dmabufCallback;
    std::function<void(const waywal_ipc_resp_workshop_sync_t &)> workshopSyncCallback;
    std::function<void(const waywal_ipc_resp_workshop_download_t &)> workshopDownloadCallback;
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &, const QString &)>
        wwpkgInstallCallback;
    std::function<void(const waywal_ipc_resp_scan_compat_t &)> scanCompatCallback;
};

struct IpcPendingRequest {
    uint64_t sendSeq{0};
    uint16_t expectedOpcode{0};
    std::chrono::steady_clock::time_point sentAt{};

    std::function<void(const QString &)> jsonCallback;
    std::function<void(const TelemetrySnapshot &)> telemetryCallback;
    std::function<void(const waywal_export_dmabuf_resp_t &, const QVector<int> &)> dmabufCallback;
    std::function<void(const waywal_ipc_resp_workshop_sync_t &)> workshopSyncCallback;
    std::function<void(const waywal_ipc_resp_workshop_download_t &)> workshopDownloadCallback;
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &, const QString &)>
        wwpkgInstallCallback;
    std::function<void(const waywal_ipc_resp_scan_compat_t &)> scanCompatCallback;

    [[nodiscard]] bool hasCallback() const noexcept
    {
        return static_cast<bool>(jsonCallback) || static_cast<bool>(telemetryCallback) ||
               static_cast<bool>(dmabufCallback) || static_cast<bool>(workshopSyncCallback) ||
               static_cast<bool>(workshopDownloadCallback) ||
               static_cast<bool>(wwpkgInstallCallback) || static_cast<bool>(scanCompatCallback);
    }

    static IpcPendingRequest fromPacket(IpcOutboundPacket &pkt, uint64_t seq, uint16_t reqOpcode,
                                        std::chrono::steady_clock::time_point now) noexcept
    {
        IpcPendingRequest req{};
        req.sendSeq = seq;
        req.expectedOpcode = reqOpcode;
        req.sentAt = now;
        req.jsonCallback = std::move(pkt.jsonCallback);
        req.telemetryCallback = std::move(pkt.telemetryCallback);
        req.dmabufCallback = std::move(pkt.dmabufCallback);
        req.workshopSyncCallback = std::move(pkt.workshopSyncCallback);
        req.workshopDownloadCallback = std::move(pkt.workshopDownloadCallback);
        req.wwpkgInstallCallback = std::move(pkt.wwpkgInstallCallback);
        req.scanCompatCallback = std::move(pkt.scanCompatCallback);
        return req;
    }
};

/* Stateless serialization, validation, memfd packaging, and filesystem lookup helpers. */
class IpcServiceCodecs {
  public:
    static bool isFireAndForgetNoReplyOpcode(uint16_t reqOpcode) noexcept;
    static bool expectsGenericOkForOpcode(uint16_t reqOpcode) noexcept;

    static bool parseUuidString(const char *str, uint8_t outUuid[16]) noexcept;
    static QString uuidBytesToDashedHex(const uint8_t uuid[16]);
    static bool isAllZeroUuid(const uint8_t uuid[16]) noexcept;
    static int dupCloexec(int fd) noexcept;
    static int tryOpenHubPackageByUuid(const uint8_t uuid[16]);
    static QString stagePuppetMdlExportPath(const QString &fileHint = QString());

    static bool encodeSetUniform(const QString &output, const QString &name, const float *vals,
                                 uint32_t count, bool persist,
                                 waywal_set_uniform_payload_t &outPayload,
                                 QString *outErr = nullptr);

    static bool prepareSetImagePacket(const waywal_img_metadata_t &meta,
                                      const QStringList &targetOutputs,
                                      const QString &customShaderGlsl, int inMemfd,
                                      waywal_ipc_hdr_t &outHdr, ScopedFd &outMemfd,
                                      QString *outErr = nullptr);

    static bool encodeAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                                       uint8_t batteryPolicy,
                                       waywal_audio_ctrl_payload_t &outPayload,
                                       QString *outErr = nullptr);

    static bool decodeTelemetrySnapshot(const std::vector<uint8_t> &payload,
                                        TelemetrySnapshot &outSnap, QString *outErr = nullptr);

    static bool decodeDmabufExport(const std::vector<uint8_t> &payload, int fds[], size_t numFds,
                                   waywal_export_dmabuf_resp_t &outDesc,
                                   QVector<int> &outTransferredFds, QString *outErr = nullptr);
};

/* Non-blocking Unix domain socket and eventfd framing transport with monotonic deadlines. */
class IpcTransport {
  public:
    IpcTransport() noexcept = default;
    ~IpcTransport();

    IpcTransport(const IpcTransport &) = delete;
    IpcTransport &operator=(const IpcTransport &) = delete;

    bool initWakeupFd() noexcept;
    void destroyWakeupFd() noexcept;

    [[nodiscard]] int socketFd() const noexcept { return m_socketFd; }
    [[nodiscard]] int wakeupFd() const noexcept { return m_wakeupFd; }
    [[nodiscard]] bool isConnected() const noexcept { return m_connected.load(); }

    void wakeup() noexcept;
    void drainWakeup() noexcept;

    bool connectSocket(const QString &namespaceStr, const QString &customSocketPath,
                       QString &outResolvedPath);
    bool closeSocket() noexcept;

    bool waitForEvents(int pollTimeoutMs, bool &outWoken, bool &outSocketReadable,
                       bool &outSocketHangup) noexcept;
    bool peekSocketReadable(bool &outHasBytes) noexcept;

    bool sendFrame(const waywal_ipc_hdr_t &hdr, const uint8_t *payload, size_t payloadSize,
                   int attachedFd, int timeoutMs = 1000) noexcept;

    bool recvFrame(waywal_ipc_hdr_t &outHdr, std::vector<uint8_t> &outPayload, int outFds[],
                   size_t maxFds, size_t &outNumFds, size_t maxPayloadBytes,
                   int timeoutMs = 500) noexcept;

    static int pollFdWithDeadline(int fd, short events,
                                  std::chrono::steady_clock::time_point deadline,
                                  short *outRevents = nullptr) noexcept;
    static bool sleepWakeupWithDeadline(int wakeupFd, int durationMs) noexcept;
    static bool readExactNonBlocking(int fd, void *buf, size_t count, int timeoutMs) noexcept;
    static bool writeExactNonBlocking(int fd, const void *buf, size_t count, int timeoutMs) noexcept;

  private:
    bool recvFrameHeaderAndFds(waywal_ipc_hdr_t &outHdr, int outFds[], size_t maxFds,
                               size_t &outNumFds,
                               std::chrono::steady_clock::time_point deadline) noexcept;

    int m_socketFd{-1};
    int m_wakeupFd{-1};
    std::atomic<bool> m_connected{false};
};

/* Thread-safe outbound queue, 144 Hz uniform coalescer, and FIFO RPC timeout tracker. */
class IpcDispatcher {
  public:
    static constexpr int UNIFORM_FLUSH_INTERVAL_MS = 7;

    IpcDispatcher() noexcept = default;

    void enqueuePacket(IpcOutboundPacket pkt);
    bool coalesceUniform(const std::string &key, const waywal_set_uniform_payload_t &payload);

    [[nodiscard]] bool hasPendingOutbound() const;
    [[nodiscard]] bool hasPendingRequests() const;

    void takeFlushBatch(std::deque<IpcOutboundPacket> &outPackets,
                        std::vector<waywal_set_uniform_payload_t> &outUniforms,
                        std::chrono::steady_clock::time_point now);

    void recordSentUniform() noexcept;
    void recordSentSubscribe() noexcept;
    void recordSentRequest(IpcOutboundPacket &pkt, uint16_t reqOpcode,
                           std::chrono::steady_clock::time_point now);

    bool popPendingForOpcode(uint16_t reqOpcode, IpcPendingRequest &outReq);
    bool popFrontForError(IpcPendingRequest &outReq);
    bool popNextGenericOk(IpcPendingRequest &outReq);

    [[nodiscard]] bool isAwaitingSubscribeAck() const noexcept;
    bool tryConsumeSubscribeAck() noexcept;
    bool tryConsumeUnackedUniform() noexcept;

    void collectExpiredRequests(int timeoutMs, std::vector<IpcPendingRequest> &outExpired,
                                std::chrono::steady_clock::time_point now);

    void drainAllOnDisconnect(std::deque<IpcPendingRequest> &outPending,
                              std::deque<IpcOutboundPacket> &outUnsent);

  private:
    static constexpr size_t MAX_TRACKED_UNIFORM_ACKS = 512;

    uint64_t nextSendSeq() noexcept { return ++m_sendSeq; }
    void pruneUnackedUniformsBefore(uint64_t seq) noexcept;

    mutable std::mutex m_queueMutex;
    std::deque<IpcOutboundPacket> m_outQueue;
    std::map<std::string, waywal_set_uniform_payload_t> m_pendingUniforms;
    std::chrono::steady_clock::time_point m_lastUniformFlush{};

    mutable std::mutex m_requestsMutex;
    std::deque<IpcPendingRequest> m_pendingRequests;
    std::deque<uint64_t> m_unackedUniformSeqs;
    std::atomic<bool> m_awaitingSubscribeAck{false};
    std::atomic<uint64_t> m_sendSeq{0};
};

class IpcClient : public QObject, public IIpcClientService {
    Q_OBJECT

  public:
    static constexpr int DEFAULT_REQUEST_TIMEOUT_MS = 15000;

    explicit IpcClient(QString namespaceStr = QString(), QString socketPath = QString(),
                       QObject *parent = nullptr, bool autoStart = true);
    ~IpcClient() override;

    void start();
    void stop();

    void setSocketPath(const QString &socketPath);
    void setRequestTimeoutMs(int timeoutMs);
    [[nodiscard]] int requestTimeoutMs() const noexcept { return m_requestTimeoutMs.load(); }

    bool isConnected() const override;
    QString socketPath() const;
    [[nodiscard]] QString namespaceStr() const { return m_namespaceStr; }

    void sendSetUniform(const QString &output, const QString &name, const float *vals,
                        uint32_t count, bool persist = false) override;
    void sendSetImage(const waywal_img_metadata_t &meta, int memfd = -1) override;
    void sendSetImageWithExtra(const waywal_img_metadata_t &meta, const QStringList &targetOutputs,
                               const QString &customShaderGlsl = {}, int memfd = -1) override;
    void setWallpaper(const WallpaperApplyOptions &opts) override;
    void sendAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                              uint8_t batteryPolicy = 0) override;
    void requestOutputsJson(std::function<void(const QString &json)> callback = nullptr) override;
    void
    requestTelemetry(std::function<void(const TelemetrySnapshot &snapshot)> callback = nullptr) override;
    void requestOutputDmabuf(
        const QString &output,
        std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)>
            callback = nullptr) override;
    void subscribeEvents() override;

    void requestWorkshopSync(
        uint32_t flags, const QString &customRoot = QString(),
        std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> callback = nullptr);
    void requestWorkshopDownload(
        uint64_t publishedFileId, uint32_t priority = 50, uint32_t preferredBackend = 0u,
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
        const QString &targetPath, uint32_t autoRepairFlags = 0u,
        std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback = nullptr);

    bool requestCompileShaderGraph(const QString &output, const QString &path,
                                   const QString &inlineJson, uint32_t flags,
                                   waywal_ipc_resp_compile_shader_graph_t *outResp = nullptr);
    bool sendSetCamera3D(const QString &output, const we_camera3d_t &cam);
    bool sendSetPhysicsParams(const QString &output, int32_t objectId, int32_t boneIndex,
                              const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                              const we_cursor_force_t &cursor);
    bool sendSetSceneLights(const QString &output, const we_lighting_uniforms_t &lights);

    bool sendSceneMutate(const waywal_ipc_req_scene_mutate_t &req,
                         const QString &payloadJson = QString());
    bool sendHotReloadCode(const waywal_ipc_req_hot_reload_code_t &req,
                           const QByteArray &utf8Source);
    bool sendQuerySceneTree(const QString &output = QString(), uint32_t flags = 0u);
    bool sendSetShaderCombo(const QString &output, int32_t objectId, uint32_t passIndex,
                            const QString &comboName, int32_t comboValue, uint32_t flags = 0u);
    bool sendBrushStroke(const waywal_ipc_req_brush_stroke_t &req,
                         const std::vector<we_brush_dab_t> &dabs);
    bool sendParticleConfig(const QString &output, int32_t objectId, uint32_t configOp,
                            uint32_t presetId, uint32_t flags, const void *payload,
                            uint32_t payloadLen);
    bool sendAudioBindings(const QString &output, uint32_t bindOp,
                           const we_audio_binding_t *bindings, uint32_t count,
                           uint32_t removeId = 0u);
    bool sendExportScenePkg(const QString &output, const QString &destPath, uint32_t flags = 0u);
    bool sendLintScenescript(const QString &sourceText, uint32_t flags = 0u);
    bool sendReadbackTexture(const QString &output, int32_t objectId, uint32_t passIndex,
                             uint32_t texSlot, uint32_t formatMode = 0u);

    using PuppetAnimTrackData = waywal::studio::PuppetAnimTrackData;
    using PuppetAnimClipData = waywal::studio::PuppetAnimClipData;

    [[nodiscard]] static QString stagePuppetMdlExportPath(const QString &fileHint = QString());
    bool sendPuppetSetMesh(uint32_t objectId, uint32_t flags,
                           const std::vector<waywal_ipc_mdl_vertex_t> &vertices,
                           const std::vector<uint16_t> &indices);
    bool sendPuppetSetMesh(uint32_t objectId, const std::vector<we_mdl_vertex_t> &vertices,
                           const std::vector<uint16_t> &indices, uint32_t flags = 1u);
    bool sendPuppetSetSkeleton(uint32_t objectId, uint32_t flags,
                               const std::vector<waywal_ipc_mdl_bone_t> &bones);
    bool sendPuppetSetSkeleton(const waywal_ipc_puppet_set_skeleton_t &req,
                               const std::vector<waywal_ipc_mdl_bone_t> &bones);
    bool sendPuppetSetSkeleton(uint32_t objectId,
                               const std::vector<waywal_ipc_mdl_bone_t> &bones,
                               uint32_t flags = 0u);
    bool sendPuppetSolveWeights(const waywal_ipc_puppet_solve_weights_t &req,
                                const std::vector<uint64_t> &lockedVertexBitset = {});
    bool sendPuppetPaintWeights(uint32_t objectId, uint32_t flags,
                                const std::vector<waywal_ipc_vertex_weight_t> &entries);
    bool sendPuppetHeatmapMode(uint32_t objectId, bool enabled, uint16_t palette,
                               uint32_t activeBoneIdx, float opacity);
    bool sendPuppetHeatmapMode(const waywal_ipc_puppet_heatmap_mode_t &req);
    bool sendPuppetHeatmapMode(uint32_t objectId, bool enabled, uint32_t activeBoneIdx,
                               float opacity, we_heatmap_palette_t palette);
    bool sendPuppetPreviewIk(const waywal_ipc_puppet_preview_ik_t &req,
                             const std::vector<waywal_ipc_bone_trs_t> &overrides = {});
    bool sendPuppetSetAnimTracks(uint32_t objectId, uint32_t flags,
                                 const std::vector<PuppetAnimClipData> &clips);
    bool sendPuppetScrubTime(uint32_t objectId, float timeSec, bool pausePlayback,
                             uint32_t flags = 0u);
    bool sendExportPuppetMdl(uint32_t objectId, uint32_t flags,
                             const QString &outPath = QString());

    [[nodiscard]] static bool expectsGenericOkForOpcode(uint16_t reqOpcode) noexcept;

  signals:
    void connectionStateChanged(bool connected);
    void telemetryReceived(const TelemetrySnapshot &snapshot);
    void outputsJsonReceived(const QString &json);
    void eventReceived(const QString &eventType, const QJsonObject &payload);
    void dmabufReceived(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds);
    void errorOccurred(const QString &message);
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
                                 const QByteArray &rgbaBytes);

    void puppetMeshAckReceived(const waywal_ipc_puppet_mesh_ack_t &resp);
    void puppetWeightsSolved(const waywal_ipc_puppet_weights_resp_t &resp,
                             const std::vector<waywal_ipc_vertex_weight_t> &entries);
    void puppetIkPoseReceived(const waywal_ipc_puppet_ik_pose_resp_t &resp,
                              const std::vector<waywal_ipc_bone_trs_t> &bones);
    void puppetMdlExported(const waywal_ipc_export_puppet_mdl_resp_t &resp,
                           const QByteArray &mdlBytes);

  private:
    void workerLoop();
    void closeAndFailPending(const QString &reason);
    void flushOutboundQueue();
    void handleIncomingPacket();
    void sweepExpiredRequests();
    void dispatchRequestFailure(IpcPendingRequest req, const QString &reason, bool emitError);
    void dispatchUnsubmittedPacketFailure(IpcOutboundPacket pkt, const QString &reason,
                                          bool emitError);
    void enqueueAndWake(IpcOutboundPacket pkt);
    bool enqueueRawStruct(uint16_t opcode, const void *data, size_t size);

    QString m_namespaceStr;
    QString m_customSocketPath;
    QString m_resolvedSocketPath;
    mutable std::mutex m_stateMutex;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_destroying{false};
    std::atomic<int> m_requestTimeoutMs{DEFAULT_REQUEST_TIMEOUT_MS};
    std::thread m_workerThread;

    IpcTransport m_transport;
    IpcDispatcher m_dispatcher;
};

} // namespace waywal::studio

Q_DECLARE_METATYPE(waywal::studio::TelemetrySnapshot)
Q_DECLARE_METATYPE(waywal_export_dmabuf_resp_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_workshop_sync_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_workshop_download_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_wwpkg_install_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_scan_compat_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_compile_shader_graph_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_set_camera_3d_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_set_physics_params_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_set_scene_lights_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_scene_mutate_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_hot_reload_code_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_query_scene_tree_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_set_shader_combo_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_brush_stroke_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_particle_config_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_set_audio_bindings_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_export_scene_pkg_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_lint_scenescript_t)
Q_DECLARE_METATYPE(waywal_ipc_resp_readback_texture_t)
Q_DECLARE_METATYPE(waywal_ipc_puppet_mesh_ack_t)
Q_DECLARE_METATYPE(waywal_ipc_puppet_weights_resp_t)
Q_DECLARE_METATYPE(std::vector<waywal_ipc_vertex_weight_t>)
Q_DECLARE_METATYPE(waywal_ipc_puppet_ik_pose_resp_t)
Q_DECLARE_METATYPE(std::vector<waywal_ipc_bone_trs_t>)
Q_DECLARE_METATYPE(waywal_ipc_export_puppet_mdl_resp_t)

#endif // WAYWAL_STUDIO_IPC_CLIENT_HPP
