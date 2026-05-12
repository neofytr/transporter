// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke-test the player-bar formatter helpers. These are pure-function
// helpers that don't require a notcurses context or a terminal.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../../src/tui/components/player_bar.hpp"

#include <chrono>

namespace tc = transporter::tui::components;
using ms = std::chrono::milliseconds;

TEST_CASE("format_mm_ss: zero and negative") {
    CHECK(tc::format_mm_ss(ms{0})    == "0:00");
    CHECK(tc::format_mm_ss(ms{-1})   == "0:00");
    CHECK(tc::format_mm_ss(ms{-999}) == "0:00");
}

TEST_CASE("format_mm_ss: sub-minute") {
    CHECK(tc::format_mm_ss(ms{42})     == "0:00");
    CHECK(tc::format_mm_ss(ms{42000})  == "0:42");
    CHECK(tc::format_mm_ss(ms{59999})  == "0:59");
}

TEST_CASE("format_mm_ss: minutes") {
    CHECK(tc::format_mm_ss(ms{60000})   == "1:00");
    CHECK(tc::format_mm_ss(ms{154000})  == "2:34");
    CHECK(tc::format_mm_ss(ms{423000})  == "7:03");
    CHECK(tc::format_mm_ss(ms{3599000}) == "59:59");
}

TEST_CASE("format_mm_ss: hours") {
    CHECK(tc::format_mm_ss(ms{3600000}) == "1:00:00");
    CHECK(tc::format_mm_ss(ms{3661000}) == "1:01:01");
    // 1h 23m 45s = 5025 s = 5025000 ms
    CHECK(tc::format_mm_ss(ms{5025000}) == "1:23:45");
}

TEST_CASE("format_rate_bits_codec: well-formed inputs") {
    // 44100 is not a multiple of 1000 — decimal branch.
    CHECK(tc::format_rate_bits_codec(44100, 16, "FLAC") == "44.1k/16 FLAC");
    CHECK(tc::format_rate_bits_codec(48000, 24, "WAV")  == "48k/24 WAV");
    CHECK(tc::format_rate_bits_codec(96000, 24, "FLAC") == "96k/24 FLAC");
    CHECK(tc::format_rate_bits_codec(88200, 24, "FLAC") == "88.2k/24 FLAC");
}

TEST_CASE("format_rate_bits_codec: placeholder when unset") {
    // No codec -> em-dash placeholder.
    CHECK(tc::format_rate_bits_codec(44100, 16, "")     == "—");
    // No rate -> em-dash placeholder.
    CHECK(tc::format_rate_bits_codec(0, 16, "FLAC")     == "—");
}

TEST_CASE("format_ring_fill: clamps and rounds") {
    CHECK(tc::format_ring_fill(0.0)   == "ring 0%");
    CHECK(tc::format_ring_fill(1.0)   == "ring 100%");
    CHECK(tc::format_ring_fill(0.82)  == "ring 82%");
    CHECK(tc::format_ring_fill(0.829) == "ring 83%");
    CHECK(tc::format_ring_fill(-0.5)  == "ring 0%");
    CHECK(tc::format_ring_fill(99.0)  == "ring 100%");
}
