// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/rigging/dope_sheet_timeline_widget.hpp"

#include "waywal/bezier.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mesh_rig.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QWindowStateChangeEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace waywal::studio {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kPi = 3.14159265358979323846;
constexpr int kRulerHeightPx = 26;
constexpr double kMinZoomPxPerFrame = 2.0;
constexpr double kMaxZoomPxPerFrame = 80.0;

const QColor kSurface0(0x11, 0x11, 0x1b);
const QColor kSurface1(0x18, 0x18, 0x25);
const QColor kSurface2(0x1e, 0x1e, 0x2e);
const QColor kBorder(0x31, 0x32, 0x44);
const QColor kMajorGrid(0x45, 0x47, 0x5a);
const QColor kTextMuted(0x94, 0xa3, 0xb8);
const QColor kAccentSky(0x38, 0xbd, 0xf8);
const QColor kChanCoral(0xf3, 0x8b, 0xa8);
const QColor kChanMint(0xa6, 0xe3, 0xa1);
const QColor kChanBlue(0x89, 0xb4, 0xfa);
const QColor kAmber(0xf9, 0xe2, 0xaf);

[[nodiscard]] double finiteOr(double v, double fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

[[nodiscard]] float finiteOrF(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

[[nodiscard]] double channelDefaultValue(uint32_t channelIndex) noexcept
{
    return (channelIndex >= 6u && channelIndex <= 8u) ? 1.0 : 0.0;
}

[[nodiscard]] QString defaultChannelLabel(uint32_t channelIndex)
{
    static constexpr const char *kLabels[9] = {
        "pos.x", "pos.y", "pos.z", "angle.x", "angle.y", "angle.z", "scale.x", "scale.y", "scale.z",
    };
    if (channelIndex < 9u) {
        return QString::fromLatin1(kLabels[channelIndex]);
    }
    return QStringLiteral("ch.%1").arg(channelIndex);
}

[[nodiscard]] QColor channelColor(uint32_t channelIndex)
{
    switch (channelIndex % 3u) {
    case 0u:
        return kChanCoral;
    case 1u:
        return kChanMint;
    default:
        return kChanBlue;
    }
}

[[nodiscard]] QString animModeToString(we_mdl_anim_mode_t mode)
{
    switch (mode) {
    case WE_ANIM_MIRROR:
        return QStringLiteral("mirror");
    case WE_ANIM_SINGLE:
        return QStringLiteral("single");
    case WE_ANIM_LOOP:
    default:
        return QStringLiteral("loop");
    }
}

[[nodiscard]] we_mdl_anim_mode_t animModeFromString(const QString &str)
{
    if (str.compare(QStringLiteral("mirror"), Qt::CaseInsensitive) == 0) {
        return WE_ANIM_MIRROR;
    }
    if (str.compare(QStringLiteral("single"), Qt::CaseInsensitive) == 0) {
        return WE_ANIM_SINGLE;
    }
    return WE_ANIM_LOOP;
}

[[nodiscard]] QString blendModeToString(we_anim_blend_mode_t mode)
{
    return (mode == WE_ANIM_BLEND_ADDITIVE) ? QStringLiteral("additive")
                                            : QStringLiteral("override");
}

[[nodiscard]] we_anim_blend_mode_t blendModeFromString(const QString &str)
{
    if (str.compare(QStringLiteral("additive"), Qt::CaseInsensitive) == 0) {
        return WE_ANIM_BLEND_ADDITIVE;
    }
    return WE_ANIM_BLEND_OVERRIDE;
}

struct PreparedKey {
    double frame{0.0};
    double unwrappedValue{0.0};
    uint8_t tangentMode{0};
    double inDx{-1.0};
    double inDy{0.0};
    double outDx{1.0};
    double outDy{0.0};
};

struct PreparedChannel {
    std::vector<PreparedKey> keys;
};

[[nodiscard]] PreparedChannel prepareChannel(const TimelineFCurveChannel &channel)
{
    PreparedChannel prep;
    if (channel.keys.empty()) {
        return prep;
    }

    const size_t n = channel.keys.size();
    prep.keys.resize(n);

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }

    const bool alreadySorted =
        std::is_sorted(channel.keys.begin(), channel.keys.end(),
                       [](const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) {
                           return finiteOr(a.frame, 0.0) < finiteOr(b.frame, 0.0);
                       });
    if (!alreadySorted) {
        std::stable_sort(order.begin(), order.end(), [&channel](size_t ia, size_t ib) {
            return finiteOr(channel.keys[ia].frame, 0.0) < finiteOr(channel.keys[ib].frame, 0.0);
        });
    }

    const bool isAngle = (channel.channelIndex >= 3u && channel.channelIndex <= 5u);
    double prevRaw = 0.0;
    double prevUnwrapped = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const FCurveKeySnapshot &src = channel.keys[order[i]];
        PreparedKey &dst = prep.keys[i];
        dst.frame = finiteOr(src.frame, 0.0);
        const double rawVal = finiteOr(src.value, 0.0);

        // Preserve intentional multi-turn curves while eliminating +/- pi atan2 seam wraps.
        if (i == 0) {
            dst.unwrappedValue = rawVal;
        } else if (isAngle && std::abs(rawVal - prevRaw) > kPi &&
                   std::abs(rawVal) <= (kPi + 1e-3) && std::abs(prevRaw) <= (kPi + 1e-3)) {
            dst.unwrappedValue = prevUnwrapped + std::remainder(rawVal - prevRaw, kTwoPi);
        } else {
            dst.unwrappedValue = prevUnwrapped + (rawVal - prevRaw);
        }

        prevRaw = rawVal;
        prevUnwrapped = dst.unwrappedValue;

        dst.tangentMode = (src.tangentMode <= 3u) ? src.tangentMode : 0u;
        dst.inDx = std::min(0.0, finiteOr(src.handleInDx, -1.0));
        dst.inDy = finiteOr(src.handleInDy, 0.0);
        dst.outDx = std::max(0.0, finiteOr(src.handleOutDx, 1.0));
        dst.outDy = finiteOr(src.handleOutDy, 0.0);
    }

    // Compute Fritsch-Carlson monotonic Auto slopes and Vector aim handles.
    for (size_t i = 0; i < n; ++i) {
        PreparedKey &k = prep.keys[i];
        const double dxPrev = (i > 0) ? (k.frame - prep.keys[i - 1].frame) : 0.0;
        const double dxNext = (i + 1 < n) ? (prep.keys[i + 1].frame - k.frame) : 0.0;
        const double dyPrev = (i > 0) ? (k.unwrappedValue - prep.keys[i - 1].unwrappedValue) : 0.0;
        const double dyNext =
            (i + 1 < n) ? (prep.keys[i + 1].unwrappedValue - k.unwrappedValue) : 0.0;

        if (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Auto)) {
            double slope = 0.0;
            if (i > 0 && i + 1 < n && dxPrev > 1e-9 && dxNext > 1e-9) {
                const double s0 = dyPrev / dxPrev;
                const double s1 = dyNext / dxNext;
                if (s0 * s1 > 0.0) {
                    slope = (2.0 * s0 * s1) / (s0 + s1);
                }
            }
            k.inDx = (dxPrev > 1e-9) ? (-dxPrev / 3.0) : -1.0;
            k.inDy = slope * k.inDx;
            k.outDx = (dxNext > 1e-9) ? (dxNext / 3.0) : 1.0;
            k.outDy = slope * k.outDx;
        } else if (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Vector)) {
            k.inDx = (dxPrev > 1e-9) ? (-dxPrev / 3.0) : -1.0;
            k.inDy = (dxPrev > 1e-9) ? (-dyPrev / 3.0) : 0.0;
            k.outDx = (dxNext > 1e-9) ? (dxNext / 3.0) : 1.0;
            k.outDy = (dxNext > 1e-9) ? (dyNext / 3.0) : 0.0;
        }
    }

    return prep;
}

[[nodiscard]] double evaluatePreparedChannel(const PreparedChannel &prep, double frame)
{
    const size_t n = prep.keys.size();
    if (n == 0) {
        return 0.0;
    }
    const double q = finiteOr(frame, 0.0);
    if (n == 1 || q <= prep.keys.front().frame) {
        return prep.keys.front().unwrappedValue;
    }
    if (q >= prep.keys.back().frame) {
        return prep.keys.back().unwrappedValue;
    }

    const auto it =
        std::upper_bound(prep.keys.begin(), prep.keys.end(), q,
                         [](double val, const PreparedKey &k) { return val < k.frame; });

    if (it == prep.keys.begin()) {
        return prep.keys.front().unwrappedValue;
    }
    if (it == prep.keys.end()) {
        return prep.keys.back().unwrappedValue;
    }

    const PreparedKey &k0 = *(it - 1);
    const PreparedKey &k1 = *it;

    if (k0.tangentMode == static_cast<uint8_t>(KeyTangentMode::Stepped)) {
        return k0.unwrappedValue;
    }

    const double dx = k1.frame - k0.frame;
    if (dx <= 1e-9) {
        return k0.unwrappedValue;
    }

    if (k0.tangentMode == static_cast<uint8_t>(KeyTangentMode::Vector) &&
        k1.tangentMode == static_cast<uint8_t>(KeyTangentMode::Vector)) {
        const double u = std::clamp((q - k0.frame) / dx, 0.0, 1.0);
        return k0.unwrappedValue + u * (k1.unwrappedValue - k0.unwrappedValue);
    }

    double d0 = std::max(0.0, k0.outDx);
    double d1 = std::max(0.0, -k1.inDx);
    double dy0 = k0.outDy;
    double dy1 = k1.inDy;

    // Enforce x'(t) >= 0 across the segment so cubic Bezier x(t) remains single-valued.
    if (d0 + d1 > dx && (d0 + d1) > 1e-12) {
        const double scale = dx / (d0 + d1);
        d0 *= scale;
        d1 *= scale;
        dy0 *= scale;
        dy1 *= scale;
    }

    const float p1x = static_cast<float>(std::clamp(d0 / dx, 0.0, 1.0));
    const float p2x = static_cast<float>(std::clamp(1.0 - (d1 / dx), 0.0, 1.0));
    const double u = std::clamp((q - k0.frame) / dx, 0.0, 1.0);

    const bezier_curve_t xCurve = bezier_create(p1x, 0.0f, p2x, 1.0f);
    const double t =
        static_cast<double>(waywal_cubic_bezier_solve_t(&xCurve, static_cast<float>(u)));
    const double omt = 1.0 - t;

    const double y0 = k0.unwrappedValue;
    const double y1 = k0.unwrappedValue + dy0;
    const double y2 = k1.unwrappedValue + dy1;
    const double y3 = k1.unwrappedValue;

    return (omt * omt * omt * y0) + (3.0 * omt * omt * t * y1) + (3.0 * omt * t * t * y2) +
           (t * t * t * y3);
}

struct SelectedKeyRef {
    int32_t boneIndex{-1};
    uint32_t channelIndex{0};
    double frame{0.0};

    [[nodiscard]] bool matches(int32_t b, uint32_t ch, double f) const noexcept
    {
        return boneIndex == b && channelIndex == ch && std::abs(frame - f) < 1e-4;
    }
};

void drawTangentGlyph(QPainter &painter, const QPointF &center, uint8_t tangentMode, double radius,
                      const QColor &fill, const QColor &stroke)
{
    painter.setPen(QPen(stroke, 1.5));
    painter.setBrush(fill);

    switch (static_cast<KeyTangentMode>(tangentMode)) {
    case KeyTangentMode::Aligned: {
        QPolygonF poly;
        poly << QPointF(center.x(), center.y() - radius) << QPointF(center.x() + radius, center.y())
             << QPointF(center.x(), center.y() + radius)
             << QPointF(center.x() - radius, center.y());
        painter.drawPolygon(poly);
        break;
    }
    case KeyTangentMode::Vector: {
        QPolygonF poly;
        poly << QPointF(center.x() - radius * 0.85, center.y() - radius * 0.95)
             << QPointF(center.x() + radius * 1.05, center.y())
             << QPointF(center.x() - radius * 0.85, center.y() + radius * 0.95);
        painter.drawPolygon(poly);
        break;
    }
    case KeyTangentMode::Stepped: {
        const QRectF sq(center.x() - radius * 0.82, center.y() - radius * 0.82, radius * 1.64,
                        radius * 1.64);
        painter.drawRect(sq);
        break;
    }
    case KeyTangentMode::Auto:
    default:
        painter.drawEllipse(center, radius * 0.9, radius * 0.9);
        break;
    }
}

[[nodiscard]] double niceGridStep(double minSpan, int targetTicks)
{
    if (minSpan <= 1e-9 || targetTicks <= 0) {
        return 1.0;
    }
    const double raw = minSpan / static_cast<double>(targetTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double nice = 1.0;
    if (norm < 1.5) {
        nice = 1.0;
    } else if (norm < 3.5) {
        nice = 2.0;
    } else if (norm < 7.5) {
        nice = 5.0;
    } else {
        nice = 10.0;
    }
    return nice * mag;
}

[[nodiscard]] int niceFrameStep(double zoomPxPerFrame)
{
    static constexpr int kSteps[] = {1, 2, 5, 10, 20, 25, 50, 100, 250, 500};
    for (int step : kSteps) {
        if (static_cast<double>(step) * zoomPxPerFrame >= 48.0) {
            return step;
        }
    }
    return 1000;
}

template <typename Client = IpcClient>
void dispatchScrubIpc(Client *client, int32_t objectId, float timeSec, bool paused)
{
    if (!client || objectId < 0) {
        return;
    }
    if constexpr (requires(Client *c) {
                      c->sendPuppetScrubTime(static_cast<uint32_t>(objectId), timeSec,
                                             paused ? 1u : 0u, 0u);
                  }) {
        client->sendPuppetScrubTime(static_cast<uint32_t>(objectId), timeSec, paused ? 1u : 0u, 0u);
    } else if constexpr (requires(Client *c, const waywal_ipc_puppet_scrub_t &req) {
                             c->sendPuppetScrubTime(req);
                         }) {
        waywal_ipc_puppet_scrub_t req{};
        req.object_id = static_cast<uint32_t>(objectId);
        req.time_sec = timeSec;
        req.pause_playback = paused ? 1u : 0u;
        req.flags = 0u;
        client->sendPuppetScrubTime(req);
    } else {
        (void)client;
        (void)objectId;
        (void)timeSec;
        (void)paused;
    }
}

template <typename Client = IpcClient>
void dispatchAnimTracksIpc(Client *client, int32_t objectId,
                           const std::vector<TimelineAnimClip> &clips,
                           const std::vector<we_mdl_bone_track_t> &tracks,
                           const std::vector<we_mdl_keyframe_t> &storage, uint32_t activeClipIdx)
{
    if (!client || objectId < 0 || activeClipIdx >= clips.size()) {
        return;
    }
    if constexpr (requires(Client *c, const std::vector<typename Client::PuppetAnimClipData> &v) {
                      c->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId), 0x3u, v);
                  }) {
        const TimelineAnimClip &clip = clips[activeClipIdx];
        typename Client::PuppetAnimClipData clipData{};
        const QByteArray nameUtf8 = clip.name.toUtf8();
        std::memcpy(
            clipData.desc.name, nameUtf8.constData(),
            std::min<size_t>(static_cast<size_t>(nameUtf8.size()), sizeof(clipData.desc.name) - 1));
        const QByteArray modeUtf8 = animModeToString(clip.mode).toLatin1();
        std::memcpy(clipData.desc.mode_str, modeUtf8.constData(),
                    std::min<size_t>(static_cast<size_t>(modeUtf8.size()),
                                     sizeof(clipData.desc.mode_str) - 1));
        clipData.desc.id = clip.id;
        clipData.desc.fps = clip.fps;
        clipData.desc.length = clip.lengthFrames;
        clipData.desc.track_count = static_cast<uint32_t>(tracks.size());
        clipData.desc.layer_blend = clip.layerBlend;
        clipData.desc.layer_rate = clip.layerRate;
        clipData.desc.layer_visible = clip.layerVisible ? 1u : 0u;
        clipData.desc.layer_blend_mode = static_cast<uint32_t>(clip.blendMode);

        clipData.tracks.reserve(tracks.size());
        for (const auto &tr : tracks) {
            typename Client::PuppetAnimTrackData tdata{};
            tdata.desc.bone_index = tr.bone_index;
            tdata.desc.keyframe_count = tr.kf_count;
            tdata.keyframes.reserve(tr.kf_count);
            for (uint32_t i = 0; i < tr.kf_count; ++i) {
                waywal_ipc_mdl_keyframe_t k{};
                std::memcpy(k.pos, tr.keyframes[i].pos, sizeof(k.pos));
                std::memcpy(k.angle, tr.keyframes[i].angle, sizeof(k.angle));
                std::memcpy(k.scale, tr.keyframes[i].scale, sizeof(k.scale));
                tdata.keyframes.push_back(k);
            }
            clipData.tracks.push_back(std::move(tdata));
        }

        std::vector<typename Client::PuppetAnimClipData> payload;
        payload.push_back(std::move(clipData));
        client->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId), 0x3u, payload);
        (void)storage;
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId), clips,
                                                        tracks);
                         }) {
        client->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId), clips, tracks);
    } else if constexpr (requires(Client *c) {
                             c->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId),
                                                        clips[activeClipIdx], tracks, storage);
                         }) {
        client->sendPuppetSetAnimTracks(static_cast<uint32_t>(objectId), clips[activeClipIdx],
                                        tracks, storage);
    } else if constexpr (requires(Client *c, const waywal_ipc_puppet_set_anim_tracks_t &req,
                                  const QByteArray &blob) {
                             c->sendPuppetSetAnimTracks(req, blob);
                         }) {
        const TimelineAnimClip &clip = clips[activeClipIdx];
        QByteArray blob;
        waywal_ipc_anim_clip_desc_t desc{};
        const QByteArray nameUtf8 = clip.name.toUtf8();
        std::memcpy(desc.name, nameUtf8.constData(),
                    std::min<size_t>(static_cast<size_t>(nameUtf8.size()), sizeof(desc.name) - 1));
        const QByteArray modeUtf8 = animModeToString(clip.mode).toLatin1();
        std::memcpy(
            desc.mode_str, modeUtf8.constData(),
            std::min<size_t>(static_cast<size_t>(modeUtf8.size()), sizeof(desc.mode_str) - 1));
        desc.id = clip.id;
        desc.fps = clip.fps;
        desc.length = clip.lengthFrames;
        desc.track_count = static_cast<uint32_t>(tracks.size());
        desc.layer_blend = clip.layerBlend;
        desc.layer_rate = clip.layerRate;
        desc.layer_visible = clip.layerVisible ? 1u : 0u;
        desc.layer_blend_mode = static_cast<uint32_t>(clip.blendMode);
        blob.append(reinterpret_cast<const char *>(&desc), sizeof(desc));

        for (const auto &tr : tracks) {
            waywal_ipc_anim_track_desc_t tdesc{};
            tdesc.bone_index = tr.bone_index;
            tdesc.keyframe_count = tr.kf_count;
            blob.append(reinterpret_cast<const char *>(&tdesc), sizeof(tdesc));
            for (uint32_t i = 0; i < tr.kf_count; ++i) {
                waywal_ipc_mdl_keyframe_t k{};
                std::memcpy(k.pos, tr.keyframes[i].pos, sizeof(k.pos));
                std::memcpy(k.angle, tr.keyframes[i].angle, sizeof(k.angle));
                std::memcpy(k.scale, tr.keyframes[i].scale, sizeof(k.scale));
                blob.append(reinterpret_cast<const char *>(&k), sizeof(k));
            }
        }

        waywal_ipc_puppet_set_anim_tracks_t req{};
        req.object_id = static_cast<uint32_t>(objectId);
        req.clip_count = 1u;
        req.flags = 0x3u;
        client->sendPuppetSetAnimTracks(req, blob);
    } else {
        (void)client;
        (void)objectId;
        (void)clips;
        (void)tracks;
        (void)storage;
        (void)activeClipIdx;
    }
}

