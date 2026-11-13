// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QTabletEvent>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QTimer>
#include <QPointF>
#include <QRectF>
#include <QColor>
#include <QString>
#include <QStringList>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QElapsedTimer>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QMatrix4x4>

#include <atomic>
#include <memory>
#include <array>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <chrono>
#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#if __has_include(<drm_fourcc.h>)
#include <drm_fourcc.h>
#elif __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#endif

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

#include "studio/core/rigging_commands.hpp"
#include "studio/viewport/viewport_gizmo_overlay.hpp"
#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_rigging.h"
#include "waywal/we_mesh_cdt.h"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

namespace waywal::studio {

struct WeightBrushDab {
    float texelX{0.0f};
    float texelY{0.0f};
    float radiusPx{32.0f};
    float strength{0.5f};
    float targetWeight{1.0f};
    float pressure{1.0f};
    uint32_t activeBoneIndex{0};
    we_weight_brush_op_t op{WE_WEIGHT_OP_ADD};
    double timestampSec{0.0};
};

enum class PreviewMode { OfflineEmpty, DmaBufLive, InProcessImage, InProcessShader };

enum class AspectRatioGuide {
    None,
    Ratio16_9,
    Ratio16_10,
    Ratio21_9,
    Ratio32_9,
    Ratio4_3,
    MatchActiveDisplay
};

struct DmaBufPlane {
    int fd{-1};
    uint32_t stride{0};
    uint32_t offset{0};
};

struct DmaBufFrame {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t drm_format{0};
    uint64_t modifier{DRM_FORMAT_MOD_INVALID};
    uint32_t num_planes{0};
    std::array<DmaBufPlane, 4> planes{};
    uint32_t flags{0};
    QString outputName{};

    EGLImageKHR eglImage{EGL_NO_IMAGE_KHR};
    GLuint glTexture{0};
    EGLSyncKHR syncFence{EGL_NO_SYNC_KHR};

    void closeFds() noexcept;
    void releaseGl(EGLDisplay dpy, PFNEGLDESTROYIMAGEKHRPROC destroyImage,
                   PFNEGLDESTROYSYNCKHRPROC destroySync) noexcept;
};

struct ShaderUniformValue {
    enum class Type { Float, Vec2, Vec3, Vec4, Int, Mat4 };
    Type type{Type::Float};
    std::vector<float> fVals{};
    std::vector<int> iVals{};
    QMatrix4x4 matVal{};
};

class GpuPreviewViewport : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

  public:
    explicit GpuPreviewViewport(QWidget *parent = nullptr);
    ~GpuPreviewViewport() override;

    // --- Path A: Zero-Copy DMA-BUF Import (SCM_RIGHTS from wwald) ---
    /**
     * @brief Import DMA-BUF plane file descriptors received from wwald.
     * Takes ownership of plane_fds; closes them upon buffer rotation, destruction, or error.
     */
    bool importDmaBufFrame(const waywal_dmabuf_frame_desc_t &desc, const int *plane_fds,
                           size_t num_fds);

    bool requestLiveDmaBuf(int socketFd, const QString &outputName = QString());
    bool requestLiveDmaBufFromNamespace(const QString &namespaceStr,
                                        const QString &outputName = QString());

    bool hasLiveDmaBuf() const noexcept;
    void clearLiveDmaBuf();

    // --- Path B: In-Process Fallback Preview ---
    void setPreviewImage(const QImage &image);
    bool setPreviewImagePath(const QString &filePath);
    void clearPreviewImage();
    [[nodiscard]] const QImage &previewImage() const noexcept { return m_fallbackImage; }

    bool setPreviewShader(const QString &glslSource, QString *outError = nullptr);
    void clearPreviewShader();

    void setShaderUniform(const QString &name, float value);
    void setShaderUniform(const QString &name, float v0, float v1);
    void setShaderUniform(const QString &name, float v0, float v1, float v2);
    void setShaderUniform(const QString &name, float v0, float v1, float v2, float v3);
    void setShaderUniform(const QString &name, const QVector2D &vec);
    void setShaderUniform(const QString &name, const QVector3D &vec);
    void setShaderUniform(const QString &name, const QVector4D &vec);
    void setShaderUniform(const QString &name, int value);
    void setShaderUniform(const QString &name, const QMatrix4x4 &mat);
    void clearShaderUniforms();

