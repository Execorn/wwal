// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/brush/dirty_tile_undo_stack.hpp"

#include "waywal/log.h"
#include "waywal/we_asset_writer.h"

#include <dlfcn.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>

namespace waywal::studio {

namespace {

constexpr uint32_t kMaxCanvasDim = 16384u;
constexpr size_t kMaxTileRawBytes = static_cast<size_t>(MacroTileUndoSlab::kMacroTileSizePx) *
                                    static_cast<size_t>(MacroTileUndoSlab::kMacroTileSizePx) * 4u;

struct Lz4Runtime {
    using CompressBoundFn = int (*)(int);
    using CompressFastFn = int (*)(const char *, char *, int, int, int);
    using CompressDefaultFn = int (*)(const char *, char *, int, int);
    using DecompressSafeFn = int (*)(const char *, char *, int, int);

    CompressBoundFn compressBound{nullptr};
    CompressFastFn compressFast{nullptr};
    CompressDefaultFn compressDefault{nullptr};
    DecompressSafeFn decompressSafe{nullptr};
    bool available{false};
};

const Lz4Runtime &lz4Runtime() noexcept
{
    static Lz4Runtime api;
    static std::once_flag once;
    std::call_once(once, []() {
        // Resolve liblz4 lazily because wwal-studio intentionally avoids a DT_NEEDED link to
        // liblz4.
        void *handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
        if (!handle) {
            return;
        }
        api.compressBound =
            reinterpret_cast<Lz4Runtime::CompressBoundFn>(dlsym(handle, "LZ4_compressBound"));
        api.compressFast =
            reinterpret_cast<Lz4Runtime::CompressFastFn>(dlsym(handle, "LZ4_compress_fast"));
        api.compressDefault =
            reinterpret_cast<Lz4Runtime::CompressDefaultFn>(dlsym(handle, "LZ4_compress_default"));
        api.decompressSafe =
            reinterpret_cast<Lz4Runtime::DecompressSafeFn>(dlsym(handle, "LZ4_decompress_safe"));
        api.available = (api.compressBound != nullptr) &&
                        (api.compressFast != nullptr || api.compressDefault != nullptr) &&
                        (api.decompressSafe != nullptr);
    });
    return api;
}

size_t lz4CompressBoundCompat(size_t srcLen) noexcept
{
    if (srcLen == 0u || srcLen > kMaxTileRawBytes) {
        return 0u;
    }
    const Lz4Runtime &rt = lz4Runtime();
    if (rt.available && srcLen <= static_cast<size_t>(INT_MAX)) {
        const int b = rt.compressBound(static_cast<int>(srcLen));
        if (b > 0) {
            return static_cast<size_t>(b);
        }
    }
    return we_lz4_compress_bound(srcLen);
}

size_t lz4CompressFastCompat(const uint8_t *src, size_t srcLen, uint8_t *dst,
                             size_t dstCap) noexcept
{
    if (!src || !dst || srcLen == 0u || srcLen > kMaxTileRawBytes || dstCap == 0u) {
        return 0u;
    }
    const Lz4Runtime &rt = lz4Runtime();
    if (rt.available && srcLen <= static_cast<size_t>(INT_MAX) &&
        dstCap <= static_cast<size_t>(INT_MAX)) {
        const int inLen = static_cast<int>(srcLen);
        const int outCap = static_cast<int>(dstCap);
        const int res = rt.compressFast
                            ? rt.compressFast(reinterpret_cast<const char *>(src),
                                              reinterpret_cast<char *>(dst), inLen, outCap, 1)
                            : rt.compressDefault(reinterpret_cast<const char *>(src),
                                                 reinterpret_cast<char *>(dst), inLen, outCap);
        if (res > 0) {
            return static_cast<size_t>(res);
        }
    }
    return we_lz4_compress_block(src, srcLen, dst, dstCap);
}

uint32_t tileCoordKey(uint16_t tx, uint16_t ty) noexcept
{
    return (static_cast<uint32_t>(ty) << 16u) | static_cast<uint32_t>(tx);
}

size_t transactionHostBytes(const DirtyTileTransaction &tx) noexcept
{
    size_t sum = sizeof(DirtyTileTransaction) +
                 tx.preTiles.capacity() * sizeof(CompressedTileSnapshot) +
                 tx.postTiles.capacity() * sizeof(CompressedTileSnapshot);
    for (const auto &tile : tx.preTiles) {
        sum += tile.payload.capacity();
    }
    for (const auto &tile : tx.postTiles) {
        sum += tile.payload.capacity();
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

bool decompressLz4BlockFallback(const uint8_t *src, size_t srcLen, uint8_t *dst,
                                size_t dstLen) noexcept
{
    if (!src || !dst || srcLen == 0u || dstLen == 0u) {
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

bool lz4DecompressCompat(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen) noexcept
{
    if (!src || !dst || srcLen == 0u || dstLen == 0u) {
        return false;
    }
    const Lz4Runtime &rt = lz4Runtime();
    if (rt.available && srcLen <= static_cast<size_t>(INT_MAX) &&
        dstLen <= static_cast<size_t>(INT_MAX)) {
        const int dec =
            rt.decompressSafe(reinterpret_cast<const char *>(src), reinterpret_cast<char *>(dst),
                              static_cast<int>(srcLen), static_cast<int>(dstLen));
        if (dec == static_cast<int>(dstLen)) {
            return true;
        }
    }
    return decompressLz4BlockFallback(src, srcLen, dst, dstLen);
}

void xorBuffers(const uint8_t *a, const uint8_t *b, uint8_t *dst, size_t len) noexcept
{
    size_t i = 0;
    for (; i + 8u <= len; i += 8u) {
        uint64_t va = 0;
        uint64_t vb = 0;
        std::memcpy(&va, a + i, 8u);
        std::memcpy(&vb, b + i, 8u);
        const uint64_t vx = va ^ vb;
        std::memcpy(dst + i, &vx, 8u);
    }
    for (; i < len; ++i) {
        dst[i] = static_cast<uint8_t>(a[i] ^ b[i]);
    }
}

thread_local std::vector<uint8_t> g_tlLz4Scratch;
thread_local std::vector<uint8_t> g_tlXorScratch;

} // namespace

std::size_t BrushStrokeUndoEntry::accounted_bytes() const noexcept
{
    std::size_t sum = sizeof(BrushStrokeUndoEntry) + tiles.capacity() * sizeof(MacroTileDelta);
    for (const auto &t : tiles) {
        sum += t.compressed_xor_lz4.capacity();
    }
    return sum;
}

bool MacroTileUndoSlab::push(BrushStrokeUndoEntry entry)
{
    entry.tiles.shrink_to_fit();
    for (auto &t : entry.tiles) {
        t.compressed_xor_lz4.shrink_to_fit();
    }
    const std::size_t bytes = entry.accounted_bytes();
    if (bytes > kMaxSlabBytes) {
        return false;
    }
    while (history_.size() > cursor_) {
        const std::size_t freed = history_.back().accounted_bytes();
        resident_bytes_ = (freed <= resident_bytes_) ? (resident_bytes_ - freed) : 0U;
        history_.pop_back();
    }
    while (!history_.empty() && (resident_bytes_ + bytes > kMaxSlabBytes)) {
        const std::size_t freed = history_.front().accounted_bytes();
        resident_bytes_ = (freed <= resident_bytes_) ? (resident_bytes_ - freed) : 0U;
        history_.erase(history_.begin());
        if (cursor_ > 0U) {
            --cursor_;
        }
    }
    resident_bytes_ += bytes;
    history_.push_back(std::move(entry));
    cursor_ = history_.size();
    return true;
}

const BrushStrokeUndoEntry *MacroTileUndoSlab::undo() noexcept
{
    if (!can_undo()) {
        return nullptr;
    }
    --cursor_;
    return &history_[cursor_];
}

const BrushStrokeUndoEntry *MacroTileUndoSlab::redo() noexcept
{
    if (!can_redo()) {
        return nullptr;
    }
    return &history_[cursor_++];
}

const BrushStrokeUndoEntry *MacroTileUndoSlab::find(uint64_t strokeId) const noexcept
{
    for (size_t i = history_.size(); i > 0u; --i) {
        if (history_[i - 1u].stroke_id == strokeId) {
            return &history_[i - 1u];
        }
    }
    return nullptr;
}

void MacroTileUndoSlab::clear() noexcept
{
    history_.clear();
    history_.shrink_to_fit();
    cursor_ = 0U;
    resident_bytes_ = 0U;
}

bool MacroTileUndoSlab::encodeXorDelta(uint32_t tileX, uint32_t tileY, const uint8_t *preBytes,
                                       const uint8_t *postBytes, size_t byteLen,
                                       MacroTileDelta &outDelta)
{
    if (!preBytes || !postBytes || byteLen == 0u || byteLen > kMaxTileRawBytes) {
        return false;
    }

    g_tlXorScratch.resize(byteLen);
    xorBuffers(preBytes, postBytes, g_tlXorScratch.data(), byteLen);

    outDelta.tile_x = tileX;
    outDelta.tile_y = tileY;
    outDelta.uncompressed_bytes = static_cast<uint32_t>(byteLen);

    const size_t bound = lz4CompressBoundCompat(byteLen);
    if (bound > 0u) {
        g_tlLz4Scratch.resize(bound);
        const size_t compSize =
            lz4CompressFastCompat(g_tlXorScratch.data(), byteLen, g_tlLz4Scratch.data(), bound);
        if (compSize > 0u && compSize < byteLen) {
            outDelta.compressed_xor_lz4.assign(g_tlLz4Scratch.data(),
                                               g_tlLz4Scratch.data() + compSize);
            return true;
        }
    }

    outDelta.compressed_xor_lz4.assign(g_tlXorScratch.data(), g_tlXorScratch.data() + byteLen);
    return true;
}

bool MacroTileUndoSlab::applyXorDelta(const MacroTileDelta &delta, const uint8_t *currentBytes,
                                      uint8_t *outRestoredBytes, size_t byteLen)
{
    if (!currentBytes || !outRestoredBytes || delta.uncompressed_bytes == 0u ||
        delta.uncompressed_bytes > kMaxTileRawBytes || byteLen != delta.uncompressed_bytes ||
        delta.compressed_xor_lz4.empty()) {
        return false;
    }

    if (delta.compressed_xor_lz4.size() == byteLen) {
        xorBuffers(currentBytes, delta.compressed_xor_lz4.data(), outRestoredBytes, byteLen);
        return true;
    }

    g_tlXorScratch.resize(byteLen);
    if (!lz4DecompressCompat(delta.compressed_xor_lz4.data(), delta.compressed_xor_lz4.size(),
                             g_tlXorScratch.data(), byteLen)) {
        return false;
    }
    xorBuffers(currentBytes, g_tlXorScratch.data(), outRestoredBytes, byteLen);
    return true;
}

void DirtyTileUndoStack::beginTransaction(uint64_t strokeId, int32_t objectId, uint32_t passIndex,
                                          uint32_t texSlot, we_brush_channel_t channel,
                                          uint32_t bytesPerPixel, uint32_t canvasWidth,
                                          uint32_t canvasHeight)
{
    if (m_inTransaction) {
        WAYWAL_LOG_WARN(
            "DirtyTileUndoStack: aborting uncommitted stroke #%llu before starting #%llu",
            static_cast<unsigned long long>(m_activeTx.strokeId),
            static_cast<unsigned long long>(strokeId));
        abortTransaction();
    }

    const uint32_t clampedW = std::clamp(canvasWidth, 1u, kMaxCanvasDim);
    const uint32_t clampedH = std::clamp(canvasHeight, 1u, kMaxCanvasDim);
    m_tilesX = (clampedW + kTileSizePx - 1u) / kTileSizePx;
    m_tilesY = (clampedH + kTileSizePx - 1u) / kTileSizePx;

    const size_t totalTiles = static_cast<size_t>(m_tilesX) * static_cast<size_t>(m_tilesY);
    const size_t wordCount = (totalTiles + 63u) / 64u;
    m_activeBitset.assign(wordCount, 0u);
    m_activePreIndex.clear();
    m_activePostIndex.clear();

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

void DirtyTileUndoStack::upsertTileSnapshot(std::vector<CompressedTileSnapshot> &tiles,
                                            std::unordered_map<uint32_t, size_t> &tileIndex,
                                            uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes,
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

    const uint32_t key = tileCoordKey(tx, ty);
    const auto it = tileIndex.find(key);
    if (it != tileIndex.end() && it->second < tiles.size()) {
        tiles[it->second] = std::move(snap);
        return;
    }

    tileIndex[key] = tiles.size();
    tiles.push_back(std::move(snap));
}

void DirtyTileUndoStack::recordPreTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes,
                                            size_t byteLen)
{
    if (!m_inTransaction || tx >= m_tilesX || ty >= m_tilesY) {
        return;
    }
    const uint32_t key = tileCoordKey(tx, ty);
    if (m_activePreIndex.find(key) != m_activePreIndex.end()) {
        // Never overwrite an already captured pre-stroke baseline on repeat dab touches.
        return;
    }
    (void)markTileTouched(tx, ty);
    upsertTileSnapshot(m_activeTx.preTiles, m_activePreIndex, tx, ty, rawTileBytes, byteLen);
}

void DirtyTileUndoStack::recordPostTileBytes(uint16_t tx, uint16_t ty, const uint8_t *rawTileBytes,
                                             size_t byteLen)
{
    upsertTileSnapshot(m_activeTx.postTiles, m_activePostIndex, tx, ty, rawTileBytes, byteLen);
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

    while (m_transactions.size() > m_cursor) {
        const auto &dropped = m_transactions.back();
        const size_t freed = transactionHostBytes(dropped);
        const size_t droppedSeq = m_evictedCount + m_transactions.size() - 1u;
        const auto it = m_strokeIndex.find(dropped.strokeId);
        if (it != m_strokeIndex.end() && it->second == droppedSeq) {
            m_strokeIndex.erase(it);
        }
        m_transactions.pop_back();
        m_totalHostBytes = (freed <= m_totalHostBytes) ? (m_totalHostBytes - freed) : 0u;
    }

    m_activeTx.preTiles.shrink_to_fit();
    m_activeTx.postTiles.shrink_to_fit();
    const size_t txBytes = transactionHostBytes(m_activeTx);
    if (txBytes + m_macroSlab.resident_bytes() > kMaxHostCacheBytes) {
        if (txBytes <= kMaxHostCacheBytes) {
            while (
                !m_transactions.empty() &&
                (m_totalHostBytes + txBytes + m_macroSlab.resident_bytes() > kMaxHostCacheBytes)) {
                const auto &oldest = m_transactions.front();
                const size_t freed = transactionHostBytes(oldest);
                const auto it = m_strokeIndex.find(oldest.strokeId);
                if (it != m_strokeIndex.end() && it->second == m_evictedCount) {
                    m_strokeIndex.erase(it);
                }
                m_transactions.pop_front();
                ++m_evictedCount;
                if (m_cursor > 0u) {
                    --m_cursor;
                }
                m_totalHostBytes = (freed <= m_totalHostBytes) ? (m_totalHostBytes - freed) : 0u;
            }
        }
    }

    if (m_totalHostBytes + txBytes + m_macroSlab.resident_bytes() <= kMaxHostCacheBytes) {
        const uint64_t strokeId = m_activeTx.strokeId;
        m_transactions.push_back(std::move(m_activeTx));
        // Store monotonic sequence so FIFO pop_front() never invalidates remaining map entries.
        m_strokeIndex[strokeId] = m_evictedCount + m_transactions.size() - 1u;
        m_cursor = m_transactions.size();
        m_totalHostBytes += txBytes;
    }

    m_inTransaction = false;
    m_activeTx = DirtyTileTransaction{};
    m_activeBitset.clear();
    m_activePreIndex.clear();
    m_activePostIndex.clear();
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
    m_activePreIndex.clear();
    m_activePostIndex.clear();
    std::vector<uint64_t>{}.swap(m_activeBitset);
}

const DirtyTileTransaction *DirtyTileUndoStack::undo() noexcept
{
    if (!canUndo()) {
        return nullptr;
    }
    --m_cursor;
    return &m_transactions[m_cursor];
}

const DirtyTileTransaction *DirtyTileUndoStack::redo() noexcept
{
    if (!canRedo()) {
        return nullptr;
    }
    return &m_transactions[m_cursor++];
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
    if (!lz4DecompressCompat(snap.payload.data(), snap.payload.size(), outRaw.data(),
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
    m_cursor = 0;
    m_totalHostBytes = 0;
    m_evictedCount = 0;
    m_macroSlab.clear();
}

void DirtyTileUndoStack::evictOldestIfOverBudget()
{
    while ((m_totalHostBytes + m_macroSlab.resident_bytes()) > kMaxHostCacheBytes &&
           !m_transactions.empty()) {
        const auto &oldest = m_transactions.front();
        const size_t freed = transactionHostBytes(oldest);
        const auto it = m_strokeIndex.find(oldest.strokeId);
        if (it != m_strokeIndex.end() && it->second == m_evictedCount) {
            m_strokeIndex.erase(it);
        }

        m_transactions.pop_front();
        ++m_evictedCount;
        if (m_cursor > 0u) {
            --m_cursor;
        }
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
    const size_t bound = lz4CompressBoundCompat(byteLen);
    if (bound > 0u) {
        g_tlLz4Scratch.resize(bound);
        const size_t compSize =
            lz4CompressFastCompat(rawBytes, byteLen, g_tlLz4Scratch.data(), bound);
        if (compSize > 0u && compSize < byteLen) {
            snap.isLz4 = true;
            snap.payload.assign(g_tlLz4Scratch.data(), g_tlLz4Scratch.data() + compSize);
            return snap;
        }
    }

    snap.isLz4 = false;
    snap.payload.assign(rawBytes, rawBytes + byteLen);
    return snap;
}

} // namespace waywal::studio
