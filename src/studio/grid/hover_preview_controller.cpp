// SPDX-License-Identifier: GPL-3.0-or-later
#include "hover_preview_controller.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <QCryptographicHash>
#include <QOpenGLContext>

extern "C" {
#include <waywal/image_loader.h>
}

namespace wwal::studio {

namespace {

QImage loadPreviewFileRobust(const std::string &path)
{
    if (path.empty())
        return {};
    QImage img(QString::fromStdString(path));
    if (!img.isNull())
        return img;

    uint8_t *rgba = nullptr;
    uint32_t w = 0, h = 0;
    if (image_load_rgba(path.c_str(), &rgba, &w, &h) && rgba && w > 0 && h > 0) {
        QImage raw(rgba, static_cast<int>(w), static_cast<int>(h), static_cast<int>(w * 4),
                   QImage::Format_RGBA8888);
        QImage copy = raw.copy();
        image_free(rgba);
        return copy;
    }
    if (rgba)
        image_free(rgba);

    // Fallback to disk thumbnail cache (~/.cache/wwal/thumbnails/<sha256>.webp) for video/XML items
    const char *xdg_cache = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path cache_dir;
    if (xdg_cache && xdg_cache[0] != '\0') {
        cache_dir = std::filesystem::path(xdg_cache) / "wwal" / "thumbnails";
    } else {
        const char *home = std::getenv("HOME");
        cache_dir = std::filesystem::path(home ? home : "/tmp") / ".cache" / "wwal" / "thumbnails";
    }
    QByteArray hash = QCryptographicHash::hash(
        QByteArray::fromRawData(path.data(), static_cast<qsizetype>(path.size())),
        QCryptographicHash::Sha256);
    std::filesystem::path cached_file = cache_dir / (hash.toHex().toStdString() + ".webp");
    std::error_code ec;
    if (std::filesystem::exists(cached_file, ec)) {
        QImage cached_img(QString::fromStdString(cached_file.string()));
        if (!cached_img.isNull())
            return cached_img;
    }
    return {};
}

const char *DEFAULT_VERTEX_SHADER_ES = R"(#version 300 es
precision highp float;
layout (location = 0) in vec2 a_pos;
out vec2 v_uv;

void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

const char *DEFAULT_VERTEX_SHADER_CORE = R"(#version 330 core
layout (location = 0) in vec2 a_pos;
out vec2 v_uv;

void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

const char *FALLBACK_PREVIEW_SHADER_ES = R"(#version 300 es
precision highp float;
in vec2 v_uv;
out vec4 fragColor;

uniform float iTime;
uniform vec3 iResolution;

void main() {
    vec2 p = (2.0 * gl_FragCoord.xy - iResolution.xy) / min(iResolution.x, iResolution.y);
    float d = length(p);
    float a = atan(p.y, p.x);
    float v = sin(10.0 * d - 3.0 * iTime + sin(3.0 * a + iTime));
    vec3 col = 0.5 + 0.5 * cos(iTime + a + vec3(0.0, 2.0, 4.0));
    col *= (0.7 + 0.3 * v);
    fragColor = vec4(col, 1.0);
}
)";

const char *FALLBACK_PREVIEW_SHADER_CORE = R"(#version 330 core
in vec2 v_uv;
out vec4 fragColor;

uniform float iTime;
uniform vec3 iResolution;

void main() {
    vec2 p = (2.0 * gl_FragCoord.xy - iResolution.xy) / min(iResolution.x, iResolution.y);
    float d = length(p);
    float a = atan(p.y, p.x);
    float v = sin(10.0 * d - 3.0 * iTime + sin(3.0 * a + iTime));
    vec3 col = 0.5 + 0.5 * cos(iTime + a + vec3(0.0, 2.0, 4.0));
    col *= (0.7 + 0.3 * v);
    fragColor = vec4(col, 1.0);
}
)";

class GlOwnerScope {
  public:
    explicit GlOwnerScope(QOpenGLWidget *owner) noexcept
    {
        if (!owner || !owner->isValid() || !owner->context()) {
            return;
        }
        if (QOpenGLContext::currentContext() == owner->context()) {
            m_ready = true;
            return;
        }
        owner->makeCurrent();
        m_owner = owner;
        m_ready = (QOpenGLContext::currentContext() == owner->context());
    }

    ~GlOwnerScope()
    {
        if (m_owner) {
            m_owner->doneCurrent();
        }
    }

    GlOwnerScope(const GlOwnerScope &) = delete;
    GlOwnerScope &operator=(const GlOwnerScope &) = delete;

    [[nodiscard]] bool isReady() const noexcept { return m_ready; }

