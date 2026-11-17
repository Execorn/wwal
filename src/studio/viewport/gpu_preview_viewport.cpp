// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_preview_viewport.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFocusEvent>
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
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <algorithm>
#include <limits>
#include <unordered_set>

extern "C" {
#include <waywal/image_loader.h>
#include <waywal/thumb_cache.h>
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

bool readExactFd(int fd, void *dst, size_t len, int timeoutMs) noexcept
{
    uint8_t *out = static_cast<uint8_t *>(dst);
    size_t offset = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeoutMs));
    while (offset < len) {
        const ssize_t n = ::read(fd, out + offset, len - offset);
        if (n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const int remMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, std::max(1, remMs));
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            return false;
        }
    }
    return true;
}

bool drainFd(int fd, size_t len, int timeoutMs) noexcept
{
    std::array<uint8_t, 512> scratch{};
    size_t remaining = len;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, scratch.size());
        if (!readExactFd(fd, scratch.data(), chunk, timeoutMs)) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

[[nodiscard]] bool isComputeShaderSource(const QString &src)
{
    static const QRegularExpression kLocalSizeRe(
        QStringLiteral(R"(^\s*layout\s*\([^)]*\blocal_size_x\b)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression kImageStoreRe(QStringLiteral(R"(\bimageStore\b)"));
    return kLocalSizeRe.match(src).hasMatch() && kImageStoreRe.match(src).hasMatch();
}

void parseComputeMetadata(const QString &src, GLuint &outBinding, GLenum &outFormat)
{
    outBinding = 0u;
    outFormat = GL_RGBA16F;
    static const QRegularExpression kBindingRe(
        QStringLiteral(R"(layout\s*\(([^)]*)\)\s*(?:[A-Za-z0-9_]+\s+)*image2D\b)"));
    static const QRegularExpression kBindNumRe(QStringLiteral(R"(\bbinding\s*=\s*(\d+))"));
    QRegularExpressionMatchIterator it = kBindingRe.globalMatch(src);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString quals = m.captured(1);
        const QRegularExpressionMatch bm = kBindNumRe.match(quals);
        if (bm.hasMatch()) {
            bool ok = false;
            const uint unit = bm.captured(1).toUInt(&ok);
            if (ok && unit < 8u) {
                outBinding = unit;
            }
        }
        if (quals.contains(QLatin1String("rgba8"), Qt::CaseInsensitive) &&
            !quals.contains(QLatin1String("rgba16f"), Qt::CaseInsensitive)) {
            outFormat = GL_RGBA8;
        } else if (quals.contains(QLatin1String("rgba16f"), Qt::CaseInsensitive)) {
            outFormat = GL_RGBA16F;
        }
        break;
    }
}

[[nodiscard]] QString buildShadertoySource(const QString &rawSrc, bool isGles)
{
    const char *preamble = isGles ? kShadertoyPreamble : kShadertoyPreambleCore;
    if (!rawSrc.contains(QLatin1String("#version"))) {
        return QString::fromUtf8(preamble) + rawSrc + QString::fromUtf8(kShadertoyEpilogue);
    }

    // Keep user #version and #extension at the top to prevent duplicate #version errors.
    static const QRegularExpression kVersionOrExtRe(
        QStringLiteral(R"(^\s*#\s*(version|extension)\b[^\n]*(?:\n|$))"),
        QRegularExpression::MultilineOption);
    QString head;
    QString body = rawSrc;
    qsizetype offset = 0;
    while (offset < body.size()) {
        const QRegularExpressionMatch m = kVersionOrExtRe.match(body, offset);
        if (m.hasMatch() && m.capturedStart() == offset) {
            head += m.captured(0);
            offset = m.capturedEnd();
        } else if (body.at(offset).isSpace()) {
            head += body.at(offset);
            ++offset;
        } else {
            break;
        }
    }
    body = body.mid(offset);

    QString uniformBlock = QString::fromUtf8(preamble);
    const qsizetype verEnd = uniformBlock.indexOf(QLatin1Char('\n'));
    if (verEnd >= 0) {
        uniformBlock = uniformBlock.mid(verEnd + 1);
    }
    QString assembled = head + QLatin1Char('\n') + uniformBlock + body;
    if (!isGles) {
        assembled = convertEsFragmentToCore330(assembled);
    }
    return assembled + QString::fromUtf8(kShadertoyEpilogue);
}

void zeroFillTexture2D(GLuint tex, int width, int height, bool isHalfFloat)
{
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx || tex == 0 || width <= 0 || height <= 0) {
        return;
    }
    auto *gl = ctx->functions();
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    // Upload in bounded row strips so 4K/8K RGBA16F FBO init avoids 66-264 MiB transient buffers.
    const int stripRows = std::min(height, 64);
    const size_t stripTexels = static_cast<size_t>(width) * static_cast<size_t>(stripRows);
    if (isHalfFloat) {
        std::vector<uint16_t> zeroBuf(stripTexels * 4u, 0u);
        for (int y = 0; y < height; y += stripRows) {
            const int rows = std::min(stripRows, height - y);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, rows, GL_RGBA, GL_HALF_FLOAT,
                                zeroBuf.data());
        }
    } else {
        std::vector<uint8_t> zeroBuf(stripTexels * 4u, 0u);
        for (int y = 0; y < height; y += stripRows) {
            const int rows = std::min(stripRows, height - y);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, rows, GL_RGBA, GL_UNSIGNED_BYTE,
                                zeroBuf.data());
        }
    }
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

[[nodiscard]] bool validateRgbaImageBounds(uint32_t w, uint32_t h, int &outStride) noexcept
{
    if (w == 0u || h == 0u || w > 65536u || h > 65536u) {
        return false;
    }
    const uint64_t stride = static_cast<uint64_t>(w) * 4ull;
    const uint64_t maxInt = static_cast<uint64_t>(std::numeric_limits<int>::max());
    if (stride > maxInt || static_cast<uint64_t>(h) > maxInt) {
        return false;
    }
    if (stride > (maxInt / static_cast<uint64_t>(h))) {
        return false;
    }
    outStride = static_cast<int>(stride);
    return true;
}

struct ProgramUniformCache {
    GLuint programId{0};
    GLint iResolution{-1};
    GLint iTime{-1};
    GLint iTimeDelta{-1};
    GLint iFrame{-1};
    GLint iMouse{-1};
    GLint iDate{-1};
    GLint uTime{-1};
    GLint uResolution{-1};
    GLint uPointer{-1};
    GLint uAudioBands{-1};
    GLint uMvp{-1};
    std::array<GLint, 5> passSamplers{-1, -1, -1, -1, -1};
    std::unordered_map<std::string, GLint> customUniforms{};

    void ensureResolved(QOpenGLShaderProgram &prog)
    {
        const GLuint pid = prog.programId();
        if (programId == pid && pid != 0u) {
            return;
        }
        programId = pid;
        iResolution = prog.uniformLocation("iResolution");
        iTime = prog.uniformLocation("iTime");
        iTimeDelta = prog.uniformLocation("iTimeDelta");
        iFrame = prog.uniformLocation("iFrame");
        iMouse = prog.uniformLocation("iMouse");
        iDate = prog.uniformLocation("iDate");
        uTime = prog.uniformLocation("u_Time");
        uResolution = prog.uniformLocation("u_Resolution");
        uPointer = prog.uniformLocation("u_Pointer");
        uAudioBands = prog.uniformLocation("u_AudioBands");
        uMvp = prog.uniformLocation("u_mvp");
        static constexpr const char *kPassSamplerNames[5] = {
            "u_BufferA", "u_BufferB", "u_BufferC", "u_BufferD", "u_DepthMap",
        };
        for (size_t i = 0; i < 5u; ++i) {
            passSamplers[i] = prog.uniformLocation(kPassSamplerNames[i]);
        }
        customUniforms.clear();
    }

    [[nodiscard]] GLint resolveCustom(QOpenGLShaderProgram &prog, const std::string &name)
    {
        auto it = customUniforms.find(name);
        if (it != customUniforms.end()) {
            return it->second;
        }
        const GLint loc = prog.uniformLocation(name.c_str());
        customUniforms.emplace(name, loc);
        return loc;
    }
};

struct ExtraRendererState {
    bool computeProcsResolved{false};
    GlTexStorage2DProc texStorage2D{nullptr};
    GlBindImageTextureProc bindImageTex{nullptr};
    GlDispatchComputeProc dispatchCompute{nullptr};
    GlMemoryBarrierProc memoryBarrier{nullptr};

    GLuint blitProgramId{0};
    GLint blitUMvp{-1};
    GLint blitUTexture{-1};
    GLint blitUFlipY{-1};
    GLint blitUIsBgr{-1};

    GLuint gizmoProgramId{0};
    GLint gizmoUMvp{-1};

    std::unordered_map<const QOpenGLShaderProgram *, ProgramUniformCache> programUniforms{};

    QOpenGLVertexArrayObject heatmapVao{};
    QOpenGLBuffer heatmapVbo{QOpenGLBuffer::VertexBuffer};
    size_t heatmapVboCapBytes{0};
    size_t cachedHeatmapVertCount{0};
    uint64_t cachedHeatmapGen{0};
    uint32_t cachedHeatCW{0};
    uint32_t cachedHeatCH{0};
    uint32_t cachedHeatBone{0};
    float cachedHeatOpacity{-1.0f};
    we_heatmap_palette_t cachedHeatPalette{WE_HEATMAP_PALETTE_TURBO};
    const void *cachedHeatVertsPtr{nullptr};
    size_t cachedHeatVertsCount{0};
    const void *cachedHeatIndicesPtr{nullptr};
    size_t cachedHeatIndicesCount{0};

    QOpenGLVertexArrayObject wireframeVao{};
    QOpenGLBuffer wireframeVbo{QOpenGLBuffer::VertexBuffer};
    size_t wireframeVboCapBytes{0};
    size_t cachedWireframeVertCount{0};
    uint64_t cachedWireframeGen{0};
    uint32_t cachedWireCW{0};
    uint32_t cachedWireCH{0};
    bool cachedWireHighlight{false};
    const void *cachedWireVertsPtr{nullptr};
    size_t cachedWireVertsCount{0};
    const void *cachedWireIndicesPtr{nullptr};
    size_t cachedWireIndicesCount{0};

    QOpenGLVertexArrayObject contourVao{};
    QOpenGLBuffer contourVbo{QOpenGLBuffer::VertexBuffer};
    size_t contourVboCapBytes{0};
    size_t cachedContourVertCount{0};
    uint64_t cachedContourGen{0};
    uint32_t cachedContourCH{0};

    QOpenGLVertexArrayObject dynVao2{};
    QOpenGLBuffer dynVbo2{QOpenGLBuffer::VertexBuffer};
    size_t dynVbo2CapBytes{0};
    uint8_t dynPingPongIdx{0};
    uint8_t dynSmallFrames{0};

    void destroyGl(bool glAlive) noexcept
    {
        if (glAlive) {
            heatmapVao.destroy();
            heatmapVbo.destroy();
            wireframeVao.destroy();
            wireframeVbo.destroy();
            contourVao.destroy();
            contourVbo.destroy();
            dynVao2.destroy();
            dynVbo2.destroy();
        }
        computeProcsResolved = false;
        texStorage2D = nullptr;
        bindImageTex = nullptr;
        dispatchCompute = nullptr;
        memoryBarrier = nullptr;
        blitProgramId = 0;
        gizmoProgramId = 0;
        programUniforms.clear();
        heatmapVboCapBytes = 0;
        cachedHeatmapVertCount = 0;
        cachedHeatmapGen = 0;
        wireframeVboCapBytes = 0;
        cachedWireframeVertCount = 0;
        cachedWireframeGen = 0;
        contourVboCapBytes = 0;
        cachedContourVertCount = 0;
        cachedContourGen = 0;
        dynVbo2CapBytes = 0;
        dynPingPongIdx = 0;
        dynSmallFrames = 0;
    }
};

std::unordered_map<const GlSceneRenderer *, std::unique_ptr<ExtraRendererState>> &
extraRendererStates()
{
    static std::unordered_map<const GlSceneRenderer *, std::unique_ptr<ExtraRendererState>> s_map;
    return s_map;
}

ExtraRendererState &getExtraRendererState(const GlSceneRenderer *r)
{
    auto &map = extraRendererStates();
    auto &ptr = map[r];
    if (!ptr) {
        ptr = std::make_unique<ExtraRendererState>();
    }
    return *ptr;
}

struct ExtraViewportState {
    QString sourceImagePath{};
    uint64_t wireframeGen{1};
    uint64_t heatmapGen{1};
    uint64_t contourGen{1};
    QSize lastLayoutSize{};

    PreviewMode cachedStatusMode{PreviewMode::OfflineEmpty};
    uint32_t cachedStatusCW{0};
    uint32_t cachedStatusCH{0};
    int cachedStatusZoomPct{-1};
    int cachedStatusFps{-1};
    int cachedStatusWidgetW{-1};
    QString cachedTelemetryStr{};
    float cachedHudW{80.0f};
};

std::unordered_map<const GpuPreviewViewport *, ExtraViewportState> &extraViewportStates()
{
    static std::unordered_map<const GpuPreviewViewport *, ExtraViewportState> s_map;
    return s_map;
}

struct WeightBridgeExtraState {
    static constexpr size_t kGridDim = 64u;
    std::vector<uint64_t> dirtyBits{};
    std::vector<uint64_t> preBlockBits{};
    bool gridValid{false};
    const void *gridVertsPtr{nullptr};
    size_t gridVertsCount{0};
    float sampleFirstX{0.0f};
    float sampleFirstY{0.0f};
    float sampleLastX{0.0f};
    float sampleLastY{0.0f};
    float minX{0.0f};
    float minY{0.0f};
    float invCellW{1.0f};
    float invCellH{1.0f};
    std::array<uint32_t, kGridDim * kGridDim + 1u> cellOffsets{};
    std::vector<uint32_t> cellVerts{};
    std::vector<uint32_t> dabCandidates{};

    void clearStrokeBits()
    {
        dirtyBits.clear();
        preBlockBits.clear();
    }

    void invalidateGrid()
    {
        gridValid = false;
        gridVertsPtr = nullptr;
        gridVertsCount = 0;
        cellVerts.clear();
    }
};

std::unordered_map<const WeightPaintBrushBridge *, WeightBridgeExtraState> &weightBridgeStates()
{
    static std::unordered_map<const WeightPaintBrushBridge *, WeightBridgeExtraState> s_map;
    return s_map;
}

void ensureWeightGrid(WeightBridgeExtraState &ws, const std::vector<we_mdl_vertex_t> &verts)
{
    const size_t n = verts.size();
    if (n < 512u) {
        ws.invalidateGrid();
        return;
    }
    const float fX = verts.front().pos[0];
    const float fY = verts.front().pos[1];
    const float lX = verts.back().pos[0];
    const float lY = verts.back().pos[1];
    if (ws.gridValid && ws.gridVertsPtr == verts.data() && ws.gridVertsCount == n &&
        ws.sampleFirstX == fX && ws.sampleFirstY == fY && ws.sampleLastX == lX &&
        ws.sampleLastY == lY) {
        return;
    }

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    size_t finiteCount = 0;
    for (size_t i = 0; i < n; ++i) {
        const float x = verts[i].pos[0];
        const float y = verts[i].pos[1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        ++finiteCount;
    }

    ws.gridVertsPtr = verts.data();
    ws.gridVertsCount = n;
    ws.sampleFirstX = fX;
    ws.sampleFirstY = fY;
    ws.sampleLastX = lX;
    ws.sampleLastY = lY;

    if (finiteCount == 0u) {
        ws.minX = 0.0f;
        ws.minY = 0.0f;
        ws.invCellW = 1.0f;
        ws.invCellH = 1.0f;
        ws.cellOffsets.fill(0u);
        ws.cellVerts.clear();
        ws.gridValid = true;
        return;
    }

    constexpr size_t G = WeightBridgeExtraState::kGridDim;
    const float spanX = std::max(1.0f, maxX - minX);
    const float spanY = std::max(1.0f, maxY - minY);
    ws.minX = minX;
    ws.minY = minY;
    ws.invCellW = static_cast<float>(G) / spanX;
    ws.invCellH = static_cast<float>(G) / spanY;

    std::array<uint32_t, G * G> counts{};
    for (size_t i = 0; i < n; ++i) {
        const float x = verts[i].pos[0];
        const float y = verts[i].pos[1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        const size_t cx =
            std::min(G - 1u, static_cast<size_t>(std::max(0.0f, (x - ws.minX) * ws.invCellW)));
        const size_t cy =
            std::min(G - 1u, static_cast<size_t>(std::max(0.0f, (y - ws.minY) * ws.invCellH)));
        ++counts[cy * G + cx];
    }

    ws.cellOffsets[0] = 0u;
    for (size_t c = 0; c < G * G; ++c) {
        ws.cellOffsets[c + 1u] = ws.cellOffsets[c] + counts[c];
    }
    ws.cellVerts.resize(ws.cellOffsets[G * G]);
    std::array<uint32_t, G * G> writeCursor = {};
    std::memcpy(writeCursor.data(), ws.cellOffsets.data(), sizeof(writeCursor));

    for (size_t i = 0; i < n; ++i) {
        const float x = verts[i].pos[0];
        const float y = verts[i].pos[1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        const size_t cx =
            std::min(G - 1u, static_cast<size_t>(std::max(0.0f, (x - ws.minX) * ws.invCellW)));
        const size_t cy =
            std::min(G - 1u, static_cast<size_t>(std::max(0.0f, (y - ws.minY) * ws.invCellH)));
        ws.cellVerts[writeCursor[cy * G + cx]++] = static_cast<uint32_t>(i);
    }
    ws.gridValid = true;
}

void buildWireframeLinesLinear(const GizmoMeshWireframeData &mesh, uint32_t cw, uint32_t ch,
                               std::vector<GizmoVertex> &outLineVerts)
{
    outLineVerts.clear();
    if (!mesh.visible || mesh.vertices.empty() || mesh.indices.size() < 3u) {
        return;
    }

    const size_t vertCount = mesh.vertices.size();
    const size_t triCount = mesh.indices.size() / 3u;
    const size_t effectiveVertCap = std::min<size_t>(vertCount, 65536u);

    std::vector<uint32_t> counts(effectiveVertCap, 0u);
    for (size_t t = 0; t < triCount; ++t) {
        const uint16_t i0 = mesh.indices[t * 3u + 0u];
        const uint16_t i1 = mesh.indices[t * 3u + 1u];
        const uint16_t i2 = mesh.indices[t * 3u + 2u];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
            continue;
        }
        ++counts[std::min(i0, i1)];
        ++counts[std::min(i1, i2)];
        ++counts[std::min(i2, i0)];
    }

    std::vector<uint32_t> offsets(effectiveVertCap + 1u, 0u);
    for (size_t v = 0; v < effectiveVertCap; ++v) {
        offsets[v + 1u] = offsets[v] + counts[v];
    }
    const uint32_t totalHalfEdges = offsets[effectiveVertCap];
    if (totalHalfEdges == 0u) {
        return;
    }

    std::vector<uint16_t> edgeTargets(totalHalfEdges);
    std::vector<uint32_t> writeCursor = offsets;
    for (size_t t = 0; t < triCount; ++t) {
        const uint16_t i0 = mesh.indices[t * 3u + 0u];
        const uint16_t i1 = mesh.indices[t * 3u + 1u];
        const uint16_t i2 = mesh.indices[t * 3u + 2u];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
            continue;
        }
        auto pushEdge = [&](uint16_t u, uint16_t v) {
            const uint16_t a = std::min(u, v);
            const uint16_t b = std::max(u, v);
            edgeTargets[writeCursor[a]++] = b;
        };
        pushEdge(i0, i1);
        pushEdge(i1, i2);
        pushEdge(i2, i0);
    }

    std::vector<uint32_t> seenStamp(effectiveVertCap, 0u);
    std::vector<uint32_t> seenCount(effectiveVertCap, 0u);
    std::vector<uint16_t> uniqueNeighbors;
    uniqueNeighbors.reserve(16u);

    const float halfW = 0.5f * static_cast<float>(cw);
    const float halfH = 0.5f * static_cast<float>(ch);
    outLineVerts.reserve(std::min<size_t>(static_cast<size_t>(totalHalfEdges) * 2u,
                                          ViewportGizmoOverlay::kMaxDynamicVertices));

    for (size_t a = 0; a < effectiveVertCap; ++a) {
        const uint32_t start = offsets[a];
        const uint32_t end = offsets[a + 1u];
        if (start == end) {
            continue;
        }
        const uint32_t stamp = static_cast<uint32_t>(a + 1u);
        uniqueNeighbors.clear();
        for (uint32_t p = start; p < end; ++p) {
            const uint16_t b = edgeTargets[p];
            if (seenStamp[b] != stamp) {
                seenStamp[b] = stamp;
                seenCount[b] = 1u;
                uniqueNeighbors.push_back(b);
            } else {
                ++seenCount[b];
            }
        }

        const auto &va = mesh.vertices[a];
        const float ax = va.pos[0] + halfW;
        const float ay = va.pos[1] + halfH;
        const float az = va.pos[2];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
            continue;
        }
        for (const uint16_t b : uniqueNeighbors) {
            if (outLineVerts.size() + 2u > ViewportGizmoOverlay::kMaxDynamicVertices) {
                return;
            }
            const auto &vb = mesh.vertices[b];
            const float bx = vb.pos[0] + halfW;
            const float by = vb.pos[1] + halfH;
            const float bz = vb.pos[2];
            if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz)) {
                continue;
            }
            const bool isBoundary = (seenCount[b] == 1u) && mesh.highlightContourEdges;
            const float r = isBoundary ? 0.220f : 0.580f;
            const float g = isBoundary ? 0.741f : 0.639f;
            const float bl = isBoundary ? 0.973f : 0.722f;
            const float al = isBoundary ? 0.85f : 0.45f;
            outLineVerts.push_back({ax, ay, az, r, g, bl, al});
            outLineVerts.push_back({bx, by, bz, r, g, bl, al});
        }
    }
}