class TimelineAuxState final : public QObject {
  public:
    explicit TimelineAuxState(DopeSheetTimelineWidget *owner) : QObject(owner), ownerWidget(owner)
    {
        setObjectName(QStringLiteral("__timeline_aux_state"));
    }

    DopeSheetTimelineWidget *ownerWidget{nullptr};

    QComboBox *blendModeCombo{nullptr};
    QCheckBox *layerVisibleCheck{nullptr};
    QComboBox *tangentCombo{nullptr};
    QCheckBox *breakHandlesCheck{nullptr};
    QSpinBox *onionBeforeSpin{nullptr};
    QSpinBox *onionAfterSpin{nullptr};
    QDoubleSpinBox *onionOpacitySpin{nullptr};
    QPushButton *addClipBtn{nullptr};
    QPushButton *syncBtn{nullptr};
    QPushButton *exportSidecarBtn{nullptr};
    QPushButton *importSidecarBtn{nullptr};

    int32_t objectId{-1};
    QObject *eventFilter{nullptr};
    QPointer<QWidget> watchedWindow;

    QStringList boneNames;
    double scrollFrameOffset{0.0};
    double fcurveMinY{-3.5};
    double fcurveMaxY{3.5};
    int mirrorDirection{1};
    bool updatingUi{false};
    bool dragPreviewSuppressIpc{false};

    std::vector<SelectedKeyRef> selectedKeys;

    [[nodiscard]] bool isSelected(int32_t boneIndex, uint32_t channelIndex, double frame) const
    {
        for (const auto &ref : selectedKeys) {
            if (ref.matches(boneIndex, channelIndex, frame)) {
                return true;
            }
        }
        return false;
    }

    void selectOnly(int32_t boneIndex, uint32_t channelIndex, double frame)
    {
        selectedKeys.clear();
        selectedKeys.push_back({boneIndex, channelIndex, frame});
    }

    void toggleSelection(int32_t boneIndex, uint32_t channelIndex, double frame)
    {
        for (auto it = selectedKeys.begin(); it != selectedKeys.end(); ++it) {
            if (it->matches(boneIndex, channelIndex, frame)) {
                selectedKeys.erase(it);
                return;
            }
        }
        selectedKeys.push_back({boneIndex, channelIndex, frame});
    }
};

[[nodiscard]] TimelineAuxState *auxState(const DopeSheetTimelineWidget *w)
{
    if (!w) {
        return nullptr;
    }
    for (QObject *child : w->children()) {
        if (auto *state = dynamic_cast<TimelineAuxState *>(child)) {
            return state;
        }
    }
    return nullptr;
}

[[nodiscard]] bool isEditableFocusWidget(QWidget *w)
{
    if (!w) {
        return false;
    }
    return qobject_cast<QAbstractSpinBox *>(w) != nullptr ||
           qobject_cast<QLineEdit *>(w) != nullptr || qobject_cast<QComboBox *>(w) != nullptr;
}

[[nodiscard]] std::vector<FCurveKeySnapshot> collectClipKeys(const TimelineAnimClip &clip)
{
    std::vector<FCurveKeySnapshot> all;
    for (const auto &ch : clip.channels) {
        for (auto k : ch.keys) {
            k.animId = static_cast<uint32_t>(std::max(0, clip.id));
            k.boneIndex = ch.boneIndex;
            k.channelIndex = ch.channelIndex;
            all.push_back(k);
        }
    }
    return all;
}

void applyClipKeysSnapshot(TimelineAnimClip &clip, const std::vector<FCurveKeySnapshot> &keys)
{
    for (auto &ch : clip.channels) {
        ch.keys.clear();
    }

    for (const auto &k : keys) {
        if (k.boneIndex < 0 || k.channelIndex >= 9u) {
            continue;
        }
        TimelineFCurveChannel *target = nullptr;
        for (auto &ch : clip.channels) {
            if (ch.boneIndex == k.boneIndex && ch.channelIndex == k.channelIndex) {
                target = &ch;
                break;
            }
        }
        if (!target) {
            TimelineFCurveChannel ch;
            ch.boneIndex = k.boneIndex;
            ch.channelIndex = k.channelIndex;
            ch.label = defaultChannelLabel(k.channelIndex);
            ch.visible = true;
            clip.channels.push_back(std::move(ch));
            target = &clip.channels.back();
        }
        target->keys.push_back(k);
    }

    // Prune channels whose keys were all removed (e.g. undoing keyframe creation).
    clip.channels.erase(
        std::remove_if(clip.channels.begin(), clip.channels.end(),
                       [](const TimelineFCurveChannel &ch) { return ch.keys.empty(); }),
        clip.channels.end());

    for (auto &ch : clip.channels) {
        std::stable_sort(ch.keys.begin(), ch.keys.end(),
                         [](const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) {
                             return a.frame < b.frame;
                         });
    }
}

void rebuildTrackTreeUi(QTreeWidget *trackTree, TimelineAuxState *aux, const TimelineAnimClip *clip,
                        double currentFrame)
{
    if (!trackTree) {
        return;
    }
    if (aux) {
        aux->updatingUi = true;
    }
    trackTree->clear();
    if (clip) {
        std::map<int32_t, std::vector<const TimelineFCurveChannel *>> byBone;
        for (const auto &ch : clip->channels) {
            byBone[ch.boneIndex].push_back(&ch);
        }
        for (const auto &[boneIdx, chList] : byBone) {
            QString boneName = QStringLiteral("Bone %1").arg(boneIdx);
            if (aux && boneIdx >= 0 && boneIdx < aux->boneNames.size() &&
                !aux->boneNames[boneIdx].isEmpty()) {
                boneName = aux->boneNames[boneIdx];
            }
            auto *boneItem = new QTreeWidgetItem(trackTree);
            boneItem->setText(0, boneName);
            boneItem->setData(0, Qt::UserRole, boneIdx);
            boneItem->setCheckState(0, Qt::Checked);
            boneItem->setExpanded(true);

            for (const TimelineFCurveChannel *ch : chList) {
                auto *chItem = new QTreeWidgetItem(boneItem);
                chItem->setText(0, ch->label.isEmpty() ? defaultChannelLabel(ch->channelIndex)
                                                       : ch->label);
                chItem->setData(0, Qt::UserRole, boneIdx);
                chItem->setData(0, Qt::UserRole + 1, static_cast<int>(ch->channelIndex));
                chItem->setCheckState(0, ch->visible ? Qt::Checked : Qt::Unchecked);
                const double val =
                    DopeSheetTimelineWidget::evaluateChannelAtFrame(*ch, currentFrame);
                chItem->setText(1, QString::number(val, 'f', 3));
            }
        }
    }
    if (aux) {
        aux->updatingUi = false;
    }
}

[[nodiscard]] QJsonObject serializeClipsToSidecarJson(int32_t objectId,
                                                      const std::vector<TimelineAnimClip> &clips,
                                                      const QJsonObject &existingRoot = {})
{
    QJsonObject root = existingRoot;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("object_id"), objectId);

    QJsonArray layerBlendModes;
    QJsonArray animCurves;

    for (const auto &clip : clips) {
        layerBlendModes.append(static_cast<int>(clip.blendMode));

        QJsonObject clipObj;
        clipObj.insert(QStringLiteral("id"), clip.id);
        clipObj.insert(QStringLiteral("name"), clip.name);
        clipObj.insert(QStringLiteral("mode"), animModeToString(clip.mode));
        clipObj.insert(QStringLiteral("blend_mode"), blendModeToString(clip.blendMode));
        clipObj.insert(QStringLiteral("fps"), static_cast<double>(clip.fps));
        clipObj.insert(QStringLiteral("length"), clip.lengthFrames);
        clipObj.insert(QStringLiteral("layer_rate"), static_cast<double>(clip.layerRate));
        clipObj.insert(QStringLiteral("layer_blend"), static_cast<double>(clip.layerBlend));
        clipObj.insert(QStringLiteral("layer_visible"), clip.layerVisible);

        QJsonArray channelsArr;
        for (const auto &ch : clip.channels) {
            QJsonObject chObj;
            chObj.insert(QStringLiteral("bone_index"), ch.boneIndex);
            chObj.insert(QStringLiteral("channel_index"), static_cast<int>(ch.channelIndex));
            chObj.insert(QStringLiteral("label"), ch.label);
            chObj.insert(QStringLiteral("visible"), ch.visible);

            QJsonArray keysArr;
            for (const auto &k : ch.keys) {
                QJsonObject kObj;
                kObj.insert(QStringLiteral("frame"), finiteOr(k.frame, 0.0));
                kObj.insert(QStringLiteral("value"), finiteOr(k.value, 0.0));
                kObj.insert(QStringLiteral("tangentMode"), static_cast<int>(k.tangentMode));
                kObj.insert(QStringLiteral("handlesBroken"), k.handlesBroken);
                kObj.insert(QStringLiteral("handleInDx"), finiteOr(k.handleInDx, -1.0));
                kObj.insert(QStringLiteral("handleInDy"), finiteOr(k.handleInDy, 0.0));
                kObj.insert(QStringLiteral("handleOutDx"), finiteOr(k.handleOutDx, 1.0));
                kObj.insert(QStringLiteral("handleOutDy"), finiteOr(k.handleOutDy, 0.0));
                keysArr.append(kObj);
            }
            chObj.insert(QStringLiteral("keys"), keysArr);
            channelsArr.append(chObj);
        }
        clipObj.insert(QStringLiteral("channels"), channelsArr);
        animCurves.append(clipObj);
    }

    root.insert(QStringLiteral("layer_blend_modes"), layerBlendModes);
    root.insert(QStringLiteral("animation_curves"), animCurves);
    return root;
}

[[nodiscard]] bool deserializeClipsFromSidecarJson(const QJsonObject &root,
                                                   std::vector<TimelineAnimClip> &outClips)
{
    if (!root.contains(QStringLiteral("animation_curves")) ||
        !root.value(QStringLiteral("animation_curves")).isArray()) {
        return false;
    }

    const QJsonArray animCurves = root.value(QStringLiteral("animation_curves")).toArray();
    const QJsonArray layerModes = root.value(QStringLiteral("layer_blend_modes")).toArray();
    std::vector<TimelineAnimClip> parsed;
    parsed.reserve(static_cast<size_t>(std::min<qsizetype>(animCurves.size(), WE_MDL_MAX_ANIMS)));

    for (qsizetype i = 0; i < animCurves.size() && parsed.size() < WE_MDL_MAX_ANIMS; ++i) {
        if (!animCurves[i].isObject()) {
            continue;
        }
        const QJsonObject clipObj = animCurves[i].toObject();
        TimelineAnimClip clip;
        clip.id = clipObj.value(QStringLiteral("id")).toInt(100 + static_cast<int>(i));
        clip.name =
            clipObj.value(QStringLiteral("name")).toString(QStringLiteral("Clip %1").arg(i));
        clip.mode = animModeFromString(clipObj.value(QStringLiteral("mode")).toString());

        if (clipObj.contains(QStringLiteral("blend_mode"))) {
            clip.blendMode =
                blendModeFromString(clipObj.value(QStringLiteral("blend_mode")).toString());
        } else if (i < layerModes.size()) {
            clip.blendMode =
                (layerModes[i].toInt(0) == 1) ? WE_ANIM_BLEND_ADDITIVE : WE_ANIM_BLEND_OVERRIDE;
        }

        const double fps = finiteOr(clipObj.value(QStringLiteral("fps")).toDouble(30.0), 30.0);
        clip.fps = static_cast<float>(std::clamp(fps, 1.0, 1000.0));
        clip.lengthFrames =
            std::clamp(clipObj.value(QStringLiteral("length")).toInt(120), 1, 1000000);
        clip.layerRate = static_cast<float>(std::clamp(
            finiteOr(clipObj.value(QStringLiteral("layer_rate")).toDouble(1.0), 1.0), -16.0, 16.0));
        clip.layerBlend = static_cast<float>(std::clamp(
            finiteOr(clipObj.value(QStringLiteral("layer_blend")).toDouble(1.0), 1.0), 0.0, 1.0));
        clip.layerVisible = clipObj.value(QStringLiteral("layer_visible")).toBool(true);

        constexpr size_t kMaxChannelsPerClip = static_cast<size_t>(WE_MDL_MAX_BONES) * 9u;
        std::set<std::pair<int32_t, uint32_t>> seenChannels;

        const QJsonArray channelsArr = clipObj.value(QStringLiteral("channels")).toArray();
        for (const QJsonValue &chVal : channelsArr) {
            if (clip.channels.size() >= kMaxChannelsPerClip) {
                break;
            }
            if (!chVal.isObject()) {
                continue;
            }
            const QJsonObject chObj = chVal.toObject();
            const int32_t boneIdx = chObj.value(QStringLiteral("bone_index")).toInt(-1);
            const int chIdxInt = chObj.value(QStringLiteral("channel_index")).toInt(-1);
            if (boneIdx < 0 || boneIdx >= static_cast<int32_t>(WE_MDL_MAX_BONES) || chIdxInt < 0 ||
                chIdxInt >= 9) {
                continue;
            }
            const uint32_t chIdx = static_cast<uint32_t>(chIdxInt);
            if (!seenChannels.emplace(boneIdx, chIdx).second) {
                continue;
            }

            TimelineFCurveChannel ch;
            ch.boneIndex = boneIdx;
            ch.channelIndex = chIdx;
            ch.label =
                chObj.value(QStringLiteral("label")).toString(defaultChannelLabel(ch.channelIndex));
            ch.visible = chObj.value(QStringLiteral("visible")).toBool(true);

            const QJsonArray keysArr = chObj.value(QStringLiteral("keys")).toArray();
            const qsizetype maxKeys =
                std::min<qsizetype>(keysArr.size(), WAYWAL_IPC_RIGGING_MAX_KEYFRAMES);
            ch.keys.reserve(static_cast<size_t>(maxKeys));
            for (qsizetype ki = 0; ki < maxKeys; ++ki) {
                const QJsonValue &kVal = keysArr[ki];
                if (!kVal.isObject()) {
                    continue;
                }
                const QJsonObject kObj = kVal.toObject();
                FCurveKeySnapshot k;
                k.animId = static_cast<uint32_t>(std::max(0, clip.id));
                k.boneIndex = boneIdx;
                k.channelIndex = ch.channelIndex;
                k.frame = finiteOr(kObj.value(QStringLiteral("frame")).toDouble(0.0), 0.0);
                k.value = finiteOr(kObj.value(QStringLiteral("value")).toDouble(0.0), 0.0);
                k.tangentMode = static_cast<uint8_t>(
                    std::clamp(kObj.value(QStringLiteral("tangentMode")).toInt(0), 0, 3));
                k.handlesBroken = kObj.value(QStringLiteral("handlesBroken")).toBool(false);
                k.handleInDx = std::min(
                    0.0, finiteOr(kObj.value(QStringLiteral("handleInDx")).toDouble(-1.0), -1.0));
                k.handleInDy =
                    finiteOr(kObj.value(QStringLiteral("handleInDy")).toDouble(0.0), 0.0);
                k.handleOutDx = std::max(
                    0.0, finiteOr(kObj.value(QStringLiteral("handleOutDx")).toDouble(1.0), 1.0));
                k.handleOutDy =
                    finiteOr(kObj.value(QStringLiteral("handleOutDy")).toDouble(0.0), 0.0);
                ch.keys.push_back(k);
            }
            std::stable_sort(ch.keys.begin(), ch.keys.end(),
                             [](const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) {
                                 return a.frame < b.frame;
                             });
            clip.channels.push_back(std::move(ch));
        }
        parsed.push_back(std::move(clip));
    }

    if (parsed.empty()) {
        return false;
    }
    outClips = std::move(parsed);
    return true;
}