    // --- Viewport State & Properties ---
    PreviewMode previewMode() const noexcept { return m_previewMode; }
    uint32_t contentWidth() const noexcept;
    uint32_t contentHeight() const noexcept;
    float currentFps() const noexcept { return m_currentFps; }
    uint64_t renderedFrameCount() const noexcept { return m_frameCount; }

    // --- Interactive Navigation & Parallax ---
    float zoom() const noexcept { return m_zoom; }
    void setZoom(float zoom);

    QPointF panOffset() const noexcept { return m_panOffset; }
    void setPanOffset(const QPointF &offset);

    void resetView();

    bool isParallaxEnabled() const noexcept { return m_parallaxEnabled; }
    void setParallaxEnabled(bool enabled);

    float parallaxStrength() const noexcept { return m_parallaxStrength; }
    void setParallaxStrength(float strength);

    // --- Safe Frame Guides ---
    bool showSafeGuides() const noexcept { return m_showSafeGuides; }
    void setShowSafeGuides(bool show);

    AspectRatioGuide aspectRatioGuide() const noexcept { return m_aspectRatioGuide; }
    void setAspectRatioGuide(AspectRatioGuide guide);

    // --- Action Bar & Monitors ---
    bool showActionBar() const noexcept { return m_showActionBar; }
    void setShowActionBar(bool show);

    QString selectedMonitor() const noexcept { return m_selectedMonitor; }
    void setSelectedMonitor(const QString &monitorName);

    QStringList availableMonitors() const noexcept { return m_availableMonitors; }
    void setAvailableMonitors(const QStringList &monitors);

    void setTargetAnimationFps(int fps);
    int targetAnimationFps() const noexcept { return m_targetFps; }

    void pauseAnimation(bool pause);
    bool isAnimationPaused() const noexcept { return m_animationPaused; }

