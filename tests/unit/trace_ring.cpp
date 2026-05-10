// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPSC trace ring smoke test. Pushes events, drains, asserts ordering and
// the dropped() counter increments past capacity.

#include <transporter/engine/trace.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <span>

namespace trace = transporter::engine::trace;

namespace {

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

} // namespace

int main() {
    constexpr std::size_t CAP = 16;
    trace::Ring r(CAP);

    if (r.capacity() != CAP) {
        return fail("capacity");
    }
    if (r.dropped() != 0) {
        return fail("initial dropped");
    }

    // Fill exactly to capacity.
    for (std::size_t i = 0; i < CAP; ++i) {
        trace::Event e{};
        e.monotonic_ns = static_cast<std::uint64_t>(i + 1);
        e.kind = trace::Kind::FrameWritten;
        e.large_a = static_cast<std::uint32_t>(i);
        if (!r.push(e)) {
            return fail("push within capacity");
        }
    }

    // One past capacity must drop.
    {
        trace::Event e{};
        e.monotonic_ns = 99999;
        if (r.push(e)) {
            return fail("push past capacity should fail");
        }
        if (r.dropped() != 1) {
            return fail("dropped count");
        }
    }

    // Drain half, in order.
    std::array<trace::Event, 8> out{};
    const std::size_t got =
        r.drain(std::span<trace::Event>(out.data(), out.size()));
    if (got != 8) {
        return fail("drain count");
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (out[i].large_a != static_cast<std::uint32_t>(i)) {
            return fail("drain order");
        }
    }

    // Now there's room: pushes succeed.
    for (std::size_t i = 0; i < 4; ++i) {
        trace::Event e{};
        e.monotonic_ns = static_cast<std::uint64_t>(1000 + i);
        e.kind = trace::Kind::Xrun;
        e.large_a = static_cast<std::uint32_t>(100 + i);
        if (!r.push(e)) {
            return fail("post-drain push");
        }
    }

    // Drain the rest. Should be 8 (remaining of original) + 4 = 12.
    std::array<trace::Event, 32> rest{};
    const std::size_t total =
        r.drain(std::span<trace::Event>(rest.data(), rest.size()));
    if (total != 12) {
        return fail("final drain count");
    }
    // First 8 are originals 8..15
    for (std::size_t i = 0; i < 8; ++i) {
        if (rest[i].large_a != static_cast<std::uint32_t>(8 + i)) {
            return fail("final drain order originals");
        }
    }
    // Next 4 are post-drain pushes
    for (std::size_t i = 0; i < 4; ++i) {
        if (rest[8 + i].large_a != static_cast<std::uint32_t>(100 + i)) {
            return fail("final drain order post-drain");
        }
    }

    std::printf("ok trace_ring capacity=%zu dropped=%llu\n",
                r.capacity(),
                static_cast<unsigned long long>(r.dropped()));
    return 0;
}
