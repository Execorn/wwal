// SPDX-License-Identifier: GPL-3.0-or-later
#include "virtualized_grid_widget.hpp"
#include "etc2_simd_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>

namespace wwal::studio {

namespace {

bool isOffscreenPlatform() { return QGuiApplication::platformName() == QLatin1String("offscreen"); }

#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#endif
#ifndef GL_COMPRESSED_SRGB8_ETC2
#define GL_COMPRESSED_SRGB8_ETC2 0x9275
#endif

QColor badgeColor(WallpaperBadge badge)
{
    switch (badge) {
    case WallpaperBadge::Image:
        return QColor(14, 165, 233);
    case WallpaperBadge::Video:
        return QColor(168, 85, 247);
    case WallpaperBadge::Shader:
        return QColor(16, 185, 129);
    case WallpaperBadge::Scene:
        return QColor(249, 115, 22);
    case WallpaperBadge::GnomeXml:
        return QColor(234, 179, 8);
    case WallpaperBadge::Favorite:
        return QColor(236, 72, 153);
    }
    return QColor(100, 116, 139);
}

const char *badgeText(WallpaperBadge badge)
{
    switch (badge) {
    case WallpaperBadge::Image:
        return "IMAGE";
    case WallpaperBadge::Video:
        return "VIDEO";
    case WallpaperBadge::Shader:
        return "SHADER";
    case WallpaperBadge::Scene:
        return "WE_SCENE";
    case WallpaperBadge::GnomeXml:
        return "XML";
    case WallpaperBadge::Favorite:
        return "FAV";
    }
    return "WALL";
}

} // namespace

// ============================================================================
// GridGlCanvas Implementation
// ============================================================================

void GridGlCanvas::encodeRgbaToEtc2(const uint8_t *rgba, int width, int height,
                                    std::vector<uint8_t> &outBlocks)
{
    if (!rgba || width <= 0 || height <= 0) {
        outBlocks.clear();
        return;
    }
    etc2::encodeRgbaToEtc2Simd(rgba, width, height, width * 4, outBlocks);
}

GridGlCanvas::GridGlCanvas(FuzzyCatalogIndex &index, GpuTexturePool &pool,
                           HoverPreviewController &hover, QWidget *parent)
    : QOpenGLWidget(parent), m_index(index), m_pool(pool), m_hover(hover)
{
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_fallbackCanvas = new QLabel(this);
    m_fallbackCanvas->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_fallbackCanvas->setVisible(false);

    m_hover.setGlOwner(this);

    QPointer<GridGlCanvas> safeSelf(this);
    m_pool.setThumbnailReadyCallback([safeSelf]() {
        if (!safeSelf || !qApp) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [safeSelf]() {
                if (safeSelf) {
                    safeSelf->update();
                }
            },
            Qt::QueuedConnection);
    });

    connect(&m_hover, &HoverPreviewController::previewFrameReady, this,
            [this](uint32_t, GLuint) { update(); });
    connect(&m_hover, &HoverPreviewController::previewStopped, this,
            [this](uint32_t) { update(); });
}

GridGlCanvas::~GridGlCanvas()
{
    m_pool.setThumbnailReadyCallback(nullptr);
    m_hover.setGlOwner(nullptr);
    QObject::disconnect(m_contextDestroyConn);
    if (!isOffscreenPlatform() && isValid()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }
}

void GridGlCanvas::destroyCompressedCache()
{
    for (auto &[id, entry] : m_etc2Cache) {
        if (entry.tex_id != 0) {
            glDeleteTextures(1, &entry.tex_id);
        }
    }
    m_etc2Cache.clear();
    m_etc2Lru.clear();
}

void GridGlCanvas::releaseGlResources()
{
    destroyCompressedCache();
    if (m_pboRing[0] != 0 || m_pboRing[1] != 0) {
        glDeleteBuffers(2, m_pboRing.data());
        m_pboRing = {0, 0};
    }
    m_pboCapacities = {0, 0};
    m_pboIndex = 0;
    m_supportsEtc2 = false;
    m_etc2InternalFormat = 0;
    if (m_cardVbo != 0) {
        glDeleteBuffers(1, &m_cardVbo);
        m_cardVbo = 0;
    }
    if (m_cardVao != 0) {
        glDeleteVertexArrays(1, &m_cardVao);
        m_cardVao = 0;
    }
    if (m_cardProgram != 0) {
        glDeleteProgram(m_cardProgram);
        m_cardProgram = 0;
    }
    if (m_cardArrayProgram != 0) {
        glDeleteProgram(m_cardArrayProgram);
        m_cardArrayProgram = 0;
    }
    m_uViewportLoc = -1;
    m_uCardRectLoc = -1;
    m_uTexLoc = -1;
    m_uArrayViewportLoc = -1;
    m_uArrayCardRectLoc = -1;
    m_uArrayTexLoc = -1;
    m_uArrayLayerLoc = -1;
    m_uArraySrgbLoc = -1;
    m_glRenderedThisFrame.clear();
    m_pool.destroyGl(this);
    m_hover.destroyGl(this);
    m_glInitialized = false;
}

int GridGlCanvas::computeColumns(int container_width) const noexcept
{
    return std::max(1, (container_width + SPACING_X) / (ITEM_WIDTH + SPACING_X));
}

int GridGlCanvas::computeTotalRows(size_t total_items, int columns) const noexcept
{
    if (total_items == 0 || columns <= 0)
        return 0;
    return static_cast<int>((total_items + static_cast<size_t>(columns) - 1) /
                            static_cast<size_t>(columns));
}

int GridGlCanvas::computeVirtualHeight(size_t total_items, int columns) const noexcept
{
    if (total_items == 0 || columns <= 0) {
        return 0;
    }
    int r_total = computeTotalRows(total_items, columns);
    return r_total * ROW_HEIGHT - SPACING_Y;
}

GridVisibleSlice GridGlCanvas::computeVisibleSlice(size_t total_items, int columns, int scroll_y,
                                                   int viewport_h) const noexcept
{
    if (total_items == 0 || columns <= 0) {
        return GridVisibleSlice{
            .is_empty = true, .row_first = 0, .row_last = 0, .index_first = 0, .index_last = 0};
    }

    int r_total = computeTotalRows(total_items, columns);
    int r_first = std::max(0, (scroll_y / ROW_HEIGHT) - OVERSCAN_ROWS);
    int r_last = std::min(r_total - 1,
                          ((scroll_y + viewport_h + ROW_HEIGHT - 1) / ROW_HEIGHT) + OVERSCAN_ROWS);

    size_t idx_first = static_cast<size_t>(columns * r_first);
    size_t idx_last = std::min(total_items - 1, static_cast<size_t>(columns * (r_last + 1) - 1));

    return GridVisibleSlice{.is_empty = false,
                            .row_first = r_first,
                            .row_last = r_last,
                            .index_first = idx_first,
                            .index_last = idx_last};
}

QRect GridGlCanvas::itemRect(size_t filtered_idx, int columns, int scroll_y) const
{
    if (columns <= 0)
        return QRect();

    int r = static_cast<int>(filtered_idx / static_cast<size_t>(columns));
    int c = static_cast<int>(filtered_idx % static_cast<size_t>(columns));

    const int card_w =
        (columns == 1) ? std::min(ITEM_WIDTH, std::max(120, width() - 16)) : ITEM_WIDTH;
    int total_grid_width = columns * card_w + (columns - 1) * SPACING_X;
    int margin_x = std::max(8, (width() - total_grid_width) / 2);

    int x = margin_x + c * (card_w + SPACING_X);
    int y = SPACING_Y + r * ROW_HEIGHT - scroll_y;

    return QRect(x, y, card_w, ITEM_HEIGHT);
}

