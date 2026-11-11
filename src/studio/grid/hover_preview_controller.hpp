// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_HOVER_PREVIEW_CONTROLLER_HPP
#define WWAL_STUDIO_GRID_HOVER_PREVIEW_CONTROLLER_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <chrono>

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QImage>
#include <QMovie>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>

#include "fuzzy_catalog_index.hpp"

namespace wwal::studio {

class HoverPreviewController : public QObject {
    Q_OBJECT

  public:
    static constexpr uint32_t PREVIEW_WIDTH = 384;
    static constexpr uint32_t PREVIEW_HEIGHT = 216;
    static constexpr int DWELL_TIMEOUT_MS = 250;        // 250 ms dwell trigger
    static constexpr int FRAME_INTERVAL_MS = 33;        // 30 FPS (~33.3 ms)
    static constexpr int MAX_VIDEO_PREVIEW_SECONDS = 4; // First 4 seconds

    explicit HoverPreviewController(QObject *parent = nullptr);
    ~HoverPreviewController() override;

    HoverPreviewController(const HoverPreviewController &) = delete;
    HoverPreviewController &operator=(const HoverPreviewController &) = delete;

    void setGlOwner(QOpenGLWidget *owner) noexcept { m_glOwner = owner; }

    // OpenGL Initialization / Destruction (must be called with active GL context)
    void initGl(QOpenGLExtraFunctions *gl);
    void destroyGl(QOpenGLExtraFunctions *gl);

    // Grid Hover Interaction Handlers
    void onCellHoverEntered(uint32_t item_id, const std::string &path, WallpaperBadge badge,
                            const std::string &preview_path = "");
    void onCellHoverExited(uint32_t item_id);
    void stop();

    [[nodiscard]] bool isPreviewActive() const noexcept { return m_previewActive; }
    [[nodiscard]] uint32_t activeItemId() const noexcept { return m_activeItemId; }
    [[nodiscard]] GLuint previewTextureId() const noexcept { return m_previewTexId; }
    [[nodiscard]] const QImage &currentPreviewImage() const noexcept
    {
        return m_currentPreviewImage;
    }

  signals:
    void previewStarted(uint32_t item_id);
    void previewFrameReady(uint32_t item_id, GLuint texture_id);
    void previewStopped(uint32_t item_id);

  private slots:
    void onDwellTimeout();
    void onPlaybackTick();

  private:
    QPointer<QOpenGLWidget> m_glOwner;
    QOpenGLExtraFunctions *m_gl{nullptr};
    GLuint m_previewTexId{0};
    GLuint m_shaderFbo{0};
    GLuint m_shaderProgram{0};
    GLuint m_quadVao{0};
    GLuint m_quadVbo{0};
    QImage m_currentPreviewImage;
    QImage m_videoBaseImage;

    // Timers
    QTimer *m_dwellTimer{nullptr};
    QTimer *m_playbackTimer{nullptr};

    // Hover State
    uint32_t m_pendingItemId{0};
    bool m_hasPendingItem{false};
    std::string m_pendingPath;
    std::string m_pendingPreviewPath;
    WallpaperBadge m_pendingBadge{WallpaperBadge::Image};

    uint32_t m_activeItemId{0};
    std::string m_activePath;
    WallpaperBadge m_activeBadge{WallpaperBadge::Image};
    bool m_previewActive{false};

    // Animated GIF / WebP playback
    std::unique_ptr<QMovie> m_movie;

    // Shader preview state
    float m_shaderTime{0.0f};
    int m_shaderFrame{0};
    bool m_shaderInitialized{false};

    // Video / Frame sequence state
    std::vector<QImage> m_preloadedFrames;
    size_t m_currentFrameIndex{0};
    std::chrono::steady_clock::time_point m_previewStartTime;

    void startActivePreview();
    void updateTextureWithImage(const QImage &img);
    void setupOffscreenShader(const std::string &shader_path);
    void renderShaderFrame();
    void cleanupActivePreview();
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_HOVER_PREVIEW_CONTROLLER_HPP