class DopeSheetCanvas final : public QWidget {
  public:
    explicit DopeSheetCanvas(DopeSheetTimelineWidget *owner, std::function<void()> onEditCommit,
                             std::function<void(bool)> onScrubbingChanged,
                             std::function<void()> onSyncView, std::function<double()> getZoom,
                             std::function<void(double)> setZoom, QWidget *parent = nullptr)
        : QWidget(parent), m_owner(owner), m_onEditCommit(std::move(onEditCommit)),
          m_onScrubbingChanged(std::move(onScrubbingChanged)), m_onSyncView(std::move(onSyncView)),
          m_getZoom(std::move(getZoom)), m_setZoom(std::move(setZoom))
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumHeight(96);
        setAccessibleName(tr("Multi-Track Keyframe Dope-Sheet"));
        setToolTip(tr("Drag ruler to scrub, drag keyframes to move, Ctrl+Wheel to zoom"));
    }

    void setTrackTree(QTreeWidget *tree) noexcept { m_trackTree = tree; }
    void clearOwner() noexcept { m_owner = nullptr; }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.fillRect(rect(), kSurface1);

        if (!m_owner) {
            return;
        }
        auto *aux = auxState(m_owner);
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = aux ? aux->scrollFrameOffset : 0.0;

        auto frameToX = [scrollF, zoom](double frame) { return (frame - scrollF) * zoom + 20.0; };

        const auto &clips = m_owner->clips();
        int32_t lengthFrames = 120;
        const TimelineAnimClip *activeClip = nullptr;
        if (!clips.empty()) {
            const QComboBox *clipCombo =
                m_owner->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"));
            const uint32_t idx =
                clipCombo ? static_cast<uint32_t>(std::max(0, clipCombo->currentIndex())) : 0u;
            if (idx < clips.size()) {
                activeClip = &clips[idx];
                lengthFrames = std::max(1, activeClip->lengthFrames);
            }
        }

        // Play range highlight
        const double rangeLeft = frameToX(0.0);
        const double rangeRight = frameToX(static_cast<double>(lengthFrames - 1));
        if (rangeRight > 0.0 && rangeLeft < width()) {
            painter.fillRect(
                QRectF(std::max(0.0, rangeLeft), kRulerHeightPx,
                       std::min<double>(width(), rangeRight) - std::max(0.0, rangeLeft),
                       height() - kRulerHeightPx),
                QColor(0x20, 0x24, 0x34, 90));
        }

        // Frame grid lines
        const int step = niceFrameStep(zoom);
        const int firstTick = static_cast<int>(std::floor((scrollF - 20.0 / zoom) / step)) * step;
        const int lastTick = static_cast<int>(std::ceil((scrollF + width() / zoom) / step)) * step;

        QFont rulerFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        rulerFont.setPixelSize(10);
        painter.setFont(rulerFont);

        for (int f = firstTick; f <= lastTick; f += step) {
            const double x = frameToX(static_cast<double>(f));
            if (x < -10.0 || x > width() + 10.0) {
                continue;
            }
            painter.setPen(QPen(f == 0 ? kMajorGrid : kBorder, 1.0));
            painter.drawLine(QPointF(x, kRulerHeightPx), QPointF(x, height()));
        }

        // Virtualized row rendering aligned with visible QTreeWidget items
        if (m_trackTree && activeClip) {
            const int treeHeaderH = m_trackTree->header() ? m_trackTree->header()->height() : 0;
            const QPoint treeOrigin = m_trackTree->viewport()->mapTo(m_owner, QPoint(0, 0));
            const QPoint canvasOrigin = mapTo(m_owner, QPoint(0, 0));
            const int yOffset = treeOrigin.y() - canvasOrigin.y();
            (void)treeHeaderH;

            int rowIdx = 0;
            for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
                const QRect bRect = m_trackTree->visualItemRect(boneItem);
                if (bRect.isValid() && !bRect.isEmpty()) {
                    const int yTop = bRect.top() + yOffset;
                    const int rowH = bRect.height();
                    if (yTop + rowH >= kRulerHeightPx && yTop <= height()) {
                        const QRectF rowRect(0.0, yTop, width(), rowH);
                        painter.fillRect(rowRect, (rowIdx % 2 == 0) ? kSurface1 : kSurface2);
                        painter.setPen(QPen(kBorder, 0.8));
                        painter.drawLine(QPointF(0.0, yTop + rowH), QPointF(width(), yTop + rowH));

                        const int32_t boneIdx = boneItem->data(0, Qt::UserRole).toInt();
                        const double cy = yTop + rowH * 0.5;
                        for (const auto &ch : activeClip->channels) {
                            if (ch.boneIndex != boneIdx || !ch.visible) {
                                continue;
                            }
                            for (const auto &k : ch.keys) {
                                const double kx = frameToX(k.frame);
                                if (kx < -12.0 || kx > width() + 12.0) {
                                    continue;
                                }
                                const bool sel =
                                    aux && aux->isSelected(boneIdx, ch.channelIndex, k.frame);
                                drawTangentGlyph(painter, QPointF(kx, cy), k.tangentMode,
                                                 sel ? 5.5 : 4.2, sel ? kAccentSky : kAmber,
                                                 sel ? Qt::white : kSurface0);
                            }
                        }
                        ++rowIdx;
                    }
                }

                if (boneItem->isExpanded()) {
                    for (int c = 0; c < boneItem->childCount(); ++c) {
                        QTreeWidgetItem *chItem = boneItem->child(c);
                        const QRect cRect = m_trackTree->visualItemRect(chItem);
                        if (!cRect.isValid() || cRect.isEmpty()) {
                            continue;
                        }
                        const int yTop = cRect.top() + yOffset;
                        const int rowH = cRect.height();
                        if (yTop + rowH < kRulerHeightPx || yTop > height()) {
                            continue;
                        }
                        const QRectF rowRect(0.0, yTop, width(), rowH);
                        painter.fillRect(rowRect, (rowIdx % 2 == 0) ? kSurface1 : kSurface2);
                        painter.setPen(QPen(kBorder, 0.8));
                        painter.drawLine(QPointF(0.0, yTop + rowH), QPointF(width(), yTop + rowH));

                        const int32_t boneIdx = chItem->data(0, Qt::UserRole).toInt();
                        const uint32_t chIdx = chItem->data(0, Qt::UserRole + 1).toUInt();
                        const double cy = yTop + rowH * 0.5;
                        const QColor col = channelColor(chIdx);

                        for (const auto &ch : activeClip->channels) {
                            if (ch.boneIndex != boneIdx || ch.channelIndex != chIdx) {
                                continue;
                            }
                            for (const auto &k : ch.keys) {
                                const double kx = frameToX(k.frame);
                                if (kx < -12.0 || kx > width() + 12.0) {
                                    continue;
                                }
                                const bool sel = aux && aux->isSelected(boneIdx, chIdx, k.frame);
                                drawTangentGlyph(painter, QPointF(kx, cy), k.tangentMode,
                                                 sel ? 5.5 : 4.5, sel ? kAccentSky : col,
                                                 sel ? Qt::white : kSurface0);
                            }
                        }
                        ++rowIdx;
                    }
                }
            }
        }

        // Ruler trench drawn last so scrolled rows never overlap header ticks
        painter.fillRect(QRectF(0.0, 0.0, width(), kRulerHeightPx), kSurface0);
        painter.setPen(QPen(kBorder, 1.0));
        painter.drawLine(QPointF(0.0, kRulerHeightPx), QPointF(width(), kRulerHeightPx));

        for (int f = firstTick; f <= lastTick; f += step) {
            const double x = frameToX(static_cast<double>(f));
            if (x < -10.0 || x > width() + 10.0) {
                continue;
            }
            painter.setPen(QPen(kMajorGrid, 1.0));
            painter.drawLine(QPointF(x, kRulerHeightPx - 8), QPointF(x, kRulerHeightPx));
            painter.setPen(kTextMuted);
            painter.drawText(QRectF(x + 3.0, 2.0, 44.0, 16.0), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(f));
        }

        // Playhead line and scrubber cap
        const double phX = frameToX(m_owner->currentFrame());
        if (phX >= -10.0 && phX <= width() + 10.0) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.drawLine(QPointF(phX, kRulerHeightPx), QPointF(phX, height()));

            QPolygonF cap;
            cap << QPointF(phX - 6.0, 3.0) << QPointF(phX + 6.0, 3.0) << QPointF(phX + 6.0, 14.0)
                << QPointF(phX, 22.0) << QPointF(phX - 6.0, 14.0);
            painter.setPen(QPen(Qt::white, 1.0));
            painter.setBrush(kAccentSky);
            painter.drawPolygon(cap);
        }

        if (hasFocus()) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect().adjusted(1, 1, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        setFocus(Qt::MouseFocusReason);
        if (!m_owner) {
            return;
        }
        auto *aux = auxState(m_owner);
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = aux ? aux->scrollFrameOffset : 0.0;

        if (event->button() == Qt::MiddleButton) {
            m_mode = DragMode::Pan;
            m_lastMouse = event->position();
            return;
        }

        if (event->button() != Qt::LeftButton) {
            return;
        }

        if (event->position().y() <= kRulerHeightPx) {
            m_mode = DragMode::Scrub;
            if (m_onScrubbingChanged) {
                m_onScrubbingChanged(true);
            }
            const double f = scrollF + (event->position().x() - 20.0) / zoom;
            m_owner->setCurrentFrame(std::round(f));
            return;
        }

        // Hit-test keyframe glyphs on visible rows
        if (hitTestKeyframe(event->position(), event->modifiers().testFlag(Qt::ShiftModifier))) {
            m_mode = DragMode::MoveKeys;
            m_dragStartFrame = scrollF + (event->position().x() - 20.0) / zoom;
            m_dragLastDelta = 0.0;
            captureDragPreState();
            update();
            return;
        }

        // Empty body click also scrubs playhead cleanly
        m_mode = DragMode::Scrub;
        if (m_onScrubbingChanged) {
            m_onScrubbingChanged(true);
        }
        const double f = scrollF + (event->position().x() - 20.0) / zoom;
        m_owner->setCurrentFrame(std::round(f));
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_owner) {
            return;
        }
        auto *aux = auxState(m_owner);
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = aux ? aux->scrollFrameOffset : 0.0;

        if (m_mode == DragMode::Pan && aux) {
            const double dx = event->position().x() - m_lastMouse.x();
            aux->scrollFrameOffset = std::max(-10.0, aux->scrollFrameOffset - dx / zoom);
            m_lastMouse = event->position();
            if (m_onSyncView) {
                m_onSyncView();
            }
            return;
        }

        if (m_mode == DragMode::Scrub) {
            const double f = scrollF + (event->position().x() - 20.0) / zoom;
            m_owner->setCurrentFrame(std::round(f));
            return;
        }

        if (m_mode == DragMode::MoveKeys) {
            const double curF = scrollF + (event->position().x() - 20.0) / zoom;
            double delta = curF - m_dragStartFrame;
            if (!event->modifiers().testFlag(Qt::AltModifier)) {
                delta = std::round(delta);
            }
            if (std::abs(delta - m_dragLastDelta) > 1e-4) {
                applyDragDelta(delta);
                m_dragLastDelta = delta;
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        Q_UNUSED(event);
        if (m_mode == DragMode::Scrub) {
            if (m_onScrubbingChanged) {
                m_onScrubbingChanged(false);
            }
        } else if (m_mode == DragMode::MoveKeys) {
            commitDragCommand();
        }
        m_mode = DragMode::None;
    }

    void wheelEvent(QWheelEvent *event) override
    {
        auto *aux = auxState(m_owner);
        if (!aux) {
            return;
        }
        const double deltaSteps = static_cast<double>(event->angleDelta().y()) / 120.0;
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const double oldZoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
            const double mouseF = aux->scrollFrameOffset + (event->position().x() - 20.0) / oldZoom;
            const double newZoom = std::clamp(oldZoom * std::pow(1.18, deltaSteps),
                                              kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
            if (m_setZoom) {
                m_setZoom(newZoom);
            }
            aux->scrollFrameOffset =
                std::max(-10.0, mouseF - (event->position().x() - 20.0) / newZoom);
            if (m_onSyncView) {
                m_onSyncView();
            }
            event->accept();
            return;
        }

        if (event->angleDelta().x() != 0 || event->modifiers().testFlag(Qt::ShiftModifier)) {
            const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
            const double panPx =
                (event->angleDelta().x() != 0) ? event->angleDelta().x() : event->angleDelta().y();
            aux->scrollFrameOffset =
                std::max(-10.0, aux->scrollFrameOffset - (panPx * 0.25) / zoom);
            if (m_onSyncView) {
                m_onSyncView();
            }
            event->accept();
            return;
        }

        if (m_trackTree && m_trackTree->verticalScrollBar()) {
            auto *sb = m_trackTree->verticalScrollBar();
            sb->setValue(sb->value() - event->angleDelta().y() / 4);
            update();
            event->accept();
        }
    }

  private:
    enum class DragMode { None, Scrub, MoveKeys, Pan };

    [[nodiscard]] int activeClipIndex() const
    {
        if (!m_owner) {
            return -1;
        }
        const auto *combo = m_owner->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"));
        return combo ? std::max(0, combo->currentIndex()) : 0;
    }

    bool hitTestKeyframe(const QPointF &pos, bool additiveToggle)
    {
        if (!m_owner) {
            return false;
        }
        auto *aux = auxState(m_owner);
        const int clipIdx = activeClipIndex();
        const auto &clips = m_owner->clips();
        if (!aux || !m_trackTree || clipIdx < 0 || static_cast<size_t>(clipIdx) >= clips.size()) {
            return false;
        }

        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = aux->scrollFrameOffset;
        const QPoint treeOrigin = m_trackTree->viewport()->mapTo(m_owner, QPoint(0, 0));
        const QPoint canvasOrigin = mapTo(m_owner, QPoint(0, 0));
        const int yOffset = treeOrigin.y() - canvasOrigin.y();
        const TimelineAnimClip &clip = clips[static_cast<size_t>(clipIdx)];

        for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
            const QRect bRect = m_trackTree->visualItemRect(boneItem);
            if (bRect.isValid()) {
                const double cy = bRect.top() + yOffset + bRect.height() * 0.5;
                if (std::abs(pos.y() - cy) <= bRect.height() * 0.55) {
                    const int32_t boneIdx = boneItem->data(0, Qt::UserRole).toInt();
                    for (const auto &ch : clip.channels) {
                        if (ch.boneIndex != boneIdx || !ch.visible) {
                            continue;
                        }
                        for (const auto &k : ch.keys) {
                            const double kx = (k.frame - scrollF) * zoom + 20.0;
                            if (std::abs(pos.x() - kx) <= 8.0) {
                                if (additiveToggle) {
                                    aux->toggleSelection(boneIdx, ch.channelIndex, k.frame);
                                } else if (!aux->isSelected(boneIdx, ch.channelIndex, k.frame)) {
                                    aux->selectOnly(boneIdx, ch.channelIndex, k.frame);
                                }
                                return true;
                            }
                        }
                    }
                }
            }
            if (boneItem->isExpanded()) {
                for (int c = 0; c < boneItem->childCount(); ++c) {
                    QTreeWidgetItem *chItem = boneItem->child(c);
                    const QRect cRect = m_trackTree->visualItemRect(chItem);
                    if (!cRect.isValid()) {
                        continue;
                    }
                    const double cy = cRect.top() + yOffset + cRect.height() * 0.5;
                    if (std::abs(pos.y() - cy) <= cRect.height() * 0.55) {
                        const int32_t boneIdx = chItem->data(0, Qt::UserRole).toInt();
                        const uint32_t chIdx = chItem->data(0, Qt::UserRole + 1).toUInt();
                        for (const auto &ch : clip.channels) {
                            if (ch.boneIndex != boneIdx || ch.channelIndex != chIdx) {
                                continue;
                            }
                            for (const auto &k : ch.keys) {
                                const double kx = (k.frame - scrollF) * zoom + 20.0;
                                if (std::abs(pos.x() - kx) <= 8.0) {
                                    if (additiveToggle) {
                                        aux->toggleSelection(boneIdx, chIdx, k.frame);
                                    } else if (!aux->isSelected(boneIdx, chIdx, k.frame)) {
                                        aux->selectOnly(boneIdx, chIdx, k.frame);
                                    }
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    void captureDragPreState();
    void applyDragDelta(double deltaFrames);
    void commitDragCommand();

    DopeSheetTimelineWidget *m_owner{nullptr};
    QTreeWidget *m_trackTree{nullptr};
    std::function<void()> m_onEditCommit;
    std::function<void(bool)> m_onScrubbingChanged;
    std::function<void()> m_onSyncView;
    std::function<double()> m_getZoom;
    std::function<void(double)> m_setZoom;

    DragMode m_mode{DragMode::None};
    QPointF m_lastMouse;
    double m_dragStartFrame{0.0};
    double m_dragLastDelta{0.0};
    std::vector<FCurveKeySnapshot> m_preDragKeys;
    std::vector<SelectedKeyRef> m_preDragSel;
};

class FCurveCanvas final : public QWidget {
  public:
    explicit FCurveCanvas(DopeSheetTimelineWidget *owner, std::function<void()> onSyncView,
                          std::function<double()> getZoom, std::function<void(double)> setZoom,
                          QWidget *parent = nullptr)
        : QWidget(parent), m_owner(owner), m_onSyncView(std::move(onSyncView)),
          m_getZoom(std::move(getZoom)), m_setZoom(std::move(setZoom))
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumHeight(110);
        setAccessibleName(tr("Cubic-Bezier F-Curve Editor"));
        setToolTip(tr(
            "Drag keys or Bezier handles; double-click curve to insert key; Ctrl/Alt+Wheel zoom"));
    }

    void setTrackTree(QTreeWidget *tree) noexcept { m_trackTree = tree; }
    void clearOwner() noexcept { m_owner = nullptr; }
    void frameFitVisibleChannels();

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.fillRect(rect(), kSurface1);

        auto *aux = auxState(m_owner);
        if (!aux) {
            return;
        }
        const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = aux->scrollFrameOffset;
        const double minY = aux->fcurveMinY;
        const double maxY = std::max(minY + 0.05, aux->fcurveMaxY);

        auto frameToX = [scrollF, zoomX](double frame) { return (frame - scrollF) * zoomX + 38.0; };
        auto valToY = [this, minY, maxY](double val) {
            const double pad = 12.0;
            const double avail = std::max(16.0, static_cast<double>(height()) - 2.0 * pad);
            return pad + (maxY - val) / (maxY - minY) * avail;
        };

        // Horizontal Y grid + numeric labels
        const double yStep = niceGridStep(maxY - minY, 6);
        const double firstY = std::floor(minY / yStep) * yStep;
        QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        monoFont.setPixelSize(10);
        painter.setFont(monoFont);

        for (double v = firstY; v <= maxY + yStep * 0.5; v += yStep) {
            const double py = valToY(v);
            if (py < 4.0 || py > height() - 4.0) {
                continue;
            }
            const bool isZero = std::abs(v) < 1e-6;
            painter.setPen(QPen(isZero ? kMajorGrid : kBorder, isZero ? 1.2 : 0.8));
            painter.drawLine(QPointF(38.0, py), QPointF(width(), py));
            painter.setPen(kTextMuted);
            painter.drawText(QRectF(2.0, py - 8.0, 34.0, 16.0), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(v, 'f', yStep < 0.5 ? 2 : 1));
        }

        // Vertical frame grid
        const int fStep = niceFrameStep(zoomX);
        const int firstTick =
            static_cast<int>(std::floor((scrollF - 38.0 / zoomX) / fStep)) * fStep;
        const int lastTick =
            static_cast<int>(std::ceil((scrollF + width() / zoomX) / fStep)) * fStep;
        for (int f = firstTick; f <= lastTick; f += fStep) {
            const double x = frameToX(static_cast<double>(f));
            if (x < 38.0 || x > width()) {
                continue;
            }
            painter.setPen(QPen(kBorder, 0.8));
            painter.drawLine(QPointF(x, 0.0), QPointF(x, height()));
        }

        const auto channels = collectVisibleChannels();
        const int sampleStepPx = 2;

        for (const auto *ch : channels) {
            if (!ch || !ch->visible || ch->keys.empty()) {
                continue;
            }
            const QColor col = channelColor(ch->channelIndex);
            const PreparedChannel prep = prepareChannel(*ch);

            QPainterPath path;
            bool firstPt = true;
            for (int px = 38; px <= width(); px += sampleStepPx) {
                const double f = scrollF + (static_cast<double>(px) - 38.0) / zoomX;
                const double v = evaluatePreparedChannel(prep, f);
                const double py = valToY(v);
                if (firstPt) {
                    path.moveTo(px, py);
                    firstPt = false;
                } else {
                    path.lineTo(px, py);
                }
            }
            painter.setPen(QPen(col, 1.8));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);

            // Keyframes and tangent handles
            for (size_t ki = 0; ki < ch->keys.size(); ++ki) {
                const FCurveKeySnapshot &k = ch->keys[ki];
                const double val = (ki < prep.keys.size()) ? prep.keys[ki].unwrappedValue : k.value;
                const QPointF pt(frameToX(k.frame), valToY(val));
                if (pt.x() < 26.0 || pt.x() > width() + 12.0) {
                    continue;
                }

                const bool sel = aux->isSelected(ch->boneIndex, ch->channelIndex, k.frame);
                if (sel && k.tangentMode != static_cast<uint8_t>(KeyTangentMode::Stepped)) {
                    const double inDx =
                        (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                            ? k.handleInDx
                            : prep.keys[ki].inDx;
                    const double inDy =
                        (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                            ? k.handleInDy
                            : prep.keys[ki].inDy;
                    const double outDx =
                        (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                            ? k.handleOutDx
                            : prep.keys[ki].outDx;
                    const double outDy =
                        (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                            ? k.handleOutDy
                            : prep.keys[ki].outDy;

                    const QPointF pIn(frameToX(k.frame + inDx), valToY(val + inDy));
                    const QPointF pOut(frameToX(k.frame + outDx), valToY(val + outDy));

                    painter.setPen(QPen(kAmber, 1.2, Qt::SolidLine));
                    painter.drawLine(pt, pIn);
                    painter.drawLine(pt, pOut);

                    painter.setPen(QPen(Qt::white, 1.2));
                    painter.setBrush(kAmber);
                    painter.drawEllipse(pIn, 4.0, 4.0);
                    painter.drawEllipse(pOut, 4.0, 4.0);
                }

                drawTangentGlyph(painter, pt, k.tangentMode, sel ? 5.8 : 4.5,
                                 sel ? kAccentSky : col, sel ? Qt::white : kSurface0);
            }
        }

        // Playhead line
        const double phX = frameToX(m_owner->currentFrame());
        if (phX >= 38.0 && phX <= width()) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.drawLine(QPointF(phX, 0.0), QPointF(phX, height()));
        }

        // Left Y-axis gutter border
        painter.setPen(QPen(kBorder, 1.0));
        painter.drawLine(QPointF(38.0, 0.0), QPointF(38.0, height()));

        if (hasFocus()) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect().adjusted(1, 1, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

  private:
    enum class DragTarget { None, KeyNode, HandleIn, HandleOut, Pan };

    [[nodiscard]] int activeClipIndex() const
    {
        if (!m_owner) {
            return -1;
        }
        const auto *combo = m_owner->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"));
        return combo ? std::max(0, combo->currentIndex()) : 0;
    }

    [[nodiscard]] std::vector<const TimelineFCurveChannel *> collectVisibleChannels() const
    {
        std::vector<const TimelineFCurveChannel *> out;
        if (!m_owner) {
            return out;
        }
        const auto &clips = m_owner->clips();
        const int idx = activeClipIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= clips.size()) {
            return out;
        }
        const TimelineAnimClip &clip = clips[static_cast<size_t>(idx)];

        int32_t filterBone = -1;
        int filterChannel = -1;
        if (m_trackTree && m_trackTree->currentItem()) {
            QTreeWidgetItem *cur = m_trackTree->currentItem();
            filterBone = cur->data(0, Qt::UserRole).toInt();
            const QVariant chVar = cur->data(0, Qt::UserRole + 1);
            if (chVar.isValid()) {
                filterChannel = chVar.toInt();
            }
        }

        for (const auto &ch : clip.channels) {
            if (!ch.visible) {
                continue;
            }
            if (filterBone >= 0 && ch.boneIndex != filterBone) {
                continue;
            }
            if (filterChannel >= 0 && static_cast<int>(ch.channelIndex) != filterChannel) {
                continue;
            }
            out.push_back(&ch);
        }

        if (out.empty()) {
            for (const auto &ch : clip.channels) {
                if (ch.visible) {
                    out.push_back(&ch);
                }
            }
        }
        return out;
    }

    DopeSheetTimelineWidget *m_owner{nullptr};
    QTreeWidget *m_trackTree{nullptr};
    std::function<void()> m_onSyncView;
    std::function<double()> m_getZoom;
    std::function<void(double)> m_setZoom;

    DragTarget m_dragTarget{DragTarget::None};
    int32_t m_dragBone{-1};
    uint32_t m_dragChannel{0};
    double m_dragOrigFrame{0.0};
    double m_dragOrigValue{0.0};
    QPointF m_dragStartMouse;
    QPointF m_lastMouse;
    std::vector<FCurveKeySnapshot> m_preDragKeys;
};

} // namespace

// Helpers defined outside anonymous namespace so they can call DopeSheetTimelineWidget
// slots/friends cleanly via public API + CommandBus.
static void commitClipKeyEdit(DopeSheetTimelineWidget *owner, uint32_t animId, const QString &label,
                              std::vector<FCurveKeySnapshot> oldKeys,
                              std::vector<FCurveKeySnapshot> newKeys)
{
    if (!owner) {
        return;
    }
    const auto *aux = auxState(owner);
    const int32_t objectId = aux ? aux->objectId : -1;
    if (owner->commandBus()) {
        QPointer<DopeSheetTimelineWidget> safeOwner(owner);
        auto applyFn = [safeOwner, objectId](int32_t /*objId*/, uint32_t targetAnimId,
                                             const std::vector<FCurveKeySnapshot> &keys) {
            if (!safeOwner) {
                return;
            }
            const auto *curAux = auxState(safeOwner.data());
            if (!curAux || curAux->objectId != objectId) {
                return;
            }
            safeOwner->applyUndoKeySnapshot(targetAnimId, keys);
        };
        auto cmd = std::make_unique<KeyframeEditCommand>(
            objectId, animId, label, std::move(oldKeys), std::move(newKeys), std::move(applyFn));
        owner->commandBus()->execute(std::move(cmd));
    } else {
        owner->applyUndoKeySnapshot(animId, newKeys);
    }
}

void DopeSheetCanvas::captureDragPreState()
{
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size()) {
        return;
    }
    m_preDragKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
    m_preDragSel = aux->selectedKeys;
}

void DopeSheetCanvas::applyDragDelta(double deltaFrames)
{
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size()) {
        return;
    }
    const auto &clip = clips[static_cast<size_t>(idx)];
    const uint32_t animId = static_cast<uint32_t>(std::max(0, clip.id));
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);
    std::vector<FCurveKeySnapshot> moved = m_preDragKeys;
    aux->selectedKeys = m_preDragSel;

    for (auto &k : moved) {
        for (size_t s = 0; s < m_preDragSel.size(); ++s) {
            if (m_preDragSel[s].matches(k.boneIndex, k.channelIndex, k.frame)) {
                k.frame = std::clamp(k.frame + deltaFrames, 0.0, maxF);
                aux->selectedKeys[s].frame = k.frame;
                break;
            }
        }
    }

    const QScopedValueRollback<bool> suppressIpc(aux->dragPreviewSuppressIpc, true);
    m_owner->applyUndoKeySnapshot(animId, moved);
    if (m_onSyncView) {
        m_onSyncView();
    }
}

void DopeSheetCanvas::commitDragCommand()
{
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size() ||
        std::abs(m_dragLastDelta) < 1e-4) {
        return;
    }
    const uint32_t animId = static_cast<uint32_t>(std::max(0, clips[static_cast<size_t>(idx)].id));
    std::vector<FCurveKeySnapshot> postKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
    {
        const QScopedValueRollback<bool> suppressIpc(aux->dragPreviewSuppressIpc, true);
        m_owner->applyUndoKeySnapshot(animId, m_preDragKeys);
    }

    commitClipKeyEdit(m_owner, animId, QStringLiteral("Move keyframes"), std::move(m_preDragKeys),
                      std::move(postKeys));
}

void FCurveCanvas::frameFitVisibleChannels()
{
    auto *aux = auxState(m_owner);
    if (!aux) {
        return;
    }
    const auto channels = collectVisibleChannels();
    bool any = false;
    double minF = 0.0;
    double maxF = 120.0;
    double minV = std::numeric_limits<double>::infinity();
    double maxV = -std::numeric_limits<double>::infinity();

    for (const auto *ch : channels) {
        if (!ch || ch->keys.empty()) {
            continue;
        }
        const PreparedChannel prep = prepareChannel(*ch);
        for (const auto &k : prep.keys) {
            if (!any) {
                minF = maxF = k.frame;
                any = true;
            } else {
                minF = std::min(minF, k.frame);
                maxF = std::max(maxF, k.frame);
            }
            minV = std::min(minV, k.unwrappedValue);
            maxV = std::max(maxV, k.unwrappedValue);
        }
    }

    if (!any) {
        aux->fcurveMinY = -3.5;
        aux->fcurveMaxY = 3.5;
    } else {
        const double spanV = std::max(0.5, maxV - minV);
        aux->fcurveMinY = minV - spanV * 0.15;
        aux->fcurveMaxY = maxV + spanV * 0.15;
        const double spanF = std::max(10.0, maxF - minF);
        if (m_setZoom && width() > 60) {
            m_setZoom(std::clamp(static_cast<double>(width() - 60) / spanF, kMinZoomPxPerFrame,
                                 kMaxZoomPxPerFrame));
        }
        aux->scrollFrameOffset = std::max(0.0, minF - spanF * 0.05);
    }
    if (m_onSyncView) {
        m_onSyncView();
    }
}

void FCurveCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size()) {
        return;
    }

    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_dragTarget = DragTarget::Pan;
        m_lastMouse = event->position();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double scrollF = aux->scrollFrameOffset;
    const double minY = aux->fcurveMinY;
    const double maxY = std::max(minY + 0.05, aux->fcurveMaxY);

    auto frameToX = [scrollF, zoomX](double frame) { return (frame - scrollF) * zoomX + 38.0; };
    auto valToY = [this, minY, maxY](double val) {
        const double pad = 12.0;
        const double avail = std::max(16.0, static_cast<double>(height()) - 2.0 * pad);
        return pad + (maxY - val) / (maxY - minY) * avail;
    };

    const auto channels = collectVisibleChannels();

    // Check Bezier handles on selected keyframes before checking keyframe centers.
    for (const auto *ch : channels) {
        if (!ch || !ch->visible) {
            continue;
        }
        const PreparedChannel prep = prepareChannel(*ch);
        for (size_t ki = 0; ki < ch->keys.size(); ++ki) {
            const auto &k = ch->keys[ki];
            if (!aux->isSelected(ch->boneIndex, ch->channelIndex, k.frame) ||
                k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Stepped)) {
                continue;
            }
            const double val = (ki < prep.keys.size()) ? prep.keys[ki].unwrappedValue : k.value;
            const double inDx = (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                                    ? k.handleInDx
                                    : prep.keys[ki].inDx;
            const double inDy = (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                                    ? k.handleInDy
                                    : prep.keys[ki].inDy;
            const double outDx = (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                                     ? k.handleOutDx
                                     : prep.keys[ki].outDx;
            const double outDy = (k.tangentMode == static_cast<uint8_t>(KeyTangentMode::Aligned))
                                     ? k.handleOutDy
                                     : prep.keys[ki].outDy;

            const QPointF pIn(frameToX(k.frame + inDx), valToY(val + inDy));
            const QPointF pOut(frameToX(k.frame + outDx), valToY(val + outDy));

            if (QLineF(event->position(), pIn).length() <= 10.0) {
                m_dragTarget = DragTarget::HandleIn;
                m_dragBone = ch->boneIndex;
                m_dragChannel = ch->channelIndex;
                m_dragOrigFrame = k.frame;
                m_dragOrigValue = k.value;
                m_dragStartMouse = event->position();
                m_preDragKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
                return;
            }
            if (QLineF(event->position(), pOut).length() <= 10.0) {
                m_dragTarget = DragTarget::HandleOut;
                m_dragBone = ch->boneIndex;
                m_dragChannel = ch->channelIndex;
                m_dragOrigFrame = k.frame;
                m_dragOrigValue = k.value;
                m_dragStartMouse = event->position();
                m_preDragKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
                return;
            }
        }
    }

    for (const auto *ch : channels) {
        if (!ch || !ch->visible) {
            continue;
        }
        const PreparedChannel prep = prepareChannel(*ch);
        for (size_t ki = 0; ki < ch->keys.size(); ++ki) {
            const auto &k = ch->keys[ki];
            const double val = (ki < prep.keys.size()) ? prep.keys[ki].unwrappedValue : k.value;
            const QPointF pt(frameToX(k.frame), valToY(val));
            if (QLineF(event->position(), pt).length() <= 10.0) {
                if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    aux->toggleSelection(ch->boneIndex, ch->channelIndex, k.frame);
                } else {
                    aux->selectOnly(ch->boneIndex, ch->channelIndex, k.frame);
                }
                m_dragTarget = DragTarget::KeyNode;
                m_dragBone = ch->boneIndex;
                m_dragChannel = ch->channelIndex;
                m_dragOrigFrame = k.frame;
                m_dragOrigValue = k.value;
                m_dragStartMouse = event->position();
                m_preDragKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
                if (m_onSyncView) {
                    m_onSyncView();
                }
                return;
            }
        }
    }
}

void FCurveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size()) {
        return;
    }

    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double minY = aux->fcurveMinY;
    const double maxY = std::max(minY + 0.05, aux->fcurveMaxY);
    const double availH = std::max(16.0, static_cast<double>(height()) - 24.0);
    const double pxPerUnitY = availH / (maxY - minY);

    if (m_dragTarget == DragTarget::Pan) {
        const QPointF d = event->position() - m_lastMouse;
        aux->scrollFrameOffset = std::max(-10.0, aux->scrollFrameOffset - d.x() / zoomX);
        const double dy = d.y() / pxPerUnitY;
        aux->fcurveMinY += dy;
        aux->fcurveMaxY += dy;
        m_lastMouse = event->position();
        if (m_onSyncView) {
            m_onSyncView();
        }
        return;
    }

    if (m_dragTarget == DragTarget::None) {
        return;
    }

    const auto &clip = clips[static_cast<size_t>(idx)];
    const uint32_t animId = static_cast<uint32_t>(std::max(0, clip.id));
    const double df = (event->position().x() - m_dragStartMouse.x()) / zoomX;
    const double dv = -(event->position().y() - m_dragStartMouse.y()) / pxPerUnitY;
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);

    // Compute neighbor keyframe spans on the dragged channel so Bezier handles cannot cross keys.
    bool hasPrev = false;
    bool hasNext = false;
    double prevFrame = 0.0;
    double nextFrame = 0.0;
    for (const auto &other : m_preDragKeys) {
        if (other.boneIndex != m_dragBone || other.channelIndex != m_dragChannel) {
            continue;
        }
        if (other.frame < m_dragOrigFrame - 1e-6 && (!hasPrev || other.frame > prevFrame)) {
            prevFrame = other.frame;
            hasPrev = true;
        }
        if (other.frame > m_dragOrigFrame + 1e-6 && (!hasNext || other.frame < nextFrame)) {
            nextFrame = other.frame;
            hasNext = true;
        }
    }

    std::vector<FCurveKeySnapshot> edited = m_preDragKeys;
    for (auto &k : edited) {
        if (k.boneIndex != m_dragBone || k.channelIndex != m_dragChannel ||
            std::abs(k.frame - m_dragOrigFrame) >= 1e-4) {
            continue;
        }
        if (m_dragTarget == DragTarget::KeyNode) {
            double snapDf = event->modifiers().testFlag(Qt::AltModifier) ? df : std::round(df);
            double newF = std::clamp(m_dragOrigFrame + snapDf, 0.0, maxF);
            double newV = m_dragOrigValue + dv;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                if (std::abs(event->position().x() - m_dragStartMouse.x()) >=
                    std::abs(event->position().y() - m_dragStartMouse.y())) {
                    newV = m_dragOrigValue;
                } else {
                    newF = m_dragOrigFrame;
                }
            }
            k.frame = newF;
            k.value = newV;
            aux->selectOnly(m_dragBone, m_dragChannel, newF);
        } else {
            k.tangentMode = static_cast<uint8_t>(KeyTangentMode::Aligned);
            const double curF = aux->scrollFrameOffset + (event->position().x() - 38.0) / zoomX;
            const double curV = maxY - ((event->position().y() - 12.0) / availH) * (maxY - minY);
            const double maxInSpan = hasPrev ? std::max(1e-3, k.frame - prevFrame) : 1e6;
            const double maxOutSpan = hasNext ? std::max(1e-3, nextFrame - k.frame) : 1e6;
            if (m_dragTarget == DragTarget::HandleIn) {
                k.handleInDx = std::clamp(curF - k.frame, -maxInSpan, -1e-3);
                k.handleInDy = curV - k.value;
                if (!k.handlesBroken) {
                    const double inLen = std::hypot(k.handleInDx, k.handleInDy);
                    const double outLen = std::max(0.5, std::hypot(k.handleOutDx, k.handleOutDy));
                    if (inLen > 1e-6) {
                        double outDx = std::max(1e-3, (-k.handleInDx / inLen) * outLen);
                        double outDy = (-k.handleInDy / inLen) * outLen;
                        if (outDx > maxOutSpan) {
                            const double scale = maxOutSpan / outDx;
                            outDx *= scale;
                            outDy *= scale;
                        }
                        k.handleOutDx = outDx;
                        k.handleOutDy = outDy;
                    }
                }
            } else {
                k.handleOutDx = std::clamp(curF - k.frame, 1e-3, maxOutSpan);
                k.handleOutDy = curV - k.value;
                if (!k.handlesBroken) {
                    const double outLen = std::hypot(k.handleOutDx, k.handleOutDy);
                    const double inLen = std::max(0.5, std::hypot(k.handleInDx, k.handleInDy));
                    if (outLen > 1e-6) {
                        double inDx = std::min(-1e-3, (-k.handleOutDx / outLen) * inLen);
                        double inDy = (-k.handleOutDy / outLen) * inLen;
                        if (-inDx > maxInSpan) {
                            const double scale = maxInSpan / (-inDx);
                            inDx *= scale;
                            inDy *= scale;
                        }
                        k.handleInDx = inDx;
                        k.handleInDy = inDy;
                    }
                }
            }
        }
        break;
    }

    const QScopedValueRollback<bool> suppressIpc(aux->dragPreviewSuppressIpc, true);
    m_owner->applyUndoKeySnapshot(animId, edited);
    if (m_onSyncView) {
        m_onSyncView();
    }
}

void FCurveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (!m_owner) {
        m_dragTarget = DragTarget::None;
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (aux &&
        (m_dragTarget == DragTarget::KeyNode || m_dragTarget == DragTarget::HandleIn ||
         m_dragTarget == DragTarget::HandleOut) &&
        idx >= 0 && static_cast<size_t>(idx) < clips.size()) {
        const uint32_t animId =
            static_cast<uint32_t>(std::max(0, clips[static_cast<size_t>(idx)].id));
        std::vector<FCurveKeySnapshot> postKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
        {
            const QScopedValueRollback<bool> suppressIpc(aux->dragPreviewSuppressIpc, true);
            m_owner->applyUndoKeySnapshot(animId, m_preDragKeys);
        }
        const QString lbl = (m_dragTarget == DragTarget::KeyNode)
                                ? QStringLiteral("Move F-curve key")
                                : QStringLiteral("Edit Bezier tangent");
        commitClipKeyEdit(m_owner, animId, lbl, std::move(m_preDragKeys), std::move(postKeys));
    }
    m_dragTarget = DragTarget::None;
}

void FCurveCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_owner) {
        return;
    }
    auto *aux = auxState(m_owner);
    const int idx = activeClipIndex();
    const auto &clips = m_owner->clips();
    if (!aux || idx < 0 || static_cast<size_t>(idx) >= clips.size() ||
        event->button() != Qt::LeftButton) {
        return;
    }

    const auto channels = collectVisibleChannels();
    if (channels.empty()) {
        return;
    }

    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double minY = aux->fcurveMinY;
    const double maxY = std::max(minY + 0.05, aux->fcurveMaxY);
    const double availH = std::max(16.0, static_cast<double>(height()) - 24.0);
    const double maxF =
        static_cast<double>(std::max(1, clips[static_cast<size_t>(idx)].lengthFrames) - 1);

    const double frame = std::clamp(
        std::round(aux->scrollFrameOffset + (event->position().x() - 38.0) / zoomX), 0.0, maxF);
    const double value = maxY - ((event->position().y() - 12.0) / availH) * (maxY - minY);

    const TimelineFCurveChannel *targetCh = channels.front();
    const uint32_t animId = static_cast<uint32_t>(std::max(0, clips[static_cast<size_t>(idx)].id));
    std::vector<FCurveKeySnapshot> oldKeys = collectClipKeys(clips[static_cast<size_t>(idx)]);
    std::vector<FCurveKeySnapshot> newKeys = oldKeys;

    bool replaced = false;
    for (auto &k : newKeys) {
        if (k.boneIndex == targetCh->boneIndex && k.channelIndex == targetCh->channelIndex &&
            std::abs(k.frame - frame) < 0.5) {
            k.value = value;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        FCurveKeySnapshot nk;
        nk.animId = animId;
        nk.boneIndex = targetCh->boneIndex;
        nk.channelIndex = targetCh->channelIndex;
        nk.frame = frame;
        nk.value = value;
        nk.tangentMode = static_cast<uint8_t>(KeyTangentMode::Auto);
        newKeys.push_back(nk);
    }

    aux->selectOnly(targetCh->boneIndex, targetCh->channelIndex, frame);
    commitClipKeyEdit(m_owner, animId, QStringLiteral("Insert F-curve key"), std::move(oldKeys),
                      std::move(newKeys));
}

