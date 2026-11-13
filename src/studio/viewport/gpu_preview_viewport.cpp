// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_preview_viewport.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDebug>
#include <QtMath>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <algorithm>
#include <limits>
#include <unordered_set>

extern "C" {
#include <waywal/image_loader.h>
}

namespace waywal::studio {

namespace {

bool isOffscreenPlatform() { return QGuiApplication::platformName() == QLatin1String("offscreen"); }

struct Vertex {
    float x, y;
    float u, v;
};

// Fullscreen quad vertices with position [-1, 1] and UV [0, 1]
static constexpr Vertex kQuadVertices[6] = {{-1.0f, -1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 1.0f, 0.0f},
                                            {1.0f, 1.0f, 1.0f, 1.0f},   {-1.0f, -1.0f, 0.0f, 0.0f},
                                            {1.0f, 1.0f, 1.0f, 1.0f},   {-1.0f, 1.0f, 0.0f, 1.0f}};

static constexpr EGLint kPlaneFdKeys[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                           EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};

static constexpr EGLint kPlaneOffsetKeys[4] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT};

static constexpr EGLint kPlanePitchKeys[4] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT};

static constexpr EGLint kPlaneModLoKeys[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};

static constexpr EGLint kPlaneModHiKeys[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

const char *kBlitVertexShader = "#version 300 es\n"
                                "precision highp float;\n"
                                "layout (location = 0) in vec2 a_pos;\n"
                                "layout (location = 1) in vec2 a_uv;\n"
                                "uniform mat4 u_mvp;\n"
                                "out vec2 v_uv;\n"
                                "void main() {\n"
                                "    v_uv = a_uv;\n"
                                "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
                                "}\n";

const char *kBlitFragmentShader = "#version 300 es\n"
                                  "precision highp float;\n"
                                  "in vec2 v_uv;\n"
                                  "uniform sampler2D u_texture;\n"
                                  "uniform int u_flip_y;\n"
                                  "uniform int u_is_bgr;\n"
                                  "out vec4 fragColor;\n"
                                  "void main() {\n"
                                  "    vec2 uv = v_uv;\n"
                                  "    if (u_flip_y != 0) {\n"
                                  "        uv.y = 1.0 - uv.y;\n"
                                  "    }\n"
                                  "    vec4 col = texture(u_texture, uv);\n"
                                  "    if (u_is_bgr != 0) {\n"
                                  "        col.rgb = col.bgr;\n"
                                  "    }\n"
                                  "    // Invariant: IMAGE_PASS must always force opaque alpha\n"
                                  "    fragColor = vec4(col.rgb, 1.0);\n"
                                  "}\n";

const char *kBlitVertexShaderCore = "#version 330 core\n"
                                    "layout (location = 0) in vec2 a_pos;\n"
                                    "layout (location = 1) in vec2 a_uv;\n"
                                    "uniform mat4 u_mvp;\n"
                                    "out vec2 v_uv;\n"
                                    "void main() {\n"
                                    "    v_uv = a_uv;\n"
                                    "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
                                    "}\n";

const char *kBlitFragmentShaderCore = "#version 330 core\n"
                                      "in vec2 v_uv;\n"
                                      "uniform sampler2D u_texture;\n"
                                      "uniform int u_flip_y;\n"
                                      "uniform int u_is_bgr;\n"
                                      "out vec4 fragColor;\n"
                                      "void main() {\n"
                                      "    vec2 uv = v_uv;\n"
                                      "    if (u_flip_y != 0) {\n"
                                      "        uv.y = 1.0 - uv.y;\n"
                                      "    }\n"
                                      "    vec4 col = texture(u_texture, uv);\n"
                                      "    if (u_is_bgr != 0) {\n"
                                      "        col.rgb = col.bgr;\n"
                                      "    }\n"
                                      "    fragColor = vec4(col.rgb, 1.0);\n"
                                      "}\n";

const char *kGizmoVertexShaderEs310 = "#version 310 es\n"
                                      "precision highp float;\n"
                                      "layout (location = 0) in vec3 a_pos;\n"
                                      "layout (location = 1) in vec4 a_color;\n"
                                      "uniform mat4 u_mvp;\n"
                                      "out vec4 v_color;\n"
                                      "void main() {\n"
                                      "    v_color = a_color;\n"
                                      "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
                                      "}\n";

const char *kGizmoFragmentShaderEs310 = "#version 310 es\n"
                                        "precision highp float;\n"
                                        "in vec4 v_color;\n"
                                        "out vec4 fragColor;\n"
                                        "void main() {\n"
                                        "    fragColor = v_color;\n"
                                        "}\n";

const char *kGizmoVertexShaderCore = "#version 330 core\n"
                                     "layout (location = 0) in vec3 a_pos;\n"
                                     "layout (location = 1) in vec4 a_color;\n"
                                     "uniform mat4 u_mvp;\n"
                                     "out vec4 v_color;\n"
                                     "void main() {\n"
                                     "    v_color = a_color;\n"
                                     "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
                                     "}\n";

const char *kGizmoFragmentShaderCore = "#version 330 core\n"
                                       "in vec4 v_color;\n"
                                       "out vec4 fragColor;\n"
                                       "void main() {\n"
                                       "    fragColor = v_color;\n"
                                       "}\n";

const char *kGraphVertexShaderEs310 = "#version 310 es\n"
                                      "precision highp float;\n"
                                      "layout (location = 0) in vec2 a_pos;\n"
                                      "layout (location = 1) in vec2 a_uv;\n"
                                      "uniform mat4 u_mvp;\n"
                                      "out vec2 v_TexCoord;\n"
                                      "void main() {\n"
                                      "    v_TexCoord = a_uv;\n"
                                      "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
                                      "}\n";

const char *kGraphVertexShaderEs300 = "#version 300 es\n"
                                      "precision highp float;\n"
                                      "layout (location = 0) in vec2 a_pos;\n"
                                      "layout (location = 1) in vec2 a_uv;\n"
                                      "uniform mat4 u_mvp;\n"
                                      "out vec2 v_TexCoord;\n"
                                      "void main() {\n"
                                      "    v_TexCoord = a_uv;\n"
                                      "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
                                      "}\n";

const char *kGraphVertexShaderCore330 = "#version 330 core\n"
                                        "layout (location = 0) in vec2 a_pos;\n"
                                        "layout (location = 1) in vec2 a_uv;\n"
                                        "uniform mat4 u_mvp;\n"
                                        "out vec2 v_TexCoord;\n"
                                        "void main() {\n"
                                        "    v_TexCoord = a_uv;\n"
                                        "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
                                        "}\n";

const char *kShadertoyPreamble = "#version 300 es\n"
                                 "precision highp float;\n"
                                 "out vec4 outFragColor;\n"
                                 "uniform vec3 iResolution;\n"
                                 "uniform float iTime;\n"
                                 "uniform float iTimeDelta;\n"
                                 "uniform int iFrame;\n"
                                 "uniform vec4 iMouse;\n"
                                 "uniform vec4 iDate;\n\n";

const char *kShadertoyPreambleCore = "#version 330 core\n"
                                     "out vec4 outFragColor;\n"
                                     "uniform vec3 iResolution;\n"
                                     "uniform float iTime;\n"
                                     "uniform float iTimeDelta;\n"
                                     "uniform int iFrame;\n"
                                     "uniform vec4 iMouse;\n"
                                     "uniform vec4 iDate;\n\n";

const char *kShadertoyEpilogue = "\nvoid main() {\n"
                                 "    vec4 color = vec4(0.0);\n"
                                 "    mainImage(color, gl_FragCoord.xy);\n"
                                 "    // Invariant: IMAGE_PASS must always force opaque alpha\n"
                                 "    outFragColor = vec4(color.rgb, 1.0);\n"
                                 "}\n";

class GlContextScope {
  public:
    explicit GlContextScope(GpuPreviewViewport *vp) noexcept
    {
        if (isOffscreenPlatform() || !vp || !vp->isValid() || !vp->context()) {
            return;
        }
        if (QOpenGLContext::currentContext() == vp->context()) {
            m_ready = true;
            return;
        }
        vp->makeCurrent();
        m_vp = vp;
        m_ready = (QOpenGLContext::currentContext() == vp->context());
    }

    ~GlContextScope()
    {
        if (m_vp) {
            m_vp->doneCurrent();
        }
    }

    GlContextScope(const GlContextScope &) = delete;
    GlContextScope &operator=(const GlContextScope &) = delete;

    [[nodiscard]] bool isReady() const noexcept { return m_ready; }

  private:
    GpuPreviewViewport *m_vp{nullptr};
    bool m_ready{false};
};

QString convertEsFragmentToCore330(QString src)
{
    static const QRegularExpression kVersionRe(
        QStringLiteral("^\\s*#version\\s+\\d+\\s+es\\b[^\\n]*"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression kPrecisionRe(
        QStringLiteral("^\\s*precision\\s+(?:lowp|mediump|highp)\\s+\\w+\\s*;\\s*$"),
        QRegularExpression::MultilineOption);
    src.replace(kVersionRe, QStringLiteral("#version 330 core"));
    src.replace(kPrecisionRe, QString());
    return src;
}

QString convertEs310ToEs300(QString src)
{
    static const QRegularExpression kVersion310Re(
        QStringLiteral("^\\s*#version\\s+310\\s+es\\b[^\\n]*"),
        QRegularExpression::MultilineOption);
    src.replace(kVersion310Re, QStringLiteral("#version 300 es"));
    return src;
}

struct GraphPassSource {
    int passIndex{4};
    QString glsl;
};

std::vector<GraphPassSource> parseGraphPasses(const QString &source)
{
    std::vector<GraphPassSource> out;
    static const QRegularExpression kPragmaPassRe(
        QStringLiteral("^\\s*#pragma\\s+Pass\\s+([A-Za-z0-9_]+)\\s*$"),
        QRegularExpression::MultilineOption);

    auto mapPassName = [](const QString &name) -> int {
        if (name == QLatin1String("BUFFER_A"))
            return 0;
        if (name == QLatin1String("BUFFER_B"))
            return 1;
        if (name == QLatin1String("BUFFER_C"))
            return 2;
        if (name == QLatin1String("BUFFER_D"))
            return 3;
        return 4;
    };

    QRegularExpressionMatchIterator it = kPragmaPassRe.globalMatch(source);
    struct MatchSpan {
        int passIndex;
        qsizetype bodyStart;
        qsizetype pragmaStart;
    };
    std::vector<MatchSpan> spans;
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        spans.push_back(MatchSpan{
            .passIndex = mapPassName(m.captured(1)),
            .bodyStart = m.capturedEnd(0),
            .pragmaStart = m.capturedStart(0),
        });
    }

    for (size_t i = 0; i < spans.size(); ++i) {
        const qsizetype start = spans[i].bodyStart;
        const qsizetype end = (i + 1 < spans.size()) ? spans[i + 1].pragmaStart : source.size();
        QString body = source.mid(start, end - start).trimmed();
        if (!body.isEmpty()) {
            out.push_back(
                GraphPassSource{.passIndex = spans[i].passIndex, .glsl = std::move(body)});
        }
    }
    return out;
}

using GlTexStorage2DProc = void (*)(GLenum target, GLsizei levels, GLenum internalformat,
                                    GLsizei width, GLsizei height);
using GlBindImageTextureProc = void (*)(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                                        GLint layer, GLenum access, GLenum format);
using GlDispatchComputeProc = void (*)(GLuint num_groups_x, GLuint num_groups_y,
                                       GLuint num_groups_z);
using GlMemoryBarrierProc = void (*)(GLbitfield barriers);

} // namespace

void DmaBufFrame::closeFds() noexcept
{
    for (auto &plane : planes) {
        if (plane.fd >= 0) {
            ::close(plane.fd);
            plane.fd = -1;
        }
    }
}

void DmaBufFrame::releaseGl(EGLDisplay dpy, PFNEGLDESTROYIMAGEKHRPROC destroyImage,
                            PFNEGLDESTROYSYNCKHRPROC destroySync) noexcept
{
    if (glTexture != 0) {
        glDeleteTextures(1, &glTexture);
        glTexture = 0;
    }
    if (eglImage != EGL_NO_IMAGE_KHR && destroyImage && dpy != EGL_NO_DISPLAY) {
        destroyImage(dpy, eglImage);
        eglImage = EGL_NO_IMAGE_KHR;
    }
    if (syncFence != EGL_NO_SYNC_KHR && destroySync && dpy != EGL_NO_DISPLAY) {
        destroySync(dpy, syncFence);
        syncFence = EGL_NO_SYNC_KHR;
    }
    closeFds();
}

GpuPreviewViewport::GpuPreviewViewport(QWidget *parent) : QOpenGLWidget(parent)
{
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Request modern GLES 3.1 context with 32-bit color
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 1);
    fmt.setSwapInterval(0); // Non-blocking vsync
    setFormat(fmt);

    m_fallbackCanvas = new QLabel(this);
    m_fallbackCanvas->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_fallbackCanvas->setVisible(false);

    m_lastFpsCalcTime = std::chrono::steady_clock::now();

    m_animationTimer = new QTimer(this);
    setTargetAnimationFps(144);
    connect(m_animationTimer, &QTimer::timeout, this, [this]() {
        if (m_animationPaused || !isVisible()) {
            m_animationTimer->stop();
            return;
        }
        if (m_parallaxEnabled) {
            constexpr float kDamping = 0.14f;
            m_currentTiltX += (m_targetTiltX - m_currentTiltX) * kDamping;
            m_currentTiltY += (m_targetTiltY - m_currentTiltY) * kDamping;
        }
        update();
        updateAnimationTimerState();
    });
    updateAnimationTimerState();
}

GpuPreviewViewport::~GpuPreviewViewport()
{
    if (m_animationTimer) {
        m_animationTimer->stop();
    }
    m_videoLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (m_videoThumbThread.joinable()) {
        m_videoThumbThread.request_stop();
        m_videoThumbThread.join();
    }

    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_currentDmaBufFrame.has_value()) {
                m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR,
                                                m_eglDestroySyncKHR);
                m_currentDmaBufFrame.reset();
            }

            if (m_inProcessImageTex != 0) {
                glDeleteTextures(1, &m_inProcessImageTex);
                m_inProcessImageTex = 0;
            }
            if (m_neutralPassTex != 0) {
                glDeleteTextures(1, &m_neutralPassTex);
                m_neutralPassTex = 0;
            }
            if (m_computeOutputTex != 0) {
                glDeleteTextures(1, &m_computeOutputTex);
                m_computeOutputTex = 0;
            }

            m_quadVao.destroy();
            m_quadVbo.destroy();
            m_blitProgram.reset();
            m_gizmoVao.destroy();
            m_gizmoVbo.destroy();
            m_gizmoProgram.reset();
            m_shaderProgram.reset();
            m_computeShaderProgram.reset();
            for (auto &prog : m_bufferPassPrograms) {
                prog.reset();
            }
            for (auto &pair : m_bufferPassFbos) {
                pair[0].reset();
                pair[1].reset();
            }
            m_offscreenFbo.reset();
        }
    }

    // Always close kernel DMA-BUF FDs even if the QOpenGLWidget was never realized.
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_currentDmaBufFrame.has_value()) {
        m_currentDmaBufFrame->closeFds();
        m_currentDmaBufFrame.reset();
    }
}

void GpuPreviewViewport::updateAnimationTimerState()
{
    if (!m_animationTimer) {
        return;
    }
    const bool parallaxSettling =
        m_parallaxEnabled && (std::abs(m_targetTiltX - m_currentTiltX) > 1e-4f ||
                              std::abs(m_targetTiltY - m_currentTiltY) > 1e-4f);
    const bool needsTick = isVisible() && !m_animationPaused &&
                           (m_previewMode == PreviewMode::InProcessShader ||
                            m_previewMode == PreviewMode::DmaBufLive || parallaxSettling);

    if (needsTick && !m_animationTimer->isActive()) {
        m_animationTimer->start();
    } else if (!needsTick && m_animationTimer->isActive()) {
        m_animationTimer->stop();
    }
}

void GpuPreviewViewport::setTargetAnimationFps(int fps)
{
    m_targetFps = std::clamp(fps, 10, 360);
    int intervalMs = std::max(1, 1000 / m_targetFps);
    if (m_animationTimer) {
        m_animationTimer->setInterval(intervalMs);
    }
}

void GpuPreviewViewport::pauseAnimation(bool pause)
{
    m_animationPaused = pause;
    updateAnimationTimerState();
}

uint32_t GpuPreviewViewport::contentWidth() const noexcept
{
    if (m_previewMode == PreviewMode::DmaBufLive && m_currentDmaBufFrame.has_value()) {
        return m_currentDmaBufFrame->width;
    }
    if (m_previewMode == PreviewMode::InProcessImage) {
        return m_imageWidth;
    }
    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_computeShaderProgram && m_computeTexWidth > 0) {
            return static_cast<uint32_t>(m_computeTexWidth);
        }
        if (m_offscreenFbo) {
            return static_cast<uint32_t>(m_offscreenFbo->width());
        }
    }
    return static_cast<uint32_t>(width());
}

uint32_t GpuPreviewViewport::contentHeight() const noexcept
{
    if (m_previewMode == PreviewMode::DmaBufLive && m_currentDmaBufFrame.has_value()) {
        return m_currentDmaBufFrame->height;
    }
    if (m_previewMode == PreviewMode::InProcessImage) {
        return m_imageHeight;
    }
    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_computeShaderProgram && m_computeTexHeight > 0) {
            return static_cast<uint32_t>(m_computeTexHeight);
        }
        if (m_offscreenFbo) {
            return static_cast<uint32_t>(m_offscreenFbo->height());
        }
    }
    return static_cast<uint32_t>(height());
}

bool GpuPreviewViewport::hasLiveDmaBuf() const noexcept
{
    return m_previewMode == PreviewMode::DmaBufLive && m_currentDmaBufFrame.has_value();
}

void GpuPreviewViewport::clearLiveDmaBuf()
{
    {
        GlContextScope glScope(this);
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentDmaBufFrame.has_value()) {
            if (glScope.isReady()) {
                m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR,
                                                m_eglDestroySyncKHR);
            } else {
                m_currentDmaBufFrame->closeFds();
            }
            m_currentDmaBufFrame.reset();
        }
    }
    if (m_inProcessImageTex != 0 || !m_fallbackImage.isNull()) {
        m_previewMode = PreviewMode::InProcessImage;
    } else if (m_shaderProgram || m_computeShaderProgram || !m_shaderSource.isEmpty()) {
        m_previewMode = PreviewMode::InProcessShader;
    } else {
        m_previewMode = PreviewMode::OfflineEmpty;
    }
    updateAnimationTimerState();
    update();
}

