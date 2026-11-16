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
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QWindowStateChangeEvent>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waywal::studio {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kPi = 3.14159265358979323846;
constexpr int kRulerHeightPx = 26;
constexpr double kMinZoomPxPerFrame = 2.0;
constexpr double kMaxZoomPxPerFrame = 80.0;
constexpr double kMinLeftGutterPx = 48.0;
constexpr int64_t kCoalesceWindowMs = 300;
constexpr size_t kInvalidKeyIndex = std::numeric_limits<size_t>::max();

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

[[nodiscard]] uint32_t toWireAnimId(int32_t clipId) noexcept
{
    return static_cast<uint32_t>(clipId);
}

[[nodiscard]] double snapHalfPx(double logicalCoord, double dpr) noexcept
{
    const double s = std::max(1.0, dpr);
    return (std::floor(logicalCoord * s) + 0.5) / s;
}

[[nodiscard]] QFont timelineUiMonoFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPixelSize(11);
    f.setStyleHint(QFont::Monospace);
    return f;
}

[[nodiscard]] double measureLeftGutterPx()
{
    const QFontMetricsF fm(timelineUiMonoFont());
    const double w = fm.horizontalAdvance(QStringLiteral("-999.99"));
    return std::max(kMinLeftGutterPx, std::ceil(w + 14.0));
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

void prepareChannelInto(const TimelineFCurveChannel &channel, PreparedChannel &prep,
                        std::vector<size_t> &orderScratch)
{
    prep.keys.clear();
    if (channel.keys.empty()) {
        return;
    }

    const size_t n = channel.keys.size();
    prep.keys.resize(n);
    orderScratch.resize(n);
    for (size_t i = 0; i < n; ++i) {
        orderScratch[i] = i;
    }

    const bool alreadySorted =
        std::is_sorted(channel.keys.begin(), channel.keys.end(),
                       [](const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) {
                           return finiteOr(a.frame, 0.0) < finiteOr(b.frame, 0.0);
                       });
    if (!alreadySorted) {
        std::stable_sort(orderScratch.begin(), orderScratch.end(),
                         [&channel](size_t ia, size_t ib) {
                             return finiteOr(channel.keys[ia].frame, 0.0) <
                                    finiteOr(channel.keys[ib].frame, 0.0);
                         });
    }

    const bool isAngle = (channel.channelIndex >= 3u && channel.channelIndex <= 5u);
    double prevRaw = 0.0;
    double prevUnwrapped = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const FCurveKeySnapshot &src = channel.keys[orderScratch[i]];
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
}

[[nodiscard]] PreparedChannel prepareChannel(const TimelineFCurveChannel &channel)
{
    PreparedChannel prep;
    std::vector<size_t> order;
    prepareChannelInto(channel, prep, order);
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
    size_t keyIndex{kInvalidKeyIndex};
    double frame{0.0};

    [[nodiscard]] bool matches(int32_t b, uint32_t ch, size_t ki, double f) const noexcept
    {
        if (boneIndex != b || channelIndex != ch) {
            return false;
        }
        if (keyIndex != kInvalidKeyIndex && ki != kInvalidKeyIndex) {
            return keyIndex == ki;
        }
        return std::abs(frame - f) < 1e-4;
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

// Wrapping toolbar layout so dense mixer/transport strips reflow cleanly at 1280x720.
class TimelineFlowLayout final : public QLayout {
  public:
    explicit TimelineFlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 4,
                                int vSpacing = 4)
        : QLayout(parent), m_hSpacing(hSpacing), m_vSpacing(vSpacing)
    {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~TimelineFlowLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem *item) override { m_items.push_back(item); }

    [[nodiscard]] int count() const override { return static_cast<int>(m_items.size()); }

    [[nodiscard]] QLayoutItem *itemAt(int index) const override
    {
        return (index >= 0 && index < count()) ? m_items[static_cast<size_t>(index)] : nullptr;
    }

    [[nodiscard]] QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= count()) {
            return nullptr;
        }
        QLayoutItem *item = m_items[static_cast<size_t>(index)];
        m_items.erase(m_items.begin() + index);
        return item;
    }

    [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }

    [[nodiscard]] int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

    [[nodiscard]] QSize sizeHint() const override { return minimumSize(); }

    [[nodiscard]] QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem *item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins m = contentsMargins();
        size += QSize(m.left() + m.right(), m.top() + m.bottom());
        return size;
    }

  private:
    int doLayout(const QRect &rect, bool testOnly) const
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        getContentsMargins(&left, &top, &right, &bottom);
        const QRect eff = rect.adjusted(left, top, -right, -bottom);
        int x = eff.x();
        int y = eff.y();
        int lineHeight = 0;

        for (QLayoutItem *item : m_items) {
            if (item->isEmpty()) {
                continue;
            }
            const QSize hint = item->sizeHint();
            int nextX = x + hint.width() + m_hSpacing;
            if (nextX - m_hSpacing > eff.right() && lineHeight > 0) {
                x = eff.x();
                y += lineHeight + m_vSpacing;
                nextX = x + hint.width() + m_hSpacing;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }
            x = nextX;
            lineHeight = std::max(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y() + bottom;
    }

    std::vector<QLayoutItem *> m_items;
    int m_hSpacing{4};
    int m_vSpacing{4};
};

[[nodiscard]] bool keysMatchExactly(const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) noexcept
{
    return a.boneIndex == b.boneIndex && a.channelIndex == b.channelIndex &&
           std::abs(a.frame - b.frame) < 1e-6 && std::abs(a.value - b.value) < 1e-6 &&
           a.tangentMode == b.tangentMode && a.handlesBroken == b.handlesBroken &&
           std::abs(a.handleInDx - b.handleInDx) < 1e-6 &&
           std::abs(a.handleInDy - b.handleInDy) < 1e-6 &&
           std::abs(a.handleOutDx - b.handleOutDx) < 1e-6 &&
           std::abs(a.handleOutDy - b.handleOutDy) < 1e-6;
}

class TimelineKeyPatchCommand final : public ICommand {
  public:
    using ApplyPatchFn = std::function<void(int32_t objectId, int32_t clipId,
                                            const std::vector<FCurveKeySnapshot> &removeKeys,
                                            const std::vector<FCurveKeySnapshot> &insertKeys,
                                            const std::vector<SelectedKeyRef> &selToRestore)>;

    TimelineKeyPatchCommand(int32_t objectId, int32_t clipId, QString label,
                            std::vector<FCurveKeySnapshot> oldDiff,
                            std::vector<FCurveKeySnapshot> newDiff,
                            std::vector<SelectedKeyRef> oldSel, std::vector<SelectedKeyRef> newSel,
                            ApplyPatchFn applyFn)
        : m_objectId(objectId), m_clipId(clipId), m_label(std::move(label)),
          m_oldDiff(std::move(oldDiff)), m_newDiff(std::move(newDiff)), m_oldSel(std::move(oldSel)),
          m_newSel(std::move(newSel)), m_applyFn(std::move(applyFn)),
          m_timestamp(std::chrono::steady_clock::now())
    {
    }

    void execute() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_objectId, m_clipId, m_oldDiff, m_newDiff, m_newSel);
        }
    }

    void undo() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_objectId, m_clipId, m_newDiff, m_oldDiff, m_oldSel);
        }
    }

    [[nodiscard]] QString describe() const override
    {
        return m_label.isEmpty() ? QStringLiteral("Edit keyframes") : m_label;
    }

    bool mergeWith(const ICommand *other) override
    {
        const auto *rhs = dynamic_cast<const TimelineKeyPatchCommand *>(other);
        if (!rhs || !isValid() || !rhs->isValid() || m_objectId != rhs->m_objectId ||
            m_clipId != rhs->m_clipId || m_label != rhs->m_label) {
            return false;
        }
        if (m_oldDiff.size() != m_newDiff.size() ||
            rhs->m_oldDiff.size() != rhs->m_newDiff.size() ||
            m_newDiff.size() != rhs->m_oldDiff.size() || m_newDiff.empty()) {
            return false;
        }
        for (size_t i = 0; i < m_newDiff.size(); ++i) {
            if (!keysMatchExactly(m_newDiff[i], rhs->m_oldDiff[i])) {
                return false;
            }
            const bool lhsFixed = (std::abs(m_oldDiff[i].frame - m_newDiff[i].frame) <= 1e-4);
            const bool rhsFixed =
                (std::abs(rhs->m_oldDiff[i].frame - rhs->m_newDiff[i].frame) <= 1e-4);
            if (lhsFixed != rhsFixed) {
                return false;
            }
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(rhs->m_timestamp - m_timestamp)
                .count();
        if (elapsedMs < 0 || elapsedMs > kCoalesceWindowMs) {
            return false;
        }
        m_newDiff = rhs->m_newDiff;
        m_newSel = rhs->m_newSel;
        m_timestamp = rhs->m_timestamp;
        if (rhs->m_applyFn) {
            m_applyFn = rhs->m_applyFn;
        }
        return true;
    }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        return sizeof(*this) + static_cast<size_t>(m_label.size()) * 2u +
               (m_oldDiff.capacity() + m_newDiff.capacity()) * sizeof(FCurveKeySnapshot) +
               (m_oldSel.capacity() + m_newSel.capacity()) * sizeof(SelectedKeyRef);
    }

    [[nodiscard]] bool isValid() const override
    {
        auto validKey = [](const FCurveKeySnapshot &k) {
            return std::isfinite(k.frame) && std::isfinite(k.value) &&
                   std::isfinite(k.handleInDx) && std::isfinite(k.handleInDy) &&
                   std::isfinite(k.handleOutDx) && std::isfinite(k.handleOutDy);
        };
        return std::all_of(m_oldDiff.begin(), m_oldDiff.end(), validKey) &&
               std::all_of(m_newDiff.begin(), m_newDiff.end(), validKey);
    }

  private:
    int32_t m_objectId{-1};
    int32_t m_clipId{100};
    QString m_label;
    std::vector<FCurveKeySnapshot> m_oldDiff;
    std::vector<FCurveKeySnapshot> m_newDiff;
    std::vector<SelectedKeyRef> m_oldSel;
    std::vector<SelectedKeyRef> m_newSel;
    ApplyPatchFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

struct ClipMetaState {
    we_mdl_anim_mode_t mode{WE_ANIM_LOOP};
    we_anim_blend_mode_t blendMode{WE_ANIM_BLEND_OVERRIDE};
    bool layerVisible{true};
    float fps{30.0f};
    int32_t lengthFrames{120};
    float layerRate{1.0f};
    float layerBlend{1.0f};
    std::vector<FCurveKeySnapshot> trimmedKeys;
};

class ClipMetaEditCommand final : public ICommand {
  public:
    using ApplyMetaFn = std::function<void(int32_t objectId, int32_t clipId,
                                           const ClipMetaState &state, bool restoringRemovedKeys)>;

    ClipMetaEditCommand(int32_t objectId, int32_t clipId, QString label, ClipMetaState oldState,
                        ClipMetaState newState, ApplyMetaFn applyFn)
        : m_objectId(objectId), m_clipId(clipId), m_label(std::move(label)),
          m_oldState(std::move(oldState)), m_newState(std::move(newState)),
          m_applyFn(std::move(applyFn)), m_timestamp(std::chrono::steady_clock::now())
    {
    }

    void execute() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_objectId, m_clipId, m_newState, false);
        }
    }

    void undo() override
    {
        if (m_applyFn && isValid()) {
            m_applyFn(m_objectId, m_clipId, m_oldState, true);
        }
    }

    [[nodiscard]] QString describe() const override { return m_label; }

    bool mergeWith(const ICommand *other) override
    {
        const auto *rhs = dynamic_cast<const ClipMetaEditCommand *>(other);
        if (!rhs || m_objectId != rhs->m_objectId || m_clipId != rhs->m_clipId ||
            m_label != rhs->m_label) {
            return false;
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(rhs->m_timestamp - m_timestamp)
                .count();
        if (elapsedMs < 0 || elapsedMs > kCoalesceWindowMs) {
            return false;
        }
        // Preserve cumulative trimmed keys from earliest clip length down to newest length.
        m_oldState.trimmedKeys.insert(m_oldState.trimmedKeys.end(),
                                      rhs->m_oldState.trimmedKeys.begin(),
                                      rhs->m_oldState.trimmedKeys.end());
        m_newState = rhs->m_newState;
        m_timestamp = rhs->m_timestamp;
        if (rhs->m_applyFn) {
            m_applyFn = rhs->m_applyFn;
        }
        return true;
    }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        return sizeof(*this) +
               (m_oldState.trimmedKeys.capacity() + m_newState.trimmedKeys.capacity()) *
                   sizeof(FCurveKeySnapshot);
    }

    [[nodiscard]] bool isValid() const override
    {
        return std::isfinite(m_oldState.fps) && m_oldState.fps > 0.0f &&
               std::isfinite(m_newState.fps) && m_newState.fps > 0.0f &&
               m_oldState.lengthFrames >= 1 && m_newState.lengthFrames >= 1;
    }

  private:
    int32_t m_objectId{-1};
    int32_t m_clipId{100};
    QString m_label;
    ClipMetaState m_oldState;
    ClipMetaState m_newState;
    ApplyMetaFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class ClipCollectionCommand final : public ICommand {
  public:
    struct Snapshot {
        int32_t objectId{-1};
        std::vector<TimelineAnimClip> clips;
        uint32_t activeClipIdx{0u};
        std::vector<SelectedKeyRef> selectedKeys;
    };

    using ApplyCollectionFn = std::function<void(const Snapshot &snap)>;

    ClipCollectionCommand(QString label, Snapshot oldSnap, Snapshot newSnap,
                          ApplyCollectionFn applyFn)
        : m_label(std::move(label)), m_oldSnap(std::move(oldSnap)), m_newSnap(std::move(newSnap)),
          m_applyFn(std::move(applyFn))
    {
    }

    void execute() override
    {
        if (m_applyFn && !m_newSnap.clips.empty()) {
            m_applyFn(m_newSnap);
        }
    }

    void undo() override
    {
        if (m_applyFn && !m_oldSnap.clips.empty()) {
            m_applyFn(m_oldSnap);
        }
    }

    [[nodiscard]] QString describe() const override { return m_label; }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        size_t bytes = sizeof(*this);
        for (const auto &c : m_oldSnap.clips) {
            for (const auto &ch : c.channels) {
                bytes += ch.keys.capacity() * sizeof(FCurveKeySnapshot);
            }
        }
        for (const auto &c : m_newSnap.clips) {
            for (const auto &ch : c.channels) {
                bytes += ch.keys.capacity() * sizeof(FCurveKeySnapshot);
            }
        }
        return bytes;
    }

  private:
    QString m_label;
    Snapshot m_oldSnap;
    Snapshot m_newSnap;
    ApplyCollectionFn m_applyFn;
};

[[nodiscard]] int32_t allocateUniqueClipId(const std::vector<TimelineAnimClip> &clips) noexcept
{
    int32_t candidate = 100;
    for (int guard = 0; guard < 100000; ++guard) {
        const bool used =
            std::any_of(clips.begin(), clips.end(),
                        [candidate](const TimelineAnimClip &c) { return c.id == candidate; });
        if (!used) {
            return candidate;
        }
        ++candidate;
    }
    return candidate;
}

void remapChannelAndKeyBones(std::vector<TimelineAnimClip> &clips,
                             const std::vector<int32_t> &oldToNew)
{
    for (auto &clip : clips) {
        clip.channels.erase(
            std::remove_if(clip.channels.begin(), clip.channels.end(),
                           [&oldToNew](const TimelineFCurveChannel &ch) {
                               return ch.boneIndex < 0 ||
                                      static_cast<size_t>(ch.boneIndex) >= oldToNew.size() ||
                                      oldToNew[static_cast<size_t>(ch.boneIndex)] < 0;
                           }),
            clip.channels.end());
        for (auto &ch : clip.channels) {
            ch.boneIndex = oldToNew[static_cast<size_t>(ch.boneIndex)];
            for (auto &k : ch.keys) {
                k.boneIndex = ch.boneIndex;
            }
        }
    }
}

[[nodiscard]] bool isEditableFocusWidget(QWidget *w, const QTreeWidget *timelineTree)
{
    if (!w) {
        return false;
    }
    if (qobject_cast<QAbstractSpinBox *>(w) != nullptr || qobject_cast<QLineEdit *>(w) != nullptr ||
        qobject_cast<QComboBox *>(w) != nullptr || qobject_cast<QTextEdit *>(w) != nullptr ||
        qobject_cast<QPlainTextEdit *>(w) != nullptr) {
        return true;
    }
    if (timelineTree && w != timelineTree && timelineTree->viewport() &&
        timelineTree->viewport()->isAncestorOf(w)) {
        return true;
    }
    if (auto *view = qobject_cast<const QAbstractItemView *>(w)) {
        return (view != timelineTree) || (view->indexWidget(view->currentIndex()) != nullptr);
    }
    return false;
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
                k.animId = toWireAnimId(clip.id);
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

[[nodiscard]] std::vector<uint32_t> sparsifyRdpIndices(const std::vector<double> &unwrapped,
                                                       double tolerance)
{
    const uint32_t n = static_cast<uint32_t>(unwrapped.size());
    std::vector<uint32_t> keep;
    if (n == 0u) {
        return keep;
    }
    if (n <= 2u) {
        for (uint32_t i = 0u; i < n; ++i) {
            keep.push_back(i);
        }
        return keep;
    }

    std::vector<uint8_t> marked(n, 0u);
    marked[0] = 1u;
    marked[n - 1u] = 1u;

    std::vector<std::pair<uint32_t, uint32_t>> stack;
    stack.emplace_back(0u, n - 1u);

    while (!stack.empty()) {
        const auto [start, end] = stack.back();
        stack.pop_back();
        if (end <= start + 1u) {
            continue;
        }
        const double y0 = unwrapped[start];
        const double y1 = unwrapped[end];
        const double span = static_cast<double>(end - start);

        double maxErr = -1.0;
        uint32_t bestIdx = start;
        for (uint32_t f = start + 1u; f < end; ++f) {
            const double alpha = static_cast<double>(f - start) / span;
            const double expected = y0 + alpha * (y1 - y0);
            const double err = std::abs(unwrapped[f] - expected);
            if (err > maxErr) {
                maxErr = err;
                bestIdx = f;
            }
        }
        if (maxErr > tolerance && bestIdx > start && bestIdx < end) {
            marked[bestIdx] = 1u;
            stack.emplace_back(start, bestIdx);
            stack.emplace_back(bestIdx, end);
        }
    }

    for (uint32_t i = 0u; i < n; ++i) {
        if (marked[i] != 0u) {
            keep.push_back(i);
        }
    }
    return keep;
}

struct TreeChannelBinding {
    uint32_t channelVecIdx{0u};
    QTreeWidgetItem *item{nullptr};
};

struct TimelineObjectState {
    uint32_t boneCount{0u};
    uint32_t activeClipIndex{0u};
    double currentFrame{0.0};
    QStringList boneNames;
    std::vector<TimelineAnimClip> clips;
    std::vector<SelectedKeyRef> selectedKeys;
};

} // namespace

