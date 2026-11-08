// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_preview_viewport.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>
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

extern "C" {
#include <waywal/image_loader.h>
}

namespace waywal::studio {

namespace {

struct Vertex {
    float x, y;
    float u, v;
};

// Fullscreen quad vertices with position [-1, 1] and UV [0, 1]
static constexpr Vertex kQuadVertices[6] = {
    {-1.0f, -1.0f,  0.0f, 0.0f},
    { 1.0f, -1.0f,  1.0f, 0.0f},
    { 1.0f,  1.0f,  1.0f, 1.0f},
    {-1.0f, -1.0f,  0.0f, 0.0f},
    { 1.0f,  1.0f,  1.0f, 1.0f},
    {-1.0f,  1.0f,  0.0f, 1.0f}
};

static constexpr EGLint kPlaneFdKeys[4] = {
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT
};

static constexpr EGLint kPlaneOffsetKeys[4] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT
};

static constexpr EGLint kPlanePitchKeys[4] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT
};

static constexpr EGLint kPlaneModLoKeys[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
};

static constexpr EGLint kPlaneModHiKeys[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
};

const char *kBlitVertexShader =
    "#version 300 es\n"
    "precision highp float;\n"
    "layout (location = 0) in vec2 a_pos;\n"
    "layout (location = 1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char *kBlitFragmentShader =
    "#version 300 es\n"
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

const char *kShadertoyPreamble =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec4 outFragColor;\n"
    "uniform vec3 iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform int iFrame;\n"
    "uniform vec4 iMouse;\n"
    "uniform vec4 iDate;\n\n";

const char *kShadertoyEpilogue =
    "\nvoid main() {\n"
    "    vec4 color = vec4(0.0);\n"
    "    mainImage(color, gl_FragCoord.xy);\n"
    "    // Invariant: IMAGE_PASS must always force opaque alpha\n"
    "    outFragColor = vec4(color.rgb, 1.0);\n"
    "}\n";

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

void DmaBufFrame::releaseGl(EGLDisplay dpy,
                            PFNEGLDESTROYIMAGEKHRPROC destroyImage,
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

GpuPreviewViewport::GpuPreviewViewport(QWidget *parent)
    : QOpenGLWidget(parent)
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
        if (!m_animationPaused) {
            // Apply spring damping for 2.5D parallax tilt
            constexpr float kDamping = 0.14f;
            m_currentTiltX += (m_targetTiltX - m_currentTiltX) * kDamping;
            m_currentTiltY += (m_targetTiltY - m_currentTiltY) * kDamping;
            update();
        }
    });
    m_animationTimer->start();
}

GpuPreviewViewport::~GpuPreviewViewport()
{
    if (isValid()) {
        makeCurrent();

        if (m_currentDmaBufFrame.has_value()) {
            m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR, m_eglDestroySyncKHR);
            m_currentDmaBufFrame.reset();
        }

        if (m_inProcessImageTex != 0) {
            glDeleteTextures(1, &m_inProcessImageTex);
            m_inProcessImageTex = 0;
        }

        m_quadVao.destroy();
        m_quadVbo.destroy();
        m_blitProgram.reset();
        m_shaderProgram.reset();
        m_offscreenFbo.reset();

        doneCurrent();
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
}

