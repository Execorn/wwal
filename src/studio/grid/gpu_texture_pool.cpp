// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_texture_pool.hpp"
#include "etc2_simd_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <QImage>
#include <QDir>
#include <QProcess>
#include <QCryptographicHash>

#include <waywal/thumb_cache.h>
#include <waywal/image_loader.h>

#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#endif
#ifndef GL_COMPRESSED_SRGB8_ETC2
#define GL_COMPRESSED_SRGB8_ETC2 0x9275
#endif

namespace wwal::studio {

namespace {

std::string computeSha256Hex(const std::string &input)
{
    QByteArray hash = QCryptographicHash::hash(
        QByteArray::fromRawData(input.data(), static_cast<qsizetype>(input.size())),
        QCryptographicHash::Sha256);
    return hash.toHex().toStdString();
}

std::string resolveDiskCachePath(const std::string &media_path)
{
    const char *xdg_cache = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path cache_dir;
    if (xdg_cache && xdg_cache[0] != '\0') {
        cache_dir = std::filesystem::path(xdg_cache) / "wwal" / "thumbnails";
    } else {
        const char *home = std::getenv("HOME");
        cache_dir = std::filesystem::path(home ? home : "/tmp") / ".cache" / "wwal" / "thumbnails";
    }

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    std::string hash_hex = computeSha256Hex(media_path);
    return (cache_dir / (hash_hex + ".webp")).string();
}

std::string toLowerExt(const std::filesystem::path &p)
{
    std::string ext = p.extension().string();
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return ext;
}

std::string resolveGnomeXmlFirstImage(const std::string &xml_path)
{
    std::ifstream f(xml_path);
    if (!f.is_open())
        return {};
    std::string line;
    const char *tags[] = {"<file>", "<from>"};
    const char *end_tags[] = {"</file>", "</from>"};
    while (std::getline(f, line)) {
        for (int t = 0; t < 2; ++t) {
            auto pos = line.find(tags[t]);
            if (pos != std::string::npos) {
                pos += std::strlen(tags[t]);
                auto end_pos = line.find(end_tags[t], pos);
                if (end_pos != std::string::npos && end_pos > pos) {
                    std::string candidate = line.substr(pos, end_pos - pos);
                    std::error_code ec;
                    if (std::filesystem::exists(candidate, ec)) {
                        return candidate;
                    }
                }
            }
        }
    }
    return {};
}

QImage loadQImageWithCoreFallback(const std::string &path)
{
    QImage img(QString::fromStdString(path));
    if (!img.isNull()) {
        return img;
    }

    uint8_t *pixels = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    if (image_load_rgba(path.c_str(), &pixels, &w, &h) && pixels && w > 0 && h > 0) {
        QImage raw_wrap(pixels, static_cast<int>(w), static_cast<int>(h),
                        static_cast<qsizetype>(w) * 4, QImage::Format_RGBA8888);
        QImage copy = raw_wrap.copy();
        image_free(pixels);
        return copy;
    }
    if (pixels) {
        image_free(pixels);
    }
    return {};
}

QImage scaleAndCenterCrop(const QImage &src, uint32_t target_w, uint32_t target_h)
{
    if (src.isNull())
        return {};
    QImage scaled = src.scaled(static_cast<int>(target_w), static_cast<int>(target_h),
                               Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.width() > static_cast<int>(target_w) ||
        scaled.height() > static_cast<int>(target_h)) {
        int cx = (scaled.width() - static_cast<int>(target_w)) / 2;
        int cy = (scaled.height() - static_cast<int>(target_h)) / 2;
        scaled = scaled.copy(cx, cy, static_cast<int>(target_w), static_cast<int>(target_h));
    }
    return scaled.convertToFormat(QImage::Format_RGBA8888);
}

void generateShaderThumbnail(const std::string &path, uint8_t *out_rgba, uint32_t w, uint32_t h)
{
    uint32_t hash = 2166136261u;
    for (char c : path) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    float phase = static_cast<float>(hash & 0xFFFFu) / 65535.0f * 6.2831853f;
    float freq = 2.5f + static_cast<float>((hash >> 16) & 0xFFu) / 255.0f * 4.0f;

    for (uint32_t y = 0; y < h; ++y) {
        float ny = (2.0f * static_cast<float>(y) - static_cast<float>(h)) / static_cast<float>(h);
        for (uint32_t x = 0; x < w; ++x) {
            float nx =
                (2.0f * static_cast<float>(x) - static_cast<float>(w)) / static_cast<float>(h);
            float d = std::sqrt(nx * nx + ny * ny);
            float a = std::atan2(ny, nx);
            float v = std::sin(freq * d - phase + std::sin(3.0f * a + phase));
            float r = (0.45f + 0.45f * std::cos(phase + a + 0.0f)) * (0.65f + 0.35f * v);
            float g = (0.45f + 0.45f * std::cos(phase + a + 2.1f)) * (0.65f + 0.35f * v);
            float b = (0.50f + 0.45f * std::cos(phase + a + 4.2f)) * (0.65f + 0.35f * v);

            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            out_rgba[idx + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
            out_rgba[idx + 1] = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
            out_rgba[idx + 2] = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
            out_rgba[idx + 3] = 255;
        }
    }
}

} // namespace

void PboUploadQueue::init(QOpenGLExtraFunctions *gl, size_t initial_bytes)
{
    if (initialized || !gl)
        return;

    buffer_size = initial_bytes;
    gl->glGenBuffers(2, pbo_ids);
    for (size_t i = 0; i < 2; ++i) {
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_ids[i]);
        gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(buffer_size), nullptr,
                         GL_STREAM_DRAW);
        pbo_capacities[i] = buffer_size;
    }
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    active_pbo = 0;
    initialized = true;
}