void buildHeatmapTrianglesFast(const GizmoMeshWireframeData &mesh, uint32_t activeBoneIdx,
                               float opacity, we_heatmap_palette_t palette, uint32_t cw,
                               uint32_t ch, std::vector<GizmoVertex> &outTriVerts)
{
    outTriVerts.clear();
    if (mesh.vertices.empty() || mesh.indices.size() < 3u) {
        return;
    }

    const size_t vertCount = mesh.vertices.size();
    const size_t triCount = mesh.indices.size() / 3u;
    const float alpha = std::clamp(opacity, 0.05f, 0.95f);
    const float halfW = 0.5f * static_cast<float>(cw);
    const float halfH = 0.5f * static_cast<float>(ch);

    struct CachedVert {
        GizmoVertex gv;
        bool finite;
    };
    std::vector<CachedVert> cachedVerts(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        const auto &v = mesh.vertices[i];
        const float wx = v.pos[0] + halfW;
        const float wy = v.pos[1] + halfH;
        const float wz = v.pos[2];
        const bool ok = std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz);
        cachedVerts[i].finite = ok;
        if (!ok) {
            continue;
        }
        float w = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (v.blend_indices[k] == activeBoneIdx && std::isfinite(v.weight[k])) {
                w += v.weight[k];
            }
        }
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        ViewportGizmoOverlay::evaluateHeatmapColor(palette, std::clamp(w, 0.0f, 1.0f), r, g, b);
        cachedVerts[i].gv = {wx, wy, wz, r, g, b, alpha};
    }

    outTriVerts.reserve(std::min<size_t>(triCount * 3u, ViewportGizmoOverlay::kMaxDynamicVertices));
    for (size_t t = 0; t < triCount; ++t) {
        if (outTriVerts.size() + 3u > ViewportGizmoOverlay::kMaxDynamicVertices) {
            break;
        }
        const uint16_t i0 = mesh.indices[t * 3u + 0u];
        const uint16_t i1 = mesh.indices[t * 3u + 1u];
        const uint16_t i2 = mesh.indices[t * 3u + 2u];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
            continue;
        }
        const auto &c0 = cachedVerts[i0];
        const auto &c1 = cachedVerts[i1];
        const auto &c2 = cachedVerts[i2];
        if (!c0.finite || !c1.finite || !c2.finite) {
            continue;
        }
        outTriVerts.push_back(c0.gv);
        outTriVerts.push_back(c1.gv);
        outTriVerts.push_back(c2.gv);
    }
}

void appendContourPinDiamonds(const GizmoMeshWireframeData &mesh, uint32_t ch,
                              std::vector<GizmoVertex> &out)
{
    auto pushLine = [&](float ax, float ay, float bx, float by, float r, float g, float b,
                        float a) {
        if (out.size() + 2u > ViewportGizmoOverlay::kMaxDynamicVertices) {
            return;
        }
        out.push_back({ax, ay, 0.0f, r, g, b, a});
        out.push_back({bx, by, 0.0f, r, g, b, a});
    };
    const double chD = static_cast<double>(std::max<uint32_t>(1u, ch));
    for (size_t i = 0; i < mesh.pins.size(); ++i) {
        if (out.size() + 8u > ViewportGizmoOverlay::kMaxDynamicVertices) {
            break;
        }
        const auto &pin = mesh.pins[i];
        const float px = static_cast<float>(pin.px);
        const float py = static_cast<float>(chD - pin.py);
        if (!std::isfinite(px) || !std::isfinite(py)) {
            continue;
        }
        const bool sel = (static_cast<int32_t>(i) == mesh.selectedPinIndex);
        const float pr = sel ? 10.0f : 7.0f;
        const float r = sel ? 0.220f : 0.976f;
        const float g = sel ? 0.741f : 0.886f;
        const float b = sel ? 0.973f : 0.686f;
        const float a = sel ? 0.98f : 0.90f;
        pushLine(px, py + pr, px + pr, py, r, g, b, a);
        pushLine(px + pr, py, px, py - pr, r, g, b, a);
        pushLine(px, py - pr, px - pr, py, r, g, b, a);
        pushLine(px - pr, py, px, py + pr, r, g, b, a);
    }
}

void setupGizmoVaoAndBuffer(QOpenGLFunctions *gl, QOpenGLVertexArrayObject &vao, QOpenGLBuffer &vbo,
                            size_t &capBytes, size_t initialBytes)
{
    if (!vao.isCreated()) {
        vao.create();
    }
    vao.bind();
    if (!vbo.isCreated()) {
        vbo.create();
        vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    }
    vbo.bind();
    if (capBytes < initialBytes) {
        vbo.allocate(static_cast<int>(initialBytes));
        capBytes = initialBytes;
    }
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                              reinterpret_cast<void *>(offsetof(GizmoVertex, x)));
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                              reinterpret_cast<void *>(offsetof(GizmoVertex, r)));
    vbo.release();
    vao.release();
}

void uploadStaticGizmoBuffer(QOpenGLFunctions *gl, QOpenGLVertexArrayObject &vao,
                             QOpenGLBuffer &vbo, size_t &capBytes,
                             const std::vector<GizmoVertex> &verts)
{
    const size_t maxBytes = ViewportGizmoOverlay::kMaxDynamicVertices * sizeof(GizmoVertex);
    const size_t bytes = std::min(verts.size() * sizeof(GizmoVertex), maxBytes);
    if (bytes == 0u) {
        if (vbo.isCreated() && capBytes > 65536u) {
            vbo.bind();
            gl->glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
            vbo.release();
            capBytes = 0u;
        }
        return;
    }
    if (!vao.isCreated() || !vbo.isCreated()) {
        setupGizmoVaoAndBuffer(gl, vao, vbo, capBytes, bytes);
    }
    vbo.bind();
    // glBufferData orphans the backing store so in-flight draws from prior frames do not stall.
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), verts.data(),
                     GL_DYNAMIC_DRAW);
    capBytes = bytes;
    vbo.release();
}

void shrinkBufferIfOversized(QOpenGLFunctions *gl, QOpenGLBuffer &vbo, size_t &capBytes)
{
    if (vbo.isCreated() && capBytes > 65536u) {
        vbo.bind();
        gl->glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        vbo.release();
        capBytes = 0u;
    }
}

class WireframeVisibilityGuard {
  public:
    WireframeVisibilityGuard(ViewportGizmoOverlay &overlay, bool restoreVisible) noexcept
        : m_overlay(overlay), m_restore(restoreVisible)
    {
        if (m_restore) {
            m_overlay.setMeshWireframeVisible(false);
        }
    }
    ~WireframeVisibilityGuard()
    {
        if (m_restore) {
            m_overlay.setMeshWireframeVisible(true);
        }
    }
    WireframeVisibilityGuard(const WireframeVisibilityGuard &) = delete;
    WireframeVisibilityGuard &operator=(const WireframeVisibilityGuard &) = delete;

  private:
    ViewportGizmoOverlay &m_overlay;
    bool m_restore{false};
};

[[nodiscard]] QImage decodeStillImageFile(const QString &targetPath)
{
    QImage img;
    if (QFileInfo::exists(targetPath)) {
        img.load(targetPath);
        if (img.isNull()) {
            const std::string stdPath = targetPath.toStdString();
            uint8_t *rgba = nullptr;
            uint32_t w = 0;
            uint32_t h = 0;
            int rgbaStride = 0;
            if (image_load_rgba(stdPath.c_str(), &rgba, &w, &h) && rgba &&
                validateRgbaImageBounds(w, h, rgbaStride)) {
                img = QImage(
                    rgba, static_cast<int>(w), static_cast<int>(h), rgbaStride,
                    QImage::Format_RGBA8888,
                    [](void *ptr) { image_free(static_cast<uint8_t *>(ptr)); }, rgba);
                if (img.isNull()) {
                    image_free(rgba);
                }
            } else if (rgba) {
                image_free(rgba);
            }
        }
    }

    if (img.isNull() && targetPath.startsWith(QLatin1String("/usr/share/wwal/samples/"))) {
        img = QImage(1280, 720, QImage::Format_RGBA8888);
        const std::string stdPath = targetPath.toStdString();
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
    return img;
}

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
        if (auto *ctx = QOpenGLContext::currentContext()) {
            ctx->functions()->glDeleteTextures(1, &glTexture);
        }
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

void DmaBufSwapchain::resolveEglExtensions() noexcept
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
        if (!p && QOpenGLContext::currentContext()) {
            p = reinterpret_cast<void *>(QOpenGLContext::currentContext()->getProcAddress(name));
        }
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

void DmaBufSwapchain::releaseGl() noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_currentFrame.has_value()) {
        m_currentFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR, m_eglDestroySyncKHR);
        m_currentFrame.reset();
    }
    m_eglInitialized = false;
    m_eglDisplay = EGL_NO_DISPLAY;
}

void DmaBufSwapchain::closeFds() noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_currentFrame.has_value()) {
        m_currentFrame->closeFds();
        m_currentFrame.reset();
    }
}

bool DmaBufSwapchain::hasLiveFrame() const noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_currentFrame.has_value();
}

uint32_t DmaBufSwapchain::frameWidth() const noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_currentFrame.has_value() ? m_currentFrame->width : 0u;
}

uint32_t DmaBufSwapchain::frameHeight() const noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_currentFrame.has_value() ? m_currentFrame->height : 0u;
}

GLuint DmaBufSwapchain::currentTexture(int &outIsBgr) const noexcept
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!m_currentFrame.has_value()) {
        outIsBgr = 0;
        return 0u;
    }
    const uint32_t fmt = m_currentFrame->drm_format;
    bool bgr = (fmt == DRM_FORMAT_XBGR8888 || fmt == DRM_FORMAT_ABGR8888);
#ifdef DRM_FORMAT_XBGR2101010
    bgr = bgr || (fmt == DRM_FORMAT_XBGR2101010);
#endif
#ifdef DRM_FORMAT_ABGR2101010
    bgr = bgr || (fmt == DRM_FORMAT_ABGR2101010);
#endif
#ifdef DRM_FORMAT_XBGR16161616F
    bgr = bgr || (fmt == DRM_FORMAT_XBGR16161616F);
#endif
#ifdef DRM_FORMAT_ABGR16161616F
    bgr = bgr || (fmt == DRM_FORMAT_ABGR16161616F);
#endif
    outIsBgr = bgr ? 1 : 0;
    return m_currentFrame->glTexture;
}

bool DmaBufSwapchain::importFrame(GpuPreviewViewport &vp, const waywal_dmabuf_frame_desc_t &desc,
                                  const int *planeFds, size_t numFds)
{
    auto cleanupIncomingFds = [planeFds, numFds]() {
        if (!planeFds) {
            return;
        }
        for (size_t i = 0; i < numFds; ++i) {
            if (planeFds[i] >= 0) {
                ::close(planeFds[i]);
            }
        }
    };

    if (desc.width == 0 || desc.height == 0 || desc.width > 16384u || desc.height > 16384u ||
        numFds == 0 || !planeFds) {
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(
            QStringLiteral("Invalid DMA-BUF parameters: dimensions out of bounds or no FDs"));
        return false;
    }

    const uint32_t planesToImport =
        std::min({static_cast<uint32_t>(numFds), desc.num_planes > 0 ? desc.num_planes : 1u, 4u});
    for (uint32_t p = 0; p < planesToImport; ++p) {
        if (planeFds[p] < 0 || desc.strides[p] == 0) {
            cleanupIncomingFds();
            emit vp.dmaBufImportFailed(
                QStringLiteral("Invalid DMA-BUF plane descriptor or zero stride"));
            return false;
        }
    }

    GlContextScope glScope(&vp);
    if (!glScope.isReady() || !QOpenGLContext::currentContext()) {
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(QStringLiteral("OpenGL context is not yet realized"));
        return false;
    }
    resolveEglExtensions();

    if (!m_eglCreateImageKHR || !m_glEGLImageTargetTexture2DOES) {
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(QStringLiteral(
            "Missing EGL DMA-BUF extensions (eglCreateImageKHR / glEGLImageTargetTexture2DOES)"));
        return false;
    }

    std::array<EGLint, 64> attribs{};
    size_t n = 0;
    auto pushAttr = [&](EGLint k, EGLint v) -> bool {
        if (n + 2u >= attribs.size()) {
            return false;
        }
        attribs[n++] = k;
        attribs[n++] = v;
        return true;
    };

    bool attrsOk = pushAttr(EGL_WIDTH, static_cast<EGLint>(desc.width)) &&
                   pushAttr(EGL_HEIGHT, static_cast<EGLint>(desc.height)) &&
                   pushAttr(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(desc.drm_format));

    for (uint32_t p = 0; attrsOk && p < planesToImport; ++p) {
        attrsOk = pushAttr(kPlaneFdKeys[p], planeFds[p]) &&
                  pushAttr(kPlaneOffsetKeys[p], static_cast<EGLint>(desc.offsets[p])) &&
                  pushAttr(kPlanePitchKeys[p], static_cast<EGLint>(desc.strides[p]));
        if (attrsOk && desc.modifier != DRM_FORMAT_MOD_INVALID) {
            attrsOk =
                pushAttr(kPlaneModLoKeys[p], static_cast<EGLint>(desc.modifier & 0xFFFFFFFFULL)) &&
                pushAttr(kPlaneModHiKeys[p],
                         static_cast<EGLint>((desc.modifier >> 32) & 0xFFFFFFFFULL));
        }
    }
    if (!attrsOk || n >= attribs.size()) {
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(QStringLiteral("EGL DMA-BUF attribute list overflow"));
        return false;
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                          nullptr, attribs.data());
    if (img == EGL_NO_IMAGE_KHR) {
        const EGLint err = eglGetError();
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(
            QStringLiteral("eglCreateImageKHR failed with error 0x%1").arg(err, 0, 16));
        return false;
    }

    auto *gl = QOpenGLContext::currentContext()->functions();
    while (gl->glGetError() != GL_NO_ERROR) {
    }
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(img));
    const GLenum targetErr = gl->glGetError();
    if (targetErr != GL_NO_ERROR) {
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        if (tex != 0) {
            gl->glDeleteTextures(1, &tex);
        }
        if (m_eglDestroyImageKHR && m_eglDisplay != EGL_NO_DISPLAY) {
            m_eglDestroyImageKHR(m_eglDisplay, img);
        }
        cleanupIncomingFds();
        emit vp.dmaBufImportFailed(
            QStringLiteral("glEGLImageTargetTexture2DOES failed (GL error 0x%1)")
                .arg(targetErr, 0, 16));
        return false;
    }
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

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
    newFrame.syncFence = EGL_NO_SYNC_KHR;

    for (uint32_t p = 0; p < 4; ++p) {
        if (p < planesToImport) {
            newFrame.planes[p].fd = planeFds[p];
            newFrame.planes[p].stride = desc.strides[p];
            newFrame.planes[p].offset = desc.offsets[p];
        } else {
            newFrame.planes[p].fd = -1;
        }
    }

    for (size_t extra = planesToImport; extra < numFds; ++extra) {
        if (planeFds[extra] >= 0) {
            ::close(planeFds[extra]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_currentFrame.has_value()) {
            m_currentFrame->releaseGl(m_eglDisplay, m_eglDestroyImageKHR, m_eglDestroySyncKHR);
        }
        m_currentFrame = std::move(newFrame);
    }

    vp.m_previewMode = PreviewMode::DmaBufLive;
    vp.m_frameCount++;
    vp.updateAnimationTimerState();
    emit vp.dmaBufFrameRendered(desc.width, desc.height, vp.m_frameCount);
    vp.update();
    return true;
}

bool DmaBufSwapchain::requestFromSocket(GpuPreviewViewport &vp, int socketFd,
                                        const QString &outputName)
{
    if (socketFd < 0) {
        return false;
    }

    waywal_ipc_hdr_t reqHdr{};
    reqHdr.magic = WAYWAL_IPC_MAGIC;
    reqHdr.version = WAYWAL_IPC_VERSION;
    reqHdr.opcode = WAYWAL_REQ_EXPORT_OUTPUT_DMABUF;
    reqHdr.payload_size = sizeof(waywal_export_dmabuf_req_t);

    waywal_export_dmabuf_req_t reqPay{};
    if (!outputName.isEmpty()) {
        const QByteArray utf8 = outputName.toUtf8();
        const size_t n = std::min<size_t>(static_cast<size_t>(utf8.size()),
                                          sizeof(reqPay.output) - 1u);
        std::memcpy(reqPay.output, utf8.constData(), n);
        reqPay.output[n] = '\0';
    }

    if (!waywal_ipc_send_payload_fds(socketFd, &reqHdr, &reqPay, sizeof(reqPay), nullptr, 0)) {
        return false;
    }

    waywal_ipc_hdr_t respHdr{};
    int fds[4] = {-1, -1, -1, -1};
    size_t numFds = 0;
    if (!waywal_ipc_recv_fds(socketFd, &respHdr, fds, 4, &numFds)) {
        return false;
    }
    auto closeAll = [&fds, numFds]() {
        for (size_t i = 0; i < numFds && i < 4u; ++i) {
            if (fds[i] >= 0) {
                ::close(fds[i]);
                fds[i] = -1;
            }
        }
    };

    if (respHdr.magic != WAYWAL_IPC_MAGIC ||
        (respHdr.opcode != WAYWAL_RESP_OK && respHdr.opcode != WAYWAL_RESP_DMABUF) ||
        respHdr.payload_size < sizeof(waywal_export_dmabuf_resp_t) ||
        respHdr.payload_size > 65536u) {
        if (respHdr.payload_size > 0u && respHdr.payload_size <= 65536u) {
            (void)drainFd(socketFd, respHdr.payload_size, 200);
        }
        closeAll();
        return false;
    }

    waywal_export_dmabuf_resp_t respPay{};
    if (!readExactFd(socketFd, &respPay, sizeof(respPay), 500)) {
        closeAll();
        return false;
    }
    if (respHdr.payload_size > sizeof(respPay)) {
        (void)drainFd(socketFd, respHdr.payload_size - sizeof(respPay), 200);
    }

    return importFrame(vp, respPay, fds, numFds);
}

bool DmaBufSwapchain::requestFromNamespace(GpuPreviewViewport &vp, const QString &namespaceStr,
                                           const QString &outputName)
{
    path_buf_t sockPath;
    if (!waywal_ipc_resolve_socket_path(&sockPath, namespaceStr.toUtf8().constData())) {
        return false;
    }
    const int clientFd = waywal_ipc_client_connect(namespaceStr.toUtf8().constData(), 200);
    if (clientFd < 0) {
        return false;
    }
    const bool ok = requestFromSocket(vp, clientFd, outputName);
    ::close(clientFd);
    return ok;
}