uint32_t GpuPreviewViewport::contentWidth() const noexcept
{
    if (m_previewMode == PreviewMode::DmaBufLive && m_currentDmaBufFrame.has_value()) {
        return m_currentDmaBufFrame->width;
    }
    if (m_previewMode == PreviewMode::InProcessImage) {
        return m_imageWidth;
    }
    if (m_previewMode == PreviewMode::InProcessShader && m_offscreenFbo) {
        return static_cast<uint32_t>(m_offscreenFbo->width());
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
    if (m_previewMode == PreviewMode::InProcessShader && m_offscreenFbo) {
        return static_cast<uint32_t>(m_offscreenFbo->height());
    }
    return static_cast<uint32_t>(height());
}

bool GpuPreviewViewport::hasLiveDmaBuf() const noexcept
{
    return m_previewMode == PreviewMode::DmaBufLive && m_currentDmaBufFrame.has_value();
}

void GpuPreviewViewport::clearLiveDmaBuf()
{
    makeCurrent();
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_currentDmaBufFrame.has_value()) {
        m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR, m_eglDestroySyncKHR);
        m_currentDmaBufFrame.reset();
    }
    if (m_inProcessImageTex != 0) {
        m_previewMode = PreviewMode::InProcessImage;
    } else if (m_shaderProgram) {
        m_previewMode = PreviewMode::InProcessShader;
    } else {
        m_previewMode = PreviewMode::OfflineEmpty;
    }
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

    auto getProc = [](const char *name) -> void* {
        void *p = reinterpret_cast<void*>(eglGetProcAddress(name));
        return p;
    };

    m_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(getProc("eglCreateImageKHR"));
    m_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(getProc("eglDestroyImageKHR"));
    m_eglCreateSyncKHR = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(getProc("eglCreateSyncKHR"));
    m_eglDestroySyncKHR = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(getProc("eglDestroySyncKHR"));
    m_eglWaitSyncKHR = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(getProc("eglWaitSyncKHR"));
    m_eglClientWaitSyncKHR = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(getProc("eglClientWaitSyncKHR"));
    m_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(getProc("glEGLImageTargetTexture2DOES"));

    m_eglInitialized = (m_eglCreateImageKHR != nullptr && m_glEGLImageTargetTexture2DOES != nullptr);
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
                          reinterpret_cast<void*>(offsetof(Vertex, x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, u)));

    m_quadVao.release();
    m_quadVbo.release();
}

void GpuPreviewViewport::initBlitShader()
{
    m_blitProgram = std::make_unique<QOpenGLShaderProgram>(this);
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kBlitVertexShader)) {
        qWarning() << "[GpuPreviewViewport] Failed to compile blit vertex shader:"
                   << m_blitProgram->log();
    }
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kBlitFragmentShader)) {
        qWarning() << "[GpuPreviewViewport] Failed to compile blit fragment shader:"
                   << m_blitProgram->log();
    }
    if (!m_blitProgram->link()) {
        qWarning() << "[GpuPreviewViewport] Failed to link blit shader program:"
                   << m_blitProgram->log();
    }
}