void PboUploadQueue::destroy(QOpenGLExtraFunctions *gl)
{
    if (initialized && gl) {
        gl->glDeleteBuffers(2, pbo_ids);
        pbo_ids[0] = 0;
        pbo_ids[1] = 0;
        pbo_capacities = {0, 0};
        initialized = false;
    }
}

void PboUploadQueue::uploadAsync(QOpenGLExtraFunctions *gl, const uint8_t *payload,
                                 size_t payload_bytes, GLuint target_tex_layer, GLuint tex_array_id,
                                 bool is_etc2, GLenum internal_fmt)
{
    if (!initialized || !gl || !payload || payload_bytes == 0 || tex_array_id == 0)
        return;

    // QPainter on QOpenGLWidget mutates unpack row length/skip state on the shared context.
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#ifdef GL_UNPACK_SKIP_PIXELS
    gl->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#ifdef GL_UNPACK_SKIP_ROWS
    gl->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif

    const size_t slot = static_cast<size_t>(active_pbo & 1u);
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_ids[slot]);
    if (payload_bytes > pbo_capacities[slot]) {
        pbo_capacities[slot] = payload_bytes;
        gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(payload_bytes), nullptr,
                         GL_STREAM_DRAW);
    }

    void *gpu_ptr =
        gl->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payload_bytes),
                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (gpu_ptr) {
        std::memcpy(gpu_ptr, payload, payload_bytes);
        gl->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    } else {
        gl->glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(payload_bytes),
                            payload);
    }

    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tex_array_id);
    if (is_etc2) {
        gl->glCompressedTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(target_tex_layer),
            static_cast<GLsizei>(GpuTexturePool::THUMB_WIDTH),
            static_cast<GLsizei>(GpuTexturePool::THUMB_HEIGHT), 1, internal_fmt,
            static_cast<GLsizei>(payload_bytes), nullptr);
    } else {
        gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(target_tex_layer),
                            static_cast<GLsizei>(GpuTexturePool::THUMB_WIDTH),
                            static_cast<GLsizei>(GpuTexturePool::THUMB_HEIGHT), 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, nullptr);
    }

    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    active_pbo = 1u - slot;
}

GpuTexturePool::GpuTexturePool(int max_vram_slots) : m_maxVramSlots(std::min(max_vram_slots, 1540))
{
    initStagingMemory();
    initLru();

    unsigned int num_workers = std::clamp(std::thread::hardware_concurrency(), 2u, 6u);
    m_workerThreads.reserve(num_workers);
    for (unsigned int i = 0; i < num_workers; ++i) {
        m_workerThreads.emplace_back(&GpuTexturePool::workerLoop, this);
    }
}

GpuTexturePool::~GpuTexturePool()
{
    shutdownWorkers();
    freeStagingMemory();
    destroyLru();
}

