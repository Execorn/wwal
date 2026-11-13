// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/particle/particle_designer_widget.hpp"
#include "studio/inspector/oklch_color_dialog.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace waywal::studio {

namespace {

constexpr QColor kBgCanvas(0x0b, 0x0c, 0x10);
constexpr QColor kBgElevated(0x18, 0x1b, 0x22);
constexpr QColor kBorderSubtle(0x2a, 0x2f, 0x3d);
constexpr QColor kTextPrimary(0xf1, 0xf5, 0xf9);
constexpr QColor kAccentSky(0x38, 0xbd, 0xf8);

constexpr int kCanvasPadX = 12;
constexpr int kCanvasPadY = 10;

QString fmtVec3(const float v[3])
{
    return QString::asprintf("%.4g %.4g %.4g", static_cast<double>(v[0]), static_cast<double>(v[1]),
                             static_cast<double>(v[2]));
}

void parseVec3(const QString &str, float out[3])
{
    const QStringList parts = str.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < 3; ++i) {
        out[i] = (i < parts.size()) ? parts[i].toFloat() : 0.0f;
    }
}

bool isControlPointAttractOp(const we_particle_operator_t &op) noexcept
{
    return op.type == WE_OP_VORTEX && std::abs(op.speed_inner) < 1e-4f &&
           std::abs(op.speed_outer) < 1e-4f;
}

QString emitterSummary(size_t index, const we_particle_emitter_t &em)
{
    const char *kind = "BoxRandom";
    if (em.type == WE_EMITTER_SPHERERANDOM) {
        kind = "SphereRandom";
    } else if (em.type == WE_EMITTER_POINT) {
        kind = "Point";
    }
    return QStringLiteral("#%1 %2 (rate %3/s, burst %4)")
        .arg(index)
        .arg(QLatin1String(kind))
        .arg(static_cast<double>(em.rate), 0, 'f', 1)
        .arg(em.instantaneous);
}

QString initializerSummary(size_t index, const we_particle_initializer_t &iz)
{
    static constexpr const char *kNames[] = {
        "LifetimeRandom", "SizeRandom",     "AlphaRandom",           "ColorRandom",
        "VelocityRandom", "RotationRandom", "AngularVelocityRandom", "TurbulentVelocityRandom",
    };
    const size_t t = static_cast<size_t>(iz.type);
    const char *name = (t < std::size(kNames)) ? kNames[t] : "Initializer";
    return QStringLiteral("#%1 %2").arg(index).arg(QLatin1String(name));
}

QString operatorSummary(size_t index, const we_particle_operator_t &op)
{
    if (isControlPointAttractOp(op)) {
        return QStringLiteral("#%1 ControlPointAttract (CP%2, r=%3..%4)")
            .arg(index)
            .arg(op.control_point)
            .arg(static_cast<double>(op.distance_inner), 0, 'f', 0)
            .arg(static_cast<double>(op.distance_outer), 0, 'f', 0);
    }
    static constexpr const char *kNames[] = {
        "Movement",       "AngularMovement", "AlphaFade",         "SizeChange", "ColorChange",
        "OscillateAlpha", "OscillateSize",   "OscillatePosition", "Vortex",     "Turbulence",
    };
    const size_t t = static_cast<size_t>(op.type);
    const char *name = (t < std::size(kNames)) ? kNames[t] : "Operator";
    return QStringLiteral("#%1 %2").arg(index).arg(QLatin1String(name));
}

template <typename Client = IpcClient>
void dispatchParticleConfigIpc(Client *client, const QString &output, int32_t objectId,
                               uint32_t configOp, uint32_t presetId, uint32_t flags,
                               const void *payload, uint32_t payloadLen)
{
    if (!client) {
        return;
    }

    if constexpr (requires(Client *c) {
                      c->sendParticleConfig(output, objectId, configOp, presetId, flags, payload,
                                            payloadLen);
                  }) {
        client->sendParticleConfig(output, objectId, configOp, presetId, flags, payload,
                                   payloadLen);
    } else if constexpr (requires(Client *c) {
                             c->requestParticleConfig(output, objectId, configOp, presetId, flags,
                                                      payload, payloadLen);
                         }) {
        client->requestParticleConfig(output, objectId, configOp, presetId, flags, payload,
                                      payloadLen);
    }
}

void computeHermiteTangents(const std::vector<we_particle_curve_knot_t> &knots,
                            std::vector<float> &outTangents)
{
    const size_t n = knots.size();
    if (n < 2 || n > 65536) {
        outTangents.assign(n < 2 ? n : 0, 0.0f);
        return;
    }

    outTangents.assign(n, 0.0f);
    const size_t segs = n - 1;
    std::vector<float> h(segs, 0.0f);
    std::vector<float> delta(segs, 0.0f);
    for (size_t i = 0; i < segs; ++i) {
        h[i] = std::max(1e-5f, knots[i + 1].t - knots[i].t);
        delta[i] = (knots[i + 1].y - knots[i].y) / h[i];
    }

    outTangents[0] = delta[0];
    outTangents[n - 1] = delta[n - 2];

    for (size_t i = 1; i + 1 < n; ++i) {
        if (delta[i - 1] * delta[i] <= 0.0f) {
            outTangents[i] = 0.0f;
        } else {
            outTangents[i] = 0.5f * (delta[i - 1] + delta[i]);
        }
    }

    /* Fritsch-Carlson overshoot clamp keeps spline monotone per interval. */
    for (size_t i = 0; i < n - 1; ++i) {
        if (std::abs(delta[i]) <= 1e-7f) {
            outTangents[i] = 0.0f;
            outTangents[i + 1] = 0.0f;
            continue;
        }
        const float a = outTangents[i] / delta[i];
        const float b = outTangents[i + 1] / delta[i];
        const float sq = a * a + b * b;
        if (sq > 9.0f) {
            const float tau = 3.0f / std::sqrt(sq);
            outTangents[i] = tau * a * delta[i];
            outTangents[i + 1] = tau * b * delta[i];
        }
    }
}

QDoubleSpinBox *makeSpin(double minV, double maxV, double step, int decimals, QWidget *parent)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minV, maxV);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setKeyboardTracking(false);
    return spin;
}

QSpinBox *makeIntSpin(int minV, int maxV, QWidget *parent)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(minV, maxV);
    spin->setKeyboardTracking(false);
    return spin;
}

bool parseCurveKnotsJson(const QJsonArray &arr, std::vector<we_particle_curve_knot_t> &out)
{
    if (arr.isEmpty()) {
        return false;
    }
    std::vector<we_particle_curve_knot_t> parsed;
    parsed.reserve(static_cast<size_t>(std::min<qsizetype>(arr.size(), WE_PARTICLE_MAX_KNOTS)));
    for (const QJsonValue &v : arr) {
        if (parsed.size() >= WE_PARTICLE_MAX_KNOTS) {
            break;
        }
        const QJsonObject ko = v.toObject();
        we_particle_curve_knot_t k{};
        const double tv = ko.value(QStringLiteral("t")).toDouble(0.0);
        const double yv = ko.value(QStringLiteral("y")).toDouble(0.0);
        k.t = std::clamp(std::isfinite(tv) ? static_cast<float>(tv) : 0.0f, 0.0f, 1.0f);
        k.y = std::isfinite(yv) ? static_cast<float>(yv) : 0.0f;
        const int m = ko.value(QStringLiteral("mode")).toInt(WE_KNOT_HANDLE_AUTO_SMOOTH);
        k.mode = static_cast<we_knot_handle_mode_t>(
            std::clamp(m, 0, static_cast<int>(WE_KNOT_HANDLE_STEP)));
        parsed.push_back(k);
    }
    if (parsed.empty()) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool parseColorStopsJson(const QJsonArray &arr, std::vector<we_particle_color_stop_t> &out)
{
    if (arr.isEmpty()) {
        return false;
    }
    std::vector<we_particle_color_stop_t> parsed;
    parsed.reserve(static_cast<size_t>(std::min<qsizetype>(arr.size(), WE_PARTICLE_MAX_KNOTS)));
    for (const QJsonValue &v : arr) {
        if (parsed.size() >= WE_PARTICLE_MAX_KNOTS) {
            break;
        }
        const QJsonObject so = v.toObject();
        we_particle_color_stop_t s{};
        const double tv = so.value(QStringLiteral("t")).toDouble(0.0);
        s.t = std::clamp(std::isfinite(tv) ? static_cast<float>(tv) : 0.0f, 0.0f, 1.0f);
        const QJsonArray rgb = so.value(QStringLiteral("rgb")).toArray();
        for (int c = 0; c < 3; ++c) {
            const double cv = (c < rgb.size()) ? rgb.at(c).toDouble(1.0) : 1.0;
            s.rgb[c] = std::clamp(std::isfinite(cv) ? static_cast<float>(cv) : 1.0f, 0.0f, 1.0f);
        }
        parsed.push_back(s);
    }
    if (parsed.empty()) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

void bakeAndSendRampLut(IpcClient *client, const QString &output, int32_t objId, uint32_t opIndex,
                        uint32_t channelKind,
                        const std::vector<we_particle_curve_knot_t> &alphaKnots,
                        const std::vector<we_particle_curve_knot_t> &sizeKnots,
                        const std::vector<we_particle_color_stop_t> &colorStops,
                        const ScalarCurveEditorWidget *alphaWidget,
                        const ScalarCurveEditorWidget *sizeWidget,
                        const OklabColorRampWidget *colorWidget)
{
    we_particle_ramp_lut_t lut{};
    lut.operator_index = opIndex;
    lut.channel_kind = channelKind;

    if (channelKind == 0u) {
        lut.knot_count = static_cast<uint32_t>(alphaKnots.size());
        if (alphaWidget) {
            alphaWidget->bakeLut(lut.scalar_lut);
        } else {
            ScalarCurveEditorWidget temp;
            temp.setValueRange(0.0f, 1.0f);
            temp.setKnots(alphaKnots);
            temp.bakeLut(lut.scalar_lut);
        }
    } else if (channelKind == 1u) {
        lut.knot_count = static_cast<uint32_t>(sizeKnots.size());
        if (sizeWidget) {
            sizeWidget->bakeLut(lut.scalar_lut);
        } else {
            ScalarCurveEditorWidget temp;
            temp.setValueRange(0.0f, 3.0f);
            temp.setKnots(sizeKnots);
            temp.bakeLut(lut.scalar_lut);
        }
    } else {
        lut.knot_count = static_cast<uint32_t>(colorStops.size());
        if (colorWidget) {
            colorWidget->bakeOklabRgbLut(lut.color_lut_r, lut.color_lut_g, lut.color_lut_b);
        } else {
            OklabColorRampWidget temp;
            temp.setStops(colorStops);
            temp.bakeOklabRgbLut(lut.color_lut_r, lut.color_lut_g, lut.color_lut_b);
        }
    }

    dispatchParticleConfigIpc(client, output, objId, WE_PARTICLE_CFG_SET_RAMP_LUT, 0u, 0u, &lut,
                              static_cast<uint32_t>(sizeof(lut)));
}

QString darkDockStyleSheet()
{
    return QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; font-size: 12px; }"
        "QGroupBox { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 6px; "
        "margin-top: 10px; padding: 8px; font-weight: 600; color: #f1f5f9; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #94a3b8; }"
        "QComboBox, QSpinBox, QDoubleSpinBox, QListWidget { background-color: #0b0c10; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; color: #f1f5f9; }"
        "QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QListWidget:focus, "
        "QPushButton:focus "
        "{ border: 2px solid #38bdf8; }"
        "QPushButton { background-color: #181b22; border: 1px solid #2a2f3d; border-radius: 4px; "
        "padding: 4px 10px; color: #f1f5f9; }"
        "QPushButton:hover { background-color: #222631; border-color: #38bdf8; }"
        "QProgressBar { background-color: #0b0c10; border: 1px solid #2a2f3d; border-radius: 4px; "
        "text-align: center; color: #f1f5f9; }"
        "QProgressBar::chunk { background-color: #38bdf8; border-radius: 3px; }");
}

} // namespace

/* ========================================================================= */
/* ScalarCurveEditorWidget                                                   */
/* ========================================================================= */

ScalarCurveEditorWidget::ScalarCurveEditorWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(110);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Scalar Curve Editor"));
    setAccessibleDescription(
        tr("Monotone cubic Hermite curve editor. Click to add knots, drag to adjust, "
           "right-click or press Delete to remove interior knots."));

    we_particle_curve_knot_t k0{};
    k0.t = 0.0f;
    k0.y = 1.0f;
    k0.mode = WE_KNOT_HANDLE_AUTO_SMOOTH;
    we_particle_curve_knot_t k1{};
    k1.t = 1.0f;
    k1.y = 0.0f;
    k1.mode = WE_KNOT_HANDLE_AUTO_SMOOTH;
    m_knots = {k0, k1};
    rebuildTangentCache();
}

void ScalarCurveEditorWidget::rebuildTangentCache()
{
    computeHermiteTangents(m_knots, m_cachedAutoTangents);
}

void ScalarCurveEditorWidget::setValueRange(float minVal, float maxVal)
{
    if (!std::isfinite(minVal)) {
        minVal = 0.0f;
    }
    if (!std::isfinite(maxVal) || maxVal <= minVal) {
        maxVal = minVal + 1.0f;
    }
    m_minVal = minVal;
    m_maxVal = maxVal;
    for (auto &k : m_knots) {
        k.y = std::clamp(k.y, m_minVal, m_maxVal);
    }
    rebuildTangentCache();
    update();
}

void ScalarCurveEditorWidget::setKnots(const std::vector<we_particle_curve_knot_t> &knots)
{
    std::vector<we_particle_curve_knot_t> cleaned = knots;
    if (cleaned.empty()) {
        we_particle_curve_knot_t a{};
        a.t = 0.0f;
        a.y = m_maxVal;
        a.mode = WE_KNOT_HANDLE_AUTO_SMOOTH;
        we_particle_curve_knot_t b{};
        b.t = 1.0f;
        b.y = m_minVal;
        b.mode = WE_KNOT_HANDLE_AUTO_SMOOTH;
        cleaned = {a, b};
    } else if (cleaned.size() == 1) {
        we_particle_curve_knot_t b = cleaned.front();
        cleaned.front().t = 0.0f;
        b.t = 1.0f;
        cleaned.push_back(b);
    }

    for (auto &k : cleaned) {
        k.t = std::clamp(std::isfinite(k.t) ? k.t : 0.0f, 0.0f, 1.0f);
        k.y = std::clamp(std::isfinite(k.y) ? k.y : m_minVal, m_minVal, m_maxVal);
    }
    std::sort(cleaned.begin(), cleaned.end(),
              [](const we_particle_curve_knot_t &a, const we_particle_curve_knot_t &b) {
                  return a.t < b.t;
              });
    if (cleaned.size() > WE_PARTICLE_MAX_KNOTS) {
        cleaned.resize(WE_PARTICLE_MAX_KNOTS);
    }
    cleaned.front().t = 0.0f;
    cleaned.back().t = 1.0f;
    for (size_t i = 1; i + 1 < cleaned.size(); ++i) {
        const float minT = cleaned[i - 1].t + 1e-4f;
        const float maxT = 1.0f - static_cast<float>(cleaned.size() - 1 - i) * 1e-4f;
        cleaned[i].t = std::clamp(cleaned[i].t, minT, std::max(minT, maxT));
    }

    m_knots = std::move(cleaned);
    rebuildTangentCache();
    if (m_selectedKnot >= static_cast<int>(m_knots.size())) {
        m_selectedKnot = static_cast<int>(m_knots.size()) - 1;
    }
    update();
}

float ScalarCurveEditorWidget::evaluateAt(float t) const noexcept
{
    if (m_knots.empty()) {
        return m_minVal;
    }
    if (m_knots.size() == 1) {
        return std::clamp(m_knots[0].y, m_minVal, m_maxVal);
    }

    t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
    if (t <= m_knots.front().t) {
        return std::clamp(m_knots.front().y, m_minVal, m_maxVal);
    }
    if (t >= m_knots.back().t) {
        return std::clamp(m_knots.back().y, m_minVal, m_maxVal);
    }

    size_t seg = 0;
    for (size_t i = 0; i + 1 < m_knots.size(); ++i) {
        if (t <= m_knots[i + 1].t) {
            seg = i;
            break;
        }
    }

    const auto &k0 = m_knots[seg];
    const auto &k1 = m_knots[seg + 1];
    const float h = k1.t - k0.t;
    if (h <= 1e-6f) {
        return std::clamp(k0.y, m_minVal, m_maxVal);
    }
    const float s = std::clamp((t - k0.t) / h, 0.0f, 1.0f);

    if (k0.mode == WE_KNOT_HANDLE_STEP) {
        return std::clamp(k0.y, m_minVal, m_maxVal);
    }
    if (k0.mode == WE_KNOT_HANDLE_LINEAR && k1.mode == WE_KNOT_HANDLE_LINEAR) {
        return std::clamp(k0.y + s * (k1.y - k0.y), m_minVal, m_maxVal);
    }

    float m0 = k0.out_tangent;
    float m1 = k1.in_tangent;
    if (seg + 1 < m_cachedAutoTangents.size()) {
        if (k0.mode == WE_KNOT_HANDLE_AUTO_SMOOTH) {
            m0 = m_cachedAutoTangents[seg];
        }
        if (k1.mode == WE_KNOT_HANDLE_AUTO_SMOOTH) {
            m1 = m_cachedAutoTangents[seg + 1];
        }
    }

    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    const float val = h00 * k0.y + h10 * h * m0 + h01 * k1.y + h11 * h * m1;
    return std::clamp(val, m_minVal, m_maxVal);
}

void ScalarCurveEditorWidget::bakeLut(float outLut[WE_PARTICLE_RAMP_SAMPLES]) const noexcept
{
    if (!outLut) {
        return;
    }
    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(WE_PARTICLE_RAMP_SAMPLES - 1u);
        outLut[k] = evaluateAt(u);
    }
}