struct DopeSheetTimelineWidget::Impl {
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

    QObject *eventFilter{nullptr};
    QPointer<QWidget> watchedWindow;

    QStringList boneNames;
    double scrollFrameOffset{0.0};
    double fcurveMinY{-3.5};
    double fcurveMaxY{3.5};
    double leftGutterPx{kMinLeftGutterPx};
    int mirrorDirection{1};
    bool updatingUi{false};
    bool dragPreviewSuppressIpc{false};

    std::vector<SelectedKeyRef> selectedKeys;
    std::vector<FCurveKeySnapshot> copiedKeys;
    double copiedAnchorFrame{0.0};

    std::unordered_map<int32_t, TimelineObjectState> objectStates;

    uint64_t clipDataVersion{1u};
    uint32_t cachedClipIdx{UINT32_MAX};
    uint64_t cachedVersion{0u};
    std::vector<PreparedChannel> cachedPrepared;
    std::vector<uint8_t> preparedValid;
    std::vector<size_t> prepOrderScratch;
    std::vector<TreeChannelBinding> treeBindings;

    void invalidatePrepCache() noexcept { ++clipDataVersion; }

    const PreparedChannel &getPreparedChannel(const TimelineAnimClip &clip, uint32_t clipIdx,
                                              size_t chVecIdx)
    {
        if (cachedClipIdx != clipIdx || cachedVersion != clipDataVersion ||
            cachedPrepared.size() != clip.channels.size()) {
            cachedClipIdx = clipIdx;
            cachedVersion = clipDataVersion;
            cachedPrepared.resize(clip.channels.size());
            preparedValid.assign(clip.channels.size(), 0u);
        }
        if (preparedValid[chVecIdx] == 0u) {
            prepareChannelInto(clip.channels[chVecIdx], cachedPrepared[chVecIdx], prepOrderScratch);
            preparedValid[chVecIdx] = 1u;
        }
        return cachedPrepared[chVecIdx];
    }

    [[nodiscard]] bool isSelected(int32_t boneIndex, uint32_t channelIndex, size_t keyIdx,
                                  double frame) const noexcept
    {
        for (const auto &ref : selectedKeys) {
            if (ref.matches(boneIndex, channelIndex, keyIdx, frame)) {
                return true;
            }
        }
        return false;
    }

    void selectOnly(int32_t boneIndex, uint32_t channelIndex, size_t keyIdx, double frame)
    {
        selectedKeys.clear();
        selectedKeys.push_back({boneIndex, channelIndex, keyIdx, frame});
    }

    void addSelectionUnique(int32_t boneIndex, uint32_t channelIndex, size_t keyIdx, double frame)
    {
        if (!isSelected(boneIndex, channelIndex, keyIdx, frame)) {
            selectedKeys.push_back({boneIndex, channelIndex, keyIdx, frame});
        }
    }

    void toggleSelection(int32_t boneIndex, uint32_t channelIndex, size_t keyIdx, double frame)
    {
        for (auto it = selectedKeys.begin(); it != selectedKeys.end(); ++it) {
            if (it->matches(boneIndex, channelIndex, keyIdx, frame)) {
                selectedKeys.erase(it);
                return;
            }
        }
        selectedKeys.push_back({boneIndex, channelIndex, keyIdx, frame});
    }
};

namespace {

[[nodiscard]] bool channelSetMatchesTree(const TimelineAnimClip &clip,
                                         const std::vector<TreeChannelBinding> &bindings)
{
    if (bindings.size() != clip.channels.size()) {
        return false;
    }
    for (size_t i = 0u; i < bindings.size(); ++i) {
        if (bindings[i].channelVecIdx != i || !bindings[i].item) {
            return false;
        }
        const int32_t b = bindings[i].item->data(0, Qt::UserRole).toInt();
        const uint32_t c = bindings[i].item->data(0, Qt::UserRole + 1).toUInt();
        if (b != clip.channels[i].boneIndex || c != clip.channels[i].channelIndex) {
            return false;
        }
    }
    return true;
}

void buildClipIpcData(const DopeSheetTimelineWidget &owner, uint32_t clipIdx, uint32_t boneCount,
                      int32_t maxFramesCap, IpcClient::PuppetAnimClipData &outClipData,
                      size_t &outTotalKeyframes)
{
    const auto &clips = owner.clips();
    const TimelineAnimClip &clip = clips[clipIdx];
    outClipData = IpcClient::PuppetAnimClipData{};

    const QByteArray nameUtf8 = clip.name.toUtf8();
    std::memcpy(
        outClipData.desc.name, nameUtf8.constData(),
        std::min<size_t>(static_cast<size_t>(nameUtf8.size()), sizeof(outClipData.desc.name) - 1));
    const QByteArray modeUtf8 = animModeToString(clip.mode).toLatin1();
    std::memcpy(outClipData.desc.mode_str, modeUtf8.constData(),
                std::min<size_t>(static_cast<size_t>(modeUtf8.size()),
                                 sizeof(outClipData.desc.mode_str) - 1));

    outClipData.desc.id = clip.id;
    outClipData.desc.fps = clip.fps;
    outClipData.desc.length = clip.lengthFrames;
    outClipData.desc.layer_blend = clip.layerBlend;
    outClipData.desc.layer_rate = clip.layerRate;
    outClipData.desc.layer_visible = clip.layerVisible ? 1u : 0u;
    outClipData.desc.layer_blend_mode = static_cast<uint32_t>(clip.blendMode);

    std::vector<we_mdl_bone_track_t> tracks;
    std::vector<we_mdl_keyframe_t> storage;
    if (maxFramesCap <= 0) {
        owner.bakeClipToTracks(clipIdx, boneCount, tracks, storage);
    } else {
        // Proportionally decimated bake when sharing payload across large multi-clip banks.
        const uint32_t cappedBones = std::min<uint32_t>(boneCount, WE_MDL_MAX_BONES);
        std::map<int32_t, std::vector<const TimelineFCurveChannel *>> boneChannels;
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex >= 0 && ch.boneIndex < static_cast<int32_t>(cappedBones) &&
                ch.channelIndex < 9u && !ch.keys.empty()) {
                boneChannels[ch.boneIndex].push_back(&ch);
            }
        }
        if (!boneChannels.empty()) {
            const uint32_t kfCount =
                static_cast<uint32_t>(std::clamp(clip.lengthFrames, 1, maxFramesCap));
            storage.resize(boneChannels.size() * static_cast<size_t>(kfCount));
            tracks.reserve(boneChannels.size());
            size_t offset = 0u;
            for (const auto &[boneIdx, channels] : boneChannels) {
                we_mdl_keyframe_t *dst = storage.data() + offset;
                for (uint32_t f = 0; f < kfCount; ++f) {
                    dst[f] = we_mdl_keyframe_t{
                        .pos = {0.0f, 0.0f, 0.0f},
                        .angle = {0.0f, 0.0f, 0.0f},
                        .scale = {1.0f, 1.0f, 1.0f},
                    };
                }
                for (const TimelineFCurveChannel *ch : channels) {
                    const PreparedChannel prep = prepareChannel(*ch);
                    for (uint32_t f = 0; f < kfCount; ++f) {
                        const double sampleFrame =
                            (kfCount < static_cast<uint32_t>(std::max(1, clip.lengthFrames)) &&
                             kfCount > 1u && clip.lengthFrames > 1)
                                ? (static_cast<double>(f) *
                                   static_cast<double>(clip.lengthFrames - 1) /
                                   static_cast<double>(kfCount - 1u))
                                : static_cast<double>(f);
                        const double val = evaluatePreparedChannel(prep, sampleFrame);
                        const float fv =
                            std::isfinite(val)
                                ? static_cast<float>(val)
                                : static_cast<float>(channelDefaultValue(ch->channelIndex));
                        if (ch->channelIndex < 3u) {
                            dst[f].pos[ch->channelIndex] = fv;
                        } else if (ch->channelIndex < 6u) {
                            dst[f].angle[ch->channelIndex - 3u] = fv;
                        } else {
                            dst[f].scale[ch->channelIndex - 6u] = fv;
                        }
                    }
                }
                we_mdl_bone_track_t tr{};
                tr.bone_index = boneIdx;
                tr.kf_count = kfCount;
                tr.keyframes = dst;
                tracks.push_back(tr);
                offset += kfCount;
            }
        }
    }

    outClipData.desc.track_count = static_cast<uint32_t>(tracks.size());
    outClipData.tracks.reserve(tracks.size());
    outTotalKeyframes = 0u;
    for (const auto &tr : tracks) {
        IpcClient::PuppetAnimTrackData tdata{};
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
        outTotalKeyframes += tr.kf_count;
        outClipData.tracks.push_back(std::move(tdata));
    }
}

void execTimelineContextMenu(DopeSheetTimelineWidget *owner, const QPoint &globalPos)
{
    if (!owner) {
        return;
    }
    QMenu menu(owner);
    QAction *copyAct = menu.addAction(QObject::tr("Copy Keys (Ctrl+C)"));
    QAction *pasteAct = menu.addAction(QObject::tr("Paste Keys at Playhead (Ctrl+V)"));
    QAction *dupAct = menu.addAction(QObject::tr("Duplicate Keys (Ctrl+D)"));
    menu.addSeparator();
    QAction *autoAct = menu.addAction(QObject::tr("Tangent: Auto (1)"));
    QAction *alignedAct = menu.addAction(QObject::tr("Tangent: Aligned (2)"));
    QAction *vecAct = menu.addAction(QObject::tr("Tangent: Vector (3)"));
    QAction *stepAct = menu.addAction(QObject::tr("Tangent: Stepped (4)"));

    QAction *chosen = menu.exec(globalPos);
    if (chosen == copyAct) {
        owner->copySelectedKeyframes();
    } else if (chosen == pasteAct) {
        owner->pasteKeyframesAtPlayhead();
    } else if (chosen == dupAct) {
        owner->duplicateSelectedKeyframes();
    } else if (chosen == autoAct) {
        owner->onSetSelectedTangentMode(KeyTangentMode::Auto);
    } else if (chosen == alignedAct) {
        owner->onSetSelectedTangentMode(KeyTangentMode::Aligned);
    } else if (chosen == vecAct) {
        owner->onSetSelectedTangentMode(KeyTangentMode::Vector);
    } else if (chosen == stepAct) {
        owner->onSetSelectedTangentMode(KeyTangentMode::Stepped);
    }
}

} // namespace