void GpuTexturePool::shutdownWorkers()
{
    bool expected = false;
    if (!m_stopWorkers.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    m_taskCv.notify_all();
    for (auto &t : m_workerThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workerThreads.clear();
}

void GpuTexturePool::setThumbnailReadyCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_thumbnailReadyCallback = std::move(callback);
}

void GpuTexturePool::initStagingMemory()
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);

    // 128 MB Page-Aligned (4096-byte) Memory Allocation via MAP_ANONYMOUS (Lazy commit)
    m_stagingBuffer = static_cast<uint8_t *>(mmap(
        nullptr, STAGING_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (m_stagingBuffer == MAP_FAILED) {
        m_stagingBuffer = nullptr;
    }

    m_stagingSlots.resize(STAGING_SLOT_COUNT);
    while (!m_freeStagingSlots.empty())
        m_freeStagingSlots.pop();

    for (size_t i = 0; i < STAGING_SLOT_COUNT; ++i) {
        m_stagingSlots[i].ring_offset = i * FRAME_SIZE_BYTES;
        m_stagingSlots[i].item_id = 0;
        m_stagingSlots[i].target_layer = -1;
        m_stagingSlots[i].ready_for_upload = false;
        m_freeStagingSlots.push(i);
    }

    std::lock_guard<std::mutex> slock(m_statsMutex);
    m_stats.staging_buffer_bytes = STAGING_BUFFER_SIZE;
}

void GpuTexturePool::freeStagingMemory()
{
    std::lock_guard<std::mutex> lock(m_stagingMutex);
    if (m_stagingBuffer && m_stagingBuffer != MAP_FAILED) {
        munmap(m_stagingBuffer, STAGING_BUFFER_SIZE);
        m_stagingBuffer = nullptr;
    }
    m_stagingSlots.clear();
}

void GpuTexturePool::initLru()
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    m_freeLayers.reserve(m_maxVramSlots);
    for (int i = 0; i < m_maxVramSlots; ++i) {
        m_freeLayers.push_back(i);
    }

    // Dummy Head and Tail nodes
    m_lruHead = new LruNode();
    m_lruTail = new LruNode();
    m_lruHead->next = m_lruTail;
    m_lruTail->prev = m_lruHead;

    std::lock_guard<std::mutex> slock(m_statsMutex);
    m_stats.max_slots = m_maxVramSlots;
    m_stats.total_vram_bytes = static_cast<size_t>(m_maxVramSlots) *
                               (m_arrayUsesEtc2 ? ETC2_FRAME_SIZE_BYTES : FRAME_SIZE_BYTES);
}

void GpuTexturePool::destroyLru()
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    for (auto &pair : m_itemToNode) {
        delete pair.second;
    }
    m_itemToNode.clear();
    m_layerToNode.clear();
    m_freeLayers.clear();

    delete m_lruHead;
    delete m_lruTail;
    m_lruHead = nullptr;
    m_lruTail = nullptr;
}

void GpuTexturePool::detachLruNode(LruNode *node)
{
    if (!node || !node->prev || !node->next)
        return;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

void GpuTexturePool::insertLruHead(LruNode *node)
{
    if (!node || !m_lruHead)
        return;
    node->next = m_lruHead->next;
    node->prev = m_lruHead;
    m_lruHead->next->prev = node;
    m_lruHead->next = node;
}

int GpuTexturePool::allocateLayerFor(uint32_t item_id)
{
    std::lock_guard<std::mutex> lock(m_lruMutex);

    // If already exists in LRU
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end()) {
        detachLruNode(it->second);
        insertLruHead(it->second);
        return it->second->layer_index;
    }

    int assigned_layer = -1;
    LruNode *node = nullptr;

    if (!m_freeLayers.empty()) {
        assigned_layer = m_freeLayers.back();
        m_freeLayers.pop_back();

        node = new LruNode();
        node->item_id = item_id;
        node->layer_index = assigned_layer;
        node->decode_queued = false;
        node->ready = false;
    } else {
        // Evict LRU from tail (O(1))
        LruNode *victim = m_lruTail->prev;
        if (victim && victim != m_lruHead) {
            assigned_layer = victim->layer_index;
            m_itemToNode.erase(victim->item_id);
            m_layerToNode.erase(assigned_layer);
            detachLruNode(victim);

            node = victim;
            node->item_id = item_id;
            node->layer_index = assigned_layer;
            node->decode_queued = false;
            node->ready = false;
            node->thumbnail = QImage();
            node->etc2_blocks.reset();
        } else {
            return -1;
        }
    }

    insertLruHead(node);
    m_itemToNode[item_id] = node;
    m_layerToNode[assigned_layer] = node;

    std::lock_guard<std::mutex> slock(m_statsMutex);
    m_stats.active_slots = m_itemToNode.size();

    return assigned_layer;
}