void FCurveCanvas::wheelEvent(QWheelEvent *event)
{
    auto *aux = auxState(m_owner);
    if (!aux) {
        return;
    }
    const double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double oldZoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double mouseF = aux->scrollFrameOffset + (event->position().x() - 38.0) / oldZoom;
        const double newZoom =
            std::clamp(oldZoom * std::pow(1.18, steps), kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
        if (m_setZoom) {
            m_setZoom(newZoom);
        }
        aux->scrollFrameOffset = std::max(-10.0, mouseF - (event->position().x() - 38.0) / newZoom);
    } else if (event->modifiers().testFlag(Qt::AltModifier)) {
        const double mid = 0.5 * (aux->fcurveMinY + aux->fcurveMaxY);
        const double half = std::max(0.05, 0.5 * (aux->fcurveMaxY - aux->fcurveMinY));
        const double newHalf = std::clamp(half * std::pow(0.85, steps), 0.05, 10000.0);
        aux->fcurveMinY = mid - newHalf;
        aux->fcurveMaxY = mid + newHalf;
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) || event->angleDelta().x() != 0) {
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double panPx =
            (event->angleDelta().x() != 0) ? event->angleDelta().x() : event->angleDelta().y();
        aux->scrollFrameOffset = std::max(-10.0, aux->scrollFrameOffset - (panPx * 0.25) / zoom);
    } else {
        const double span = aux->fcurveMaxY - aux->fcurveMinY;
        const double shift = -steps * span * 0.1;
        aux->fcurveMinY += shift;
        aux->fcurveMaxY += shift;
    }

    if (m_onSyncView) {
        m_onSyncView();
    }
    event->accept();
}

namespace {

class TimelineEventFilter final : public QObject {
  public:
    TimelineEventFilter(DopeSheetTimelineWidget *owner, std::function<void()> onVisibilityUpdate,
                        std::function<void()> onFrameFit, std::function<void()> onDeleteSelected,
                        std::function<void(int)> onStepKeyframe, QObject *parent)
        : QObject(parent), m_owner(owner), m_onVisibilityUpdate(std::move(onVisibilityUpdate)),
          m_onFrameFit(std::move(onFrameFit)), m_onDeleteSelected(std::move(onDeleteSelected)),
          m_onStepKeyframe(std::move(onStepKeyframe))
    {
    }

    void disarm() noexcept
    {
        m_disarmed = true;
        m_owner = nullptr;
        m_onVisibilityUpdate = nullptr;
        m_onFrameFit = nullptr;
        m_onDeleteSelected = nullptr;
        m_onStepKeyframe = nullptr;
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_disarmed || !m_owner) {
            return false;
        }
        QWidget *topWin = m_owner->window();
        if ((watched == m_owner || (topWin != nullptr && watched == topWin)) &&
            (event->type() == QEvent::Hide || event->type() == QEvent::Show ||
             event->type() == QEvent::WindowStateChange)) {
            if (m_onVisibilityUpdate) {
                m_onVisibilityUpdate();
            }
            return false;
        }

        if (event->type() == QEvent::ShortcutOverride) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (!isEditableFocusWidget(QApplication::focusWidget())) {
                if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Space ||
                    ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right ||
                    ke->key() == Qt::Key_K || ke->key() == Qt::Key_I) {
                    event->accept();
                    return true;
                }
            }
            return false;
        }

        if (event->type() == QEvent::KeyPress) {
            if (!m_owner || isEditableFocusWidget(QApplication::focusWidget())) {
                return false;
            }
            auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Space:
                m_owner->onPlayPauseToggled();
                return true;
            case Qt::Key_Left:
                if (ke->modifiers().testFlag(Qt::ShiftModifier) && m_onStepKeyframe) {
                    m_onStepKeyframe(-1);
                } else {
                    m_owner->onStepFrame(-1);
                }
                return true;
            case Qt::Key_Right:
                if (ke->modifiers().testFlag(Qt::ShiftModifier) && m_onStepKeyframe) {
                    m_onStepKeyframe(1);
                } else {
                    m_owner->onStepFrame(1);
                }
                return true;
            case Qt::Key_Home:
                m_owner->setCurrentFrame(0.0);
                return true;
            case Qt::Key_End: {
                const auto &clips = m_owner->clips();
                size_t clipIdx = 0;
                if (const auto *clipCombo =
                        m_owner->findChild<QComboBox *>(QStringLiteral("timelineClipCombo"));
                    clipCombo && clipCombo->currentIndex() >= 0 &&
                    static_cast<size_t>(clipCombo->currentIndex()) < clips.size()) {
                    clipIdx = static_cast<size_t>(clipCombo->currentIndex());
                }
                const double endF =
                    clips.empty()
                        ? 119.0
                        : static_cast<double>(std::max(1, clips[clipIdx].lengthFrames) - 1);
                m_owner->setCurrentFrame(endF);
                return true;
            }
            case Qt::Key_K:
            case Qt::Key_I:
                m_owner->onInsertKeyframeAtPlayhead();
                return true;
            case Qt::Key_Tab:
                if (ke->modifiers().testFlag(Qt::ControlModifier)) {
                    m_owner->setViewMode(TimelineViewMode::Split);
                } else {
                    const TimelineViewMode cur = m_owner->viewMode();
                    m_owner->setViewMode(cur == TimelineViewMode::FCurve
                                             ? TimelineViewMode::DopeSheet
                                             : TimelineViewMode::FCurve);
                }
                return true;
            case Qt::Key_1:
                m_owner->onSetSelectedTangentMode(KeyTangentMode::Auto);
                return true;
            case Qt::Key_2:
                m_owner->onSetSelectedTangentMode(KeyTangentMode::Aligned);
                return true;
            case Qt::Key_3:
                m_owner->onSetSelectedTangentMode(KeyTangentMode::Vector);
                return true;
            case Qt::Key_4:
                m_owner->onSetSelectedTangentMode(KeyTangentMode::Stepped);
                return true;
            case Qt::Key_F:
                if (m_onFrameFit) {
                    m_onFrameFit();
                }
                return true;
            case Qt::Key_O: {
                auto *onion = m_owner->findChild<QCheckBox *>(QStringLiteral("timelineOnionCheck"));
                m_owner->setOnionSkinEnabled(onion ? !onion->isChecked() : true);
                return true;
            }
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                if (m_onDeleteSelected) {
                    m_onDeleteSelected();
                }
                return true;
            default:
                break;
            }
        }
        return false;
    }

  private:
    DopeSheetTimelineWidget *m_owner{nullptr};
    bool m_disarmed{false};
    std::function<void()> m_onVisibilityUpdate;
    std::function<void()> m_onFrameFit;
    std::function<void()> m_onDeleteSelected;
    std::function<void(int)> m_onStepKeyframe;
};

} // namespace

DopeSheetTimelineWidget::DopeSheetTimelineWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("DopeSheetTimelineWidget"));
    setAccessibleName(tr("Animation Dope-Sheet and F-Curve Timeline"));
    setFocusPolicy(Qt::StrongFocus);

    // Seed one default clip so controls and canvases stay responsive before a puppet loads.
    TimelineAnimClip defaultClip;
    defaultClip.id = 100;
    defaultClip.name = QStringLiteral("Idle");
    defaultClip.mode = WE_ANIM_LOOP;
    defaultClip.blendMode = WE_ANIM_BLEND_OVERRIDE;
    defaultClip.fps = 30.0f;
    defaultClip.lengthFrames = 120;
    m_clips.push_back(std::move(defaultClip));

    m_playTimer.setTimerType(Qt::PreciseTimer);
    m_playTimer.setInterval(8);
    connect(&m_playTimer, &QTimer::timeout, this, &DopeSheetTimelineWidget::onPlayTick);

    setupUi();
    updatePlaybackTimerState();
}

