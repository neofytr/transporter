// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_RING_HPP
#define TRANSPORTER_ENGINE_RING_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>

namespace transporter::engine {

// Provisional SPSC byte ring. Power-of-two capacity. Lock-free with two
// atomic indices: producer mutates head, consumer mutates tail.
//
// Memory ordering: a side that publishes an index uses release; the side
// reading the *opposite* side's index uses acquire. The implementation does
// not use seq_cst — only acq/rel pairs, which is sufficient for SPSC.
//
// Phase 5 will revisit this (final shape to live in src/engine/rt/).
class SpscByteRing {
public:
    explicit SpscByteRing(std::size_t capacity_pow2)
        : capacity_(capacity_pow2),
          mask_(capacity_pow2 - 1),
          buffer_(new std::byte[capacity_pow2]),
          head_(0),
          tail_(0) {
        // capacity must be a non-zero power of two so mask_ works.
    }

    SpscByteRing(const SpscByteRing&) = delete;
    SpscByteRing& operator=(const SpscByteRing&) = delete;
    SpscByteRing(SpscByteRing&&) = delete;
    SpscByteRing& operator=(SpscByteRing&&) = delete;

    static constexpr bool is_pow2(std::size_t n) noexcept {
        return n != 0 && std::has_single_bit(n);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Producer side: bytes available to write.
    std::size_t writable() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return capacity_ - (h - t);
    }

    // Consumer side: bytes available to read.
    std::size_t readable() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    // Producer: copy as much as fits, return bytes copied. Non-blocking.
    std::size_t write(std::span<const std::byte> in) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t free_bytes = capacity_ - (h - t);
        const std::size_t n = in.size() < free_bytes ? in.size() : free_bytes;
        if (n == 0) {
            return 0;
        }
        const std::size_t off = h & mask_;
        const std::size_t first = (off + n) <= capacity_ ? n : (capacity_ - off);
        std::memcpy(buffer_.get() + off, in.data(), first);
        if (first < n) {
            std::memcpy(buffer_.get(), in.data() + first, n - first);
        }
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Consumer: copy as much as is available into out, return bytes copied.
    std::size_t read(std::span<std::byte> out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t avail = h - t;
        const std::size_t n = out.size() < avail ? out.size() : avail;
        if (n == 0) {
            return 0;
        }
        const std::size_t off = t & mask_;
        const std::size_t first = (off + n) <= capacity_ ? n : (capacity_ - off);
        std::memcpy(out.data(), buffer_.get() + off, first);
        if (first < n) {
            std::memcpy(out.data() + first, buffer_.get(), n - first);
        }
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<std::byte[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
};

} // namespace transporter::engine

#endif