class DopeSheetCanvas final : public QWidget {
  public:
    explicit DopeSheetCanvas(DopeSheetTimelineWidget *owner, std::function<void(bool)> onScrub,
                             std::function<void()> onSyncView, std::function<double()> getZoom,
                             std::function<void(double)> setZoom, QWidget *parent = nullptr)
        : QWidget(parent), m_owner(owner), m_onScrubbingChanged(std::move(onScrub)),
          m_onSyncView(std::move(onSyncView)), m_getZoom(std::move(getZoom)),
          m_setZoom(std::move(setZoom))
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumHeight(96);
        setContextMenuPolicy(Qt::DefaultContextMenu);
        setAccessibleName(tr("Multi-Track Keyframe Dope-Sheet"));
        setToolTip(tr("Drag ruler to scrub, drag keyframes or marquee, Ctrl+Wheel to zoom"));
    }

    void setTrackTree(QTreeWidget *tree) noexcept { m_trackTree = tree; }
    void clearOwner() noexcept { m_owner = nullptr; }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_owner) {
            execTimelineContextMenu(m_owner, event->globalPos());
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), kSurface1);

        if (!m_owner || !m_owner->m_impl) {
            return;
        }
        const auto *impl = m_owner->m_impl.get();
        const double dpr = devicePixelRatioF();
        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;

        auto frameToX = [scrollF, zoom, gutter](double frame) {
            return (frame - scrollF) * zoom + gutter;
        };

        const auto &clips = m_owner->clips();
        int32_t lengthFrames = 120;
        const TimelineAnimClip *activeClip = nullptr;
        if (m_owner->m_activeClipIndex < clips.size()) {
            activeClip = &clips[m_owner->m_activeClipIndex];
            lengthFrames = std::max(1, activeClip->lengthFrames);
        }

        const double rangeLeft = frameToX(0.0);
        const double rangeRight = frameToX(static_cast<double>(lengthFrames - 1));
        if (rangeRight > gutter && rangeLeft < width()) {
            const double rl = std::max(gutter, rangeLeft);
            const double rr = std::min<double>(width(), rangeRight);
            if (rr > rl) {
                painter.fillRect(QRectF(rl, kRulerHeightPx, rr - rl, height() - kRulerHeightPx),
                                 QColor(0x20, 0x24, 0x34, 90));
            }
        }

        const int step = niceFrameStep(zoom);
        const int firstTick = static_cast<int>(std::floor((scrollF - gutter / zoom) / step)) * step;
        const int lastTick = static_cast<int>(std::ceil((scrollF + width() / zoom) / step)) * step;

        painter.setFont(timelineUiMonoFont());

        for (int f = firstTick; f <= lastTick; f += step) {
            const double x = frameToX(static_cast<double>(f));
            if (x < gutter - 1.0 || x > width() + 10.0) {
                continue;
            }
            const double sx = snapHalfPx(x, dpr);
            painter.setPen(QPen(f == 0 ? kMajorGrid : kBorder, 1.0));
            painter.drawLine(QPointF(sx, kRulerHeightPx), QPointF(sx, height()));
        }

        if (m_trackTree && activeClip) {
            const QPoint treeOrigin = m_trackTree->viewport()->mapTo(m_owner, QPoint(0, 0));
            const QPoint canvasOrigin = mapTo(m_owner, QPoint(0, 0));
            const int yOffset = treeOrigin.y() - canvasOrigin.y();

            int rowIdx = 0;
            for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
                const QRect bRect = m_trackTree->visualItemRect(boneItem);
                if (bRect.isValid() && !bRect.isEmpty()) {
                    const int yTop = bRect.top() + yOffset;
                    const int rowH = bRect.height();
                    if (yTop + rowH >= kRulerHeightPx && yTop <= height()) {
                        const QRectF rowRect(gutter, yTop, std::max(0.0, width() - gutter), rowH);
                        painter.fillRect(rowRect, (rowIdx % 2 == 0) ? kSurface1 : kSurface2);
                        const double sy = snapHalfPx(yTop + rowH, dpr);
                        painter.setPen(QPen(kBorder, 1.0));
                        painter.drawLine(QPointF(gutter, sy), QPointF(width(), sy));

                        const int32_t boneIdx = boneItem->data(0, Qt::UserRole).toInt();
                        const double cy = yTop + rowH * 0.5;
                        for (const auto &ch : activeClip->channels) {
                            if (ch.boneIndex != boneIdx || !ch.visible) {
                                continue;
                            }
                            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                                const auto &k = ch.keys[ki];
                                const double kx = frameToX(k.frame);
                                if (kx < gutter - 8.0 || kx > width() + 12.0) {
                                    continue;
                                }
                                const bool sel =
                                    impl->isSelected(boneIdx, ch.channelIndex, ki, k.frame);
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
                        const QRectF rowRect(gutter, yTop, std::max(0.0, width() - gutter), rowH);
                        painter.fillRect(rowRect, (rowIdx % 2 == 0) ? kSurface1 : kSurface2);
                        const double sy = snapHalfPx(yTop + rowH, dpr);
                        painter.setPen(QPen(kBorder, 1.0));
                        painter.drawLine(QPointF(gutter, sy), QPointF(width(), sy));

                        const uint32_t chVecIdx = chItem->data(0, Qt::UserRole + 2).toUInt();
                        if (chVecIdx < activeClip->channels.size()) {
                            const auto &ch = activeClip->channels[chVecIdx];
                            const double cy = yTop + rowH * 0.5;
                            const QColor col = channelColor(ch.channelIndex);
                            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                                const auto &k = ch.keys[ki];
                                const double kx = frameToX(k.frame);
                                if (kx < gutter - 8.0 || kx > width() + 12.0) {
                                    continue;
                                }
                                const bool sel =
                                    impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame);
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

        // Left alignment gutter matching F-curve Y-axis gutter width for Split view parity.
        painter.fillRect(QRectF(0.0, 0.0, gutter, height()), kSurface0);
        const double gx = snapHalfPx(gutter, dpr);
        painter.setPen(QPen(kBorder, 1.0));
        painter.drawLine(QPointF(gx, 0.0), QPointF(gx, height()));

        // Ruler drawn over scrolled rows.
        painter.fillRect(QRectF(0.0, 0.0, width(), kRulerHeightPx), kSurface0);
        const double ry = snapHalfPx(kRulerHeightPx, dpr);
        painter.setPen(QPen(kBorder, 1.0));
        painter.drawLine(QPointF(0.0, ry), QPointF(width(), ry));

        for (int f = firstTick; f <= lastTick; f += step) {
            const double x = frameToX(static_cast<double>(f));
            if (x < gutter - 1.0 || x > width() + 10.0) {
                continue;
            }
            const double sx = snapHalfPx(x, dpr);
            painter.setPen(QPen(kMajorGrid, 1.0));
            painter.drawLine(QPointF(sx, kRulerHeightPx - 8), QPointF(sx, kRulerHeightPx));
            painter.setPen(kTextMuted);
            painter.drawText(QRectF(x + 3.0, 2.0, 52.0, 18.0), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(f));
        }

        const double phX = frameToX(m_owner->currentFrame());
        if (phX >= gutter - 8.0 && phX <= width() + 10.0) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.drawLine(QPointF(phX, kRulerHeightPx), QPointF(phX, height()));

            QPolygonF cap;
            cap << QPointF(phX - 6.0, 3.0) << QPointF(phX + 6.0, 3.0) << QPointF(phX + 6.0, 14.0)
                << QPointF(phX, 22.0) << QPointF(phX - 6.0, 14.0);
            painter.setPen(QPen(Qt::white, 1.0));
            painter.setBrush(kAccentSky);
            painter.drawPolygon(cap);
        }

        if (m_mode == DragMode::Marquee) {
            const QRectF marqueeRect = QRectF(m_dragStartPos, m_lastMouse).normalized();
            painter.setPen(QPen(kAccentSky, 1.0, Qt::DashLine));
            painter.setBrush(QColor(0x38, 0xbd, 0xf8, 40));
            painter.drawRect(marqueeRect);
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
        if (!m_owner || !m_owner->m_impl) {
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;

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
            const double f = scrollF + (event->position().x() - gutter) / zoom;
            m_owner->setCurrentFrame(std::round(f));
            return;
        }

        if (hitTestKeyframe(event->position(), event->modifiers().testFlag(Qt::ShiftModifier))) {
            if (!impl->selectedKeys.empty()) {
                m_mode = DragMode::MoveKeys;
                m_dragStartFrame = scrollF + (event->position().x() - gutter) / zoom;
                m_dragLastDelta = 0.0;
                captureDragPreState();
            }
            m_owner->syncClipAndSelectionControls();
            if (m_onSyncView) {
                m_onSyncView();
            }
            return;
        }

        m_mode = DragMode::PendingBodyClick;
        m_dragStartPos = event->position();
        m_lastMouse = event->position();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_owner || !m_owner->m_impl) {
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;

        if (m_mode == DragMode::Pan) {
            const double dx = event->position().x() - m_lastMouse.x();
            impl->scrollFrameOffset = std::max(-10.0, impl->scrollFrameOffset - dx / zoom);
            m_lastMouse = event->position();
            if (m_onSyncView) {
                m_onSyncView();
            }
            return;
        }

        if (m_mode == DragMode::Scrub) {
            const double f = scrollF + (event->position().x() - gutter) / zoom;
            m_owner->setCurrentFrame(std::round(f));
            return;
        }

        if (m_mode == DragMode::PendingBodyClick) {
            m_lastMouse = event->position();
            if ((event->position() - m_dragStartPos).manhattanLength() > 4.0) {
                m_mode = DragMode::Marquee;
                update();
            }
            return;
        }

        if (m_mode == DragMode::Marquee) {
            m_lastMouse = event->position();
            update();
            return;
        }

        if (m_mode == DragMode::MoveKeys) {
            const double curF = scrollF + (event->position().x() - gutter) / zoom;
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
        if (m_mode == DragMode::Pan && event->button() != Qt::MiddleButton) {
            return;
        }
        if ((m_mode == DragMode::Scrub || m_mode == DragMode::MoveKeys ||
             m_mode == DragMode::PendingBodyClick || m_mode == DragMode::Marquee) &&
            event->button() != Qt::LeftButton) {
            return;
        }

        if (!m_owner || !m_owner->m_impl) {
            m_mode = DragMode::None;
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;

        if (m_mode == DragMode::Scrub) {
            if (m_onScrubbingChanged) {
                m_onScrubbingChanged(false);
            }
        } else if (m_mode == DragMode::PendingBodyClick) {
            if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
                impl->selectedKeys.clear();
                m_owner->syncClipAndSelectionControls();
            }
            const double f = scrollF + (event->position().x() - gutter) / zoom;
            m_owner->setCurrentFrame(std::round(f));
        } else if (m_mode == DragMode::Marquee) {
            const QRectF box = QRectF(m_dragStartPos, event->position()).normalized();
            selectMarquee(box, event->modifiers().testFlag(Qt::ShiftModifier));
            m_owner->syncClipAndSelectionControls();
            if (m_onSyncView) {
                m_onSyncView();
            }
        } else if (m_mode == DragMode::MoveKeys) {
            commitDragCommand();
            m_owner->syncClipAndSelectionControls();
        }
        m_mode = DragMode::None;
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (!m_owner || !m_owner->m_impl) {
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const double gutter = impl->leftGutterPx;
        const double deltaSteps = static_cast<double>(event->angleDelta().y()) / 120.0;
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const double oldZoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
            const double mouseF =
                impl->scrollFrameOffset + (event->position().x() - gutter) / oldZoom;
            const double newZoom = std::clamp(oldZoom * std::pow(1.18, deltaSteps),
                                              kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
            if (m_setZoom) {
                m_setZoom(newZoom);
            }
            impl->scrollFrameOffset =
                std::max(-10.0, mouseF - (event->position().x() - gutter) / newZoom);
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
            impl->scrollFrameOffset =
                std::max(-10.0, impl->scrollFrameOffset - (panPx * 0.25) / zoom);
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
    enum class DragMode { None, Scrub, PendingBodyClick, MoveKeys, Marquee, Pan };

    struct SelectedSlotOrigin {
        int32_t boneIndex{-1};
        uint32_t channelIndex{0u};
        size_t preChKeyIndex{kInvalidKeyIndex};
        FCurveKeySnapshot snapshot;
    };

    bool hitTestKeyframe(const QPointF &pos, bool additiveToggle)
    {
        if (!m_owner || !m_owner->m_impl || !m_trackTree) {
            return false;
        }
        auto *impl = m_owner->m_impl.get();
        const uint32_t clipIdx = m_owner->m_activeClipIndex;
        const auto &clips = m_owner->clips();
        if (clipIdx >= clips.size()) {
            return false;
        }

        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;
        const QPoint treeOrigin = m_trackTree->viewport()->mapTo(m_owner, QPoint(0, 0));
        const QPoint canvasOrigin = mapTo(m_owner, QPoint(0, 0));
        const int yOffset = treeOrigin.y() - canvasOrigin.y();
        const TimelineAnimClip &clip = clips[clipIdx];

        for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
            const QRect bRect = m_trackTree->visualItemRect(boneItem);
            if (bRect.isValid()) {
                const double cy = bRect.top() + yOffset + bRect.height() * 0.5;
                if (std::abs(pos.y() - cy) <= bRect.height() * 0.55) {
                    const int32_t boneIdx = boneItem->data(0, Qt::UserRole).toInt();
                    double bestDistPx = 8.1;
                    double targetFrame = 0.0;
                    bool foundHit = false;
                    for (const auto &ch : clip.channels) {
                        if (ch.boneIndex != boneIdx || !ch.visible) {
                            continue;
                        }
                        for (const auto &k : ch.keys) {
                            const double kx = (k.frame - scrollF) * zoom + gutter;
                            const double distPx = std::abs(pos.x() - kx);
                            if (distPx <= bestDistPx) {
                                bestDistPx = distPx;
                                targetFrame = k.frame;
                                foundHit = true;
                            }
                        }
                    }
                    if (foundHit) {
                        std::vector<SelectedKeyRef> clickedGroup;
                        bool allAlreadySelected = true;
                        for (const auto &ch : clip.channels) {
                            if (ch.boneIndex != boneIdx || !ch.visible) {
                                continue;
                            }
                            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                                if (std::abs(ch.keys[ki].frame - targetFrame) < 1e-2) {
                                    clickedGroup.push_back(
                                        {boneIdx, ch.channelIndex, ki, ch.keys[ki].frame});
                                    if (!impl->isSelected(boneIdx, ch.channelIndex, ki,
                                                          ch.keys[ki].frame)) {
                                        allAlreadySelected = false;
                                    }
                                }
                            }
                        }
                        if (additiveToggle) {
                            if (allAlreadySelected) {
                                for (const auto &g : clickedGroup) {
                                    if (impl->isSelected(g.boneIndex, g.channelIndex, g.keyIndex,
                                                         g.frame)) {
                                        impl->toggleSelection(g.boneIndex, g.channelIndex,
                                                              g.keyIndex, g.frame);
                                    }
                                }
                            } else {
                                for (const auto &g : clickedGroup) {
                                    impl->addSelectionUnique(g.boneIndex, g.channelIndex,
                                                             g.keyIndex, g.frame);
                                }
                            }
                        } else if (!allAlreadySelected) {
                            impl->selectedKeys = std::move(clickedGroup);
                        }
                        return true;
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
                        const uint32_t chVecIdx = chItem->data(0, Qt::UserRole + 2).toUInt();
                        if (chVecIdx >= clip.channels.size()) {
                            continue;
                        }
                        const auto &ch = clip.channels[chVecIdx];
                        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                            const auto &k = ch.keys[ki];
                            const double kx = (k.frame - scrollF) * zoom + gutter;
                            if (std::abs(pos.x() - kx) <= 8.0) {
                                if (additiveToggle) {
                                    impl->toggleSelection(ch.boneIndex, ch.channelIndex, ki,
                                                          k.frame);
                                } else if (!impl->isSelected(ch.boneIndex, ch.channelIndex, ki,
                                                             k.frame)) {
                                    impl->selectOnly(ch.boneIndex, ch.channelIndex, ki, k.frame);
                                }
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    void selectMarquee(const QRectF &box, bool additive)
    {
        if (!m_owner || !m_owner->m_impl || !m_trackTree) {
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const uint32_t clipIdx = m_owner->m_activeClipIndex;
        const auto &clips = m_owner->clips();
        if (clipIdx >= clips.size()) {
            return;
        }
        const TimelineAnimClip &clip = clips[clipIdx];
        if (!additive) {
            impl->selectedKeys.clear();
        }

        const double gutter = impl->leftGutterPx;
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;
        const QPoint treeOrigin = m_trackTree->viewport()->mapTo(m_owner, QPoint(0, 0));
        const QPoint canvasOrigin = mapTo(m_owner, QPoint(0, 0));
        const int yOffset = treeOrigin.y() - canvasOrigin.y();

        for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *boneItem = m_trackTree->topLevelItem(i);
            const QRect bRect = m_trackTree->visualItemRect(boneItem);
            if (bRect.isValid()) {
                const double cy = bRect.top() + yOffset + bRect.height() * 0.5;
                if (cy >= box.top() && cy <= box.bottom()) {
                    const int32_t boneIdx = boneItem->data(0, Qt::UserRole).toInt();
                    for (const auto &ch : clip.channels) {
                        if (ch.boneIndex != boneIdx || !ch.visible) {
                            continue;
                        }
                        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                            const double kx = (ch.keys[ki].frame - scrollF) * zoom + gutter;
                            if (kx >= box.left() && kx <= box.right()) {
                                impl->addSelectionUnique(boneIdx, ch.channelIndex, ki,
                                                         ch.keys[ki].frame);
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
                    if (cy >= box.top() && cy <= box.bottom()) {
                        const uint32_t chVecIdx = chItem->data(0, Qt::UserRole + 2).toUInt();
                        if (chVecIdx < clip.channels.size()) {
                            const auto &ch = clip.channels[chVecIdx];
                            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                                const double kx = (ch.keys[ki].frame - scrollF) * zoom + gutter;
                                if (kx >= box.left() && kx <= box.right()) {
                                    impl->addSelectionUnique(ch.boneIndex, ch.channelIndex, ki,
                                                             ch.keys[ki].frame);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void captureDragPreState();
    void applyDragDelta(double deltaFrames);
    void commitDragCommand();

    DopeSheetTimelineWidget *m_owner{nullptr};
    QTreeWidget *m_trackTree{nullptr};
    std::function<void(bool)> m_onScrubbingChanged;
    std::function<void()> m_onSyncView;
    std::function<double()> m_getZoom;
    std::function<void(double)> m_setZoom;

    DragMode m_mode{DragMode::None};
    QPointF m_dragStartPos;
    QPointF m_lastMouse;
    double m_dragStartFrame{0.0};
    double m_dragLastDelta{0.0};
    std::vector<SelectedSlotOrigin> m_preDragSlots;
    std::vector<SelectedSlotOrigin> m_curDragSlots;
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
        setContextMenuPolicy(Qt::DefaultContextMenu);
        setAccessibleName(tr("Cubic-Bezier F-Curve Editor"));
        setToolTip(tr(
            "Drag keys, marquee, or handles; double-click curve to insert; Ctrl/Alt+Wheel zoom"));
    }

    void setTrackTree(QTreeWidget *tree) noexcept { m_trackTree = tree; }
    void clearOwner() noexcept { m_owner = nullptr; }
    void frameFitVisibleChannels();

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (m_owner) {
            execTimelineContextMenu(m_owner, event->globalPos());
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), kSurface1);

        if (!m_owner || !m_owner->m_impl) {
            return;
        }
        auto *impl = m_owner->m_impl.get();
        const double dpr = devicePixelRatioF();
        const double gutter = impl->leftGutterPx;
        const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;
        const double minY = impl->fcurveMinY;
        const double maxY = std::max(minY + 0.05, impl->fcurveMaxY);

        auto frameToX = [scrollF, zoomX, gutter](double frame) {
            return (frame - scrollF) * zoomX + gutter;
        };
        auto valToY = [this, minY, maxY](double val) {
            const double pad = 12.0;
            const double avail = std::max(16.0, static_cast<double>(height()) - 2.0 * pad);
            return pad + (maxY - val) / (maxY - minY) * avail;
        };

        const double yStep = niceGridStep(maxY - minY, 6);
        const double firstY = std::floor(minY / yStep) * yStep;
        painter.setFont(timelineUiMonoFont());

        for (double v = firstY; v <= maxY + yStep * 0.5; v += yStep) {
            const double py = valToY(v);
            if (py < 4.0 || py > height() - 4.0) {
                continue;
            }
            const double spy = snapHalfPx(py, dpr);
            const bool isZero = std::abs(v) < 1e-6;
            painter.setPen(QPen(isZero ? kMajorGrid : kBorder, 1.0));
            painter.drawLine(QPointF(gutter, spy), QPointF(width(), spy));
            painter.setPen(kTextMuted);
            painter.drawText(QRectF(2.0, py - 9.0, gutter - 6.0, 18.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(v, 'f', yStep < 0.5 ? 2 : 1));
        }

        const int fStep = niceFrameStep(zoomX);
        const int firstTick =
            static_cast<int>(std::floor((scrollF - gutter / zoomX) / fStep)) * fStep;
        const int lastTick =
            static_cast<int>(std::ceil((scrollF + width() / zoomX) / fStep)) * fStep;
        for (int f = firstTick; f <= lastTick; f += fStep) {
            const double x = frameToX(static_cast<double>(f));
            if (x < gutter || x > width()) {
                continue;
            }
            const double sx = snapHalfPx(x, dpr);
            painter.setPen(QPen(kBorder, 1.0));
            painter.drawLine(QPointF(sx, 0.0), QPointF(sx, height()));
        }

        const uint32_t clipIdx = m_owner->m_activeClipIndex;
        const auto channels = collectVisibleChannelIndices();
        const int sampleStepPx = 2;

        if (clipIdx < m_owner->clips().size()) {
            const TimelineAnimClip &clip = m_owner->clips()[clipIdx];
            for (size_t chVecIdx : channels) {
                const auto &ch = clip.channels[chVecIdx];
                if (!ch.visible || ch.keys.empty()) {
                    continue;
                }
                const QColor col = channelColor(ch.channelIndex);
                const PreparedChannel &prep = impl->getPreparedChannel(clip, clipIdx, chVecIdx);

                QPainterPath path;
                bool firstPt = true;
                for (int px = static_cast<int>(std::ceil(gutter)); px <= width();
                     px += sampleStepPx) {
                    const double f = scrollF + (static_cast<double>(px) - gutter) / zoomX;
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

                for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                    const FCurveKeySnapshot &k = ch.keys[ki];
                    const double val =
                        (ki < prep.keys.size()) ? prep.keys[ki].unwrappedValue : k.value;
                    const QPointF pt(frameToX(k.frame), valToY(val));
                    if (pt.x() < gutter - 12.0 || pt.x() > width() + 12.0) {
                        continue;
                    }

                    const bool sel = impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame);
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
        }

        const double phX = frameToX(m_owner->currentFrame());
        if (phX >= gutter && phX <= width()) {
            painter.setPen(QPen(kAccentSky, 2.0));
            painter.drawLine(QPointF(phX, 0.0), QPointF(phX, height()));
        }

        const double gx = snapHalfPx(gutter, dpr);
        painter.setPen(QPen(kBorder, 1.0));
        painter.drawLine(QPointF(gx, 0.0), QPointF(gx, height()));

        if (m_dragTarget == DragTarget::Marquee) {
            const QRectF marqueeRect = QRectF(m_dragStartMouse, m_lastMouse).normalized();
            painter.setPen(QPen(kAccentSky, 1.0, Qt::DashLine));
            painter.setBrush(QColor(0x38, 0xbd, 0xf8, 40));
            painter.drawRect(marqueeRect);
        }

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
    enum class DragTarget { None, KeyNode, HandleIn, HandleOut, Marquee, Pan };

    struct SelectedFCurveSlot {
        int32_t boneIndex{-1};
        uint32_t channelIndex{0u};
        size_t keyIndex{kInvalidKeyIndex};
        FCurveKeySnapshot origKey;
        FCurveKeySnapshot curKey;
        double unwrapOffset{0.0};
    };

    [[nodiscard]] std::vector<size_t> collectVisibleChannelIndices() const
    {
        std::vector<size_t> out;
        if (!m_owner) {
            return out;
        }
        const auto &clips = m_owner->clips();
        const uint32_t idx = m_owner->m_activeClipIndex;
        if (idx >= clips.size()) {
            return out;
        }
        const TimelineAnimClip &clip = clips[idx];

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

        for (size_t i = 0; i < clip.channels.size(); ++i) {
            const auto &ch = clip.channels[i];
            if (!ch.visible) {
                continue;
            }
            if (filterBone >= 0 && ch.boneIndex != filterBone) {
                continue;
            }
            if (filterChannel >= 0 && static_cast<int>(ch.channelIndex) != filterChannel) {
                continue;
            }
            out.push_back(i);
        }

        if (out.empty()) {
            for (size_t i = 0; i < clip.channels.size(); ++i) {
                if (clip.channels[i].visible) {
                    out.push_back(i);
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
    size_t m_dragKeyIndex{kInvalidKeyIndex};
    double m_dragOrigFrame{0.0};
    double m_dragOrigValue{0.0};
    double m_dragUnwrapOffset{0.0};
    QPointF m_dragStartMouse;
    QPointF m_lastMouse;
    std::vector<SelectedFCurveSlot> m_preDragSlots;
    std::vector<SelectedFCurveSlot> m_curDragSlots;
    std::vector<SelectedKeyRef> m_preDragSel;
    double m_preDragPrevFrame{0.0};
    double m_preDragNextFrame{0.0};
    bool m_hasPrevFrame{false};
    bool m_hasNextFrame{false};
    bool m_dragMutated{false};
};

class TimelineEventFilter final : public QObject {
  public:
    TimelineEventFilter(DopeSheetTimelineWidget *owner, QTreeWidget *trackTree,
                        std::function<void()> onVisibilityUpdate, std::function<void()> onFrameFit,
                        std::function<void()> onDeleteSelected,
                        std::function<void(int)> onStepKeyframe, QObject *parent)
        : QObject(parent), m_owner(owner), m_trackTree(trackTree),
          m_onVisibilityUpdate(std::move(onVisibilityUpdate)), m_onFrameFit(std::move(onFrameFit)),
          m_onDeleteSelected(std::move(onDeleteSelected)),
          m_onStepKeyframe(std::move(onStepKeyframe))
    {
    }

    void disarm() noexcept
    {
        m_disarmed = true;
        m_owner = nullptr;
        m_trackTree = nullptr;
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

        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) {
            return false;
        }

        // Only handle shortcuts when timeline itself is visible and holds keyboard focus.
        if (!m_owner->isVisible()) {
            return false;
        }
        QWidget *focusW = QApplication::focusWidget();
        if (!focusW || (focusW != m_owner && !m_owner->isAncestorOf(focusW))) {
            return false;
        }
        if (isEditableFocusWidget(focusW, m_trackTree)) {
            return false;
        }

        auto *ke = static_cast<QKeyEvent *>(event);
        const bool ctrl = ke->modifiers().testFlag(Qt::ControlModifier);

        if (event->type() == QEvent::ShortcutOverride) {
            if (ctrl &&
                (ke->key() == Qt::Key_C || ke->key() == Qt::Key_V || ke->key() == Qt::Key_D)) {
                event->accept();
                return true;
            }
            if (!ctrl && (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Space ||
                          ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right ||
                          ke->key() == Qt::Key_K || ke->key() == Qt::Key_I)) {
                event->accept();
                return true;
            }
            if (ctrl && ke->key() == Qt::Key_Tab) {
                event->accept();
                return true;
            }
            return false;
        }

        if (ctrl) {
            switch (ke->key()) {
            case Qt::Key_C:
                m_owner->copySelectedKeyframes();
                return true;
            case Qt::Key_V:
                m_owner->pasteKeyframesAtPlayhead();
                return true;
            case Qt::Key_D:
                m_owner->duplicateSelectedKeyframes();
                return true;
            case Qt::Key_Tab:
                m_owner->setViewMode(TimelineViewMode::Split);
                return true;
            default:
                return false;
            }
        }

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
            const uint32_t clipIdx = m_owner->m_activeClipIndex;
            const double endF =
                (clipIdx < clips.size())
                    ? static_cast<double>(std::max(1, clips[clipIdx].lengthFrames) - 1)
                    : 119.0;
            m_owner->setCurrentFrame(endF);
            return true;
        }
        case Qt::Key_K:
        case Qt::Key_I:
            m_owner->onInsertKeyframeAtPlayhead();
            return true;
        case Qt::Key_Tab: {
            const TimelineViewMode cur = m_owner->viewMode();
            m_owner->setViewMode(cur == TimelineViewMode::FCurve ? TimelineViewMode::DopeSheet
                                                                 : TimelineViewMode::FCurve);
            return true;
        }
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
            const bool checked = m_owner->m_onionCheck ? m_owner->m_onionCheck->isChecked() : false;
            m_owner->setOnionSkinEnabled(!checked);
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
        return false;
    }

  private:
    DopeSheetTimelineWidget *m_owner{nullptr};
    QTreeWidget *m_trackTree{nullptr};
    bool m_disarmed{false};
    std::function<void()> m_onVisibilityUpdate;
    std::function<void()> m_onFrameFit;
    std::function<void()> m_onDeleteSelected;
    std::function<void(int)> m_onStepKeyframe;
};

void DopeSheetCanvas::captureDragPreState()
{
    m_preDragSlots.clear();
    m_curDragSlots.clear();
    m_preDragSel.clear();
    if (!m_owner || !m_owner->m_impl || m_owner->m_activeClipIndex >= m_owner->m_clips.size()) {
        return;
    }
    const auto &clip = m_owner->m_clips[m_owner->m_activeClipIndex];
    auto *impl = m_owner->m_impl.get();
    m_preDragSel = impl->selectedKeys;

    for (const auto &ch : clip.channels) {
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            if (impl->isSelected(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame)) {
                SelectedSlotOrigin slot;
                slot.boneIndex = ch.boneIndex;
                slot.channelIndex = ch.channelIndex;
                slot.preChKeyIndex = ki;
                slot.snapshot = ch.keys[ki];
                m_preDragSlots.push_back(slot);
            }
        }
    }
    m_curDragSlots = m_preDragSlots;
}

void DopeSheetCanvas::applyDragDelta(double deltaFrames)
{
    if (!m_owner || !m_owner->m_impl || m_owner->m_activeClipIndex >= m_owner->m_clips.size() ||
        m_preDragSlots.empty()) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const auto &clip = m_owner->m_clips[m_owner->m_activeClipIndex];
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);

    std::vector<FCurveKeySnapshot> toRemove;
    std::vector<FCurveKeySnapshot> toInsert;
    toRemove.reserve(m_curDragSlots.size());
    toInsert.reserve(m_curDragSlots.size());

    std::vector<SelectedSlotOrigin> nextSlots = m_preDragSlots;
    impl->selectedKeys.clear();
    for (size_t i = 0; i < m_curDragSlots.size(); ++i) {
        toRemove.push_back(m_curDragSlots[i].snapshot);
        nextSlots[i].snapshot.frame =
            std::clamp(m_preDragSlots[i].snapshot.frame + deltaFrames, 0.0, maxF);
        toInsert.push_back(nextSlots[i].snapshot);
    }

    m_owner->applyClipDiffPatch(m_owner->m_objectId, clip.id, toRemove, toInsert, true, true);
    m_curDragSlots = std::move(nextSlots);

    // Track moved slots across stable_sort so coincident frames stay individually selectable.
    for (const auto &slot : m_curDragSlots) {
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex != slot.boneIndex || ch.channelIndex != slot.channelIndex) {
                continue;
            }
            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                if (keysMatchExactly(ch.keys[ki], slot.snapshot) &&
                    !impl->isSelected(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame)) {
                    impl->addSelectionUnique(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame);
                    break;
                }
            }
        }
    }

    if (m_onSyncView) {
        m_onSyncView();
    }
}

void DopeSheetCanvas::commitDragCommand()
{
    if (!m_owner || !m_owner->m_impl || m_owner->m_activeClipIndex >= m_owner->m_clips.size() ||
        m_preDragSlots.empty() || std::abs(m_dragLastDelta) < 1e-4) {
        m_preDragSlots.clear();
        m_curDragSlots.clear();
        return;
    }

    const int32_t clipId = m_owner->m_clips[m_owner->m_activeClipIndex].id;
    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> curRemove;
    std::vector<FCurveKeySnapshot> newDiff;
    oldDiff.reserve(m_preDragSlots.size());
    curRemove.reserve(m_curDragSlots.size());
    newDiff.reserve(m_curDragSlots.size());

    for (size_t i = 0; i < m_preDragSlots.size(); ++i) {
        oldDiff.push_back(m_preDragSlots[i].snapshot);
        curRemove.push_back(m_curDragSlots[i].snapshot);
        newDiff.push_back(m_curDragSlots[i].snapshot);
    }

    std::vector<SelectedKeyRef> postSel = m_owner->m_impl->selectedKeys;
    m_owner->applyClipDiffPatch(m_owner->m_objectId, clipId, curRemove, oldDiff, true, true);
    m_owner->m_impl->selectedKeys = m_preDragSel;
    m_owner->commitClipDiffEdit(clipId, tr("Move keyframes"), std::move(oldDiff),
                                std::move(newDiff), true);
    m_owner->m_impl->selectedKeys = std::move(postSel);
    m_preDragSlots.clear();
    m_curDragSlots.clear();
}

void FCurveCanvas::frameFitVisibleChannels()
{
    if (!m_owner || !m_owner->m_impl) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const double gutter = impl->leftGutterPx;
    const auto channels = collectVisibleChannelIndices();
    const uint32_t clipIdx = m_owner->m_activeClipIndex;
    if (clipIdx >= m_owner->clips().size()) {
        return;
    }
    const TimelineAnimClip &clip = m_owner->clips()[clipIdx];

    bool any = false;
    double minF = 0.0;
    double maxF = 120.0;
    double minV = std::numeric_limits<double>::infinity();
    double maxV = -std::numeric_limits<double>::infinity();

    for (size_t chVecIdx : channels) {
        const auto &ch = clip.channels[chVecIdx];
        if (ch.keys.empty()) {
            continue;
        }
        const PreparedChannel &prep = impl->getPreparedChannel(clip, clipIdx, chVecIdx);
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
        impl->fcurveMinY = -3.5;
        impl->fcurveMaxY = 3.5;
    } else {
        const double spanV = std::max(0.5, maxV - minV);
        impl->fcurveMinY = minV - spanV * 0.15;
        impl->fcurveMaxY = maxV + spanV * 0.15;
        const double spanF = std::max(10.0, maxF - minF);
        const double availW = std::max(40.0, static_cast<double>(width()) - gutter - 16.0);
        if (m_setZoom && width() > gutter + 24.0) {
            m_setZoom(std::clamp(availW / spanF, kMinZoomPxPerFrame, kMaxZoomPxPerFrame));
        }
        impl->scrollFrameOffset = std::max(0.0, minF - spanF * 0.05);
    }
    if (m_onSyncView) {
        m_onSyncView();
    }
}

void FCurveCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (!m_owner || !m_owner->m_impl) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const uint32_t clipIdx = m_owner->m_activeClipIndex;
    const auto &clips = m_owner->clips();
    if (clipIdx >= clips.size()) {
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

    const double gutter = impl->leftGutterPx;
    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double scrollF = impl->scrollFrameOffset;
    const double minY = impl->fcurveMinY;
    const double maxY = std::max(minY + 0.05, impl->fcurveMaxY);

    auto frameToX = [scrollF, zoomX, gutter](double frame) {
        return (frame - scrollF) * zoomX + gutter;
    };
    auto valToY = [this, minY, maxY](double val) {
        const double pad = 12.0;
        const double avail = std::max(16.0, static_cast<double>(height()) - 2.0 * pad);
        return pad + (maxY - val) / (maxY - minY) * avail;
    };

    const TimelineAnimClip &clip = clips[clipIdx];
    const auto channels = collectVisibleChannelIndices();

    auto captureChannelNeighborSpan = [&](const TimelineFCurveChannel &ch, size_t ki) {
        m_hasPrevFrame = (ki > 0);
        m_hasNextFrame = (ki + 1 < ch.keys.size());
        m_preDragPrevFrame = m_hasPrevFrame ? ch.keys[ki - 1].frame : 0.0;
        m_preDragNextFrame = m_hasNextFrame ? ch.keys[ki + 1].frame : 0.0;
    };

    for (size_t chVecIdx : channels) {
        const auto &ch = clip.channels[chVecIdx];
        if (!ch.visible) {
            continue;
        }
        const PreparedChannel &prep = impl->getPreparedChannel(clip, clipIdx, chVecIdx);
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            const auto &k = ch.keys[ki];
            if (!impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame) ||
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

            auto beginHandleDrag = [&](DragTarget target) {
                m_dragTarget = target;
                m_dragBone = ch.boneIndex;
                m_dragChannel = ch.channelIndex;
                m_dragKeyIndex = ki;
                m_dragOrigFrame = k.frame;
                m_dragOrigValue = val;
                m_dragUnwrapOffset = val - k.value;
                m_dragStartMouse = event->position();
                m_dragMutated = false;
                m_preDragSel = impl->selectedKeys;
                m_preDragSlots = {
                    SelectedFCurveSlot{ch.boneIndex, ch.channelIndex, ki, k, k, val - k.value}};
                m_curDragSlots = m_preDragSlots;
                captureChannelNeighborSpan(ch, ki);
            };

            if (QLineF(event->position(), pIn).length() <= 10.0) {
                beginHandleDrag(DragTarget::HandleIn);
                return;
            }
            if (QLineF(event->position(), pOut).length() <= 10.0) {
                beginHandleDrag(DragTarget::HandleOut);
                return;
            }
        }
    }

    for (size_t chVecIdx : channels) {
        const auto &ch = clip.channels[chVecIdx];
        if (!ch.visible) {
            continue;
        }
        const PreparedChannel &prep = impl->getPreparedChannel(clip, clipIdx, chVecIdx);
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            const auto &k = ch.keys[ki];
            const double val = (ki < prep.keys.size()) ? prep.keys[ki].unwrappedValue : k.value;
            const QPointF pt(frameToX(k.frame), valToY(val));
            if (QLineF(event->position(), pt).length() <= 10.0) {
                if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    impl->toggleSelection(ch.boneIndex, ch.channelIndex, ki, k.frame);
                } else if (!impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame)) {
                    impl->selectOnly(ch.boneIndex, ch.channelIndex, ki, k.frame);
                }

                if (!impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame)) {
                    m_owner->syncClipAndSelectionControls();
                    if (m_onSyncView) {
                        m_onSyncView();
                    }
                    return;
                }

                m_dragTarget = DragTarget::KeyNode;
                m_dragBone = ch.boneIndex;
                m_dragChannel = ch.channelIndex;
                m_dragKeyIndex = ki;
                m_dragOrigFrame = k.frame;
                m_dragOrigValue = val;
                m_dragUnwrapOffset = val - k.value;
                m_dragStartMouse = event->position();
                m_dragMutated = false;
                m_preDragSel = impl->selectedKeys;

                // Capture all selected visible keys for coherent multi-key dragging.
                m_preDragSlots.clear();
                for (size_t subVecIdx : channels) {
                    const auto &subCh = clip.channels[subVecIdx];
                    if (!subCh.visible) {
                        continue;
                    }
                    const PreparedChannel &subPrep =
                        impl->getPreparedChannel(clip, clipIdx, subVecIdx);
                    for (size_t ski = 0; ski < subCh.keys.size(); ++ski) {
                        const auto &sk = subCh.keys[ski];
                        if (impl->isSelected(subCh.boneIndex, subCh.channelIndex, ski, sk.frame)) {
                            const double sval = (ski < subPrep.keys.size())
                                                    ? subPrep.keys[ski].unwrappedValue
                                                    : sk.value;
                            m_preDragSlots.push_back(SelectedFCurveSlot{
                                subCh.boneIndex, subCh.channelIndex, ski, sk, sk, sval - sk.value});
                        }
                    }
                }
                m_curDragSlots = m_preDragSlots;
                m_owner->syncClipAndSelectionControls();
                if (m_onSyncView) {
                    m_onSyncView();
                }
                return;
            }
        }
    }

    m_dragTarget = DragTarget::Marquee;
    m_dragStartMouse = event->position();
    m_lastMouse = event->position();
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
        impl->selectedKeys.clear();
        m_owner->syncClipAndSelectionControls();
    }
    update();
}

void FCurveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_owner || !m_owner->m_impl) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const uint32_t clipIdx = m_owner->m_activeClipIndex;
    const auto &clips = m_owner->clips();
    if (clipIdx >= clips.size()) {
        return;
    }

    const double gutter = impl->leftGutterPx;
    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double minY = impl->fcurveMinY;
    const double maxY = std::max(minY + 0.05, impl->fcurveMaxY);
    const double availH = std::max(16.0, static_cast<double>(height()) - 24.0);
    const double pxPerUnitY = availH / (maxY - minY);

    if (m_dragTarget == DragTarget::Pan) {
        const QPointF d = event->position() - m_lastMouse;
        impl->scrollFrameOffset = std::max(-10.0, impl->scrollFrameOffset - d.x() / zoomX);
        const double dy = d.y() / pxPerUnitY;
        impl->fcurveMinY += dy;
        impl->fcurveMaxY += dy;
        m_lastMouse = event->position();
        if (m_onSyncView) {
            m_onSyncView();
        }
        return;
    }

    if (m_dragTarget == DragTarget::Marquee) {
        m_lastMouse = event->position();
        update();
        return;
    }

    if (m_dragTarget == DragTarget::None || m_preDragSlots.empty()) {
        return;
    }

    const auto &clip = clips[clipIdx];
    const double df = (event->position().x() - m_dragStartMouse.x()) / zoomX;
    const double dv = -(event->position().y() - m_dragStartMouse.y()) / pxPerUnitY;
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);

    std::vector<FCurveKeySnapshot> toRemove;
    std::vector<FCurveKeySnapshot> toInsert;
    std::vector<SelectedFCurveSlot> nextSlots = m_preDragSlots;

    if (m_dragTarget == DragTarget::KeyNode) {
        double snapDf = event->modifiers().testFlag(Qt::AltModifier) ? df : std::round(df);
        double effDv = dv;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            if (std::abs(event->position().x() - m_dragStartMouse.x()) >=
                std::abs(event->position().y() - m_dragStartMouse.y())) {
                effDv = 0.0;
            } else {
                snapDf = 0.0;
            }
        }
        for (size_t i = 0; i < nextSlots.size(); ++i) {
            toRemove.push_back(m_curDragSlots[i].curKey);
            FCurveKeySnapshot &nk = nextSlots[i].curKey;
            nk.frame = std::clamp(m_preDragSlots[i].origKey.frame + snapDf, 0.0, maxF);
            // Keep unwrapped continuity across 2*pi atan2 seams when dragging F-curves.
            nk.value = (m_preDragSlots[i].origKey.value + m_preDragSlots[i].unwrapOffset) + effDv;
            toInsert.push_back(nk);
        }
    } else {
        const SelectedFCurveSlot &preSlot = m_preDragSlots[0];
        toRemove.push_back(m_curDragSlots[0].curKey);
        FCurveKeySnapshot &nk = nextSlots[0].curKey;
        nk.tangentMode = static_cast<uint8_t>(KeyTangentMode::Aligned);

        const double curF = impl->scrollFrameOffset + (event->position().x() - gutter) / zoomX;
        const double curV = maxY - ((event->position().y() - 12.0) / availH) * (maxY - minY);
        const double maxInSpan =
            m_hasPrevFrame ? std::max(1e-3, nk.frame - m_preDragPrevFrame) : 1e6;
        const double maxOutSpan =
            m_hasNextFrame ? std::max(1e-3, m_preDragNextFrame - nk.frame) : 1e6;

        if (m_dragTarget == DragTarget::HandleIn) {
            nk.handleInDx = std::clamp(curF - nk.frame, -maxInSpan, -1e-4);
            nk.handleInDy = curV - m_dragOrigValue;
            if (!nk.handlesBroken) {
                const double inLen = std::hypot(nk.handleInDx, nk.handleInDy);
                const double outLen =
                    std::hypot(preSlot.origKey.handleOutDx, preSlot.origKey.handleOutDy);
                if (inLen > 1e-9 && outLen > 1e-9) {
                    const double ux = -nk.handleInDx / inLen;
                    const double uy = -nk.handleInDy / inLen;
                    double scale = outLen;
                    if (ux * scale > maxOutSpan && ux > 1e-9) {
                        scale = maxOutSpan / ux;
                    }
                    nk.handleOutDx = ux * scale;
                    nk.handleOutDy = uy * scale;
                }
            }
        } else {
            nk.handleOutDx = std::clamp(curF - nk.frame, 1e-4, maxOutSpan);
            nk.handleOutDy = curV - m_dragOrigValue;
            if (!nk.handlesBroken) {
                const double outLen = std::hypot(nk.handleOutDx, nk.handleOutDy);
                const double inLen =
                    std::hypot(preSlot.origKey.handleInDx, preSlot.origKey.handleInDy);
                if (outLen > 1e-9 && inLen > 1e-9) {
                    const double ux = -nk.handleOutDx / outLen;
                    const double uy = -nk.handleOutDy / outLen;
                    double scale = inLen;
                    if (-ux * scale > maxInSpan && -ux > 1e-9) {
                        scale = maxInSpan / (-ux);
                    }
                    nk.handleInDx = ux * scale;
                    nk.handleInDy = uy * scale;
                }
            }
        }
        toInsert.push_back(nk);
    }

    m_dragMutated = true;
    m_owner->applyClipDiffPatch(m_owner->m_objectId, clip.id, toRemove, toInsert, true, true);
    m_curDragSlots = std::move(nextSlots);

    impl->selectedKeys.clear();
    for (const auto &slot : m_curDragSlots) {
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex != slot.boneIndex || ch.channelIndex != slot.channelIndex) {
                continue;
            }
            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                if (keysMatchExactly(ch.keys[ki], slot.curKey) &&
                    !impl->isSelected(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame)) {
                    impl->addSelectionUnique(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame);
                    break;
                }
            }
        }
    }

    if (m_onSyncView) {
        m_onSyncView();
    }
}

void FCurveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragTarget == DragTarget::Pan && event->button() != Qt::MiddleButton &&
        event->button() != Qt::RightButton) {
        return;
    }
    if ((m_dragTarget == DragTarget::KeyNode || m_dragTarget == DragTarget::HandleIn ||
         m_dragTarget == DragTarget::HandleOut || m_dragTarget == DragTarget::Marquee) &&
        event->button() != Qt::LeftButton) {
        return;
    }

    if (!m_owner || !m_owner->m_impl) {
        m_dragTarget = DragTarget::None;
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const uint32_t clipIdx = m_owner->m_activeClipIndex;
    const auto &clips = m_owner->clips();

    if (m_dragTarget == DragTarget::Marquee && clipIdx < clips.size()) {
        const QRectF box = QRectF(m_dragStartMouse, event->position()).normalized();
        const double gutter = impl->leftGutterPx;
        const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double scrollF = impl->scrollFrameOffset;
        const double minY = impl->fcurveMinY;
        const double maxY = std::max(minY + 0.05, impl->fcurveMaxY);
        const double pad = 12.0;
        const double avail = std::max(16.0, static_cast<double>(height()) - 2.0 * pad);
        const TimelineAnimClip &clip = clips[clipIdx];

        for (size_t chVecIdx : collectVisibleChannelIndices()) {
            const auto &ch = clip.channels[chVecIdx];
            if (!ch.visible) {
                continue;
            }
            const PreparedChannel &prep = impl->getPreparedChannel(clip, clipIdx, chVecIdx);
            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                const double kx = (ch.keys[ki].frame - scrollF) * zoomX + gutter;
                const double ky =
                    pad + (maxY - prep.keys[ki].unwrappedValue) / (maxY - minY) * avail;
                if (box.contains(QPointF(kx, ky))) {
                    impl->addSelectionUnique(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame);
                }
            }
        }
        m_dragTarget = DragTarget::None;
        m_owner->syncClipAndSelectionControls();
        if (m_onSyncView) {
            m_onSyncView();
        }
        return;
    }

    if (m_dragMutated && !m_preDragSlots.empty() && clipIdx < clips.size() &&
        (m_dragTarget == DragTarget::KeyNode || m_dragTarget == DragTarget::HandleIn ||
         m_dragTarget == DragTarget::HandleOut)) {
        const int32_t clipId = clips[clipIdx].id;
        std::vector<FCurveKeySnapshot> oldDiff;
        std::vector<FCurveKeySnapshot> curRemove;
        std::vector<FCurveKeySnapshot> newDiff;
        for (size_t i = 0; i < m_preDragSlots.size(); ++i) {
            oldDiff.push_back(m_preDragSlots[i].origKey);
            curRemove.push_back(m_curDragSlots[i].curKey);
            newDiff.push_back(m_curDragSlots[i].curKey);
        }
        std::vector<SelectedKeyRef> postSel = impl->selectedKeys;
        m_owner->applyClipDiffPatch(m_owner->m_objectId, clipId, curRemove, oldDiff, true, true);
        impl->selectedKeys = m_preDragSel;
        const QString lbl = (m_dragTarget == DragTarget::KeyNode) ? tr("Move F-curve key")
                                                                  : tr("Edit Bezier tangent");
        m_owner->commitClipDiffEdit(clipId, lbl, std::move(oldDiff), std::move(newDiff), true);
        impl->selectedKeys = std::move(postSel);
        m_owner->syncClipAndSelectionControls();
    }

    m_dragTarget = DragTarget::None;
    m_preDragSlots.clear();
    m_curDragSlots.clear();
    m_dragMutated = false;
}

void FCurveCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_owner || !m_owner->m_impl || event->button() != Qt::LeftButton) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const uint32_t clipIdx = m_owner->m_activeClipIndex;
    const auto &clips = m_owner->clips();
    if (clipIdx >= clips.size()) {
        return;
    }

    const auto channels = collectVisibleChannelIndices();
    if (channels.empty()) {
        return;
    }

    const TimelineAnimClip &clip = clips[clipIdx];
    const double gutter = impl->leftGutterPx;
    const double zoomX = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
    const double minY = impl->fcurveMinY;
    const double maxY = std::max(minY + 0.05, impl->fcurveMaxY);
    const double availH = std::max(16.0, static_cast<double>(height()) - 24.0);
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);

    const double frame = std::clamp(
        std::round(impl->scrollFrameOffset + (event->position().x() - gutter) / zoomX), 0.0, maxF);
    const double value = maxY - ((event->position().y() - 12.0) / availH) * (maxY - minY);

    const TimelineFCurveChannel &targetCh = clip.channels[channels.front()];
    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> newDiff;

    bool found = false;
    for (const auto &k : targetCh.keys) {
        if (std::abs(k.frame - frame) < 0.5) {
            oldDiff.push_back(k);
            FCurveKeySnapshot nk = k;
            nk.value = value;
            newDiff.push_back(nk);
            found = true;
            break;
        }
    }
    if (!found) {
        FCurveKeySnapshot nk;
        nk.animId = toWireAnimId(clip.id);
        nk.boneIndex = targetCh.boneIndex;
        nk.channelIndex = targetCh.channelIndex;
        nk.frame = frame;
        nk.value = value;
        nk.tangentMode = static_cast<uint8_t>(KeyTangentMode::Auto);
        newDiff.push_back(nk);
    }

    m_owner->commitClipDiffEdit(clip.id, tr("Insert F-curve key"), std::move(oldDiff),
                                std::move(newDiff), found);
    impl->selectOnly(targetCh.boneIndex, targetCh.channelIndex, kInvalidKeyIndex, frame);
    m_owner->syncClipAndSelectionControls();
}

