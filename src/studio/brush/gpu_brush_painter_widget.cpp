// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/brush/gpu_brush_painter_widget.hpp"

#include "studio/inspector/oklch_color_dialog.hpp"

#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace waywal::studio {

namespace {

constexpr uint32_t kTexFmtRgba8888 = 0u;
constexpr uint32_t kTexFmtRg88 = 8u;
constexpr uint32_t kTexFmtR8 = 9u;
constexpr size_t kDabBatchFlushThreshold = 8u;

float finiteOr(float v, float fallback) noexcept { return std::isfinite(v) ? v : fallback; }

we_brush_params_t defaultBrushParams() noexcept
{
    we_brush_params_t p{};
    p.channel = WE_BRUSH_CHAN_MASK_R8;
    p.op_mode = WE_BRUSH_OP_ADD_BUILDUP;
    p.radius_px = 32.0f;
    p.hardness = 0.65f;
    p.falloff_power = 1.50f;
    p.opacity = 0.85f;
    p.target_value = 1.0f;
    p.flow_strength = 0.85f;
    p.blur_radius_px = 8.0f;
    p.pressure_size_weight = 0.50f;
    p.pressure_alpha_weight = 0.70f;
    p.velocity_damp_weight = 0.25f;
    p.color_rgba[0] = 1.0f;
    p.color_rgba[1] = 1.0f;
    p.color_rgba[2] = 1.0f;
    p.color_rgba[3] = 1.0f;
    return p;
}

we_brush_params_t sanitizeBrushParams(const we_brush_params_t &in) noexcept
{
    we_brush_params_t p = in;
    if (p.channel > WE_BRUSH_CHAN_SMUDGE) {
        p.channel = WE_BRUSH_CHAN_MASK_R8;
    }
    if (p.op_mode > WE_BRUSH_OP_SMUDGE_ADV) {
        p.op_mode = WE_BRUSH_OP_ADD_BUILDUP;
    }
    p.radius_px = std::clamp(finiteOr(p.radius_px, 32.0f), 1.0f, 512.0f);
    p.hardness = std::clamp(finiteOr(p.hardness, 0.65f), 0.0f, 0.999f);
    p.falloff_power = std::clamp(finiteOr(p.falloff_power, 1.50f), 0.25f, 4.0f);
    p.opacity = std::clamp(finiteOr(p.opacity, 0.85f), 0.0f, 1.0f);
    p.target_value = std::clamp(finiteOr(p.target_value, 1.0f), 0.0f, 1.0f);
    p.flow_strength = std::clamp(finiteOr(p.flow_strength, 0.85f), 0.0f, 1.0f);
    p.blur_radius_px = std::clamp(finiteOr(p.blur_radius_px, 8.0f), 1.0f, 64.0f);
    p.pressure_size_weight = std::clamp(finiteOr(p.pressure_size_weight, 0.50f), 0.0f, 1.0f);
    p.pressure_alpha_weight = std::clamp(finiteOr(p.pressure_alpha_weight, 0.70f), 0.0f, 1.0f);
    p.velocity_damp_weight = std::clamp(finiteOr(p.velocity_damp_weight, 0.25f), 0.0f, 1.0f);
    for (float &c : p.color_rgba) {
        c = std::clamp(finiteOr(c, 1.0f), 0.0f, 1.0f);
    }
    return p;
}

uint32_t bytesPerPixelForBrushTarget(we_brush_channel_t channel, uint32_t texFormat) noexcept
{
    if (texFormat == kTexFmtR8 || channel == WE_BRUSH_CHAN_MASK_R8) {
        return 1u;
    }
    if (texFormat == kTexFmtRg88 || channel == WE_BRUSH_CHAN_FLOW_RG88) {
        return 2u;
    }
    return 4u;
}

we_brush_op_mode_t defaultOpModeForChannel(we_brush_channel_t channel,
                                           uint32_t currentMode) noexcept
{
    switch (channel) {
    case WE_BRUSH_CHAN_MASK_R8:
    case WE_BRUSH_CHAN_COLOR_RGBA8:
        if (currentMode <= WE_BRUSH_OP_ERASE) {
            return static_cast<we_brush_op_mode_t>(currentMode);
        }
        return WE_BRUSH_OP_ADD_BUILDUP;
    case WE_BRUSH_CHAN_FLOW_RG88:
        if (currentMode >= WE_BRUSH_OP_FLOW_PAINT && currentMode <= WE_BRUSH_OP_FLOW_COMB) {
            return static_cast<we_brush_op_mode_t>(currentMode);
        }
        return WE_BRUSH_OP_FLOW_PAINT;
    case WE_BRUSH_CHAN_BLUR:
        return WE_BRUSH_OP_BLUR_MASKED;
    case WE_BRUSH_CHAN_SMUDGE:
        return WE_BRUSH_OP_SMUDGE_ADV;
    }
    return WE_BRUSH_OP_ADD_BUILDUP;
}

float evalFalloffWeight(float normDist, float hardness, float falloffPower, float opacity) noexcept
{
    const float inner = std::clamp(hardness, 0.0f, 0.999f);
    const float edgeT = std::clamp((normDist - inner) / std::max(1.0f - inner, 1e-3f), 0.0f, 1.0f);
    const float shaped = (edgeT <= 0.0f) ? 0.0f : std::pow(edgeT, std::max(falloffPower, 0.05f));
    const float s = shaped * shaped * (3.0f - 2.0f * shaped);
    return std::clamp((1.0f - s) * opacity, 0.0f, 1.0f);
}

QColor brushColorFromParams(const we_brush_params_t &p)
{
    QColor c;
    c.setRgbF(p.color_rgba[0], p.color_rgba[1], p.color_rgba[2], p.color_rgba[3]);
    return c;
}

template <typename Client = IpcClient>
void dispatchBrushIpc(Client *client, const waywal_ipc_req_brush_stroke_t &req,
                      const std::vector<we_brush_dab_t> &dabs)
{
    if (!client) {
        return;
    }

    if constexpr (requires(Client *c) { c->sendBrushStroke(req, dabs); }) {
        client->sendBrushStroke(req, dabs);
    } else if constexpr (requires(Client *c) {
                             c->sendBrushStroke(QString::fromUtf8(req.target_output),
                                                static_cast<we_brush_ipc_action_t>(req.action),
                                                req.object_id, req.pass_index, req.tex_slot,
                                                req.stroke_id, req.params, dabs);
                         }) {
        client->sendBrushStroke(QString::fromUtf8(req.target_output),
                                static_cast<we_brush_ipc_action_t>(req.action), req.object_id,
                                req.pass_index, req.tex_slot, req.stroke_id, req.params, dabs);
    } else {
        (void)req;
        (void)dabs;
    }
}

/* Renders live brush radial falloff and pressure curves using the dark authoring palette. */
class BrushProfilePreviewWidget final : public QWidget {
  public:
    explicit BrushProfilePreviewWidget(const GpuBrushPainterWidget *owner,
                                       QWidget *parent = nullptr)
        : QWidget(parent), m_owner(owner)
    {
        setMinimumHeight(76);
        setMaximumHeight(88);
        setAccessibleName(QStringLiteral("Brush Falloff and Pressure Curve Preview"));
        setAccessibleDescription(
            QStringLiteral("Displays radial hardness falloff and stylus pressure response curves"));
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = rect().adjusted(1.0, 1.0, -1.0, -1.0);
        p.setPen(QPen(QColor(QStringLiteral("#2a2f3d")), 1.0));
        p.setBrush(QColor(QStringLiteral("#0b0c10")));
        p.drawRoundedRect(r, 4.0, 4.0);

        const QRectF plot = r.adjusted(8.0, 8.0, -8.0, -14.0);
        if (plot.width() <= 4.0 || plot.height() <= 4.0) {
            return;
        }

        p.setPen(QPen(QColor(QStringLiteral("#181b22")), 1.0));
        for (int i = 1; i < 4; ++i) {
            const double gx = plot.left() + plot.width() * (0.25 * i);
            const double gy = plot.top() + plot.height() * (0.25 * i);
            p.drawLine(QPointF(gx, plot.top()), QPointF(gx, plot.bottom()));
            p.drawLine(QPointF(plot.left(), gy), QPointF(plot.right(), gy));
        }

        const we_brush_params_t params = m_owner ? m_owner->brushParams() : defaultBrushParams();

        QPainterPath falloffPath;
        QPainterPath pressureSizePath;
        QPainterPath pressureAlphaPath;

        constexpr int kSteps = 48;
        for (int i = 0; i <= kSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(kSteps);
            const double x = plot.left() + plot.width() * u;

            const float fw =
                evalFalloffWeight(u, params.hardness, params.falloff_power, params.opacity);
            const double fy = plot.bottom() - plot.height() * fw;

            const float pSize =
                (1.0f - params.pressure_size_weight) + params.pressure_size_weight * u;
            const double psy = plot.bottom() - plot.height() * pSize;

            const float pAlpha =
                (1.0f - params.pressure_alpha_weight) + params.pressure_alpha_weight * u;
            const double pay = plot.bottom() - plot.height() * pAlpha;

            if (i == 0) {
                falloffPath.moveTo(x, fy);
                pressureSizePath.moveTo(x, psy);
                pressureAlphaPath.moveTo(x, pay);
            } else {
                falloffPath.lineTo(x, fy);
                pressureSizePath.lineTo(x, psy);
                pressureAlphaPath.lineTo(x, pay);
            }
        }

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(QStringLiteral("#22c55e")), 1.3, Qt::DashLine));
        p.drawPath(pressureSizePath);