void GlSceneRenderer::releaseGl(bool hasCurrentContext) noexcept
{
    QObject::disconnect(m_ctxDestroyConn);
    m_ctxDestroyConn = {};

    auto *ctx = QOpenGLContext::currentContext();
    const bool glAlive = hasCurrentContext && (ctx != nullptr);
    if (glAlive) {
        auto *gl = ctx->functions();
        if (m_inProcessImageTex != 0) {
            gl->glDeleteTextures(1, &m_inProcessImageTex);
        }
        if (m_neutralPassTex != 0) {
            gl->glDeleteTextures(1, &m_neutralPassTex);
        }
        if (m_computeOutputTex != 0) {
            gl->glDeleteTextures(1, &m_computeOutputTex);
        }
        m_quadVao.destroy();
        m_quadVbo.destroy();
        m_gizmoVao.destroy();
        m_gizmoVbo.destroy();
    }

    auto &states = extraRendererStates();
    if (auto it = states.find(this); it != states.end()) {
        if (it->second) {
            it->second->destroyGl(glAlive);
        }
        states.erase(it);
    }

    m_inProcessImageTex = 0;
    m_neutralPassTex = 0;
    m_computeOutputTex = 0;
    m_computeTexWidth = 0;
    m_computeTexHeight = 0;
    m_computeInternalFormat = GL_RGBA16F;
    m_computeBindingUnit = 0u;
    m_gizmoVboCapacityBytes = 0;
    m_bufferPingPongIdx = 0;

    m_blitProgram.reset();
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

void GlSceneRenderer::deleteInProcessImageTexture() noexcept
{
    if (m_inProcessImageTex != 0) {
        if (auto *ctx = QOpenGLContext::currentContext()) {
            ctx->functions()->glDeleteTextures(1, &m_inProcessImageTex);
        }
        m_inProcessImageTex = 0;
    }
}

void GlSceneRenderer::uploadFallbackImageTexture(const QImage &image)
{
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx || image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return;
    }
    auto *gl = ctx->functions();
    GLint maxTexSize = 0;
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    if (maxTexSize > 0 && (image.width() > maxTexSize || image.height() > maxTexSize)) {
        deleteInProcessImageTexture();
        return;
    }

    QImage formatted = (image.format() == QImage::Format_RGBA8888)
                           ? image
                           : image.convertToFormat(QImage::Format_RGBA8888);
    if (formatted.isNull()) {
        return;
    }

    const int expectedRowBytes = formatted.width() * 4;
    int unpackRowLength = 0;
    if (formatted.bytesPerLine() != expectedRowBytes) {
#ifdef GL_UNPACK_ROW_LENGTH
        if (formatted.bytesPerLine() > expectedRowBytes && (formatted.bytesPerLine() % 4) == 0) {
            unpackRowLength = formatted.bytesPerLine() / 4;
        } else {
            formatted = formatted.copy();
        }
#else
        formatted = formatted.copy();
#endif
    }

    while (gl->glGetError() != GL_NO_ERROR) {
    }
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
#endif
    if (m_inProcessImageTex == 0) {
        gl->glGenTextures(1, &m_inProcessImageTex);
    }
    gl->glBindTexture(GL_TEXTURE_2D, m_inProcessImageTex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, formatted.width(), formatted.height(), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, formatted.constBits());
    const GLenum uploadErr = gl->glGetError();
#ifdef GL_UNPACK_ROW_LENGTH
    if (unpackRowLength != 0) {
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
#endif
    if (uploadErr != GL_NO_ERROR) {
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glDeleteTextures(1, &m_inProcessImageTex);
        m_inProcessImageTex = 0;
        return;
    }
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlSceneRenderer::setupQuadGeometry()
{
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *gl = ctx->functions();
    if (!m_quadVao.isCreated()) {
        m_quadVao.create();
    }
    m_quadVao.bind();
    if (!m_quadVbo.isCreated()) {
        m_quadVbo.create();
    }
    m_quadVbo.bind();
    m_quadVbo.allocate(kQuadVertices, sizeof(kQuadVertices));

    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, x)));
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void *>(offsetof(Vertex, u)));
    m_quadVao.release();
    m_quadVbo.release();
}

void GlSceneRenderer::initBlitShader(QObject *parent)
{
    (void)parent;
    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();
    const char *vsSrc = isGles ? kBlitVertexShader : kBlitVertexShaderCore;
    const char *fsSrc = isGles ? kBlitFragmentShader : kBlitFragmentShaderCore;

    m_blitProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vsSrc) ||
        !m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fsSrc) ||
        !m_blitProgram->link()) {
        m_blitProgram = std::make_unique<QOpenGLShaderProgram>();
        if (!m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kBlitVertexShaderCore) ||
            !m_blitProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                    kBlitFragmentShaderCore) ||
            !m_blitProgram->link()) {
            qWarning() << "[GpuPreviewViewport] Failed to compile/link blit shader:"
                       << m_blitProgram->log();
        }
    }
}

void GlSceneRenderer::ensureNeutralPassTexture()
{
    if (m_neutralPassTex != 0) {
        return;
    }
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *gl = ctx->functions();
    static constexpr uint8_t kNeutralPixel[4] = {28, 34, 48, 255};
    gl->glGenTextures(1, &m_neutralPassTex);
    gl->glBindTexture(GL_TEXTURE_2D, m_neutralPassTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kNeutralPixel);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlSceneRenderer::ensureOffscreenFbo(int w, int h)
{
    const int targetW = std::clamp(w, 64, 7680);
    const int targetH = std::clamp(h, 64, 4320);
    if (m_offscreenFbo && m_offscreenFbo->size() == QSize(targetW, targetH)) {
        return;
    }
    QOpenGLFramebufferObjectFormat fboFmt;
    fboFmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFmt.setTextureTarget(GL_TEXTURE_2D);
    fboFmt.setInternalTextureFormat(GL_RGBA16F);
    auto nextFbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
    bool isHalfFloat = true;
    if (!nextFbo->isValid()) {
        fboFmt.setInternalTextureFormat(GL_RGBA8);
        nextFbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
        isHalfFloat = false;
    }
    if (nextFbo->isValid()) {
        zeroFillTexture2D(nextFbo->texture(), targetW, targetH, isHalfFloat);
    }
    m_offscreenFbo = std::move(nextFbo);
}

void GlSceneRenderer::ensureBufferPassFbos(int w, int h)
{
    const int targetW = std::clamp(w, 64, 7680);
    const int targetH = std::clamp(h, 64, 4320);
    auto *ctx = QOpenGLContext::currentContext();
    auto *gl = ctx ? ctx->functions() : nullptr;

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
                fboFmt.setInternalTextureFormat(GL_RGBA16F);
                auto nextFbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
                bool isHalfFloat = true;
                if (!nextFbo->isValid()) {
                    fboFmt.setInternalTextureFormat(GL_RGBA8);
                    nextFbo = std::make_unique<QOpenGLFramebufferObject>(targetW, targetH, fboFmt);
                    isHalfFloat = false;
                }
                if (nextFbo->isValid()) {
                    // Zero-initialize both ping-pong slots before first feedback read.
                    zeroFillTexture2D(nextFbo->texture(), targetW, targetH, isHalfFloat);
                    if (gl && nextFbo->bind()) {
                        gl->glViewport(0, 0, targetW, targetH);
                        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        gl->glClear(GL_COLOR_BUFFER_BIT);
                        nextFbo->release();
                    }
                }
                fbo = std::move(nextFbo);
            }
        }
    }
}

void GlSceneRenderer::ensureGizmoGlInitialized(QObject *parent)
{
    (void)parent;
    if (m_gizmoProgram) {
        return;
    }
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *gl = ctx->functions();
    const bool isGles = ctx->isOpenGLES();
    const char *vsSrc = isGles ? kGizmoVertexShaderEs310 : kGizmoVertexShaderCore;
    const char *fsSrc = isGles ? kGizmoFragmentShaderEs310 : kGizmoFragmentShaderCore;

    m_gizmoProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vsSrc) ||
        !m_gizmoProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fsSrc) ||
        !m_gizmoProgram->link()) {
        m_gizmoProgram = std::make_unique<QOpenGLShaderProgram>();
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
    m_gizmoVboCapacityBytes = 1024u * sizeof(GizmoVertex);
    m_gizmoVbo.allocate(static_cast<int>(m_gizmoVboCapacityBytes));

    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                              reinterpret_cast<void *>(offsetof(GizmoVertex, x)));
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GizmoVertex),
                              reinterpret_cast<void *>(offsetof(GizmoVertex, r)));

    m_gizmoVbo.release();
    m_gizmoVao.release();
}

void GlSceneRenderer::ensureGizmoVboCapacity(size_t requiredBytes)
{
    if (!m_gizmoVbo.isCreated() || requiredBytes <= m_gizmoVboCapacityBytes) {
        return;
    }
    const size_t maxBytes = ViewportGizmoOverlay::kMaxDynamicVertices * sizeof(GizmoVertex);
    const size_t clampedReq = std::min(requiredBytes, maxBytes);
    if (clampedReq <= m_gizmoVboCapacityBytes) {
        return;
    }
    size_t newCap = std::max<size_t>(m_gizmoVboCapacityBytes * 2u, 1024u * sizeof(GizmoVertex));
    while (newCap < clampedReq && newCap < maxBytes) {
        newCap = std::min(newCap * 2u, maxBytes);
    }
    m_gizmoVbo.bind();
    m_gizmoVbo.allocate(static_cast<int>(newCap));
    m_gizmoVboCapacityBytes = newCap;
    m_gizmoVbo.release();
}

bool GlSceneRenderer::compileShader(QObject *parent, const QString &glslSource, QSize fbSize,
                                    QString *outError)
{
    (void)parent;
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
                attempts.push_back({kBlitVertexShader, convertEs310ToEs300(fragSrc)});
                if (fragSrc.contains(QLatin1String("#version 310 es"))) {
                    attempts.push_back({kGraphVertexShaderEs310, fragSrc});
                }
            } else {
                attempts.push_back({kBlitVertexShaderCore, convertEsFragmentToCore330(fragSrc)});
                attempts.push_back({kBlitVertexShader, fragSrc});
            }
        }

        QString lastLog;
        for (const auto &attempt : attempts) {
            auto prog = std::make_unique<QOpenGLShaderProgram>();
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

    if (isComputeShaderSource(trimmed)) {
        auto csProg = std::make_unique<QOpenGLShaderProgram>();
        if (!csProg->addShaderFromSourceCode(QOpenGLShader::Compute, trimmed) || !csProg->link()) {
            if (outError) {
                *outError = csProg->log();
            }
            return false;
        }
        parseComputeMetadata(trimmed, m_computeBindingUnit, m_computeInternalFormat);
        // Reset any previous immutable compute storage so a format change (e.g. rgba16f -> rgba8)
        // allocates fresh glTexStorage2D backing on the next dispatch.
        if (m_computeOutputTex != 0) {
            if (auto *ctx = QOpenGLContext::currentContext()) {
                ctx->functions()->glDeleteTextures(1, &m_computeOutputTex);
            }
            m_computeOutputTex = 0;
        }
        m_computeTexWidth = 0;
        m_computeTexHeight = 0;
        getExtraRendererState(this).programUniforms.clear();
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

    if (trimmed.contains(QLatin1String("#pragma Pass"))) {
        const std::vector<GraphPassSource> passes = parseGraphPasses(trimmed);
        if (passes.empty()) {
            if (outError) {
                *outError = QStringLiteral("No shader passes found after #pragma Pass directive");
            }
            return false;
        }

        std::array<std::unique_ptr<QOpenGLShaderProgram>, 4> nextBufferProgs{};
        std::array<QString, 4> nextBufferSources{};
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
                const size_t idx = static_cast<size_t>(pass.passIndex);
                nextBufferProgs[idx] = std::move(compiled);
                nextBufferSources[idx] = pass.glsl;
            } else {
                nextImageProg = std::move(compiled);
            }
        }

        if (!nextImageProg) {
            // Keep the promoted buffer pass program alive so self-feedback ping-pong continues.
            for (int i = 3; i >= 0; --i) {
                const size_t idx = static_cast<size_t>(i);
                if (nextBufferProgs[idx]) {
                    nextImageProg = compileLinkedFragmentProgram(nextBufferSources[idx],
                                                                 /*graphVaryings=*/true, nullptr);
                    if (!nextImageProg) {
                        nextImageProg = std::move(nextBufferProgs[idx]);
                    }
                    break;
                }
            }
        }

        getExtraRendererState(this).programUniforms.clear();
        m_computeShaderProgram.reset();
        m_bufferPassPrograms = std::move(nextBufferProgs);
        m_shaderProgram = std::move(nextImageProg);
        m_bufferPingPongIdx = 0;
        ensureNeutralPassTexture();
        ensureOffscreenFbo(fbSize.width(), fbSize.height());
        ensureBufferPassFbos(fbSize.width(), fbSize.height());
        auto *ctx = QOpenGLContext::currentContext();
        auto *gl = ctx ? ctx->functions() : nullptr;
        for (size_t i = 0; i < 4; ++i) {
            for (size_t slot = 0; slot < 2; ++slot) {
                auto &fbo = m_bufferPassFbos[i][slot];
                if (m_bufferPassPrograms[i] && fbo && fbo->isValid()) {
                    const bool halfFloat = (fbo->format().internalTextureFormat() == GL_RGBA16F);
                    zeroFillTexture2D(fbo->texture(), fbo->width(), fbo->height(), halfFloat);
                    if (gl && fbo->bind()) {
                        gl->glViewport(0, 0, fbo->width(), fbo->height());
                        gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        gl->glClear(GL_COLOR_BUFFER_BIT);
                        fbo->release();
                    }
                }
            }
        }
        return true;
    }

    QString fullShaderSource;
    const bool usesGraphVarying = trimmed.contains(QLatin1String("v_TexCoord"));
    if (trimmed.contains(QLatin1String("mainImage"))) {
        fullShaderSource = buildShadertoySource(trimmed, isGles);
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

    getExtraRendererState(this).programUniforms.clear();
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
    ensureOffscreenFbo(fbSize.width(), fbSize.height());
    return true;
}

QSize GlSceneRenderer::physicalSizeFor(const QWidget &w, int logicalW, int logicalH) const noexcept
{
    const qreal dpr = w.devicePixelRatioF();
    return QSize(std::max(1, static_cast<int>(std::lround(static_cast<double>(logicalW) * dpr))),
                 std::max(1, static_cast<int>(std::lround(static_cast<double>(logicalH) * dpr))));
}

QMatrix4x4 GlSceneRenderer::computeBlitMvp(const QSize &viewportSize, uint32_t contentW,
                                           uint32_t contentH, float zoom, const QPointF &panOffset,
                                           bool parallaxEnabled, float currentTiltX,
                                           float currentTiltY, float parallaxStrength) const noexcept
{
    const float vW = static_cast<float>(std::max(1, viewportSize.width()));
    const float vH = static_cast<float>(std::max(1, viewportSize.height()));
    const float vAspect = vW / vH;
    const float cAspect =
        static_cast<float>(std::max(1u, contentW)) / static_cast<float>(std::max(1u, contentH));

    QMatrix4x4 mvp;
    if (parallaxEnabled) {
        // Place Z=0 plane at exact frustum distance for 45 deg vertical FOV so scale matches ortho.
        const float dist = 1.0f / std::tan(qDegreesToRadians(22.5f));
        mvp.perspective(45.0f, vAspect, 0.1f, 100.0f);
        mvp.translate(0.0f, 0.0f, -dist);
        mvp.translate(static_cast<float>(panOffset.x()) / (vW * 0.5f) * vAspect,
                      -static_cast<float>(panOffset.y()) / (vH * 0.5f), 0.0f);
        mvp.scale(zoom, zoom, 1.0f);

        const float tiltAngleX = currentTiltX * 14.0f * parallaxStrength;
        const float tiltAngleY = -currentTiltY * 11.0f * parallaxStrength;
        mvp.rotate(tiltAngleX, 0.0f, 1.0f, 0.0f);
        mvp.rotate(tiltAngleY, 1.0f, 0.0f, 0.0f);

        if (cAspect > vAspect) {
            mvp.scale(vAspect, vAspect / cAspect, 1.0f);
        } else {
            mvp.scale(cAspect, 1.0f, 1.0f);
        }
    } else {
        mvp.ortho(-vAspect, vAspect, -1.0f, 1.0f, -1.0f, 1.0f);
        mvp.translate(static_cast<float>(panOffset.x()) / (vW * 0.5f) * vAspect,
                      -static_cast<float>(panOffset.y()) / (vH * 0.5f), 0.0f);
        mvp.scale(zoom, zoom, 1.0f);

        if (cAspect > vAspect) {
            mvp.scale(vAspect, vAspect / cAspect, 1.0f);
        } else {
            mvp.scale(cAspect, 1.0f, 1.0f);
        }
    }
    return mvp;
}

void ViewportInputMapper::updateLayout(const QSize &widgetSize) noexcept
{
    const float w = static_cast<float>(std::max(1, widgetSize.width()));
    const float h = static_cast<float>(std::max(1, widgetSize.height()));
    const float availW = std::max(200.0f, w - 20.0f);
    const float barW = std::min(availW, 640.0f);
    const float barH = 46.0f;
    const float barX = (w - barW) * 0.5f;
    const float barY = std::max(8.0f, h - barH - 12.0f);

    m_layout.barRect = QRectF(barX, barY, barW, barH);
    m_layout.compact = (barW < 520.0f);
    m_layout.ultraCompact = (barW < 390.0f);

    const float padX = 12.0f;
    float gap = m_layout.ultraCompact ? 5.0f : 8.0f;
    const float btnY = barY + 6.0f;
    const float btnH = barH - 12.0f;

    float wStatus = m_layout.ultraCompact ? 72.0f : (m_layout.compact ? 92.0f : 118.0f);
    float wMon = m_layout.ultraCompact ? 68.0f : (m_layout.compact ? 86.0f : 108.0f);
    float wApply = m_layout.ultraCompact ? 72.0f : (m_layout.compact ? 96.0f : 124.0f);
    float wReset = m_layout.ultraCompact ? 42.0f : 48.0f;
    float wGuide = m_layout.ultraCompact ? 52.0f : (m_layout.compact ? 60.0f : 68.0f);
    float wPara = m_layout.ultraCompact ? 46.0f : 52.0f;

    const float rawSum = wStatus + wMon + wApply + wReset + wGuide + wPara + 5.0f * gap + 12.0f;
    const float innerW = std::max(160.0f, barW - 2.0f * padX);
    const float scale = std::min(1.0f, innerW / rawSum);

    wStatus *= scale;
    wMon *= scale;
    wApply *= scale;
    wReset *= scale;
    wGuide *= scale;
    wPara *= scale;
    gap *= scale;

    m_layout.scale = scale;
    m_layout.gap = gap;

    float curX = barX + padX;
    m_layout.statusRect = QRectF(curX, btnY, wStatus, btnH);
    curX += wStatus + 12.0f * scale;

    m_layout.monitorBtnRect = QRectF(curX, btnY, wMon, btnH);
    curX += wMon + gap;

    m_layout.applyBtnRect = QRectF(curX, btnY, wApply, btnH);
    curX += wApply + gap;

    m_layout.resetBtnRect = QRectF(curX, btnY, wReset, btnH);
    curX += wReset + gap;

    m_layout.guideBtnRect = QRectF(curX, btnY, wGuide, btnH);
    curX += wGuide + gap;

    m_layout.parallaxBtnRect = QRectF(curX, btnY, wPara, btnH);
}

ActionBarButton ViewportInputMapper::hitTestActionBar(const QPointF &pos,
                                                      bool showActionBar) const noexcept
{
    if (!showActionBar || !m_layout.barRect.contains(pos)) {
        return ActionBarButton::None;
    }
    if (m_layout.monitorBtnRect.contains(pos)) {
        return ActionBarButton::MonitorSelector;
    }
    if (m_layout.applyBtnRect.contains(pos)) {
        return ActionBarButton::ApplyButton;
    }
    if (m_layout.resetBtnRect.contains(pos)) {
        return ActionBarButton::ResetButton;
    }
    if (m_layout.guideBtnRect.contains(pos)) {
        return ActionBarButton::GuideButton;
    }
    if (m_layout.parallaxBtnRect.contains(pos)) {
        return ActionBarButton::ParallaxButton;
    }
    return ActionBarButton::None;
}

float ViewportInputMapper::calculateGuideAspect(AspectRatioGuide guide,
                                                const QSize &widgetSize) const noexcept
{
    switch (guide) {
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
        return static_cast<float>(std::max(1, widgetSize.width())) /
               std::max(1.0f, static_cast<float>(widgetSize.height()));
    }
}

QRectF ViewportInputMapper::computeContentDisplayRect(const QSize &widgetSize, uint32_t contentW,
                                                      uint32_t contentH, float zoom,
                                                      const QPointF &panOffset) const noexcept
{
    float cW = static_cast<float>(contentW);
    float cH = static_cast<float>(contentH);
    const float vW = static_cast<float>(std::max(1, widgetSize.width()));
    const float vH = static_cast<float>(std::max(1, widgetSize.height()));
    if (cW <= 0.0f || cH <= 0.0f) {
        cW = vW;
        cH = vH;
    }

    const float cAspect = cW / cH;
    const float vAspect = vW / vH;
    QRectF baseRect;
    if (cAspect > vAspect) {
        const float h = vW / cAspect;
        baseRect = QRectF(0.0f, (vH - h) * 0.5f, vW, h);
    } else {
        const float w = vH * cAspect;
        baseRect = QRectF((vW - w) * 0.5f, 0.0f, w, vH);
    }

    const QPointF center = baseRect.center() + panOffset;
    const float scaledW = baseRect.width() * zoom;
    const float scaledH = baseRect.height() * zoom;
    return QRectF(center.x() - scaledW * 0.5f, center.y() - scaledH * 0.5f, scaledW, scaledH);
}

