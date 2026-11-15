// SPDX-License-Identifier: GPL-3.0-or-later
#include "hover_preview_controller.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <sys/syscall.h>
#include <unistd.h>
#include <QImageReader>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QProcess>

extern "C" {
#include <waywal/image_loader.h>
#include <waywal/thumb_cache.h>
}

namespace wwal::studio {

namespace {

std::string toLowerExtension(const std::string &path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return ext;
}

bool isVideoExtension(const std::string &ext)
{
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov";
}

QImage scaleCoverAndCropRgba(const QImage &src, uint32_t target_w, uint32_t target_h)
{
    if (src.isNull()) {
        return {};
    }
    if (src.width() == static_cast<int>(target_w) &&
        src.height() == static_cast<int>(target_h) &&
        src.format() == QImage::Format_RGBA8888) {
        return src;
    }
    QImage scaled = src.scaled(static_cast<int>(target_w), static_cast<int>(target_h),
                               Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() > static_cast<int>(target_w) ||
        scaled.height() > static_cast<int>(target_h)) {
        const int cx = (scaled.width() - static_cast<int>(target_w)) / 2;
        const int cy = (scaled.height() - static_cast<int>(target_h)) / 2;
        scaled = scaled.copy(cx, cy, static_cast<int>(target_w), static_cast<int>(target_h));
    }
    return scaled.convertToFormat(QImage::Format_RGBA8888);
}

QImage readImageWithScaledLimit(const std::string &path, uint32_t max_w, uint32_t max_h)
{
    QImageReader reader(QString::fromStdString(path));
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QSize sz = reader.size();
    if (sz.isValid() && sz.width() > 0 && sz.height() > 0 &&
        (sz.width() > static_cast<int>(max_w) || sz.height() > static_cast<int>(max_h))) {
        QSize scaled = sz.scaled(static_cast<int>(max_w), static_cast<int>(max_h),
                                 Qt::KeepAspectRatioByExpanding);
        if (scaled.isValid() && scaled.width() > 0 && scaled.height() > 0 &&
            scaled.width() <= 8192 && scaled.height() <= 8192) {
            reader.setScaledSize(scaled);
        }
    }
    return reader.read();
}

QImage loadPreviewFileRobust(const std::string &path)
{
    if (path.empty()) {
        return {};
    }

    QImage img = readImageWithScaledLimit(path, HoverPreviewController::PREVIEW_WIDTH * 2,
                                          HoverPreviewController::PREVIEW_HEIGHT * 2);
    if (!img.isNull()) {
        return img;
    }

    uint8_t *rgba = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    if (image_load_rgba(path.c_str(), &rgba, &w, &h) && rgba && w > 0 && h > 0) {
        const qsizetype stride = static_cast<qsizetype>(w) * 4;
        const bool dims_ok = w <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                             h <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                             stride <= std::numeric_limits<int>::max();
        if (dims_ok) {
            QImage raw(rgba, static_cast<int>(w), static_cast<int>(h), stride,
                       QImage::Format_RGBA8888);
            QImage copy = raw.copy();
            image_free(rgba);
            return copy;
        }
    }
    if (rgba) {
        image_free(rgba);
    }

    char cache_path[4096];
    if (waywal_thumb_resolve_cache_path(path.c_str(), cache_path, sizeof(cache_path))) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(cache_path, ec) && !ec) {
            QImage cached_img = readImageWithScaledLimit(
                cache_path, HoverPreviewController::PREVIEW_WIDTH,
                HoverPreviewController::PREVIEW_HEIGHT);
            if (!cached_img.isNull()) {
                return cached_img;
            }
        }
        const std::string png_cache = std::string(cache_path) + ".png";
        if (std::filesystem::is_regular_file(png_cache, ec) && !ec) {
            QImage cached_img = readImageWithScaledLimit(
                png_cache, HoverPreviewController::PREVIEW_WIDTH,
                HoverPreviewController::PREVIEW_HEIGHT);
            if (!cached_img.isNull()) {
                return cached_img;
            }
        }
    }
    return {};
}