void FCurveCanvas::wheelEvent(QWheelEvent *event)
{
    if (!m_owner || !m_owner->m_impl) {
        return;
    }
    auto *impl = m_owner->m_impl.get();
    const double gutter = impl->leftGutterPx;
    const double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double oldZoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double mouseF = impl->scrollFrameOffset + (event->position().x() - gutter) / oldZoom;
        const double newZoom =
            std::clamp(oldZoom * std::pow(1.18, steps), kMinZoomPxPerFrame, kMaxZoomPxPerFrame);
        if (m_setZoom) {
            m_setZoom(newZoom);
        }
        impl->scrollFrameOffset =
            std::max(-10.0, mouseF - (event->position().x() - gutter) / newZoom);
    } else if (event->modifiers().testFlag(Qt::AltModifier)) {
        const double mid = 0.5 * (impl->fcurveMinY + impl->fcurveMaxY);
        const double half = std::max(0.05, 0.5 * (impl->fcurveMaxY - impl->fcurveMinY));
        const double newHalf = std::clamp(half * std::pow(0.85, steps), 0.05, 10000.0);
        impl->fcurveMinY = mid - newHalf;
        impl->fcurveMaxY = mid + newHalf;
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) || event->angleDelta().x() != 0) {
        const double zoom = std::max(kMinZoomPxPerFrame, m_getZoom ? m_getZoom() : 12.0);
        const double panPx =
            (event->angleDelta().x() != 0) ? event->angleDelta().x() : event->angleDelta().y();
        impl->scrollFrameOffset = std::max(-10.0, impl->scrollFrameOffset - (panPx * 0.25) / zoom);
    } else {
        const double span = impl->fcurveMaxY - impl->fcurveMinY;
        const double shift = -steps * span * 0.1;
        impl->fcurveMinY += shift;
        impl->fcurveMaxY += shift;
    }

    if (m_onSyncView) {
        m_onSyncView();
    }
    event->accept();
}