void ScalarCurveEditorWidget::paintEvent(QPaintEvent *event)
{
    (void)event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fullRect = rect();
    p.fillRect(fullRect, kBgCanvas);
    p.setPen(QPen(hasFocus() ? kAccentSky : kBorderSubtle, hasFocus() ? 2.0 : 1.0));
    p.drawRoundedRect(fullRect.adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);

    const QRectF plot = fullRect.adjusted(kCanvasPadX, kCanvasPadY, -kCanvasPadX, -kCanvasPadY);
    if (plot.width() <= 2.0 || plot.height() <= 2.0) {
        return;
    }

    p.setPen(QPen(kBorderSubtle, 1.0, Qt::DotLine));
    for (int i = 0; i <= 4; ++i) {
        const qreal frac = static_cast<qreal>(i) / 4.0;
        const qreal gx = plot.left() + frac * plot.width();
        const qreal gy = plot.top() + frac * plot.height();
        p.drawLine(QPointF(gx, plot.top()), QPointF(gx, plot.bottom()));
        p.drawLine(QPointF(plot.left(), gy), QPointF(plot.right(), gy));
    }

    const float span = std::max(1e-5f, m_maxVal - m_minVal);
    auto toScreen = [&](float t, float y) -> QPointF {
        const qreal sx = plot.left() + static_cast<qreal>(std::clamp(t, 0.0f, 1.0f)) * plot.width();
        const qreal normY = static_cast<qreal>(std::clamp((y - m_minVal) / span, 0.0f, 1.0f));
        const qreal sy = plot.bottom() - normY * plot.height();
        return QPointF(sx, sy);
    };

    QPainterPath curvePath;
    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(WE_PARTICLE_RAMP_SAMPLES - 1u);
        const QPointF pt = toScreen(u, evaluateAt(u));
        if (k == 0) {
            curvePath.moveTo(pt);
        } else {
            curvePath.lineTo(pt);
        }
    }
    p.setPen(QPen(kAccentSky, 2.0));
    p.drawPath(curvePath);

    for (size_t i = 0; i < m_knots.size(); ++i) {
        const QPointF pt = toScreen(m_knots[i].t, m_knots[i].y);
        if (static_cast<int>(i) == m_selectedKnot) {
            /* Selected knot uses an 8px diamond with primary halo so state is shape-coded. */
            QPolygonF diamond;
            diamond << QPointF(pt.x(), pt.y() - 6.0) << QPointF(pt.x() + 6.0, pt.y())
                    << QPointF(pt.x(), pt.y() + 6.0) << QPointF(pt.x() - 6.0, pt.y());
            p.setPen(QPen(kTextPrimary, 1.5));
            p.setBrush(kAccentSky);
            p.drawPolygon(diamond);
        } else {
            p.setPen(QPen(kBgCanvas, 1.5));
            p.setBrush(kTextPrimary);
            p.drawEllipse(pt, 4.0, 4.0);
        }
    }
}

void ScalarCurveEditorWidget::mousePressEvent(QMouseEvent *event)
{
    const QRectF plot =
        QRectF(rect()).adjusted(kCanvasPadX, kCanvasPadY, -kCanvasPadX, -kCanvasPadY);
    if (plot.width() <= 2.0 || plot.height() <= 2.0) {
        return;
    }

    const float span = std::max(1e-5f, m_maxVal - m_minVal);
    auto toScreen = [&](const we_particle_curve_knot_t &k) -> QPointF {
        const qreal sx = plot.left() + static_cast<qreal>(k.t) * plot.width();
        const qreal normY = static_cast<qreal>((k.y - m_minVal) / span);
        const qreal sy = plot.bottom() - normY * plot.height();
        return QPointF(sx, sy);
    };

    const QPointF pos = event->position();
    int hitIdx = -1;
    qreal bestDist2 = 100.0;
    for (size_t i = 0; i < m_knots.size(); ++i) {
        const QPointF d = pos - toScreen(m_knots[i]);
        const qreal dist2 = d.x() * d.x() + d.y() * d.y();
        if (dist2 <= bestDist2) {
            bestDist2 = dist2;
            hitIdx = static_cast<int>(i);
        }
    }

    if (event->button() == Qt::RightButton) {
        if (hitIdx > 0 && hitIdx + 1 < static_cast<int>(m_knots.size()) && m_knots.size() > 2) {
            m_knots.erase(m_knots.begin() + hitIdx);
            rebuildTangentCache();
            m_selectedKnot = std::min(hitIdx, static_cast<int>(m_knots.size()) - 1);
            emit curveEdited(m_knots);
            update();
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (hitIdx >= 0) {
        m_selectedKnot = hitIdx;
        m_draggingKnot = true;
        update();
        return;
    }

    if (m_knots.size() < WE_PARTICLE_MAX_KNOTS) {
        const float t =
            std::clamp(static_cast<float>((pos.x() - plot.left()) / plot.width()), 0.01f, 0.99f);
        const float normY =
            std::clamp(static_cast<float>((plot.bottom() - pos.y()) / plot.height()), 0.0f, 1.0f);
        we_particle_curve_knot_t nk{};
        nk.t = t;
        nk.y = m_minVal + normY * span;
        nk.mode = WE_KNOT_HANDLE_AUTO_SMOOTH;
        const auto insIt = std::lower_bound(
            m_knots.begin(), m_knots.end(), nk.t,
            [](const we_particle_curve_knot_t &k, float key) { return k.t < key; });
        const auto insIdx = static_cast<int>(std::distance(m_knots.begin(), insIt));
        m_knots.insert(insIt, nk);
        rebuildTangentCache();
        m_selectedKnot = insIdx;
        m_draggingKnot = true;
        emit curveEdited(m_knots);
        update();
    }
}

void ScalarCurveEditorWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingKnot || m_selectedKnot < 0 ||
        m_selectedKnot >= static_cast<int>(m_knots.size())) {
        return;
    }
    const QRectF plot =
        QRectF(rect()).adjusted(kCanvasPadX, kCanvasPadY, -kCanvasPadX, -kCanvasPadY);
    if (plot.width() <= 2.0 || plot.height() <= 2.0) {
        return;
    }

    const QPointF pos = event->position();
    const float span = std::max(1e-5f, m_maxVal - m_minVal);
    const float normY =
        std::clamp(static_cast<float>((plot.bottom() - pos.y()) / plot.height()), 0.0f, 1.0f);
    m_knots[static_cast<size_t>(m_selectedKnot)].y = m_minVal + normY * span;

    if (m_selectedKnot > 0 && m_selectedKnot + 1 < static_cast<int>(m_knots.size())) {
        const float minT = m_knots[static_cast<size_t>(m_selectedKnot - 1)].t + 0.005f;
        const float maxT = m_knots[static_cast<size_t>(m_selectedKnot + 1)].t - 0.005f;
        const float rawT = static_cast<float>((pos.x() - plot.left()) / plot.width());
        m_knots[static_cast<size_t>(m_selectedKnot)].t =
            std::clamp(rawT, minT, std::max(minT, maxT));
    }
    rebuildTangentCache();
    emit curveEdited(m_knots);
    update();
}

void ScalarCurveEditorWidget::mouseReleaseEvent(QMouseEvent *event)
{
    (void)event;
    m_draggingKnot = false;
}

void ScalarCurveEditorWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_selectedKnot < 0 || m_selectedKnot >= static_cast<int>(m_knots.size())) {
        QWidget::keyPressEvent(event);
        return;
    }
    const size_t idx = static_cast<size_t>(m_selectedKnot);
    const float stepY = 0.02f * (m_maxVal - m_minVal);
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (idx > 0 && idx + 1 < m_knots.size() && m_knots.size() > 2) {
            m_knots.erase(m_knots.begin() + static_cast<ptrdiff_t>(idx));
            rebuildTangentCache();
            m_selectedKnot = std::min(m_selectedKnot, static_cast<int>(m_knots.size()) - 1);
            emit curveEdited(m_knots);
            update();
        }
        return;
    case Qt::Key_Up:
        m_knots[idx].y = std::clamp(m_knots[idx].y + stepY, m_minVal, m_maxVal);
        rebuildTangentCache();
        emit curveEdited(m_knots);
        update();
        return;
    case Qt::Key_Down:
        m_knots[idx].y = std::clamp(m_knots[idx].y - stepY, m_minVal, m_maxVal);
        rebuildTangentCache();
        emit curveEdited(m_knots);
        update();
        return;
    case Qt::Key_Left:
        if (idx > 0 && idx + 1 < m_knots.size()) {
            const float minT = m_knots[idx - 1].t + 0.005f;
            m_knots[idx].t = std::max(minT, m_knots[idx].t - 0.02f);
            rebuildTangentCache();
            emit curveEdited(m_knots);
            update();
        }
        return;
    case Qt::Key_Right:
        if (idx > 0 && idx + 1 < m_knots.size()) {
            const float maxT = m_knots[idx + 1].t - 0.005f;
            m_knots[idx].t = std::min(maxT, m_knots[idx].t + 0.02f);
            rebuildTangentCache();
            emit curveEdited(m_knots);
            update();
        }
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

/* ========================================================================= */
/* OklabColorRampWidget                                                      */
/* ========================================================================= */

OklabColorRampWidget::OklabColorRampWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(48);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("OKLab Color Ramp Editor"));
    setAccessibleDescription(
        tr("Click gradient bar to add a stop, drag to reposition, double-click to edit color, "
           "right-click interior stop to remove."));

    we_particle_color_stop_t s0{0.0f, {1.0f, 1.0f, 1.0f}};
    we_particle_color_stop_t s1{1.0f, {0.22f, 0.74f, 0.97f}};
    m_stops = {s0, s1};
}

void OklabColorRampWidget::setStops(const std::vector<we_particle_color_stop_t> &stops)
{
    std::vector<we_particle_color_stop_t> cleaned = stops;
    if (cleaned.empty()) {
        cleaned.push_back(we_particle_color_stop_t{0.0f, {1.0f, 1.0f, 1.0f}});
        cleaned.push_back(we_particle_color_stop_t{1.0f, {1.0f, 1.0f, 1.0f}});
    } else if (cleaned.size() == 1) {
        we_particle_color_stop_t s1 = cleaned.front();
        cleaned.front().t = 0.0f;
        s1.t = 1.0f;
        cleaned.push_back(s1);
    }

    for (auto &s : cleaned) {
        s.t = std::clamp(std::isfinite(s.t) ? s.t : 0.0f, 0.0f, 1.0f);
        for (float &c : s.rgb) {
            c = std::clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
        }
    }
    std::sort(cleaned.begin(), cleaned.end(),
              [](const we_particle_color_stop_t &a, const we_particle_color_stop_t &b) {
                  return a.t < b.t;
              });
    if (cleaned.size() > WE_PARTICLE_MAX_KNOTS) {
        cleaned.resize(WE_PARTICLE_MAX_KNOTS);
    }
    if (cleaned.front().t > 1e-4f) {
        if (cleaned.size() < WE_PARTICLE_MAX_KNOTS) {
            we_particle_color_stop_t lead = cleaned.front();
            lead.t = 0.0f;
            cleaned.insert(cleaned.begin(), lead);
        } else {
            cleaned.front().t = 0.0f;
        }
    } else {
        cleaned.front().t = 0.0f;
    }
    if (cleaned.back().t < 1.0f - 1e-4f) {
        if (cleaned.size() < WE_PARTICLE_MAX_KNOTS) {
            we_particle_color_stop_t tail = cleaned.back();
            tail.t = 1.0f;
            cleaned.push_back(tail);
        } else {
            cleaned.back().t = 1.0f;
        }
    } else {
        cleaned.back().t = 1.0f;
    }
    for (size_t i = 1; i + 1 < cleaned.size(); ++i) {
        const float minT = cleaned[i - 1].t + 1e-4f;
        const float maxT = 1.0f - static_cast<float>(cleaned.size() - 1 - i) * 1e-4f;
        cleaned[i].t = std::clamp(cleaned[i].t, minT, std::max(minT, maxT));
    }

    m_stops = std::move(cleaned);
    if (m_selectedStop >= static_cast<int>(m_stops.size())) {
        m_selectedStop = static_cast<int>(m_stops.size()) - 1;
    }
    update();
}

void OklabColorRampWidget::bakeOklabRgbLut(float outR[WE_PARTICLE_RAMP_SAMPLES],
                                           float outG[WE_PARTICLE_RAMP_SAMPLES],
                                           float outB[WE_PARTICLE_RAMP_SAMPLES]) const noexcept
{
    if (!outR || !outG || !outB) {
        return;
    }
    if (m_stops.empty()) {
        std::fill_n(outR, WE_PARTICLE_RAMP_SAMPLES, 1.0f);
        std::fill_n(outG, WE_PARTICLE_RAMP_SAMPLES, 1.0f);
        std::fill_n(outB, WE_PARTICLE_RAMP_SAMPLES, 1.0f);
        return;
    }

    struct LabStop {
        float t;
        float L;
        float a;
        float b;
        float rgb[3];
    };
    std::vector<LabStop> labStops(m_stops.size());
    for (size_t i = 0; i < m_stops.size(); ++i) {
        labStops[i].t = std::clamp(m_stops[i].t, 0.0f, 1.0f);
        labStops[i].rgb[0] = std::clamp(m_stops[i].rgb[0], 0.0f, 1.0f);
        labStops[i].rgb[1] = std::clamp(m_stops[i].rgb[1], 0.0f, 1.0f);
        labStops[i].rgb[2] = std::clamp(m_stops[i].rgb[2], 0.0f, 1.0f);
        const float rl = ColorScience::srgbToLinear(labStops[i].rgb[0]);
        const float gl = ColorScience::srgbToLinear(labStops[i].rgb[1]);
        const float bl = ColorScience::srgbToLinear(labStops[i].rgb[2]);
        ColorScience::linearRgbToOklab(rl, gl, bl, labStops[i].L, labStops[i].a, labStops[i].b);
    }

    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(WE_PARTICLE_RAMP_SAMPLES - 1u);
        if (u <= labStops.front().t) {
            outR[k] = labStops.front().rgb[0];
            outG[k] = labStops.front().rgb[1];
            outB[k] = labStops.front().rgb[2];
            continue;
        }
        if (u >= labStops.back().t) {
            outR[k] = labStops.back().rgb[0];
            outG[k] = labStops.back().rgb[1];
            outB[k] = labStops.back().rgb[2];
            continue;
        }

        size_t seg = 0;
        for (size_t i = 0; i + 1 < labStops.size(); ++i) {
            if (u <= labStops[i + 1].t) {
                seg = i;
                break;
            }
        }
        const float span = labStops[seg + 1].t - labStops[seg].t;
        const float alpha =
            (span > 1e-6f) ? std::clamp((u - labStops[seg].t) / span, 0.0f, 1.0f) : 0.0f;

        const float L = labStops[seg].L + alpha * (labStops[seg + 1].L - labStops[seg].L);
        const float a = labStops[seg].a + alpha * (labStops[seg + 1].a - labStops[seg].a);
        const float b = labStops[seg].b + alpha * (labStops[seg + 1].b - labStops[seg].b);

        float rl = 0.0f;
        float gl = 0.0f;
        float bl = 0.0f;
        ColorScience::oklabToLinearRgb(L, a, b, rl, gl, bl);
        outR[k] = std::clamp(ColorScience::linearToSrgb(rl), 0.0f, 1.0f);
        outG[k] = std::clamp(ColorScience::linearToSrgb(gl), 0.0f, 1.0f);
        outB[k] = std::clamp(ColorScience::linearToSrgb(bl), 0.0f, 1.0f);
    }
}

void OklabColorRampWidget::paintEvent(QPaintEvent *event)
{
    (void)event;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fullRect = rect();
    p.fillRect(fullRect, kBgElevated);

    const QRectF barRect = fullRect.adjusted(kCanvasPadX, 6.0, -kCanvasPadX, -18.0);
    if (barRect.width() <= 2.0 || barRect.height() <= 2.0) {
        return;
    }

    float rLut[WE_PARTICLE_RAMP_SAMPLES]{};
    float gLut[WE_PARTICLE_RAMP_SAMPLES]{};
    float bLut[WE_PARTICLE_RAMP_SAMPLES]{};
    bakeOklabRgbLut(rLut, gLut, bLut);

    QLinearGradient grad(barRect.topLeft(), barRect.topRight());
    for (uint32_t k = 0; k < WE_PARTICLE_RAMP_SAMPLES; ++k) {
        const qreal u = static_cast<qreal>(k) / static_cast<qreal>(WE_PARTICLE_RAMP_SAMPLES - 1u);
        grad.setColorAt(u, QColor::fromRgbF(rLut[k], gLut[k], bLut[k]));
    }
    p.setPen(QPen(kBorderSubtle, 1.0));
    p.setBrush(grad);
    p.drawRoundedRect(barRect, 3.0, 3.0);

    const qreal handleY = barRect.bottom() + 8.0;
    for (size_t i = 0; i < m_stops.size(); ++i) {
        const qreal hx = barRect.left() + static_cast<qreal>(m_stops[i].t) * barRect.width();
        const QColor stopCol =
            QColor::fromRgbF(m_stops[i].rgb[0], m_stops[i].rgb[1], m_stops[i].rgb[2]);
        if (static_cast<int>(i) == m_selectedStop) {
            QPolygonF diamond;
            diamond << QPointF(hx, handleY - 6.0) << QPointF(hx + 6.0, handleY)
                    << QPointF(hx, handleY + 6.0) << QPointF(hx - 6.0, handleY);
            p.setPen(QPen(kTextPrimary, 1.5));
            p.setBrush(kAccentSky);
            p.drawPolygon(diamond);
        } else {
            p.setPen(QPen(kTextPrimary, 1.0));
            p.setBrush(stopCol);
            p.drawEllipse(QPointF(hx, handleY), 4.5, 4.5);
        }
    }
}

void OklabColorRampWidget::mousePressEvent(QMouseEvent *event)
{
    const QRectF barRect = QRectF(rect()).adjusted(kCanvasPadX, 6.0, -kCanvasPadX, -18.0);
    if (barRect.width() <= 2.0) {
        return;
    }

    const QPointF pos = event->position();
    const qreal handleY = barRect.bottom() + 8.0;
    int hitIdx = -1;
    qreal bestDist = 11.0;
    if (std::abs(pos.y() - handleY) <= 10.0) {
        for (size_t i = 0; i < m_stops.size(); ++i) {
            const qreal hx = barRect.left() + static_cast<qreal>(m_stops[i].t) * barRect.width();
            const qreal dist = std::hypot(pos.x() - hx, pos.y() - handleY);
            if (dist <= bestDist) {
                bestDist = dist;
                hitIdx = static_cast<int>(i);
            }
        }
    }

    if (event->button() == Qt::RightButton) {
        if (hitIdx > 0 && hitIdx + 1 < static_cast<int>(m_stops.size()) && m_stops.size() > 2) {
            m_stops.erase(m_stops.begin() + hitIdx);
            m_selectedStop = std::min(hitIdx, static_cast<int>(m_stops.size()) - 1);
            emit rampEdited(m_stops);
            update();
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (hitIdx >= 0) {
        m_selectedStop = hitIdx;
        m_draggingStop = true;
        update();
        return;
    }

    if (m_stops.size() < WE_PARTICLE_MAX_KNOTS && pos.y() >= barRect.top() - 4.0 &&
        pos.y() <= barRect.bottom() + 4.0 && pos.x() >= barRect.left() - 2.0 &&
        pos.x() <= barRect.right() + 2.0) {
        const float t = std::clamp(static_cast<float>((pos.x() - barRect.left()) / barRect.width()),
                                   0.005f, 0.995f);
        float rLut[WE_PARTICLE_RAMP_SAMPLES]{};
        float gLut[WE_PARTICLE_RAMP_SAMPLES]{};
        float bLut[WE_PARTICLE_RAMP_SAMPLES]{};
        bakeOklabRgbLut(rLut, gLut, bLut);
        const size_t sampleIdx = static_cast<size_t>(
            std::clamp(static_cast<int>(std::lround(t * (WE_PARTICLE_RAMP_SAMPLES - 1))), 0,
                       static_cast<int>(WE_PARTICLE_RAMP_SAMPLES - 1)));

        we_particle_color_stop_t stop{t, {rLut[sampleIdx], gLut[sampleIdx], bLut[sampleIdx]}};
        const auto insIt = std::lower_bound(
            m_stops.begin(), m_stops.end(), stop.t,
            [](const we_particle_color_stop_t &s, float key) { return s.t < key; });
        const auto insIdx = static_cast<int>(std::distance(m_stops.begin(), insIt));
        m_stops.insert(insIt, stop);
        m_selectedStop = insIdx;
        m_draggingStop = true;
        emit rampEdited(m_stops);
        update();
    }
}

void OklabColorRampWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_draggingStop || m_selectedStop < 0 ||
        m_selectedStop >= static_cast<int>(m_stops.size())) {
        return;
    }
    const QRectF barRect = QRectF(rect()).adjusted(kCanvasPadX, 6.0, -kCanvasPadX, -18.0);
    if (barRect.width() <= 2.0) {
        return;
    }
    const float rawT = std::clamp(
        static_cast<float>((event->position().x() - barRect.left()) / barRect.width()), 0.0f, 1.0f);
    if (m_selectedStop > 0 && m_selectedStop + 1 < static_cast<int>(m_stops.size())) {
        const float minT = m_stops[static_cast<size_t>(m_selectedStop - 1)].t + 0.005f;
        const float maxT = m_stops[static_cast<size_t>(m_selectedStop + 1)].t - 0.005f;
        m_stops[static_cast<size_t>(m_selectedStop)].t =
            std::clamp(rawT, minT, std::max(minT, maxT));
        emit rampEdited(m_stops);
        update();
    }
}

void OklabColorRampWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QRectF barRect = QRectF(rect()).adjusted(kCanvasPadX, 6.0, -kCanvasPadX, -18.0);
    if (barRect.width() <= 2.0) {
        return;
    }
    const QPointF pos = event->position();
    const qreal handleY = barRect.bottom() + 8.0;
    if (pos.y() < barRect.top() - 4.0 || pos.y() > handleY + 10.0) {
        return;
    }
    int hitIdx = -1;
    qreal bestDist = 12.0;
    for (size_t i = 0; i < m_stops.size(); ++i) {
        const qreal hx = barRect.left() + static_cast<qreal>(m_stops[i].t) * barRect.width();
        const qreal dist = (pos.y() >= barRect.bottom())
                               ? std::hypot(pos.x() - hx, pos.y() - handleY)
                               : std::abs(pos.x() - hx);
        if (dist <= bestDist) {
            bestDist = dist;
            hitIdx = static_cast<int>(i);
        }
    }
    if (hitIdx >= 0) {
        m_selectedStop = hitIdx;
        const auto &st = m_stops[static_cast<size_t>(hitIdx)];
        emit stopColorPickerRequested(hitIdx, QColor::fromRgbF(st.rgb[0], st.rgb[1], st.rgb[2]));
    }
}

void OklabColorRampWidget::mouseReleaseEvent(QMouseEvent *event)
{
    (void)event;
    m_draggingStop = false;
}

/* ========================================================================= */
/* ParticleDesignerWidget                                                    */
/* ========================================================================= */

struct ParticleDesignerWidget::ExtraState {
    QString materialPath{QStringLiteral("util/particle_soft")};
    float trailLength{0.05f};
    float trailMinLength{1.0f};
    float trailMaxLength{100.0f};
    uint32_t subdivision{1};
    std::vector<we_particle_curve_knot_t> alphaKnots;
    std::vector<we_particle_curve_knot_t> sizeKnots;
    std::vector<we_particle_color_stop_t> colorStops;

    QWidget *emitterDetailCard{nullptr};
    QComboBox *emTypeCombo{nullptr};
    QDoubleSpinBox *emOrigX{nullptr};
    QDoubleSpinBox *emOrigY{nullptr};
    QDoubleSpinBox *emOrigZ{nullptr};
    QDoubleSpinBox *emDistMinX{nullptr};
    QDoubleSpinBox *emDistMinY{nullptr};
    QDoubleSpinBox *emDistMaxX{nullptr};
    QDoubleSpinBox *emDistMaxY{nullptr};
    QDoubleSpinBox *emRateSpin{nullptr};
    QSpinBox *emBurstSpin{nullptr};
    QDoubleSpinBox *emSpeedMinSpin{nullptr};
    QDoubleSpinBox *emSpeedMaxSpin{nullptr};
    QSpinBox *emCpSpin{nullptr};

    QWidget *initDetailCard{nullptr};
    QComboBox *initTypeCombo{nullptr};
    QDoubleSpinBox *initMin0{nullptr};
    QDoubleSpinBox *initMin1{nullptr};
    QDoubleSpinBox *initMin2{nullptr};
    QDoubleSpinBox *initMax0{nullptr};
    QDoubleSpinBox *initMax1{nullptr};
    QDoubleSpinBox *initMax2{nullptr};
    QDoubleSpinBox *initExpSpin{nullptr};
    QDoubleSpinBox *initScaleSpin{nullptr};

    QWidget *opDetailCard{nullptr};
    QComboBox *opTypeCombo{nullptr};
    QDoubleSpinBox *opVec0X{nullptr};
    QDoubleSpinBox *opVec0Y{nullptr};
    QDoubleSpinBox *opVec0Z{nullptr};
    QDoubleSpinBox *opVec1X{nullptr};
    QDoubleSpinBox *opVec1Y{nullptr};
    QDoubleSpinBox *opVec1Z{nullptr};
    QDoubleSpinBox *opScalar0{nullptr};
    QDoubleSpinBox *opScalar1{nullptr};
    QDoubleSpinBox *opScalar2{nullptr};
    QDoubleSpinBox *opScalar3{nullptr};
    QLabel *opFieldLabel0{nullptr};
    QLabel *opFieldLabel1{nullptr};
    QLabel *opFieldLabel2{nullptr};
    QLabel *opFieldLabel3{nullptr};
    QLabel *opVec0Label{nullptr};
    QLabel *opVec1Label{nullptr};
    QSpinBox *opCpSpin{nullptr};

    QComboBox *cpIndexCombo{nullptr};
    QCheckBox *cpLockPointerCheck{nullptr};
    QCheckBox *cpWorldSpaceCheck{nullptr};
    QDoubleSpinBox *cpOffsetX{nullptr};
    QDoubleSpinBox *cpOffsetY{nullptr};

    uint32_t pendingAlive{0};
    uint32_t pendingMax{WE_PARTICLE_DEFAULT_CAP};
    uint32_t pendingSimUs{0};
    bool telemetryDirty{false};
};

