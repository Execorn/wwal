// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ipc_client.hpp"

#include "waywal/image_loader.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/wwpkg.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMetaType>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace waywal::studio {

namespace {

int hexNibble(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

void closeFdChecked(int &fd) noexcept
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void closeFdArray(int *fds, size_t count) noexcept
{
    if (!fds)
        return;
    for (size_t i = 0; i < count; ++i) {
        closeFdChecked(fds[i]);
    }
}

bool readExactWithDeadline(int fd, void *buf, size_t count,
                           std::chrono::steady_clock::time_point deadline) noexcept
{
    uint8_t *ptr = static_cast<uint8_t *>(buf);
    size_t total = 0;

    while (total < count) {
        const ssize_t n = ::recv(fd, ptr + total, count - total, 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            short revents = 0;
            const int pr = IpcTransport::pollFdWithDeadline(fd, POLLIN, deadline, &revents);
            if (pr > 0 && (revents & (POLLIN | POLLERR | POLLHUP)) != 0)
                continue;
            return false;
        }
        return false;
    }
    return true;
}

bool writeExactWithDeadline(int fd, const void *buf, size_t count,
                            std::chrono::steady_clock::time_point deadline) noexcept
{
    const uint8_t *ptr = static_cast<const uint8_t *>(buf);
    size_t total = 0;

    while (total < count) {
        const ssize_t n = ::send(fd, ptr + total, count - total, MSG_NOSIGNAL);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            short revents = 0;
            const int pr = IpcTransport::pollFdWithDeadline(fd, POLLOUT, deadline, &revents);
            if (pr > 0 && (revents & POLLOUT) != 0 && (revents & (POLLERR | POLLNVAL)) == 0)
                continue;
            return false;
        }
        return false;
    }
    return true;
}

struct QueuedFdGuard {
    QVector<int> fds;
    bool handedOff{false};

    ~QueuedFdGuard()
    {
        if (!handedOff) {
            for (int fd : fds) {
                if (fd >= 0)
                    ::close(fd);
            }
        }
    }
};

} // namespace

/* ========================================================================= */
/* RAII Descriptor Guards                                                    */
/* ========================================================================= */

void ScopedFd::reset(int newFd) noexcept
{
    if (m_fd >= 0 && m_fd != newFd) {
        ::close(m_fd);
    }
    m_fd = newFd;
}

ScopedFdArrayGuard::~ScopedFdArrayGuard()
{
    if (!fds)
        return;
    const size_t limit = std::min(count, MAX_ANCILLARY_FDS);
    for (size_t i = 0; i < limit; ++i) {
        closeFdChecked(fds[i]);
    }
}

/* ========================================================================= */
/* IpcServiceCodecs                                                          */
/* ========================================================================= */

bool IpcServiceCodecs::isFireAndForgetNoReplyOpcode(uint16_t reqOpcode) noexcept
{
    // Only high-rate coalesced uniform updates run without strict RPC reply tracking.
    // WAYWAL_REQ_PUPPET_SCRUB_TIME and WAYWAL_REQ_SUBSCRIBE receive explicit daemon replies
    // and must not sit here or error responses shift across unrelated in-flight RPCs.
    return reqOpcode == WAYWAL_REQ_SET_UNIFORM;
}

bool IpcServiceCodecs::expectsGenericOkForOpcode(uint16_t reqOpcode) noexcept
{
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
    default:
        return false;
    }
}

bool IpcServiceCodecs::parseUuidString(const char *str, uint8_t outUuid[16]) noexcept
{
    if (!str || !outUuid)
        return false;
    uint8_t tmp[16]{};
    size_t nibbles = 0;
    int hi = -1;
    for (const char *p = str; *p != '\0'; ++p) {
        if (*p == '-')
            continue;
        const int v = hexNibble(*p);
        if (v < 0 || nibbles >= 32)
            return false;
        if ((nibbles & 1u) == 0u) {
            hi = v;
        } else {
            tmp[nibbles / 2] = static_cast<uint8_t>((hi << 4) | v);
        }
        ++nibbles;
    }
    if (nibbles != 32)
        return false;
    std::memcpy(outUuid, tmp, 16);
    return true;
}

QString IpcServiceCodecs::uuidBytesToDashedHex(const uint8_t uuid[16])
{
    char buf[37]{};
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
                  uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
                  uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return QString::fromLatin1(buf);
}

bool IpcServiceCodecs::isAllZeroUuid(const uint8_t uuid[16]) noexcept
{
    if (!uuid)
        return true;
    uint8_t acc = 0;
    for (size_t i = 0; i < 16; ++i)
        acc |= uuid[i];
    return acc == 0;
}

int IpcServiceCodecs::dupCloexec(int fd) noexcept
{
    if (fd < 0)
        return -1;
    int duped = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0 && errno == EINVAL) {
        duped = ::dup(fd);
        if (duped >= 0) {
            const int fl = ::fcntl(duped, F_GETFD, 0);
            if (fl >= 0) {
                (void)::fcntl(duped, F_SETFD, fl | FD_CLOEXEC);
            }
        }
    }
    return duped;
}

int IpcServiceCodecs::tryOpenHubPackageByUuid(const uint8_t uuid[16])
{
    if (!uuid || isAllZeroUuid(uuid))
        return -1;

    const QString targetId = uuidBytesToDashedHex(uuid).toLower();
    QStringList dirs;
    const QString xdgHub = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                           QStringLiteral("/waywal/hub");
    if (!xdgHub.isEmpty())
        dirs << xdgHub;
    const QString homeHub = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
    if (!dirs.contains(homeHub))
        dirs << homeHub;

    for (const QString &dirStr : dirs) {
        std::error_code ec;
        const std::filesystem::path dirPath(dirStr.toStdString());
        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec))
            continue;
        for (const auto &entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".wwpkg")
                continue;
            const std::string p = entry.path().string();
            int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            wwpkg_package_t *pkg = wwpkg_open_fd(fd);
            if (!pkg) {
                ::close(fd);
                continue;
            }
            const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
            const bool matched = man && QString::fromUtf8(man->id).trimmed().toLower() == targetId;
            wwpkg_close(pkg);
            if (matched)
                return fd;
            ::close(fd);
        }
    }
    return -1;
}

QString IpcServiceCodecs::stagePuppetMdlExportPath(const QString &fileHint)
{
    QString cacheRoot = qEnvironmentVariable("XDG_CACHE_HOME");
    if (cacheRoot.isEmpty())
        cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cacheRoot.isEmpty())
        cacheRoot = QDir::homePath() + QStringLiteral("/.cache");

    const QString exportStageRoot = cacheRoot + QStringLiteral("/wwal/exports");
    if (!QDir().mkpath(exportStageRoot))
        return {};

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
    if (!waywal_ipc_export_path_is_allowed(candidateUtf8.constData(), resolved, sizeof(resolved)))
        return {};
    return QString::fromUtf8(resolved);
}

bool IpcServiceCodecs::encodeSetUniform(const QString &output, const QString &name, const float *vals,
                                        uint32_t count, bool persist,
                                        waywal_set_uniform_payload_t &outPayload, QString *outErr)
{
    if (!vals || count == 0 || count > 4u) {
        if (outErr)
            *outErr = QStringLiteral("Invalid uniform component count");
        return false;
    }

    const QByteArray outBa = output.toUtf8();
    const QByteArray nameBa = name.toUtf8();
    if (nameBa.isEmpty() ||
        static_cast<size_t>(nameBa.size()) >= sizeof(outPayload.structured.uniform_name)) {
        if (outErr)
            *outErr = QStringLiteral("Uniform name empty or exceeds 63 bytes");
        return false;
    }
    if (static_cast<size_t>(outBa.size()) >= sizeof(outPayload.structured.target_output)) {
        if (outErr)
            *outErr = QStringLiteral("Uniform target output name exceeds 31 bytes");
        return false;
    }

    std::memset(&outPayload, 0, sizeof(outPayload));
    waywal_set_uniform_structured_t &st = outPayload.structured;
    if (!outBa.isEmpty()) {
        std::memcpy(st.target_output, outBa.constData(), static_cast<size_t>(outBa.size()));
    }
    std::memcpy(st.uniform_name, nameBa.constData(), static_cast<size_t>(nameBa.size()));
    st.uniform_components = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(vals[i])) {
            if (outErr)
                *outErr = QStringLiteral("Non-finite uniform value rejected");
            return false;
        }
        st.uniform_values[i] = vals[i];
    }
    st.flags = WAYWAL_UNIFORM_FLAG_STRUCTURED | (persist ? WAYWAL_UNIFORM_FLAG_PERSIST : 0U);
    st.struct_size = sizeof(waywal_set_uniform_structured_t);

    if (!waywal_validate_set_uniform_structured(&st)) {
        if (outErr)
            *outErr = QStringLiteral("Structured uniform validation failed");
        return false;
    }
    return true;
}

