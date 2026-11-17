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
#include <QDir>
#include <QFile>
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
#include <QStandardPaths>

namespace wwal::studio {

namespace {

bool isOffscreenPlatform() { return QGuiApplication::platformName() == QLatin1String("offscreen"); }

void drawImageAspectFill(QPainter &painter, const QRect &target, const QImage &img)
{
    if (img.isNull() || target.isEmpty()) {
        return;
    }
    const QSize scaledSize = img.size().scaled(target.size(), Qt::KeepAspectRatioByExpanding);
    QRect drawRect(0, 0, scaledSize.width(), scaledSize.height());
    drawRect.moveCenter(target.center());
    painter.drawImage(drawRect, img);
}

QString ensureSampleAssetDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    QDir dir(base + QStringLiteral("/wwal_samples"));
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
    return dir.absolutePath();
}

void ensureSamplePng(const QString &path, uint32_t rgb)
{
    if (QFile::exists(path)) {
        return;
    }
    QImage img(64, 36, QImage::Format_RGBA8888);
    const QColor base((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    for (int y = 0; y < img.height(); ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(img.height() - 1);
        for (int x = 0; x < img.width(); ++x) {
            const float s = static_cast<float>(x) / static_cast<float>(img.width() - 1);
            const int r = std::clamp(static_cast<int>(base.red() * (0.65f + 0.35f * s)), 0, 255);
            const int g = std::clamp(static_cast<int>(base.green() * (0.65f + 0.35f * t)), 0, 255);
            const int b =
                std::clamp(static_cast<int>(base.blue() * (0.75f + 0.25f * (1.0f - t))), 0, 255);
            img.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    (void)img.save(path, "PNG");
}

void ensureSampleTextFile(const QString &path, const QByteArray &content)
{
    if (QFile::exists(path)) {
        return;
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(content);
        f.close();
    }
}

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
    setAccessibleName(QStringLiteral("Wallpaper Catalog Grid"));

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

void GridGlCanvas::releaseGlResources()
{
    if (m_cardVbo != 0) {
        glDeleteBuffers(1, &m_cardVbo);
        m_cardVbo = 0;
    }
    if (m_cardVao != 0) {
        glDeleteVertexArrays(1, &m_cardVao);
        m_cardVao = 0;
    }
    if (m_cardArrayProgram != 0) {
        glDeleteProgram(m_cardArrayProgram);
        m_cardArrayProgram = 0;
    }
    m_uArrayViewportLoc = -1;
    m_uArrayCardRectLoc = -1;
    m_uArrayTexLoc = -1;
    m_uArrayLayerLoc = -1;
    m_uArraySrgbLoc = -1;
    m_uArrayDprLoc = -1;
    std::fill(m_glRenderedSliceMask.begin(), m_glRenderedSliceMask.end(), uint8_t{0});
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

    // Clamp via int64_t so negative or extreme scroll_y never underflows size_t indices (AUD-055).
    const int64_t r_total = computeTotalRows(total_items, columns);
    const int64_t clamped_scroll = std::max<int64_t>(0, scroll_y);
    const int64_t safe_view_h = std::max<int64_t>(0, viewport_h);
    const int64_t row_h = ROW_HEIGHT;
    const int64_t raw_first = (clamped_scroll / row_h) - OVERSCAN_ROWS;
    const int64_t raw_last = ((clamped_scroll + safe_view_h + row_h - 1) / row_h) + OVERSCAN_ROWS;
    const int64_t r_first_i = std::clamp<int64_t>(raw_first, 0, r_total - 1);
    const int64_t r_last_i = std::clamp<int64_t>(raw_last, 0, r_total - 1);

    const size_t idx_first = static_cast<size_t>(columns) * static_cast<size_t>(r_first_i);
    const size_t idx_last = std::min(
        total_items - 1U, static_cast<size_t>(columns) * static_cast<size_t>(r_last_i + 1) - 1U);
    if (idx_first > idx_last) {
        return GridVisibleSlice{
            .is_empty = true, .row_first = 0, .row_last = 0, .index_first = 0, .index_last = 0};
    }

    return GridVisibleSlice{.is_empty = false,
                            .row_first = static_cast<int>(r_first_i),
                            .row_last = static_cast<int>(r_last_i),
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

    // Gutter begins strictly at card_w / ITEM_HEIGHT (AUD-218).
    if (in_card_x >= card_w || in_card_y >= ITEM_HEIGHT) {
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
        emit scrollYChanged(m_scrollY);
        update();
    }
}

void GridGlCanvas::toggleSelection(uint32_t item_id)
{
    if (m_selectedIds.erase(item_id) > 0U) {
        std::erase(m_selectedOrder, item_id);
    } else {
        m_selectedIds.insert(item_id);
        m_selectedOrder.push_back(item_id);
    }
    m_lastSelectedId = item_id;

    emit selectionChanged(m_selectedOrder);
    update();
}

void GridGlCanvas::selectRange(uint32_t from_id, uint32_t to_id)
{
    const auto fromPos = m_index.filteredPositionOfId(from_id);
    const auto toPos = m_index.filteredPositionOfId(to_id);
    if (fromPos.has_value() && toPos.has_value()) {
        const size_t lo = std::min(*fromPos, *toPos);
        const size_t hi = std::max(*fromPos, *toPos);
        for (size_t i = lo; i <= hi; ++i) {
            const uint32_t id = m_index.filteredItemAt(i).id;
            if (m_selectedIds.insert(id).second) {
                m_selectedOrder.push_back(id);
            }
        }
    } else if (toPos.has_value()) {
        if (m_selectedIds.insert(to_id).second) {
            m_selectedOrder.push_back(to_id);
        }
    }
    m_lastSelectedId = to_id;

    emit selectionChanged(m_selectedOrder);
    update();
}

void GridGlCanvas::clearSelection()
{
    if (!m_selectedIds.empty()) {
        m_selectedIds.clear();
        m_selectedOrder.clear();
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

    const char *fsArraySrc =
        isGles ? "#version 300 es\n"
                 "precision mediump float;\n"
                 "precision mediump sampler2DArray;\n"
                 "in vec2 v_uv;\n"
                 "uniform vec4 u_cardRect;\n"
                 "uniform sampler2DArray u_texArray;\n"
                 "uniform float u_texLayer;\n"
                 "uniform int u_srgbEncode;\n"
                 "uniform float u_dpr;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    float scale = max(1.0, u_dpr);\n"
                 "    vec2 halfPx = u_cardRect.zw * (0.5 * scale);\n"
                 "    vec2 pPx = (v_uv - 0.5) * u_cardRect.zw * scale;\n"
                 "    float radiusPx = 8.0 * scale;\n"
                 "    vec2 q = abs(pPx) - (halfPx - vec2(radiusPx));\n"
                 "    float distPx = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radiusPx;\n"
                 "    float alpha = clamp(0.5 - distPx, 0.0, 1.0);\n"
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
                 "uniform float u_dpr;\n"
                 "out vec4 fragColor;\n"
                 "void main() {\n"
                 "    float scale = max(1.0, u_dpr);\n"
                 "    vec2 halfPx = u_cardRect.zw * (0.5 * scale);\n"
                 "    vec2 pPx = (v_uv - 0.5) * u_cardRect.zw * scale;\n"
                 "    float radiusPx = 8.0 * scale;\n"
                 "    vec2 q = abs(pPx) - (halfPx - vec2(radiusPx));\n"
                 "    float distPx = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radiusPx;\n"
                 "    float alpha = clamp(0.5 - distPx, 0.0, 1.0);\n"
                 "    if (alpha <= 0.001) discard;\n"
                 "    vec3 col = texture(u_texArray, vec3(v_uv, u_texLayer)).rgb;\n"
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
            m_uArrayDprLoc = glGetUniformLocation(m_cardArrayProgram, "u_dpr");
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

void GridGlCanvas::renderGlThumbnails()
{
    m_glRenderedSliceMask.assign(m_glRenderedSliceMask.size(), uint8_t{0});
    if (m_cardArrayProgram == 0 || m_cardVao == 0 || !m_pool.isInitialized() ||
        m_pool.textureArrayId() == 0 || width() <= 0 || height() <= 0) {
        return;
    }

    const size_t total = m_index.filteredCount();
    const int cols = computeColumns(width());
    const GridVisibleSlice slice = computeVisibleSlice(total, cols, m_scrollY, height());
    if (slice.is_empty) {
        return;
    }

    m_glRenderedSliceStart = slice.index_first;
    m_glRenderedSliceMask.assign(slice.index_last - slice.index_first + 1U, uint8_t{0});

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_cardVao);

    glUseProgram(m_cardArrayProgram);
    glUniform2f(m_uArrayViewportLoc, static_cast<float>(width()), static_cast<float>(height()));
    glUniform1i(m_uArrayTexLoc, 0);
    if (m_uArraySrgbLoc >= 0) {
        glUniform1i(m_uArraySrgbLoc, (m_pool.isArrayEtc2() &&
                                      m_pool.arrayInternalFormat() == GL_COMPRESSED_SRGB8_ETC2)
                                         ? 1
                                         : 0);
    }
    if (m_uArrayDprLoc >= 0) {
        glUniform1f(m_uArrayDprLoc, static_cast<float>(std::max<qreal>(1.0, devicePixelRatioF())));
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_pool.textureArrayId());

    for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
        const auto &item = m_index.filteredItemAt(i);
        const QRect card = itemRect(i, cols, m_scrollY);
        if (card.bottom() < 0 || card.top() > height()) {
            continue;
        }

        const int layer = m_pool.requestTexture(item.id, item.path, item.preview_path);
        if (layer < 0) {
            continue;
        }

        const bool is_hovered = (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
        QRect render_rect = card;
        if (is_hovered) {
            const int dw = static_cast<int>(card.width() * 0.03f);
            const int dh = static_cast<int>(card.height() * 0.03f);
            render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
        }

        glUniform1f(m_uArrayLayerLoc, static_cast<float>(layer));
        glUniform4f(m_uArrayCardRectLoc, static_cast<float>(render_rect.x()),
                    static_cast<float>(render_rect.y()), static_cast<float>(render_rect.width()),
                    static_cast<float>(render_rect.height()));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        m_glRenderedSliceMask[i - m_glRenderedSliceStart] = uint8_t{1};
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

void GridGlCanvas::initializeGL()
{
    if (isOffscreenPlatform()) {
        return;
    }

    m_cardArrayProgram = 0;
    m_cardVao = 0;
    m_cardVbo = 0;
    m_uArrayViewportLoc = -1;
    m_uArrayCardRectLoc = -1;
    m_uArrayTexLoc = -1;
    m_uArrayLayerLoc = -1;
    m_uArraySrgbLoc = -1;
    m_uArrayDprLoc = -1;
    std::fill(m_glRenderedSliceMask.begin(), m_glRenderedSliceMask.end(), uint8_t{0});

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
    initCardShaders();

    m_glInitialized = true;
}

void GridGlCanvas::resizeGL(int w, int h)
{
    if (isOffscreenPlatform()) {
        const int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
        emit virtualHeightChanged(vh);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, std::max(1, qRound(w * dpr)), std::max(1, qRound(h * dpr)));
    const int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
    emit virtualHeightChanged(vh);
}

void GridGlCanvas::resizeEvent(QResizeEvent *event)
{
    if (isOffscreenPlatform()) {
        // Offscreen QPA lacks a GL backing context; stay on the QPainter card fallback without
        // letting QOpenGLWidgetPrivate::initialize() allocate and leak a failed QOpenGLContext.
        QWidget::resizeEvent(event);
        const int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
        emit virtualHeightChanged(vh);
        return;
    }
    QOpenGLWidget::resizeEvent(event);
    const int vh = computeVirtualHeight(m_index.filteredCount(), computeColumns(width()));
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
        glViewport(0, 0, std::max(1, qRound(width() * dpr)), std::max(1, qRound(height() * dpr)));
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        renderGlThumbnails();
    } else {
        std::fill(m_glRenderedSliceMask.begin(), m_glRenderedSliceMask.end(), uint8_t{0});
        m_pool.processPendingUploads(nullptr, 12);
    }

    QPainter painter(this);
    renderCards(painter);
}

void GridGlCanvas::paintEvent(QPaintEvent *event)
{
    if (!isOffscreenPlatform() && isValid()) {
        QOpenGLWidget::paintEvent(event);
        return;
    }
    if (width() <= 0 || height() <= 0) {
        return;
    }

    std::fill(m_glRenderedSliceMask.begin(), m_glRenderedSliceMask.end(), uint8_t{0});
    m_pool.processPendingUploads(nullptr, 12);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 23, 28));
    renderCards(painter);
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
        font.setPixelSize(12);
        font.setWeight(QFont::Medium);
        painter.setFont(font);
        painter.drawText(rect().adjusted(14, 14, -14, -14), Qt::AlignCenter | Qt::TextWordWrap,
                         "No wallpapers found matching query or tags.");
    } else {
        const bool canvasHasFocus = hasFocus();
        for (size_t i = slice.index_first; i <= slice.index_last; ++i) {
            const auto &item = m_index.filteredItemAt(i);
            const QRect card = itemRect(i, cols, m_scrollY);
            if (card.bottom() < 0 || card.top() > height()) {
                continue;
            }

            const bool is_hovered =
                (m_hoveredFilteredIdx.has_value() && *m_hoveredFilteredIdx == i);
            const bool is_focused =
                (canvasHasFocus && m_focusedFilteredIdx.has_value() && *m_focusedFilteredIdx == i);
            const bool is_selected = m_selectedIds.contains(item.id);
            bool renderedByGl = false;
            if (i >= m_glRenderedSliceStart) {
                const size_t rel = i - m_glRenderedSliceStart;
                if (rel < m_glRenderedSliceMask.size()) {
                    renderedByGl = (m_glRenderedSliceMask[rel] != 0U);
                }
            }

            QRect render_rect = card;
            if (is_hovered) {
                const int dw = static_cast<int>(card.width() * 0.03f);
                const int dh = static_cast<int>(card.height() * 0.03f);
                render_rect.adjust(-dw / 2, -dh / 2, dw / 2, dh / 2);
            }

            painter.save();
            QPainterPath card_clip;
            card_clip.addRoundedRect(QRectF(render_rect), 8.0, 8.0);
            painter.setClipPath(card_clip);

            if (!renderedByGl) {
                const uint32_t dc = item.dominant_color;
                const QColor base_color((dc >> 16) & 0xFF, (dc >> 8) & 0xFF, dc & 0xFF);
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
                const QImage thumb = m_pool.getThumbnailImage(item.id);
                if (!thumb.isNull()) {
                    drawImageAspectFill(painter, render_rect, thumb);
                }
            }

            if (m_hover.isPreviewActive() && m_hover.activeItemId() == item.id) {
                const QImage &previewImg = m_hover.currentPreviewImage();
                if (!previewImg.isNull()) {
                    drawImageAspectFill(painter, render_rect, previewImg);
                }
            }

            drawBadgePill(painter, render_rect, item.badge);
            drawCardSelectionIndicator(painter, render_rect, is_selected);
            drawCardTitleOverlay(painter, render_rect, item.title);

            painter.restore();

            if (is_focused || is_selected) {
                const QPen focus_pen(QColor(56, 189, 248), 2.0);
                painter.setPen(focus_pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
            } else if (is_hovered) {
                const QPen hover_pen(QColor(129, 140, 248, 220), 2.0);
                painter.setPen(hover_pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(render_rect.adjusted(1, 1, -1, -1), 7.5, 7.5);
            } else {
                const QPen border_pen(QColor(255, 255, 255, 28), 1.0);
                painter.setPen(border_pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(render_rect, 8.0, 8.0);
            }
        }
    }

    if (hasFocus() && rect().width() > 4 && rect().height() > 4) {
        painter.setPen(QPen(QColor(56, 189, 248), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4.0, 4.0);
    }
}

void GridGlCanvas::drawBadgePill(QPainter &painter, const QRect &card_rect, WallpaperBadge badge)
{
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setWeight(QFont::Bold);
    painter.setFont(font);

    QString label = QString::fromUtf8(badgeText(badge));
    QFontMetrics fm(font);
    int pill_w =
        std::clamp(fm.horizontalAdvance(label) + 12, 46, std::max(46, card_rect.width() - 44));
    QRect pill_rect(card_rect.left() + 8, card_rect.top() + 8, pill_w, 20);

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
    font.setPixelSize(11);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);

    QString qtitle = QString::fromStdString(title);
    QFontMetrics fm(font);
    QString elided = fm.elidedText(qtitle, Qt::ElideRight, text_rect.width());
    painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

void GridGlCanvas::ensureIndexVisible(size_t filtered_idx)
{
    if (width() <= 0 || height() <= 0 || filtered_idx >= m_index.filteredCount()) {
        return;
    }
    const int cols = computeColumns(width());
    const int row = static_cast<int>(filtered_idx / static_cast<size_t>(cols));
    const int cardTop = SPACING_Y + row * ROW_HEIGHT;
    const int cardBottom = cardTop + ITEM_HEIGHT;
    if (cardTop < m_scrollY + SPACING_Y) {
        setScrollY(std::max(0, cardTop - SPACING_Y));
    } else if (cardBottom > m_scrollY + height() - SPACING_Y) {
        setScrollY(cardBottom - height() + SPACING_Y);
    }
}

void GridGlCanvas::selectSingleFilteredIndex(size_t filtered_idx)
{
    if (filtered_idx >= m_index.filteredCount()) {
        return;
    }
    const CatalogItem &item = m_index.filteredItemAt(filtered_idx);
    m_focusedFilteredIdx = filtered_idx;
    ensureIndexVisible(filtered_idx);
    clearSelection();
    toggleSelection(item.id);
    emit itemSelected(item);
    update();
}

void GridGlCanvas::mouseMoveEvent(QMouseEvent *event)
{
    auto hit = itemAtPoint(event->pos());
    if (hit != m_hoveredFilteredIdx) {
        if (m_hoveredFilteredIdx.has_value()) {
            if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
                const uint32_t prev_id = m_index.filteredItemAt(*m_hoveredFilteredIdx).id;
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
    setFocus(Qt::MouseFocusReason);
    auto hit = itemAtPoint(event->pos());
    if (!hit.has_value()) {
        if (event->button() == Qt::RightButton) {
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->setStyleSheet("QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid "
                                "#334155; padding: 4px; }"
                                "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
                                "QMenu::item:selected { background: #0284c7; color: white; }");
            QAction *cfgAct = menu->addAction("Configure Wallpaper Folders && Exclusions...");
            QPointer<GridGlCanvas> safeSelf(this);
            connect(cfgAct, &QAction::triggered, this, [safeSelf]() {
                if (safeSelf) {
                    emit safeSelf->configureFoldersRequested();
                }
            });
            menu->popup(event->globalPosition().toPoint());
        }
        return;
    }

    m_focusedFilteredIdx = hit;
    const auto item = m_index.filteredItemAt(*hit);

    if (event->button() == Qt::RightButton) {
        clearSelection();
        toggleSelection(item.id);
        emit itemSelected(item);

        std::filesystem::path item_p(item.path);
        std::filesystem::path parent_dir = item_p.parent_path();
        QString parent_qstr = QString::fromStdString(parent_dir.string());
        QString parent_name = QString::fromStdString(parent_dir.filename().string());

        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(
            "QMenu { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; padding: 4px; "
            "}"
            "QMenu::item { padding: 5px 14px; border-radius: 4px; }"
            "QMenu::item:selected { background: #0284c7; color: white; }"
            "QMenu::separator { height: 1px; background: #334155; margin: 4px 2px; }");
        QAction *applyAct = menu->addAction("Apply Wallpaper Now");
        QPointer<GridGlCanvas> safeSelf(this);
        connect(applyAct, &QAction::triggered, this, [safeSelf, item]() {
            if (safeSelf) {
                emit safeSelf->itemDoubleClicked(item);
            }
        });
        if (!m_embeddedMode) {
            menu->addSeparator();
            if (!parent_qstr.isEmpty()) {
                QAction *exclAct = menu->addAction(
                    QStringLiteral("Exclude Folder '%1' (%2)").arg(parent_name, parent_qstr));
                connect(exclAct, &QAction::triggered, this, [safeSelf, parent_qstr]() {
                    if (safeSelf) {
                        emit safeSelf->excludeFolderRequested(parent_qstr);
                    }
                });
            }
            QAction *cfgAct = menu->addAction("Configure Wallpaper Folders && Exclusions...");
            connect(cfgAct, &QAction::triggered, this, [safeSelf]() {
                if (safeSelf) {
                    emit safeSelf->configureFoldersRequested();
                }
            });
        }
        menu->popup(event->globalPosition().toPoint());
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
        m_focusedFilteredIdx = hit;
        const auto &item = m_index.filteredItemAt(*hit);
        emit itemDoubleClicked(item);
    }
}

void GridGlCanvas::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (m_hoveredFilteredIdx.has_value()) {
        if (*m_hoveredFilteredIdx < m_index.filteredCount()) {
            const uint32_t id = m_index.filteredItemAt(*m_hoveredFilteredIdx).id;
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

void GridGlCanvas::keyPressEvent(QKeyEvent *event)
{
    const size_t total = m_index.filteredCount();
    if (total == 0U) {
        QOpenGLWidget::keyPressEvent(event);
        return;
    }

    const int cols = computeColumns(width());
    const size_t current = (m_focusedFilteredIdx.has_value() && *m_focusedFilteredIdx < total)
                               ? *m_focusedFilteredIdx
                               : 0U;
    const size_t visibleRows = static_cast<size_t>(std::max(1, height() / ROW_HEIGHT));
    const size_t pageStep = visibleRows * static_cast<size_t>(cols);

    std::optional<size_t> nextIdx;
    switch (event->key()) {
    case Qt::Key_Left:
        nextIdx = (current > 0U) ? (current - 1U) : 0U;
        break;
    case Qt::Key_Right:
        nextIdx = std::min(total - 1U, current + 1U);
        break;
    case Qt::Key_Up:
        nextIdx = (current >= static_cast<size_t>(cols)) ? (current - static_cast<size_t>(cols))
                                                         : current;
        break;
    case Qt::Key_Down:
        nextIdx = std::min(total - 1U, current + static_cast<size_t>(cols));
        break;
    case Qt::Key_PageUp:
        nextIdx = (current > pageStep) ? (current - pageStep) : 0U;
        break;
    case Qt::Key_PageDown:
        nextIdx = std::min(total - 1U, current + pageStep);
        break;
    case Qt::Key_Home:
        nextIdx = 0U;
        break;
    case Qt::Key_End:
        nextIdx = total - 1U;
        break;
    case Qt::Key_Space: {
        m_focusedFilteredIdx = current;
        ensureIndexVisible(current);
        const auto &item = m_index.filteredItemAt(current);
        toggleSelection(item.id);
        emit itemSelected(item);
        event->accept();
        return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        m_focusedFilteredIdx = current;
        ensureIndexVisible(current);
        const auto &item = m_index.filteredItemAt(current);
        emit itemDoubleClicked(item);
        event->accept();
        return;
    }
    case Qt::Key_A:
        if (event->modifiers() & Qt::ControlModifier) {
            const size_t limit = std::min<size_t>(total, 10000U);
            m_selectedIds.clear();
            m_selectedOrder.clear();
            m_selectedOrder.reserve(limit);
            for (size_t i = 0; i < limit; ++i) {
                const uint32_t id = m_index.filteredItemAt(i).id;
                if (m_selectedIds.insert(id).second) {
                    m_selectedOrder.push_back(id);
                }
            }
            if (!m_selectedOrder.empty()) {
                m_lastSelectedId = m_selectedOrder.back();
            }
            emit selectionChanged(m_selectedOrder);
            update();
            event->accept();
            return;
        }
        QOpenGLWidget::keyPressEvent(event);
        return;
    case Qt::Key_Escape:
        clearSelection();
        event->accept();
        return;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }

    if (nextIdx.has_value()) {
        if ((event->modifiers() & Qt::ShiftModifier) && m_lastSelectedId.has_value()) {
            m_focusedFilteredIdx = *nextIdx;
            ensureIndexVisible(*nextIdx);
            const auto &item = m_index.filteredItemAt(*nextIdx);
            selectRange(*m_lastSelectedId, item.id);
            emit itemSelected(item);
        } else {
            selectSingleFilteredIndex(*nextIdx);
        }
        event->accept();
    }
}

void GridGlCanvas::focusInEvent(QFocusEvent *event)
{
    if (!m_focusedFilteredIdx.has_value() && m_index.filteredCount() > 0U) {
        m_focusedFilteredIdx = 0U;
    }
    update();
    QOpenGLWidget::focusInEvent(event);
}

void GridGlCanvas::focusOutEvent(QFocusEvent *event)
{
    update();
    QOpenGLWidget::focusOutEvent(event);
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
    if (m_searchDebounceTimer) {
        m_searchDebounceTimer->stop();
    }
    if (m_folderDialog) {
        m_folderDialog->close();
        m_folderDialog.clear();
    }
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
    header_layout->setSpacing(6);

    m_searchEdit = new QLineEdit(m_headerContainer);
    m_searchEdit->setPlaceholderText("Search wallpapers...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Search wallpapers"));
    m_searchEdit->setMinimumHeight(28);
    m_searchEdit->setStyleSheet(
        "QLineEdit { background: #14171f; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 4px 10px; font-size: 11px; }"
        "QLineEdit:focus { border: 2px solid #38bdf8; }");

    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(16);
    connect(m_searchDebounceTimer, &QTimer::timeout, this,
            [this]() { applySearchText(m_pendingSearchText); });

    connect(m_searchEdit, &QLineEdit::textChanged, this,
            &VirtualizedGridWidget::onSearchTextChanged);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_searchDebounceTimer) {
            m_searchDebounceTimer->stop();
        }
        applySearchText(m_searchEdit->text());
    });
    header_layout->addWidget(m_searchEdit);

    const QString comboStyle = QStringLiteral(
        "QComboBox { background: #14171f; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 2px 8px; font-size: 11px; }"
        "QComboBox:focus { border: 2px solid #38bdf8; }");

    auto configureCombo = [&comboStyle](QComboBox *cb, const QString &accName) {
        cb->setMinimumHeight(28);
        cb->setMinimumWidth(0);
        cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cb->setMinimumContentsLength(5);
        cb->setAccessibleName(accName);
        cb->setStyleSheet(comboStyle);
    };

    auto *filter_row = new QHBoxLayout();
    filter_row->setContentsMargins(0, 0, 0, 0);
    filter_row->setSpacing(6);

    m_tagCombo = new QComboBox(m_headerContainer);
    m_tagCombo->addItem("All Tags", QVariant::fromValue<qulonglong>(WallpaperTags::None));
    for (const auto &tag : WallpaperTags::allKnownTags()) {
        m_tagCombo->addItem(QString::fromUtf8(tag.display_name),
                            QVariant::fromValue<qulonglong>(tag.mask));
    }
    configureCombo(m_tagCombo, QStringLiteral("Tag Filter"));
    connect(m_tagCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        const QVariant v = m_tagCombo->itemData(idx);
        if (!v.isValid()) {
            return;
        }
        setTagFilter(v.toULongLong());
    });
    filter_row->addWidget(m_tagCombo, 1);

    m_badgeCombo = new QComboBox(m_headerContainer);
    m_badgeCombo->addItem("All Types", QVariant::fromValue<uint>(0xFFFFFFFFu));
    m_badgeCombo->addItem("Image",
                          QVariant::fromValue<uint>(static_cast<uint>(WallpaperBadge::Image)));
    m_badgeCombo->addItem("Video",
                          QVariant::fromValue<uint>(static_cast<uint>(WallpaperBadge::Video)));
    m_badgeCombo->addItem("Shader",
                          QVariant::fromValue<uint>(static_cast<uint>(WallpaperBadge::Shader)));
    m_badgeCombo->addItem("Scene",
                          QVariant::fromValue<uint>(static_cast<uint>(WallpaperBadge::Scene)));
    m_badgeCombo->addItem("XML",
                          QVariant::fromValue<uint>(static_cast<uint>(WallpaperBadge::GnomeXml)));
    configureCombo(m_badgeCombo, QStringLiteral("Type Filter"));
    connect(m_badgeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                const QVariant v = m_badgeCombo->itemData(idx);
                if (!v.isValid()) {
                    return;
                }
                setBadgeFilter(v.toUInt());
            });
    filter_row->addWidget(m_badgeCombo, 1);
    header_layout->addLayout(filter_row);

    auto *sort_row = new QHBoxLayout();
    sort_row->setContentsMargins(0, 0, 0, 0);
    sort_row->setSpacing(6);

    m_sortCombo = new QComboBox(m_headerContainer);
    m_sortCombo->addItem("Name (A-Z)", static_cast<int>(SortCriteria::NameAsc));
    m_sortCombo->addItem("Name (Z-A)", static_cast<int>(SortCriteria::NameDesc));
    m_sortCombo->addItem("Newest", static_cast<int>(SortCriteria::DateNewest));
    m_sortCombo->addItem("Oldest", static_cast<int>(SortCriteria::DateOldest));
    m_sortCombo->addItem("Largest", static_cast<int>(SortCriteria::SizeLargest));
    m_sortCombo->addItem("Smallest", static_cast<int>(SortCriteria::SizeSmallest));
    m_sortCombo->addItem("Type", static_cast<int>(SortCriteria::Type));
    m_sortCombo->addItem("Color", static_cast<int>(SortCriteria::DominantHue));
    configureCombo(m_sortCombo, QStringLiteral("Sort Order"));
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VirtualizedGridWidget::onSortChanged);
    sort_row->addWidget(m_sortCombo, 1);

    m_foldersButton = new QPushButton("Folders...", m_headerContainer);
    m_foldersButton->setMinimumHeight(28);
    m_foldersButton->setToolTip("Choose which wallpaper folders to include or exclude");
    m_foldersButton->setAccessibleName(QStringLiteral("Configure Wallpaper Folders"));
    m_foldersButton->setStyleSheet(
        "QPushButton { background: #20242e; color: #f1f5f9; border: 1px solid #2e3545; "
        "border-radius: 5px; padding: 2px 10px; font-size: 11px; font-weight: 500; }"
        "QPushButton:hover { background: #2a303d; border-color: #38bdf8; color: #ffffff; }"
        "QPushButton:focus { border: 2px solid #38bdf8; }");
    connect(m_foldersButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::openFolderSettingsDialog);
    sort_row->addWidget(m_foldersButton);

    header_layout->addLayout(sort_row);
    root_layout->addWidget(m_headerContainer);

    m_statusLabel = new QLabel("0 wallpapers", this);
    m_statusLabel->setStyleSheet("color: #94a3b8; font-size: 11px;");
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
    m_scrollBar->setAccessibleName(QStringLiteral("Catalog Scrollbar"));
    m_scrollBar->setStyleSheet(
        "QScrollBar:vertical { background: #14171f; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #2e3545; min-height: 24px; border-radius: 4px; }"
        "QScrollBar::handle:vertical:hover { background: #475569; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    connect(m_scrollBar, &QScrollBar::valueChanged, this,
            &VirtualizedGridWidget::onScrollValueChanged);
    connect(m_canvas, &GridGlCanvas::scrollYChanged, this, [this](int y) {
        if (m_scrollBar && m_scrollBar->value() != y) {
            const QSignalBlocker blocker(m_scrollBar);
            m_scrollBar->setValue(y);
        }
    });
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
    m_playlistAddButton->setMinimumHeight(28);
    m_playlistAddButton->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: none; border-radius: 4px; "
        "padding: 4px 12px; font-weight: 600; font-size: 11px; }"
        "QPushButton:hover { background: #0369a1; }"
        "QPushButton:focus { border: 2px solid #38bdf8; }");
    connect(m_playlistAddButton, &QPushButton::clicked, this,
            &VirtualizedGridWidget::onPlaylistAddClicked);
    pl_layout->addWidget(m_playlistAddButton);

    m_playlistClearButton = new QPushButton("Clear", m_playlistBar);
    m_playlistClearButton->setMinimumHeight(28);
    m_playlistClearButton->setStyleSheet(
        "QPushButton { background: #20242e; color: #94a3b8; "
        "border: 1px solid #2e3545; border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
        "QPushButton:hover { color: #f1f5f9; border-color: #38bdf8; }"
        "QPushButton:focus { border: 2px solid #38bdf8; }");
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
            const QVariant v = m_tagCombo->itemData(i);
            if (v.isValid() && v.toULongLong() == tag_mask) {
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
        const int cols = m_canvas->computeColumns(m_canvas->width());
        const int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
        onVirtualHeightChanged(vh);
        updateStatusText();
        m_canvas->update();
    }
}

void VirtualizedGridWidget::setBadgeFilter(uint32_t badge_mask)
{
    if (m_badgeCombo) {
        QSignalBlocker blocker(m_badgeCombo);
        int matchedIdx = 0;
        for (int i = 0; i < m_badgeCombo->count(); ++i) {
            const QVariant v = m_badgeCombo->itemData(i);
            if (v.isValid() && v.toUInt() == badge_mask) {
                matchedIdx = i;
                break;
            }
        }
        m_badgeCombo->setCurrentIndex(matchedIdx);
    }

    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setBadgeFilter(badge_mask);
    if (m_canvas) {
        const int cols = m_canvas->computeColumns(m_canvas->width());
        const int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
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
    const int cols = m_canvas->computeColumns(m_canvas->width());
    const int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
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
    m_pool.clear();
    const size_t count = m_index.scanAll(extra_paths);
    refreshCatalogView();
    emit catalogScanned(count);
}

void VirtualizedGridWidget::populateSampleCatalogIfEmpty(bool force_showcase_cards)
{
    if (!force_showcase_cards && m_index.totalCount() > 0) {
        return;
    }

    const QString sampleDir = ensureSampleAssetDir();

    struct SampleDef {
        const char *title;
        const char *filename;
        const char *previewName;
        WallpaperBadge badge;
        uint32_t flags;
        uint32_t color;
        uint64_t tags;
    };

    const SampleDef samples[] = {
        {"01 Cyberpunk Neon Rainstorm", "cyberpunk_neon.glsl", "cyberpunk_neon_preview.png",
         WallpaperBadge::Shader, WAYWAL_THUMB_FLAG_SHADER, 0x0ea5e9,
         WallpaperTags::Cyberpunk | WallpaperTags::Dark | WallpaperTags::Urban},
        {"02 Abyssal Bioluminescence", "abyssal_reef.json", "abyssal_reef_preview.png",
         WallpaperBadge::Scene, WAYWAL_THUMB_FLAG_SCENE, 0x6366f1,
         WallpaperTags::Nature | WallpaperTags::Dark | WallpaperTags::Fantasy},
        {"03 Solarized Horizon 4K", "solarized_horizon.png", "solarized_horizon.png",
         WallpaperBadge::Image, WAYWAL_THUMB_FLAG_NONE, 0xf97316,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Nature},
        {"04 Quantum Fluid Vortex", "quantum_fluid.glsl", "quantum_fluid_preview.png",
         WallpaperBadge::Shader, WAYWAL_THUMB_FLAG_SHADER, 0x10b981,
         WallpaperTags::Abstract | WallpaperTags::SciFi | WallpaperTags::Dark},
        {"05 Tokyo Midnight Shinjuku", "tokyo_shinjuku.mp4", "tokyo_shinjuku_preview.png",
         WallpaperBadge::Video, WAYWAL_THUMB_FLAG_VIDEO, 0xa855f7,
         WallpaperTags::Urban | WallpaperTags::Cyberpunk | WallpaperTags::Photography},
        {"06 Celestial Nebula Drift", "celestial_nebula.json", "celestial_nebula_preview.png",
         WallpaperBadge::Scene, WAYWAL_THUMB_FLAG_SCENE, 0xec4899,
         WallpaperTags::Space | WallpaperTags::SciFi | WallpaperTags::DigitalArt},
        {"07 Nordic Fjord Minimal", "nordic_fjord.png", "nordic_fjord.png", WallpaperBadge::Image,
         WAYWAL_THUMB_FLAG_NONE, 0x3b82f6,
         WallpaperTags::Landscape | WallpaperTags::Minimal | WallpaperTags::Light},
        {"08 Synthwave Outrun Grid", "synthwave_grid.glsl", "synthwave_grid_preview.png",
         WallpaperBadge::Shader, WAYWAL_THUMB_FLAG_SHADER, 0xd946ef,
         WallpaperTags::Retro | WallpaperTags::Cyberpunk | WallpaperTags::Vehicles},
    };

    const int64_t base_time = 1710000000;
    for (size_t i = 0; i < std::size(samples); ++i) {
        const QString assetPath = sampleDir + QLatin1Char('/') + QLatin1String(samples[i].filename);
        const QString previewPath =
            sampleDir + QLatin1Char('/') + QLatin1String(samples[i].previewName);
        ensureSamplePng(previewPath, samples[i].color);
        if (samples[i].badge == WallpaperBadge::Shader) {
            ensureSampleTextFile(assetPath, "void mainImage(out vec4 fragColor, in vec2 fragCoord) "
                                            "{ fragColor = vec4(0.1, 0.5, 0.9, 1.0); }\n");
        } else if (samples[i].badge == WallpaperBadge::Scene) {
            ensureSampleTextFile(assetPath, "{\"title\":\"Sample Scene\",\"type\":\"scene\"}\n");
        } else if (samples[i].badge == WallpaperBadge::Video) {
            ensureSampleTextFile(assetPath, "WWAL-SAMPLE-VIDEO-PLACEHOLDER\n");
        }

        CatalogItem item;
        item.title = samples[i].title;
        item.path = assetPath.toStdString();
        item.preview_path = previewPath.toStdString();
        item.badge = samples[i].badge;
        item.flags = samples[i].flags;
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
    if (m_folderDialog) {
        m_folderDialog->raise();
        m_folderDialog->activateWindow();
        return;
    }

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setWindowTitle("Wallpaper Folders & Exclusions");
    dlg->setMinimumSize(540, 480);
    m_folderDialog = dlg;

    dlg->setStyleSheet(
        "QDialog { background: #14171d; color: #f1f5f9; }"
        "QLabel { color: #f1f5f9; font-size: 11px; }"
        "QListWidget { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px; font-size: 11px; }"
        "QLineEdit { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 4px 8px; font-size: 11px; }"
        "QLineEdit:focus { border: 2px solid #38bdf8; }"
        "QPushButton { background: #1e222b; color: #f1f5f9; border: 1px solid #334155; "
        "border-radius: 5px; padding: 5px 10px; font-size: 11px; font-weight: 600; min-height: "
        "28px; }"
        "QPushButton:hover { background: #283040; border-color: #38bdf8; }"
        "QPushButton:focus { border: 2px solid #38bdf8; }"
        "QCheckBox { color: #f1f5f9; font-size: 11px; spacing: 6px; }");

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *incHeader =
        new QLabel("<b>Included Wallpaper Folders</b> (only these folders are scanned):", dlg);
    layout->addWidget(incHeader);

    auto *incList = new QListWidget(dlg);
    for (const auto &dir : m_index.includedDirectories()) {
        incList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(incList, 1);

    auto *incBtnRow = new QHBoxLayout();
    auto *addIncBtn = new QPushButton("+ Add Folder...", dlg);
    auto *onlyWallpapersBtn = new QPushButton("Only ~/Pictures/Wallpapers", dlg);
    auto *remIncBtn = new QPushButton("Remove Selected", dlg);
    incBtnRow->addWidget(addIncBtn);
    incBtnRow->addWidget(onlyWallpapersBtn);
    incBtnRow->addWidget(remIncBtn);
    incBtnRow->addStretch();
    layout->addLayout(incBtnRow);

    QPointer<QDialog> safeDlg(dlg);
    QPointer<QListWidget> safeIncList(incList);
    connect(addIncBtn, &QPushButton::clicked, dlg, [safeDlg, safeIncList]() {
        if (!safeDlg || !safeIncList) {
            return;
        }
        auto *fd = new QFileDialog(safeDlg, QStringLiteral("Select Wallpaper Folder to Include"));
        fd->setAttribute(Qt::WA_DeleteOnClose);
        fd->setFileMode(QFileDialog::Directory);
        fd->setOption(QFileDialog::ShowDirsOnly, true);
        connect(fd, &QFileDialog::fileSelected, safeIncList, [safeIncList](const QString &dir) {
            if (safeIncList && !dir.isEmpty()) {
                safeIncList->addItem(dir);
            }
        });
        fd->open();
    });
    connect(onlyWallpapersBtn, &QPushButton::clicked, dlg, [safeIncList]() {
        if (!safeIncList) {
            return;
        }
        safeIncList->clear();
        safeIncList->addItem(
            QString::fromStdString(FuzzyCatalogIndex::expandUserPath("~/Pictures/Wallpapers")));
    });
    connect(remIncBtn, &QPushButton::clicked, dlg, [safeIncList]() {
        if (safeIncList) {
            qDeleteAll(safeIncList->selectedItems());
        }
    });

    auto *excHeader =
        new QLabel("<b>Excluded Folders or Folder Names</b> (e.g. <code>Screenshots</code>, "
                   "<code>~/Pictures/Camera</code>):",
                   dlg);
    layout->addWidget(excHeader);

    auto *excList = new QListWidget(dlg);
    for (const auto &dir : m_index.excludedDirectories()) {
        excList->addItem(QString::fromStdString(dir));
    }
    layout->addWidget(excList, 1);

    auto *excInputRow = new QHBoxLayout();
    auto *excPatternEdit = new QLineEdit(dlg);
    excPatternEdit->setPlaceholderText(
        "Folder name or path to exclude (e.g. Screenshots, Memes)...");
    auto *addExcPatternBtn = new QPushButton("+ Exclude Name/Path", dlg);
    auto *addExcDirBtn = new QPushButton("Browse Folder...", dlg);
    auto *remExcBtn = new QPushButton("Remove Selected", dlg);
    excInputRow->addWidget(excPatternEdit, 1);
    excInputRow->addWidget(addExcPatternBtn);
    excInputRow->addWidget(addExcDirBtn);
    excInputRow->addWidget(remExcBtn);
    layout->addLayout(excInputRow);

    QPointer<QLineEdit> safeExcEdit(excPatternEdit);
    QPointer<QListWidget> safeExcList(excList);
    connect(addExcPatternBtn, &QPushButton::clicked, dlg, [safeExcEdit, safeExcList]() {
        if (!safeExcEdit || !safeExcList) {
            return;
        }
        const QString txt = safeExcEdit->text().trimmed();
        if (!txt.isEmpty()) {
            safeExcList->addItem(txt);
            safeExcEdit->clear();
        }
    });
    connect(excPatternEdit, &QLineEdit::returnPressed, addExcPatternBtn, &QPushButton::click);
    connect(addExcDirBtn, &QPushButton::clicked, dlg, [safeDlg, safeExcList]() {
        if (!safeDlg || !safeExcList) {
            return;
        }
        auto *fd = new QFileDialog(safeDlg, QStringLiteral("Select Folder to Exclude"));
        fd->setAttribute(Qt::WA_DeleteOnClose);
        fd->setFileMode(QFileDialog::Directory);
        fd->setOption(QFileDialog::ShowDirsOnly, true);
        connect(fd, &QFileDialog::fileSelected, safeExcList, [safeExcList](const QString &dir) {
            if (safeExcList && !dir.isEmpty()) {
                safeExcList->addItem(dir);
            }
        });
        fd->open();
    });
    connect(remExcBtn, &QPushButton::clicked, dlg, [safeExcList]() {
        if (safeExcList) {
            qDeleteAll(safeExcList->selectedItems());
        }
    });

    auto *sysCheck =
        new QCheckBox("Also scan system (/usr/share/backgrounds) and Steam Workshop folders", dlg);
    sysCheck->setChecked(m_index.scanSystemAndWorkshopPaths());
    layout->addWidget(sysCheck);

    auto *cfgPathNote = new QLabel(
        QStringLiteral("Saved to: <code>%1</code> (or set <code>WWAL_WALLPAPER_DIRS</code> / "
                       "<code>WWAL_EXCLUDE_DIRS</code>)")
            .arg(QString::fromStdString(FuzzyCatalogIndex::defaultConfigFilePath())),
        dlg);
    cfgPathNote->setStyleSheet("color: #94a3b8; font-size: 11px;");
    cfgPathNote->setWordWrap(true);
    layout->addWidget(cfgPathNote);

    auto *bottomRow = new QHBoxLayout();
    auto *resetBtn = new QPushButton("Reset Defaults", dlg);
    bottomRow->addWidget(resetBtn);
    bottomRow->addStretch();

    auto *cancelBtn = new QPushButton("Cancel", dlg);
    auto *saveBtn = new QPushButton("Save && Rescan", dlg);
    saveBtn->setStyleSheet(
        "QPushButton { background: #0284c7; color: white; border: 1px solid #38bdf8; "
        "border-radius: 5px; padding: 6px 14px; font-weight: bold; min-height: 28px; }"
        "QPushButton:hover { background: #0369a1; }");
    bottomRow->addWidget(cancelBtn);
    bottomRow->addWidget(saveBtn);
    layout->addLayout(bottomRow);

    QPointer<QCheckBox> safeSysCheck(sysCheck);
    connect(resetBtn, &QPushButton::clicked, dlg, [safeIncList, safeExcList, safeSysCheck]() {
        if (!safeIncList || !safeExcList || !safeSysCheck) {
            return;
        }
        FuzzyCatalogIndex defaultsIdx;
        defaultsIdx.resetFolderConfigToDefaults();
        safeIncList->clear();
        for (const auto &d : defaultsIdx.includedDirectories()) {
            safeIncList->addItem(QString::fromStdString(d));
        }
        safeExcList->clear();
        for (const auto &d : defaultsIdx.excludedDirectories()) {
            safeExcList->addItem(QString::fromStdString(d));
        }
        safeSysCheck->setChecked(defaultsIdx.scanSystemAndWorkshopPaths());
    });
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    QPointer<VirtualizedGridWidget> safeSelf(this);
    connect(dlg, &QDialog::accepted, this, [safeSelf, safeIncList, safeExcList, safeSysCheck]() {
        if (!safeSelf || !safeIncList || !safeExcList || !safeSysCheck) {
            return;
        }
        std::vector<std::string> newInc;
        newInc.reserve(static_cast<size_t>(safeIncList->count()));
        for (int i = 0; i < safeIncList->count(); ++i) {
            newInc.push_back(safeIncList->item(i)->text().toStdString());
        }
        std::vector<std::string> newExc;
        newExc.reserve(static_cast<size_t>(safeExcList->count()));
        for (int i = 0; i < safeExcList->count(); ++i) {
            newExc.push_back(safeExcList->item(i)->text().toStdString());
        }

        safeSelf->m_index.setIncludedDirectories(newInc);
        safeSelf->m_index.setExcludedDirectories(newExc);
        safeSelf->m_index.setScanSystemAndWorkshopPaths(safeSysCheck->isChecked());
        (void)safeSelf->m_index.saveConfigToFile();

        safeSelf->scanWallpapers();
        safeSelf->selectFirstItemIfAvailable();
    });
    dlg->open();
}

void VirtualizedGridWidget::applySearchText(const QString &text)
{
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setSearchQuery(text.toStdString());
    if (m_canvas) {
        const int cols = m_canvas->computeColumns(m_canvas->width());
        const int vh = m_canvas->computeVirtualHeight(m_index.filteredCount(), cols);
        onVirtualHeightChanged(vh);
        updateStatusText();
        m_canvas->update();
    }
}

void VirtualizedGridWidget::onSearchTextChanged(const QString &text)
{
    m_pendingSearchText = text;
    if (m_searchDebounceTimer) {
        m_searchDebounceTimer->start();
    } else {
        applySearchText(text);
    }
}

void VirtualizedGridWidget::onSortChanged(int combo_index)
{
    const QVariant sortVar = m_sortCombo ? m_sortCombo->itemData(combo_index) : QVariant{};
    if (!sortVar.isValid()) {
        return;
    }
    const int rawVal = sortVar.toInt();
    if (rawVal < static_cast<int>(SortCriteria::NameAsc) ||
        rawVal > static_cast<int>(SortCriteria::DominantHue)) {
        return;
    }
    if (m_canvas) {
        m_canvas->resetHoverState();
    }
    m_index.setSorting(static_cast<SortCriteria>(rawVal));
    if (m_canvas) {
        m_canvas->update();
    }
}

void VirtualizedGridWidget::onScrollValueChanged(int value) { m_canvas->setScrollY(value); }

void VirtualizedGridWidget::onVirtualHeightChanged(int virtual_height)
{
    if (!m_canvas || !m_scrollBar) {
        return;
    }
    const int max_val = std::max(0, virtual_height - m_canvas->height());
    const int clampedScroll = std::clamp(m_canvas->scrollY(), 0, max_val);
    if (clampedScroll != m_canvas->scrollY()) {
        m_canvas->setScrollY(clampedScroll);
    }
    const QSignalBlocker blocker(m_scrollBar);
    m_scrollBar->setRange(0, max_val);
    m_scrollBar->setPageStep(m_canvas->height());
    m_scrollBar->setValue(m_canvas->scrollY());
    m_scrollBar->setVisible(max_val > 0);
}

void VirtualizedGridWidget::onPlaylistAddClicked()
{
    const auto ids = selectedIds();
    if (ids.empty()) {
        return;
    }
    for (uint32_t id : ids) {
        if (std::find(m_playlistQueue.begin(), m_playlistQueue.end(), id) ==
            m_playlistQueue.end()) {
            m_playlistQueue.push_back(id);
        }
    }
    if (m_playlistSelectionLabel) {
        m_playlistSelectionLabel->setText(
            QStringLiteral("%1 in playlist").arg(m_playlistQueue.size()));
    }
    emit playlistAddRequested(ids);
    emit playlistSelectionChanged(ids);
}

void VirtualizedGridWidget::clearSelection() { m_canvas->clearSelection(); }

std::vector<uint32_t> VirtualizedGridWidget::selectedIds() const { return m_canvas->selectedIds(); }

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
