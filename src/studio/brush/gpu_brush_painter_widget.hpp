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
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace waywal::studio {

class OklchColorDialog;

class GpuBrushPainterWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr float kFlowEmaArcLambdaPx = 8.0f;
    static constexpr size_t kMaxShadowCacheBytes = 16u * 1024u * 1024u;

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
    [[nodiscard]] size_t hostShadowBytes() const noexcept { return m_hostShadowBytes; }
    [[nodiscard]] size_t hostShadowTileCount() const noexcept { return m_hostShadowTiles.size(); }
    [[nodiscard]] const std::vector<uint8_t> *findHostShadowTile(uint16_t tx,
                                                                 uint16_t ty) const noexcept;

    /* Pointer stroke capture routed from GpuPreviewViewport in content pixel coordinates */
    void handlePointerPress(const QPointF &texelPos, float stylusPressure, double timestampSec);
    void handlePointerMove(const QPointF &texelPos, float stylusPressure, double timestampSec);
    void handlePointerRelease(const QPointF &texelPos, float stylusPressure, double timestampSec);

    /* Extended stylus capture preserving tablet tilt and barrel rotation without breaking 3-arg
     * slots. */
    void setStylusTiltAndRotation(float xTiltDeg, float yTiltDeg, float rotationDeg) noexcept;
    void handlePointerPressWithTilt(const QPointF &texelPos, float stylusPressure,
                                    double timestampSec, float xTiltDeg, float yTiltDeg,
                                    float rotationDeg);
    void handlePointerMoveWithTilt(const QPointF &texelPos, float stylusPressure,
                                   double timestampSec, float xTiltDeg, float yTiltDeg,
                                   float rotationDeg);
    void handlePointerReleaseWithTilt(const QPointF &texelPos, float stylusPressure,
                                      double timestampSec, float xTiltDeg, float yTiltDeg,
                                      float rotationDeg);

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
    struct ShadowTileEntry {
        std::vector<uint8_t> bytes;
        std::list<DirtyTileCoord>::iterator lruIt;
    };

    struct PendingReadbackRequest {
        uint64_t strokeId{0};
        uint64_t targetGeneration{0};
        int32_t objectId{-1};
        uint32_t passIndex{0xFFFFFFFFu};
        uint32_t texSlot{0};
        std::vector<DirtyTileCoord> tiles;
    };

    void ensureUiInitialized();
    void syncUiFromParams();
    void flushDabBatch(bool isFinal);
    [[nodiscard]] we_brush_dab_t buildDabSegment(const QPointF &a, const QPointF &b,
                                                 float pressureA, float pressureB, double dtSec);
    void sendBrushIpc(we_brush_ipc_action_t action, uint64_t strokeId,
                      const std::vector<we_brush_dab_t> &dabs);

    void touchTilesForDab(const we_brush_dab_t &dab, we_brush_dirty_rect_t &accumRect);
    void upsertHostShadowTile(const DirtyTileCoord &coord, std::vector<uint8_t> bytes);
    void evictShadowTilesIfOverBudget();
    void applyHostStrokeHistory(uint64_t strokeId, bool isRedo);
    void invalidateHostShadowCache();
    void requestTargetReadback(uint64_t strokeId = 0u, std::vector<DirtyTileCoord> tiles = {});
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
    uint64_t m_targetGeneration{1};

    we_brush_params_t m_params{};
    we_brush_op_mode_t m_lastMaskOpMode{WE_BRUSH_OP_ADD_BUILDUP};
    we_brush_op_mode_t m_lastFlowOpMode{WE_BRUSH_OP_FLOW_PAINT};
    bool m_invertFlowY{false};
    bool m_showChannelOverlay{true};

    bool m_strokeActive{false};
    uint64_t m_nextStrokeId{1};
    uint64_t m_activeStrokeId{0};
    QPointF m_lastTexelPos;
    float m_lastPressure{1.0f};
    double m_lastTimestampSec{0.0};
    float m_stylusTiltXDeg{0.0f};
    float m_stylusTiltYDeg{0.0f};
    float m_stylusRotationDeg{0.0f};
    float m_emaTangentX{1.0f};
    float m_emaTangentY{0.0f};
    std::vector<we_brush_dab_t> m_pendingDabs;
    uint32_t m_activeStrokeTileCount{0};
    we_brush_dirty_rect_t m_lastDirtyRect{};

    DirtyTileUndoStack m_tileUndoStack;
    std::list<DirtyTileCoord> m_shadowLru;
    std::unordered_map<DirtyTileCoord, ShadowTileEntry, DirtyTileCoordHash> m_hostShadowTiles;
    size_t m_hostShadowBytes{0};
    QByteArray m_lastReadbackPixels;
    uint32_t m_lastReadbackWidth{0};
    uint32_t m_lastReadbackHeight{0};
    uint32_t m_lastReadbackBpp{0};
    std::vector<DirtyTileCoord> m_activeStrokeTiles;
    std::unordered_set<DirtyTileCoord, DirtyTileCoordHash> m_activeStrokeTileSet;
    std::deque<PendingReadbackRequest> m_inflightReadbacks;
    QMetaObject::Connection m_readbackConn;
    QPointer<OklchColorDialog> m_colorDialog;
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