bool IpcServiceCodecs::prepareSetImagePacket(const waywal_img_metadata_t &meta,
                                             const QStringList &targetOutputs,
                                             const QString &customShaderGlsl, int inMemfd,
                                             waywal_ipc_hdr_t &outHdr, ScopedFd &outMemfd,
                                             QString *outErr)
{
    auto fail = [&](const QString &msg) {
        if (outErr)
            *outErr = msg;
        return false;
    };

    if (meta.scaling_mode > 8u || meta.filter_mode > 8u || meta.transition_type > 32u ||
        meta.pixel_format > 16u) {
        return fail(QStringLiteral("SET_IMAGE enum field out of range"));
    }
    if ((meta.has_crop_gravity &&
         (!std::isfinite(meta.crop_gravity_x) || !std::isfinite(meta.crop_gravity_y))) ||
        !std::isfinite(meta.depth_strength) || !std::isfinite(meta.transition_angle_rad) ||
        !std::isfinite(meta.transition_wave_freq) || !std::isfinite(meta.transition_wave_amp) ||
        !std::isfinite(meta.transition_center_x) || !std::isfinite(meta.transition_center_y)) {
        return fail(QStringLiteral("Non-finite SET_IMAGE geometry or transition float"));
    }
    if (meta.has_custom_bezier &&
        (!std::isfinite(meta.bezier.p1x) || !std::isfinite(meta.bezier.p1y) ||
         !std::isfinite(meta.bezier.p2x) || !std::isfinite(meta.bezier.p2y))) {
        return fail(QStringLiteral("Non-finite SET_IMAGE bezier control point"));
    }
    if (meta.has_solar_coords &&
        (!std::isfinite(meta.solar_latitude) || !std::isfinite(meta.solar_longitude))) {
        return fail(QStringLiteral("Non-finite SET_IMAGE solar coordinates"));
    }

    if (inMemfd >= 0) {
        ScopedFd fdGuard(dupCloexec(inMemfd));
        if (!fdGuard.valid())
            return fail(QStringLiteral("Failed to duplicate image memfd"));

        struct stat st{};
        if (::fstat(fdGuard.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
            return fail(QStringLiteral("Provided image FD is not a regular file"));

        int seals = ::fcntl(fdGuard.get(), F_GET_SEALS);
        if (seals < 0)
            return fail(QStringLiteral("Provided image FD does not support seals"));
        if ((seals & F_SEAL_SHRINK) == 0) {
            if (::fcntl(fdGuard.get(), F_ADD_SEALS, F_SEAL_SHRINK) < 0)
                return fail(QStringLiteral("Failed to enforce F_SEAL_SHRINK on image memfd"));
        }

        if (meta.width == 0 || meta.height == 0 || meta.width > 16384u || meta.height > 16384u ||
            meta.extra_data_len > WWAL_MAX_EXTRA_IPC_BYTES) {
            return fail(QStringLiteral("SET_IMAGE dimensions or extra_data_len out of bounds"));
        }
        const uint64_t pixelBytes =
            static_cast<uint64_t>(meta.width) * static_cast<uint64_t>(meta.height) * 4ULL;
        const uint64_t expectedSize = sizeof(waywal_img_metadata_t) +
                                      static_cast<uint64_t>(meta.extra_data_len) + pixelBytes;
        if (static_cast<uint64_t>(st.st_size) < expectedSize)
            return fail(QStringLiteral("Image memfd smaller than metadata + pixel payload"));

        outHdr.magic = WAYWAL_IPC_MAGIC;
        outHdr.version = WAYWAL_IPC_VERSION;
        outHdr.opcode = WAYWAL_REQ_SET_IMAGE;
        outHdr.payload_size = expectedSize;
        outMemfd = std::move(fdGuard);
        return true;
    }

    if (meta.extra_data_len != 0 && targetOutputs.isEmpty() && customShaderGlsl.isEmpty()) {
        return fail(QStringLiteral("sendSetImage rejected extra_data_len without memfd or data"));
    }

    std::vector<QByteArray> normalizedOutputs;
    normalizedOutputs.reserve(static_cast<size_t>(targetOutputs.size()));
    size_t outputsLen = 0;
    bool allOutputs = false;
    for (const QString &rawOut : targetOutputs) {
        const QString trimmed = rawOut.trimmed();
        if (trimmed.isEmpty() || trimmed == QStringLiteral("*") ||
            trimmed.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0) {
            allOutputs = true;
            continue;
        }
        const QByteArray ba = trimmed.toUtf8();
        if (normalizedOutputs.size() >= 64u)
            return fail(QStringLiteral("Too many target outputs for SET_IMAGE"));
        outputsLen += static_cast<size_t>(ba.size()) + 1u;
        normalizedOutputs.push_back(ba);
    }
    if (allOutputs) {
        normalizedOutputs.clear();
        outputsLen = 0;
    }

    const QByteArray shaderBa = customShaderGlsl.toUtf8();
    const size_t shaderLen = shaderBa.isEmpty() ? 0u : static_cast<size_t>(shaderBa.size()) + 1u;
    const size_t extraBytes = outputsLen + shaderLen;
    if (extraBytes > WWAL_MAX_EXTRA_IPC_BYTES)
        return fail(QStringLiteral("SET_IMAGE extra payload exceeds maximum IPC limit"));

    uint32_t imgW = 0;
    uint32_t imgH = 0;
    std::vector<uint8_t> solidBuf;
    uint8_t *loadedPixels = nullptr;
    const uint8_t *srcPixels = nullptr;

    struct LoadedPixelGuard {
        uint8_t **ptr;
        ~LoadedPixelGuard()
        {
            if (ptr && *ptr) {
                image_free(*ptr);
                *ptr = nullptr;
            }
        }
    } pixelGuard{&loadedPixels};

    waywal_img_metadata_t packedMeta = meta;
    packedMeta.filepath[sizeof(packedMeta.filepath) - 1] = '\0';
    packedMeta.depth_map_path[sizeof(packedMeta.depth_map_path) - 1] = '\0';
    packedMeta.backdrop[sizeof(packedMeta.backdrop) - 1] = '\0';
    packedMeta.layer_name[sizeof(packedMeta.layer_name) - 1] = '\0';
    packedMeta.battery_policy[sizeof(packedMeta.battery_policy) - 1] = '\0';

    if (packedMeta.is_solid_color != 0u) {
        imgW = (packedMeta.width > 0 && packedMeta.width <= 64u) ? packedMeta.width : 16u;
        imgH = (packedMeta.height > 0 && packedMeta.height <= 64u) ? packedMeta.height : 16u;
        const size_t count = static_cast<size_t>(imgW) * static_cast<size_t>(imgH);
        solidBuf.resize(count * 4u);
        for (size_t i = 0; i < count; ++i) {
            solidBuf[i * 4u + 0] = packedMeta.color[2];
            solidBuf[i * 4u + 1] = packedMeta.color[1];
            solidBuf[i * 4u + 2] = packedMeta.color[0];
            solidBuf[i * 4u + 3] = packedMeta.color[3];
        }
        srcPixels = solidBuf.data();
    } else {
        if (packedMeta.filepath[0] == '\0')
            return fail(QStringLiteral("SET_IMAGE requires filepath or solid color when memfd < 0"));

        char resolvedPath[PATH_MAX] = {};
        const char *loadPath = packedMeta.filepath;
        if (::realpath(packedMeta.filepath, resolvedPath) != nullptr) {
            const size_t rlen = ::strnlen(resolvedPath, sizeof(packedMeta.filepath) - 1u);
            std::memcpy(packedMeta.filepath, resolvedPath, rlen);
            packedMeta.filepath[rlen] = '\0';
            loadPath = packedMeta.filepath;
        }
        if (!image_load(loadPath, &loadedPixels, &imgW, &imgH) || !loadedPixels || imgW == 0 ||
            imgH == 0 || imgW > 16384u || imgH > 16384u) {
            return fail(QStringLiteral("Failed to decode wallpaper image: %1")
                            .arg(QString::fromUtf8(packedMeta.filepath)));
        }
        srcPixels = loadedPixels;
    }

    packedMeta.width = imgW;
    packedMeta.height = imgH;
    packedMeta.pixel_format =
        packedMeta.ten_bit ? WAYWAL_FORMAT_XRGB2101010 : WAYWAL_FORMAT_ARGB8888;
    packedMeta.num_target_outputs = static_cast<uint32_t>(normalizedOutputs.size());
    packedMeta.custom_shader_len = static_cast<uint32_t>(shaderLen);
    packedMeta.extra_data_len = static_cast<uint32_t>(extraBytes);
    if (packedMeta.depth_strength <= 0.0f)
        packedMeta.depth_strength = 1.0f;

    const uint64_t pixelBytes = static_cast<uint64_t>(imgW) * static_cast<uint64_t>(imgH) * 4ULL;
    const uint64_t totalPayloadSize =
        sizeof(waywal_img_metadata_t) + static_cast<uint64_t>(extraBytes) + pixelBytes;
    if (totalPayloadSize > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return fail(QStringLiteral("SET_IMAGE memfd payload overflows off_t"));

    ScopedFd memfd(waywal_create_memfd("wwal-studio-img", static_cast<size_t>(totalPayloadSize),
                                       F_SEAL_SHRINK | F_SEAL_GROW));
    if (!memfd.valid())
        return fail(QStringLiteral("Failed to allocate sealed image memfd"));

    void *mapped = ::mmap(nullptr, static_cast<size_t>(totalPayloadSize), PROT_READ | PROT_WRITE,
                          MAP_SHARED, memfd.get(), 0);
    if (mapped == MAP_FAILED)
        return fail(QStringLiteral("Failed to mmap Studio image memfd"));

    uint8_t *dst = static_cast<uint8_t *>(mapped);
    std::memcpy(dst, &packedMeta, sizeof(packedMeta));
    dst += sizeof(packedMeta);

    for (const QByteArray &outBa : normalizedOutputs) {
        const size_t slen = static_cast<size_t>(outBa.size()) + 1u;
        std::memcpy(dst, outBa.constData(), slen);
        dst += slen;
    }
    if (shaderLen > 0) {
        std::memcpy(dst, shaderBa.constData(), shaderLen);
        dst += shaderLen;
    }
    std::memcpy(dst, srcPixels, static_cast<size_t>(pixelBytes));
    ::munmap(mapped, static_cast<size_t>(totalPayloadSize));

    (void)::fcntl(memfd.get(), F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL);

    outHdr.magic = WAYWAL_IPC_MAGIC;
    outHdr.version = WAYWAL_IPC_VERSION;
    outHdr.opcode = WAYWAL_REQ_SET_IMAGE;
    outHdr.payload_size = totalPayloadSize;
    outMemfd = std::move(memfd);
    return true;
}

bool IpcServiceCodecs::encodeAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                                              uint8_t batteryPolicy,
                                              waywal_audio_ctrl_payload_t &outPayload,
                                              QString *outErr)
{
    if (!std::isfinite(volume)) {
        if (outErr)
            *outErr = QStringLiteral("Audio volume must be finite");
        return false;
    }
    if ((muteOnAudio != -1 && muteOnAudio != 0 && muteOnAudio != 1) ||
        (themeFromMedia != -1 && themeFromMedia != 0 && themeFromMedia != 1)) {
        if (outErr)
            *outErr = QStringLiteral("Invalid audio/theme boolean tri-state");
        return false;
    }

    std::memset(&outPayload, 0, sizeof(outPayload));
    outPayload.volume = (volume >= 0.0f) ? std::clamp(volume, 0.0f, 1.0f) : -1.0f;
    outPayload.mute_on_audio = muteOnAudio;
    outPayload.theme_from_media = themeFromMedia;
    outPayload.reserved[0] = batteryPolicy;
    outPayload.reserved[1] = 1u;
    return true;
}

bool IpcServiceCodecs::decodeTelemetrySnapshot(const std::vector<uint8_t> &payload,
                                             TelemetrySnapshot &outSnap, QString *outErr)
{
    if (payload.size() != sizeof(waywal_telemetry_snapshot_t)) {
        if (outErr)
            *outErr = QStringLiteral("Malformed WAYWAL_RESP_TELEMETRY payload size");
        return false;
    }

    waywal_telemetry_snapshot_t raw{};
    std::memcpy(&raw, payload.data(), sizeof(raw));

    if (!std::isfinite(raw.rms_level) || !std::isfinite(raw.beat_pulse) ||
        !std::isfinite(raw.audio_volume) || raw.num_outputs > WAYWAL_MAX_TELEMETRY_OUTPUTS) {
        if (outErr)
            *outErr = QStringLiteral("Invalid telemetry audio or output count fields");
        return false;
    }
    for (float b : raw.bark_bands) {
        if (!std::isfinite(b)) {
            if (outErr)
                *outErr = QStringLiteral("Non-finite telemetry Bark spectrum band");
            return false;
        }
    }

    outSnap = TelemetrySnapshot{};
    outSnap.timestampNs = raw.timestamp_ns;
    outSnap.rssKiB = raw.rss_kib;
    outSnap.pssKiB = raw.pss_kib;
    outSnap.totalVramKiB = raw.total_vram_kib;
    outSnap.gpuActive = (raw.gpu_active != 0);
    outSnap.onBattery = (raw.on_battery != 0);
    outSnap.batteryPolicy = raw.battery_policy;
    outSnap.globalPauseMask = raw.global_pause_mask;
    outSnap.barkBands.assign(raw.bark_bands, raw.bark_bands + 64);
    outSnap.rmsLevel = raw.rms_level;
    outSnap.beatPulse = raw.beat_pulse;
    outSnap.beatOnset = (raw.beat_onset != 0);
    outSnap.audioActive = (raw.audio_active != 0);
    outSnap.pipewireActive = (raw.pipewire_active != 0);
    outSnap.muteOnAudio = (raw.mute_on_audio != 0);
    outSnap.audioVolume = raw.audio_volume;

    outSnap.outputs.reserve(raw.num_outputs);
    for (uint32_t i = 0; i < raw.num_outputs; ++i) {
        const auto &ro = raw.outputs[i];
        if (!std::isfinite(ro.scanout_fps)) {
            if (outErr)
                *outErr = QStringLiteral("Non-finite output scanout FPS in telemetry");
            return false;
        }
        OutputTelemetry ot{};
        const size_t nameLen = ::strnlen(ro.name, sizeof(ro.name));
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
        outSnap.outputs.push_back(std::move(ot));
    }
    return true;
}

bool IpcServiceCodecs::decodeDmabufExport(const std::vector<uint8_t> &payload, int fds[],
                                          size_t numFds, waywal_export_dmabuf_resp_t &outDesc,
                                          QVector<int> &outTransferredFds, QString *outErr)
{
    if (payload.size() != sizeof(waywal_export_dmabuf_resp_t)) {
        if (outErr)
            *outErr = QStringLiteral("Malformed WAYWAL_RESP_DMABUF payload size");
        return false;
    }

    std::memcpy(&outDesc, payload.data(), sizeof(outDesc));
    if (outDesc.width == 0 || outDesc.height == 0 || outDesc.num_planes == 0 ||
        outDesc.num_planes > 4u || numFds < outDesc.num_planes) {
        if (outErr)
            *outErr = QStringLiteral("Invalid DMA-BUF export metadata or missing plane FDs");
        return false;
    }

    outTransferredFds.clear();
    outTransferredFds.reserve(static_cast<qsizetype>(outDesc.num_planes));
    for (uint32_t i = 0; i < outDesc.num_planes; ++i) {
        if (fds[i] < 0) {
            if (outErr)
                *outErr = QStringLiteral("Missing plane file descriptor in WAYWAL_RESP_DMABUF");
            return false;
        }
    }
    for (uint32_t i = 0; i < outDesc.num_planes; ++i) {
        outTransferredFds.append(fds[i]);
        fds[i] = -1;
    }
    return true;
}

/* ========================================================================= */
/* IpcTransport                                                              */
/* ========================================================================= */

IpcTransport::~IpcTransport()
{
    (void)closeSocket();
    destroyWakeupFd();
}

bool IpcTransport::initWakeupFd() noexcept
{
    if (m_wakeupFd >= 0)
        return true;
    m_wakeupFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return m_wakeupFd >= 0;
}

void IpcTransport::destroyWakeupFd() noexcept { closeFdChecked(m_wakeupFd); }

void IpcTransport::wakeup() noexcept
{
    if (m_wakeupFd < 0)
        return;
    const uint64_t one = 1;
    ssize_t n = 0;
    do {
        n = ::write(m_wakeupFd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
}

void IpcTransport::drainWakeup() noexcept
{
    if (m_wakeupFd < 0)
        return;
    uint64_t val = 0;
    while (::read(m_wakeupFd, &val, sizeof(val)) > 0 || errno == EINTR) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        errno = 0;
    }
}

int IpcTransport::pollFdWithDeadline(int fd, short events,
                                     std::chrono::steady_clock::time_point deadline,
                                     short *outRevents) noexcept
{
    if (outRevents)
        *outRevents = 0;
    if (fd < 0)
        return -1;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return 0;
        const auto remMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int sliceMs = static_cast<int>(
            std::clamp<int64_t>(remMs > 0 ? remMs : 1, 1, std::numeric_limits<int>::max()));

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;
        const int pr = ::poll(&pfd, 1, sliceMs);
        if (pr < 0 && errno == EINTR)
            continue;
        if (outRevents && pr > 0)
            *outRevents = pfd.revents;
        return pr;
    }
}

bool IpcTransport::sleepWakeupWithDeadline(int wakeupFd, int durationMs) noexcept
{
    if (wakeupFd < 0 || durationMs <= 0)
        return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    short revents = 0;
    const int pr = pollFdWithDeadline(wakeupFd, POLLIN, deadline, &revents);
    return pr > 0 && (revents & POLLIN) != 0;
}

bool IpcTransport::readExactNonBlocking(int fd, void *buf, size_t count, int timeoutMs) noexcept
{
    if (fd < 0 || (!buf && count > 0) || timeoutMs < 0)
        return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    return readExactWithDeadline(fd, buf, count, deadline);
}

bool IpcTransport::writeExactNonBlocking(int fd, const void *buf, size_t count,
                                         int timeoutMs) noexcept
{
    if (fd < 0 || (!buf && count > 0) || timeoutMs < 0)
        return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    return writeExactWithDeadline(fd, buf, count, deadline);
}

bool IpcTransport::connectSocket(const QString &namespaceStr, const QString &customSocketPath,
                                 QString &outResolvedPath)
{
    QString targetPath = customSocketPath;
    if (targetPath.isEmpty()) {
        path_buf_t pb;
        const QByteArray nsBa = namespaceStr.toUtf8();
        if (!waywal_ipc_resolve_socket_path(&pb, nsBa.isEmpty() ? nullptr : nsBa.constData()))
            return false;
        targetPath = QString::fromUtf8(path_buf_cstr(&pb));
        outResolvedPath = targetPath;
    }

    const QByteArray pathBa = targetPath.toUtf8();
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (pathBa.isEmpty() || static_cast<size_t>(pathBa.size()) >= sizeof(addr.sun_path))
        return false;
    std::memcpy(addr.sun_path, pathBa.constData(), static_cast<size_t>(pathBa.size()));

    ScopedFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd.valid())
        return false;

    const int bufSize = 262144;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

    int res = 0;
    do {
        res = ::connect(fd.get(), reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
    } while (res < 0 && errno == EINTR);

    if (res < 0 && errno != EINPROGRESS)
        return false;

    if (res < 0 && errno == EINPROGRESS) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        short revents = 0;
        const int pr = pollFdWithDeadline(fd.get(), POLLOUT, deadline, &revents);
        if (pr <= 0 || (revents & (POLLOUT | POLLERR | POLLHUP)) == 0)
            return false;
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0)
            return false;
    }

    m_socketFd = fd.release();
    m_connected.store(true);
    return true;
}