        p.setPen(QPen(QColor(QStringLiteral("#f59e0b")), 1.3, Qt::DotLine));
        p.drawPath(pressureAlphaPath);

        p.setPen(QPen(QColor(QStringLiteral("#38bdf8")), 2.0));
        p.drawPath(falloffPath);

        p.setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
        p.setPen(QColor(QStringLiteral("#94a3b8")));
        p.drawText(QRectF(plot.left(), plot.bottom() + 1.0, plot.width(), 12.0), Qt::AlignLeft,
                   QStringLiteral("Falloff (Sky)  P->Size (Green)  P->Alpha (Amber)"));
    }

  private:
    const GpuBrushPainterWidget *m_owner{nullptr};
};

QGroupBox *createStyledGroup(const QString &title, QWidget *parent)
{
    auto *group = new QGroupBox(title, parent);
    group->setObjectName(QStringLiteral("brushSectionGroup"));
    return group;
}

} // namespace

GpuBrushPainterWidget::GpuBrushPainterWidget(QWidget *parent)
    : QWidget(parent), m_params(defaultBrushParams())
{
    // Defer QWidget child hierarchy and styling until showEvent() so startup RssAnon stays lean.
}

void GpuBrushPainterWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureUiInitialized();
}

void GpuBrushPainterWidget::setIpcClient(IpcClient *client)
{
    if (m_ipcClient == client) {
        return;
    }
    if (m_readbackConn) {
        disconnect(m_readbackConn);
        m_readbackConn = QMetaObject::Connection{};
    }
    m_ipcClient = client;
    if (m_ipcClient) {
        m_readbackConn = connect(m_ipcClient, &IpcClient::textureReadbackReceived, this,
                                 &GpuBrushPainterWidget::onTextureReadbackReceived);
        requestTargetReadback();
    }
}

void GpuBrushPainterWidget::invalidateHostShadowCache()
{
    m_hostShadowTiles.clear();
    m_lastReadbackPixels.clear();
    m_lastReadbackWidth = 0u;
    m_lastReadbackHeight = 0u;
    m_lastReadbackBpp = 0u;
    m_activeStrokeTiles.clear();
    m_pendingPostReadbackStrokeId = 0u;
    m_pendingPostReadbackTiles.clear();
}

void GpuBrushPainterWidget::requestTargetReadback()
{
    if (m_ipcClient && m_ipcClient->isConnected() && m_targetObjectId >= 0) {
        m_ipcClient->sendReadbackTexture(m_targetOutput, m_targetObjectId, m_targetPassIndex,
                                         m_targetTexSlot, 0u);
    }
}