void GpuPreviewViewport::resolveEglExtensions()
{
    if (m_eglInitialized) {
        return;
    }

    m_eglDisplay = eglGetCurrentDisplay();
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        return;
    }

    auto getProc = [](const char *name) -> void * {
        void *p = reinterpret_cast<void *>(eglGetProcAddress(name));
        return p;
    };

    m_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(getProc("eglCreateImageKHR"));
    m_eglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(getProc("eglDestroyImageKHR"));
    m_eglCreateSyncKHR = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(getProc("eglCreateSyncKHR"));
    m_eglDestroySyncKHR = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(getProc("eglDestroySyncKHR"));
    m_eglWaitSyncKHR = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(getProc("eglWaitSyncKHR"));
    m_eglClientWaitSyncKHR =
        reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(getProc("eglClientWaitSyncKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        getProc("glEGLImageTargetTexture2DOES"));

    m_eglInitialized =
        (m_eglCreateImageKHR != nullptr && m_glEGLImageTargetTexture2DOES != nullptr);
}

void GpuPreviewViewport::setupQuadGeometry()
{
    if (!m_quadVao.isCreated()) {
        m_quadVao.create();
    }
    m_quadVao.bind();

    if (!m_quadVbo.isCreated()) {
        m_quadVbo.create();
    }
    m_quadVbo.bind();
    m_quadVbo.allocate(kQuadVertices, sizeof(kQuadVertices));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, u)));

    m_quadVao.release();
    m_quadVbo.release();
}

void GpuPreviewViewport::initBlitShader()
{
    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();
    const char *vsSrc = isGles ? kBlitVertexShader : kBlitVertexShaderCore;
    const char *fsSrc = isGles ? kBlitFragmentShader : kBlitFragmentShaderCore;

    m_blitProgram = std::make_unique<QOpenGLShaderProgram>(this);
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vsSrc) ||
        !m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fsSrc) ||
        !m_blitProgram->link()) {
        m_blitProgram = std::make_unique<QOpenGLShaderProgram>(this);
        if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kBlitVertexShaderCore) ||
            !m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                    kBlitFragmentShaderCore) ||
            !m_blitProgram->link()) {
            qWarning() << "[GpuPreviewViewport] Failed to compile/link blit shader:"
                       << m_blitProgram->log();
        }
    }
}

void GpuPreviewViewport::ensureNeutralPassTexture()
{
    if (m_neutralPassTex != 0) {
        return;
    }
    static constexpr uint8_t kNeutralPixel[4] = {28, 34, 48, 255};
    glGenTextures(1, &m_neutralPassTex);
    glBindTexture(GL_TEXTURE_2D, m_neutralPassTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kNeutralPixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GpuPreviewViewport::initializeGL()
{
    if (isOffscreenPlatform()) {
        return;
    }
    initializeOpenGLFunctions();
    resolveEglExtensions();
    setupQuadGeometry();
    initBlitShader();
    ensureNeutralPassTexture();

    if (!m_fallbackImage.isNull() && m_inProcessImageTex == 0) {
        QImage formatted = m_fallbackImage.convertToFormat(QImage::Format_RGBA8888);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
        glGenTextures(1, &m_inProcessImageTex);
        glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, formatted.constBits());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (!m_shaderSource.isEmpty() && !m_shaderProgram && !m_computeShaderProgram) {
        (void)compileShaderInternal(m_shaderSource, nullptr);
    }

    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

QSize GpuPreviewViewport::physicalSizeFor(int logicalW, int logicalH) const noexcept
{
    const qreal dpr = devicePixelRatioF();
    return QSize(std::max(1, static_cast<int>(std::lround(static_cast<double>(logicalW) * dpr))),
                 std::max(1, static_cast<int>(std::lround(static_cast<double>(logicalH) * dpr))));
}

QSize GpuPreviewViewport::physicalFramebufferSize() const noexcept
{
    return physicalSizeFor(width(), height());
}

void GpuPreviewViewport::resizeGL(int w, int h)
{
    if (isOffscreenPlatform()) {
        return;
    }
    const QSize fbSize = physicalSizeFor(w, h);
    glViewport(0, 0, fbSize.width(), fbSize.height());
    if (m_previewMode == PreviewMode::InProcessShader) {
        ensureOffscreenFbo(fbSize.width(), fbSize.height());
        ensureBufferPassFbos(fbSize.width(), fbSize.height());
    }
}

void GpuPreviewViewport::resizeEvent(QResizeEvent *event)
{
    if (isOffscreenPlatform()) {
        QWidget::resizeEvent(event);
        return;
    }
    QOpenGLWidget::resizeEvent(event);
}

void GpuPreviewViewport::ensureOffscreenFbo(int w, int h)
{
    int targetW = std::clamp(w, 64, 7680);
    int targetH = std::clamp(h, 64, 4320);

    if (!m_offscreenFbo || m_offscreenFbo->size() != QSize(targetW, targetH)) {
        QOpenGLFramebufferObjectFormat fboFmt;
        fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        fboFmt.setTextureTarget(GL_TEXTURE_2D);
        fboFmt.setInternalTextureFormat(GL_RGBA8);
        m_offscreenFbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
    }
}

void GpuPreviewViewport::ensureBufferPassFbos(int w, int h)
{
    const int targetW = std::clamp(w, 64, 7680);
    const int targetH = std::clamp(h, 64, 4320);

    for (size_t i = 0; i < 4; ++i) {
        if (!m_bufferPassPrograms[i]) {
            m_bufferPassFbos[i][0].reset();
            m_bufferPassFbos[i][1].reset();
            continue;
        }
        for (size_t slot = 0; slot < 2; ++slot) {
            auto &fbo = m_bufferPassFbos[i][slot];
            if (!fbo || fbo->size() != QSize(targetW, targetH)) {
                QOpenGLFramebufferObjectFormat fboFmt;
                fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
                fboFmt.setTextureTarget(GL_TEXTURE_2D);
                fboFmt.setInternalTextureFormat(GL_RGBA8);
                fbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
                if (fbo->bind()) {
                    glViewport(0, 0, targetW, targetH);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    fbo->release();
                }
            }
        }
    }
}

bool GpuPreviewViewport::importDmaBufFrame(const waywal_dmabuf_frame_desc_t &desc,
                                           const int *plane_fds, size_t num_fds)
{
    auto cleanupIncomingFds = [plane_fds, num_fds]() {
        if (!plane_fds)
            return;
        for (size_t i = 0; i < num_fds; ++i) {
            if (plane_fds[i] >= 0) {
                ::close(plane_fds[i]);
            }
        }
    };

    if (desc.width == 0 || desc.height == 0 || num_fds == 0 || !plane_fds) {
        cleanupIncomingFds();
        emit dmaBufImportFailed(
            QStringLiteral("Invalid DMA-BUF parameters: dimensions 0 or no FDs"));
        return false;
    }

    GlContextScope glScope(this);
    if (!glScope.isReady()) {
        cleanupIncomingFds();
        emit dmaBufImportFailed(QStringLiteral("OpenGL context is not yet realized"));
        return false;
    }
    resolveEglExtensions();

    if (!m_eglCreateImageKHR || !m_glEGLImageTargetTexture2DOES) {
        cleanupIncomingFds();
        emit dmaBufImportFailed(QStringLiteral(
            "Missing EGL DMA-BUF extensions (eglCreateImageKHR / glEGLImageTargetTexture2DOES)"));
        return false;
    }

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = static_cast<EGLint>(desc.width);
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = static_cast<EGLint>(desc.height);
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = static_cast<EGLint>(desc.drm_format);

    uint32_t planesToImport =
        std::min({static_cast<uint32_t>(num_fds), desc.num_planes > 0 ? desc.num_planes : 1u, 4u});

    for (uint32_t p = 0; p < planesToImport; ++p) {
        if (plane_fds[p] < 0) {
            continue;
        }
        attribs[n++] = kPlaneFdKeys[p];
        attribs[n++] = plane_fds[p];
        attribs[n++] = kPlaneOffsetKeys[p];
        attribs[n++] = static_cast<EGLint>(desc.offsets[p]);
        attribs[n++] = kPlanePitchKeys[p];
        attribs[n++] = static_cast<EGLint>(desc.strides[p]);

        if (desc.modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = kPlaneModLoKeys[p];
            attribs[n++] = static_cast<EGLint>(desc.modifier & 0xFFFFFFFFULL);
            attribs[n++] = kPlaneModHiKeys[p];
            attribs[n++] = static_cast<EGLint>((desc.modifier >> 32) & 0xFFFFFFFFULL);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img =
        m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        cleanupIncomingFds();
        emit dmaBufImportFailed(
            QStringLiteral("eglCreateImageKHR failed with error 0x%1").arg(err, 0, 16));
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    if (m_eglCreateSyncKHR) {
        sync = m_eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        if (sync != EGL_NO_SYNC_KHR && m_eglWaitSyncKHR) {
            m_eglWaitSyncKHR(m_eglDisplay, sync, 0);
        }
    }

    DmaBufFrame newFrame;
    newFrame.width = desc.width;
    newFrame.height = desc.height;
    newFrame.drm_format = desc.drm_format;
    newFrame.modifier = desc.modifier;
    newFrame.num_planes = planesToImport;
    newFrame.flags = desc.flags;
    newFrame.outputName = QString::fromUtf8(desc.output);
    newFrame.eglImage = img;
    newFrame.glTexture = tex;
    newFrame.syncFence = sync;

    for (uint32_t p = 0; p < 4; ++p) {
        if (p < planesToImport) {
            newFrame.planes[p].fd = plane_fds[p];
            newFrame.planes[p].stride = desc.strides[p];
            newFrame.planes[p].offset = desc.offsets[p];
        } else {
            newFrame.planes[p].fd = -1;
        }
    }

    // Close any excess SCM_RIGHTS FDs beyond planesToImport so kernel descriptors never leak.
    for (size_t extra = planesToImport; extra < num_fds; ++extra) {
        if (plane_fds[extra] >= 0) {
            ::close(plane_fds[extra]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentDmaBufFrame.has_value()) {
            m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR,
                                            m_eglDestroySyncKHR);
        }
        m_currentDmaBufFrame = newFrame;
        m_previewMode = PreviewMode::DmaBufLive;
    }

    m_frameCount++;
    updateAnimationTimerState();
    emit dmaBufFrameRendered(desc.width, desc.height, m_frameCount);
    update();
    return true;
}

bool GpuPreviewViewport::requestLiveDmaBuf(int socketFd, const QString &outputName)
{
    if (socketFd < 0) {
        return false;
    }

    waywal_ipc_hdr_t req_hdr;
    std::memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.magic = WAYWAL_IPC_MAGIC;
    req_hdr.version = WAYWAL_IPC_VERSION;
    req_hdr.opcode = WAYWAL_REQ_EXPORT_OUTPUT_DMABUF;
    req_hdr.payload_size = sizeof(waywal_export_dmabuf_req_t);

    waywal_export_dmabuf_req_t req_pay;
    std::memset(&req_pay, 0, sizeof(req_pay));
    if (!outputName.isEmpty()) {
        std::strncpy(req_pay.output, outputName.toUtf8().constData(), sizeof(req_pay.output) - 1);
    }

    if (!waywal_ipc_send(socketFd, &req_hdr, -1)) {
        return false;
    }
    if (::write(socketFd, &req_pay, sizeof(req_pay)) != static_cast<ssize_t>(sizeof(req_pay))) {
        return false;
    }

    waywal_ipc_hdr_t resp_hdr;
    int fds[4] = {-1, -1, -1, -1};
    size_t num_fds = 0;
    if (!waywal_ipc_recv_fds(socketFd, &resp_hdr, fds, 4, &num_fds)) {
        return false;
    }

    if ((resp_hdr.opcode != WAYWAL_RESP_OK && resp_hdr.opcode != WAYWAL_RESP_DMABUF) ||
        resp_hdr.payload_size < sizeof(waywal_export_dmabuf_resp_t)) {
        for (size_t i = 0; i < num_fds; ++i) {
            if (fds[i] >= 0)
                ::close(fds[i]);
        }
        return false;
    }

    waywal_export_dmabuf_resp_t resp_pay;
    if (::read(socketFd, &resp_pay, sizeof(resp_pay)) != static_cast<ssize_t>(sizeof(resp_pay))) {
        for (size_t i = 0; i < num_fds; ++i) {
            if (fds[i] >= 0)
                ::close(fds[i]);
        }
        return false;
    }

    return importDmaBufFrame(resp_pay, fds, num_fds);
}

bool GpuPreviewViewport::requestLiveDmaBufFromNamespace(const QString &namespaceStr,
                                                        const QString &outputName)
{
    path_buf_t sockPath;
    if (!waywal_ipc_resolve_socket_path(&sockPath, namespaceStr.toUtf8().constData())) {
        return false;
    }

    int clientFd = waywal_ipc_client_connect(namespaceStr.toUtf8().constData(), 200);
    if (clientFd < 0) {
        return false;
    }

    bool ok = requestLiveDmaBuf(clientFd, outputName);
    ::close(clientFd);
    return ok;
}

void GpuPreviewViewport::setPreviewImage(const QImage &image)
{
    if (image.isNull()) {
        clearPreviewImage();
        return;
    }

    m_fallbackImage = image;
    QImage formatted = image.convertToFormat(QImage::Format_RGBA8888);
    m_imageWidth = static_cast<uint32_t>(formatted.width());
    m_imageHeight = static_cast<uint32_t>(formatted.height());

    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
            if (m_inProcessImageTex == 0) {
                glGenTextures(1, &m_inProcessImageTex);
            }
            glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, formatted.constBits());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    m_previewMode = PreviewMode::InProcessImage;
    updateAnimationTimerState();
    update();
}

bool GpuPreviewViewport::setPreviewImagePath(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return false;
    }

    QString targetPath = filePath;
    std::string stdPath = filePath.toStdString();
    std::string ext = std::filesystem::path(stdPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // 1. Resolve Gnome XML timed wallpaper first image
    if (ext == ".xml" && QFileInfo::exists(filePath)) {
        std::ifstream xmlFile(stdPath);
        if (xmlFile.is_open()) {
            std::stringstream ss;
            ss << xmlFile.rdbuf();
            std::string content = ss.str();
            for (const char *tag : {"<file>", "<from>"}) {
                std::string closeTag = (std::string(tag) == "<file>") ? "</file>" : "</from>";
                auto start = content.find(tag);
                auto end = content.find(closeTag, start);
                if (start != std::string::npos && end != std::string::npos) {
                    start += std::strlen(tag);
                    std::string inner = content.substr(start, end - start);
                    auto first = inner.find_first_not_of(" \t\r\n");
                    auto last = inner.find_last_not_of(" \t\r\n");
                    if (first != std::string::npos && last != std::string::npos) {
                        std::string candidate = inner.substr(first, last - first + 1);
                        if (QFileInfo::exists(QString::fromStdString(candidate))) {
                            targetPath = QString::fromStdString(candidate);
                            stdPath = candidate;
                            break;
                        }
                    }
                }
            }
        }
    }

    // 2. Try standard Qt QImage reader
    QImage img(targetPath);

    // 3. Fallback to libwaywal_core's image_load_rgba (JXL, AVIF, WebP, SVG, QOI, HDR, etc.)
    if (img.isNull() && QFileInfo::exists(targetPath)) {
        uint8_t *rgba = nullptr;
        uint32_t w = 0, h = 0;
        if (image_load_rgba(stdPath.c_str(), &rgba, &w, &h) && rgba && w > 0 && h > 0) {
            QImage raw(rgba, static_cast<int>(w), static_cast<int>(h), static_cast<int>(w * 4),
                       QImage::Format_RGBA8888);
            img = raw.copy();
            image_free(rgba);
        } else if (rgba) {
            image_free(rgba);
        }
    }

    // 4. Video wallpaper preview frame extraction (check disk cache first, then async ffmpeg)
    if (img.isNull() && QFileInfo::exists(targetPath) &&
        (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov")) {
        const char *xdgCache = std::getenv("XDG_CACHE_HOME");
        std::filesystem::path cacheDir;
        if (xdgCache && xdgCache[0] != '\0') {
            cacheDir = std::filesystem::path(xdgCache) / "wwal" / "thumbnails";
        } else {
            const char *home = std::getenv("HOME");
            cacheDir =
                std::filesystem::path(home ? home : "/tmp") / ".cache" / "wwal" / "thumbnails";
        }
        QByteArray hash = QCryptographicHash::hash(
            QByteArray::fromRawData(stdPath.data(), static_cast<qsizetype>(stdPath.size())),
            QCryptographicHash::Sha256);
        const std::filesystem::path cachedThumb = cacheDir / (hash.toHex().toStdString() + ".webp");
        std::error_code ec;
        if (std::filesystem::exists(cachedThumb, ec)) {
            img.load(QString::fromStdString(cachedThumb.string()));
        }

        if (img.isNull()) {
            // Render immediate procedural placeholder and decode video frame off the GUI thread.
            QImage placeholder(640, 360, QImage::Format_RGBA8888);
            QPainter pp(&placeholder);
            QLinearGradient vgrad(0, 0, 640, 360);
            vgrad.setColorAt(0.0, QColor(24, 32, 48));
            vgrad.setColorAt(1.0, QColor(14, 18, 28));
            pp.fillRect(placeholder.rect(), vgrad);
            pp.end();
            setPreviewImage(placeholder);

            const uint64_t reqGen =
                m_videoLoadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (m_videoThumbThread.joinable()) {
                m_videoThumbThread.request_stop();
                m_videoThumbThread.join();
            }
            QPointer<GpuPreviewViewport> safeSelf(this);
            m_videoThumbThread = std::jthread([safeSelf, targetPath, reqGen](std::stop_token st) {
                if (st.stop_requested()) {
                    return;
                }
                QProcess proc;
                proc.start(QStringLiteral("ffmpeg"),
                           {QStringLiteral("-loglevel"), QStringLiteral("error"),
                            QStringLiteral("-ss"), QStringLiteral("00:00:00.50"),
                            QStringLiteral("-i"), targetPath, QStringLiteral("-frames:v"),
                            QStringLiteral("1"), QStringLiteral("-vf"),
                            QStringLiteral("scale=1280:-1"), QStringLiteral("-f"),
                            QStringLiteral("image2pipe"), QStringLiteral("-vcodec"),
                            QStringLiteral("bmp"), QStringLiteral("-")});
                if (!proc.waitForFinished(2500) || proc.exitCode() != 0 || st.stop_requested()) {
                    return;
                }
                const QByteArray data = proc.readAllStandardOutput();
                QImage decoded;
                if (!data.isEmpty()) {
                    decoded.loadFromData(data, "BMP");
                }
                if (decoded.isNull() || !safeSelf || !qApp) {
                    return;
                }
                QMetaObject::invokeMethod(
                    qApp,
                    [safeSelf, decoded = std::move(decoded), reqGen]() {
                        if (safeSelf && safeSelf->m_videoLoadGeneration.load(
                                            std::memory_order_acquire) == reqGen) {
                            safeSelf->setPreviewImage(decoded);
                        }
                    },
                    Qt::QueuedConnection);
            });
            return true;
        }
    }

    // 5. Synthetic showcase preview fallback for built-in /usr/share/wwal/samples/* cards
    if (img.isNull() && filePath.startsWith(QLatin1String("/usr/share/wwal/samples/"))) {
        img = QImage(1280, 720, QImage::Format_RGBA8888);
        uint32_t seed = 2166136261u;
        for (char c : stdPath) {
            seed ^= static_cast<uint8_t>(c);
            seed *= 16777619u;
        }
        QColor c1(static_cast<int>((seed >> 16) & 0x7F) + 40,
                  static_cast<int>((seed >> 8) & 0x7F) + 50, static_cast<int>(seed & 0x7F) + 80);
        QColor c2(static_cast<int>((seed >> 12) & 0x7F) + 20,
                  static_cast<int>((seed >> 4) & 0x7F) + 30,
                  static_cast<int>((seed >> 20) & 0x7F) + 60);
        QPainter p(&img);
        QLinearGradient grad(0, 0, 1280, 720);
        grad.setColorAt(0.0, c1.lighter(135));
        grad.setColorAt(0.5, c2);
        grad.setColorAt(1.0, c1.darker(165));
        p.fillRect(img.rect(), grad);
        p.end();
    }

    if (img.isNull()) {
        return false;
    }
    setPreviewImage(img);
    return true;
}

void GpuPreviewViewport::clearPreviewImage()
{
    m_videoLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_fallbackImage = QImage();
    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            if (m_inProcessImageTex != 0) {
                glDeleteTextures(1, &m_inProcessImageTex);
                m_inProcessImageTex = 0;
            }
        } else {
            m_inProcessImageTex = 0;
        }
    }
    m_imageWidth = 0;
    m_imageHeight = 0;

    if (m_previewMode == PreviewMode::InProcessImage) {
        if (m_currentDmaBufFrame.has_value()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (m_shaderProgram || m_computeShaderProgram || !m_shaderSource.isEmpty()) {
            m_previewMode = PreviewMode::InProcessShader;
        } else {
            m_previewMode = PreviewMode::OfflineEmpty;
        }
    }
    updateAnimationTimerState();
    update();
}

bool GpuPreviewViewport::compileShaderInternal(const QString &glslSource, QString *outError)
{
    const QString trimmed = glslSource.trimmed();
    if (trimmed.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("Empty GLSL shader source");
        }
        return false;
    }

    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();

    auto compileLinkedFragmentProgram =
        [&](const QString &fragSrc, bool graphVaryings,
            QString *errOut) -> std::unique_ptr<QOpenGLShaderProgram> {
        struct Variant {
            const char *vs;
            QString fs;
        };
        std::vector<Variant> attempts;
        if (graphVaryings) {
            if (isGles) {
                attempts.push_back({kGraphVertexShaderEs310, fragSrc});
                attempts.push_back({kGraphVertexShaderEs300, convertEs310ToEs300(fragSrc)});
            } else {
                attempts.push_back(
                    {kGraphVertexShaderCore330, convertEsFragmentToCore330(fragSrc)});
                attempts.push_back({kGraphVertexShaderEs310, fragSrc});
            }
        } else {
            if (isGles) {
                attempts.push_back({kBlitVertexShader, fragSrc});
                if (fragSrc.contains(QLatin1String("#version 310 es"))) {
                    attempts.push_back({kGraphVertexShaderEs310, fragSrc});
                    attempts.push_back({kBlitVertexShader, convertEs310ToEs300(fragSrc)});
                }
            } else {
                attempts.push_back({kBlitVertexShaderCore, convertEsFragmentToCore330(fragSrc)});
                attempts.push_back({kBlitVertexShader, fragSrc});
            }
        }

        QString lastLog;
        for (const auto &attempt : attempts) {
            auto prog = std::make_unique<QOpenGLShaderProgram>(this);
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, attempt.vs)) {
                lastLog = prog->log();
                continue;
            }
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Fragment, attempt.fs)) {
                lastLog = prog->log();
                continue;
            }
            if (!prog->link()) {
                lastLog = prog->log();
                continue;
            }
            return prog;
        }
        if (errOut) {
            *errOut = lastLog;
        }
        return nullptr;
    };

    // 1. Compute Shader support (e.g. #version 310 es / #version 430 with local_size_x +
    // imageStore)
    if (trimmed.contains(QLatin1String("local_size_x")) &&
        trimmed.contains(QLatin1String("imageStore"))) {
        auto csProg = std::make_unique<QOpenGLShaderProgram>(this);
        if (!csProg->addShaderFromSourceCode(QOpenGLShader::Compute, trimmed) || !csProg->link()) {
            if (outError) {
                *outError = csProg->log();
            }
            return false;
        }
        m_computeShaderProgram = std::move(csProg);
        m_shaderProgram.reset();
        for (auto &bp : m_bufferPassPrograms) {
            bp.reset();
        }
        for (auto &pair : m_bufferPassFbos) {
            pair[0].reset();
            pair[1].reset();
        }
        m_bufferPingPongIdx = 0;
        return true;
    }

    // 2. .wwgraph multi-pass / single-pass #pragma Pass GLSL output
    if (trimmed.contains(QLatin1String("#pragma Pass"))) {
        const std::vector<GraphPassSource> passes = parseGraphPasses(trimmed);
        if (passes.empty()) {
            if (outError) {
                *outError = QStringLiteral("No shader passes found after #pragma Pass directive");
            }
            return false;
        }

        std::array<std::unique_ptr<QOpenGLShaderProgram>, 4> nextBufferProgs{};
        std::unique_ptr<QOpenGLShaderProgram> nextImageProg;

        for (const auto &pass : passes) {
            QString passErr;
            auto compiled =
                compileLinkedFragmentProgram(pass.glsl, /*graphVaryings=*/true, &passErr);
            if (!compiled) {
                if (outError) {
                    *outError = passErr;
                }
                return false;
            }
            if (pass.passIndex >= 0 && pass.passIndex < 4) {
                nextBufferProgs[static_cast<size_t>(pass.passIndex)] = std::move(compiled);
            } else {
                nextImageProg = std::move(compiled);
            }
        }

        if (!nextImageProg) {
            // If a graph only emitted buffer passes, promote the highest active buffer pass to
            // screen
            for (int i = 3; i >= 0; --i) {
                if (nextBufferProgs[static_cast<size_t>(i)]) {
                    nextImageProg = std::move(nextBufferProgs[static_cast<size_t>(i)]);
                    break;
                }
            }
        }

        m_computeShaderProgram.reset();
        m_bufferPassPrograms = std::move(nextBufferProgs);
        m_shaderProgram = std::move(nextImageProg);
        m_bufferPingPongIdx = 0;
        for (size_t i = 0; i < 4; ++i) {
            for (size_t slot = 0; slot < 2; ++slot) {
                if (m_bufferPassPrograms[i] && m_bufferPassFbos[i][slot] &&
                    m_bufferPassFbos[i][slot]->bind()) {
                    glViewport(0, 0, m_bufferPassFbos[i][slot]->width(),
                               m_bufferPassFbos[i][slot]->height());
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    m_bufferPassFbos[i][slot]->release();
                }
            }
        }
        ensureNeutralPassTexture();
        const QSize fbSize = physicalFramebufferSize();
        ensureOffscreenFbo(fbSize.width(), fbSize.height());
        ensureBufferPassFbos(fbSize.width(), fbSize.height());
        return true;
    }

    // 3. Shadertoy (mainImage) or standalone fragment shader
    QString fullShaderSource;
    const bool usesGraphVarying = trimmed.contains(QLatin1String("v_TexCoord"));
    if (trimmed.contains(QLatin1String("mainImage"))) {
        const char *preamble = isGles ? kShadertoyPreamble : kShadertoyPreambleCore;
        fullShaderSource =
            QString::fromUtf8(preamble) + trimmed + QString::fromUtf8(kShadertoyEpilogue);
    } else if (trimmed.contains(QLatin1String("#version"))) {
        fullShaderSource = trimmed;
    } else {
        const char *preamble = isGles ? kShadertoyPreamble : kShadertoyPreambleCore;
        fullShaderSource = QString::fromUtf8(preamble) + trimmed;
    }

    auto program = compileLinkedFragmentProgram(fullShaderSource, usesGraphVarying, outError);
    if (!program) {
        return false;
    }

    m_computeShaderProgram.reset();
    for (auto &bp : m_bufferPassPrograms) {
        bp.reset();
    }
    for (auto &pair : m_bufferPassFbos) {
        pair[0].reset();
        pair[1].reset();
    }
    m_bufferPingPongIdx = 0;
    m_shaderProgram = std::move(program);
    ensureNeutralPassTexture();
    const QSize fbSize = physicalFramebufferSize();
    ensureOffscreenFbo(fbSize.width(), fbSize.height());
    return true;
}