void GpuPreviewViewport::initializeGL()
{
    initializeOpenGLFunctions();
    resolveEglExtensions();
    setupQuadGeometry();
    initBlitShader();

    if (!m_fallbackImage.isNull() && m_inProcessImageTex == 0) {
        QImage formatted = m_fallbackImage.convertToFormat(QImage::Format_RGBA8888);
        glGenTextures(1, &m_inProcessImageTex);
        glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, formatted.constBits());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void GpuPreviewViewport::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
    if (m_previewMode == PreviewMode::InProcessShader) {
        ensureOffscreenFbo(w, h);
    }
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

bool GpuPreviewViewport::importDmaBufFrame(const waywal_dmabuf_frame_desc_t &desc,
                                          const int *plane_fds,
                                          size_t num_fds)
{
    auto cleanupIncomingFds = [plane_fds, num_fds]() {
        if (!plane_fds) return;
        for (size_t i = 0; i < num_fds; ++i) {
            if (plane_fds[i] >= 0) {
                ::close(plane_fds[i]);
            }
        }
    };

    if (desc.width == 0 || desc.height == 0 || num_fds == 0 || !plane_fds) {
        cleanupIncomingFds();
        emit dmaBufImportFailed(QStringLiteral("Invalid DMA-BUF parameters: dimensions 0 or no FDs"));
        return false;
    }

    makeCurrent();
    resolveEglExtensions();

    if (!m_eglCreateImageKHR || !m_glEGLImageTargetTexture2DOES) {
        cleanupIncomingFds();
        emit dmaBufImportFailed(QStringLiteral("Missing EGL DMA-BUF extensions (eglCreateImageKHR / glEGLImageTargetTexture2DOES)"));
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

    uint32_t planesToImport = std::min({static_cast<uint32_t>(num_fds),
                                        desc.num_planes > 0 ? desc.num_planes : 1u,
                                        4u});

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

    EGLImageKHR img = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        cleanupIncomingFds();
        emit dmaBufImportFailed(QStringLiteral("eglCreateImageKHR failed with error 0x%1")
                                    .arg(err, 0, 16));
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

    // Enforce non-blocking GPU fence synchronization
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    if (m_eglCreateSyncKHR) {
        sync = m_eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        if (sync != EGL_NO_SYNC_KHR && m_eglWaitSyncKHR) {
            // GPU-side wait only: orders GPU command stream without blocking CPU event loop
            m_eglWaitSyncKHR(m_eglDisplay, sync, 0);
        }
    }

    // Prepare new frame
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

    // Rotate buffers thread-safely: retire old frame and close old FDs
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentDmaBufFrame.has_value()) {
            m_currentDmaBufFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR, m_eglDestroySyncKHR);
        }
        m_currentDmaBufFrame = newFrame;
        m_previewMode = PreviewMode::DmaBufLive;
    }

    m_frameCount++;
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

    if (resp_hdr.opcode != WAYWAL_RESP_OK ||
        resp_hdr.payload_size < sizeof(waywal_export_dmabuf_resp_t)) {
        for (size_t i = 0; i < num_fds; ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
        }
        return false;
    }

    waywal_export_dmabuf_resp_t resp_pay;
    if (::read(socketFd, &resp_pay, sizeof(resp_pay)) != static_cast<ssize_t>(sizeof(resp_pay))) {
        for (size_t i = 0; i < num_fds; ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
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

    if (isValid()) {
        makeCurrent();
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
        doneCurrent();
    }

    m_previewMode = PreviewMode::InProcessImage;
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
            QImage raw(rgba, static_cast<int>(w), static_cast<int>(h),
                       static_cast<int>(w * 4), QImage::Format_RGBA8888);
            img = raw.copy();
            image_free(rgba);
        } else if (rgba) {
            image_free(rgba);
        }
    }

    // 4. Video wallpaper preview frame extraction via ffmpeg
    if (img.isNull() && QFileInfo::exists(targetPath) &&
        (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov")) {
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-loglevel"), QStringLiteral("error"),
                    QStringLiteral("-ss"), QStringLiteral("00:00:00.50"),
                    QStringLiteral("-i"), targetPath,
                    QStringLiteral("-frames:v"), QStringLiteral("1"),
                    QStringLiteral("-vf"), QStringLiteral("scale=1280:-1"),
                    QStringLiteral("-f"), QStringLiteral("image2pipe"),
                    QStringLiteral("-vcodec"), QStringLiteral("bmp"),
                    QStringLiteral("-")});
        if (proc.waitForFinished(2500) && proc.exitCode() == 0) {
            QByteArray data = proc.readAllStandardOutput();
            if (!data.isEmpty()) {
                img.loadFromData(data, "BMP");
            }
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
                  static_cast<int>((seed >> 8) & 0x7F) + 50,
                  static_cast<int>(seed & 0x7F) + 80);
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
    m_fallbackImage = QImage();
    if (isValid()) {
        makeCurrent();
        if (m_inProcessImageTex != 0) {
            glDeleteTextures(1, &m_inProcessImageTex);
            m_inProcessImageTex = 0;
        }
        doneCurrent();
    } else {
        m_inProcessImageTex = 0;
    }
    m_imageWidth = 0;
    m_imageHeight = 0;

    if (m_previewMode == PreviewMode::InProcessImage) {
        if (m_currentDmaBufFrame.has_value()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (m_shaderProgram || !m_shaderSource.isEmpty()) {
            m_previewMode = PreviewMode::InProcessShader;
        } else {
            m_previewMode = PreviewMode::OfflineEmpty;
        }
    }
    update();
}