void GpuBrushPainterWidget::setTargetTextureSlot(int32_t objectId, uint32_t passIndex,
                                                 uint32_t texSlot, uint32_t width, uint32_t height,
                                                 uint32_t texFormat)
{
    const bool targetChanged =
        (m_targetObjectId != objectId) || (m_targetPassIndex != passIndex) ||
        (m_targetTexSlot != texSlot) || (m_canvasWidth != std::max(1u, width)) ||
        (m_canvasHeight != std::max(1u, height)) || (m_targetTexFormat != texFormat);

    m_targetObjectId = objectId;
    m_targetPassIndex = passIndex;
    m_targetTexSlot = texSlot;
    m_canvasWidth = std::max(1u, width);
    m_canvasHeight = std::max(1u, height);
    m_targetTexFormat = texFormat;

    if (targetChanged) {
        invalidateHostShadowCache();
    }

    if (texFormat == kTexFmtR8) {
        m_params.channel = WE_BRUSH_CHAN_MASK_R8;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_MASK_R8, m_params.op_mode);
    } else if (texFormat == kTexFmtRg88) {
        m_params.channel = WE_BRUSH_CHAN_FLOW_RG88;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_FLOW_RG88, m_params.op_mode);
    } else if (texFormat == kTexFmtRgba8888 && (m_params.channel == WE_BRUSH_CHAN_MASK_R8 ||
                                                m_params.channel == WE_BRUSH_CHAN_FLOW_RG88)) {
        m_params.channel = WE_BRUSH_CHAN_COLOR_RGBA8;
        m_params.op_mode = defaultOpModeForChannel(WE_BRUSH_CHAN_COLOR_RGBA8, m_params.op_mode);
    }

    sendBrushIpc(WE_BRUSH_IPC_BIND_TARGET, 0u, {});
    if (targetChanged) {
        requestTargetReadback();
    }
    syncUiFromParams();
    emit targetChannelOverlayRequested(static_cast<we_brush_channel_t>(m_params.channel),
                                       m_showChannelOverlay);
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushChannel(we_brush_channel_t channel)
{
    if (static_cast<uint32_t>(channel) > WE_BRUSH_CHAN_SMUDGE) {
        return;
    }
    m_params.channel = static_cast<uint32_t>(channel);
    m_params.op_mode = defaultOpModeForChannel(channel, m_params.op_mode);
    syncUiFromParams();
    emit targetChannelOverlayRequested(channel, m_showChannelOverlay);
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushOpMode(we_brush_op_mode_t mode)
{
    if (static_cast<uint32_t>(mode) > WE_BRUSH_OP_SMUDGE_ADV) {
        return;
    }
    m_params.op_mode = static_cast<uint32_t>(mode);
    syncUiFromParams();
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::setBrushParams(const we_brush_params_t &params)
{
    m_params = sanitizeBrushParams(params);
    syncUiFromParams();
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::adjustRadiusByFactor(float factor)
{
    if (!std::isfinite(factor) || factor <= 0.0f) {
        return;
    }
    m_params.radius_px = std::clamp(m_params.radius_px * factor, 1.0f, 512.0f);
    syncUiFromParams();
    emit brushParamsChanged(m_params);
}

void GpuBrushPainterWidget::handlePointerPress(const QPointF &texelPos, float stylusPressure,
                                               double timestampSec)
{
    if (m_strokeActive) {
        handlePointerRelease(m_lastTexelPos, m_lastPressure, timestampSec);
    }

    const float p = std::clamp(finiteOr(stylusPressure, 1.0f), 0.0f, 1.0f);
    m_activeStrokeId = m_nextStrokeId++;
    m_strokeActive = true;
    m_lastTexelPos = texelPos;
    m_lastPressure = p;
    m_lastTimestampSec = timestampSec;
    m_emaTangentX = 1.0f;
    m_emaTangentY = 0.0f;
    m_pendingDabs.clear();
    m_activeStrokeTiles.clear();
    m_activeStrokeTileCount = 0u;
    m_lastDirtyRect = we_brush_dirty_rect_t{};

    const auto channel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t bpp = bytesPerPixelForBrushTarget(channel, m_targetTexFormat);
    m_tileUndoStack.beginTransaction(m_activeStrokeId, m_targetObjectId, m_targetPassIndex,
                                     m_targetTexSlot, channel, bpp, m_canvasWidth, m_canvasHeight);

    sendBrushIpc(WE_BRUSH_IPC_BEGIN_STROKE, m_activeStrokeId, {});

    const we_brush_dab_t initialDab = buildDabSegment(texelPos, texelPos, p, p, 0.0);
    m_pendingDabs.push_back(initialDab);
    // Flush the initial tap dab immediately so single clicks preview without drag motion.
    flushDabBatch(false);
}

void GpuBrushPainterWidget::handlePointerMove(const QPointF &texelPos, float stylusPressure,
                                              double timestampSec)
{
    if (!m_strokeActive) {
        return;
    }

    const float p = std::clamp(finiteOr(stylusPressure, 1.0f), 0.0f, 1.0f);
    const double dtSec = std::max(0.0, timestampSec - m_lastTimestampSec);
    const we_brush_dab_t dab = buildDabSegment(m_lastTexelPos, texelPos, m_lastPressure, p, dtSec);
    m_pendingDabs.push_back(dab);

    m_lastTexelPos = texelPos;
    m_lastPressure = p;
    m_lastTimestampSec = timestampSec;

    if (m_pendingDabs.size() >= kDabBatchFlushThreshold) {
        flushDabBatch(false);
    }
}

void GpuBrushPainterWidget::handlePointerRelease(const QPointF &texelPos, float stylusPressure,
                                                 double timestampSec)
{
    if (!m_strokeActive) {
        return;
    }

    const float p = std::clamp(finiteOr(stylusPressure, 1.0f), 0.0f, 1.0f);
    const double dx = texelPos.x() - m_lastTexelPos.x();
    const double dy = texelPos.y() - m_lastTexelPos.y();
    if (std::hypot(dx, dy) > 0.05) {
        const double dtSec = std::max(0.0, timestampSec - m_lastTimestampSec);
        m_pendingDabs.push_back(
            buildDabSegment(m_lastTexelPos, texelPos, m_lastPressure, p, dtSec));
    }

    flushDabBatch(true);
    sendBrushIpc(WE_BRUSH_IPC_END_STROKE, m_activeStrokeId, {});
    for (const DirtyTileCoord &coord : m_activeStrokeTiles) {
        const auto it = m_hostShadowTiles.find(coord);
        if (it != m_hostShadowTiles.end() && !it->second.empty()) {
            m_tileUndoStack.recordPostTileBytes(coord.tx, coord.ty, it->second.data(),
                                                it->second.size());
        }
    }
    m_tileUndoStack.commitTransaction();

    const uint64_t committedStrokeId = m_activeStrokeId;
    const auto committedChannel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t touchedTiles = m_activeStrokeTileCount;

    if (m_ipcClient && m_ipcClient->isConnected() && m_targetObjectId >= 0 &&
        !m_activeStrokeTiles.empty()) {
        m_pendingPostReadbackStrokeId = committedStrokeId;
        m_pendingPostReadbackTiles = m_activeStrokeTiles;
        requestTargetReadback();
    }

    m_strokeActive = false;
    m_activeStrokeId = 0u;
    m_activeStrokeTileCount = 0u;
    m_activeStrokeTiles.clear();
    updateTelemetryReadout();

    if (m_commandBus) {
        QPointer<GpuBrushPainterWidget> self(this);
        auto cmd = std::make_unique<BrushStrokeCommand>(
            committedStrokeId, committedChannel, touchedTiles, [self](uint64_t sid, bool isRedo) {
                if (!self) {
                    return;
                }
                self->sendBrushIpc(isRedo ? WE_BRUSH_IPC_REDO_STROKE : WE_BRUSH_IPC_UNDO_STROKE,
                                   sid, {});
                emit self->statusMessage(isRedo ? tr("Redid brush stroke #%1").arg(sid)
                                                : tr("Undid brush stroke #%1").arg(sid));
            });
        m_commandBus->execute(std::move(cmd));
    }

    updateTelemetryReadout();
}

void GpuBrushPainterWidget::flushDabBatch(bool isFinal)
{
    if (m_pendingDabs.empty()) {
        return;
    }

    we_brush_dirty_rect_t accumRect{};
    size_t offset = 0;
    while (offset < m_pendingDabs.size()) {
        const size_t chunkCount =
            std::min<size_t>(WE_BRUSH_MAX_STROKE_DABS, m_pendingDabs.size() - offset);
        std::vector<we_brush_dab_t> chunk(m_pendingDabs.begin() + static_cast<ptrdiff_t>(offset),
                                          m_pendingDabs.begin() +
                                              static_cast<ptrdiff_t>(offset + chunkCount));

        for (const auto &dab : chunk) {
            touchTilesForDab(dab, accumRect);
        }

        sendBrushIpc(WE_BRUSH_IPC_DAB_BATCH, m_activeStrokeId, chunk);
        offset += chunkCount;
    }

    m_pendingDabs.clear();
    if (accumRect.width > 0 && accumRect.height > 0) {
        m_lastDirtyRect = accumRect;
        emit brushStrokeUpdated(accumRect);
    }

    if (!isFinal) {
        updateTelemetryReadout();
    }
}

we_brush_dab_t GpuBrushPainterWidget::buildDabSegment(const QPointF &a, const QPointF &b,
                                                      float pressureA, float pressureB,
                                                      double dtSec)
{
    const float pA = std::clamp(finiteOr(pressureA, 1.0f), 0.0f, 1.0f);
    const float pB = std::clamp(finiteOr(pressureB, 1.0f), 0.0f, 1.0f);
    const float sizeScaleA =
        (1.0f - m_params.pressure_size_weight) + m_params.pressure_size_weight * pA;
    const float sizeScaleB =
        (1.0f - m_params.pressure_size_weight) + m_params.pressure_size_weight * pB;

    const float dx = static_cast<float>(b.x() - a.x());
    const float dy = static_cast<float>(b.y() - a.y());
    const float ds = std::hypot(dx, dy);

    if (ds > 1e-4f) {
        const float rawTx = dx / ds;
        const float rawTy = dy / ds;
        const float alphaEma = 1.0f - std::exp(-ds / kFlowEmaArcLambdaPx);
        const float nx = m_emaTangentX + alphaEma * (rawTx - m_emaTangentX);
        const float ny = m_emaTangentY + alphaEma * (rawTy - m_emaTangentY);
        const float len = std::hypot(nx, ny);
        if (len > 1e-6f) {
            m_emaTangentX = nx / len;
            m_emaTangentY = ny / len;
        } else {
            m_emaTangentX = rawTx;
            m_emaTangentY = rawTy;
        }
    }

    const float pMid = 0.5f * (pA + pB);
    const float alphaScale =
        (1.0f - m_params.pressure_alpha_weight) + m_params.pressure_alpha_weight * pMid;
    const float speedPxPerSec = (dtSec > 1e-5) ? (ds / static_cast<float>(dtSec)) : 0.0f;
    const float velocityFactor =
        1.0f / (1.0f + m_params.velocity_damp_weight * (speedPxPerSec / 1200.0f));

    we_brush_dab_t dab{};
    dab.ax = static_cast<float>(a.x());
    dab.ay = static_cast<float>(a.y());
    dab.bx = static_cast<float>(b.x());
    dab.by = static_cast<float>(b.y());
    dab.radius_a = std::clamp(m_params.radius_px * sizeScaleA, 0.5f, 512.0f);
    dab.radius_b = std::clamp(m_params.radius_px * sizeScaleB, 0.5f, 512.0f);
    dab.opacity_eff = std::clamp(m_params.opacity * alphaScale * velocityFactor, 0.0f, 1.0f);
    dab.flow_dir_x = m_emaTangentX * m_params.flow_strength;
    dab.flow_dir_y = (m_invertFlowY ? -m_emaTangentY : m_emaTangentY) * m_params.flow_strength;
    dab.drag_dx_px = dx;
    dab.drag_dy_px = dy;
    dab.flags = m_invertFlowY ? 1u : 0u;
    return dab;
}

void GpuBrushPainterWidget::sendBrushIpc(we_brush_ipc_action_t action, uint64_t strokeId,
                                         const std::vector<we_brush_dab_t> &dabs)
{
    if (!m_ipcClient) {
        return;
    }

    waywal_ipc_req_brush_stroke_t req{};
    const QByteArray outBa = m_targetOutput.isEmpty() ? QByteArray("*") : m_targetOutput.toUtf8();
    std::strncpy(req.target_output, outBa.constData(), sizeof(req.target_output) - 1u);
    req.action = static_cast<uint32_t>(action);
    req.object_id = m_targetObjectId;
    req.pass_index = m_targetPassIndex;
    req.tex_slot = m_targetTexSlot;
    req.stroke_id = strokeId;
    req.dab_count = static_cast<uint32_t>(
        std::min<size_t>(dabs.size(), static_cast<size_t>(WE_BRUSH_MAX_STROKE_DABS)));
    req.params = m_params;

    dispatchBrushIpc(m_ipcClient, req, dabs);
}

void GpuBrushPainterWidget::touchTilesForDab(const we_brush_dab_t &dab,
                                             we_brush_dirty_rect_t &accumRect)
{
    const float maxR = std::max(dab.radius_a, dab.radius_b);
    const bool needsBlurHalo =
        (m_params.channel == WE_BRUSH_CHAN_BLUR || m_params.channel == WE_BRUSH_CHAN_SMUDGE);
    const float halo =
        needsBlurHalo ? std::clamp(std::ceil(1.5f * m_params.blur_radius_px), 16.0f, 64.0f) : 2.0f;
    const float pad = maxR + halo;

    const int32_t maxW = static_cast<int32_t>(std::max(1u, m_canvasWidth)) - 1;
    const int32_t maxH = static_cast<int32_t>(std::max(1u, m_canvasHeight)) - 1;

    const int32_t minX =
        std::clamp(static_cast<int32_t>(std::floor(std::min(dab.ax, dab.bx) - pad)), 0, maxW);
    const int32_t minY =
        std::clamp(static_cast<int32_t>(std::floor(std::min(dab.ay, dab.by) - pad)), 0, maxH);
    const int32_t maxX =
        std::clamp(static_cast<int32_t>(std::ceil(std::max(dab.ax, dab.bx) + pad)), 0, maxW);
    const int32_t maxY =
        std::clamp(static_cast<int32_t>(std::ceil(std::max(dab.ay, dab.by) + pad)), 0, maxH);

    const uint16_t tx0 = static_cast<uint16_t>(minX / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t tx1 = static_cast<uint16_t>(maxX / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t ty0 = static_cast<uint16_t>(minY / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));
    const uint16_t ty1 = static_cast<uint16_t>(maxY / static_cast<int32_t>(WE_BRUSH_TILE_SIZE));

    const auto channel = static_cast<we_brush_channel_t>(m_params.channel);
    const uint32_t bpp = bytesPerPixelForBrushTarget(channel, m_targetTexFormat);
    const size_t tileByteLen =
        static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * bpp;

    for (uint16_t ty = ty0; ty <= ty1; ++ty) {
        for (uint16_t tx = tx0; tx <= tx1; ++tx) {
            const DirtyTileCoord coord{tx, ty};
            auto it = m_hostShadowTiles.find(coord);
            if (it == m_hostShadowTiles.end() || it->second.size() != tileByteLen) {
                std::vector<uint8_t> baseline(tileByteLen, 0u);
                if (m_lastReadbackWidth > 0u && m_lastReadbackHeight > 0u &&
                    m_lastReadbackBpp == bpp &&
                    static_cast<size_t>(m_lastReadbackPixels.size()) >=
                        static_cast<size_t>(m_lastReadbackWidth) * m_lastReadbackHeight * bpp) {
                    const uint8_t *srcBase =
                        reinterpret_cast<const uint8_t *>(m_lastReadbackPixels.constData());
                    const uint32_t tileOriginX = static_cast<uint32_t>(tx) * WE_BRUSH_TILE_SIZE;
                    const uint32_t tileOriginY = static_cast<uint32_t>(ty) * WE_BRUSH_TILE_SIZE;
                    const uint32_t copyW =
                        (tileOriginX < m_lastReadbackWidth)
                            ? std::min<uint32_t>(WE_BRUSH_TILE_SIZE,
                                                 m_lastReadbackWidth - tileOriginX)
                            : 0u;
                    const uint32_t copyH =
                        (tileOriginY < m_lastReadbackHeight)
                            ? std::min<uint32_t>(WE_BRUSH_TILE_SIZE,
                                                 m_lastReadbackHeight - tileOriginY)
                            : 0u;
                    for (uint32_t row = 0; row < copyH; ++row) {
                        const size_t srcOff =
                            (static_cast<size_t>(tileOriginY + row) * m_lastReadbackWidth +
                             tileOriginX) *
                            bpp;
                        const size_t dstOff = static_cast<size_t>(row) * WE_BRUSH_TILE_SIZE * bpp;
                        std::memcpy(baseline.data() + dstOff, srcBase + srcOff,
                                    static_cast<size_t>(copyW) * bpp);
                    }
                } else if (channel == WE_BRUSH_CHAN_FLOW_RG88 && bpp >= 2u) {
                    for (size_t off = 0; off + 1u < tileByteLen; off += bpp) {
                        baseline[off] = 128u;
                        baseline[off + 1u] = 128u;
                    }
                }
                it = m_hostShadowTiles.insert_or_assign(coord, std::move(baseline)).first;
            }
            if (m_tileUndoStack.markTileTouched(tx, ty)) {
                ++m_activeStrokeTileCount;
                m_activeStrokeTiles.push_back(coord);
                m_tileUndoStack.recordPreTileBytes(tx, ty, it->second.data(), it->second.size());
            }
            paintDabIntoTile(dab, tx, ty, bpp, it->second);
        }
    }

    const int32_t w = maxX - minX + 1;
    const int32_t h = maxY - minY + 1;
    if (accumRect.width <= 0 || accumRect.height <= 0) {
        accumRect = we_brush_dirty_rect_t{minX, minY, w, h};
    } else {
        const int32_t x0 = std::min(accumRect.x, minX);
        const int32_t y0 = std::min(accumRect.y, minY);
        const int32_t x1 = std::max(accumRect.x + accumRect.width, maxX + 1);
        const int32_t y1 = std::max(accumRect.y + accumRect.height, maxY + 1);
        accumRect = we_brush_dirty_rect_t{x0, y0, x1 - x0, y1 - y0};
    }
}

void GpuBrushPainterWidget::onTextureReadbackReceived(
    const waywal_ipc_resp_readback_texture_t &resp, const QByteArray &data)
{
    if (resp.status_code != 0u || resp.width == 0u || resp.height == 0u || data.isEmpty()) {
        m_pendingPostReadbackStrokeId = 0u;
        m_pendingPostReadbackTiles.clear();
        return;
    }

    const uint64_t pixelCount = static_cast<uint64_t>(resp.width) * resp.height;
    if (pixelCount == 0u || static_cast<uint64_t>(data.size()) < pixelCount) {
        return;
    }
    const uint32_t bpp = static_cast<uint32_t>(static_cast<uint64_t>(data.size()) / pixelCount);
    if (bpp != 1u && bpp != 2u && bpp != 4u) {
        return;
    }

    m_lastReadbackPixels = data;
    m_lastReadbackWidth = resp.width;
    m_lastReadbackHeight = resp.height;
    m_lastReadbackBpp = bpp;

    const size_t tileByteLen =
        static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * bpp;
    const uint8_t *srcBase = reinterpret_cast<const uint8_t *>(data.constData());

    auto extractTile = [&](const DirtyTileCoord &coord, std::vector<uint8_t> &outTile) {
        outTile.assign(tileByteLen, 0u);
        const uint32_t tileOriginX = static_cast<uint32_t>(coord.tx) * WE_BRUSH_TILE_SIZE;
        const uint32_t tileOriginY = static_cast<uint32_t>(coord.ty) * WE_BRUSH_TILE_SIZE;
        if (tileOriginX >= resp.width || tileOriginY >= resp.height) {
            return;
        }
        const uint32_t copyW = std::min<uint32_t>(WE_BRUSH_TILE_SIZE, resp.width - tileOriginX);
        const uint32_t copyH = std::min<uint32_t>(WE_BRUSH_TILE_SIZE, resp.height - tileOriginY);
        for (uint32_t row = 0; row < copyH; ++row) {
            const size_t srcOff =
                (static_cast<size_t>(tileOriginY + row) * resp.width + tileOriginX) * bpp;
            const size_t dstOff = static_cast<size_t>(row) * WE_BRUSH_TILE_SIZE * bpp;
            std::memcpy(outTile.data() + dstOff, srcBase + srcOff,
                        static_cast<size_t>(copyW) * bpp);
        }
    };

    for (auto &kv : m_hostShadowTiles) {
        extractTile(kv.first, kv.second);
    }

    if (m_pendingPostReadbackStrokeId != 0u) {
        const uint64_t strokeId = m_pendingPostReadbackStrokeId;
        const std::vector<DirtyTileCoord> tiles = std::move(m_pendingPostReadbackTiles);
        m_pendingPostReadbackStrokeId = 0u;
        m_pendingPostReadbackTiles.clear();

        std::vector<uint8_t> tileBuf;
        for (const DirtyTileCoord &coord : tiles) {
            extractTile(coord, tileBuf);
            m_hostShadowTiles[coord] = tileBuf;
            m_tileUndoStack.recordPostTileBytesForStroke(strokeId, coord.tx, coord.ty,
                                                         tileBuf.data(), tileBuf.size(), resp.width,
                                                         resp.height);
        }
        updateTelemetryReadout();
    }
}

void GpuBrushPainterWidget::paintDabIntoTile(const we_brush_dab_t &dab, uint16_t tx, uint16_t ty,
                                             uint32_t bpp, std::vector<uint8_t> &tileBytes) const
{
    const size_t expectedLen =
        static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * bpp;
    if (tileBytes.size() != expectedLen || (bpp != 1u && bpp != 2u && bpp != 4u)) {
        return;
    }

    const float maxR = std::max(1.0f, std::max(dab.radius_a, dab.radius_b));
    const float opacity = std::clamp(dab.opacity_eff, 0.0f, 1.0f);
    if (opacity <= 1e-4f) {
        return;
    }

    const int32_t tileX0 = static_cast<int32_t>(tx) * static_cast<int32_t>(WE_BRUSH_TILE_SIZE);
    const int32_t tileY0 = static_cast<int32_t>(ty) * static_cast<int32_t>(WE_BRUSH_TILE_SIZE);
    const int32_t lx0 =
        std::clamp(static_cast<int32_t>(std::floor(std::min(dab.ax, dab.bx) - maxR)) - tileX0, 0,
                   static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);
    const int32_t ly0 =
        std::clamp(static_cast<int32_t>(std::floor(std::min(dab.ay, dab.by) - maxR)) - tileY0, 0,
                   static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);
    const int32_t lx1 =
        std::clamp(static_cast<int32_t>(std::ceil(std::max(dab.ax, dab.bx) + maxR)) - tileX0, 0,
                   static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);
    const int32_t ly1 =
        std::clamp(static_cast<int32_t>(std::ceil(std::max(dab.ay, dab.by) + maxR)) - tileY0, 0,
                   static_cast<int32_t>(WE_BRUSH_TILE_SIZE) - 1);

    const float segDx = dab.bx - dab.ax;
    const float segDy = dab.by - dab.ay;
    const float segLenSq = segDx * segDx + segDy * segDy;
    const bool isErase =
        (m_params.op_mode == WE_BRUSH_OP_ERASE || m_params.op_mode == WE_BRUSH_OP_FLOW_ERASE);
    const float hardness = std::clamp(m_params.hardness, 0.0f, 1.0f);

    for (int32_t ly = ly0; ly <= ly1; ++ly) {
        const float py = static_cast<float>(tileY0 + ly) + 0.5f;
        for (int32_t lx = lx0; lx <= lx1; ++lx) {
            const float px = static_cast<float>(tileX0 + lx) + 0.5f;
            float t = 0.0f;
            if (segLenSq > 1e-6f) {
                t = std::clamp(((px - dab.ax) * segDx + (py - dab.ay) * segDy) / segLenSq, 0.0f,
                               1.0f);
            }
            const float cx = dab.ax + t * segDx;
            const float cy = dab.ay + t * segDy;
            const float r = std::max(1.0f, dab.radius_a + t * (dab.radius_b - dab.radius_a));
            const float dist = std::hypot(px - cx, py - cy);
            if (dist > r) {
                continue;
            }
            const float normDist = dist / r;
            const float edge =
                (normDist <= hardness)
                    ? 1.0f
                    : std::clamp(1.0f - (normDist - hardness) / std::max(1e-4f, 1.0f - hardness),
                                 0.0f, 1.0f);
            const float alpha = opacity * edge;
            const size_t idx =
                (static_cast<size_t>(ly) * WE_BRUSH_TILE_SIZE + static_cast<size_t>(lx)) * bpp;

            if (bpp == 1u) {
                const float target =
                    isErase ? 0.0f : std::clamp(m_params.target_value, 0.0f, 1.0f) * 255.0f;
                const float cur = static_cast<float>(tileBytes[idx]);
                tileBytes[idx] = static_cast<uint8_t>(
                    std::clamp(std::lround(cur + (target - cur) * alpha), 0L, 255L));
            } else if (bpp == 2u) {
                const float flowStrength = std::clamp(m_params.flow_strength, 0.0f, 1.0f);
                const float txByte =
                    isErase
                        ? 128.0f
                        : std::clamp(128.0f + dab.flow_dir_x * 127.0f * flowStrength, 0.0f, 255.0f);
                const float tyByte =
                    isErase
                        ? 128.0f
                        : std::clamp(128.0f + dab.flow_dir_y * 127.0f * flowStrength, 0.0f, 255.0f);
                const float curX = static_cast<float>(tileBytes[idx]);
                const float curY = static_cast<float>(tileBytes[idx + 1u]);
                tileBytes[idx] = static_cast<uint8_t>(
                    std::clamp(std::lround(curX + (txByte - curX) * alpha), 0L, 255L));
                tileBytes[idx + 1u] = static_cast<uint8_t>(
                    std::clamp(std::lround(curY + (tyByte - curY) * alpha), 0L, 255L));
            } else {
                for (size_t c = 0; c < 4u; ++c) {
                    const float target =
                        isErase ? 0.0f : std::clamp(m_params.color_rgba[c], 0.0f, 1.0f) * 255.0f;
                    const float cur = static_cast<float>(tileBytes[idx + c]);
                    tileBytes[idx + c] = static_cast<uint8_t>(
                        std::clamp(std::lround(cur + (target - cur) * alpha), 0L, 255L));
                }
            }
        }
    }
}

void GpuBrushPainterWidget::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;

    setStyleSheet(QStringLiteral(
        "QWidget { background-color: #12141a; color: #f1f5f9; }"
        "QGroupBox#brushSectionGroup { background-color: #181b22; border: 1px solid #2a2f3d; "
        "border-radius: 6px; margin-top: 10px; padding-top: 10px; font-weight: 600; "
        "color: #f1f5f9; }"
        "QGroupBox#brushSectionGroup::title { subcontrol-origin: margin; left: 8px; padding: 0 "
        "4px; "
        "color: #94a3b8; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background-color: #181b22; color: #f1f5f9; "
        "border: 1px solid #2a2f3d; border-radius: 4px; padding: 3px 6px; }"
        "QComboBox:hover, QDoubleSpinBox:hover, QPushButton:hover { background-color: #222631; }"
        "QComboBox:focus, QDoubleSpinBox:focus, QPushButton:focus, QCheckBox:focus, "
        "QSlider:focus { border: 2px solid #38bdf8; outline: none; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled, "
        "QCheckBox:disabled { color: #64748b; border-color: #222631; }"
        "QDoubleSpinBox { font-family: 'JetBrains Mono', monospace; font-size: 12px; }"
        "QPushButton[activeToggle=\"true\"] { border: 1px solid #38bdf8; "
        "background-color: #222631; color: #38bdf8; }"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(6);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(container);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(8);

    // 1. Channel & Operation Mode
    auto *modeGroup = createStyledGroup(tr("Target Channel & Brush Mode"), container);
    auto *modeLayout = new QGridLayout(modeGroup);
    modeLayout->setContentsMargins(8, 8, 8, 8);
    modeLayout->setSpacing(6);

    m_channelCombo = new QComboBox(modeGroup);
    m_channelCombo->addItem(tr("Mask (R8)"), WE_BRUSH_CHAN_MASK_R8);
    m_channelCombo->addItem(tr("Flow Tangent (RG88)"), WE_BRUSH_CHAN_FLOW_RG88);
    m_channelCombo->addItem(tr("Color Albedo (RGBA8)"), WE_BRUSH_CHAN_COLOR_RGBA8);
    m_channelCombo->addItem(tr("Blur (Kawase / Gauss)"), WE_BRUSH_CHAN_BLUR);
    m_channelCombo->addItem(tr("Smudge (Advect)"), WE_BRUSH_CHAN_SMUDGE);
    m_channelCombo->setAccessibleName(tr("Brush Channel Target"));
    m_channelCombo->setAccessibleDescription(
        tr("Select R8 mask, RG88 flow map, RGBA8 color, blur, or smudge channel"));
    modeLayout->addWidget(new QLabel(tr("Channel:"), modeGroup), 0, 0);
    modeLayout->addWidget(m_channelCombo, 0, 1, 1, 2);

    m_opModeCombo = new QComboBox(modeGroup);
    m_opModeCombo->addItem(tr("Add Buildup"), WE_BRUSH_OP_ADD_BUILDUP);
    m_opModeCombo->addItem(tr("Max Cap"), WE_BRUSH_OP_MAX_CAP);
    m_opModeCombo->addItem(tr("Subtract"), WE_BRUSH_OP_SUBTRACT);
    m_opModeCombo->addItem(tr("Erase"), WE_BRUSH_OP_ERASE);
    m_opModeCombo->addItem(tr("Flow Paint"), WE_BRUSH_OP_FLOW_PAINT);
    m_opModeCombo->addItem(tr("Flow Erase (128,128)"), WE_BRUSH_OP_FLOW_ERASE);
    m_opModeCombo->addItem(tr("Flow Comb (Add)"), WE_BRUSH_OP_FLOW_COMB);
    m_opModeCombo->addItem(tr("Blur Masked"), WE_BRUSH_OP_BLUR_MASKED);
    m_opModeCombo->addItem(tr("Smudge Advect"), WE_BRUSH_OP_SMUDGE_ADV);
    m_opModeCombo->setAccessibleName(tr("Brush Blend Operation"));
    m_opModeCombo->setAccessibleDescription(tr("Choose blending operation for the active channel"));
    modeLayout->addWidget(new QLabel(tr("Operation:"), modeGroup), 1, 0);
    modeLayout->addWidget(m_opModeCombo, 1, 1);

    m_eraserBtn = new QPushButton(tr("Eraser"), modeGroup);
    m_eraserBtn->setCheckable(true);
    m_eraserBtn->setAccessibleName(tr("Toggle Eraser Mode"));
    m_eraserBtn->setAccessibleDescription(
        tr("Switches between painting and erasing on the active R8, RG88, or RGBA8 channel"));
    modeLayout->addWidget(m_eraserBtn, 1, 2);

    m_overlayCheck = new QCheckBox(tr("Show Channel Overlay"), modeGroup);
    m_overlayCheck->setChecked(m_showChannelOverlay);
    m_overlayCheck->setAccessibleName(tr("Show Channel Overlay"));
    modeLayout->addWidget(m_overlayCheck, 2, 0, 1, 3);

    contentLayout->addWidget(modeGroup);

    // 2. Tip Profile & Core Stroke Parameters
    auto *tipGroup = createStyledGroup(tr("Brush Tip & Falloff"), container);
    auto *tipLayout = new QGridLayout(tipGroup);
    tipLayout->setContentsMargins(8, 8, 8, 8);
    tipLayout->setSpacing(6);

    m_profilePreview = new BrushProfilePreviewWidget(this, tipGroup);
    tipLayout->addWidget(m_profilePreview, 0, 0, 1, 3);

    m_radiusSlider = new QSlider(Qt::Horizontal, tipGroup);
    m_radiusSlider->setRange(1, 512);
    m_radiusSlider->setAccessibleName(tr("Brush Radius Slider"));
    m_radiusSpin = new QDoubleSpinBox(tipGroup);
    m_radiusSpin->setRange(1.0, 512.0);
    m_radiusSpin->setDecimals(1);
    m_radiusSpin->setSingleStep(2.0);
    m_radiusSpin->setSuffix(QStringLiteral(" px"));
    m_radiusSpin->setAccessibleName(tr("Brush Radius Pixels"));
    tipLayout->addWidget(new QLabel(tr("Radius:"), tipGroup), 1, 0);
    tipLayout->addWidget(m_radiusSlider, 1, 1);
    tipLayout->addWidget(m_radiusSpin, 1, 2);

    auto configureSpin = [](QDoubleSpinBox *s, double lo, double hi, double step, int dec,
                            const QString &name) {
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(dec);
        s->setAccessibleName(name);
    };

    m_hardnessSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_hardnessSpin, 0.0, 0.999, 0.05, 3, tr("Brush Hardness"));
    tipLayout->addWidget(new QLabel(tr("Hardness:"), tipGroup), 2, 0);
    tipLayout->addWidget(m_hardnessSpin, 2, 1, 1, 2);

    m_falloffSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_falloffSpin, 0.25, 4.0, 0.1, 2, tr("Edge Falloff Exponent"));
    tipLayout->addWidget(new QLabel(tr("Falloff Power:"), tipGroup), 3, 0);
    tipLayout->addWidget(m_falloffSpin, 3, 1, 1, 2);

    m_opacitySpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_opacitySpin, 0.0, 1.0, 0.05, 2, tr("Brush Opacity"));
    tipLayout->addWidget(new QLabel(tr("Opacity / Flow:"), tipGroup), 4, 0);
    tipLayout->addWidget(m_opacitySpin, 4, 1, 1, 2);

    m_targetValueSpin = new QDoubleSpinBox(tipGroup);
    configureSpin(m_targetValueSpin, 0.0, 1.0, 0.05, 2, tr("R8 Mask Target Value"));
    tipLayout->addWidget(new QLabel(tr("Mask Target:"), tipGroup), 5, 0);
    tipLayout->addWidget(m_targetValueSpin, 5, 1, 1, 2);

    m_colorBtn = new QPushButton(tr("Paint Color..."), tipGroup);
    m_colorBtn->setAccessibleName(tr("Select RGBA8 Paint Color"));
    tipLayout->addWidget(new QLabel(tr("RGBA8 Color:"), tipGroup), 6, 0);
    tipLayout->addWidget(m_colorBtn, 6, 1, 1, 2);

    contentLayout->addWidget(tipGroup);

    // 3. Flow Tangent & Blur / Smudge
    auto *fxGroup = createStyledGroup(tr("Flow Tangent & Blur / Smudge"), container);
    auto *fxLayout = new QGridLayout(fxGroup);
    fxLayout->setContentsMargins(8, 8, 8, 8);
    fxLayout->setSpacing(6);

    m_flowStrengthSpin = new QDoubleSpinBox(fxGroup);
    configureSpin(m_flowStrengthSpin, 0.0, 1.0, 0.05, 2, tr("RG88 Flow Tangent Strength"));
    fxLayout->addWidget(new QLabel(tr("Flow Strength:"), fxGroup), 0, 0);
    fxLayout->addWidget(m_flowStrengthSpin, 0, 1);

    m_invertFlowYCheck = new QCheckBox(tr("Invert Flow Y"), fxGroup);
    m_invertFlowYCheck->setAccessibleName(tr("Invert RG88 Flow Y Vector"));
    fxLayout->addWidget(m_invertFlowYCheck, 1, 0, 1, 2);

    m_resetFlowRestBtn = new QPushButton(tr("Reset Flow Rest (128, 128)"), fxGroup);
    m_resetFlowRestBtn->setAccessibleName(tr("Reset Flow to Neutral 128, 128 Rest"));
    fxLayout->addWidget(m_resetFlowRestBtn, 2, 0, 1, 2);

    m_blurRadiusSpin = new QDoubleSpinBox(fxGroup);
    configureSpin(m_blurRadiusSpin, 1.0, 64.0, 1.0, 1, tr("Dual-Kawase and Gaussian Blur Radius"));
    m_blurRadiusSpin->setSuffix(QStringLiteral(" px"));
    m_blurRadiusSpin->setToolTip(
        tr("<= 4 px uses 5-tap bilinear Gaussian; > 4 px uses 4-level Dual-Kawase pyramid"));
    fxLayout->addWidget(new QLabel(tr("Blur Radius:"), fxGroup), 3, 0);
    fxLayout->addWidget(m_blurRadiusSpin, 3, 1);

    contentLayout->addWidget(fxGroup);

    // 4. Stylus Pressure & Velocity Curves
    auto *dynGroup = createStyledGroup(tr("Stylus Pressure & Velocity Curves"), container);
    auto *dynLayout = new QGridLayout(dynGroup);
    dynLayout->setContentsMargins(8, 8, 8, 8);
    dynLayout->setSpacing(6);

    m_pressureSizeSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_pressureSizeSpin, 0.0, 1.0, 0.05, 2, tr("Pressure to Radius Coupling"));
    dynLayout->addWidget(new QLabel(tr("Pressure -> Size:"), dynGroup), 0, 0);
    dynLayout->addWidget(m_pressureSizeSpin, 0, 1);

    m_pressureAlphaSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_pressureAlphaSpin, 0.0, 1.0, 0.05, 2, tr("Pressure to Opacity Coupling"));
    dynLayout->addWidget(new QLabel(tr("Pressure -> Opacity:"), dynGroup), 1, 0);
    dynLayout->addWidget(m_pressureAlphaSpin, 1, 1);

    m_velocityDampSpin = new QDoubleSpinBox(dynGroup);
    configureSpin(m_velocityDampSpin, 0.0, 1.0, 0.05, 2, tr("Pointer Velocity Damping"));
    dynLayout->addWidget(new QLabel(tr("Velocity Damping:"), dynGroup), 2, 0);
    dynLayout->addWidget(m_velocityDampSpin, 2, 1);

    contentLayout->addWidget(dynGroup);
    contentLayout->addStretch(1);

    scrollArea->setWidget(container);
    rootLayout->addWidget(scrollArea, 1);

    m_telemetryLabel = new QLabel(this);
    m_telemetryLabel->setWordWrap(true);
    m_telemetryLabel->setStyleSheet(
        QStringLiteral("QLabel { font-family: 'JetBrains Mono', monospace; font-size: 11px; "
                       "color: #94a3b8; background-color: #181b22; border: 1px solid #2a2f3d; "
                       "border-radius: 4px; padding: 6px; }"));
    m_telemetryLabel->setAccessibleName(tr("Brush Tile Cache Telemetry"));
    rootLayout->addWidget(m_telemetryLabel);

    setTabOrder(m_channelCombo, m_opModeCombo);
    setTabOrder(m_opModeCombo, m_eraserBtn);
    setTabOrder(m_eraserBtn, m_overlayCheck);
    setTabOrder(m_overlayCheck, m_radiusSlider);
    setTabOrder(m_radiusSlider, m_radiusSpin);
    setTabOrder(m_radiusSpin, m_hardnessSpin);
    setTabOrder(m_hardnessSpin, m_falloffSpin);
    setTabOrder(m_falloffSpin, m_opacitySpin);
    setTabOrder(m_opacitySpin, m_targetValueSpin);
    setTabOrder(m_targetValueSpin, m_colorBtn);
    setTabOrder(m_colorBtn, m_flowStrengthSpin);
    setTabOrder(m_flowStrengthSpin, m_invertFlowYCheck);
    setTabOrder(m_invertFlowYCheck, m_resetFlowRestBtn);
    setTabOrder(m_resetFlowRestBtn, m_blurRadiusSpin);
    setTabOrder(m_blurRadiusSpin, m_pressureSizeSpin);
    setTabOrder(m_pressureSizeSpin, m_pressureAlphaSpin);
    setTabOrder(m_pressureAlphaSpin, m_velocityDampSpin);

    connect(m_channelCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updatingUi || idx < 0) {
            return;
        }
        setBrushChannel(static_cast<we_brush_channel_t>(m_channelCombo->itemData(idx).toUInt()));
    });

    connect(m_opModeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updatingUi || idx < 0) {
            return;
        }
        setBrushOpMode(static_cast<we_brush_op_mode_t>(m_opModeCombo->itemData(idx).toUInt()));
    });

    connect(m_eraserBtn, &QPushButton::toggled, this, [this](bool checked) {
        if (m_updatingUi) {
            return;
        }
        if (m_params.channel == WE_BRUSH_CHAN_FLOW_RG88) {
            setBrushOpMode(checked ? WE_BRUSH_OP_FLOW_ERASE : WE_BRUSH_OP_FLOW_PAINT);
        } else {
            setBrushOpMode(checked ? WE_BRUSH_OP_ERASE : WE_BRUSH_OP_ADD_BUILDUP);
        }
    });

    connect(m_overlayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_showChannelOverlay = checked;
        emit targetChannelOverlayRequested(static_cast<we_brush_channel_t>(m_params.channel),
                                           checked);
    });

    connect(m_radiusSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updatingUi) {
            return;
        }
        m_params.radius_px = static_cast<float>(std::clamp(value, 1, 512));
        syncUiFromParams();
        emit brushParamsChanged(m_params);
    });

    auto bindSpin = [this](QDoubleSpinBox *spin, auto setter) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) {
                    if (m_updatingUi) {
                        return;
                    }
                    setter(static_cast<float>(v));
                    m_params = sanitizeBrushParams(m_params);
                    syncUiFromParams();
                    emit brushParamsChanged(m_params);
                });
    };

    bindSpin(m_radiusSpin, [this](float v) { m_params.radius_px = v; });
    bindSpin(m_hardnessSpin, [this](float v) { m_params.hardness = v; });
    bindSpin(m_falloffSpin, [this](float v) { m_params.falloff_power = v; });
    bindSpin(m_opacitySpin, [this](float v) { m_params.opacity = v; });
    bindSpin(m_targetValueSpin, [this](float v) { m_params.target_value = v; });
    bindSpin(m_flowStrengthSpin, [this](float v) { m_params.flow_strength = v; });
    bindSpin(m_blurRadiusSpin, [this](float v) { m_params.blur_radius_px = v; });
    bindSpin(m_pressureSizeSpin, [this](float v) { m_params.pressure_size_weight = v; });
    bindSpin(m_pressureAlphaSpin, [this](float v) { m_params.pressure_alpha_weight = v; });
    bindSpin(m_velocityDampSpin, [this](float v) { m_params.velocity_damp_weight = v; });

    connect(m_invertFlowYCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updatingUi) {
            return;
        }
        m_invertFlowY = checked;
        emit brushParamsChanged(m_params);
    });

    connect(m_resetFlowRestBtn, &QPushButton::clicked, this, [this]() {
        m_emaTangentX = 1.0f;
        m_emaTangentY = 0.0f;
        setBrushChannel(WE_BRUSH_CHAN_FLOW_RG88);
        setBrushOpMode(WE_BRUSH_OP_FLOW_ERASE);
        emit statusMessage(tr("Flow brush set to neutral rest (128, 128)"));
    });

    connect(m_colorBtn, &QPushButton::clicked, this, [this]() {
        const QColor chosen = OklchColorDialog::getColor(brushColorFromParams(m_params), this,
                                                         tr("Brush Paint Color"));
        if (!chosen.isValid()) {
            return;
        }
        m_params.color_rgba[0] = static_cast<float>(chosen.redF());
        m_params.color_rgba[1] = static_cast<float>(chosen.greenF());
        m_params.color_rgba[2] = static_cast<float>(chosen.blueF());
        m_params.color_rgba[3] = static_cast<float>(chosen.alphaF());
        syncUiFromParams();
        emit brushParamsChanged(m_params);
    });

    syncUiFromParams();
}