bool GpuPreviewViewport::setPreviewShader(const QString &glslSource, QString *outError)
{
    if (glslSource.trimmed().isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("Empty GLSL shader source");
        }
        return false;
    }

    if (!isValid()) {
        m_shaderSource = glslSource;
        m_shaderTimer.restart();
        m_lastFrameTimeNs = 0;
        m_shaderFrameIndex = 0;
        m_previewMode = PreviewMode::InProcessShader;
        updateAnimationTimerState();
        update();
        return true;
    }

    {
        GlContextScope glScope(this);
        if (!glScope.isReady() || !compileShaderInternal(glslSource, outError)) {
            return false;
        }
    }

    m_shaderSource = glslSource;
    m_shaderTimer.restart();
    m_lastFrameTimeNs = 0;
    m_shaderFrameIndex = 0;
    m_previewMode = PreviewMode::InProcessShader;
    updateAnimationTimerState();
    update();
    return true;
}

void GpuPreviewViewport::clearPreviewShader()
{
    {
        GlContextScope glScope(this);
        m_shaderProgram.reset();
        m_computeShaderProgram.reset();
        for (auto &bp : m_bufferPassPrograms) {
            bp.reset();
        }
        for (auto &pair : m_bufferPassFbos) {
            pair[0].reset();
            pair[1].reset();
        }
        m_bufferPingPongIdx = 0;
    }
    m_shaderSource.clear();

    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_currentDmaBufFrame.has_value()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (m_inProcessImageTex != 0 || !m_fallbackImage.isNull()) {
            m_previewMode = PreviewMode::InProcessImage;
        } else {
            m_previewMode = PreviewMode::OfflineEmpty;
        }
    }
    updateAnimationTimerState();
    update();
}

void GpuPreviewViewport::setShaderUniform(const QString &name, float value)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Float;
    val.fVals.assign(1, value);
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::setShaderUniform(const QString &name, float v0, float v1)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Vec2;
    val.fVals = {v0, v1};
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::setShaderUniform(const QString &name, float v0, float v1, float v2)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Vec3;
    val.fVals = {v0, v1, v2};
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::setShaderUniform(const QString &name, float v0, float v1, float v2,
                                          float v3)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Vec4;
    val.fVals = {v0, v1, v2, v3};
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::setShaderUniform(const QString &name, const QVector2D &vec)
{
    setShaderUniform(name, vec.x(), vec.y());
}

void GpuPreviewViewport::setShaderUniform(const QString &name, const QVector3D &vec)
{
    setShaderUniform(name, vec.x(), vec.y(), vec.z());
}

void GpuPreviewViewport::setShaderUniform(const QString &name, const QVector4D &vec)
{
    setShaderUniform(name, vec.x(), vec.y(), vec.z(), vec.w());
}

void GpuPreviewViewport::setShaderUniform(const QString &name, int value)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Int;
    val.iVals.assign(1, value);
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::setShaderUniform(const QString &name, const QMatrix4x4 &mat)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Mat4;
    val.matVal = mat;
    val.fVals.assign(mat.constData(), mat.constData() + 16);
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::clearShaderUniforms() { m_customUniforms.clear(); }

void GpuPreviewViewport::setZoom(float zoom)
{
    m_zoom = std::clamp(zoom, 0.1f, 10.0f);
    emit zoomChanged(m_zoom);
    update();
}

void GpuPreviewViewport::setPanOffset(const QPointF &offset)
{
    m_panOffset = offset;
    emit panChanged(m_panOffset);
    update();
}

void GpuPreviewViewport::resetView()
{
    m_zoom = 1.0f;
    m_panOffset = QPointF(0.0, 0.0);
    m_targetTiltX = 0.0f;
    m_targetTiltY = 0.0f;
    m_currentTiltX = 0.0f;
    m_currentTiltY = 0.0f;
    emit zoomChanged(m_zoom);
    emit panChanged(m_panOffset);
    update();
}

void GpuPreviewViewport::setParallaxEnabled(bool enabled)
{
    if (m_parallaxEnabled != enabled) {
        m_parallaxEnabled = enabled;
        if (!enabled) {
            m_targetTiltX = 0.0f;
            m_targetTiltY = 0.0f;
            m_currentTiltX = 0.0f;
            m_currentTiltY = 0.0f;
        }
        updateAnimationTimerState();
        emit parallaxToggled(enabled);
        update();
    }
}

void GpuPreviewViewport::setParallaxStrength(float strength)
{
    m_parallaxStrength = std::clamp(strength, 0.0f, 5.0f);
    update();
}

void GpuPreviewViewport::setShowSafeGuides(bool show)
{
    m_showSafeGuides = show;
    update();
}

void GpuPreviewViewport::setAspectRatioGuide(AspectRatioGuide guide)
{
    m_aspectRatioGuide = guide;
    emit aspectRatioGuideChanged(guide);
    update();
}

void GpuPreviewViewport::setShowActionBar(bool show)
{
    m_showActionBar = show;
    update();
}

void GpuPreviewViewport::setSelectedMonitor(const QString &monitorName)
{
    if (m_selectedMonitor != monitorName) {
        m_selectedMonitor = monitorName;
        emit monitorSelectionChanged(monitorName);
        update();
    }
}

void GpuPreviewViewport::setAvailableMonitors(const QStringList &monitors)
{
    m_availableMonitors = monitors;
    if (!m_availableMonitors.isEmpty() && !m_availableMonitors.contains(m_selectedMonitor)) {
        setSelectedMonitor(m_availableMonitors.first());
    }
    update();
}

void GpuPreviewViewport::applyShaderUniforms(QOpenGLShaderProgram &prog, float fboW, float fboH,
                                             float timeSec, float timeDeltaSec, int activeWritePass)
{
    const float safeH = std::max(1.0f, fboH);
    const float safeW = std::max(1.0f, fboW);

    // Standard Shadertoy uniforms
    prog.setUniformValue("iResolution", QVector3D(safeW, safeH, safeW / safeH));
    prog.setUniformValue("iTime", timeSec);
    prog.setUniformValue("iTimeDelta", timeDeltaSec);
    prog.setUniformValue("iFrame", m_shaderFrameIndex);
    prog.setUniformValue("iMouse", QVector4D(static_cast<float>(m_mouseGlCoord.x()),
                                             static_cast<float>(m_mouseGlCoord.y()),
                                             static_cast<float>(m_mouseClickCoord.x()),
                                             static_cast<float>(m_mouseClickCoord.y())));

    const QDateTime dt = QDateTime::currentDateTime();
    const float secOfDay = dt.time().msecsSinceStartOfDay() / 1000.0f;
    prog.setUniformValue("iDate", QVector4D(static_cast<float>(dt.date().year()),
                                            static_cast<float>(dt.date().month()),
                                            static_cast<float>(dt.date().day()), secOfDay));

    // Standard wwald / .wwgraph uniforms
    prog.setUniformValue("u_Time", timeSec);
    prog.setUniformValue("u_Resolution", QVector2D(safeW, safeH));
    const float ptrX = std::clamp(static_cast<float>(m_mouseGlCoord.x()) /
                                      std::max(1.0f, static_cast<float>(width())),
                                  0.0f, 1.0f);
    const float ptrY = std::clamp(static_cast<float>(m_mouseGlCoord.y()) /
                                      std::max(1.0f, static_cast<float>(height())),
                                  0.0f, 1.0f);
    prog.setUniformValue("u_Pointer", QVector2D(ptrX, ptrY));

    static const QVector4D kZeroAudio[4] = {
        QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
        QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
        QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
        QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
    };
    const int audioLoc = prog.uniformLocation("u_AudioBands");
    if (audioLoc >= 0) {
        prog.setUniformValueArray(audioLoc, kZeroAudio, 4);
    }

    ensureNeutralPassTexture();
    const GLuint fallbackSourceTex =
        (m_inProcessImageTex != 0) ? m_inProcessImageTex : m_neutralPassTex;
    const int readSlot = m_bufferPingPongIdx & 1;
    const int writeSlot = 1 - readSlot;
    GLuint passTex[5] = {
        fallbackSourceTex, m_neutralPassTex, m_neutralPassTex, m_neutralPassTex, m_neutralPassTex,
    };
    for (int u = 0; u < 4; ++u) {
        const int slot = (u < activeWritePass) ? writeSlot : readSlot;
        const auto &fbo = m_bufferPassFbos[static_cast<size_t>(u)][static_cast<size_t>(slot)];
        if (fbo && fbo->texture() != 0) {
            passTex[u] = fbo->texture();
        }
    }
    static constexpr const char *kPassSamplerNames[5] = {
        "u_BufferA", "u_BufferB", "u_BufferC", "u_BufferD", "u_DepthMap",
    };
    for (int unit = 0; unit < 5; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, passTex[unit]);
        prog.setUniformValue(kPassSamplerNames[unit], unit);
    }
    glActiveTexture(GL_TEXTURE0);

    // Custom inspector & graph node constant uniforms
    for (const auto &[name, val] : m_customUniforms) {
        const char *cName = name.c_str();
        switch (val.type) {
        case ShaderUniformValue::Type::Float:
            if (!val.fVals.empty())
                prog.setUniformValue(cName, val.fVals[0]);
            break;
        case ShaderUniformValue::Type::Vec2:
            if (val.fVals.size() >= 2)
                prog.setUniformValue(cName, QVector2D(val.fVals[0], val.fVals[1]));
            break;
        case ShaderUniformValue::Type::Vec3:
            if (val.fVals.size() >= 3)
                prog.setUniformValue(cName, QVector3D(val.fVals[0], val.fVals[1], val.fVals[2]));
            break;
        case ShaderUniformValue::Type::Vec4:
            if (val.fVals.size() >= 4)
                prog.setUniformValue(
                    cName, QVector4D(val.fVals[0], val.fVals[1], val.fVals[2], val.fVals[3]));
            break;
        case ShaderUniformValue::Type::Int:
            if (!val.iVals.empty())
                prog.setUniformValue(cName, val.iVals[0]);
            break;
        case ShaderUniformValue::Type::Mat4:
            prog.setUniformValue(cName, val.matVal);
            break;
        }
    }

    QMatrix4x4 ident;
    prog.setUniformValue("u_mvp", ident);
}