bool GpuPreviewViewport::setPreviewShader(const QString &glslSource, QString *outError)
{
    if (!isValid()) {
        m_shaderSource = glslSource;
        m_shaderTimer.restart();
        m_lastFrameTimeNs = 0;
        m_shaderFrameIndex = 0;
        m_previewMode = PreviewMode::InProcessShader;
        update();
        return true;
    }

    makeCurrent();

    QString fullShaderSource;
    if (glslSource.contains(QLatin1String("mainImage"))) {
        fullShaderSource = QString::fromUtf8(kShadertoyPreamble) + glslSource +
                           QString::fromUtf8(kShadertoyEpilogue);
    } else if (glslSource.contains(QLatin1String("#version"))) {
        fullShaderSource = glslSource;
    } else {
        fullShaderSource = QString::fromUtf8(kShadertoyPreamble) + glslSource;
    }

    auto program = std::make_unique<QOpenGLShaderProgram>(this);
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, kBlitVertexShader)) {
        if (outError) {
            *outError = program->log();
        }
        return false;
    }
    if (!program->addShaderFromSourceCode(QOpenGLShader::Fragment, fullShaderSource)) {
        if (outError) {
            *outError = program->log();
        }
        return false;
    }
    if (!program->link()) {
        if (outError) {
            *outError = program->log();
        }
        return false;
    }

    m_shaderProgram = std::move(program);
    m_shaderSource = glslSource;
    m_shaderTimer.restart();
    m_lastFrameTimeNs = 0;
    m_shaderFrameIndex = 0;
    m_previewMode = PreviewMode::InProcessShader;

    ensureOffscreenFbo(width(), height());
    update();
    return true;
}

void GpuPreviewViewport::clearPreviewShader()
{
    m_shaderProgram.reset();
    m_shaderSource.clear();

    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_currentDmaBufFrame.has_value()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (m_inProcessImageTex != 0) {
            m_previewMode = PreviewMode::InProcessImage;
        } else {
            m_previewMode = PreviewMode::OfflineEmpty;
        }
    }
    update();
}

void GpuPreviewViewport::setShaderUniform(const QString &name, float value)
{
    ShaderUniformValue val;
    val.type = ShaderUniformValue::Type::Float;
    val.fVals = {value};
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

void GpuPreviewViewport::setShaderUniform(const QString &name, float v0, float v1, float v2, float v3)
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
    val.iVals = {value};
    m_customUniforms[name.toStdString()] = std::move(val);
}

void GpuPreviewViewport::clearShaderUniforms()
{
    m_customUniforms.clear();
}

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