DopeSheetTimelineWidget::DopeSheetTimelineWidget(QWidget *parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>())
{
    m_impl->leftGutterPx = measureLeftGutterPx();

    setObjectName(QStringLiteral("DopeSheetTimelineWidget"));
    setAccessibleName(tr("Animation Dope-Sheet and F-Curve Timeline"));
    setFocusPolicy(Qt::StrongFocus);

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
        "border-bottom: 1px solid #313244; padding: 4px 6px; font-size: 11px; font-weight: bold; "
        "}"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 4, 6, 6);
    rootLayout->setSpacing(4);

    auto *transportContainer = new QWidget(this);
    transportContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *transportBar = new TimelineFlowLayout(transportContainer, 0, 4, 4);

    auto *firstBtn = new QPushButton(QStringLiteral("|<"), transportContainer);
    firstBtn->setAccessibleName(tr("Jump to first frame"));
    firstBtn->setToolTip(tr("Jump to frame 0 (Home)"));
    connect(firstBtn, &QPushButton::clicked, this, [this]() { setCurrentFrame(0.0); });

    auto *prevBtn = new QPushButton(QStringLiteral("<"), transportContainer);
    prevBtn->setAccessibleName(tr("Step back one frame"));
    prevBtn->setToolTip(tr("Step backward 1 frame (Left Arrow)"));
    connect(prevBtn, &QPushButton::clicked, this, [this]() { onStepFrame(-1); });

    m_playBtn = new QPushButton(tr("Play"), transportContainer);
    m_playBtn->setCheckable(true);
    m_playBtn->setAccessibleName(tr("Play or pause animation"));
    m_playBtn->setToolTip(tr("Toggle playback (Space)"));
    connect(m_playBtn, &QPushButton::clicked, this, &DopeSheetTimelineWidget::onPlayPauseToggled);

    auto *nextBtn = new QPushButton(QStringLiteral(">"), transportContainer);
    nextBtn->setAccessibleName(tr("Step forward one frame"));
    nextBtn->setToolTip(tr("Step forward 1 frame (Right Arrow)"));
    connect(nextBtn, &QPushButton::clicked, this, [this]() { onStepFrame(1); });

    auto *lastBtn = new QPushButton(QStringLiteral(">|"), transportContainer);
    lastBtn->setAccessibleName(tr("Jump to last frame"));
    lastBtn->setToolTip(tr("Jump to final frame (End)"));
    connect(lastBtn, &QPushButton::clicked, this, [this]() {
        const double endF =
            (m_activeClipIndex < m_clips.size())
                ? static_cast<double>(std::max(1, m_clips[m_activeClipIndex].lengthFrames) - 1)
                : 119.0;
        setCurrentFrame(endF);
    });

    m_timecodeLabel = new QLabel(QStringLiteral("00:00.00 | f0000 / 0120"), transportContainer);
    m_timecodeLabel->setFont(timelineUiMonoFont());
    m_timecodeLabel->setStyleSheet(
        QStringLiteral("color: #38bdf8; background-color: #11111b; border: 1px solid #313244; "
                       "border-radius: 4px; padding: 2px 8px;"));
    m_timecodeLabel->setAccessibleName(tr("Current playback timecode and frame"));

    auto *insertKeyBtn = new QPushButton(tr("+ Key"), transportContainer);
    insertKeyBtn->setAccessibleName(tr("Insert keyframe at playhead"));
    insertKeyBtn->setToolTip(tr("Insert keyframe at playhead (K or I)"));
    connect(insertKeyBtn, &QPushButton::clicked, this,
            &DopeSheetTimelineWidget::onInsertKeyframeAtPlayhead);

    m_impl->tangentCombo = new QComboBox(transportContainer);
    m_impl->tangentCombo->addItem(tr("Auto"), static_cast<int>(KeyTangentMode::Auto));
    m_impl->tangentCombo->addItem(tr("Aligned"), static_cast<int>(KeyTangentMode::Aligned));
    m_impl->tangentCombo->addItem(tr("Vector"), static_cast<int>(KeyTangentMode::Vector));
    m_impl->tangentCombo->addItem(tr("Stepped"), static_cast<int>(KeyTangentMode::Stepped));
    m_impl->tangentCombo->setAccessibleName(tr("Selected keyframe tangent mode"));
    m_impl->tangentCombo->setToolTip(tr("Set tangent mode for selected keys (1..4)"));
    connect(m_impl->tangentCombo, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        if (!m_impl || m_impl->updatingUi) {
            return;
        }
        const auto mode = static_cast<KeyTangentMode>(m_impl->tangentCombo->itemData(idx).toInt());
        onSetSelectedTangentMode(mode);
    });

    m_impl->breakHandlesCheck = new QCheckBox(tr("Break"), transportContainer);
    m_impl->breakHandlesCheck->setAccessibleName(tr("Break Bezier tangent handles"));
    m_impl->breakHandlesCheck->setToolTip(
        tr("Unlink in/out tangent handle angles on selected keys"));
    connect(m_impl->breakHandlesCheck, &QCheckBox::toggled, this, [this](bool broken) {
        if (!m_impl || m_impl->updatingUi || m_activeClipIndex >= m_clips.size()) {
            return;
        }
        const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
        std::vector<FCurveKeySnapshot> oldDiff;
        std::vector<FCurveKeySnapshot> newDiff;
        for (const auto &ch : clip.channels) {
            for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                const auto &k = ch.keys[ki];
                if (m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame) &&
                    k.handlesBroken != broken) {
                    oldDiff.push_back(k);
                    FCurveKeySnapshot nk = k;
                    nk.handlesBroken = broken;
                    newDiff.push_back(nk);
                }
            }
        }
        if (!oldDiff.empty()) {
            commitClipDiffEdit(clip.id, tr("Toggle broken Bezier handles"), std::move(oldDiff),
                               std::move(newDiff), true);
        }
    });

    m_viewModeCombo = new QComboBox(transportContainer);
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

    m_impl->syncBtn = new QPushButton(tr("Bake & Sync"), transportContainer);
    m_impl->syncBtn->setAccessibleName(tr("Bake F-curves and sync to daemon"));
    m_impl->syncBtn->setToolTip(
        tr("Bake sparse Bezier F-curves into MDLA tracks and push to daemon"));
    connect(m_impl->syncBtn, &QPushButton::clicked, this,
            &DopeSheetTimelineWidget::onBakeAndSyncTracks);

    transportBar->addWidget(firstBtn);
    transportBar->addWidget(prevBtn);
    transportBar->addWidget(m_playBtn);
    transportBar->addWidget(nextBtn);
    transportBar->addWidget(lastBtn);
    transportBar->addWidget(m_timecodeLabel);
    transportBar->addWidget(insertKeyBtn);
    transportBar->addWidget(m_impl->tangentCombo);
    transportBar->addWidget(m_impl->breakHandlesCheck);
    transportBar->addWidget(m_viewModeCombo);
    transportBar->addWidget(m_impl->syncBtn);
    rootLayout->addWidget(transportContainer);

    auto *mixerContainer = new QWidget(this);
    mixerContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto *mixerBar = new TimelineFlowLayout(mixerContainer, 0, 6, 4);

    m_clipCombo = new QComboBox(mixerContainer);
    m_clipCombo->setObjectName(QStringLiteral("timelineClipCombo"));
    m_clipCombo->setMinimumWidth(96);
    m_clipCombo->setAccessibleName(tr("Active animation clip"));
    m_clipCombo->setToolTip(tr("Select animation clip layer"));

    m_impl->addClipBtn = new QPushButton(QStringLiteral("+Clip"), mixerContainer);
    m_impl->addClipBtn->setAccessibleName(tr("Add animation clip"));
    m_impl->addClipBtn->setToolTip(tr("Create a new animation clip layer (max 64)"));

    m_animModeCombo = new QComboBox(mixerContainer);
    m_animModeCombo->addItem(tr("Loop"), static_cast<int>(WE_ANIM_LOOP));
    m_animModeCombo->addItem(tr("Mirror"), static_cast<int>(WE_ANIM_MIRROR));
    m_animModeCombo->addItem(tr("Single"), static_cast<int>(WE_ANIM_SINGLE));
    m_animModeCombo->setAccessibleName(tr("Clip playback mode"));
    m_animModeCombo->setToolTip(tr("Loop, Mirror ping-pong, or Single play mode"));

    m_impl->blendModeCombo = new QComboBox(mixerContainer);
    m_impl->blendModeCombo->addItem(tr("Override"), static_cast<int>(WE_ANIM_BLEND_OVERRIDE));
    m_impl->blendModeCombo->addItem(tr("Additive"), static_cast<int>(WE_ANIM_BLEND_ADDITIVE));
    m_impl->blendModeCombo->setAccessibleName(tr("Layer blend mode"));
    m_impl->blendModeCombo->setToolTip(tr("Layer mixing mode: Override or Additive delta"));

    m_impl->layerVisibleCheck = new QCheckBox(tr("Vis"), mixerContainer);
    m_impl->layerVisibleCheck->setChecked(true);
    m_impl->layerVisibleCheck->setAccessibleName(tr("Layer visibility"));
    m_impl->layerVisibleCheck->setToolTip(tr("Enable or mute active animation layer"));

    auto *fpsLbl = new QLabel(tr("FPS"), mixerContainer);
    fpsLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_fpsSpin = new QDoubleSpinBox(mixerContainer);
    m_fpsSpin->setRange(1.0, 1000.0);
    m_fpsSpin->setDecimals(1);
    m_fpsSpin->setValue(30.0);
    m_fpsSpin->setKeyboardTracking(false);
    m_fpsSpin->setAccessibleName(tr("Frames per second"));
    m_fpsSpin->setToolTip(tr("Uniform playback and MDLA bake frame rate (FPS)"));

    auto *lenLbl = new QLabel(tr("Len"), mixerContainer);
    lenLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_lengthSpin = new QSpinBox(mixerContainer);
    m_lengthSpin->setRange(1, 1000000);
    m_lengthSpin->setValue(120);
    m_lengthSpin->setKeyboardTracking(false);
    m_lengthSpin->setAccessibleName(tr("Clip length in frames"));
    m_lengthSpin->setToolTip(tr("Total frame count of the active animation clip"));

    auto *rateLbl = new QLabel(tr("Rate"), mixerContainer);
    rateLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_layerRateSpin = new QDoubleSpinBox(mixerContainer);
    m_layerRateSpin->setRange(-16.0, 16.0);
    m_layerRateSpin->setSingleStep(0.1);
    m_layerRateSpin->setDecimals(2);
    m_layerRateSpin->setValue(1.0);
    m_layerRateSpin->setKeyboardTracking(false);
    m_layerRateSpin->setAccessibleName(tr("Layer playback rate multiplier"));
    m_layerRateSpin->setToolTip(tr("Layer playback speed multiplier"));

    auto *blendLbl = new QLabel(tr("Blend"), mixerContainer);
    blendLbl->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_layerBlendSpin = new QDoubleSpinBox(mixerContainer);
    m_layerBlendSpin->setRange(0.0, 1.0);
    m_layerBlendSpin->setSingleStep(0.05);
    m_layerBlendSpin->setDecimals(2);
    m_layerBlendSpin->setValue(1.0);
    m_layerBlendSpin->setKeyboardTracking(false);
    m_layerBlendSpin->setAccessibleName(tr("Layer blend weight"));
    m_layerBlendSpin->setToolTip(tr("Layer influence weight (0.0 to 1.0)"));

    m_onionCheck = new QCheckBox(tr("Onion"), mixerContainer);
    m_onionCheck->setObjectName(QStringLiteral("timelineOnionCheck"));
    m_onionCheck->setAccessibleName(tr("Toggle onion-skinning ghost frames"));
    m_onionCheck->setToolTip(tr("Toggle past and future ghost poses in viewport (O)"));

    m_impl->onionBeforeSpin = new QSpinBox(mixerContainer);
    m_impl->onionBeforeSpin->setRange(0, 5);
    m_impl->onionBeforeSpin->setValue(2);
    m_impl->onionBeforeSpin->setPrefix(QStringLiteral("-"));
    m_impl->onionBeforeSpin->setKeyboardTracking(false);
    m_impl->onionBeforeSpin->setAccessibleName(tr("Onion-skin before frame count"));
    m_impl->onionBeforeSpin->setToolTip(tr("Ghost frames before playhead"));

    m_impl->onionAfterSpin = new QSpinBox(mixerContainer);
    m_impl->onionAfterSpin->setRange(0, 5);
    m_impl->onionAfterSpin->setValue(2);
    m_impl->onionAfterSpin->setPrefix(QStringLiteral("+"));
    m_impl->onionAfterSpin->setKeyboardTracking(false);
    m_impl->onionAfterSpin->setAccessibleName(tr("Onion-skin after frame count"));
    m_impl->onionAfterSpin->setToolTip(tr("Ghost frames after playhead"));

    m_impl->onionOpacitySpin = new QDoubleSpinBox(mixerContainer);
    m_impl->onionOpacitySpin->setRange(0.10, 0.60);
    m_impl->onionOpacitySpin->setSingleStep(0.05);
    m_impl->onionOpacitySpin->setDecimals(2);
    m_impl->onionOpacitySpin->setValue(0.30);
    m_impl->onionOpacitySpin->setKeyboardTracking(false);
    m_impl->onionOpacitySpin->setAccessibleName(tr("Onion-skin ghost opacity"));
    m_impl->onionOpacitySpin->setToolTip(tr("Ghost overlay opacity"));

    m_impl->exportSidecarBtn = new QPushButton(tr("Save Sidecar"), mixerContainer);
    m_impl->exportSidecarBtn->setAccessibleName(tr("Save sparse curves sidecar JSON"));
    m_impl->exportSidecarBtn->setToolTip(
        tr("Save lossless sparse Bezier F-curves to studio_rig_<id>.json"));

    m_impl->importSidecarBtn = new QPushButton(tr("Load Sidecar"), mixerContainer);
    m_impl->importSidecarBtn->setAccessibleName(tr("Load sparse curves sidecar JSON"));
    m_impl->importSidecarBtn->setToolTip(
        tr("Load lossless sparse Bezier F-curves from studio_rig_<id>.json"));

    mixerBar->addWidget(m_clipCombo);
    mixerBar->addWidget(m_impl->addClipBtn);
    mixerBar->addWidget(m_animModeCombo);
    mixerBar->addWidget(m_impl->blendModeCombo);
    mixerBar->addWidget(m_impl->layerVisibleCheck);
    mixerBar->addWidget(fpsLbl);
    mixerBar->addWidget(m_fpsSpin);
    mixerBar->addWidget(lenLbl);
    mixerBar->addWidget(m_lengthSpin);
    mixerBar->addWidget(rateLbl);
    mixerBar->addWidget(m_layerRateSpin);
    mixerBar->addWidget(blendLbl);
    mixerBar->addWidget(m_layerBlendSpin);
    mixerBar->addWidget(m_onionCheck);
    mixerBar->addWidget(m_impl->onionBeforeSpin);
    mixerBar->addWidget(m_impl->onionAfterSpin);
    mixerBar->addWidget(m_impl->onionOpacitySpin);
    mixerBar->addWidget(m_impl->exportSidecarBtn);
    mixerBar->addWidget(m_impl->importSidecarBtn);
    rootLayout->addWidget(mixerContainer);

    auto *outerSplitter = new QSplitter(Qt::Horizontal, this);
    outerSplitter->setChildrenCollapsible(false);

    m_trackTree = new QTreeWidget(outerSplitter);
    m_trackTree->setHeaderLabels({tr("Track / Channel"), tr("Value")});
    m_trackTree->setColumnCount(2);
    m_trackTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_trackTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_trackTree->header()->setFixedHeight(kRulerHeightPx);
    m_trackTree->setMinimumWidth(180);
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
        this,
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
    outerSplitter->setSizes({220, 760});
    rootLayout->addWidget(outerSplitter, 1);

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
            [this, syncCanvases](QTreeWidgetItem *item, int col) {
                if (!item || col != 0 || !m_impl || m_impl->updatingUi ||
                    m_activeClipIndex >= m_clips.size()) {
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

    m_clipCombo->addItem(m_clips[0].name, m_clips[0].id);
    syncClipAndSelectionControls();

    connect(m_clipCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_impl || m_impl->updatingUi || idx < 0 ||
            static_cast<size_t>(idx) >= m_clips.size()) {
            return;
        }
        m_activeClipIndex = static_cast<uint32_t>(idx);
        m_impl->selectedKeys.clear();
        m_impl->invalidatePrepCache();
        syncClipAndSelectionControls();
        rebuildTrackTree();
        setCurrentFrame(m_currentFrame);
    });

    connect(m_impl->addClipBtn, &QPushButton::clicked, this, [this]() {
        if (!m_impl) {
            return;
        }
        if (m_clips.size() >= WE_MDL_MAX_ANIMS) {
            emit statusMessage(tr("Clip limit (%1) reached").arg(WE_MDL_MAX_ANIMS));
            return;
        }
        ClipCollectionCommand::Snapshot oldSnap{m_objectId, m_clips, m_activeClipIndex,
                                                m_impl->selectedKeys};
        ClipCollectionCommand::Snapshot newSnap = oldSnap;
        TimelineAnimClip clip;
        clip.id = allocateUniqueClipId(newSnap.clips);
        clip.name = QStringLiteral("Clip_%1").arg(newSnap.clips.size());
        newSnap.clips.push_back(std::move(clip));
        newSnap.activeClipIdx = static_cast<uint32_t>(newSnap.clips.size() - 1u);
        newSnap.selectedKeys.clear();

        QPointer<DopeSheetTimelineWidget> self(this);
        auto applyFn = [self](const ClipCollectionCommand::Snapshot &snap) {
            if (!self || !self->m_impl) {
                return;
            }
            if (snap.objectId != self->m_objectId) {
                self->saveActiveObjectState();
                self->restoreObjectState(snap.objectId);
            }
            self->m_clips = snap.clips;
            self->m_activeClipIndex = std::min<uint32_t>(
                snap.activeClipIdx, static_cast<uint32_t>(self->m_clips.size() - 1u));
            self->m_impl->selectedKeys = snap.selectedKeys;
            self->m_impl->invalidatePrepCache();
            {
                const QSignalBlocker b(self->m_clipCombo);
                self->m_clipCombo->clear();
                for (const auto &c : self->m_clips) {
                    self->m_clipCombo->addItem(c.name, c.id);
                }
            }
            self->syncClipAndSelectionControls();
            self->rebuildTrackTree();
            self->setCurrentFrame(self->m_currentFrame);
            self->syncClipsToDaemon(self->m_activeClipIndex, true);
        };

        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<ClipCollectionCommand>(
                tr("Add animation clip"), std::move(oldSnap), std::move(newSnap),
                std::move(applyFn)));
        } else {
            applyFn(newSnap);
        }
    });

    auto commitMixerChanges = [this]() {
        if (!m_impl || m_impl->updatingUi || m_activeClipIndex >= m_clips.size()) {
            return;
        }
        const TimelineAnimClip &cur = m_clips[m_activeClipIndex];
        ClipMetaState oldMeta;
        oldMeta.mode = cur.mode;
        oldMeta.blendMode = cur.blendMode;
        oldMeta.layerVisible = cur.layerVisible;
        oldMeta.fps = cur.fps;
        oldMeta.lengthFrames = cur.lengthFrames;
        oldMeta.layerRate = cur.layerRate;
        oldMeta.layerBlend = cur.layerBlend;

        ClipMetaState newMeta;
        newMeta.mode = static_cast<we_mdl_anim_mode_t>(m_animModeCombo->currentData().toInt());
        newMeta.blendMode =
            static_cast<we_anim_blend_mode_t>(m_impl->blendModeCombo->currentData().toInt());
        newMeta.layerVisible = m_impl->layerVisibleCheck->isChecked();
        newMeta.fps = static_cast<float>(m_fpsSpin->value());
        newMeta.lengthFrames = m_lengthSpin->value();
        newMeta.layerRate = static_cast<float>(m_layerRateSpin->value());
        newMeta.layerBlend = static_cast<float>(m_layerBlendSpin->value());

        if (newMeta.lengthFrames < oldMeta.lengthFrames) {
            const double newMaxFrame = static_cast<double>(std::max(1, newMeta.lengthFrames) - 1);
            for (const auto &ch : cur.channels) {
                for (const auto &k : ch.keys) {
                    if (k.frame > newMaxFrame + 1e-4) {
                        oldMeta.trimmedKeys.push_back(k);
                    }
                }
            }
        }

        QPointer<DopeSheetTimelineWidget> self(this);
        auto applyFn = [self](int32_t objId, int32_t clipId, const ClipMetaState &st,
                              bool restoreTrimmed) {
            if (!self || !self->m_impl) {
                return;
            }
            if (objId != self->m_objectId) {
                self->saveActiveObjectState();
                self->restoreObjectState(objId);
            }
            auto it = std::find_if(self->m_clips.begin(), self->m_clips.end(),
                                   [clipId](const TimelineAnimClip &c) { return c.id == clipId; });
            if (it == self->m_clips.end()) {
                return;
            }
            const uint32_t cIdx = static_cast<uint32_t>(std::distance(self->m_clips.begin(), it));
            it->mode = st.mode;
            it->blendMode = st.blendMode;
            it->layerVisible = st.layerVisible;
            it->fps = st.fps;
            it->lengthFrames = st.lengthFrames;
            it->layerRate = st.layerRate;
            it->layerBlend = st.layerBlend;

            bool structuralChange = false;
            const double maxFrame = static_cast<double>(std::max(1, it->lengthFrames) - 1);
            for (auto &ch : it->channels) {
                const size_t beforeSz = ch.keys.size();
                ch.keys.erase(std::remove_if(ch.keys.begin(), ch.keys.end(),
                                             [maxFrame](const FCurveKeySnapshot &k) {
                                                 return k.frame > maxFrame + 1e-4;
                                             }),
                              ch.keys.end());
                if (ch.keys.size() != beforeSz) {
                    structuralChange = true;
                }
            }
            if (restoreTrimmed && !st.trimmedKeys.empty()) {
                self->applyClipDiffPatch(objId, clipId, {}, st.trimmedKeys, false, true);
            } else if (structuralChange) {
                it->channels.erase(
                    std::remove_if(it->channels.begin(), it->channels.end(),
                                   [](const TimelineFCurveChannel &ch) { return ch.keys.empty(); }),
                    it->channels.end());
                self->m_impl->invalidatePrepCache();
                if (cIdx == self->m_activeClipIndex) {
                    self->rebuildTrackTree();
                }
            }

            if (cIdx == self->m_activeClipIndex) {
                self->syncClipAndSelectionControls();
                self->setCurrentFrame(self->m_currentFrame);
            }
            self->syncClipsToDaemon(cIdx, false);
        };

        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<ClipMetaEditCommand>(
                m_objectId, cur.id, tr("Edit clip settings"), std::move(oldMeta),
                std::move(newMeta), std::move(applyFn)));
        } else {
            applyFn(m_objectId, cur.id, newMeta, false);
        }
    };

    connect(m_animModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [commitMixerChanges](int) { commitMixerChanges(); });
    connect(m_impl->blendModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [commitMixerChanges](int) { commitMixerChanges(); });
    connect(m_impl->layerVisibleCheck, &QCheckBox::toggled, this,
            [commitMixerChanges](bool) { commitMixerChanges(); });
    connect(m_fpsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [commitMixerChanges](double) { commitMixerChanges(); });
    connect(m_lengthSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [commitMixerChanges](int) { commitMixerChanges(); });
    connect(m_layerRateSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [commitMixerChanges](double) { commitMixerChanges(); });
    connect(m_layerBlendSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [commitMixerChanges](double) { commitMixerChanges(); });

    auto emitOnion = [this]() {
        if (!m_impl) {
            return;
        }
        emit onionSkinChanged(m_onionEnabled, m_impl->onionBeforeSpin->value(),
                              m_impl->onionAfterSpin->value(),
                              static_cast<float>(m_impl->onionOpacitySpin->value()));
    };
    connect(m_onionCheck, &QCheckBox::toggled, this, [this](bool en) { setOnionSkinEnabled(en); });
    connect(m_impl->onionBeforeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [emitOnion](int) { emitOnion(); });
    connect(m_impl->onionAfterSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [emitOnion](int) { emitOnion(); });
    connect(m_impl->onionOpacitySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitOnion](double) { emitOnion(); });

    auto writeSidecarFile = [this](const QString &path) {
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
    };

    connect(m_impl->exportSidecarBtn, &QPushButton::clicked, this, [this, writeSidecarFile]() {
        const QVariant headlessProp = property("headlessSaveSidecarPath");
        if (headlessProp.isValid()) {
            writeSidecarFile(headlessProp.toString().trimmed());
            return;
        }
        const QString defName = QStringLiteral("studio_rig_%1.json").arg(std::max(0, m_objectId));
        auto *dialog = new QFileDialog(this, tr("Save Rig Animation Sidecar"), defName,
                                       tr("JSON Files (*.json)"));
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        QPointer<DopeSheetTimelineWidget> self(this);
        connect(dialog, &QDialog::finished, this, [self, dialog, writeSidecarFile](int result) {
            if (!self || result != QDialog::Accepted || dialog->selectedFiles().isEmpty()) {
                return;
            }
            writeSidecarFile(dialog->selectedFiles().constFirst().trimmed());
        });
        dialog->open();
    });

    auto loadSidecarFile = [this](const QString &path) {
        if (path.isEmpty() || !m_impl) {
            return;
        }
        QFile inFile(path);
        if (!inFile.open(QIODevice::ReadOnly)) {
            emit statusMessage(tr("Failed to open sidecar"));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(inFile.readAll());
        std::vector<TimelineAnimClip> parsed;
        if (!doc.isObject() || !deserializeClipsFromSidecarJson(doc.object(), parsed)) {
            emit statusMessage(tr("Invalid studio_rig sidecar schema"));
            return;
        }

        ClipCollectionCommand::Snapshot oldSnap{m_objectId, m_clips, m_activeClipIndex,
                                                m_impl->selectedKeys};
        ClipCollectionCommand::Snapshot newSnap{m_objectId, std::move(parsed), 0u, {}};

        QPointer<DopeSheetTimelineWidget> self(this);
        auto applyFn = [self](const ClipCollectionCommand::Snapshot &snap) {
            if (!self || !self->m_impl) {
                return;
            }
            if (snap.objectId != self->m_objectId) {
                self->saveActiveObjectState();
                self->restoreObjectState(snap.objectId);
            }
            self->m_clips = snap.clips;
            self->m_activeClipIndex = std::min<uint32_t>(
                snap.activeClipIdx, static_cast<uint32_t>(self->m_clips.size() - 1u));
            self->m_impl->selectedKeys = snap.selectedKeys;
            self->m_impl->invalidatePrepCache();
            {
                const QSignalBlocker b(self->m_clipCombo);
                self->m_clipCombo->clear();
                for (const auto &clip : self->m_clips) {
                    self->m_clipCombo->addItem(clip.name, clip.id);
                }
            }
            self->syncClipAndSelectionControls();
            self->rebuildTrackTree();
            self->setCurrentFrame(0.0);
            self->syncClipsToDaemon(self->m_activeClipIndex, true);
        };

        if (m_commandBus) {
            m_commandBus->execute(std::make_unique<ClipCollectionCommand>(
                tr("Load animation sidecar"), std::move(oldSnap), std::move(newSnap),
                std::move(applyFn)));
        } else {
            applyFn(newSnap);
        }
        emit statusMessage(tr("Loaded animation sidecar: %1").arg(path));
    };

    connect(m_impl->importSidecarBtn, &QPushButton::clicked, this, [this, loadSidecarFile]() {
        const QVariant headlessProp = property("headlessSidecarPath");
        if (headlessProp.isValid()) {
            loadSidecarFile(headlessProp.toString().trimmed());
            return;
        }
        auto *dialog = new QFileDialog(this, tr("Load Rig Animation Sidecar"), QString(),
                                       tr("JSON Files (*.json)"));
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        dialog->setFileMode(QFileDialog::ExistingFile);
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        QPointer<DopeSheetTimelineWidget> self(this);
        connect(dialog, &QDialog::finished, this, [self, dialog, loadSidecarFile](int result) {
            if (!self || result != QDialog::Accepted || dialog->selectedFiles().isEmpty()) {
                return;
            }
            loadSidecarFile(dialog->selectedFiles().constFirst().trimmed());
        });
        dialog->open();
    });

    auto *filter = new TimelineEventFilter(
        this, m_trackTree, [this]() { updatePlaybackTimerState(); },
        [fcurve]() { fcurve->frameFitVisibleChannels(); },
        [this]() {
            if (!m_impl || m_activeClipIndex >= m_clips.size() || m_impl->selectedKeys.empty()) {
                return;
            }
            const auto &clip = m_clips[m_activeClipIndex];
            std::vector<FCurveKeySnapshot> oldDiff;
            for (const auto &ch : clip.channels) {
                for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                    if (m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame)) {
                        oldDiff.push_back(ch.keys[ki]);
                    }
                }
            }
            if (oldDiff.empty()) {
                return;
            }
            commitClipDiffEdit(clip.id, tr("Delete keyframes"), std::move(oldDiff), {}, false);
            m_impl->selectedKeys.clear();
            syncClipAndSelectionControls();
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

    m_impl->eventFilter = filter;
    installEventFilter(filter);
    m_dopeCanvas->installEventFilter(filter);
    m_fcurveCanvas->installEventFilter(filter);
    m_trackTree->installEventFilter(filter);
    if (QWidget *win = window(); win && win != this) {
        win->installEventFilter(filter);
        m_impl->watchedWindow = win;
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
    if (m_impl && m_impl->eventFilter) {
        QObject *ef = m_impl->eventFilter;
        m_impl->eventFilter = nullptr;
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
        if (m_impl->watchedWindow) {
            m_impl->watchedWindow->removeEventFilter(ef);
            m_impl->watchedWindow.clear();
        }
        static_cast<TimelineEventFilter *>(ef)->disarm();
    }
}

void DopeSheetTimelineWidget::saveActiveObjectState()
{
    if (!m_impl || m_objectId < 0) {
        return;
    }
    TimelineObjectState &st = m_impl->objectStates[m_objectId];
    st.boneCount = m_boneCount;
    st.activeClipIndex = m_activeClipIndex;
    st.currentFrame = m_currentFrame;
    st.boneNames = m_impl->boneNames;
    st.clips = m_clips;
    st.selectedKeys = m_impl->selectedKeys;
}

void DopeSheetTimelineWidget::restoreObjectState(int32_t objectId)
{
    m_objectId = objectId;
    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        m_boneCount = it->second.boneCount;
        m_activeClipIndex = it->second.activeClipIndex;
        m_currentFrame = it->second.currentFrame;
        m_impl->boneNames = it->second.boneNames;
        m_clips = it->second.clips;
        m_impl->selectedKeys = it->second.selectedKeys;
    }
    if (m_clipCombo) {
        const QSignalBlocker blocker(m_clipCombo);
        m_clipCombo->clear();
        for (const auto &clip : m_clips) {
            m_clipCombo->addItem(clip.name, clip.id);
        }
        if (m_activeClipIndex < m_clips.size()) {
            m_clipCombo->setCurrentIndex(static_cast<int>(m_activeClipIndex));
        }
    }
    m_impl->invalidatePrepCache();
}

