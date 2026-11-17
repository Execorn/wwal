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
    uint32_t rawByteSize{0}; /* 4096..262144 depending on tile size and BPP */
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

struct MacroTileDelta {
    uint32_t tile_x{0};
    uint32_t tile_y{0};
    uint32_t uncompressed_bytes{0};
    std::vector<uint8_t> compressed_xor_lz4;
};

struct BrushStrokeUndoEntry {
    uint64_t stroke_id{0};
    std::vector<MacroTileDelta> tiles;

    [[nodiscard]] std::size_t accounted_bytes() const noexcept;
};

/* Bounded 64 MiB XOR-LZ4 macro-tile undo/redo slab (256x256 macro-pages). */
class MacroTileUndoSlab {
  public:
    static constexpr uint32_t kMacroTileSizePx = 256u;
    static constexpr std::size_t kMaxSlabBytes = 64U * 1024U * 1024U;

    bool push(BrushStrokeUndoEntry entry);
    [[nodiscard]] bool can_undo() const noexcept { return cursor_ > 0U; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] const BrushStrokeUndoEntry *undo() noexcept;
    [[nodiscard]] const BrushStrokeUndoEntry *redo() noexcept;
    [[nodiscard]] const BrushStrokeUndoEntry *find(uint64_t strokeId) const noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return resident_bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

    [[nodiscard]] static bool encodeXorDelta(uint32_t tileX, uint32_t tileY,
                                             const uint8_t *preBytes, const uint8_t *postBytes,
                                             size_t byteLen, MacroTileDelta &outDelta);
    [[nodiscard]] static bool applyXorDelta(const MacroTileDelta &delta,
                                            const uint8_t *currentBytes, uint8_t *outRestoredBytes,
                                            size_t byteLen);

  private:
    std::vector<BrushStrokeUndoEntry> history_{};
    std::size_t cursor_{0U};
    std::size_t resident_bytes_{0U};
};

/* Studio-side dirty-tile CoW undo/redo coordinator capped strictly at 64 MiB. */
class DirtyTileUndoStack {
  public:
    static constexpr uint32_t kTileSizePx = WE_BRUSH_TILE_SIZE;
    static constexpr uint32_t kMacroTileSizePx = MacroTileUndoSlab::kMacroTileSizePx;
    static constexpr size_t kMaxHostCacheBytes = 64u * 1024u * 1024u;

    DirtyTileUndoStack() = default;
    ~DirtyTileUndoStack() = default;

    void beginTransaction(uint64_t strokeId, int32_t objectId, uint32_t passIndex, uint32_t texSlot,
                          we_brush_channel_t channel, uint32_t bytesPerPixel, uint32_t canvasWidth,
                          uint32_t canvasHeight);

    /* Returns true iff this (tx, ty) has not yet been marked in the active transaction. */
    [[nodiscard]] bool markTileTouched(uint16_t tx, uint16_t ty);

    void recordPreTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes, size_t byteLen);
    void recordPostTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes, size_t byteLen);
    void recordPostTileBytesForStroke(uint64_t strokeId, uint16_t tx, uint16_t ty,
                                      const uint8_t *rawTileBytes, size_t byteLen,
                                      uint32_t canvasWidth, uint32_t canvasHeight);
    void commitTransaction();
    void abortTransaction();

    [[nodiscard]] bool canUndo() const noexcept { return m_cursor > 0u; }
    [[nodiscard]] bool canRedo() const noexcept { return m_cursor < m_transactions.size(); }
    [[nodiscard]] const DirtyTileTransaction *undo() noexcept;
    [[nodiscard]] const DirtyTileTransaction *redo() noexcept;
    [[nodiscard]] size_t cursor() const noexcept { return m_cursor; }
    [[nodiscard]] size_t evictedCount() const noexcept { return m_evictedCount; }

    [[nodiscard]] MacroTileUndoSlab &macroSlab() noexcept { return m_macroSlab; }
    [[nodiscard]] const MacroTileUndoSlab &macroSlab() const noexcept { return m_macroSlab; }

    [[nodiscard]] const DirtyTileTransaction *findTransaction(uint64_t strokeId) const noexcept;
    [[nodiscard]] static bool decompressTile(const CompressedTileSnapshot &snap,
                                             std::vector<uint8_t> &outRaw);
    [[nodiscard]] bool isInTransaction() const noexcept { return m_inTransaction; }
    [[nodiscard]] size_t transactionCount() const noexcept { return m_transactions.size(); }
    [[nodiscard]] size_t totalHostBytes() const noexcept
    {
        return m_totalHostBytes + m_macroSlab.resident_bytes();
    }
    void clear();

  private:
    void evictOldestIfOverBudget();
    void upsertTileSnapshot(std::vector<CompressedTileSnapshot> &tiles,
                            std::unordered_map<uint32_t, size_t> &tileIndex, uint16_t tx,
                            uint16_t ty, const uint8_t *rawTileBytes, size_t byteLen);
    static CompressedTileSnapshot compressTile(DirtyTileCoord coord, const uint8_t *rawBytes,
                                               size_t byteLen);

    bool m_inTransaction{false};
    DirtyTileTransaction m_activeTx;
    uint32_t m_tilesX{0};
    uint32_t m_tilesY{0};
    std::vector<uint64_t> m_activeBitset; /* 256 B at 4K (2,040 tiles) */
    std::unordered_map<uint32_t, size_t> m_activePreIndex;
    std::unordered_map<uint32_t, size_t> m_activePostIndex;
    std::deque<DirtyTileTransaction> m_transactions;
    std::unordered_map<uint64_t, size_t> m_strokeIndex;
    size_t m_cursor{0};
    size_t m_totalHostBytes{0};
    size_t m_evictedCount{0};
    MacroTileUndoSlab m_macroSlab;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_BRUSH_DIRTY_TILE_UNDO_STACK_HPP