bool GpuTexturePool::initGl(QOpenGLExtraFunctions *gl)
{
    if (m_initialized || !gl)
        return false;

    while (gl->glGetError() != GL_NO_ERROR) {
    }

    auto tryAllocCompressedArray = [&](GLenum internalFmt) -> bool {
        GLuint tex = 0;
        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, internalFmt, static_cast<GLsizei>(THUMB_WIDTH),
                           static_cast<GLsizei>(THUMB_HEIGHT), m_maxVramSlots);
        if (gl->glGetError() == GL_NO_ERROR) {
            gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            m_texArrayId = tex;
            m_arrayUsesEtc2 = true;
            m_arrayInternalFormat = internalFmt;
            return true;
        }
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        gl->glDeleteTextures(1, &tex);
        while (gl->glGetError() != GL_NO_ERROR) {
        }

        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const GLsizei totalBytes =
            static_cast<GLsizei>(ETC2_FRAME_SIZE_BYTES * static_cast<size_t>(m_maxVramSlots));
        gl->glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internalFmt,
                                   static_cast<GLsizei>(THUMB_WIDTH),
                                   static_cast<GLsizei>(THUMB_HEIGHT), m_maxVramSlots, 0,
                                   totalBytes, nullptr);
        if (gl->glGetError() == GL_NO_ERROR) {
            gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            m_texArrayId = tex;
            m_arrayUsesEtc2 = true;
            m_arrayInternalFormat = internalFmt;
            return true;
        }
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        gl->glDeleteTextures(1, &tex);
        while (gl->glGetError() != GL_NO_ERROR) {
        }
        return false;
    };

    if (!tryAllocCompressedArray(GL_COMPRESSED_SRGB8_ETC2) &&
        !tryAllocCompressedArray(GL_COMPRESSED_RGB8_ETC2)) {
        gl->glGenTextures(1, &m_texArrayId);
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, m_texArrayId);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, THUMB_WIDTH, THUMB_HEIGHT,
                         m_maxVramSlots, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        m_arrayUsesEtc2 = false;
        m_arrayInternalFormat = GL_RGBA8;
    }

    const size_t perSlotBytes = m_arrayUsesEtc2 ? ETC2_FRAME_SIZE_BYTES : FRAME_SIZE_BYTES;
    {
        std::lock_guard<std::mutex> slock(m_statsMutex);
        m_stats.total_vram_bytes = static_cast<size_t>(m_maxVramSlots) * perSlotBytes;
    }

    m_pboQueue.init(gl, perSlotBytes);
    m_initialized = true;
    return true;
}

void GpuTexturePool::destroyGl(QOpenGLExtraFunctions *gl)
{
    if (!m_initialized || !gl)
        return;

    m_pboQueue.destroy(gl);

    if (m_texArrayId != 0) {
        gl->glDeleteTextures(1, &m_texArrayId);
        m_texArrayId = 0;
    }

    m_initialized = false;
}