  private:
    QOpenGLWidget *m_owner{nullptr};
    bool m_ready{false};
};

QImage computeVideoZoomFrame(const QImage &baseImg, size_t frameIdx, int outW, int outH)
{
    if (baseImg.isNull()) {
        return {};
    }
    const float zoom = 1.0f + 0.08f * std::sin(static_cast<float>(frameIdx % 30) * 0.2f);
    const int cropW = std::max(1, static_cast<int>(static_cast<float>(baseImg.width()) / zoom));
    const int cropH = std::max(1, static_cast<int>(static_cast<float>(baseImg.height()) / zoom));
    const int cropX = (baseImg.width() - cropW) / 2;
    const int cropY = (baseImg.height() - cropH) / 2;
    QImage frame = baseImg.copy(cropX, cropY, cropW, cropH)
                       .scaled(outW, outH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (frame.width() > outW || frame.height() > outH) {
        frame = frame.copy((frame.width() - outW) / 2, (frame.height() - outH) / 2, outW, outH);
    }
    return frame.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace

HoverPreviewController::HoverPreviewController(QObject *parent)
    : QObject(parent), m_dwellTimer(new QTimer(this)), m_playbackTimer(new QTimer(this))
{

    m_dwellTimer->setSingleShot(true);
    connect(m_dwellTimer, &QTimer::timeout, this, &HoverPreviewController::onDwellTimeout);

    m_playbackTimer->setInterval(FRAME_INTERVAL_MS);
    connect(m_playbackTimer, &QTimer::timeout, this, &HoverPreviewController::onPlaybackTick);
}

HoverPreviewController::~HoverPreviewController() { stop(); }

void HoverPreviewController::initGl(QOpenGLExtraFunctions *gl)
{
    if (!gl)
        return;
    m_gl = gl;

    if (m_previewTexId == 0) {
        m_gl->glGenTextures(1, &m_previewTexId);
        m_gl->glBindTexture(GL_TEXTURE_2D, m_previewTexId);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PREVIEW_WIDTH, PREVIEW_HEIGHT, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, nullptr);
        m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (m_shaderFbo == 0) {
        m_gl->glGenFramebuffers(1, &m_shaderFbo);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFbo);
        m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     m_previewTexId, 0);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (m_quadVao == 0) {
        static constexpr float kQuadVerts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        m_gl->glGenVertexArrays(1, &m_quadVao);
        m_gl->glGenBuffers(1, &m_quadVbo);
        m_gl->glBindVertexArray(m_quadVao);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
        m_gl->glEnableVertexAttribArray(0);
        m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_gl->glBindVertexArray(0);
    }
}

void HoverPreviewController::destroyGl(QOpenGLExtraFunctions *gl)
{
    stop();

    if (gl) {
        if (m_shaderProgram != 0) {
            gl->glDeleteProgram(m_shaderProgram);
            m_shaderProgram = 0;
        }
        if (m_quadVbo != 0) {
            gl->glDeleteBuffers(1, &m_quadVbo);
            m_quadVbo = 0;
        }
        if (m_quadVao != 0) {
            gl->glDeleteVertexArrays(1, &m_quadVao);
            m_quadVao = 0;
        }
        if (m_shaderFbo != 0) {
            gl->glDeleteFramebuffers(1, &m_shaderFbo);
            m_shaderFbo = 0;
        }
        if (m_previewTexId != 0) {
            gl->glDeleteTextures(1, &m_previewTexId);
            m_previewTexId = 0;
        }
    }
    m_gl = nullptr;
}

void HoverPreviewController::onCellHoverEntered(uint32_t item_id, const std::string &path,
                                                WallpaperBadge badge,
                                                const std::string &preview_path)
{
    if (m_previewActive && m_activeItemId == item_id) {
        return; // Already playing this preview
    }

    // Cancel any previous dwell timer
    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }

    m_pendingItemId = item_id;
    m_hasPendingItem = true;
    m_pendingPath = path;
    m_pendingBadge = badge;
    m_pendingPreviewPath = preview_path;

    // Start 250 ms dwell countdown
    m_dwellTimer->start(DWELL_TIMEOUT_MS);
}

void HoverPreviewController::onCellHoverExited(uint32_t item_id)
{
    if (m_hasPendingItem && m_pendingItemId == item_id) {
        if (m_dwellTimer && m_dwellTimer->isActive()) {
            m_dwellTimer->stop();
        }
        m_hasPendingItem = false;
    }

    if (m_previewActive && m_activeItemId == item_id) {
        cleanupActivePreview();
    }
}

