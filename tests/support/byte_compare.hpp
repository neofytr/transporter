// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_TESTS_SUPPORT_BYTE_COMPARE_HPP
#define TRANSPORTER_TESTS_SUPPORT_BYTE_COMPARE_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace transporter::testing {

// Outcome of a byte-for-byte comparison between a reference (source) buffer
// and a captured buffer. `match` is true iff every reference byte appears at
// the same offset in the capture. The capture is permitted to be longer than
// the reference: trailing capture bytes (the loopback's silence after the
// payload) are not compared.
struct ByteCompareResult {
    bool match = false;
    std::size_t reference_size = 0;
    std::size_t capture_size = 0;
    std::size_t mismatch_offset = 0;     // valid iff !match
    std::uint8_t reference_byte = 0;     // valid iff !match
    std::uint8_t capture_byte = 0;       // valid iff !match
    std::size_t mismatch_count = 0;      // total differing bytes within reference range
};

// Compare reference against capture across `reference.size()` bytes. The
// capture must be at least as long as the reference. Hand-rolled rather than
// `std::ranges::equal` so we report the first-offset / count of mismatches
// when the comparison fails — `equal` returns a single bool.
inline ByteCompareResult compare_bytes(std::span<const std::byte> reference,
                                       std::span<const std::byte> capture) noexcept {
    ByteCompareResult r;
    r.reference_size = reference.size();
    r.capture_size = capture.size();
    if (capture.size() < reference.size()) {
        r.match = false;
        r.mismatch_offset = capture.size();
        r.mismatch_count = reference.size() - capture.size();
        return r;
    }
    bool first_seen = false;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (reference[i] != capture[i]) {
            if (!first_seen) {
                r.mismatch_offset = i;
                r.reference_byte = static_cast<std::uint8_t>(reference[i]);
                r.capture_byte = static_cast<std::uint8_t>(capture[i]);
                first_seen = true;
            }
            ++r.mismatch_count;
        }
    }
    r.match = (r.mismatch_count == 0);
    return r;
}

} // namespace transporter::testing

#endif