int GpuTexturePool::requestTexture(uint32_t item_id, const std::string &media_path,
                                   const std::string &preview_path)
{
    int layer = -1;

    // 1. Check if already in LRU
    {
        std::lock_guard<std::mutex> lock(m_lruMutex);
        auto it = m_itemToNode.find(item_id);
        if (it != m_itemToNode.end()) {
            detachLruNode(it->second);
            insertLruHead(it->second);
            if (it->second->media_path != media_path) {
                // Item ID was reassigned to a different wallpaper after a catalog rescan
                it->second->media_path = media_path;
                it->second->ready = false;
                it->second->decode_queued = false;
                it->second->thumbnail = QImage();
                it->second->etc2_blocks.reset();
                layer = it->second->layer_index;
            } else {
                if (it->second->ready) {
                    return it->second->layer_index;
                }
                if (it->second->decode_queued) {
                    return -1; // Decode already in flight
                }
                layer = it->second->layer_index;
            }
        }
    }

    // 2. Allocate an LRU VRAM layer slot if not yet allocated
    if (layer < 0) {
        layer = allocateLayerFor(item_id);
        if (layer < 0)
            return -1;
    }

    // 3. Acquire an L2 staging ringbuffer slot
    size_t staging_slot_idx = 0;
    {
        std::lock_guard<std::mutex> lock(m_stagingMutex);
        if (m_freeStagingSlots.empty()) {
            // Staging buffer temporarily full; will retry next frame
            return -1;
        }
        staging_slot_idx = m_freeStagingSlots.front();
        m_freeStagingSlots.pop();

        m_stagingSlots[staging_slot_idx].item_id = item_id;
        m_stagingSlots[staging_slot_idx].target_layer = layer;
        m_stagingSlots[staging_slot_idx].media_path = media_path;
        m_stagingSlots[staging_slot_idx].ready_for_upload = false;
        m_stagingSlots[staging_slot_idx].decode_succeeded = false;
        m_stagingSlots[staging_slot_idx].etc2_blocks.reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_lruMutex);
        auto it = m_itemToNode.find(item_id);
        if (it != m_itemToNode.end()) {
            it->second->media_path = media_path;
            it->second->decode_queued = true;
        }
    }

    // 4. Enqueue background decode task
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_taskQueue.push(WorkerTask{.item_id = item_id,
                                    .target_layer = layer,
                                    .media_path = media_path,
                                    .preview_path = preview_path,
                                    .staging_slot_idx = staging_slot_idx});
    }
    m_taskCv.notify_one();

    return -1; // Upload pending
}

int GpuTexturePool::processPendingUploads(QOpenGLExtraFunctions *gl, int max_uploads_per_frame)
{
    int uploaded_count = 0;
    while (uploaded_count < max_uploads_per_frame) {
        size_t staging_idx = 0;
        {
            std::lock_guard<std::mutex> lock(m_uploadMutex);
            if (m_uploadQueue.empty())
                break;
            staging_idx = m_uploadQueue.front();
            m_uploadQueue.pop();
        }

        uint8_t *slot_rgba = nullptr;
        int target_layer = -1;
        uint32_t item_id = 0;
        std::string slot_media_path;
        bool decode_ok = false;
        std::shared_ptr<std::vector<uint8_t>> slot_etc2;
        {
            std::lock_guard<std::mutex> lock(m_stagingMutex);
            if (staging_idx < m_stagingSlots.size() &&
                m_stagingSlots[staging_idx].ready_for_upload) {
                slot_rgba = m_stagingBuffer + m_stagingSlots[staging_idx].ring_offset;
                target_layer = m_stagingSlots[staging_idx].target_layer;
                item_id = m_stagingSlots[staging_idx].item_id;
                slot_media_path = m_stagingSlots[staging_idx].media_path;
                decode_ok = m_stagingSlots[staging_idx].decode_succeeded;
                slot_etc2 = std::move(m_stagingSlots[staging_idx].etc2_blocks);
            }
        }

        if (slot_rgba && target_layer >= 0) {
            QImage thumb_img(slot_rgba, static_cast<int>(THUMB_WIDTH),
                             static_cast<int>(THUMB_HEIGHT),
                             static_cast<qsizetype>(THUMB_WIDTH) * 4, QImage::Format_RGBA8888);
            QImage thumb_copy = decode_ok ? thumb_img.copy() : QImage();

            bool slot_still_valid = false;
            {
                std::lock_guard<std::mutex> lock(m_lruMutex);
                auto it = m_itemToNode.find(item_id);
                if (it != m_itemToNode.end() && it->second->layer_index == target_layer &&
                    it->second->media_path == slot_media_path) {
                    it->second->thumbnail = std::move(thumb_copy);
                    it->second->etc2_blocks = slot_etc2;
                    it->second->ready = true;
                    it->second->decode_queued = false;
                    slot_still_valid = true;
                }
            }

            if (slot_still_valid && m_initialized && gl && m_texArrayId != 0) {
                if (m_arrayUsesEtc2 && slot_etc2 && slot_etc2->size() == ETC2_FRAME_SIZE_BYTES) {
                    m_pboQueue.uploadAsync(gl, slot_etc2->data(), slot_etc2->size(),
                                           static_cast<GLuint>(target_layer), m_texArrayId, true,
                                           m_arrayInternalFormat);
                } else if (!m_arrayUsesEtc2) {
                    m_pboQueue.uploadAsync(gl, slot_rgba, FRAME_SIZE_BYTES,
                                           static_cast<GLuint>(target_layer), m_texArrayId, false,
                                           GL_RGBA8);
                }
            }
            ++uploaded_count;
        }

        // Return staging slot to free ring queue
        {
            std::lock_guard<std::mutex> lock(m_stagingMutex);
            m_stagingSlots[staging_idx].ready_for_upload = false;
            m_stagingSlots[staging_idx].etc2_blocks.reset();
            m_freeStagingSlots.push(staging_idx);
        }
    }

    return uploaded_count;
}