std::optional<size_t> GridGlCanvas::itemAtPoint(const QPoint &pos) const
{
    size_t total = m_index.filteredCount();
    if (total == 0)
        return std::nullopt;

    int cols = computeColumns(width());
    const int card_w = (cols == 1) ? std::min(ITEM_WIDTH, std::max(120, width() - 16)) : ITEM_WIDTH;
    int total_grid_width = cols * card_w + (cols - 1) * SPACING_X;
    int margin_x = std::max(8, (width() - total_grid_width) / 2);

    int rel_x = pos.x() - margin_x;
    int rel_y = pos.y() + m_scrollY - SPACING_Y;

    if (rel_x < 0 || rel_y < 0)
        return std::nullopt;

    int c = rel_x / (card_w + SPACING_X);
    int r = rel_y / ROW_HEIGHT;

    if (c >= cols)
        return std::nullopt;

    int in_card_x = rel_x % (card_w + SPACING_X);
    int in_card_y = rel_y % ROW_HEIGHT;

    if (in_card_x > card_w || in_card_y > ITEM_HEIGHT) {
        return std::nullopt;
    }

    size_t idx = static_cast<size_t>(r * cols + c);
    if (idx < total) {
        return idx;
    }
    return std::nullopt;
}

void GridGlCanvas::setScrollY(int scroll_y)
{
    int max_scroll = std::max(
        0, computeVirtualHeight(m_index.filteredCount(), computeColumns(width())) - height());
    int clamped = std::clamp(scroll_y, 0, max_scroll);
    if (m_scrollY != clamped) {
        m_scrollY = clamped;
        update();
    }
}

void GridGlCanvas::toggleSelection(uint32_t item_id)
{
    if (m_selectedIds.contains(item_id)) {
        m_selectedIds.erase(item_id);
    } else {
        m_selectedIds.insert(item_id);
    }
    m_lastSelectedId = item_id;

    std::vector<uint32_t> list(m_selectedIds.begin(), m_selectedIds.end());
    emit selectionChanged(list);
    update();
}

void GridGlCanvas::selectRange(uint32_t from_id, uint32_t to_id)
{
    size_t total = m_index.filteredCount();
    bool in_range = false;
    for (size_t i = 0; i < total; ++i) {
        uint32_t id = m_index.filteredIdAt(i);
        if (id == from_id || id == to_id) {
            m_selectedIds.insert(id);
            if (!in_range) {
                in_range = true;
            } else {
                break;
            }
        } else if (in_range) {
            m_selectedIds.insert(id);
        }
    }
    m_lastSelectedId = to_id;

    std::vector<uint32_t> list(m_selectedIds.begin(), m_selectedIds.end());
    emit selectionChanged(list);
    update();
}

void GridGlCanvas::clearSelection()
{
    if (!m_selectedIds.empty()) {
        m_selectedIds.clear();
        m_lastSelectedId = std::nullopt;
        emit selectionChanged({});
        update();
    }
}

void GridGlCanvas::resetHoverState()
{
    m_hover.stop();
    if (m_hoveredFilteredIdx.has_value()) {
        m_hoveredFilteredIdx = std::nullopt;
        emit hoverChanged(std::nullopt);
    }
}