void GpuPreviewViewport::renderInProcessShaderPass()
{
    if (!m_shaderProgram) {
        return;
    }

    ensureOffscreenFbo(width(), height());
    m_offscreenFbo->bind();

    glViewport(0, 0, m_offscreenFbo->width(), m_offscreenFbo->height());
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_shaderProgram->bind();

    // Standard Shadertoy uniforms
    float fboW = static_cast<float>(m_offscreenFbo->width());
    float fboH = static_cast<float>(m_offscreenFbo->height());
    m_shaderProgram->setUniformValue("iResolution", QVector3D(fboW, fboH, fboW / fboH));

    qint64 nowNs = m_shaderTimer.nsecsElapsed();
    float timeSec = static_cast<float>(nowNs) / 1e9f;
    float timeDeltaSec = (m_lastFrameTimeNs > 0)
                             ? static_cast<float>(nowNs - m_lastFrameTimeNs) / 1e9f
                             : (1.0f / static_cast<float>(m_targetFps));
    m_lastFrameTimeNs = nowNs;

    m_shaderProgram->setUniformValue("iTime", timeSec);
    m_shaderProgram->setUniformValue("iTimeDelta", timeDeltaSec);
    m_shaderProgram->setUniformValue("iFrame", m_shaderFrameIndex++);

    m_shaderProgram->setUniformValue("iMouse", QVector4D(
        static_cast<float>(m_mouseGlCoord.x()),
        static_cast<float>(m_mouseGlCoord.y()),
        static_cast<float>(m_mouseClickCoord.x()),
        static_cast<float>(m_mouseClickCoord.y())
    ));

    QDateTime dt = QDateTime::currentDateTime();
    float secOfDay = dt.time().msecsSinceStartOfDay() / 1000.0f;
    m_shaderProgram->setUniformValue("iDate", QVector4D(
        static_cast<float>(dt.date().year()),
        static_cast<float>(dt.date().month()),
        static_cast<float>(dt.date().day()),
        secOfDay
    ));

    // Custom inspector uniforms
    for (const auto &[name, val] : m_customUniforms) {
        const char *cName = name.c_str();
        switch (val.type) {
        case ShaderUniformValue::Type::Float:
            if (!val.fVals.empty()) m_shaderProgram->setUniformValue(cName, val.fVals[0]);
            break;
        case ShaderUniformValue::Type::Vec2:
            if (val.fVals.size() >= 2)
                m_shaderProgram->setUniformValue(cName, QVector2D(val.fVals[0], val.fVals[1]));
            break;
        case ShaderUniformValue::Type::Vec3:
            if (val.fVals.size() >= 3)
                m_shaderProgram->setUniformValue(cName, QVector3D(val.fVals[0], val.fVals[1], val.fVals[2]));
            break;
        case ShaderUniformValue::Type::Vec4:
            if (val.fVals.size() >= 4)
                m_shaderProgram->setUniformValue(cName, QVector4D(val.fVals[0], val.fVals[1], val.fVals[2], val.fVals[3]));
            break;
        case ShaderUniformValue::Type::Int:
            if (!val.iVals.empty()) m_shaderProgram->setUniformValue(cName, val.iVals[0]);
            break;
        case ShaderUniformValue::Type::Mat4:
            break;
        }
    }

    // Fullscreen identity quad for offscreen pass
    QMatrix4x4 ident;
    m_shaderProgram->setUniformValue("u_mvp", ident);

    m_quadVao.bind();
    m_quadVbo.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, u)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    m_quadVbo.release();
    m_quadVao.release();

    m_shaderProgram->release();
    m_offscreenFbo->release();
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
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFpsCalcTime).count();
    if (elapsedMs >= 250) {
        m_currentFps = (static_cast<float>(m_fpsFrameCounter) * 1000.0f) / static_cast<float>(elapsedMs);
        m_fpsFrameCounter = 0;
        m_lastFpsCalcTime = now;
        emit fpsUpdated(m_currentFps);
    }
}