    // --- Interactive Authoring Gizmos & Brush Pointer Routing ---
    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    void setGizmoActiveNode(const SceneLayerNode *node);
    void setGizmoCameraAndLights(const we_camera3d_t &cam, const we_lighting_uniforms_t &lights);
    void setGizmoEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters);
    void setToolMode(ViewportToolMode mode);
    [[nodiscard]] ViewportToolMode toolMode() const noexcept { return m_gizmoOverlay.toolMode(); }
    void setSnapConfig(const ViewportGizmoSnapConfig &cfg);
    [[nodiscard]] const ViewportGizmoSnapConfig &snapConfig() const noexcept
    {
        return m_gizmoOverlay.snapConfig();
    }
    void setSceneViewProj(const QMatrix4x4 &viewProj, bool hasCustomViewProj = true);
    [[nodiscard]] ViewportGizmoOverlay &gizmoOverlay() noexcept { return m_gizmoOverlay; }
    [[nodiscard]] QPointF mapWidgetToContentTexel(const QPointF &widgetPos) const noexcept;
    [[nodiscard]] static bool isBrushToolMode(ViewportToolMode mode) noexcept;

    // --- Puppet Rigging, Wireframe, IK, Onion Skin & Weight Paint Overlays ---
    void setPuppetObjectId(int32_t objectId) noexcept { m_puppetObjectId = objectId; }
    [[nodiscard]] int32_t puppetObjectId() const noexcept { return m_puppetObjectId; }
    void setPuppetMeshWireframe(bool visible, const we_mesh_gen_result_t &mesh);
    void setPuppetMeshData(bool wireframeVisible, std::vector<we_mdl_vertex_t> vertices,
                           std::vector<uint16_t> indices);
    void setPuppetWireframeVisible(bool visible);
    [[nodiscard]] bool isPuppetWireframeVisible() const noexcept
    {
        return m_gizmoOverlay.meshWireframe().visible;
    }
    void setPuppetContourPins(std::vector<we_cdt_pin_t> pins, int32_t selectedPin = -1);
    void setPuppetSkeleton(const BoneHierarchySnapshot &rig, int32_t selectedBoneIndex = -1);
    void setSelectedPuppetBone(int32_t boneIndex);
    void setPuppetIkHandles(std::vector<GizmoIkHandleData> chains);
    void setOnionSkinEnabled(bool enabled);
    void setOnionSkinFrames(std::vector<GizmoOnionGhostFrame> frames);
    void setWeightHeatmapMode(bool enabled, uint32_t activeBoneIdx, float opacity,
                              we_heatmap_palette_t palette);
    void setWeightBrushParameters(we_weight_brush_op_t op, float radiusPx, float strength,
                                  float targetWeight,
                                  const std::array<uint64_t, 2> &lockedBones = {0ull, 0ull});
    void applyWeightBrushDab(const WeightBrushDab &dab);
    void applyVertexWeightBlocks(int32_t objectId,
                                 const std::vector<VertexWeightBlockSnapshot> &blocks);
    void applySolvedVertexWeights(int32_t objectId,
                                  const std::vector<waywal_ipc_vertex_weight_t> &entries);
    void normalizeAllVertexWeights(int32_t objectId, const std::array<uint64_t, 2> &lockedBones);

  signals:
    void applyRequested(const QString &monitorSpec);
    void monitorSelectionChanged(const QString &monitorName);
    void zoomChanged(float newZoom);
    void panChanged(const QPointF &newOffset);
    void parallaxToggled(bool enabled);
    void aspectRatioGuideChanged(AspectRatioGuide guide);
    void dmaBufImportFailed(const QString &reason);
    void dmaBufFrameRendered(uint32_t width, uint32_t height, uint64_t frameCount);
    void fpsUpdated(float fps);
    void toolModeChanged(ViewportToolMode mode);
    void layerTransformPreviewed(int32_t objectId,
                                 const SetLayerTransformCommand::TransformState &state);
    void layerTransformCommitted(int32_t objectId,
                                 const SetLayerTransformCommand::TransformState &oldState,
                                 const SetLayerTransformCommand::TransformState &newState);
    void brushPointerPressed(const QPointF &texelPos, float pressure, double timestampSec);
    void brushPointerMoved(const QPointF &texelPos, float pressure, double timestampSec);
    void brushPointerReleased(const QPointF &texelPos, float pressure, double timestampSec);

    void contourPinAdded(int32_t objectId, double px, double py);
    void contourPinMoved(int32_t objectId, uint32_t pinIndex, double px, double py);
    void contourPinSelected(int32_t objectId, int32_t pinIndex);
    void puppetBoneSelected(int32_t boneIndex);
    void puppetBoneCreated(int32_t objectId, int32_t parentBoneIndex, const QPointF &headCanvas,
                           const QPointF &tailCanvas, float angleZRad);
    void puppetBoneReparented(int32_t objectId, int32_t childBoneIndex, int32_t newParentIndex);
    void puppetBonePivotChanged(int32_t objectId, int32_t boneIndex, const QPointF &newLocalPos,
                                float newAngleZRad, bool committed);
    void puppetIkTargetChanged(int32_t objectId, int32_t chainIndex, const QVector3D &targetWorld,
                               bool committed);
    void puppetIkPoleChanged(int32_t objectId, int32_t chainIndex, const QVector3D &poleWorld,
                             bool poleFlip, bool committed);
    void weightBrushDabBatchReady(int32_t objectId, const std::vector<WeightBrushDab> &dabs);
    void weightPaintStrokeCommitted(int32_t objectId,
                                    const std::vector<VertexWeightBlockSnapshot> &preBlocks,
                                    const std::vector<VertexWeightBlockSnapshot> &postBlocks,
                                    const std::vector<waywal_ipc_vertex_weight_t> &dirtyEntries);

  public slots:
    void onApplyClicked();
    void onResetViewClicked();
    void onToggleParallax();
    void onCycleGuides();
    void onCycleMonitors();

  protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintGL() override;
    void paintEvent(QPaintEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void tabletEvent(QTabletEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    void resolveEglExtensions();
    void setupQuadGeometry();
    void initBlitShader();
    void ensureGizmoGlInitialized();
    void ensureGizmoVboCapacity(size_t requiredBytes);
    void renderGizmoOverlayPass(uint32_t contentW, uint32_t contentH);
    [[nodiscard]] QMatrix4x4 computeBlitMvp(uint32_t contentW, uint32_t contentH) const noexcept;
    [[nodiscard]] QMatrix4x4 computeGizmoViewProj(uint32_t contentW,
                                                  uint32_t contentH) const noexcept;
    [[nodiscard]] QPointF mapTexelToCanvasWorld(const QPointF &texel) const noexcept;
    [[nodiscard]] QPointF mapTexelToPuppetLocal(const QPointF &texel) const noexcept;
    void beginWeightPaintStroke();
    void recordWeightBrushDab(const QPointF &texel, float pressure, double tsSec);
    void flushWeightBrushDabBatch();
    void finishWeightPaintStroke();
    [[nodiscard]] bool tryBeginRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods);
    void updateRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods);
    void finishRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods);
    [[nodiscard]] QSize physicalSizeFor(int logicalW, int logicalH) const noexcept;
    [[nodiscard]] QSize physicalFramebufferSize() const noexcept;
    void ensureNeutralPassTexture();
    void ensureOffscreenFbo(int w, int h);
    void ensureBufferPassFbos(int w, int h);
    bool compileShaderInternal(const QString &glslSource, QString *outError);
    void applyShaderUniforms(QOpenGLShaderProgram &prog, float fboW, float fboH, float timeSec,
                             float timeDeltaSec, int activeWritePass = 4);
    void renderInProcessShaderPass();
    void updateAnimationTimerState();
    void drawFallbackBackdrop(class QPainter &painter, const QRectF &displayRect);
    void drawSafeFrameGuides(class QPainter &painter, const QRectF &displayRect);
    void drawActionBar(class QPainter &painter);
    void drawStatusOverlay(class QPainter &painter);
    float calculateGuideAspect() const noexcept;
    QRectF computeContentDisplayRect() const noexcept;
    void updateFpsTelemetry();

    enum class ActionBarButton {
        None,
        MonitorSelector,
        ApplyButton,
        ResetButton,
        GuideButton,
        ParallaxButton
    };
    ActionBarButton hitTestActionBar(const QPointF &pos) const noexcept;

    // EGL function pointers
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
    PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR{nullptr};
    PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR{nullptr};
    PFNEGLWAITSYNCKHRPROC m_eglWaitSyncKHR{nullptr};
    PFNEGLCLIENTWAITSYNCKHRPROC m_eglClientWaitSyncKHR{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};

    EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
    bool m_eglInitialized{false};

    // State
    PreviewMode m_previewMode{PreviewMode::OfflineEmpty};
    std::optional<DmaBufFrame> m_currentDmaBufFrame{};
    std::mutex m_frameMutex{};

    // Path B In-Process Fallback State
    GLuint m_inProcessImageTex{0};
    GLuint m_neutralPassTex{0};
    GLuint m_computeOutputTex{0};
    int m_computeTexWidth{0};
    int m_computeTexHeight{0};
    QImage m_fallbackImage{};
    uint32_t m_imageWidth{0};
    uint32_t m_imageHeight{0};
    std::unique_ptr<QOpenGLFramebufferObject> m_offscreenFbo{nullptr};
    std::array<std::array<std::unique_ptr<QOpenGLFramebufferObject>, 2>, 4> m_bufferPassFbos{};
    int m_bufferPingPongIdx{0};
    std::array<std::unique_ptr<QOpenGLShaderProgram>, 4> m_bufferPassPrograms{};
    std::unique_ptr<QOpenGLShaderProgram> m_shaderProgram{nullptr};
    std::unique_ptr<QOpenGLShaderProgram> m_computeShaderProgram{nullptr};
    QString m_shaderSource{};
    std::unordered_map<std::string, ShaderUniformValue> m_customUniforms{};
    QElapsedTimer m_shaderTimer{};
    qint64 m_lastFrameTimeNs{0};
    int m_shaderFrameIndex{0};
    std::atomic<uint64_t> m_videoLoadGeneration{0};
    std::jthread m_videoThumbThread{};

    // Quad rendering & Blit Program
    QOpenGLVertexArrayObject m_quadVao{};
    QOpenGLBuffer m_quadVbo{QOpenGLBuffer::VertexBuffer};
    std::unique_ptr<QOpenGLShaderProgram> m_blitProgram{nullptr};

    // Viewport Navigation & 2.5D Parallax
    float m_zoom{1.0f};
    QPointF m_panOffset{0.0, 0.0};
    bool m_isPanning{false};
    QPointF m_lastMousePos{0.0, 0.0};

    bool m_parallaxEnabled{false};
    float m_parallaxStrength{1.0f};
    float m_currentTiltX{0.0f};
    float m_currentTiltY{0.0f};
    float m_targetTiltX{0.0f};
    float m_targetTiltY{0.0f};

    // Safe Guides
    bool m_showSafeGuides{true};
    AspectRatioGuide m_aspectRatioGuide{AspectRatioGuide::Ratio16_9};

    // Action Bar & Monitors
    bool m_showActionBar{true};
    QString m_selectedMonitor{"DP-1"};
    QStringList m_availableMonitors{"DP-1", "HDMI-A-1", "All Monitors (Span)"};
    ActionBarButton m_hoveredButton{ActionBarButton::None};

    QRectF m_actionBarRect{};
    QRectF m_monitorBtnRect{};
    QRectF m_applyBtnRect{};
    QRectF m_resetBtnRect{};
    QRectF m_guideBtnRect{};
    QRectF m_parallaxBtnRect{};

    // Telemetry & Timers
    QLabel *m_fallbackCanvas{nullptr};
    QTimer *m_animationTimer{nullptr};
    int m_targetFps{144};
    bool m_animationPaused{false};
    uint64_t m_frameCount{0};
    float m_currentFps{0.0f};
    std::chrono::steady_clock::time_point m_lastFpsCalcTime{};
    uint64_t m_fpsFrameCounter{0};
    QPointF m_mouseGlCoord{0.0, 0.0};
    QPointF m_mouseClickCoord{0.0, 0.0};

    // Authoring Gizmos & Brush Pointer State (lazy GL allocation on first active draw)
    CommandBus *m_commandBus{nullptr};
    ViewportGizmoOverlay m_gizmoOverlay{};
    QOpenGLVertexArrayObject m_gizmoVao{};
    QOpenGLBuffer m_gizmoVbo{QOpenGLBuffer::VertexBuffer};
    size_t m_gizmoVboCapacityBytes{0};
    std::unique_ptr<QOpenGLShaderProgram> m_gizmoProgram{nullptr};
    std::vector<GizmoVertex> m_gizmoVertices{};
    std::vector<GizmoVertex> m_heatmapVertices{};
    bool m_hasCustomSceneViewProj{false};
    QMatrix4x4 m_customSceneViewProj{};
    int32_t m_activeGizmoNodeId{-1};
    QString m_activeGizmoNodeName{};
    SetLayerTransformCommand::TransformState m_dragStartTransform{};
    SetLayerTransformCommand::TransformState m_dragCurrentTransform{};
    bool m_hasGizmoEmittersOrLights{false};
    bool m_brushStrokeActive{false};
    QElapsedTimer m_brushClock{};

    enum class RigDragKind : uint8_t {
        None = 0,
        ContourPinMove,
        BoneCreate,
        BoneJointMoveOrReparent,
        BonePivotRotate,
        IkTargetMove,
        IkPoleMove,
    };

    int32_t m_puppetObjectId{-1};
    RigDragKind m_rigDragKind{RigDragKind::None};
    int32_t m_rigDragIndex{-1};
    QVector3D m_rigDragStartWorld{0.0f, 0.0f, 0.0f};
    QVector3D m_rigDragInitialBoneHead{0.0f, 0.0f, 0.0f};
    QVector3D m_rigDragInitialBoneTail{0.0f, 0.0f, 0.0f};
    float m_rigDragStartAngleRad{0.0f};
    float m_rigDragInitialBoneAngleRad{0.0f};

    we_weight_brush_op_t m_weightBrushOp{WE_WEIGHT_OP_ADD};
    float m_weightBrushRadiusPx{32.0f};
    float m_weightBrushStrength{0.5f};
    float m_weightTargetValue{1.0f};
    std::array<uint64_t, 2> m_lockedBoneMask{0ull, 0ull};
    std::vector<WeightBrushDab> m_pendingWeightDabs{};
    std::unordered_map<uint32_t, VertexWeightBlockSnapshot> m_weightPreBlocks{};
    std::vector<uint32_t> m_weightDirtyVertices{};
};

} // namespace waywal::studio