QImage renderProceduralCpuShaderFrame(const std::string &path, float shader_time, uint32_t w,
                                      uint32_t h)
{
    QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
    uint32_t seed = 2166136261u;
    for (char c : path) {
        seed = (seed ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    const float phase = static_cast<float>(seed & 0xFFFFu) * 0.001f + shader_time;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = img.scanLine(static_cast<int>(y));
        const float ny = (2.0f * static_cast<float>(y) - fh) / fh;
        for (uint32_t x = 0; x < w; ++x) {
            const float nx = (2.0f * static_cast<float>(x) - fw) / fh;
            const float d = std::sqrt(nx * nx + ny * ny);
            const float a = std::atan2(ny, nx);
            const float v = std::sin(8.0f * d - 2.5f * phase + std::sin(3.0f * a + phase));
            const float r = 0.5f + 0.5f * std::cos(phase + a + 0.0f);
            const float g = 0.5f + 0.5f * std::cos(phase + a + 2.0f);
            const float b = 0.5f + 0.5f * std::cos(phase + a + 4.0f);
            const float mod = 0.65f + 0.35f * v;
            row[x * 4 + 0] = static_cast<uint8_t>(std::clamp(r * mod * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 1] = static_cast<uint8_t>(std::clamp(g * mod * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 2] = static_cast<uint8_t>(std::clamp(b * mod * 255.0f, 0.0f, 255.0f));
            row[x * 4 + 3] = 255;
        }
    }
    return img;
}

std::vector<QImage> extractVideoPreviewFramesBounded(const std::string &video_path,
                                                     std::stop_token stop_token,
                                                     const std::atomic<uint64_t> &live_epoch,
                                                     uint64_t expected_epoch)
{
    auto shouldAbort = [&]() {
        return stop_token.stop_requested() ||
               live_epoch.load(std::memory_order_acquire) != expected_epoch;
    };

    std::error_code ec;
    if (shouldAbort() || !std::filesystem::is_regular_file(video_path, ec)) {
        return {};
    }

    QProcess proc;
    const QString clip_sec = QString::number(HoverPreviewController::MAX_VIDEO_PREVIEW_SECONDS);
    const QString vf_filter = QStringLiteral("fps=6,scale=%1:%2:force_original_aspect_ratio="
                                             "increase,crop=%1:%2")
                                  .arg(HoverPreviewController::PREVIEW_WIDTH)
                                  .arg(HoverPreviewController::PREVIEW_HEIGHT);
    const QStringList args = {QStringLiteral("-loglevel"), QStringLiteral("error"),
                              QStringLiteral("-ss"),       QStringLiteral("0"),
                              QStringLiteral("-t"),        clip_sec,
                              QStringLiteral("-i"),        QString::fromStdString(video_path),
                              QStringLiteral("-vf"),       vf_filter,
                              QStringLiteral("-f"),        QStringLiteral("rawvideo"),
                              QStringLiteral("-pix_fmt"),  QStringLiteral("rgba"),
                              QStringLiteral("-")};

    proc.start(QStringLiteral("ffmpeg"), args);
    if (!proc.waitForStarted(500)) {
        return {};
    }

    std::mutex pidfd_mutex;
    int pidfd = -1;
#ifdef SYS_pidfd_open
    if (const qint64 pid = proc.processId(); pid > 0) {
        pidfd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0u));
    }
#endif
    std::stop_callback stop_cb(stop_token, [&]() {
        std::lock_guard<std::mutex> lk(pidfd_mutex);
#ifdef SYS_pidfd_send_signal
        if (pidfd >= 0) {
            ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0u);
        }
#endif
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    bool finished = false;
    while (!finished) {
        if (shouldAbort() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now())
                                .count();
        finished = proc.waitForFinished(static_cast<int>(std::clamp<int64_t>(rem_ms, 1, 100)));
    }

    if (!finished && proc.state() != QProcess::NotRunning) {
        proc.kill();
        proc.waitForFinished(500);
    }

    {
        std::lock_guard<std::mutex> lk(pidfd_mutex);
        if (pidfd >= 0) {
            ::close(pidfd);
            pidfd = -1;
        }
    }

    if (shouldAbort()) {
        return {};
    }

    const QByteArray raw = proc.readAllStandardOutput();
    constexpr size_t kFrameBytes =
        static_cast<size_t>(HoverPreviewController::PREVIEW_WIDTH) *
        HoverPreviewController::PREVIEW_HEIGHT * 4u;
    const size_t frame_count =
        std::min<size_t>(static_cast<size_t>(raw.size()) / kFrameBytes, 24u);
    if (frame_count == 0) {
        return {};
    }

    std::vector<QImage> frames;
    frames.reserve(frame_count);
    const auto *data = reinterpret_cast<const uchar *>(raw.constData());
    for (size_t i = 0; i < frame_count; ++i) {
        if (shouldAbort()) {
            return {};
        }
        QImage frame(data + (i * kFrameBytes), static_cast<int>(HoverPreviewController::PREVIEW_WIDTH),
                     static_cast<int>(HoverPreviewController::PREVIEW_HEIGHT),
                     static_cast<int>(HoverPreviewController::PREVIEW_WIDTH) * 4,
                     QImage::Format_RGBA8888);
        frames.push_back(frame.copy());
    }
    return frames;
}

std::string stripLeadingVersionDirective(const std::string &src)
{
    std::istringstream in(src);
    std::ostringstream out;
    std::string line;
    bool stripped_first = false;
    while (std::getline(in, line)) {
        if (!stripped_first) {
            const auto first_non_ws = line.find_first_not_of(" \t\r");
            if (first_non_ws == std::string::npos) {
                continue;
            }
            if (line.compare(first_non_ws, 8, "#version") == 0) {
                stripped_first = true;
                continue;
            }
            stripped_first = true;
        }
        out << line << '\n';
    }
    return out.str();
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

const char *WWAL_SHADER_UNIFORM_PROLOGUE = R"(in vec2 v_uv;
out vec4 fragColor;

uniform vec3 iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform int iFrame;
uniform float iFrameRate;
uniform vec4 iMouse;
uniform vec4 iDate;
uniform float iSampleRate;
uniform float iChannelTime[4];
uniform vec3 iChannelResolution[4];
uniform sampler2D iChannel0;
uniform sampler2D iChannel1;
uniform sampler2D iChannel2;
uniform sampler2D iChannel3;

uniform float u_audio_bass;
uniform float u_audio_mid;
uniform float u_audio_treble;
uniform float u_audio_energy;
uniform float u_audio_rms;
uniform float u_audio_beat;
uniform vec3 u_palette_primary;
uniform vec3 u_palette_secondary;
uniform vec3 u_palette_accent;
uniform vec3 u_palette_background;
uniform vec2 u_mouse_smooth;
uniform vec2 u_mouse_vel;
uniform vec2 u_workspace_offset;
uniform float u_depth_strength;
uniform vec4 u_span_rect;
uniform vec2 u_local_size;
uniform int u_swap_rb;
uniform int u_flip_y;
uniform sampler2D u_album_art;
uniform float u_media_playing;
uniform sampler2D u_noise_tex;

float waywal_hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float waywal_noise2d(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = waywal_hash21(i);
    float b = waywal_hash21(i + vec2(1.0, 0.0));
    float c = waywal_hash21(i + vec2(0.0, 1.0));
    float d = waywal_hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec4 waywal_noise2d_vec4(vec2 uv) {
    float n = waywal_noise2d(uv * 8.0);
    return vec4(n, waywal_noise2d(uv * 16.0 + 13.7), waywal_noise2d(uv * 32.0 + 41.2), 1.0);
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
    return scaleCoverAndCropRgba(baseImg.copy(cropX, cropY, cropW, cropH),
                                 static_cast<uint32_t>(outW), static_cast<uint32_t>(outH));
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

HoverPreviewController::~HoverPreviewController()
{
    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }
    if (m_playbackTimer && m_playbackTimer->isActive()) {
        m_playbackTimer->stop();
    }
    m_hasPendingItem = false;
    cancelWorker();
    cleanupActivePreview(false);
}

void HoverPreviewController::setGlOwner(QOpenGLWidget *owner)
{
    if (m_glOwnerDestroyedConn) {
        disconnect(m_glOwnerDestroyedConn);
    }
    m_glOwner = owner;
    if (owner) {
        m_glOwnerDestroyedConn = connect(owner, &QObject::destroyed, this, [this]() {
            m_gl = nullptr;
            resetGlIdsLocked();
        });
    }
}

void HoverPreviewController::resetGlIdsLocked() noexcept
{
    m_previewTexId = 0;
    m_neutralTexId = 0;
    m_shaderFbo = 0;
    m_shaderProgram = 0;
    m_quadVao = 0;
    m_quadVbo = 0;
    m_readbackPbos[0] = 0;
    m_readbackPbos[1] = 0;
    m_readbackPrimed[0] = false;
    m_readbackPrimed[1] = false;
    m_readbackIdx = 0;
    m_shaderInitialized = false;
}

void HoverPreviewController::cancelWorker()
{
    m_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (m_workerThread.joinable()) {
        m_workerThread.request_stop();
        m_workerThread.join();
    }
    m_cpuFrameInFlight.store(false, std::memory_order_release);
}

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

    if (m_neutralTexId == 0) {
        static constexpr uint8_t kNeutralPx[4] = {128, 128, 128, 255};
        m_gl->glGenTextures(1, &m_neutralTexId);
        m_gl->glBindTexture(GL_TEXTURE_2D, m_neutralTexId);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                           kNeutralPx);
        m_gl->glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (m_shaderFbo == 0) {
        GLint prev_fbo = 0;
        m_gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        m_gl->glGenFramebuffers(1, &m_shaderFbo);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFbo);
        m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     m_previewTexId, 0);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    }

    if (m_quadVao == 0) {
        static constexpr float kQuadVerts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        GLint prev_vao = 0;
        m_gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        m_gl->glGenVertexArrays(1, &m_quadVao);
        m_gl->glGenBuffers(1, &m_quadVbo);
        m_gl->glBindVertexArray(m_quadVao);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
        m_gl->glEnableVertexAttribArray(0);
        m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_gl->glBindVertexArray(static_cast<GLuint>(prev_vao));
    }

    if (m_readbackPbos[0] == 0) {
        GLint prev_pack = 0;
        m_gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
        constexpr GLsizeiptr kReadbackBytes =
            static_cast<GLsizeiptr>(PREVIEW_WIDTH) * PREVIEW_HEIGHT * 4;
        m_gl->glGenBuffers(2, m_readbackPbos);
        for (int i = 0; i < 2; ++i) {
            m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readbackPbos[i]);
            m_gl->glBufferData(GL_PIXEL_PACK_BUFFER, kReadbackBytes, nullptr, GL_STREAM_READ);
            m_readbackPrimed[i] = false;
        }
        m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pack));
        m_readbackIdx = 0;
    }
}