void GpuPreviewViewport::renderInProcessShaderPass()
{
    const qint64 nowNs = m_shaderTimer.nsecsElapsed();
    const float timeSec = static_cast<float>(nowNs) / 1e9f;
    const float timeDeltaSec = (m_lastFrameTimeNs > 0)
                                   ? static_cast<float>(nowNs - m_lastFrameTimeNs) / 1e9f
                                   : (1.0f / static_cast<float>(std::max(1, m_targetFps)));
    m_lastFrameTimeNs = nowNs;

    if (m_computeShaderProgram && QOpenGLContext::currentContext()) {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        auto texStorage2D =
            reinterpret_cast<GlTexStorage2DProc>(ctx->getProcAddress("glTexStorage2D"));
        auto bindImageTex =
            reinterpret_cast<GlBindImageTextureProc>(ctx->getProcAddress("glBindImageTexture"));
        auto dispatchCompute =
            reinterpret_cast<GlDispatchComputeProc>(ctx->getProcAddress("glDispatchCompute"));
        auto memoryBarrier =
            reinterpret_cast<GlMemoryBarrierProc>(ctx->getProcAddress("glMemoryBarrier"));

        const QSize fbSize = physicalFramebufferSize();
        const int targetW = std::clamp(fbSize.width(), 64, 7680);
        const int targetH = std::clamp(fbSize.height(), 64, 4320);
        if (texStorage2D && bindImageTex && dispatchCompute && memoryBarrier) {
            if (m_computeOutputTex == 0 || m_computeTexWidth != targetW ||
                m_computeTexHeight != targetH) {
                if (m_computeOutputTex != 0) {
                    glDeleteTextures(1, &m_computeOutputTex);
                    m_computeOutputTex = 0;
                }
                glGenTextures(1, &m_computeOutputTex);
                glBindTexture(GL_TEXTURE_2D, m_computeOutputTex);
                texStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, targetW, targetH);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                m_computeTexWidth = targetW;
                m_computeTexHeight = targetH;
            }

            m_computeShaderProgram->bind();
            applyShaderUniforms(*m_computeShaderProgram, static_cast<float>(targetW),
                                static_cast<float>(targetH), timeSec, timeDeltaSec);
            bindImageTex(1, m_computeOutputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            dispatchCompute(static_cast<GLuint>((targetW + 15) / 16),
                            static_cast<GLuint>((targetH + 15) / 16), 1u);
            memoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            m_computeShaderProgram->release();
            m_shaderFrameIndex++;
        }
        return;
    }

    if (!m_shaderProgram) {
        return;
    }

    const QSize fbSize = physicalFramebufferSize();
    ensureOffscreenFbo(fbSize.width(), fbSize.height());
    ensureBufferPassFbos(fbSize.width(), fbSize.height());

    auto drawFullscreenQuad = [this]() {
        m_quadVao.bind();
        m_quadVbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, u)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(0);
        m_quadVbo.release();
        m_quadVao.release();
    };

    const int writeSlot = 1 - (m_bufferPingPongIdx & 1);

    // Render BUFFER_A..BUFFER_D into writeSlot while sampling upstream passes from writeSlot and
    // self/downstream feedback passes from readSlot to prevent FBO feedback loops.
    for (size_t i = 0; i < 4; ++i) {
        auto &writeFbo = m_bufferPassFbos[i][static_cast<size_t>(writeSlot)];
        if (!m_bufferPassPrograms[i] || !writeFbo) {
            continue;
        }
        writeFbo->bind();
        const float bw = static_cast<float>(writeFbo->width());
        const float bh = static_cast<float>(writeFbo->height());
        glViewport(0, 0, writeFbo->width(), writeFbo->height());

        m_bufferPassPrograms[i]->bind();
        applyShaderUniforms(*m_bufferPassPrograms[i], bw, bh, timeSec, timeDeltaSec,
                            static_cast<int>(i));
        drawFullscreenQuad();
        m_bufferPassPrograms[i]->release();
        writeFbo->release();
    }

    m_offscreenFbo->bind();
    const float fboW = static_cast<float>(m_offscreenFbo->width());
    const float fboH = static_cast<float>(m_offscreenFbo->height());
    glViewport(0, 0, m_offscreenFbo->width(), m_offscreenFbo->height());
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_shaderProgram->bind();
    applyShaderUniforms(*m_shaderProgram, fboW, fboH, timeSec, timeDeltaSec, 4);
    drawFullscreenQuad();
    m_shaderProgram->release();
    m_offscreenFbo->release();

    m_bufferPingPongIdx ^= 1;

    for (int unit = 4; unit >= 0; --unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_shaderFrameIndex++;
}

float GpuPreviewViewport::calculateGuideAspect() const noexcept
{
    switch (m_aspectRatioGuide) {
    case AspectRatioGuide::Ratio16_9:
        return 16.0f / 9.0f;
    case AspectRatioGuide::Ratio16_10:
        return 16.0f / 10.0f;
    case AspectRatioGuide::Ratio21_9:
        return 21.0f / 9.0f;
    case AspectRatioGuide::Ratio32_9:
        return 32.0f / 9.0f;
    case AspectRatioGuide::Ratio4_3:
        return 4.0f / 3.0f;
    case AspectRatioGuide::MatchActiveDisplay:
    case AspectRatioGuide::None:
    default:
        return static_cast<float>(width()) / std::max(1.0f, static_cast<float>(height()));
    }
}

QRectF GpuPreviewViewport::computeContentDisplayRect() const noexcept
{
    if (m_parallaxEnabled && width() > 0 && height() > 0) {
        const uint32_t cWU = std::max<uint32_t>(1u, contentWidth());
        const uint32_t cHU = std::max<uint32_t>(1u, contentHeight());
        const QMatrix4x4 mvp = computeBlitMvp(cWU, cHU);
        const QVector3D corners[4] = {
            QVector3D(-1.0f, -1.0f, 0.0f),
            QVector3D(1.0f, -1.0f, 0.0f),
            QVector3D(1.0f, 1.0f, 0.0f),
            QVector3D(-1.0f, 1.0f, 0.0f),
        };
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();
        bool anyValid = false;
        for (const QVector3D &c : corners) {
            const QVector4D clip = mvp.map(QVector4D(c, 1.0f));
            if (std::abs(clip.w()) > 1e-6f) {
                const float invW = 1.0f / clip.w();
                const double sx = (clip.x() * invW * 0.5 + 0.5) * static_cast<double>(width());
                const double sy =
                    (1.0 - (clip.y() * invW * 0.5 + 0.5)) * static_cast<double>(height());
                minX = std::min(minX, sx);
                minY = std::min(minY, sy);
                maxX = std::max(maxX, sx);
                maxY = std::max(maxY, sy);
                anyValid = true;
            }
        }
        if (anyValid) {
            return QRectF(minX, minY, std::max(1.0, maxX - minX), std::max(1.0, maxY - minY));
        }
    }

    float cW = static_cast<float>(contentWidth());
    float cH = static_cast<float>(contentHeight());
    if (cW <= 0.0f || cH <= 0.0f) {
        cW = static_cast<float>(width());
        cH = static_cast<float>(height());
    }

    float cAspect = cW / cH;
    float vW = static_cast<float>(width());
    float vH = static_cast<float>(height());
    float vAspect = vW / std::max(1.0f, vH);

    QRectF baseRect;
    if (cAspect > vAspect) {
        float h = vW / cAspect;
        baseRect = QRectF(0.0f, (vH - h) * 0.5f, vW, h);
    } else {
        float w = vH * cAspect;
        baseRect = QRectF((vW - w) * 0.5f, 0.0f, w, vH);
    }

    QPointF center = baseRect.center() + m_panOffset;
    float scaledW = baseRect.width() * m_zoom;
    float scaledH = baseRect.height() * m_zoom;
    return QRectF(center.x() - scaledW * 0.5f, center.y() - scaledH * 0.5f, scaledW, scaledH);
}

void GpuPreviewViewport::updateFpsTelemetry()
{
    m_fpsFrameCounter++;
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFpsCalcTime).count();
    if (elapsedMs >= 250) {
        m_currentFps =
            (static_cast<float>(m_fpsFrameCounter) * 1000.0f) / static_cast<float>(elapsedMs);
        m_fpsFrameCounter = 0;
        m_lastFpsCalcTime = now;
        emit fpsUpdated(m_currentFps);
    }
}

void GpuPreviewViewport::paintGL()
{
    if (isOffscreenPlatform()) {
        return;
    }
    // Reset any GL state left dirty by QOpenGL2PaintEngineEx (e.g. setClipPath stencil/scissor)
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    if (m_previewMode == PreviewMode::InProcessImage && m_inProcessImageTex == 0 &&
        !m_fallbackImage.isNull()) {
        QImage formatted = m_fallbackImage.convertToFormat(QImage::Format_RGBA8888);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
        glGenTextures(1, &m_inProcessImageTex);
        glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, formatted.constBits());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    GLuint activeTex = 0;
    int flipY = 0;
    int isBgr = 0;
    uint32_t cW = contentWidth();
    uint32_t cH = contentHeight();

    if (m_previewMode == PreviewMode::DmaBufLive) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentDmaBufFrame.has_value()) {
            activeTex = m_currentDmaBufFrame->glTexture;
            flipY = 1; // Wayland DMA-BUF coordinate convention
            // Check for BGR DRM formats
            if (m_currentDmaBufFrame->drm_format == DRM_FORMAT_XBGR8888 ||
                m_currentDmaBufFrame->drm_format == DRM_FORMAT_ABGR8888) {
                isBgr = 1;
            }
        }
    } else if (m_previewMode == PreviewMode::InProcessShader) {
        if (!m_shaderProgram && !m_computeShaderProgram && !m_shaderSource.isEmpty()) {
            (void)compileShaderInternal(m_shaderSource, nullptr);
        }
        renderInProcessShaderPass();
        if (m_computeShaderProgram && m_computeOutputTex != 0) {
            activeTex = m_computeOutputTex;
            flipY = 0;
        } else if (m_offscreenFbo) {
            activeTex = m_offscreenFbo->texture();
            flipY = 0; // FBO is already in OpenGL bottom-left origin coordinates
        }
    } else if (m_previewMode == PreviewMode::InProcessImage) {
        activeTex = m_inProcessImageTex;
        flipY = 1; // QImage::constBits() has row 0 at top-left
    }

    const qreal dpr = devicePixelRatioF();
    const int fbW = std::max(1, static_cast<int>(std::round(width() * dpr)));
    const int fbH = std::max(1, static_cast<int>(std::round(height() * dpr)));

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (activeTex != 0 && m_blitProgram) {
        m_blitProgram->bind();

        const QMatrix4x4 mvp = computeBlitMvp(cW, cH);

        m_blitProgram->setUniformValue("u_mvp", mvp);
        m_blitProgram->setUniformValue("u_texture", 0);
        m_blitProgram->setUniformValue("u_flip_y", flipY);
        m_blitProgram->setUniformValue("u_is_bgr", isBgr);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, activeTex);

        m_quadVao.bind();
        m_quadVbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, u)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(0);
        m_quadVbo.release();
        m_quadVao.release();

        glBindTexture(GL_TEXTURE_2D, 0);
        m_blitProgram->release();
    }

    renderGizmoOverlayPass(cW, cH);

    // --- Interactive 2D Overlays via QPainter ---
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QRectF displayRect = computeContentDisplayRect();

    if (activeTex == 0 || !m_blitProgram) {
        drawFallbackBackdrop(painter, displayRect);
    }

    if (m_showSafeGuides && m_aspectRatioGuide != AspectRatioGuide::None) {
        drawSafeFrameGuides(painter, displayRect);
    }

    if (m_showActionBar) {
        drawActionBar(painter);
    }

    drawStatusOverlay(painter);
    painter.end();

    updateFpsTelemetry();
}

