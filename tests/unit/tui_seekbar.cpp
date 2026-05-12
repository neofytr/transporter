// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure-function tests for the seekbar component. Hit-test maps column to
// cell index; format_time renders the "MM:SS / MM:SS" label. No notcurses
// context is required.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../../src/tui/components/seekbar.hpp"

namespace tc = transporter::tui::components;

TEST_CASE("hit_test: in-range maps to identity") {
    CHECK(tc::hit_test(0, 100)  == 0);
    CHECK(tc::hit_test(50, 100) == 50);
    CHECK(tc::hit_test(99, 100) == 99);
}

TEST_CASE("hit_test: above width clamps to width-1") {
    CHECK(tc::hit_test(100, 100) == 99);
    CHECK(tc::hit_test(1000, 100) == 99);
}

TEST_CASE("hit_test: negative clamps to zero") {
    CHECK(tc::hit_test(-1, 100)   == 0);
    CHECK(tc::hit_test(-1000, 50) == 0);
}

TEST_CASE("hit_test: degenerate width") {
    CHECK(tc::hit_test(0, 0)  == 0);
    CHECK(tc::hit_test(5, -1) == 0);
}

TEST_CASE("position_ratio: linear across the span") {
    CHECK(tc::position_ratio(0, 11)  == doctest::Approx(0.0));
    CHECK(tc::position_ratio(5, 11)  == doctest::Approx(0.5));
    CHECK(tc::position_ratio(10, 11) == doctest::Approx(1.0));
}

TEST_CASE("position_ratio: clamps") {
    CHECK(tc::position_ratio(-1, 11) == doctest::Approx(0.0));
    CHECK(tc::position_ratio(99, 11) == doctest::Approx(1.0));
}

TEST_CASE("format_time: zero position, two-minute duration") {
    CHECK(tc::format_time(0, 120000) == "0:00 / 2:00");
}

TEST_CASE("format_time: mid-track") {
    CHECK(tc::format_time(154000, 423000) == "2:34 / 7:03");
}

TEST_CASE("format_time: hours bracket") {
    CHECK(tc::format_time(3600000, 7200000) == "1:00:00 / 2:00:00");
}

TEST_CASE("format_time: zero duration renders zeros") {
    CHECK(tc::format_time(0, 0) == "0:00 / 0:00");
}