QPointF ViewportInputMapper::mapWidgetToContentTexel(const QPointF &widgetPos,
                                                     const QSize &widgetSize, uint32_t contentW,
                                                     uint32_t contentH, float zoom,
                                                     const QPointF &panOffset,
                                                     bool parallaxEnabled,
                                                     const QMatrix4x4 &blitMvp) const noexcept
{
    const uint32_t cwU = std::max<uint32_t>(1u, contentW);
    const uint32_t chU = std::max<uint32_t>(1u, contentH);
    const double cw = static_cast<double>(cwU);
    const double ch = static_cast<double>(chU);

    if (parallaxEnabled && widgetSize.width() > 0 && widgetSize.height() > 0) {
        const float ndcX =
            static_cast<float>(2.0 * widgetPos.x() / static_cast<double>(widgetSize.width()) - 1.0);
        const float ndcY =
            static_cast<float>(1.0 - 2.0 * widgetPos.y() / static_cast<double>(widgetSize.height()));
        bool invertible = false;
        const QMatrix4x4 invMvp = blitMvp.inverted(&invertible);
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

    const QRectF displayRect =
        computeContentDisplayRect(widgetSize, cwU, chU, zoom, panOffset);
    const double u = (widgetPos.x() - displayRect.left()) / std::max(1.0, displayRect.width());
    const double v = (widgetPos.y() - displayRect.top()) / std::max(1.0, displayRect.height());
    return QPointF(std::clamp(u, 0.0, 1.0) * cw, std::clamp(v, 0.0, 1.0) * ch);
}

bool WeightPaintBrushBridge::isBoneLockedInMask(const std::array<uint64_t, 2> &mask,
                                                uint32_t boneIdx) noexcept
{
    if (boneIdx >= 128u) {
        return false;
    }
    return (mask[boneIdx >> 6u] & (1ull << (boneIdx & 63u))) != 0ull;
}

bool WeightPaintBrushBridge::isBoneLocked(uint32_t boneIdx) const noexcept
{
    return isBoneLockedInMask(m_lockedBoneMask, boneIdx);
}

bool WeightPaintBrushBridge::isVertexLocked(size_t vertIdx) const noexcept
{
    const size_t wordIdx = vertIdx >> 6u;
    if (wordIdx >= m_lockedVertexBits.size()) {
        return false;
    }
    return (m_lockedVertexBits[wordIdx] & (1ull << (vertIdx & 63u))) != 0ull;
}

void WeightPaintBrushBridge::beginStroke()
{
    m_pendingDabs.clear();
    m_preBlocks.clear();
    m_dirtyVertices.clear();
    auto &wbMap = weightBridgeStates();
    if (auto it = wbMap.find(this); it != wbMap.end()) {
        it->second.clearStrokeBits();
    }
}

void WeightPaintBrushBridge::applyDab(ViewportGizmoOverlay &overlay, uint32_t contentW,
                                      uint32_t contentH, const WeightBrushDab &dab)
{
    auto &verts = overlay.mutableMeshWireframe().vertices;
    if (verts.empty() || !std::isfinite(dab.texelX) || !std::isfinite(dab.texelY) ||
        !std::isfinite(dab.radiusPx) || !std::isfinite(dab.strength) ||
        !std::isfinite(dab.targetWeight) || !std::isfinite(dab.pressure) ||
        dab.radiusPx <= 0.0f) {
        return;
    }
    if (isBoneLocked(dab.activeBoneIndex) && dab.op != WE_WEIGHT_OP_NORMALIZE) {
        return;
    }

    const float cw = static_cast<float>(std::max<uint32_t>(1u, contentW));
    const float ch = static_cast<float>(std::max<uint32_t>(1u, contentH));
    const float dabLocalX = dab.texelX - 0.5f * cw;
    const float dabLocalY = 0.5f * ch - dab.texelY;
    const float radiusSq = dab.radiusPx * dab.radiusPx;
    const size_t totalVerts = verts.size();

    WeightBridgeExtraState *ws = nullptr;
    {
        auto &wbMap = weightBridgeStates();
        if (auto it = wbMap.find(this); it != wbMap.end()) {
            ws = &it->second;
            ensureWeightGrid(*ws, verts);
        }
    }

    const size_t dirtyWords = (totalVerts + 63u) >> 6u;
    const size_t blockCount = (totalVerts + 255u) / 256u;
    const size_t blockWords = (blockCount + 63u) >> 6u;
    if (ws) {
        if (ws->dirtyBits.size() < dirtyWords) {
            ws->dirtyBits.resize(dirtyWords, 0ull);
        }
        if (ws->preBlockBits.size() < blockWords) {
            ws->preBlockBits.resize(blockWords, 0ull);
        }
    }

    const uint32_t maxBones =
        std::clamp<uint32_t>(static_cast<uint32_t>(overlay.puppetBones().size()), 1u, 128u);
    auto findUnlockedFallbackBone = [&](uint32_t avoidBone) -> uint32_t {
        if (avoidBone != 0u && !isBoneLocked(0u)) {
            return 0u;
        }
        for (uint32_t b = 0u; b < maxBones; ++b) {
            if (b != avoidBone && !isBoneLocked(b)) {
                return b;
            }
        }
        if (!isBoneLocked(0u)) {
            return 0u;
        }
        for (uint32_t b = 0u; b < 128u; ++b) {
            if (!isBoneLocked(b)) {
                return b;
            }
        }
        return avoidBone;
    };

    const float rawFalloff = (dab.falloff > 0.0f) ? dab.falloff : m_falloff;
    const float falloffExp = std::clamp(std::isfinite(rawFalloff) ? rawFalloff : 1.5f, 0.25f, 4.0f);

    const bool useGrid = (ws && ws->gridValid);
    size_t cx0 = 0;
    size_t cx1 = 0;
    size_t cy0 = 0;
    size_t cy1 = 0;
    if (useGrid) {
        constexpr size_t G = WeightBridgeExtraState::kGridDim;
        const float minDabX = dabLocalX - dab.radiusPx;
        const float maxDabX = dabLocalX + dab.radiusPx;
        const float minDabY = dabLocalY - dab.radiusPx;
        const float maxDabY = dabLocalY + dab.radiusPx;
        if (ws->cellVerts.empty()) {
            return;
        }
        const float fx0 = (minDabX - ws->minX) * ws->invCellW;
        const float fx1 = (maxDabX - ws->minX) * ws->invCellW;
        const float fy0 = (minDabY - ws->minY) * ws->invCellH;
        const float fy1 = (maxDabY - ws->minY) * ws->invCellH;
        if (fx1 < 0.0f || fy1 < 0.0f || fx0 >= static_cast<float>(G) ||
            fy0 >= static_cast<float>(G)) {
            return;
        }
        cx0 = static_cast<size_t>(std::max(0.0f, std::floor(fx0)));
        cx1 = std::min(G - 1u, static_cast<size_t>(std::max(0.0f, std::floor(fx1))));
        cy0 = static_cast<size_t>(std::max(0.0f, std::floor(fy0)));
        cy1 = std::min(G - 1u, static_cast<size_t>(std::max(0.0f, std::floor(fy1))));
    }

    auto forEachSpatialCandidate = [&](auto &&fn) {
        if (useGrid) {
            constexpr size_t G = WeightBridgeExtraState::kGridDim;
            for (size_t cy = cy0; cy <= cy1; ++cy) {
                for (size_t cx = cx0; cx <= cx1; ++cx) {
                    const size_t cell = cy * G + cx;
                    const uint32_t begin = ws->cellOffsets[cell];
                    const uint32_t end = ws->cellOffsets[cell + 1u];
                    for (uint32_t p = begin; p < end; ++p) {
                        fn(static_cast<size_t>(ws->cellVerts[p]));
                    }
                }
            }
        } else {
            for (size_t i = 0; i < totalVerts; ++i) {
                fn(i);
            }
        }
    };

    float neighborAvg = 0.0f;
    float neighborW = 0.0f;
    if (dab.op == WE_WEIGHT_OP_SMOOTH) {
        if (ws) {
            ws->dabCandidates.clear();
        }
        forEachSpatialCandidate([&](size_t i) {
            if (!std::isfinite(verts[i].pos[0]) || !std::isfinite(verts[i].pos[1])) {
                return;
            }
            const float dx = verts[i].pos[0] - dabLocalX;
            const float dy = verts[i].pos[1] - dabLocalY;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= radiusSq) {
                float w = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    if (verts[i].blend_indices[k] == dab.activeBoneIndex &&
                        std::isfinite(verts[i].weight[k])) {
                        w += std::max(0.0f, verts[i].weight[k]);
                    }
                }
                const float d = std::sqrt(d2);
                const float cosFalloff = std::clamp(
                    0.5f * (1.0f + std::cos(static_cast<float>(M_PI) * d / dab.radiusPx)), 0.0f,
                    1.0f);
                const float g = std::pow(cosFalloff, falloffExp);
                neighborAvg += w * g;
                neighborW += g;
                if (ws && !isVertexLocked(i)) {
                    ws->dabCandidates.push_back(static_cast<uint32_t>(i));
                }
            }
        });
        if (neighborW > 1e-6f) {
            neighborAvg /= neighborW;
        }
    }

    auto paintVertex = [&](size_t i) {
        if (!std::isfinite(verts[i].pos[0]) || !std::isfinite(verts[i].pos[1])) {
            return;
        }
        const float dx = verts[i].pos[0] - dabLocalX;
        const float dy = verts[i].pos[1] - dabLocalY;
        const float distSq = dx * dx + dy * dy;
        if (distSq > radiusSq || isVertexLocked(i)) {
            return;
        }

        const uint32_t blockIdx = static_cast<uint32_t>(i / 256u);
        const bool blockCaptured =
            ws ? ((ws->preBlockBits[blockIdx >> 6u] & (1ull << (blockIdx & 63u))) != 0ull)
               : m_preBlocks.contains(blockIdx);
        if (!blockCaptured) {
            if (ws) {
                ws->preBlockBits[blockIdx >> 6u] |= (1ull << (blockIdx & 63u));
            }
            if (!m_preBlocks.contains(blockIdx)) {
                const size_t bStart = static_cast<size_t>(blockIdx) * 256u;
                const size_t bEnd = std::min(bStart + 256u, totalVerts);
                VertexWeightBlockSnapshot snap{};
                snap.blockIndex = blockIdx;
                snap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                                  verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
                m_preBlocks.emplace(blockIdx, std::move(snap));
            }
        }

        const float dist = std::sqrt(distSq);
        const float cosFalloff = std::clamp(
            0.5f * (1.0f + std::cos(static_cast<float>(M_PI) * dist / dab.radiusPx)), 0.0f, 1.0f);
        const float falloff = std::pow(cosFalloff, falloffExp);
        const float alpha = std::clamp(dab.strength * dab.pressure * falloff, 0.0f, 1.0f);

        auto &v = verts[i];
        float lockedSum = 0.0f;
        int targetSlot = -1;
        int weakestUnlockedSlot = -1;
        float minUnlockedW = std::numeric_limits<float>::max();

        for (int k = 0; k < 4; ++k) {
            v.weight[k] = std::isfinite(v.weight[k]) ? std::max(0.0f, v.weight[k]) : 0.0f;
            const bool locked = isBoneLocked(v.blend_indices[k]);
            if (locked) {
                lockedSum += v.weight[k];
            } else if (v.blend_indices[k] == dab.activeBoneIndex && targetSlot < 0) {
                targetSlot = k;
            } else if (v.weight[k] < minUnlockedW) {
                minUnlockedW = v.weight[k];
                weakestUnlockedSlot = k;
            }
        }

        const float targetFree = std::max(0.0f, 1.0f - lockedSum);
        if (targetSlot < 0 && dab.op != WE_WEIGHT_OP_NORMALIZE && dab.op != WE_WEIGHT_OP_SUBTRACT &&
            weakestUnlockedSlot >= 0) {
            targetSlot = weakestUnlockedSlot;
            v.blend_indices[targetSlot] = dab.activeBoneIndex;
            v.weight[targetSlot] = 0.0f;
        }

        int paintedSlot = -1;
        if (targetSlot >= 0 && dab.op != WE_WEIGHT_OP_NORMALIZE) {
            const float curW = v.weight[targetSlot];
            float nextW = curW;
            switch (dab.op) {
            case WE_WEIGHT_OP_ADD:
                nextW = std::clamp(curW + alpha, 0.0f, targetFree);
                break;
            case WE_WEIGHT_OP_SUBTRACT:
                nextW = std::clamp(curW - alpha, 0.0f, targetFree);
                break;
            case WE_WEIGHT_OP_REPLACE:
                nextW = std::clamp(curW + (dab.targetWeight - curW) * alpha, 0.0f, targetFree);
                break;
            case WE_WEIGHT_OP_SMOOTH:
                nextW = std::clamp(curW + (neighborAvg - curW) * alpha, 0.0f, targetFree);
                break;
            case WE_WEIGHT_OP_NORMALIZE:
                break;
            }
            v.weight[targetSlot] = nextW;
            paintedSlot = targetSlot;
        }

        // Preserve exact painted slot weight and rebalance only the remaining unlocked slots.
        float otherFreeSum = 0.0f;
        int firstOtherUnlocked = -1;
        for (int k = 0; k < 4; ++k) {
            if (!isBoneLocked(v.blend_indices[k]) && k != paintedSlot) {
                otherFreeSum += v.weight[k];
                if (firstOtherUnlocked < 0) {
                    firstOtherUnlocked = k;
                }
            }
        }

        if (paintedSlot >= 0) {
            const float remFree = std::max(0.0f, targetFree - v.weight[paintedSlot]);
            if (otherFreeSum > 1e-6f) {
                const float scale = remFree / otherFreeSum;
                for (int k = 0; k < 4; ++k) {
                    if (!isBoneLocked(v.blend_indices[k]) && k != paintedSlot) {
                        v.weight[k] *= scale;
                    }
                }
            } else if (remFree > 1e-6f) {
                if (firstOtherUnlocked >= 0) {
                    const uint32_t fbBone = findUnlockedFallbackBone(dab.activeBoneIndex);
                    if (fbBone != dab.activeBoneIndex) {
                        v.blend_indices[firstOtherUnlocked] = fbBone;
                        v.weight[firstOtherUnlocked] = remFree;
                    } else {
                        v.weight[paintedSlot] = targetFree;
                    }
                } else {
                    v.weight[paintedSlot] = targetFree;
                }
            }
        } else if (otherFreeSum > 1e-6f) {
            const float scale = targetFree / otherFreeSum;
            for (int k = 0; k < 4; ++k) {
                if (!isBoneLocked(v.blend_indices[k])) {
                    v.weight[k] *= scale;
                }
            }
        } else if (targetFree > 1e-6f && firstOtherUnlocked >= 0) {
            v.blend_indices[firstOtherUnlocked] = findUnlockedFallbackBone(UINT32_MAX);
            v.weight[firstOtherUnlocked] = targetFree;
        }

        if (ws) {
            const uint64_t bit = 1ull << (i & 63u);
            if ((ws->dirtyBits[i >> 6u] & bit) == 0ull) {
                ws->dirtyBits[i >> 6u] |= bit;
                m_dirtyVertices.push_back(static_cast<uint32_t>(i));
            }
        } else {
            m_dirtyVertices.push_back(static_cast<uint32_t>(i));
        }
    };

    if (dab.op == WE_WEIGHT_OP_SMOOTH && ws) {
        for (const uint32_t cand : ws->dabCandidates) {
            paintVertex(static_cast<size_t>(cand));
        }
    } else {
        forEachSpatialCandidate(paintVertex);
    }

    if (!ws && m_dirtyVertices.size() >= std::max<size_t>(1024u, totalVerts)) {
        std::sort(m_dirtyVertices.begin(), m_dirtyVertices.end());
        m_dirtyVertices.erase(std::unique(m_dirtyVertices.begin(), m_dirtyVertices.end()),
                              m_dirtyVertices.end());
    }
}

void WeightPaintBrushBridge::normalizeAllWeights(
    int32_t objectId, int32_t expectedObjectId, ViewportGizmoOverlay &overlay,
    const std::array<uint64_t, 2> &lockedBones,
    std::vector<VertexWeightBlockSnapshot> &outPreBlocks,
    std::vector<VertexWeightBlockSnapshot> &outPostBlocks,
    std::vector<waywal_ipc_vertex_weight_t> &outDirtyEntries)
{
    outPreBlocks.clear();
    outPostBlocks.clear();
    outDirtyEntries.clear();
    if (objectId < 0 || expectedObjectId != objectId) {
        return;
    }
    auto &verts = overlay.mutableMeshWireframe().vertices;
    if (verts.empty()) {
        return;
    }

    const uint32_t maxBones =
        std::clamp<uint32_t>(static_cast<uint32_t>(overlay.puppetBones().size()), 1u, 128u);
    auto findFallbackBone = [&]() -> uint32_t {
        if (!isBoneLockedInMask(lockedBones, 0u)) {
            return 0u;
        }
        for (uint32_t b = 0u; b < maxBones; ++b) {
            if (!isBoneLockedInMask(lockedBones, b)) {
                return b;
            }
        }
        for (uint32_t b = 0u; b < 128u; ++b) {
            if (!isBoneLockedInMask(lockedBones, b)) {
                return b;
            }
        }
        return 0u;
    };
    const uint32_t fallbackBone = findFallbackBone();
    const size_t totalVerts = verts.size();

    for (size_t bStart = 0; bStart < totalVerts; bStart += 256u) {
        const size_t bEnd = std::min(bStart + 256u, totalVerts);
        bool blockDirty = false;
        VertexWeightBlockSnapshot preSnap{};
        preSnap.blockIndex = static_cast<uint32_t>(bStart / 256u);

        for (size_t i = bStart; i < bEnd; ++i) {
            if (isVertexLocked(i)) {
                continue;
            }
            auto &v = verts[i];
            float lockedSum = 0.0f;
            float freeSum = 0.0f;
            int firstUnlockedSlot = -1;
            std::array<float, 4> cleanW{};
            std::array<uint32_t, 4> nextIdx{v.blend_indices[0], v.blend_indices[1],
                                            v.blend_indices[2], v.blend_indices[3]};
            for (int k = 0; k < 4; ++k) {
                const float w = std::isfinite(v.weight[k]) ? std::max(0.0f, v.weight[k]) : 0.0f;
                cleanW[k] = w;
                if (isBoneLockedInMask(lockedBones, v.blend_indices[k])) {
                    lockedSum += w;
                } else {
                    freeSum += w;
                    if (firstUnlockedSlot < 0) {
                        firstUnlockedSlot = k;
                    }
                }
            }

            const float targetFree = std::max(0.0f, 1.0f - lockedSum);
            std::array<float, 4> nextW = cleanW;
            if (freeSum > 1e-6f) {
                const float scale = targetFree / freeSum;
                for (int k = 0; k < 4; ++k) {
                    if (!isBoneLockedInMask(lockedBones, v.blend_indices[k])) {
                        nextW[k] = cleanW[k] * scale;
                    }
                }
            } else if (targetFree > 1e-6f && firstUnlockedSlot >= 0) {
                nextIdx[firstUnlockedSlot] = fallbackBone;
                nextW[firstUnlockedSlot] = targetFree;
            }

            bool vertChanged = false;
            for (int k = 0; k < 4; ++k) {
                if (v.blend_indices[k] != nextIdx[k] || !std::isfinite(v.weight[k]) ||
                    std::fabs(v.weight[k] - nextW[k]) > 1e-6f) {
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
                    v.blend_indices[k] = nextIdx[k];
                    v.weight[k] = nextW[k];
                }
                waywal_ipc_vertex_weight_t vw{};
                vw.vertex_index = static_cast<uint32_t>(i);
                std::memcpy(vw.blend_indices, v.blend_indices, sizeof(vw.blend_indices));
                std::memcpy(vw.weight, v.weight, sizeof(vw.weight));
                outDirtyEntries.push_back(vw);
            }
        }

        if (blockDirty) {
            VertexWeightBlockSnapshot postSnap{};
            postSnap.blockIndex = preSnap.blockIndex;
            postSnap.slice.assign(verts.begin() + static_cast<std::ptrdiff_t>(bStart),
                                  verts.begin() + static_cast<std::ptrdiff_t>(bEnd));
            outPreBlocks.push_back(std::move(preSnap));
            outPostBlocks.push_back(std::move(postSnap));
        }
    }
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

    extraViewportStates()[this];
    weightBridgeStates()[&m_weightBridge];
    m_weightBridge.m_pendingDabs.reserve(16u);

    m_lastFpsCalcTime = std::chrono::steady_clock::now();
    m_inputMapper.updateLayout(size());

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
            if (std::abs(m_targetTiltX - m_currentTiltX) <= 1e-4f) {
                m_currentTiltX = m_targetTiltX;
            }
            if (std::abs(m_targetTiltY - m_currentTiltY) <= 1e-4f) {
                m_currentTiltY = m_targetTiltY;
            }
        }
        update();
        updateAnimationTimerState();
    });
    updateAnimationTimerState();
}

