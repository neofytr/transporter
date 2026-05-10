// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_TRACE_HPP
#define TRANSPORTER_ENGINE_TRACE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace transporter::engine::trace {

// Event categories the audio / decoder / engine threads emit. Codes are
// stable; never renumber once shipped.
enum class Kind : std::uint8_t {
    AudioStart,
    AudioStop,
    RatePrepare,
    RateLocked,
    Xrun,
    Recover,
    FrameWritten,
    DecodeOpen,
    DecodeEof,
    DecodeError,
    DeviceOpen,
    DeviceClose,
    DeviceLost,
    DeviceReturn,
};

// Fixed 32-byte payload. Producers push by value; no allocation, no system
// calls, no locks. Field interpretation depends on Kind:
//   Xrun        : small_a = xrun-seq, large_a = errno
//   FrameWritten: large_a = frames written this call, large_b = period_size
//   Rate*       : large_a = rate_hz, large_b = period_size, large_c = periods
//   Device*     : (kind alone is sufficient)
struct alignas(32) Event {
    std::uint64_t monotonic_ns;
    Kind kind;
    std::uint8_t reserved;
    std::uint16_t small_a;
    std::uint32_t large_a;
    std::uint32_t large_b;
    std::uint32_t large_c;
};
static_assert(sizeof(Event) == 32, "Event layout pinned at 32 bytes");

// Single-producer / single-consumer ring of fixed-size events. Producer is
// the audio thread (`push`); consumer is a low-priority drain thread
// (`drain`). Capacity is fixed at construction; no resizing.
//
// Memory ordering:
//   producer: load tail with acquire, store head with release;
//   consumer: load head with acquire, store tail with release.
// Drop counter uses relaxed since it's a debug-grade statistic.
class Ring {
public:
    explicit Ring(std::size_t capacity_pow2);

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    Ring(Ring&&) = delete;
    Ring& operator=(Ring&&) = delete;
    ~Ring() = default;

    std::size_t capacity() const noexcept { return capacity_; }

    // Producer (audio thread). Wait-free; never allocates, never syscalls.
    // Returns false and bumps `dropped()` if the ring is full.
    bool push(const Event& e) noexcept;

    // Consumer (drain thread). Copies up to out.size() events. Returns the
    // number written. Wait-free.
    std::size_t drain(std::span<Event> out) noexcept;

    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static std::size_t round_up_pow2(std::size_t n) noexcept;

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Event[]> buffer_;
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
    alignas(64) std::atomic<std::uint64_t> dropped_;
};

// Monotonic clock helper. Audio-thread safe (clock_gettime CLOCK_MONOTONIC
// is on the RT-safe list on Linux glibc).
std::uint64_t monotonic_ns_now() noexcept;

} // namespace transporter::engine::trace

#endif