bool IpcTransport::closeSocket() noexcept
{
    closeFdChecked(m_socketFd);
    return m_connected.exchange(false);
}

bool IpcTransport::waitForEvents(int pollTimeoutMs, bool &outWoken, bool &outSocketReadable,
                                 bool &outSocketHangup) noexcept
{
    outWoken = false;
    outSocketReadable = false;
    outSocketHangup = false;

    struct pollfd pfds[2]{};
    pfds[0].fd = m_wakeupFd;
    pfds[0].events = POLLIN;
    pfds[1].fd = m_socketFd;
    pfds[1].events = POLLIN;

    int pr = 0;
    do {
        pr = ::poll(pfds, 2, pollTimeoutMs);
    } while (pr < 0 && errno == EINTR);

    if (pr < 0)
        return false;

    outWoken = (pfds[0].revents & POLLIN) != 0;
    outSocketReadable = (pfds[1].revents & POLLIN) != 0;
    outSocketHangup = (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    return true;
}

bool IpcTransport::peekSocketReadable(bool &outHasBytes) noexcept
{
    outHasBytes = false;
    if (m_socketFd < 0)
        return false;

    char peekByte = 0;
    ssize_t pk = 0;
    do {
        pk = ::recv(m_socketFd, &peekByte, 1, MSG_PEEK | MSG_DONTWAIT);
    } while (pk < 0 && errno == EINTR);

    if (pk == 0)
        return false;
    if (pk < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        return false;
    }
    outHasBytes = true;
    return true;
}

bool IpcTransport::sendFrame(const waywal_ipc_hdr_t &hdr, const uint8_t *payload,
                             size_t payloadSize, int attachedFd, int timeoutMs) noexcept
{
    if (m_socketFd < 0)
        return false;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    const uint8_t *hdrBytes = reinterpret_cast<const uint8_t *>(&hdr);
    const size_t totalBytes = sizeof(hdr) + payloadSize;
    size_t sentBytes = 0;
    bool fdSent = (attachedFd < 0);

    while (sentBytes < totalBytes) {
        struct iovec iovs[2]{};
        size_t iovCount = 0;

        if (sentBytes < sizeof(hdr)) {
            iovs[iovCount].iov_base = const_cast<uint8_t *>(hdrBytes + sentBytes);
            iovs[iovCount].iov_len = sizeof(hdr) - sentBytes;
            ++iovCount;
        }
        if (payload && payloadSize > 0) {
            const size_t payOff = sentBytes > sizeof(hdr) ? (sentBytes - sizeof(hdr)) : 0u;
            if (payOff < payloadSize) {
                iovs[iovCount].iov_base = const_cast<uint8_t *>(payload + payOff);
                iovs[iovCount].iov_len = payloadSize - payOff;
                ++iovCount;
            }
        }

        struct msghdr msg{};
        msg.msg_iov = iovs;
        msg.msg_iovlen = iovCount;

        alignas(struct cmsghdr) char ctrlBuf[CMSG_SPACE(sizeof(int))]{};
        if (!fdSent) {
            msg.msg_control = ctrlBuf;
            msg.msg_controllen = sizeof(ctrlBuf);
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &attachedFd, sizeof(int));
        }

        const ssize_t n = ::sendmsg(m_socketFd, &msg, MSG_NOSIGNAL);
        if (n > 0) {
            sentBytes += static_cast<size_t>(n);
            // Linux AF_UNIX stream delivers SCM_RIGHTS once the first non-empty write lands.
            fdSent = true;
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            short revents = 0;
            const int pr = pollFdWithDeadline(m_socketFd, POLLOUT, deadline, &revents);
            if (pr > 0 && (revents & POLLOUT) != 0 && (revents & (POLLERR | POLLNVAL)) == 0)
                continue;
            return false;
        }
        return false;
    }

    return true;
}

bool IpcTransport::recvFrameHeaderAndFds(waywal_ipc_hdr_t &outHdr, int outFds[], size_t maxFds,
                                         size_t &outNumFds,
                                         std::chrono::steady_clock::time_point deadline) noexcept
{
    outNumFds = 0;
    if (outFds) {
        for (size_t i = 0; i < maxFds; ++i)
            outFds[i] = -1;
    }
    if (m_socketFd < 0)
        return false;

    uint8_t *hdrBytes = reinterpret_cast<uint8_t *>(&outHdr);
    size_t totalRead = 0;
    size_t extracted = 0;

    while (totalRead < sizeof(outHdr)) {
        struct msghdr msg{};
        struct iovec iov{};
        iov.iov_base = hdrBytes + totalRead;
        iov.iov_len = sizeof(outHdr) - totalRead;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        union {
            char buf[CMSG_SPACE(sizeof(int) * 253)];
            struct cmsghdr align;
        } ctrl{};

        // Ancillary SCM_RIGHTS control data travels with the leading bytes of the frame.
        if (totalRead == 0) {
            msg.msg_control = ctrl.buf;
            msg.msg_controllen = sizeof(ctrl.buf);
        }

        const ssize_t r = ::recvmsg(m_socketFd, &msg, MSG_CMSG_CLOEXEC);
        if (r > 0) {
            if (msg.msg_controllen >= sizeof(struct cmsghdr)) {
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
                     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                        cmsg->cmsg_len >= CMSG_LEN(0)) {
                        const size_t fdCount = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                        const unsigned char *data = CMSG_DATA(cmsg);
                        for (size_t i = 0; i < fdCount; ++i) {
                            int fd = -1;
                            std::memcpy(&fd, data + i * sizeof(int), sizeof(int));
                            if (fd >= 0) {
                                if (outFds && extracted < maxFds &&
                                    (msg.msg_flags & MSG_CTRUNC) == 0) {
                                    outFds[extracted++] = fd;
                                } else {
                                    ::close(fd);
                                }
                            }
                        }
                    }
                }
            }
            if ((msg.msg_flags & MSG_CTRUNC) != 0) {
                closeFdArray(outFds, extracted);
                return false;
            }
            totalRead += static_cast<size_t>(r);
            continue;
        }

        if (r == 0) {
            closeFdArray(outFds, extracted);
            return false;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            short revents = 0;
            const int pr = pollFdWithDeadline(m_socketFd, POLLIN, deadline, &revents);
            if (pr > 0 && (revents & (POLLIN | POLLERR | POLLHUP)) != 0)
                continue;
            closeFdArray(outFds, extracted);
            return false;
        }
        closeFdArray(outFds, extracted);
        return false;
    }

    if (outHdr.magic != WAYWAL_IPC_MAGIC || outHdr.version != WAYWAL_IPC_VERSION) {
        WAYWAL_LOG_ERR("IpcTransport: Header magic/version mismatch: magic=0x%08x ver=%u",
                       outHdr.magic, outHdr.version);
        closeFdArray(outFds, extracted);
        return false;
    }

    outNumFds = extracted;
    return true;
}