void DopeSheetTimelineWidget::syncClipAndSelectionControls()
{
    if (!m_impl) {
        return;
    }
    const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
    if (m_activeClipIndex < m_clips.size()) {
        const auto &clip = m_clips[m_activeClipIndex];
        if (m_clipCombo) {
            m_clipCombo->setCurrentIndex(static_cast<int>(m_activeClipIndex));
        }
        if (m_animModeCombo) {
            m_animModeCombo->setCurrentIndex(
                m_animModeCombo->findData(static_cast<int>(clip.mode)));
        }
        if (m_impl->blendModeCombo) {
            m_impl->blendModeCombo->setCurrentIndex(
                m_impl->blendModeCombo->findData(static_cast<int>(clip.blendMode)));
        }
        if (m_impl->layerVisibleCheck) {
            m_impl->layerVisibleCheck->setChecked(clip.layerVisible);
        }
        if (m_fpsSpin) {
            m_fpsSpin->setValue(static_cast<double>(clip.fps));
        }
        if (m_lengthSpin) {
            m_lengthSpin->setValue(clip.lengthFrames);
        }
        if (m_layerRateSpin) {
            m_layerRateSpin->setValue(static_cast<double>(clip.layerRate));
        }
        if (m_layerBlendSpin) {
            m_layerBlendSpin->setValue(static_cast<double>(clip.layerBlend));
        }

        if (!m_impl->selectedKeys.empty()) {
            bool foundFirst = false;
            uint8_t firstMode = 0u;
            bool firstBroken = false;
            for (const auto &ch : clip.channels) {
                for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
                    if (m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, ch.keys[ki].frame)) {
                        firstMode = ch.keys[ki].tangentMode;
                        firstBroken = ch.keys[ki].handlesBroken;
                        foundFirst = true;
                        break;
                    }
                }
                if (foundFirst) {
                    break;
                }
            }
            if (foundFirst) {
                if (m_impl->tangentCombo) {
                    const int idx = m_impl->tangentCombo->findData(static_cast<int>(firstMode));
                    if (idx >= 0) {
                        m_impl->tangentCombo->setCurrentIndex(idx);
                    }
                }
                if (m_impl->breakHandlesCheck) {
                    m_impl->breakHandlesCheck->setChecked(firstBroken);
                }
            }
        }
    }
}