void GpuPreviewViewport::releaseGlResources() noexcept
{
    QOpenGLContext *cur = QOpenGLContext::currentContext();
    bool hasCurrent = (cur != nullptr && cur == context());
    if (hasCurrent) {
        m_swapchain.releaseGl();
        m_renderer.releaseGl(true);
        return;
    }

    if (!isOffscreenPlatform() && isValid() && context()) {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            m_swapchain.releaseGl();
            m_renderer.releaseGl(true);
            return;
        }
    }

    // Context is already gone or headless; close kernel DMA-BUF FDs without calling GL/EGL.
    m_swapchain.closeFds();
    m_renderer.releaseGl(false);
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

    // Clear active input states without emitting signals during widget teardown.
    m_isPanning = false;
    m_panButton = Qt::NoButton;
    m_brushStrokeActive = false;
    m_rigDragKind = RigDragKind::None;
    m_rigDragIndex = -1;
    m_gizmoOverlay.endDrag();

    releaseGlResources();
    weightBridgeStates().erase(&m_weightBridge);
    extraViewportStates().erase(this);
}

void GpuPreviewViewport::updateAnimationTimerState()
{
    if (!m_animationTimer) {
        return;
    }
    const bool parallaxSettling =
        m_parallaxEnabled && (std::abs(m_targetTiltX - m_currentTiltX) > 1e-4f ||
                              std::abs(m_targetTiltY - m_currentTiltY) > 1e-4f);
    // DmaBufLive repaints are driven by importFrame() arrival; do not spin at 144 Hz while static.
    const bool needsTick = isVisible() && !m_animationPaused &&
                           (m_previewMode == PreviewMode::InProcessShader || parallaxSettling);

    if (needsTick && !m_animationTimer->isActive()) {
        m_animationTimer->start();
    } else if (!needsTick && m_animationTimer->isActive()) {
        m_animationTimer->stop();
    }
}

void GpuPreviewViewport::setTargetAnimationFps(int fps)
{
    m_targetFps = std::clamp(fps, 10, 360);
    int intervalMs = std::max(1, (1000 + m_targetFps / 2) / m_targetFps);
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
    if (m_previewMode == PreviewMode::DmaBufLive && m_swapchain.hasLiveFrame()) {
        return m_swapchain.frameWidth();
    }
    if (m_previewMode == PreviewMode::InProcessImage) {
        return m_imageWidth;
    }
    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_renderer.m_computeShaderProgram && m_renderer.m_computeTexWidth > 0) {
            return static_cast<uint32_t>(m_renderer.m_computeTexWidth);
        }
        if (m_renderer.m_offscreenFbo) {
            return static_cast<uint32_t>(m_renderer.m_offscreenFbo->width());
        }
    }
    return static_cast<uint32_t>(width());
}

uint32_t GpuPreviewViewport::contentHeight() const noexcept
{
    if (m_previewMode == PreviewMode::DmaBufLive && m_swapchain.hasLiveFrame()) {
        return m_swapchain.frameHeight();
    }
    if (m_previewMode == PreviewMode::InProcessImage) {
        return m_imageHeight;
    }
    if (m_previewMode == PreviewMode::InProcessShader) {
        if (m_renderer.m_computeShaderProgram && m_renderer.m_computeTexHeight > 0) {
            return static_cast<uint32_t>(m_renderer.m_computeTexHeight);
        }
        if (m_renderer.m_offscreenFbo) {
            return static_cast<uint32_t>(m_renderer.m_offscreenFbo->height());
        }
    }
    return static_cast<uint32_t>(height());
}

bool GpuPreviewViewport::hasLiveDmaBuf() const noexcept
{
    return m_previewMode == PreviewMode::DmaBufLive && m_swapchain.hasLiveFrame();
}

void GpuPreviewViewport::clearLiveDmaBuf()
{
    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            m_swapchain.releaseGl();
        } else {
            m_swapchain.closeFds();
        }
    }
    const bool hasInProcessImg = m_renderer.m_inProcessImageTex != 0 || !m_fallbackImage.isNull() ||
                                 (m_imageWidth > 0 && m_imageHeight > 0 &&
                                  !extraViewportStates()[this].sourceImagePath.isEmpty());
    if (hasInProcessImg) {
        m_previewMode = PreviewMode::InProcessImage;
    } else if (m_renderer.m_shaderProgram || m_renderer.m_computeShaderProgram ||
               !m_shaderSource.isEmpty()) {
        m_previewMode = PreviewMode::InProcessShader;
    } else {
        m_previewMode = PreviewMode::OfflineEmpty;
    }
    updateAnimationTimerState();
    update();
}

void GpuPreviewViewport::resolveEglExtensions()
{
    m_swapchain.resolveEglExtensions();
}

void GpuPreviewViewport::setupQuadGeometry()
{
    m_renderer.setupQuadGeometry();
}

void GpuPreviewViewport::initBlitShader()
{
    m_renderer.initBlitShader(this);
}

void GpuPreviewViewport::ensureNeutralPassTexture()
{
    m_renderer.ensureNeutralPassTexture();
}