void DopeSheetTimelineWidget::setupUi()
{
    auto *aux = new TimelineAuxState(this);

    setStyleSheet(QStringLiteral(
        "DopeSheetTimelineWidget { background-color: #181825; color: #cdd6f4; }"
        "QToolTip { background-color: #11111b; color: #cdd6f4; border: 1px solid #45475a; }"
        "QPushButton { background-color: #313244; color: #cdd6f4; border: 1px solid #45475a; "
        "border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
        "QPushButton:hover { background-color: #45475a; }"
        "QPushButton:checked { background-color: #0369a1; color: #f8fafc; border-color: #38bdf8; }"
        "QPushButton:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, "
        "QCheckBox:focus, QTreeWidget:focus { border: 2px solid #38bdf8; outline: none; }"
        "QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { "
        "color: #7c869f; background-color: #181825; border-color: #262b36; }"
        "QComboBox, QSpinBox, QDoubleSpinBox { background-color: #11111b; color: #cdd6f4; "
        "border: 1px solid #45475a; border-radius: 4px; padding: 2px 5px; font-size: 11px; }"
        "QCheckBox { color: #cdd6f4; spacing: 4px; font-size: 11px; }"
        "QTreeWidget { background-color: #181825; color: #cdd6f4; border: 1px solid #313244; "
        "font-size: 11px; }"
        "QTreeWidget::item:selected { background-color: #313244; color: #38bdf8; }"
        "QHeaderView::section { background-color: #11111b; color: #94a3b8; border: none; "
        "border-bottom: 1px solid #313244; padding: 4px 6px; font-size: 10px; font-weight: bold; "
        "}"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 4, 6, 6);
    rootLayout->setSpacing(4);

    // Row 1: Transport & Keyframe Tangent Bar
    auto *transportBar = new QHBoxLayout();
    transportBar->setSpacing(4);

    auto *firstBtn = new QPushButton(QStringLiteral("|<"), this);
    firstBtn->setAccessibleName(tr("Jump to first frame"));
    firstBtn->setToolTip(tr("Jump to frame 0 (Home)"));
    connect(firstBtn, &QPushButton::clicked, this, [this]() { setCurrentFrame(0.0); });

    auto *prevBtn = new QPushButton(QStringLiteral("<"), this);
    prevBtn->setAccessibleName(tr("Step back one frame"));
    prevBtn->setToolTip(tr("Step backward 1 frame (Left Arrow)"));
    connect(prevBtn, &QPushButton::clicked, this, [this]() { onStepFrame(-1); });

    m_playBtn = new QPushButton(tr("Play"), this);
    m_playBtn->setCheckable(true);
    m_playBtn->setAccessibleName(tr("Play or pause animation"));
    m_playBtn->setToolTip(tr("Toggle playback (Space)"));
    connect(m_playBtn, &QPushButton::clicked, this, &DopeSheetTimelineWidget::onPlayPauseToggled);

    auto *nextBtn = new QPushButton(QStringLiteral(">"), this);
    nextBtn->setAccessibleName(tr("Step forward one frame"));
    nextBtn->setToolTip(tr("Step forward 1 frame (Right Arrow)"));
    connect(nextBtn, &QPushButton::clicked, this, [this]() { onStepFrame(1); });

    auto *lastBtn = new QPushButton(QStringLiteral(">|"), this);
    lastBtn->setAccessibleName(tr("Jump to last frame"));
    lastBtn->setToolTip(tr("Jump to final frame (End)"));
    connect(lastBtn, &QPushButton::clicked, this, [this]() {
        const double endF =
            (m_activeClipIndex < m_clips.size())
                ? static_cast<double>(std::max(1, m_clips[m_activeClipIndex].lengthFrames) - 1)
                : 119.0;
        setCurrentFrame(endF);
    });

    m_timecodeLabel = new QLabel(QStringLiteral("00:00.00 | f0000 / 0120"), this);
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPixelSize(11);
    monoFont.setStyleHint(QFont::Monospace);
    m_timecodeLabel->setFont(monoFont);
    m_timecodeLabel->setStyleSheet(
        QStringLiteral("color: #38bdf8; background-color: #11111b; border: 1px solid #313244; "
                       "border-radius: 4px; padding: 2px 8px;"));
    m_timecodeLabel->setAccessibleName(tr("Current playback timecode and frame"));

    auto *insertKeyBtn = new QPushButton(tr("+ Key"), this);
    insertKeyBtn->setAccessibleName(tr("Insert keyframe at playhead"));
    insertKeyBtn->setToolTip(tr("Insert keyframe at playhead (K or I)"));
    connect(insertKeyBtn, &QPushButton::clicked, this,
            &DopeSheetTimelineWidget::onInsertKeyframeAtPlayhead);

    aux->tangentCombo = new QComboBox(this);
    aux->tangentCombo->addItem(tr("Auto"), static_cast<int>(KeyTangentMode::Auto));
    aux->tangentCombo->addItem(tr("Aligned"), static_cast<int>(KeyTangentMode::Aligned));
    aux->tangentCombo->addItem(tr("Vector"), static_cast<int>(KeyTangentMode::Vector));
    aux->tangentCombo->addItem(tr("Stepped"), static_cast<int>(KeyTangentMode::Stepped));
    aux->tangentCombo->setAccessibleName(tr("Selected keyframe tangent mode"));
    aux->tangentCombo->setToolTip(tr("Set tangent mode for selected keys (1..4)"));
    connect(aux->tangentCombo, qOverload<int>(&QComboBox::activated), this, [this, aux](int idx) {
        const auto mode = static_cast<KeyTangentMode>(aux->tangentCombo->itemData(idx).toInt());
        onSetSelectedTangentMode(mode);
    });

    aux->breakHandlesCheck = new QCheckBox(tr("Break"), this);
    aux->breakHandlesCheck->setAccessibleName(tr("Break Bezier tangent handles"));
    aux->breakHandlesCheck->setToolTip(tr("Unlink in/out tangent handle angles on selected keys"));
    connect(aux->breakHandlesCheck, &QCheckBox::toggled, this, [this, aux](bool broken) {
        if (aux->updatingUi || m_activeClipIndex >= m_clips.size()) {
            return;
        }
        auto &clip = m_clips[m_activeClipIndex];
        for (auto &ch : clip.channels) {
            for (auto &k : ch.keys) {
                if (aux->isSelected(ch.boneIndex, ch.channelIndex, k.frame)) {
                    k.handlesBroken = broken;
                }
            }
        }
        if (m_fcurveCanvas) {
            m_fcurveCanvas->update();
        }
    });

    m_viewModeCombo = new QComboBox(this);
    m_viewModeCombo->addItem(tr("Split"), static_cast<int>(TimelineViewMode::Split));
    m_viewModeCombo->addItem(tr("Dope-Sheet"), static_cast<int>(TimelineViewMode::DopeSheet));
    m_viewModeCombo->addItem(tr("F-Curve"), static_cast<int>(TimelineViewMode::FCurve));
    m_viewModeCombo->setAccessibleName(tr("Timeline view mode"));
    m_viewModeCombo->setToolTip(tr("Switch Dope-Sheet, F-Curve, or Split view (Tab)"));
    connect(
        m_viewModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
            const auto mode = static_cast<TimelineViewMode>(m_viewModeCombo->itemData(idx).toInt());
            setViewMode(mode);
        });

    aux->syncBtn = new QPushButton(tr("Bake & Sync"), this);
    aux->syncBtn->setAccessibleName(tr("Bake F-curves and sync to daemon"));
    aux->syncBtn->setToolTip(tr("Bake sparse Bezier F-curves into MDLA tracks and push to daemon"));
    connect(aux->syncBtn, &QPushButton::clicked, this,
            &DopeSheetTimelineWidget::onBakeAndSyncTracks);

    transportBar->addWidget(firstBtn);
    transportBar->addWidget(prevBtn);
    transportBar->addWidget(m_playBtn);
    transportBar->addWidget(nextBtn);
    transportBar->addWidget(lastBtn);
    transportBar->addWidget(m_timecodeLabel);
    transportBar->addWidget(insertKeyBtn);
    transportBar->addWidget(aux->tangentCombo);
    transportBar->addWidget(aux->breakHandlesCheck);
    transportBar->addStretch(1);
    transportBar->addWidget(m_viewModeCombo);
    transportBar->addWidget(aux->syncBtn);
    rootLayout->addLayout(transportBar);

    // Row 2: Clip / Layer Mixer & Onion-Skin Controls
    auto *mixerBar = new QHBoxLayout();
    mixerBar->setSpacing(6);

    m_clipCombo = new QComboBox(this);
    m_clipCombo->setObjectName(QStringLiteral("timelineClipCombo"));
    m_clipCombo->setMinimumWidth(110);
    m_clipCombo->setAccessibleName(tr("Active animation clip"));
    m_clipCombo->setToolTip(tr("Select animation clip layer"));

    aux->addClipBtn = new QPushButton(QStringLiteral("+Clip"), this);
    aux->addClipBtn->setAccessibleName(tr("Add animation clip"));
    aux->addClipBtn->setToolTip(tr("Create a new animation clip layer (max 64)"));

    m_animModeCombo = new QComboBox(this);
    m_animModeCombo->addItem(tr("Loop"), static_cast<int>(WE_ANIM_LOOP));
    m_animModeCombo->addItem(tr("Mirror"), static_cast<int>(WE_ANIM_MIRROR));
    m_animModeCombo->addItem(tr("Single"), static_cast<int>(WE_ANIM_SINGLE));
    m_animModeCombo->setAccessibleName(tr("Clip playback mode"));
    m_animModeCombo->setToolTip(tr("Loop, Mirror ping-pong, or Single play mode"));

    aux->blendModeCombo = new QComboBox(this);
    aux->blendModeCombo->addItem(tr("Override"), static_cast<int>(WE_ANIM_BLEND_OVERRIDE));
    aux->blendModeCombo->addItem(tr("Additive"), static_cast<int>(WE_ANIM_BLEND_ADDITIVE));
    aux->blendModeCombo->setAccessibleName(tr("Layer blend mode"));
    aux->blendModeCombo->setToolTip(tr("Layer mixing mode: Override or Additive delta"));

    aux->layerVisibleCheck = new QCheckBox(tr("Vis"), this);
    aux->layerVisibleCheck->setChecked(true);
    aux->layerVisibleCheck->setAccessibleName(tr("Layer visibility"));
    aux->layerVisibleCheck->setToolTip(tr("Enable or mute active animation layer"));

    auto *fpsLbl = new QLabel(tr("FPS"), this);
    fpsLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setRange(1.0, 1000.0);
    m_fpsSpin->setDecimals(1);
    m_fpsSpin->setValue(30.0);
    m_fpsSpin->setKeyboardTracking(false);
    m_fpsSpin->setAccessibleName(tr("Frames per second"));
    m_fpsSpin->setToolTip(tr("Uniform playback and MDLA bake frame rate (FPS)"));

    auto *lenLbl = new QLabel(tr("Len"), this);
    lenLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_lengthSpin = new QSpinBox(this);
    m_lengthSpin->setRange(1, 1000000);
    m_lengthSpin->setValue(120);
    m_lengthSpin->setKeyboardTracking(false);
    m_lengthSpin->setAccessibleName(tr("Clip length in frames"));
    m_lengthSpin->setToolTip(tr("Total frame count of the active animation clip"));

    auto *rateLbl = new QLabel(tr("Rate"), this);
    rateLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_layerRateSpin = new QDoubleSpinBox(this);
    m_layerRateSpin->setRange(-16.0, 16.0);
    m_layerRateSpin->setSingleStep(0.1);
    m_layerRateSpin->setDecimals(2);
    m_layerRateSpin->setValue(1.0);
    m_layerRateSpin->setKeyboardTracking(false);
    m_layerRateSpin->setAccessibleName(tr("Layer playback rate multiplier"));
    m_layerRateSpin->setToolTip(tr("Layer playback speed multiplier"));

    auto *blendLbl = new QLabel(tr("Blend"), this);
    blendLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_layerBlendSpin = new QDoubleSpinBox(this);
    m_layerBlendSpin->setRange(0.0, 1.0);
    m_layerBlendSpin->setSingleStep(0.05);
    m_layerBlendSpin->setDecimals(2);
    m_layerBlendSpin->setValue(1.0);
    m_layerBlendSpin->setKeyboardTracking(false);
    m_layerBlendSpin->setAccessibleName(tr("Layer blend weight"));
    m_layerBlendSpin->setToolTip(tr("Layer influence weight (0.0 to 1.0)"));

    m_onionCheck = new QCheckBox(tr("Onion"), this);
    m_onionCheck->setObjectName(QStringLiteral("timelineOnionCheck"));
    m_onionCheck->setAccessibleName(tr("Toggle onion-skinning ghost frames"));
    m_onionCheck->setToolTip(tr("Toggle past and future ghost poses in viewport (O)"));

    aux->onionBeforeSpin = new QSpinBox(this);
    aux->onionBeforeSpin->setRange(0, 5);
    aux->onionBeforeSpin->setValue(2);
    aux->onionBeforeSpin->setPrefix(QStringLiteral("-"));
    aux->onionBeforeSpin->setKeyboardTracking(false);
    aux->onionBeforeSpin->setAccessibleName(tr("Onion-skin before frame count"));
    aux->onionBeforeSpin->setToolTip(tr("Ghost frames before playhead"));

    aux->onionAfterSpin = new QSpinBox(this);
    aux->onionAfterSpin->setRange(0, 5);
    aux->onionAfterSpin->setValue(2);
    aux->onionAfterSpin->setPrefix(QStringLiteral("+"));
    aux->onionAfterSpin->setKeyboardTracking(false);
    aux->onionAfterSpin->setAccessibleName(tr("Onion-skin after frame count"));
    aux->onionAfterSpin->setToolTip(tr("Ghost frames after playhead"));

    aux->onionOpacitySpin = new QDoubleSpinBox(this);
    aux->onionOpacitySpin->setRange(0.10, 0.60);
    aux->onionOpacitySpin->setSingleStep(0.05);
    aux->onionOpacitySpin->setDecimals(2);
    aux->onionOpacitySpin->setValue(0.30);
    aux->onionOpacitySpin->setKeyboardTracking(false);
    aux->onionOpacitySpin->setAccessibleName(tr("Onion-skin ghost opacity"));
    aux->onionOpacitySpin->setToolTip(tr("Ghost overlay opacity"));

    aux->exportSidecarBtn = new QPushButton(tr("Save Sidecar"), this);
    aux->exportSidecarBtn->setAccessibleName(tr("Save sparse curves sidecar JSON"));
    aux->exportSidecarBtn->setToolTip(
        tr("Save lossless sparse Bezier F-curves to studio_rig_<id>.json"));

    aux->importSidecarBtn = new QPushButton(tr("Load Sidecar"), this);
    aux->importSidecarBtn->setAccessibleName(tr("Load sparse curves sidecar JSON"));
    aux->importSidecarBtn->setToolTip(
        tr("Load lossless sparse Bezier F-curves from studio_rig_<id>.json"));

    mixerBar->addWidget(m_clipCombo);
    mixerBar->addWidget(aux->addClipBtn);
    mixerBar->addWidget(m_animModeCombo);
    mixerBar->addWidget(aux->blendModeCombo);
    mixerBar->addWidget(aux->layerVisibleCheck);
    mixerBar->addWidget(fpsLbl);
    mixerBar->addWidget(m_fpsSpin);
    mixerBar->addWidget(lenLbl);
    mixerBar->addWidget(m_lengthSpin);
    mixerBar->addWidget(rateLbl);
    mixerBar->addWidget(m_layerRateSpin);
    mixerBar->addWidget(blendLbl);
    mixerBar->addWidget(m_layerBlendSpin);
    mixerBar->addWidget(m_onionCheck);
    mixerBar->addWidget(aux->onionBeforeSpin);
    mixerBar->addWidget(aux->onionAfterSpin);
    mixerBar->addWidget(aux->onionOpacitySpin);
    mixerBar->addWidget(aux->exportSidecarBtn);
    mixerBar->addWidget(aux->importSidecarBtn);
    mixerBar->addStretch(1);
    rootLayout->addLayout(mixerBar);

    // Main Splitter: Track Tree (left) + Vertical Split Canvases (right)
    auto *outerSplitter = new QSplitter(Qt::Horizontal, this);
    outerSplitter->setChildrenCollapsible(false);

    m_trackTree = new QTreeWidget(outerSplitter);
    m_trackTree->setHeaderLabels({tr("Track / Channel"), tr("Value")});
    m_trackTree->setColumnCount(2);
    m_trackTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_trackTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_trackTree->setMinimumWidth(190);
    m_trackTree->setMaximumWidth(360);
    m_trackTree->setUniformRowHeights(true);
    m_trackTree->setAccessibleName(tr("Animation tracks and bone channels"));

    m_mainSplitter = new QSplitter(Qt::Vertical, outerSplitter);
    m_mainSplitter->setChildrenCollapsible(false);

    auto syncCanvases = [this]() {
        if (m_dopeCanvas) {
            m_dopeCanvas->update();
        }
        if (m_fcurveCanvas) {
            m_fcurveCanvas->update();
        }
    };

    auto *dope = new DopeSheetCanvas(
        this, [this]() { onBakeAndSyncTracks(); },
        [this](bool scrubbing) {
            m_scrubbing = scrubbing;
            updatePlaybackTimerState();
        },
        syncCanvases, [this]() { return m_zoomPxPerFrame; },
        [this](double z) {
            m_zoomPxPerFrame = std::clamp(z, kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
        },
        m_mainSplitter);
    dope->setTrackTree(m_trackTree);
    m_dopeCanvas = dope;

    auto *fcurve = new FCurveCanvas(
        this, syncCanvases, [this]() { return m_zoomPxPerFrame; },
        [this](double z) {
            m_zoomPxPerFrame = std::clamp(z, kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
        },
        m_mainSplitter);
    fcurve->setTrackTree(m_trackTree);
    m_fcurveCanvas = fcurve;

    m_mainSplitter->addWidget(m_dopeCanvas);
    m_mainSplitter->addWidget(m_fcurveCanvas);
    m_mainSplitter->setSizes({150, 210});

    outerSplitter->addWidget(m_trackTree);
    outerSplitter->addWidget(m_mainSplitter);
    outerSplitter->setStretchFactor(0, 0);
    outerSplitter->setStretchFactor(1, 1);
    outerSplitter->setSizes({230, 760});
    rootLayout->addWidget(outerSplitter, 1);

    // Sync canvas repaint on tree expand/collapse/scroll/selection
    connect(m_trackTree, &QTreeWidget::itemExpanded, this,
            [syncCanvases](QTreeWidgetItem *) { syncCanvases(); });
    connect(m_trackTree, &QTreeWidget::itemCollapsed, this,
            [syncCanvases](QTreeWidgetItem *) { syncCanvases(); });
    connect(m_trackTree, &QTreeWidget::currentItemChanged, this,
            [syncCanvases](QTreeWidgetItem *, QTreeWidgetItem *) { syncCanvases(); });
    connect(m_trackTree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (m_dopeCanvas) {
            m_dopeCanvas->update();
        }
    });
    connect(m_trackTree, &QTreeWidget::itemChanged, this,
            [this, aux, syncCanvases](QTreeWidgetItem *item, int col) {
                if (!item || col != 0 || aux->updatingUi || m_activeClipIndex >= m_clips.size()) {
                    return;
                }
                const int32_t boneIdx = item->data(0, Qt::UserRole).toInt();
                const QVariant chVar = item->data(0, Qt::UserRole + 1);
                const bool vis = (item->checkState(0) == Qt::Checked);
                auto &clip = m_clips[m_activeClipIndex];
                for (auto &ch : clip.channels) {
                    if (ch.boneIndex != boneIdx) {
                        continue;
                    }
                    if (!chVar.isValid() ||
                        static_cast<uint32_t>(chVar.toInt()) == ch.channelIndex) {
                        ch.visible = vis;
                    }
                }
                syncCanvases();
            });

    // Clip / mixer bindings
    auto syncClipControls = [this, aux]() {
        if (m_activeClipIndex >= m_clips.size()) {
            return;
        }
        aux->updatingUi = true;
        const QSignalBlocker b1(m_clipCombo);
        const QSignalBlocker b2(m_animModeCombo);
        const QSignalBlocker b3(aux->blendModeCombo);
        const QSignalBlocker b4(aux->layerVisibleCheck);
        const QSignalBlocker b5(m_fpsSpin);
        const QSignalBlocker b6(m_lengthSpin);
        const QSignalBlocker b7(m_layerRateSpin);
        const QSignalBlocker b8(m_layerBlendSpin);

        const auto &clip = m_clips[m_activeClipIndex];
        m_clipCombo->setCurrentIndex(static_cast<int>(m_activeClipIndex));
        m_animModeCombo->setCurrentIndex(m_animModeCombo->findData(static_cast<int>(clip.mode)));
        aux->blendModeCombo->setCurrentIndex(
            aux->blendModeCombo->findData(static_cast<int>(clip.blendMode)));
        aux->layerVisibleCheck->setChecked(clip.layerVisible);
        m_fpsSpin->setValue(static_cast<double>(clip.fps));
        m_lengthSpin->setValue(clip.lengthFrames);
        m_layerRateSpin->setValue(static_cast<double>(clip.layerRate));
        m_layerBlendSpin->setValue(static_cast<double>(clip.layerBlend));
        aux->updatingUi = false;
    };

    auto rebuildTrackTreeForClip = [this, aux, syncCanvases]() {
        const TimelineAnimClip *activeClip =
            (m_activeClipIndex < m_clips.size()) ? &m_clips[m_activeClipIndex] : nullptr;
        rebuildTrackTreeUi(m_trackTree, aux, activeClip, m_currentFrame);
        syncCanvases();
    };

    m_clipCombo->addItem(m_clips[0].name, m_clips[0].id);
    syncClipControls();

    connect(m_clipCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, syncClipControls, rebuildTrackTreeForClip](int idx) {
                if (idx >= 0 && static_cast<size_t>(idx) < m_clips.size()) {
                    m_activeClipIndex = static_cast<uint32_t>(idx);
                    syncClipControls();
                    rebuildTrackTreeForClip();
                    setCurrentFrame(m_currentFrame);
                }
            });

    connect(aux->addClipBtn, &QPushButton::clicked, this,
            [this, syncClipControls, rebuildTrackTreeForClip]() {
                if (m_clips.size() >= WE_MDL_MAX_ANIMS) {
                    emit statusMessage(tr("Clip limit (%1) reached").arg(WE_MDL_MAX_ANIMS));
                    return;
                }
                TimelineAnimClip clip;
                clip.id = 100 + static_cast<int32_t>(m_clips.size());
                clip.name = QStringLiteral("Clip_%1").arg(m_clips.size());
                m_clips.push_back(std::move(clip));
                m_clipCombo->addItem(m_clips.back().name, m_clips.back().id);
                m_activeClipIndex = static_cast<uint32_t>(m_clips.size() - 1);
                syncClipControls();
                rebuildTrackTreeForClip();
            });

    auto applyMixerChanges = [this, aux]() {
        if (aux->updatingUi || m_activeClipIndex >= m_clips.size()) {
            return;
        }
        auto &clip = m_clips[m_activeClipIndex];
        clip.mode = static_cast<we_mdl_anim_mode_t>(m_animModeCombo->currentData().toInt());
        clip.blendMode =
            static_cast<we_anim_blend_mode_t>(aux->blendModeCombo->currentData().toInt());
        clip.layerVisible = aux->layerVisibleCheck->isChecked();
        clip.fps = static_cast<float>(m_fpsSpin->value());
        clip.lengthFrames = m_lengthSpin->value();
        clip.layerRate = static_cast<float>(m_layerRateSpin->value());
        clip.layerBlend = static_cast<float>(m_layerBlendSpin->value());
        setCurrentFrame(m_currentFrame);
        onBakeAndSyncTracks();
    };

    connect(m_animModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [applyMixerChanges](int) { applyMixerChanges(); });
    connect(aux->blendModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [applyMixerChanges](int) { applyMixerChanges(); });
    connect(aux->layerVisibleCheck, &QCheckBox::toggled, this,
            [applyMixerChanges](bool) { applyMixerChanges(); });
    connect(m_fpsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyMixerChanges](double) { applyMixerChanges(); });
    connect(m_lengthSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [applyMixerChanges](int) { applyMixerChanges(); });
    connect(m_layerRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyMixerChanges](double) { applyMixerChanges(); });
    connect(m_layerBlendSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyMixerChanges](double) { applyMixerChanges(); });

    auto emitOnion = [this, aux]() {
        emit onionSkinChanged(m_onionEnabled, aux->onionBeforeSpin->value(),
                              aux->onionAfterSpin->value(),
                              static_cast<float>(aux->onionOpacitySpin->value()));
    };
    connect(m_onionCheck, &QCheckBox::toggled, this, [this](bool en) { setOnionSkinEnabled(en); });
    connect(aux->onionBeforeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [emitOnion](int) { emitOnion(); });
    connect(aux->onionAfterSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [emitOnion](int) { emitOnion(); });
    connect(aux->onionOpacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitOnion](double) { emitOnion(); });

    connect(aux->exportSidecarBtn, &QPushButton::clicked, this, [this]() {
        const QString defName = QStringLiteral("studio_rig_%1.json").arg(std::max(0, m_objectId));
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Rig Animation Sidecar"),
                                                          defName, tr("JSON Files (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        QJsonObject existing;
        QFile inFile(path);
        if (inFile.exists() && inFile.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(inFile.readAll());
            if (doc.isObject()) {
                existing = doc.object();
            }
        }
        const QJsonObject outObj = serializeClipsToSidecarJson(m_objectId, m_clips, existing);
        QSaveFile outFile(path);
        if (!outFile.open(QIODevice::WriteOnly)) {
            emit statusMessage(tr("Failed to open sidecar for writing"));
            return;
        }
        outFile.write(QJsonDocument(outObj).toJson(QJsonDocument::Indented));
        if (!outFile.commit()) {
            emit statusMessage(tr("Failed to commit sidecar file"));
            return;
        }
        emit statusMessage(tr("Saved animation sidecar: %1").arg(path));
    });

    connect(aux->importSidecarBtn, &QPushButton::clicked, this,
            [this, syncClipControls, rebuildTrackTreeForClip]() {
                const QVariant headlessProp = property("headlessSidecarPath");
                const QString path =
                    headlessProp.isValid()
                        ? headlessProp.toString().trimmed()
                        : QFileDialog::getOpenFileName(this, tr("Load Rig Animation Sidecar"),
                                                       QString(), tr("JSON Files (*.json)"));
                if (path.isEmpty()) {
                    return;
                }
                QFile inFile(path);
                if (!inFile.open(QIODevice::ReadOnly)) {
                    emit statusMessage(tr("Failed to open sidecar"));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(inFile.readAll());
                if (!doc.isObject() || !deserializeClipsFromSidecarJson(doc.object(), m_clips)) {
                    emit statusMessage(tr("Invalid studio_rig sidecar schema"));
                    return;
                }
                m_clipCombo->clear();
                for (const auto &clip : m_clips) {
                    m_clipCombo->addItem(clip.name, clip.id);
                }
                m_activeClipIndex = 0;
                syncClipControls();
                rebuildTrackTreeForClip();
                setCurrentFrame(0.0);
                onBakeAndSyncTracks();
                emit statusMessage(tr("Loaded animation sidecar: %1").arg(path));
            });

    // Install keyboard + visibility event filter across widget and interactive children
    auto *filter = new TimelineEventFilter(
        this, [this]() { updatePlaybackTimerState(); },
        [fcurve]() { fcurve->frameFitVisibleChannels(); },
        [this, aux]() {
            if (m_activeClipIndex >= m_clips.size() || aux->selectedKeys.empty()) {
                return;
            }
            const auto &clip = m_clips[m_activeClipIndex];
            const uint32_t animId = static_cast<uint32_t>(std::max(0, clip.id));
            std::vector<FCurveKeySnapshot> oldKeys = collectClipKeys(clip);
            std::vector<FCurveKeySnapshot> newKeys;
            newKeys.reserve(oldKeys.size());
            for (const auto &k : oldKeys) {
                if (!aux->isSelected(k.boneIndex, k.channelIndex, k.frame)) {
                    newKeys.push_back(k);
                }
            }
            if (newKeys.size() == oldKeys.size()) {
                return;
            }
            aux->selectedKeys.clear();
            commitClipKeyEdit(this, animId, QStringLiteral("Delete keyframes"), std::move(oldKeys),
                              std::move(newKeys));
        },
        [this](int dir) {
            if (m_activeClipIndex >= m_clips.size()) {
                return;
            }
            std::set<double> frames;
            for (const auto &ch : m_clips[m_activeClipIndex].channels) {
                if (!ch.visible) {
                    continue;
                }
                for (const auto &k : ch.keys) {
                    frames.insert(k.frame);
                }
            }
            if (frames.empty()) {
                return;
            }
            if (dir > 0) {
                auto it = frames.upper_bound(m_currentFrame + 1e-4);
                if (it != frames.end()) {
                    setCurrentFrame(*it);
                }
            } else {
                auto it = frames.lower_bound(m_currentFrame - 1e-4);
                if (it != frames.begin()) {
                    --it;
                    setCurrentFrame(*it);
                }
            }
        },
        this);

    aux->eventFilter = filter;
    installEventFilter(filter);
    m_dopeCanvas->installEventFilter(filter);
    m_fcurveCanvas->installEventFilter(filter);
    m_trackTree->installEventFilter(filter);
    if (QWidget *win = window(); win && win != this) {
        win->installEventFilter(filter);
        aux->watchedWindow = win;
    }
}

DopeSheetTimelineWidget::~DopeSheetTimelineWidget()
{
    m_playTimer.stop();
    if (m_dopeCanvas) {
        static_cast<DopeSheetCanvas *>(m_dopeCanvas)->clearOwner();
    }
    if (m_fcurveCanvas) {
        static_cast<FCurveCanvas *>(m_fcurveCanvas)->clearOwner();
    }
    if (auto *aux = auxState(this)) {
        if (QObject *ef = aux->eventFilter) {
            aux->eventFilter = nullptr;
            removeEventFilter(ef);
            if (m_dopeCanvas) {
                m_dopeCanvas->removeEventFilter(ef);
            }
            if (m_fcurveCanvas) {
                m_fcurveCanvas->removeEventFilter(ef);
            }
            if (m_trackTree) {
                m_trackTree->removeEventFilter(ef);
            }
            if (aux->watchedWindow) {
                aux->watchedWindow->removeEventFilter(ef);
                aux->watchedWindow.clear();
            }
            static_cast<TimelineEventFilter *>(ef)->disarm();
        }
    }
}

void DopeSheetTimelineWidget::loadPuppetAnimations(const we_puppet_t *puppet, int32_t objectId)
{
    m_objectId = objectId;
    auto *aux = auxState(this);
    if (aux) {
        aux->objectId = objectId;
        aux->boneNames.clear();
        aux->selectedKeys.clear();
    }

    m_clips.clear();
    m_boneCount = 0;

    if (puppet) {
        m_boneCount = std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES);
        if (aux && puppet->bones) {
            for (uint32_t b = 0; b < m_boneCount; ++b) {
                QString bn = QString::fromUtf8(puppet->bones[b].name);
                if (bn.isEmpty()) {
                    bn = QStringLiteral("Bone %1").arg(b);
                }
                aux->boneNames.push_back(bn);
            }
        }

        const uint32_t animCnt = std::min<uint32_t>(puppet->anim_count, WE_MDL_MAX_ANIMS);
        for (uint32_t a = 0; a < animCnt && puppet->anims; ++a) {
            const we_mdl_anim_t &srcAnim = puppet->anims[a];
            TimelineAnimClip clip;
            clip.id = srcAnim.id;
            clip.name = QString::fromUtf8(srcAnim.name);
            if (clip.name.isEmpty()) {
                clip.name = QStringLiteral("Clip %1").arg(a);
            }
            clip.mode = srcAnim.mode;
            clip.fps = (std::isfinite(srcAnim.fps) && srcAnim.fps > 0.0f)
                           ? std::clamp(srcAnim.fps, 1.0f, 1000.0f)
                           : 30.0f;
            clip.lengthFrames = std::clamp(srcAnim.length, 1, 1000000);

            const auto *rigCtx =
                puppet->rig_ctx ? static_cast<const we_rig_pose_ctx_t *>(puppet->rig_ctx) : nullptr;
            const uint32_t lCnt = std::min<uint32_t>(puppet->layer_count, WE_MDL_MAX_LAYERS);
            for (uint32_t l = 0; l < lCnt; ++l) {
                const we_puppet_layer_t &lay = puppet->layers[l];
                if (lay.anim_id == srcAnim.id ||
                    (lay.anim_name[0] != '\0' && QString::fromUtf8(lay.anim_name) == clip.name)) {
                    clip.layerRate = std::clamp(finiteOrF(lay.rate, 1.0f), -16.0f, 16.0f);
                    clip.layerBlend = std::clamp(finiteOrF(lay.blend, 1.0f), 0.0f, 1.0f);
                    clip.layerVisible = lay.visible;
                    if (rigCtx && rigCtx->layer_blend_modes[l] <= WE_ANIM_BLEND_ADDITIVE) {
                        clip.blendMode = rigCtx->layer_blend_modes[l];
                    }
                    break;
                }
            }

            const uint32_t trCnt = std::min<uint32_t>(srcAnim.track_count, WE_MDL_MAX_BONES);
            for (uint32_t t = 0; t < trCnt && srcAnim.tracks; ++t) {
                const we_mdl_bone_track_t &tr = srcAnim.tracks[t];
                if (tr.bone_index < 0 || tr.bone_index >= static_cast<int32_t>(m_boneCount) ||
                    !tr.keyframes || tr.kf_count == 0u) {
                    continue;
                }

                for (uint32_t c = 0; c < 9u; ++c) {
                    auto sampleAt = [&tr, c](uint32_t f) -> double {
                        const we_mdl_keyframe_t &kf = tr.keyframes[f];
                        if (c < 3u) {
                            return static_cast<double>(finiteOrF(kf.pos[c], 0.0f));
                        }
                        if (c < 6u) {
                            return static_cast<double>(finiteOrF(kf.angle[c - 3u], 0.0f));
                        }
                        return static_cast<double>(finiteOrF(kf.scale[c - 6u], 1.0f));
                    };

                    const double defVal = channelDefaultValue(c);
                    double minV = sampleAt(0u);
                    double maxV = minV;
                    for (uint32_t f = 1; f < tr.kf_count; ++f) {
                        const double v = sampleAt(f);
                        minV = std::min(minV, v);
                        maxV = std::max(maxV, v);
                    }
                    if ((maxV - minV) < 1e-6 && std::abs(minV - defVal) < 1e-6) {
                        continue;
                    }

                    TimelineFCurveChannel ch;
                    ch.boneIndex = tr.bone_index;
                    ch.channelIndex = c;
                    ch.label = defaultChannelLabel(c);
                    ch.visible = true;

                    // Keep sparse keyframes when dense frames are piecewise-linear within 1e-4.
                    std::vector<uint32_t> keepIndices;
                    keepIndices.push_back(0u);
                    if (tr.kf_count > 2u) {
                        uint32_t anchor = 0u;
                        for (uint32_t f = 1; f + 1 < tr.kf_count; ++f) {
                            const double v0 = sampleAt(anchor);
                            const double v2 = sampleAt(f + 1u);
                            const double alpha = static_cast<double>(f - anchor) /
                                                 static_cast<double>((f + 1u) - anchor);
                            const double interp = v0 + alpha * (v2 - v0);
                            if (std::abs(sampleAt(f) - interp) > 1e-4) {
                                keepIndices.push_back(f);
                                anchor = f;
                            }
                        }
                    }
                    if (tr.kf_count > 1u) {
                        keepIndices.push_back(tr.kf_count - 1u);
                    }

                    for (uint32_t idxF : keepIndices) {
                        FCurveKeySnapshot k;
                        k.animId = static_cast<uint32_t>(std::max(0, clip.id));
                        k.boneIndex = tr.bone_index;
                        k.channelIndex = c;
                        k.frame = static_cast<double>(idxF);
                        k.value = sampleAt(idxF);
                        k.tangentMode = static_cast<uint8_t>(KeyTangentMode::Vector);
                        ch.keys.push_back(k);
                    }
                    clip.channels.push_back(std::move(ch));
                }
            }
            m_clips.push_back(std::move(clip));
        }
    }

    if (m_clips.empty()) {
        TimelineAnimClip fallback;
        fallback.id = 100;
        fallback.name = QStringLiteral("Idle");
        fallback.fps = 30.0f;
        fallback.lengthFrames = 120;
        m_clips.push_back(std::move(fallback));
    }

    m_activeClipIndex = 0;
    if (m_clipCombo) {
        const QSignalBlocker blocker(m_clipCombo);
        m_clipCombo->clear();
        for (const auto &clip : m_clips) {
            m_clipCombo->addItem(clip.name, clip.id);
        }
        m_clipCombo->setCurrentIndex(0);
    }
    if (aux && m_animModeCombo && m_fpsSpin && m_lengthSpin && m_layerRateSpin &&
        m_layerBlendSpin) {
        aux->updatingUi = true;
        const auto &c = m_clips[0];
        m_animModeCombo->setCurrentIndex(m_animModeCombo->findData(static_cast<int>(c.mode)));
        if (aux->blendModeCombo) {
            aux->blendModeCombo->setCurrentIndex(
                aux->blendModeCombo->findData(static_cast<int>(c.blendMode)));
        }
        if (aux->layerVisibleCheck) {
            aux->layerVisibleCheck->setChecked(c.layerVisible);
        }
        m_fpsSpin->setValue(static_cast<double>(c.fps));
        m_lengthSpin->setValue(c.lengthFrames);
        m_layerRateSpin->setValue(static_cast<double>(c.layerRate));
        m_layerBlendSpin->setValue(static_cast<double>(c.layerBlend));
        aux->updatingUi = false;
    }

    rebuildTrackTreeUi(m_trackTree, aux, &m_clips[0], 0.0);

    setCurrentFrame(0.0);
}