void GridGlCanvas::initPboAndEtc2Support()
{
    while (glGetError() != GL_NO_ERROR) {
    }

    // Probe GL_COMPRESSED_SRGB8_ETC2 hardware upload support with a single 4x4 block
    GLuint probeTex = 0;
    glGenTextures(1, &probeTex);
    glBindTexture(GL_TEXTURE_2D, probeTex);
    const uint8_t dummyBlock[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SRGB8_ETC2, 4, 4, 0, 8, dummyBlock);
    if (glGetError() == GL_NO_ERROR) {
        m_supportsEtc2 = true;
        m_etc2InternalFormat = GL_COMPRESSED_SRGB8_ETC2;
    } else {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB8_ETC2, 4, 4, 0, 8, dummyBlock);
        if (glGetError() == GL_NO_ERROR) {
            m_supportsEtc2 = true;
            m_etc2InternalFormat = GL_COMPRESSED_RGB8_ETC2;
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (probeTex != 0) {
        glDeleteTextures(1, &probeTex);
    }

    const size_t initialCap = GpuTexturePool::ETC2_FRAME_SIZE_BYTES;
    glGenBuffers(2, m_pboRing.data());
    for (size_t i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboRing[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(initialCap), nullptr,
                     GL_STREAM_DRAW);
        m_pboCapacities[i] = initialCap;
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void GridGlCanvas::initCardShaders()
{
    const bool isGles =
        QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->isOpenGLES();

    const char *vsSrc = isGles ? "#version 300 es\n"
                                 "layout(location = 0) in vec2 a_uv;\n"
                                 "uniform vec2 u_viewport;\n"
                                 "uniform vec4 u_cardRect;\n"
                                 "out vec2 v_uv;\n"
                                 "void main() {\n"
                                 "    v_uv = a_uv;\n"
                                 "    vec2 px = u_cardRect.xy + a_uv * u_cardRect.zw;\n"
                                 "    vec2 ndc = vec2((px.x / u_viewport.x) * 2.0 - 1.0,\n"
                                 "                    1.0 - (px.y / u_viewport.y) * 2.0);\n"
                                 "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
                                 "}\n"
                               : "#version 330 core\n"
                                 "layout(location = 0) in vec2 a_uv;\n"
                                 "uniform vec2 u_viewport;\n"
                                 "uniform vec4 u_cardRect;\n"
                                 "out vec2 v_uv;\n"
                                 "void main() {\n"
                                 "    v_uv = a_uv;\n"
                                 "    vec2 px = u_cardRect.xy + a_uv * u_cardRect.zw;\n"
                                 "    vec2 ndc = vec2((px.x / u_viewport.x) * 2.0 - 1.0,\n"
                                 "                    1.0 - (px.y / u_viewport.y) * 2.0);\n"
                                 "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
                                 "}\n";

    const char *fsSrc =
        isGles ? "#version 300 es\n"
                 "precision mediump float;\n"
                 "in vec2 v_uv;\n"
                 "uniform vec4 u_cardRect;\n"
                 "uniform sampler2D u_tex;\n"
                 "uniform int u_srgbEncode;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    vec2 halfSize = u_cardRect.zw * 0.5;\n"
                 "    vec2 p = (v_uv - 0.5) * u_cardRect.zw;\n"
                 "    vec2 q = abs(p) - (halfSize - vec2(8.0));\n"
                 "    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 8.0;\n"
                 "    float alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
                 "    if (alpha <= 0.001) discard;\n"
                 "    vec3 col = texture(u_tex, v_uv).rgb;\n"
                 "    if (u_srgbEncode != 0) {\n"
                 "        col = pow(max(col, vec3(0.0)), vec3(1.0 / 2.2));\n"
                 "    }\n"
                 "    fragColor = vec4(col, alpha);\n"
                 "}\n"
               : "#version 330 core\n"
                 "in vec2 v_uv;\n"
                 "uniform vec4 u_cardRect;\n"
                 "uniform sampler2D u_tex;\n"
                 "uniform int u_srgbEncode;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    vec2 halfSize = u_cardRect.zw * 0.5;\n"
                 "    vec2 p = (v_uv - 0.5) * u_cardRect.zw;\n"
                 "    vec2 q = abs(p) - (halfSize - vec2(8.0));\n"
                 "    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 8.0;\n"
                 "    float alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
                 "    if (alpha <= 0.001) discard;\n"
                 "    vec3 col = texture(u_tex, v_uv).rgb;\n"
                 "    if (u_srgbEncode != 0) {\n"
                 "        col = pow(max(col, vec3(0.0)), vec3(1.0 / 2.2));\n"
                 "    }\n"
                 "    fragColor = vec4(col, alpha);\n"
                 "}\n";

    auto compileShader = [this](GLenum type, const char *src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0)
            glDeleteShader(vs);
        if (fs != 0)
            glDeleteShader(fs);
        return;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(prog);
        return;
    }

    m_cardProgram = prog;
    m_uViewportLoc = glGetUniformLocation(m_cardProgram, "u_viewport");
    m_uCardRectLoc = glGetUniformLocation(m_cardProgram, "u_cardRect");
    m_uTexLoc = glGetUniformLocation(m_cardProgram, "u_tex");

    const char *fsArraySrc =
        isGles ? "#version 300 es\n"
                 "precision mediump float;\n"
                 "precision mediump sampler2DArray;\n"
                 "in vec2 v_uv;\n"
                 "uniform vec4 u_cardRect;\n"
                 "uniform sampler2DArray u_texArray;\n"
                 "uniform float u_texLayer;\n"
                 "uniform int u_srgbEncode;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    vec2 halfSize = u_cardRect.zw * 0.5;\n"
                 "    vec2 p = (v_uv - 0.5) * u_cardRect.zw;\n"
                 "    vec2 q = abs(p) - (halfSize - vec2(8.0));\n"
                 "    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 8.0;\n"
                 "    float alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
                 "    if (alpha <= 0.001) discard;\n"
                 "    vec3 col = texture(u_texArray, vec3(v_uv, u_texLayer)).rgb;\n"
                 "    if (u_srgbEncode != 0) {\n"
                 "        col = pow(max(col, vec3(0.0)), vec3(1.0 / 2.2));\n"
                 "    }\n"
                 "    fragColor = vec4(col, alpha);\n"
                 "}\n"
               : "#version 330 core\n"
                 "in vec2 v_uv;\n"
                 "uniform vec4 u_cardRect;\n"
                 "uniform sampler2DArray u_texArray;\n"
                 "uniform float u_texLayer;\n"
                 "uniform int u_srgbEncode;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    vec2 halfSize = u_cardRect.zw * 0.5;\n"
                 "    vec2 p = (v_uv - 0.5) * u_cardRect.zw;\n"
                 "    vec2 q = abs(p) - (halfSize - vec2(8.0));\n"
                 "    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 8.0;\n"
                 "    float alpha = clamp(0.5 - dist, 0.0, 1.0);\n"
                 "    if (alpha <= 0.001) discard;\n"
                 "    vec3 col = texture(u_texArray, vec3(v_uv, u_texLayer)).rgb;\n"
                 "    if (u_srgbEncode != 0) {\n"
                 "        col = pow(max(col, vec3(0.0)), vec3(1.0 / 2.2));\n"
                 "    }\n"
                 "    fragColor = vec4(col, alpha);\n"
                 "}\n";

    GLuint vsArr = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fsArr = compileShader(GL_FRAGMENT_SHADER, fsArraySrc);
    if (vsArr != 0 && fsArr != 0) {
        GLuint arrProg = glCreateProgram();
        glAttachShader(arrProg, vsArr);
        glAttachShader(arrProg, fsArr);
        glLinkProgram(arrProg);
        GLint arrLinked = GL_FALSE;
        glGetProgramiv(arrProg, GL_LINK_STATUS, &arrLinked);
        if (arrLinked == GL_TRUE) {
            m_cardArrayProgram = arrProg;
            m_uArrayViewportLoc = glGetUniformLocation(m_cardArrayProgram, "u_viewport");
            m_uArrayCardRectLoc = glGetUniformLocation(m_cardArrayProgram, "u_cardRect");
            m_uArrayTexLoc = glGetUniformLocation(m_cardArrayProgram, "u_texArray");
            m_uArrayLayerLoc = glGetUniformLocation(m_cardArrayProgram, "u_texLayer");
            m_uArraySrgbLoc = glGetUniformLocation(m_cardArrayProgram, "u_srgbEncode");
        } else {
            glDeleteProgram(arrProg);
        }
    }
    if (vsArr != 0) {
        glDeleteShader(vsArr);
    }
    if (fsArr != 0) {
        glDeleteShader(fsArr);
    }

    static constexpr float kQuadUvs[12] = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
    };

    glGenVertexArrays(1, &m_cardVao);
    glGenBuffers(1, &m_cardVbo);
    glBindVertexArray(m_cardVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_cardVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadUvs), kQuadUvs, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

GLuint GridGlCanvas::uploadOrGetCardTexture(uint32_t item_id, const QImage &thumb,
                                            int &uploadBudget)
{
    if (thumb.isNull() || m_pboRing[0] == 0) {
        return 0;
    }

    const qint64 cacheKey = thumb.cacheKey();
    auto it = m_etc2Cache.find(item_id);
    if (it != m_etc2Cache.end()) {
        if (it->second.image_cache_key == cacheKey && it->second.tex_id != 0) {
            m_etc2Lru.splice(m_etc2Lru.begin(), m_etc2Lru, it->second.lru_it);
            return it->second.tex_id;
        }
        if (it->second.tex_id != 0) {
            glDeleteTextures(1, &it->second.tex_id);
        }
        m_etc2Lru.erase(it->second.lru_it);
        m_etc2Cache.erase(it);
    }

    if (uploadBudget <= 0) {
        return 0;
    }
    --uploadBudget;

    QImage rgbaImg = (thumb.format() == QImage::Format_RGBA8888)
                         ? thumb
                         : thumb.convertToFormat(QImage::Format_RGBA8888);
    const int w = (rgbaImg.width() + 3) & ~3;
    const int h = (rgbaImg.height() + 3) & ~3;
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (rgbaImg.width() != w || rgbaImg.height() != h) {
        rgbaImg = rgbaImg.scaled(w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }

    const int expectedRowBytes = w * 4;
    const size_t tightBytes = static_cast<size_t>(expectedRowBytes) * static_cast<size_t>(h);
    const uint8_t *rgbaPixels = rgbaImg.constBits();
    thread_local std::vector<uint8_t> tightBuffer;
    if (rgbaImg.bytesPerLine() != expectedRowBytes) {
        tightBuffer.resize(tightBytes);
        for (int row = 0; row < h; ++row) {
            std::memcpy(tightBuffer.data() +
                            static_cast<size_t>(row) * static_cast<size_t>(expectedRowBytes),
                        rgbaImg.constScanLine(row), static_cast<size_t>(expectedRowBytes));
        }
        rgbaPixels = tightBuffer.data();
    }

    while (m_etc2Cache.size() >= MAX_COMPRESSED_CARD_TEXTURES && !m_etc2Lru.empty()) {
        uint32_t victimId = m_etc2Lru.back();
        m_etc2Lru.pop_back();
        auto vit = m_etc2Cache.find(victimId);
        if (vit != m_etc2Cache.end()) {
            if (vit->second.tex_id != 0) {
                glDeleteTextures(1, &vit->second.tex_id);
            }
            m_etc2Cache.erase(vit);
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Restore default unpack state since QPainter mutates unpack stride on the shared GL context.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#ifdef GL_UNPACK_SKIP_PIXELS
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#ifdef GL_UNPACK_SKIP_ROWS
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif

    const size_t ringSlot = static_cast<size_t>(m_pboIndex & 1);
    const GLuint pbo = m_pboRing[ringSlot];
    m_pboIndex ^= 1;

    if (m_supportsEtc2) {
        const uint8_t *compressedData = nullptr;
        size_t payloadBytes = 0;
        auto precompressed = m_pool.getCompressedEtc2Blocks(item_id);
        thread_local std::vector<uint8_t> etc2Blocks;
        if (precompressed && w == static_cast<int>(GpuTexturePool::THUMB_WIDTH) &&
            h == static_cast<int>(GpuTexturePool::THUMB_HEIGHT) &&
            precompressed->size() == GpuTexturePool::ETC2_FRAME_SIZE_BYTES) {
            compressedData = precompressed->data();
            payloadBytes = precompressed->size();
        } else {
            encodeRgbaToEtc2(rgbaPixels, w, h, etc2Blocks);
            compressedData = etc2Blocks.data();
            payloadBytes = etc2Blocks.size();
        }

        glCompressedTexImage2D(GL_TEXTURE_2D, 0, m_etc2InternalFormat, w, h, 0,
                               static_cast<GLsizei>(payloadBytes), nullptr);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        if (payloadBytes > m_pboCapacities[ringSlot]) {
            m_pboCapacities[ringSlot] = payloadBytes;
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(payloadBytes), nullptr,
                         GL_STREAM_DRAW);
        }
        void *mapped =
            glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payloadBytes),
                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (mapped) {
            std::memcpy(mapped, compressedData, payloadBytes);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        } else {
            glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payloadBytes),
                            compressedData);
        }

        glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, m_etc2InternalFormat,
                                  static_cast<GLsizei>(payloadBytes), nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        const size_t payloadBytes = tightBytes;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        if (payloadBytes > m_pboCapacities[ringSlot]) {
            m_pboCapacities[ringSlot] = payloadBytes;
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(payloadBytes), nullptr,
                         GL_STREAM_DRAW);
        }
        void *mapped =
            glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payloadBytes),
                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (mapped) {
            std::memcpy(mapped, rgbaPixels, payloadBytes);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        } else {
            glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payloadBytes),
                            rgbaPixels);
        }

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    m_etc2Lru.push_front(item_id);
    m_etc2Cache.emplace(item_id, CompressedCardEntry{.tex_id = tex,
                                                     .width = w,
                                                     .height = h,
                                                     .image_cache_key = cacheKey,
                                                     .lru_it = m_etc2Lru.begin()});
    return tex;
}