void GpuPreviewViewport::initializeGL()
{
    if (isOffscreenPlatform()) {
        return;
    }
    initializeOpenGLFunctions();

    QObject::disconnect(m_renderer.m_ctxDestroyConn);
    m_renderer.m_ctxDestroyConn = {};
    if (QOpenGLContext *ctx = context()) {
        m_renderer.m_ctxDestroyConn = connect(
            ctx, &QOpenGLContext::aboutToBeDestroyed, this, [this]() { releaseGlResources(); },
            Qt::DirectConnection);
    }

    resolveEglExtensions();
    setupQuadGeometry();
    initBlitShader();
    ensureNeutralPassTexture();

    if (m_renderer.m_inProcessImageTex == 0) {
        if (!m_fallbackImage.isNull()) {
            m_renderer.uploadFallbackImageTexture(m_fallbackImage);
            if (m_renderer.m_inProcessImageTex != 0) {
                m_fallbackImage = QImage();
            }
        } else if (m_previewMode == PreviewMode::InProcessImage) {
            const QString &srcPath = extraViewportStates()[this].sourceImagePath;
            if (!srcPath.isEmpty()) {
                const QImage restored = decodeStillImageFile(srcPath);
                if (!restored.isNull()) {
                    m_renderer.uploadFallbackImageTexture(restored);
                }
            }
        }
    }

    if (!m_shaderSource.isEmpty() && !m_renderer.m_shaderProgram &&
        !m_renderer.m_computeShaderProgram) {
        (void)compileShaderInternal(m_shaderSource, nullptr);
    }

    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

QSize GpuPreviewViewport::physicalSizeFor(int logicalW, int logicalH) const noexcept
{
    return m_renderer.physicalSizeFor(*this, logicalW, logicalH);
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
    const QSize curSize = size();
    m_inputMapper.updateLayout(curSize);
    extraViewportStates()[this].lastLayoutSize = curSize;
    if (isOffscreenPlatform()) {
        QWidget::resizeEvent(event);
        return;
    }
    QOpenGLWidget::resizeEvent(event);
}

void GpuPreviewViewport::ensureOffscreenFbo(int w, int h)
{
    m_renderer.ensureOffscreenFbo(w, h);
}

void GpuPreviewViewport::ensureBufferPassFbos(int w, int h)
{
    m_renderer.ensureBufferPassFbos(w, h);
}

bool GpuPreviewViewport::importDmaBufFrame(const waywal_dmabuf_frame_desc_t &desc,
                                           const int *plane_fds, size_t num_fds)
{
    return m_swapchain.importFrame(*this, desc, plane_fds, num_fds);
}

bool GpuPreviewViewport::requestLiveDmaBuf(int socketFd, const QString &outputName)
{
    return m_swapchain.requestFromSocket(*this, socketFd, outputName);
}

bool GpuPreviewViewport::requestLiveDmaBufFromNamespace(const QString &namespaceStr,
                                                        const QString &outputName)
{
    return m_swapchain.requestFromNamespace(*this, namespaceStr, outputName);
}

void GpuPreviewViewport::setPreviewImage(const QImage &image)
{
    if (image.isNull()) {
        clearPreviewImage();
        return;
    }

    int validatedStride = 0;
    if (image.width() <= 0 || image.height() <= 0 ||
        !validateRgbaImageBounds(static_cast<uint32_t>(image.width()),
                                 static_cast<uint32_t>(image.height()), validatedStride)) {
        clearPreviewImage();
        return;
    }

    m_videoLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
    extraViewportStates()[this].sourceImagePath.clear();
    m_imageWidth = static_cast<uint32_t>(image.width());
    m_imageHeight = static_cast<uint32_t>(image.height());

    bool uploaded = false;
    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            m_renderer.uploadFallbackImageTexture(image);
            uploaded = (m_renderer.m_inProcessImageTex != 0);
        }
    }
    // Release CPU shadow residency once GPU upload succeeds to avoid 33-132 MB duplicate RAM.
    m_fallbackImage = uploaded ? QImage() : image;

    m_previewMode = PreviewMode::InProcessImage;
    updateAnimationTimerState();
    update();
    emit previewImageChanged(image);
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

    // 2. Try standard Qt QImage reader, libwaywal_core decoders, or synthetic sample fallback.
    QImage img = decodeStillImageFile(targetPath);
    QString persistentSourcePath = targetPath;

    // 4. Video wallpaper preview frame extraction via canonical cache resolver + async ffmpeg
    if (img.isNull() && QFileInfo::exists(targetPath) &&
        (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov")) {
        char thumbPathBuf[PATH_MAX];
        if (waywal_thumb_resolve_cache_path(stdPath.c_str(), thumbPathBuf, sizeof(thumbPathBuf))) {
            std::error_code ec;
            if (std::filesystem::exists(thumbPathBuf, ec)) {
                const QString thumbQPath = QString::fromUtf8(thumbPathBuf);
                if (img.load(thumbQPath)) {
                    persistentSourcePath = thumbQPath;
                }
            }
        }

        if (img.isNull()) {
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
                struct VideoProcCancelState {
                    std::mutex mu;
                    qint64 pid{0};
                    bool finished{false};
                };
                auto cancelState = std::make_shared<VideoProcCancelState>();
                // Register stop_callback so viewport teardown terminates blocking ffmpeg
                // immediately via SIGKILL instead of waiting for the full timeout.
                std::stop_callback stopCb(st, [cancelState]() noexcept {
                    std::lock_guard<std::mutex> lock(cancelState->mu);
                    if (!cancelState->finished && cancelState->pid > 0) {
                        ::kill(static_cast<pid_t>(cancelState->pid), SIGKILL);
                    }
                });

                QProcess proc;
                proc.start(QStringLiteral("ffmpeg"),
                           {QStringLiteral("-loglevel"), QStringLiteral("error"),
                            QStringLiteral("-ss"), QStringLiteral("00:00:00.50"),
                            QStringLiteral("-i"), targetPath, QStringLiteral("-frames:v"),
                            QStringLiteral("1"), QStringLiteral("-vf"),
                            QStringLiteral("scale=1280:-1"), QStringLiteral("-f"),
                            QStringLiteral("image2pipe"), QStringLiteral("-vcodec"),
                            QStringLiteral("bmp"), QStringLiteral("-")});
                if (!proc.waitForStarted(500)) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(cancelState->mu);
                    cancelState->pid = proc.processId();
                    if (st.stop_requested() && cancelState->pid > 0) {
                        ::kill(static_cast<pid_t>(cancelState->pid), SIGKILL);
                    }
                }
                bool exited = false;
                for (int i = 0; i < 50; ++i) {
                    if (st.stop_requested()) {
                        break;
                    }
                    if (proc.waitForFinished(50)) {
                        exited = true;
                        break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(cancelState->mu);
                    cancelState->finished = true;
                    cancelState->pid = 0;
                }
                if (!exited || st.stop_requested()) {
                    if (proc.state() != QProcess::NotRunning) {
                        proc.kill();
                        proc.waitForFinished(250);
                    }
                    return;
                }
                if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
                    return;
                }
                const QByteArray data = proc.readAllStandardOutput();
                QImage decoded;
                if (!data.isEmpty()) {
                    decoded.loadFromData(data, "BMP");
                }
                if (decoded.isNull() || !safeSelf || !qApp || st.stop_requested()) {
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

    if (img.isNull()) {
        return false;
    }
    setPreviewImage(img);
    extraViewportStates()[this].sourceImagePath = std::move(persistentSourcePath);
    return true;
}

void GpuPreviewViewport::clearPreviewImage()
{
    m_videoLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
    extraViewportStates()[this].sourceImagePath.clear();
    m_fallbackImage = QImage();
    {
        GlContextScope glScope(this);
        if (glScope.isReady()) {
            m_renderer.deleteInProcessImageTexture();
        } else {
            m_renderer.m_inProcessImageTex = 0;
        }
    }
    m_imageWidth = 0;
    m_imageHeight = 0;

    if (m_previewMode == PreviewMode::InProcessImage) {
        if (m_swapchain.hasLiveFrame()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (m_renderer.m_shaderProgram || m_renderer.m_computeShaderProgram ||
                   !m_shaderSource.isEmpty()) {
            m_previewMode = PreviewMode::InProcessShader;
        } else {
            m_previewMode = PreviewMode::OfflineEmpty;
        }
    }
    updateAnimationTimerState();
    update();
    emit previewImageChanged(QImage());
}

bool GpuPreviewViewport::compileShaderInternal(const QString &glslSource, QString *outError)
{
    return m_renderer.compileShader(this, glslSource, physicalFramebufferSize(), outError);
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
        if (glScope.isReady() && m_renderer.m_computeOutputTex != 0) {
            glDeleteTextures(1, &m_renderer.m_computeOutputTex);
        }
        m_renderer.m_computeOutputTex = 0;
        m_renderer.m_computeTexWidth = 0;
        m_renderer.m_computeTexHeight = 0;
        m_renderer.m_computeInternalFormat = GL_RGBA16F;
        m_renderer.m_computeBindingUnit = 0u;
        m_renderer.m_shaderProgram.reset();
        m_renderer.m_computeShaderProgram.reset();
        for (auto &bp : m_renderer.m_bufferPassPrograms) {
            bp.reset();
        }
        for (auto &pair : m_renderer.m_bufferPassFbos) {
            pair[0].reset();
            pair[1].reset();
        }
        m_renderer.m_bufferPingPongIdx = 0;
    }
    m_shaderSource.clear();

    if (m_previewMode == PreviewMode::InProcessShader) {
        const bool hasInProcessImg = m_renderer.m_inProcessImageTex != 0 ||
                                     !m_fallbackImage.isNull() ||
                                     (m_imageWidth > 0 && m_imageHeight > 0 &&
                                      !extraViewportStates()[this].sourceImagePath.isEmpty());
        if (m_swapchain.hasLiveFrame()) {
            m_previewMode = PreviewMode::DmaBufLive;
        } else if (hasInProcessImg) {
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

    ProgramUniformCache &uc = getExtraRendererState(&m_renderer).programUniforms[&prog];
    uc.ensureResolved(prog);

    // Standard Shadertoy uniforms
    if (uc.iResolution >= 0) {
        prog.setUniformValue(uc.iResolution, QVector3D(safeW, safeH, safeW / safeH));
    }
    if (uc.iTime >= 0) {
        prog.setUniformValue(uc.iTime, timeSec);
    }
    if (uc.iTimeDelta >= 0) {
        prog.setUniformValue(uc.iTimeDelta, timeDeltaSec);
    }
    if (uc.iFrame >= 0) {
        prog.setUniformValue(uc.iFrame, m_shaderFrameIndex);
    }
    if (uc.iMouse >= 0) {
        prog.setUniformValue(uc.iMouse, QVector4D(static_cast<float>(m_mouseGlCoord.x()),
                                                  static_cast<float>(m_mouseGlCoord.y()),
                                                  static_cast<float>(m_mouseClickCoord.x()),
                                                  static_cast<float>(m_mouseClickCoord.y())));
    }
    if (uc.iDate >= 0) {
        const QDateTime dt = QDateTime::currentDateTime();
        const float secOfDay = dt.time().msecsSinceStartOfDay() / 1000.0f;
        prog.setUniformValue(uc.iDate, QVector4D(static_cast<float>(dt.date().year()),
                                                 static_cast<float>(dt.date().month()),
                                                 static_cast<float>(dt.date().day()), secOfDay));
    }

    // Standard wwald / .wwgraph uniforms
    if (uc.uTime >= 0) {
        prog.setUniformValue(uc.uTime, timeSec);
    }
    if (uc.uResolution >= 0) {
        prog.setUniformValue(uc.uResolution, QVector2D(safeW, safeH));
    }
    if (uc.uPointer >= 0) {
        const float ptrX = std::clamp(static_cast<float>(m_mouseGlCoord.x()) /
                                          std::max(1.0f, static_cast<float>(width())),
                                      0.0f, 1.0f);
        const float ptrY = std::clamp(static_cast<float>(m_mouseGlCoord.y()) /
                                          std::max(1.0f, static_cast<float>(height())),
                                      0.0f, 1.0f);
        prog.setUniformValue(uc.uPointer, QVector2D(ptrX, ptrY));
    }

    if (uc.uAudioBands >= 0) {
        static const QVector4D kZeroAudio[4] = {
            QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
            QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
            QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
            QVector4D(0.0f, 0.0f, 0.0f, 0.0f),
        };
        prog.setUniformValueArray(uc.uAudioBands, kZeroAudio, 4);
    }

    ensureNeutralPassTexture();
    const GLuint fallbackSourceTex = (m_renderer.m_inProcessImageTex != 0)
                                         ? m_renderer.m_inProcessImageTex
                                         : m_renderer.m_neutralPassTex;
    const int readSlot = m_renderer.m_bufferPingPongIdx & 1;
    const int writeSlot = 1 - readSlot;
    GLuint passTex[5] = {
        fallbackSourceTex,          m_renderer.m_neutralPassTex, m_renderer.m_neutralPassTex,
        m_renderer.m_neutralPassTex, m_renderer.m_neutralPassTex,
    };
    for (int u = 0; u < 4; ++u) {
        const int slot = (u < activeWritePass) ? writeSlot : readSlot;
        const auto &fbo =
            m_renderer.m_bufferPassFbos[static_cast<size_t>(u)][static_cast<size_t>(slot)];
        if (fbo && fbo->texture() != 0) {
            passTex[u] = fbo->texture();
        }
    }
    for (int unit = 0; unit < 5; ++unit) {
        if (uc.passSamplers[static_cast<size_t>(unit)] >= 0) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, passTex[unit]);
            prog.setUniformValue(uc.passSamplers[static_cast<size_t>(unit)], unit);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    // Custom inspector & graph node constant uniforms
    for (const auto &[name, val] : m_customUniforms) {
        const GLint loc = uc.resolveCustom(prog, name);
        if (loc < 0) {
            continue;
        }
        switch (val.type) {
        case ShaderUniformValue::Type::Float:
            if (!val.fVals.empty())
                prog.setUniformValue(loc, val.fVals[0]);
            break;
        case ShaderUniformValue::Type::Vec2:
            if (val.fVals.size() >= 2)
                prog.setUniformValue(loc, QVector2D(val.fVals[0], val.fVals[1]));
            break;
        case ShaderUniformValue::Type::Vec3:
            if (val.fVals.size() >= 3)
                prog.setUniformValue(loc, QVector3D(val.fVals[0], val.fVals[1], val.fVals[2]));
            break;
        case ShaderUniformValue::Type::Vec4:
            if (val.fVals.size() >= 4)
                prog.setUniformValue(
                    loc, QVector4D(val.fVals[0], val.fVals[1], val.fVals[2], val.fVals[3]));
            break;
        case ShaderUniformValue::Type::Int:
            if (!val.iVals.empty())
                prog.setUniformValue(loc, val.iVals[0]);
            break;
        case ShaderUniformValue::Type::Mat4:
            prog.setUniformValue(loc, val.matVal);
            break;
        }
    }

    if (uc.uMvp >= 0) {
        QMatrix4x4 ident;
        prog.setUniformValue(uc.uMvp, ident);
    }
}

void GpuPreviewViewport::renderInProcessShaderPass()
{
    const qint64 nowNs = m_shaderTimer.nsecsElapsed();
    const float timeSec = static_cast<float>(nowNs) / 1e9f;
    const float timeDeltaSec = (m_lastFrameTimeNs > 0)
                                   ? static_cast<float>(nowNs - m_lastFrameTimeNs) / 1e9f
                                   : (1.0f / static_cast<float>(std::max(1, m_targetFps)));
    m_lastFrameTimeNs = nowNs;

    if (m_renderer.m_computeShaderProgram && QOpenGLContext::currentContext()) {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        ExtraRendererState &extra = getExtraRendererState(&m_renderer);
        if (!extra.computeProcsResolved) {
            extra.texStorage2D =
                reinterpret_cast<GlTexStorage2DProc>(ctx->getProcAddress("glTexStorage2D"));
            extra.bindImageTex =
                reinterpret_cast<GlBindImageTextureProc>(ctx->getProcAddress("glBindImageTexture"));
            extra.dispatchCompute =
                reinterpret_cast<GlDispatchComputeProc>(ctx->getProcAddress("glDispatchCompute"));
            extra.memoryBarrier =
                reinterpret_cast<GlMemoryBarrierProc>(ctx->getProcAddress("glMemoryBarrier"));
            extra.computeProcsResolved = true;
        }

        const QSize fbSize = physicalFramebufferSize();
        const int targetW = std::clamp(fbSize.width(), 64, 7680);
        const int targetH = std::clamp(fbSize.height(), 64, 4320);
        const GLenum imgFmt = (m_renderer.m_computeInternalFormat != 0)
                                  ? m_renderer.m_computeInternalFormat
                                  : GL_RGBA16F;
        if (extra.texStorage2D && extra.bindImageTex && extra.dispatchCompute &&
            extra.memoryBarrier) {
            if (m_renderer.m_computeOutputTex == 0 || m_renderer.m_computeTexWidth != targetW ||
                m_renderer.m_computeTexHeight != targetH) {
                if (m_renderer.m_computeOutputTex != 0) {
                    glDeleteTextures(1, &m_renderer.m_computeOutputTex);
                    m_renderer.m_computeOutputTex = 0;
                }
                glGenTextures(1, &m_renderer.m_computeOutputTex);
                glBindTexture(GL_TEXTURE_2D, m_renderer.m_computeOutputTex);
                extra.texStorage2D(GL_TEXTURE_2D, 1, imgFmt, targetW, targetH);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (imgFmt == GL_RGBA16F || imgFmt == GL_RGBA8) {
                    zeroFillTexture2D(m_renderer.m_computeOutputTex, targetW, targetH,
                                      imgFmt == GL_RGBA16F);
                }
                m_renderer.m_computeTexWidth = targetW;
                m_renderer.m_computeTexHeight = targetH;
            }

            m_renderer.m_computeShaderProgram->bind();
            applyShaderUniforms(*m_renderer.m_computeShaderProgram, static_cast<float>(targetW),
                                static_cast<float>(targetH), timeSec, timeDeltaSec);
            extra.bindImageTex(m_renderer.m_computeBindingUnit, m_renderer.m_computeOutputTex, 0,
                               GL_FALSE, 0, GL_WRITE_ONLY, imgFmt);
            extra.dispatchCompute(static_cast<GLuint>((targetW + 15) / 16),
                                  static_cast<GLuint>((targetH + 15) / 16), 1u);
            extra.memoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            extra.bindImageTex(m_renderer.m_computeBindingUnit, 0, 0, GL_FALSE, 0, GL_READ_ONLY,
                               imgFmt);
            m_renderer.m_computeShaderProgram->release();
            for (int unit = 4; unit >= 0; --unit) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            m_shaderFrameIndex++;
        }
        return;
    }

    if (!m_renderer.m_shaderProgram) {
        return;
    }

    const QSize fbSize = physicalFramebufferSize();
    ensureOffscreenFbo(fbSize.width(), fbSize.height());
    ensureBufferPassFbos(fbSize.width(), fbSize.height());

    auto drawFullscreenQuad = [this]() {
        m_renderer.m_quadVao.bind();
        glDrawArrays(GL_TRIANGLES, 0, 6);
        m_renderer.m_quadVao.release();
    };

    const int writeSlot = 1 - (m_renderer.m_bufferPingPongIdx & 1);

    // Render BUFFER_A..BUFFER_D into writeSlot while sampling upstream passes from writeSlot and
    // self/downstream feedback passes from readSlot to prevent FBO feedback loops.
    for (size_t i = 0; i < 4; ++i) {
        auto &writeFbo = m_renderer.m_bufferPassFbos[i][static_cast<size_t>(writeSlot)];
        if (!m_renderer.m_bufferPassPrograms[i] || !writeFbo) {
            continue;
        }
        writeFbo->bind();
        const float bw = static_cast<float>(writeFbo->width());
        const float bh = static_cast<float>(writeFbo->height());
        glViewport(0, 0, writeFbo->width(), writeFbo->height());

        m_renderer.m_bufferPassPrograms[i]->bind();
        applyShaderUniforms(*m_renderer.m_bufferPassPrograms[i], bw, bh, timeSec, timeDeltaSec,
                            static_cast<int>(i));
        drawFullscreenQuad();
        m_renderer.m_bufferPassPrograms[i]->release();
        writeFbo->release();
    }

    m_renderer.m_offscreenFbo->bind();
    const float fboW = static_cast<float>(m_renderer.m_offscreenFbo->width());
    const float fboH = static_cast<float>(m_renderer.m_offscreenFbo->height());
    glViewport(0, 0, m_renderer.m_offscreenFbo->width(), m_renderer.m_offscreenFbo->height());
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_renderer.m_shaderProgram->bind();
    applyShaderUniforms(*m_renderer.m_shaderProgram, fboW, fboH, timeSec, timeDeltaSec, 4);
    drawFullscreenQuad();
    m_renderer.m_shaderProgram->release();
    m_renderer.m_offscreenFbo->release();

    m_renderer.m_bufferPingPongIdx ^= 1;

    for (int unit = 4; unit >= 0; --unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_shaderFrameIndex++;
}

float GpuPreviewViewport::calculateGuideAspect() const noexcept
{
    return m_inputMapper.calculateGuideAspect(m_aspectRatioGuide, size());
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

    return m_inputMapper.computeContentDisplayRect(size(), contentWidth(), contentHeight(), m_zoom,
                                                   m_panOffset);
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

    if (m_previewMode == PreviewMode::InProcessImage && m_renderer.m_inProcessImageTex == 0) {
        if (!m_fallbackImage.isNull()) {
            m_renderer.uploadFallbackImageTexture(m_fallbackImage);
            if (m_renderer.m_inProcessImageTex != 0) {
                m_fallbackImage = QImage();
            }
        } else {
            const QString &srcPath = extraViewportStates()[this].sourceImagePath;
            if (!srcPath.isEmpty()) {
                const QImage restored = decodeStillImageFile(srcPath);
                if (!restored.isNull()) {
                    m_renderer.uploadFallbackImageTexture(restored);
                }
            }
        }
    }

    GLuint activeTex = 0;
    int flipY = 0;
    int isBgr = 0;
    const uint32_t cW = contentWidth();
    const uint32_t cH = contentHeight();

    if (m_previewMode == PreviewMode::DmaBufLive) {
        activeTex = m_swapchain.currentTexture(isBgr);
        if (activeTex != 0) {
            flipY = 1; // Wayland DMA-BUF coordinate convention
        }
    } else if (m_previewMode == PreviewMode::InProcessShader) {
        if (!m_renderer.m_shaderProgram && !m_renderer.m_computeShaderProgram &&
            !m_shaderSource.isEmpty()) {
            (void)compileShaderInternal(m_shaderSource, nullptr);
        }
        renderInProcessShaderPass();
        if (m_renderer.m_computeShaderProgram && m_renderer.m_computeOutputTex != 0) {
            activeTex = m_renderer.m_computeOutputTex;
            flipY = 0;
        } else if (m_renderer.m_offscreenFbo) {
            activeTex = m_renderer.m_offscreenFbo->texture();
            flipY = 0; // FBO is already in OpenGL bottom-left origin coordinates
        }
    } else if (m_previewMode == PreviewMode::InProcessImage) {
        activeTex = m_renderer.m_inProcessImageTex;
        flipY = 1; // QImage::constBits() has row 0 at top-left
    }

    const QSize fbSize = physicalFramebufferSize();
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, fbSize.width(), fbSize.height());
    glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (activeTex != 0 && m_renderer.m_blitProgram) {
        m_renderer.m_blitProgram->bind();

        ExtraRendererState &extra = getExtraRendererState(&m_renderer);
        const GLuint blitId = m_renderer.m_blitProgram->programId();
        if (extra.blitProgramId != blitId || blitId == 0u) {
            extra.blitProgramId = blitId;
            extra.blitUMvp = m_renderer.m_blitProgram->uniformLocation("u_mvp");
            extra.blitUTexture = m_renderer.m_blitProgram->uniformLocation("u_texture");
            extra.blitUFlipY = m_renderer.m_blitProgram->uniformLocation("u_flip_y");
            extra.blitUIsBgr = m_renderer.m_blitProgram->uniformLocation("u_is_bgr");
        }

        const QMatrix4x4 mvp = computeBlitMvp(cW, cH);
        if (extra.blitUMvp >= 0) {
            m_renderer.m_blitProgram->setUniformValue(extra.blitUMvp, mvp);
        }
        if (extra.blitUTexture >= 0) {
            m_renderer.m_blitProgram->setUniformValue(extra.blitUTexture, 0);
        }
        if (extra.blitUFlipY >= 0) {
            m_renderer.m_blitProgram->setUniformValue(extra.blitUFlipY, flipY);
        }
        if (extra.blitUIsBgr >= 0) {
            m_renderer.m_blitProgram->setUniformValue(extra.blitUIsBgr, isBgr);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, activeTex);

        m_renderer.m_quadVao.bind();
        glDrawArrays(GL_TRIANGLES, 0, 6);
        m_renderer.m_quadVao.release();

        glBindTexture(GL_TEXTURE_2D, 0);
        m_renderer.m_blitProgram->release();
    }

    renderGizmoOverlayPass(cW, cH);

    // --- Interactive 2D Overlays via QPainter ---
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF displayRect = computeContentDisplayRect();

    if (activeTex == 0 || !m_renderer.m_blitProgram) {
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
        QOpenGLWidget::paintEvent(event);
        return;
    }

    if (width() <= 0 || height() <= 0) {
        return;
    }

    // Paint directly onto the widget backing store without mutating child widgets inside
    // paintEvent, preventing recursive Qt layout/repaint invalidations under offscreen mode.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor(15, 16, 22));

    const QRectF displayRect = computeContentDisplayRect();
    drawFallbackBackdrop(painter, displayRect);

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

    // Outside letterbox/pillarbox dim mask using axis-aligned strips to avoid per-frame
    // QPainterPath boolean tessellation overhead.
    painter.save();
    const QColor dimColor(0, 0, 0, 110);
    if (safeRect.top() > displayRect.top()) {
        painter.fillRect(QRectF(displayRect.left(), displayRect.top(), displayRect.width(),
                                safeRect.top() - displayRect.top()),
                         dimColor);
    }
    if (safeRect.bottom() < displayRect.bottom()) {
        painter.fillRect(QRectF(displayRect.left(), safeRect.bottom(), displayRect.width(),
                                displayRect.bottom() - safeRect.bottom()),
                         dimColor);
    }
    const double midTop = std::max(displayRect.top(), safeRect.top());
    const double midBottom = std::min(displayRect.bottom(), safeRect.bottom());
    const double midH = std::max(0.0, midBottom - midTop);
    if (midH > 0.0) {
        if (safeRect.left() > displayRect.left()) {
            painter.fillRect(
                QRectF(displayRect.left(), midTop, safeRect.left() - displayRect.left(), midH),
                dimColor);
        }
        if (safeRect.right() < displayRect.right()) {
            painter.fillRect(
                QRectF(safeRect.right(), midTop, displayRect.right() - safeRect.right(), midH),
                dimColor);
        }
    }

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
    tagFont.setPixelSize(11);
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
    ExtraViewportState &vpExtra = extraViewportStates()[this];
    const QSize curSize = size();
    if (vpExtra.lastLayoutSize != curSize) {
        m_inputMapper.updateLayout(curSize);
        vpExtra.lastLayoutSize = curSize;
    }
    const auto &lay = m_inputMapper.layout();

    painter.save();

    // Dark glassmorphism pill background
    QPainterPath barPath;
    barPath.addRoundedRect(lay.barRect, 12.0, 12.0);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1.0));
    painter.setBrush(QColor(22, 23, 28, 228));
    painter.drawPath(barPath);
    painter.setClipPath(barPath);

    // 1. Status Indicator & Mode Pill
    QColor dotColor(120, 120, 130);
    QString statusText = lay.ultraCompact ? QStringLiteral("IDLE") : QStringLiteral("OFFLINE");

    if (m_previewMode == PreviewMode::DmaBufLive) {
        dotColor = QColor(16, 185, 129);
        statusText = lay.ultraCompact ? QStringLiteral("LIVE") : QStringLiteral("LIVE DMA-BUF");
    } else if (m_previewMode == PreviewMode::InProcessShader) {
        dotColor = QColor(6, 182, 212);
        statusText = QStringLiteral("SHADER");
    } else if (m_previewMode == PreviewMode::InProcessImage) {
        dotColor = QColor(245, 158, 11);
        statusText = QStringLiteral("IMAGE");
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(dotColor);
    painter.drawEllipse(QPointF(lay.statusRect.left() + 6.0, lay.statusRect.center().y()), 4.0, 4.0);

    QFont statusFont = painter.font();
    statusFont.setPixelSize(11);
    statusFont.setBold(true);
    painter.setFont(statusFont);
    QFontMetrics fm(statusFont);
    painter.setPen(QColor(230, 230, 235));
    QRectF statusTextRect(lay.statusRect.left() + 14.0, lay.statusRect.top(),
                          std::max(10.0, lay.statusRect.width() - 16.0), lay.statusRect.height());
    painter.drawText(
        statusTextRect, Qt::AlignVCenter | Qt::AlignLeft,
        fm.elidedText(statusText, Qt::ElideRight, static_cast<int>(statusTextRect.width())));

    const double divX = lay.statusRect.right() + 6.0 * static_cast<double>(lay.scale);
    painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
    painter.drawLine(QPointF(divX, lay.barRect.top() + 9.0),
                     QPointF(divX, lay.barRect.bottom() - 9.0));

    // 2. Monitor Selection Button
    const bool monitorHovered = (m_hoveredButton == ActionBarButton::MonitorSelector);
    painter.setPen(
        QPen(monitorHovered ? QColor(56, 189, 248, 140) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(monitorHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210));
    painter.drawRoundedRect(lay.monitorBtnRect, 5.0, 5.0);

    painter.setPen(QColor(220, 225, 235));
    painter.drawText(
        lay.monitorBtnRect, Qt::AlignCenter,
        fm.elidedText(m_selectedMonitor, Qt::ElideRight,
                      std::max(8, static_cast<int>(lay.monitorBtnRect.width() - 8.0))));

    // 3. "Apply to Selected Monitor(s)" Action Button
    const bool applyHovered = (m_hoveredButton == ActionBarButton::ApplyButton);
    painter.setPen(Qt::NoPen);
    painter.setBrush(applyHovered ? QColor(3, 105, 161) : QColor(2, 132, 199));
    painter.drawRoundedRect(lay.applyBtnRect, 5.0, 5.0);

    painter.setPen(Qt::white);
    QString applyText =
        lay.compact ? QStringLiteral("Apply") : QStringLiteral("Apply to %1").arg(m_selectedMonitor);
    painter.drawText(
        lay.applyBtnRect, Qt::AlignCenter,
        fm.elidedText(applyText, Qt::ElideRight,
                      std::max(8, static_cast<int>(lay.applyBtnRect.width() - 8.0))));

    // 4. Quick Action Buttons: Reset, Guide, 2.5D Parallax
    const bool resetHovered = (m_hoveredButton == ActionBarButton::ResetButton);
    painter.setPen(
        QPen(resetHovered ? QColor(255, 255, 255, 100) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(resetHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210));
    painter.drawRoundedRect(lay.resetBtnRect, 5.0, 5.0);
    painter.setPen(QColor(210, 215, 225));
    painter.drawText(lay.resetBtnRect, Qt::AlignCenter, QStringLiteral("1:1"));

    const bool guideHovered = (m_hoveredButton == ActionBarButton::GuideButton);
    const bool guideActive = (m_showSafeGuides && m_aspectRatioGuide != AspectRatioGuide::None);
    painter.setPen(QPen(guideActive ? QColor(56, 189, 248, 180) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(guideActive
                         ? QColor(7, 89, 133, 200)
                         : (guideHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210)));
    painter.drawRoundedRect(lay.guideBtnRect, 5.0, 5.0);
    painter.setPen(guideActive ? QColor(56, 189, 248) : QColor(210, 215, 225));
    painter.drawText(lay.guideBtnRect, Qt::AlignCenter,
                     lay.ultraCompact ? QStringLiteral("Grid") : QStringLiteral("Guides"));

    const bool parallaxHovered = (m_hoveredButton == ActionBarButton::ParallaxButton);
    painter.setPen(
        QPen(m_parallaxEnabled ? QColor(168, 85, 247, 200) : QColor(255, 255, 255, 25), 1.0));
    painter.setBrush(m_parallaxEnabled
                         ? QColor(107, 33, 168, 200)
                         : (parallaxHovered ? QColor(42, 48, 61, 230) : QColor(32, 36, 46, 210)));
    painter.drawRoundedRect(lay.parallaxBtnRect, 5.0, 5.0);
    painter.setPen(m_parallaxEnabled ? QColor(233, 213, 255) : QColor(210, 215, 225));
    painter.drawText(lay.parallaxBtnRect, Qt::AlignCenter, QStringLiteral("2.5D"));

    painter.restore();
}

void GpuPreviewViewport::drawStatusOverlay(QPainter &painter)
{
    // Top-left telemetry readout
    painter.save();
    QFont hudFont = painter.font();
    hudFont.setPixelSize(11);
    painter.setFont(hudFont);

    const uint32_t cW = contentWidth();
    const uint32_t cH = contentHeight();
    const int roundedZoomPct = static_cast<int>(std::lround(m_zoom * 100.0f));
    const int roundedFps = static_cast<int>(std::lround(m_currentFps));
    const int widgetW = width();

    ExtraViewportState &vpExtra = extraViewportStates()[this];
    QFontMetrics fm(hudFont);
    if (vpExtra.cachedTelemetryStr.isEmpty() || vpExtra.cachedStatusMode != m_previewMode ||
        vpExtra.cachedStatusCW != cW || vpExtra.cachedStatusCH != cH ||
        vpExtra.cachedStatusZoomPct != roundedZoomPct || vpExtra.cachedStatusFps != roundedFps ||
        vpExtra.cachedStatusWidgetW != widgetW) {
        const QString modeStr =
            (m_previewMode == PreviewMode::DmaBufLive)        ? QStringLiteral("LIVE DMA-BUF")
            : (m_previewMode == PreviewMode::InProcessShader) ? QStringLiteral("SHADER")
            : (m_previewMode == PreviewMode::InProcessImage)  ? QStringLiteral("IMAGE")
                                                              : QStringLiteral("IDLE");
        vpExtra.cachedTelemetryStr = QStringLiteral("%1 | %2x%3 | %4% | %5 FPS")
                                         .arg(modeStr)
                                         .arg(cW)
                                         .arg(cH)
                                         .arg(roundedZoomPct)
                                         .arg(roundedFps);
        const float maxHudW = std::max(80.0f, static_cast<float>(widgetW) - 20.0f);
        vpExtra.cachedHudW = std::min(
            maxHudW, static_cast<float>(fm.horizontalAdvance(vpExtra.cachedTelemetryStr) + 16));
        vpExtra.cachedStatusMode = m_previewMode;
        vpExtra.cachedStatusCW = cW;
        vpExtra.cachedStatusCH = cH;
        vpExtra.cachedStatusZoomPct = roundedZoomPct;
        vpExtra.cachedStatusFps = roundedFps;
        vpExtra.cachedStatusWidgetW = widgetW;
    }

    const QString &telemetryStr = vpExtra.cachedTelemetryStr;
    const float hudW = vpExtra.cachedHudW;
    QRectF hudRect(10.0f, 10.0f, hudW, 22.0f);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 16, 22, 185));
    painter.drawRoundedRect(hudRect, 5.0, 5.0);

    painter.setPen(QColor(180, 185, 195));
    painter.drawText(hudRect.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     fm.elidedText(telemetryStr, Qt::ElideRight, static_cast<int>(hudW - 12.0f)));

    painter.restore();
}

ActionBarButton GpuPreviewViewport::hitTestActionBar(const QPointF &pos) const noexcept
{
    return m_inputMapper.hitTestActionBar(pos, m_showActionBar);
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
        m_panButton = event->button();
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
            if (m_weightBridge.m_pendingDabs.size() >= 8) {
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
    if (m_isPanning && event->button() == m_panButton) {
        m_isPanning = false;
        m_panButton = Qt::NoButton;
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
        if (m_showActionBar && hitTestActionBar(event->position()) != ActionBarButton::None) {
            event->accept();
            return;
        }
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
                if (m_weightBridge.m_pendingDabs.size() >= 8) {
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
    m_hasCameraOrLightsOverlay = (lights.active_light_count > 0u) ||
                                 (cam.projection_mode != WE_PROJ_ORTHOGRAPHIC_2D) ||
                                 (cam.dof_enabled > 0.5f);
    m_hasGizmoEmittersOrLights = m_hasCameraOrLightsOverlay || m_hasEmittersOverlay;
    update();
}

void GpuPreviewViewport::setGizmoEmitterWireframes(std::vector<GizmoEmitterWireframe> emitters)
{
    m_hasEmittersOverlay = !emitters.empty();
    m_hasGizmoEmittersOrLights = m_hasCameraOrLightsOverlay || m_hasEmittersOverlay;
    m_gizmoOverlay.setEmitterWireframes(std::move(emitters));
    update();
}

void GpuPreviewViewport::abortActiveInteractions() noexcept
{
    bool changed = false;
    if (m_isPanning) {
        m_isPanning = false;
        m_panButton = Qt::NoButton;
        changed = true;
    }
    if (m_brushStrokeActive) {
        m_brushStrokeActive = false;
        const QPointF lastWidgetPos(m_mouseGlCoord.x(),
                                    static_cast<double>(height()) - m_mouseGlCoord.y());
        const QPointF texel = mapWidgetToContentTexel(lastWidgetPos);
        const double tsSec =
            m_brushClock.isValid() ? static_cast<double>(m_brushClock.nsecsElapsed()) * 1e-9 : 0.0;
        if (m_gizmoOverlay.toolMode() == ViewportToolMode::WeightBrush) {
            finishWeightPaintStroke();
        }
        emit brushPointerReleased(texel, 0.0f, tsSec);
        changed = true;
    }
    if (m_gizmoOverlay.isDragging()) {
        m_gizmoOverlay.endDrag();
        m_dragCurrentTransform = m_dragStartTransform;
        if (m_activeGizmoNodeId >= 0) {
            emit layerTransformPreviewed(m_activeGizmoNodeId, m_dragStartTransform);
        }
        changed = true;
    }
    if (m_rigDragKind != RigDragKind::None) {
        if (m_rigDragKind == RigDragKind::BoneJointMoveOrReparent ||
            m_rigDragKind == RigDragKind::BonePivotRotate) {
            auto &bones = m_gizmoOverlay.mutablePuppetBones();
            if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < bones.size()) {
                auto &b = bones[static_cast<size_t>(m_rigDragIndex)];
                b.headWorld = m_rigDragInitialBoneHead;
                b.tailWorld = m_rigDragInitialBoneTail;
                b.localAngleRad = m_rigDragInitialBoneAngleRad;
            }
        }
        m_gizmoOverlay.setBoneCreatePreview(false, QVector3D(), QVector3D());
        m_rigDragKind = RigDragKind::None;
        m_rigDragIndex = -1;
        m_rigDragIkL1 = 0.0f;
        m_rigDragIkL2 = 0.0f;
        changed = true;
    }
    if (changed) {
        unsetCursor();
    }
}

void GpuPreviewViewport::setToolMode(ViewportToolMode mode)
{
    if (m_gizmoOverlay.toolMode() == mode) {
        return;
    }
    abortActiveInteractions();
    m_gizmoOverlay.setToolMode(mode);
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
    if (event->key() == Qt::Key_Escape) {
        setToolMode(ViewportToolMode::Navigate);
        event->accept();
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers();
    if ((mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == Qt::NoModifier) {
        switch (event->key()) {
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
    }
    QOpenGLWidget::keyPressEvent(event);
}

void GpuPreviewViewport::focusOutEvent(QFocusEvent *event)
{
    abortActiveInteractions();
    m_gizmoOverlay.setWeightBrushCursor(QPointF(0.0, 0.0), false);
    update();
    QOpenGLWidget::focusOutEvent(event);
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
    const QMatrix4x4 blitMvp = m_parallaxEnabled ? computeBlitMvp(cwU, chU) : QMatrix4x4();
    return m_inputMapper.mapWidgetToContentTexel(widgetPos, size(), cwU, chU, m_zoom, m_panOffset,
                                                 m_parallaxEnabled, blitMvp);
}

QMatrix4x4 GpuPreviewViewport::computeBlitMvp(uint32_t contentW, uint32_t contentH) const noexcept
{
    return m_renderer.computeBlitMvp(size(), contentW, contentH, m_zoom, m_panOffset,
                                     m_parallaxEnabled, m_currentTiltX, m_currentTiltY,
                                     m_parallaxStrength);
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
    m_renderer.ensureGizmoGlInitialized(this);
}

void GpuPreviewViewport::ensureGizmoVboCapacity(size_t requiredBytes)
{
    m_renderer.ensureGizmoVboCapacity(requiredBytes);
}

void GpuPreviewViewport::renderGizmoOverlayPass(uint32_t contentW, uint32_t contentH)
{
    m_gizmoOverlay.setContentSize(contentW, contentH);
    ExtraViewportState &vpExtra = extraViewportStates()[this];
    ExtraRendererState &extra = getExtraRendererState(&m_renderer);

    if (m_gizmoOverlay.toolMode() == ViewportToolMode::Navigate && !m_hasGizmoEmittersOrLights &&
        !m_gizmoOverlay.hasVisibleRiggingOverlay() && m_contourPreviewLoops.empty()) {
        if (m_heatmapVertices.capacity() > 4096u) {
            std::vector<GizmoVertex>().swap(m_heatmapVertices);
        } else {
            m_heatmapVertices.clear();
        }
        if (m_gizmoVertices.capacity() > 8192u) {
            std::vector<GizmoVertex>().swap(m_gizmoVertices);
        } else {
            m_gizmoVertices.clear();
        }
        if (auto *ctx = QOpenGLContext::currentContext()) {
            auto *gl = ctx->functions();
            shrinkBufferIfOversized(gl, extra.heatmapVbo, extra.heatmapVboCapBytes);
            shrinkBufferIfOversized(gl, extra.wireframeVbo, extra.wireframeVboCapBytes);
            shrinkBufferIfOversized(gl, extra.contourVbo, extra.contourVboCapBytes);
            shrinkBufferIfOversized(gl, m_renderer.m_gizmoVbo, m_renderer.m_gizmoVboCapacityBytes);
            shrinkBufferIfOversized(gl, extra.dynVbo2, extra.dynVbo2CapBytes);
        }
        extra.cachedHeatmapVertCount = 0;
        extra.cachedHeatmapGen = 0;
        extra.cachedWireframeVertCount = 0;
        extra.cachedWireframeGen = 0;
        extra.cachedContourVertCount = 0;
        extra.cachedContourGen = 0;
        return;
    }

    const uint32_t effCW = (contentW > 0u) ? contentW : static_cast<uint32_t>(std::max(0, width()));
    const uint32_t effCH =
        (contentH > 0u) ? contentH : static_cast<uint32_t>(std::max(0, height()));
    const QMatrix4x4 viewProj = computeGizmoViewProj(contentW, contentH);
    const QRectF displayRect = computeContentDisplayRect();
    const float orthoZoom = std::max(0.01f, static_cast<float>(displayRect.height()) /
                                                static_cast<float>(std::max(1u, contentH)));
    const we_camera3d_t &cam = m_gizmoOverlay.camera();
    const QVector3D camEye(cam.eye_pos[0], cam.eye_pos[1], cam.eye_pos[2]);
    const float fovYDeg = (cam.fov_y_deg > 1.0f) ? cam.fov_y_deg : 60.0f;

    const auto &mesh = m_gizmoOverlay.meshWireframe();
    const auto &wb = m_gizmoOverlay.weightBrushState();
    const bool wantHeatmap =
        wb.heatmapEnabled && !mesh.vertices.empty() && mesh.indices.size() >= 3u;
    const bool wantWireframe = mesh.visible && !mesh.vertices.empty() && mesh.indices.size() >= 3u;
    const bool wantContour = !m_contourPreviewLoops.empty();

    // Build dynamic overlays (bones, IK, handles, cursor) while hiding mesh wireframe from
    // ViewportGizmoOverlay so it skips per-frame O(T log T) edge sorting; wireframe edges are
    // deduplicated in O(V + T) and cached in wireframeVbo across frames.
    {
        WireframeVisibilityGuard wireGuard(m_gizmoOverlay, mesh.visible);
        m_gizmoOverlay.buildRenderVertices(size(), viewProj, camEye, fovYDeg, orthoZoom,
                                           m_hasCustomSceneViewProj, m_gizmoVertices);
    }
    if (mesh.visible && m_gizmoOverlay.toolMode() != ViewportToolMode::ContourPin &&
        !mesh.pins.empty()) {
        appendContourPinDiamonds(mesh, (effCH > 0u) ? effCH : 1u, m_gizmoVertices);
    }

    if (!wantHeatmap && !wantWireframe && !wantContour && m_gizmoVertices.size() < 2u) {
        return;
    }

    ensureGizmoGlInitialized();
    if (!m_renderer.m_gizmoProgram || !m_renderer.m_gizmoProgram->isLinked() ||
        !m_renderer.m_gizmoVao.isCreated()) {
        return;
    }
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *gl = ctx->functions();

    if (!wantHeatmap) {
        extra.cachedHeatmapVertCount = 0;
        extra.cachedHeatmapGen = 0;
        if (m_heatmapVertices.capacity() > 4096u) {
            std::vector<GizmoVertex>().swap(m_heatmapVertices);
        } else {
            m_heatmapVertices.clear();
        }
        shrinkBufferIfOversized(gl, extra.heatmapVbo, extra.heatmapVboCapBytes);
    } else {
        const bool heatmapDirty =
            (extra.cachedHeatmapGen != vpExtra.heatmapGen || extra.cachedHeatCW != effCW ||
             extra.cachedHeatCH != effCH || extra.cachedHeatBone != wb.activeBoneIndex ||
             extra.cachedHeatOpacity != wb.heatmapOpacity ||
             extra.cachedHeatPalette != wb.palette ||
             extra.cachedHeatVertsPtr != mesh.vertices.data() ||
             extra.cachedHeatVertsCount != mesh.vertices.size() ||
             extra.cachedHeatIndicesPtr != mesh.indices.data() ||
             extra.cachedHeatIndicesCount != mesh.indices.size());
        if (heatmapDirty) {
            buildHeatmapTrianglesFast(mesh, wb.activeBoneIndex, wb.heatmapOpacity, wb.palette,
                                      effCW, effCH, m_heatmapVertices);
            uploadStaticGizmoBuffer(gl, extra.heatmapVao, extra.heatmapVbo,
                                    extra.heatmapVboCapBytes, m_heatmapVertices);
            extra.cachedHeatmapVertCount =
                std::min(m_heatmapVertices.size(), ViewportGizmoOverlay::kMaxDynamicVertices);
            extra.cachedHeatmapGen = vpExtra.heatmapGen;
            extra.cachedHeatCW = effCW;
            extra.cachedHeatCH = effCH;
            extra.cachedHeatBone = wb.activeBoneIndex;
            extra.cachedHeatOpacity = wb.heatmapOpacity;
            extra.cachedHeatPalette = wb.palette;
            extra.cachedHeatVertsPtr = mesh.vertices.data();
            extra.cachedHeatVertsCount = mesh.vertices.size();
            extra.cachedHeatIndicesPtr = mesh.indices.data();
            extra.cachedHeatIndicesCount = mesh.indices.size();
            if (!m_brushStrokeActive && m_heatmapVertices.capacity() > 4096u) {
                std::vector<GizmoVertex>().swap(m_heatmapVertices);
            }
        }
    }

    if (!wantWireframe) {
        extra.cachedWireframeVertCount = 0;
        extra.cachedWireframeGen = 0;
        shrinkBufferIfOversized(gl, extra.wireframeVbo, extra.wireframeVboCapBytes);
    } else {
        const bool wireframeDirty = (extra.cachedWireframeGen != vpExtra.wireframeGen ||
                                     extra.cachedWireCW != effCW || extra.cachedWireCH != effCH ||
                                     extra.cachedWireHighlight != mesh.highlightContourEdges ||
                                     extra.cachedWireVertsPtr != mesh.vertices.data() ||
                                     extra.cachedWireVertsCount != mesh.vertices.size() ||
                                     extra.cachedWireIndicesPtr != mesh.indices.data() ||
                                     extra.cachedWireIndicesCount != mesh.indices.size());
        if (wireframeDirty) {
            std::vector<GizmoVertex> wireVerts;
            buildWireframeLinesLinear(mesh, effCW, effCH, wireVerts);
            uploadStaticGizmoBuffer(gl, extra.wireframeVao, extra.wireframeVbo,
                                    extra.wireframeVboCapBytes, wireVerts);
            extra.cachedWireframeVertCount =
                std::min(wireVerts.size(), ViewportGizmoOverlay::kMaxDynamicVertices);
            extra.cachedWireframeGen = vpExtra.wireframeGen;
            extra.cachedWireCW = effCW;
            extra.cachedWireCH = effCH;
            extra.cachedWireHighlight = mesh.highlightContourEdges;
            extra.cachedWireVertsPtr = mesh.vertices.data();
            extra.cachedWireVertsCount = mesh.vertices.size();
            extra.cachedWireIndicesPtr = mesh.indices.data();
            extra.cachedWireIndicesCount = mesh.indices.size();
        }
    }

    const uint32_t contourCH =
        (contentH > 0u) ? contentH : static_cast<uint32_t>(std::max(1, height()));
    if (!wantContour) {
        extra.cachedContourVertCount = 0;
        extra.cachedContourGen = 0;
        shrinkBufferIfOversized(gl, extra.contourVbo, extra.contourVboCapBytes);
    } else if (extra.cachedContourGen != vpExtra.contourGen || extra.cachedContourCH != contourCH) {
        std::vector<GizmoVertex> contourVerts;
        const double chD = static_cast<double>(std::max<uint32_t>(1u, contourCH));
        for (const auto &loop : m_contourPreviewLoops) {
            if (loop.points.size() < 2u) {
                continue;
            }
            const float r = loop.isHole ? 0.976f : 0.220f;
            const float g = loop.isHole ? 0.886f : 0.741f;
            const float b = loop.isHole ? 0.686f : 0.973f;
            const float a = loop.isHole ? 0.85f : 0.92f;
            for (size_t i = 0; i < loop.points.size(); ++i) {
                if (contourVerts.size() + 2u > ViewportGizmoOverlay::kMaxDynamicVertices) {
                    break;
                }
                const auto &p0 = loop.points[i];
                const auto &p1 = loop.points[(i + 1u) % loop.points.size()];
                if (!std::isfinite(p0.x) || !std::isfinite(p0.y) || !std::isfinite(p1.x) ||
                    !std::isfinite(p1.y)) {
                    continue;
                }
                contourVerts.push_back(GizmoVertex{
                    static_cast<float>(p0.x), static_cast<float>(chD - p0.y), 0.0f, r, g, b, a});
                contourVerts.push_back(GizmoVertex{
                    static_cast<float>(p1.x), static_cast<float>(chD - p1.y), 0.0f, r, g, b, a});
            }
        }
        uploadStaticGizmoBuffer(gl, extra.contourVao, extra.contourVbo, extra.contourVboCapBytes,
                                contourVerts);
        extra.cachedContourVertCount =
            std::min(contourVerts.size(), ViewportGizmoOverlay::kMaxDynamicVertices);
        extra.cachedContourGen = vpExtra.contourGen;
        extra.cachedContourCH = contourCH;
    }

    const size_t maxVerts = ViewportGizmoOverlay::kMaxDynamicVertices;
    const size_t heatCount = extra.cachedHeatmapVertCount;
    const size_t wireCount = extra.cachedWireframeVertCount;
    const size_t contourCount = extra.cachedContourVertCount;
    const size_t lineCount = std::min(m_gizmoVertices.size(), maxVerts);
    if (heatCount < 3u && wireCount < 2u && contourCount < 2u && lineCount < 2u) {
        return;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_renderer.m_gizmoProgram->bind();
    const GLuint gizmoId = m_renderer.m_gizmoProgram->programId();
    if (extra.gizmoProgramId != gizmoId || gizmoId == 0u) {
        extra.gizmoProgramId = gizmoId;
        extra.gizmoUMvp = m_renderer.m_gizmoProgram->uniformLocation("u_mvp");
    }
    if (extra.gizmoUMvp >= 0) {
        m_renderer.m_gizmoProgram->setUniformValue(extra.gizmoUMvp, viewProj);
    }

    if (heatCount >= 3u && extra.heatmapVao.isCreated()) {
        extra.heatmapVao.bind();
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(heatCount));
        extra.heatmapVao.release();
    }
    if (wireCount >= 2u && extra.wireframeVao.isCreated()) {
        extra.wireframeVao.bind();
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(wireCount));
        extra.wireframeVao.release();
    }
    if (contourCount >= 2u && extra.contourVao.isCreated()) {
        extra.contourVao.bind();
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(contourCount));
        extra.contourVao.release();
    }
    if (lineCount >= 2u) {
        const uint8_t slot = extra.dynPingPongIdx & 1u;
        extra.dynPingPongIdx ^= 1u;
        QOpenGLVertexArrayObject &dynVao = (slot == 0u) ? m_renderer.m_gizmoVao : extra.dynVao2;
        QOpenGLBuffer &dynVbo = (slot == 0u) ? m_renderer.m_gizmoVbo : extra.dynVbo2;
        size_t &dynCap = (slot == 0u) ? m_renderer.m_gizmoVboCapacityBytes : extra.dynVbo2CapBytes;

        const size_t maxBytes = maxVerts * sizeof(GizmoVertex);
        const size_t lineBytes = lineCount * sizeof(GizmoVertex);
        if (!dynVao.isCreated() || !dynVbo.isCreated()) {
            setupGizmoVaoAndBuffer(
                gl, dynVao, dynVbo, dynCap,
                std::min(std::max<size_t>(lineBytes, 1024u * sizeof(GizmoVertex)), maxBytes));
        }

        if (dynCap < lineBytes) {
            size_t newCap = std::max<size_t>(dynCap * 2u, 1024u * sizeof(GizmoVertex));
            while (newCap < lineBytes && newCap < maxBytes) {
                newCap = std::min(newCap * 2u, maxBytes);
            }
            dynCap = std::min(newCap, maxBytes);
            extra.dynSmallFrames = 0u;
        } else if (dynCap > 262144u && lineBytes <= 32768u) {
            if (++extra.dynSmallFrames > 16u) {
                dynCap = 65536u;
                extra.dynSmallFrames = 0u;
            }
        } else {
            extra.dynSmallFrames = 0u;
        }

        dynVao.bind();
        dynVbo.bind();
        // Orphan active ping-pong VBO storage before sub-data upload to prevent pipeline stalls.
        gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dynCap), nullptr, GL_STREAM_DRAW);
        gl->glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(lineBytes),
                            m_gizmoVertices.data());
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineCount));
        dynVbo.release();
        dynVao.release();
    }

    if (m_gizmoVertices.capacity() > 8192u && m_gizmoVertices.size() <= 2048u) {
        std::vector<GizmoVertex>().swap(m_gizmoVertices);
    }

    m_renderer.m_gizmoProgram->release();
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
    auto &vpExtra = extraViewportStates()[this];
    ++vpExtra.wireframeGen;
    ++vpExtra.heatmapGen;
    if (auto it = weightBridgeStates().find(&m_weightBridge); it != weightBridgeStates().end()) {
        it->second.invalidateGrid();
    }
    update();
}

void GpuPreviewViewport::setPuppetWireframeVisible(bool visible)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setMeshWireframeVisible(visible);
    ++extraViewportStates()[this].wireframeGen;
    update();
}

void GpuPreviewViewport::setPuppetContourPins(std::vector<we_cdt_pin_t> pins, int32_t selectedPin)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_gizmoOverlay.setMeshPins(std::move(pins), selectedPin);
    update();
}