void HoverPreviewController::stop()
{
    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }
    m_hasPendingItem = false;
    cleanupActivePreview();
}

void HoverPreviewController::onDwellTimeout()
{
    if (!m_hasPendingItem)
        return;
    startActivePreview();
}

void HoverPreviewController::startActivePreview()
{
    cleanupActivePreview();

    m_activeItemId = m_pendingItemId;
    m_activePath = m_pendingPath;
    m_activeBadge = m_pendingBadge;
    m_previewActive = true;
    m_previewStartTime = std::chrono::steady_clock::now();
    m_currentFrameIndex = 0;

    emit previewStarted(m_activeItemId);

    std::string preview_file = !m_pendingPreviewPath.empty() ? m_pendingPreviewPath : m_activePath;

    if (m_activeBadge == WallpaperBadge::Shader) {
        {
            GlOwnerScope glScope(m_glOwner);
            if (m_gl && glScope.isReady()) {
                setupOffscreenShader(m_activePath);
            }
        }
        m_shaderTime = 0.0f;
        m_shaderFrame = 0;
        renderShaderFrame();
        emit previewFrameReady(m_activeItemId, m_previewTexId);
        m_playbackTimer->start(FRAME_INTERVAL_MS);
    } else if (m_activeBadge == WallpaperBadge::Video) {
        QImage initial_img = loadPreviewFileRobust(preview_file);
        if (!initial_img.isNull()) {
            // Downscale large sources once so per-tick Ken Burns crop stays sub-millisecond
            if (initial_img.width() > static_cast<int>(PREVIEW_WIDTH * 2) ||
                initial_img.height() > static_cast<int>(PREVIEW_HEIGHT * 2)) {
                initial_img =
                    initial_img.scaled(PREVIEW_WIDTH * 2, PREVIEW_HEIGHT * 2,
                                       Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            }
            m_videoBaseImage = initial_img.convertToFormat(QImage::Format_RGBA8888);
            QImage firstFrame = computeVideoZoomFrame(m_videoBaseImage, 0,
                                                      static_cast<int>(PREVIEW_WIDTH),
                                                      static_cast<int>(PREVIEW_HEIGHT));
            if (!firstFrame.isNull()) {
                updateTextureWithImage(firstFrame);
                m_currentFrameIndex = 1;
                emit previewFrameReady(m_activeItemId, m_previewTexId);
                m_playbackTimer->start(FRAME_INTERVAL_MS);
            }
        }
    } else {
        // Check for animated GIF or animated WebP
        if (preview_file.ends_with(".gif") || preview_file.ends_with(".webp")) {
            m_movie = std::make_unique<QMovie>(QString::fromStdString(preview_file));
            if (m_movie->isValid() && m_movie->frameCount() > 1) {
                connect(m_movie.get(), &QMovie::frameChanged, this, [this]() {
                    if (m_movie && m_previewActive) {
                        updateTextureWithImage(m_movie->currentImage());
                        emit previewFrameReady(m_activeItemId, m_previewTexId);
                    }
                });
                m_movie->start();
                return;
            }
            m_movie.reset();
        }

        // Static or Scene preview: load image with aspect-preserving crop
        QImage img = loadPreviewFileRobust(preview_file);
        if (!img.isNull()) {
            QImage scaled = img.scaled(PREVIEW_WIDTH, PREVIEW_HEIGHT,
                                       Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            if (scaled.width() > static_cast<int>(PREVIEW_WIDTH) ||
                scaled.height() > static_cast<int>(PREVIEW_HEIGHT)) {
                scaled = scaled.copy((scaled.width() - static_cast<int>(PREVIEW_WIDTH)) / 2,
                                     (scaled.height() - static_cast<int>(PREVIEW_HEIGHT)) / 2,
                                     PREVIEW_WIDTH, PREVIEW_HEIGHT);
            }
            QImage rgba = scaled.convertToFormat(QImage::Format_RGBA8888);
            m_preloadedFrames.push_back(rgba);
            updateTextureWithImage(m_preloadedFrames[0]);
            emit previewFrameReady(m_activeItemId, m_previewTexId);
        }
    }
}

void HoverPreviewController::updateTextureWithImage(const QImage &img)
{
    if (img.isNull())
        return;

    QImage rgba;
    if (img.width() == static_cast<int>(PREVIEW_WIDTH) &&
        img.height() == static_cast<int>(PREVIEW_HEIGHT) &&
        img.format() == QImage::Format_RGBA8888) {
        rgba = img;
    } else {
        QImage scaled = img.scaled(PREVIEW_WIDTH, PREVIEW_HEIGHT, Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
        if (scaled.width() > static_cast<int>(PREVIEW_WIDTH) ||
            scaled.height() > static_cast<int>(PREVIEW_HEIGHT)) {
            scaled = scaled.copy((scaled.width() - static_cast<int>(PREVIEW_WIDTH)) / 2,
                                 (scaled.height() - static_cast<int>(PREVIEW_HEIGHT)) / 2,
                                 PREVIEW_WIDTH, PREVIEW_HEIGHT);
        }
        rgba = scaled.convertToFormat(QImage::Format_RGBA8888);
    }
    m_currentPreviewImage = rgba;

    GlOwnerScope glScope(m_glOwner);
    if (!m_gl || m_previewTexId == 0 || !glScope.isReady())
        return;

    m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    m_gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    m_gl->glBindTexture(GL_TEXTURE_2D, m_previewTexId);
    m_gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT, GL_RGBA,
                          GL_UNSIGNED_BYTE, rgba.constBits());
    m_gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void HoverPreviewController::setupOffscreenShader(const std::string &shader_path)
{
    if (!m_gl)
        return;

    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();
    const char *vsSrc = isGles ? DEFAULT_VERTEX_SHADER_ES : DEFAULT_VERTEX_SHADER_CORE;
    const char *fallbackFsSrc = isGles ? FALLBACK_PREVIEW_SHADER_ES : FALLBACK_PREVIEW_SHADER_CORE;

    std::string fragment_code;
    std::ifstream file(shader_path);
    if (file.is_open()) {
        std::stringstream ss;
        ss << file.rdbuf();
        fragment_code = ss.str();
    }

    if (fragment_code.empty() || fragment_code.find("mainImage") == std::string::npos) {
        fragment_code = fallbackFsSrc;
    } else {
        std::string wrapper = isGles ? "#version 300 es\nprecision highp float;\n"
                                     : "#version 330 core\n";
        wrapper += R"(in vec2 v_uv;
out vec4 fragColor;

uniform float iTime;
uniform vec3 iResolution;
uniform int iFrame;

)";
        wrapper += fragment_code;
        wrapper += R"(
void main() {
    vec4 col = vec4(0.0);
    mainImage(col, gl_FragCoord.xy);
    fragColor = vec4(col.rgb, 1.0);
}
)";
        fragment_code = wrapper;
    }

    GLuint vs = m_gl->glCreateShader(GL_VERTEX_SHADER);
    m_gl->glShaderSource(vs, 1, &vsSrc, nullptr);
    m_gl->glCompileShader(vs);

    const char *fs_src = fragment_code.c_str();
    GLuint fs = m_gl->glCreateShader(GL_FRAGMENT_SHADER);
    m_gl->glShaderSource(fs, 1, &fs_src, nullptr);
    m_gl->glCompileShader(fs);

    GLint compiled = 0;
    m_gl->glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        m_gl->glDeleteShader(fs);
        fs = m_gl->glCreateShader(GL_FRAGMENT_SHADER);
        m_gl->glShaderSource(fs, 1, &fallbackFsSrc, nullptr);
        m_gl->glCompileShader(fs);
    }

    if (m_shaderProgram != 0) {
        m_gl->glDeleteProgram(m_shaderProgram);
    }

    m_shaderProgram = m_gl->glCreateProgram();
    m_gl->glAttachShader(m_shaderProgram, vs);
    m_gl->glAttachShader(m_shaderProgram, fs);
    m_gl->glLinkProgram(m_shaderProgram);

    m_gl->glDeleteShader(vs);
    m_gl->glDeleteShader(fs);

    GLint linked = GL_FALSE;
    m_gl->glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        m_gl->glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        m_shaderInitialized = false;
        return;
    }

    m_shaderTime = 0.0f;
    m_shaderFrame = 0;
    m_shaderInitialized = true;
}