void DopeSheetTimelineWidget::setCurrentFrame(double frame)
{
    double fps = 30.0;
    int32_t lengthFrames = 120;
    if (m_activeClipIndex < m_clips.size()) {
        const auto &clip = m_clips[m_activeClipIndex];
        fps = (std::isfinite(clip.fps) && clip.fps > 0.0f) ? static_cast<double>(clip.fps) : 30.0;
        lengthFrames = std::max(1, clip.lengthFrames);
    }

    const double maxFrame = static_cast<double>(lengthFrames - 1);
    const double cleanFrame = std::clamp(finiteOr(frame, 0.0), 0.0, maxFrame);
    m_currentFrame = cleanFrame;
    const double timeSec = cleanFrame / fps;

    if (m_timecodeLabel) {
        const int totalCentis = static_cast<int>(std::round(timeSec * 100.0));
        const int mins = totalCentis / 6000;
        const int secs = (totalCentis / 100) % 60;
        const int centis = totalCentis % 100;
        const int frameInt = static_cast<int>(std::round(cleanFrame));
        m_timecodeLabel->setText(QStringLiteral("%1:%2.%3 | f%4 / %5")
                                     .arg(mins, 2, 10, QLatin1Char('0'))
                                     .arg(secs, 2, 10, QLatin1Char('0'))
                                     .arg(centis, 2, 10, QLatin1Char('0'))
                                     .arg(frameInt, 4, 10, QLatin1Char('0'))
                                     .arg(lengthFrames, 4, 10, QLatin1Char('0')));
    }

    if (m_trackTree && m_activeClipIndex < m_clips.size()) {
        const auto &clip = m_clips[m_activeClipIndex];
        for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
            for (int c = 0; c < boneItem->childCount(); ++c) {
                QTreeWidgetItem *chItem = boneItem->child(c);
                const int32_t boneIdx = chItem->data(0, Qt::UserRole).toInt();
                const uint32_t chIdx = chItem->data(0, Qt::UserRole + 1).toUInt();
                for (const auto &ch : clip.channels) {
                    if (ch.boneIndex == boneIdx && ch.channelIndex == chIdx) {
                        chItem->setText(
                            1, QString::number(evaluateChannelAtFrame(ch, cleanFrame), 'f', 3));
                        break;
                    }
                }
            }
        }
    }

    if (m_dopeCanvas) {
        m_dopeCanvas->update();
    }
    if (m_fcurveCanvas) {
        m_fcurveCanvas->update();
    }

    emit frameChanged(m_currentFrame, timeSec);
    dispatchScrubIpc(m_ipcClient, m_objectId, static_cast<float>(timeSec), !m_isPlaying);
}

