// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/brush/dirty_tile_undo_stack.hpp"

#include "waywal/we_asset_writer.h"

#include <algorithm>
#include <cstring>

namespace waywal::studio {

namespace {

constexpr uint32_t kMaxCanvasDim = 16384u;
constexpr size_t kMaxTileRawBytes =
    static_cast<size_t>(WE_BRUSH_TILE_SIZE) * static_cast<size_t>(WE_BRUSH_TILE_SIZE) * 4u;

size_t snapshotHostBytes(const CompressedTileSnapshot &snap) noexcept
{
    return snap.payload.size();
}

size_t transactionHostBytes(const DirtyTileTransaction &tx) noexcept
{
    size_t sum = 0;
    for (const auto &tile : tx.preTiles) {
        sum += snapshotHostBytes(tile);
    }
    for (const auto &tile : tx.postTiles) {
        sum += snapshotHostBytes(tile);
    }
    return sum;
}

uint32_t sanitizeBytesPerPixel(uint32_t bpp, we_brush_channel_t channel) noexcept
{
    if (bpp == 1u || bpp == 2u || bpp == 4u) {
        return bpp;
    }
    switch (channel) {
    case WE_BRUSH_CHAN_MASK_R8:
        return 1u;
    case WE_BRUSH_CHAN_FLOW_RG88:
        return 2u;
    case WE_BRUSH_CHAN_COLOR_RGBA8:
    case WE_BRUSH_CHAN_BLUR:
    case WE_BRUSH_CHAN_SMUDGE:
        return 4u;
    }
    return 4u;
}

bool readLz4LsicLength(const uint8_t *src, size_t srcLen, size_t &cursor, size_t &outLen) noexcept
{
    while (true) {
        if (cursor >= srcLen) {
            return false;
        }
        const uint8_t b = src[cursor++];
        if (outLen > SIZE_MAX - static_cast<size_t>(b)) {
            return false;
        }
        outLen += static_cast<size_t>(b);
        if (b != 255u) {
            return true;
        }
    }
}

bool decompressLz4Block(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) noexcept
{
    if (!src || !dst || srcLen == 0 || dstLen == 0) {
        return false;
    }

    size_t ip = 0;
    size_t op = 0;

    while (ip < srcLen) {
        const uint8_t token = src[ip++];
        size_t litLen = static_cast<size_t>(token >> 4u);
        if (litLen == 15u && !readLz4LsicLength(src, srcLen, ip, litLen)) {
            return false;
        }

        if (litLen > srcLen - ip || litLen > dstLen - op) {
            return false;
        }
        if (litLen > 0u) {
            std::memcpy(dst + op, src + ip, litLen);
            ip += litLen;
            op += litLen;
        }

        if (ip == srcLen) {
            break;
        }
        if (ip + 2u > srcLen) {
            return false;
        }

        const size_t offset =
            static_cast<size_t>(src[ip]) | (static_cast<size_t>(src[ip + 1u]) << 8u);
        ip += 2u;
        if (offset == 0u || offset > op) {
            return false;
        }

        size_t matchLen = static_cast<size_t>(token & 0x0Fu) + 4u;
        if ((token & 0x0Fu) == 15u && !readLz4LsicLength(src, srcLen, ip, matchLen)) {
            return false;
        }
        if (matchLen > dstLen - op) {
            return false;
        }

        // Copy byte-by-byte because LZ4 matches may overlap when offset < matchLen.
        size_t ref = op - offset;
        for (size_t i = 0; i < matchLen; ++i) {
            dst[op++] = dst[ref++];
        }
    }

    return op == dstLen;
}

} // namespace

void DirtyTileUndoStack::beginTransaction(uint64_t strokeId, int32_t objectId, uint32_t passIndex,
                                          uint32_t texSlot, we_brush_channel_t channel,
                                          uint32_t bytesPerPixel, uint32_t canvasWidth,
                                          uint32_t canvasHeight)
{
    if (m_inTransaction) {
        abortTransaction();
    }

    const uint32_t clampedW = std::clamp(canvasWidth, 1u, kMaxCanvasDim);
    const uint32_t clampedH = std::clamp(canvasHeight, 1u, kMaxCanvasDim);
    m_tilesX = (clampedW + kTileSizePx - 1u) / kTileSizePx;
    m_tilesY = (clampedH + kTileSizePx - 1u) / kTileSizePx;

    const size_t totalTiles = static_cast<size_t>(m_tilesX) * static_cast<size_t>(m_tilesY);
    const size_t wordCount = (totalTiles + 63u) / 64u;
    m_activeBitset.assign(wordCount, 0u);

    m_activeTx = DirtyTileTransaction{};
    m_activeTx.strokeId = strokeId;
    m_activeTx.objectId = objectId;
    m_activeTx.passIndex = passIndex;
    m_activeTx.texSlot = texSlot;
    m_activeTx.channel = channel;
    m_activeTx.bytesPerPixel = sanitizeBytesPerPixel(bytesPerPixel, channel);
    m_inTransaction = true;
}

bool DirtyTileUndoStack::markTileTouched(uint16_t tx, uint16_t ty)
{
    if (!m_inTransaction || tx >= m_tilesX || ty >= m_tilesY) {
        return false;
    }

    const size_t bitIdx = static_cast<size_t>(ty) * static_cast<size_t>(m_tilesX) + tx;
    const size_t wordIdx = bitIdx >> 6u;
    const uint64_t mask = uint64_t{1} << (bitIdx & 63u);
    if (wordIdx >= m_activeBitset.size() || (m_activeBitset[wordIdx] & mask) != 0u) {
        return false;
    }

    m_activeBitset[wordIdx] |= mask;
    return true;
}

void DirtyTileUndoStack::upsertTileSnapshot(std::vector<CompressedTileSnapshot> &tiles, uint16_t tx,
                                            uint16_t ty, const uint8_t *rawTileBytes,
                                            size_t byteLen)
{
    if (!m_inTransaction || !rawTileBytes || byteLen == 0u || byteLen > kMaxTileRawBytes) {
        return;
    }
    if (tx >= m_tilesX || ty >= m_tilesY) {
        return;
    }

    const DirtyTileCoord coord{tx, ty};
    CompressedTileSnapshot snap = compressTile(coord, rawTileBytes, byteLen);
    if (snap.payload.empty()) {
        return;
    }

    for (auto &existing : tiles) {
        if (existing.coord == coord) {
            existing = std::move(snap);
            return;
        }
    }
    tiles.push_back(std::move(snap));
}

void DirtyTileUndoStack::recordPreTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes,
                                            size_t byteLen)
{
    (void)markTileTouched(tx, ty);
    upsertTileSnapshot(m_activeTx.preTiles, tx, ty, rawTileBytes, byteLen);
}

