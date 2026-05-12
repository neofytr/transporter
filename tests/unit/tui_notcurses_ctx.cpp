// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke-test the notcurses capability probe. The probe runs without taking
// over the terminal, so it is safe in CI; when no /dev/tty is available it
// must still return a well-formed struct (env fields populated, terminal
// fields zeroed).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../../src/tui/notcurses_ctx.hpp"

#include <cstdlib>

namespace tt = transporter::tui;

TEST_CASE("probe_capabilities: returns a well-formed struct without crashing") {
    const auto caps = tt::probe_capabilities();
    // Dimensions are non-negative by construction. Actual values depend on
    // whether a controlling tty was available during the probe; CI without a
    // real terminal yields zeros, which is correct.
    CHECK(caps.cells_cols >= 0);
    CHECK(caps.cells_rows >= 0);
    CHECK(caps.pixel_cols >= 0);
    CHECK(caps.pixel_rows >= 0);
}

TEST_CASE("probe_capabilities: env fields reflect process environment") {
    setenv("TERM", "xterm-256color", 1);
    setenv("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    unsetenv("STY");
    unsetenv("TERM_PROGRAM");

    const auto caps = tt::probe_capabilities();
    CHECK(caps.term == "xterm-256color");
    CHECK(caps.inside_tmux == true);
    CHECK(caps.inside_screen == false);
    CHECK(caps.term_program.empty());
}

TEST_CASE("probe_capabilities: $TERM starting with 'screen' implies inside_screen") {
    setenv("TERM", "screen.xterm-256color", 1);
    unsetenv("TMUX");
    unsetenv("STY");

    const auto caps = tt::probe_capabilities();
    CHECK(caps.inside_screen == true);
}

TEST_CASE("probe_capabilities: empty TERM gracefully handled") {
    setenv("TERM", "", 1);
    unsetenv("TMUX");
    unsetenv("STY");
    unsetenv("TERM_PROGRAM");

    const auto caps = tt::probe_capabilities();
    CHECK(caps.term.empty());
    CHECK(caps.inside_tmux == false);
    CHECK(caps.inside_screen == false);
}