void DopeSheetTimelineWidget::rebuildTrackTree()
{
    if (!m_trackTree || !m_impl) {
        return;
    }

    const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
    std::set<int32_t> collapsedBones;
    int32_t curBone = -1;
    int curChannel = -1;
    if (QTreeWidgetItem *cur = m_trackTree->currentItem()) {
        curBone = cur->data(0, Qt::UserRole).toInt();
        const QVariant cv = cur->data(0, Qt::UserRole + 1);
        if (cv.isValid()) {
            curChannel = cv.toInt();
        }
    }
    for (int i = 0; i < m_trackTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = m_trackTree->topLevelItem(i);
        if (top && !top->isExpanded()) {
            collapsedBones.insert(top->data(0, Qt::UserRole).toInt());
        }
    }
    const int savedScroll =
        m_trackTree->verticalScrollBar() ? m_trackTree->verticalScrollBar()->value() : 0;

    m_trackTree->clear();
    m_impl->treeBindings.clear();

    const TimelineAnimClip *clip =
        (m_activeClipIndex < m_clips.size()) ? &m_clips[m_activeClipIndex] : nullptr;
    if (!clip) {
        return;
    }

    std::map<int32_t, std::vector<size_t>> byBone;
    for (size_t i = 0u; i < clip->channels.size(); ++i) {
        byBone[clip->channels[i].boneIndex].push_back(i);
    }

    QTreeWidgetItem *restoreCur = nullptr;
    for (const auto &[boneIdx, chIdxList] : byBone) {
        QString boneName = QStringLiteral("Bone %1").arg(boneIdx);
        if (boneIdx >= 0 && boneIdx < m_impl->boneNames.size() &&
            !m_impl->boneNames[boneIdx].isEmpty()) {
            boneName = m_impl->boneNames[boneIdx];
        }
        auto *boneItem = new QTreeWidgetItem(m_trackTree);
        boneItem->setText(0, boneName);
        boneItem->setData(0, Qt::UserRole, boneIdx);
        boneItem->setCheckState(0, Qt::Checked);
        boneItem->setExpanded(!collapsedBones.contains(boneIdx));
        if (curBone == boneIdx && curChannel < 0) {
            restoreCur = boneItem;
        }

        for (size_t chVecIdx : chIdxList) {
            const TimelineFCurveChannel &ch = clip->channels[chVecIdx];
            auto *chItem = new QTreeWidgetItem(boneItem);
            chItem->setText(0,
                            ch.label.isEmpty() ? defaultChannelLabel(ch.channelIndex) : ch.label);
            chItem->setData(0, Qt::UserRole, boneIdx);
            chItem->setData(0, Qt::UserRole + 1, static_cast<int>(ch.channelIndex));
            chItem->setData(0, Qt::UserRole + 2, static_cast<uint32_t>(chVecIdx));
            chItem->setCheckState(0, ch.visible ? Qt::Checked : Qt::Unchecked);

            const PreparedChannel &prep =
                m_impl->getPreparedChannel(*clip, m_activeClipIndex, chVecIdx);
            const double val = evaluatePreparedChannel(prep, m_currentFrame);
            chItem->setText(1, QString::number(val, 'f', 3));

            m_impl->treeBindings.push_back(
                TreeChannelBinding{static_cast<uint32_t>(chVecIdx), chItem});
            if (curBone == boneIdx && curChannel == static_cast<int>(ch.channelIndex)) {
                restoreCur = chItem;
            }
        }
    }

    if (restoreCur) {
        m_trackTree->setCurrentItem(restoreCur);
    }
    if (m_trackTree->verticalScrollBar()) {
        m_trackTree->verticalScrollBar()->setValue(savedScroll);
    }
}

void DopeSheetTimelineWidget::updateTrackTreeValues()
{
    if (!m_trackTree || !m_impl || m_activeClipIndex >= m_clips.size()) {
        return;
    }
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
    if (!channelSetMatchesTree(clip, m_impl->treeBindings)) {
        rebuildTrackTree();
        return;
    }

    const QScopedValueRollback<bool> guard(m_impl->updatingUi, true);
    for (const TreeChannelBinding &b : m_impl->treeBindings) {
        if (!b.item || b.channelVecIdx >= clip.channels.size()) {
            continue;
        }
        const PreparedChannel &prep =
            m_impl->getPreparedChannel(clip, m_activeClipIndex, b.channelVecIdx);
        const double val = evaluatePreparedChannel(prep, m_currentFrame);
        b.item->setText(1, QString::number(val, 'f', 3));
    }
}

void DopeSheetTimelineWidget::applyClipDiffPatch(int32_t objectId, int32_t clipId,
                                                 const std::vector<FCurveKeySnapshot> &removeKeys,
                                                 const std::vector<FCurveKeySnapshot> &insertKeys,
                                                 bool suppressTreeRebuild, bool suppressIpc)
{
    if (!m_impl) {
        return;
    }
    if (objectId >= 0 && objectId != m_objectId) {
        saveActiveObjectState();
        restoreObjectState(objectId);
    }

    auto it = std::find_if(m_clips.begin(), m_clips.end(),
                           [clipId](const TimelineAnimClip &c) { return c.id == clipId; });
    if (it == m_clips.end()) {
        return;
    }
    const uint32_t targetIdx = static_cast<uint32_t>(std::distance(m_clips.begin(), it));
    TimelineAnimClip &clip = *it;

    for (const FCurveKeySnapshot &rk : removeKeys) {
        for (auto &ch : clip.channels) {
            if (ch.boneIndex != rk.boneIndex || ch.channelIndex != rk.channelIndex) {
                continue;
            }
            auto hit =
                std::find_if(ch.keys.begin(), ch.keys.end(),
                             [&](const FCurveKeySnapshot &k) { return keysMatchExactly(k, rk); });
            if (hit == ch.keys.end()) {
                hit = std::find_if(ch.keys.begin(), ch.keys.end(), [&](const FCurveKeySnapshot &k) {
                    return std::abs(k.frame - rk.frame) < 1e-4;
                });
            }
            if (hit != ch.keys.end()) {
                ch.keys.erase(hit);
                break;
            }
        }
    }

    bool structuralChange = false;
    for (const FCurveKeySnapshot &ik : insertKeys) {
        if (ik.boneIndex < 0 || ik.channelIndex >= 9u) {
            continue;
        }
        TimelineFCurveChannel *targetCh = nullptr;
        for (auto &ch : clip.channels) {
            if (ch.boneIndex == ik.boneIndex && ch.channelIndex == ik.channelIndex) {
                targetCh = &ch;
                break;
            }
        }
        if (!targetCh) {
            TimelineFCurveChannel ch;
            ch.boneIndex = ik.boneIndex;
            ch.channelIndex = ik.channelIndex;
            ch.label = defaultChannelLabel(ik.channelIndex);
            ch.visible = true;
            clip.channels.push_back(std::move(ch));
            targetCh = &clip.channels.back();
            structuralChange = true;
        }
        targetCh->keys.push_back(ik);
    }

    const size_t beforePrune = clip.channels.size();
    clip.channels.erase(
        std::remove_if(clip.channels.begin(), clip.channels.end(),
                       [](const TimelineFCurveChannel &ch) { return ch.keys.empty(); }),
        clip.channels.end());
    if (clip.channels.size() != beforePrune) {
        structuralChange = true;
    }

    for (auto &ch : clip.channels) {
        std::stable_sort(ch.keys.begin(), ch.keys.end(),
                         [](const FCurveKeySnapshot &a, const FCurveKeySnapshot &b) {
                             return a.frame < b.frame;
                         });
    }

    m_impl->invalidatePrepCache();

    if (targetIdx == m_activeClipIndex) {
        if (structuralChange && !suppressTreeRebuild) {
            rebuildTrackTree();
        } else {
            updateTrackTreeValues();
        }
    }

    saveActiveObjectState();

    if (!suppressIpc && !m_impl->dragPreviewSuppressIpc) {
        syncClipsToDaemon(targetIdx, false);
    }
    if (m_dopeCanvas) {
        m_dopeCanvas->update();
    }
    if (m_fcurveCanvas) {
        m_fcurveCanvas->update();
    }
}

void DopeSheetTimelineWidget::commitClipDiffEdit(int32_t clipId, const QString &label,
                                                 std::vector<FCurveKeySnapshot> oldDiff,
                                                 std::vector<FCurveKeySnapshot> newDiff,
                                                 bool suppressTreeRebuild)
{
    if (!m_impl) {
        return;
    }
    if (m_commandBus) {
        QPointer<DopeSheetTimelineWidget> self(this);
        std::vector<SelectedKeyRef> curSel = m_impl->selectedKeys;
        auto applyFn = [self, suppressTreeRebuild](int32_t objId, int32_t cId,
                                                   const std::vector<FCurveKeySnapshot> &rem,
                                                   const std::vector<FCurveKeySnapshot> &ins,
                                                   const std::vector<SelectedKeyRef> &sel) {
            if (!self || !self->m_impl) {
                return;
            }
            self->m_impl->selectedKeys = sel;
            self->applyClipDiffPatch(objId, cId, rem, ins, suppressTreeRebuild, false);
            self->syncClipAndSelectionControls();
        };
        m_commandBus->execute(std::make_unique<TimelineKeyPatchCommand>(
            m_objectId, clipId, label, std::move(oldDiff), std::move(newDiff), curSel, curSel,
            std::move(applyFn)));
    } else {
        applyClipDiffPatch(m_objectId, clipId, oldDiff, newDiff, suppressTreeRebuild, false);
        syncClipAndSelectionControls();
    }
}