void DopeSheetTimelineWidget::setPlaying(bool playing)
{
    m_isPlaying = playing;
    if (m_playBtn) {
        const QSignalBlocker blocker(m_playBtn);
        m_playBtn->setChecked(playing);
        m_playBtn->setText(playing ? tr("Pause") : tr("Play"));
    }
    updatePlaybackTimerState();
}

void DopeSheetTimelineWidget::setViewMode(TimelineViewMode mode)
{
    m_viewMode = mode;
    if (m_viewModeCombo) {
        const QSignalBlocker blocker(m_viewModeCombo);
        const int idx = m_viewModeCombo->findData(static_cast<int>(mode));
        if (idx >= 0) {
            m_viewModeCombo->setCurrentIndex(idx);
        }
    }

    if (m_dopeCanvas && m_fcurveCanvas) {
        switch (mode) {
        case TimelineViewMode::DopeSheet:
            m_dopeCanvas->show();
            m_fcurveCanvas->hide();
            break;
        case TimelineViewMode::FCurve:
            m_dopeCanvas->hide();
            m_fcurveCanvas->show();
            break;
        case TimelineViewMode::Split:
        default:
            m_dopeCanvas->show();
            m_fcurveCanvas->show();
            if (m_mainSplitter) {
                m_mainSplitter->setSizes({160, 220});
            }
            break;
        }
    }
}

void DopeSheetTimelineWidget::setOnionSkinEnabled(bool enabled)
{
    m_onionEnabled = enabled;
    if (m_onionCheck) {
        const QSignalBlocker blocker(m_onionCheck);
        m_onionCheck->setChecked(enabled);
    }
    auto *aux = auxState(this);
    const int before = (aux && aux->onionBeforeSpin) ? aux->onionBeforeSpin->value() : 2;
    const int after = (aux && aux->onionAfterSpin) ? aux->onionAfterSpin->value() : 2;
    const float opacity =
        (aux && aux->onionOpacitySpin) ? static_cast<float>(aux->onionOpacitySpin->value()) : 0.30f;
    emit onionSkinChanged(enabled, before, after, opacity);
}

double DopeSheetTimelineWidget::evaluateChannelAtFrame(const TimelineFCurveChannel &channel,
                                                       double frame)
{
    if (channel.keys.empty()) {
        return 0.0;
    }
    if (channel.keys.size() == 1u) {
        return finiteOr(channel.keys[0].value, 0.0);
    }
    const PreparedChannel prep = prepareChannel(channel);
    return evaluatePreparedChannel(prep, frame);
}

void DopeSheetTimelineWidget::bakeClipToTracks(uint32_t clipIndex, uint32_t boneCount,
                                               std::vector<we_mdl_bone_track_t> &outTracks,
                                               std::vector<we_mdl_keyframe_t> &outStorage) const
{
    outTracks.clear();
    outStorage.clear();

    if (clipIndex >= m_clips.size() || boneCount == 0u) {
        return;
    }

    const uint32_t cappedBones = std::min<uint32_t>(boneCount, WE_MDL_MAX_BONES);
    const TimelineAnimClip &clip = m_clips[clipIndex];

    // Group animated channels by strictly distinct bone_index for MDLA0003 compatibility.
    std::map<int32_t, std::vector<const TimelineFCurveChannel *>> boneChannels;
    for (const auto &ch : clip.channels) {
        if (ch.boneIndex < 0 || ch.boneIndex >= static_cast<int32_t>(cappedBones) ||
            ch.channelIndex >= 9u || ch.keys.empty()) {
            continue;
        }
        boneChannels[ch.boneIndex].push_back(&ch);
    }

    if (boneChannels.empty()) {
        return;
    }

    const size_t trackCount = boneChannels.size();
    const size_t headerBytes = sizeof(waywal_ipc_puppet_set_anim_tracks_t) +
                               sizeof(waywal_ipc_anim_clip_desc_t) +
                               trackCount * sizeof(waywal_ipc_anim_track_desc_t);
    const size_t availKeyBytes = (WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES > headerBytes)
                                     ? (WAYWAL_IPC_RIGGING_MAX_PAYLOAD_BYTES - headerBytes)
                                     : 0u;
    const size_t maxTotalKeys = std::min<size_t>(WAYWAL_IPC_RIGGING_MAX_KEYFRAMES,
                                                 availKeyBytes / sizeof(waywal_ipc_mdl_keyframe_t));
    if (maxTotalKeys < trackCount) {
        return;
    }
    const uint32_t maxPerTrackFrames = static_cast<uint32_t>(maxTotalKeys / trackCount);
    const uint32_t requestedFrames = static_cast<uint32_t>(std::clamp<int32_t>(
        clip.lengthFrames, 1, static_cast<int32_t>(WAYWAL_IPC_RIGGING_MAX_KEYFRAMES)));
    const uint32_t kfCount = std::min(requestedFrames, maxPerTrackFrames);
    const bool decimated = (kfCount < static_cast<uint32_t>(std::max(1, clip.lengthFrames)));

    // Reserve full backing storage before populating track pointers so reallocation never
    // invalidates them.
    outStorage.resize(trackCount * static_cast<size_t>(kfCount));
    outTracks.reserve(trackCount);

    size_t trackOffset = 0;
    for (const auto &[boneIdx, channels] : boneChannels) {
        we_mdl_keyframe_t *dstFrames = outStorage.data() + trackOffset;
        for (uint32_t f = 0; f < kfCount; ++f) {
            dstFrames[f] = we_mdl_keyframe_t{
                .pos = {0.0f, 0.0f, 0.0f},
                .angle = {0.0f, 0.0f, 0.0f},
                .scale = {1.0f, 1.0f, 1.0f},
            };
        }

        for (const TimelineFCurveChannel *ch : channels) {
            const PreparedChannel prep = prepareChannel(*ch);
            for (uint32_t f = 0; f < kfCount; ++f) {
                const double sampleFrame =
                    (decimated && kfCount > 1u && clip.lengthFrames > 1)
                        ? (static_cast<double>(f) * static_cast<double>(clip.lengthFrames - 1) /
                           static_cast<double>(kfCount - 1u))
                        : static_cast<double>(f);
                const double val = evaluatePreparedChannel(prep, sampleFrame);
                const float fv = std::isfinite(val)
                                     ? static_cast<float>(val)
                                     : static_cast<float>(channelDefaultValue(ch->channelIndex));
                if (ch->channelIndex < 3u) {
                    dstFrames[f].pos[ch->channelIndex] = fv;
                } else if (ch->channelIndex < 6u) {
                    dstFrames[f].angle[ch->channelIndex - 3u] = fv;
                } else {
                    dstFrames[f].scale[ch->channelIndex - 6u] = fv;
                }
            }
        }

        we_mdl_bone_track_t tr{};
        tr.bone_index = boneIdx;
        tr.unk = 0;
        tr.kf_count = kfCount;
        tr.keyframes = dstFrames;
        outTracks.push_back(tr);
        trackOffset += kfCount;
    }
}

void DopeSheetTimelineWidget::onPlayPauseToggled() { setPlaying(!m_isPlaying); }

void DopeSheetTimelineWidget::onStepFrame(int deltaFrames)
{
    setCurrentFrame(std::round(m_currentFrame) + static_cast<double>(deltaFrames));
}

void DopeSheetTimelineWidget::onInsertKeyframeAtPlayhead()
{
    if (m_activeClipIndex >= m_clips.size()) {
        return;
    }

    auto *aux = auxState(this);
    const uint32_t clipIdx = m_activeClipIndex;
    TimelineAnimClip &clip = m_clips[clipIdx];
    const double frame = std::clamp(std::round(m_currentFrame), 0.0,
                                    static_cast<double>(std::max(1, clip.lengthFrames) - 1));

    int32_t targetBone = 0;
    int targetChannel = -1;
    if (m_trackTree && m_trackTree->currentItem()) {
        targetBone = std::max(0, m_trackTree->currentItem()->data(0, Qt::UserRole).toInt());
        const QVariant chVar = m_trackTree->currentItem()->data(0, Qt::UserRole + 1);
        if (chVar.isValid()) {
            targetChannel = chVar.toInt();
        }
    }

    std::vector<FCurveKeySnapshot> oldKeys = collectClipKeys(clip);
    std::vector<FCurveKeySnapshot> newKeys = oldKeys;

    auto upsertChannelKey = [&clip, &newKeys, frame, targetBone](uint32_t chIdx) {
        double val = channelDefaultValue(chIdx);
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex == targetBone && ch.channelIndex == chIdx) {
                val = ch.keys.empty() ? channelDefaultValue(chIdx)
                                      : evaluateChannelAtFrame(ch, frame);
                break;
            }
        }
        for (auto &k : newKeys) {
            if (k.boneIndex == targetBone && k.channelIndex == chIdx &&
                std::abs(k.frame - frame) < 0.5) {
                k.value = val;
                return;
            }
        }
        FCurveKeySnapshot k;
        k.animId = static_cast<uint32_t>(std::max(0, clip.id));
        k.boneIndex = targetBone;
        k.channelIndex = chIdx;
        k.frame = frame;
        k.value = val;
        k.tangentMode = static_cast<uint8_t>(KeyTangentMode::Auto);
        newKeys.push_back(k);
    };

    if (targetChannel >= 0 && targetChannel < 9) {
        upsertChannelKey(static_cast<uint32_t>(targetChannel));
        if (aux) {
            aux->selectOnly(targetBone, static_cast<uint32_t>(targetChannel), frame);
        }
    } else {
        // Insert keyframe on primary 2D channels (pos.x, pos.y, angle.z) if channel is unspecified.
        static constexpr uint32_t kDefaultChans[3] = {0u, 1u, 5u};
        for (uint32_t c : kDefaultChans) {
            upsertChannelKey(c);
        }
        if (aux) {
            aux->selectOnly(targetBone, 5u, frame);
        }
    }

    const uint32_t animId = static_cast<uint32_t>(std::max(0, clip.id));
    commitClipKeyEdit(this, animId, QStringLiteral("Insert keyframe"), std::move(oldKeys),
                      std::move(newKeys));

    emit statusMessage(tr("Inserted keyframe at frame %1").arg(static_cast<int>(frame)));
}

void DopeSheetTimelineWidget::onSetSelectedTangentMode(KeyTangentMode mode)
{
    if (m_activeClipIndex >= m_clips.size()) {
        return;
    }
    auto *aux = auxState(this);
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
    const uint32_t animId = static_cast<uint32_t>(std::max(0, clip.id));

    std::vector<FCurveKeySnapshot> oldKeys = collectClipKeys(clip);
    std::vector<FCurveKeySnapshot> newKeys = oldKeys;
    bool changed = false;

    for (auto &k : newKeys) {
        const bool matchSel = aux && !aux->selectedKeys.empty()
                                  ? aux->isSelected(k.boneIndex, k.channelIndex, k.frame)
                                  : (std::abs(k.frame - m_currentFrame) < 0.5);
        if (matchSel) {
            k.tangentMode = static_cast<uint8_t>(mode);
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    commitClipKeyEdit(this, animId, QStringLiteral("Change keyframe tangent mode"),
                      std::move(oldKeys), std::move(newKeys));
}

void DopeSheetTimelineWidget::applyUndoKeySnapshot(uint32_t animId,
                                                   const std::vector<FCurveKeySnapshot> &keys)
{
    auto it = std::find_if(m_clips.begin(), m_clips.end(), [animId](const TimelineAnimClip &clip) {
        return clip.id >= 0 && static_cast<uint32_t>(clip.id) == animId;
    });
    if (it == m_clips.end()) {
        return;
    }

    const uint32_t targetIdx = static_cast<uint32_t>(std::distance(m_clips.begin(), it));
    applyClipKeysSnapshot(*it, keys);

    auto *aux = auxState(this);
    if (targetIdx == m_activeClipIndex) {
        rebuildTrackTreeUi(m_trackTree, aux, &(*it), m_currentFrame);
    }

    if (!aux || !aux->dragPreviewSuppressIpc) {
        if (targetIdx == m_activeClipIndex) {
            onBakeAndSyncTracks();
        } else {
            const uint32_t effectiveBones = std::max<uint32_t>(1u, m_boneCount);
            std::vector<we_mdl_bone_track_t> tracks;
            std::vector<we_mdl_keyframe_t> storage;
            bakeClipToTracks(targetIdx, effectiveBones, tracks, storage);
            dispatchAnimTracksIpc(m_ipcClient, m_objectId, m_clips, tracks, storage, targetIdx);
        }
    }

    if (m_dopeCanvas) {
        m_dopeCanvas->update();
    }
    if (m_fcurveCanvas) {
        m_fcurveCanvas->update();
    }
}

void DopeSheetTimelineWidget::onBakeAndSyncTracks()
{
    if (m_activeClipIndex >= m_clips.size()) {
        return;
    }
    const uint32_t effectiveBones = std::max<uint32_t>(1u, m_boneCount);
    std::vector<we_mdl_bone_track_t> tracks;
    std::vector<we_mdl_keyframe_t> storage;
    bakeClipToTracks(m_activeClipIndex, effectiveBones, tracks, storage);
    dispatchAnimTracksIpc(m_ipcClient, m_objectId, m_clips, tracks, storage, m_activeClipIndex);
}

void DopeSheetTimelineWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (auto *aux = auxState(this); aux && aux->eventFilter) {
        QWidget *win = window();
        if (win && win != this && aux->watchedWindow != win) {
            if (aux->watchedWindow) {
                aux->watchedWindow->removeEventFilter(aux->eventFilter);
            }
            win->installEventFilter(aux->eventFilter);
            aux->watchedWindow = win;
        }
    }
    m_windowMinimized = window() && ((window()->windowState() & Qt::WindowMinimized) != 0);
    updatePlaybackTimerState();
}

void DopeSheetTimelineWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_playTimer.stop();
}

void DopeSheetTimelineWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ParentChange) {
        if (auto *aux = auxState(this); aux && aux->eventFilter) {
            if (aux->watchedWindow) {
                aux->watchedWindow->removeEventFilter(aux->eventFilter);
                aux->watchedWindow.clear();
            }
            if (QWidget *win = window(); win && win != this) {
                win->installEventFilter(aux->eventFilter);
                aux->watchedWindow = win;
            }
        }
        updatePlaybackTimerState();
    } else if (event->type() == QEvent::WindowStateChange) {
        const bool topMin = window() && ((window()->windowState() & Qt::WindowMinimized) != 0);
        m_windowMinimized = ((windowState() & Qt::WindowMinimized) != 0) || topMin;
        updatePlaybackTimerState();
    }
}

void DopeSheetTimelineWidget::onPlayTick()
{
    const bool winMinimized = window() && ((window()->windowState() & Qt::WindowMinimized) != 0);
    if (!isVisible() || !m_isPlaying || m_windowMinimized || winMinimized || m_scrubbing) {
        m_playTimer.stop();
        return;
    }

    const qint64 elapsedNs = m_frameClock.restart();
    const double dtSec = std::clamp(static_cast<double>(elapsedNs) * 1e-9, 0.0, 0.1);

    double fps = 30.0;
    double rate = 1.0;
    int32_t lengthFrames = 120;
    we_mdl_anim_mode_t mode = WE_ANIM_LOOP;

    if (m_activeClipIndex < m_clips.size()) {
        const auto &clip = m_clips[m_activeClipIndex];
        fps = (std::isfinite(clip.fps) && clip.fps > 0.0f) ? static_cast<double>(clip.fps) : 30.0;
        rate = std::isfinite(clip.layerRate)
                   ? std::clamp(static_cast<double>(clip.layerRate), -16.0, 16.0)
                   : 1.0;
        lengthFrames = std::max(1, clip.lengthFrames);
        mode = clip.mode;
    }

    const double maxFrame = static_cast<double>(lengthFrames - 1);
    auto *aux = auxState(this);
    const int mirrorDir = aux ? aux->mirrorDirection : 1;

    double nextFrame =
        m_currentFrame +
        dtSec * fps * rate * (mode == WE_ANIM_MIRROR ? static_cast<double>(mirrorDir) : 1.0);

    if (mode == WE_ANIM_SINGLE) {
        if (nextFrame >= maxFrame) {
            setCurrentFrame(maxFrame);
            setPlaying(false);
            return;
        }
        if (nextFrame <= 0.0) {
            setCurrentFrame(0.0);
            setPlaying(false);
            return;
        }
    } else if (mode == WE_ANIM_MIRROR) {
        if (maxFrame <= 0.0) {
            nextFrame = 0.0;
        } else if (nextFrame >= maxFrame) {
            nextFrame = std::max(0.0, 2.0 * maxFrame - nextFrame);
            if (aux) {
                aux->mirrorDirection = -mirrorDir;
            }
        } else if (nextFrame <= 0.0) {
            nextFrame = std::min(maxFrame, -nextFrame);
            if (aux) {
                aux->mirrorDirection = -mirrorDir;
            }
        }
    } else {
        if (maxFrame <= 0.0) {
            nextFrame = 0.0;
        } else {
            nextFrame = std::fmod(nextFrame, maxFrame);
            if (nextFrame < 0.0) {
                nextFrame += maxFrame;
            }
        }
    }

    setCurrentFrame(nextFrame);
}

void DopeSheetTimelineWidget::updatePlaybackTimerState()
{
    const bool winMinimized = window() && ((window()->windowState() & Qt::WindowMinimized) != 0);
    m_windowMinimized = ((windowState() & Qt::WindowMinimized) != 0) || winMinimized;
    const bool shouldTick = isVisible() && m_isPlaying && !m_windowMinimized && !m_scrubbing;
    if (shouldTick) {
        if (!m_playTimer.isActive()) {
            m_frameClock.restart();
            m_playTimer.start();
        }
    } else if (m_playTimer.isActive()) {
        m_playTimer.stop();
    }
}

} // namespace waywal::studio