void GridGlCanvas::renderGlThumbnails()
{
    m_glRenderedThisFrame.clear();
    if (m_cardProgram == 0 || m_cardVao == 0 || width() <= 0 || height() <= 0) {
        return;
    }

    size_t total = m_index.filteredCount();
    int cols = computeColumns(width());
    GridVisibleSlice slice = computeVisibleSlice(total, cols, m_scrollY, height());
    if (slice.is_empty) {
        return;
    }

    int uploadBudget = 4;
    const GLint srgbLoc = glGetUniformLocation(m_cardProgram, "u_srgbEncode");

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_cardVao);

    const bool canUsePoolArray =
        (m_cardArrayProgram != 0 && m_pool.isInitialized() && m_pool.textureArrayId() != 0);
    if (canUsePoolArray) {
        glUseProgram(m_cardArrayProgram);
        glUniform2f(m_uArrayViewportLoc, static_cast<float>(width()), static_cast<float>(height()));
        glUniform1i(m_uArrayTexLoc, 0);
        if (m_uArraySrgbLoc >= 0) {
            glUniform1i(m_uArraySrgbLoc, (m_pool.isArrayEtc2() &&
                                          m_pool.arrayInternalFormat() == GL_COMPRESSED_SRGB8_ETC2)
                                             ? 1
                                             : 0);
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_pool.textureArrayId());

        for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
            const auto &item = m_index.filteredItemAt(i);
            QRect card = itemRect(i, cols, m_scrollY);
            if (card.bottom() < 0 || card.top() > height()) {
                continue;
            }

            const int layer = m_pool.requestTexture(item.id, item.path, item.preview_path);
            if (layer < 0) {
                continue;
            }

            bool is_hovered = (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
            QRect render_rect = card;
            if (is_hovered) {
                int dw = static_cast<int>(card.width() * 0.03f);
                int dh = static_cast<int>(card.height() * 0.03f);
                render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
            }

            glUniform1f(m_uArrayLayerLoc, static_cast<float>(layer));
            glUniform4f(m_uArrayCardRectLoc, static_cast<float>(render_rect.x()),
                        static_cast<float>(render_rect.y()),
                        static_cast<float>(render_rect.width()),
                        static_cast<float>(render_rect.height()));
            glDrawArrays(GL_TRIANGLES, 0, 6);
            m_glRenderedThisFrame.insert(item.id);
        }

        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    } else {
        glUseProgram(m_cardProgram);
        glUniform2f(m_uViewportLoc, static_cast<float>(width()), static_cast<float>(height()));
        glUniform1i(m_uTexLoc, 0);
        if (srgbLoc >= 0) {
            glUniform1i(srgbLoc,
                        (m_supportsEtc2 && m_etc2InternalFormat == GL_COMPRESSED_SRGB8_ETC2) ? 1
                                                                                             : 0);
        }

        for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
            const auto &item = m_index.filteredItemAt(i);
            QRect card = itemRect(i, cols, m_scrollY);
            if (card.bottom() < 0 || card.top() > height()) {
                continue;
            }

            (void)m_pool.requestTexture(item.id, item.path, item.preview_path);
            QImage thumb = m_pool.getThumbnailImage(item.id);
            if (thumb.isNull()) {
                continue;
            }

            GLuint tex = uploadOrGetCardTexture(item.id, thumb, uploadBudget);
            if (tex == 0) {
                continue;
            }

            bool is_hovered = (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
            QRect render_rect = card;
            if (is_hovered) {
                int dw = static_cast<int>(card.width() * 0.03f);
                int dh = static_cast<int>(card.height() * 0.03f);
                render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
            }

            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform4f(m_uCardRectLoc, static_cast<float>(render_rect.x()),
                        static_cast<float>(render_rect.y()),
                        static_cast<float>(render_rect.width()),
                        static_cast<float>(render_rect.height()));
            glDrawArrays(GL_TRIANGLES, 0, 6);
            m_glRenderedThisFrame.insert(item.id);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

void GridGlCanvas::initializeGL()
{
    if (isOffscreenPlatform()) {
        return;
    }

    // Clear any stale handles left over if a previous GL context was torn down on dock/monitor
    // reparenting before initializing resources on the current context.
    m_etc2Cache.clear();
    m_etc2Lru.clear();
    m_pboRing = {0, 0};
    m_pboCapacities = {0, 0};
    m_pboIndex = 0;
    m_supportsEtc2 = false;
    m_etc2InternalFormat = 0;
    m_cardProgram = 0;
    m_cardArrayProgram = 0;
    m_cardVao = 0;
    m_cardVbo = 0;
    m_uViewportLoc = -1;
    m_uCardRectLoc = -1;
    m_uTexLoc = -1;
    m_uArrayViewportLoc = -1;
    m_uArrayCardRectLoc = -1;
    m_uArrayTexLoc = -1;
    m_uArrayLayerLoc = -1;
    m_uArraySrgbLoc = -1;
    m_glRenderedThisFrame.clear();

    QObject::disconnect(m_contextDestroyConn);
    if (QOpenGLContext *ctx = context()) {
        m_contextDestroyConn = connect(
            ctx, &QOpenGLContext::aboutToBeDestroyed, this,
            [this]() {
                makeCurrent();
                releaseGlResources();
                doneCurrent();
            },
            Qt::DirectConnection);
    }

    initializeOpenGLFunctions();

    m_pool.initGl(this);
    m_hover.initGl(this);
    initPboAndEtc2Support();
    initCardShaders();

    m_glInitialized = true;
}

void GridGlCanvas::resizeGL(int w, int h)
{
    if (isOffscreenPlatform()) {
        int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
        emit virtualHeightChanged(vh);
        return;
    }
    glViewport(0, 0, w, h);
    int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
    emit virtualHeightChanged(vh);
}

void GridGlCanvas::resizeEvent(QResizeEvent *event)
{
    if (isOffscreenPlatform()) {
        // Offscreen QPA lacks a GL backing context; stay on the QPainter card fallback without
        // letting QOpenGLWidgetPrivate::initialize() allocate and leak a failed QOpenGLContext.
        QWidget::resizeEvent(event);
        int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
        emit virtualHeightChanged(vh);
        return;
    }
    QOpenGLWidget::resizeEvent(event);
    int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
    emit virtualHeightChanged(vh);
}

void GridGlCanvas::paintGL()
{
    if (isOffscreenPlatform()) {
        return;
    }
    if (isValid()) {
        m_pool.processPendingUploads(this, 12);
        const qreal dpr = devicePixelRatioF();
        glViewport(0, 0, std::max(1, static_cast<int>(width() * dpr)),
                   std::max(1, static_cast<int>(height() * dpr)));
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        renderGlThumbnails();
    } else {
        m_glRenderedThisFrame.clear();
        m_pool.processPendingUploads(nullptr, 12);
    }

    QPainter painter(this);
    renderCards(painter);
}

void GridGlCanvas::paintEvent(QPaintEvent *event)
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

    m_glRenderedThisFrame.clear();
    m_pool.processPendingUploads(nullptr, 12);

    const qreal dpr = devicePixelRatioF();
    QImage frame(std::max(1, static_cast<int>(width() * dpr)),
                 std::max(1, static_cast<int>(height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    frame.setDevicePixelRatio(dpr);
    frame.fill(QColor(20, 23, 28));

    QPainter painter(&frame);
    renderCards(painter);
    painter.end();

    m_fallbackCanvas->setGeometry(rect());
    m_fallbackCanvas->setPixmap(QPixmap::fromImage(frame));
    if (!m_fallbackCanvas->isVisible()) {
        m_fallbackCanvas->show();
    }
}

void GridGlCanvas::renderCards(QPainter &painter)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    size_t total = m_index.filteredCount();
    int cols = computeColumns(width());
    GridVisibleSlice slice = computeVisibleSlice(total, cols, m_scrollY, height());

    if (slice.is_empty) {
        painter.setPen(QColor(148, 163, 184));
        QFont font = painter.font();
        font.setPointSize(10);
        font.setWeight(QFont::Medium);
        painter.setFont(font);
        painter.drawText(rect().adjusted(14, 14, -14, -14), Qt::AlignCenter | Qt::TextWordWrap,
                         "No wallpapers found matching query or tags.");
        return;
    }

    for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
        const auto &item = m_index.filteredItemAt(i);
        QRect card = itemRect(i, cols, m_scrollY);
        if (card.bottom() < 0 || card.top() > height()) {
            continue;
        }

        bool is_hovered = (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
        bool is_selected = m_selectedIds.contains(item.id);
        bool renderedByGl = m_glRenderedThisFrame.contains(item.id);

        QRect render_rect = card;
        if (is_hovered) {
            int dw = static_cast<int>(card.width() * 0.03f);
            int dh = static_cast<int>(card.height() * 0.03f);
            render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
        }

        painter.save();
        QPainterPath card_clip;
        card_clip.addRoundedRect(QRectF(render_rect), 8.0, 8.0);
        painter.setClipPath(card_clip);

        if (!renderedByGl) {
            uint32_t dc = item.dominant_color;
            QColor base_color((dc >> 16) & 0xFF, (dc >> 8) & 0xFF, dc & 0xFF);
            QLinearGradient bg_grad(render_rect.topLeft(), render_rect.bottomRight());
            bg_grad.setColorAt(0.0, base_color.lighter(120));
            bg_grad.setColorAt(0.55, base_color.darker(115));
            bg_grad.setColorAt(1.0, base_color.darker(175));
            painter.fillRect(render_rect, bg_grad);

            QPainterPath wave;
            const qreal wl = render_rect.left();
            const qreal wr = render_rect.right();
            const qreal wb = render_rect.bottom();
            const qreal wy = render_rect.top() + render_rect.height() * 0.56;
            wave.moveTo(wl, wb);
            wave.lineTo(wl, wy);
            wave.cubicTo(wl + render_rect.width() * 0.35, wy - 18.0,
                         wl + render_rect.width() * 0.65, wy + 16.0, wr, wy - 6.0);
            wave.lineTo(wr, wb);
            wave.closeSubpath();
            QColor wave_col = base_color.darker(210);
            wave_col.setAlpha(110);
            painter.fillPath(wave, wave_col);

            (void)m_pool.requestTexture(item.id, item.path, item.preview_path);
            QImage thumb = m_pool.getThumbnailImage(item.id);
            if (!thumb.isNull()) {
                painter.drawImage(render_rect, thumb);
            }
        }

        if (m_hover.isPreviewActive() && m_hover.activeItemId() == item.id) {
            const QImage &previewImg = m_hover.currentPreviewImage();
            if (!previewImg.isNull()) {
                painter.drawImage(render_rect, previewImg);
            }
        }

        drawBadgePill(painter, render_rect, item.badge);
        drawCardSelectionIndicator(painter, render_rect, is_selected);
        drawCardTitleOverlay(painter, render_rect, item.title);

        painter.restore();

        if (is_selected) {
            QPen sel_pen(QColor(56, 189, 248), 2.0);
            painter.setPen(sel_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
        } else if (is_hovered) {
            QPen hover_pen(QColor(129, 140, 248, 220), 2.0);
            painter.setPen(hover_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
        } else {
            QPen border_pen(QColor(255, 255, 255, 28), 1.0);
            painter.setPen(border_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(render_rect, 8.0, 8.0);
        }
    }
}

void GridGlCanvas::drawBadgePill(QPainter &painter, const QRect &card_rect, WallpaperBadge badge)
{
    QFont font = painter.font();
    font.setPixelSize(9);
    font.setWeight(QFont::Bold);
    painter.setFont(font);

    QString label = QString::fromUtf8(badgeText(badge));
    QFontMetrics fm(font);
    int pill_w =
        std::clamp(fm.horizontalAdvance(label) + 12, 42, std::max(42, card_rect.width() - 44));
    QRect pill_rect(card_rect.left() + 8, card_rect.top() + 8, pill_w, 19);

    QColor pill_bg = badgeColor(badge);
    pill_bg.setAlpha(215);

    painter.setPen(Qt::NoPen);
    painter.setBrush(pill_bg);
    painter.drawRoundedRect(pill_rect, 4.0, 4.0);

    painter.setPen(Qt::white);
    painter.drawText(pill_rect, Qt::AlignCenter, label);
}

void GridGlCanvas::drawCardSelectionIndicator(QPainter &painter, const QRect &card_rect,
                                              bool is_selected)
{
    QRect box_rect(card_rect.right() - 26, card_rect.top() + 8, 18, 18);

    if (is_selected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(56, 189, 248));
        painter.drawRoundedRect(box_rect, 4.0, 4.0);

        painter.setPen(QPen(Qt::black, 2.0));
        painter.drawLine(box_rect.left() + 4, box_rect.top() + 9, box_rect.left() + 7,
                         box_rect.top() + 13);
        painter.drawLine(box_rect.left() + 7, box_rect.top() + 13, box_rect.left() + 14,
                         box_rect.top() + 5);
    } else {
        painter.setPen(QPen(QColor(255, 255, 255, 130), 1.5));
        painter.setBrush(QColor(15, 23, 42, 110));
        painter.drawRoundedRect(box_rect, 4.0, 4.0);
    }
}

void GridGlCanvas::drawCardTitleOverlay(QPainter &painter, const QRect &card_rect,
                                        const std::string &title)
{
    int banner_h = 34;
    QRect banner_rect(card_rect.left(), card_rect.bottom() - banner_h + 1, card_rect.width(),
                      banner_h);

    QLinearGradient grad(banner_rect.topLeft(), banner_rect.bottomLeft());
    grad.setColorAt(0.0, QColor(10, 12, 16, 0));
    grad.setColorAt(0.35, QColor(10, 12, 16, 175));
    grad.setColorAt(1.0, QColor(10, 12, 16, 238));

    painter.fillRect(banner_rect, grad);

    QRect text_rect(card_rect.left() + 9, card_rect.bottom() - 22,
                    std::max(20, card_rect.width() - 18), 18);

    painter.setPen(QColor(241, 245, 249));
    QFont font = painter.font();
    font.setPixelSize(10);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);

    QString qtitle = QString::fromStdString(title);
    QFontMetrics fm(font);
    QString elided = fm.elidedText(qtitle, Qt::ElideRight, text_rect.width());
    painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

void GridGlCanvas::mouseMoveEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (hit != m_hoveredFilteredIdx) {
        if (m_hoveredFilteredIdx.has_value()) {
            if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
                uint32_t prev_id = m_index.filteredIdAt(*m_hoveredFilteredIdx);
                m_hover.onCellHoverExited(prev_id);
            } else {
                m_hover.stop();
            }
        }

        m_hoveredFilteredIdx = hit;
        emit hoverChanged(hit);

        if (hit.has_value() && *hit < m_index.filteredCount()) {
            const auto &item = m_index.filteredItemAt(*hit);
            m_hover.onCellHoverEntered(item.id, item.path, item.badge, item.preview_path);
        }

        update();
    }
}

void GridGlCanvas::mousePressEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (!hit.has_value()) {
        if (event->button() == Qt::RightButton) {
            QMenu menu(this);
            menu.setStyleSheet("QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid "
                               "#334155; padding: 4px; }"
                               "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
                               "QMenu::item:selected { background: #0284c7; color: white; }");
            QAction *cfgAct = menu.addAction("Configure Wallpaper Folders && Exclusions...");
            connect(cfgAct, &QAction::triggered, this, &GridGlCanvas::configureFoldersRequested);
            menu.exec(event->globalPosition().toPoint());
        }
        return;
    }

    const auto &item = m_index.filteredItemAt(*hit);

    if (event->button() == Qt::RightButton) {
        clearSelection();
        toggleSelection(item.id);
        emit itemSelected(item);

        std::filesystem::path item_p(item.path);
        std::filesystem::path parent_dir = item_p.parent_path();
        QString parent_qstr = QString::fromStdString(parent_dir.string());
        QString parent_name = QString::fromStdString(parent_dir.filename().string());

        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; padding: 4px; "
            "}"
            "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
            "QMenu::item:selected { background: #0284c7; color: white; }"
            "QMenu::separator { height: 1px; background: #334155; margin: 4px 2px; }");
        QAction *applyAct = menu.addAction("Apply Wallpaper Now");
        connect(applyAct, &QAction::triggered, this,
                [this, item]() { emit itemDoubleClicked(item); });
        if (!m_embeddedMode) {
            menu.addSeparator();
            if (!parent_qstr.isEmpty()) {
                QAction *exclAct = menu.addAction(
                    QStringLiteral("Exclude Folder '%1' (%2)").arg(parent_name, parent_qstr));
                connect(exclAct, &QAction::triggered, this,
                        [this, parent_qstr]() { emit excludeFolderRequested(parent_qstr); });
            }
            QAction *cfgAct = menu.addAction("Configure Wallpaper Folders && Exclusions...");
            connect(cfgAct, &QAction::triggered, this, &GridGlCanvas::configureFoldersRequested);
        }
        menu.exec(event->globalPosition().toPoint());
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
        if (m_lastSelectedId.has_value()) {
            selectRange(*m_lastSelectedId, item.id);
        } else {
            toggleSelection(item.id);
        }
    } else if (event->modifiers() & Qt::ControlModifier) {
        toggleSelection(item.id);
    } else {
        clearSelection();
        toggleSelection(item.id);
    }

    emit itemSelected(item);
}

void GridGlCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (hit.has_value()) {
        const auto &item = m_index.filteredItemAt(*hit);
        emit itemDoubleClicked(item);
    }
}

