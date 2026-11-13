// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_BRUSH_GPU_BRUSH_PAINTER_WIDGET_HPP
#define WAYWAL_STUDIO_BRUSH_GPU_BRUSH_PAINTER_WIDGET_HPP

#include "studio/brush/dirty_tile_undo_stack.hpp"
#include "studio/core/command_bus_authoring.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/we_gpu_brush.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMetaObject>
#include <QPointF>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

class GpuBrushPainterWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr float kFlowEmaArcLambdaPx = 8.0f;

    explicit GpuBrushPainterWidget(QWidget *parent = nullptr);
    ~GpuBrushPainterWidget() override = default;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client);
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void setTargetTextureSlot(int32_t objectId, uint32_t passIndex, uint32_t texSlot,
                              uint32_t width, uint32_t height, uint32_t texFormat);
    void setBrushChannel(we_brush_channel_t channel);
    void setBrushOpMode(we_brush_op_mode_t mode);
    void setBrushParams(const we_brush_params_t &params);
    [[nodiscard]] const we_brush_params_t &brushParams() const noexcept { return m_params; }

    [[nodiscard]] const DirtyTileUndoStack &tileUndoStack() const noexcept
    {
        return m_tileUndoStack;
    }
    [[nodiscard]] DirtyTileUndoStack &tileUndoStack() noexcept { return m_tileUndoStack; }

    /* Pointer stroke capture routed from GpuPreviewViewport in content pixel coordinates */
    void handlePointerPress(const QPointF &texelPos, float stylusPressure, double timestampSec);
    void handlePointerMove(const QPointF &texelPos, float stylusPressure, double timestampSec);
    void handlePointerRelease(const QPointF &texelPos, float stylusPressure, double timestampSec);

    /* Quick keyboard radius step ([ and ] keys) */
    void adjustRadiusByFactor(float factor);
    [[nodiscard]] bool isStrokeActive() const noexcept { return m_strokeActive; }

  signals:
    void brushParamsChanged(const we_brush_params_t &params);
    void brushStrokeUpdated(const we_brush_dirty_rect_t &dirtyRect);
    void targetChannelOverlayRequested(we_brush_channel_t channel, bool showOverlay);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void ensureUiInitialized();
    void syncUiFromParams();
    void flushDabBatch(bool isFinal);
    [[nodiscard]] we_brush_dab_t buildDabSegment(const QPointF &a, const QPointF &b,
                                                 float pressureA, float pressureB, double dtSec);
    void sendBrushIpc(we_brush_ipc_action_t action, uint64_t strokeId,
                      const std::vector<we_brush_dab_t> &dabs);

    void touchTilesForDab(const we_brush_dab_t &dab, we_brush_dirty_rect_t &accumRect);
    void invalidateHostShadowCache();
    void requestTargetReadback();
    void onTextureReadbackReceived(const waywal_ipc_resp_readback_texture_t &resp,
                                   const QByteArray &data);
    void paintDabIntoTile(const we_brush_dab_t &dab, uint16_t tx, uint16_t ty, uint32_t bpp,
                          std::vector<uint8_t> &tileBytes) const;
    void updateTelemetryReadout();

    bool m_uiInitialized{false};
    bool m_updatingUi{false};
    QString m_targetOutput{QStringLiteral("*")};
    int32_t m_targetObjectId{-1};
    uint32_t m_targetPassIndex{0xFFFFFFFFu};
    uint32_t m_targetTexSlot{0};
    uint32_t m_canvasWidth{1920};
    uint32_t m_canvasHeight{1080};
    uint32_t m_targetTexFormat{0};

    we_brush_params_t m_params{};
    bool m_invertFlowY{false};
    bool m_showChannelOverlay{true};

    bool m_strokeActive{false};
    uint64_t m_nextStrokeId{1};
    uint64_t m_activeStrokeId{0};
    QPointF m_lastTexelPos;
    float m_lastPressure{1.0f};
    double m_lastTimestampSec{0.0};
    float m_emaTangentX{1.0f};
    float m_emaTangentY{0.0f};
    std::vector<we_brush_dab_t> m_pendingDabs;
    uint32_t m_activeStrokeTileCount{0};
    we_brush_dirty_rect_t m_lastDirtyRect{};

    DirtyTileUndoStack m_tileUndoStack;
    std::unordered_map<DirtyTileCoord, std::vector<uint8_t>, DirtyTileCoordHash> m_hostShadowTiles;
    QByteArray m_lastReadbackPixels;
    uint32_t m_lastReadbackWidth{0};
    uint32_t m_lastReadbackHeight{0};
    uint32_t m_lastReadbackBpp{0};
    std::vector<DirtyTileCoord> m_activeStrokeTiles;
    uint64_t m_pendingPostReadbackStrokeId{0};
    std::vector<DirtyTileCoord> m_pendingPostReadbackTiles;
    QMetaObject::Connection m_readbackConn;
    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QComboBox *m_channelCombo{nullptr};
    QComboBox *m_opModeCombo{nullptr};
    QSlider *m_radiusSlider{nullptr};
    QDoubleSpinBox *m_radiusSpin{nullptr};
    QDoubleSpinBox *m_hardnessSpin{nullptr};
    QDoubleSpinBox *m_falloffSpin{nullptr};
    QDoubleSpinBox *m_opacitySpin{nullptr};
    QDoubleSpinBox *m_targetValueSpin{nullptr};
    QDoubleSpinBox *m_flowStrengthSpin{nullptr};
    QDoubleSpinBox *m_blurRadiusSpin{nullptr};
    QDoubleSpinBox *m_pressureSizeSpin{nullptr};
    QDoubleSpinBox *m_pressureAlphaSpin{nullptr};
    QDoubleSpinBox *m_velocityDampSpin{nullptr};
    QCheckBox *m_invertFlowYCheck{nullptr};
    QCheckBox *m_overlayCheck{nullptr};
    QLabel *m_telemetryLabel{nullptr};
    QPushButton *m_resetFlowRestBtn{nullptr};
    QPushButton *m_eraserBtn{nullptr};
    QPushButton *m_colorBtn{nullptr};
    QWidget *m_profilePreview{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_BRUSH_GPU_BRUSH_PAINTER_WIDGET_HPP