void GpuPreviewViewport::paintEvent(QPaintEvent *event)
{
    if (!isOffscreenPlatform() && isValid()) {
        if (m_fallbackCanvas && m_fallbackCanvas->isVisible()) {
            m_fallbackCanvas->hide();
        }
        QOpenGLWidget::paintEvent(event);
        return;
    }

    if (!m_fallbackCanvas || width() <= 0 || height() <= 0) {
        return;
    }

    const qreal dpr = devicePixelRatioF();
    QImage frame(std::max(1, static_cast<int>(width() * dpr)),
                 std::max(1, static_cast<int>(height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(dpr);
    frame.fill(QColor(15, 16, 22));

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QRectF displayRect = computeContentDisplayRect();
    drawFallbackBackdrop(painter, displayRect);

    if (m_showSafeGuides && m_aspectRatioGuide != AspectRatioGuide::None) {
        drawSafeFrameGuides(painter, displayRect);
    }

    if (m_showActionBar) {
        drawActionBar(painter);
    }

    drawStatusOverlay(painter);
    painter.end();

    m_fallbackCanvas->setGeometry(rect());
    m_fallbackCanvas->setPixmap(QPixmap::fromImage(frame));
    if (!m_fallbackCanvas->isVisible()) {
        m_fallbackCanvas->show();
    }
    updateFpsTelemetry();
}

void GpuPreviewViewport::drawFallbackBackdrop(QPainter &painter, const QRectF &displayRect)
{
    painter.save();
    if (!m_fallbackImage.isNull()) {
        painter.drawImage(displayRect, m_fallbackImage);
        painter.restore();
        return;
    }

    // Subtle procedural studio preview canvas when no live DMA-BUF texture is bound yet
    QLinearGradient bgGrad(displayRect.topLeft(), displayRect.bottomRight());
    bgGrad.setColorAt(0.0, QColor(18, 22, 34));
    bgGrad.setColorAt(0.5, QColor(24, 28, 44));
    bgGrad.setColorAt(1.0, QColor(16, 20, 30));
    painter.fillRect(displayRect, bgGrad);

    QRadialGradient glow1(displayRect.left() + displayRect.width() * 0.72,
                          displayRect.top() + displayRect.height() * 0.34,
                          displayRect.width() * 0.45);
    glow1.setColorAt(0.0, QColor(56, 189, 248, 55));
    glow1.setColorAt(0.6, QColor(99, 102, 241, 25));
    glow1.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(displayRect, glow1);

    QRadialGradient glow2(displayRect.left() + displayRect.width() * 0.28,
                          displayRect.top() + displayRect.height() * 0.68,
                          displayRect.width() * 0.40);
    glow2.setColorAt(0.0, QColor(168, 85, 247, 45));
    glow2.setColorAt(0.7, QColor(14, 165, 233, 18));
    glow2.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(displayRect, glow2);
    painter.restore();
}

void GpuPreviewViewport::drawSafeFrameGuides(QPainter &painter, const QRectF &displayRect)
{
    float targetAspect = calculateGuideAspect();
    if (targetAspect <= 0.01f || displayRect.width() <= 2.0f || displayRect.height() <= 2.0f) {
        return;
    }

    QRectF safeRect;
    if (displayRect.width() / displayRect.height() > targetAspect) {
        float h = displayRect.height();
        float w = h * targetAspect;
        safeRect = QRectF(displayRect.center().x() - w * 0.5f, displayRect.top(), w, h);
    } else {
        float w = displayRect.width();
        float h = w / targetAspect;
        safeRect = QRectF(displayRect.left(), displayRect.center().y() - h * 0.5f, w, h);
    }

    // Outside letterbox/pillarbox dim mask
    painter.save();
    QPainterPath fullPath;
    fullPath.addRect(displayRect);
    QPainterPath safePath;
    safePath.addRect(safeRect);
    QPainterPath maskPath = fullPath.subtracted(safePath);

    painter.fillPath(maskPath, QColor(0, 0, 0, 110));

    // Safe Frame Boundary (Cyan Accent)
    QPen boundaryPen(QColor(0, 229, 255, 180), 1.5, Qt::SolidLine);
    painter.setPen(boundaryPen);
    painter.drawRect(safeRect);

    // Corner tick brackets
    constexpr float kCornerLen = 14.0f;
    QPen cornerPen(QColor(0, 229, 255, 240), 2.5, Qt::SolidLine);
    painter.setPen(cornerPen);

    // Top-Left
    painter.drawLine(safeRect.topLeft(), safeRect.topLeft() + QPointF(kCornerLen, 0));
    painter.drawLine(safeRect.topLeft(), safeRect.topLeft() + QPointF(0, kCornerLen));
    // Top-Right
    painter.drawLine(safeRect.topRight(), safeRect.topRight() - QPointF(kCornerLen, 0));
    painter.drawLine(safeRect.topRight(), safeRect.topRight() + QPointF(0, kCornerLen));
    // Bottom-Left
    painter.drawLine(safeRect.bottomLeft(), safeRect.bottomLeft() + QPointF(kCornerLen, 0));
    painter.drawLine(safeRect.bottomLeft(), safeRect.bottomLeft() - QPointF(0, kCornerLen));
    // Bottom-Right
    painter.drawLine(safeRect.bottomRight(), safeRect.bottomRight() - QPointF(kCornerLen, 0));
    painter.drawLine(safeRect.bottomRight(), safeRect.bottomRight() - QPointF(0, kCornerLen));

    // Rule of Thirds
    QPen gridPen(QColor(255, 255, 255, 50), 1.0, Qt::DashLine);
    painter.setPen(gridPen);
    float x1 = safeRect.left() + safeRect.width() / 3.0f;
    float x2 = safeRect.left() + safeRect.width() * 2.0f / 3.0f;
    float y1 = safeRect.top() + safeRect.height() / 3.0f;
    float y2 = safeRect.top() + safeRect.height() * 2.0f / 3.0f;

    painter.drawLine(QPointF(x1, safeRect.top()), QPointF(x1, safeRect.bottom()));
    painter.drawLine(QPointF(x2, safeRect.top()), QPointF(x2, safeRect.bottom()));
    painter.drawLine(QPointF(safeRect.left(), y1), QPointF(safeRect.right(), y1));
    painter.drawLine(QPointF(safeRect.left(), y2), QPointF(safeRect.right(), y2));

    // Center Crosshair
    QPointF center = safeRect.center();
    QPen crossPen(QColor(0, 229, 255, 160), 1.0, Qt::SolidLine);
    painter.setPen(crossPen);
    painter.drawLine(center - QPointF(8, 0), center + QPointF(8, 0));
    painter.drawLine(center - QPointF(0, 8), center + QPointF(0, 8));

    // Safe Frame Label Tag
    QString guideLabel;
    switch (m_aspectRatioGuide) {
    case AspectRatioGuide::Ratio16_9:
        guideLabel = QStringLiteral("SAFE FRAME: 16:9 UHD");
        break;
    case AspectRatioGuide::Ratio16_10:
        guideLabel = QStringLiteral("SAFE FRAME: 16:10");
        break;
    case AspectRatioGuide::Ratio21_9:
        guideLabel = QStringLiteral("SAFE FRAME: 21:9 ULTRAWIDE");
        break;
    case AspectRatioGuide::Ratio32_9:
        guideLabel = QStringLiteral("SAFE FRAME: 32:9 DUAL SPAN");
        break;
    case AspectRatioGuide::Ratio4_3:
        guideLabel = QStringLiteral("SAFE FRAME: 4:3");
        break;
    default:
        guideLabel = QStringLiteral("SAFE FRAME: DISPLAY MATCH");
        break;
    }

    QFont tagFont = painter.font();
    tagFont.setPixelSize(10);
    tagFont.setBold(true);
    painter.setFont(tagFont);

    QFontMetrics tagFm(tagFont);
    float tagW = std::min(static_cast<float>(safeRect.width()) - 12.0f,
                          static_cast<float>(tagFm.horizontalAdvance(guideLabel) + 16));
    if (tagW > 24.0f && safeRect.height() > 28.0f) {
        QRectF tagRect(safeRect.right() - tagW - 6.0f, safeRect.top() + 6.0f, tagW, 20.0f);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 20, 30, 200));
        painter.drawRoundedRect(tagRect, 4.0, 4.0);

        painter.setPen(QColor(0, 229, 255, 230));
        painter.drawText(
            tagRect, Qt::AlignCenter,
            tagFm.elidedText(guideLabel, Qt::ElideRight, static_cast<int>(tagW - 8.0f)));
    }

    painter.restore();
}

void GpuPreviewViewport::drawActionBar(QPainter &painter)
{
    float availW = std::max(200.0f, static_cast<float>(width()) - 20.0f);
    float barW = std::min(availW, 600.0f);
    float barH = 42.0f;
    float barX = (static_cast<float>(width()) - barW) * 0.5f;
    float barY = static_cast<float>(height()) - barH - 12.0f;
    m_actionBarRect = QRectF(barX, barY, barW, barH);

    const bool compact = (barW < 520.0f);
    const bool ultraCompact = (barW < 390.0f);

    painter.save();

    // Dark glassmorphism pill background
    QPainterPath barPath;
    barPath.addRoundedRect(m_actionBarRect, 12.0, 12.0);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1.0));
    painter.setBrush(QColor(22, 23, 28, 228));
    painter.drawPath(barPath);
    painter.setClipPath(barPath);

    float padX = 12.0f;
    float gap = ultraCompact ? 6.0f : 8.0f;
    float btnY = barY + 6.0f;
    float btnH = barH - 12.0f;

    float wStatus = ultraCompact ? 68.0f : (compact ? 90.0f : 116.0f);
    float wMon = ultraCompact ? 64.0f : (compact ? 84.0f : 104.0f);
    float wApply = ultraCompact ? 68.0f : (compact ? 94.0f : 122.0f);
    float wReset = ultraCompact ? 38.0f : 44.0f;
    float wGuide = ultraCompact ? 46.0f : (compact ? 56.0f : 64.0f);
    float wPara = ultraCompact ? 42.0f : 50.0f;

    float rawSum = wStatus + wMon + wApply + wReset + wGuide + wPara + 5.0f * gap + 12.0f;
    float innerW = std::max(160.0f, barW - 2.0f * padX);
    float scale = std::min(1.0f, innerW / rawSum);

    wStatus *= scale;
    wMon *= scale;
    wApply *= scale;
    wReset *= scale;
    wGuide *= scale;
    wPara *= scale;
    gap *= scale;

    float curX = barX + padX;

    // 1. Status Indicator & Mode Pill
    QRectF statusRect(curX, btnY, wStatus, btnH);
    QColor dotColor(120, 120, 130);
    QString statusText = ultraCompact ? QStringLiteral("IDLE") : QStringLiteral("OFFLINE");

    if (m_previewMode == PreviewMode::DmaBufLive) {
        dotColor = QColor(16, 185, 129);
        statusText = ultraCompact ? QStringLiteral("LIVE") : QStringLiteral("LIVE DMA-BUF");
    } else if (m_previewMode == PreviewMode::InProcessShader) {
        dotColor = QColor(6, 182, 212);
        statusText = QStringLiteral("SHADER");
    } else if (m_previewMode == PreviewMode::InProcessImage) {
        dotColor = QColor(245, 158, 11);
        statusText = QStringLiteral("IMAGE");
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(dotColor);
    painter.drawEllipse(QPointF(statusRect.left() + 6.0f, statusRect.center().y()), 4.0, 4.0);

    QFont statusFont = painter.font();
    statusFont.setPixelSize(ultraCompact ? 9 : 10);
    statusFont.setBold(true);
    painter.setFont(statusFont);
    QFontMetrics fm(statusFont);
    painter.setPen(QColor(230, 230, 235));
    QRectF statusTextRect(statusRect.left() + 14.0f, statusRect.top(),
                          std::max(10.0f, static_cast<float>(statusRect.width()) - 16.0f),
                          statusRect.height());
    painter.drawText(
        statusTextRect, Qt::AlignVCenter | Qt::AlignLeft,
        fm.elidedText(statusText, Qt::ElideRight, static_cast<int>(statusTextRect.width())));

    curX += wStatus + 6.0f * scale;

    // Vertical Divider
    painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
    painter.drawLine(QPointF(curX, barY + 9.0f), QPointF(curX, barY + barH - 9.0f));
    curX += 6.0f * scale;

    // 2. Monitor Selection Button
    m_monitorBtnRect = QRectF(curX, btnY, wMon, btnH);
    bool monitorHovered = (m_hoveredButton == ActionBarButton::MonitorSelector);
    painter.setPen(
        QPen(monitorHovered ? QColor(56, 189, 248, 140) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(monitorHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210));
    painter.drawRoundedRect(m_monitorBtnRect, 5.0, 5.0);

    painter.setPen(QColor(220, 225, 235));
    QString monitorText = m_selectedMonitor;
    painter.drawText(m_monitorBtnRect, Qt::AlignCenter,
                     fm.elidedText(monitorText, Qt::ElideRight, static_cast<int>(wMon - 8.0f)));
    curX += wMon + gap;

    // 3. "Apply to Selected Monitor(s)" Action Button
    m_applyBtnRect = QRectF(curX, btnY, wApply, btnH);
    bool applyHovered = (m_hoveredButton == ActionBarButton::ApplyButton);
    painter.setPen(Qt::NoPen);
    painter.setBrush(applyHovered ? QColor(3, 105, 161) : QColor(2, 132, 199));
    painter.drawRoundedRect(m_applyBtnRect, 5.0, 5.0);

    painter.setPen(Qt::white);
    QString applyText =
        compact ? QStringLiteral("Apply") : QStringLiteral("Apply to %1").arg(m_selectedMonitor);
    painter.drawText(m_applyBtnRect, Qt::AlignCenter,
                     fm.elidedText(applyText, Qt::ElideRight, static_cast<int>(wApply - 8.0f)));
    curX += wApply + gap;

    // 4. Quick Action Buttons: Reset, Guide, 2.5D Parallax
    m_resetBtnRect = QRectF(curX, btnY, wReset, btnH);
    bool resetHovered = (m_hoveredButton == ActionBarButton::ResetButton);
    painter.setPen(
        QPen(resetHovered ? QColor(255, 255, 255, 100) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(resetHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210));
    painter.drawRoundedRect(m_resetBtnRect, 5.0, 5.0);
    painter.setPen(QColor(210, 215, 225));
    painter.drawText(m_resetBtnRect, Qt::AlignCenter, QStringLiteral("1:1"));
    curX += wReset + gap;

    m_guideBtnRect = QRectF(curX, btnY, wGuide, btnH);
    bool guideHovered = (m_hoveredButton == ActionBarButton::GuideButton);
    bool guideActive = (m_showSafeGuides && m_aspectRatioGuide != AspectRatioGuide::None);
    painter.setPen(QPen(guideActive ? QColor(56, 189, 248, 180) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(guideActive
                         ? QColor(7, 89, 133, 200)
                         : (guideHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210)));
    painter.drawRoundedRect(m_guideBtnRect, 5.0, 5.0);
    painter.setPen(guideActive ? QColor(56, 189, 248) : QColor(210, 215, 225));
    painter.drawText(m_guideBtnRect, Qt::AlignCenter,
                     ultraCompact ? QStringLiteral("Grid") : QStringLiteral("Guides"));
    curX += wGuide + gap;

    m_parallaxBtnRect = QRectF(curX, btnY, wPara, btnH);
    bool parallaxHovered = (m_hoveredButton == ActionBarButton::ParallaxButton);
    painter.setPen(
        QPen(m_parallaxEnabled ? QColor(168, 85, 247, 200) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(m_parallaxEnabled
                         ? QColor(107, 33, 168, 200)
                         : (parallaxHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210)));
    painter.drawRoundedRect(m_parallaxBtnRect, 5.0, 5.0);
    painter.setPen(m_parallaxEnabled ? QColor(233, 213, 255) : QColor(210, 215, 225));
    painter.drawText(m_parallaxBtnRect, Qt::AlignCenter, QStringLiteral("2.5D"));

    painter.restore();
}

void GpuPreviewViewport::drawStatusOverlay(QPainter &painter)
{
    // Top-left telemetry readout
    painter.save();
    QFont hudFont = painter.font();
    hudFont.setPixelSize(10);
    painter.setFont(hudFont);

    QString modeStr = (m_previewMode == PreviewMode::DmaBufLive) ? QStringLiteral("LIVE DMA-BUF")
                      : (m_previewMode == PreviewMode::InProcessShader) ? QStringLiteral("SHADER")
                      : (m_previewMode == PreviewMode::InProcessImage)  ? QStringLiteral("IMAGE")
                                                                        : QStringLiteral("IDLE");

    QString telemetryStr = QStringLiteral("%1 | %2x%3 | %4% | %5 FPS")
                               .arg(modeStr)
                               .arg(contentWidth())
                               .arg(contentHeight())
                               .arg(std::round(m_zoom * 100.0f))
                               .arg(QString::number(m_currentFps, 'f', 0));

    QFontMetrics fm(hudFont);
    float maxHudW = std::max(80.0f, static_cast<float>(width()) - 20.0f);
    float hudW = std::min(maxHudW, static_cast<float>(fm.horizontalAdvance(telemetryStr) + 16));
    QRectF hudRect(10.0f, 10.0f, hudW, 22.0f);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 16, 22, 185));
    painter.drawRoundedRect(hudRect, 5.0, 5.0);

    painter.setPen(QColor(180, 185, 195));
    painter.drawText(hudRect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     fm.elidedText(telemetryStr, Qt::ElideRight, static_cast<int>(hudW - 12.0f)));

    painter.restore();
}

GpuPreviewViewport::ActionBarButton
GpuPreviewViewport::hitTestActionBar(const QPointF &pos) const noexcept
{
    if (!m_showActionBar || !m_actionBarRect.contains(pos)) {
        return ActionBarButton::None;
    }
    if (m_monitorBtnRect.contains(pos))
        return ActionBarButton::MonitorSelector;
    if (m_applyBtnRect.contains(pos))
        return ActionBarButton::ApplyButton;
    if (m_resetBtnRect.contains(pos))
        return ActionBarButton::ResetButton;
    if (m_guideBtnRect.contains(pos))
        return ActionBarButton::GuideButton;
    if (m_parallaxBtnRect.contains(pos))
        return ActionBarButton::ParallaxButton;
    return ActionBarButton::None;
}

void GpuPreviewViewport::mousePressEvent(QMouseEvent *event)
{
    if (m_showActionBar && event->button() == Qt::LeftButton) {
        ActionBarButton hit = hitTestActionBar(event->position());
        switch (hit) {
        case ActionBarButton::ApplyButton:
            onApplyClicked();
            return;
        case ActionBarButton::MonitorSelector:
            onCycleMonitors();
            return;
        case ActionBarButton::ResetButton:
            onResetViewClicked();
            return;
        case ActionBarButton::GuideButton:
            onCycleGuides();
            return;
        case ActionBarButton::ParallaxButton:
            onToggleParallax();
            return;
        case ActionBarButton::None:
            break;
        }
    }

    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))) {
        m_isPanning = true;
        m_lastMousePos = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_mouseClickCoord = event->position();

        const ViewportToolMode mode = m_gizmoOverlay.toolMode();
        if (isBrushToolMode(mode)) {
            if (!m_brushClock.isValid()) {
                m_brushClock.start();
            }
            m_brushStrokeActive = true;
            const QPointF texel = mapWidgetToContentTexel(event->position());
            const double tsSec = static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9;
            if (mode == ViewportToolMode::WeightBrush) {
                m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
                m_gizmoOverlay.setWeightBrushCursor(mapTexelToCanvasWorld(texel), true);
                beginWeightPaintStroke();
                recordWeightBrushDab(texel, 1.0f, tsSec);
                flushWeightBrushDabBatch();
                update();
            }
            emit brushPointerPressed(texel, 1.0f, tsSec);
            event->accept();
            return;
        }

        if (mode != ViewportToolMode::Navigate) {
            const uint32_t cW = contentWidth();
            const uint32_t cH = contentHeight();
            const QMatrix4x4 vp = computeGizmoViewProj(cW, cH);
            const bool preferSceneGizmo =
                (mode == ViewportToolMode::SelectBBox || mode == ViewportToolMode::Translate ||
                 mode == ViewportToolMode::Rotate || mode == ViewportToolMode::Scale);
            if (preferSceneGizmo && m_gizmoOverlay.beginDrag(event->position(), size(), vp)) {
                m_dragStartTransform = m_gizmoOverlay.dragInitialTransform();
                m_dragCurrentTransform = m_gizmoOverlay.updateDrag(
                    event->position(), size(), vp, event->modifiers().testFlag(Qt::ControlModifier),
                    event->modifiers().testFlag(Qt::ShiftModifier));
                setCursor(Qt::SizeAllCursor);
                update();
                event->accept();
                return;
            }
            if (tryBeginRiggingDrag(event->position(), event->modifiers())) {
                update();
                event->accept();
                return;
            }
        }
    }

    QOpenGLWidget::mousePressEvent(event);
}

void GpuPreviewViewport::mouseMoveEvent(QMouseEvent *event)
{
    m_mouseGlCoord =
        QPointF(event->position().x(), static_cast<float>(height()) - event->position().y());

    if (m_showActionBar) {
        ActionBarButton hit = hitTestActionBar(event->position());
        if (hit != m_hoveredButton) {
            m_hoveredButton = hit;
            if (m_hoveredButton != ActionBarButton::None) {
                setCursor(Qt::PointingHandCursor);
            } else if (!m_isPanning && !m_gizmoOverlay.isDragging()) {
                unsetCursor();
            }
            update();
        }
    }

    if (m_isPanning) {
        QPointF delta = event->position() - m_lastMousePos;
        m_lastMousePos = event->position();
        m_panOffset += delta;
        emit panChanged(m_panOffset);
        update();
        event->accept();
        return;
    }

    if (m_gizmoOverlay.toolMode() == ViewportToolMode::WeightBrush) {
        m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
        const QPointF hoverTexel = mapWidgetToContentTexel(event->position());
        m_gizmoOverlay.setWeightBrushCursor(mapTexelToCanvasWorld(hoverTexel), true);
        if (!m_brushStrokeActive) {
            update();
        }
    }

    if (m_brushStrokeActive && isBrushToolMode(m_gizmoOverlay.toolMode())) {
        const QPointF texel = mapWidgetToContentTexel(event->position());
        const double tsSec =
            m_brushClock.isValid() ? static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9 : 0.0;
        if (m_gizmoOverlay.toolMode() == ViewportToolMode::WeightBrush) {
            recordWeightBrushDab(texel, 1.0f, tsSec);
            if (m_pendingWeightDabs.size() >= 8) {
                flushWeightBrushDabBatch();
            }
            update();
        }
        emit brushPointerMoved(texel, 1.0f, tsSec);
        event->accept();
        return;
    }

    if (m_rigDragKind != RigDragKind::None) {
        updateRiggingDrag(event->position(), event->modifiers());
        update();
        event->accept();
        return;
    }

    if (m_gizmoOverlay.isDragging()) {
        const QMatrix4x4 vp = computeGizmoViewProj(contentWidth(), contentHeight());
        m_dragCurrentTransform = m_gizmoOverlay.updateDrag(
            event->position(), size(), vp, event->modifiers().testFlag(Qt::ControlModifier),
            event->modifiers().testFlag(Qt::ShiftModifier));
        if (m_activeGizmoNodeId >= 0) {
            emit layerTransformPreviewed(m_activeGizmoNodeId, m_dragCurrentTransform);
        }
        update();
        event->accept();
        return;
    }

    const ViewportToolMode mode = m_gizmoOverlay.toolMode();
    if (mode != ViewportToolMode::Navigate && !isBrushToolMode(mode)) {
        const uint32_t cW = contentWidth();
        const uint32_t cH = contentHeight();
        m_gizmoOverlay.setContentSize(cW, cH);
        const QMatrix4x4 vp = computeGizmoViewProj(cW, cH);
        const GizmoAxisHandle hovered =
            (m_activeGizmoNodeId >= 0) ? m_gizmoOverlay.pickHandle(event->position(), size(), vp)
                                       : GizmoAxisHandle::None;
        const RigPickResult rigHovered =
            m_gizmoOverlay.pickRiggingHandle(event->position(), size(), vp);
        if (m_hoveredButton == ActionBarButton::None) {
            if (hovered != GizmoAxisHandle::None || rigHovered.handle != GizmoAxisHandle::None) {
                setCursor(Qt::OpenHandCursor);
            } else {
                unsetCursor();
            }
        }
    }

    if (m_parallaxEnabled) {
        float halfW = static_cast<float>(width()) * 0.5f;
        float halfH = static_cast<float>(height()) * 0.5f;
        if (halfW > 1.0f && halfH > 1.0f) {
            m_targetTiltX = std::clamp((event->position().x() - halfW) / halfW, -1.0, 1.0);
            m_targetTiltY = std::clamp((event->position().y() - halfH) / halfH, -1.0, 1.0);
            updateAnimationTimerState();
        }
    }

    QOpenGLWidget::mouseMoveEvent(event);
}

void GpuPreviewViewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_isPanning && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_isPanning = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_brushStrokeActive) {
        m_brushStrokeActive = false;
        const QPointF texel = mapWidgetToContentTexel(event->position());
        const double tsSec =
            m_brushClock.isValid() ? static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9 : 0.0;
        if (m_gizmoOverlay.toolMode() == ViewportToolMode::WeightBrush) {
            finishWeightPaintStroke();
            update();
        }
        emit brushPointerReleased(texel, 0.0f, tsSec);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_rigDragKind != RigDragKind::None) {
        finishRiggingDrag(event->position(), event->modifiers());
        unsetCursor();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_gizmoOverlay.isDragging()) {
        const QMatrix4x4 vp = computeGizmoViewProj(contentWidth(), contentHeight());
        m_dragCurrentTransform = m_gizmoOverlay.updateDrag(
            event->position(), size(), vp, event->modifiers().testFlag(Qt::ControlModifier),
            event->modifiers().testFlag(Qt::ShiftModifier));
        m_gizmoOverlay.endDrag();
        unsetCursor();
        if (m_activeGizmoNodeId >= 0) {
            const int32_t objId = m_activeGizmoNodeId;
            emit layerTransformCommitted(objId, m_dragStartTransform, m_dragCurrentTransform);
        }
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void GpuPreviewViewport::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void GpuPreviewViewport::wheelEvent(QWheelEvent *event)
{
    float numDegrees = event->angleDelta().y() / 8.0f;
    float numSteps = numDegrees / 15.0f;
    float factor = std::pow(1.12f, numSteps);

    float oldZoom = m_zoom;
    m_zoom = std::clamp(m_zoom * factor, 0.1f, 10.0f);

    // Zoom anchored to cursor position
    QPointF mousePos = event->position();
    QPointF center =
        QPointF(static_cast<float>(width()) * 0.5f, static_cast<float>(height()) * 0.5f);
    QPointF mouseFromCenter = mousePos - center;
    m_panOffset = mouseFromCenter - (mouseFromCenter - m_panOffset) * (m_zoom / oldZoom);

    emit zoomChanged(m_zoom);
    emit panChanged(m_panOffset);
    update();
    event->accept();
}

void GpuPreviewViewport::tabletEvent(QTabletEvent *event)
{
    const ViewportToolMode mode = m_gizmoOverlay.toolMode();
    if (!isBrushToolMode(mode)) {
        QOpenGLWidget::tabletEvent(event);
        return;
    }

    const QPointF texel = mapWidgetToContentTexel(event->position());
    const float pressure = std::clamp(static_cast<float>(event->pressure()), 0.0f, 1.0f);

    switch (event->type()) {
    case QEvent::TabletPress: {
        if (!m_brushClock.isValid()) {
            m_brushClock.start();
        }
        m_brushStrokeActive = true;
        const double tsSec = static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9;
        const float effPressure = pressure > 0.0f ? pressure : 1.0f;
        if (mode == ViewportToolMode::WeightBrush) {
            m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
            m_gizmoOverlay.setWeightBrushCursor(mapTexelToCanvasWorld(texel), true);
            beginWeightPaintStroke();
            recordWeightBrushDab(texel, effPressure, tsSec);
            flushWeightBrushDabBatch();
            update();
        }
        emit brushPointerPressed(texel, effPressure, tsSec);
        event->accept();
        return;
    }
    case QEvent::TabletMove:
        if (m_brushStrokeActive) {
            const double tsSec = m_brushClock.isValid()
                                     ? static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9
                                     : 0.0;
            if (mode == ViewportToolMode::WeightBrush) {
                m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
                m_gizmoOverlay.setWeightBrushCursor(mapTexelToCanvasWorld(texel), true);
                recordWeightBrushDab(texel, pressure > 0.0f ? pressure : 1.0f, tsSec);
                if (m_pendingWeightDabs.size() >= 8) {
                    flushWeightBrushDabBatch();
                }
                update();
            }
            emit brushPointerMoved(texel, pressure, tsSec);
            event->accept();
            return;
        }
        break;
    case QEvent::TabletRelease:
        if (m_brushStrokeActive) {
            m_brushStrokeActive = false;
            const double tsSec = m_brushClock.isValid()
                                     ? static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9
                                     : 0.0;
            if (mode == ViewportToolMode::WeightBrush) {
                finishWeightPaintStroke();
                update();
            }
            emit brushPointerReleased(texel, pressure, tsSec);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }

    QOpenGLWidget::tabletEvent(event);
}

void GpuPreviewViewport::leaveEvent(QEvent *event)
{
    m_gizmoOverlay.setWeightBrushCursor(QPointF(0.0, 0.0), false);
    if (m_hoveredButton != ActionBarButton::None) {
        m_hoveredButton = ActionBarButton::None;
        unsetCursor();
    }
    if (m_parallaxEnabled) {
        m_targetTiltX = 0.0f;
        m_targetTiltY = 0.0f;
        updateAnimationTimerState();
    }
    update();
    QOpenGLWidget::leaveEvent(event);
}

void GpuPreviewViewport::showEvent(QShowEvent *event)
{
    if (isOffscreenPlatform()) {
        QWidget::showEvent(event);
    } else {
        QOpenGLWidget::showEvent(event);
    }
    updateAnimationTimerState();
}

void GpuPreviewViewport::hideEvent(QHideEvent *event)
{
    if (m_animationTimer && m_animationTimer->isActive()) {
        m_animationTimer->stop();
    }
    if (isOffscreenPlatform()) {
        QWidget::hideEvent(event);
    } else {
        QOpenGLWidget::hideEvent(event);
    }
}

void GpuPreviewViewport::onApplyClicked() { emit applyRequested(m_selectedMonitor); }

void GpuPreviewViewport::onResetViewClicked() { resetView(); }

void GpuPreviewViewport::onToggleParallax() { setParallaxEnabled(!m_parallaxEnabled); }

void GpuPreviewViewport::onCycleGuides()
{
    switch (m_aspectRatioGuide) {
    case AspectRatioGuide::None:
        setAspectRatioGuide(AspectRatioGuide::Ratio16_9);
        setShowSafeGuides(true);
        break;
    case AspectRatioGuide::Ratio16_9:
        setAspectRatioGuide(AspectRatioGuide::Ratio16_10);
        break;
    case AspectRatioGuide::Ratio16_10:
        setAspectRatioGuide(AspectRatioGuide::Ratio21_9);
        break;
    case AspectRatioGuide::Ratio21_9:
        setAspectRatioGuide(AspectRatioGuide::Ratio32_9);
        break;
    case AspectRatioGuide::Ratio32_9:
        setAspectRatioGuide(AspectRatioGuide::Ratio4_3);
        break;
    case AspectRatioGuide::Ratio4_3:
        setAspectRatioGuide(AspectRatioGuide::MatchActiveDisplay);
        break;
    case AspectRatioGuide::MatchActiveDisplay:
    default:
        setAspectRatioGuide(AspectRatioGuide::None);
        setShowSafeGuides(false);
        break;
    }
}

void GpuPreviewViewport::onCycleMonitors()
{
    if (m_availableMonitors.isEmpty()) {
        return;
    }
    int idx = m_availableMonitors.indexOf(m_selectedMonitor);
    int nextIdx = (idx + 1) % m_availableMonitors.size();
    setSelectedMonitor(m_availableMonitors.at(nextIdx));
}

void GpuPreviewViewport::setGizmoActiveNode(const SceneLayerNode *node)
{
    m_gizmoOverlay.setActiveNode(node);
    if (node) {
        m_activeGizmoNodeId = node->id;
        m_activeGizmoNodeName = node->name;
    } else {
        m_activeGizmoNodeId = -1;
        m_activeGizmoNodeName.clear();
    }
    update();
}

void GpuPreviewViewport::setGizmoCameraAndLights(const we_camera3d_t &cam,
                                                 const we_lighting_uniforms_t &lights)
{
    m_gizmoOverlay.setCameraAndLights(cam, lights);
    m_hasGizmoEmittersOrLights = m_hasGizmoEmittersOrLights || (lights.active_light_count > 0u) ||
                                 (cam.projection_mode != WE_PROJ_ORTHOGRAPHIC_2D) ||
                                 (cam.dof_enabled > 0.5f);
    update();
}

void GpuPreviewViewport::setGizmoEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters)
{
    if (!emitters.empty()) {
        m_hasGizmoEmittersOrLights = true;
    }
    m_gizmoOverlay.setEmitterWireframes(std::move(emitters));
    update();
}

void GpuPreviewViewport::setToolMode(ViewportToolMode mode)
{
    if (m_gizmoOverlay.toolMode() == mode) {
        return;
    }
    m_gizmoOverlay.setToolMode(mode);
    m_brushStrokeActive = false;
    m_rigDragKind = RigDragKind::None;
    m_rigDragIndex = -1;
    m_gizmoOverlay.setBoneCreatePreview(false, QVector3D(), QVector3D());
    emit toolModeChanged(mode);
    update();
}

void GpuPreviewViewport::setSnapConfig(const ViewportGizmoSnapConfig &cfg)
{
    m_gizmoOverlay.setSnapConfig(cfg);
    update();
}

void GpuPreviewViewport::setSceneViewProj(const QMatrix4x4 &viewProj, bool hasCustomViewProj)
{
    m_customSceneViewProj = viewProj;
    m_hasCustomSceneViewProj = hasCustomViewProj;
    update();
}

void GpuPreviewViewport::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        setToolMode(ViewportToolMode::Navigate);
        event->accept();
        return;
    case Qt::Key_Q:
        setToolMode(ViewportToolMode::SelectBBox);
        event->accept();
        return;
    case Qt::Key_W:
        setToolMode(ViewportToolMode::Translate);
        event->accept();
        return;
    case Qt::Key_E:
        setToolMode(ViewportToolMode::Rotate);
        event->accept();
        return;
    case Qt::Key_R:
        setToolMode(ViewportToolMode::Scale);
        event->accept();
        return;
    default:
        break;
    }
    QOpenGLWidget::keyPressEvent(event);
}

bool GpuPreviewViewport::isBrushToolMode(ViewportToolMode mode) noexcept
{
    return mode == ViewportToolMode::BrushMask || mode == ViewportToolMode::BrushFlow ||
           mode == ViewportToolMode::BrushBlurSmudge || mode == ViewportToolMode::WeightBrush;
}

QPointF GpuPreviewViewport::mapWidgetToContentTexel(const QPointF &widgetPos) const noexcept
{
    const uint32_t cwU = std::max<uint32_t>(1u, contentWidth());
    const uint32_t chU = std::max<uint32_t>(1u, contentHeight());
    const double cw = static_cast<double>(cwU);
    const double ch = static_cast<double>(chU);

    if (m_parallaxEnabled && width() > 0 && height() > 0) {
        const float ndcX =
            static_cast<float>(2.0 * widgetPos.x() / static_cast<double>(width()) - 1.0);
        const float ndcY =
            static_cast<float>(1.0 - 2.0 * widgetPos.y() / static_cast<double>(height()));
        bool invertible = false;
        const QMatrix4x4 invMvp = computeBlitMvp(cwU, chU).inverted(&invertible);
        if (invertible) {
            const QVector4D pNear = invMvp.map(QVector4D(ndcX, ndcY, -1.0f, 1.0f));
            const QVector4D pFar = invMvp.map(QVector4D(ndcX, ndcY, 1.0f, 1.0f));
            if (std::abs(pNear.w()) > 1e-6f && std::abs(pFar.w()) > 1e-6f) {
                const QVector3D o = pNear.toVector3DAffine();
                const QVector3D f = pFar.toVector3DAffine();
                const float dz = f.z() - o.z();
                if (std::abs(dz) > 1e-6f) {
                    const float t = -o.z() / dz;
                    const QVector3D hit = o + (f - o) * t;
                    const double u = static_cast<double>(hit.x()) * 0.5 + 0.5;
                    const double v = 0.5 - static_cast<double>(hit.y()) * 0.5;
                    return QPointF(std::clamp(u, 0.0, 1.0) * cw, std::clamp(v, 0.0, 1.0) * ch);
                }
            }
        }
    }

    const QRectF displayRect = computeContentDisplayRect();
    const double u = (widgetPos.x() - displayRect.left()) / std::max(1.0, displayRect.width());
    const double v = (widgetPos.y() - displayRect.top()) / std::max(1.0, displayRect.height());
    return QPointF(std::clamp(u, 0.0, 1.0) * cw, std::clamp(v, 0.0, 1.0) * ch);
}

QMatrix4x4 GpuPreviewViewport::computeBlitMvp(uint32_t contentW, uint32_t contentH) const noexcept
{
    const float vW = static_cast<float>(std::max(1, width()));
    const float vH = static_cast<float>(std::max(1, height()));
    const float vAspect = vW / vH;
    const float cAspect =
        static_cast<float>(std::max(1u, contentW)) / static_cast<float>(std::max(1u, contentH));

    QMatrix4x4 mvp;
    if (m_parallaxEnabled) {
        mvp.perspective(45.0f, vAspect, 0.1f, 100.0f);
        mvp.translate(0.0f, 0.0f, -2.42f);
        mvp.translate(static_cast<float>(m_panOffset.x()) / (vW * 0.5f),
                      -static_cast<float>(m_panOffset.y()) / (vH * 0.5f), 0.0f);
        mvp.scale(m_zoom, m_zoom, 1.0f);

        const float tiltAngleX = m_currentTiltX * 14.0f * m_parallaxStrength;
        const float tiltAngleY = -m_currentTiltY * 11.0f * m_parallaxStrength;
        mvp.rotate(tiltAngleX, 0.0f, 1.0f, 0.0f);
        mvp.rotate(tiltAngleY, 1.0f, 0.0f, 0.0f);

        if (cAspect >= 1.0f) {
            mvp.scale(cAspect, 1.0f, 1.0f);
        } else {
            mvp.scale(1.0f, 1.0f / cAspect, 1.0f);
        }
    } else {
        mvp.ortho(-vAspect, vAspect, -1.0f, 1.0f, -1.0f, 1.0f);
        mvp.translate(static_cast<float>(m_panOffset.x()) / (vW * 0.5f) * vAspect,
                      -static_cast<float>(m_panOffset.y()) / (vH * 0.5f), 0.0f);
        mvp.scale(m_zoom, m_zoom, 1.0f);

        if (cAspect > vAspect) {
            mvp.scale(vAspect, vAspect / cAspect, 1.0f);
        } else {
            mvp.scale(cAspect, 1.0f, 1.0f);
        }
    }
    return mvp;
}

QMatrix4x4 GpuPreviewViewport::computeGizmoViewProj(uint32_t contentW,
                                                    uint32_t contentH) const noexcept
{
    if (m_hasCustomSceneViewProj) {
        return m_customSceneViewProj;
    }
    const QMatrix4x4 blitMvp = computeBlitMvp(contentW, contentH);
    QMatrix4x4 canvasToQuad;
    canvasToQuad.translate(-1.0f, -1.0f, 0.0f);
    canvasToQuad.scale(2.0f / static_cast<float>(std::max(1u, contentW)),
                       2.0f / static_cast<float>(std::max(1u, contentH)), 1.0f);
    return blitMvp * canvasToQuad;
}

QPointF GpuPreviewViewport::mapTexelToCanvasWorld(const QPointF &texel) const noexcept
{
    const double ch = static_cast<double>(std::max<uint32_t>(1u, contentHeight()));
    return QPointF(texel.x(), ch - texel.y());
}

QPointF GpuPreviewViewport::mapTexelToPuppetLocal(const QPointF &texel) const noexcept
{
    const double cw = static_cast<double>(std::max<uint32_t>(1u, contentWidth()));
    const double ch = static_cast<double>(std::max<uint32_t>(1u, contentHeight()));
    return QPointF(texel.x() - 0.5 * cw, 0.5 * ch - texel.y());
}

void GpuPreviewViewport::ensureGizmoGlInitialized()
{
    if (m_gizmoProgram) {
        return;
    }
    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();
    const char *vsSrc = isGles ? kGizmoVertexShaderEs310 : kGizmoVertexShaderCore;
    const char *fsSrc = isGles ? kGizmoFragmentShaderEs310 : kGizmoFragmentShaderCore;

    m_gizmoProgram = std::make_unique<QOpenGLShaderProgram>(this);
    if (!m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vsSrc) ||
        !m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fsSrc) ||
        !m_gizmoProgram->link()) {
        m_gizmoProgram = std::make_unique<QOpenGLShaderProgram>(this);
        if (!m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                     kGizmoVertexShaderCore) ||
            !m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                     kGizmoFragmentShaderCore) ||
            !m_gizmoProgram->link()) {
            qWarning() << "[GpuPreviewViewport] Failed to compile/link gizmo shader:"
                       << m_gizmoProgram->log();
            m_gizmoProgram.reset();
            return;
        }
    }

    if (!m_gizmoVao.isCreated()) {
        m_gizmoVao.create();
    }
    m_gizmoVao.bind();
    if (!m_gizmoVbo.isCreated()) {
        m_gizmoVbo.create();
    }
    m_gizmoVbo.bind();
    m_gizmoVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_gizmoVboCapacityBytes = 4096u * sizeof(GizmoVertex);
    m_gizmoVbo.allocate(static_cast<int>(m_gizmoVboCapacityBytes));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                          reinterpret_cast<void *>(offsetof(GizmoVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                          reinterpret_cast<void *>(offsetof(GizmoVertex, r)));

    m_gizmoVbo.release();
    m_gizmoVao.release();
}

void GpuPreviewViewport::ensureGizmoVboCapacity(size_t requiredBytes)
{
    if (!m_gizmoVbo.isCreated() || requiredBytes <= m_gizmoVboCapacityBytes) {
        return;
    }
    const size_t maxBytes = ViewportGizmoOverlay::kMaxDynamicVertices * sizeof(GizmoVertex);
    const size_t clampedReq = std::min(requiredBytes, maxBytes);
    if (clampedReq <= m_gizmoVboCapacityBytes) {
        return;
    }
    size_t newCap = std::max<size_t>(m_gizmoVboCapacityBytes * 2u, 4096u * sizeof(GizmoVertex));
    while (newCap < clampedReq && newCap < maxBytes) {
        newCap = std::min(newCap * 2u, maxBytes);
    }
    m_gizmoVbo.bind();
    m_gizmoVbo.allocate(static_cast<int>(newCap));
    m_gizmoVboCapacityBytes = newCap;
    m_gizmoVbo.release();
}

void GpuPreviewViewport::renderGizmoOverlayPass(uint32_t contentW, uint32_t contentH)
{
    m_gizmoOverlay.setContentSize(contentW, contentH);
    if (m_gizmoOverlay.toolMode() == ViewportToolMode::Navigate && !m_hasGizmoEmittersOrLights &&
        !m_gizmoOverlay.hasVisibleRiggingOverlay()) {
        return;
    }

    const QMatrix4x4 viewProj = computeGizmoViewProj(contentW, contentH);
    const QRectF displayRect = computeContentDisplayRect();
    const float orthoZoom = std::max(0.01f, static_cast<float>(displayRect.height()) /
                                                static_cast<float>(std::max(1u, contentH)));
    const we_camera3d_t &cam = m_gizmoOverlay.camera();
    const QVector3D camEye(cam.eye_pos[0], cam.eye_pos[1], cam.eye_pos[2]);
    const float fovYDeg = (cam.fov_y_deg > 1.0f) ? cam.fov_y_deg : 60.0f;

    m_gizmoOverlay.buildHeatmapTriangleVertices(m_heatmapVertices);
    m_gizmoOverlay.buildRenderVertices(size(), viewProj, camEye, fovYDeg, orthoZoom,
                                       m_hasCustomSceneViewProj, m_gizmoVertices);
    if (m_heatmapVertices.empty() && m_gizmoVertices.empty()) {
        return;
    }

    ensureGizmoGlInitialized();
    if (!m_gizmoProgram || !m_gizmoProgram->isLinked() || !m_gizmoVao.isCreated()) {
        return;
    }

    const size_t maxVerts = ViewportGizmoOverlay::kMaxDynamicVertices;
    const size_t heatCount = std::min(m_heatmapVertices.size(), maxVerts);
    const size_t lineCount = std::min(m_gizmoVertices.size(), maxVerts);
    const size_t maxBytesNeeded = std::max(heatCount, lineCount) * sizeof(GizmoVertex);
    ensureGizmoVboCapacity(maxBytesNeeded);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_gizmoProgram->bind();
    m_gizmoProgram->setUniformValue("u_mvp", viewProj);

    m_gizmoVao.bind();
    m_gizmoVbo.bind();
    if (heatCount >= 3u) {
        m_gizmoVbo.write(0, m_heatmapVertices.data(),
                         static_cast<int>(heatCount * sizeof(GizmoVertex)));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(heatCount));
    }
    if (lineCount >= 2u) {
        m_gizmoVbo.write(0, m_gizmoVertices.data(),
                         static_cast<int>(lineCount * sizeof(GizmoVertex)));
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineCount));
    }
    m_gizmoVbo.release();
    m_gizmoVao.release();

    m_gizmoProgram->release();
    glDisable(GL_BLEND);
}