void DopeSheetTimelineWidget::syncClipsToDaemon(uint32_t priorityClipIdx, bool forceReplaceAll)
{
    if (!m_ipcClient || m_objectId < 0 || m_clips.empty()) {
        return;
    }

    auto effectiveBonesFor = [this](const TimelineAnimClip &clip) -> uint32_t {
        if (m_boneCount > 0u) {
            return m_boneCount;
        }
        int32_t maxBone = 0;
        for (const auto &ch : clip.channels) {
            maxBone = std::max(maxBone, ch.boneIndex);
        }
        return static_cast<uint32_t>(
            std::clamp(maxBone + 1, 1, static_cast<int32_t>(WE_MDL_MAX_BONES)));
    };

    std::vector<IpcClient::PuppetAnimClipData> allClipsData;
    allClipsData.reserve(m_clips.size());
    size_t totalKeys = 0u;

    for (uint32_t c = 0u; c < static_cast<uint32_t>(m_clips.size()); ++c) {
        IpcClient::PuppetAnimClipData cdata;
        size_t clipKeys = 0u;
        buildClipIpcData(*this, c, effectiveBonesFor(m_clips[c]), -1, cdata, clipKeys);
        totalKeys += clipKeys;
        allClipsData.push_back(std::move(cdata));
    }

    // Send all clips together with replace_all | sync_layers unless combined budget exceeds 16384.
    if (totalKeys <= WAYWAL_IPC_RIGGING_MAX_KEYFRAMES) {
        m_ipcClient->sendPuppetSetAnimTracks(static_cast<uint32_t>(m_objectId), 0x3u, allClipsData);
        return;
    }

    if (!forceReplaceAll && priorityClipIdx < allClipsData.size()) {
        size_t singleKeys = 0u;
        for (const auto &tr : allClipsData[priorityClipIdx].tracks) {
            singleKeys += tr.keyframes.size();
        }
        if (singleKeys <= WAYWAL_IPC_RIGGING_MAX_KEYFRAMES) {
            std::vector<IpcClient::PuppetAnimClipData> partialPayload;
            partialPayload.push_back(std::move(allClipsData[priorityClipIdx]));
            // flags = 0x2u updates only the edited clip and preserves sibling daemon clips/layers.
            m_ipcClient->sendPuppetSetAnimTracks(static_cast<uint32_t>(m_objectId), 0x2u,
                                                 partialPayload);
            return;
        }
    }

    size_t totalTracks = 0u;
    for (const auto &cd : allClipsData) {
        totalTracks += cd.tracks.size();
    }
    const int32_t perTrackBudget =
        (totalTracks > 0u)
            ? std::max<int32_t>(
                  1, static_cast<int32_t>(WAYWAL_IPC_RIGGING_MAX_KEYFRAMES / totalTracks))
            : 1;
    std::vector<IpcClient::PuppetAnimClipData> decimatedClips;
    decimatedClips.reserve(m_clips.size());
    for (uint32_t c = 0u; c < static_cast<uint32_t>(m_clips.size()); ++c) {
        IpcClient::PuppetAnimClipData cdata;
        size_t dummyKeys = 0u;
        buildClipIpcData(*this, c, effectiveBonesFor(m_clips[c]), perTrackBudget, cdata, dummyKeys);
        decimatedClips.push_back(std::move(cdata));
    }
    m_ipcClient->sendPuppetSetAnimTracks(static_cast<uint32_t>(m_objectId), 0x3u, decimatedClips);
}

void DopeSheetTimelineWidget::loadPuppetAnimations(const we_puppet_t *puppet, int32_t objectId)
{
    if (m_impl && m_objectId >= 0 && objectId != m_objectId) {
        saveActiveObjectState();
    }

    m_objectId = objectId;
    if (m_impl) {
        m_impl->boneNames.clear();
        m_impl->selectedKeys.clear();
        m_impl->invalidatePrepCache();
    }

    m_clips.clear();
    m_boneCount = 0;

    if (puppet) {
        m_boneCount = std::min<uint32_t>(puppet->bone_count, WE_MDL_MAX_BONES);
        if (m_impl && puppet->bones) {
            for (uint32_t b = 0; b < m_boneCount; ++b) {
                QString bn = QString::fromUtf8(puppet->bones[b].name);
                if (bn.isEmpty()) {
                    bn = QStringLiteral("Bone %1").arg(b);
                }
                m_impl->boneNames.push_back(bn);
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
                    const bool isAngle = (c >= 3u && c <= 5u);
                    std::vector<double> rawSamples(tr.kf_count);
                    std::vector<double> unwrapped(tr.kf_count);

                    double minV = 0.0;
                    double maxV = 0.0;
                    for (uint32_t f = 0u; f < tr.kf_count; ++f) {
                        const double raw = sampleAt(f);
                        rawSamples[f] = raw;
                        if (f == 0u) {
                            unwrapped[f] = raw;
                            minV = maxV = raw;
                        } else {
                            if (isAngle && std::abs(raw - rawSamples[f - 1u]) > kPi &&
                                std::abs(raw) <= (kPi + 1e-3) &&
                                std::abs(rawSamples[f - 1u]) <= (kPi + 1e-3)) {
                                unwrapped[f] = unwrapped[f - 1u] +
                                               std::remainder(raw - rawSamples[f - 1u], kTwoPi);
                            } else {
                                unwrapped[f] = unwrapped[f - 1u] + (raw - rawSamples[f - 1u]);
                            }
                            minV = std::min(minV, unwrapped[f]);
                            maxV = std::max(maxV, unwrapped[f]);
                        }
                    }

                    if ((maxV - minV) < 1e-6 && std::abs(minV - defVal) < 1e-6) {
                        continue;
                    }

                    TimelineFCurveChannel ch;
                    ch.boneIndex = tr.bone_index;
                    ch.channelIndex = c;
                    ch.label = defaultChannelLabel(c);
                    ch.visible = true;

                    // Global RDP sparsification preserves ramp-to-hold corners across all frames.
                    const std::vector<uint32_t> keepIndices = sparsifyRdpIndices(unwrapped, 1e-4);
                    ch.keys.reserve(keepIndices.size());
                    for (uint32_t idxF : keepIndices) {
                        FCurveKeySnapshot k;
                        k.animId = toWireAnimId(clip.id);
                        k.boneIndex = tr.bone_index;
                        k.channelIndex = c;
                        k.frame = static_cast<double>(idxF);
                        k.value = rawSamples[idxF];
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

    syncClipAndSelectionControls();
    rebuildTrackTree();
    saveActiveObjectState();
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

    updateTrackTreeValues();

    if (m_dopeCanvas) {
        m_dopeCanvas->update();
    }
    if (m_fcurveCanvas) {
        m_fcurveCanvas->update();
    }

    emit frameChanged(m_currentFrame, timeSec);
    if (m_ipcClient && m_objectId >= 0) {
        m_ipcClient->sendPuppetScrubTime(static_cast<uint32_t>(m_objectId),
                                         static_cast<float>(timeSec), !m_isPlaying, 0u);
    }
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
    const int before = (m_impl && m_impl->onionBeforeSpin) ? m_impl->onionBeforeSpin->value() : 2;
    const int after = (m_impl && m_impl->onionAfterSpin) ? m_impl->onionAfterSpin->value() : 2;
    const float opacity = (m_impl && m_impl->onionOpacitySpin)
                              ? static_cast<float>(m_impl->onionOpacitySpin->value())
                              : 0.30f;
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
    if (!m_impl || m_activeClipIndex >= m_clips.size()) {
        return;
    }

    const uint32_t clipIdx = m_activeClipIndex;
    const TimelineAnimClip &clip = m_clips[clipIdx];
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

    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> newDiff;

    auto appendUpsert = [&clip, &oldDiff, &newDiff, frame, targetBone](uint32_t chIdx) {
        double val = channelDefaultValue(chIdx);
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex == targetBone && ch.channelIndex == chIdx) {
                val = ch.keys.empty() ? channelDefaultValue(chIdx)
                                      : evaluateChannelAtFrame(ch, frame);
                for (const auto &existing : ch.keys) {
                    if (std::abs(existing.frame - frame) < 0.5) {
                        oldDiff.push_back(existing);
                        FCurveKeySnapshot nk = existing;
                        nk.value = val;
                        newDiff.push_back(nk);
                        return;
                    }
                }
                break;
            }
        }
        FCurveKeySnapshot nk;
        nk.animId = toWireAnimId(clip.id);
        nk.boneIndex = targetBone;
        nk.channelIndex = chIdx;
        nk.frame = frame;
        nk.value = val;
        nk.tangentMode = static_cast<uint8_t>(KeyTangentMode::Auto);
        newDiff.push_back(nk);
    };

    if (targetChannel >= 0 && targetChannel < 9) {
        appendUpsert(static_cast<uint32_t>(targetChannel));
        m_impl->selectOnly(targetBone, static_cast<uint32_t>(targetChannel), kInvalidKeyIndex,
                           frame);
    } else {
        static constexpr uint32_t kDefaultChans[3] = {0u, 1u, 5u};
        for (uint32_t c : kDefaultChans) {
            appendUpsert(c);
        }
        m_impl->selectOnly(targetBone, 5u, kInvalidKeyIndex, frame);
    }

    commitClipDiffEdit(clip.id, tr("Insert keyframe"), std::move(oldDiff), std::move(newDiff),
                       false);
    syncClipAndSelectionControls();
    emit statusMessage(tr("Inserted keyframe at frame %1").arg(static_cast<int>(frame)));
}

void DopeSheetTimelineWidget::onSetSelectedTangentMode(KeyTangentMode mode)
{
    if (!m_impl || m_activeClipIndex >= m_clips.size()) {
        return;
    }
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];

    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> newDiff;

    for (const auto &ch : clip.channels) {
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            const auto &k = ch.keys[ki];
            const bool matchSel =
                !m_impl->selectedKeys.empty()
                    ? m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame)
                    : (std::abs(k.frame - m_currentFrame) < 0.5);
            if (matchSel && k.tangentMode != static_cast<uint8_t>(mode)) {
                oldDiff.push_back(k);
                FCurveKeySnapshot nk = k;
                nk.tangentMode = static_cast<uint8_t>(mode);
                newDiff.push_back(nk);
            }
        }
    }

    if (oldDiff.empty()) {
        return;
    }

    commitClipDiffEdit(clip.id, tr("Change keyframe tangent mode"), std::move(oldDiff),
                       std::move(newDiff), true);
    syncClipAndSelectionControls();
}

void DopeSheetTimelineWidget::copySelectedKeyframes()
{
    if (!m_impl || m_activeClipIndex >= m_clips.size()) {
        return;
    }
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
    std::vector<FCurveKeySnapshot> gathered;
    double minFrame = std::numeric_limits<double>::infinity();

    for (const auto &ch : clip.channels) {
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            const auto &k = ch.keys[ki];
            const bool take = m_impl->selectedKeys.empty()
                                  ? (std::abs(k.frame - m_currentFrame) < 0.5)
                                  : m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame);
            if (take) {
                gathered.push_back(k);
                minFrame = std::min(minFrame, k.frame);
            }
        }
    }

    if (gathered.empty()) {
        emit statusMessage(tr("No keyframes selected to copy"));
        return;
    }

    m_impl->copiedKeys = std::move(gathered);
    m_impl->copiedAnchorFrame = minFrame;
    emit statusMessage(tr("Copied %1 keyframe(s)").arg(m_impl->copiedKeys.size()));
}

void DopeSheetTimelineWidget::pasteKeyframesAtPlayhead()
{
    if (!m_impl || m_activeClipIndex >= m_clips.size() || m_impl->copiedKeys.empty()) {
        return;
    }
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);
    const double targetAnchor = std::clamp(std::round(m_currentFrame), 0.0, maxF);
    const double delta = targetAnchor - m_impl->copiedAnchorFrame;

    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> newDiff;
    m_impl->selectedKeys.clear();

    for (FCurveKeySnapshot src : m_impl->copiedKeys) {
        src.animId = toWireAnimId(clip.id);
        src.frame = std::clamp(src.frame + delta, 0.0, maxF);
        for (const auto &ch : clip.channels) {
            if (ch.boneIndex != src.boneIndex || ch.channelIndex != src.channelIndex) {
                continue;
            }
            for (const auto &existing : ch.keys) {
                if (std::abs(existing.frame - src.frame) < 1e-4) {
                    oldDiff.push_back(existing);
                    break;
                }
            }
        }
        newDiff.push_back(src);
        m_impl->addSelectionUnique(src.boneIndex, src.channelIndex, kInvalidKeyIndex, src.frame);
    }

    if (!newDiff.empty()) {
        commitClipDiffEdit(clip.id, tr("Paste keyframes"), std::move(oldDiff), std::move(newDiff),
                           false);
        syncClipAndSelectionControls();
        emit statusMessage(tr("Pasted %1 keyframe(s) at frame %2")
                               .arg(m_impl->selectedKeys.size())
                               .arg(static_cast<int>(targetAnchor)));
    }
}

void DopeSheetTimelineWidget::duplicateSelectedKeyframes()
{
    if (!m_impl || m_activeClipIndex >= m_clips.size()) {
        return;
    }
    const TimelineAnimClip &clip = m_clips[m_activeClipIndex];
    const double maxF = static_cast<double>(std::max(1, clip.lengthFrames) - 1);

    std::vector<FCurveKeySnapshot> oldDiff;
    std::vector<FCurveKeySnapshot> newDiff;
    std::vector<SelectedKeyRef> nextSel;

    for (const auto &ch : clip.channels) {
        for (size_t ki = 0; ki < ch.keys.size(); ++ki) {
            const auto &k = ch.keys[ki];
            if (!m_impl->isSelected(ch.boneIndex, ch.channelIndex, ki, k.frame)) {
                continue;
            }
            FCurveKeySnapshot dup = k;
            dup.animId = toWireAnimId(clip.id);
            dup.frame = std::clamp(k.frame + 1.0, 0.0, maxF);
            for (const auto &existing : ch.keys) {
                if (std::abs(existing.frame - dup.frame) < 1e-4) {
                    oldDiff.push_back(existing);
                    break;
                }
            }
            newDiff.push_back(dup);
            nextSel.push_back({dup.boneIndex, dup.channelIndex, kInvalidKeyIndex, dup.frame});
        }
    }

    if (newDiff.empty()) {
        return;
    }

    m_impl->selectedKeys = std::move(nextSel);
    commitClipDiffEdit(clip.id, tr("Duplicate keyframes"), std::move(oldDiff), std::move(newDiff),
                       false);
    syncClipAndSelectionControls();
    emit statusMessage(tr("Duplicated %1 keyframe(s)").arg(m_impl->selectedKeys.size()));
}

void DopeSheetTimelineWidget::remapBoneIndices(int32_t objectId,
                                               const std::vector<int32_t> &oldToNew)
{
    if (!m_impl || objectId < 0 || oldToNew.empty()) {
        return;
    }

    if (objectId == m_objectId) {
        remapChannelAndKeyBones(m_clips, oldToNew);
        m_impl->selectedKeys.erase(
            std::remove_if(m_impl->selectedKeys.begin(), m_impl->selectedKeys.end(),
                           [&oldToNew](SelectedKeyRef &r) {
                               if (r.boneIndex < 0 ||
                                   static_cast<size_t>(r.boneIndex) >= oldToNew.size() ||
                                   oldToNew[static_cast<size_t>(r.boneIndex)] < 0) {
                                   return true;
                               }
                               r.boneIndex = oldToNew[static_cast<size_t>(r.boneIndex)];
                               r.keyIndex = kInvalidKeyIndex;
                               return false;
                           }),
            m_impl->selectedKeys.end());
        m_impl->invalidatePrepCache();
        rebuildTrackTree();
        syncClipAndSelectionControls();
        saveActiveObjectState();
        syncClipsToDaemon(m_activeClipIndex, true);
        if (m_dopeCanvas) {
            m_dopeCanvas->update();
        }
        if (m_fcurveCanvas) {
            m_fcurveCanvas->update();
        }
        return;
    }

    const auto it = m_impl->objectStates.find(objectId);
    if (it != m_impl->objectStates.end()) {
        remapChannelAndKeyBones(it->second.clips, oldToNew);
    }
}

void DopeSheetTimelineWidget::applyUndoKeySnapshot(uint32_t animId,
                                                   const std::vector<FCurveKeySnapshot> &keys)
{
    auto it = std::find_if(m_clips.begin(), m_clips.end(), [animId](const TimelineAnimClip &clip) {
        return toWireAnimId(clip.id) == animId;
    });
    if (it == m_clips.end()) {
        return;
    }

    const uint32_t targetIdx = static_cast<uint32_t>(std::distance(m_clips.begin(), it));
    TimelineAnimClip &clip = *it;

    std::vector<std::pair<int32_t, uint32_t>> oldChannelOrder;
    oldChannelOrder.reserve(clip.channels.size());
    for (const auto &ch : clip.channels) {
        oldChannelOrder.emplace_back(ch.boneIndex, ch.channelIndex);
        clip.channels[&ch - clip.channels.data()].keys.clear();
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

    std::vector<std::pair<int32_t, uint32_t>> newChannelOrder;
    newChannelOrder.reserve(clip.channels.size());
    for (const auto &ch : clip.channels) {
        newChannelOrder.emplace_back(ch.boneIndex, ch.channelIndex);
    }

    if (m_impl) {
        m_impl->invalidatePrepCache();
    }

    if (targetIdx == m_activeClipIndex) {
        if (oldChannelOrder == newChannelOrder) {
            updateTrackTreeValues();
        } else {
            rebuildTrackTree();
        }
    }

    saveActiveObjectState();

    if (!m_impl || !m_impl->dragPreviewSuppressIpc) {
        syncClipsToDaemon(targetIdx, false);
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
    syncClipsToDaemon(m_activeClipIndex, false);
}

void DopeSheetTimelineWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_impl && m_impl->eventFilter) {
        QWidget *win = window();
        if (win && win != this && m_impl->watchedWindow != win) {
            if (m_impl->watchedWindow) {
                m_impl->watchedWindow->removeEventFilter(m_impl->eventFilter);
            }
            win->installEventFilter(m_impl->eventFilter);
            m_impl->watchedWindow = win;
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
        if (m_impl && m_impl->eventFilter) {
            if (m_impl->watchedWindow) {
                m_impl->watchedWindow->removeEventFilter(m_impl->eventFilter);
                m_impl->watchedWindow.clear();
            }
            if (QWidget *win = window(); win && win != this) {
                win->installEventFilter(m_impl->eventFilter);
                m_impl->watchedWindow = win;
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
    const int mirrorDir = m_impl ? m_impl->mirrorDirection : 1;

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
            if (m_impl) {
                m_impl->mirrorDirection = -mirrorDir;
            }
        } else if (nextFrame <= 0.0) {
            nextFrame = std::min(maxFrame, -nextFrame);
            if (m_impl) {
                m_impl->mirrorDirection = -mirrorDir;
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