void GridGlCanvas::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (m_hoveredFilteredIdx.has_value()) {
        if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
            uint32_t id = m_index.filteredIdAt(*m_hoveredFilteredIdx);
            m_hover.onCellHoverExited(id);
        } else {
            m_hover.stop();
        }
        m_hoveredFilteredIdx = std::nullopt;
        emit hoverChanged(std::nullopt);
        update();
    }
}

void GridGlCanvas::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y();
    setScrollY(m_scrollY - delta);
    event->accept();
}

// ============================================================================
// VirtualizedGridWidget Implementation
// ============================================================================

VirtualizedGridWidget::VirtualizedGridWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(200);
    setupUi();
}

VirtualizedGridWidget::~VirtualizedGridWidget()
{
    m_pool.setThumbnailReadyCallback(nullptr);
    m_pool.shutdownWorkers();
    m_hover.stop();
    delete m_canvas;
    m_canvas = nullptr;
}

void VirtualizedGridWidget::setupUi()
{
    auto *root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(10, 10, 10, 10);
    root_layout->setSpacing(8);

    m_headerContainer = new QWidget(this);
    auto *header_layout = new QVBoxLayout(m_headerContainer);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(8);

    m_searchEdit = new QLineEdit(m_headerContainer);
    m_searchEdit->setPlaceholderText("Search wallpapers...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(28);
    m_searchEdit->setStyleSheet(
        "QLineEdit { background: #14171f; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 4px 10px; font-size: 11px; }"
        "QLineEdit:focus { border: 1px solid #38bdf8; }");
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            &VirtualizedGridWidget::onSearchTextChanged);
    header_layout->addWidget(m_searchEdit);

    auto *top_bar = new QHBoxLayout();
    top_bar->setContentsMargins(0, 0, 0, 0);
    top_bar->setSpacing(8);

    m_tagCombo = new QComboBox(m_headerContainer);
    m_tagCombo->setFixedHeight(26);
    m_tagCombo->addItem("All Tags", QVariant::fromValue<qulonglong>(WallpaperTags::None));
    for (const auto &tag : WallpaperTags::allKnownTags()) {
        m_tagCombo->addItem(QString::fromUtf8(tag.display_name),
                            QVariant::fromValue<qulonglong>(tag.mask));
    }
    m_tagCombo->setStyleSheet(
        "QComboBox { background: #14171f; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 2px 8px; font-size: 11px; }");
    connect(m_tagCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        uint64_t mask = m_tagCombo->itemData(idx).toULongLong();
        setTagFilter(mask);
    });
    top_bar->addWidget(m_tagCombo, 1);

    m_sortCombo = new QComboBox(m_headerContainer);
    m_sortCombo->setFixedHeight(26);
    m_sortCombo->addItem("Name (A-Z)", static_cast<int>(SortCriteria::NameAsc));
    m_sortCombo->addItem("Name (Z-A)", static_cast<int>(SortCriteria::NameDesc));
    m_sortCombo->addItem("Newest", static_cast<int>(SortCriteria::DateNewest));
    m_sortCombo->addItem("Oldest", static_cast<int>(SortCriteria::DateOldest));
    m_sortCombo->addItem("Largest", static_cast<int>(SortCriteria::SizeLargest));
    m_sortCombo->addItem("Smallest", static_cast<int>(SortCriteria::SizeSmallest));
    m_sortCombo->addItem("Type", static_cast<int>(SortCriteria::Type));
    m_sortCombo->addItem("Color", static_cast<int>(SortCriteria::DominantHue));
    m_sortCombo->setStyleSheet(
        "QComboBox { background: #14171f; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 2px 8px; font-size: 11px; }");
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VirtualizedGridWidget::onSortChanged);
    top_bar->addWidget(m_sortCombo, 1);

    m_foldersButton = new QPushButton("Folders...", m_headerContainer);
    m_foldersButton->setFixedHeight(26);
    m_foldersButton->setToolTip("Choose which wallpaper folders to include or exclude");
    m_foldersButton->setStyleSheet(
        "QPushButton { background: #20242e; color: #e2e8f0; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 2px 10px; font-size: 11px; font-weight: 500; }"
        "QPushButton:hover { background: #2a303d; border-color: #38bdf8; color: #ffffff; }");
    connect(m_foldersButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::openFolderSettingsDialog);
    top_bar->addWidget(m_foldersButton);

    header_layout->addLayout(top_bar);
    root_layout->addWidget(m_headerContainer);

    m_statusLabel = new QLabel("0 wallpapers", this);
    m_statusLabel->setStyleSheet("color: #64748b; font-size: 11px;");
    root_layout->addWidget(m_statusLabel);

    auto *grid_layout = new QHBoxLayout();
    grid_layout->setContentsMargins(0, 0, 0, 0);
    grid_layout->setSpacing(4);

