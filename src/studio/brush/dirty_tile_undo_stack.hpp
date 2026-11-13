// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_BRUSH_DIRTY_TILE_UNDO_STACK_HPP
#define WAYWAL_STUDIO_BRUSH_DIRTY_TILE_UNDO_STACK_HPP

#include "waywal/we_gpu_brush.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace waywal::studio {

struct DirtyTileCoord {
    uint16_t tx{0};
    uint16_t ty{0};

    [[nodiscard]] bool operator==(const DirtyTileCoord &other) const noexcept = default;
};

struct DirtyTileCoordHash {
    [[nodiscard]] size_t operator()(const DirtyTileCoord &c) const noexcept
    {
        return (static_cast<size_t>(c.tx) << 16) | static_cast<size_t>(c.ty);
    }
};

struct CompressedTileSnapshot {
    DirtyTileCoord coord;
    uint32_t rawByteSize{0}; /* 4096 (R8), 8192 (RG88), or 16384 (RGBA8) */
    bool isLz4{false};
    std::vector<uint8_t> payload;
};

struct DirtyTileTransaction {
    uint64_t strokeId{0};
    int32_t objectId{-1};
    uint32_t passIndex{0};
    uint32_t texSlot{0};
    we_brush_channel_t channel{WE_BRUSH_CHAN_MASK_R8};
    uint32_t bytesPerPixel{1};
    std::vector<CompressedTileSnapshot> preTiles;
    std::vector<CompressedTileSnapshot> postTiles;
};

/* Studio-side 64x64 CoW dirty-tile host cache and VRAM pool coordinator.
 * Enforces a bounded 64 MiB memory cap via FIFO LZ4 compression spillover. */
class DirtyTileUndoStack {
  public:
    static constexpr uint32_t kTileSizePx = WE_BRUSH_TILE_SIZE;
    static constexpr size_t kMaxHostCacheBytes = 64u * 1024u * 1024u;

    DirtyTileUndoStack() = default;
    ~DirtyTileUndoStack() = default;

    void beginTransaction(uint64_t strokeId, int32_t objectId, uint32_t passIndex, uint32_t texSlot,
                          we_brush_channel_t channel, uint32_t bytesPerPixel, uint32_t canvasWidth,
                          uint32_t canvasHeight);

    /* Returns true iff this (tx, ty) has not yet been snapshotted in the active transaction. */
    [[nodiscard]] bool markTileTouched(uint16_t tx, uint16_t ty);

    void recordPreTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes, size_t byteLen);
    void recordPostTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes, size_t byteLen);
    void recordPostTileBytesForStroke(uint64_t strokeId, uint16_t tx, uint16_t ty,
                                      const uint8_t *rawTileBytes, size_t byteLen,
                                      uint32_t canvasWidth, uint32_t canvasHeight);
    void commitTransaction();
    void abortTransaction();

    [[nodiscard]] const DirtyTileTransaction *findTransaction(uint64_t strokeId) const noexcept;
    [[nodiscard]] static bool decompressTile(const CompressedTileSnapshot &snap,
                                             std::vector<uint8_t> &outRaw);
    [[nodiscard]] bool isInTransaction() const noexcept { return m_inTransaction; }
    [[nodiscard]] size_t transactionCount() const noexcept { return m_transactions.size(); }
    [[nodiscard]] size_t totalHostBytes() const noexcept { return m_totalHostBytes; }
    void clear();

  private:
    void evictOldestIfOverBudget();
    void upsertTileSnapshot(std::vector<CompressedTileSnapshot> &tiles, uint16_t tx, uint16_t ty,
                            const uint8_t *rawTileBytes, size_t byteLen);
    static CompressedTileSnapshot compressTile(DirtyTileCoord coord, const uint8_t *rawBytes,
                                               size_t byteLen);

    bool m_inTransaction{false};
    DirtyTileTransaction m_activeTx;
    uint32_t m_tilesX{0};
    uint32_t m_tilesY{0};
    std::vector<uint64_t> m_activeBitset; /* 256 B at 4K (2,040 tiles) */
    std::deque<DirtyTileTransaction> m_transactions;
    std::unordered_map<uint64_t, size_t> m_strokeIndex;
    size_t m_totalHostBytes{0};
    size_t m_evictedCount{0};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_BRUSH_DIRTY_TILE_UNDO_STACK_HPP