void HoverPreviewController::destroyGl(QOpenGLExtraFunctions *gl)
{
    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }
    m_hasPendingItem = false;
    cancelWorker();
    cleanupActivePreview(false);

    if (gl) {
        if (m_readbackPbos[0] != 0 || m_readbackPbos[1] != 0) {
            gl->glDeleteBuffers(2, m_readbackPbos);
        }
        if (m_shaderProgram != 0) {
            gl->glDeleteProgram(m_shaderProgram);
        }
        if (m_quadVbo != 0) {
            gl->glDeleteBuffers(1, &m_quadVbo);
        }
        if (m_quadVao != 0) {
            gl->glDeleteVertexArrays(1, &m_quadVao);
        }
        if (m_shaderFbo != 0) {
            gl->glDeleteFramebuffers(1, &m_shaderFbo);
        }
        if (m_neutralTexId != 0) {
            gl->glDeleteTextures(1, &m_neutralTexId);
        }
        if (m_previewTexId != 0) {
            gl->glDeleteTextures(1, &m_previewTexId);
        }
    }
    resetGlIdsLocked();
    m_gl = nullptr;
}

void HoverPreviewController::onCellHoverEntered(uint32_t item_id, const std::string &path,
                                                WallpaperBadge badge,
                                                const std::string &preview_path)
{
    if (m_previewActive && m_activeItemId == item_id) {
        return;
    }

    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }

    m_pendingItemId = item_id;
    m_hasPendingItem = true;
    m_pendingPath = path;
    m_pendingBadge = badge;
    m_pendingPreviewPath = preview_path;

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
        cancelWorker();
        cleanupActivePreview(true);
    }
}