ParticleDesignerWidget::ParticleDesignerWidget(QWidget *parent)
    : QWidget(parent), m_extra(std::make_unique<ExtraState>())
{
    setAccessibleName(tr("Particle Designer"));
    setAccessibleDescription(tr("Visual particle system inspector with clean-room presets, curve "
                                "LUTs, and OKLab color ramps."));

    auto &ex = *m_extra;
    ex.alphaKnots = {
        {0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {0.15f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
    };
    ex.sizeKnots = {
        {0.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {1.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
    };
    ex.colorStops = {
        {0.0f, {1.0f, 1.0f, 1.0f}},
        {1.0f, {0.22f, 0.74f, 0.97f}},
    };
}

ParticleDesignerWidget::~ParticleDesignerWidget() = default;

const std::array<we_particle_preset_desc_t, WE_PARTICLE_PRESET_COUNT> &
ParticleDesignerWidget::builtinPresetCatalog() noexcept
{
    static const std::array<we_particle_preset_desc_t, WE_PARTICLE_PRESET_COUNT> kCatalog = {{
        {WE_PARTICLE_PRESET_RAIN,
         1600u,
         WE_RENDERER_SPRITETRAIL,
         0u,
         2.0f,
         {0.0f, -420.0f, 0.0f},
         "rain",
         "Rain"},
        {WE_PARTICLE_PRESET_SNOW,
         900u,
         WE_RENDERER_SPRITE,
         0u,
         8.0f,
         {12.0f, -18.0f, 0.0f},
         "snow",
         "Snow"},
        {WE_PARTICLE_PRESET_EMBERS,
         450u,
         WE_RENDERER_SPRITETRAIL,
         1u,
         4.0f,
         {0.0f, 85.0f, 0.0f},
         "embers",
         "Embers"},
        {WE_PARTICLE_PRESET_FIREFLIES,
         140u,
         WE_RENDERER_SPRITE,
         1u,
         5.0f,
         {0.0f, 0.0f, 0.0f},
         "fireflies",
         "Fireflies"},
        {WE_PARTICLE_PRESET_CHERRY_BLOSSOMS,
         320u,
         WE_RENDERER_SPRITE,
         0u,
         7.0f,
         {-28.0f, -12.0f, 0.0f},
         "cherry_blossoms",
         "Cherry Blossoms"},
        {WE_PARTICLE_PRESET_STARFIELD,
         600u,
         WE_RENDERER_SPRITE,
         1u,
         6.0f,
         {0.0f, 0.0f, 0.0f},
         "starfield",
         "Starfield"},
        {WE_PARTICLE_PRESET_RISING_SMOKE,
         180u,
         WE_RENDERER_SPRITE,
         0u,
         6.0f,
         {8.0f, 18.0f, 0.0f},
         "rising_smoke",
         "Rising Smoke"},
        {WE_PARTICLE_PRESET_MAGIC_DUST,
         800u,
         WE_RENDERER_SPRITE,
         1u,
         4.0f,
         {0.0f, 0.0f, 0.0f},
         "magic_dust",
         "Magic Dust"},
    }};
    return kCatalog;
}

void ParticleDesignerWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureUiInitialized();
}

void ParticleDesignerWidget::loadParticleSystem(int32_t objectId, const we_particle_sys_t *sys)
{
    m_objectId = objectId;
    if (!sys) {
        return;
    }

    m_maxCount = std::clamp(sys->max_count, 1u, static_cast<uint32_t>(WE_PARTICLE_MAX_CAP));
    m_startTime = std::clamp(sys->start_time, 0.0f, 30.0f);
    m_worldSpace = sys->world_space;
    m_perspective = sys->perspective;
    m_rendererType = sys->renderer_type;

    auto &ex = *m_extra;
    ex.materialPath = QString::fromUtf8(sys->material_path);
    if (ex.materialPath.isEmpty()) {
        ex.materialPath = QStringLiteral("util/particle_soft");
    }
    ex.trailLength = sys->trail_length;
    ex.trailMinLength = sys->trail_min_length;
    ex.trailMaxLength = sys->trail_max_length;
    ex.subdivision = sys->subdivision;
    ex.alphaKnots = {
        {0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {0.15f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
    };
    ex.sizeKnots = {
        {0.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
        {1.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
    };
    ex.colorStops = {
        {0.0f, {1.0f, 1.0f, 1.0f}},
        {1.0f, {0.22f, 0.74f, 0.97f}},
    };

    const uint32_t emCount =
        std::min(sys->emitter_count, static_cast<uint32_t>(WE_PARTICLE_MAX_EMITTERS));
    m_emitters.assign(sys->emitters, sys->emitters + emCount);

    const uint32_t initCount =
        std::min(sys->initializer_count, static_cast<uint32_t>(WE_PARTICLE_MAX_INITIALIZERS));
    m_initializers.assign(sys->initializers, sys->initializers + initCount);

    const uint32_t opCount =
        std::min(sys->operator_count, static_cast<uint32_t>(WE_PARTICLE_MAX_OPERATORS));
    m_operators.assign(sys->operators, sys->operators + opCount);

    const uint32_t cpCount =
        std::min(sys->control_point_count, static_cast<uint32_t>(WE_PARTICLE_MAX_CONTROL_PTS));
    m_controlPoints.assign(sys->control_points, sys->control_points + cpCount);

    for (const auto &op : m_operators) {
        if (op.type == WE_OP_ALPHA_FADE) {
            const float fin = std::clamp(op.fade_in_time, 0.0f, 0.99f);
            const float fout = std::clamp(op.fade_out_time, fin + 0.005f, 1.0f);
            ex.alphaKnots = {
                {0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                {fin, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                {fout, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
            };
        } else if (op.type == WE_OP_SIZE_CHANGE) {
            const float st = std::clamp(op.start_time, 0.0f, 0.99f);
            const float et = std::clamp(op.end_time, st + 0.005f, 1.0f);
            ex.sizeKnots = {
                {0.0f, std::clamp(op.start_val[0], 0.0f, 4.0f), 0.0f, 0.0f,
                 WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                {et, std::clamp(op.end_val[0], 0.0f, 4.0f), 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH,
                 0},
                {1.0f, std::clamp(op.end_val[0], 0.0f, 4.0f), 0.0f, 0.0f,
                 WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
            };
            if (st > 0.01f) {
                ex.sizeKnots.insert(ex.sizeKnots.begin() + 1,
                                    {st, std::clamp(op.start_val[0], 0.0f, 4.0f), 0.0f, 0.0f,
                                     WE_KNOT_HANDLE_AUTO_SMOOTH, 0});
            }
        } else if (op.type == WE_OP_COLOR_CHANGE) {
            ex.colorStops = {
                {std::clamp(op.start_time, 0.0f, 1.0f),
                 {std::clamp(op.start_val[0], 0.0f, 1.0f), std::clamp(op.start_val[1], 0.0f, 1.0f),
                  std::clamp(op.start_val[2], 0.0f, 1.0f)}},
                {std::clamp(op.end_time, 0.0f, 1.0f),
                 {std::clamp(op.end_val[0], 0.0f, 1.0f), std::clamp(op.end_val[1], 0.0f, 1.0f),
                  std::clamp(op.end_val[2], 0.0f, 1.0f)}},
            };
        }
    }

    m_lastCommittedJson = exportSystemJson();
    if (m_uiInitialized) {
        syncUiFromModel();
    }
}

void ParticleDesignerWidget::applyCleanRoomPreset(we_particle_preset_id_t presetId)
{
    const auto &catalog = builtinPresetCatalog();
    const uint32_t idx = std::min<uint32_t>(static_cast<uint32_t>(presetId),
                                            static_cast<uint32_t>(catalog.size() - 1));
    const auto &desc = catalog[idx];

    m_maxCount = desc.max_count;
    m_startTime = desc.start_time_sec;
    m_rendererType = static_cast<we_renderer_type_t>(desc.renderer_type);
    m_blending = desc.blending;
    m_worldSpace = false;
    m_perspective = false;
    m_emitters.clear();
    m_initializers.clear();
    m_operators.clear();
    m_controlPoints.clear();

    auto &ex = *m_extra;
    ex.trailLength = 0.05f;
    ex.trailMinLength = 1.0f;
    ex.trailMaxLength = 100.0f;
    ex.subdivision = 1u;

    auto addInit = [&](we_initializer_type_t type, float min0, float max0, float exp = 1.0f) {
        we_particle_initializer_t iz{};
        iz.type = type;
        iz.min_val[0] = min0;
        iz.max_val[0] = max0;
        iz.exponent = exp;
        m_initializers.push_back(iz);
    };

    switch (static_cast<we_particle_preset_id_t>(desc.preset_id)) {
    case WE_PARTICLE_PRESET_RAIN: {
        ex.materialPath = QStringLiteral("util/particle_rain");
        ex.trailLength = 0.045f;
        ex.trailMinLength = 2.0f;
        ex.trailMaxLength = 28.0f;

        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 1140.0f;
        em.distance_min[0] = -1100.0f;
        em.distance_min[1] = -40.0f;
        em.distance_max[0] = 1100.0f;
        em.distance_max[1] = 40.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 950.0f;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 1.0f, 1.4f);
        addInit(WE_INIT_SIZE_RANDOM, 2.0f, 4.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.35f, 0.75f);
        we_particle_initializer_t vel{};
        vel.type = WE_INIT_VELOCITY_RANDOM;
        vel.min_val[0] = -180.0f;
        vel.min_val[1] = -1350.0f;
        vel.max_val[0] = -110.0f;
        vel.max_val[1] = -1050.0f;
        vel.exponent = 1.0f;
        m_initializers.push_back(vel);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.gravity[1] = -420.0f;
        mov.drag = 0.05f;
        m_operators.push_back(mov);

        we_particle_operator_t fade{};
        fade.type = WE_OP_ALPHA_FADE;
        fade.fade_in_time = 0.05f;
        fade.fade_out_time = 0.90f;
        m_operators.push_back(fade);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.08f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.3f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.72f, 0.88f, 1.0f}}, {1.0f, {0.48f, 0.72f, 0.96f}}};
        break;
    }
    case WE_PARTICLE_PRESET_SNOW: {
        ex.materialPath = QStringLiteral("util/particle_soft");
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 1120.0f;
        em.distance_min[0] = -1020.0f;
        em.distance_min[1] = -30.0f;
        em.distance_max[0] = 1020.0f;
        em.distance_max[1] = 30.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 110.0f;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 7.0f, 12.0f);
        addInit(WE_INIT_SIZE_RANDOM, 4.0f, 14.0f, 2.2f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.55f, 0.95f);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.gravity[0] = 12.0f;
        mov.gravity[1] = -18.0f;
        mov.drag = 0.25f;
        m_operators.push_back(mov);

        we_particle_operator_t turb{};
        turb.type = WE_OP_TURBULENCE;
        turb.turb_scale = 0.008f;
        turb.turb_speed = 35.0f;
        turb.turb_timescale = 0.6f;
        turb.mask[0] = 1.0f;
        turb.mask[1] = 0.4f;
        m_operators.push_back(turb);

        we_particle_operator_t oscPos{};
        oscPos.type = WE_OP_OSCILLATE_POSITION;
        oscPos.freq_min = 0.6f;
        oscPos.freq_max = 1.4f;
        oscPos.scale_min = 12.0f;
        oscPos.scale_max = 28.0f;
        oscPos.mask[0] = 1.0f;
        oscPos.mask[1] = 0.1f;
        m_operators.push_back(oscPos);

        we_particle_operator_t fade{};
        fade.type = WE_OP_ALPHA_FADE;
        fade.fade_in_time = 0.12f;
        fade.fade_out_time = 0.82f;
        m_operators.push_back(fade);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.15f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.82f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.8f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.95f, 0.98f, 1.0f}}, {1.0f, {0.82f, 0.90f, 0.98f}}};
        break;
    }
    case WE_PARTICLE_PRESET_EMBERS: {
        ex.materialPath = QStringLiteral("util/particle_glow");
        ex.trailLength = 0.025f;
        ex.trailMinLength = 1.0f;
        ex.trailMaxLength = 6.0f;

        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 40.0f;
        em.distance_min[0] = -800.0f;
        em.distance_min[1] = -30.0f;
        em.distance_max[0] = 800.0f;
        em.distance_max[1] = 30.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 90.0f;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 2.5f, 5.0f);
        addInit(WE_INIT_SIZE_RANDOM, 3.0f, 9.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.7f, 1.0f);
        we_particle_initializer_t vel{};
        vel.type = WE_INIT_VELOCITY_RANDOM;
        vel.min_val[0] = -40.0f;
        vel.min_val[1] = 120.0f;
        vel.max_val[0] = 40.0f;
        vel.max_val[1] = 260.0f;
        vel.exponent = 1.0f;
        m_initializers.push_back(vel);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.gravity[1] = 85.0f;
        mov.drag = 0.15f;
        m_operators.push_back(mov);

        we_particle_operator_t col{};
        col.type = WE_OP_COLOR_CHANGE;
        col.start_time = 0.0f;
        col.end_time = 1.0f;
        col.start_val[0] = 1.0f;
        col.start_val[1] = 0.88f;
        col.start_val[2] = 0.30f;
        col.end_val[0] = 0.65f;
        col.end_val[1] = 0.12f;
        col.end_val[2] = 0.04f;
        m_operators.push_back(col);

        we_particle_operator_t oscA{};
        oscA.type = WE_OP_OSCILLATE_ALPHA;
        oscA.freq_min = 3.0f;
        oscA.freq_max = 7.0f;
        oscA.scale_min = 0.35f;
        oscA.scale_max = 1.0f;
        m_operators.push_back(oscA);

        we_particle_operator_t turb{};
        turb.type = WE_OP_TURBULENCE;
        turb.turb_scale = 0.01f;
        turb.turb_speed = 65.0f;
        turb.turb_timescale = 1.2f;
        turb.mask[0] = 1.0f;
        turb.mask[1] = 0.6f;
        m_operators.push_back(turb);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.1f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 0.25f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {1.0f, 0.88f, 0.30f}},
                         {0.45f, {0.98f, 0.45f, 0.09f}},
                         {1.0f, {0.60f, 0.10f, 0.06f}}};
        break;
    }
    case WE_PARTICLE_PRESET_FIREFLIES: {
        ex.materialPath = QStringLiteral("util/particle_glow");
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 460.0f;
        em.distance_min[0] = -860.0f;
        em.distance_min[1] = -360.0f;
        em.distance_max[0] = 860.0f;
        em.distance_max[1] = 360.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 22.0f;
        em.instantaneous = 25u;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 4.0f, 8.0f);
        addInit(WE_INIT_SIZE_RANDOM, 6.0f, 14.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.5f, 1.0f);
        we_particle_initializer_t tv{};
        tv.type = WE_INIT_TURBULENT_VELOCITY_RANDOM;
        tv.min_val[0] = 18.0f;
        tv.max_val[0] = 48.0f;
        tv.scale = 0.012f;
        tv.forward[0] = 1.0f;
        tv.forward[1] = 1.0f;
        tv.exponent = 1.0f;
        m_initializers.push_back(tv);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.drag = 0.3f;
        m_operators.push_back(mov);

        we_particle_operator_t oscA{};
        oscA.type = WE_OP_OSCILLATE_ALPHA;
        oscA.freq_min = 0.8f;
        oscA.freq_max = 2.4f;
        oscA.scale_min = 0.1f;
        oscA.scale_max = 1.0f;
        m_operators.push_back(oscA);

        we_particle_operator_t oscS{};
        oscS.type = WE_OP_OSCILLATE_SIZE;
        oscS.freq_min = 0.8f;
        oscS.freq_max = 2.4f;
        oscS.scale_min = 0.7f;
        oscS.scale_max = 1.35f;
        m_operators.push_back(oscS);

        we_particle_operator_t turb{};
        turb.type = WE_OP_TURBULENCE;
        turb.turb_scale = 0.009f;
        turb.turb_speed = 40.0f;
        turb.turb_timescale = 0.8f;
        turb.mask[0] = 1.0f;
        turb.mask[1] = 1.0f;
        m_operators.push_back(turb);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.25f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.75f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.7f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {0.5f, 1.1f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 0.6f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.85f, 1.0f, 0.42f}}, {1.0f, {0.42f, 0.92f, 0.56f}}};
        break;
    }
    case WE_PARTICLE_PRESET_CHERRY_BLOSSOMS: {
        ex.materialPath = QStringLiteral("util/particle_petal");
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 1080.0f;
        em.origin[1] = 1120.0f;
        em.distance_min[0] = -1000.0f;
        em.distance_min[1] = -60.0f;
        em.distance_max[0] = 1000.0f;
        em.distance_max[1] = 60.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 40.0f;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 6.0f, 10.0f);
        addInit(WE_INIT_SIZE_RANDOM, 12.0f, 22.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.75f, 0.98f);
        we_particle_initializer_t rot{};
        rot.type = WE_INIT_ROTATION_RANDOM;
        rot.max_val[2] = 6.2831853f;
        rot.exponent = 1.0f;
        m_initializers.push_back(rot);

        we_particle_initializer_t ang{};
        ang.type = WE_INIT_ANGULAR_VELOCITY_RANDOM;
        ang.min_val[2] = -2.4f;
        ang.max_val[2] = 2.4f;
        ang.exponent = 1.0f;
        m_initializers.push_back(ang);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.gravity[0] = -28.0f;
        mov.gravity[1] = -12.0f;
        mov.drag = 0.2f;
        m_operators.push_back(mov);

        we_particle_operator_t osc{};
        osc.type = WE_OP_OSCILLATE_POSITION;
        osc.freq_min = 0.7f;
        osc.freq_max = 1.6f;
        osc.scale_min = 16.0f;
        osc.scale_max = 36.0f;
        osc.mask[0] = 1.0f;
        osc.mask[1] = 0.5f;
        m_operators.push_back(osc);

        we_particle_operator_t fade{};
        fade.type = WE_OP_ALPHA_FADE;
        fade.fade_in_time = 0.08f;
        fade.fade_out_time = 0.85f;
        m_operators.push_back(fade);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.1f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.85f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.9f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.99f, 0.81f, 0.91f}}, {1.0f, {0.96f, 0.45f, 0.71f}}};
        break;
    }
    case WE_PARTICLE_PRESET_STARFIELD: {
        ex.materialPath = QStringLiteral("util/particle_glow");
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_SPHERERANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 540.0f;
        em.distance_min[0] = 18.0f;
        em.distance_max[0] = 180.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 0.5625f;
        em.directions[2] = 1.0f;
        em.speed_min = 45.0f;
        em.speed_max = 120.0f;
        em.rate = 110.0f;
        em.instantaneous = 80u;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 3.0f, 6.0f);
        addInit(WE_INIT_SIZE_RANDOM, 4.0f, 10.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.65f, 1.0f);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.drag = -0.35f;
        m_operators.push_back(mov);

        we_particle_operator_t sz{};
        sz.type = WE_OP_SIZE_CHANGE;
        sz.start_time = 0.0f;
        sz.end_time = 1.0f;
        sz.start_val[0] = 0.25f;
        sz.end_val[0] = 1.65f;
        m_operators.push_back(sz);

        we_particle_operator_t fade{};
        fade.type = WE_OP_ALPHA_FADE;
        fade.fade_in_time = 0.15f;
        fade.fade_out_time = 0.90f;
        m_operators.push_back(fade);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.2f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.25f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 1.65f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.75f, 0.90f, 1.0f}}, {1.0f, {1.0f, 0.96f, 0.88f}}};
        break;
    }
    case WE_PARTICLE_PRESET_RISING_SMOKE: {
        ex.materialPath = QStringLiteral("util/particle_smoke");
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 80.0f;
        em.distance_min[0] = -420.0f;
        em.distance_min[1] = -30.0f;
        em.distance_max[0] = 420.0f;
        em.distance_max[1] = 30.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 24.0f;
        em.control_point = -1;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 5.0f, 9.0f);
        addInit(WE_INIT_SIZE_RANDOM, 90.0f, 140.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.14f, 0.28f);

        we_particle_operator_t mov{};
        mov.type = WE_OP_MOVEMENT;
        mov.gravity[0] = 8.0f;
        mov.gravity[1] = 18.0f;
        mov.drag = 0.15f;
        m_operators.push_back(mov);

        we_particle_operator_t sz{};
        sz.type = WE_OP_SIZE_CHANGE;
        sz.start_time = 0.0f;
        sz.end_time = 1.0f;
        sz.start_val[0] = 0.65f;
        sz.end_val[0] = 2.60f;
        m_operators.push_back(sz);

        we_particle_operator_t fade{};
        fade.type = WE_OP_ALPHA_FADE;
        fade.fade_in_time = 0.18f;
        fade.fade_out_time = 0.72f;
        m_operators.push_back(fade);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.2f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.65f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 2.60f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.80f, 0.84f, 0.88f}}, {1.0f, {0.28f, 0.33f, 0.41f}}};
        break;
    }
    case WE_PARTICLE_PRESET_MAGIC_DUST:
    default: {
        ex.materialPath = QStringLiteral("util/particle_glow");
        we_particle_cp_t cp0{};
        cp0.id = 0;
        cp0.pos[0] = 960.0f;
        cp0.pos[1] = 540.0f;
        cp0.lock_to_pointer = true;
        m_controlPoints.push_back(cp0);

        we_particle_emitter_t em{};
        em.type = WE_EMITTER_SPHERERANDOM;
        em.origin[0] = 0.0f;
        em.origin[1] = 0.0f;
        em.distance_min[0] = 40.0f;
        em.distance_max[0] = 580.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 160.0f;
        em.control_point = 0;
        m_emitters.push_back(em);

        addInit(WE_INIT_LIFETIME_RANDOM, 2.5f, 4.5f);
        addInit(WE_INIT_SIZE_RANDOM, 4.0f, 12.0f);
        addInit(WE_INIT_ALPHA_RANDOM, 0.6f, 1.0f);

        we_particle_operator_t vortex{};
        vortex.type = WE_OP_VORTEX;
        vortex.control_point = 0;
        vortex.axis[2] = 1.0f;
        vortex.distance_inner = 50.0f;
        vortex.distance_outer = 420.0f;
        vortex.speed_inner = 180.0f;
        vortex.speed_outer = 60.0f;
        vortex.center_force = 20.0f;
        m_operators.push_back(vortex);

        we_particle_operator_t attract{};
        attract.type = WE_OP_VORTEX;
        attract.control_point = 0;
        attract.axis[2] = 1.0f;
        attract.distance_inner = 40.0f;
        attract.distance_outer = 580.0f;
        attract.speed_inner = 0.0f;
        attract.speed_outer = 0.0f;
        attract.center_force = 95.0f;
        m_operators.push_back(attract);

        we_particle_operator_t turb{};
        turb.type = WE_OP_TURBULENCE;
        turb.turb_scale = 0.01f;
        turb.turb_speed = 55.0f;
        turb.turb_timescale = 1.0f;
        turb.mask[0] = 1.0f;
        turb.mask[1] = 1.0f;
        m_operators.push_back(turb);

        we_particle_operator_t oscA{};
        oscA.type = WE_OP_OSCILLATE_ALPHA;
        oscA.freq_min = 2.0f;
        oscA.freq_max = 5.0f;
        oscA.scale_min = 0.25f;
        oscA.scale_max = 1.0f;
        m_operators.push_back(oscA);

        ex.alphaKnots = {{0.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {0.18f, 1.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                         {1.0f, 0.0f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.sizeKnots = {{0.0f, 0.4f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {0.5f, 1.15f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0},
                        {1.0f, 0.1f, 0.0f, 0.0f, WE_KNOT_HANDLE_AUTO_SMOOTH, 0}};
        ex.colorStops = {{0.0f, {0.22f, 0.74f, 0.97f}}, {1.0f, {0.75f, 0.52f, 0.99f}}};
        break;
    }
    }

    if (m_uiInitialized) {
        syncUiFromModel();
    }

    commitSystemEdit(QStringLiteral("preset"));
    emit controlPointOverlayUpdated(m_objectId);
    emit statusMessage(tr("Applied clean-room preset: %1").arg(QLatin1String(desc.display_name)));

    dispatchParticleConfigIpc(m_ipcClient, m_targetOutput, m_objectId, WE_PARTICLE_CFG_APPLY_PRESET,
                              desc.preset_id, m_simPaused ? 1u : 3u, nullptr, 0u);
    pushRampLutIpc(0u, 0u);
    pushRampLutIpc(0u, 1u);
    pushRampLutIpc(0u, 2u);
}

void ParticleDesignerWidget::triggerBurst()
{
    dispatchParticleConfigIpc(m_ipcClient, m_targetOutput, m_objectId,
                              WE_PARTICLE_CFG_TRIGGER_BURST, 0u, 0u, nullptr, 0u);
    emit statusMessage(tr("Triggered instantaneous particle burst"));
}

void ParticleDesignerWidget::updateTelemetry(uint32_t aliveCount, uint32_t maxCount,
                                             uint32_t simTimeUs)
{
    auto &ex = *m_extra;
    ex.pendingAlive = aliveCount;
    ex.pendingMax = std::max(1u, maxCount);
    ex.pendingSimUs = simTimeUs;
    ex.telemetryDirty = true;

    if (!m_uiInitialized || !m_aliveBar || !m_simTimeLabel) {
        return;
    }
    if (m_telemetryThrottleTimer && m_telemetryThrottleTimer->isActive()) {
        return;
    }
    m_aliveBar->setRange(0, static_cast<int>(ex.pendingMax));
    m_aliveBar->setValue(static_cast<int>(std::min(ex.pendingAlive, ex.pendingMax)));
    m_aliveBar->setFormat(QStringLiteral("%1 / %2 alive").arg(ex.pendingAlive).arg(ex.pendingMax));
    m_simTimeLabel->setText(
        QString::asprintf("%.2f ms/frame", static_cast<double>(ex.pendingSimUs) * 0.001));
    ex.telemetryDirty = false;
    if (m_telemetryThrottleTimer) {
        m_telemetryThrottleTimer->start();
    }
}

QString ParticleDesignerWidget::exportSystemJson() const
{
    const auto &ex = *m_extra;
    QJsonObject root;
    root.insert(QStringLiteral("maxcount"), static_cast<int>(m_maxCount));
    root.insert(QStringLiteral("starttime"), static_cast<double>(m_startTime));
    root.insert(QStringLiteral("material"), ex.materialPath);
    root.insert(QStringLiteral("blending"), static_cast<int>(m_blending));
    root.insert(QStringLiteral("flags"), (m_worldSpace ? 1 : 0) | (m_perspective ? 4 : 0));

    QJsonArray rendArr;
    QJsonObject rendObj;
    const char *rname = "sprite";
    if (m_rendererType == WE_RENDERER_SPRITETRAIL) {
        rname = "spritetrail";
    } else if (m_rendererType == WE_RENDERER_ROPETRAIL) {
        rname = "ropetrail";
    }
    rendObj.insert(QStringLiteral("name"), QLatin1String(rname));
    rendObj.insert(QStringLiteral("length"), static_cast<double>(ex.trailLength));
    rendObj.insert(QStringLiteral("minlength"), static_cast<double>(ex.trailMinLength));
    rendObj.insert(QStringLiteral("maxlength"), static_cast<double>(ex.trailMaxLength));
    rendObj.insert(QStringLiteral("subdivision"), static_cast<int>(ex.subdivision));
    rendArr.append(rendObj);
    root.insert(QStringLiteral("renderer"), rendArr);

    QJsonArray cpArr;
    for (const auto &cp : m_controlPoints) {
        QJsonObject cpObj;
        cpObj.insert(QStringLiteral("id"), cp.id);
        cpObj.insert(QStringLiteral("offset"), fmtVec3(cp.offset));
        cpObj.insert(QStringLiteral("flags"),
                     (cp.lock_to_pointer ? 1 : 0) | (cp.world_space ? 2 : 0));
        cpArr.append(cpObj);
    }
    root.insert(QStringLiteral("controlpoint"), cpArr);

    QJsonArray emArr;
    for (const auto &em : m_emitters) {
        QJsonObject emObj;
        const char *ename = "boxrandom";
        if (em.type == WE_EMITTER_SPHERERANDOM) {
            ename = "sphererandom";
        } else if (em.type == WE_EMITTER_POINT) {
            ename = "point";
        }
        emObj.insert(QStringLiteral("name"), QLatin1String(ename));
        emObj.insert(QStringLiteral("distancemin"), fmtVec3(em.distance_min));
        emObj.insert(QStringLiteral("distancemax"), fmtVec3(em.distance_max));
        emObj.insert(QStringLiteral("directions"), fmtVec3(em.directions));
        emObj.insert(QStringLiteral("origin"), fmtVec3(em.origin));
        emObj.insert(QStringLiteral("speedmin"), static_cast<double>(em.speed_min));
        emObj.insert(QStringLiteral("speedmax"), static_cast<double>(em.speed_max));
        emObj.insert(QStringLiteral("rate"), static_cast<double>(em.rate));
        emObj.insert(QStringLiteral("instantaneous"), static_cast<int>(em.instantaneous));
        emObj.insert(QStringLiteral("controlpoint"), em.control_point);
        emArr.append(emObj);
    }
    root.insert(QStringLiteral("emitter"), emArr);

    QJsonArray initArr;
    for (const auto &iz : m_initializers) {
        QJsonObject inObj;
        inObj.insert(QStringLiteral("exponent"), static_cast<double>(iz.exponent));
        switch (iz.type) {
        case WE_INIT_LIFETIME_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("lifetimerandom"));
            inObj.insert(QStringLiteral("min"), static_cast<double>(iz.min_val[0]));
            inObj.insert(QStringLiteral("max"), static_cast<double>(iz.max_val[0]));
            break;
        case WE_INIT_SIZE_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("sizerandom"));
            inObj.insert(QStringLiteral("min"), static_cast<double>(iz.min_val[0]));
            inObj.insert(QStringLiteral("max"), static_cast<double>(iz.max_val[0]));
            break;
        case WE_INIT_ALPHA_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("alpharandom"));
            inObj.insert(QStringLiteral("min"), static_cast<double>(iz.min_val[0]));
            inObj.insert(QStringLiteral("max"), static_cast<double>(iz.max_val[0]));
            break;
        case WE_INIT_COLOR_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("colorrandom"));
            inObj.insert(QStringLiteral("min"), fmtVec3(iz.min_val));
            inObj.insert(QStringLiteral("max"), fmtVec3(iz.max_val));
            break;
        case WE_INIT_VELOCITY_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("velocityrandom"));
            inObj.insert(QStringLiteral("min"), fmtVec3(iz.min_val));
            inObj.insert(QStringLiteral("max"), fmtVec3(iz.max_val));
            break;
        case WE_INIT_ROTATION_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("rotationrandom"));
            inObj.insert(QStringLiteral("min"), fmtVec3(iz.min_val));
            inObj.insert(QStringLiteral("max"), fmtVec3(iz.max_val));
            break;
        case WE_INIT_ANGULAR_VELOCITY_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("angularvelocityrandom"));
            inObj.insert(QStringLiteral("min"), fmtVec3(iz.min_val));
            inObj.insert(QStringLiteral("max"), fmtVec3(iz.max_val));
            break;
        case WE_INIT_TURBULENT_VELOCITY_RANDOM:
            inObj.insert(QStringLiteral("name"), QStringLiteral("turbulentvelocityrandom"));
            inObj.insert(QStringLiteral("speedmin"), static_cast<double>(iz.min_val[0]));
            inObj.insert(QStringLiteral("speedmax"), static_cast<double>(iz.max_val[0]));
            inObj.insert(QStringLiteral("scale"), static_cast<double>(iz.scale));
            inObj.insert(QStringLiteral("offset"), static_cast<double>(iz.offset));
            inObj.insert(QStringLiteral("forward"), fmtVec3(iz.forward));
            break;
        }
        initArr.append(inObj);
    }
    root.insert(QStringLiteral("initializer"), initArr);

    QJsonArray opArr;
    for (const auto &op : m_operators) {
        QJsonObject opObj;
        switch (op.type) {
        case WE_OP_MOVEMENT:
            opObj.insert(QStringLiteral("name"), QStringLiteral("movement"));
            opObj.insert(QStringLiteral("gravity"), fmtVec3(op.gravity));
            opObj.insert(QStringLiteral("drag"), static_cast<double>(op.drag));
            break;
        case WE_OP_ANGULAR_MOVEMENT:
            opObj.insert(QStringLiteral("name"), QStringLiteral("angularmovement"));
            opObj.insert(QStringLiteral("drag"), static_cast<double>(op.drag));
            opObj.insert(QStringLiteral("force"), fmtVec3(op.gravity));
            break;
        case WE_OP_ALPHA_FADE:
            opObj.insert(QStringLiteral("name"), QStringLiteral("alphafade"));
            opObj.insert(QStringLiteral("fadeintime"), static_cast<double>(op.fade_in_time));
            opObj.insert(QStringLiteral("fadeouttime"), static_cast<double>(op.fade_out_time));
            break;
        case WE_OP_SIZE_CHANGE:
            opObj.insert(QStringLiteral("name"), QStringLiteral("sizechange"));
            opObj.insert(QStringLiteral("starttime"), static_cast<double>(op.start_time));
            opObj.insert(QStringLiteral("endtime"), static_cast<double>(op.end_time));
            opObj.insert(QStringLiteral("startvalue"), static_cast<double>(op.start_val[0]));
            opObj.insert(QStringLiteral("endvalue"), static_cast<double>(op.end_val[0]));
            break;
        case WE_OP_COLOR_CHANGE:
            opObj.insert(QStringLiteral("name"), QStringLiteral("colorchange"));
            opObj.insert(QStringLiteral("starttime"), static_cast<double>(op.start_time));
            opObj.insert(QStringLiteral("endtime"), static_cast<double>(op.end_time));
            opObj.insert(QStringLiteral("startvalue"), fmtVec3(op.start_val));
            opObj.insert(QStringLiteral("endvalue"), fmtVec3(op.end_val));
            break;
        case WE_OP_OSCILLATE_ALPHA:
            opObj.insert(QStringLiteral("name"), QStringLiteral("oscillatealpha"));
            opObj.insert(QStringLiteral("frequencymin"), static_cast<double>(op.freq_min));
            opObj.insert(QStringLiteral("frequencymax"), static_cast<double>(op.freq_max));
            opObj.insert(QStringLiteral("scalemin"), static_cast<double>(op.scale_min));
            opObj.insert(QStringLiteral("scalemax"), static_cast<double>(op.scale_max));
            break;
        case WE_OP_OSCILLATE_SIZE:
            opObj.insert(QStringLiteral("name"), QStringLiteral("oscillatesize"));
            opObj.insert(QStringLiteral("frequencymin"), static_cast<double>(op.freq_min));
            opObj.insert(QStringLiteral("frequencymax"), static_cast<double>(op.freq_max));
            opObj.insert(QStringLiteral("scalemin"), static_cast<double>(op.scale_min));
            opObj.insert(QStringLiteral("scalemax"), static_cast<double>(op.scale_max));
            break;
        case WE_OP_OSCILLATE_POSITION:
            opObj.insert(QStringLiteral("name"), QStringLiteral("oscillateposition"));
            opObj.insert(QStringLiteral("frequencymin"), static_cast<double>(op.freq_min));
            opObj.insert(QStringLiteral("frequencymax"), static_cast<double>(op.freq_max));
            opObj.insert(QStringLiteral("scalemin"), static_cast<double>(op.scale_min));
            opObj.insert(QStringLiteral("scalemax"), static_cast<double>(op.scale_max));
            opObj.insert(QStringLiteral("mask"), fmtVec3(op.mask));
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
        case WE_OP_VORTEX:
            opObj.insert(QStringLiteral("name"), isControlPointAttractOp(op)
                                                     ? QStringLiteral("controlpointattract")
                                                     : QStringLiteral("vortex"));
            opObj.insert(QStringLiteral("controlpoint"), op.control_point);
            opObj.insert(QStringLiteral("axis"), fmtVec3(op.axis));
            opObj.insert(QStringLiteral("offset"), fmtVec3(op.offset));
            opObj.insert(QStringLiteral("distanceinner"), static_cast<double>(op.distance_inner));
            opObj.insert(QStringLiteral("distanceouter"), static_cast<double>(op.distance_outer));
            opObj.insert(QStringLiteral("speedinner"), static_cast<double>(op.speed_inner));
            opObj.insert(QStringLiteral("speedouter"), static_cast<double>(op.speed_outer));
            opObj.insert(QStringLiteral("centerforce"), static_cast<double>(op.center_force));
            break;
        case WE_OP_TURBULENCE:
            opObj.insert(QStringLiteral("name"), QStringLiteral("turbulence"));
            opObj.insert(QStringLiteral("scale"), static_cast<double>(op.turb_scale));
            opObj.insert(QStringLiteral("speed"), static_cast<double>(op.turb_speed));
            opObj.insert(QStringLiteral("timescale"), static_cast<double>(op.turb_timescale));
            opObj.insert(QStringLiteral("mask"), fmtVec3(op.mask));
            break;
        }
        opArr.append(opObj);
    }
    root.insert(QStringLiteral("operator"), opArr);

    auto serializeKnots = [](const std::vector<we_particle_curve_knot_t> &knots) {
        QJsonArray arr;
        for (const auto &k : knots) {
            QJsonObject ko;
            ko.insert(QStringLiteral("t"), static_cast<double>(k.t));
            ko.insert(QStringLiteral("y"), static_cast<double>(k.y));
            ko.insert(QStringLiteral("mode"), static_cast<int>(k.mode));
            arr.append(ko);
        }
        return arr;
    };
    root.insert(QStringLiteral("alphaknots"), serializeKnots(ex.alphaKnots));
    root.insert(QStringLiteral("sizeknots"), serializeKnots(ex.sizeKnots));

    QJsonArray stopsArr;
    for (const auto &s : ex.colorStops) {
        QJsonObject so;
        so.insert(QStringLiteral("t"), static_cast<double>(s.t));
        so.insert(QStringLiteral("rgb"),
                  QJsonArray{static_cast<double>(s.rgb[0]), static_cast<double>(s.rgb[1]),
                             static_cast<double>(s.rgb[2])});
        stopsArr.append(so);
    }
    root.insert(QStringLiteral("colorstops"), stopsArr);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void ParticleDesignerWidget::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;
    setStyleSheet(darkDockStyleSheet());

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(6, 6, 6, 6);
    outerLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *container = new QWidget(scrollArea);
    auto *root = new QVBoxLayout(container);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(8);

    /* Preset & Playback Bar */
    auto *presetGroup = new QGroupBox(tr("Clean-Room Presets & Telemetry"), container);
    auto *presetV = new QVBoxLayout(presetGroup);
    presetV->setSpacing(6);

    auto *presetRow = new QHBoxLayout();
    m_presetCombo = new QComboBox(presetGroup);
    m_presetCombo->setAccessibleName(tr("Particle Preset"));
    for (const auto &desc : builtinPresetCatalog()) {
        m_presetCombo->addItem(QString::fromUtf8(desc.display_name),
                               static_cast<uint32_t>(desc.preset_id));
    }
    m_applyPresetBtn = new QPushButton(tr("Apply"), presetGroup);
    m_applyPresetBtn->setAccessibleName(tr("Apply Particle Preset"));
    m_burstBtn = new QPushButton(tr("Burst"), presetGroup);
    m_burstBtn->setAccessibleName(tr("Trigger Particle Burst"));
    m_pauseBtn = new QPushButton(tr("Pause"), presetGroup);
    m_pauseBtn->setCheckable(true);
    m_pauseBtn->setAccessibleName(tr("Pause Simulation"));
    presetRow->addWidget(m_presetCombo, 1);
    presetRow->addWidget(m_applyPresetBtn);
    presetRow->addWidget(m_burstBtn);
    presetRow->addWidget(m_pauseBtn);
    presetV->addLayout(presetRow);

    auto *teleRow = new QHBoxLayout();
    m_aliveBar = new QProgressBar(presetGroup);
    m_aliveBar->setRange(0, static_cast<int>(m_maxCount));
    m_aliveBar->setValue(0);
    m_aliveBar->setFormat(tr("0 / %1 alive").arg(m_maxCount));
    m_simTimeLabel = new QLabel(QStringLiteral("0.00 ms/frame"), presetGroup);
    m_simTimeLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-family: monospace;"));
    teleRow->addWidget(m_aliveBar, 1);
    teleRow->addWidget(m_simTimeLabel);
    presetV->addLayout(teleRow);
    root->addWidget(presetGroup);

    /* System Properties */
    auto *sysGroup = new QGroupBox(tr("System & Renderer"), container);
    auto *sysForm = new QFormLayout(sysGroup);
    sysForm->setSpacing(4);
    m_maxCountSpin = makeIntSpin(1, static_cast<int>(WE_PARTICLE_MAX_CAP), sysGroup);
    m_maxCountSpin->setAccessibleName(tr("Max Particle Count"));

    m_startTimeSpin = makeSpin(0.0, 30.0, 0.25, 2, sysGroup);
    m_startTimeSpin->setSuffix(QStringLiteral(" s"));
    m_startTimeSpin->setAccessibleName(tr("Pre-warm Start Time"));

    m_rendererCombo = new QComboBox(sysGroup);
    m_rendererCombo->addItem(tr("Sprite"), static_cast<int>(WE_RENDERER_SPRITE));
    m_rendererCombo->addItem(tr("SpriteTrail"), static_cast<int>(WE_RENDERER_SPRITETRAIL));
    m_rendererCombo->addItem(tr("RopeTrail"), static_cast<int>(WE_RENDERER_ROPETRAIL));
    m_rendererCombo->setAccessibleName(tr("Renderer Type"));

    m_blendingCombo = new QComboBox(sysGroup);
    m_blendingCombo->addItem(tr("Normal"), 0);
    m_blendingCombo->addItem(tr("Additive"), 1);
    m_blendingCombo->addItem(tr("Multiply"), 2);
    m_blendingCombo->addItem(tr("Screen"), 3);
    m_blendingCombo->setAccessibleName(tr("Blending Mode"));

    m_worldSpaceCheck = new QCheckBox(tr("Simulate in World Space"), sysGroup);
    m_worldSpaceCheck->setAccessibleName(tr("World Space Simulation"));

    sysForm->addRow(tr("Max Count:"), m_maxCountSpin);
    sysForm->addRow(tr("Pre-warm:"), m_startTimeSpin);
    sysForm->addRow(tr("Renderer:"), m_rendererCombo);
    sysForm->addRow(tr("Blending:"), m_blendingCombo);
    sysForm->addRow(QString(), m_worldSpaceCheck);
    root->addWidget(sysGroup);

    /* Curves & OKLab Color Ramp */
    auto *curvesGroup =
        new QGroupBox(tr("Lifetime Curves & OKLab Ramp (65-Sample LUT)"), container);
    auto *curvesV = new QVBoxLayout(curvesGroup);
    curvesV->setSpacing(4);
    curvesV->addWidget(new QLabel(tr("Alpha over Lifetime [0..1]"), curvesGroup));
    m_alphaCurveEditor = new ScalarCurveEditorWidget(curvesGroup);
    m_alphaCurveEditor->setValueRange(0.0f, 1.0f);
    curvesV->addWidget(m_alphaCurveEditor);

    curvesV->addWidget(new QLabel(tr("Size Multiplier over Lifetime [0..3]"), curvesGroup));
    m_sizeCurveEditor = new ScalarCurveEditorWidget(curvesGroup);
    m_sizeCurveEditor->setValueRange(0.0f, 3.0f);
    curvesV->addWidget(m_sizeCurveEditor);

    curvesV->addWidget(
        new QLabel(tr("OKLab Color Ramp (Double-click stop for OklchColorDialog)"), curvesGroup));
    m_colorRampEditor = new OklabColorRampWidget(curvesGroup);
    curvesV->addWidget(m_colorRampEditor);
    root->addWidget(curvesGroup);

    auto &ex = *m_extra;

    /* Emitters */
    auto *emGroup = new QGroupBox(tr("Emitters"), container);
    auto *emV = new QVBoxLayout(emGroup);
    m_emitterList = new QListWidget(emGroup);
    m_emitterList->setMaximumHeight(76);
    emV->addWidget(m_emitterList);
    auto *emBtnRow = new QHBoxLayout();
    auto *addEmBtn = new QPushButton(tr("+ Emitter"), emGroup);
    auto *delEmBtn = new QPushButton(tr("- Remove"), emGroup);
    emBtnRow->addWidget(addEmBtn);
    emBtnRow->addWidget(delEmBtn);
    emBtnRow->addStretch(1);
    emV->addLayout(emBtnRow);

    ex.emitterDetailCard = new QWidget(emGroup);
    auto *emForm = new QFormLayout(ex.emitterDetailCard);
    emForm->setContentsMargins(0, 4, 0, 0);
    ex.emTypeCombo = new QComboBox(ex.emitterDetailCard);
    ex.emTypeCombo->addItem(tr("BoxRandom"), static_cast<int>(WE_EMITTER_BOXRANDOM));
    ex.emTypeCombo->addItem(tr("SphereRandom"), static_cast<int>(WE_EMITTER_SPHERERANDOM));
    ex.emTypeCombo->addItem(tr("Point"), static_cast<int>(WE_EMITTER_POINT));
    ex.emOrigX = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    ex.emOrigY = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    ex.emOrigZ = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    auto *emOrigRow = new QHBoxLayout();
    emOrigRow->addWidget(ex.emOrigX);
    emOrigRow->addWidget(ex.emOrigY);
    emOrigRow->addWidget(ex.emOrigZ);
    ex.emDistMinX = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    ex.emDistMinY = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    auto *emDMinRow = new QHBoxLayout();
    emDMinRow->addWidget(ex.emDistMinX);
    emDMinRow->addWidget(ex.emDistMinY);
    ex.emDistMaxX = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    ex.emDistMaxY = makeSpin(-8192, 8192, 5, 1, ex.emitterDetailCard);
    auto *emDMaxRow = new QHBoxLayout();
    emDMaxRow->addWidget(ex.emDistMaxX);
    emDMaxRow->addWidget(ex.emDistMaxY);
    ex.emRateSpin = makeSpin(0.0, 10000.0, 5.0, 1, ex.emitterDetailCard);
    ex.emBurstSpin = makeIntSpin(0, static_cast<int>(WE_PARTICLE_MAX_CAP), ex.emitterDetailCard);
    ex.emSpeedMinSpin = makeSpin(-4000, 4000, 5, 1, ex.emitterDetailCard);
    ex.emSpeedMaxSpin = makeSpin(-4000, 4000, 5, 1, ex.emitterDetailCard);
    ex.emCpSpin =
        makeIntSpin(-1, static_cast<int>(WE_PARTICLE_MAX_CONTROL_PTS) - 1, ex.emitterDetailCard);
    emForm->addRow(tr("Shape:"), ex.emTypeCombo);
    emForm->addRow(tr("Origin:"), emOrigRow);
    emForm->addRow(tr("Dist Min XY:"), emDMinRow);
    emForm->addRow(tr("Dist Max XY:"), emDMaxRow);
    emForm->addRow(tr("Rate (/s):"), ex.emRateSpin);
    emForm->addRow(tr("Burst:"), ex.emBurstSpin);
    emForm->addRow(tr("Speed Min:"), ex.emSpeedMinSpin);
    emForm->addRow(tr("Speed Max:"), ex.emSpeedMaxSpin);
    emForm->addRow(tr("Control Pt:"), ex.emCpSpin);
    emV->addWidget(ex.emitterDetailCard);
    root->addWidget(emGroup);

    /* Initializers */
    auto *initGroup = new QGroupBox(tr("Initializers (8 Types)"), container);
    auto *initV = new QVBoxLayout(initGroup);
    m_initializerList = new QListWidget(initGroup);
    m_initializerList->setMaximumHeight(76);
    initV->addWidget(m_initializerList);
    auto *initBtnRow = new QHBoxLayout();
    auto *addInitBtn = new QPushButton(tr("+ Initializer"), initGroup);
    auto *delInitBtn = new QPushButton(tr("- Remove"), initGroup);
    initBtnRow->addWidget(addInitBtn);
    initBtnRow->addWidget(delInitBtn);
    initBtnRow->addStretch(1);
    initV->addLayout(initBtnRow);

    ex.initDetailCard = new QWidget(initGroup);
    auto *initForm = new QFormLayout(ex.initDetailCard);
    initForm->setContentsMargins(0, 4, 0, 0);
    ex.initTypeCombo = new QComboBox(ex.initDetailCard);
    ex.initTypeCombo->addItem(tr("LifetimeRandom"), static_cast<int>(WE_INIT_LIFETIME_RANDOM));
    ex.initTypeCombo->addItem(tr("SizeRandom"), static_cast<int>(WE_INIT_SIZE_RANDOM));
    ex.initTypeCombo->addItem(tr("AlphaRandom"), static_cast<int>(WE_INIT_ALPHA_RANDOM));
    ex.initTypeCombo->addItem(tr("ColorRandom"), static_cast<int>(WE_INIT_COLOR_RANDOM));
    ex.initTypeCombo->addItem(tr("VelocityRandom"), static_cast<int>(WE_INIT_VELOCITY_RANDOM));
    ex.initTypeCombo->addItem(tr("RotationRandom"), static_cast<int>(WE_INIT_ROTATION_RANDOM));
    ex.initTypeCombo->addItem(tr("AngularVelocityRandom"),
                              static_cast<int>(WE_INIT_ANGULAR_VELOCITY_RANDOM));
    ex.initTypeCombo->addItem(tr("TurbulentVelocityRandom"),
                              static_cast<int>(WE_INIT_TURBULENT_VELOCITY_RANDOM));
    ex.initMin0 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    ex.initMin1 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    ex.initMin2 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    auto *inMinRow = new QHBoxLayout();
    inMinRow->addWidget(ex.initMin0);
    inMinRow->addWidget(ex.initMin1);
    inMinRow->addWidget(ex.initMin2);
    ex.initMax0 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    ex.initMax1 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    ex.initMax2 = makeSpin(-5000, 5000, 0.1, 3, ex.initDetailCard);
    auto *inMaxRow = new QHBoxLayout();
    inMaxRow->addWidget(ex.initMax0);
    inMaxRow->addWidget(ex.initMax1);
    inMaxRow->addWidget(ex.initMax2);
    ex.initExpSpin = makeSpin(0.1, 10.0, 0.1, 2, ex.initDetailCard);
    ex.initScaleSpin = makeSpin(0.0001, 100.0, 0.005, 4, ex.initDetailCard);
    initForm->addRow(tr("Type:"), ex.initTypeCombo);
    initForm->addRow(tr("Min:"), inMinRow);
    initForm->addRow(tr("Max:"), inMaxRow);
    initForm->addRow(tr("Exponent:"), ex.initExpSpin);
    initForm->addRow(tr("Scale:"), ex.initScaleSpin);
    initV->addWidget(ex.initDetailCard);
    root->addWidget(initGroup);

    /* Operators (including ControlPointAttract) */
    auto *opGroup = new QGroupBox(tr("Operators & Forces"), container);
    auto *opV = new QVBoxLayout(opGroup);
    m_operatorList = new QListWidget(opGroup);
    m_operatorList->setMaximumHeight(84);
    opV->addWidget(m_operatorList);
    auto *opBtnRow = new QHBoxLayout();
    auto *addOpBtn = new QPushButton(tr("+ Operator"), opGroup);
    auto *delOpBtn = new QPushButton(tr("- Remove"), opGroup);
    opBtnRow->addWidget(addOpBtn);
    opBtnRow->addWidget(delOpBtn);
    opBtnRow->addStretch(1);
    opV->addLayout(opBtnRow);

    ex.opDetailCard = new QWidget(opGroup);
    auto *opForm = new QFormLayout(ex.opDetailCard);
    opForm->setContentsMargins(0, 4, 0, 0);
    ex.opTypeCombo = new QComboBox(ex.opDetailCard);
    ex.opTypeCombo->addItem(tr("Movement"), static_cast<int>(WE_OP_MOVEMENT));
    ex.opTypeCombo->addItem(tr("AngularMovement"), static_cast<int>(WE_OP_ANGULAR_MOVEMENT));
    ex.opTypeCombo->addItem(tr("AlphaFade"), static_cast<int>(WE_OP_ALPHA_FADE));
    ex.opTypeCombo->addItem(tr("SizeChange"), static_cast<int>(WE_OP_SIZE_CHANGE));
    ex.opTypeCombo->addItem(tr("ColorChange"), static_cast<int>(WE_OP_COLOR_CHANGE));
    ex.opTypeCombo->addItem(tr("OscillateAlpha"), static_cast<int>(WE_OP_OSCILLATE_ALPHA));
    ex.opTypeCombo->addItem(tr("OscillateSize"), static_cast<int>(WE_OP_OSCILLATE_SIZE));
    ex.opTypeCombo->addItem(tr("OscillatePosition"), static_cast<int>(WE_OP_OSCILLATE_POSITION));
    ex.opTypeCombo->addItem(tr("Vortex"), static_cast<int>(WE_OP_VORTEX));
    ex.opTypeCombo->addItem(tr("Turbulence"), static_cast<int>(WE_OP_TURBULENCE));
    ex.opTypeCombo->addItem(tr("ControlPointAttract"), 100);

    ex.opVec0Label = new QLabel(tr("Vec A:"), ex.opDetailCard);
    ex.opVec0X = makeSpin(-5000, 5000, 1.0, 2, ex.opDetailCard);
    ex.opVec0Y = makeSpin(-5000, 5000, 1.0, 2, ex.opDetailCard);
    ex.opVec0Z = makeSpin(-5000, 5000, 1.0, 2, ex.opDetailCard);
    auto *opV0Row = new QHBoxLayout();
    opV0Row->addWidget(ex.opVec0X);
    opV0Row->addWidget(ex.opVec0Y);
    opV0Row->addWidget(ex.opVec0Z);

    ex.opVec1Label = new QLabel(tr("Vec B:"), ex.opDetailCard);
    ex.opVec1X = makeSpin(-5000, 5000, 0.1, 2, ex.opDetailCard);
    ex.opVec1Y = makeSpin(-5000, 5000, 0.1, 2, ex.opDetailCard);
    ex.opVec1Z = makeSpin(-5000, 5000, 0.1, 2, ex.opDetailCard);
    auto *opV1Row = new QHBoxLayout();
    opV1Row->addWidget(ex.opVec1X);
    opV1Row->addWidget(ex.opVec1Y);
    opV1Row->addWidget(ex.opVec1Z);

    ex.opFieldLabel0 = new QLabel(tr("Param 0:"), ex.opDetailCard);
    ex.opScalar0 = makeSpin(-5000, 5000, 0.05, 3, ex.opDetailCard);
    ex.opFieldLabel1 = new QLabel(tr("Param 1:"), ex.opDetailCard);
    ex.opScalar1 = makeSpin(-5000, 5000, 0.05, 3, ex.opDetailCard);
    ex.opFieldLabel2 = new QLabel(tr("Param 2:"), ex.opDetailCard);
    ex.opScalar2 = makeSpin(-5000, 5000, 0.05, 3, ex.opDetailCard);
    ex.opFieldLabel3 = new QLabel(tr("Param 3:"), ex.opDetailCard);
    ex.opScalar3 = makeSpin(-5000, 5000, 0.05, 3, ex.opDetailCard);
    ex.opCpSpin =
        makeIntSpin(0, static_cast<int>(WE_PARTICLE_MAX_CONTROL_PTS) - 1, ex.opDetailCard);

    opForm->addRow(tr("Operator:"), ex.opTypeCombo);
    opForm->addRow(ex.opVec0Label, opV0Row);
    opForm->addRow(ex.opVec1Label, opV1Row);
    opForm->addRow(ex.opFieldLabel0, ex.opScalar0);
    opForm->addRow(ex.opFieldLabel1, ex.opScalar1);
    opForm->addRow(ex.opFieldLabel2, ex.opScalar2);
    opForm->addRow(ex.opFieldLabel3, ex.opScalar3);
    opForm->addRow(tr("Control Pt:"), ex.opCpSpin);
    opV->addWidget(ex.opDetailCard);
    root->addWidget(opGroup);

    /* Control Points */
    auto *cpGroup = new QGroupBox(tr("Control Points (0..7)"), container);
    auto *cpForm = new QFormLayout(cpGroup);
    ex.cpIndexCombo = new QComboBox(cpGroup);
    for (uint32_t c = 0; c < WE_PARTICLE_MAX_CONTROL_PTS; ++c) {
        ex.cpIndexCombo->addItem(QStringLiteral("CP %1").arg(c), static_cast<int>(c));
    }
    ex.cpLockPointerCheck = new QCheckBox(tr("Lock to Pointer"), cpGroup);
    ex.cpWorldSpaceCheck = new QCheckBox(tr("World Space"), cpGroup);
    ex.cpOffsetX = makeSpin(-8192, 8192, 5, 1, cpGroup);
    ex.cpOffsetY = makeSpin(-8192, 8192, 5, 1, cpGroup);
    auto *cpOffRow = new QHBoxLayout();
    cpOffRow->addWidget(ex.cpOffsetX);
    cpOffRow->addWidget(ex.cpOffsetY);
    cpForm->addRow(tr("Slot:"), ex.cpIndexCombo);
    cpForm->addRow(tr("Offset XY:"), cpOffRow);
    cpForm->addRow(QString(), ex.cpLockPointerCheck);
    cpForm->addRow(QString(), ex.cpWorldSpaceCheck);
    root->addWidget(cpGroup);

    root->addStretch(1);
    scrollArea->setWidget(container);
    outerLayout->addWidget(scrollArea);

    m_telemetryThrottleTimer = new QTimer(this);
    m_telemetryThrottleTimer->setSingleShot(true);
    m_telemetryThrottleTimer->setInterval(100);
    connect(m_telemetryThrottleTimer, &QTimer::timeout, this, [this]() {
        auto &state = *m_extra;
        if (state.telemetryDirty) {
            updateTelemetry(state.pendingAlive, state.pendingMax, state.pendingSimUs);
        }
    });

    /* Signal Wiring */
    connect(m_applyPresetBtn, &QPushButton::clicked, this, [this]() {
        const auto pid =
            static_cast<we_particle_preset_id_t>(m_presetCombo->currentData().toUInt());
        applyCleanRoomPreset(pid);
    });
    connect(m_burstBtn, &QPushButton::clicked, this, &ParticleDesignerWidget::triggerBurst);
    connect(m_pauseBtn, &QPushButton::toggled, this, [this](bool paused) {
        m_simPaused = paused;
        m_pauseBtn->setText(paused ? tr("Resume") : tr("Pause"));
        dispatchParticleConfigIpc(m_ipcClient, m_targetOutput, m_objectId,
                                  WE_PARTICLE_CFG_PAUSE_TOGGLE, 0u, paused ? 1u : 0u, nullptr, 0u);
    });

    auto onSystemScalarChanged = [this]() {
        if (m_updatingUi) {
            return;
        }
        m_maxCount = static_cast<uint32_t>(m_maxCountSpin->value());
        m_startTime = static_cast<float>(m_startTimeSpin->value());
        m_rendererType = static_cast<we_renderer_type_t>(m_rendererCombo->currentData().toInt());
        m_blending = m_blendingCombo->currentData().toUInt();
        m_worldSpace = m_worldSpaceCheck->isChecked();
        commitSystemEdit(QStringLiteral("system"));
    };
    connect(m_maxCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            onSystemScalarChanged);
    connect(m_startTimeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onSystemScalarChanged);
    connect(m_rendererCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onSystemScalarChanged);
    connect(m_blendingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onSystemScalarChanged);
    connect(m_worldSpaceCheck, &QCheckBox::toggled, this, onSystemScalarChanged);

    connect(m_alphaCurveEditor, &ScalarCurveEditorWidget::curveEdited, this,
            [this](const std::vector<we_particle_curve_knot_t> &knots) {
                if (m_updatingUi) {
                    return;
                }
                m_extra->alphaKnots = knots;
                pushRampLutIpc(0u, 0u);
                commitSystemEdit(QStringLiteral("alpha_curve"));
            });
    connect(m_sizeCurveEditor, &ScalarCurveEditorWidget::curveEdited, this,
            [this](const std::vector<we_particle_curve_knot_t> &knots) {
                if (m_updatingUi) {
                    return;
                }
                m_extra->sizeKnots = knots;
                pushRampLutIpc(0u, 1u);
                commitSystemEdit(QStringLiteral("size_curve"));
            });
    connect(m_colorRampEditor, &OklabColorRampWidget::rampEdited, this,
            [this](const std::vector<we_particle_color_stop_t> &stops) {
                if (m_updatingUi) {
                    return;
                }
                m_extra->colorStops = stops;
                pushRampLutIpc(0u, 2u);
                commitSystemEdit(QStringLiteral("color_ramp"));
            });
    connect(
        m_colorRampEditor, &OklabColorRampWidget::stopColorPickerRequested, this,
        [this](int stopIndex, const QColor &initialColor) {
            const QColor chosen =
                OklchColorDialog::getColor(initialColor, this, tr("Edit Ramp Stop Color"));
            if (!chosen.isValid()) {
                return;
            }
            auto stops = m_colorRampEditor->stops();
            if (stopIndex >= 0 && stopIndex < static_cast<int>(stops.size())) {
                stops[static_cast<size_t>(stopIndex)].rgb[0] = static_cast<float>(chosen.redF());
                stops[static_cast<size_t>(stopIndex)].rgb[1] = static_cast<float>(chosen.greenF());
                stops[static_cast<size_t>(stopIndex)].rgb[2] = static_cast<float>(chosen.blueF());
                m_colorRampEditor->setStops(stops);
                m_extra->colorStops = m_colorRampEditor->stops();
                pushRampLutIpc(0u, 2u);
                commitSystemEdit(QStringLiteral("color_ramp"));
            }
        });

    /* Emitter selection & editing */
    connect(m_emitterList, &QListWidget::currentRowChanged, this, [this](int) {
        if (!m_updatingUi) {
            syncUiFromModel();
        }
    });
    connect(addEmBtn, &QPushButton::clicked, this, [this]() {
        if (m_emitters.size() >= WE_PARTICLE_MAX_EMITTERS) {
            return;
        }
        we_particle_emitter_t em{};
        em.type = WE_EMITTER_BOXRANDOM;
        em.origin[0] = 960.0f;
        em.origin[1] = 540.0f;
        em.distance_min[0] = -100.0f;
        em.distance_min[1] = -100.0f;
        em.distance_max[0] = 100.0f;
        em.distance_max[1] = 100.0f;
        em.directions[0] = 1.0f;
        em.directions[1] = 1.0f;
        em.directions[2] = 1.0f;
        em.rate = 30.0f;
        em.control_point = -1;
        m_emitters.push_back(em);
        syncUiFromModel();
        m_emitterList->setCurrentRow(static_cast<int>(m_emitters.size()) - 1);
        commitSystemEdit(QStringLiteral("emitter"));
    });
    connect(delEmBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_emitterList->currentRow();
        if (row >= 0 && row < static_cast<int>(m_emitters.size())) {
            m_emitters.erase(m_emitters.begin() + row);
            syncUiFromModel();
            commitSystemEdit(QStringLiteral("emitter"));
        }
    });
    auto onEmitterEdited = [this]() {
        if (m_updatingUi) {
            return;
        }
        const int row = m_emitterList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_emitters.size())) {
            return;
        }
        auto &em = m_emitters[static_cast<size_t>(row)];
        auto &state = *m_extra;
        em.type = static_cast<we_emitter_type_t>(state.emTypeCombo->currentData().toInt());
        em.origin[0] = static_cast<float>(state.emOrigX->value());
        em.origin[1] = static_cast<float>(state.emOrigY->value());
        em.origin[2] = static_cast<float>(state.emOrigZ->value());
        em.distance_min[0] = static_cast<float>(state.emDistMinX->value());
        em.distance_min[1] = static_cast<float>(state.emDistMinY->value());
        em.distance_max[0] = static_cast<float>(state.emDistMaxX->value());
        em.distance_max[1] = static_cast<float>(state.emDistMaxY->value());
        em.rate = static_cast<float>(state.emRateSpin->value());
        em.instantaneous = static_cast<uint32_t>(state.emBurstSpin->value());
        em.speed_min = static_cast<float>(state.emSpeedMinSpin->value());
        em.speed_max = static_cast<float>(state.emSpeedMaxSpin->value());
        em.control_point = state.emCpSpin->value();
        m_emitterList->item(row)->setText(emitterSummary(static_cast<size_t>(row), em));
        commitSystemEdit(QStringLiteral("emitter"));
    };
    connect(ex.emTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onEmitterEdited);
    connect(ex.emOrigX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emOrigY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emOrigZ, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emDistMinX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emDistMinY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emDistMaxX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emDistMaxY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emRateSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emBurstSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onEmitterEdited);
    connect(ex.emSpeedMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emSpeedMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onEmitterEdited);
    connect(ex.emCpSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onEmitterEdited);

    /* Initializer selection & editing */
    connect(m_initializerList, &QListWidget::currentRowChanged, this, [this](int) {
        if (!m_updatingUi) {
            syncUiFromModel();
        }
    });
    connect(addInitBtn, &QPushButton::clicked, this, [this]() {
        if (m_initializers.size() >= WE_PARTICLE_MAX_INITIALIZERS) {
            return;
        }
        we_particle_initializer_t iz{};
        iz.type = WE_INIT_SIZE_RANDOM;
        iz.min_val[0] = 8.0f;
        iz.max_val[0] = 16.0f;
        iz.exponent = 1.0f;
        m_initializers.push_back(iz);
        syncUiFromModel();
        m_initializerList->setCurrentRow(static_cast<int>(m_initializers.size()) - 1);
        commitSystemEdit(QStringLiteral("initializer"));
    });
    connect(delInitBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_initializerList->currentRow();
        if (row >= 0 && row < static_cast<int>(m_initializers.size())) {
            m_initializers.erase(m_initializers.begin() + row);
            syncUiFromModel();
            commitSystemEdit(QStringLiteral("initializer"));
        }
    });
    auto onInitEdited = [this]() {
        if (m_updatingUi) {
            return;
        }
        const int row = m_initializerList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_initializers.size())) {
            return;
        }
        auto &iz = m_initializers[static_cast<size_t>(row)];
        auto &state = *m_extra;
        iz.type = static_cast<we_initializer_type_t>(state.initTypeCombo->currentData().toInt());
        iz.min_val[0] = static_cast<float>(state.initMin0->value());
        iz.min_val[1] = static_cast<float>(state.initMin1->value());
        iz.min_val[2] = static_cast<float>(state.initMin2->value());
        iz.max_val[0] = static_cast<float>(state.initMax0->value());
        iz.max_val[1] = static_cast<float>(state.initMax1->value());
        iz.max_val[2] = static_cast<float>(state.initMax2->value());
        iz.exponent = static_cast<float>(state.initExpSpin->value());
        iz.scale = static_cast<float>(state.initScaleSpin->value());
        m_initializerList->item(row)->setText(initializerSummary(static_cast<size_t>(row), iz));
        commitSystemEdit(QStringLiteral("initializer"));
    };
    connect(ex.initTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            onInitEdited);
    connect(ex.initMin0, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initMin1, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initMin2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initMax0, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initMax1, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initMax2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onInitEdited);
    connect(ex.initExpSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onInitEdited);
    connect(ex.initScaleSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            onInitEdited);

    /* Operator selection & editing */
    connect(m_operatorList, &QListWidget::currentRowChanged, this, [this](int) {
        if (!m_updatingUi) {
            syncUiFromModel();
        }
    });
    connect(addOpBtn, &QPushButton::clicked, this, [this]() {
        if (m_operators.size() >= WE_PARTICLE_MAX_OPERATORS) {
            return;
        }
        we_particle_operator_t op{};
        op.type = WE_OP_MOVEMENT;
        op.gravity[1] = -98.0f;
        m_operators.push_back(op);
        syncUiFromModel();
        m_operatorList->setCurrentRow(static_cast<int>(m_operators.size()) - 1);
        commitSystemEdit(QStringLiteral("operator"));
    });
    connect(delOpBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_operatorList->currentRow();
        if (row >= 0 && row < static_cast<int>(m_operators.size())) {
            m_operators.erase(m_operators.begin() + row);
            syncUiFromModel();
            commitSystemEdit(QStringLiteral("operator"));
        }
    });
    auto onOpEdited = [this]() {
        if (m_updatingUi) {
            return;
        }
        const int row = m_operatorList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_operators.size())) {
            return;
        }
        auto &op = m_operators[static_cast<size_t>(row)];
        auto &state = *m_extra;
        const int rawType = state.opTypeCombo->currentData().toInt();
        const bool isCpAttract = (rawType == 100);
        op.type = isCpAttract ? WE_OP_VORTEX : static_cast<we_operator_type_t>(rawType);
        op.control_point = state.opCpSpin->value();

        const float v0x = static_cast<float>(state.opVec0X->value());
        const float v0y = static_cast<float>(state.opVec0Y->value());
        const float v0z = static_cast<float>(state.opVec0Z->value());
        const float v1x = static_cast<float>(state.opVec1X->value());
        const float v1y = static_cast<float>(state.opVec1Y->value());
        const float v1z = static_cast<float>(state.opVec1Z->value());
        const float s0 = static_cast<float>(state.opScalar0->value());
        const float s1 = static_cast<float>(state.opScalar1->value());
        const float s2 = static_cast<float>(state.opScalar2->value());
        const float s3 = static_cast<float>(state.opScalar3->value());

        switch (op.type) {
        case WE_OP_MOVEMENT:
        case WE_OP_ANGULAR_MOVEMENT:
            op.gravity[0] = v0x;
            op.gravity[1] = v0y;
            op.gravity[2] = v0z;
            op.drag = s0;
            break;
        case WE_OP_ALPHA_FADE:
            op.fade_in_time = s0;
            op.fade_out_time = s1;
            break;
        case WE_OP_SIZE_CHANGE:
            op.start_time = s0;
            op.end_time = s1;
            op.start_val[0] = s2;
            op.end_val[0] = s3;
            break;
        case WE_OP_COLOR_CHANGE:
            op.start_val[0] = v0x;
            op.start_val[1] = v0y;
            op.start_val[2] = v0z;
            op.end_val[0] = v1x;
            op.end_val[1] = v1y;
            op.end_val[2] = v1z;
            op.start_time = s0;
            op.end_time = s1;
            break;
        case WE_OP_OSCILLATE_ALPHA:
        case WE_OP_OSCILLATE_SIZE:
        case WE_OP_OSCILLATE_POSITION:
            op.mask[0] = v0x;
            op.mask[1] = v0y;
            op.mask[2] = v0z;
            op.freq_min = s0;
            op.freq_max = s1;
            op.scale_min = s2;
            op.scale_max = s3;
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
        case WE_OP_VORTEX:
            op.axis[0] = v0x;
            op.axis[1] = v0y;
            op.axis[2] = v0z;
            op.offset[0] = v1x;
            op.offset[1] = v1y;
            op.offset[2] = v1z;
            op.distance_inner = s0;
            op.distance_outer = s1;
            op.center_force = s2;
            if (isCpAttract) {
                op.speed_inner = 0.0f;
                op.speed_outer = 0.0f;
            } else {
                op.speed_inner = s3;
                op.speed_outer = s3 * 0.4f;
            }
            break;
        case WE_OP_TURBULENCE:
            op.mask[0] = v0x;
            op.mask[1] = v0y;
            op.mask[2] = v0z;
            op.turb_scale = s0;
            op.turb_speed = s1;
            op.turb_timescale = s2;
            break;
        }
        m_operatorList->item(row)->setText(operatorSummary(static_cast<size_t>(row), op));
        commitSystemEdit(QStringLiteral("operator"));
    };
    connect(ex.opTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updatingUi) {
            return;
        }
        const int row = m_operatorList->currentRow();
        if (row < 0 || row >= static_cast<int>(m_operators.size())) {
            return;
        }
        auto &op = m_operators[static_cast<size_t>(row)];
        const int rawType = m_extra->opTypeCombo->currentData().toInt();
        const bool isCpAttract = (rawType == 100);
        const int32_t savedCp = op.control_point;
        op = we_particle_operator_t{};
        op.type = isCpAttract ? WE_OP_VORTEX : static_cast<we_operator_type_t>(rawType);
        op.control_point = savedCp;
        switch (op.type) {
        case WE_OP_MOVEMENT:
            op.gravity[1] = -98.0f;
            break;
        case WE_OP_ALPHA_FADE:
            op.fade_in_time = 0.1f;
            op.fade_out_time = 0.9f;
            break;
        case WE_OP_SIZE_CHANGE:
            op.start_time = 0.0f;
            op.end_time = 1.0f;
            op.start_val[0] = 1.0f;
            op.end_val[0] = 1.0f;
            break;
        case WE_OP_COLOR_CHANGE:
            op.start_time = 0.0f;
            op.end_time = 1.0f;
            op.start_val[0] = op.start_val[1] = op.start_val[2] = 1.0f;
            break;
        case WE_OP_OSCILLATE_ALPHA:
        case WE_OP_OSCILLATE_SIZE:
        case WE_OP_OSCILLATE_POSITION:
            op.freq_min = 1.0f;
            op.freq_max = 2.0f;
            op.scale_min = 0.5f;
            op.scale_max = 1.0f;
            op.mask[0] = 1.0f;
            op.mask[1] = 1.0f;
            break;
        case WE_OP_CONTROL_POINT_ATTRACT:
        case WE_OP_VORTEX:
            op.axis[2] = 1.0f;
            op.distance_outer = 300.0f;
            op.center_force = 200.0f;
            op.speed_inner = isCpAttract ? 0.0f : 120.0f;
            op.speed_outer = isCpAttract ? 0.0f : 48.0f;
            break;
        case WE_OP_TURBULENCE:
            op.turb_scale = 0.01f;
            op.turb_speed = 40.0f;
            op.turb_timescale = 1.0f;
            op.mask[0] = 1.0f;
            op.mask[1] = 1.0f;
            break;
        default:
            break;
        }
        syncUiFromModel();
        commitSystemEdit(QStringLiteral("operator"));
    });
    connect(ex.opVec0X, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opVec0Y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opVec0Z, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opVec1X, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opVec1Y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opVec1Z, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opScalar0, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opScalar1, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opScalar2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opScalar3, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onOpEdited);
    connect(ex.opCpSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, onOpEdited);

    /* Control points editing */
    connect(ex.cpIndexCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { syncUiFromModel(); });
    auto onCpEdited = [this]() {
        if (m_updatingUi) {
            return;
        }
        auto &state = *m_extra;
        const int cid = state.cpIndexCombo->currentData().toInt();
        if (cid < 0 || cid >= static_cast<int>(WE_PARTICLE_MAX_CONTROL_PTS)) {
            return;
        }
        while (static_cast<int>(m_controlPoints.size()) <= cid) {
            we_particle_cp_t cp{};
            cp.id = static_cast<int32_t>(m_controlPoints.size());
            m_controlPoints.push_back(cp);
        }
        auto &cp = m_controlPoints[static_cast<size_t>(cid)];
        cp.lock_to_pointer = state.cpLockPointerCheck->isChecked();
        cp.world_space = state.cpWorldSpaceCheck->isChecked();
        cp.offset[0] = static_cast<float>(state.cpOffsetX->value());
        cp.offset[1] = static_cast<float>(state.cpOffsetY->value());
        emit controlPointOverlayUpdated(m_objectId);
        commitSystemEdit(QStringLiteral("control_point"));
    };
    connect(ex.cpLockPointerCheck, &QCheckBox::toggled, this, onCpEdited);
    connect(ex.cpWorldSpaceCheck, &QCheckBox::toggled, this, onCpEdited);
    connect(ex.cpOffsetX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onCpEdited);
    connect(ex.cpOffsetY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, onCpEdited);

    syncUiFromModel();
}