void GpuBrushPainterWidget::syncUiFromParams()
{
    if (!m_uiInitialized) {
        return;
    }

    const QScopedValueRollback<bool> guard(m_updatingUi, true);

    auto selectComboByData = [](QComboBox *combo, uint32_t dataVal) {
        const QSignalBlocker blocker(combo);
        const int idx = combo->findData(dataVal);
        if (idx >= 0) {
            combo->setCurrentIndex(idx);
        }
    };

    selectComboByData(m_channelCombo, m_params.channel);
    selectComboByData(m_opModeCombo, m_params.op_mode);

    const bool isEraser =
        (m_params.op_mode == WE_BRUSH_OP_ERASE || m_params.op_mode == WE_BRUSH_OP_FLOW_ERASE);
    {
        const QSignalBlocker blocker(m_eraserBtn);
        m_eraserBtn->setChecked(isEraser);
    }

    {
        const QSignalBlocker blocker(m_radiusSlider);
        m_radiusSlider->setValue(static_cast<int>(std::lround(m_params.radius_px)));
    }
    auto setSpin = [](QDoubleSpinBox *spin, float val) {
        const QSignalBlocker blocker(spin);
        spin->setValue(static_cast<double>(val));
    };

    setSpin(m_radiusSpin, m_params.radius_px);
    setSpin(m_hardnessSpin, m_params.hardness);
    setSpin(m_falloffSpin, m_params.falloff_power);
    setSpin(m_opacitySpin, m_params.opacity);
    setSpin(m_targetValueSpin, m_params.target_value);
    setSpin(m_flowStrengthSpin, m_params.flow_strength);
    setSpin(m_blurRadiusSpin, m_params.blur_radius_px);
    setSpin(m_pressureSizeSpin, m_params.pressure_size_weight);
    setSpin(m_pressureAlphaSpin, m_params.pressure_alpha_weight);
    setSpin(m_velocityDampSpin, m_params.velocity_damp_weight);

    const bool isMask = (m_params.channel == WE_BRUSH_CHAN_MASK_R8);
    const bool isFlow = (m_params.channel == WE_BRUSH_CHAN_FLOW_RG88);
    const bool isColor = (m_params.channel == WE_BRUSH_CHAN_COLOR_RGBA8);
    const bool isBlurOrSmudge =
        (m_params.channel == WE_BRUSH_CHAN_BLUR || m_params.channel == WE_BRUSH_CHAN_SMUDGE);

    m_targetValueSpin->setEnabled(isMask);
    m_colorBtn->setEnabled(isColor);
    m_flowStrengthSpin->setEnabled(isFlow);
    m_invertFlowYCheck->setEnabled(isFlow);
    m_resetFlowRestBtn->setEnabled(isFlow);
    m_blurRadiusSpin->setEnabled(isBlurOrSmudge);
    m_eraserBtn->setEnabled(!isBlurOrSmudge);

    const QColor col = brushColorFromParams(m_params);
    m_colorBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; color: %2; font-weight: 600; }"
                       "QPushButton:disabled { background-color: #181b22; color: #64748b; "
                       "border-color: #222631; }")
            .arg(col.name(QColor::HexRgb),
                 col.lightnessF() > 0.5 ? QStringLiteral("#0b0c10") : QStringLiteral("#f1f5f9")));

    if (m_profilePreview) {
        m_profilePreview->update();
    }
    updateTelemetryReadout();
}

void GpuBrushPainterWidget::updateTelemetryReadout()
{
    if (!m_telemetryLabel) {
        return;
    }
    const double cacheMb =
        static_cast<double>(m_tileUndoStack.totalHostBytes()) / (1024.0 * 1024.0);
    m_telemetryLabel->setText(tr("Canvas: %1x%2 | Slot: obj=%3 pass=%4 tex=%5\n"
                                 "CoW Strokes: %6 | Host Cache: %7 / 64.00 MiB")
                                  .arg(m_canvasWidth)
                                  .arg(m_canvasHeight)
                                  .arg(m_targetObjectId)
                                  .arg(m_targetPassIndex == 0xFFFFFFFFu
                                           ? QStringLiteral("base")
                                           : QString::number(m_targetPassIndex))
                                  .arg(m_targetTexSlot)
                                  .arg(m_tileUndoStack.transactionCount())
                                  .arg(cacheMb, 0, 'f', 2));
}

} // namespace waywal::studio
