// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPSC trace ring. Power-of-two capacity, fixed-size events. Producer is the
// audio thread; consumer is a non-RT drain thread.
//
// `head_` / `tail_` are full sequence counters (not modulo capacity); the
// difference is the live count, capped by capacity. The mask isolates the
// slot index. This avoids the "full vs empty" ambiguity classic SPSC rings
// would otherwise need a sentinel slot for.

#include <transporter/engine/trace.hpp>

#include <bit>
#include <cstddef>
#include <ctime>

namespace transporter::engine::trace {

std::size_t Ring::round_up_pow2(std::size_t n) noexcept {
    if (n < 2) {
        return 2;
    }
    if (std::has_single_bit(n)) {
        return n;
    }
    return std::bit_ceil(n);
}

Ring::Ring(std::size_t capacity_pow2)
    : capacity_(round_up_pow2(capacity_pow2)),
      mask_(round_up_pow2(capacity_pow2) - 1),
      buffer_(new Event[round_up_pow2(capacity_pow2)]{}),
      head_(0),
      tail_(0),
      dropped_(0) {}

bool Ring::push(const Event& e) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    if ((h - t) >= capacity_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    buffer_[h & mask_] = e;
    head_.store(h + 1, std::memory_order_release);
    return true;
}

std::size_t Ring::drain(std::span<Event> out) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t avail = h - t;
    const std::size_t want = out.size();
    const std::size_t n = avail < want ? avail : want;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = buffer_[(t + i) & mask_];
    }
    tail_.store(t + n, std::memory_order_release);
    return n;
}

std::uint64_t monotonic_ns_now() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

} // namespace transporter::engine::trace