void GpuPreviewViewport::setPuppetContourPreviewLoops(std::vector<ContourPreviewLoop> loops)
{
    m_contourPreviewLoops = std::move(loops);
    ++extraViewportStates()[this].contourGen;
    update();
}

void GpuPreviewViewport::setPuppetSkeleton(const BoneHierarchySnapshot &rig,
                                           int32_t selectedBoneIndex)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    const auto lockIt = m_lockedVertexBitsByObject.find(rig.objectId);
    if (lockIt != m_lockedVertexBitsByObject.end()) {
        m_weightBridge.m_lockedVertexBits = lockIt->second;
    } else if (m_puppetObjectId != rig.objectId) {
        m_weightBridge.m_lockedVertexBits.clear();
    }
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

void GpuPreviewViewport::remapPuppetBoneIndices(int32_t objectId,
                                                const std::vector<int32_t> &oldToNew)
{
    if (oldToNew.empty()) {
        return;
    }
    if (objectId >= 0 && m_puppetObjectId >= 0 && objectId != m_puppetObjectId) {
        return;
    }

    const size_t mapCount = std::min(oldToNew.size(), static_cast<size_t>(WE_MDL_MAX_BONES));
    int32_t maxNew = -1;
    for (size_t i = 0; i < mapCount; ++i) {
        const int32_t mapped = oldToNew[i];
        if (mapped >= 0 && mapped < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
            maxNew = std::max(maxNew, mapped);
        }
    }
    const size_t newBoneCount = (maxNew >= 0) ? static_cast<size_t>(maxNew + 1) : 0u;
    const bool anySurviving = newBoneCount > 0u;

    const auto &oldBones = m_gizmoOverlay.puppetBones();
    const int32_t oldSelected = m_gizmoOverlay.selectedBoneIndex();
    int32_t nextSelected = -1;
    if (oldSelected >= 0 && static_cast<size_t>(oldSelected) < mapCount) {
        const int32_t mapped = oldToNew[static_cast<size_t>(oldSelected)];
        if (mapped >= 0 && static_cast<size_t>(mapped) < newBoneCount) {
            nextSelected = mapped;
        }
    }

    std::vector<GizmoBoneNode> nextBones(newBoneCount);
    for (size_t i = 0; i < newBoneCount; ++i) {
        nextBones[i].index = static_cast<uint32_t>(i);
    }
    for (size_t oldIdx = 0; oldIdx < mapCount && oldIdx < oldBones.size(); ++oldIdx) {
        const int32_t newIdx = oldToNew[oldIdx];
        if (newIdx < 0 || static_cast<size_t>(newIdx) >= newBoneCount) {
            continue;
        }
        const size_t n = static_cast<size_t>(newIdx);
        nextBones[n] = oldBones[oldIdx];
        nextBones[n].index = static_cast<uint32_t>(n);
        nextBones[n].selected = (newIdx == nextSelected);

        const uint32_t oldParent = oldBones[oldIdx].parentIndex;
        if (oldParent != 0xFFFFFFFFu && oldParent < mapCount) {
            const int32_t mappedParent = oldToNew[oldParent];
            nextBones[n].parentIndex =
                (mappedParent >= 0 && static_cast<size_t>(mappedParent) < newBoneCount &&
                 mappedParent != newIdx)
                    ? static_cast<uint32_t>(mappedParent)
                    : 0xFFFFFFFFu;
        } else {
            nextBones[n].parentIndex = 0xFFFFFFFFu;
        }
    }
    m_gizmoOverlay.setPuppetBones(std::move(nextBones), nextSelected);

    std::vector<GizmoIkHandleData> nextIkHandles;
    for (const auto &ik : m_gizmoOverlay.ikHandles()) {
        auto remapU16 = [&](uint16_t oldB, uint16_t &outB) -> bool {
            if (oldB < mapCount) {
                const int32_t mapped = oldToNew[oldB];
                if (mapped >= 0 && static_cast<size_t>(mapped) < newBoneCount) {
                    outB = static_cast<uint16_t>(mapped);
                    return true;
                }
            }
            return false;
        };
        GizmoIkHandleData updated = ik;
        if (remapU16(ik.chain.root_bone, updated.chain.root_bone) &&
            remapU16(ik.chain.mid_bone, updated.chain.mid_bone) &&
            remapU16(ik.chain.end_bone, updated.chain.end_bone)) {
            nextIkHandles.push_back(updated);
        }
    }
    m_gizmoOverlay.setIkHandles(std::move(nextIkHandles));

    auto &verts = m_gizmoOverlay.mutableMeshWireframe().vertices;
    for (auto &v : verts) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const uint32_t oldB = v.blend_indices[k];
            const float w = std::isfinite(v.weight[k]) ? v.weight[k] : 0.0f;
            if (oldB < mapCount && oldToNew[oldB] >= 0 &&
                static_cast<size_t>(oldToNew[oldB]) < newBoneCount && w > 0.0f) {
                v.blend_indices[k] = static_cast<uint32_t>(oldToNew[oldB]);
                sum += w;
            } else {
                v.blend_indices[k] = 0u;
                v.weight[k] = 0.0f;
            }
        }
        if (sum > 1e-6f) {
            for (int k = 0; k < 4; ++k) {
                v.weight[k] /= sum;
            }
        } else {
            v.blend_indices[0] = 0u;
            v.blend_indices[1] = 0u;
            v.blend_indices[2] = 0u;
            v.blend_indices[3] = 0u;
            v.weight[0] = anySurviving ? 1.0f : 0.0f;
            v.weight[1] = 0.0f;
            v.weight[2] = 0.0f;
            v.weight[3] = 0.0f;
        }
    }

    auto brushState = m_gizmoOverlay.weightBrushState();
    if (brushState.activeBoneIndex < mapCount && oldToNew[brushState.activeBoneIndex] >= 0 &&
        static_cast<size_t>(oldToNew[brushState.activeBoneIndex]) < newBoneCount) {
        brushState.activeBoneIndex = static_cast<uint32_t>(oldToNew[brushState.activeBoneIndex]);
    } else {
        brushState.activeBoneIndex = 0u;
    }
    m_gizmoOverlay.setWeightBrushState(brushState);

    std::array<uint64_t, 2> nextLockedMask{0ull, 0ull};
    const size_t maskLimit = std::min<size_t>(mapCount, 128u);
    for (size_t oldIdx = 0; oldIdx < maskLimit; ++oldIdx) {
        const int32_t newIdx = oldToNew[oldIdx];
        if (newIdx >= 0 && newIdx < 128 &&
            WeightPaintBrushBridge::isBoneLockedInMask(m_weightBridge.m_lockedBoneMask,
                                                       static_cast<uint32_t>(oldIdx))) {
            const size_t n = static_cast<size_t>(newIdx);
            nextLockedMask[n >> 6u] |= (1ull << (n & 63u));
        }
    }
    m_weightBridge.m_lockedBoneMask = nextLockedMask;
    // Discard pre-remap undo snapshots/dabs because their bone indices are stale.
    m_weightBridge.beginStroke();
    ++extraViewportStates()[this].heatmapGen;

    if (m_rigDragKind == RigDragKind::BoneCreate ||
        m_rigDragKind == RigDragKind::BoneJointMoveOrReparent ||
        m_rigDragKind == RigDragKind::BonePivotRotate ||
        m_rigDragKind == RigDragKind::IkTargetMove || m_rigDragKind == RigDragKind::IkPoleMove) {
        m_gizmoOverlay.setBoneCreatePreview(false, QVector3D(), QVector3D());
        m_rigDragKind = RigDragKind::None;
        m_rigDragIndex = -1;
        m_rigDragIkL1 = 0.0f;
        m_rigDragIkL2 = 0.0f;
    }

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
    ++extraViewportStates()[this].heatmapGen;
    update();
}