bool GpuTexturePool::isTextureReady(uint32_t item_id) const
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    return it != m_itemToNode.end() && it->second->ready;
}

int GpuTexturePool::getLayerIndex(uint32_t item_id) const
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end() && it->second->ready) {
        return it->second->layer_index;
    }
    return -1;
}

QImage GpuTexturePool::getThumbnailImage(uint32_t item_id) const
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end() && it->second->ready) {
        return it->second->thumbnail;
    }
    return {};
}

std::shared_ptr<const std::vector<uint8_t>>
GpuTexturePool::getCompressedEtc2Blocks(uint32_t item_id) const
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end() && it->second->ready) {
        return it->second->etc2_blocks;
    }
    return {};
}

void GpuTexturePool::touch(uint32_t item_id)
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end()) {
        detachLruNode(it->second);
        insertLruHead(it->second);
    }
}

void GpuTexturePool::clear()
{
    std::vector<size_t> reclaimed_slots;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        while (!m_taskQueue.empty()) {
            reclaimed_slots.push_back(m_taskQueue.front().staging_slot_idx);
            m_taskQueue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        while (!m_uploadQueue.empty()) {
            reclaimed_slots.push_back(m_uploadQueue.front());
            m_uploadQueue.pop();
        }
    }
    if (!reclaimed_slots.empty()) {
        std::lock_guard<std::mutex> lock(m_stagingMutex);
        for (size_t idx : reclaimed_slots) {
            if (idx < m_stagingSlots.size()) {
                m_stagingSlots[idx].ready_for_upload = false;
                m_stagingSlots[idx].etc2_blocks.reset();
                m_freeStagingSlots.push(idx);
            }
        }
    }
    destroyLru();
    initLru();
}

TexturePoolStats GpuTexturePool::stats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