#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
    const bool offscreenNoGl = isOffscreenPlatform();
#else
    const bool offscreenNoGl = false;
#endif
    m_canvas = new GridGlCanvas(m_index, m_pool, m_hover, offscreenNoGl ? nullptr : this);
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setVisible(false);
    m_scrollBar->setStyleSheet(
        "QScrollBar:vertical { background: #14171f; width: 8px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #2e3545; min-height: 24px; border-radius: 4px; }"
        "QScrollBar::handle:vertical:hover { background: #475569; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    connect(m_scrollBar, &QScrollBar::valueChanged, this,
            &VirtualizedGridWidget::onScrollValueChanged);
    connect(m_canvas, &GridGlCanvas::virtualHeightChanged, this,
            &VirtualizedGridWidget::onVirtualHeightChanged);

    connect(m_canvas, &GridGlCanvas::itemSelected, this, &VirtualizedGridWidget::wallpaperSelected);
    connect(m_canvas, &GridGlCanvas::itemDoubleClicked, this,
            &VirtualizedGridWidget::wallpaperDoubleClicked);
    connect(m_canvas, &GridGlCanvas::configureFoldersRequested, this,
            &VirtualizedGridWidget::openFolderSettingsDialog);
    connect(m_canvas, &GridGlCanvas::excludeFolderRequested, this,
            [this](const QString &folder_path) {
                m_index.addExcludedDirectory(folder_path.toStdString());
                (void)m_index.saveConfigToFile();
                scanWallpapers();
                selectFirstItemIfAvailable();
            });
    connect(m_canvas, &GridGlCanvas::selectionChanged, this, [this](const auto &ids) {
        emit playlistSelectionChanged(ids);
        if (m_playlistBar) {
            m_playlistBar->setVisible(!ids.empty());
            if (m_playlistSelectionLabel) {
                m_playlistSelectionLabel->setText(QString("%1 selected").arg(ids.size()));
            }
        }
    });

