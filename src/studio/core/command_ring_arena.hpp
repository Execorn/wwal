// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_COMMAND_RING_ARENA_HPP
#define WAYWAL_STUDIO_COMMAND_RING_ARENA_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace waywal::studio {

template <std::size_t CapacityBytes = 4096U> class CommandRingArena {
    static_assert(CapacityBytes >= 64U && (CapacityBytes & (CapacityBytes - 1U)) == 0U,
                  "Ring capacity must be power-of-two >= 64 bytes");

  public:
    static constexpr uint32_t kWrapSentinelTag = std::numeric_limits<uint32_t>::max();
    static constexpr std::size_t kMaxCoalescedPayload = 64U;
    // Cap coalesced staging to try_push's CapacityBytes/4 limit so small rings never deadlock
    // flush.
    static constexpr std::size_t kEffectiveMaxCoalesced =
        std::min<std::size_t>(kMaxCoalescedPayload, CapacityBytes / 4U);
    static constexpr uint32_t kCoalesceWindowMs = 500U;

    struct EntryHeader {
        uint32_t type_tag;
        uint32_t payload_bytes;
        uint32_t uniform_hash;
        uint32_t timestamp_ms;
    };
    static_assert(sizeof(EntryHeader) == 16U && (sizeof(EntryHeader) & 15U) == 0U,
                  "EntryHeader must preserve 16-byte ring stride alignment");

    bool try_push(uint32_t type_tag, uint32_t uniform_hash, uint32_t now_ms, const void *payload,
                  uint32_t payload_bytes) noexcept
    {
        if (type_tag == kWrapSentinelTag || payload_bytes > CapacityBytes / 4U) {
            return false;
        }
        if (payload_bytes > 0U && payload == nullptr) {
            return false;
        }
        const std::size_t rec_bytes = align16(sizeof(EntryHeader) + payload_bytes);
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t used = h - t;
        const std::size_t free_bytes = CapacityBytes - used;
        const std::size_t idx = (h & kMask) & ~std::size_t{15};
        const std::size_t tail_gap = CapacityBytes - idx;
        if (tail_gap < sizeof(EntryHeader)) {
            return false;
        }

        std::size_t write_idx = idx;
        std::size_t total_advance = rec_bytes;
        if (tail_gap < rec_bytes) {
            if (free_bytes < tail_gap + rec_bytes) {
                return false;
            }
            EntryHeader wrap_hdr{kWrapSentinelTag,
                                 static_cast<uint32_t>(tail_gap - sizeof(EntryHeader)), 0U, 0U};
            std::memcpy(buf_.data() + idx, &wrap_hdr, sizeof(EntryHeader));
            write_idx = 0U;
            total_advance = tail_gap + rec_bytes;
        } else if (free_bytes < rec_bytes) {
            return false;
        }

        EntryHeader hdr{type_tag, payload_bytes, uniform_hash, now_ms};
        std::memcpy(buf_.data() + write_idx, &hdr, sizeof(EntryHeader));
        if (payload_bytes > 0U && payload != nullptr) {
            std::memcpy(buf_.data() + write_idx + sizeof(EntryHeader), payload, payload_bytes);
        }
        head_.store(h + total_advance, std::memory_order_release);
        return true;
    }

    bool try_pop(EntryHeader &hdr_out, void *payload_out, std::size_t max_payload_bytes) noexcept
    {
        std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        while (t != h) {
            const std::size_t idx = (t & kMask) & ~std::size_t{15};
            const std::size_t tail_avail = CapacityBytes - idx;
            if (tail_avail < sizeof(EntryHeader)) {
                return false;
            }
            EntryHeader hdr{};
            std::memcpy(&hdr, buf_.data() + idx, sizeof(EntryHeader));
            if (hdr.type_tag == kWrapSentinelTag) {
                t += tail_avail;
                tail_.store(t, std::memory_order_release);
                continue;
            }
            if (hdr.payload_bytes > max_payload_bytes || hdr.payload_bytes > CapacityBytes / 4U ||
                tail_avail < sizeof(EntryHeader) + hdr.payload_bytes ||
                (hdr.payload_bytes > 0U && payload_out == nullptr)) {
                return false;
            }
            if (hdr.payload_bytes > 0U && payload_out != nullptr) {
                std::memcpy(payload_out, buf_.data() + idx + sizeof(EntryHeader),
                            hdr.payload_bytes);
            }
            hdr_out = hdr;
            const std::size_t rec_bytes = align16(sizeof(EntryHeader) + hdr.payload_bytes);
            tail_.store(t + rec_bytes, std::memory_order_release);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_coalesce_staged(uint32_t type_tag, uint32_t uniform_hash,
                                           uint32_t now_ms, const void *payload,
                                           uint32_t payload_bytes) noexcept
    {
        if (type_tag == kWrapSentinelTag || payload_bytes > kEffectiveMaxCoalesced ||
            (payload_bytes > 0U && payload == nullptr)) {
            return false;
        }
        if (staged_valid_ && staged_hdr_.type_tag == type_tag &&
            staged_hdr_.uniform_hash == uniform_hash &&
            staged_hdr_.payload_bytes == payload_bytes &&
            (now_ms - staged_hdr_.timestamp_ms) <= kCoalesceWindowMs) {
            if (payload_bytes > 0U && payload != nullptr) {
                std::memcpy(staged_payload_.data(), payload, payload_bytes);
            }
            staged_hdr_.timestamp_ms = now_ms;
            return true;
        }
        return false;
    }

    bool try_push_or_coalesce(uint32_t type_tag, uint32_t uniform_hash, uint32_t now_ms,
                              const void *payload, uint32_t payload_bytes) noexcept
    {
        if (type_tag == kWrapSentinelTag || payload_bytes > CapacityBytes / 4U ||
            (payload_bytes > 0U && payload == nullptr)) {
            return false;
        }
        if (try_coalesce_staged(type_tag, uniform_hash, now_ms, payload, payload_bytes)) {
            return true;
        }
        if (!flush_coalesced()) {
            return false;
        }
        if (payload_bytes <= kEffectiveMaxCoalesced) {
            staged_hdr_ = EntryHeader{type_tag, payload_bytes, uniform_hash, now_ms};
            if (payload_bytes > 0U && payload != nullptr) {
                std::memcpy(staged_payload_.data(), payload, payload_bytes);
            }
            staged_valid_ = true;
            return true;
        }
        return try_push(type_tag, uniform_hash, now_ms, payload, payload_bytes);
    }

    bool flush_coalesced() noexcept
    {
        if (!staged_valid_) {
            return true;
        }
        if (!try_push(staged_hdr_.type_tag, staged_hdr_.uniform_hash, staged_hdr_.timestamp_ms,
                      staged_payload_.data(), staged_hdr_.payload_bytes)) {
            return false;
        }
        staged_valid_ = false;
        return true;
    }

    [[nodiscard]] bool has_staged() const noexcept { return staged_valid_; }
    [[nodiscard]] const EntryHeader &staged_header() const noexcept { return staged_hdr_; }
    [[nodiscard]] const void *staged_payload() const noexcept { return staged_payload_.data(); }
    void discard_staged() noexcept { staged_valid_ = false; }

    [[nodiscard]] std::size_t used_bytes() const noexcept
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    void reset() noexcept
    {
        staged_valid_ = false;
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

  private:
    static constexpr std::size_t kMask = CapacityBytes - 1U;
    static constexpr std::size_t align16(std::size_t n) noexcept
    {
        return (n + 15U) & ~std::size_t{15};
    }

    alignas(64) std::array<uint8_t, CapacityBytes> buf_{};
    alignas(64) std::atomic<std::size_t> head_{0U};
    alignas(64) std::atomic<std::size_t> tail_{0U};
    EntryHeader staged_hdr_{};
    std::array<uint8_t, kMaxCoalescedPayload> staged_payload_{};
    bool staged_valid_{false};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_COMMAND_RING_ARENA_HPP