void HoverPreviewController::stop()
{
    if (m_dwellTimer && m_dwellTimer->isActive()) {
        m_dwellTimer->stop();
    }
    m_hasPendingItem = false;
    cancelWorker();
    cleanupActivePreview(true);
}

void HoverPreviewController::onDwellTimeout()
{
    if (!m_hasPendingItem)
        return;
    startActivePreview();
}

void HoverPreviewController::startActivePreview()
{
    cancelWorker();
    cleanupActivePreview(true);

    const uint64_t expected_epoch = m_epoch.load(std::memory_order_acquire);
    m_activeItemId = m_pendingItemId;
    m_activePath = m_pendingPath;
    m_activeBadge = m_pendingBadge;
    m_previewActive = true;
    m_previewStartTime = std::chrono::steady_clock::now();
    m_currentFrameIndex = 0;

    emit previewStarted(m_activeItemId);

    const std::string preview_file =
        !m_pendingPreviewPath.empty() ? m_pendingPreviewPath : m_activePath;

    if (m_activeBadge == WallpaperBadge::Shader) {
        m_shaderTime = 0.0f;
        m_shaderFrame = 0;
        m_currentPreviewImage =
            renderProceduralCpuShaderFrame(m_activePath, 0.0f, PREVIEW_WIDTH, PREVIEW_HEIGHT);

        bool gl_ready = false;
        {
            GlOwnerScope glScope(m_glOwner);
            if (m_gl && glScope.isReady()) {
                setupOffscreenShader(m_activePath);
                gl_ready = m_shaderInitialized;
            }
        }

        if (gl_ready) {
            renderShaderFrame();
            emit previewFrameReady(m_activeItemId, m_previewTexId);
            m_playbackTimer->start(FRAME_INTERVAL_MS);
        } else {
            emit previewFrameReady(m_activeItemId, m_previewTexId);
            const std::string shader_path = m_activePath;
            m_workerThread = std::jthread([this, self = QPointer<HoverPreviewController>(this),
                                           expected_epoch, shader_path](std::stop_token st) {
                std::mutex sleep_mutex;
                std::condition_variable_any sleep_cv;
                float t = 0.0333f;
                while (!st.stop_requested() &&
                       m_epoch.load(std::memory_order_acquire) == expected_epoch) {
                    {
                        std::unique_lock<std::mutex> lk(sleep_mutex);
                        if (sleep_cv.wait_for(lk, st,
                                              std::chrono::milliseconds(FRAME_INTERVAL_MS),
                                              [&]() {
                                                  return m_epoch.load(std::memory_order_acquire) !=
                                                         expected_epoch;
                                              })) {
                            break;
                        }
                    }
                    if (st.stop_requested() ||
                        m_epoch.load(std::memory_order_acquire) != expected_epoch) {
                        break;
                    }
                    QImage frame = renderProceduralCpuShaderFrame(shader_path, t, PREVIEW_WIDTH,
                                                                  PREVIEW_HEIGHT);
                    t += 0.0333f;
                    QMetaObject::invokeMethod(
                        this,
                        [self, expected_epoch, img = std::move(frame)]() mutable {
                            if (!self || !self->m_previewActive ||
                                self->m_epoch.load(std::memory_order_acquire) != expected_epoch) {
                                return;
                            }
                            self->updateTextureWithImage(img);
                            emit self->previewFrameReady(self->m_activeItemId,
                                                         self->m_previewTexId);
                        },
                        Qt::QueuedConnection);
                }
            });
        }
        return;
    }

    if (m_activeBadge == WallpaperBadge::Video) {
        QImage initial_img = loadPreviewFileRobust(preview_file);
        if (initial_img.isNull() && preview_file != m_activePath) {
            initial_img = loadPreviewFileRobust(m_activePath);
        }
        if (initial_img.isNull()) {
            initial_img =
                renderProceduralCpuShaderFrame(m_activePath, 0.0f, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        }

        if (initial_img.width() > static_cast<int>(PREVIEW_WIDTH * 2) ||
            initial_img.height() > static_cast<int>(PREVIEW_HEIGHT * 2)) {
            initial_img =
                initial_img.scaled(PREVIEW_WIDTH * 2, PREVIEW_HEIGHT * 2,
                                   Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
        m_videoBaseImage = initial_img.convertToFormat(QImage::Format_RGBA8888);
        QImage firstFrame = computeVideoZoomFrame(
            m_videoBaseImage, 0, static_cast<int>(PREVIEW_WIDTH), static_cast<int>(PREVIEW_HEIGHT));
        if (!firstFrame.isNull()) {
            updateTextureWithImage(firstFrame);
            m_currentFrameIndex = 1;
            emit previewFrameReady(m_activeItemId, m_previewTexId);
            m_playbackTimer->start(FRAME_INTERVAL_MS);
        }

        const std::string video_candidate =
            isVideoExtension(toLowerExtension(preview_file)) ? preview_file : m_activePath;
        if (isVideoExtension(toLowerExtension(video_candidate))) {
            m_workerThread = std::jthread(
                [this, self = QPointer<HoverPreviewController>(this), expected_epoch,
                 video_candidate](std::stop_token st) {
                    std::vector<QImage> frames = extractVideoPreviewFramesBounded(
                        video_candidate, st, m_epoch, expected_epoch);
                    if (frames.empty() || st.stop_requested() ||
                        m_epoch.load(std::memory_order_acquire) != expected_epoch) {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        this,
                        [self, expected_epoch, seq = std::move(frames)]() mutable {
                            if (!self || !self->m_previewActive ||
                                self->m_epoch.load(std::memory_order_acquire) != expected_epoch ||
                                seq.empty()) {
                                return;
                            }
                            self->m_preloadedFrames = std::move(seq);
                            self->m_currentFrameIndex = 0;
                        },
                        Qt::QueuedConnection);
                });
        }
        return;
    }

    // Animated GIF / WebP check (case-insensitive; QMovie::frameCount() == 0 for animated WebP).
    const std::string ext = toLowerExtension(preview_file);
    if (ext == ".gif" || ext == ".webp") {
        m_movie = std::make_unique<QMovie>(QString::fromStdString(preview_file));
        if (m_movie->isValid() && m_movie->frameCount() != 1) {
            m_movie->setCacheMode(QMovie::CacheAll);
            connect(m_movie.get(), &QMovie::frameChanged, this, [this, expected_epoch]() {
                if (m_movie && m_previewActive &&
                    m_epoch.load(std::memory_order_acquire) == expected_epoch) {
                    updateTextureWithImage(m_movie->currentImage());
                    emit previewFrameReady(m_activeItemId, m_previewTexId);
                }
            });
            m_movie->start();
            if (!m_movie->currentImage().isNull()) {
                updateTextureWithImage(m_movie->currentImage());
                emit previewFrameReady(m_activeItemId, m_previewTexId);
            }
            return;
        }
        m_movie.reset();
    }

    QImage img = loadPreviewFileRobust(preview_file);
    if (!img.isNull()) {
        QImage rgba = scaleCoverAndCropRgba(img, PREVIEW_WIDTH, PREVIEW_HEIGHT);
        m_preloadedFrames.push_back(rgba);
        updateTextureWithImage(m_preloadedFrames[0]);
        emit previewFrameReady(m_activeItemId, m_previewTexId);
    }
}

void HoverPreviewController::updateTextureWithImage(const QImage &img)
{
    if (img.isNull())
        return;

    QImage rgba = scaleCoverAndCropRgba(img, PREVIEW_WIDTH, PREVIEW_HEIGHT);
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
        fragment_code = stripLeadingVersionDirective(ss.str());
    }

    const bool has_main_image = fragment_code.find("mainImage") != std::string::npos;
    const bool has_void_main = fragment_code.find("void main") != std::string::npos;
    if (fragment_code.empty() || (!has_main_image && !has_void_main)) {
        fragment_code = fallbackFsSrc;
    } else {
        std::string wrapper = isGles ? "#version 300 es\nprecision highp float;\n"
                                     : "#version 330 core\n";
        wrapper += WWAL_SHADER_UNIFORM_PROLOGUE;
        wrapper += fragment_code;
        if (has_main_image && !has_void_main) {
            wrapper += R"(
void main() {
    vec4 col = vec4(0.0);
    mainImage(col, gl_FragCoord.xy);
    fragColor = vec4(col.rgb, 1.0);
}
)";
        }
        fragment_code = std::move(wrapper);
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
        GLint prev_fbo = 0;
        GLint prev_viewport[4]{0, 0, 0, 0};
        GLint prev_program = 0;
        GLint prev_vao = 0;
        GLint prev_active_tex = GL_TEXTURE0;
        GLint prev_pack_pbo = 0;
        m_gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        m_gl->glGetIntegerv(GL_VIEWPORT, prev_viewport);
        m_gl->glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
        m_gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        m_gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
        m_gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack_pbo);

        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_shaderFbo);
        m_gl->glViewport(0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT);

        if (m_neutralTexId != 0) {
            m_gl->glActiveTexture(GL_TEXTURE0);
            m_gl->glBindTexture(GL_TEXTURE_2D, m_neutralTexId);
        }

        m_gl->glUseProgram(m_shaderProgram);

        const auto set1f = [&](const char *name, float v) {
            if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, name); loc >= 0) {
                m_gl->glUniform1f(loc, v);
            }
        };
        const auto set1i = [&](const char *name, int v) {
            if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, name); loc >= 0) {
                m_gl->glUniform1i(loc, v);
            }
        };
        const auto set3f = [&](const char *name, float a, float b, float c) {
            if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, name); loc >= 0) {
                m_gl->glUniform3f(loc, a, b, c);
            }
        };

        const float w = static_cast<float>(PREVIEW_WIDTH);
        const float h = static_cast<float>(PREVIEW_HEIGHT);
        set1f("iTime", m_shaderTime);
        set1f("iTimeDelta", 0.0333f);
        set1i("iFrame", m_shaderFrame);
        set1f("iFrameRate", 30.0f);
        set3f("iResolution", w, h, 1.0f);
        set1f("iSampleRate", 44100.0f);

        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "iMouse"); loc >= 0) {
            m_gl->glUniform4f(loc, w * 0.5f, h * 0.5f, 0.0f, 0.0f);
        }
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "iDate"); loc >= 0) {
            const std::time_t now = std::time(nullptr);
            std::tm tm_now{};
            localtime_r(&now, &tm_now);
            const float secs = static_cast<float>((tm_now.tm_hour * 3600) +
                                                  (tm_now.tm_min * 60) + tm_now.tm_sec);
            m_gl->glUniform4f(loc, static_cast<float>(tm_now.tm_year + 1900),
                              static_cast<float>(tm_now.tm_mon + 1),
                              static_cast<float>(tm_now.tm_mday), secs);
        }
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "iChannelTime"); loc >= 0) {
            const float ch_t[4] = {m_shaderTime, m_shaderTime, m_shaderTime, m_shaderTime};
            m_gl->glUniform1fv(loc, 4, ch_t);
        }
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "iChannelResolution");
            loc >= 0) {
            const float ch_res[12] = {w, h, 1.0f, w, h, 1.0f, w, h, 1.0f, w, h, 1.0f};
            m_gl->glUniform3fv(loc, 4, ch_res);
        }

        set1i("iChannel0", 0);
        set1i("iChannel1", 0);
        set1i("iChannel2", 0);
        set1i("iChannel3", 0);
        set1i("u_noise_tex", 0);
        set1i("u_album_art", 0);
        set1f("u_audio_bass", 0.25f + 0.15f * std::sin(m_shaderTime * 2.0f));
        set1f("u_audio_mid", 0.20f + 0.10f * std::cos(m_shaderTime * 1.5f));
        set1f("u_audio_treble", 0.15f);
        set1f("u_audio_energy", 0.22f);
        set1f("u_audio_rms", 0.18f);
        set1f("u_audio_beat", 0.0f);
        set3f("u_palette_primary", 0.35f, 0.58f, 0.92f);
        set3f("u_palette_secondary", 0.18f, 0.28f, 0.48f);
        set3f("u_palette_accent", 0.92f, 0.45f, 0.58f);
        set3f("u_palette_background", 0.06f, 0.08f, 0.12f);
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "u_mouse_smooth");
            loc >= 0) {
            m_gl->glUniform2f(loc, 0.5f, 0.5f);
        }
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "u_local_size");
            loc >= 0) {
            m_gl->glUniform2f(loc, w, h);
        }
        if (const GLint loc = m_gl->glGetUniformLocation(m_shaderProgram, "u_span_rect");
            loc >= 0) {
            m_gl->glUniform4f(loc, 0.0f, 0.0f, 1.0f, 1.0f);
        }

        m_gl->glBindVertexArray(m_quadVao);
        m_gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Ping-pong PBO readback avoids stalling the GPU command queue on every tick.
        if (m_readbackPbos[0] != 0 && m_readbackPbos[1] != 0) {
            constexpr size_t kFrameBytes =
                static_cast<size_t>(PREVIEW_WIDTH) * PREVIEW_HEIGHT * 4u;
            const uint32_t curr = m_readbackIdx & 1u;
            const uint32_t prev = 1u - curr;

            if (m_readbackPrimed[prev]) {
                m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readbackPbos[prev]);
                void *mapped = m_gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                      static_cast<GLsizeiptr>(kFrameBytes),
                                                      GL_MAP_READ_BIT);
                if (mapped) {
                    QImage raw(static_cast<const uchar *>(mapped), PREVIEW_WIDTH, PREVIEW_HEIGHT,
                               static_cast<qsizetype>(PREVIEW_WIDTH) * 4,
                               QImage::Format_RGBA8888);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                    m_currentPreviewImage = raw.flipped(Qt::Vertical);
#else
                    m_currentPreviewImage = raw.mirrored(false, true);
#endif
                    m_gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
            }

            m_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
            m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readbackPbos[curr]);
            m_gl->glReadPixels(0, 0, PREVIEW_WIDTH, PREVIEW_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                               nullptr);
            m_readbackPrimed[curr] = true;
            m_readbackIdx = prev;
        }

        m_gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pack_pbo));
        m_gl->glActiveTexture(static_cast<GLenum>(prev_active_tex));
        m_gl->glBindVertexArray(static_cast<GLuint>(prev_vao));
        m_gl->glUseProgram(static_cast<GLuint>(prev_program));
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
        m_gl->glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    } else {
        m_currentPreviewImage = renderProceduralCpuShaderFrame(m_activePath, m_shaderTime,
                                                               PREVIEW_WIDTH, PREVIEW_HEIGHT);
    }

    m_shaderTime += 0.0333f;
    m_shaderFrame++;
}

