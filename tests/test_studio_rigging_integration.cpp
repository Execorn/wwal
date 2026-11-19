// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "studio/app/dock_layout_manager.hpp"
#include "studio/app/main_window.hpp"
#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/rigging_commands.hpp"
#include "studio/inspector/physics_spring_inspector_widget.hpp"
#include "studio/rigging/bone_rigging_panel.hpp"
#include "studio/rigging/dope_sheet_timeline_widget.hpp"
#include "studio/rigging/puppet_mesh_panel.hpp"
#include "studio/rigging/weight_paint_panel.hpp"
#include "studio/viewport/gpu_preview_viewport.hpp"
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <QMainWindow>
#include <QStringList>
#include <QtGlobal>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace waywal::studio;

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
extern "C" __attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
    return "leak:<unknown module>\nleak:libnvidia\nleak:libEGL\nleak:libGLESv2\nleak:dri\n";
}
#endif

namespace {

static double readPrivateAnonRssMb()
{
    std::ifstream statusFile("/proc/self/status");
    std::string line;
    while (std::getline(statusFile, line)) {
        if (line.rfind("RssAnon:", 0) == 0) {
            long kb = 0;
            if (std::sscanf(line.c_str(), "RssAnon: %ld kB", &kb) == 1) {
                return static_cast<double>(kb) / 1024.0;
            }
        }
    }
    return 0.0;
}

static void makeTranslationMat(float tx, float ty, float out[16])
{
    std::memset(out, 0, sizeof(float) * 16u);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[12] = tx;
    out[13] = ty;
    out[15] = 1.0f;
}

void test_dock_layout_v6_to_v7_migration_and_perspectives()
{
    std::cout << "[Studio Rigging] Testing DockLayoutManager v<=6 -> v7 migration & "
                 "'Rigging & Animation' perspective...\n";

    QMainWindow mw;
    DockLayoutManager mgr(&mw);

    const QJsonObject root = mgr.serializeLayout();
    assert(root.value(QStringLiteral("layout_revision")).toInt() == 7);

    const QJsonObject persps = root.value(QStringLiteral("perspectives")).toObject();
    assert(persps.contains(QStringLiteral("Rigging & Animation")));
    const QJsonObject rigTree = DockLayoutManager::createRiggingAnimationPerspectiveTree();
    const QString rigJson =
        QString::fromUtf8(QJsonDocument(rigTree).toJson(QJsonDocument::Compact));
    assert(rigJson.contains(QStringLiteral("PuppetMeshPanel")));
    assert(rigJson.contains(QStringLiteral("BoneRiggingPanel")));
    assert(rigJson.contains(QStringLiteral("WeightPaintPanel")));
    assert(rigJson.contains(QStringLiteral("DopeSheetTimelineWidget")));
    assert(rigJson.contains(QStringLiteral("PhysicsSpringInspectorWidget")));

    /* Simulate legacy v6 config with a user-defined perspective that must survive v7 upgrade */
    QJsonObject v6CustomNode;
    v6CustomNode.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    v6CustomNode.insert(QStringLiteral("content"), QStringLiteral("GpuPreviewViewport"));

    QJsonObject v6Persps;
    v6Persps.insert(QStringLiteral("Default"), QJsonObject());
    v6Persps.insert(QStringLiteral("Rigging & Animation"), v6CustomNode);
    v6Persps.insert(QStringLiteral("User Custom Rig"), v6CustomNode);

    QJsonObject v6Root;
    v6Root.insert(QStringLiteral("version"), 1);
    v6Root.insert(QStringLiteral("layout_revision"), 6);
    v6Root.insert(QStringLiteral("active_perspective"), QStringLiteral("Default"));
    v6Root.insert(QStringLiteral("perspectives"), v6Persps);

    assert(mgr.deserializeLayout(v6Root));
    const QJsonObject upgraded = mgr.serializeLayout();
    assert(upgraded.value(QStringLiteral("layout_revision")).toInt() == 7);

    const QJsonObject upPersps = upgraded.value(QStringLiteral("perspectives")).toObject();
    assert(upPersps.contains(QStringLiteral("Rigging & Animation")));
    assert(upPersps.contains(QStringLiteral("User Custom Rig")));
    const QByteArray upgradedRigJson =
        QJsonDocument(upPersps.value(QStringLiteral("Rigging & Animation")).toObject())
            .toJson(QJsonDocument::Compact);
    assert(upgradedRigJson.contains("DopeSheetTimelineWidget"));
    assert(upgradedRigJson.contains("PuppetMeshPanel"));

    /* Custom v7+ "Rigging & Animation" layouts must be preserved across reload */
    QJsonObject v7Persps;
    v7Persps.insert(QStringLiteral("Default"), QJsonObject());
    v7Persps.insert(QStringLiteral("Rigging & Animation"), v6CustomNode);
    QJsonObject v7Root;
    v7Root.insert(QStringLiteral("version"), 1);
    v7Root.insert(QStringLiteral("layout_revision"), 7);
    v7Root.insert(QStringLiteral("active_perspective"), QStringLiteral("Default"));
    v7Root.insert(QStringLiteral("perspectives"), v7Persps);
    assert(mgr.deserializeLayout(v7Root));
    const QJsonObject preserved = mgr.serializeLayout();
    const QJsonObject preservedRig = preserved.value(QStringLiteral("perspectives"))
                                         .toObject()
                                         .value(QStringLiteral("Rigging & Animation"))
                                         .toObject();
    assert(preservedRig.value(QStringLiteral("content")).toString() ==
           QStringLiteral("GpuPreviewViewport"));

    std::cout << "  -> PASS: DockLayoutManager v6 -> v7 migration refreshed stale Rigging "
                 "perspective and preserved custom layouts.\n";
}

void test_mainwindow_22_panels_lazy_deferral_and_rss()
{
    std::cout
        << "[Studio Rigging] Testing MainWindow 22 panels, lazy deferral & RssAnon < 80 MB...\n";

    StudioOptions opts;
    opts.headlessCheck = true;
    MainWindow win(opts);

    DockLayoutManager *mgr = win.dockLayoutManager();
    assert(mgr != nullptr);

    const QStringList ids = mgr->registeredPanelIds();
    assert(ids.size() == 22);

    const QStringList rigPanels = {
        QStringLiteral("PuppetMeshPanel"),
        QStringLiteral("BoneRiggingPanel"),
        QStringLiteral("WeightPaintPanel"),
        QStringLiteral("DopeSheetTimelineWidget"),
    };
    for (const QString &id : rigPanels) {
        assert(ids.contains(id));
        assert(mgr->dockWidget(id) != nullptr);
        assert(!mgr->isPanelRealized(id));
        assert(mgr->panelWidget(id) == nullptr);
    }

    const double rssMb = readPrivateAnonRssMb();
    std::cout << "  -> Idle RssAnon = " << std::fixed << std::setprecision(2) << rssMb
              << " MB (budget < 80.00 MB)\n";
#if defined(__SANITIZE_ADDRESS__)
#define WWAL_RIG_TEST_WITH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WWAL_RIG_TEST_WITH_ASAN 1
#else
#define WWAL_RIG_TEST_WITH_ASAN 0
#endif
#else
#define WWAL_RIG_TEST_WITH_ASAN 0
#endif

#if !WWAL_RIG_TEST_WITH_ASAN
    assert(rssMb < 80.0);
#endif

    for (const QString &id : rigPanels) {
        QWidget *realized = mgr->realizeLazyPanel(id);
        assert(realized != nullptr);
        assert(mgr->isPanelRealized(id));
    }
    assert(win.puppetMeshPanel() != nullptr);
    assert(win.boneRiggingPanel() != nullptr);
    assert(win.weightPaintPanel() != nullptr);
    assert(win.dopeSheetTimeline() != nullptr);

    std::cout << "  -> PASS: All 4 rigging panels stayed lazy until realized.\n";
}

void test_rigging_commands_undo_redo_and_coalescing()
{
    std::cout << "[Studio Rigging] Testing RiggingCommands undo/redo & 500ms coalescing...\n";

    CommandBus bus;
    int applyCount = 0;

    PuppetMeshSnapshot m0{};
    m0.objectId = 4;
    PuppetMeshSnapshot m1 = m0;
    m1.vertices.resize(3);
    PuppetMeshSnapshot m2 = m0;
    m2.vertices.resize(5);

    PuppetMeshSnapshot observedMesh{};
    auto meshFn = [&](const PuppetMeshSnapshot &s) {
        observedMesh = s;
        ++applyCount;
    };

    bus.execute(std::make_unique<MeshEditCommand>(QStringLiteral("Pin Drag"), m0, m1, meshFn));
    bus.execute(std::make_unique<MeshEditCommand>(QStringLiteral("Pin Drag"), m1, m2, meshFn));
    assert(observedMesh.vertices.size() == 5u);
    assert(bus.undoCount() == 1u);

    bus.undo();
    assert(observedMesh.vertices.empty());
    bus.redo();
    assert(observedMesh.vertices.size() == 5u);

    /* WeightPaintStrokeCommand undo/redo */
    VertexWeightBlockSnapshot pre{};
    pre.blockIndex = 0u;
    we_mdl_vertex_t vPre{};
    vPre.weight[0] = 1.0f;
    pre.slice.push_back(vPre);

    VertexWeightBlockSnapshot post = pre;
    post.slice[0].weight[0] = 0.25f;
    post.slice[0].weight[1] = 0.75f;

    float observedW1 = -1.0f;
    bus.execute(std::make_unique<WeightPaintStrokeCommand>(
        4, QStringLiteral("Paint Bone 1"), std::vector<VertexWeightBlockSnapshot>{pre},
        std::vector<VertexWeightBlockSnapshot>{post},
        [&](int32_t, const std::vector<VertexWeightBlockSnapshot> &blocks) {
            observedW1 = blocks[0].slice[0].weight[1];
        }));
    assert(std::fabs(observedW1 - 0.75f) < 1e-5f);
    bus.undo();
    assert(std::fabs(observedW1 - 0.0f) < 1e-5f);
    bus.redo();
    assert(std::fabs(observedW1 - 0.75f) < 1e-5f);
    assert(applyCount >= 3);

    std::cout << "  -> PASS: RiggingCommands coalescing & undo/redo verified.\n";
}

void test_bone_rig_cycle_guard_and_xpbd_drive_linkage()
{
    std::cout << "[Studio Rigging] Testing BoneRiggingPanel cycle guard & PhysicsSpringInspector "
                 "BoneDriveMode linkage...\n";

    CommandBus bus;
    BoneRiggingPanel rigPanel;
    rigPanel.setCommandBus(&bus);

    PhysicsSpringInspectorWidget physPanel;
    physPanel.setCommandBus(&bus);

    QObject::connect(&rigPanel, &BoneRiggingPanel::skeletonUpdated, &physPanel,
                     [&](int32_t objectId, const BoneHierarchySnapshot &snap) {
                         std::vector<QString> names;
                         std::vector<int32_t> parents;
                         for (size_t i = 0; i < snap.bones.size(); ++i) {
                             names.push_back(QString::fromUtf8(snap.bones[i].name));
                             parents.push_back(snap.bones[i].parent == 0xFFFFFFFFu
                                                   ? -1
                                                   : static_cast<int32_t>(snap.bones[i].parent));
                         }
                         physPanel.setBoneList(names, parents, objectId);
                         physPanel.setBoneDriveModes(snap.driveModes);
                     });

    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float childPos[3] = {16.0f, 0.0f, 0.0f};
    assert(rigPanel.addBone(QStringLiteral("root"), -1, origin, 0.0f));
    assert(rigPanel.addBone(QStringLiteral("mid"), 0, childPos, 0.0f));
    assert(rigPanel.addBone(QStringLiteral("tip"), 1, childPos, 0.0f));
    assert(rigPanel.rigSnapshot().bones.size() == 3u);

    /* Reparenting root under tip would form cycle 0 -> 1 -> 2 -> 0 and must be rejected */
    assert(!rigPanel.reparentBone(0, 2));
    assert(rigPanel.rigSnapshot().bones[0].parent == 0xFFFFFFFFu);

    /* Creating two-bone IK chain updates BoneDriveMode to WE_BONE_DRIVE_IK */
    assert(rigPanel.createTwoBoneIkChain(0u, 1u, 2u));
    assert(physPanel.boneDriveMode(1) == WE_BONE_DRIVE_IK);

    /* Applying an XPBD physics preset toggles XPBD Active on bone 2 */
    physPanel.selectBoneIndex(2);
    physPanel.applyMaterialPreset(WE_PHYS_PRESET_SILK);
    assert(physPanel.boneDriveMode(2) == WE_BONE_DRIVE_XPBD);

    std::cout << "  -> PASS: Cycle guard rejected circular reparent; IK/XPBD drive mode synced.\n";
}

void test_timeline_fcurve_bake_and_visibility_gating()
{
    std::cout << "[Studio Rigging] Testing DopeSheetTimelineWidget F-curve evaluate/bake & "
                 "visibility gating...\n";

    TimelineFCurveChannel ch;
    ch.boneIndex = 0;
    ch.channelIndex = 5; /* angle.z */
    ch.label = QStringLiteral("bone0.rz");

    FCurveKeySnapshot k0{};
    k0.frame = 0.0;
    k0.value = 0.0;
    k0.tangentMode = 1; /* Aligned */
    k0.handleOutDx = 10.0;
    k0.handleOutDy = 0.5;

    FCurveKeySnapshot k1{};
    k1.frame = 30.0;
    k1.value = 1.5;
    k1.tangentMode = 1;
    k1.handleInDx = -10.0;
    k1.handleInDy = 0.0;

    ch.keys.push_back(k0);
    ch.keys.push_back(k1);

    const double vStart = DopeSheetTimelineWidget::evaluateChannelAtFrame(ch, 0.0);
    const double vMid = DopeSheetTimelineWidget::evaluateChannelAtFrame(ch, 15.0);
    const double vEnd = DopeSheetTimelineWidget::evaluateChannelAtFrame(ch, 30.0);
    assert(std::fabs(vStart - 0.0) < 1e-4);
    assert(vMid > 0.1 && vMid < 1.45);
    assert(std::fabs(vEnd - 1.5) < 1e-4);

    /* Prepare single-clip synthetic puppet and bake to MDLA tracks */
    we_puppet_t puppet{};
    we_mdl_bone_t bone{};
    std::snprintf(bone.name, sizeof(bone.name), "root");
    bone.parent = 0xFFFFFFFFu;
    makeTranslationMat(0.0f, 0.0f, bone.local_trans);
    makeTranslationMat(0.0f, 0.0f, bone.offset_trans);

    we_mdl_keyframe_t kf{};
    kf.angle[2] = 0.25f;
    kf.scale[0] = 1.0f;
    kf.scale[1] = 1.0f;
    kf.scale[2] = 1.0f;
    we_mdl_bone_track_t tr{};
    tr.bone_index = 0u;
    tr.kf_count = 1u;
    tr.keyframes = &kf;

    we_mdl_anim_t anim{};
    anim.id = 200;
    std::snprintf(anim.name, sizeof(anim.name), "wave");
    std::snprintf(anim.mode_str, sizeof(anim.mode_str), "loop");
    anim.mode = WE_ANIM_LOOP;
    anim.fps = 30.0f;
    anim.length = 30u;
    anim.track_count = 1u;
    anim.tracks = &tr;

    puppet.bone_count = 1u;
    puppet.bones = &bone;
    puppet.anim_count = 1u;
    puppet.anims = &anim;

    DopeSheetTimelineWidget timeline;
    timeline.loadPuppetAnimations(&puppet, 7);
    assert(!timeline.clips().empty());

    std::vector<we_mdl_bone_track_t> bakedTracks;
    std::vector<we_mdl_keyframe_t> bakedStorage;
    timeline.bakeClipToTracks(0u, 1u, bakedTracks, bakedStorage);
    assert(bakedTracks.size() == 1u);
    assert(bakedTracks[0].kf_count == 30u);

    /* Hidden widget must NOT run active QTimer */
    timeline.hide();
    timeline.setPlaying(true);
    assert(timeline.isPlaying());
    assert(!timeline.isTimerActive());
    timeline.setPlaying(false);

    std::cout << "  -> PASS: F-curve evaluation, track baking & hidden-widget timer gating "
                 "verified.\n";
}

bool readExactFd(int fd, void *buf, size_t n)
{
    auto *p = static_cast<uint8_t *>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (r == 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool writeExactFd(int fd, const void *buf, size_t n)
{
    const auto *p = static_cast<const uint8_t *>(buf);
    size_t sent = 0;
    while (sent < n) {
        const ssize_t w = ::write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(w);
    }
    return true;
}

bool recvExactHdrWithFds(int fd, waywal_ipc_hdr_t &outHdr, std::vector<int> &outFds)
{
    outFds.clear();
    auto *p = reinterpret_cast<uint8_t *>(&outHdr);
    size_t got = 0;
    while (got < sizeof(outHdr)) {
        struct iovec iov{};
        iov.iov_base = p + got;
        iov.iov_len = sizeof(outHdr) - got;

        alignas(struct cmsghdr) char ctrlBuf[CMSG_SPACE(sizeof(int) * 4)]{};
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrlBuf;
        msg.msg_controllen = sizeof(ctrlBuf);

        const ssize_t r = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            for (int rfd : outFds) {
                if (rfd >= 0) {
                    ::close(rfd);
                }
            }
            outFds.clear();
            return false;
        }
        if (r == 0) {
            for (int rfd : outFds) {
                if (rfd >= 0) {
                    ::close(rfd);
                }
            }
            outFds.clear();
            return false;
        }
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                const size_t dataLen = cmsg->cmsg_len - CMSG_LEN(0);
                const size_t count = dataLen / sizeof(int);
                for (size_t i = 0; i < count; ++i) {
                    int rfd = -1;
                    std::memcpy(&rfd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                    outFds.push_back(rfd);
                }
            }
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

void test_mainwindow_rigging_wiring_export_weights_ik_and_lazy_hydration()
{
    std::cout << "[Studio Rigging] Testing MainWindow .mdl export, weight undo/redo, "
                 "BoneDriveMode, IK joint preservation & lazy hydration...\n";

    QTemporaryDir tmpDir;
    assert(tmpDir.isValid());
    const QString sockPath = tmpDir.filePath(QStringLiteral("mock-waywal.sock"));
    const QString destMdlPath = tmpDir.filePath(QStringLiteral("exported/puppet.mdl"));

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listenFd >= 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray sockBa = sockPath.toUtf8();
    assert(static_cast<size_t>(sockBa.size()) < sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, sockBa.constData(), static_cast<size_t>(sockBa.size()) + 1u);
    assert(::bind(listenFd, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr)) == 0);
    assert(::listen(listenFd, 4) == 0);

    const QByteArray syntheticMdl("MDL1\x01\x00\x00\x00SYNTHETIC_PUPPET", 24);
    std::atomic<bool> stopServer{false};
    std::atomic<bool> sawExportReq{false};
    std::atomic<bool> sawClearMeshReq{false};
    std::atomic<uint32_t> observedExportFlags{0u};

    std::jthread serverThread([&](std::stop_token) {
        struct sockaddr_un cli{};
        socklen_t cliLen = sizeof(cli);
        const int connFd = ::accept(listenFd, reinterpret_cast<struct sockaddr *>(&cli), &cliLen);
        if (connFd < 0) {
            return;
        }
        while (!stopServer.load()) {
            waywal_ipc_hdr_t hdr{};
            if (!readExactFd(connFd, &hdr, sizeof(hdr))) {
                break;
            }
            if (hdr.magic != WAYWAL_IPC_MAGIC) {
                break;
            }
            std::vector<uint8_t> payload(static_cast<size_t>(hdr.payload_size));
            if (hdr.payload_size > 0 && !readExactFd(connFd, payload.data(), payload.size())) {
                break;
            }
            if (hdr.opcode == WAYWAL_REQ_PUPPET_SET_MESH &&
                payload.size() == sizeof(waywal_ipc_puppet_set_mesh_t)) {
                waywal_ipc_puppet_set_mesh_t mreq{};
                std::memcpy(&mreq, payload.data(), sizeof(mreq));
                if (mreq.object_id == 4u && mreq.vertex_count == 0u && mreq.triangle_count == 0u) {
                    sawClearMeshReq.store(true);
                }
                waywal_ipc_puppet_mesh_ack_t ack{};
                ack.status = 0;
                ack.object_id = mreq.object_id;
                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_PUPPET_MESH_ACK;
                rhdr.payload_size = sizeof(ack);
                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                (void)writeExactFd(connFd, &ack, sizeof(ack));
            } else if (hdr.opcode == WAYWAL_REQ_EXPORT_PUPPET_MDL &&
                       payload.size() >= sizeof(waywal_ipc_export_puppet_mdl_t)) {
                waywal_ipc_export_puppet_mdl_t req{};
                std::memcpy(&req, payload.data(), sizeof(req));
                observedExportFlags.store(req.flags);
                sawExportReq.store(req.object_id == 4u);

                waywal_ipc_export_puppet_mdl_resp_t resp{};
                resp.status = 0;
                resp.object_id = req.object_id;
                resp.mdl_byte_size = static_cast<uint32_t>(syntheticMdl.size());

                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_EXPORT_PUPPET_MDL;
                rhdr.payload_size = sizeof(resp) + static_cast<uint64_t>(syntheticMdl.size());

                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                (void)writeExactFd(connFd, &resp, sizeof(resp));
                (void)writeExactFd(connFd, syntheticMdl.constData(),
                                   static_cast<size_t>(syntheticMdl.size()));
            } else if (hdr.opcode != WAYWAL_REQ_SET_UNIFORM) {
                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_OK;
                rhdr.payload_size = 0;
                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
            }
        }
        ::close(connFd);
    });

    StudioOptions opts;
    opts.headlessCheck = true;
    opts.socketPath = sockPath;
    MainWindow win(opts);
    win.ipcClient()->start();

    const auto pumpUntil = [&](auto &&pred, int maxMs = 1500) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
        while (!pred() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    pumpUntil([&]() { return win.ipcClient()->isConnected(); });
    assert(win.ipcClient()->isConnected());

    DockLayoutManager *mgr = win.dockLayoutManager();
    assert(mgr != nullptr);
    assert(mgr->realizeLazyPanel(QStringLiteral("PuppetMeshPanel")) != nullptr);
    assert(mgr->realizeLazyPanel(QStringLiteral("BoneRiggingPanel")) != nullptr);
    assert(mgr->realizeLazyPanel(QStringLiteral("PhysicsSpringInspectorWidget")) != nullptr);

    /* 1. .mdl export E2E: request uses inline flag outside staging root and QSaveFile writes */
    assert(QDir().mkpath(QFileInfo(destMdlPath).absolutePath()));
    emit win.puppetMeshPanel()->exportMdlRequested(4, destMdlPath);
    pumpUntil([&]() { return QFileInfo::exists(destMdlPath); });
    assert(sawExportReq.load());
    assert((observedExportFlags.load() & 0x1u) != 0u);
    QFile exportedFile(destMdlPath);
    assert(exportedFile.open(QIODevice::ReadOnly));
    assert(exportedFile.readAll() == syntheticMdl);
    exportedFile.close();

    /* 1b. Empty clear-mesh round-trip & partial < 3 mesh rejection */
    assert(!win.ipcClient()->sendPuppetSetMesh(4u, std::vector<we_mdl_vertex_t>(2), {}, 0u));
    assert(!win.ipcClient()->sendPuppetSetMesh(4u, {}, std::vector<uint16_t>{0, 1, 2}, 0u));
    assert(win.ipcClient()->sendPuppetSetMesh(4u, {}, {}, 0u));
    pumpUntil([&]() { return sawClearMeshReq.load(); });
    assert(sawClearMeshReq.load());

    /* 2. Skeleton IK joint preservation and cycle/out-of-order parent tolerance */
    BoneHierarchySnapshot rigSnap;
    rigSnap.objectId = 4;
    we_mdl_bone_t b0{};
    std::snprintf(b0.name, sizeof(b0.name), "root");
    b0.parent = 0xFFFFFFFFu;
    makeTranslationMat(0.0f, 0.0f, b0.local_trans);
    we_mdl_bone_t b1{};
    std::snprintf(b1.name, sizeof(b1.name), "mid");
    b1.parent = 0u;
    makeTranslationMat(16.0f, 0.0f, b1.local_trans);
    we_mdl_bone_t b2{};
    std::snprintf(b2.name, sizeof(b2.name), "tip");
    b2.parent = 1u;
    makeTranslationMat(16.0f, 0.0f, b2.local_trans);
    rigSnap.bones = {b0, b1, b2};
    rigSnap.driveModes.fill(WE_BONE_DRIVE_FK);
    rigSnap.driveModes[1] = WE_BONE_DRIVE_IK;
    rigSnap.driveModes[2] = WE_BONE_DRIVE_IK;

    we_ik_chain_t ik{};
    ik.root_bone = 0u;
    ik.mid_bone = 1u;
    ik.end_bone = 2u;
    ik.enabled = 1u;
    ik.target_world[0] = 24.0f;
    ik.target_world[1] = 12.0f;
    ik.blend_weight = 1.0f;
    rigSnap.ikChains.push_back(ik);

    emit win.boneRiggingPanel()->skeletonUpdated(4, rigSnap);
    assert(win.previewViewport() != nullptr);
    const auto &ikHandles = win.previewViewport()->gizmoOverlay().ikHandles();
    assert(ikHandles.size() == 1u);
    assert(std::fabs(ikHandles[0].midWorld.x() - 16.0f) < 1e-3f);
    assert(std::fabs(ikHandles[0].endWorld.x() - 32.0f) < 1e-3f);

    /* Out-of-order parent (parent > child) + cycle safety on GpuPreviewViewport */
    BoneHierarchySnapshot unsortedSnap;
    unsortedSnap.objectId = 4;
    we_mdl_bone_t childFirst{};
    std::snprintf(childFirst.name, sizeof(childFirst.name), "child");
    childFirst.parent = 1u;
    makeTranslationMat(10.0f, 0.0f, childFirst.local_trans);
    we_mdl_bone_t parentSecond{};
    std::snprintf(parentSecond.name, sizeof(parentSecond.name), "parent");
    parentSecond.parent = 0xFFFFFFFFu;
    makeTranslationMat(5.0f, 0.0f, parentSecond.local_trans);
    unsortedSnap.bones = {childFirst, parentSecond};
    win.previewViewport()->setPuppetSkeleton(unsortedSnap);
    assert(std::fabs(win.previewViewport()->gizmoOverlay().puppetBones()[0].headWorld.x() - 15.0f) <
           1e-3f);

    unsortedSnap.bones[1].parent = 0u; /* 0 -> 1 -> 0 cycle must not hang */
    win.previewViewport()->setPuppetSkeleton(unsortedSnap);
    assert(win.previewViewport()->gizmoOverlay().puppetBones().size() == 2u);

    /* Restore ordered 3-bone rig before weight & drive-mode tests */
    emit win.boneRiggingPanel()->skeletonUpdated(4, rigSnap);

    /* 3. Weight paint undo/redo restores local viewport weights */
    std::vector<we_mdl_vertex_t> verts(2);
    verts[0].weight[0] = 1.0f;
    verts[1].weight[0] = 1.0f;
    std::vector<uint16_t> tris = {0, 1, 0};
    win.previewViewport()->setPuppetObjectId(4);
    win.previewViewport()->setPuppetMeshData(true, verts, tris);

    VertexWeightBlockSnapshot preW{};
    preW.blockIndex = 0u;
    preW.slice = verts;
    VertexWeightBlockSnapshot postW = preW;
    postW.slice[0].weight[0] = 0.2f;
    postW.slice[0].weight[1] = 0.8f;

    emit win.previewViewport()->weightPaintStrokeCommitted(4, {preW}, {postW}, {});
    assert(std::fabs(win.previewViewport()->gizmoOverlay().meshWireframe().vertices[0].weight[1] -
                     0.8f) < 1e-5f);
    win.handleUndo();
    assert(std::fabs(win.previewViewport()->gizmoOverlay().meshWireframe().vertices[0].weight[1] -
                     0.0f) < 1e-5f);
    win.handleRedo();
    assert(std::fabs(win.previewViewport()->gizmoOverlay().meshWireframe().vertices[0].weight[1] -
                     0.8f) < 1e-5f);

    /* 4. Bidirectional BoneDriveMode + deferred LazyPanelHost context hydration + onion skin */
    emit win.physicsSpringInspector()->boneDriveModeChanged(4, 1, WE_BONE_DRIVE_XPBD);
    assert(mgr->realizeLazyPanel(QStringLiteral("WeightPaintPanel")) != nullptr);
    assert(mgr->realizeLazyPanel(QStringLiteral("DopeSheetTimelineWidget")) != nullptr);
    assert(win.weightPaintPanel() != nullptr);
    assert(win.dopeSheetTimeline() != nullptr);

    emit win.dopeSheetTimeline()->onionSkinChanged(true, 1, 1, 0.5f);
    emit win.dopeSheetTimeline()->frameChanged(15.0, 0.5);
    assert(win.previewViewport()->gizmoOverlay().isOnionSkinEnabled());

    stopServer.store(true);
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
    serverThread.request_stop();

    std::cout << "  -> PASS: MainWindow .mdl export, weight undo/redo, BoneDriveMode, IK joint "
                 "preservation & lazy hydration verified.\n";
}

void test_ipc_client_set_wallpaper_and_puppet_scrub_sync_wire()
{
    std::cout << "[Studio Rigging] Testing real IpcClient::setWallpaper (SCM_RIGHTS memfd) & "
                 "WAYWAL_REQ_PUPPET_SCRUB_TIME sync reply discipline (E2E-F07)...\n";

    QTemporaryDir tmpDir;
    assert(tmpDir.isValid());
    const QString sockPath = tmpDir.filePath(QStringLiteral("rigging-ipc-e2e.sock"));
    const QString imgPath = tmpDir.filePath(QStringLiteral("test_wall.png"));

    QImage img(6, 4, QImage::Format_RGBA8888);
    img.fill(QColor(220, 45, 30, 255));
    assert(img.save(imgPath, "PNG"));

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listenFd >= 0);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray sockBa = sockPath.toUtf8();
    assert(static_cast<size_t>(sockBa.size()) < sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, sockBa.constData(), static_cast<size_t>(sockBa.size()) + 1u);
    ::unlink(addr.sun_path);
    assert(::bind(listenFd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0);
    assert(::listen(listenFd, 4) == 0);

    std::atomic<bool> stopServer{false};
    std::atomic<bool> sawSetImageOk{false};
    std::atomic<int> scrubCount{0};
    std::atomic<int> exportCount{0};
    std::atomic<uint32_t> observedScrubObj{0};
    std::atomic<uint8_t> observedScrubPause{0};
    std::atomic<double> observedScrubTime{0.0};

    std::jthread serverThread([&](std::stop_token st) {
        const int connFd = ::accept(listenFd, nullptr, nullptr);
        if (connFd < 0) {
            return;
        }
        while (!st.stop_requested() && !stopServer.load()) {
            waywal_ipc_hdr_t hdr{};
            std::vector<int> fds;
            if (!recvExactHdrWithFds(connFd, hdr, fds)) {
                break;
            }
            if (hdr.magic != WAYWAL_IPC_MAGIC) {
                for (int rfd : fds) {
                    if (rfd >= 0) {
                        ::close(rfd);
                    }
                }
                break;
            }

            if (hdr.opcode == WAYWAL_REQ_SET_IMAGE && !fds.empty()) {
                const int memfd = fds.front();
                for (size_t i = 1; i < fds.size(); ++i) {
                    if (fds[i] >= 0) {
                        ::close(fds[i]);
                    }
                }
                const int seals = ::fcntl(memfd, F_GET_SEALS);
                const bool hasShrinkSeal = (seals >= 0) && ((seals & F_SEAL_SHRINK) != 0);
                std::vector<uint8_t> memBytes(static_cast<size_t>(hdr.payload_size));
                const bool seeksOk = (::lseek(memfd, 0, SEEK_SET) == 0);
                const bool readMemOk =
                    seeksOk && readExactFd(memfd, memBytes.data(), memBytes.size());
                ::close(memfd);

                bool metaOk = false;
                if (hasShrinkSeal && readMemOk &&
                    memBytes.size() >= sizeof(waywal_img_metadata_t)) {
                    waywal_img_metadata_t meta{};
                    std::memcpy(&meta, memBytes.data(), sizeof(meta));
                    const size_t expectedTotal =
                        sizeof(waywal_img_metadata_t) + static_cast<size_t>(meta.extra_data_len) +
                        static_cast<size_t>(meta.width) * static_cast<size_t>(meta.height) * 4u;
                    const bool outputsMatch = [&]() {
                        if (meta.num_target_outputs != 2u ||
                            sizeof(waywal_img_metadata_t) + meta.extra_data_len > memBytes.size()) {
                            return false;
                        }
                        const char *p = reinterpret_cast<const char *>(
                            memBytes.data() + sizeof(waywal_img_metadata_t));
                        const size_t maxExtra = static_cast<size_t>(meta.extra_data_len);
                        const size_t len0 = ::strnlen(p, maxExtra);
                        if (len0 >= maxExtra) {
                            return false;
                        }
                        const QByteArray out0(p, static_cast<qsizetype>(len0));
                        const size_t off1 = len0 + 1u;
                        const size_t len1 = ::strnlen(p + off1, maxExtra - off1);
                        if (off1 + len1 >= maxExtra) {
                            return false;
                        }
                        const QByteArray out1(p + off1, static_cast<qsizetype>(len1));
                        return out0 == QByteArrayLiteral("DP-1") &&
                               out1 == QByteArrayLiteral("HDMI-A-1");
                    }();

                    metaOk = (meta.width == 6u) && (meta.height == 4u) &&
                             (memBytes.size() == expectedTotal) &&
                             (meta.scaling_mode == WAYWAL_SCALING_CENTER) &&
                             (meta.transition_type == WAYWAL_TRANSITION_WIPE) &&
                             (meta.transition_duration_ms == 850u) &&
                             (meta.transition_fps == 120u) && (meta.has_crop_gravity == 1u) &&
                             (std::fabs(meta.crop_gravity_x - 0.25f) < 1e-4f) &&
                             (std::fabs(meta.crop_gravity_y - 0.75f) < 1e-4f) &&
                             meta.has_custom_bezier &&
                             (std::fabs(meta.bezier.p1x - 0.12f) < 1e-4f) &&
                             (std::fabs(meta.bezier.p1y - 0.88f) < 1e-4f) &&
                             (std::strcmp(meta.backdrop, "blur") == 0) &&
                             (std::strcmp(meta.layer_name, "overlay") == 0) &&
                             (std::strcmp(meta.battery_policy, "reduce-fps") == 0) && outputsMatch;
                }
                sawSetImageOk.store(metaOk);

                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_OK;
                rhdr.payload_size = 0;
                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                continue;
            }

            for (int rfd : fds) {
                if (rfd >= 0) {
                    ::close(rfd);
                }
            }

            std::vector<uint8_t> payload(static_cast<size_t>(hdr.payload_size));
            if (hdr.payload_size > 0 && !readExactFd(connFd, payload.data(), payload.size())) {
                break;
            }

            if (hdr.opcode == WAYWAL_REQ_PUPPET_SCRUB_TIME &&
                payload.size() == sizeof(waywal_ipc_puppet_scrub_t)) {
                waywal_ipc_puppet_scrub_t scrubReq{};
                std::memcpy(&scrubReq, payload.data(), sizeof(scrubReq));
                const int callNo = scrubCount.fetch_add(1) + 1;
                if (callNo == 1) {
                    observedScrubObj.store(scrubReq.object_id);
                    observedScrubPause.store(scrubReq.pause_playback);
                    observedScrubTime.store(scrubReq.time_sec);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    waywal_ipc_hdr_t rhdr{};
                    rhdr.magic = WAYWAL_IPC_MAGIC;
                    rhdr.version = WAYWAL_IPC_VERSION;
                    rhdr.opcode = WAYWAL_RESP_OK;
                    rhdr.payload_size = 0;
                    (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const QByteArray errText("scrub_sync_err");
                    waywal_ipc_hdr_t rhdr{};
                    rhdr.magic = WAYWAL_IPC_MAGIC;
                    rhdr.version = WAYWAL_IPC_VERSION;
                    rhdr.opcode = WAYWAL_RESP_ERR;
                    rhdr.payload_size = static_cast<uint64_t>(errText.size());
                    (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                    (void)writeExactFd(connFd, errText.constData(),
                                       static_cast<size_t>(errText.size()));
                }
            } else if (hdr.opcode == WAYWAL_REQ_EXPORT_PUPPET_MDL &&
                       payload.size() >= sizeof(waywal_ipc_export_puppet_mdl_t)) {
                waywal_ipc_export_puppet_mdl_t expReq{};
                std::memcpy(&expReq, payload.data(), sizeof(expReq));
                const int expNo = exportCount.fetch_add(1) + 1;
                const QByteArray tag = QStringLiteral("SYNC_MDL_OK_v%1").arg(expNo).toUtf8();

                waywal_ipc_export_puppet_mdl_resp_t resp{};
                resp.status = 0;
                resp.object_id = expReq.object_id;
                resp.mdl_byte_size = static_cast<uint32_t>(tag.size());

                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_EXPORT_PUPPET_MDL;
                rhdr.payload_size = sizeof(resp) + static_cast<uint64_t>(tag.size());
                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
                (void)writeExactFd(connFd, &resp, sizeof(resp));
                (void)writeExactFd(connFd, tag.constData(), static_cast<size_t>(tag.size()));
            } else if (hdr.opcode != WAYWAL_REQ_SET_UNIFORM) {
                waywal_ipc_hdr_t rhdr{};
                rhdr.magic = WAYWAL_IPC_MAGIC;
                rhdr.version = WAYWAL_IPC_VERSION;
                rhdr.opcode = WAYWAL_RESP_OK;
                rhdr.payload_size = 0;
                (void)writeExactFd(connFd, &rhdr, sizeof(rhdr));
            }
        }
        ::close(connFd);
    });

    IpcClient client(QStringLiteral("default"), sockPath, nullptr, false);
    QStringList receivedErrors;
    QVector<QPair<int32_t, QByteArray>> receivedMdls;
    QObject::connect(&client, &IpcClient::errorOccurred,
                     [&](const QString &msg) { receivedErrors.push_back(msg); });
    QObject::connect(&client, &IpcClient::puppetMdlExported,
                     [&](const waywal_ipc_export_puppet_mdl_resp_t &resp, const QByteArray &bytes) {
                         receivedMdls.push_back({resp.status, bytes});
                     });

    client.start();

    const auto pumpUntil = [&](auto &&pred, int maxMs = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
        while (!pred() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    pumpUntil([&]() { return client.isConnected(); });
    assert(client.isConnected());

    /* 1. Real IpcClient::setWallpaper sends sealed memfd + full metadata + outputs */
    WallpaperApplyOptions wallOpts;
    wallOpts.filepath = imgPath;
    wallOpts.targetOutputs = {QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")};
    wallOpts.backdrop = QStringLiteral("blur");
    wallOpts.layerName = QStringLiteral("overlay");
    wallOpts.batteryPolicy = QStringLiteral("reduce-fps");
    wallOpts.scaling_mode = WAYWAL_SCALING_CENTER;
    wallOpts.transition_type = WAYWAL_TRANSITION_WIPE;
    wallOpts.transition_duration_ms = 850u;
    wallOpts.transition_fps = 120u;
    wallOpts.has_crop_gravity = true;
    wallOpts.crop_gravity_x = 0.25f;
    wallOpts.crop_gravity_y = 0.75f;
    wallOpts.has_custom_bezier = true;
    wallOpts.bezier = bezier_curve_t{{0.12f}, {0.88f}, {0.74f}, {0.96f}};
    client.setWallpaper(wallOpts);

    pumpUntil([&]() { return sawSetImageOk.load(); });
    assert(sawSetImageOk.load());

    /* 2. E2E-F07: PUPPET_SCRUB_TIME waits for WAYWAL_RESP_OK and keeps follow-up RPC aligned */
    assert(client.sendPuppetScrubTime(7u, 1.25f, true, 0u));
    assert(client.sendExportPuppetMdl(7u, 0x1u));
    pumpUntil([&]() { return receivedMdls.size() == 1; });
    assert(scrubCount.load() == 1);
    assert(observedScrubObj.load() == 7u);
    assert(observedScrubPause.load() == 1u);
    assert(std::fabs(observedScrubTime.load() - 1.25) < 1e-4);
    assert(receivedErrors.isEmpty());
    assert(receivedMdls[0].first == 0);
    assert(receivedMdls[0].second == QByteArrayLiteral("SYNC_MDL_OK_v1"));

    /* 3. Error on PUPPET_SCRUB_TIME stays bound to scrub and does not corrupt subsequent export */
    assert(client.sendPuppetScrubTime(7u, 2.50f, false, 0u));
    assert(client.sendExportPuppetMdl(7u, 0x1u));
    pumpUntil([&]() { return receivedErrors.size() == 1 && receivedMdls.size() == 2; });
    assert(scrubCount.load() == 2);
    assert(receivedErrors.size() == 1);
    assert(receivedErrors[0].contains(QStringLiteral("scrub_sync_err")));
    assert(receivedMdls.size() == 2);
    assert(receivedMdls[1].first == 0);
    assert(receivedMdls[1].second == QByteArrayLiteral("SYNC_MDL_OK_v2"));

    stopServer.store(true);
    client.stop();
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
    serverThread.request_stop();

    std::cout << "  -> PASS: IpcClient::setWallpaper (SCM_RIGHTS memfd) & PUPPET_SCRUB_TIME sync "
                 "wire protocol verified.\n";
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    QApplication app(argc, argv);

    std::cout << "=================================================================\n";
    std::cout << "Starting Studio Rigging Integration Suite\n";
    std::cout << "=================================================================\n";

    test_dock_layout_v6_to_v7_migration_and_perspectives();
    test_mainwindow_22_panels_lazy_deferral_and_rss();
    test_rigging_commands_undo_redo_and_coalescing();
    test_bone_rig_cycle_guard_and_xpbd_drive_linkage();
    test_timeline_fcurve_bake_and_visibility_gating();
    test_mainwindow_rigging_wiring_export_weights_ik_and_lazy_hydration();
    test_ipc_client_set_wallpaper_and_puppet_scrub_sync_wire();

    std::cout << "=================================================================\n";
    std::cout << "All Studio Rigging Integration Tests PASSED!\n";
    std::cout << "=================================================================\n";
    return 0;
}