void GpuPreviewViewport::setWeightBrushActiveBone(uint32_t activeBoneIdx)
{
    auto state = m_gizmoOverlay.weightBrushState();
    if (state.activeBoneIndex == activeBoneIdx) {
        return;
    }
    state.activeBoneIndex = activeBoneIdx;
    m_gizmoOverlay.setWeightBrushState(state);
    ++extraViewportStates()[this].heatmapGen;
    update();
}

void GpuPreviewViewport::setWeightBrushParameters(we_weight_brush_op_t op, float radiusPx,
                                                  float strength, float targetWeight,
                                                  const std::array<uint64_t, 2> &lockedBones,
                                                  float falloff)
{
    m_gizmoOverlay.setContentSize(contentWidth(), contentHeight());
    m_weightBridge.m_op = op;
    m_weightBridge.m_radiusPx = std::max(1.0f, radiusPx);
    m_weightBridge.m_strength = std::clamp(strength, 0.0f, 1.0f);
    m_weightBridge.m_targetValue = std::clamp(targetWeight, 0.0f, 1.0f);
    m_weightBridge.m_lockedBoneMask = lockedBones;
    m_weightBridge.m_falloff = std::clamp(std::isfinite(falloff) ? falloff : 1.5f, 0.25f, 4.0f);

    auto state = m_gizmoOverlay.weightBrushState();
    state.op = m_weightBridge.m_op;
    state.radiusPx = m_weightBridge.m_radiusPx;
    state.strength = m_weightBridge.m_strength;
    m_gizmoOverlay.setWeightBrushState(state);
    update();
}

void GpuPreviewViewport::setWeightLockedVertices(int32_t objectId,
                                                 std::vector<uint64_t> lockedVertexBitset)
{
    if (objectId >= 0) {
        m_lockedVertexBitsByObject[objectId] = lockedVertexBitset;
    }
    if (objectId < 0 || m_puppetObjectId < 0 || objectId == m_puppetObjectId) {
        m_weightBridge.m_lockedVertexBits = std::move(lockedVertexBitset);
    }
}

void GpuPreviewViewport::beginWeightPaintStroke()
{
    m_weightBridge.beginStroke();
}

void GpuPreviewViewport::applyWeightBrushDab(const WeightBrushDab &dab)
{
    WeightBrushDab effectiveDab = dab;
    if (effectiveDab.falloff <= 0.0f || !std::isfinite(effectiveDab.falloff)) {
        effectiveDab.falloff = m_weightBridge.m_falloff;
    }
    m_weightBridge.applyDab(m_gizmoOverlay, contentWidth(), contentHeight(), effectiveDab);
    ++extraViewportStates()[this].heatmapGen;
}

void GpuPreviewViewport::recordWeightBrushDab(const QPointF &texel, float pressure, double tsSec)
{
    WeightBrushDab dab{};
    dab.texelX = static_cast<float>(texel.x());
    dab.texelY = static_cast<float>(texel.y());
    dab.radiusPx = m_weightBridge.m_radiusPx;
    dab.strength = m_weightBridge.m_strength;
    dab.targetWeight = m_weightBridge.m_targetValue;
    dab.pressure = std::clamp(pressure, 0.0f, 1.0f);
    dab.activeBoneIndex = m_gizmoOverlay.weightBrushState().activeBoneIndex;
    dab.op = m_weightBridge.m_op;
    dab.timestampSec = tsSec;
    dab.falloff = m_weightBridge.m_falloff;

    applyWeightBrushDab(dab);
    m_weightBridge.m_pendingDabs.push_back(dab);
}

void GpuPreviewViewport::flushWeightBrushDabBatch()
{
    if (m_weightBridge.m_pendingDabs.empty()) {
        return;
    }
    emit weightBrushDabBatchReady(m_puppetObjectId, m_weightBridge.m_pendingDabs);
    m_weightBridge.m_pendingDabs.clear();
}

void GpuPreviewViewport::finishWeightPaintStroke()
{
    flushWeightBrushDabBatch();
    if (m_weightBridge.m_preBlocks.empty()) {
        m_weightBridge.m_dirtyVertices.clear();
        return;
    }

    const auto &verts = m_gizmoOverlay.meshWireframe().vertices;
    const size_t totalVerts = verts.size();

    std::vector<VertexWeightBlockSnapshot> preBlocks;
    std::vector<VertexWeightBlockSnapshot> postBlocks;
    preBlocks.reserve(m_weightBridge.m_preBlocks.size());
    postBlocks.reserve(m_weightBridge.m_preBlocks.size());
    for (auto &[blockIdx, preSnap] : m_weightBridge.m_preBlocks) {
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

    if (!std::is_sorted(m_weightBridge.m_dirtyVertices.begin(),
                        m_weightBridge.m_dirtyVertices.end())) {
        std::sort(m_weightBridge.m_dirtyVertices.begin(), m_weightBridge.m_dirtyVertices.end());
    }
    m_weightBridge.m_dirtyVertices.erase(
        std::unique(m_weightBridge.m_dirtyVertices.begin(), m_weightBridge.m_dirtyVertices.end()),
        m_weightBridge.m_dirtyVertices.end());

    std::vector<waywal_ipc_vertex_weight_t> dirtyEntries;
    dirtyEntries.reserve(m_weightBridge.m_dirtyVertices.size());
    for (uint32_t vIdx : m_weightBridge.m_dirtyVertices) {
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

    m_weightBridge.m_preBlocks.clear();
    if (m_weightBridge.m_dirtyVertices.capacity() > 4096u) {
        std::vector<uint32_t>().swap(m_weightBridge.m_dirtyVertices);
    } else {
        m_weightBridge.m_dirtyVertices.clear();
    }
    if (auto it = weightBridgeStates().find(&m_weightBridge); it != weightBridgeStates().end()) {
        it->second.clearStrokeBits();
    }
    if (m_heatmapVertices.capacity() > 4096u) {
        std::vector<GizmoVertex>().swap(m_heatmapVertices);
    }
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
        ++extraViewportStates()[this].heatmapGen;
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
        ++extraViewportStates()[this].heatmapGen;
        update();
    }
}

void GpuPreviewViewport::normalizeAllVertexWeights(int32_t objectId,
                                                   const std::array<uint64_t, 2> &lockedBones)
{
    std::vector<VertexWeightBlockSnapshot> preBlocks;
    std::vector<VertexWeightBlockSnapshot> postBlocks;
    std::vector<waywal_ipc_vertex_weight_t> dirtyEntries;
    m_weightBridge.normalizeAllWeights(objectId, m_puppetObjectId, m_gizmoOverlay, lockedBones,
                                       preBlocks, postBlocks, dirtyEntries);
    if (!dirtyEntries.empty()) {
        ++extraViewportStates()[this].heatmapGen;
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
        pin.px = std::clamp(texel.x(), 0.0, static_cast<double>(std::max(1u, cW)));
        pin.py = std::clamp(texel.y(), 0.0, static_cast<double>(std::max(1u, cH)));
        pin.flags = 1u;
        auto &pins = m_gizmoOverlay.mutableMeshWireframe().pins;
        pins.push_back(pin);
        const int32_t newPinIdx = static_cast<int32_t>(pins.size()) - 1;
        m_gizmoOverlay.mutableMeshWireframe().selectedPinIndex = newPinIdx;
        emit contourPinAdded(m_puppetObjectId, pin.px, pin.py);
        emit contourPinSelected(m_puppetObjectId, newPinIdx);
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
        if (hit.handle == GizmoAxisHandle::IkTarget && hit.index >= 0 &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.ikHandles().size()) {
            const auto &ik = m_gizmoOverlay.ikHandles()[static_cast<size_t>(hit.index)];
            m_rigDragKind = RigDragKind::IkTargetMove;
            m_rigDragIndex = hit.index;
            m_rigDragIkL1 = std::max(1.0f, (ik.midWorld - ik.rootWorld).length());
            m_rigDragIkL2 = std::max(1.0f, (ik.endWorld - ik.midWorld).length());
            setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if (hit.handle == GizmoAxisHandle::IkPole && hit.index >= 0 &&
            static_cast<size_t>(hit.index) < m_gizmoOverlay.ikHandles().size()) {
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
    const uint32_t cW = std::max<uint32_t>(1u, contentWidth());
    const uint32_t cH = std::max<uint32_t>(1u, contentHeight());
    const QPointF texel = mapWidgetToContentTexel(widgetPos);
    const QPointF localPt = mapTexelToPuppetLocal(texel);
    const QVector3D curWorld(static_cast<float>(localPt.x()), static_cast<float>(localPt.y()),
                             0.0f);

    switch (m_rigDragKind) {
    case RigDragKind::ContourPinMove: {
        auto &pins = m_gizmoOverlay.mutableMeshWireframe().pins;
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < pins.size()) {
            pins[static_cast<size_t>(m_rigDragIndex)].px =
                std::clamp(texel.x(), 0.0, static_cast<double>(cW));
            pins[static_cast<size_t>(m_rigDragIndex)].py =
                std::clamp(texel.y(), 0.0, static_cast<double>(cH));
        }
        break;
    }
    case RigDragKind::BoneCreate:
        m_gizmoOverlay.setBoneCreatePreview(true, m_rigDragStartWorld, curWorld);
        break;
    case RigDragKind::BoneJointMoveOrReparent: {
        auto &bones = m_gizmoOverlay.mutablePuppetBones();
        if (m_rigDragIndex >= 0 && static_cast<size_t>(m_rigDragIndex) < bones.size()) {
            auto &b = bones[static_cast<size_t>(m_rigDragIndex)];
            if (mods.testFlag(Qt::AltModifier)) {
                b.headWorld = m_rigDragInitialBoneHead;
                b.tailWorld = m_rigDragInitialBoneTail;
            } else {
                const QVector3D delta = curWorld - m_rigDragStartWorld;
                const QVector3D seg = m_rigDragInitialBoneTail - m_rigDragInitialBoneHead;
                b.headWorld = m_rigDragInitialBoneHead + delta;
                b.tailWorld = b.headWorld + seg;
                emit puppetBonePivotChanged(m_puppetObjectId, m_rigDragIndex,
                                            QPointF(b.headWorld.x(), b.headWorld.y()),
                                            b.localAngleRad, false);
            }
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
            // Use segment lengths captured at drag start so 2-bone IK never drifts radially.
            const float l1 = (m_rigDragIkL1 > 0.0f)
                                 ? m_rigDragIkL1
                                 : std::max(1.0f, (ik.midWorld - ik.rootWorld).length());
            const float l2 = (m_rigDragIkL2 > 0.0f)
                                 ? m_rigDragIkL2
                                 : std::max(1.0f, (ik.endWorld - ik.midWorld).length());
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

            auto &bones = m_gizmoOverlay.mutablePuppetBones();
            if (ik.chain.root_bone < bones.size()) {
                bones[ik.chain.root_bone].headWorld = ik.rootWorld;
                bones[ik.chain.root_bone].tailWorld = ik.midWorld;
            }
            if (ik.chain.mid_bone < bones.size()) {
                bones[ik.chain.mid_bone].headWorld = ik.midWorld;
                bones[ik.chain.mid_bone].tailWorld = ik.endWorld;
            }
            if (ik.chain.end_bone < bones.size()) {
                const QVector3D endSeg =
                    bones[ik.chain.end_bone].tailWorld - bones[ik.chain.end_bone].headWorld;
                bones[ik.chain.end_bone].headWorld = ik.endWorld;
                bones[ik.chain.end_bone].tailWorld = ik.endWorld + endSeg;
            }

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
            bool shouldEmit = (dropHit.handle == GizmoAxisHandle::None);
            if ((dropHit.handle == GizmoAxisHandle::BoneJoint ||
                 dropHit.handle == GizmoAxisHandle::BoneBody) &&
                dropHit.index >= 0 && dropHit.index != m_rigDragIndex) {
                const auto &bones = m_gizmoOverlay.puppetBones();
                bool createsCycle = false;
                int32_t probe = dropHit.index;
                // Walk ancestors from proposed parent to forbid reparenting onto a descendant.
                for (size_t step = 0;
                     step < bones.size() && probe >= 0 && static_cast<size_t>(probe) < bones.size();
                     ++step) {
                    if (probe == m_rigDragIndex) {
                        createsCycle = true;
                        break;
                    }
                    const uint32_t nextP = bones[static_cast<size_t>(probe)].parentIndex;
                    probe = (nextP < bones.size()) ? static_cast<int32_t>(nextP) : -1;
                }
                if (!createsCycle) {
                    newParent = dropHit.index;
                    shouldEmit = true;
                }
            }
            if (shouldEmit) {
                emit puppetBoneReparented(m_puppetObjectId, m_rigDragIndex, newParent);
            }
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
    m_rigDragIkL1 = 0.0f;
    m_rigDragIkL2 = 0.0f;
}

} // namespace waywal::studio