void DirtyTileUndoStack::recordPostTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes,
                                             size_t byteLen)
{
    upsertTileSnapshot(m_activeTx.postTiles, tx, ty, rawTileBytes, byteLen);
}

void DirtyTileUndoStack::recordPostTileBytesForStroke(uint64_t strokeId, uint16_t tx, uint16_t ty,
                                                      const uint8_t *rawTileBytes, size_t byteLen,
                                                      uint32_t canvasWidth, uint32_t canvasHeight)
{
    if (!rawTileBytes || byteLen == 0u || byteLen > kMaxTileRawBytes) {
        return;
    }
    const uint32_t clampedW = std::clamp(canvasWidth, 1u, kMaxCanvasDim);
    const uint32_t clampedH = std::clamp(canvasHeight, 1u, kMaxCanvasDim);
    const uint32_t tilesX = (clampedW + kTileSizePx - 1u) / kTileSizePx;
    const uint32_t tilesY = (clampedH + kTileSizePx - 1u) / kTileSizePx;
    if (tx >= tilesX || ty >= tilesY) {
        return;
    }

    const auto it = m_strokeIndex.find(strokeId);
    if (it == m_strokeIndex.end() || it->second < m_evictedCount) {
        return;
    }
    const size_t idx = it->second - m_evictedCount;
    if (idx >= m_transactions.size()) {
        return;
    }

    DirtyTileTransaction &txObj = m_transactions[idx];
    const DirtyTileCoord coord{tx, ty};
    CompressedTileSnapshot snap = compressTile(coord, rawTileBytes, byteLen);
    if (snap.payload.empty()) {
        return;
    }

    const size_t prevBytes = transactionHostBytes(txObj);
    bool replaced = false;
    for (auto &existing : txObj.postTiles) {
        if (existing.coord == coord) {
            existing = std::move(snap);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        txObj.postTiles.push_back(std::move(snap));
    }
    const size_t nextBytes = transactionHostBytes(txObj);
    if (nextBytes >= prevBytes) {
        m_totalHostBytes += (nextBytes - prevBytes);
    } else {
        const size_t diff = prevBytes - nextBytes;
        m_totalHostBytes = (diff <= m_totalHostBytes) ? (m_totalHostBytes - diff) : 0u;
    }
    evictOldestIfOverBudget();
}

void DirtyTileUndoStack::commitTransaction()
{
    if (!m_inTransaction) {
        return;
    }

    const size_t txBytes = transactionHostBytes(m_activeTx);
    const uint64_t strokeId = m_activeTx.strokeId;

    m_transactions.push_back(std::move(m_activeTx));
    // Store monotonic sequence so FIFO pop_front() never invalidates remaining map entries.
    m_strokeIndex[strokeId] = m_evictedCount + m_transactions.size() - 1u;
    m_totalHostBytes += txBytes;

    m_inTransaction = false;
    m_activeTx = DirtyTileTransaction{};
    m_activeBitset.clear();
    m_tilesX = 0;
    m_tilesY = 0;

    evictOldestIfOverBudget();
}

void DirtyTileUndoStack::abortTransaction()
{
    m_inTransaction = false;
    m_activeTx = DirtyTileTransaction{};
    m_tilesX = 0;
    m_tilesY = 0;
    std::vector<uint64_t>{}.swap(m_activeBitset);
}

const DirtyTileTransaction *DirtyTileUndoStack::findTransaction(uint64_t strokeId) const noexcept
{
    const auto it = m_strokeIndex.find(strokeId);
    if (it == m_strokeIndex.end() || it->second < m_evictedCount) {
        return nullptr;
    }

    const size_t idx = it->second - m_evictedCount;
    if (idx >= m_transactions.size()) {
        return nullptr;
    }
    return &m_transactions[idx];
}

bool DirtyTileUndoStack::decompressTile(const CompressedTileSnapshot &snap,
                                        std::vector<uint8_t> &outRaw)
{
    if (snap.rawByteSize == 0u || snap.rawByteSize > kMaxTileRawBytes || snap.payload.empty()) {
        return false;
    }

    if (!snap.isLz4) {
        if (snap.payload.size() != snap.rawByteSize) {
            return false;
        }
        outRaw = snap.payload;
        return true;
    }

    outRaw.resize(snap.rawByteSize);
    if (!decompressLz4Block(snap.payload.data(), snap.payload.size(), outRaw.data(),
                            outRaw.size())) {
        outRaw.clear();
        return false;
    }
    return true;
}

void DirtyTileUndoStack::clear()
{
    abortTransaction();
    m_transactions.clear();
    m_strokeIndex.clear();
    m_totalHostBytes = 0;
    m_evictedCount = 0;
}

void DirtyTileUndoStack::evictOldestIfOverBudget()
{
    while (m_totalHostBytes > kMaxHostCacheBytes && !m_transactions.empty()) {
        const auto &oldest = m_transactions.front();
        const size_t freed = transactionHostBytes(oldest);
        const auto it = m_strokeIndex.find(oldest.strokeId);
        if (it != m_strokeIndex.end() && it->second == m_evictedCount) {
            m_strokeIndex.erase(it);
        }

        m_transactions.pop_front();
        ++m_evictedCount;
        m_totalHostBytes = (freed <= m_totalHostBytes) ? (m_totalHostBytes - freed) : 0u;
    }
}

CompressedTileSnapshot DirtyTileUndoStack::compressTile(DirtyTileCoord coord,
                                                        const uint8_t *rawBytes, size_t byteLen)
{
    CompressedTileSnapshot snap{};
    snap.coord = coord;
    if (!rawBytes || byteLen == 0u || byteLen > kMaxTileRawBytes) {
        return snap;
    }

    snap.rawByteSize = static_cast<uint32_t>(byteLen);
    const size_t bound = we_lz4_compress_bound(byteLen);
    if (bound > 0u) {
        std::vector<uint8_t> compBuf(bound);
        const size_t compSize =
            we_lz4_compress_block(rawBytes, byteLen, compBuf.data(), compBuf.size());
        if (compSize > 0u && compSize < byteLen) {
            compBuf.resize(compSize);
            compBuf.shrink_to_fit();
            snap.isLz4 = true;
            snap.payload = std::move(compBuf);
            return snap;
        }
    }

    snap.isLz4 = false;
    snap.payload.assign(rawBytes, rawBytes + byteLen);
    return snap;
}

} // namespace waywal::studio