bool IpcTransport::recvFrame(waywal_ipc_hdr_t &outHdr, std::vector<uint8_t> &outPayload,
                             int outFds[], size_t maxFds, size_t &outNumFds,
                             size_t maxPayloadBytes, int timeoutMs) noexcept
{
    outPayload.clear();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));

    if (!recvFrameHeaderAndFds(outHdr, outFds, maxFds, outNumFds, deadline))
        return false;

    if (outHdr.payload_size > 0) {
        if (outHdr.payload_size > maxPayloadBytes) {
            WAYWAL_LOG_ERR("IpcTransport: Payload size exceeds limit: %lu",
                           static_cast<unsigned long>(outHdr.payload_size));
            closeFdArray(outFds, outNumFds);
            outNumFds = 0;
            return false;
        }
        outPayload.resize(static_cast<size_t>(outHdr.payload_size));
        if (!readExactWithDeadline(m_socketFd, outPayload.data(), outPayload.size(), deadline)) {
            closeFdArray(outFds, outNumFds);
            outNumFds = 0;
            outPayload.clear();
            return false;
        }
    }
    return true;
}

/* ========================================================================= */
/* IpcDispatcher                                                             */
/* ========================================================================= */

void IpcDispatcher::enqueuePacket(IpcOutboundPacket pkt)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_outQueue.push_back(std::move(pkt));
}

bool IpcDispatcher::coalesceUniform(const std::string &key,
                                    const waywal_set_uniform_payload_t &payload)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    const bool shouldWake = m_pendingUniforms.empty();
    m_pendingUniforms[key] = payload;
    return shouldWake;
}

bool IpcDispatcher::hasPendingOutbound() const
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return !m_outQueue.empty() || !m_pendingUniforms.empty();
}

bool IpcDispatcher::hasPendingRequests() const
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    return !m_pendingRequests.empty();
}

void IpcDispatcher::takeFlushBatch(std::deque<IpcOutboundPacket> &outPackets,
                                   std::vector<waywal_set_uniform_payload_t> &outUniforms,
                                   std::chrono::steady_clock::time_point now)
{
    outPackets.clear();
    outUniforms.clear();

    std::lock_guard<std::mutex> lock(m_queueMutex);
    outPackets.swap(m_outQueue);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUniformFlush).count();
    if (!m_pendingUniforms.empty() && elapsedMs >= UNIFORM_FLUSH_INTERVAL_MS) {
        outUniforms.reserve(m_pendingUniforms.size());
        for (const auto &kv : m_pendingUniforms) {
            outUniforms.push_back(kv.second);
        }
        m_pendingUniforms.clear();
        m_lastUniformFlush = now;
    }
}

void IpcDispatcher::recordSentUniform() noexcept
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    if (m_unackedUniformSeqs.size() >= MAX_TRACKED_UNIFORM_ACKS) {
        m_unackedUniformSeqs.pop_front();
    }
    m_unackedUniformSeqs.push_back(nextSendSeq());
}

void IpcDispatcher::recordSentSubscribe() noexcept
{
    nextSendSeq();
    m_awaitingSubscribeAck.store(true);
}

void IpcDispatcher::recordSentRequest(IpcOutboundPacket &pkt, uint16_t reqOpcode,
                                      std::chrono::steady_clock::time_point now)
{
    const uint64_t seq = nextSendSeq();
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    m_pendingRequests.push_back(IpcPendingRequest::fromPacket(pkt, seq, reqOpcode, now));
}

void IpcDispatcher::pruneUnackedUniformsBefore(uint64_t seq) noexcept
{
    while (!m_unackedUniformSeqs.empty() && m_unackedUniformSeqs.front() < seq) {
        m_unackedUniformSeqs.pop_front();
    }
}

bool IpcDispatcher::popPendingForOpcode(uint16_t reqOpcode, IpcPendingRequest &outReq)
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    for (size_t idx = 0; idx < m_pendingRequests.size(); ++idx) {
        if (m_pendingRequests[idx].expectedOpcode == reqOpcode) {
            outReq = std::move(m_pendingRequests[idx]);
            m_pendingRequests.erase(m_pendingRequests.begin() + static_cast<std::ptrdiff_t>(idx));
            pruneUnackedUniformsBefore(outReq.sendSeq);
            return true;
        }
    }
    return false;
}

bool IpcDispatcher::popFrontForError(IpcPendingRequest &outReq)
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    if (m_pendingRequests.empty())
        return false;
    outReq = std::move(m_pendingRequests.front());
    m_pendingRequests.pop_front();
    pruneUnackedUniformsBefore(outReq.sendSeq);
    return true;
}

bool IpcDispatcher::popNextGenericOk(IpcPendingRequest &outReq)
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    for (size_t idx = 0; idx < m_pendingRequests.size(); ++idx) {
        if (IpcServiceCodecs::expectsGenericOkForOpcode(m_pendingRequests[idx].expectedOpcode)) {
            outReq = std::move(m_pendingRequests[idx]);
            m_pendingRequests.erase(m_pendingRequests.begin() + static_cast<std::ptrdiff_t>(idx));
            pruneUnackedUniformsBefore(outReq.sendSeq);
            return true;
        }
    }
    return false;
}

bool IpcDispatcher::isAwaitingSubscribeAck() const noexcept
{
    return m_awaitingSubscribeAck.load();
}

bool IpcDispatcher::tryConsumeSubscribeAck() noexcept
{
    return m_awaitingSubscribeAck.exchange(false);
}

bool IpcDispatcher::tryConsumeUnackedUniform() noexcept
{
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    if (m_unackedUniformSeqs.empty())
        return false;
    m_unackedUniformSeqs.pop_front();
    return true;
}

void IpcDispatcher::collectExpiredRequests(int timeoutMs,
                                           std::vector<IpcPendingRequest> &outExpired,
                                           std::chrono::steady_clock::time_point now)
{
    outExpired.clear();
    if (timeoutMs <= 0)
        return;

    const auto deadline = std::chrono::milliseconds(timeoutMs);
    std::lock_guard<std::mutex> lock(m_requestsMutex);
    auto it = m_pendingRequests.begin();
    while (it != m_pendingRequests.end()) {
        if (now - it->sentAt >= deadline) {
            outExpired.push_back(std::move(*it));
            it = m_pendingRequests.erase(it);
        } else {
            ++it;
        }
    }
}

void IpcDispatcher::drainAllOnDisconnect(std::deque<IpcPendingRequest> &outPending,
                                         std::deque<IpcOutboundPacket> &outUnsent)
{
    outPending.clear();
    outUnsent.clear();
    {
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        outPending.swap(m_pendingRequests);
        m_unackedUniformSeqs.clear();
        m_awaitingSubscribeAck.store(false);
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        outUnsent.swap(m_outQueue);
        m_pendingUniforms.clear();
    }
}

/* ========================================================================= */
/* IpcClient                                                                 */
/* ========================================================================= */

IpcClient::IpcClient(QString namespaceStr, QString socketPath, QObject *parent, bool autoStart)
    : QObject(parent), m_namespaceStr(std::move(namespaceStr)),
      m_customSocketPath(std::move(socketPath))
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

    if (m_namespaceStr.isEmpty())
        m_namespaceStr = QStringLiteral("default");

    if (!m_transport.initWakeupFd())
        WAYWAL_LOG_ERR("IpcClient: eventfd creation failed: %s", std::strerror(errno));

    if (autoStart)
        start();
}

IpcClient::~IpcClient()
{
    m_destroying.store(true);
    stop();
    m_transport.destroyWakeupFd();
}

void IpcClient::start()
{
    if (m_running.exchange(true))
        return;
    m_workerThread = std::thread(&IpcClient::workerLoop, this);
}

void IpcClient::stop()
{
    // Fail in-flight and unsent requests while event delivery is still permitted on explicit stop(),
    // before tearing down the socket and flipping m_running offline.
    m_running.store(false);
    m_transport.wakeup();
    if (m_workerThread.joinable())
        m_workerThread.join();
    closeAndFailPending(QStringLiteral("IPC client stopped"));
}

bool IpcClient::isConnected() const { return m_transport.isConnected(); }

QString IpcClient::socketPath() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_customSocketPath.isEmpty())
        return m_customSocketPath;
    return m_resolvedSocketPath;
}

void IpcClient::setSocketPath(const QString &socketPath)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_customSocketPath = socketPath;
        m_resolvedSocketPath.clear();
    }
    m_transport.wakeup();
}

void IpcClient::setRequestTimeoutMs(int timeoutMs)
{
    m_requestTimeoutMs.store(std::max(100, timeoutMs));
    m_transport.wakeup();
}

void IpcClient::enqueueAndWake(IpcOutboundPacket pkt)
{
    m_dispatcher.enqueuePacket(std::move(pkt));
    m_transport.wakeup();
}

bool IpcClient::enqueueRawStruct(uint16_t opcode, const void *data, size_t size)
{
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = opcode;
    pkt.hdr.payload_size = static_cast<uint64_t>(size);
    if (data && size > 0) {
        pkt.payload.resize(size);
        std::memcpy(pkt.payload.data(), data, size);
    }
    enqueueAndWake(std::move(pkt));
    return true;
}

void IpcClient::dispatchRequestFailure(IpcPendingRequest req, const QString &reason, bool emitError)
{
    if (m_destroying.load())
        return;

    const bool hasCb = req.hasCallback();
    if (!hasCb && !emitError)
        return;

    QMetaObject::invokeMethod(
        this,
        [this, req = std::move(req), reason, emitError]() mutable {
            if (req.jsonCallback)
                req.jsonCallback(QString{});
            if (req.telemetryCallback)
                req.telemetryCallback(TelemetrySnapshot{});
            if (req.dmabufCallback) {
                waywal_export_dmabuf_resp_t emptyDesc{};
                req.dmabufCallback(emptyDesc, QVector<int>{});
            }
            if (req.workshopSyncCallback) {
                waywal_ipc_resp_workshop_sync_t resp{};
                resp.status_code = 1u;
                req.workshopSyncCallback(resp);
            }
            if (req.workshopDownloadCallback) {
                waywal_ipc_resp_workshop_download_t resp{};
                resp.task_state = 7u;
                const QByteArray ba = reason.toUtf8();
                const size_t n = std::min(static_cast<size_t>(ba.size()), sizeof(resp.error_msg) - 1u);
                if (n > 0)
                    std::memcpy(resp.error_msg, ba.constData(), n);
                req.workshopDownloadCallback(resp);
            }
            if (req.wwpkgInstallCallback ||
                req.expectedOpcode == WAYWAL_REQ_WWPKG_INSTALL) {
                waywal_ipc_resp_wwpkg_install_t resp{};
                resp.status_code = 1u;
                if (req.wwpkgInstallCallback)
                    req.wwpkgInstallCallback(resp, reason);
                emit wwpkgInstallReceived(resp, reason);
            }
            if (req.scanCompatCallback) {
                waywal_ipc_resp_scan_compat_t resp{};
                resp.status_code = 1u;
                req.scanCompatCallback(resp);
            }
            if (emitError)
                emit errorOccurred(reason);
        },
        Qt::QueuedConnection);
}

void IpcClient::dispatchUnsubmittedPacketFailure(IpcOutboundPacket pkt, const QString &reason,
                                                 bool emitError)
{
    pkt.attachedFd.reset();
    IpcPendingRequest req =
        IpcPendingRequest::fromPacket(pkt, 0, pkt.hdr.opcode, std::chrono::steady_clock::now());
    dispatchRequestFailure(std::move(req), reason, emitError);
}