void HoverPreviewController::onPlaybackTick()
{
    if (!m_previewActive)
        return;

    if (m_activeBadge == WallpaperBadge::Video &&
        (std::chrono::steady_clock::now() - m_previewStartTime) >=
            std::chrono::seconds(MAX_VIDEO_PREVIEW_SECONDS)) {
        if (m_playbackTimer && m_playbackTimer->isActive()) {
            m_playbackTimer->stop();
        }
        return;
    }

    if (m_activeBadge == WallpaperBadge::Shader) {
        renderShaderFrame();
        emit previewFrameReady(m_activeItemId, m_previewTexId);
    } else if (m_activeBadge == WallpaperBadge::Video) {
        if (!m_preloadedFrames.empty()) {
            const auto &frame =
                m_preloadedFrames[m_currentFrameIndex % m_preloadedFrames.size()];
            m_currentFrameIndex++;
            updateTextureWithImage(frame);
            emit previewFrameReady(m_activeItemId, m_previewTexId);
        } else if (!m_videoBaseImage.isNull()) {
            QImage frame = computeVideoZoomFrame(m_videoBaseImage, m_currentFrameIndex,
                                                 static_cast<int>(PREVIEW_WIDTH),
                                                 static_cast<int>(PREVIEW_HEIGHT));
            m_currentFrameIndex++;
            if (!frame.isNull()) {
                updateTextureWithImage(frame);
                emit previewFrameReady(m_activeItemId, m_previewTexId);
            }
        }
    } else if (!m_preloadedFrames.empty()) {
        const auto &frame = m_preloadedFrames[m_currentFrameIndex % m_preloadedFrames.size()];
        updateTextureWithImage(frame);
        m_currentFrameIndex++;
        emit previewFrameReady(m_activeItemId, m_previewTexId);
    }
}

void HoverPreviewController::cleanupActivePreview(bool emit_signal)
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
    m_readbackPrimed[0] = false;
    m_readbackPrimed[1] = false;

    const bool was_active = m_previewActive;
    const uint32_t stopped_id = m_activeItemId;
    m_activeItemId = 0;
    m_previewActive = false;

    if (was_active && emit_signal) {
        emit previewStopped(stopped_id);
    }
}

} // namespace wwal::studio