#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
    if (isOffscreenPlatform()) {
        m_canvas->hide();
        auto *offscreenPlaceholder = new QWidget(this);
        offscreenPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        grid_layout->addWidget(offscreenPlaceholder, 1);
    } else {
        grid_layout->addWidget(m_canvas, 1);
    }
#else
    grid_layout->addWidget(m_canvas, 1);
#endif
    grid_layout->addWidget(m_scrollBar);
    root_layout->addLayout(grid_layout, 1);

    m_playlistBar = new QWidget(this);
    m_playlistBar->setVisible(false);
    m_playlistBar->setStyleSheet("QWidget { background: #181b22; border: 1px solid #2c3240; "
                                 "border-radius: 5px; }");
    auto *pl_layout = new QHBoxLayout(m_playlistBar);
    pl_layout->setContentsMargins(10, 6, 10, 6);
    pl_layout->setSpacing(8);

    m_playlistSelectionLabel = new QLabel("0 selected", m_playlistBar);
    m_playlistSelectionLabel->setStyleSheet(
        "color: #f1f5f9; font-weight: 600; font-size: 11px; border: none;");
    pl_layout->addWidget(m_playlistSelectionLabel);

    pl_layout->addStretch();

    m_playlistAddButton = new QPushButton("Add to Playlist", m_playlistBar);
    m_playlistAddButton->setFixedHeight(26);
    m_playlistAddButton->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: none; border-radius: 4px; "
        "padding: 4px 12px; font-weight: 600; font-size: 11px; }"
        "QPushButton:hover { background: #0369a1; }");
    connect(m_playlistAddButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::onPlaylistAddClicked);
    pl_layout->addWidget(m_playlistAddButton);

    m_playlistClearButton = new QPushButton("Clear", m_playlistBar);
    m_playlistClearButton->setFixedHeight(26);
    m_playlistClearButton->setStyleSheet(
        "QPushButton { background: #20242e; color: #94a3b8; "
        "border: 1px solid #2e3545; border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
        "QPushButton:hover { color: #f1f5f9; border-color: #38bdf8; }");
    connect(m_playlistClearButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::clearSelection);
    pl_layout->addWidget(m_playlistClearButton);

    root_layout->addWidget(m_playlistBar);
}

void VirtualizedGridWidget::setTagFilter(uint64_t tag_mask)
{
    if (m_tagCombo) {
        QSignalBlocker blocker(m_tagCombo);
        int matchedIdx = 0;
        for (int i = 0; i < m_tagCombo->count(); ++i) {
            if (m_tagCombo->itemData(i).toULongLong() == tag_mask) {
                matchedIdx = i;
                break;
            }
        }
        m_tagCombo->setCurrentIndex(matchedIdx);
    }

    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setTagMask(tag_mask, false);
    if (m_canvas) {
        int cols = m_canvas->computeColumns(m_canvas->width());
        int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
        onVirtualHeightChanged(vh);
        updateStatusText();
        m_canvas->update();
    }
}

void VirtualizedGridWidget::setEmbeddedMode(bool embedded)
{
    m_embeddedMode = embedded;
    if (m_headerContainer) {
        m_headerContainer->setVisible(!embedded);
    }
    if (m_statusLabel) {
        m_statusLabel->setVisible(!embedded);
    }
    if (m_canvas) {
        m_canvas->setEmbeddedMode(embedded);
    }
    if (auto *root = qobject_cast<QVBoxLayout *>(layout())) {
        if (embedded) {
            root->setContentsMargins(8, 6, 8, 6);
            root->setSpacing(4);
        } else {
            root->setContentsMargins(10, 10, 10, 10);
            root->setSpacing(8);
        }
    }
}

void VirtualizedGridWidget::refreshCatalogView()
{
    if (!m_canvas) {
        return;
    }
    m_canvas->resetHoverState();
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
}

void VirtualizedGridWidget::scanWallpapers(const std::vector<std::string> &extra_paths)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
        m_canvas->clearSelection();
    }
    size_t count = m_index.scanAll(extra_paths);
    refreshCatalogView();
    emit catalogScanned(count);
}

void VirtualizedGridWidget::populateSampleCatalogIfEmpty(bool force_showcase_cards)
{
    if (!force_showcase_cards && m_index.totalCount() > 0) {
        return;
    }

    struct SampleDef {
        const char *title;
        const char *path;
        WallpaperBadge badge;
        uint32_t color;
        uint64_t tags;
    };

    const SampleDef samples[] = {
        {"01 Cyberpunk Neon Rainstorm", "/usr/share/wwal/samples/cyberpunk_neon.glsl",
         WallpaperBadge::Shader, 0x0ea5e9,
         WallpaperTags::Cyberpunk | WallpaperTags::Dark | WallpaperTags::Urban},
        {"02 Abyssal Bioluminescence", "/usr/share/wwal/samples/abyssal_reef.json",
         WallpaperBadge::Scene, 0x6366f1,
         WallpaperTags::Nature | WallpaperTags::Dark | WallpaperTags::Fantasy},
        {"03 Solarized Horizon 4K", "/usr/share/wwal/samples/solarized_horizon.png",
         WallpaperBadge::Image, 0xf97316,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Nature},
        {"04 Quantum Fluid Vortex", "/usr/share/wwal/samples/quantum_fluid.glsl",
         WallpaperBadge::Shader, 0x10b981,
         WallpaperTags::Abstract | WallpaperTags::SciFi | WallpaperTags::Dark},
        {"05 Tokyo Midnight Shinjuku", "/usr/share/wwal/samples/tokyo_shinjuku.mp4",
         WallpaperBadge::Video, 0xa855f7,
         WallpaperTags::Urban | WallpaperTags::Cyberpunk | WallpaperTags::Photography},
        {"06 Celestial Nebula Drift", "/usr/share/wwal/samples/celestial_nebula.json",
         WallpaperBadge::Scene, 0xec4899,
         WallpaperTags::Space | WallpaperTags::SciFi | WallpaperTags::DigitalArt},
        {"07 Nordic Fjord Minimal", "/usr/share/wwal/samples/nordic_fjord.png",
         WallpaperBadge::Image, 0x3b82f6,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Light},
        {"08 Synthwave Outrun Grid", "/usr/share/wwal/samples/synthwave_grid.glsl",
         WallpaperBadge::Shader, 0xd946ef,
         WallpaperTags::Retro | WallpaperTags::Cyberpunk | WallpaperTags::Vehicles},
    };

    int64_t base_time = 1710000000;
    for (size_t i = 0; i < std::size(samples); ++i) {
        CatalogItem item;
        item.title = samples[i].title;
        item.path = samples[i].path;
        item.badge = samples[i].badge;
        item.dominant_color = samples[i].color;
        item.tag_mask = samples[i].tags;
        item.file_size = (2 + i * 3) * 1024 * 1024;
        item.date_modified = base_time + static_cast<int64_t>(i * 3600);
        m_index.addItem(std::move(item));
    }

    m_index.refilter();
    refreshCatalogView();
    emit catalogScanned(m_index.totalCount());
}

void VirtualizedGridWidget::selectFirstItemIfAvailable()
{
    if (m_index.filteredCount() > 0 && m_canvas) {
        const CatalogItem &first = m_index.filteredItemAt(0);
        m_canvas->clearSelection();
        m_canvas->toggleSelection(first.id);
        emit wallpaperSelected(first);
    }
}