void HoverPreviewController::renderShaderFrame()
{
    GlOwnerScope glScope(m_glOwner);
    if (m_gl && m_shaderInitialized && m_shaderProgram != 0 && m_shaderFbo != 0 &&
        m_quadVao != 0 && glScope.isReady()) {
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFbo);
        m_gl->glViewport(0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT);

        m_gl->glUseProgram(m_shaderProgram);

        GLint u_time = m_gl->glGetUniformLocation(m_shaderProgram, "iTime");
        if (u_time >= 0)
            m_gl->glUniform1f(u_time, m_shaderTime);

        GLint u_res = m_gl->glGetUniformLocation(m_shaderProgram, "iResolution");
        if (u_res >= 0)
            m_gl->glUniform3f(u_res, static_cast<float>(PREVIEW_WIDTH),
                              static_cast<float>(PREVIEW_HEIGHT), 1.0f);

        GLint u_frame = m_gl->glGetUniformLocation(m_shaderProgram, "iFrame");
        if (u_frame >= 0)
            m_gl->glUniform1i(u_frame, m_shaderFrame);

        m_gl->glBindVertexArray(m_quadVao);
        m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_gl->glBindVertexArray(0);

        QImage readback(PREVIEW_WIDTH, PREVIEW_HEIGHT, QImage::Format_RGBA8888);
        m_gl->glReadPixels(0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                           readback.bits());
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        m_currentPreviewImage = readback.flipped(Qt::Vertical);
#else
        m_currentPreviewImage = readback.mirrored(false, true);
#endif

        m_gl->glUseProgram(0);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        // Generate procedural animated shader preview when headless without a realized GL widget
        QImage img(PREVIEW_WIDTH, PREVIEW_HEIGHT, QImage::Format_RGBA8888);
        uint32_t seed = 2166136261u;
        for (char c : m_activePath) {
            seed ^= static_cast<uint8_t>(c);
            seed *= 16777619u;
        }
        float phase = static_cast<float>(seed & 0xFFFFu) * 0.001f + m_shaderTime;
        for (uint32_t y = 0; y < PREVIEW_HEIGHT; ++y) {
            uint8_t *row = img.scanLine(static_cast<int>(y));
            float ny = (2.0f * static_cast<float>(y) - static_cast<float>(PREVIEW_HEIGHT)) /
                       static_cast<float>(PREVIEW_HEIGHT);
            for (uint32_t x = 0; x < PREVIEW_WIDTH; ++x) {
                float nx = (2.0f * static_cast<float>(x) - static_cast<float>(PREVIEW_WIDTH)) /
                           static_cast<float>(PREVIEW_HEIGHT);
                float d = std::sqrt(nx * nx + ny * ny);
                float a = std::atan2(ny, nx);
                float v = std::sin(8.0f * d - 2.5f * phase + std::sin(3.0f * a + phase));
                float r = 0.5f + 0.5f * std::cos(phase + a + 0.0f);
                float g = 0.5f + 0.5f * std::cos(phase + a + 2.0f);
                float b = 0.5f + 0.5f * std::cos(phase + a + 4.0f);
                float mod = 0.65f + 0.35f * v;
                row[x * 4 + 0] = static_cast<uint8_t>(std::clamp(r * mod * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 1] = static_cast<uint8_t>(std::clamp(g * mod * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 2] = static_cast<uint8_t>(std::clamp(b * mod * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 3] = 255;
            }
        }
        m_currentPreviewImage = img;
    }

    m_shaderTime += 0.0333f;
    m_shaderFrame++;
}

void HoverPreviewController::onPlaybackTick()
{
    if (!m_previewActive)
        return;

    if (m_activeBadge == WallpaperBadge::Shader) {
        renderShaderFrame();
        emit previewFrameReady(m_activeItemId, m_previewTexId);
    } else if (m_activeBadge == WallpaperBadge::Video && !m_videoBaseImage.isNull()) {
        QImage frame = computeVideoZoomFrame(m_videoBaseImage, m_currentFrameIndex,
                                             static_cast<int>(PREVIEW_WIDTH),
                                             static_cast<int>(PREVIEW_HEIGHT));
        m_currentFrameIndex++;
        if (!frame.isNull()) {
            updateTextureWithImage(frame);
            emit previewFrameReady(m_activeItemId, m_previewTexId);
        }
    } else if (!m_preloadedFrames.empty()) {
        const auto &frame = m_preloadedFrames[m_currentFrameIndex % m_preloadedFrames.size()];
        updateTextureWithImage(frame);
        m_currentFrameIndex++;
        emit previewFrameReady(m_activeItemId, m_previewTexId);
    }
}

void HoverPreviewController::cleanupActivePreview()
{
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
    m_movie.reset();
    m_preloadedFrames.clear();
    m_videoBaseImage = QImage();
    m_currentPreviewImage = QImage();

    if (m_shaderProgram != 0 && m_gl) {
        GlOwnerScope glScope(m_glOwner);
        if (glScope.isReady()) {
            m_gl->glDeleteProgram(m_shaderProgram);
            m_shaderProgram = 0;
        }
    }
    m_shaderInitialized = false;

    bool was_active = m_previewActive;
    uint32_t stopped_id = m_activeItemId;
    m_activeItemId = 0;
    m_previewActive = false;

    if (was_active) {
        emit previewStopped(stopped_id);
    }
}

} // namespace wwal::studio