void GpuPreviewViewport::setPuppetMeshWireframe(bool visible, const we_mesh_gen_result_t &mesh)
{
    std::vector<we_mdl_vertex_t> verts;
    std::vector<uint16_t> indices;
    if (mesh.vertices && mesh.vertex_count > 0) {
        verts.assign(mesh.vertices, mesh.vertices + mesh.vertex_count);
    }
    if (mesh.indices && mesh.triangle_count > 0) {
        indices.assign(mesh.indices, mesh.indices + static_cast<size_t>(mesh.triangle_count) * 3u);
    }
    setPuppetMeshData(visible, std::move(verts), std::move(indices));
}

void GpuPreviewViewport::setPuppetMeshData(bool wireframeVisible,
                                           std::vector<we_mdl_vertex_t> vertices,
                                           std::vector<uint16_t> indices)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setMeshWireframe(wireframeVisible, std::move(vertices), std::move(indices));
    update();
}

void GpuPreviewViewport::setPuppetWireframeVisible(bool visible)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setMeshWireframeVisible(visible);
    update();
}

void GpuPreviewViewport::setPuppetContourPins(std::vector<we_cdt_pin_t> pins, int32_t selectedPin)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setMeshPins(std::move(pins), selectedPin);
    update();
}

void GpuPreviewViewport::setPuppetSkeleton(const BoneHierarchySnapshot &rig,
                                           int32_t selectedBoneIndex)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_puppetObjectId = rig.objectId;
    const size_t boneCount = rig.bones.size();
    std::vector<std::array<float, 16>> worldMats(boneCount);
    auto mulColMajor = [](const float *a, const float *b, float *out) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                out[col * 4 + row] =
                    a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                    a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
            }
        }
    };

    // Visit states: 0 = unvisited, 1 = on current ancestor path, 2 = resolved.
    std::vector<uint8_t> visitState(boneCount, 0u);
    std::vector<size_t> chainStack;
    chainStack.reserve(boneCount);
    for (size_t i = 0; i < boneCount; ++i) {
        if (visitState[i] == 2u) {
            continue;
        }
        chainStack.clear();
        size_t cur = i;
        while (cur < boneCount && visitState[cur] == 0u) {
            visitState[cur] = 1u;
            chainStack.push_back(cur);
            const uint32_t p = rig.bones[cur].parent;
            if (p == 0xFFFFFFFFu || p >= boneCount) {
                break;
            }
            cur = static_cast<size_t>(p);
        }
        while (!chainStack.empty()) {
            const size_t idx = chainStack.back();
            chainStack.pop_back();
            const auto &src = rig.bones[idx];
            if (src.parent != 0xFFFFFFFFu && src.parent < boneCount &&
                visitState[src.parent] == 2u) {
                mulColMajor(worldMats[src.parent].data(), src.local_trans, worldMats[idx].data());
            } else {
                std::memcpy(worldMats[idx].data(), src.local_trans, sizeof(src.local_trans));
            }
            visitState[idx] = 2u;
        }
    }

    std::vector<GizmoBoneNode> nodes(boneCount);
    std::vector<bool> hasChild(boneCount, false);
    for (size_t i = 0; i < boneCount; ++i) {
        const auto &src = rig.bones[i];
        auto &dst = nodes[i];
        dst.index = static_cast<uint32_t>(i);
        dst.parentIndex = src.parent;
        dst.headWorld = QVector3D(worldMats[i][12], worldMats[i][13], worldMats[i][14]);
        dst.localPos = {src.local_trans[12], src.local_trans[13], src.local_trans[14]};
        dst.localAngleRad = std::atan2(src.local_trans[1], src.local_trans[0]);
        dst.driveMode = (i < WE_MDL_MAX_BONES) ? rig.driveModes[i] : WE_BONE_DRIVE_FK;
        dst.selected = (static_cast<int32_t>(i) == selectedBoneIndex);
        if (src.parent != 0xFFFFFFFFu && src.parent < boneCount) {
            hasChild[src.parent] = true;
            nodes[src.parent].tailWorld = dst.headWorld;
        }
    }
    for (size_t i = 0; i < boneCount; ++i) {
        if (!hasChild[i]) {
            const QVector3D axisX(worldMats[i][0], worldMats[i][1], worldMats[i][2]);
            const QVector3D dir =
                (axisX.lengthSquared() > 1e-5f) ? axisX.normalized() : QVector3D(1.0f, 0.0f, 0.0f);
            nodes[i].tailWorld = nodes[i].headWorld + dir * 28.0f;
        }
    }
    m_gizmoOverlay.setPuppetBones(std::move(nodes), selectedBoneIndex);

    std::vector<GizmoIkHandleData> ikHandles;
    ikHandles.reserve(rig.ikChains.size());
    for (size_t c = 0; c < rig.ikChains.size(); ++c) {
        const auto &ch = rig.ikChains[c];
        if (ch.root_bone >= boneCount || ch.mid_bone >= boneCount || ch.end_bone >= boneCount) {
            continue;
        }
        GizmoIkHandleData handle{};
        handle.chainIndex = static_cast<int32_t>(c);
        handle.chain = ch;
        handle.rootWorld = m_gizmoOverlay.puppetBones()[ch.root_bone].headWorld;
        handle.midWorld = m_gizmoOverlay.puppetBones()[ch.mid_bone].headWorld;
        handle.endWorld = m_gizmoOverlay.puppetBones()[ch.end_bone].headWorld;
        handle.targetWorld = QVector3D(ch.target_world[0], ch.target_world[1], ch.target_world[2]);
        if (handle.targetWorld.lengthSquared() < 1e-6f) {
            handle.targetWorld = handle.endWorld;
        }
        const QVector3D poleFromChain(ch.pole_world[0], ch.pole_world[1], ch.pole_world[2]);
        if (poleFromChain.lengthSquared() > 1e-6f) {
            handle.poleWorld = poleFromChain;
        } else {
            const QVector3D baseDir = handle.endWorld - handle.rootWorld;
            const QVector3D perp = (baseDir.lengthSquared() > 1e-4f)
                                       ? QVector3D(-baseDir.y(), baseDir.x(), 0.0f).normalized()
                                       : QVector3D(0.0f, 1.0f, 0.0f);
            const float poleSign = (ch.bend_sign < 0) ? -1.0f : 1.0f;
            handle.poleWorld = handle.midWorld + perp * (36.0f * poleSign);
        }
        ikHandles.push_back(handle);
    }
    m_gizmoOverlay.setIkHandles(std::move(ikHandles));
    update();
}

void GpuPreviewViewport::setSelectedPuppetBone(int32_t boneIndex)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setSelectedBoneIndex(boneIndex);
    update();
}

void GpuPreviewViewport::setPuppetIkHandles(std::vector<GizmoIkHandleData> chains)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setIkHandles(std::move(chains));
    update();
}

void GpuPreviewViewport::setOnionSkinEnabled(bool enabled)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setOnionSkinEnabled(enabled);
    update();
}

void GpuPreviewViewport::setOnionSkinFrames(std::vector<GizmoOnionGhostFrame> frames)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setOnionSkinFrames(std::move(frames));
    update();
}

void GpuPreviewViewport::setWeightHeatmapMode(bool enabled, uint32_t activeBoneIdx, float opacity,
                                              we_heatmap_palette_t palette)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setWeightHeatmapEnabled(enabled, activeBoneIdx, opacity, palette);
    update();
}

void GpuPreviewViewport::setWeightBrushParameters(we_weight_brush_op_t op, float radiusPx,
                                                  float strength, float targetWeight,
                                                  const std::array<uint64_t, 2> &lockedBones)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_weightBrushOp = op;
    m_weightBrushRadiusPx = std::max(1.0f, radiusPx);
    m_weightBrushStrength = std::clamp(strength, 0.0f, 1.0f);
    m_weightTargetValue = std::clamp(targetWeight, 0.0f, 1.0f);
    m_lockedBoneMask = lockedBones;

    auto state = m_gizmoOverlay.weightBrushState();
    state.op = m_weightBrushOp;
    state.radiusPx = m_weightBrushRadiusPx;
    state.strength = m_weightBrushStrength;
    m_gizmoOverlay.setWeightBrushState(state);
    update();
}

void GpuPreviewViewport::beginWeightPaintStroke()
{
    m_pendingWeightDabs.clear();
    m_weightPreBlocks.clear();
    m_weightDirtyVertices.clear();
}

void GpuPreviewViewport::applyWeightBrushDab(const WeightBrushDab &dab)
{
    auto &verts = m_gizmoOverlay.mutableMeshWireframe().vertices;
    if (verts.empty() || dab.radiusPx <= 0.0f) {
        return;
    }

    auto isBoneLocked = [&](uint32_t boneIdx) -> bool {
        if (boneIdx >= 128u) {
            return false;
        }
        return (m_lockedBoneMask[boneIdx >> 6u] & (1ull << (boneIdx & 63u))) != 0ull;
    };

    if (isBoneLocked(dab.activeBoneIndex) && dab.op != WE_WEIGHT_OP_NORMALIZE) {
        return;
    }

    const float cw = static_cast<float>(std::max<uint32_t>(1u, contentWidth()));
    const float ch = static_cast<float>(std::max<uint32_t>(1u, contentHeight()));
    const float dabLocalX = dab.texelX - 0.5f * cw;
    const float dabLocalY = 0.5f * ch - dab.texelY;
    const float radiusSq = dab.radiusPx * dab.radiusPx;
    const size_t totalVerts = verts.size();

    float neighborAvg = 0.0f;
    float neighborW = 0.0f;
    if (dab.op == WE_WEIGHT_OP_SMOOTH) {
        for (size_t i = 0; i < totalVerts; ++i) {
            const float dx = verts[i].pos[0] - dabLocalX;
            const float dy = verts[i].pos[1] - dabLocalY;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= radiusSq) {
                float w = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    if (verts[i].blend_indices[k] == dab.activeBoneIndex) {
                        w += verts[i].weight[k];
                    }
                }
                const float g = 1.0f - std::sqrt(d2) / dab.radiusPx;
                neighborAvg += w * g;
                neighborW += g;
            }
        }
        if (neighborW > 1e-6f) {
            neighborAvg /= neighborW;
        }
    }

    for (size_t i = 0; i < totalVerts; ++i) {
        const float dx = verts[i].pos[0] - dabLocalX;
        const float dy = verts[i].pos[1] - dabLocalY;
        const float distSq = dx * dx + dy * dy;
        if (distSq > radiusSq) {
            continue;
        }

        const uint32_t blockIdx = static_cast<uint32_t>(i / 256u);
        if (!m_weightPreBlocks.contains(blockIdx)) {
            const size_t bStart = static_cast<size_t>(blockIdx) * 256u;
            const size_t bEnd = std::min(bStart + 256u, totalVerts);
            VertexWeightBlockSnapshot snap{};
            snap.blockIndex = blockIdx;
            snap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                              verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
            m_weightPreBlocks.emplace(blockIdx, std::move(snap));
        }

        const float dist = std::sqrt(distSq);
        const float falloff =
            0.5f * (1.0f + std::cos(static_cast<float>(M_PI) * dist / dab.radiusPx));
        const float alpha = std::clamp(dab.strength * dab.pressure * falloff, 0.0f, 1.0f);

        auto &v = verts[i];
        int targetSlot = -1;
        int weakestSlot = 0;
        float minW = v.weight[0];
        for (int k = 0; k < 4; ++k) {
            if (v.blend_indices[k] == dab.activeBoneIndex) {
                targetSlot = k;
                break;
            }
            if (v.weight[k] < minW) {
                minW = v.weight[k];
                weakestSlot = k;
            }
        }
        if (targetSlot < 0 && dab.op != WE_WEIGHT_OP_NORMALIZE && dab.op != WE_WEIGHT_OP_SUBTRACT) {
            targetSlot = weakestSlot;
            v.blend_indices[targetSlot] = dab.activeBoneIndex;
            v.weight[targetSlot] = 0.0f;
        }

        if (targetSlot >= 0) {
            const float curW = v.weight[targetSlot];
            float nextW = curW;
            switch (dab.op) {
            case WE_WEIGHT_OP_ADD:
                nextW = std::clamp(curW + alpha, 0.0f, 1.0f);
                break;
            case WE_WEIGHT_OP_SUBTRACT:
                nextW = std::clamp(curW - alpha, 0.0f, 1.0f);
                break;
            case WE_WEIGHT_OP_REPLACE:
                nextW = curW + (dab.targetWeight - curW) * alpha;
                break;
            case WE_WEIGHT_OP_SMOOTH:
                nextW = curW + (neighborAvg - curW) * alpha;
                break;
            case WE_WEIGHT_OP_NORMALIZE:
                break;
            }
            v.weight[targetSlot] = std::clamp(nextW, 0.0f, 1.0f);
        }

        float lockedSum = 0.0f;
        float freeSum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (isBoneLocked(v.blend_indices[k])) {
                lockedSum += std::max(0.0f, v.weight[k]);
            } else {
                freeSum += std::max(0.0f, v.weight[k]);
            }
        }
        const float targetFree = std::max(0.0f, 1.0f - lockedSum);
        if (freeSum > 1e-6f) {
            const float scale = targetFree / freeSum;
            for (int k = 0; k < 4; ++k) {
                if (!isBoneLocked(v.blend_indices[k])) {
                    v.weight[k] = std::max(0.0f, v.weight[k]) * scale;
                }
            }
        }

        m_weightDirtyVertices.push_back(static_cast<uint32_t>(i));
    }
}

void GpuPreviewViewport::recordWeightBrushDab(const QPointF &texel, float pressure, double tsSec)
{
    WeightBrushDab dab{};
    dab.texelX = static_cast<float>(texel.x());
    dab.texelY = static_cast<float>(texel.y());
    dab.radiusPx = m_weightBrushRadiusPx;
    dab.strength = m_weightBrushStrength;
    dab.targetWeight = m_weightTargetValue;
    dab.pressure = std::clamp(pressure, 0.0f, 1.0f);
    dab.activeBoneIndex = m_gizmoOverlay.weightBrushState().activeBoneIndex;
    dab.op = m_weightBrushOp;
    dab.timestampSec = tsSec;

    applyWeightBrushDab(dab);
    m_pendingWeightDabs.push_back(dab);
}

void GpuPreviewViewport::flushWeightBrushDabBatch()
{
    if (m_pendingWeightDabs.empty()) {
        return;
    }
    emit weightBrushDabBatchReady(m_puppetObjectId, m_pendingWeightDabs);
    m_pendingWeightDabs.clear();
}

void GpuPreviewViewport::finishWeightPaintStroke()
{
    flushWeightBrushDabBatch();
    if (m_weightPreBlocks.empty()) {
        m_weightDirtyVertices.clear();
        return;
    }

    const auto &verts = m_gizmoOverlay.meshWireframe().vertices;
    const size_t totalVerts = verts.size();

    std::vector<VertexWeightBlockSnapshot> preBlocks;
    std::vector<VertexWeightBlockSnapshot> postBlocks;
    preBlocks.reserve(m_weightPreBlocks.size());
    postBlocks.reserve(m_weightPreBlocks.size());
    for (auto &[blockIdx, preSnap] : m_weightPreBlocks) {
        const size_t bStart = static_cast<size_t>(blockIdx) * 256u;
        const size_t bEnd = std::min(bStart + 256u, totalVerts);
        if (bStart < bEnd) {
            VertexWeightBlockSnapshot postSnap{};
            postSnap.blockIndex = blockIdx;
            postSnap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                                  verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
            preBlocks.push_back(std::move(preSnap));
            postBlocks.push_back(std::move(postSnap));
        }
    }

    std::sort(m_weightDirtyVertices.begin(), m_weightDirtyVertices.end());
    m_weightDirtyVertices.erase(
        std::unique(m_weightDirtyVertices.begin(), m_weightDirtyVertices.end()),
        m_weightDirtyVertices.end());

    std::vector<waywal_ipc_vertex_weight_t> dirtyEntries;
    dirtyEntries.reserve(m_weightDirtyVertices.size());
    for (uint32_t vIdx : m_weightDirtyVertices) {
        if (vIdx >= totalVerts) {
            continue;
        }
        waywal_ipc_vertex_weight_t vw{};
        vw.vertex_index = vIdx;
        for (int k = 0; k < 4; ++k) {
            vw.blend_indices[k] = verts[vIdx].blend_indices[k];
            vw.weight[k] = verts[vIdx].weight[k];
        }
        dirtyEntries.push_back(vw);
    }

    m_weightPreBlocks.clear();
    m_weightDirtyVertices.clear();
    emit weightPaintStrokeCommitted(m_puppetObjectId, preBlocks, postBlocks, dirtyEntries);
}

void GpuPreviewViewport::applyVertexWeightBlocks(
    int32_t objectId, const std::vector<VertexWeightBlockSnapshot> &blocks)
{
    if (objectId < 0 || m_puppetObjectId != objectId || blocks.empty()) {
        return;
    }
    auto &verts = m_gizmoOverlay.mutableMeshWireframe().vertices;
    if (verts.empty()) {
        return;
    }

    bool anyApplied = false;
    for (const auto &blk : blocks) {
        const size_t base = static_cast<size_t>(blk.blockIndex) * 256u;
        if (base >= verts.size()) {
            continue;
        }
        const size_t count = std::min(blk.slice.size(), verts.size() - base);
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(verts[base + i].blend_indices, blk.slice[i].blend_indices,
                        sizeof(verts[base + i].blend_indices));
            std::memcpy(verts[base + i].weight, blk.slice[i].weight,
                        sizeof(verts[base + i].weight));
            anyApplied = true;
        }
    }
    if (anyApplied) {
        update();
    }
}

void GpuPreviewViewport::applySolvedVertexWeights(
    int32_t objectId, const std::vector<waywal_ipc_vertex_weight_t> &entries)
{
    if (objectId < 0 || m_puppetObjectId != objectId || entries.empty()) {
        return;
    }
    auto &verts = m_gizmoOverlay.mutableMeshWireframe().vertices;
    if (verts.empty()) {
        return;
    }

    bool anyApplied = false;
    for (const auto &entry : entries) {
        if (entry.vertex_index >= verts.size()) {
            continue;
        }
        auto &v = verts[entry.vertex_index];
        for (int k = 0; k < 4; ++k) {
            v.blend_indices[k] = entry.blend_indices[k];
            v.weight[k] =
                std::isfinite(entry.weight[k]) ? std::clamp(entry.weight[k], 0.0f, 1.0f) : 0.0f;
        }
        anyApplied = true;
    }
    if (anyApplied) {
        update();
    }
}

