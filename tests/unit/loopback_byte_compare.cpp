// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test for the byte-comparator helper used by the bit-perfect loopback
// integration harness. Mocks the source / capture pair as plain buffers; no
// live ALSA or Engine.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../support/byte_compare.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tt = transporter::testing;

namespace {

std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> xs) {
    std::vector<std::byte> out;
    out.reserve(xs.size());
    for (auto x : xs) {
        out.push_back(std::byte{x});
    }
    return out;
}

} // namespace

TEST_CASE("compare_bytes: identical buffers match") {
    const auto a = bytes({0x00, 0x11, 0x22, 0x33, 0x44});
    const auto b = bytes({0x00, 0x11, 0x22, 0x33, 0x44});
    auto r = tt::compare_bytes(a, b);
    CHECK(r.match);
    CHECK(r.mismatch_count == 0);
    CHECK(r.reference_size == 5);
    CHECK(r.capture_size == 5);
}

TEST_CASE("compare_bytes: capture longer than reference is allowed") {
    const auto a = bytes({0x10, 0x20, 0x30});
    const auto b = bytes({0x10, 0x20, 0x30, 0x00, 0x00, 0x00});
    auto r = tt::compare_bytes(a, b);
    CHECK(r.match);
    CHECK(r.mismatch_count == 0);
    CHECK(r.reference_size == 3);
    CHECK(r.capture_size == 6);
}

TEST_CASE("compare_bytes: single-byte mismatch reports first offset and value") {
    const auto a = bytes({0xAA, 0xBB, 0xCC, 0xDD});
    const auto b = bytes({0xAA, 0xBB, 0x00, 0xDD});
    auto r = tt::compare_bytes(a, b);
    CHECK_FALSE(r.match);
    CHECK(r.mismatch_count == 1);
    CHECK(r.mismatch_offset == 2);
    CHECK(r.reference_byte == 0xCC);
    CHECK(r.capture_byte == 0x00);
}

TEST_CASE("compare_bytes: many mismatches still report first offset") {
    const auto a = bytes({0xAA, 0xBB, 0xCC, 0xDD, 0xEE});
    const auto b = bytes({0xAA, 0x00, 0x00, 0x00, 0x00});
    auto r = tt::compare_bytes(a, b);
    CHECK_FALSE(r.match);
    CHECK(r.mismatch_count == 4);
    CHECK(r.mismatch_offset == 1);
    CHECK(r.reference_byte == 0xBB);
    CHECK(r.capture_byte == 0x00);
}

TEST_CASE("compare_bytes: capture shorter than reference is a mismatch") {
    const auto a = bytes({0x01, 0x02, 0x03, 0x04});
    const auto b = bytes({0x01, 0x02});
    auto r = tt::compare_bytes(a, b);
    CHECK_FALSE(r.match);
    CHECK(r.reference_size == 4);
    CHECK(r.capture_size == 2);
    CHECK(r.mismatch_offset == 2);
    CHECK(r.mismatch_count == 2);
}

TEST_CASE("compare_bytes: empty reference always matches") {
    std::vector<std::byte> a;
    const auto b = bytes({0x99});
    auto r = tt::compare_bytes(a, b);
    CHECK(r.match);
    CHECK(r.reference_size == 0);
    CHECK(r.capture_size == 1);
}
