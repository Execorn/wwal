// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_texture_pool.hpp"
#include "etc2_simd_compressor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>

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

std::string resolveDiskCachePath(const std::string &media_path)
{
    char out_path[4096];
    if (!waywal_thumb_resolve_cache_path(media_path.c_str(), out_path, sizeof(out_path))) {
        return {};
    }
    return std::string(out_path);
}

void ensureCacheDirectoryOnce(const std::filesystem::path &cache_dir)
{
    static std::mutex s_dirMutex;
    static std::filesystem::path s_createdDir;

    std::lock_guard<std::mutex> lock(s_dirMutex);
    if (s_createdDir == cache_dir) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (!ec) {
        s_createdDir = cache_dir;
    }
}

bool webpWritingSupported()
{
    static const bool supported = []() {
        const auto formats = QImageWriter::supportedImageFormats();
        for (const QByteArray &fmt : formats) {
            if (fmt.compare("webp", Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        return false;
    }();
    return supported;
}

bool isDiskCacheFresh(const std::string &candidate_path, const std::string &source_file)
{
    if (candidate_path.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate_path, ec) || ec) {
        return false;
    }
    const auto cached_size = std::filesystem::file_size(candidate_path, ec);
    if (ec || cached_size == 0) {
        return false;
    }
    if (source_file.empty() || !std::filesystem::exists(source_file, ec) || ec) {
        return true;
    }
    const auto cache_mtime = std::filesystem::last_write_time(candidate_path, ec);
    if (ec) {
        return false;
    }
    const auto src_mtime = std::filesystem::last_write_time(source_file, ec);
    if (ec) {
        return false;
    }
    if (cache_mtime < src_mtime) {
        return false;
    }

    const std::string stamp_path = candidate_path + ".stamp";
    if (std::filesystem::is_regular_file(stamp_path, ec) && !ec) {
        std::ifstream sf(stamp_path, std::ios::binary);
        uint64_t stamped_size = 0;
        int64_t stamped_mtime = 0;
        sf.read(reinterpret_cast<char *>(&stamped_size), sizeof(stamped_size));
        sf.read(reinterpret_cast<char *>(&stamped_mtime), sizeof(stamped_mtime));
        if (sf.good()) {
            const auto src_size = std::filesystem::file_size(source_file, ec);
            if (!ec) {
                const auto src_ticks = static_cast<int64_t>(src_mtime.time_since_epoch().count());
                if (stamped_size != src_size || stamped_mtime != src_ticks) {
                    return false;
                }
            }
        }
    }
    return true;
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

QImage loadQImageWithCoreFallback(const std::string &path, uint32_t target_w = 0, uint32_t target_h = 0)
{
    QImageReader reader(QString::fromStdString(path));
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);

    if (target_w > 0 && target_h > 0) {
        const QSize src_size = reader.size();
        if (src_size.isValid() && src_size.width() > 0 && src_size.height() > 0 &&
            (src_size.width() > static_cast<int>(target_w) ||
             src_size.height() > static_cast<int>(target_h))) {
            QSize scaled = src_size.scaled(static_cast<int>(target_w), static_cast<int>(target_h),
                                           Qt::KeepAspectRatioByExpanding);
            if (scaled.isValid() && scaled.width() > 0 && scaled.height() > 0 &&
                scaled.width() <= 8192 && scaled.height() <= 8192) {
                reader.setScaledSize(scaled);
            }
        }
    }

    QImage img = reader.read();
    if (!img.isNull()) {
        return img;
    }

    uint8_t *pixels = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    if (image_load_rgba(path.c_str(), &pixels, &w, &h) && pixels && w > 0 && h > 0) {
        const qsizetype stride = static_cast<qsizetype>(w) * 4;
        const bool dims_valid = w <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                                h <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                                stride <= std::numeric_limits<int>::max();
        if (dims_valid) {
            QImage raw_wrap(pixels, static_cast<int>(w), static_cast<int>(h), stride,
                            QImage::Format_RGBA8888);
            QImage copy = raw_wrap.copy();
            image_free(pixels);
            return copy;
        }
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

bool writeThumbnailAtomically(const QImage &rgba, const std::string &dest_path, const char *format,
                              int quality, const std::string &source_file)
{
    static std::atomic<uint64_t> s_tmpSeq{0};
    const std::filesystem::path dest(dest_path);
    ensureCacheDirectoryOnce(dest.parent_path());

    const uint64_t seq = s_tmpSeq.fetch_add(1, std::memory_order_relaxed);
    const std::string tmp_path =
        dest_path + ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(seq);

    QFile out_file(QString::fromStdString(tmp_path));
    if (!out_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QImageWriter writer(&out_file, format);
    if (quality >= 0) {
        writer.setQuality(quality);
    }
    const bool wrote = writer.write(rgba) && out_file.flush();
    out_file.close();

    std::error_code ec;
    if (!wrote) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::filesystem::rename(tmp_path, dest_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    if (!source_file.empty() && std::filesystem::exists(source_file, ec) && !ec) {
        const auto src_size = std::filesystem::file_size(source_file, ec);
        if (!ec) {
            const auto src_mtime = std::filesystem::last_write_time(source_file, ec);
            if (!ec) {
                const std::string stamp_tmp = tmp_path + ".stamp";
                const std::string stamp_dest = dest_path + ".stamp";
                {
                    std::ofstream sf(stamp_tmp, std::ios::binary | std::ios::trunc);
                    const uint64_t sz = src_size;
                    const int64_t ticks = static_cast<int64_t>(src_mtime.time_since_epoch().count());
                    sf.write(reinterpret_cast<const char *>(&sz), sizeof(sz));
                    sf.write(reinterpret_cast<const char *>(&ticks), sizeof(ticks));
                }
                std::filesystem::rename(stamp_tmp, stamp_dest, ec);
                if (ec) {
                    std::filesystem::remove(stamp_tmp, ec);
                }
            }
        }
    }

    return true;
}

QImage extractVideoFrameBounded(const std::string &source_file, std::stop_token stop_token,
                                const std::atomic<uint64_t> &live_epoch, uint64_t expected_epoch)
{
    auto shouldAbort = [&]() {
        return stop_token.stop_requested() ||
               live_epoch.load(std::memory_order_acquire) != expected_epoch;
    };

    auto runOnce = [&](const char *seek_pos, int budget_ms) -> std::pair<bool, QByteArray> {
        if (shouldAbort()) {
            return {false, {}};
        }

        QProcess proc;
        QStringList args;
        if (seek_pos != nullptr) {
            args << QStringLiteral("-ss") << QString::fromLatin1(seek_pos);
        }
        args << QStringLiteral("-loglevel") << QStringLiteral("error") << QStringLiteral("-i")
             << QString::fromStdString(source_file) << QStringLiteral("-frames:v")
             << QStringLiteral("1") << QStringLiteral("-vf")
             << QStringLiteral("scale=384:216:force_original_aspect_ratio=increase,crop=384:216")
             << QStringLiteral("-f") << QStringLiteral("image2pipe") << QStringLiteral("-vcodec")
             << QStringLiteral("png") << QStringLiteral("-");

        proc.start(QStringLiteral("ffmpeg"), args);
        if (!proc.waitForStarted(500)) {
            return {false, {}};
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

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
        bool finished = false;
        while (!finished) {
            if (shouldAbort()) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            const auto rem_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int slice_ms = static_cast<int>(std::clamp<int64_t>(rem_ms, 1, 100));
            finished = proc.waitForFinished(slice_ms);
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

        if (!finished || shouldAbort()) {
            return {false, {}};
        }
        return {proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0,
                proc.readAllStandardOutput()};
    };

    const auto start_tp = std::chrono::steady_clock::now();
    // Seek past black fade-in frames first; fall back to frame 0 for ultra-short clips.
    auto [ok, png_bytes] = runOnce("00:00:00.50", 1600);
    if (!shouldAbort() && png_bytes.isEmpty()) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start_tp)
                                    .count();
        const int rem = static_cast<int>(2500 - elapsed_ms);
        if (rem >= 300) {
            auto retry = runOnce(nullptr, rem);
            ok = retry.first;
            png_bytes = std::move(retry.second);
        }
    }

    if (!ok || png_bytes.isEmpty() || shouldAbort()) {
        return {};
    }

    QImage img;
    img.loadFromData(png_bytes, "PNG");
    return img;
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

GpuTexturePool::GpuTexturePool(int max_vram_slots)
    : m_maxVramSlots(std::clamp(max_vram_slots, 1, 1540))
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
    m_shutdownGate.beginShutdown();
    m_taskCv.notify_all();
    m_shutdownGate.waitForDrain();
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
    void *mapped = mmap(nullptr, STAGING_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    while (!m_freeStagingSlots.empty()) {
        m_freeStagingSlots.pop();
    }
    if (mapped == MAP_FAILED) {
        m_stagingBuffer = nullptr;
        m_stagingSlots.clear();
        std::lock_guard<std::mutex> slock(m_statsMutex);
        m_stats.staging_buffer_bytes = 0;
        return;
    }
    m_stagingBuffer = static_cast<uint8_t *>(mapped);

    m_stagingSlots.resize(STAGING_SLOT_COUNT);
    for (size_t i = 0; i < STAGING_SLOT_COUNT; ++i) {
        m_stagingSlots[i].ring_offset = i * FRAME_SIZE_BYTES;
        m_stagingSlots[i].item_id = 0;
        m_stagingSlots[i].target_layer = -1;
        m_stagingSlots[i].ready_for_upload = false;
        m_stagingSlots[i].decode_succeeded = false;
        m_stagingSlots[i].etc2_blocks.reset();
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
    while (!m_freeStagingSlots.empty()) {
        m_freeStagingSlots.pop();
    }
}

void GpuTexturePool::initLruLocked()
{
    m_freeLayers.clear();
    m_freeLayers.reserve(static_cast<size_t>(m_maxVramSlots));
    for (int i = 0; i < m_maxVramSlots; ++i) {
        m_freeLayers.push_back(i);
    }

    // Dummy Head and Tail nodes
    m_lruHead = new LruNode();
    m_lruTail = new LruNode();
    m_lruHead->next = m_lruTail;
    m_lruTail->prev = m_lruHead;
}

void GpuTexturePool::destroyLruLocked()
{
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

void GpuTexturePool::initLru()
{
    std::scoped_lock lock(m_lruMutex, m_statsMutex);
    initLruLocked();
    m_stats.max_slots = static_cast<size_t>(m_maxVramSlots);
    m_stats.total_vram_bytes = static_cast<size_t>(m_maxVramSlots) *
                               (m_arrayUsesEtc2 ? ETC2_FRAME_SIZE_BYTES : FRAME_SIZE_BYTES);
    m_stats.active_slots = 0;
}

void GpuTexturePool::destroyLru()
{
    std::scoped_lock lock(m_lruMutex, m_statsMutex);
    destroyLruLocked();
    m_stats.active_slots = 0;
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
    if (!node || !m_lruHead || !m_lruTail)
        return;
    node->next = m_lruHead->next;
    node->prev = m_lruHead;
    m_lruHead->next->prev = node;
    m_lruHead->next = node;
}

int GpuTexturePool::allocateLayerLocked(uint32_t item_id)
{
    // If already exists in LRU
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end()) {
        detachLruNode(it->second);
        insertLruHead(it->second);
        return it->second->layer_index;
    }

    if (!m_lruHead || !m_lruTail) {
        return -1;
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
        node->cpu_ready = false;
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
            node->cpu_ready = false;
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

int GpuTexturePool::allocateLayerFor(uint32_t item_id)
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    return allocateLayerLocked(item_id);
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

    {
        std::lock_guard<std::mutex> lock(m_lruMutex);
        for (auto &pair : m_itemToNode) {
            pair.second->ready = false;
        }
    }

    m_initialized = false;
}

int GpuTexturePool::requestTexture(uint32_t item_id, const std::string &media_path,
                                   const std::string &preview_path)
{
    if (m_shutdownGate.closing.load(std::memory_order_acquire)) {
        return -1;
    }

    // Fast path for already resident or pending entries
    {
        std::lock_guard<std::mutex> lock(m_lruMutex);
        auto it = m_itemToNode.find(item_id);
        if (it != m_itemToNode.end()) {
            detachLruNode(it->second);
            insertLruHead(it->second);
            if (it->second->media_path == media_path) {
                if (it->second->ready) {
                    return it->second->layer_index;
                }
                if (it->second->decode_queued || it->second->cpu_ready) {
                    return -1;
                }
            }
        }
    }

    bool notify_worker = false;
    {
        std::scoped_lock lock(m_taskMutex, m_stagingMutex, m_lruMutex);
        if (m_shutdownGate.closing.load(std::memory_order_acquire) || !m_stagingBuffer) {
            return -1;
        }

        int layer = -1;
        auto it = m_itemToNode.find(item_id);
        if (it != m_itemToNode.end()) {
            detachLruNode(it->second);
            insertLruHead(it->second);
            if (it->second->media_path == media_path) {
                if (it->second->ready) {
                    return it->second->layer_index;
                }
                if (it->second->decode_queued || it->second->cpu_ready) {
                    return -1;
                }
                layer = it->second->layer_index;
            } else {
                // Item ID was reassigned to a different wallpaper after a catalog rescan
                it->second->media_path = media_path;
                it->second->ready = false;
                it->second->cpu_ready = false;
                it->second->decode_queued = false;
                it->second->thumbnail = QImage();
                it->second->etc2_blocks.reset();
                layer = it->second->layer_index;
            }
        }

        // Avoid evicting resident VRAM LRU slots when staging is full; unused layers can still
        // be claimed until the pool reaches capacity.
        if (m_freeStagingSlots.empty()) {
            if (layer < 0 && !m_freeLayers.empty()) {
                allocateLayerLocked(item_id);
            }
            return -1;
        }

        if (layer < 0) {
            layer = allocateLayerLocked(item_id);
            if (layer < 0) {
                return -1;
            }
        }

        const size_t staging_slot_idx = m_freeStagingSlots.front();
        m_freeStagingSlots.pop();

        m_stagingSlots[staging_slot_idx].item_id = item_id;
        m_stagingSlots[staging_slot_idx].target_layer = layer;
        m_stagingSlots[staging_slot_idx].media_path = media_path;
        m_stagingSlots[staging_slot_idx].ready_for_upload = false;
        m_stagingSlots[staging_slot_idx].decode_succeeded = false;
        m_stagingSlots[staging_slot_idx].etc2_blocks.reset();

        auto node_it = m_itemToNode.find(item_id);
        if (node_it != m_itemToNode.end()) {
            node_it->second->media_path = media_path;
            node_it->second->decode_queued = true;
        }

        const uint64_t current_epoch = m_shutdownGate.epoch.load(std::memory_order_acquire);
        m_taskQueue.push(WorkerTask{.item_id = item_id,
                                    .target_layer = layer,
                                    .epoch = current_epoch,
                                    .media_path = media_path,
                                    .preview_path = preview_path,
                                    .staging_slot_idx = staging_slot_idx});
        notify_worker = true;
    }

    if (notify_worker) {
        m_taskCv.notify_one();
    }

    return -1;
}

int GpuTexturePool::processPendingUploads(QOpenGLExtraFunctions *gl, int max_uploads_per_frame)
{
    if (max_uploads_per_frame <= 0) {
        return 0;
    }

    const bool gl_upload_ready = (m_initialized && gl != nullptr && m_texArrayId != 0);
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
            if (m_stagingBuffer != nullptr && staging_idx < m_stagingSlots.size() &&
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
                    it->second->cpu_ready = true;
                    // Only mark VRAM resident when a live GL context uploaded the layer.
                    it->second->ready = gl_upload_ready;
                    it->second->decode_queued = false;
                    slot_still_valid = true;
                }
            }

            if (slot_still_valid && gl_upload_ready) {
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
            if (staging_idx < m_stagingSlots.size()) {
                m_stagingSlots[staging_idx].ready_for_upload = false;
                m_stagingSlots[staging_idx].etc2_blocks.reset();
                m_freeStagingSlots.push(staging_idx);
            }
        }
    }

    // Promote CPU-decoded nodes to VRAM once an OpenGL context becomes available.
    if (gl_upload_ready && uploaded_count < max_uploads_per_frame) {
        struct PendingVramUpload {
            uint32_t item_id;
            int layer_index;
            std::string media_path;
            QImage thumbnail;
            std::shared_ptr<const std::vector<uint8_t>> etc2_blocks;
        };
        std::vector<PendingVramUpload> deferred;
        deferred.reserve(static_cast<size_t>(max_uploads_per_frame - uploaded_count));

        {
            std::lock_guard<std::mutex> lock(m_lruMutex);
            for (LruNode *cur = m_lruHead ? m_lruHead->next : nullptr;
                 cur != nullptr && cur != m_lruTail &&
                 static_cast<int>(deferred.size()) < (max_uploads_per_frame - uploaded_count);
                 cur = cur->next) {
                if (cur->cpu_ready && !cur->ready && !cur->decode_queued && cur->layer_index >= 0) {
                    deferred.push_back(PendingVramUpload{.item_id = cur->item_id,
                                                         .layer_index = cur->layer_index,
                                                         .media_path = cur->media_path,
                                                         .thumbnail = cur->thumbnail,
                                                         .etc2_blocks = cur->etc2_blocks});
                }
            }
        }

        for (const auto &pending : deferred) {
            bool uploaded = false;
            if (m_arrayUsesEtc2 && pending.etc2_blocks &&
                pending.etc2_blocks->size() == ETC2_FRAME_SIZE_BYTES) {
                m_pboQueue.uploadAsync(gl, pending.etc2_blocks->data(),
                                       pending.etc2_blocks->size(),
                                       static_cast<GLuint>(pending.layer_index), m_texArrayId,
                                       true, m_arrayInternalFormat);
                uploaded = true;
            } else if (!m_arrayUsesEtc2 && !pending.thumbnail.isNull()) {
                QImage rgba = pending.thumbnail.convertToFormat(QImage::Format_RGBA8888);
                if (static_cast<size_t>(rgba.sizeInBytes()) >= FRAME_SIZE_BYTES) {
                    m_pboQueue.uploadAsync(gl, rgba.constBits(), FRAME_SIZE_BYTES,
                                           static_cast<GLuint>(pending.layer_index), m_texArrayId,
                                           false, GL_RGBA8);
                    uploaded = true;
                }
            }

            std::lock_guard<std::mutex> lock(m_lruMutex);
            auto it = m_itemToNode.find(pending.item_id);
            if (it != m_itemToNode.end() && it->second->layer_index == pending.layer_index &&
                it->second->media_path == pending.media_path) {
                it->second->ready = uploaded;
            }
            ++uploaded_count;
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
    if (it != m_itemToNode.end() && it->second->cpu_ready) {
        return it->second->thumbnail;
    }
    return {};
}

std::shared_ptr<const std::vector<uint8_t>>
GpuTexturePool::getCompressedEtc2Blocks(uint32_t item_id) const
{
    std::lock_guard<std::mutex> lock(m_lruMutex);
    auto it = m_itemToNode.find(item_id);
    if (it != m_itemToNode.end() && it->second->cpu_ready) {
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
    std::scoped_lock lock(m_taskMutex, m_uploadMutex, m_stagingMutex, m_lruMutex, m_statsMutex);
    m_shutdownGate.epoch.fetch_add(1, std::memory_order_acq_rel);

    std::vector<size_t> reclaimed_slots;
    while (!m_taskQueue.empty()) {
        reclaimed_slots.push_back(m_taskQueue.front().staging_slot_idx);
        m_taskQueue.pop();
    }
    while (!m_uploadQueue.empty()) {
        reclaimed_slots.push_back(m_uploadQueue.front());
        m_uploadQueue.pop();
    }
    for (size_t idx : reclaimed_slots) {
        if (idx < m_stagingSlots.size()) {
            m_stagingSlots[idx].ready_for_upload = false;
            m_stagingSlots[idx].etc2_blocks.reset();
            m_freeStagingSlots.push(idx);
        }
    }

    destroyLruLocked();
    initLruLocked();
    m_stats.active_slots = 0;
}

TexturePoolStats GpuTexturePool::stats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

bool GpuTexturePool::decodeThumbnail(const std::string &path, const std::string &preview,
                                     uint8_t *out_rgba, std::stop_token stop_token,
                                     uint64_t expected_epoch)
{
    if (!out_rgba)
        return false;

    auto isCancelled = [&]() {
        return stop_token.stop_requested() ||
               m_shutdownGate.epoch.load(std::memory_order_acquire) != expected_epoch;
    };
    if (isCancelled()) {
        return false;
    }

    const std::string source_file = !preview.empty() ? preview : path;
    const std::string cache_path = resolveDiskCachePath(path);
    const bool use_webp = webpWritingSupported();
    const std::string active_cache = use_webp ? cache_path : (cache_path + ".png");

    // L1: Check Disk Cache (~/.cache/wwal/thumbnails/<sha256>.webp)
    const std::string hit_path =
        isDiskCacheFresh(cache_path, source_file)
            ? cache_path
            : (!use_webp && isDiskCacheFresh(active_cache, source_file) ? active_cache
                                                                        : std::string{});
    if (!hit_path.empty()) {
        QImage cached_img = loadQImageWithCoreFallback(hit_path, THUMB_WIDTH, THUMB_HEIGHT);
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

    if (isCancelled()) {
        return false;
    }

    std::string resolved_source = source_file;
    std::string ext = toLowerExt(resolved_source);

    // Handle shader files with procedural shader thumbnail
    if (ext == ".glsl" || ext == ".frag" || ext == ".shadertoy" || ext == ".comp") {
        generateShaderThumbnail(resolved_source, out_rgba, THUMB_WIDTH, THUMB_HEIGHT);
        return true;
    }

    // Handle Gnome timed wallpaper XML files by resolving their first referenced image
    std::error_code ec;
    if (ext == ".xml" && std::filesystem::exists(resolved_source, ec)) {
        std::string xml_img = resolveGnomeXmlFirstImage(resolved_source);
        if (!xml_img.empty()) {
            resolved_source = xml_img;
            ext = toLowerExt(resolved_source);
        }
    }

    QImage src_img;
    if (std::filesystem::exists(resolved_source, ec)) {
        src_img = loadQImageWithCoreFallback(resolved_source, THUMB_WIDTH, THUMB_HEIGHT);

        // If video file and not yet decoded, extract a bounded frame via ffmpeg
        if (src_img.isNull() &&
            (ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov")) {
            src_img = extractVideoFrameBounded(resolved_source, stop_token, m_shutdownGate.epoch,
                                               expected_epoch);
        }
    }

    if (isCancelled()) {
        return false;
    }

    if (src_img.isNull()) {
        generateShaderThumbnail(resolved_source, out_rgba, THUMB_WIDTH, THUMB_HEIGHT);
        return false;
    }

    QImage rgba = scaleAndCenterCrop(src_img, THUMB_WIDTH, THUMB_HEIGHT);
    std::memcpy(out_rgba, rgba.constBits(), FRAME_SIZE_BYTES);

    if (!active_cache.empty()) {
        if (use_webp) {
            writeThumbnailAtomically(rgba, active_cache, "WEBP", 85, source_file);
        } else {
            writeThumbnailAtomically(rgba, active_cache, "PNG", -1, source_file);
        }
    }

    return true;
}

void GpuTexturePool::workerLoop()
{
    const std::stop_token stop_token = m_shutdownGate.stop_source.get_token();

    while (!m_shutdownGate.closing.load(std::memory_order_acquire)) {
        WorkerTask task;
        {
            std::unique_lock<std::mutex> lock(m_taskMutex);
            m_taskCv.wait(lock, [this] {
                return m_shutdownGate.closing.load(std::memory_order_acquire) ||
                       !m_taskQueue.empty();
            });

            if (m_shutdownGate.closing.load(std::memory_order_acquire) && m_taskQueue.empty()) {
                return;
            }

            task = m_taskQueue.front();
            m_taskQueue.pop();
        }

        PoolShutdownGate::ReaderGuard reader_guard(m_shutdownGate);
        if (!reader_guard.engaged()) {
            return;
        }

        auto returnStagingSlot = [&]() {
            std::lock_guard<std::mutex> lock(m_stagingMutex);
            if (task.staging_slot_idx < m_stagingSlots.size()) {
                m_stagingSlots[task.staging_slot_idx].ready_for_upload = false;
                m_stagingSlots[task.staging_slot_idx].etc2_blocks.reset();
                m_freeStagingSlots.push(task.staging_slot_idx);
            }
        };

        if (task.epoch != m_shutdownGate.epoch.load(std::memory_order_acquire)) {
            returnStagingSlot();
            continue;
        }

        uint8_t *slot_ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_stagingMutex);
            if (m_stagingBuffer != nullptr && task.staging_slot_idx < m_stagingSlots.size()) {
                slot_ptr = m_stagingBuffer + m_stagingSlots[task.staging_slot_idx].ring_offset;
            }
        }

        if (!slot_ptr) {
            returnStagingSlot();
            continue;
        }

        bool ok = decodeThumbnail(task.media_path, task.preview_path, slot_ptr, stop_token,
                                  task.epoch);
        const bool cancelled =
            m_shutdownGate.closing.load(std::memory_order_acquire) ||
            stop_token.stop_requested() ||
            task.epoch != m_shutdownGate.epoch.load(std::memory_order_acquire);
        if (cancelled) {
            returnStagingSlot();
            continue;
        }

        auto etc2Buf = std::make_shared<std::vector<uint8_t>>();
        etc2::encodeRgbaToEtc2Simd(slot_ptr, static_cast<int>(THUMB_WIDTH),
                                   static_cast<int>(THUMB_HEIGHT),
                                   static_cast<int>(THUMB_WIDTH * BYTES_PER_PIXEL), *etc2Buf);

        bool published = false;
        {
            std::scoped_lock lock(m_uploadMutex, m_stagingMutex);
            if (!m_shutdownGate.closing.load(std::memory_order_acquire) &&
                task.epoch == m_shutdownGate.epoch.load(std::memory_order_acquire) &&
                task.staging_slot_idx < m_stagingSlots.size()) {
                m_stagingSlots[task.staging_slot_idx].decode_succeeded = ok;
                m_stagingSlots[task.staging_slot_idx].etc2_blocks = std::move(etc2Buf);
                m_stagingSlots[task.staging_slot_idx].ready_for_upload = true;
                m_uploadQueue.push(task.staging_slot_idx);
                published = true;
            } else if (task.staging_slot_idx < m_stagingSlots.size()) {
                m_stagingSlots[task.staging_slot_idx].ready_for_upload = false;
                m_stagingSlots[task.staging_slot_idx].etc2_blocks.reset();
                m_freeStagingSlots.push(task.staging_slot_idx);
            }
        }

        if (published) {
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