void ParticleDesignerWidget::syncUiFromModel()
{
    if (!m_uiInitialized) {
        return;
    }

    QScopedValueRollback<bool> guard(m_updatingUi, true);
    const QSignalBlocker bEmList(m_emitterList);
    const QSignalBlocker bInitList(m_initializerList);
    const QSignalBlocker bOpList(m_operatorList);
    auto &ex = *m_extra;

    m_maxCountSpin->setValue(static_cast<int>(m_maxCount));
    m_startTimeSpin->setValue(static_cast<double>(m_startTime));
    if (const int rIdx = m_rendererCombo->findData(static_cast<int>(m_rendererType)); rIdx >= 0) {
        m_rendererCombo->setCurrentIndex(rIdx);
    }
    if (const int bIdx = m_blendingCombo->findData(m_blending); bIdx >= 0) {
        m_blendingCombo->setCurrentIndex(bIdx);
    }
    m_worldSpaceCheck->setChecked(m_worldSpace);

    m_alphaCurveEditor->setKnots(ex.alphaKnots);
    m_sizeCurveEditor->setKnots(ex.sizeKnots);
    m_colorRampEditor->setStops(ex.colorStops);

    const int prevEmRow = m_emitterList->currentRow();
    m_emitterList->clear();
    for (size_t i = 0; i < m_emitters.size(); ++i) {
        m_emitterList->addItem(emitterSummary(i, m_emitters[i]));
    }
    const int emRow = m_emitters.empty() ? -1
                                         : std::clamp(prevEmRow < 0 ? 0 : prevEmRow, 0,
                                                      static_cast<int>(m_emitters.size()) - 1);
    if (emRow >= 0) {
        m_emitterList->setCurrentRow(emRow);
        const auto &em = m_emitters[static_cast<size_t>(emRow)];
        ex.emitterDetailCard->setEnabled(true);
        if (const int ti = ex.emTypeCombo->findData(static_cast<int>(em.type)); ti >= 0) {
            ex.emTypeCombo->setCurrentIndex(ti);
        }
        ex.emOrigX->setValue(em.origin[0]);
        ex.emOrigY->setValue(em.origin[1]);
        ex.emOrigZ->setValue(em.origin[2]);
        ex.emDistMinX->setValue(em.distance_min[0]);
        ex.emDistMinY->setValue(em.distance_min[1]);
        ex.emDistMaxX->setValue(em.distance_max[0]);
        ex.emDistMaxY->setValue(em.distance_max[1]);
        ex.emRateSpin->setValue(em.rate);
        ex.emBurstSpin->setValue(static_cast<int>(em.instantaneous));
        ex.emSpeedMinSpin->setValue(em.speed_min);
        ex.emSpeedMaxSpin->setValue(em.speed_max);
        ex.emCpSpin->setValue(em.control_point);
    } else {
        ex.emitterDetailCard->setEnabled(false);
    }

    const int prevInitRow = m_initializerList->currentRow();
    m_initializerList->clear();
    for (size_t i = 0; i < m_initializers.size(); ++i) {
        m_initializerList->addItem(initializerSummary(i, m_initializers[i]));
    }
    const int initRow = m_initializers.empty()
                            ? -1
                            : std::clamp(prevInitRow < 0 ? 0 : prevInitRow, 0,
                                         static_cast<int>(m_initializers.size()) - 1);
    if (initRow >= 0) {
        m_initializerList->setCurrentRow(initRow);
        const auto &iz = m_initializers[static_cast<size_t>(initRow)];
        ex.initDetailCard->setEnabled(true);
        if (const int ti = ex.initTypeCombo->findData(static_cast<int>(iz.type)); ti >= 0) {
            ex.initTypeCombo->setCurrentIndex(ti);
        }
        ex.initMin0->setValue(iz.min_val[0]);
        ex.initMin1->setValue(iz.min_val[1]);
        ex.initMin2->setValue(iz.min_val[2]);
        ex.initMax0->setValue(iz.max_val[0]);
        ex.initMax1->setValue(iz.max_val[1]);
        ex.initMax2->setValue(iz.max_val[2]);
        ex.initExpSpin->setValue(iz.exponent);
        ex.initScaleSpin->setValue(iz.scale);
    } else {
        ex.initDetailCard->setEnabled(false);
    }

    const int prevOpRow = m_operatorList->currentRow();
    m_operatorList->clear();
    for (size_t i = 0; i < m_operators.size(); ++i) {
        m_operatorList->addItem(operatorSummary(i, m_operators[i]));
    }
    const int opRow = m_operators.empty() ? -1
                                          : std::clamp(prevOpRow < 0 ? 0 : prevOpRow, 0,
                                                       static_cast<int>(m_operators.size()) - 1);
    if (opRow >= 0) {
        m_operatorList->setCurrentRow(opRow);
        const auto &op = m_operators[static_cast<size_t>(opRow)];
        ex.opDetailCard->setEnabled(true);
        const int typeData = isControlPointAttractOp(op) ? 100 : static_cast<int>(op.type);
        if (const int ti = ex.opTypeCombo->findData(typeData); ti >= 0) {
            ex.opTypeCombo->setCurrentIndex(ti);
        }
        ex.opCpSpin->setValue(op.control_point);
        auto setOpFieldMask = [&ex](bool useVec0, bool useVec1, bool useS0, bool useS1, bool useS2,
                                    bool useS3, bool useCp) {
            ex.opVec0X->setEnabled(useVec0);
            ex.opVec0Y->setEnabled(useVec0);
            ex.opVec0Z->setEnabled(useVec0);
            ex.opVec1X->setEnabled(useVec1);
            ex.opVec1Y->setEnabled(useVec1);
            ex.opVec1Z->setEnabled(useVec1);
            ex.opScalar0->setEnabled(useS0);
            ex.opScalar1->setEnabled(useS1);
            ex.opScalar2->setEnabled(useS2);
            ex.opScalar3->setEnabled(useS3);
            ex.opCpSpin->setEnabled(useCp);
        };
        if (isControlPointAttractOp(op)) {
            ex.opVec0Label->setText(tr("Axis:"));
            ex.opVec1Label->setText(tr("Offset:"));
            ex.opFieldLabel0->setText(tr("Inner R:"));
            ex.opFieldLabel1->setText(tr("Outer R:"));
            ex.opFieldLabel2->setText(tr("Attract Force:"));
            ex.opFieldLabel3->setText(tr("Unused:"));
            ex.opVec0X->setValue(op.axis[0]);
            ex.opVec0Y->setValue(op.axis[1]);
            ex.opVec0Z->setValue(op.axis[2]);
            ex.opVec1X->setValue(op.offset[0]);
            ex.opVec1Y->setValue(op.offset[1]);
            ex.opVec1Z->setValue(op.offset[2]);
            ex.opScalar0->setValue(op.distance_inner);
            ex.opScalar1->setValue(op.distance_outer);
            ex.opScalar2->setValue(op.center_force);
            ex.opScalar3->setValue(0.0);
            setOpFieldMask(true, true, true, true, true, false, true);
        } else if (op.type == WE_OP_MOVEMENT || op.type == WE_OP_ANGULAR_MOVEMENT) {
            ex.opVec0Label->setText(tr("Gravity:"));
            ex.opVec1Label->setText(tr("Unused:"));
            ex.opFieldLabel0->setText(tr("Drag:"));
            ex.opFieldLabel1->setText(tr("Unused:"));
            ex.opFieldLabel2->setText(tr("Unused:"));
            ex.opFieldLabel3->setText(tr("Unused:"));
            ex.opVec0X->setValue(op.gravity[0]);
            ex.opVec0Y->setValue(op.gravity[1]);
            ex.opVec0Z->setValue(op.gravity[2]);
            ex.opScalar0->setValue(op.drag);
            setOpFieldMask(true, false, true, false, false, false, false);
        } else if (op.type == WE_OP_VORTEX) {
            ex.opVec0Label->setText(tr("Axis:"));
            ex.opVec1Label->setText(tr("Offset:"));
            ex.opFieldLabel0->setText(tr("Inner R:"));
            ex.opFieldLabel1->setText(tr("Outer R:"));
            ex.opFieldLabel2->setText(tr("Center Force:"));
            ex.opFieldLabel3->setText(tr("Orbit Speed:"));
            ex.opVec0X->setValue(op.axis[0]);
            ex.opVec0Y->setValue(op.axis[1]);
            ex.opVec0Z->setValue(op.axis[2]);
            ex.opVec1X->setValue(op.offset[0]);
            ex.opVec1Y->setValue(op.offset[1]);
            ex.opVec1Z->setValue(op.offset[2]);
            ex.opScalar0->setValue(op.distance_inner);
            ex.opScalar1->setValue(op.distance_outer);
            ex.opScalar2->setValue(op.center_force);
            ex.opScalar3->setValue(op.speed_inner);
            setOpFieldMask(true, true, true, true, true, true, true);
        } else if (op.type == WE_OP_TURBULENCE) {
            ex.opVec0Label->setText(tr("Mask:"));
            ex.opVec1Label->setText(tr("Unused:"));
            ex.opFieldLabel0->setText(tr("Noise Scale:"));
            ex.opFieldLabel1->setText(tr("Speed:"));
            ex.opFieldLabel2->setText(tr("Time Scale:"));
            ex.opFieldLabel3->setText(tr("Unused:"));
            ex.opVec0X->setValue(op.mask[0]);
            ex.opVec0Y->setValue(op.mask[1]);
            ex.opVec0Z->setValue(op.mask[2]);
            ex.opScalar0->setValue(op.turb_scale);
            ex.opScalar1->setValue(op.turb_speed);
            ex.opScalar2->setValue(op.turb_timescale);
            setOpFieldMask(true, false, true, true, true, false, false);
        } else if (op.type == WE_OP_ALPHA_FADE) {
            ex.opVec0Label->setText(tr("Unused:"));
            ex.opVec1Label->setText(tr("Unused:"));
            ex.opFieldLabel0->setText(tr("Fade In:"));
            ex.opFieldLabel1->setText(tr("Fade Out:"));
            ex.opFieldLabel2->setText(tr("Unused:"));
            ex.opFieldLabel3->setText(tr("Unused:"));
            ex.opScalar0->setValue(op.fade_in_time);
            ex.opScalar1->setValue(op.fade_out_time);
            setOpFieldMask(false, false, true, true, false, false, false);
        } else if (op.type == WE_OP_SIZE_CHANGE) {
            ex.opVec0Label->setText(tr("Unused:"));
            ex.opVec1Label->setText(tr("Unused:"));
            ex.opFieldLabel0->setText(tr("Start Time:"));
            ex.opFieldLabel1->setText(tr("End Time:"));
            ex.opFieldLabel2->setText(tr("Start Size:"));
            ex.opFieldLabel3->setText(tr("End Size:"));
            ex.opScalar0->setValue(op.start_time);
            ex.opScalar1->setValue(op.end_time);
            ex.opScalar2->setValue(op.start_val[0]);
            ex.opScalar3->setValue(op.end_val[0]);
            setOpFieldMask(false, false, true, true, true, true, false);
        } else if (op.type == WE_OP_COLOR_CHANGE) {
            ex.opVec0Label->setText(tr("Start Color:"));
            ex.opVec1Label->setText(tr("End Color:"));
            ex.opFieldLabel0->setText(tr("Start Time:"));
            ex.opFieldLabel1->setText(tr("End Time:"));
            ex.opFieldLabel2->setText(tr("Unused:"));
            ex.opFieldLabel3->setText(tr("Unused:"));
            ex.opVec0X->setValue(op.start_val[0]);
            ex.opVec0Y->setValue(op.start_val[1]);
            ex.opVec0Z->setValue(op.start_val[2]);
            ex.opVec1X->setValue(op.end_val[0]);
            ex.opVec1Y->setValue(op.end_val[1]);
            ex.opVec1Z->setValue(op.end_val[2]);
            ex.opScalar0->setValue(op.start_time);
            ex.opScalar1->setValue(op.end_time);
            setOpFieldMask(true, true, true, true, false, false, false);
        } else {
            const bool hasMask = (op.type == WE_OP_OSCILLATE_POSITION);
            ex.opVec0Label->setText(hasMask ? tr("Mask:") : tr("Unused:"));
            ex.opVec1Label->setText(tr("Unused:"));
            ex.opFieldLabel0->setText(tr("Freq Min:"));
            ex.opFieldLabel1->setText(tr("Freq Max:"));
            ex.opFieldLabel2->setText(tr("Scale Min:"));
            ex.opFieldLabel3->setText(tr("Scale Max:"));
            ex.opVec0X->setValue(op.mask[0]);
            ex.opVec0Y->setValue(op.mask[1]);
            ex.opVec0Z->setValue(op.mask[2]);
            ex.opScalar0->setValue(op.freq_min);
            ex.opScalar1->setValue(op.freq_max);
            ex.opScalar2->setValue(op.scale_min);
            ex.opScalar3->setValue(op.scale_max);
            setOpFieldMask(hasMask, false, true, true, true, true, false);
        }
    } else {
        ex.opDetailCard->setEnabled(false);
    }

    const int cpId = ex.cpIndexCombo->currentData().toInt();
    if (cpId >= 0 && cpId < static_cast<int>(m_controlPoints.size())) {
        const auto &cp = m_controlPoints[static_cast<size_t>(cpId)];
        ex.cpLockPointerCheck->setChecked(cp.lock_to_pointer);
        ex.cpWorldSpaceCheck->setChecked(cp.world_space);
        ex.cpOffsetX->setValue(cp.offset[0]);
        ex.cpOffsetY->setValue(cp.offset[1]);
    } else {
        ex.cpLockPointerCheck->setChecked(false);
        ex.cpWorldSpaceCheck->setChecked(false);
        ex.cpOffsetX->setValue(0.0);
        ex.cpOffsetY->setValue(0.0);
    }

    if (ex.telemetryDirty) {
        updateTelemetry(ex.pendingAlive, ex.pendingMax, ex.pendingSimUs);
    }
}