void GpuPreviewViewport::paintGL()
{
    // Reset any GL state left dirty by QOpenGL2PaintEngineEx (e.g. setClipPath stencil/scissor)
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    if (m_previewMode == PreviewMode::InProcessImage &&
        m_inProcessImageTex == 0 && !m_fallbackImage.isNull()) {
        QImage formatted = m_fallbackImage.convertToFormat(QImage::Format_RGBA8888);
        glGenTextures(1, &m_inProcessImageTex);
        glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, formatted.constBits());
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
        renderInProcessShaderPass();
        if (m_offscreenFbo) {
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

        float vW = static_cast<float>(width());
        float vH = static_cast<float>(height());
        float vAspect = vW / std::max(1.0f, vH);
        float cAspect = static_cast<float>(cW) / std::max(1.0f, static_cast<float>(cH));

        QMatrix4x4 mvp;
        if (m_parallaxEnabled) {
            mvp.perspective(45.0f, vAspect, 0.1f, 100.0f);
            mvp.translate(0.0f, 0.0f, -2.42f);
            mvp.translate(m_panOffset.x() / (vW * 0.5f), -m_panOffset.y() / (vH * 0.5f), 0.0f);
            mvp.scale(m_zoom, m_zoom, 1.0f);

            // 2.5D Parallax 3D tilt
            float tiltAngleX = m_currentTiltX * 14.0f * m_parallaxStrength;
            float tiltAngleY = -m_currentTiltY * 11.0f * m_parallaxStrength;
            mvp.rotate(tiltAngleX, 0.0f, 1.0f, 0.0f);
            mvp.rotate(tiltAngleY, 1.0f, 0.0f, 0.0f);

            if (cAspect >= 1.0f) {
                mvp.scale(cAspect, 1.0f, 1.0f);
            } else {
                mvp.scale(1.0f, 1.0f / cAspect, 1.0f);
            }
        } else {
            mvp.ortho(-vAspect, vAspect, -1.0f, 1.0f, -1.0f, 1.0f);
            mvp.translate(m_panOffset.x() / (vW * 0.5f) * vAspect,
                          -m_panOffset.y() / (vH * 0.5f), 0.0f);
            mvp.scale(m_zoom, m_zoom, 1.0f);

            if (cAspect > vAspect) {
                mvp.scale(vAspect, vAspect / cAspect, 1.0f);
            } else {
                mvp.scale(cAspect, 1.0f, 1.0f);
            }
        }

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
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, u)));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(0);
        m_quadVbo.release();
        m_quadVao.release();

        glBindTexture(GL_TEXTURE_2D, 0);
        m_blitProgram->release();
    }

    // --- Interactive 2D Overlays via QPainter ---
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QRectF displayRect = computeContentDisplayRect();

    if (activeTex == 0) {
        drawFallbackBackdrop(painter, displayRect);
    } else if (m_previewMode == PreviewMode::InProcessImage &&
               !m_fallbackImage.isNull() && !m_parallaxEnabled) {
        painter.drawImage(displayRect, m_fallbackImage);
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
    if (isValid()) {
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
    case AspectRatioGuide::Ratio16_9: guideLabel = QStringLiteral("SAFE FRAME: 16:9 UHD"); break;
    case AspectRatioGuide::Ratio16_10: guideLabel = QStringLiteral("SAFE FRAME: 16:10"); break;
    case AspectRatioGuide::Ratio21_9: guideLabel = QStringLiteral("SAFE FRAME: 21:9 ULTRAWIDE"); break;
    case AspectRatioGuide::Ratio32_9: guideLabel = QStringLiteral("SAFE FRAME: 32:9 DUAL SPAN"); break;
    case AspectRatioGuide::Ratio4_3: guideLabel = QStringLiteral("SAFE FRAME: 4:3"); break;
    default: guideLabel = QStringLiteral("SAFE FRAME: DISPLAY MATCH"); break;
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
        painter.drawText(tagRect, Qt::AlignCenter,
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

    float padX = 10.0f;
    float gap = ultraCompact ? 4.0f : 6.0f;
    float btnY = barY + 6.0f;
    float btnH = barH - 12.0f;

    float wStatus = ultraCompact ? 68.0f : (compact ? 92.0f : 124.0f);
    float wMon    = ultraCompact ? 62.0f : (compact ? 82.0f : 102.0f);
    float wApply  = ultraCompact ? 68.0f : (compact ? 96.0f : 126.0f);
    float wReset  = ultraCompact ? 34.0f : 42.0f;
    float wGuide  = ultraCompact ? 42.0f : (compact ? 52.0f : 62.0f);
    float wPara   = ultraCompact ? 38.0f : 46.0f;

    float rawSum = wStatus + wMon + wApply + wReset + wGuide + wPara + 5.0f * gap + 8.0f;
    float innerW = std::max(160.0f, barW - 2.0f * padX);
    float scale = std::min(1.0f, innerW / rawSum);

    wStatus *= scale;
    wMon    *= scale;
    wApply  *= scale;
    wReset  *= scale;
    wGuide  *= scale;
    wPara   *= scale;
    gap     *= scale;

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
        statusText = ultraCompact ? QStringLiteral("SHADER") : QStringLiteral("SHADER");
    } else if (m_previewMode == PreviewMode::InProcessImage) {
        dotColor = QColor(245, 158, 11);
        statusText = ultraCompact ? QStringLiteral("IMAGE") : QStringLiteral("IMAGE");
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
                          std::max(10.0f, static_cast<float>(statusRect.width()) - 16.0f), statusRect.height());
    painter.drawText(statusTextRect, Qt::AlignVCenter | Qt::AlignLeft,
                     fm.elidedText(statusText, Qt::ElideRight, static_cast<int>(statusTextRect.width())));

    curX += wStatus + 4.0f * scale;

    // Vertical Divider
    painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
    painter.drawLine(QPointF(curX, barY + 9.0f), QPointF(curX, barY + barH - 9.0f));
    curX += 4.0f * scale;

    // 2. Monitor Selection Button
    m_monitorBtnRect = QRectF(curX, btnY, wMon, btnH);
    bool monitorHovered = (m_hoveredButton == ActionBarButton::MonitorSelector);
    painter.setPen(QPen(monitorHovered ? QColor(0, 229, 255, 120) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(monitorHovered ? QColor(40, 44, 56, 230) : QColor(32, 34, 42, 200));
    painter.drawRoundedRect(m_monitorBtnRect, 5.0, 5.0);

    painter.setPen(QColor(220, 225, 235));
    QString monitorText = ultraCompact
                              ? QStringLiteral("%1 ▾").arg(m_selectedMonitor)
                              : QStringLiteral("🖥 %1 ▾").arg(m_selectedMonitor);
    painter.drawText(m_monitorBtnRect, Qt::AlignCenter,
                     fm.elidedText(monitorText, Qt::ElideRight, static_cast<int>(wMon - 6.0f)));
    curX += wMon + gap;

    // 3. "Apply to Selected Monitor(s)" Action Button
    m_applyBtnRect = QRectF(curX, btnY, wApply, btnH);
    bool applyHovered = (m_hoveredButton == ActionBarButton::ApplyButton);
    painter.setPen(Qt::NoPen);
    painter.setBrush(applyHovered ? QColor(59, 130, 246) : QColor(37, 99, 235));
    painter.drawRoundedRect(m_applyBtnRect, 5.0, 5.0);

    painter.setPen(Qt::white);
    QString applyText = compact
                            ? QStringLiteral("⚡ Apply")
                            : QStringLiteral("⚡ Apply to %1").arg(m_selectedMonitor);
    painter.drawText(m_applyBtnRect, Qt::AlignCenter,
                     fm.elidedText(applyText, Qt::ElideRight, static_cast<int>(wApply - 6.0f)));
    curX += wApply + gap;

    // 4. Quick Action Buttons: Reset, Guide, 2.5D Parallax
    m_resetBtnRect = QRectF(curX, btnY, wReset, btnH);
    bool resetHovered = (m_hoveredButton == ActionBarButton::ResetButton);
    painter.setPen(QPen(resetHovered ? QColor(255, 255, 255, 100) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(resetHovered ? QColor(50, 52, 64, 230) : QColor(32, 34, 42, 200));
    painter.drawRoundedRect(m_resetBtnRect, 5.0, 5.0);
    painter.setPen(QColor(210, 215, 225));
    painter.drawText(m_resetBtnRect, Qt::AlignCenter, QStringLiteral("1:1"));
    curX += wReset + gap;

    m_guideBtnRect = QRectF(curX, btnY, wGuide, btnH);
    bool guideHovered = (m_hoveredButton == ActionBarButton::GuideButton);
    bool guideActive = (m_showSafeGuides && m_aspectRatioGuide != AspectRatioGuide::None);
    painter.setPen(QPen(guideActive ? QColor(0, 229, 255, 180) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(guideActive ? QColor(0, 100, 120, 180) :
                     (guideHovered ? QColor(50, 52, 64, 230) : QColor(32, 34, 42, 200)));
    painter.drawRoundedRect(m_guideBtnRect, 5.0, 5.0);
    painter.setPen(guideActive ? QColor(0, 229, 255) : QColor(210, 215, 225));
    painter.drawText(m_guideBtnRect, Qt::AlignCenter,
                     ultraCompact ? QStringLiteral("Grid") : QStringLiteral("Guides"));
    curX += wGuide + gap;

    m_parallaxBtnRect = QRectF(curX, btnY, wPara, btnH);
    bool parallaxHovered = (m_hoveredButton == ActionBarButton::ParallaxButton);
    painter.setPen(QPen(m_parallaxEnabled ? QColor(168, 85, 247, 200) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(m_parallaxEnabled ? QColor(107, 33, 168, 200) :
                     (parallaxHovered ? QColor(50, 52, 64, 230) : QColor(32, 34, 42, 200)));
    painter.drawRoundedRect(m_parallaxBtnRect, 5.0, 5.0);
    painter.setPen(m_parallaxEnabled ? QColor(233, 213, 255) : QColor(210, 215, 225));
    painter.drawText(m_parallaxBtnRect, Qt::AlignCenter, QStringLiteral("2.5D"));

    painter.restore();
}

void GpuPreviewViewport::drawStatusOverlay(QPainter &painter)
{
    // Sleek top-left telemetry readout
    painter.save();
    QFont hudFont = painter.font();
    hudFont.setPixelSize(10);
    painter.setFont(hudFont);

    QString modeStr = (m_previewMode == PreviewMode::DmaBufLive) ? QStringLiteral("LIVE DMA-BUF")
                      : (m_previewMode == PreviewMode::InProcessShader) ? QStringLiteral("SHADER")
                      : (m_previewMode == PreviewMode::InProcessImage) ? QStringLiteral("IMAGE")
                      : QStringLiteral("IDLE");

    QString telemetryStr = QStringLiteral("%1 · %2x%3 · %4% · %5 FPS")
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

GpuPreviewViewport::ActionBarButton GpuPreviewViewport::hitTestActionBar(const QPointF &pos) const noexcept
{
    if (!m_showActionBar || !m_actionBarRect.contains(pos)) {
        return ActionBarButton::None;
    }
    if (m_monitorBtnRect.contains(pos)) return ActionBarButton::MonitorSelector;
    if (m_applyBtnRect.contains(pos)) return ActionBarButton::ApplyButton;
    if (m_resetBtnRect.contains(pos)) return ActionBarButton::ResetButton;
    if (m_guideBtnRect.contains(pos)) return ActionBarButton::GuideButton;
    if (m_parallaxBtnRect.contains(pos)) return ActionBarButton::ParallaxButton;
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
    }

    QOpenGLWidget::mousePressEvent(event);
}

void GpuPreviewViewport::mouseMoveEvent(QMouseEvent *event)
{
    m_mouseGlCoord = QPointF(event->position().x(),
                             static_cast<float>(height()) - event->position().y());

    if (m_showActionBar) {
        ActionBarButton hit = hitTestActionBar(event->position());
        if (hit != m_hoveredButton) {
            m_hoveredButton = hit;
            if (m_hoveredButton != ActionBarButton::None) {
                setCursor(Qt::PointingHandCursor);
            } else if (!m_isPanning) {
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

    if (m_parallaxEnabled) {
        float halfW = static_cast<float>(width()) * 0.5f;
        float halfH = static_cast<float>(height()) * 0.5f;
        if (halfW > 1.0f && halfH > 1.0f) {
            m_targetTiltX = std::clamp((event->position().x() - halfW) / halfW, -1.0, 1.0);
            m_targetTiltY = std::clamp((event->position().y() - halfH) / halfH, -1.0, 1.0);
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
    QPointF center = QPointF(static_cast<float>(width()) * 0.5f, static_cast<float>(height()) * 0.5f);
    QPointF mouseFromCenter = mousePos - center;
    m_panOffset = mouseFromCenter - (mouseFromCenter - m_panOffset) * (m_zoom / oldZoom);

    emit zoomChanged(m_zoom);
    emit panChanged(m_panOffset);
    update();
    event->accept();
}

void GpuPreviewViewport::leaveEvent(QEvent *event)
{
    if (m_hoveredButton != ActionBarButton::None) {
        m_hoveredButton = ActionBarButton::None;
        unsetCursor();
    }
    if (m_parallaxEnabled) {
        m_targetTiltX = 0.0f;
        m_targetTiltY = 0.0f;
    }
    update();
    QOpenGLWidget::leaveEvent(event);
}

void GpuPreviewViewport::onApplyClicked()
{
    emit applyRequested(m_selectedMonitor);
}

void GpuPreviewViewport::onResetViewClicked()
{
    resetView();
}

void GpuPreviewViewport::onToggleParallax()
{
    setParallaxEnabled(!m_parallaxEnabled);
}

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

} // namespace waywal::studio