bool GpuTexturePool::decodeThumbnail(const std::string &path, const std::string &preview,
                                     uint8_t *out_rgba)
{
    if (!out_rgba)
        return false;

    std::string cache_path = resolveDiskCachePath(path);

    // L1: Check Disk Cache (~/.cache/wwal/thumbnails/<sha256>.webp)
    std::error_code ec;
    if (std::filesystem::exists(cache_path, ec)) {
        QImage cached_img = loadQImageWithCoreFallback(cache_path);
        if (!cached_img.isNull() && cached_img.width() == static_cast<int>(THUMB_WIDTH) &&
            cached_img.height() == static_cast<int>(THUMB_HEIGHT)) {
            QImage rgba = cached_img.convertToFormat(QImage::Format_RGBA8888);
            std::memcpy(out_rgba, rgba.constBits(), FRAME_SIZE_BYTES);

            std::lock_guard<std::mutex> slock(m_statsMutex);
            m_stats.disk_hits++;
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> slock(m_statsMutex);
        m_stats.disk_misses++;
    }

    std::string source_file = !preview.empty() ? preview : path;
    std::string ext = toLowerExt(source_file);

    // Handle shader files with procedural shader thumbnail
    if (ext == ".glsl" || ext == ".frag" || ext == ".shadertoy" || ext == ".comp") {
        generateShaderThumbnail(source_file, out_rgba, THUMB_WIDTH, THUMB_HEIGHT);
        return true;
    }

    // Handle Gnome timed wallpaper XML files by resolving their first referenced image
    if (ext == ".xml" && std::filesystem::exists(source_file, ec)) {
        std::string xml_img = resolveGnomeXmlFirstImage(source_file);
        if (!xml_img.empty()) {
            source_file = xml_img;
            ext = toLowerExt(source_file);
        }
    }

    QImage src_img;
    if (std::filesystem::exists(source_file, ec)) {
        src_img = loadQImageWithCoreFallback(source_file);

        // If video file and not yet decoded, extract a frame via ffmpeg
        if (src_img.isNull() &&
            (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov")) {
            QProcess proc;
            QStringList args = {
                QStringLiteral("-loglevel"),
                QStringLiteral("error"),
                QStringLiteral("-i"),
                QString::fromStdString(source_file),
                QStringLiteral("-frames:v"),
                QStringLiteral("1"),
                QStringLiteral("-vf"),
                QStringLiteral("scale=384:216:force_original_aspect_ratio=increase,crop=384:216"),
                QStringLiteral("-f"),
                QStringLiteral("image2pipe"),
                QStringLiteral("-vcodec"),
                QStringLiteral("png"),
                QStringLiteral("-")};
            proc.start(QStringLiteral("ffmpeg"), args);
            if (proc.waitForFinished(2500)) {
                QByteArray png_bytes = proc.readAllStandardOutput();
                if (!png_bytes.isEmpty()) {
                    src_img.loadFromData(png_bytes, "PNG");
                }
            }
        }
    }

    if (src_img.isNull()) {
        // Fallback: generate subtle gradient thumbnail so even synthetic/missing items have a valid
        // buffer
        generateShaderThumbnail(source_file, out_rgba, THUMB_WIDTH, THUMB_HEIGHT);
        return false;
    }

    // Downscale & center-crop to 384x216 with high quality smooth filtering
    QImage rgba = scaleAndCenterCrop(src_img, THUMB_WIDTH, THUMB_HEIGHT);
    std::memcpy(out_rgba, rgba.constBits(), FRAME_SIZE_BYTES);

    // Save lossy WebP quality 85 to disk cache (fallback to PNG if Qt WebP writer plugin is absent)
    if (!rgba.save(QString::fromStdString(cache_path), "WEBP", 85)) {
        rgba.save(QString::fromStdString(cache_path), "PNG");
    }

    return true;
}

void GpuTexturePool::workerLoop()
{
    while (!m_stopWorkers) {
        WorkerTask task;
        {
            std::unique_lock<std::mutex> lock(m_taskMutex);
            m_taskCv.wait(lock, [this] { return m_stopWorkers || !m_taskQueue.empty(); });

            if (m_stopWorkers && m_taskQueue.empty()) {
                return;
            }

            task = m_taskQueue.front();
            m_taskQueue.pop();
        }

        uint8_t *slot_ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_stagingMutex);
            if (task.staging_slot_idx < m_stagingSlots.size()) {
                slot_ptr = m_stagingBuffer + m_stagingSlots[task.staging_slot_idx].ring_offset;
            }
        }

        if (slot_ptr) {
            bool ok = decodeThumbnail(task.media_path, task.preview_path, slot_ptr);
            auto etc2Buf = std::make_shared<std::vector<uint8_t>>();
            etc2::encodeRgbaToEtc2Simd(slot_ptr, static_cast<int>(THUMB_WIDTH),
                                       static_cast<int>(THUMB_HEIGHT),
                                       static_cast<int>(THUMB_WIDTH * BYTES_PER_PIXEL), *etc2Buf);

            {
                std::lock_guard<std::mutex> lock(m_stagingMutex);
                m_stagingSlots[task.staging_slot_idx].decode_succeeded = ok;
                m_stagingSlots[task.staging_slot_idx].etc2_blocks = std::move(etc2Buf);
                m_stagingSlots[task.staging_slot_idx].ready_for_upload = true;
            }

            {
                std::lock_guard<std::mutex> lock(m_uploadMutex);
                m_uploadQueue.push(task.staging_slot_idx);
            }

            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_thumbnailReadyCallback;
            }
            if (cb) {
                cb();
            }
        }
    }
}

} // namespace wwal::studio