bool ParticleDesignerWidget::importSystemJson(const QString &jsonState)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonState.toUtf8());
    if (!doc.isObject()) {
        return false;
    }

    const QJsonObject root = doc.object();
    auto &ex = *m_extra;
    m_maxCount = static_cast<uint32_t>(
        std::clamp(root.value(QStringLiteral("maxcount")).toInt(static_cast<int>(m_maxCount)), 1,
                   static_cast<int>(WE_PARTICLE_MAX_CAP)));
    m_startTime = static_cast<float>(root.value(QStringLiteral("starttime")).toDouble(m_startTime));
    ex.materialPath = root.value(QStringLiteral("material")).toString(ex.materialPath);
    m_blending = static_cast<uint32_t>(
        root.value(QStringLiteral("blending")).toInt(static_cast<int>(m_blending)));
    const int flags = root.value(QStringLiteral("flags")).toInt(0);
    m_worldSpace = (flags & 1) != 0;
    m_perspective = (flags & 4) != 0;

    const QJsonArray rendArr = root.value(QStringLiteral("renderer")).toArray();
    if (!rendArr.isEmpty()) {
        const QJsonObject ro = rendArr.at(0).toObject();
        const QString rname = ro.value(QStringLiteral("name")).toString();
        if (rname == QLatin1String("spritetrail")) {
            m_rendererType = WE_RENDERER_SPRITETRAIL;
        } else if (rname == QLatin1String("ropetrail")) {
            m_rendererType = WE_RENDERER_ROPETRAIL;
        } else {
            m_rendererType = WE_RENDERER_SPRITE;
        }
        ex.trailLength = static_cast<float>(ro.value(QStringLiteral("length")).toDouble(0.05));
        ex.trailMinLength = static_cast<float>(ro.value(QStringLiteral("minlength")).toDouble(1.0));
        ex.trailMaxLength =
            static_cast<float>(ro.value(QStringLiteral("maxlength")).toDouble(100.0));
        ex.subdivision =
            static_cast<uint32_t>(std::max(1, ro.value(QStringLiteral("subdivision")).toInt(1)));
    }

    m_controlPoints.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("controlpoint")).toArray()) {
        const QJsonObject o = v.toObject();
        we_particle_cp_t cp{};
        cp.id = o.value(QStringLiteral("id")).toInt(0);
        parseVec3(o.value(QStringLiteral("offset")).toString(), cp.offset);
        const int cpFlags = o.value(QStringLiteral("flags")).toInt(0);
        cp.lock_to_pointer = (cpFlags & 1) != 0;
        cp.world_space = (cpFlags & 2) != 0;
        m_controlPoints.push_back(cp);
    }

    m_emitters.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("emitter")).toArray()) {
        const QJsonObject o = v.toObject();
        we_particle_emitter_t em{};
        const QString ename = o.value(QStringLiteral("name")).toString();
        if (ename == QLatin1String("sphererandom")) {
            em.type = WE_EMITTER_SPHERERANDOM;
        } else if (ename == QLatin1String("point")) {
            em.type = WE_EMITTER_POINT;
        } else {
            em.type = WE_EMITTER_BOXRANDOM;
        }
        parseVec3(o.value(QStringLiteral("distancemin")).toString(), em.distance_min);
        parseVec3(o.value(QStringLiteral("distancemax")).toString(), em.distance_max);
        parseVec3(o.value(QStringLiteral("directions")).toString(), em.directions);
        parseVec3(o.value(QStringLiteral("origin")).toString(), em.origin);
        em.speed_min = static_cast<float>(o.value(QStringLiteral("speedmin")).toDouble(0.0));
        em.speed_max = static_cast<float>(o.value(QStringLiteral("speedmax")).toDouble(0.0));
        em.rate = static_cast<float>(o.value(QStringLiteral("rate")).toDouble(0.0));
        em.instantaneous =
            static_cast<uint32_t>(std::max(0, o.value(QStringLiteral("instantaneous")).toInt(0)));
        em.control_point = o.value(QStringLiteral("controlpoint")).toInt(0);
        m_emitters.push_back(em);
    }

    m_initializers.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("initializer")).toArray()) {
        const QJsonObject o = v.toObject();
        we_particle_initializer_t iz{};
        iz.exponent = static_cast<float>(o.value(QStringLiteral("exponent")).toDouble(1.0));
        const QString iname = o.value(QStringLiteral("name")).toString();
        if (iname == QLatin1String("lifetimerandom")) {
            iz.type = WE_INIT_LIFETIME_RANDOM;
            iz.min_val[0] = static_cast<float>(o.value(QStringLiteral("min")).toDouble(1.0));
            iz.max_val[0] = static_cast<float>(o.value(QStringLiteral("max")).toDouble(1.0));
        } else if (iname == QLatin1String("sizerandom")) {
            iz.type = WE_INIT_SIZE_RANDOM;
            iz.min_val[0] = static_cast<float>(o.value(QStringLiteral("min")).toDouble(1.0));
            iz.max_val[0] = static_cast<float>(o.value(QStringLiteral("max")).toDouble(1.0));
        } else if (iname == QLatin1String("alpharandom")) {
            iz.type = WE_INIT_ALPHA_RANDOM;
            iz.min_val[0] = static_cast<float>(o.value(QStringLiteral("min")).toDouble(1.0));
            iz.max_val[0] = static_cast<float>(o.value(QStringLiteral("max")).toDouble(1.0));
        } else if (iname == QLatin1String("colorrandom")) {
            iz.type = WE_INIT_COLOR_RANDOM;
            parseVec3(o.value(QStringLiteral("min")).toString(), iz.min_val);
            parseVec3(o.value(QStringLiteral("max")).toString(), iz.max_val);
        } else if (iname == QLatin1String("velocityrandom")) {
            iz.type = WE_INIT_VELOCITY_RANDOM;
            parseVec3(o.value(QStringLiteral("min")).toString(), iz.min_val);
            parseVec3(o.value(QStringLiteral("max")).toString(), iz.max_val);
        } else if (iname == QLatin1String("rotationrandom")) {
            iz.type = WE_INIT_ROTATION_RANDOM;
            parseVec3(o.value(QStringLiteral("min")).toString(), iz.min_val);
            parseVec3(o.value(QStringLiteral("max")).toString(), iz.max_val);
        } else if (iname == QLatin1String("angularvelocityrandom")) {
            iz.type = WE_INIT_ANGULAR_VELOCITY_RANDOM;
            parseVec3(o.value(QStringLiteral("min")).toString(), iz.min_val);
            parseVec3(o.value(QStringLiteral("max")).toString(), iz.max_val);
        } else if (iname == QLatin1String("turbulentvelocityrandom")) {
            iz.type = WE_INIT_TURBULENT_VELOCITY_RANDOM;
            iz.min_val[0] = static_cast<float>(o.value(QStringLiteral("speedmin")).toDouble(0.0));
            iz.max_val[0] = static_cast<float>(o.value(QStringLiteral("speedmax")).toDouble(0.0));
            iz.scale = static_cast<float>(o.value(QStringLiteral("scale")).toDouble(1.0));
            iz.offset = static_cast<float>(o.value(QStringLiteral("offset")).toDouble(0.0));
            parseVec3(o.value(QStringLiteral("forward")).toString(), iz.forward);
        }
        m_initializers.push_back(iz);
    }

    m_operators.clear();
    for (const QJsonValue &v : root.value(QStringLiteral("operator")).toArray()) {
        const QJsonObject o = v.toObject();
        we_particle_operator_t op{};
        const QString oname = o.value(QStringLiteral("name")).toString();
        if (oname == QLatin1String("movement")) {
            op.type = WE_OP_MOVEMENT;
            parseVec3(o.value(QStringLiteral("gravity")).toString(), op.gravity);
            op.drag = static_cast<float>(o.value(QStringLiteral("drag")).toDouble(0.0));
        } else if (oname == QLatin1String("angularmovement")) {
            op.type = WE_OP_ANGULAR_MOVEMENT;
            op.drag = static_cast<float>(o.value(QStringLiteral("drag")).toDouble(0.0));
            parseVec3(o.value(QStringLiteral("force")).toString(), op.gravity);
        } else if (oname == QLatin1String("alphafade")) {
            op.type = WE_OP_ALPHA_FADE;
            op.fade_in_time =
                static_cast<float>(o.value(QStringLiteral("fadeintime")).toDouble(0.0));
            op.fade_out_time =
                static_cast<float>(o.value(QStringLiteral("fadeouttime")).toDouble(1.0));
        } else if (oname == QLatin1String("sizechange")) {
            op.type = WE_OP_SIZE_CHANGE;
            op.start_time = static_cast<float>(o.value(QStringLiteral("starttime")).toDouble(0.0));
            op.end_time = static_cast<float>(o.value(QStringLiteral("endtime")).toDouble(1.0));
            op.start_val[0] =
                static_cast<float>(o.value(QStringLiteral("startvalue")).toDouble(1.0));
            op.end_val[0] = static_cast<float>(o.value(QStringLiteral("endvalue")).toDouble(1.0));
        } else if (oname == QLatin1String("colorchange")) {
            op.type = WE_OP_COLOR_CHANGE;
            op.start_time = static_cast<float>(o.value(QStringLiteral("starttime")).toDouble(0.0));
            op.end_time = static_cast<float>(o.value(QStringLiteral("endtime")).toDouble(1.0));
            parseVec3(o.value(QStringLiteral("startvalue")).toString(), op.start_val);
            parseVec3(o.value(QStringLiteral("endvalue")).toString(), op.end_val);
        } else if (oname == QLatin1String("oscillatealpha")) {
            op.type = WE_OP_OSCILLATE_ALPHA;
            op.freq_min = static_cast<float>(o.value(QStringLiteral("frequencymin")).toDouble(0.0));
            op.freq_max = static_cast<float>(o.value(QStringLiteral("frequencymax")).toDouble(0.0));
            op.scale_min = static_cast<float>(o.value(QStringLiteral("scalemin")).toDouble(0.0));
            op.scale_max = static_cast<float>(o.value(QStringLiteral("scalemax")).toDouble(1.0));
        } else if (oname == QLatin1String("oscillatesize")) {
            op.type = WE_OP_OSCILLATE_SIZE;
            op.freq_min = static_cast<float>(o.value(QStringLiteral("frequencymin")).toDouble(0.0));
            op.freq_max = static_cast<float>(o.value(QStringLiteral("frequencymax")).toDouble(0.0));
            op.scale_min = static_cast<float>(o.value(QStringLiteral("scalemin")).toDouble(0.0));
            op.scale_max = static_cast<float>(o.value(QStringLiteral("scalemax")).toDouble(1.0));
        } else if (oname == QLatin1String("oscillateposition")) {
            op.type = WE_OP_OSCILLATE_POSITION;
            op.freq_min = static_cast<float>(o.value(QStringLiteral("frequencymin")).toDouble(0.0));
            op.freq_max = static_cast<float>(o.value(QStringLiteral("frequencymax")).toDouble(0.0));
            op.scale_min = static_cast<float>(o.value(QStringLiteral("scalemin")).toDouble(0.0));
            op.scale_max = static_cast<float>(o.value(QStringLiteral("scalemax")).toDouble(1.0));
            parseVec3(o.value(QStringLiteral("mask")).toString(), op.mask);
        } else if (oname == QLatin1String("vortex") ||
                   oname == QLatin1String("controlpointattract")) {
            op.type = WE_OP_VORTEX;
            op.control_point = o.value(QStringLiteral("controlpoint")).toInt(0);
            parseVec3(o.value(QStringLiteral("axis")).toString(), op.axis);
            parseVec3(o.value(QStringLiteral("offset")).toString(), op.offset);
            op.distance_inner =
                static_cast<float>(o.value(QStringLiteral("distanceinner")).toDouble(0.0));
            op.distance_outer =
                static_cast<float>(o.value(QStringLiteral("distanceouter")).toDouble(0.0));
            op.speed_inner =
                static_cast<float>(o.value(QStringLiteral("speedinner")).toDouble(0.0));
            op.speed_outer =
                static_cast<float>(o.value(QStringLiteral("speedouter")).toDouble(0.0));
            op.center_force =
                static_cast<float>(o.value(QStringLiteral("centerforce")).toDouble(0.0));
        } else if (oname == QLatin1String("turbulence")) {
            op.type = WE_OP_TURBULENCE;
            op.turb_scale = static_cast<float>(o.value(QStringLiteral("scale")).toDouble(0.0));
            op.turb_speed = static_cast<float>(o.value(QStringLiteral("speed")).toDouble(0.0));
            op.turb_timescale =
                static_cast<float>(o.value(QStringLiteral("timescale")).toDouble(1.0));
            parseVec3(o.value(QStringLiteral("mask")).toString(), op.mask);
        }
        m_operators.push_back(op);
    }

    (void)parseCurveKnotsJson(root.value(QStringLiteral("alphaknots")).toArray(), ex.alphaKnots);
    (void)parseCurveKnotsJson(root.value(QStringLiteral("sizeknots")).toArray(), ex.sizeKnots);
    (void)parseColorStopsJson(root.value(QStringLiteral("colorstops")).toArray(), ex.colorStops);
    for (auto &k : ex.alphaKnots) {
        k.y = std::clamp(k.y, 0.0f, 1.0f);
    }
    for (auto &k : ex.sizeKnots) {
        k.y = std::clamp(k.y, 0.0f, 3.0f);
    }

    syncUiFromModel();
    return true;
}

