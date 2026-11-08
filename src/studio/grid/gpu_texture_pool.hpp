// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_GPU_TEXTURE_POOL_HPP
#define WWAL_STUDIO_GRID_GPU_TEXTURE_POOL_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include <QImage>
#include <QOpenGLExtraFunctions>

namespace wwal::studio {

struct PboUploadQueue {
    GLuint pbo_ids[2]{0, 0};
    uint32_t active_pbo{0};
    size_t buffer_size{384 * 216 * 4}; // 331,776 bytes per thumbnail
    bool initialized{false};

    void init(QOpenGLExtraFunctions *gl);
    void destroy(QOpenGLExtraFunctions *gl);
    void uploadAsync(QOpenGLExtraFunctions *gl, const uint8_t *decoded_rgba,
                     GLuint target_tex_layer, GLuint tex_array_id);
};

struct TexturePoolStats {
    size_t total_vram_bytes{0};
    size_t active_slots{0};
    size_t max_slots{0};
    size_t staging_buffer_bytes{0};
    size_t disk_hits{0};
    size_t disk_misses{0};
};

class GpuTexturePool {
  public:
    static constexpr uint32_t THUMB_WIDTH = 384;
    static constexpr uint32_t THUMB_HEIGHT = 216;
    static constexpr size_t BYTES_PER_PIXEL = 4;
    static constexpr size_t FRAME_SIZE_BYTES =
        THUMB_WIDTH * THUMB_HEIGHT * BYTES_PER_PIXEL;                // 331,776 bytes
    static constexpr size_t STAGING_BUFFER_SIZE = 128 * 1024 * 1024; // 128 MB L2 CPU ringbuffer
    static constexpr size_t STAGING_SLOT_COUNT =
        STAGING_BUFFER_SIZE / FRAME_SIZE_BYTES;         // ~404 slots
    static constexpr int DEFAULT_MAX_VRAM_SLOTS = 1500; // <= 1,540 slots (512 MB VRAM cap)

    explicit GpuTexturePool(int max_vram_slots = DEFAULT_MAX_VRAM_SLOTS);
    ~GpuTexturePool();

    GpuTexturePool(const GpuTexturePool &) = delete;
    GpuTexturePool &operator=(const GpuTexturePool &) = delete;

    // OpenGL Lifecycle (must be called with active OpenGL context)
    bool initGl(QOpenGLExtraFunctions *gl);
    void destroyGl(QOpenGLExtraFunctions *gl);

    [[nodiscard]] GLuint textureArrayId() const noexcept { return m_texArrayId; }
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // Notification callback invoked when a background worker completes decoding a thumbnail
    void setThumbnailReadyCallback(std::function<void()> callback);

    // Request texture for an item.
    // If resident: returns layer index (>= 0) and touches LRU.
    // If pending: returns -1 and dispatches background decode/upload.
    int requestTexture(uint32_t item_id, const std::string &media_path,
                       const std::string &preview_path = "");

    // Called on the UI render thread every frame (e.g., in paintGL / paintEvent).
    // Uploads decoded frames from L2 staging ringbuffer into L3 VRAM via dual-PBO
    // ping-pong. Returns number of frames uploaded this frame.
    int processPendingUploads(QOpenGLExtraFunctions *gl, int max_uploads_per_frame = 6);

    [[nodiscard]] bool isTextureReady(uint32_t item_id) const;
    [[nodiscard]] int getLayerIndex(uint32_t item_id) const;
    [[nodiscard]] QImage getThumbnailImage(uint32_t item_id) const;
    void touch(uint32_t item_id);

    // Evicts all textures from VRAM and cancels background tasks
    void clear();

    [[nodiscard]] TexturePoolStats stats() const;

  private:
    struct LruNode {
        uint32_t item_id{0};
        int layer_index{-1};
        std::string media_path;
        bool decode_queued{false};
        bool ready{false};
        QImage thumbnail;
        LruNode *prev{nullptr};
        LruNode *next{nullptr};
    };

    struct StagingSlot {
        uint32_t item_id{0};
        int target_layer{-1};
        std::string media_path;
        size_t ring_offset{0};
        bool ready_for_upload{false};
        bool decode_succeeded{true};
    };

    struct WorkerTask {
        uint32_t item_id{0};
        int target_layer{-1};
        std::string media_path;
        std::string preview_path;
        size_t staging_slot_idx{0};
    };

    int m_maxVramSlots;
    GLuint m_texArrayId{0};
    bool m_initialized{false};
    PboUploadQueue m_pboQueue;

    // L2: 128 MB Page-Aligned CPU Staging Ringbuffer
    uint8_t *m_stagingBuffer{nullptr};
    std::vector<StagingSlot> m_stagingSlots;
    std::queue<size_t> m_freeStagingSlots;
    std::mutex m_stagingMutex;

    // Completed Staging Slots awaiting GPU upload
    std::queue<size_t> m_uploadQueue;
    std::mutex m_uploadMutex;

    // L3: O(1) LRU Doubly Linked List & Hash Map
    std::unordered_map<uint32_t, LruNode *> m_itemToNode;
    std::unordered_map<int, LruNode *> m_layerToNode;
    std::vector<int> m_freeLayers;
    LruNode *m_lruHead{nullptr}; // MRU
    LruNode *m_lruTail{nullptr}; // LRU
    mutable std::mutex m_lruMutex;

    // Background Thread Pool (Worker threads for L1 disk -> L2 staging decode)
    std::vector<std::thread> m_workerThreads;
    std::queue<WorkerTask> m_taskQueue;
    std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    std::atomic<bool> m_stopWorkers{false};

    mutable std::mutex m_callbackMutex;
    std::function<void()> m_thumbnailReadyCallback;

    // Telemetry & Stats
    mutable std::mutex m_statsMutex;
    TexturePoolStats m_stats;

    // Helper methods
    void initStagingMemory();
    void freeStagingMemory();
    void initLru();
    void destroyLru();
    void detachLruNode(LruNode *node);
    void insertLruHead(LruNode *node);
    int allocateLayerFor(uint32_t item_id);

    void workerLoop();
    bool decodeThumbnail(const std::string &path, const std::string &preview, uint8_t *out_rgba);
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_GPU_TEXTURE_POOL_HPP