void IpcClient::closeAndFailPending(const QString &reason)
{
    const bool wasConnected = m_transport.closeSocket();

    std::deque<IpcPendingRequest> stalePending;
    std::deque<IpcOutboundPacket> staleUnsent;
    m_dispatcher.drainAllOnDisconnect(stalePending, staleUnsent);

    for (auto &req : stalePending)
        dispatchRequestFailure(std::move(req), reason, false);
    for (auto &pkt : staleUnsent)
        dispatchUnsubmittedPacketFailure(std::move(pkt), reason, false);

    if (wasConnected && !m_destroying.load()) {
        QMetaObject::invokeMethod(
            this, [this]() { emit connectionStateChanged(false); }, Qt::QueuedConnection);
    }
}

void IpcClient::sweepExpiredRequests()
{
    if (m_destroying.load())
        return;

    const int timeoutMs = m_requestTimeoutMs.load();
    std::vector<IpcPendingRequest> expired;
    m_dispatcher.collectExpiredRequests(timeoutMs, expired, std::chrono::steady_clock::now());
    for (auto &req : expired) {
        const QString reason = QStringLiteral("IPC request 0x%1 timed out after %2 ms")
                                   .arg(req.expectedOpcode, 4, 16, QLatin1Char('0'))
                                   .arg(timeoutMs);
        dispatchRequestFailure(std::move(req), reason, true);
    }
}

void IpcClient::workerLoop()
{
    int backoffMs = 50;

    while (m_running.load()) {
        if (!m_transport.isConnected()) {
            QString customPath;
            QString ns;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                customPath = m_customSocketPath;
                ns = m_namespaceStr;
            }
            QString resolvedPath;
            if (!m_transport.connectSocket(ns, customPath, resolvedPath)) {
                if (IpcTransport::sleepWakeupWithDeadline(m_transport.wakeupFd(), backoffMs))
                    m_transport.drainWakeup();
                backoffMs = std::min(backoffMs * 2, 1500);
                continue;
            }
            if (!resolvedPath.isEmpty()) {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_resolvedSocketPath = resolvedPath;
            }
            backoffMs = 50;
            if (!m_destroying.load()) {
                QMetaObject::invokeMethod(
                    this, [this]() { emit connectionStateChanged(true); }, Qt::QueuedConnection);
            }
            subscribeEvents();
        }

        int pollTimeoutMs = 250;
        if (m_dispatcher.hasPendingRequests()) {
            const int reqTimeout = m_requestTimeoutMs.load();
            if (reqTimeout > 0)
                pollTimeoutMs = std::min(pollTimeoutMs, std::max(10, reqTimeout));
        }
        if (m_dispatcher.hasPendingOutbound())
            pollTimeoutMs = 0;

        bool woken = false;
        bool readable = false;
        bool hangup = false;
        if (!m_transport.waitForEvents(pollTimeoutMs, woken, readable, hangup)) {
            closeAndFailPending(QStringLiteral("IPC socket poll failed"));
            continue;
        }

        if (woken)
            m_transport.drainWakeup();

        if (readable) {
            for (int frames = 0; frames < 64 && m_transport.isConnected() && m_running.load();
                 ++frames) {
                bool hasBytes = false;
                if (!m_transport.peekSocketReadable(hasBytes)) {
                    closeAndFailPending(QStringLiteral("IPC peer closed connection"));
                    break;
                }
                if (!hasBytes)
                    break;
                handleIncomingPacket();
            }
        } else if (hangup) {
            closeAndFailPending(QStringLiteral("IPC socket disconnected"));
        }

        if (m_transport.isConnected() && m_running.load())
            flushOutboundQueue();

        sweepExpiredRequests();
    }
}

void IpcClient::flushOutboundQueue()
{
    if (!m_transport.isConnected())
        return;

    std::deque<IpcOutboundPacket> packets;
    std::vector<waywal_set_uniform_payload_t> uniforms;
    m_dispatcher.takeFlushBatch(packets, uniforms, std::chrono::steady_clock::now());

    for (auto &pkt : packets) {
        if (!m_transport.isConnected() || !m_running.load()) {
            dispatchUnsubmittedPacketFailure(
                std::move(pkt), QStringLiteral("IPC connection closed before send"), false);
            continue;
        }

        if (pkt.kind == IpcOutboundPacket::Kind::DeferredSetImage) {
            QString err;
            waywal_ipc_hdr_t imgHdr{};
            ScopedFd imgFd;
            if (!IpcServiceCodecs::prepareSetImagePacket(pkt.imgMeta, pkt.imgTargetOutputs,
                                                         pkt.imgCustomShader, -1, imgHdr, imgFd,
                                                         &err)) {
                dispatchUnsubmittedPacketFailure(std::move(pkt), err, true);
                continue;
            }
            pkt.hdr = imgHdr;
            pkt.payload.clear();
            pkt.attachedFd = std::move(imgFd);
        } else if (pkt.kind == IpcOutboundPacket::Kind::DeferredWwpkgHubLookup) {
            const int hubFd = IpcServiceCodecs::tryOpenHubPackageByUuid(pkt.hubPackageUuid);
            if (hubFd >= 0)
                pkt.attachedFd.reset(hubFd);
        }

        if (!m_transport.sendFrame(pkt.hdr, pkt.payload.empty() ? nullptr : pkt.payload.data(),
                                   pkt.payload.size(), pkt.attachedFd.get())) {
            dispatchUnsubmittedPacketFailure(
                std::move(pkt), QStringLiteral("Failed to write IPC frame to daemon"), true);
            closeAndFailPending(QStringLiteral("IPC write failure"));
            return;
        }

        pkt.attachedFd.reset();
        if (pkt.kind == IpcOutboundPacket::Kind::Subscribe ||
            pkt.hdr.opcode == WAYWAL_REQ_SUBSCRIBE) {
            m_dispatcher.recordSentSubscribe();
        } else if (IpcServiceCodecs::isFireAndForgetNoReplyOpcode(pkt.hdr.opcode)) {
            m_dispatcher.recordSentUniform();
        } else {
            m_dispatcher.recordSentRequest(pkt, pkt.hdr.opcode, std::chrono::steady_clock::now());
        }
    }

    for (const auto &upay : uniforms) {
        if (!m_transport.isConnected() || !m_running.load())
            return;
        waywal_ipc_hdr_t hdr{};
        hdr.magic = WAYWAL_IPC_MAGIC;
        hdr.version = WAYWAL_IPC_VERSION;
        hdr.opcode = WAYWAL_REQ_SET_UNIFORM;
        hdr.payload_size = sizeof(upay);
        if (!m_transport.sendFrame(hdr, reinterpret_cast<const uint8_t *>(&upay), sizeof(upay),
                                   -1)) {
            closeAndFailPending(QStringLiteral("IPC uniform write failure"));
            return;
        }
        m_dispatcher.recordSentUniform();
    }
}