void ParticleDesignerWidget::commitSystemEdit(const QString &paramGroup)
{
    const QString newJson = exportSystemJson();
    QPointer<ParticleDesignerWidget> self(this);
    auto applyFn = [self](int32_t objId, const QString &jsonState) {
        if (!self) {
            return;
        }
        const QByteArray utf8 = jsonState.toUtf8();
        if (self->m_objectId == objId) {
            if (jsonState != self->exportSystemJson()) {
                (void)self->importSystemJson(jsonState);
            }
            self->m_lastCommittedJson = jsonState;
            self->pushRampLutIpcForObject(objId, 0u, 0u);
            self->pushRampLutIpcForObject(objId, 0u, 1u);
            self->pushRampLutIpcForObject(objId, 0u, 2u);
        } else {
            // Active selection differs; bake LUTs for the target object without altering UI state.
            const QJsonDocument doc = QJsonDocument::fromJson(utf8);
            if (doc.isObject()) {
                const QJsonObject root = doc.object();
                std::vector<we_particle_curve_knot_t> aKnots;
                std::vector<we_particle_curve_knot_t> sKnots;
                std::vector<we_particle_color_stop_t> cStops;
                if (parseCurveKnotsJson(root.value(QStringLiteral("alphaknots")).toArray(),
                                        aKnots)) {
                    bakeAndSendRampLut(self->m_ipcClient, self->m_targetOutput, objId, 0u, 0u,
                                       aKnots, sKnots, cStops, nullptr, nullptr, nullptr);
                }
                if (parseCurveKnotsJson(root.value(QStringLiteral("sizeknots")).toArray(),
                                        sKnots)) {
                    bakeAndSendRampLut(self->m_ipcClient, self->m_targetOutput, objId, 0u, 1u,
                                       aKnots, sKnots, cStops, nullptr, nullptr, nullptr);
                }
                if (parseColorStopsJson(root.value(QStringLiteral("colorstops")).toArray(),
                                        cStops)) {
                    bakeAndSendRampLut(self->m_ipcClient, self->m_targetOutput, objId, 0u, 2u,
                                       aKnots, sKnots, cStops, nullptr, nullptr, nullptr);
                }
            }
        }
        emit self->particleSystemModified(objId, jsonState);
        dispatchParticleConfigIpc(self->m_ipcClient, self->m_targetOutput, objId,
                                  WE_PARTICLE_CFG_SET_SYSTEM_JSON, 0u, 2u, utf8.constData(),
                                  static_cast<uint32_t>(utf8.size()));
    };

    if (m_commandBus && !m_lastCommittedJson.isEmpty() && newJson != m_lastCommittedJson) {
        m_commandBus->execute(std::make_unique<EditParticleSystemCommand>(
            m_objectId, paramGroup, m_lastCommittedJson, newJson, std::move(applyFn)));
    } else {
        applyFn(m_objectId, newJson);
    }
}

void ParticleDesignerWidget::pushRampLutIpc(uint32_t opIndex, uint32_t channelKind)
{
    pushRampLutIpcForObject(m_objectId, opIndex, channelKind);
}

void ParticleDesignerWidget::pushRampLutIpcForObject(int32_t objectId, uint32_t opIndex,
                                                     uint32_t channelKind)
{
    const auto &ex = *m_extra;
    bakeAndSendRampLut(m_ipcClient, m_targetOutput, objectId, opIndex, channelKind, ex.alphaKnots,
                       ex.sizeKnots, ex.colorStops, m_alphaCurveEditor, m_sizeCurveEditor,
                       m_colorRampEditor);
}

} // namespace waywal::studio