void VirtualizedGridWidget::openFolderSettingsDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Wallpaper Folders & Exclusions");
    dlg.setMinimumSize(540, 480);
    dlg.setStyleSheet(
        "QDialog { background: #14171d; color: #f1f5f9; }"
        "QLabel { color: #e2e8f0; font-size: 11px; }"
        "QListWidget { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px; font-size: 11px; }"
        "QLineEdit { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px 8px; font-size: 11px; }"
        "QPushButton { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 5px 10px; font-size: 11px; font-weight: 600; }"
        "QPushButton:hover { background: #283040; border-color: #38bdf8; }"
        "QCheckBox { color: #cbd5e1; font-size: 11px; spacing: 6px; }");

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *incHeader =
        new QLabel("<b>Included Wallpaper Folders</b> (only these folders are scanned):", &dlg);
    layout->addWidget(incHeader);

    auto *incList = new QListWidget(&dlg);
    for (const auto &dir : m_index.includedDirectories()) {
        incList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(incList, 1);

    auto *incBtnRow = new QHBoxLayout();
    auto *addIncBtn = new QPushButton("+ Add Folder...", &dlg);
    auto *onlyWallpapersBtn = new QPushButton("Only ~/Pictures/Wallpapers", &dlg);
    auto *remIncBtn = new QPushButton("Remove Selected", &dlg);
    incBtnRow->addWidget(addIncBtn);
    incBtnRow->addWidget(onlyWallpapersBtn);
    incBtnRow->addWidget(remIncBtn);
    incBtnRow->addStretch();
    layout->addLayout(incBtnRow);

    connect(addIncBtn, &QPushButton::clicked, &dlg, [&dlg, incList]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, "Select Wallpaper Folder to Include");
        if (!dir.isEmpty()) {
            incList->addItem(dir);
        }
    });
    connect(onlyWallpapersBtn, &QPushButton::clicked, &dlg, [incList]() {
        incList->clear();
        incList->addItem(
            QString::fromStdString(FuzzyCatalogIndex::expandUserPath("~/Pictures/Wallpapers")));
    });
    connect(remIncBtn, &QPushButton::clicked, &dlg,
            [incList]() { qDeleteAll(incList->selectedItems()); });

    auto *excHeader =
        new QLabel("<b>Excluded Folders or Folder Names</b> (e.g. <code>Screenshots</code>, "
                   "<code>~/Pictures/Camera</code>):",
                   &dlg);
    layout->addWidget(excHeader);

    auto *excList = new QListWidget(&dlg);
    for (const auto &dir : m_index.excludedDirectories()) {
        excList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(excList, 1);

    auto *excInputRow = new QHBoxLayout();
    auto *excPatternEdit = new QLineEdit(&dlg);
    excPatternEdit->setPlaceholderText(
        "Folder name or path to exclude (e.g. Screenshots, Memes)...");
    auto *addExcPatternBtn = new QPushButton("+ Exclude Name/Path", &dlg);
    auto *addExcDirBtn = new QPushButton("Browse Folder...", &dlg);
    auto *remExcBtn = new QPushButton("Remove Selected", &dlg);
    excInputRow->addWidget(excPatternEdit, 1);
    excInputRow->addWidget(addExcPatternBtn);
    excInputRow->addWidget(addExcDirBtn);
    excInputRow->addWidget(remExcBtn);
    layout->addLayout(excInputRow);

    connect(addExcPatternBtn, &QPushButton::clicked, &dlg, [excPatternEdit, excList]() {
        QString txt = excPatternEdit->text().trimmed();
        if (!txt.isEmpty()) {
            excList->addItem(txt);
            excPatternEdit->clear();
        }
    });
    connect(excPatternEdit, &QLineEdit::returnPressed, addExcPatternBtn, &QPushButton::click);
    connect(addExcDirBtn, &QPushButton::clicked, &dlg, [&dlg, excList]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, "Select Folder to Exclude");
        if (!dir.isEmpty()) {
            excList->addItem(dir);
        }
    });
    connect(remExcBtn, &QPushButton::clicked, &dlg,
            [excList]() { qDeleteAll(excList->selectedItems()); });

    auto *sysCheck =
        new QCheckBox("Also scan system (/usr/share/backgrounds) and Steam Workshop folders", &dlg);
    sysCheck->setChecked(m_index.scanSystemAndWorkshopPaths());
    layout->addWidget(sysCheck);

    auto *cfgPathNote = new QLabel(
        QStringLiteral("Saved to: <code>%1</code> (or set <code>WWAL_WALLPAPER_DIRS</code> / "
                       "<code>WWAL_EXCLUDE_DIRS</code>)")
            .arg(QString::fromStdString(FuzzyCatalogIndex::defaultConfigFilePath())),
        &dlg);
    cfgPathNote->setStyleSheet("color: #64748b; font-size: 10px;");
    cfgPathNote->setWordWrap(true);
    layout->addWidget(cfgPathNote);

    auto *bottomRow = new QHBoxLayout();
    auto *resetBtn = new QPushButton("Reset Defaults", &dlg);
    bottomRow->addWidget(resetBtn);
    bottomRow->addStretch();

    auto *cancelBtn = new QPushButton("Cancel", &dlg);
    auto *saveBtn = new QPushButton("Save && Rescan", &dlg);
    saveBtn->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: 1px solid #38bdf8; "
        "border-radius: 5px; padding: 6px 14px; font-weight: bold; }"
        "QPushButton:hover { background: #0369a1; }");
    bottomRow->addWidget(cancelBtn);
    bottomRow->addWidget(saveBtn);
    layout->addLayout(bottomRow);

    connect(resetBtn, &QPushButton::clicked, &dlg, [incList, excList, sysCheck]() {
        FuzzyCatalogIndex defaultsIdx;
        defaultsIdx.resetFolderConfigToDefaults();
        incList->clear();
        for (const auto &d : defaultsIdx.includedDirectories()) {
            incList->addItem(QString::fromStdString(d));
        }
        excList->clear();
        for (const auto &d : defaultsIdx.excludedDirectories()) {
            excList->addItem(QString::fromStdString(d));
        }
        sysCheck->setChecked(defaultsIdx.scanSystemAndWorkshopPaths());
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() == QDialog::Accepted) {
        std::vector<std::string> newInc;
        newInc.reserve(static_cast<size_t>(incList->count()));
        for (int i = 0; i < incList->count(); ++i) {
            newInc.push_back(incList->item(i)->text().toStdString());
        }
        std::vector<std::string> newExc;
        newExc.reserve(static_cast<size_t>(excList->count()));
        for (int i = 0; i < excList->count(); ++i) {
            newExc.push_back(excList->item(i)->text().toStdString());
        }

        m_index.setIncludedDirectories(newInc);
        m_index.setExcludedDirectories(newExc);
        m_index.setScanSystemAndWorkshopPaths(sysCheck->isChecked());
        (void)m_index.saveConfigToFile();

        scanWallpapers();
        selectFirstItemIfAvailable();
    }
}

void VirtualizedGridWidget::onSearchTextChanged(const QString &text)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setSearchQuery(text.toStdString());
    int cols = m_canvas->computeColumns(m_canvas->width());
    int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
    onVirtualHeightChanged(vh);
    updateStatusText();
    m_canvas->update();
}

void VirtualizedGridWidget::onSortChanged(int combo_index)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    auto criteria = static_cast<SortCriteria>(m_sortCombo->itemData(combo_index).toInt());
    m_index.setSorting(criteria);
    m_canvas->update();
}

void VirtualizedGridWidget::onScrollValueChanged(int value) { m_canvas->setScrollY(value); }

void VirtualizedGridWidget::onVirtualHeightChanged(int virtual_height)
{
    int max_val = std::max(0, virtual_height - m_canvas->height());
    m_scrollBar->setRange(0, max_val);
    m_scrollBar->setPageStep(m_canvas->height());
    m_scrollBar->setVisible(max_val > 0);
}

void VirtualizedGridWidget::onPlaylistAddClicked()
{
    auto ids = selectedIds();
    if (!ids.empty()) {
        emit playlistSelectionChanged(ids);
    }
}

void VirtualizedGridWidget::clearSelection() { m_canvas->clearSelection(); }

std::vector<uint32_t> VirtualizedGridWidget::selectedIds() const
{
    const auto &set = m_canvas->selectedIds();
    return std::vector<uint32_t>(set.begin(), set.end());
}

void VirtualizedGridWidget::updateStatusText()
{
    size_t filtered = m_index.filteredCount();
    size_t total = m_index.totalCount();

    if (filtered == total) {
        m_statusLabel->setText(total == 1 ? QStringLiteral("1 wallpaper")
                                          : QStringLiteral("%1 wallpapers").arg(total));
    } else {
        m_statusLabel->setText(QStringLiteral("%1 of %2 wallpapers").arg(filtered).arg(total));
    }
}

} // namespace wwal::studio