void IpcClient::handleIncomingPacket()
{
    waywal_ipc_hdr_t hdr{};
    std::vector<uint8_t> payload;
    int fds[ScopedFdArrayGuard::MAX_ANCILLARY_FDS] = {-1, -1, -1, -1};
    size_t numFds = 0;
    const size_t maxAllowed =
        sizeof(waywal_ipc_resp_readback_texture_t) + WAYWAL_IPC_MAX_READBACK_BYTES;

    if (!m_transport.recvFrame(hdr, payload, fds, ScopedFdArrayGuard::MAX_ANCILLARY_FDS, numFds,
                               maxAllowed)) {
        closeAndFailPending(QStringLiteral("IPC frame receive failure"));
        return;
    }

    ScopedFdArrayGuard fdGuard{fds, numFds};

    auto failMalformed = [&](IpcPendingRequest &req, const QString &errMsg) {
        WAYWAL_LOG_ERR("IpcClient: %s", errMsg.toUtf8().constData());
        dispatchRequestFailure(std::move(req), errMsg, true);
    };

    if (hdr.opcode == WAYWAL_RESP_EVENT) {
        const QByteArray rawJson(reinterpret_cast<const char *>(payload.data()),
                                 static_cast<qsizetype>(payload.size()));
        const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            const QString eventType =
                obj.value(QStringLiteral("event")).toString(QStringLiteral("unknown"));
            QMetaObject::invokeMethod(
                this, [this, eventType, obj]() { emit eventReceived(eventType, obj); },
                Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_ERR) {
        QString err = QStringLiteral("Daemon returned error");
        if (!payload.empty()) {
            const size_t len =
                ::strnlen(reinterpret_cast<const char *>(payload.data()), payload.size());
            err = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                    static_cast<qsizetype>(len));
        }

        IpcPendingRequest failedReq{};
        if (m_dispatcher.popFrontForError(failedReq)) {
            dispatchRequestFailure(std::move(failedReq), err, true);
        } else {
            (void)m_dispatcher.tryConsumeSubscribeAck();
            (void)m_dispatcher.tryConsumeUnackedUniform();
            QMetaObject::invokeMethod(
                this, [this, err]() { emit errorOccurred(err); }, Qt::QueuedConnection);
        }
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_DMABUF) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_EXPORT_OUTPUT_DMABUF, req);
        waywal_export_dmabuf_resp_t desc{};
        QVector<int> transferred;
        QString errMsg;
        if (!IpcServiceCodecs::decodeDmabufExport(payload, fds, numFds, desc, transferred,
                                                  &errMsg)) {
            failMalformed(req, errMsg);
            return;
        }
        auto guard = std::make_shared<QueuedFdGuard>();
        guard->fds = std::move(transferred);
        QMetaObject::invokeMethod(
            this,
            [this, desc, guard, cb = std::move(req.dmabufCallback)]() {
                const bool hasSignalListeners =
                    receivers(SIGNAL(dmabufReceived(waywal_export_dmabuf_resp_t, QVector<int>))) >
                    0;
                if (!cb && !hasSignalListeners)
                    return;
                guard->handedOff = true;
                if (cb)
                    cb(desc, guard->fds);
                if (hasSignalListeners)
                    emit dmabufReceived(desc, guard->fds);
            },
            Qt::QueuedConnection);
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_TELEMETRY) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_GET_TELEMETRY, req);
        TelemetrySnapshot snap{};
        QString errMsg;
        if (!IpcServiceCodecs::decodeTelemetrySnapshot(payload, snap, &errMsg)) {
            failMalformed(req, errMsg);
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, snap = std::move(snap), cb = std::move(req.telemetryCallback)]() {
                if (cb)
                    cb(snap);
                emit telemetryReceived(snap);
            },
            Qt::QueuedConnection);
        return;
    }

    auto handleFixedStruct = [&](uint16_t expectedReqOpcode, size_t expectedSize, auto validateFn,
                                 const QString &malMsg, auto dispatchOk) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(expectedReqOpcode, req);
        if (payload.size() != expectedSize) {
            failMalformed(req, malMsg);
            return;
        }
        if (!validateFn(req)) {
            failMalformed(req, malMsg);
        } else {
            dispatchOk(std::move(req));
        }
    };

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_SYNC) {
        waywal_ipc_resp_workshop_sync_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_WORKSHOP_SYNC, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_workshop_sync(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_WORKSHOP_SYNC"),
            [&](IpcPendingRequest req) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.workshopSyncCallback)]() {
                        if (cb)
                            cb(resp);
                        emit workshopSyncReceived(resp);
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WORKSHOP_DOWNLOAD) {
        waywal_ipc_resp_workshop_download_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_WORKSHOP_DOWNLOAD, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_workshop_download(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_WORKSHOP_DOWNLOAD"),
            [&](IpcPendingRequest req) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.workshopDownloadCallback)]() {
                        if (cb)
                            cb(resp);
                        emit workshopDownloadReceived(resp);
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_WWPKG_INSTALL) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_WWPKG_INSTALL, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_wwpkg_install_t)) {
            waywal_ipc_resp_wwpkg_install_t resp{};
            const char *errPtr = nullptr;
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_wwpkg_install(
                    reinterpret_cast<const waywal_ipc_resp_wwpkg_install_t *>(payload.data()),
                    payload.size(), &errPtr)) {
                QString errMsg;
                if (errPtr && resp.error_msg_len > 0) {
                    errMsg = QString::fromUtf8(
                        errPtr, static_cast<qsizetype>(::strnlen(errPtr, resp.error_msg_len)));
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, errMsg, cb = std::move(req.wwpkgInstallCallback)]() {
                        if (cb)
                            cb(resp, errMsg);
                        emit wwpkgInstallReceived(resp, errMsg);
                    },
                    Qt::QueuedConnection);
                return;
            }
        }
        failMalformed(req, QStringLiteral("Malformed WAYWAL_RESP_WWPKG_INSTALL payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SCAN_COMPAT) {
        waywal_ipc_resp_scan_compat_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SCAN_COMPAT, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_scan_compat(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SCAN_COMPAT"),
            [&](IpcPendingRequest req) {
                QMetaObject::invokeMethod(
                    this,
                    [this, resp, cb = std::move(req.scanCompatCallback)]() {
                        if (cb)
                            cb(resp);
                        emit scanCompatReceived(resp);
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_COMPILE_SHADER_GRAPH) {
        waywal_ipc_resp_compile_shader_graph_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_COMPILE_SHADER_GRAPH, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_compile_shader_graph(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_COMPILE_SHADER_GRAPH payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit shaderGraphCompiled(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_CAMERA_3D) {
        waywal_ipc_resp_set_camera_3d_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SET_CAMERA_3D, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_set_camera_3d(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SET_CAMERA_3D payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit camera3DUpdated(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_PHYSICS_PARAMS) {
        waywal_ipc_resp_set_physics_params_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SET_PHYSICS_PARAMS, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_set_physics_params(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SET_PHYSICS_PARAMS payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit physicsParamsUpdated(resp); },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_SCENE_LIGHTS) {
        waywal_ipc_resp_set_scene_lights_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SET_SCENE_LIGHTS, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_set_scene_lights(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SET_SCENE_LIGHTS payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneLightsUpdated(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SCENE_MUTATE) {
        waywal_ipc_resp_scene_mutate_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SCENE_MUTATE, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_scene_mutate(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SCENE_MUTATE payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneMutated(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_HOT_RELOAD_CODE) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_HOT_RELOAD_CODE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_hot_reload_code_t)) {
            waywal_ipc_resp_hot_reload_code_t resp{};
            const char *jsonPtr = nullptr;
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_hot_reload_code(
                    reinterpret_cast<const waywal_ipc_resp_hot_reload_code_t *>(payload.data()),
                    payload.size(), &jsonPtr)) {
                QString metaJson;
                if (jsonPtr && resp.meta_json_len > 0)
                    metaJson =
                        QString::fromUtf8(jsonPtr, static_cast<qsizetype>(resp.meta_json_len));
                QMetaObject::invokeMethod(
                    this, [this, resp, metaJson]() { emit codeHotReloaded(resp, metaJson); },
                    Qt::QueuedConnection);
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_HOT_RELOAD_CODE payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_QUERY_SCENE_TREE) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_QUERY_SCENE_TREE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_query_scene_tree_t)) {
            waywal_ipc_resp_query_scene_tree_t resp{};
            const char *jsonPtr = nullptr;
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_query_scene_tree(
                    reinterpret_cast<const waywal_ipc_resp_query_scene_tree_t *>(payload.data()),
                    payload.size(), &jsonPtr)) {
                QString treeJson;
                if (jsonPtr && resp.json_len > 0)
                    treeJson = QString::fromUtf8(jsonPtr, static_cast<qsizetype>(resp.json_len));
                QMetaObject::invokeMethod(
                    this, [this, resp, treeJson]() { emit sceneTreeQueried(resp, treeJson); },
                    Qt::QueuedConnection);
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_QUERY_SCENE_TREE payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_SHADER_COMBO) {
        waywal_ipc_resp_set_shader_combo_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SET_SHADER_COMBO, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_set_shader_combo(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SET_SHADER_COMBO payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit shaderComboUpdated(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_BRUSH_STROKE) {
        waywal_ipc_resp_brush_stroke_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_BRUSH_STROKE, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_brush_stroke(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_BRUSH_STROKE payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit brushStrokeCompleted(resp); },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PARTICLE_CONFIG) {
        waywal_ipc_resp_particle_config_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_PARTICLE_CONFIG, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_particle_config(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_PARTICLE_CONFIG payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit particleConfigUpdated(resp); },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_SET_AUDIO_BINDINGS) {
        waywal_ipc_resp_set_audio_bindings_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_SET_AUDIO_BINDINGS, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_set_audio_bindings(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_SET_AUDIO_BINDINGS payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit audioBindingsUpdated(resp); },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_EXPORT_SCENE_PKG) {
        waywal_ipc_resp_export_scene_pkg_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_EXPORT_SCENE_PKG, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_export_scene_pkg(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_EXPORT_SCENE_PKG payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit scenePkgExported(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_LINT_SCENESCRIPT) {
        waywal_ipc_resp_lint_scenescript_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_LINT_SCENESCRIPT, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return waywal_ipc_validate_resp_lint_scenescript(&resp, payload.size());
            },
            QStringLiteral("Invalid WAYWAL_RESP_LINT_SCENESCRIPT payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit sceneScriptLinted(resp); }, Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_READBACK_TEXTURE) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_READBACK_TEXTURE, req);
        if (payload.size() >= sizeof(waywal_ipc_resp_readback_texture_t)) {
            waywal_ipc_resp_readback_texture_t resp{};
            const void *pxPtr = nullptr;
            uint32_t pxLen = 0;
            std::memcpy(&resp, payload.data(), sizeof(resp));
            if (waywal_ipc_validate_resp_readback_texture(
                    reinterpret_cast<const waywal_ipc_resp_readback_texture_t *>(payload.data()),
                    payload.size(), &pxPtr, &pxLen)) {
                QByteArray bytes;
                if (pxPtr && pxLen > 0) {
                    bytes = QByteArray(static_cast<const char *>(pxPtr),
                                       static_cast<qsizetype>(pxLen));
                }
                QMetaObject::invokeMethod(
                    this, [this, resp, bytes]() { emit textureReadbackReceived(resp, bytes); },
                    Qt::QueuedConnection);
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_READBACK_TEXTURE payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_MESH_ACK) {
        waywal_ipc_puppet_mesh_ack_t resp{};
        handleFixedStruct(
            WAYWAL_REQ_PUPPET_SET_MESH, sizeof(resp),
            [&](IpcPendingRequest &) {
                std::memcpy(&resp, payload.data(), sizeof(resp));
                return true;
            },
            QStringLiteral("Invalid WAYWAL_RESP_PUPPET_MESH_ACK payload"),
            [&](IpcPendingRequest) {
                QMetaObject::invokeMethod(
                    this, [this, resp]() { emit puppetMeshAckReceived(resp); },
                    Qt::QueuedConnection);
            });
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_WEIGHTS) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS, req);
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
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_PUPPET_WEIGHTS payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_PUPPET_IK_POSE) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_PUPPET_PREVIEW_IK, req);
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
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_PUPPET_IK_POSE payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_EXPORT_PUPPET_MDL) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_EXPORT_PUPPET_MDL, req);
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
                return;
            }
        }
        failMalformed(req, QStringLiteral("Invalid WAYWAL_RESP_EXPORT_PUPPET_MDL payload"));
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_INFO) {
        IpcPendingRequest req{};
        (void)m_dispatcher.popPendingForOpcode(WAYWAL_REQ_QUERY_JSON, req);
        const QString jsonStr = QString::fromUtf8(reinterpret_cast<const char *>(payload.data()),
                                                  static_cast<qsizetype>(payload.size()));
        QMetaObject::invokeMethod(
            this,
            [this, jsonStr, cb = std::move(req.jsonCallback)]() {
                if (cb)
                    cb(jsonStr);
                emit outputsJsonReceived(jsonStr);
            },
            Qt::QueuedConnection);
        return;
    }

    if (hdr.opcode == WAYWAL_RESP_OK) {
        if (!payload.empty() && m_dispatcher.tryConsumeSubscribeAck()) {
            const QByteArray rawJson(reinterpret_cast<const char *>(payload.data()),
                                     static_cast<qsizetype>(payload.size()));
            const QString jsonStr = QString::fromUtf8(rawJson);
            const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
            const bool hasObj = doc.isObject();
            const QJsonObject obj = hasObj ? doc.object() : QJsonObject{};
            const QString eventType =
                obj.value(QStringLiteral("event")).toString(QStringLiteral("initial"));
            QMetaObject::invokeMethod(
                this,
                [this, jsonStr, hasObj, eventType, obj]() {
                    emit outputsJsonReceived(jsonStr);
                    if (hasObj)
                        emit eventReceived(eventType, obj);
                },
                Qt::QueuedConnection);
            return;
        }

        IpcPendingRequest req{};
        if (m_dispatcher.popNextGenericOk(req))
            return;

        if (!m_dispatcher.tryConsumeUnackedUniform())
            (void)m_dispatcher.tryConsumeSubscribeAck();
        return;
    }
}

/* ========================================================================= */
/* Public Sender & Request Methods                                           */
/* ========================================================================= */

void IpcClient::sendSetUniform(const QString &output, const QString &name, const float *vals,
                               uint32_t count, bool persist)
{
    waywal_set_uniform_payload_t payload{};
    QString err;
    if (!IpcServiceCodecs::encodeSetUniform(output, name, vals, count, persist, payload, &err)) {
        if (!err.isEmpty() && !m_destroying.load())
            emit errorOccurred(err);
        return;
    }

    const std::string key = output.toStdString() + ":" + name.toStdString();
    if (m_dispatcher.coalesceUniform(key, payload))
        m_transport.wakeup();
}

void IpcClient::sendSetImage(const waywal_img_metadata_t &meta, int memfd)
{
    sendSetImageWithExtra(meta, {}, {}, memfd);
}

void IpcClient::sendSetImageWithExtra(const waywal_img_metadata_t &meta,
                                      const QStringList &targetOutputs,
                                      const QString &customShaderGlsl, int memfd)
{
    if (memfd >= 0) {
        QString err;
        waywal_ipc_hdr_t hdr{};
        ScopedFd dupedFd;
        if (!IpcServiceCodecs::prepareSetImagePacket(meta, targetOutputs, customShaderGlsl, memfd,
                                                     hdr, dupedFd, &err)) {
            if (!m_destroying.load())
                emit errorOccurred(err);
            return;
        }
        IpcOutboundPacket pkt{};
        pkt.kind = IpcOutboundPacket::Kind::Standard;
        pkt.hdr = hdr;
        pkt.attachedFd = std::move(dupedFd);
        enqueueAndWake(std::move(pkt));
        return;
    }

    if (meta.extra_data_len != 0 && targetOutputs.isEmpty() && customShaderGlsl.isEmpty()) {
        if (!m_destroying.load()) {
            emit errorOccurred(
                QStringLiteral("sendSetImage rejected inconsistent extra_data_len without payload"));
        }
        return;
    }

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::DeferredSetImage;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SET_IMAGE;
    pkt.imgMeta = meta;
    pkt.imgTargetOutputs = targetOutputs;
    pkt.imgCustomShader = customShaderGlsl;
    enqueueAndWake(std::move(pkt));
}

void IpcClient::setWallpaper(const WallpaperApplyOptions &opts)
{
    sendSetImageWithExtra(opts.toMetadata(), opts.targetOutputs, opts.customShaderGlsl, -1);
}

void IpcClient::sendAudioPowerConfig(float volume, int8_t muteOnAudio, int8_t themeFromMedia,
                                     uint8_t batteryPolicy)
{
    waywal_audio_ctrl_payload_t apay{};
    QString err;
    if (!IpcServiceCodecs::encodeAudioPowerConfig(volume, muteOnAudio, themeFromMedia,
                                                  batteryPolicy, apay, &err)) {
        if (!m_destroying.load())
            emit errorOccurred(err);
        return;
    }
    (void)enqueueRawStruct(WAYWAL_REQ_AUDIO_CTRL, &apay, sizeof(apay));
}

void IpcClient::requestOutputsJson(std::function<void(const QString &json)> callback)
{
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_QUERY_JSON;
    pkt.hdr.payload_size = 0;
    pkt.jsonCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
}

void IpcClient::requestTelemetry(std::function<void(const TelemetrySnapshot &snapshot)> callback)
{
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_GET_TELEMETRY;
    pkt.hdr.payload_size = 0;
    pkt.telemetryCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
}

void IpcClient::requestOutputDmabuf(
    const QString &output,
    std::function<void(const waywal_export_dmabuf_resp_t &desc, const QVector<int> &fds)> callback)
{
    waywal_export_dmabuf_req_t req{};
    const QByteArray outBa = output.toUtf8();
    const size_t n = std::min(static_cast<size_t>(outBa.size()), sizeof(req.output) - 1u);
    if (n > 0)
        std::memcpy(req.output, outBa.constData(), n);

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_EXPORT_OUTPUT_DMABUF;
    pkt.hdr.payload_size = sizeof(req);
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.dmabufCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
}

void IpcClient::subscribeEvents()
{
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Subscribe;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_SUBSCRIBE;
    pkt.hdr.payload_size = 0;
    enqueueAndWake(std::move(pkt));
}

void IpcClient::requestWorkshopSync(
    uint32_t flags, const QString &customRoot,
    std::function<void(const waywal_ipc_resp_workshop_sync_t &resp)> callback)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_workshop_sync_t req{};
    const QByteArray rootBa = customRoot.toUtf8();
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

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.workshopSyncCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
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

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.workshopDownloadCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
}

void IpcClient::requestWwpkgInstall(
    const uint8_t packageUuid[16], uint32_t flags, const QString &targetOutput,
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
        callback,
    int wwpkgFd)
{
    uint8_t zeroUuid[16]{};
    const uint8_t *uuidPtr = packageUuid ? packageUuid : zeroUuid;
    const QByteArray outBa = targetOutput.toUtf8();

    const size_t bufCap =
        sizeof(waywal_ipc_req_wwpkg_install_t) + static_cast<size_t>(outBa.size()) + 2u;
    std::vector<uint8_t> payloadBuf(bufCap, 0);
    size_t totalLen = 0;
    waywal_ipc_hdr_t hdr{};

    auto failImmediate = [&](uint32_t statusCode, const QString &errMsg) {
        QMetaObject::invokeMethod(
            this,
            [this, uuidVec = std::vector<uint8_t>(uuidPtr, uuidPtr + 16), statusCode, errMsg,
             cb = std::move(callback)]() {
                waywal_ipc_resp_wwpkg_install_t resp{};
                std::memcpy(resp.package_uuid, uuidVec.data(), 16);
                resp.status_code = statusCode;
                if (cb)
                    cb(resp, errMsg);
                emit wwpkgInstallReceived(resp, errMsg);
                emit errorOccurred(errMsg);
            },
            Qt::QueuedConnection);
    };

    if (!waywal_ipc_serialize_req_wwpkg_install(
            uuidPtr, flags, outBa.isEmpty() ? nullptr : outBa.constData(), &hdr, nullptr,
            payloadBuf.data(), payloadBuf.size(), &totalLen)) {
        failImmediate(1u, QStringLiteral("Target output exceeds maximum IPC string length"));
        return;
    }

    IpcOutboundPacket pkt{};
    pkt.hdr = hdr;
    pkt.payload.assign(payloadBuf.begin(), payloadBuf.begin() + static_cast<ptrdiff_t>(totalLen));
    pkt.wwpkgInstallCallback = std::move(callback);

    if (wwpkgFd >= 0) {
        ScopedFd duped(IpcServiceCodecs::dupCloexec(wwpkgFd));
        if (!duped.valid()) {
            failImmediate(2u, QStringLiteral("Failed to duplicate .wwpkg file descriptor"));
            return;
        }
        pkt.kind = IpcOutboundPacket::Kind::Standard;
        pkt.attachedFd = std::move(duped);
    } else {
        pkt.kind = IpcOutboundPacket::Kind::DeferredWwpkgHubLookup;
        std::memcpy(pkt.hubPackageUuid, uuidPtr, 16);
    }

    enqueueAndWake(std::move(pkt));
}

void IpcClient::installWwpkg(
    const QString &packagePath, const QString &targetOutput, uint32_t flags,
    std::function<void(const waywal_ipc_resp_wwpkg_install_t &resp, const QString &errorMsg)>
        callback)
{
    const QByteArray pathBa = packagePath.toUtf8();
    ScopedFd fd(pathBa.isEmpty() ? -1 : ::open(pathBa.constData(), O_RDONLY | O_CLOEXEC));
    struct stat st{};
    if (!fd.valid() || ::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        const QString errMsg = QStringLiteral("Unable to open .wwpkg file: %1").arg(packagePath);
        QMetaObject::invokeMethod(
            this,
            [this, errMsg, cb = std::move(callback)]() {
                waywal_ipc_resp_wwpkg_install_t resp{};
                resp.status_code = 2u;
                if (cb)
                    cb(resp, errMsg);
                emit wwpkgInstallReceived(resp, errMsg);
                emit errorOccurred(errMsg);
            },
            Qt::QueuedConnection);
        return;
    }

    uint8_t pkgUuid[16]{};
    // Parse the manifest through the already-opened descriptor to avoid a double open TOCTOU race.
    wwpkg_package_t *pkg = wwpkg_open_fd(fd.get());
    if (pkg) {
        const wwpkg_manifest_t *man = wwpkg_get_manifest(pkg);
        if (man)
            (void)IpcServiceCodecs::parseUuidString(man->id, pkgUuid);
        wwpkg_close(pkg);
    }

    QString normOutput = targetOutput.trimmed();
    if (normOutput.isEmpty() ||
        normOutput.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0) {
        normOutput = QStringLiteral("*");
    }

    requestWwpkgInstall(pkgUuid, flags, normOutput, std::move(callback), fd.get());
}

void IpcClient::requestScanCompat(
    const QString &targetPath, uint32_t autoRepairFlags,
    std::function<void(const waywal_ipc_resp_scan_compat_t &resp)> callback)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_scan_compat_t req{};
    const QByteArray pathBa = targetPath.toUtf8();
    if (!waywal_ipc_serialize_req_scan_compat(pathBa.constData(), autoRepairFlags, &hdr, &req)) {
        if (callback) {
            waywal_ipc_resp_scan_compat_t resp{};
            resp.status_code = 1u;
            QMetaObject::invokeMethod(
                this, [cb = std::move(callback), resp]() { cb(resp); }, Qt::QueuedConnection);
        }
        return;
    }

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr = hdr;
    pkt.payload.resize(sizeof(req));
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    pkt.scanCompatCallback = std::move(callback);
    enqueueAndWake(std::move(pkt));
}

bool IpcClient::requestCompileShaderGraph(const QString &output, const QString &path,
                                          const QString &inlineJson, uint32_t flags,
                                          waywal_ipc_resp_compile_shader_graph_t *outResp)
{
    const QByteArray outBa = output.toUtf8();
    const QByteArray pathBa = path.toUtf8();
    const QByteArray jsonBa = inlineJson.toUtf8();

    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_compile_shader_graph_t req{};
    std::vector<uint8_t> sendBuf(sizeof(hdr) + sizeof(req) + static_cast<size_t>(jsonBa.size()));
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_compile_shader_graph(
            outBa.isEmpty() ? "*" : outBa.constData(), pathBa.constData(),
            jsonBa.isEmpty() ? nullptr : jsonBa.constData(), flags, &hdr, &req, sendBuf.data(),
            sendBuf.size(), &totalLen)) {
        if (outResp) {
            std::memset(outResp, 0, sizeof(*outResp));
            outResp->status_code = 1u;
            std::snprintf(outResp->error_msg, sizeof(outResp->error_msg),
                          "Shader graph request serialization failed");
        }
        return false;
    }

    // Delivery is strictly asynchronous via shaderGraphCompiled(resp); avoid writing
    // status_code=0 into outResp because callers must not mistake queued dispatch for OK.
    if (outResp) {
        std::memset(outResp, 0, sizeof(*outResp));
        outResp->status_code = WAYWAL_STATUS_ERR_SYNTAX;
        std::snprintf(outResp->error_msg, sizeof(outResp->error_msg),
                      "Asynchronous compile queued; observe shaderGraphCompiled signal");
    }

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr = hdr;
    pkt.payload.resize(totalLen - sizeof(hdr));
    std::memcpy(pkt.payload.data(), sendBuf.data() + sizeof(hdr), pkt.payload.size());
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendSetCamera3D(const QString &output, const we_camera3d_t &cam)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_camera_3d_t req{};
    const QByteArray outBa = output.toUtf8();
    uint32_t flags = 0;
    if (cam.dof_enabled > 0.5f)
        flags |= WAYWAL_CAMERA_FLAG_ENABLE_DOF;
    if (cam.path_enabled != 0)
        flags |= WAYWAL_CAMERA_FLAG_ENABLE_KEYFRAMES;

    if (!waywal_ipc_serialize_req_set_camera_3d(
            outBa.isEmpty() ? "*" : outBa.constData(), cam.projection_mode, flags, cam.eye_pos,
            cam.target_pos, cam.up_vec, cam.fov_y_deg, cam.near_z, cam.far_z, cam.parallax_amount,
            cam.parallax_smoothing, cam.dof_focal_dist, cam.dof_focal_region,
            cam.dof_bokeh_radius_px, &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendSetPhysicsParams(const QString &output, int32_t objectId, int32_t boneIndex,
                                     const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                                     const we_cursor_force_t &cursor)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_physics_params_t req{};
    const QByteArray outBa = output.toUtf8();
    if (!waywal_ipc_serialize_req_set_physics_params(outBa.isEmpty() ? "*" : outBa.constData(),
                                                     objectId, boneIndex, &cfg, &wind, &cursor,
                                                     &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendSetSceneLights(const QString &output, const we_lighting_uniforms_t &lights)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_scene_lights_t req{};
    const QByteArray outBa = output.toUtf8();
    if (!waywal_ipc_serialize_req_set_scene_lights(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   &lights, &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendSceneMutate(const waywal_ipc_req_scene_mutate_t &req,
                                const QString &payloadJson)
{
    const QByteArray jsonBa = payloadJson.toUtf8();
    const char *jsonPtr = jsonBa.isEmpty() ? nullptr : jsonBa.constData();
    const size_t cap = sizeof(waywal_ipc_req_scene_mutate_t) + static_cast<size_t>(jsonBa.size());
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_scene_mutate(&req, jsonPtr, &pkt.hdr, nullptr, pkt.payload.data(),
                                               pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendHotReloadCode(const waywal_ipc_req_hot_reload_code_t &req,
                                  const QByteArray &utf8Source)
{
    const char *targetOut = req.target_output[0] != '\0' ? req.target_output : "*";
    const size_t cap =
        sizeof(waywal_ipc_req_hot_reload_code_t) + static_cast<size_t>(utf8Source.size());
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
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
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendQuerySceneTree(const QString &output, uint32_t flags)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_query_scene_tree_t req{};
    const QByteArray outBa = output.toUtf8();
    if (!waywal_ipc_serialize_req_query_scene_tree(outBa.isEmpty() ? "*" : outBa.constData(), flags,
                                                   &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendSetShaderCombo(const QString &output, int32_t objectId, uint32_t passIndex,
                                   const QString &comboName, int32_t comboValue, uint32_t flags)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_set_shader_combo_t req{};
    const QByteArray outBa = output.toUtf8();
    const QByteArray comboBa = comboName.toUtf8();
    if (!waywal_ipc_serialize_req_set_shader_combo(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   objectId, passIndex, comboBa.constData(),
                                                   comboValue, flags, &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendBrushStroke(const waywal_ipc_req_brush_stroke_t &req,
                                const std::vector<we_brush_dab_t> &dabs)
{
    const char *targetOut = req.target_output[0] != '\0' ? req.target_output : "*";
    const size_t cap = sizeof(waywal_ipc_req_brush_stroke_t) + dabs.size() * sizeof(we_brush_dab_t);
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_brush_stroke(
            targetOut, req.action, req.object_id, req.pass_index, req.tex_slot, req.stroke_id,
            &req.params, dabs.empty() ? nullptr : dabs.data(), static_cast<uint32_t>(dabs.size()),
            &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendParticleConfig(const QString &output, int32_t objectId, uint32_t configOp,
                                   uint32_t presetId, uint32_t flags, const void *payload,
                                   uint32_t payloadLen)
{
    const QByteArray outBa = output.toUtf8();
    const size_t cap = sizeof(waywal_ipc_req_particle_config_t) + static_cast<size_t>(payloadLen);
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_particle_config(
            outBa.isEmpty() ? "*" : outBa.constData(), objectId, configOp, presetId, flags, payload,
            payloadLen, &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendAudioBindings(const QString &output, uint32_t bindOp,
                                  const we_audio_binding_t *bindings, uint32_t count,
                                  uint32_t removeId)
{
    const QByteArray outBa = output.toUtf8();
    const size_t cap = sizeof(waywal_ipc_req_set_audio_bindings_t) +
                       static_cast<size_t>(count) * sizeof(we_audio_binding_t);
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_set_audio_bindings(
            outBa.isEmpty() ? "*" : outBa.constData(), bindOp, removeId, bindings, count, &pkt.hdr,
            nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendExportScenePkg(const QString &output, const QString &destPath, uint32_t flags)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_export_scene_pkg_t req{};
    const QByteArray outBa = output.toUtf8();
    const QByteArray dstBa = destPath.toUtf8();
    if (!waywal_ipc_serialize_req_export_scene_pkg(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   dstBa.constData(), flags, &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

bool IpcClient::sendLintScenescript(const QString &sourceText, uint32_t flags)
{
    const QByteArray srcBa = sourceText.toUtf8();
    const size_t cap =
        sizeof(waywal_ipc_req_lint_scenescript_t) + static_cast<size_t>(srcBa.size());
    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.payload.resize(cap);
    size_t totalLen = 0;

    if (!waywal_ipc_serialize_req_lint_scenescript(
            srcBa.isEmpty() ? "" : srcBa.constData(), static_cast<size_t>(srcBa.size()), flags,
            &pkt.hdr, nullptr, pkt.payload.data(), pkt.payload.size(), &totalLen)) {
        return false;
    }
    pkt.payload.resize(totalLen);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendReadbackTexture(const QString &output, int32_t objectId, uint32_t passIndex,
                                    uint32_t texSlot, uint32_t formatMode)
{
    waywal_ipc_hdr_t hdr{};
    waywal_ipc_req_readback_texture_t req{};
    const QByteArray outBa = output.toUtf8();
    if (!waywal_ipc_serialize_req_readback_texture(outBa.isEmpty() ? "*" : outBa.constData(),
                                                   objectId, passIndex, texSlot, formatMode,
                                                   &hdr, &req)) {
        return false;
    }
    return enqueueRawStruct(hdr.opcode, &req, sizeof(req));
}

QString IpcClient::stagePuppetMdlExportPath(const QString &fileHint)
{
    return IpcServiceCodecs::stagePuppetMdlExportPath(fileHint);
}

bool IpcClient::sendPuppetSetMesh(uint32_t objectId, uint32_t flags,
                                  const std::vector<waywal_ipc_mdl_vertex_t> &vertices,
                                  const std::vector<uint16_t> &indices)
{
    const bool clearMesh = vertices.empty() && indices.empty();
    if (!clearMesh) {
        if (vertices.size() < 3 || vertices.size() > WAYWAL_IPC_RIGGING_MAX_VERTICES ||
            indices.size() < 3 || (indices.size() % 3) != 0) {
            return false;
        }
    }

    const uint32_t vertCount = static_cast<uint32_t>(vertices.size());
    const uint32_t triCount = static_cast<uint32_t>(indices.size() / 3);
    if (triCount > WAYWAL_IPC_RIGGING_MAX_TRIANGLES)
        return false;

    size_t totalBytes = sizeof(waywal_ipc_puppet_set_mesh_t);
    if (!clearMesh) {
        if (!waywal_ipc_rigging_calc_mesh_bytes(vertCount, triCount, &totalBytes) ||
            totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES) {
            return false;
        }
    }

    for (const auto &v : vertices) {
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(v.pos[k]))
                return false;
        }
        for (int k = 0; k < 4; ++k) {
            if (!std::isfinite(v.weight[k]))
                return false;
        }
        if (!std::isfinite(v.texcoord[0]) || !std::isfinite(v.texcoord[1]))
            return false;
    }
    for (uint16_t idx : indices) {
        if (idx >= vertCount)
            return false;
    }

    waywal_ipc_puppet_set_mesh_t req{};
    req.object_id = objectId;
    req.vertex_count = vertCount;
    req.triangle_count = triCount;
    req.flags = flags;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_MESH;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
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
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendPuppetSetMesh(uint32_t objectId, const std::vector<we_mdl_vertex_t> &vertices,
                                  const std::vector<uint16_t> &indices, uint32_t flags)
{
    std::vector<waywal_ipc_mdl_vertex_t> wireVerts(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto &src = vertices[i];
        auto &dst = wireVerts[i];
        for (int k = 0; k < 3; ++k)
            dst.pos[k] = src.pos[k];
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
    if (bones.size() > WAYWAL_IPC_RIGGING_MAX_BONES)
        return false;

    const size_t boneBytes = bones.size() * sizeof(waywal_ipc_mdl_bone_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_set_skeleton_t) + boneBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES)
        return false;

    for (size_t i = 0; i < bones.size(); ++i) {
        const auto &b = bones[i];
        if ((i == 0 && b.parent != 0xFFFFFFFFu) ||
            (i > 0 && b.parent != 0xFFFFFFFFu && b.parent >= i)) {
            return false;
        }
        if (std::memchr(b.name, '\0', sizeof(b.name)) == nullptr ||
            std::memchr(b.simulation_json, '\0', sizeof(b.simulation_json)) == nullptr) {
            return false;
        }
        for (float f : b.local_trans) {
            if (!std::isfinite(f))
                return false;
        }
    }

    waywal_ipc_puppet_set_skeleton_t req{};
    req.object_id = objectId;
    req.bone_count = static_cast<uint32_t>(bones.size());
    req.flags = flags;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_SKELETON;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (boneBytes > 0)
        std::memcpy(pkt.payload.data() + sizeof(req), bones.data(), boneBytes);
    enqueueAndWake(std::move(pkt));
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
        if (lockedVertexBitset.size() != req.locked_vert_word_count)
            return false;
    } else if (req.locked_vert_word_count != 0u || !lockedVertexBitset.empty()) {
        return false;
    }

    const size_t tailBytes = lockedVertexBitset.size() * sizeof(uint64_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_solve_weights_t) + tailBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES)
        return false;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SOLVE_WEIGHTS;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (tailBytes > 0)
        std::memcpy(pkt.payload.data() + sizeof(req), lockedVertexBitset.data(), tailBytes);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendPuppetPaintWeights(uint32_t objectId, uint32_t flags,
                                       const std::vector<waywal_ipc_vertex_weight_t> &entries)
{
    if (entries.empty() || entries.size() > WAYWAL_IPC_RIGGING_MAX_VERTICES)
        return false;

    const size_t entryBytes = entries.size() * sizeof(waywal_ipc_vertex_weight_t);
    const size_t totalBytes = sizeof(waywal_ipc_puppet_paint_weights_t) + entryBytes;
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES)
        return false;

    for (const auto &e : entries) {
        for (float w : e.weight) {
            if (!std::isfinite(w))
                return false;
        }
    }

    waywal_ipc_puppet_paint_weights_t req{};
    req.object_id = objectId;
    req.entry_count = static_cast<uint32_t>(entries.size());
    req.flags = flags;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_PAINT_WEIGHTS;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    std::memcpy(pkt.payload.data() + sizeof(req), entries.data(), entryBytes);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendPuppetHeatmapMode(uint32_t objectId, bool enabled, uint16_t palette,
                                      uint32_t activeBoneIdx, float opacity)
{
    if (palette > 2u || !std::isfinite(opacity))
        return false;

    waywal_ipc_puppet_heatmap_mode_t req{};
    req.object_id = objectId;
    req.enabled = enabled ? 1u : 0u;
    req.palette = palette;
    req.active_bone_idx = activeBoneIdx;
    req.opacity = std::clamp(opacity, 0.0f, 1.0f);
    return enqueueRawStruct(WAYWAL_REQ_PUPPET_SET_HEATMAP_MODE, &req, sizeof(req));
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
        if (!std::isfinite(f))
            return false;
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
    if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES)
        return false;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_PREVIEW_IK;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
    pkt.payload.resize(totalBytes);
    std::memcpy(pkt.payload.data(), &req, sizeof(req));
    if (tailBytes > 0)
        std::memcpy(pkt.payload.data() + sizeof(req), overrides.data(), tailBytes);
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendPuppetSetAnimTracks(uint32_t objectId, uint32_t flags,
                                        const std::vector<PuppetAnimClipData> &clips)
{
    if (clips.size() > WAYWAL_IPC_RIGGING_MAX_ANIMS)
        return false;

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
            if (totalKeyframes > WAYWAL_IPC_RIGGING_MAX_KEYFRAMES)
                return false;
            for (const auto &kf : track.keyframes) {
                for (int k = 0; k < 3; ++k) {
                    if (!std::isfinite(kf.pos[k]) || !std::isfinite(kf.angle[k]) ||
                        !std::isfinite(kf.scale[k])) {
                        return false;
                    }
                }
            }
            totalBytes += track.keyframes.size() * sizeof(waywal_ipc_mdl_keyframe_t);
            if (totalBytes > WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES)
                return false;
        }
    }

    waywal_ipc_puppet_set_anim_tracks_t req{};
    req.object_id = objectId;
    req.clip_count = static_cast<uint32_t>(clips.size());
    req.flags = flags;

    IpcOutboundPacket pkt{};
    pkt.kind = IpcOutboundPacket::Kind::Standard;
    pkt.hdr.magic = WAYWAL_IPC_MAGIC;
    pkt.hdr.version = WAYWAL_IPC_VERSION;
    pkt.hdr.opcode = WAYWAL_REQ_PUPPET_SET_ANIM_TRACKS;
    pkt.hdr.payload_size = static_cast<uint64_t>(totalBytes);
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
    enqueueAndWake(std::move(pkt));
    return true;
}

bool IpcClient::sendPuppetScrubTime(uint32_t objectId, float timeSec, bool pausePlayback,
                                    uint32_t flags)
{
    if (!std::isfinite(timeSec))
        return false;

    waywal_ipc_puppet_scrub_t req{};
    req.object_id = objectId;
    req.time_sec = timeSec;
    req.pause_playback = pausePlayback ? 1u : 0u;
    req.flags = flags;
    return enqueueRawStruct(WAYWAL_REQ_PUPPET_SCRUB_TIME, &req, sizeof(req));
}

bool IpcClient::expectsGenericOkForOpcode(uint16_t reqOpcode) noexcept
{
    return IpcServiceCodecs::expectsGenericOkForOpcode(reqOpcode);
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
        const size_t resolvedLen = ::strnlen(resolved, sizeof(req.out_path) - 1u);
        std::memcpy(req.out_path, resolved, resolvedLen);
        req.out_path[resolvedLen] = '\0';
    }

    return enqueueRawStruct(WAYWAL_REQ_EXPORT_PUPPET_MDL, &req, sizeof(req));
}

} // namespace waywal::studio