void GpuPreviewViewport::normalizeAllVertexWeights(int32_t objectId,
                                                   const std::array<uint64_t, 2> &lockedBones)
{
    if (objectId < 0 || m_puppetObjectId != objectId) {
        return;
    }
    auto &verts = m_gizmoOverlay.mutableMeshWireframe().vertices;
    if (verts.empty()) {
        return;
    }

    auto isBoneLocked = [&lockedBones](uint32_t boneIdx) -> bool {
        if (boneIdx >= 128u) {
            return false;
        }
        return (lockedBones[boneIdx >> 6u] & (1ull << (boneIdx & 63u))) != 0ull;
    };

    const size_t totalVerts = verts.size();
    std::vector<VertexWeightBlockSnapshot> preBlocks;
    std::vector<VertexWeightBlockSnapshot> postBlocks;
    std::vector<waywal_ipc_vertex_weight_t> dirtyEntries;

    for (size_t bStart = 0; bStart < totalVerts; bStart += 256u) {
        const size_t bEnd = std::min(bStart + 256u, totalVerts);
        bool blockDirty = false;
        VertexWeightBlockSnapshot preSnap{};
        preSnap.blockIndex = static_cast<uint32_t>(bStart / 256u);

        for (size_t i = bStart; i < bEnd; ++i) {
            auto &v = verts[i];
            float lockedSum = 0.0f;
            float freeSum = 0.0f;
            std::array<float, 4> cleanW{};
            for (int k = 0; k < 4; ++k) {
                const float w = std::isfinite(v.weight[k]) ? std::max(0.0f, v.weight[k]) : 0.0f;
                cleanW[k] = w;
                if (isBoneLocked(v.blend_indices[k])) {
                    lockedSum += w;
                } else {
                    freeSum += w;
                }
            }

            const float targetFree = std::max(0.0f, 1.0f - lockedSum);
            std::array<float, 4> nextW = cleanW;
            if (freeSum > 1e-6f) {
                const float scale = targetFree / freeSum;
                for (int k = 0; k < 4; ++k) {
                    if (!isBoneLocked(v.blend_indices[k])) {
                        nextW[k] = cleanW[k] * scale;
                    }
                }
            }

            bool vertChanged = false;
            for (int k = 0; k < 4; ++k) {
                if (!std::isfinite(v.weight[k]) || std::fabs(v.weight[k] - nextW[k]) > 1e-6f) {
                    vertChanged = true;
                    break;
                }
            }
            if (vertChanged) {
                if (!blockDirty) {
                    blockDirty = true;
                    preSnap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                                         verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
                }
                for (int k = 0; k < 4; ++k) {
                    v.weight[k] = nextW[k];
                }
                waywal_ipc_vertex_weight_t vw{};
                vw.vertex_index = static_cast<uint32_t>(i);
                std::memcpy(vw.blend_indices, v.blend_indices, sizeof(vw.blend_indices));
                std::memcpy(vw.weight, v.weight, sizeof(vw.weight));
                dirtyEntries.push_back(vw);
            }
        }

        if (blockDirty) {
            VertexWeightBlockSnapshot postSnap{};
            postSnap.blockIndex = preSnap.blockIndex;
            postSnap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                                  verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
            preBlocks.push_back(std::move(preSnap));
            postBlocks.push_back(std::move(postSnap));
        }
    }

    if (!dirtyEntries.empty()) {
        update();
        emit weightPaintStrokeCommitted(m_puppetObjectId, preBlocks, postBlocks, dirtyEntries);
    }
}

bool GpuPreviewViewport::tryBeginRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods)
{
    (void)mods;
    const ViewportToolMode mode = m_gizmoOverlay.toolMode();
    const uint32_t cW = contentWidth();
    const uint32_t cH = contentHeight();
    m_gizmoOverlay.setContentSize(cW, cH);
    const QMatrix4x4 vp = computeGizmoViewProj(cW, cH);
    const RigPickResult hit = m_gizmoOverlay.pickRiggingHandle(widgetPos, size(), vp);
    const QPointF texel = mapWidgetToContentTexel(widgetPos);
    const QPointF localPt = mapTexelToPuppetLocal(texel);
    const QVector3D curLocal(static_cast<float>(localPt.x()), static_cast<float>(localPt.y()),
                             0.0f);

    if (mode == ViewportToolMode::ContourPin) {
        if (hit.handle == GizmoAxisHandle::ContourPinHandle && hit.index >= 0) {
            m_gizmoOverlay.mutableMeshWireframe().selectedPinIndex = hit.index;
            m_rigDragKind = RigDragKind::ContourPinMove;
            m_rigDragIndex = hit.index;
            emit contourPinSelected(m_puppetObjectId, hit.index);
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
        we_cdt_pin_t pin{};
        pin.px = texel.x();
        pin.py = texel.y();
        pin.flags = 1u;
        auto &pins = m_gizmoOverlay.mutableMeshWireframe().pins;
        pins.push_back(pin);
        m_gizmoOverlay.mutableMeshWireframe().selectedPinIndex =
            static_cast<int32_t>(pins.size()) - 1;
        emit contourPinAdded(m_puppetObjectId, pin.px, pin.py);
        return true;
    }

    if (mode == ViewportToolMode::BonePlace) {
        if (hit.handle == GizmoAxisHandle::BonePivotRotate && hit.index >= 0 &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.puppetBones().size()) {
            const auto &bone = m_gizmoOverlay.puppetBones()[static_cast<size_t>(hit.index)];
            m_rigDragKind = RigDragKind::BonePivotRotate;
            m_rigDragIndex = hit.index;
            m_rigDragStartWorld = curLocal;
            m_rigDragInitialBoneHead = bone.headWorld;
            m_rigDragInitialBoneTail = bone.tailWorld;
            m_rigDragInitialBoneAngleRad = bone.localAngleRad;
            m_rigDragStartAngleRad =
                std::atan2(curLocal.y() - bone.headWorld.y(), curLocal.x() - bone.headWorld.x());
            setCursor(Qt::SizeAllCursor);
            return true;
        }
        if ((hit.handle == GizmoAxisHandle::BoneJoint || hit.handle == GizmoAxisHandle::BoneBody) &&
            hit.index >= 0 &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.puppetBones().size()) {
            m_gizmoOverlay.setSelectedBoneIndex(hit.index);
            emit puppetBoneSelected(hit.index);
            const auto &bone = m_gizmoOverlay.puppetBones()[static_cast<size_t>(hit.index)];
            m_rigDragKind = RigDragKind::BoneJointMoveOrReparent;
            m_rigDragIndex = hit.index;
            m_rigDragStartWorld = curLocal;
            m_rigDragInitialBoneHead = bone.headWorld;
            m_rigDragInitialBoneTail = bone.tailWorld;
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
        m_rigDragKind = RigDragKind::BoneCreate;
        m_rigDragIndex = m_gizmoOverlay.selectedBoneIndex();
        m_rigDragStartWorld = curLocal;
        m_gizmoOverlay.setBoneCreatePreview(true, m_rigDragStartWorld, m_rigDragStartWorld);
        setCursor(Qt::CrossCursor);
        return true;
    }

    if (mode == ViewportToolMode::IkDrag) {
        if (hit.handle == GizmoAxisHandle::IkTarget && hit.index >= 0) {
            m_rigDragKind = RigDragKind::IkTargetMove;
            m_rigDragIndex = hit.index;
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if (hit.handle == GizmoAxisHandle::IkPole && hit.index >= 0) {
            m_rigDragKind = RigDragKind::IkPoleMove;
            m_rigDragIndex = hit.index;
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if ((hit.handle == GizmoAxisHandle::BoneJoint || hit.handle == GizmoAxisHandle::BoneBody) &&
            hit.index >= 0) {
            m_gizmoOverlay.setSelectedBoneIndex(hit.index);
            emit puppetBoneSelected(hit.index);
            return true;
        }
        return false;
    }

    if ((mode == ViewportToolMode::Rotate || mode == ViewportToolMode::Translate) &&
        hit.index >= 0) {
        if (hit.handle == GizmoAxisHandle::BonePivotRotate &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.puppetBones().size()) {
            const auto &bone = m_gizmoOverlay.puppetBones()[static_cast<size_t>(hit.index)];
            m_rigDragKind = RigDragKind::BonePivotRotate;
            m_rigDragIndex = hit.index;
            m_rigDragStartWorld = curLocal;
            m_rigDragInitialBoneHead = bone.headWorld;
            m_rigDragInitialBoneTail = bone.tailWorld;
            m_rigDragInitialBoneAngleRad = bone.localAngleRad;
            m_rigDragStartAngleRad =
                std::atan2(curLocal.y() - bone.headWorld.y(), curLocal.x() - bone.headWorld.x());
            setCursor(Qt::SizeAllCursor);
            return true;
        }
        if (hit.handle == GizmoAxisHandle::BoneJoint &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.puppetBones().size()) {
            m_gizmoOverlay.setSelectedBoneIndex(hit.index);
            emit puppetBoneSelected(hit.index);
            const auto &bone = m_gizmoOverlay.puppetBones()[static_cast<size_t>(hit.index)];
            m_rigDragKind = RigDragKind::BoneJointMoveOrReparent;
            m_rigDragIndex = hit.index;
            m_rigDragStartWorld = curLocal;
            m_rigDragInitialBoneHead = bone.headWorld;
            m_rigDragInitialBoneTail = bone.tailWorld;
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
    }

    return false;
}

void GpuPreviewViewport::updateRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods)
{
    const QPointF texel = mapWidgetToContentTexel(widgetPos);
    const QPointF localPt = mapTexelToPuppetLocal(texel);
    const QVector3D curWorld(static_cast<float>(localPt.x()), static_cast<float>(localPt.y()),
                             0.0f);

    switch (m_rigDragKind) {
    case RigDragKind::ContourPinMove: {
        auto &pins = m_gizmoOverlay.mutableMeshWireframe().pins;
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < pins.size()) {
            pins[static_cast<size_t>(m_rigDragIndex)].px = texel.x();
            pins[static_cast<size_t>(m_rigDragIndex)].py = texel.y();
        }
        break;
    }
    case RigDragKind::BoneCreate:
        m_gizmoOverlay.setBoneCreatePreview(true, m_rigDragStartWorld, curWorld);
        break;
    case RigDragKind::BoneJointMoveOrReparent: {
        auto &bones = m_gizmoOverlay.mutablePuppetBones();
        if (!mods.testFlag(Qt::AltModifier) && m_rigDragIndex >= 0 &&
            static_cast<size_t>(m_rigDragIndex) < bones.size()) {
            const QVector3D delta = curWorld - m_rigDragStartWorld;
            const QVector3D seg = m_rigDragInitialBoneTail - m_rigDragInitialBoneHead;
            auto &b = bones[static_cast<size_t>(m_rigDragIndex)];
            b.headWorld = m_rigDragInitialBoneHead + delta;
            b.tailWorld = b.headWorld + seg;
            emit puppetBonePivotChanged(m_puppetObjectId, m_rigDragIndex,
                                        QPointF(b.headWorld.x(), b.headWorld.y()), b.localAngleRad,
                                        false);
        }
        break;
    }
    case RigDragKind::BonePivotRotate: {
        auto &bones = m_gizmoOverlay.mutablePuppetBones();
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < bones.size()) {
            auto &b = bones[static_cast<size_t>(m_rigDragIndex)];
            const float curAngle = std::atan2(curWorld.y() - m_rigDragInitialBoneHead.y(),
                                              curWorld.x() - m_rigDragInitialBoneHead.x());
            float deltaAng = curAngle - m_rigDragStartAngleRad;
            if (mods.testFlag(Qt::ControlModifier)) {
                const float stepRad =
                    qDegreesToRadians(std::max(1.0f, m_gizmoOverlay.snapConfig().angleStepDeg));
                deltaAng = std::round(deltaAng / stepRad) * stepRad;
            }
            const float c = std::cos(deltaAng);
            const float s = std::sin(deltaAng);
            const QVector3D seg = m_rigDragInitialBoneTail - m_rigDragInitialBoneHead;
            b.tailWorld = m_rigDragInitialBoneHead +
                          QVector3D(seg.x() * c - seg.y() * s, seg.x() * s + seg.y() * c, seg.z());
            b.localAngleRad = m_rigDragInitialBoneAngleRad + deltaAng;
            emit puppetBonePivotChanged(m_puppetObjectId, m_rigDragIndex,
                                        QPointF(b.headWorld.x(), b.headWorld.y()), b.localAngleRad,
                                        false);
        }
        break;
    }
    case RigDragKind::IkTargetMove: {
        auto &chains = m_gizmoOverlay.mutableIkHandles();
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < chains.size()) {
            auto &ik = chains[static_cast<size_t>(m_rigDragIndex)];
            ik.targetWorld = curWorld;
            const QVector3D toTarget = curWorld - ik.rootWorld;
            const float l1 = std::max(1.0f, (ik.midWorld - ik.rootWorld).length());
            const float l2 = std::max(1.0f, (ik.endWorld - ik.midWorld).length());
            const float d = std::clamp(toTarget.length(), 1e-3f, l1 + l2 - 1e-3f);
            const QVector3D dir = (toTarget.lengthSquared() > 1e-5f) ? toTarget.normalized()
                                                                     : QVector3D(1.0f, 0.0f, 0.0f);
            const float cosA =
                std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
            const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
            const QVector3D perp(-dir.y(), dir.x(), 0.0f);
            const float flip = (ik.chain.bend_sign < 0) ? -1.0f : 1.0f;
            ik.midWorld = ik.rootWorld + dir * (l1 * cosA) + perp * (l1 * sinA * flip);
            ik.endWorld = ik.rootWorld + dir * d;
            ik.chain.target_world[0] = curWorld.x();
            ik.chain.target_world[1] = curWorld.y();
            ik.chain.target_world[2] = curWorld.z();
            emit puppetIkTargetChanged(m_puppetObjectId, m_rigDragIndex, curWorld, false);
        }
        break;
    }
    case RigDragKind::IkPoleMove: {
        auto &chains = m_gizmoOverlay.mutableIkHandles();
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < chains.size()) {
            auto &ik = chains[static_cast<size_t>(m_rigDragIndex)];
            ik.poleWorld = curWorld;
            ik.chain.pole_world[0] = curWorld.x();
            ik.chain.pole_world[1] = curWorld.y();
            ik.chain.pole_world[2] = curWorld.z();
            const QVector3D span = ik.endWorld - ik.rootWorld;
            const QVector3D toPole = curWorld - ik.rootWorld;
            const float crossZ = span.x() * toPole.y() - span.y() * toPole.x();
            const bool poleFlip = (crossZ < 0.0f);
            ik.chain.bend_sign = poleFlip ? -1 : 1;
            emit puppetIkPoleChanged(m_puppetObjectId, m_rigDragIndex, curWorld, poleFlip, false);
        }
        break;
    }
    case RigDragKind::None:
        break;
    }
}

void GpuPreviewViewport::finishRiggingDrag(const QPointF &widgetPos, Qt::KeyboardModifiers mods)
{
    updateRiggingDrag(widgetPos, mods);
    const QPointF texel = mapWidgetToContentTexel(widgetPos);
    const QPointF localPt = mapTexelToPuppetLocal(texel);

    switch (m_rigDragKind) {
    case RigDragKind::ContourPinMove: {
        const auto &pins = m_gizmoOverlay.meshWireframe().pins;
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < pins.size()) {
            const auto &pin = pins[static_cast<size_t>(m_rigDragIndex)];
            emit contourPinMoved(m_puppetObjectId, static_cast<uint32_t>(m_rigDragIndex), pin.px,
                                 pin.py);
        }
        break;
    }
    case RigDragKind::BoneCreate: {
        m_gizmoOverlay.setBoneCreatePreview(false, QVector3D(), QVector3D());
        const QPointF headLocal(m_rigDragStartWorld.x(), m_rigDragStartWorld.y());
        const QPointF tailLocal = localPt;
        const float dx = static_cast<float>(tailLocal.x() - headLocal.x());
        const float dy = static_cast<float>(tailLocal.y() - headLocal.y());
        if (std::hypot(dx, dy) >= 4.0f) {
            const float angleZRad = std::atan2(dy, dx);
            emit puppetBoneCreated(m_puppetObjectId, m_rigDragIndex, headLocal, tailLocal,
                                   angleZRad);
        }
        break;
    }
    case RigDragKind::BoneJointMoveOrReparent: {
        const uint32_t cW = contentWidth();
        const uint32_t cH = contentHeight();
        m_gizmoOverlay.setContentSize(cW, cH);
        const QMatrix4x4 vp = computeGizmoViewProj(cW, cH);
        const RigPickResult dropHit = m_gizmoOverlay.pickRiggingHandle(widgetPos, size(), vp);
        if (mods.testFlag(Qt::AltModifier)) {
            int32_t newParent = -1;
            if ((dropHit.handle == GizmoAxisHandle::BoneJoint ||
                 dropHit.handle == GizmoAxisHandle::BoneBody) &&
                dropHit.index != m_rigDragIndex) {
                newParent = dropHit.index;
            }
            emit puppetBoneReparented(m_puppetObjectId, m_rigDragIndex, newParent);
        } else if (m_rigDragIndex >= 0 &&
                   static_cast<size_t>(m_rigDragIndex) < m_gizmoOverlay.puppetBones().size()) {
            const auto &b = m_gizmoOverlay.puppetBones()[static_cast<size_t>(m_rigDragIndex)];
            emit puppetBonePivotChanged(m_puppetObjectId, m_rigDragIndex,
                                        QPointF(b.headWorld.x(), b.headWorld.y()), b.localAngleRad,
                                        true);
        }
        break;
    }
    case RigDragKind::BonePivotRotate: {
        if (m_rigDragIndex >= 0 &&
            static_cast<size_t>(m_rigDragIndex) < m_gizmoOverlay.puppetBones().size()) {
            const auto &b = m_gizmoOverlay.puppetBones()[static_cast<size_t>(m_rigDragIndex)];
            emit puppetBonePivotChanged(m_puppetObjectId, m_rigDragIndex,
                                        QPointF(b.headWorld.x(), b.headWorld.y()), b.localAngleRad,
                                        true);
        }
        break;
    }
    case RigDragKind::IkTargetMove: {
        if (m_rigDragIndex >= 0 &&
            static_cast<size_t>(m_rigDragIndex) < m_gizmoOverlay.ikHandles().size()) {
            const auto &ik = m_gizmoOverlay.ikHandles()[static_cast<size_t>(m_rigDragIndex)];
            emit puppetIkTargetChanged(m_puppetObjectId, m_rigDragIndex, ik.targetWorld, true);
        }
        break;
    }
    case RigDragKind::IkPoleMove: {
        if (m_rigDragIndex >= 0 &&
            static_cast<size_t>(m_rigDragIndex) < m_gizmoOverlay.ikHandles().size()) {
            const auto &ik = m_gizmoOverlay.ikHandles()[static_cast<size_t>(m_rigDragIndex)];
            emit puppetIkPoleChanged(m_puppetObjectId, m_rigDragIndex, ik.poleWorld,
                                     ik.chain.bend_sign < 0, true);
        }
        break;
    }
    case RigDragKind::None:
        break;
    }

    m_rigDragKind = RigDragKind::None;
    m_rigDragIndex = -1;
}

} // namespace waywal::studio
